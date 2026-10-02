#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * ina226_hal.h -- the only way the rest of the firmware touches a sensor.
 *
 * Two implementations satisfy this interface:
 *   ina226_real.cpp  -- the actual chip over I2C
 *   ina226_sim.cpp   -- a synthetic DC testbed with three "appliances"
 *
 * The build picks one (-D USE_INA226_SIM). Nothing above this line changes.
 * That is deliberate: it means the entire telemetry / Modbus / MQTT / control
 * stack can be written and tested before the hardware arrives, and later it
 * gives Phase 5 a NILM test rig with known ground truth.
 * -------------------------------------------------------------------------*/

typedef struct {
    float bus_voltage_v;
    float current_a;
    float power_w;
} ina226_reading_t;

bool ina226_init(void);
bool ina226_read(ina226_reading_t *out);

/* True when the simulator backend is compiled in. The sampler uses this to
 * stamp TLM_FLAG_SIMULATED, so a log can never be mistaken for real data. */
bool ina226_is_simulated(void);

/* LOAD_* bitmask of what is really drawing power right now.
 * Simulator: the true answer -- NILM ground truth.
 * Hardware:  LOAD_NONE. Recovering this from the aggregate IS Phase 5. */
uint8_t ina226_truth_mask(void);
