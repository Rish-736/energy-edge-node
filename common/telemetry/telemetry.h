#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * telemetry.h -- the one data structure every layer of this project agrees on.
 *
 * Shared by BOTH nodes. The meter produces these from its sensor; the gateway
 * produces them from Modbus polls. Both then feed the same ring buffer, so the
 * logger, the Modbus register map, the MQTT publisher, the demand-response
 * controller and (eventually) the NILM classifier are all consumers of one
 * format at both tiers of the architecture.
 *
 * Keeping one struct means the store-and-forward buffer on the gateway is the
 * same code as the sampling buffer on the meter, already tested.
 * -------------------------------------------------------------------------*/

typedef struct {
    uint32_t seq;        /* monotonic sample number, never resets -- lets a
                          * reader detect exactly how many samples it missed */
    uint32_t t_ms;       /* milliseconds since the producing node booted      */
    uint64_t t_unix_ms;  /* wall-clock time in ms since the epoch, or 0 when
                          * unknown. The meter has no clock and leaves this 0;
                          * the gateway fills it in from SNTP. Billing, tariff
                          * windows and cross-device correlation all need real
                          * time, and "ms since boot" cannot provide it.      */
    float    v;          /* bus voltage, volts                                */
    float    i;          /* current, amps                                     */
    float    p;          /* power, watts (measured, not v*i -- the INA226 has
                          * a dedicated power register that is more accurate) */
    float    wh;         /* cumulative energy since boot, watt-hours          */
    uint8_t  flags;      /* TLM_FLAG_* bits                                   */
    uint8_t  truth;      /* LOAD_* bitmask: which appliances were really on.
                          * Simulator fills this in; real hardware leaves 0.
                          * Phase 5 scores its guess against this column.     */
} telemetry_sample_t;

/* --- status flags ------------------------------------------------------- */
/* Part of the data contract rather than per-node configuration: the gateway
 * has to interpret flags the meter set, so both ends need the same values. */
#define TLM_FLAG_OK           0x00
#define TLM_FLAG_SENSOR_FAIL  0x01  /* sensor read failed, values are stale   */
#define TLM_FLAG_OVER_POWER   0x02  /* above the demand-response threshold    */
#define TLM_FLAG_BACKFILL     0x04  /* delivered late, after a link outage    */
#define TLM_FLAG_SIMULATED    0x80  /* data came from the simulator, not iron */

/* --- which loads are on the bus ----------------------------------------- */
/* A bitmask rather than a string so a sample is self-contained and safe to
 * copy between tasks -- and because an on/off vector is exactly what a NILM
 * classifier emits, so Phase 5 compares its output against this directly. */
#define LOAD_NONE     0x00
#define LOAD_BASE     0x01   /* lighting, controls -- never shed            */
#define LOAD_CHARGER  0x02   /* EV charger -- deferrable, has a deadline    */
#define LOAD_THERMAL  0x04   /* cooling/pumping -- flexible within a band   */

/* Render a mask as e.g. "charger+thermal" into buf. Always NUL-terminates. */
void telemetry_truth_str(uint8_t mask, char *buf, unsigned len);
