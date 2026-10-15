#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * demand_window.h -- the rolling demand average a utility actually bills on.
 *
 * WHY THIS EXISTS
 *   Commercial tariffs do not charge for instantaneous watts. They charge for
 *   the highest AVERAGE power over a fixed interval -- typically 15 minutes --
 *   seen anywhere in the billing period. A 2 kW spike lasting three seconds
 *   costs nothing. A 2 kW load held for the whole interval sets the bill for
 *   the month.
 *
 *   So a controller that reacts to instantaneous power is solving the wrong
 *   problem. It will shed for harmless spikes and stay idle through the slow
 *   creep that actually costs money.
 *
 * THE USEFUL PART: PROJECTION
 *   Knowing the average SO FAR is not enough either -- by the time the average
 *   crosses the limit, the interval is nearly over and shedding cannot pull it
 *   back. What matters is where the average will LAND when the interval closes
 *   if nothing changes:
 *
 *       projected = (energy_so_far + current_power * time_remaining)
 *                   / window_duration
 *
 *   Acting on that forecast is the difference between a demand controller and
 *   a thermostat. It is also why shedding late is worth less than shedding
 *   early: with little time left, removing a load barely moves the average.
 *
 * IMPLEMENTATION
 *   Energy is accumulated into fixed time buckets rather than stored per
 *   sample. 60 buckets over a 60 s window is 1 s of resolution for ~500 bytes,
 *   against ~24 kB to keep every sample at 10 Hz. The error is bounded by one
 *   bucket's width and is irrelevant next to the forecast's own uncertainty.
 *
 * FIXED vs ROLLING
 *   Real tariffs mostly use FIXED intervals that reset on the clock (:00, :15,
 *   :30, :45). A ROLLING window is stricter -- it is the worst fixed window
 *   that could have been drawn -- so a controller that satisfies the rolling
 *   average satisfies any fixed one. We use rolling, and say so.
 * -------------------------------------------------------------------------*/

#ifndef DEMAND_BUCKETS
#define DEMAND_BUCKETS 64          /* power of two; see dw_init for sizing */
#endif

typedef struct {
    uint32_t window_ms;            /* the billing interval being modelled    */
    uint32_t bucket_ms;            /* window_ms / DEMAND_BUCKETS             */

    double   wh[DEMAND_BUCKETS];   /* energy accumulated per bucket          */
    uint32_t bucket_index;         /* absolute bucket number, never wraps    */
    bool     primed;

    uint32_t last_ms;              /* for trapezoidal integration            */
    float    last_w;

    /* Observed results, for reporting. */
    float    peak_avg_w;           /* worst window average ever seen         */
    float    last_avg_w;
    float    last_projected_w;
} demand_window_t;

/* window_ms is the billing interval. On a bench it is scaled down (60 s
 * standing in for 15 minutes) so a demo completes in real time. */
void dw_init(demand_window_t *d, uint32_t window_ms);

/* Feed one power reading. Integrates trapezoidally over real elapsed time, so
 * an irregular sample rate does not distort the average. */
void dw_update(demand_window_t *d, float power_w, uint32_t t_ms);

/* Average power over the window so far, in watts. */
float dw_average_w(const demand_window_t *d);

/* Milliseconds until the current window closes. */
uint32_t dw_time_remaining_ms(const demand_window_t *d, uint32_t t_ms);

/* Where the window average will land at close if power stays at power_w. */
float dw_projected_w(const demand_window_t *d, float power_w, uint32_t t_ms);

/* How many watts must be removed NOW for the projection to land on target.
 * Returns 0 when already on course.
 *
 * Note the time term: shedding with little of the window left has to remove
 * proportionally more power to move the average by the same amount. */
float dw_excess_w(const demand_window_t *d, float power_w, uint32_t t_ms,
                  float target_w);

void dw_reset_peak(demand_window_t *d);
