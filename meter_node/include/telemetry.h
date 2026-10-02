#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * telemetry.h -- the one data structure every layer of this project agrees on.
 *
 * The sampler produces these. The logger, the Modbus slave, the MQTT gateway,
 * the demand-response controller and (eventually) the NILM classifier all
 * consume them from the same ring buffer. Nothing downstream ever talks to the
 * sensor directly, so adding a consumer never touches the sampling path.
 * -------------------------------------------------------------------------*/

typedef struct {
    uint32_t seq;     /* monotonic sample number, never resets -- lets a
                       * reader detect exactly how many samples it missed */
    uint32_t t_ms;    /* milliseconds since boot                           */
    float    v;       /* bus voltage, volts                                */
    float    i;       /* current, amps                                     */
    float    p;       /* power, watts (measured, not v*i -- the INA226 has
                       * a dedicated power register that is more accurate) */
    float    wh;      /* cumulative energy since boot, watt-hours          */
    uint8_t  flags;   /* TLM_FLAG_* bits                                   */
    uint8_t  truth;   /* LOAD_* bitmask: which appliances were really on.
                       * Simulator fills this in; real hardware leaves it 0.
                       * Phase 5 scores its guess against this column.      */
} telemetry_sample_t;

/* Which loads are on the bus. A bitmask rather than a string so a sample is
 * self-contained and safe to copy between tasks -- and because an on/off
 * vector is exactly what a NILM classifier emits, so Phase 5 compares its
 * output against this field directly. */
#define LOAD_NONE    0x00
#define LOAD_LIGHTS  0x01
#define LOAD_HEATER  0x02
#define LOAD_MOTOR   0x04

/* Render a mask as e.g. "heater+motor" into buf. Always NUL-terminates. */
void telemetry_truth_str(uint8_t mask, char *buf, unsigned len);
