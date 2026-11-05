#include <Arduino.h>
#include "control.h"
#include "config.h"
#include "telemetry.h"
#include "demand_window.h"
#include "scheduler.h"
#include "nilm.h"
#include "verify.h"

/* ---------------------------------------------------------------------------
 * The demand-response controller, on the edge gateway.
 *
 * WHY IT LIVES HERE AND NOT IN THE CLOUD
 *   This is the whole point of an "Edge Control" tier. Networks drop. If the
 *   shed decision lived upstream, a dropped link would mean no protection at
 *   exactly the moment you want it most. Unplug the network and this task
 *   carries on: it reads from the ring buffer, not from MQTT.
 *
 * HOW IT GETS ITS MEASUREMENT
 *   It attaches its OWN reader to each device's ring, exactly like the MQTT
 *   publisher does, and fast-forwards to the newest sample. It needs present
 *   values, not history -- so it discards the backlog, while the publisher
 *   reading the same rings keeps every sample. Same data, two different
 *   needs, neither aware of the other.
 *
 * WHAT IT USED TO ASSUME, AND WHAT NOW CHECKS IT
 *   There is still no per-load metering. The scheduler credits delivered
 *   energy from the commanded relay state and the nameplate rating, so if a
 *   relay welds shut or a channel is miswired, that accounting drifts away
 *   from reality -- and the demand target ends up being "defended" by a load
 *   that never actually stopped drawing.
 *
 *   Phase 5 closes that gap. nilm.h recovers which loads are really running
 *   from the aggregate power signal alone, and verify.h holds that
 *   measurement up against what was commanded. The controller no longer
 *   trusts its own relays; it checks them against the meter.
 *
 *   Note which clock each part uses. The classifier is fed each sample's own
 *   t_ms, because its windows are counted in SAMPLES and it only uses time
 *   for diagnostics. The verifier runs on millis(), because its grace window
 *   is real elapsed time. Mixing them is deliberate, not an oversight.
 * -------------------------------------------------------------------------*/

/* The classifier's stability and smoothing windows are counted in samples,
 * so they only mean what the tuning intends if samples arrive at the rate
 * they were tuned for. Change the poll rate and the windows must move with
 * it, which a compile-time check is a great deal cheaper than discovering
 * on a bench. */
static_assert(POLL_PERIOD_MS == 100,
              "NILM windows are tuned for 10 Hz samples; retune "
              "NILM_STABLE_SAMPLES and NILM_SMOOTH_TAPS if this changes");

static demand_window_t   s_dw;
static scheduler_t       s_sched;
static telemetry_reader_t s_reader[MAX_DEVICES];
static int               s_reader_count;

/* Phase 5. The classifier gets its OWN cursor on the metered feed, because it
 * needs every sample in order while the controller above wants only the
 * newest -- an edge that falls between two control steps is an edge the
 * classifier would otherwise never see. Two readers, one ring, no coupling. */
static nilm_t             s_nilm;
static verifier_t         s_verify;
static telemetry_reader_t s_nilm_reader;

static int s_idx_charger = -1;
static int s_idx_thermal = -1;

static void relay_write(int pin, bool energised)
{
#if RELAY_ACTIVE_LOW
    digitalWrite(pin, energised ? LOW : HIGH);
#else
    digitalWrite(pin, energised ? HIGH : LOW);
#endif
}

static void apply_mask(uint8_t mask)
{
    relay_write(RELAY_CHARGER_PIN, (mask & LOAD_CHARGER) != 0);
    relay_write(RELAY_THERMAL_PIN, (mask & LOAD_THERMAL) != 0);
    /* LOAD_BASE has no relay by design: a fixed load must not be switchable,
     * so the safest implementation is no wire to cut. */
}

void control_set_charger_job(float wh, uint32_t deadline_ms)
{
    if (s_idx_charger >= 0) {
        sched_set_job(&s_sched, s_idx_charger, wh, deadline_ms);
        Serial.printf("# control: charger job %.3f Wh by t=%lu ms\n",
                      wh, (unsigned long)deadline_ms);
    }
}

void control_get_state(control_state_t *out)
{
    out->target_w      = s_sched.target_w;
    out->window_avg_w  = s_dw.last_avg_w;
    out->peak_avg_w    = s_dw.peak_avg_w;
    out->mask          = sched_mask(&s_sched);
    out->sheds         = s_sched.sheds;
    out->restores      = s_sched.restores;
    out->forced        = s_sched.forced_restores;
    out->refusals      = s_sched.refusals;
    out->deadline_misses = s_sched.deadline_misses;
    out->charger_slack_s = (s_idx_charger >= 0)
        ? sched_slack_s(&s_sched.loads[s_idx_charger], millis()) : 0.0f;
    out->charger_delivered_wh = (s_idx_charger >= 0)
        ? s_sched.loads[s_idx_charger].energy_delivered_wh : 0.0f;
    out->charger_required_wh = (s_idx_charger >= 0)
        ? s_sched.loads[s_idx_charger].energy_required_wh : 0.0f;

    out->nilm_mask       = nilm_mask(&s_nilm);
    out->nilm_confident  = nilm_confident(&s_nilm);
    out->nilm_residual_w = s_nilm.residual_w;
    out->fault_mask      = verify_fault_mask(&s_verify);
    out->act_faults      = s_verify.faults_total;
}

/* Feed EVERY sample from the metered feed to the classifier, in order.
 *
 * Device 0 only: the three loads in this model sit on one metered circuit, so
 * one classifier covers the site. A site with several metered circuits wants
 * one classifier per circuit rather than one over the sum -- which is also
 * how PowerTag-per-breaker metering is laid out, and is the natural extension
 * rather than a change of approach. Summing several meters first would need
 * the samples time-aligned across devices, and would blur exactly the edges
 * the method depends on. */
static void feed_classifier(device_ring_accessor_t accessor)
{
    telemetry_ring_t *ring = accessor(0);
    if (!ring) return;

    telemetry_sample_t s;
    while (ring_read(ring, &s_nilm_reader, &s)) {
        /* A failed sensor read leaves stale values behind; feeding those to
         * an edge detector would invent a step that never happened. */
        if (s.flags & TLM_FLAG_SENSOR_FAIL) continue;
        nilm_update(&s_nilm, s.p, s.t_ms);
    }
}

/* Sum the newest reading from every meter on the bus. */
static float read_site_power(device_ring_accessor_t accessor, int count)
{
    float total = 0.0f;

    for (int k = 0; k < count; k++) {
        telemetry_ring_t *ring = accessor(k);
        if (!ring) continue;

        telemetry_sample_t s;
        bool got = false;
        while (ring_read(ring, &s_reader[k], &s)) got = true;   /* to newest */
        if (got && !(s.flags & TLM_FLAG_SENSOR_FAIL)) total += s.p;
    }
    return total;
}

typedef struct {
    device_ring_accessor_t accessor;
    int count;
} ctrl_arg_t;

static void task_control(void *arg)
{
    ctrl_arg_t *a = (ctrl_arg_t *)arg;

    for (int k = 0; k < a->count; k++) {
        telemetry_ring_t *r = a->accessor(k);
        if (r) ring_reader_attach(r, &s_reader[k], "control");
    }
    s_reader_count = a->count;

    telemetry_ring_t *r0 = a->accessor(0);
    if (r0) ring_reader_attach(r0, &s_nilm_reader, "nilm");

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t   last_log  = 0;
    uint8_t    last_mask = sched_mask(&s_sched);
    uint8_t    last_fault = LOAD_NONE;

    apply_mask(last_mask);

    for (;;) {
        uint32_t now = millis();

        /* Classify first, so the verdict below is drawn from the same
         * samples the shed decision is about to be taken on. */
        feed_classifier(a->accessor);

        float    w   = read_site_power(a->accessor, a->count);

        uint8_t mask = sched_step(&s_sched, w, now);

        /* Hold the command up against what the meter says is really running.
         * Verified against the POST-step mask on purpose: if the command just
         * changed, the verifier restarts its grace window rather than judging
         * a relay on evidence gathered under the previous order. */
        verify_step(&s_verify, mask, nilm_mask(&s_nilm),
                    nilm_confident(&s_nilm), now);

        uint8_t fault = verify_fault_mask(&s_verify);
        if (fault != last_fault) {
            if (fault) {
                /* The loudest thing this task ever says. A relay disagreeing
                 * with its command means the shed arithmetic is fiction and
                 * the demand target is not actually being defended. */
                for (int i = 0; i < s_verify.count; i++) {
                    act_state_t st = s_verify.loads[i].state;
                    if (st != ACT_STUCK_ON && st != ACT_STUCK_OFF) continue;
                    Serial.printf("# ACTUATION FAULT: %s is %s "
                                  "(commanded %s, meter says %s, %lu ms after "
                                  "the command)\n",
                                  s_verify.loads[i].name,
                                  verify_state_str(st),
                                  s_verify.loads[i].commanded ? "on" : "off",
                                  s_verify.loads[i].commanded ? "off" : "on",
                                  (unsigned long)s_verify.loads[i].detect_latency_ms);
                }
            } else {
                Serial.printf("# actuation: all relays agree with their "
                              "commands again\n");
            }
            last_fault = fault;
        }

        if (mask != last_mask) {
            apply_mask(mask);
            char before[32], after[32];
            telemetry_truth_str(last_mask, before, sizeof(before));
            telemetry_truth_str(mask, after, sizeof(after));
            Serial.printf("# control: %s -> %s  (avg %.1f W, projected %.1f W, "
                          "target %.1f W)\n",
                          before, after, s_dw.last_avg_w,
                          dw_projected_w(&s_dw, w, now), s_sched.target_w);
            last_mask = mask;
        }

        if (now - last_log >= 10000) {
            last_log = now;
            Serial.printf("# control avg=%.1f W peak=%.1f W target=%.1f W "
                          "sheds=%lu restores=%lu forced=%lu refused=%lu "
                          "misses=%lu slack=%.0f s\n",
                          s_dw.last_avg_w, s_dw.peak_avg_w, s_sched.target_w,
                          (unsigned long)s_sched.sheds,
                          (unsigned long)s_sched.restores,
                          (unsigned long)s_sched.forced_restores,
                          (unsigned long)s_sched.refusals,
                          (unsigned long)s_sched.deadline_misses,
                          s_idx_charger >= 0
                            ? sched_slack_s(&s_sched.loads[s_idx_charger], now)
                            : 0.0f);

            char cmd[32], seen[32];
            telemetry_truth_str(mask, cmd, sizeof(cmd));
            telemetry_truth_str(nilm_mask(&s_nilm), seen, sizeof(seen));
            Serial.printf("# nilm commanded=%s measured=%s %s residual=%+.1f W "
                          "edges=%lu rejected=%lu resync=%lu faults=%lu\n",
                          cmd, seen,
                          nilm_confident(&s_nilm) ? "(confident)" : "(unsure)",
                          s_nilm.residual_w,
                          (unsigned long)s_nilm.edges,
                          (unsigned long)s_nilm.edges_rejected,
                          (unsigned long)s_nilm.resyncs,
                          (unsigned long)s_verify.faults_total);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void control_task_start(device_ring_accessor_t accessor, int device_count)
{
    static ctrl_arg_t arg;
    arg.accessor = accessor;
    arg.count    = device_count;

    pinMode(RELAY_CHARGER_PIN, OUTPUT);
    pinMode(RELAY_THERMAL_PIN, OUTPUT);

    dw_init(&s_dw, DEMAND_WINDOW_MS);
    sched_init(&s_sched, &s_dw, DEMAND_TARGET_W);
    s_sched.stagger_ms = CONTROL_STAGGER_MS;

    sched_load_t p;

    memset(&p, 0, sizeof(p));
    p.name = "base"; p.bit = LOAD_BASE; p.cls = LOAD_CLASS_FIXED;
    p.rated_w = LOAD_BASE_W; p.priority = 0;
    sched_add_load(&s_sched, &p);

    memset(&p, 0, sizeof(p));
    p.name = "charger"; p.bit = LOAD_CHARGER; p.cls = LOAD_CLASS_DEFERRABLE;
    p.rated_w = LOAD_CHARGER_W; p.priority = 2;
    p.min_on_ms = CHARGER_MIN_ON_MS; p.min_off_ms = CHARGER_MIN_OFF_MS;
    s_idx_charger = sched_add_load(&s_sched, &p);

    memset(&p, 0, sizeof(p));
    p.name = "thermal"; p.bit = LOAD_THERMAL; p.cls = LOAD_CLASS_FLEXIBLE;
    p.rated_w = LOAD_THERMAL_W; p.priority = 1;
    p.min_on_ms = THERMAL_MIN_ON_MS; p.min_off_ms = THERMAL_MIN_OFF_MS;
    p.max_shed_ms = THERMAL_MAX_SHED_MS;
    s_idx_thermal = sched_add_load(&s_sched, &p);
    (void)s_idx_thermal;

    /* The classifier is seeded with the same nameplate ratings the scheduler
     * reasons about, and then corrects them from the edges it observes -- so
     * the two start from one set of numbers and the measurement is free to
     * disagree with it. That disagreement is the whole signal. */
    nilm_init(&s_nilm);
    nilm_add_load(&s_nilm, "base",    LOAD_BASE,    LOAD_BASE_W,    false);
    nilm_add_load(&s_nilm, "charger", LOAD_CHARGER, LOAD_CHARGER_W, false);
    nilm_add_load(&s_nilm, "thermal", LOAD_THERMAL, LOAD_THERMAL_W,
                  NILM_THERMAL_INRUSH != 0);

    verify_init(&s_verify, VERIFY_GRACE_MS, VERIFY_CONFIRM);
    /* Seeded with the mask the relays are about to be driven to, so a load
     * that is energised at boot is not read as a brand-new command. The base
     * load has no relay at all, but it is still worth watching: if it stops
     * drawing, something has died. */
    verify_add_load(&s_verify, "base",    LOAD_BASE,
                    (sched_mask(&s_sched) & LOAD_BASE)    != 0);
    verify_add_load(&s_verify, "charger", LOAD_CHARGER,
                    (sched_mask(&s_sched) & LOAD_CHARGER) != 0);
    verify_add_load(&s_verify, "thermal", LOAD_THERMAL,
                    (sched_mask(&s_sched) & LOAD_THERMAL) != 0);

    Serial.printf("# control: target=%.1f W window=%lu s period=%lu ms "
                  "relays charger=%d thermal=%d\n",
                  (double)DEMAND_TARGET_W,
                  (unsigned long)(DEMAND_WINDOW_MS / 1000),
                  (unsigned long)CONTROL_PERIOD_MS,
                  RELAY_CHARGER_PIN, RELAY_THERMAL_PIN);
    Serial.printf("# nilm: %d loads, verifying relays against the meter "
                  "(grace %lu ms, %d confirmations)\n",
                  s_nilm.count, (unsigned long)VERIFY_GRACE_MS, VERIFY_CONFIRM);

    /* Priority 2: below polling (4) and the uplink (3). A late control
     * decision costs a little demand; a late poll corrupts the measurement
     * everything else depends on. */
    xTaskCreatePinnedToCore(task_control, "control", 4096, &arg, 2, NULL, 1);
}
