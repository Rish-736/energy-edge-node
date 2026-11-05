#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "telemetry.h"   /* LOAD_* mask bits */

/* ---------------------------------------------------------------------------
 * nilm.h -- non-intrusive load monitoring: which loads are on, from ONE meter.
 *
 * THE PROBLEM
 *   The meter gives you a single number. 45 watts. Which appliances made it?
 *   With three loads rated 5, 18 and 40 W there are eight possible answers:
 *
 *       0   5   18   23   40   45   58   63     W
 *      --  base th   b+t  ch  c+b  c+t  all
 *
 *   The levels are all distinct, so the naive answer is "measure power, pick
 *   the nearest level". That is the ANCHOR here, not the method, because the
 *   closest pair of levels is only 5 W apart and real loads do not sit still:
 *
 *     - the charger droops ~6% (2.4 W) as its element warms and its
 *       resistance rises,
 *     - the thermal load carries an 8% commutation ripple (1.4 W),
 *     - plus sensor noise.
 *
 *   Worst case that is ~4 W of wander against a 2.5 W margin for error. At
 *   the 63 W state the nearest-level answer is decided by 0.2 W, which is to
 *   say it is decided by noise. Measured, not assumed: see docs/DECISIONS.md.
 *
 * THE METHOD: TRACK EDGES, NOT LEVELS
 *   So the state is tracked DIFFERENTIALLY. We hold a belief about which loads
 *   are on, and we update it when the power STEPS:
 *
 *       delta = settled_after - settled_before
 *
 *   A step of +18 W means the thermal load started, whatever the charger's
 *   element happens to be doing at the time. Droop in one load cannot corrupt
 *   an edge belonging to another, which is the whole advantage -- and it is
 *   the classic result from Hart's 1992 NILM work.
 *
 *   Three things make the attribution robust:
 *
 *   1. CANDIDATE ENUMERATION. We do not ask "which load changed?" but "which
 *      of the 2^n reachable states best explains this step?", so two loads
 *      switching together is a candidate rather than a special case. A
 *      penalty per simultaneous change keeps the simple explanation ahead.
 *
 *   2. INRUSH AS A FINGERPRINT. A motor draws ~3.5x its running current for a
 *      couple of hundred milliseconds. If a step overshoots its own settled
 *      height by NILM_INRUSH_RATIO, a motor-like load must have STARTED in
 *      this transition -- regardless of the step's size. That identifies a
 *      load by its shape, which is what lets this scale to loads whose
 *      wattages collide.
 *
 *   3. ADAPTIVE RATINGS. Each load's working estimate is nudged toward what
 *      its edges actually measure, clamped near the nameplate. A 40 W element
 *      that really draws 37.6 W warm converges there, and the anchor's margins
 *      widen instead of narrowing. Nameplates lie; edges do not.
 *
 * WHY SMOOTH THE INPUT, BUT NOT FOR EVERYTHING
 *   At 10 Hz the thermal load's 7 Hz ripple aliases to ~3 Hz, where a 3-tap
 *   moving average sits almost exactly on its null -- it removes about 87% of
 *   it and leaves a 5 W step essentially intact. So levels and edges are
 *   detected on the SMOOTHED signal.
 *
 *   The inrush peak is measured on the RAW signal, because smoothing is
 *   precisely what would destroy it. One signal, two jobs, by design.
 *
 * WHAT THIS IS FOR
 *   Not curiosity. The scheduler credits delivered energy from the relay
 *   state it COMMANDED and the nameplate rating -- it has no per-load meter
 *   and cannot tell that a relay welded shut. This module is the independent
 *   measurement that verify.h checks commands against.
 * -------------------------------------------------------------------------*/

#ifndef NILM_MAX_LOADS
#define NILM_MAX_LOADS 4        /* 2^4 = 16 candidate states per decision */
#endif

/* Consecutive stable samples before a level is believed. 5 at 10 Hz = 500 ms,
 * long enough to average out the ripple, short enough that a transition is
 * identified well inside the controller's 500 ms step. */
#ifndef NILM_STABLE_SAMPLES
#define NILM_STABLE_SAMPLES 5
#endif

/* Moving-average taps on the input. 3 at 10 Hz nulls the aliased ripple.
 * Must be <= NILM_HISTORY. */
#ifndef NILM_SMOOTH_TAPS
#define NILM_SMOOTH_TAPS 3
#endif

#ifndef NILM_HISTORY
#define NILM_HISTORY 8          /* >= NILM_STABLE_SAMPLES and SMOOTH_TAPS */
#endif

/* A level is "stable" when the last NILM_STABLE_SAMPLES smoothed samples span
 * no more than this. Above the residual ripple, below the smallest load. */
#ifndef NILM_STABLE_BAND_W
#define NILM_STABLE_BAND_W 1.2f
#endif

/* Smallest step called an edge. Above the stable band so ripple cannot fake
 * one, below the smallest load (5 W) so no real switch-on is missed. */
#ifndef NILM_EDGE_W
#define NILM_EDGE_W 2.5f
#endif

/* Added to a candidate's error for each load beyond the first that it claims
 * changed. Keeps "one load switched" ahead of a contrived multi-load fit. */
#ifndef NILM_SIMULTANEITY_W
#define NILM_SIMULTANEITY_W 2.0f
#endif

/* Peak overshoot over settled step height that counts as motor inrush.
 * A 3.5x inrush with an 80 ms time constant still reads >=1.7x even if the
 * first sample lands a full period late, so 1.5 separates it from the clean
 * steps (~1.0) with margin at both ends. */
#ifndef NILM_INRUSH_RATIO
#define NILM_INRUSH_RATIO 1.5f
#endif

/* Error added when inrush was seen but a candidate starts no inrush-capable
 * load (or vice versa). Strong evidence, deliberately not absolute. */
#ifndef NILM_INRUSH_PENALTY_W
#define NILM_INRUSH_PENALTY_W 6.0f
#endif

/* |measured - expected| above which the belief is treated as wrong and the
 * absolute anchor is consulted again. */
#ifndef NILM_RESYNC_W
#define NILM_RESYNC_W 4.0f
#endif

/* Consecutive steady samples over NILM_RESYNC_W before resynchronising, so a
 * single odd reading does not discard a good belief. */
#ifndef NILM_RESYNC_SAMPLES
#define NILM_RESYNC_SAMPLES 10
#endif

/* |residual| at or below which the answer is reported as confident. */
#ifndef NILM_CONFIDENT_W
#define NILM_CONFIDENT_W 3.0f
#endif

typedef struct {
    const char *name;
    uint8_t     bit;        /* LOAD_* bit, so output compares directly
                             * against telemetry_sample_t.truth           */
    float       rated_w;    /* nameplate                                  */
    float       est_w;      /* working estimate, adapted from edges        */
    bool        has_inrush; /* motor-like: expect a startup overshoot      */
} nilm_load_t;

typedef enum {
    NILM_SETTLING = 0,   /* no level believed yet -- startup, or post-resync */
    NILM_STEADY,         /* level stable, mask believed                      */
    NILM_TRANSIENT,      /* a step is in progress; the old belief still holds
                          * because nothing better is known until it settles */
} nilm_phase_t;

typedef struct {
    nilm_load_t  loads[NILM_MAX_LOADS];
    int          count;

    nilm_phase_t phase;
    uint8_t      mask;          /* the answer                               */
    bool         confident;

    /* --- tunables, defaulted by nilm_init() --------------------------- */
    float    stable_band_w;
    float    edge_w;
    float    adapt_alpha;       /* 0 disables adaptation                    */
    float    adapt_limit;       /* est_w stays within rated*(1 +/- limit)   */

    /* --- signal conditioning ------------------------------------------ */
    float    hist[NILM_HISTORY];   /* smoothed, newest at (head-1)          */
    int      head;
    int      filled;
    float    raw_hist[NILM_SMOOTH_TAPS];
    int      raw_head;
    int      raw_filled;

    /* --- level tracking ----------------------------------------------- */
    float    settled_w;         /* believed level of the current state      */
    float    residual_w;        /* settled_w - expected(mask)               */
    int      resync_run;        /* consecutive samples over NILM_RESYNC_W   */
    bool     level_doubt;       /* the level is moving but no edge has been
                                 * declared yet, so the belief is stale.
                                 * Suppresses confidence for that sample --
                                 * being late beats being confidently wrong */

    /* --- transient capture -------------------------------------------- */
    float    pre_edge_w;        /* level before the step began              */
    float    peak_dev_w;        /* max |raw - pre_edge_w| during the step   */
    uint32_t edge_start_ms;
    int      settle_block;      /* samples to ignore while the filter fills */

    /* --- diagnostics, reported rather than hidden ---------------------- */
    uint32_t samples;
    uint32_t confident_samples;
    uint32_t edges;             /* steps attributed to a state change       */
    uint32_t edges_rejected;    /* steps that explained nothing             */
    uint32_t resyncs;           /* beliefs the anchor had to correct        */
    uint32_t inrush_seen;
    uint32_t last_settle_ms;    /* when the current belief was adopted      */
    uint32_t last_edge_ms;
} nilm_t;

void nilm_init(nilm_t *n);

/* Returns the load's index, or -1 if full. `has_inrush` marks a load with a
 * motor-like startup overshoot. */
int  nilm_add_load(nilm_t *n, const char *name, uint8_t bit, float rated_w,
                   bool has_inrush);

/* Feed one aggregate power reading. Call at a steady rate -- the stability and
 * smoothing windows are counted in samples, not milliseconds. Returns the
 * believed LOAD_* mask, which is also available from nilm_mask(). */
uint8_t nilm_update(nilm_t *n, float power_w, uint32_t t_ms);

uint8_t nilm_mask(const nilm_t *n);

/* False during startup and transitions, and whenever the believed state does
 * not account for the measured power. A caller acting on the mask -- verify.h
 * especially -- must gate on this, or it will read a mid-transition belief as
 * a hardware fault. */
bool    nilm_confident(const nilm_t *n);

/* Total of the working estimates for the loads in `mask`. */
float   nilm_expected_w(const nilm_t *n, uint8_t mask);

/* Drop the belief and re-anchor from the next stable level. For a caller that
 * knows the plant changed underneath it. */
void    nilm_resync(nilm_t *n);

const char *nilm_phase_str(nilm_phase_t p);
