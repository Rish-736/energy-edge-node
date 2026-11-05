#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "nilm.h"

/* ---------------------------------------------------------------------------
 * verify.h -- did the relay actually do what it was told?
 *
 * THE GAP THIS CLOSES
 *   The scheduler decides to shed a load and sets a pin. Pin set, done.
 *
 *   Except a relay contact can weld shut, a spade terminal can vibrate off, a
 *   channel can be miswired, and a load can simply be unplugged. In every one
 *   of those cases the controller believes the shed happened. Worse, it keeps
 *   crediting delivered energy from the commanded state and the nameplate
 *   rating, so its slack arithmetic quietly drifts away from reality and its
 *   demand target is defended by a load that never stopped drawing.
 *
 *   The relay's own state cannot detect this. A welded contact still reads
 *   "commanded off" -- that is what makes it dangerous. Even an auxiliary
 *   contact only reports the armature position, not whether current flows.
 *
 *   So the command is checked against an INDEPENDENT measurement: what the
 *   meter says is actually drawing, via nilm.h.
 *
 *       commanded   measured    verdict
 *       ---------   --------    -------
 *       off         off         ACT_OK
 *       off         DRAWING     ACT_STUCK_ON   <- welded contact; the shed
 *                                                 did nothing and the demand
 *                                                 target is unprotected
 *       on          NOTHING     ACT_STUCK_OFF  <- failed open, miswired, or
 *                                                 the load is unplugged
 *
 * WHY IT NEEDS TO BE PATIENT
 *   Two reasons a disagreement is not yet a fault:
 *
 *   1. A command takes time to show up. The relay settles in milliseconds,
 *      but NILM needs a stable level before it will commit -- around 800 ms
 *      with the default windows. Judging sooner would report a fault every
 *      single time the controller did its job. Hence `grace_ms`.
 *
 *   2. NILM is not always confident. During any transition it says so, and
 *      while it does, this module must not accumulate evidence. An
 *      unconfident classifier is not evidence of a hardware fault.
 *
 *   On top of that, a fault must persist for `confirm_count` consecutive
 *   confident evaluations. One odd reading is noise; ten in a row is a relay.
 *
 * WHAT IT DOES NOT CATCH
 *   A PARTIAL failure -- a load that switches correctly but draws the wrong
 *   amount, say one element of two burnt out. That does not change the on/off
 *   mask, so it does not appear here; it appears as a persistent non-zero
 *   nilm_t.residual_w, which a caller can read directly. Attributing a
 *   residual to a particular load needs per-load metering, which is the thing
 *   this whole module exists to avoid needing.
 * -------------------------------------------------------------------------*/

typedef enum {
    ACT_UNKNOWN = 0,  /* no confident measurement yet -- say nothing       */
    ACT_PENDING,      /* command changed recently; waiting for it to show  */
    ACT_OK,           /* command and measurement agree                    */
    ACT_STUCK_ON,     /* commanded off, still drawing  -> welded contact   */
    ACT_STUCK_OFF,    /* commanded on, drawing nothing -> open / unplugged */
} act_state_t;

typedef struct {
    const char *name;
    uint8_t     bit;               /* LOAD_* bit                           */

    bool        commanded;         /* last commanded state                 */
    uint32_t    cmd_change_ms;     /* when it last changed                 */
    uint16_t    disagree;          /* consecutive confident disagreements  */

    act_state_t state;
    uint32_t    faults;            /* times it has entered a stuck state   */
    uint32_t    detect_latency_ms; /* command change -> fault declared,
                                    * for the most recent fault           */
} act_load_t;

typedef struct {
    act_load_t loads[NILM_MAX_LOADS];
    int        count;

    uint32_t   grace_ms;       /* settle time allowed after a command change */
    uint16_t   confirm_count;  /* confident disagreements before declaring   */

    uint32_t   faults_total;
    uint32_t   steps;
    uint32_t   steps_judged;   /* steps where a verdict was actually possible */
} verifier_t;

/* `grace_ms` must exceed NILM's settle time -- roughly
 * (NILM_SMOOTH_TAPS + 2 * NILM_STABLE_SAMPLES) sample periods -- plus the
 * slowest transient being watched. 3000 ms is comfortable at 10 Hz. */
void verify_init(verifier_t *v, uint32_t grace_ms, uint16_t confirm_count);

/* Returns the load's index, or -1 if full. `initial_commanded` seeds the
 * command state so a load that is on at boot is not read as a fresh command. */
int  verify_add_load(verifier_t *v, const char *name, uint8_t bit,
                     bool initial_commanded);

/* One evaluation. `commanded` is the scheduler's mask, `inferred` is NILM's,
 * and `nilm_is_confident` gates whether any evidence is accumulated at all.
 * Call at the control rate; the grace window is in milliseconds, so the rate
 * does not have to match NILM's. */
void verify_step(verifier_t *v, uint8_t commanded, uint8_t inferred,
                 bool nilm_is_confident, uint32_t now_ms);

act_state_t verify_state(const verifier_t *v, uint8_t bit);

/* Bitmask of loads currently in a stuck state -- one value to log, alarm on,
 * or publish. LOAD_NONE when everything agrees. */
uint8_t     verify_fault_mask(const verifier_t *v);

const char *verify_state_str(act_state_t s);
