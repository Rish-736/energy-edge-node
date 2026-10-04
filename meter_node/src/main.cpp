#include <Arduino.h>
#include "config.h"
#include "telemetry.h"
#include "ring_buffer.h"
#include "ina226_hal.h"
#include "energy.h"
#include "modbus_task.h"

/* ---------------------------------------------------------------------------
 * meter_node -- Phase 0
 *
 * Two FreeRTOS tasks, one shared ring buffer:
 *
 *   sampler  (prio 5, core 1)  sensor -> energy -> ring.   Never blocks on a
 *                              consumer. This is the real-time path and it is
 *                              kept deliberately short.
 *   logger   (prio 2, core 0)  ring -> serial CSV.         Its own cursor, so
 *                              if serial backs up the sampler does not care.
 *
 * Phases 1-3 add tasks (Modbus slave, MQTT, demand-response control) that each
 * attach their own reader to this same ring. None of them touch the sampler.
 * -------------------------------------------------------------------------*/

static telemetry_ring_t   g_ring;
static telemetry_reader_t g_logger_reader;

/* --- sampling task ------------------------------------------------------ */
static void task_sampler(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(SAMPLE_PERIOD_MS);

    for (;;) {
        ina226_reading_t r;
        telemetry_sample_t s = {};

        s.t_ms  = millis();
        s.flags = ina226_is_simulated() ? TLM_FLAG_SIMULATED : TLM_FLAG_OK;

        if (ina226_read(&r)) {
            s.v = r.bus_voltage_v;
            s.i = r.current_a;
            s.p = r.power_w;
        } else {
            /* Keep the cadence and mark the gap. A missing sample is data:
             * downstream can see the fault instead of guessing from silence. */
            s.flags |= TLM_FLAG_SENSOR_FAIL;
        }

        s.wh    = energy_update(s.p, s.t_ms);
        s.truth = ina226_truth_mask();

        ring_push(&g_ring, &s);

        vTaskDelayUntil(&last_wake, period);
    }
}

/* --- logging task ------------------------------------------------------- */
static void task_logger(void *arg)
{
    (void)arg;
    ring_reader_attach(&g_ring, &g_logger_reader, "logger");

    uint32_t last_stats_ms = 0;

    for (;;) {
        telemetry_sample_t s;
        while (ring_read(&g_ring, &g_logger_reader, &s)) {
            /* 'T,' prefixes every data row so a log can be recovered from a
             * serial capture that also contains boot chatter and '#' notes. */
            Serial.printf("T,%lu,%lu,%.3f,%.4f,%.3f,%.6f,%u,%u\n",
                          (unsigned long)s.seq, (unsigned long)s.t_ms,
                          s.v, s.i, s.p, s.wh, s.flags, s.truth);
        }

        uint32_t now = millis();
        if (now - last_stats_ms >= 10000) {
            last_stats_ms = now;
            char truth[32];
            telemetry_truth_str(ina226_truth_mask(), truth, sizeof(truth));
            Serial.printf("# stats seq=%lu overruns=%lu wh=%.4f load=%s heap=%lu\n",
                          (unsigned long)g_ring.write_seq,
                          (unsigned long)g_logger_reader.overruns,
                          energy_wh(), truth,
                          (unsigned long)ESP.getFreeHeap());
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* --- bring-up ----------------------------------------------------------- */
void setup(void)
{
    Serial.begin(SERIAL_BAUD);
    delay(300);

    ring_init(&g_ring);
    energy_reset();

    bool sensor_ok = ina226_init();

    Serial.println("# energy-edge-node telemetry v1");
    Serial.printf("# source=%s sensor_init=%s rate_hz=%d ring=%d\n",
                  ina226_is_simulated() ? "sim" : "ina226",
                  sensor_ok ? "ok" : "FAIL",
                  SAMPLE_RATE_HZ, TELEMETRY_RING_SIZE);
    Serial.println("# fields: seq,t_ms,v_v,i_a,p_w,wh,flags,truth");

    xTaskCreatePinnedToCore(task_sampler, "sampler", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_logger,  "logger",  4096, NULL, 2, NULL, 0);
    modbus_task_start(&g_ring);
}

void loop(void)
{
    /* Everything runs in tasks. Arduino's loop task just idles out of the way. */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
