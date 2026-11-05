#include "nilm.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * nilm.cpp -- see nilm.h for the method and the reasoning behind it.
 *
 * No dynamic allocation, no floating-point division in the hot path beyond a
 * handful of operations, and a fixed 2^NILM_MAX_LOADS candidate sweep per
 * decision -- 16 comparisons, taken only when a level settles, not per sample.
 * It runs inside the gateway's existing control task without needing one of
 * its own.
 * -------------------------------------------------------------------------*/

/* Margin by which the winning candidate must beat the runner-up before its
 * edge is allowed to adapt a load's working estimate. Adapting on an
 * ambiguous edge is how a classifier teaches itself to be wrong.
 *
 * Note this is a MARGIN, not an error bound: the whole purpose of adaptation
 * is to fix a large absolute error (a 40 W nameplate reading 37.6 W warm),
 * so gating on absolute error would prevent exactly the correction wanted. */
#ifndef NILM_ADAPT_MARGIN_W
#define NILM_ADAPT_MARGIN_W 4.0f
#endif

/* --- small helpers ------------------------------------------------------- */

static int popcount8(uint8_t x)
{
    int c = 0;
    while (x) { c += (x & 1u); x >>= 1; }
    return c;
}

/* Candidate index -> LOAD_* mask. The enumeration is over registered loads,
 * so it does not assume the LOAD_* bits are contiguous or in any order. */
static uint8_t mask_of_index(const nilm_t *n, int idx)
{
    uint8_t m = 0;
    for (int i = 0; i < n->count; i++)
        if (idx & (1 << i)) m |= n->loads[i].bit;
    return m;
}

static bool starts_inrush_load(const nilm_t *n, uint8_t from, uint8_t to)
{
    for (int i = 0; i < n->count; i++) {
        uint8_t b = n->loads[i].bit;
        if (n->loads[i].has_inrush && !(from & b) && (to & b)) return true;
    }
    return false;
}

/* --- signal conditioning ------------------------------------------------ */

static float smooth(nilm_t *n, float raw)
{
    n->raw_hist[n->raw_head] = raw;
    n->raw_head = (n->raw_head + 1) % NILM_SMOOTH_TAPS;
    if (n->raw_filled < NILM_SMOOTH_TAPS) n->raw_filled++;

    float sum = 0.0f;
    for (int j = 0; j < n->raw_filled; j++) sum += n->raw_hist[j];
    return sum / (float)n->raw_filled;
}

static void hist_push(nilm_t *n, float v)
{
    n->hist[n->head] = v;
    n->head = (n->head + 1) % NILM_HISTORY;
    if (n->filled < NILM_HISTORY) n->filled++;
}

/* Mean and peak-to-peak span of the newest `k` smoothed samples. */
static bool window_stats(const nilm_t *n, int k, float *mean, float *span)
{
    if (n->filled < k) return false;

    float lo = 0.0f, hi = 0.0f, sum = 0.0f;
    for (int j = 0; j < k; j++) {
        int   idx = (n->head - 1 - j + NILM_HISTORY * 2) % NILM_HISTORY;
        float v   = n->hist[idx];
        if (j == 0)      { lo = hi = v; }
        else             { if (v < lo) lo = v; if (v > hi) hi = v; }
        sum += v;
    }
    *mean = sum / (float)k;
    *span = hi - lo;
    return true;
}

/* --- candidate scoring -------------------------------------------------- */

/* The anchor: which state best explains this ABSOLUTE power level?
 * Used at startup, and to correct a belief that has drifted from reality. */
static uint8_t best_absolute(const nilm_t *n, float p, float *err_out)
{
    uint8_t best = LOAD_NONE;
    float   best_err = 1e30f;

    int states = 1 << n->count;
    for (int idx = 0; idx < states; idx++) {
        uint8_t m = mask_of_index(n, idx);
        float   e = fabsf(p - nilm_expected_w(n, m));
        if (e < best_err) { best_err = e; best = m; }
    }
    if (err_out) *err_out = best_err;
    return best;
}

/* The method: which state best explains this STEP, starting from `from`?
 *
 * `err_out`    the winner's raw power error, used to reject a step that
 *              explains nothing.
 * `margin_out` how far ahead of the runner-up it finished. Small means the
 *              answer was a coin toss, which is what gates adaptation.
 */
static uint8_t best_transition(const nilm_t *n, uint8_t from, float delta,
                               bool inrush, float *err_out, float *margin_out)
{
    uint8_t best = from;
    float   best_score = 1e30f, second_score = 1e30f, best_err = 1e30f;

    int states = 1 << n->count;
    for (int idx = 0; idx < states; idx++) {
        uint8_t m = mask_of_index(n, idx);
        if (m == from) continue;        /* a step means SOMETHING changed */

        float expect = nilm_expected_w(n, m) - nilm_expected_w(n, from);
        float err    = fabsf(delta - expect);
        float score  = err;

        /* Prefer the simple explanation. */
        int chg = popcount8((uint8_t)(m ^ from));
        if (chg > 1) score += (float)(chg - 1) * NILM_SIMULTANEITY_W;

        /* Inrush is a fingerprint, so it must agree in BOTH directions: seen
         * but unexplained is as suspicious as a motor starting silently. */
        if (inrush != starts_inrush_load(n, from, m))
            score += NILM_INRUSH_PENALTY_W;

        if (score < best_score) {
            second_score = best_score;
            best_score   = score;
            best_err     = err;
            best         = m;
        } else if (score < second_score) {
            second_score = score;
        }
    }

    if (err_out)    *err_out    = best_err;
    if (margin_out) *margin_out = second_score - best_score;
    return best;
}

/* Nudge a load's working estimate toward what its own edge measured.
 * Clamped near the nameplate so one bad attribution cannot run away. */
static void adapt(nilm_t *n, uint8_t from, uint8_t to, float delta, float margin)
{
    if (n->adapt_alpha <= 0.0f)            return;
    if (margin < NILM_ADAPT_MARGIN_W)      return;   /* too close to call */

    uint8_t changed = (uint8_t)(from ^ to);
    if (popcount8(changed) != 1)           return;   /* ambiguous split */

    for (int i = 0; i < n->count; i++) {
        if (n->loads[i].bit != changed) continue;

        float e  = n->loads[i].est_w;
        e += n->adapt_alpha * (fabsf(delta) - e);

        float lo = n->loads[i].rated_w * (1.0f - n->adapt_limit);
        float hi = n->loads[i].rated_w * (1.0f + n->adapt_limit);
        if (e < lo) e = lo;
        if (e > hi) e = hi;

        n->loads[i].est_w = e;
        return;
    }
}

/* --- public ------------------------------------------------------------- */

void nilm_init(nilm_t *n)
{
    memset(n, 0, sizeof(*n));
    n->phase         = NILM_SETTLING;
    n->mask          = LOAD_NONE;
    n->stable_band_w = NILM_STABLE_BAND_W;
    n->edge_w        = NILM_EDGE_W;
    n->adapt_alpha   = 0.25f;
    n->adapt_limit   = 0.30f;
}

int nilm_add_load(nilm_t *n, const char *name, uint8_t bit, float rated_w,
                  bool has_inrush)
{
    if (n->count >= NILM_MAX_LOADS) return -1;
    if (bit == 0)                   return -1;

    /* A duplicate bit would make two candidate states indistinguishable and
     * silently halve the search space. */
    for (int i = 0; i < n->count; i++)
        if (n->loads[i].bit == bit) return -1;

    nilm_load_t *l = &n->loads[n->count];
    l->name       = name;
    l->bit        = bit;
    l->rated_w    = rated_w;
    l->est_w      = rated_w;
    l->has_inrush = has_inrush;
    return n->count++;
}

float nilm_expected_w(const nilm_t *n, uint8_t mask)
{
    float sum = 0.0f;
    for (int i = 0; i < n->count; i++)
        if (mask & n->loads[i].bit) sum += n->loads[i].est_w;
    return sum;
}

void nilm_resync(nilm_t *n)
{
    n->phase      = NILM_SETTLING;
    n->confident  = false;
    n->filled     = 0;
    n->head       = 0;
    n->resync_run = 0;
}

uint8_t nilm_update(nilm_t *n, float power_w, uint32_t t_ms)
{
    float mean = 0.0f, span = 0.0f;

    n->samples++;
    n->level_doubt = false;

    /* Levels and edges come off the smoothed signal; the inrush peak is taken
     * from the raw one, because smoothing is what would destroy it. */
    float p = smooth(n, power_w);
    hist_push(n, p);

    switch (n->phase) {

    case NILM_SETTLING:
        if (window_stats(n, NILM_STABLE_SAMPLES, &mean, &span)
            && span <= n->stable_band_w) {
            n->settled_w      = mean;
            n->mask           = best_absolute(n, mean, NULL);
            n->residual_w     = mean - nilm_expected_w(n, n->mask);
            n->resync_run     = 0;
            n->phase          = NILM_STEADY;
            n->last_settle_ms = t_ms;
        }
        break;

    case NILM_STEADY: {
        float dev = p - n->settled_w;

        if (fabsf(dev) > n->edge_w) {
            n->phase         = NILM_TRANSIENT;
            n->pre_edge_w    = n->settled_w;
            n->peak_dev_w    = fabsf(power_w - n->settled_w);   /* raw */
            n->edge_start_ms = t_ms;
            n->last_edge_ms  = t_ms;
            /* Ignore the next few samples: the moving average is still
             * mixing pre- and post-step values, so any window over them is
             * neither level and would settle on a meaningless average. */
            n->settle_block  = NILM_STABLE_SAMPLES;
            break;
        }

        /* Below the edge threshold but above the noise band: the level is
         * moving and no edge has been declared yet, so the belief is stale
         * even though it is still the best available. Withhold confidence
         * rather than answer a question we have stopped being sure about.
         *
         * This costs about one sample per switching event on the SMALLEST
         * load -- a 5 W step needs two samples to climb through a 3-tap
         * average and cross the threshold, while an 18 or 40 W step crosses
         * on the first. Measured: it is the entire difference between "wrong
         * and confident" and "late but honest", and verify.h gates on
         * confidence precisely so a stale belief cannot be read as a relay
         * fault. */
        n->level_doubt = fabsf(dev) > n->stable_band_w;

        /* Follow slow drift -- a warming element loses a couple of watts over
         * seconds, and without this that eventually reads as an edge. The
         * 2 s time constant is far slower than any real switching event. */
        n->settled_w += 0.05f * dev;
        n->residual_w = n->settled_w - nilm_expected_w(n, n->mask);

        /* The belief does not account for the power being measured. Give it
         * NILM_RESYNC_SAMPLES to be a transient artefact, then overrule it
         * from the absolute anchor and say so in the counters. */
        if (fabsf(n->residual_w) > NILM_RESYNC_W) {
            if (++n->resync_run >= NILM_RESYNC_SAMPLES) {
                uint8_t m = best_absolute(n, n->settled_w, NULL);
                if (m != n->mask) { n->mask = m; n->resyncs++; }
                n->residual_w     = n->settled_w - nilm_expected_w(n, n->mask);
                n->resync_run     = 0;
                n->last_settle_ms = t_ms;
            }
        } else {
            n->resync_run = 0;
        }
        break;
    }

    case NILM_TRANSIENT: {
        float dev_raw = fabsf(power_w - n->pre_edge_w);
        if (dev_raw > n->peak_dev_w) n->peak_dev_w = dev_raw;

        if (n->settle_block > 0) { n->settle_block--; break; }

        if (!window_stats(n, NILM_STABLE_SAMPLES, &mean, &span)
            || span > n->stable_band_w)
            break;                                  /* still moving */

        float delta = mean - n->pre_edge_w;

        if (fabsf(delta) < n->edge_w) {
            /* It wobbled and came back. Nothing switched. */
            n->settled_w  = mean;
            n->residual_w = mean - nilm_expected_w(n, n->mask);
            n->resync_run = 0;
            n->phase      = NILM_STEADY;
            break;
        }

        bool inrush = (delta > 0.0f)
                   && (n->peak_dev_w > NILM_INRUSH_RATIO * fabsf(delta));
        if (inrush) n->inrush_seen++;

        float err = 0.0f, margin = 0.0f;
        uint8_t to = best_transition(n, n->mask, delta, inrush, &err, &margin);

        if (err > NILM_RESYNC_W) {
            /* No state explains this step -- an unmodelled load, or two
             * events overlapping. Committing to the least-bad guess would
             * corrupt the belief AND the working estimates, so instead drop
             * the belief and re-anchor on the next stable level. */
            n->edges_rejected++;
            n->settled_w  = mean;
            n->residual_w = mean - nilm_expected_w(n, n->mask);
            nilm_resync(n);
            break;
        }

        adapt(n, n->mask, to, delta, margin);

        n->mask           = to;
        n->settled_w      = mean;
        n->residual_w     = mean - nilm_expected_w(n, n->mask);
        n->resync_run     = 0;
        n->phase          = NILM_STEADY;
        n->edges++;
        n->last_settle_ms = t_ms;
        break;
    }
    }

    n->confident = (n->phase == NILM_STEADY)
                && !n->level_doubt
                && (fabsf(n->residual_w) <= NILM_CONFIDENT_W);
    if (n->confident) n->confident_samples++;

    return n->mask;
}

uint8_t nilm_mask(const nilm_t *n)      { return n->mask; }
bool    nilm_confident(const nilm_t *n) { return n->confident; }

const char *nilm_phase_str(nilm_phase_t p)
{
    switch (p) {
    case NILM_SETTLING:  return "settling";
    case NILM_STEADY:    return "steady";
    case NILM_TRANSIENT: return "transient";
    }
    return "?";
}
