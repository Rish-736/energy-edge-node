#ifndef ARDUINO
/* ---------------------------------------------------------------------------
 * sim_main.cpp -- run the meter node on your PC.
 *
 * Drives the REAL simulator, the REAL energy accumulator and the REAL ring
 * buffer -- the same source files the ESP32 builds -- and emits the same CSV
 * the firmware emits. The only thing stubbed out is the board.
 *
 * So a capture produced here is a legitimate dataset for tools/replay.py and,
 * later, for training the Phase 5 classifier.
 *
 *   pio run -e native-sim -d meter_node
 *   .pio/build/native-sim/program 120 > capture.csv
 * -------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "telemetry.h"
#include "ring_buffer.h"
#include "ina226_hal.h"
#include "energy.h"

void sim_set_time_ms(uint32_t ms);

int main(int argc, char **argv)
{
    unsigned seconds = (argc > 1) ? (unsigned)atoi(argv[1]) : 120;

    telemetry_ring_t   ring;
    telemetry_reader_t logger;

    ring_init(&ring);
    energy_reset();
    ina226_init();
    ring_reader_attach(&ring, &logger, "logger");

    printf("# energy-edge-node telemetry v1\n");
    printf("# source=sim sensor_init=ok rate_hz=%d ring=%d host=1\n",
           SAMPLE_RATE_HZ, TELEMETRY_RING_SIZE);
    printf("# fields: seq,t_ms,v_v,i_a,p_w,wh,flags,truth\n");

    unsigned total = seconds * SAMPLE_RATE_HZ;
    for (unsigned k = 0; k < total; k++) {
        uint32_t t_ms = k * SAMPLE_PERIOD_MS;
        sim_set_time_ms(t_ms);

        ina226_reading_t r;
        telemetry_sample_t s = {};
        s.t_ms  = t_ms;
        s.flags = TLM_FLAG_SIMULATED;

        if (ina226_read(&r)) {
            s.v = r.bus_voltage_v;
            s.i = r.current_a;
            s.p = r.power_w;
        } else {
            s.flags |= TLM_FLAG_SENSOR_FAIL;
        }

        s.wh    = energy_update(s.p, s.t_ms);
        s.truth = ina226_truth_mask();

        ring_push(&ring, &s);

        /* Drain exactly as the firmware's logger task does. */
        telemetry_sample_t o;
        while (ring_read(&ring, &logger, &o)) {
            printf("T,%lu,%lu,%.3f,%.4f,%.3f,%.6f,%u,%u\n",
                   (unsigned long)o.seq, (unsigned long)o.t_ms,
                   o.v, o.i, o.p, o.wh, o.flags, o.truth);
        }
    }

    fprintf(stderr, "# %u samples, %.4f Wh, %lu overruns\n",
            total, energy_wh(), (unsigned long)logger.overruns);
    return 0;
}
#endif /* !ARDUINO */
