#include "scheduler.h"
#include <string.h>
#include <math.h>

void sched_init(scheduler_t *s, demand_window_t *dw, float target_w)
{
    memset(s, 0, sizeof(*s));
    s->dw               = dw;
    s->target_w         = target_w;
    s->restore_fraction = 0.90f;   /* hysteresis: without a gap between the
                                    * shed and restore thresholds the relay
                                    * chatters at the boundary */
    s->stagger_ms       = 5000;
}

int sched_add_load(scheduler_t *s, const sched_load_t *proto)
{
    if (s->count >= SCHED_MAX_LOADS) return -1;
    sched_load_t *l = &s->loads[s->count];
    *l = *proto;
    l->on             = true;      /* loads start energised; the scheduler
                                    * earns the right to turn them off */
    l->shed           = false;
    l->last_change_ms = 0;
    return s->count++;
}

void sched_set_job(scheduler_t *s, int idx, float wh, uint32_t deadline_ms)
{
    if (idx < 0 || idx >= s->count) return;
    sched_load_t *l = &s->loads[idx];
    l->energy_required_wh  = wh;
    l->energy_delivered_wh = 0.0f;
    l->deadline_ms         = deadline_ms;
    l->deadline_missed     = false;
}

float sched_slack_s(const sched_load_t *l, uint32_t now_ms)
{
    if (l->cls != LOAD_CLASS_DEFERRABLE) return 1e9f;   /* effectively infinite */

    float remaining_wh = l->energy_required_wh - l->energy_delivered_wh;
    if (remaining_wh <= 0.0f) return 1e9f;              /* job already done */
    if (l->rated_w <= 0.0f)   return 0.0f;

    float need_s = (remaining_wh / l->rated_w) * 3600.0f;

    /* Signed: the deadline can be in the past. */
    float left_s = ((float)(int32_t)(l->deadline_ms - now_ms)) / 1000.0f;
    return left_s - need_s;
}

bool sched_is_critical(const sched_load_t *l, uint32_t now_ms)
{
    return sched_slack_s(l, now_ms) <= SCHED_SLACK_MARGIN_S;
}

const char *sched_class_str(load_class_t c)
{
    switch (c) {
    case LOAD_CLASS_FIXED:      return "fixed";
    case LOAD_CLASS_DEFERRABLE: return "deferrable";
    case LOAD_CLASS_FLEXIBLE:   return "flexible";
    }
    return "?";
}

uint8_t sched_mask(const scheduler_t *s)
{
    uint8_t m = 0;
    for (int k = 0; k < s->count; k++)
        if (s->loads[k].on) m |= s->loads[k].bit;
    return m;
}

/* --- eligibility -------------------------------------------------------- */

static bool may_shed(const sched_load_t *l, uint32_t now_ms)
{
    if (l->cls == LOAD_CLASS_FIXED) return false;
    if (!l->on)                     return false;
    if (sched_is_critical(l, now_ms)) return false;   /* no slack left */

    if (l->min_on_ms && (now_ms - l->last_change_ms) < l->min_on_ms)
        return false;                                 /* protect the hardware */

    /* Still recovering from a forced restore. Signed compare so the test
     * survives the millis() rollover. */
    if (l->protect_until_ms && (int32_t)(now_ms - l->protect_until_ms) < 0)
        return false;

    return true;
}

static bool may_restore(const sched_load_t *l, uint32_t now_ms)
{
    if (l->on)    return false;
    if (!l->shed) return false;   /* off for its own reasons, not ours */

    if (l->min_off_ms && (now_ms - l->last_change_ms) < l->min_off_ms)
        return false;

    return true;
}

/* A load we are no longer permitted to keep off, whatever the demand target.
 * Equipment protection and deadlines outrank cost. */
static bool must_restore(const sched_load_t *l, uint32_t now_ms)
{
    if (l->on || !l->shed) return false;

    if (sched_is_critical(l, now_ms)) return true;

    if (l->max_shed_ms && (now_ms - l->shed_since_ms) >= l->max_shed_ms)
        return true;

    return false;
}

static void set_state(scheduler_t *s, sched_load_t *l, bool on, bool by_us,
                      uint32_t now_ms)
{
    if (l->on == on) return;

    l->on             = on;
    l->last_change_ms = now_ms;

    if (!on && by_us) {
        l->shed         = true;
        l->shed_since_ms = now_ms;
        l->shed_count++;
        s->sheds++;
    } else if (on) {
        if (l->shed) {
            l->total_shed_ms += (now_ms - l->shed_since_ms);
            s->restores++;
        }
        l->shed = false;
    }
}

/* --- energy accounting --------------------------------------------------
 * Each energised load is credited at its rated power. We do not have per-load
 * metering -- that is the whole retrofit premise -- so delivery is inferred
 * from the commanded state and the nameplate rating.
 *
 * That inference is exactly what Phase 5 exists to verify: if a relay welds
 * shut or a channel is miswired, this accounting silently drifts from reality
 * and nothing else in the system would notice. */
static void credit_energy(scheduler_t *s, uint32_t dt_ms)
{
    if (!dt_ms) return;
    float hours = (float)dt_ms / 3600000.0f;

    for (int k = 0; k < s->count; k++) {
        sched_load_t *l = &s->loads[k];
        if (l->cls != LOAD_CLASS_DEFERRABLE || !l->on) continue;

        /* Stop crediting once the job is done. Otherwise a load left
         * energised after it finished keeps accruing phantom energy, and a
         * load with no job at all (required == 0) accrues it from boot. */
        if (l->energy_delivered_wh >= l->energy_required_wh) continue;

        l->energy_delivered_wh += l->rated_w * hours;
    }
}

uint8_t sched_step(scheduler_t *s, float measured_w, uint32_t now_ms)
{
    static_assert(SCHED_MAX_LOADS <= 8, "mask is 8 bits wide");

    /* 1. Demand window, from the real measurement. */
    dw_update(s->dw, measured_w, now_ms);

    /* 2. Credit energy delivered since the last step, to every deferrable
     *    load that was energised for it. */
    if (s->stepped) {
        uint32_t dt = now_ms - s->last_step_ms;      /* unsigned: wrap-safe */
        if (dt > 0 && dt < 3600000u) credit_energy(s, dt);
    }
    s->last_step_ms = now_ms;
    s->stepped      = true;

    /* 3. Deadline accounting: a miss is recorded once, when it happens. */
    for (int k = 0; k < s->count; k++) {
        sched_load_t *l = &s->loads[k];
        if (l->cls != LOAD_CLASS_DEFERRABLE || l->deadline_missed) continue;

        bool past   = (int32_t)(now_ms - l->deadline_ms) >= 0;
        bool unmet  = l->energy_delivered_wh < l->energy_required_wh - 1e-6f;
        if (past && unmet) {
            l->deadline_missed = true;
            s->deadline_misses++;
        }
    }

    /* 4. Forced restores first -- these override the demand target entirely. */
    for (int k = 0; k < s->count; k++) {
        sched_load_t *l = &s->loads[k];
        if (must_restore(l, now_ms)) {
            uint32_t recover = l->recovery_ms ? l->recovery_ms : l->max_shed_ms;
            set_state(s, l, true, true, now_ms);
            l->protect_until_ms = now_ms + recover;
            s->forced_restores++;
        }
    }

    /* 5. Do we need to shed? Act on the PROJECTION, not the present value. */
    float excess = dw_excess_w(s->dw, measured_w, now_ms, s->target_w);

    if (excess > 0.0f) {
        /* Shed one load per step, then let the next measurement show the
         * effect. Shedding everything at once overshoots and causes a
         * thrash of restores afterwards. */
        int best = -1;
        float best_slack = -1.0f;

        for (int k = 0; k < s->count; k++) {
            sched_load_t *l = &s->loads[k];
            if (!may_shed(l, now_ms)) continue;

            float slack = sched_slack_s(l, now_ms);

            /* Most slack first; priority breaks ties. */
            if (best < 0 || slack > best_slack ||
                (slack == best_slack && l->priority < s->loads[best].priority)) {
                best = k;
                best_slack = slack;
            }
        }

        if (best >= 0) set_state(s, &s->loads[best], false, true, now_ms);
        else           s->refusals++;   /* over target and nothing may be shed */
    }
    /* 6. Otherwise, is there room to put something back? */
    else {
        float projected = dw_projected_w(s->dw, measured_w, now_ms);

        if (projected < s->target_w * s->restore_fraction &&
            (now_ms - s->last_restore_ms) >= s->stagger_ms) {

            int best = -1;
            float best_slack = 0.0f;

            for (int k = 0; k < s->count; k++) {
                sched_load_t *l = &s->loads[k];
                if (!may_restore(l, now_ms)) continue;

                /* Would putting this back breach the target again? */
                float after = dw_projected_w(s->dw, measured_w + l->rated_w, now_ms);
                if (after > s->target_w) continue;

                /* LEAST slack first -- the most urgent job comes back soonest. */
                float slack = sched_slack_s(l, now_ms);
                if (best < 0 || slack < best_slack) {
                    best = k;
                    best_slack = slack;
                }
            }

            if (best >= 0) {
                set_state(s, &s->loads[best], true, true, now_ms);
                s->last_restore_ms = now_ms;   /* stagger: one per interval */
            }
        }
    }

    return sched_mask(s);
}
