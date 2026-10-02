#include "energy.h"

static double   s_wh     = 0.0;   /* double: this runs for hours and the
                                   * per-sample increment is ~1e-6 Wh, which
                                   * float32 would quietly round away */
static uint32_t s_last_ms = 0;
static float    s_last_p  = 0.0f;
static bool     s_primed  = false;

void energy_reset(void)
{
    s_wh = 0.0; s_last_ms = 0; s_last_p = 0.0f; s_primed = false;
}

float energy_update(float power_w, uint32_t t_ms)
{
    if (!s_primed) {                 /* first sample defines t0 only */
        s_primed  = true;
        s_last_ms = t_ms;
        s_last_p  = power_w;
        return (float)s_wh;
    }

    uint32_t dt_ms = t_ms - s_last_ms;   /* unsigned: survives millis() wrap */

    /* Trapezoid: average of the two endpoints over the interval. */
    double avg_w    = 0.5 * ((double)s_last_p + (double)power_w);
    s_wh           += avg_w * ((double)dt_ms / 3600000.0);

    s_last_ms = t_ms;
    s_last_p  = power_w;
    return (float)s_wh;
}

float energy_wh(void) { return (float)s_wh; }
