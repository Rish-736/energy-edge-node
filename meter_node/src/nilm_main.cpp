#ifdef NILM_SCORE
/* ---------------------------------------------------------------------------
 * nilm_main.cpp -- score the Phase 5 classifier against ground truth, and
 * prove the actuation verifier catches a lying relay.
 *
 * WHY THIS CAN EXIST AT ALL
 *   Every sample the simulator produces carries a `truth` bitmask of which
 *   loads were really on. Real hardware cannot give you that -- you would need
 *   a meter per load, which is the very thing NILM exists to avoid. So the
 *   simulator is not a stand-in for a missing test rig here; for this phase it
 *   is a BETTER rig than hardware would be, because it is labelled.
 *
 *   PART 1 scores disaggregation: how often does the classifier name exactly
 *   the right set of loads, how fast does it notice a change, and where does
 *   it go wrong.
 *
 *   PART 2 is the one that matters operationally. The simulator runs a FIXED
 *   schedule -- it does not listen to anybody. So if we command a load off
 *   while the simulator keeps it running, we have modelled a welded relay
 *   contact exactly, without touching a line of the simulator. Command it on
 *   while the simulator holds it off and we have modelled a failed-open
 *   contact or an unplugged load. Three scenarios:
 *
 *     healthy      commands obeyed             -> expect no faults, ever
 *     welded       commanded off, still drawing -> expect STUCK-ON
 *     failed open  commanded on, drawing nothing-> expect STUCK-OFF, then
 *                                                  recovery when it really
 *                                                  does come on
 *
 * EXIT CODE
 *   Non-zero if accuracy regresses below the gates below, if the healthy
 *   scenario reports any fault, or if either fault scenario goes undetected.
 *   That makes this a regression test and not just a demo.
 *
 *   pio run -e native-nilm -d meter_node
 *   .pio/build/native-nilm/program            # scored report
 *   .pio/build/native-nilm/program --csv      # per-sample CSV for plotting
 * -------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "telemetry.h"
#include "ina226_hal.h"
#include "nilm.h"
#include "verify.h"

void sim_set_time_ms(uint32_t ms);

/* --- regression gates ---------------------------------------------------
 * Set just below what the implementation actually achieves, so a real
 * regression trips them and ordinary float jitter does not. */
#define GATE_EXACT_PCT      95.0f   /* exact-mask accuracy, confident samples */
#define GATE_CONFIDENT_PCT  80.0f   /* fraction of samples it will commit to  */
#define GATE_LATENCY_MS     2500u   /* mean time to recognise a change        */

#define SCORE_SECONDS       600u    /* 10 full simulator cycles */

/* Nameplate ratings the classifier is SEEDED with. Declared locally, like
 * site_main.cpp does, because the gateway's config.h belongs to the other
 * project -- and because being approximate is the point: the simulator's
 * charger settles near 37.6 W warm, not the 40 W printed on it, and watching
 * the adapted estimates converge on the truth is part of what this scores. */
#define BASE_W    5.0f
#define CHARGER_W 40.0f
#define THERMAL_W 18.0f

/* The classifier's view of the site. `has_inrush` is the motor-like startup
 * overshoot -- only the thermal load has one, and that is what identifies it
 * by shape rather than by size. */
static void build_nilm(nilm_t *n)
{
    nilm_init(n);
    nilm_add_load(n, "base",    LOAD_BASE,    BASE_W,    false);
    nilm_add_load(n, "charger", LOAD_CHARGER, CHARGER_W, false);
    nilm_add_load(n, "thermal", LOAD_THERMAL, THERMAL_W, true);
}

/* =========================================================================
 * PART 1 -- disaggregation accuracy
 * =======================================================================*/

typedef struct {
    const char *name;
    uint8_t bit;
    unsigned tp, fp, fn, tn;
} load_score_t;

typedef struct {
    unsigned samples, confident, exact;
    unsigned transitions, resolved;
    double   latency_sum_ms;
    unsigned latency_max_ms;
    load_score_t per[3];
    nilm_t   n;
} score_t;

static void score_run(score_t *sc, bool csv)
{
    memset(sc, 0, sizeof(*sc));
    sc->per[0].name = "base";    sc->per[0].bit = LOAD_BASE;
    sc->per[1].name = "charger"; sc->per[1].bit = LOAD_CHARGER;
    sc->per[2].name = "thermal"; sc->per[2].bit = LOAD_THERMAL;

    build_nilm(&sc->n);
    ina226_init();

    uint8_t  last_truth   = 0xFF;   /* force a transition on the first sample */
    uint8_t  pending      = 0;
    uint32_t pending_t    = 0;
    bool     awaiting     = false;

    if (csv)
        printf("t_ms,power_w,truth,inferred,confident,phase,residual_w,"
               "est_base,est_charger,est_thermal\n");

    unsigned total = SCORE_SECONDS * SAMPLE_RATE_HZ;
    for (unsigned k = 0; k < total; k++) {
        uint32_t t_ms = k * SAMPLE_PERIOD_MS;
        sim_set_time_ms(t_ms);

        ina226_reading_t r;
        ina226_read(&r);
        uint8_t truth = ina226_truth_mask();

        uint8_t guess = nilm_update(&sc->n, r.power_w, t_ms);
        bool    conf  = nilm_confident(&sc->n);

        sc->samples++;

        /* --- transition latency ---
         * Measured from the instant the truth changed to the instant the
         * classifier both agrees AND is willing to commit. A change that is
         * superseded before it is recognised is counted as unresolved rather
         * than quietly dropped. */
        if (truth != last_truth) {
            if (last_truth != 0xFF) {
                sc->transitions++;
                pending   = truth;
                pending_t = t_ms;
                awaiting  = true;
            }
            last_truth = truth;
        }
        if (awaiting && conf && guess == pending) {
            uint32_t lat = t_ms - pending_t;
            sc->latency_sum_ms += (double)lat;
            if (lat > sc->latency_max_ms) sc->latency_max_ms = lat;
            sc->resolved++;
            awaiting = false;
        }

        /* Accuracy is only claimed over samples the classifier committed to.
         * The confident fraction is reported alongside it so that cannot be
         * gamed by simply refusing to answer. */
        if (conf) {
            sc->confident++;
            if (guess == truth) sc->exact++;
            for (int i = 0; i < 3; i++) {
                bool t_on = (truth & sc->per[i].bit) != 0;
                bool g_on = (guess & sc->per[i].bit) != 0;
                if      ( t_on &&  g_on) sc->per[i].tp++;
                else if (!t_on &&  g_on) sc->per[i].fp++;
                else if ( t_on && !g_on) sc->per[i].fn++;
                else                     sc->per[i].tn++;
            }
        }

        if (csv) {
            char tb[32], gb[32];
            telemetry_truth_str(truth, tb, sizeof(tb));
            telemetry_truth_str(guess, gb, sizeof(gb));
            printf("%lu,%.4f,%s,%s,%d,%s,%.4f,%.3f,%.3f,%.3f\n",
                   (unsigned long)t_ms, r.power_w, tb, gb, conf ? 1 : 0,
                   nilm_phase_str(sc->n.phase), sc->n.residual_w,
                   sc->n.loads[0].est_w, sc->n.loads[1].est_w,
                   sc->n.loads[2].est_w);
        }
    }
}

static void score_report(const score_t *sc)
{
    float conf_pct  = 100.0f * (float)sc->confident / (float)sc->samples;
    float exact_pct = sc->confident
                    ? 100.0f * (float)sc->exact / (float)sc->confident : 0.0f;
    double mean_lat = sc->resolved
                    ? sc->latency_sum_ms / (double)sc->resolved : 0.0;

    printf("PART 1 -- disaggregation against ground truth\n");
    printf("-------------------------------------------------------------\n");
    printf("  samples              %u  (%u s at %d Hz)\n",
           sc->samples, SCORE_SECONDS, SAMPLE_RATE_HZ);
    printf("  committed to answer  %u  (%.1f%%)\n", sc->confident, conf_pct);
    printf("  exact mask correct   %u  (%.2f%% of those)\n",
           sc->exact, exact_pct);
    printf("\n");
    printf("  per load (over committed samples):\n");
    printf("    %-9s %7s %7s %7s %7s %9s %9s\n",
           "load", "TP", "FP", "FN", "TN", "precision", "recall");
    for (int i = 0; i < 3; i++) {
        const load_score_t *p = &sc->per[i];
        float prec = (p->tp + p->fp) ? 100.0f * p->tp / (float)(p->tp + p->fp) : 100.0f;
        float rec  = (p->tp + p->fn) ? 100.0f * p->tp / (float)(p->tp + p->fn) : 100.0f;
        printf("    %-9s %7u %7u %7u %7u %8.2f%% %8.2f%%\n",
               p->name, p->tp, p->fp, p->fn, p->tn, prec, rec);
    }
    printf("\n");
    printf("  state changes        %u, recognised %u\n",
           sc->transitions, sc->resolved);
    printf("  recognition latency  mean %.0f ms, worst %u ms\n",
           mean_lat, sc->latency_max_ms);
    printf("\n");
    printf("  classifier internals:\n");
    printf("    edges attributed   %lu\n", (unsigned long)sc->n.edges);
    printf("    edges rejected     %lu  (explained nothing)\n",
           (unsigned long)sc->n.edges_rejected);
    printf("    inrush detected    %lu\n", (unsigned long)sc->n.inrush_seen);
    printf("    anchor corrections %lu\n", (unsigned long)sc->n.resyncs);
    printf("    adapted ratings    base %.2f W (rated %.1f), "
           "charger %.2f W (rated %.1f), thermal %.2f W (rated %.1f)\n",
           sc->n.loads[0].est_w, sc->n.loads[0].rated_w,
           sc->n.loads[1].est_w, sc->n.loads[1].rated_w,
           sc->n.loads[2].est_w, sc->n.loads[2].rated_w);
    printf("\n");
}

/* =========================================================================
 * PART 2 -- actuation verification
 * =======================================================================*/

typedef enum { SC_HEALTHY = 0, SC_WELDED, SC_OPEN } scenario_t;

typedef struct {
    uint8_t     fault_mask;
    act_state_t charger_state;
    act_state_t charger_state_at_end;
    uint32_t    latency_ms;
    uint32_t    faults_total;
    uint32_t    steps_judged;
} sc_result_t;

/* 3 s of grace comfortably exceeds NILM's ~800 ms settle at 10 Hz plus the
 * thermal load's 250 ms inrush. 5 confident disagreements at 10 Hz is a
 * further 500 ms of confirmation. */
#define VERIFY_GRACE_MS   3000u
#define VERIFY_CONFIRM    5

static void run_scenario(scenario_t which, unsigned seconds, sc_result_t *out)
{
    nilm_t    n;
    verifier_t v;

    build_nilm(&n);
    ina226_init();

    verify_init(&v, VERIFY_GRACE_MS, VERIFY_CONFIRM);
    verify_add_load(&v, "base",    LOAD_BASE,    false);
    verify_add_load(&v, "charger", LOAD_CHARGER, false);
    verify_add_load(&v, "thermal", LOAD_THERMAL, false);

    memset(out, 0, sizeof(*out));
    uint8_t worst_mask = LOAD_NONE;
    act_state_t seen   = ACT_UNKNOWN;
    uint32_t    lat    = 0;

    unsigned total = seconds * SAMPLE_RATE_HZ;
    for (unsigned k = 0; k < total; k++) {
        uint32_t t_ms = k * SAMPLE_PERIOD_MS;
        sim_set_time_ms(t_ms);

        ina226_reading_t r;
        ina226_read(&r);
        uint8_t truth = ina226_truth_mask();

        /* An obedient plant does exactly what it is told, so "commanded"
         * tracks truth -- then one bit is corrupted to model the fault. */
        uint8_t commanded = truth;
        if (which == SC_WELDED && t_ms >= 28000)
            commanded &= (uint8_t)~LOAD_CHARGER;   /* ordered off at 28 s */
        else if (which == SC_OPEN && t_ms >= 16000 && t_ms < 25000)
            commanded |= LOAD_CHARGER;             /* ordered on at 16 s */

        uint8_t guess = nilm_update(&n, r.power_w, t_ms);
        verify_step(&v, commanded, guess, nilm_confident(&n), t_ms);

        uint8_t fm = verify_fault_mask(&v);
        worst_mask |= fm;

        act_state_t cs = verify_state(&v, LOAD_CHARGER);
        if ((cs == ACT_STUCK_ON || cs == ACT_STUCK_OFF) && seen == ACT_UNKNOWN) {
            seen = cs;
            for (int i = 0; i < v.count; i++)
                if (v.loads[i].bit == LOAD_CHARGER) lat = v.loads[i].detect_latency_ms;
        }
    }

    out->fault_mask           = worst_mask;
    out->charger_state        = seen;
    out->charger_state_at_end = verify_state(&v, LOAD_CHARGER);
    out->latency_ms           = lat;
    out->faults_total         = v.faults_total;
    out->steps_judged         = v.steps_judged;
}

/* =========================================================================*/

int main(int argc, char **argv)
{
    bool csv = (argc > 1) && strcmp(argv[1], "--csv") == 0;

    score_t sc;
    score_run(&sc, csv);
    if (csv) return 0;

    printf("=============================================================\n");
    printf("Phase 5 -- NILM disaggregation and actuation verification\n");
    printf("=============================================================\n\n");

    score_report(&sc);

    printf("PART 2 -- actuation verification\n");
    printf("-------------------------------------------------------------\n");
    printf("  The simulator runs a fixed schedule and ignores commands, so\n");
    printf("  commanding a load against it models a relay that lies.\n\n");

    sc_result_t healthy, welded, open_;
    run_scenario(SC_HEALTHY, 120, &healthy);
    run_scenario(SC_WELDED,  60,  &welded);
    run_scenario(SC_OPEN,    60,  &open_);

    printf("  healthy  (commands obeyed)\n");
    printf("    faults %lu, verdicts given on %lu steps  -> %s\n",
           (unsigned long)healthy.faults_total,
           (unsigned long)healthy.steps_judged,
           healthy.faults_total == 0 ? "no false alarms" : "FALSE ALARM");

    printf("  welded   (charger ordered off at t=28 s, keeps drawing)\n");
    printf("    detected as %s after %lu ms\n",
           verify_state_str(welded.charger_state),
           (unsigned long)welded.latency_ms);

    printf("  open     (charger ordered on at t=16 s, draws nothing)\n");
    printf("    detected as %s after %lu ms, recovered to %s once it\n"
           "    really came on at t=25 s\n",
           verify_state_str(open_.charger_state),
           (unsigned long)open_.latency_ms,
           verify_state_str(open_.charger_state_at_end));
    printf("\n");

    /* --- gates ---------------------------------------------------------- */
    float conf_pct  = 100.0f * (float)sc.confident / (float)sc.samples;
    float exact_pct = sc.confident
                    ? 100.0f * (float)sc.exact / (float)sc.confident : 0.0f;
    double mean_lat = sc.resolved
                    ? sc.latency_sum_ms / (double)sc.resolved : 1e9;

    int fail = 0;
    printf("VERDICT\n");
    printf("-------------------------------------------------------------\n");

    if (exact_pct < GATE_EXACT_PCT) {
        printf("  FAIL  exact accuracy %.2f%% < %.2f%%\n",
               exact_pct, GATE_EXACT_PCT); fail = 1;
    } else {
        printf("  pass  exact accuracy %.2f%% (gate %.2f%%)\n",
               exact_pct, GATE_EXACT_PCT);
    }

    if (conf_pct < GATE_CONFIDENT_PCT) {
        printf("  FAIL  committed on %.1f%% of samples < %.1f%%\n",
               conf_pct, GATE_CONFIDENT_PCT); fail = 1;
    } else {
        printf("  pass  committed on %.1f%% of samples (gate %.1f%%)\n",
               conf_pct, GATE_CONFIDENT_PCT);
    }

    if (mean_lat > (double)GATE_LATENCY_MS) {
        printf("  FAIL  mean recognition latency %.0f ms > %u ms\n",
               mean_lat, GATE_LATENCY_MS); fail = 1;
    } else {
        printf("  pass  mean recognition latency %.0f ms (gate %u ms)\n",
               mean_lat, GATE_LATENCY_MS);
    }

    if (sc.resolved < sc.transitions) {
        printf("  FAIL  %u of %u state changes went unrecognised\n",
               sc.transitions - sc.resolved, sc.transitions); fail = 1;
    } else {
        printf("  pass  all %u state changes recognised\n", sc.transitions);
    }

    if (healthy.faults_total != 0) {
        printf("  FAIL  healthy plant raised %lu false alarm(s)\n",
               (unsigned long)healthy.faults_total); fail = 1;
    } else {
        printf("  pass  healthy plant raised no false alarms\n");
    }

    if (welded.charger_state != ACT_STUCK_ON) {
        printf("  FAIL  welded contact not detected (saw %s)\n",
               verify_state_str(welded.charger_state)); fail = 1;
    } else {
        printf("  pass  welded contact detected in %lu ms\n",
               (unsigned long)welded.latency_ms);
    }

    if (open_.charger_state != ACT_STUCK_OFF) {
        printf("  FAIL  failed-open contact not detected (saw %s)\n",
               verify_state_str(open_.charger_state)); fail = 1;
    } else {
        printf("  pass  failed-open contact detected in %lu ms\n",
               (unsigned long)open_.latency_ms);
    }

    if (open_.charger_state_at_end != ACT_OK) {
        printf("  FAIL  verifier did not recover after the fault cleared "
               "(saw %s)\n", verify_state_str(open_.charger_state_at_end));
        fail = 1;
    } else {
        printf("  pass  verifier recovered once the fault cleared\n");
    }

    printf("\n%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");
    return fail;
}

#endif /* NILM_SCORE */
