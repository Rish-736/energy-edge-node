#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * energy.h -- turn a stream of instantaneous power readings into accumulated
 * energy (watt-hours), which is the thing you are actually billed for.
 *
 * Integrates with the trapezoidal rule over the real elapsed time between
 * samples, not the nominal period. If a task overruns or the scheduler is
 * busy, the energy figure stays honest instead of silently drifting -- which
 * matters, because every cost and CO2 number downstream is derived from it.
 * -------------------------------------------------------------------------*/

void   energy_reset(void);

/* Feed one sample. Returns cumulative energy in watt-hours. */
float  energy_update(float power_w, uint32_t t_ms);

float  energy_wh(void);
