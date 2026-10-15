#include "demand_window.h"
#include <string.h>
#include <math.h>

#define BUCKET_MASK (DEMAND_BUCKETS - 1)
static_assert((DEMAND_BUCKETS & BUCKET_MASK) == 0,
              "DEMAND_BUCKETS must be a power of two");

void dw_init(demand_window_t *d, uint32_t window_ms)
{
    memset(d, 0, sizeof(*d));
    d->window_ms = window_ms ? window_ms : 1;
    d->bucket_ms = d->window_ms / DEMAND_BUCKETS;
    if (d->bucket_ms == 0) d->bucket_ms = 1;
}

/* Drop every bucket that has aged out of the window. Called before each
 * accumulation so the sum only ever covers the last window_ms. */
static void advance_to(demand_window_t *d, uint32_t t_ms)
{
    uint32_t idx = t_ms / d->bucket_ms;

    if (!d->primed) {
        d->bucket_index = idx;
        d->primed = true;
        return;
    }
    if (idx == d->bucket_index) return;

    uint32_t steps = idx - d->bucket_index;
    if (steps >= DEMAND_BUCKETS) {
        /* Gap longer than the whole window: everything held is stale. */
        memset(d->wh, 0, sizeof(d->wh));
    } else {
        for (uint32_t k = 1; k <= steps; k++)
            d->wh[(d->bucket_index + k) & BUCKET_MASK] = 0.0;
    }
    d->bucket_index = idx;
}

void dw_update(demand_window_t *d, float power_w, uint32_t t_ms)
{
    if (!isfinite(power_w) || power_w < 0.0f) power_w = 0.0f;

    advance_to(d, t_ms);

    if (d->last_ms != 0 || d->last_w != 0.0f) {
        uint32_t dt_ms = t_ms - d->last_ms;      /* unsigned: survives wrap */

        /* Ignore an implausible gap rather than injecting a huge phantom
         * energy slug that would poison the average for a whole window. */
        if (dt_ms > 0 && dt_ms <= d->window_ms) {
            double avg_w = 0.5 * ((double)d->last_w + (double)power_w);
            d->wh[d->bucket_index & BUCKET_MASK] +=
                avg_w * ((double)dt_ms / 3600000.0);
        }
    }

    d->last_ms = t_ms;
    d->last_w  = power_w;

    float avg = dw_average_w(d);
    d->last_avg_w = avg;
    if (avg > d->peak_avg_w) d->peak_avg_w = avg;
}

float dw_average_w(const demand_window_t *d)
{
    double total = 0.0;
    for (int k = 0; k < DEMAND_BUCKETS; k++) total += d->wh[k];

    /* Average over the FULL window, not over elapsed time. Energy not yet
     * drawn counts as zero, which is what a utility meter does -- a window
     * that is half over with no load has a genuinely low average. */
    return (float)(total * 3600000.0 / (double)d->window_ms);
}

uint32_t dw_time_remaining_ms(const demand_window_t *d, uint32_t t_ms)
{
    if (!d->primed) return d->window_ms;
    uint32_t elapsed = t_ms % d->window_ms;
    return d->window_ms - elapsed;
}

float dw_projected_w(const demand_window_t *d, float power_w, uint32_t t_ms)
{
    if (!isfinite(power_w) || power_w < 0.0f) power_w = 0.0f;

    double total = 0.0;
    for (int k = 0; k < DEMAND_BUCKETS; k++) total += d->wh[k];

    uint32_t remain = dw_time_remaining_ms(d, t_ms);
    double future_wh = (double)power_w * ((double)remain / 3600000.0);

    return (float)((total + future_wh) * 3600000.0 / (double)d->window_ms);
}

float dw_excess_w(const demand_window_t *d, float power_w, uint32_t t_ms,
                  float target_w)
{
    float projected = dw_projected_w(d, power_w, t_ms);
    if (projected <= target_w) return 0.0f;

    uint32_t remain = dw_time_remaining_ms(d, t_ms);
    if (remain == 0) return 0.0f;            /* nothing left to influence */

    /* Removing P watts for the remaining time lowers the window average by
     * P * remain / window. Invert that for the P we need. As `remain` shrinks
     * the required P grows -- shedding late costs more, which is exactly why
     * the controller acts on the forecast rather than waiting. */
    float needed = (projected - target_w) * (float)d->window_ms / (float)remain;
    return needed > 0.0f ? needed : 0.0f;
}

void dw_reset_peak(demand_window_t *d) { d->peak_avg_w = 0.0f; }
