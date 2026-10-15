#include <Arduino.h>
#include "control.h"
#include "config.h"
#include "telemetry.h"
#include "demand_window.h"
#include "scheduler.h"

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
 * WHAT IT ASSUMES, AND WHY THAT MATTERS
 *   There is no per-load metering. The scheduler credits delivered energy
 *   from the commanded relay state and the nameplate rating. If a relay welds
 *   shut or a channel is miswired, that accounting silently drifts from
 *   reality and nothing here would notice. Verifying it from the aggregate
 *   signal is Phase 5's job, and it is the reason Phase 5 exists.
 * -------------------------------------------------------------------------*/

static demand_window_t   s_dw;
static scheduler_t       s_sched;
static telemetry_reader_t s_reader[MAX_DEVICES];
static int               s_reader_count;

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

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t   last_log  = 0;
    uint8_t    last_mask = sched_mask(&s_sched);

    apply_mask(last_mask);

    for (;;) {
        uint32_t now = millis();
        float    w   = read_site_power(a->accessor, a->count);

        uint8_t mask = sched_step(&s_sched, w, now);

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

    Serial.printf("# control: target=%.1f W window=%lu s period=%lu ms "
                  "relays charger=%d thermal=%d\n",
                  (double)DEMAND_TARGET_W,
                  (unsigned long)(DEMAND_WINDOW_MS / 1000),
                  (unsigned long)CONTROL_PERIOD_MS,
                  RELAY_CHARGER_PIN, RELAY_THERMAL_PIN);

    /* Priority 2: below polling (4) and the uplink (3). A late control
     * decision costs a little demand; a late poll corrupts the measurement
     * everything else depends on. */
    xTaskCreatePinnedToCore(task_control, "control", 4096, &arg, 2, NULL, 1);
}
