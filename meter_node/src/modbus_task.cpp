#include <Arduino.h>
#include "modbus_task.h"
#include "config.h"
#include "telemetry.h"
#include "ina226_hal.h"
#include "energy.h"
#include "modbus_rtu.h"
#include "meter_regs.h"
#include "rs485.h"

/* ---------------------------------------------------------------------------
 * The meter node as a Modbus-RTU slave.
 *
 * A slave is passive: it keeps its registers current and answers when asked.
 * It never initiates anything. So this task has two jobs, and they are
 * deliberately independent of each other:
 *
 *   1. Keep the register map mirroring the newest reading.
 *   2. Answer whatever arrives on the bus.
 *
 * Note how it consumes the ring: it drains to the NEWEST sample and keeps
 * only that one. A register map holds present values, not history, so old
 * samples are of no interest here -- while the logger, reading the same ring,
 * wants every single one. Same data, two different needs, neither aware of
 * the other. That is what the reader-cursor design buys.
 * -------------------------------------------------------------------------*/

/* Guards the include-order hazard documented in config.h. This file includes
 * ring_buffer.h (via modbus_task.h) BEFORE config.h, so if the ring size ever
 * reaches this translation unit with a different value from the rest of the
 * build, sizeof(telemetry_ring_t) would differ between object files and the
 * linker would not complain. Fail at compile time instead. */
static_assert(TELEMETRY_RING_SIZE == 256,
              "TELEMETRY_RING_SIZE must come from platformio.ini, not a header default");

static rs485_t           s_bus;
static modbus_slave_t    s_slave;
static meter_regs_t      s_regs;
static telemetry_reader_t s_reader;

static void refresh_registers(telemetry_ring_t *ring)
{
    telemetry_sample_t s;
    bool got = false;

    /* Fast-forward to the latest sample. */
    while (ring_read(ring, &s_reader, &s)) got = true;

    if (got) {
        s_regs.data.v            = s.v;
        s_regs.data.i            = s.i;
        s_regs.data.p            = s.p;
        s_regs.data.wh           = s.wh;
        s_regs.data.status_flags = s.flags;
        s_regs.data.truth_mask   = s.truth;
        s_regs.data.sample_seq   = s.seq;
    }

    s_regs.data.uptime_s = millis() / 1000;

    /* The reader's own overrun count goes on the bus. A master can therefore
     * see that this slave is losing samples internally -- a fault that would
     * otherwise be invisible from outside the device. */
    s_regs.data.overruns = (uint16_t)(s_reader.overruns > 0xFFFF
                                      ? 0xFFFF : s_reader.overruns);
}

static void task_modbus(void *arg)
{
    telemetry_ring_t *ring = (telemetry_ring_t *)arg;

    rs485_begin(&s_bus, &Serial2, MODBUS_BAUD,
                RS485_RX_PIN, RS485_TX_PIN, RS485_DE_PIN);

    meter_regs_init(&s_regs, FW_VERSION, ina226_is_simulated());
    modbus_slave_init(&s_slave, MODBUS_UNIT_ID,
                      meter_regs_read, meter_regs_write, &s_regs);
    ring_reader_attach(ring, &s_reader, "modbus");

    Serial.printf("# modbus slave: unit=%d baud=%lu t3.5=%lu us pins rx=%d tx=%d de=%d\n",
                  MODBUS_UNIT_ID, (unsigned long)MODBUS_BAUD,
                  (unsigned long)s_bus.t35_us,
                  RS485_RX_PIN, RS485_TX_PIN, RS485_DE_PIN);

    uint8_t  resp[MODBUS_MAX_FRAME];
    uint32_t last_stats_ms = 0;

    for (;;) {
        refresh_registers(ring);

        /* A master can zero the energy counter by writing the command
         * register. We act on it here and clear the request, so the write is
         * a one-shot rather than a latched state. */
        if (s_regs.energy_reset_requested) {
            energy_reset();
            s_regs.energy_reset_requested = false;
            Serial.println("# modbus: energy counter reset by master");
        }

        size_t n = rs485_poll_frame(&s_bus);
        if (n) {
            size_t rn = modbus_slave_handle(&s_slave, s_bus.buf, n,
                                            resp, sizeof(resp));
            if (rn) rs485_send(&s_bus, resp, rn);
        }

        uint32_t now = millis();
        if (now - last_stats_ms >= 10000) {
            last_stats_ms = now;
            Serial.printf("# modbus req=%lu crc_err=%lu exc=%lu ignored=%lu "
                          "tx=%lu overrun=%lu\n",
                          (unsigned long)s_slave.requests,
                          (unsigned long)s_slave.crc_errors,
                          (unsigned long)s_slave.exceptions,
                          (unsigned long)s_slave.ignored,
                          (unsigned long)s_bus.frames_tx,
                          (unsigned long)s_reader.overruns);
        }

        /* 1 ms. At 19200 baud a byte takes ~570 us, so we cannot miss one,
         * and the reply goes out well inside any master's timeout. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void modbus_task_start(telemetry_ring_t *ring)
{
    /* Priority 4: below the sampler (5), which must never be late, and above
     * the logger (2), because a master is waiting on the other end of a wire
     * with a timeout running. */
    xTaskCreatePinnedToCore(task_modbus, "modbus", 4096, ring, 4, NULL, 1);
}
