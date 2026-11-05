#include "verify.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * verify.cpp -- see verify.h for what this is for and why it waits.
 * -------------------------------------------------------------------------*/

void verify_init(verifier_t *v, uint32_t grace_ms, uint16_t confirm_count)
{
    memset(v, 0, sizeof(*v));
    v->grace_ms      = grace_ms;
    v->confirm_count = confirm_count ? confirm_count : 1;
}

int verify_add_load(verifier_t *v, const char *name, uint8_t bit,
                    bool initial_commanded)
{
    if (v->count >= NILM_MAX_LOADS) return -1;
    if (bit == 0)                   return -1;

    for (int i = 0; i < v->count; i++)
        if (v->loads[i].bit == bit) return -1;

    act_load_t *l = &v->loads[v->count];
    memset(l, 0, sizeof(*l));
    l->name      = name;
    l->bit       = bit;
    l->commanded = initial_commanded;
    l->state     = ACT_UNKNOWN;
    /* cmd_change_ms stays 0: at boot the grace window is measured from the
     * start of time, so the first verdict waits out grace_ms like any other. */
    return v->count++;
}

void verify_step(verifier_t *v, uint8_t commanded, uint8_t inferred,
                 bool nilm_is_confident, uint32_t now_ms)
{
    v->steps++;
    bool judged_any = false;

    for (int i = 0; i < v->count; i++) {
        act_load_t *l   = &v->loads[i];
        bool want       = (commanded & l->bit) != 0;

        /* A fresh command restarts the clock and discards evidence gathered
         * against the previous one. */
        if (want != l->commanded) {
            l->commanded     = want;
            l->cmd_change_ms = now_ms;
            l->disagree      = 0;
            l->state         = ACT_PENDING;
            continue;
        }

        /* Still inside the settle window. Not a verdict, and deliberately not
         * evidence either. */
        if ((uint32_t)(now_ms - l->cmd_change_ms) < v->grace_ms) {
            l->state    = ACT_PENDING;
            l->disagree = 0;
            continue;
        }

        /* An unconfident classifier is not evidence of a hardware fault.
         * Hold the previous verdict rather than inventing one. */
        if (!nilm_is_confident) {
            if (l->state == ACT_PENDING) l->state = ACT_UNKNOWN;
            continue;
        }

        judged_any = true;
        bool measured = (inferred & l->bit) != 0;

        if (measured == want) {
            l->state    = ACT_OK;
            l->disagree = 0;
            continue;
        }

        /* Disagreement. Needs to persist before it is called a fault. */
        if (l->disagree < 0xFFFFu) l->disagree++;
        if (l->disagree < v->confirm_count) {
            if (l->state != ACT_STUCK_ON && l->state != ACT_STUCK_OFF)
                l->state = ACT_PENDING;
            continue;
        }

        act_state_t verdict = want ? ACT_STUCK_OFF : ACT_STUCK_ON;
        if (l->state != verdict) {
            /* Latency is measured from the COMMAND, not from the first
             * disagreement: the number that matters operationally is how long
             * after ordering a shed you find out it never happened. */
            l->detect_latency_ms = (uint32_t)(now_ms - l->cmd_change_ms);
            l->state             = verdict;
            l->faults++;
            v->faults_total++;
        }
    }

    if (judged_any) v->steps_judged++;
}

act_state_t verify_state(const verifier_t *v, uint8_t bit)
{
    for (int i = 0; i < v->count; i++)
        if (v->loads[i].bit == bit) return v->loads[i].state;
    return ACT_UNKNOWN;
}

uint8_t verify_fault_mask(const verifier_t *v)
{
    uint8_t m = LOAD_NONE;
    for (int i = 0; i < v->count; i++)
        if (v->loads[i].state == ACT_STUCK_ON ||
            v->loads[i].state == ACT_STUCK_OFF)
            m |= v->loads[i].bit;
    return m;
}

const char *verify_state_str(act_state_t s)
{
    switch (s) {
    case ACT_UNKNOWN:   return "unknown";
    case ACT_PENDING:   return "pending";
    case ACT_OK:        return "ok";
    case ACT_STUCK_ON:  return "STUCK-ON";
    case ACT_STUCK_OFF: return "STUCK-OFF";
    }
    return "?";
}
