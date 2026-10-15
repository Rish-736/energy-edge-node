#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "demand_window.h"
#include "telemetry.h"   /* LOAD_* mask bits */

/* ---------------------------------------------------------------------------
 * scheduler.h -- deadline-aware load scheduling.
 *
 * THE QUESTION THIS ASKS
 *   Not "whose service do I cut?" but:
 *
 *       Each load needs E watt-hours by time T.
 *       Find a schedule that meets EVERY deadline
 *       while keeping the rolling demand average as low as possible.
 *
 *   Deadlines are hard constraints. Peak is the objective. That inversion is
 *   the whole design: service becomes a guarantee rather than something traded
 *   for a smaller bill.
 *
 * SLACK IS THE RESOURCE
 *   For a deferrable load:
 *
 *       energy_remaining = required - delivered
 *       time_needed      = energy_remaining / rated_power
 *       slack            = (deadline - now) - time_needed
 *
 *   Slack is how long the load can stay off and still finish on time. A car
 *   plugged in at 19:00 needing 4 hours of charge by 07:00 has 8 hours of
 *   slack. That slack is what the controller spends, and when it reaches zero
 *   the load becomes untouchable -- it must run to the deadline.
 *
 *   This also makes eligibility DYNAMIC. Priorities never change, but which
 *   load may be shed right now changes second to second as slack drains.
 *
 * WHY SLACK BEATS A STATIC PRIORITY NUMBER
 *   A fixed priority list always sheds the same load first, even when that
 *   load is the one about to miss its deadline. Sorting by slack adapts: the
 *   load with the most room to spare goes first, and one running out of time
 *   is protected automatically rather than by a special case.
 *
 * WHAT IT REFUSES TO DO
 *   - shed a FIXED load, ever
 *   - shed a load with no slack left
 *   - shed a load that has not met its minimum on-time
 *   - restore a load that has not met its minimum off-time
 *   - restore more than one load per stagger interval, because releasing
 *     several at once creates a brand-new peak (motor inrush included), which
 *     is the classic way a naive controller makes things worse
 * -------------------------------------------------------------------------*/

#ifndef SCHED_MAX_LOADS
#define SCHED_MAX_LOADS 8
#endif

/* Safety buffer on slack, in seconds.
 *
 * Two reasons. Physically, a job should finish slightly early rather than
 * exactly on the deadline -- arriving with zero margin means any disturbance
 * makes it late. Numerically, slack is a float difference of two floats, so a
 * load with "exactly zero" slack lands either side of zero depending on
 * rounding, and the scheduler would shed it on one step and protect it on the
 * next. Treating anything below the margin as critical removes both problems. */
#ifndef SCHED_SLACK_MARGIN_S
#define SCHED_SLACK_MARGIN_S 1.0f
#endif

typedef enum {
    LOAD_CLASS_FIXED = 0,   /* never shed: lighting, controls, metering     */
    LOAD_CLASS_DEFERRABLE,  /* needs E by T; free to move inside its slack  */
    LOAD_CLASS_FLEXIBLE,    /* no hard deadline, but a bounded off-time     */
} load_class_t;

typedef struct {
    const char  *name;
    uint8_t      bit;             /* LOAD_* mask bit, for telemetry/NILM    */
    load_class_t cls;
    float        rated_w;

    /* DEFERRABLE only. */
    float    energy_required_wh;
    uint32_t deadline_ms;         /* absolute, same clock as update()       */
    float    energy_delivered_wh; /* maintained by the scheduler            */

    /* Equipment protection. Violating these damages hardware, so they
     * override the demand target. */
    uint32_t min_on_ms;
    uint32_t min_off_ms;
    uint32_t max_shed_ms;         /* 0 = unlimited. A crude proxy for buffer
                                   * state, used when the real state (tank
                                   * temperature, state of charge) cannot be
                                   * measured. */
    uint32_t recovery_ms;         /* after a FORCED restore, the load must run
                                   * at least this long before it may be shed
                                   * again. 0 = use max_shed_ms.
                                   *
                                   * Without this the scheduler force-restores
                                   * a load and then immediately re-sheds it on
                                   * the same step, cycling the equipment and
                                   * defeating max_shed_ms entirely. A buffer
                                   * that was drained needs time to refill. */

    uint8_t  priority;            /* tie-break only; lower sheds first      */

    /* --- runtime state, owned by the scheduler --------------------------- */
    bool     on;
    bool     shed;                /* off because WE shed it, not because it
                                   * finished or was never wanted           */
    uint32_t last_change_ms;
    uint32_t shed_since_ms;
    uint32_t protect_until_ms;    /* set by a forced restore; see recovery_ms */
    uint32_t total_shed_ms;
    uint32_t shed_count;
    bool     deadline_missed;
} sched_load_t;

typedef struct {
    sched_load_t loads[SCHED_MAX_LOADS];
    int          count;

    demand_window_t *dw;

    float    target_w;            /* the demand ceiling we are defending    */
    float    restore_fraction;    /* hysteresis: restore below target*this  */
    uint32_t stagger_ms;          /* minimum gap between restores           */
    uint32_t last_restore_ms;

    uint32_t last_step_ms;        /* for crediting delivered energy         */
    bool     stepped;

    /* --- results ---------------------------------------------------------
     * Reported alongside kW and currency. A demand-response result quoted
     * without its service cost is incomplete. */
    uint32_t sheds;
    uint32_t restores;
    uint32_t forced_restores;     /* slack ran out while shed               */
    uint32_t deadline_misses;     /* the number that must stay zero         */
    uint32_t refusals;            /* shed wanted but nothing was eligible   */
} scheduler_t;

void sched_init(scheduler_t *s, demand_window_t *dw, float target_w);

/* Returns the load's index, or -1 if full. */
int  sched_add_load(scheduler_t *s, const sched_load_t *proto);

/* Declare a new job for a deferrable load: deliver `wh` by `deadline_ms`.
 * Resets delivered energy and clears any previous miss. */
void sched_set_job(scheduler_t *s, int idx, float wh, uint32_t deadline_ms);

/* Seconds of slack, negative when the load can no longer finish in time. */
float sched_slack_s(const sched_load_t *l, uint32_t now_ms);

/* True when the load must run now to meet its deadline. */
bool sched_is_critical(const sched_load_t *l, uint32_t now_ms);

/* Run one control step.
 *
 * `measured_w` is the site's actual total power; it drives the demand window
 * and the per-load energy accounting. Call at a steady rate.
 *
 * Returns the bitmask of loads that should be ENERGISED. The caller drives
 * relays from it -- the scheduler never touches hardware, which is what makes
 * it testable on a PC. */
uint8_t sched_step(scheduler_t *s, float measured_w, uint32_t now_ms);

/* Current commanded mask without advancing anything. */
uint8_t sched_mask(const scheduler_t *s);

const char *sched_class_str(load_class_t c);
