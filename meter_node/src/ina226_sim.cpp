#include "ina226_hal.h"
#ifdef USE_INA226_SIM

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "telemetry.h"

/* ---------------------------------------------------------------------------
 * ina226_sim.cpp -- a synthetic 12 V DC testbed.
 *
 * Three "appliances" on a shared bus, each with a deliberately distinct
 * electrical signature:
 *
 *   LIGHTS  5 W   clean step, no transient            (LED strip)
 *   HEATER  40 W  step, then a slow droop as the element heats and its
 *                 resistance rises                     (power resistor)
 *   MOTOR   18 W  ~3.5x inrush decaying over ~250 ms, then a steady
 *                 commutation ripple                   (small DC motor)
 *
 * Those three behaviours are exactly what a NILM classifier keys off in
 * Phase 5, which is the point: this file is both a stand-in for missing
 * hardware today AND the labelled training rig later. ina226_truth_mask()
 * is the ground truth no real sensor can ever give you.
 *
 * Everything is deterministic -- a fixed schedule and a seeded PRNG -- so two
 * runs produce identical logs and a regression is always a real regression.
 * -------------------------------------------------------------------------*/

#define BUS_NOMINAL_V    12.0f
#define BUS_SAG_OHMS     0.18f   /* supply + wiring resistance */

#define P_LIGHTS         5.0f
#define P_HEATER         40.0f
#define P_MOTOR          18.0f

#define MOTOR_INRUSH_X   3.5f
#define MOTOR_INRUSH_TAU 0.08f   /* seconds */
#define MOTOR_RIPPLE_HZ  7.0f    /* commutation ripple */
#define TWO_PI_F         6.28318531f
#define HEATER_DROOP     0.06f   /* 6% power fade as it warms */
#define HEATER_WARM_TAU  2.0f    /* seconds */

#define SIM_CYCLE_MS     60000u

/* --- deterministic noise ------------------------------------------------ */
static uint32_t s_rng = 0xC0FFEEu;
static float noise(float amplitude)
{
    s_rng = s_rng * 1664525u + 1013904223u;          /* Numerical Recipes LCG */
    float u = (float)((s_rng >> 8) & 0xFFFF) / 65535.0f;  /* 0..1 */
    return (u - 0.5f) * 2.0f * amplitude;
}

/* --- clock -------------------------------------------------------------- */
#ifdef ARDUINO
  #include <Arduino.h>
  static uint32_t sim_now_ms(void) { return millis(); }
#else
  /* Off-target the clock is driven explicitly, so a host run generates a
   * capture as fast as the CPU allows instead of in real time -- and two
   * runs produce byte-identical output. */
  static uint32_t s_virtual_ms = 0;
  void sim_set_time_ms(uint32_t ms) { s_virtual_ms = ms; }
  static uint32_t sim_now_ms(void)  { return s_virtual_ms; }
#endif

/* --- appliance schedule (repeats every SIM_CYCLE_MS) -------------------- */
/* Chosen so the three loads overlap into a ~63 W peak around t=25..38 s --
 * comfortably above the Phase 3 shed threshold, so the demand-response demo
 * has something to actually react to. */
typedef struct { uint32_t on_ms, off_ms; } window_t;
static const window_t W_LIGHTS = {  5000, 55000 };
static const window_t W_MOTOR  = { 12000, 38000 };
static const window_t W_HEATER = { 25000, 48000 };

static bool in_window(uint32_t t, const window_t *w)
{ return t >= w->on_ms && t < w->off_ms; }

static uint8_t s_mask = LOAD_NONE;

bool ina226_init(void)
{
    s_rng = 0xC0FFEEu;
    return true;
}

bool ina226_read(ina226_reading_t *out)
{
    uint32_t t = sim_now_ms() % SIM_CYCLE_MS;
    float    p = 0.0f;
    s_mask     = LOAD_NONE;

    if (in_window(t, &W_LIGHTS)) {
        p      += P_LIGHTS + noise(0.05f);
        s_mask |= LOAD_LIGHTS;
    }

    if (in_window(t, &W_HEATER)) {
        /* Resistance rises with temperature, so power droops toward a floor. */
        float warm = 1.0f - expf(-(float)(t - W_HEATER.on_ms) / 1000.0f
                                 / HEATER_WARM_TAU);
        p      += P_HEATER * (1.0f - HEATER_DROOP * warm) + noise(0.15f);
        s_mask |= LOAD_HEATER;
    }

    if (in_window(t, &W_MOTOR)) {
        float dt     = (float)(t - W_MOTOR.on_ms) / 1000.0f;
        float inrush = 1.0f + (MOTOR_INRUSH_X - 1.0f) * expf(-dt / MOTOR_INRUSH_TAU);
        float ripple = 1.0f + 0.08f * sinf(dt * TWO_PI_F * MOTOR_RIPPLE_HZ);
        p      += P_MOTOR * inrush * ripple + noise(0.20f);
        s_mask |= LOAD_MOTOR;
    }

    if (p < 0.0f) p = 0.0f;

    /* Bus sags under load. Solve V and I together: estimate the current at
     * nominal volts, apply the sag, then correct the current for the lower
     * bus. One pass is well inside the sensor's own accuracy. */
    float i_est = p / BUS_NOMINAL_V;
    float v     = BUS_NOMINAL_V - BUS_SAG_OHMS * i_est + noise(0.01f);
    float i     = (v > 0.1f) ? (p / v) : 0.0f;

    out->bus_voltage_v = v;
    out->current_a     = i;
    out->power_w       = p;
    return true;
}

bool    ina226_is_simulated(void) { return true; }
uint8_t ina226_truth_mask(void)   { return s_mask; }

#endif /* USE_INA226_SIM */
