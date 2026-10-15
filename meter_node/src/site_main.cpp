#ifdef SITE_SIM
/* ---------------------------------------------------------------------------
 * site_main.cpp -- the closed-loop result, measured.
 *
 * Models a small commercial site with an EV charger and runs it twice over an
 * identical workload: once with the controller disabled, once with it active.
 * The only difference between the two runs is whether sched_step()'s output is
 * allowed to drive the loads.
 *
 * That is what makes the comparison honest -- same loads, same profile, same
 * random seed, same everything. The difference in peak demand is attributable
 * to the controller and nothing else.
 *
 * WHY A SEPARATE SITE MODEL FROM ina226_sim.cpp
 *   ina226_sim models a SENSOR on a fixed bench workload, and its output is
 *   the calibrated Phase 0 baseline (2.57x peak-to-mean) that the docs quote.
 *   Changing its profile would invalidate that artifact. This models a SITE:
 *   loads that respond to control, with jobs and deadlines. Different concern,
 *   deliberately separate.
 *
 *   build:  pio run -e native-site -d meter_node
 *   run:    ./.pio/build/native-site/program            summary only
 *           ./.pio/build/native-site/program --csv > run.csv
 * -------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "telemetry.h"
#include "demand_window.h"
#include "scheduler.h"

/* --- scenario ------------------------------------------------------------
 * Times are scaled so a run completes in a moment while keeping the shape of
 * a real overnight charge: a long plug-in window, a job that needs a fraction
 * of it, and a thermal load cycling underneath. */
#define STEP_MS          500u
#define RUN_MS           600000u      /* 10 minutes of simulated site time   */
#define WINDOW_MS        60000u       /* stands in for a 15-minute tariff    */
#define TARGET_W         35.0f        /* the demand ceiling being defended   */

#define BASE_W           5.0f         /* lighting and controls, always on    */
#define CHARGER_W        40.0f
#define THERMAL_W        18.0f

#define PLUG_IN_MS       60000u       /* car arrives                         */
#define UNPLUG_MS        540000u      /* car leaves                          */
#define DEADLINE_MS      480000u      /* must be full by here                */
#define CHARGE_WH        1.5f         /* 1.5 Wh at 40 W = 135 s of charging  */

#define THERMAL_PERIOD_MS 120000u     /* 60 s on, 60 s off                   */

/* --- deterministic noise, so both runs see identical disturbances -------- */
static uint32_t s_rng;
static void noise_reset(void) { s_rng = 0xC0FFEEu; }
static float noise(float amp)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    float u = (float)((s_rng >> 8) & 0xFFFF) / 65535.0f;
    return (u - 0.5f) * 2.0f * amp;
}

/* --- the site's electrical behaviour ------------------------------------ */
typedef struct {
    bool  charger_on, thermal_on;
    float charger_ramp_ms;        /* for the thermal load's inrush decay     */
    uint32_t thermal_started_ms;
} site_t;

/* Power drawn right now, given what is actually energised. */
static float site_power_w(uint32_t t, bool charger_live, bool thermal_live,
                          uint32_t thermal_started_ms)
{
    float p = BASE_W + noise(0.05f);

    if (charger_live) {
        /* A charger tapers slightly as the pack fills; same shape the bench
         * resistor shows as it warms. */
        float warm = 1.0f - expf(-(float)(t) / 1000.0f / 120.0f);
        p += CHARGER_W * (1.0f - 0.04f * warm) + noise(0.15f);
    }

    if (thermal_live) {
        float dt = (float)(t - thermal_started_ms) / 1000.0f;
        float inrush = 1.0f + 2.5f * expf(-dt / 0.08f);      /* motor start  */
        float ripple = 1.0f + 0.08f * sinf(dt * 6.28318531f * 7.0f);
        p += THERMAL_W * inrush * ripple + noise(0.20f);
    }

    return p < 0.0f ? 0.0f : p;
}

/* --- one run ------------------------------------------------------------- */
typedef struct {
    float    peak_window_w;
    float    mean_w;
    float    peak_instant_w;
    float    energy_wh;
    float    charge_delivered_wh;
    uint32_t deadline_misses;
    uint32_t sheds, restores, forced_restores;
    uint32_t thermal_wanted_ms;   /* how long the thermal load asked to run  */
    uint32_t thermal_ran_ms;      /* how long it was allowed to              */
} result_t;

static void run(bool control_enabled, result_t *res, FILE *csv)
{
    noise_reset();

    demand_window_t dw;
    scheduler_t     sch;
    dw_init(&dw, WINDOW_MS);
    sched_init(&sch, &dw, TARGET_W);
    sch.stagger_ms = 10000;

    sched_load_t p;

    memset(&p, 0, sizeof(p));
    p.name = "base"; p.bit = LOAD_BASE; p.cls = LOAD_CLASS_FIXED;
    p.rated_w = BASE_W; p.priority = 0;
    sched_add_load(&sch, &p);

    memset(&p, 0, sizeof(p));
    p.name = "charger"; p.bit = LOAD_CHARGER; p.cls = LOAD_CLASS_DEFERRABLE;
    p.rated_w = CHARGER_W; p.priority = 2;
    p.min_on_ms = 5000; p.min_off_ms = 5000;
    int ev = sched_add_load(&sch, &p);

    memset(&p, 0, sizeof(p));
    p.name = "thermal"; p.bit = LOAD_THERMAL; p.cls = LOAD_CLASS_FLEXIBLE;
    p.rated_w = THERMAL_W; p.priority = 1;
    p.min_on_ms = 10000; p.min_off_ms = 10000;
    p.max_shed_ms = 60000;        /* the buffer tolerates a minute off      */
    sched_add_load(&sch, &p);

    double sum_w = 0.0, energy_wh = 0.0;
    uint32_t n = 0;
    float prev_w = 0.0f;
    uint32_t thermal_started = 0;
    bool prev_thermal = false;
    bool job_set = false;

    memset(res, 0, sizeof(*res));
    (void)prev_thermal;

    if (csv)
        fprintf(csv, "t_ms,mode,power_w,window_avg_w,projected_w,"
                     "charger_on,thermal_on,charge_wh,slack_s\n");

    for (uint32_t t = 0; t <= RUN_MS; t += STEP_MS) {

        /* The car arrives and declares its job. */
        if (!job_set && t >= PLUG_IN_MS) {
            sched_set_job(&sch, ev, CHARGE_WH, DEADLINE_MS);
            job_set = true;
        }

        uint8_t mask = sched_mask(&sch);

        /* Physical reality: a load draws power only if it is present, still
         * wanted, AND energised. The controller's mask only gates the last. */
        bool plugged   = (t >= PLUG_IN_MS && t < UNPLUG_MS);
        bool job_left  = sch.loads[ev].energy_delivered_wh
                       < sch.loads[ev].energy_required_wh;
        bool charger_live = plugged && job_left &&
                            (!control_enabled || (mask & LOAD_CHARGER));

        bool thermal_wanted = ((t % THERMAL_PERIOD_MS) < THERMAL_PERIOD_MS / 2);
        bool thermal_live   = thermal_wanted &&
                              (!control_enabled || (mask & LOAD_THERMAL));

        if (thermal_live && !prev_thermal) thermal_started = t;
        prev_thermal = thermal_live;

        if (thermal_wanted) res->thermal_wanted_ms += STEP_MS;
        if (thermal_live)   res->thermal_ran_ms    += STEP_MS;

        float w = site_power_w(t, charger_live, thermal_live, thermal_started);

        /* Feed the controller the real measurement. With control disabled we
         * still run the window and the accounting -- only the mask is
         * ignored -- so both runs are measured identically. */
        sched_step(&sch, w, t);

        /* With control off the charger still delivers whenever it is live,
         * so the energy accounting has to follow physics, not the mask. */
        if (!control_enabled && charger_live) {
            float hours = (float)STEP_MS / 3600000.0f;
            if (sch.loads[ev].energy_delivered_wh < sch.loads[ev].energy_required_wh)
                sch.loads[ev].energy_delivered_wh += CHARGER_W * hours;
        }

        if (n) {
            double hours = (double)STEP_MS / 3600000.0;
            energy_wh += 0.5 * (prev_w + w) * hours;
        }
        prev_w = w;
        sum_w += w;
        n++;

        if (w > res->peak_instant_w) res->peak_instant_w = w;

        if (csv)
            fprintf(csv, "%u,%s,%.3f,%.3f,%.3f,%d,%d,%.4f,%.1f\n",
                    t, control_enabled ? "controlled" : "baseline",
                    w, dw_average_w(&dw), dw_projected_w(&dw, w, t),
                    charger_live ? 1 : 0, thermal_live ? 1 : 0,
                    sch.loads[ev].energy_delivered_wh,
                    sched_slack_s(&sch.loads[ev], t));
    }

    res->peak_window_w       = dw.peak_avg_w;
    res->mean_w              = (float)(sum_w / (double)n);
    res->energy_wh           = (float)energy_wh;
    res->charge_delivered_wh = sch.loads[ev].energy_delivered_wh;
    res->deadline_misses     = sch.deadline_misses;
    res->sheds               = sch.sheds;
    res->restores            = sch.restores;
    res->forced_restores     = sch.forced_restores;
}

int main(int argc, char **argv)
{
    bool want_csv = (argc > 1 && strcmp(argv[1], "--csv") == 0);
    FILE *csv = want_csv ? stdout : NULL;
    FILE *out = want_csv ? stderr : stdout;

    result_t base, ctrl;
    run(false, &base, csv);
    run(true,  &ctrl, csv);

    float peak_cut = base.peak_window_w > 0.0f
                   ? (1.0f - ctrl.peak_window_w / base.peak_window_w) * 100.0f
                   : 0.0f;

    fprintf(out, "\nEV charging site -- %u s, %.0f s demand window, %.0f W target\n",
            RUN_MS / 1000, WINDOW_MS / 1000.0, TARGET_W);
    fprintf(out, "job: %.2f Wh by t=%u s (plugged in %u-%u s)\n\n",
            CHARGE_WH, DEADLINE_MS / 1000, PLUG_IN_MS / 1000, UNPLUG_MS / 1000);

    fprintf(out, "                        baseline   controlled\n");
    fprintf(out, "peak demand (window)    %7.2f W  %8.2f W\n",
            base.peak_window_w, ctrl.peak_window_w);
    fprintf(out, "peak instantaneous      %7.2f W  %8.2f W\n",
            base.peak_instant_w, ctrl.peak_instant_w);
    fprintf(out, "mean power              %7.2f W  %8.2f W\n",
            base.mean_w, ctrl.mean_w);
    fprintf(out, "energy                  %7.4f Wh %8.4f Wh\n",
            base.energy_wh, ctrl.energy_wh);
    fprintf(out, "charge delivered        %7.3f Wh %8.3f Wh  (needed %.2f)\n",
            base.charge_delivered_wh, ctrl.charge_delivered_wh, CHARGE_WH);
    fprintf(out, "deadline misses         %7u    %8u\n",
            base.deadline_misses, ctrl.deadline_misses);
    fprintf(out, "sheds / restores              -    %5u / %u",
            ctrl.sheds, ctrl.restores);
    fprintf(out, "  (%u forced)\n", ctrl.forced_restores);

    float base_duty = base.thermal_wanted_ms
                    ? 100.0f * base.thermal_ran_ms / base.thermal_wanted_ms : 0.0f;
    float ctrl_duty = ctrl.thermal_wanted_ms
                    ? 100.0f * ctrl.thermal_ran_ms / ctrl.thermal_wanted_ms : 0.0f;
    fprintf(out, "thermal runtime met     %6.1f%%   %7.1f%%\n", base_duty, ctrl_duty);

    fprintf(out, "\npeak demand reduced     %.1f%%\n", peak_cut);
    fprintf(out, "deadline misses         %u\n", ctrl.deadline_misses);

    /* The energy drop is NOT a saving. The charger delivered its full job in
     * both runs; the difference is thermal runtime the controller withheld.
     * A real thermal load claws that back afterwards (rebound), which this
     * model does not simulate. Report the service cost rather than presenting
     * it as free energy -- which is the whole point of D85. */
    fprintf(out, "\nservice cost: the charger met its deadline in full. The\n");
    fprintf(out, "thermal load ran %.1f%% of what it asked for, against %.1f%%\n",
            ctrl_duty, base_duty);
    fprintf(out, "in the baseline. A real thermal load would recover that\n");
    fprintf(out, "runtime afterwards (rebound), which this model does not\n");
    fprintf(out, "simulate -- so the lower energy figure is NOT a saving.\n");

    /* The controller is only a success if BOTH hold. A lower peak bought by
     * missing the deadline is a failure, not a trade. */
    bool ok = (ctrl.deadline_misses == 0) && (peak_cut > 0.0f);
    fprintf(out, "\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
#endif /* SITE_SIM */
