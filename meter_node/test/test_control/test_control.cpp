#include <unity.h>
#include <string.h>
#include "demand_window.h"
#include "scheduler.h"

/* ---------------------------------------------------------------------------
 * The whole controller, verified without a relay, a sensor, or a board.
 *
 * sched_step() takes a power reading and a timestamp and returns a bitmask.
 * It touches no hardware, so a test can drive it through a complete scenario
 * -- a full demand window, a deadline approaching, slack running out -- in
 * microseconds, and assert on outcomes that would take minutes to observe on
 * a bench.
 *
 * The assertion that matters most is `deadline_misses == 0`. That is the
 * project's central claim, and these tests are what make it a claim rather
 * than a hope.
 * -------------------------------------------------------------------------*/

#define WINDOW_MS  60000u          /* 60 s standing in for a 15-minute tariff */

static demand_window_t dw;
static scheduler_t     sch;

void setUp(void)    { dw_init(&dw, WINDOW_MS); sched_init(&sch, &dw, 40.0f); }
void tearDown(void) {}

static int add(const char *name, uint8_t bit, load_class_t cls, float w,
               uint8_t prio)
{
    sched_load_t p;
    memset(&p, 0, sizeof(p));
    p.name = name; p.bit = bit; p.cls = cls; p.rated_w = w; p.priority = prio;
    return sched_add_load(&sch, &p);
}

/* =========================================================================
 * Demand window
 * =======================================================================*/

/* A brief spike costs nothing on a demand tariff; a sustained load costs
 * everything. If the window did not distinguish these, the controller would
 * be solving the wrong problem entirely. */
static void test_short_spike_barely_moves_the_average(void)
{
    uint32_t t = 0;
    dw_update(&dw, 0.0f, t);
    for (; t <= 2000; t += 100) dw_update(&dw, 100.0f, t);   /* 2 s at 100 W */

    float avg = dw_average_w(&dw);
    /* 100 W for 2 s of a 60 s window ~= 3.3 W average. */
    TEST_ASSERT_TRUE(avg < 5.0f);
}

static void test_sustained_load_fills_the_average(void)
{
    uint32_t t = 0;
    dw_update(&dw, 0.0f, t);
    for (; t < WINDOW_MS; t += 100) dw_update(&dw, 50.0f, t);

    float avg = dw_average_w(&dw);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 50.0f, avg);
}

/* The projection is the whole point: it says where the window will LAND, so
 * the controller can act before the average has already crossed the line. */
static void test_projection_sees_trouble_before_the_average_does(void)
{
    uint32_t t = 0;
    dw_update(&dw, 0.0f, t);
    for (; t <= 5000; t += 100) dw_update(&dw, 80.0f, t);    /* 5 s in */

    float avg       = dw_average_w(&dw);
    float projected = dw_projected_w(&dw, 80.0f, t);

    TEST_ASSERT_TRUE(avg < 10.0f);          /* still looks harmless... */
    TEST_ASSERT_TRUE(projected > 70.0f);    /* ...but it is heading for 80 W */
}

/* Shedding late must remove more power for the same effect, because less of
 * the window remains to average over. */
static void test_excess_grows_as_the_window_runs_out(void)
{
    /* Two windows carrying the same load, caught at different points. Both
     * project to ~60 W against a 40 W target; the later one has less time
     * left to average over, so more power has to come off to fix it. */
    demand_window_t early_w, late_w;
    dw_init(&early_w, WINDOW_MS);
    dw_init(&late_w,  WINDOW_MS);

    for (uint32_t t = 0; t <= 20000; t += 100) dw_update(&early_w, 60.0f, t);
    for (uint32_t t = 0; t <= 50000; t += 100) dw_update(&late_w,  60.0f, t);

    float early = dw_excess_w(&early_w, 60.0f, 20000, 40.0f);
    float late  = dw_excess_w(&late_w,  60.0f, 50000, 40.0f);

    TEST_ASSERT_TRUE(early > 0.0f);
    TEST_ASSERT_TRUE(late > early);
}

static void test_no_excess_when_on_course(void)
{
    uint32_t t = 0;
    dw_update(&dw, 0.0f, t);
    for (; t <= 10000; t += 100) dw_update(&dw, 20.0f, t);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, dw_excess_w(&dw, 20.0f, t, 40.0f));
}

/* =========================================================================
 * Slack
 * =======================================================================*/

static void test_slack_is_spare_time_before_the_deadline(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);

    /* 0.4 Wh at 40 W needs 36 s. Deadline 100 s away -> 64 s of slack. */
    sched_set_job(&sch, ev, 0.4f, 100000);

    float slack = sched_slack_s(&sch.loads[ev], 0);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 64.0f, slack);
    TEST_ASSERT_FALSE(sched_is_critical(&sch.loads[ev], 0));
}

static void test_load_with_no_slack_is_critical(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    sched_set_job(&sch, ev, 0.4f, 36000);    /* needs 36 s, has exactly 36 s */
    TEST_ASSERT_TRUE(sched_is_critical(&sch.loads[ev], 0));
}

static void test_finished_job_has_infinite_slack(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    sched_set_job(&sch, ev, 0.4f, 100000);
    sch.loads[ev].energy_delivered_wh = 0.4f;
    TEST_ASSERT_TRUE(sched_slack_s(&sch.loads[ev], 0) > 1000.0f);
}

/* =========================================================================
 * Scheduling
 * =======================================================================*/

/* The base load is lighting and controls. It must never be touched, whatever
 * the demand target says. */
static void test_fixed_load_is_never_shed(void)
{
    add("base", LOAD_BASE, LOAD_CLASS_FIXED, 5.0f, 0);

    uint8_t mask = 0;
    for (uint32_t t = 0; t <= 40000; t += 1000)
        mask = sched_step(&sch, 200.0f, t);     /* wildly over target */

    TEST_ASSERT_TRUE(mask & LOAD_BASE);
    TEST_ASSERT_EQUAL_UINT32(0, sch.sheds);
    TEST_ASSERT_TRUE(sch.refusals > 0);         /* and it said so */
}

static void test_deferrable_load_is_shed_when_over_target(void)
{
    add("base", LOAD_BASE, LOAD_CLASS_FIXED, 5.0f, 0);
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    sched_set_job(&sch, ev, 0.2f, 600000);      /* huge slack */

    uint8_t mask = 0;
    for (uint32_t t = 0; t <= 20000; t += 1000)
        mask = sched_step(&sch, 45.0f, t);

    TEST_ASSERT_FALSE(mask & LOAD_CHARGER);
    TEST_ASSERT_TRUE(mask & LOAD_BASE);
    TEST_ASSERT_TRUE(sch.sheds > 0);
}

/* The core guarantee: a load that can no longer finish in time becomes
 * untouchable, even though the site is still over its demand target. */
static void test_critical_load_is_never_shed(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    sched_set_job(&sch, ev, 0.4f, 36000);       /* zero slack from the start */

    uint8_t mask = 0;
    for (uint32_t t = 0; t <= 30000; t += 1000)
        mask = sched_step(&sch, 120.0f, t);     /* far over target */

    TEST_ASSERT_TRUE(mask & LOAD_CHARGER);
    TEST_ASSERT_EQUAL_UINT32(0, sch.sheds);
}

/* Slack drains while a load is shed. When it hits zero the scheduler must put
 * the load back WITHOUT being asked, even though the demand target is still
 * breached -- the deadline outranks the bill. */
static void test_slack_running_out_forces_a_restore(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    /* Needs 36 s of charging; deadline at 60 s. 24 s of slack. */
    sched_set_job(&sch, ev, 0.4f, 60000);

    uint8_t mask = 0;
    for (uint32_t t = 0; t <= 50000; t += 1000)
        mask = sched_step(&sch, 100.0f, t);     /* permanently over target */

    TEST_ASSERT_TRUE(sch.sheds > 0);            /* it did shed, early on */
    TEST_ASSERT_TRUE(sch.forced_restores > 0);  /* then gave the time back */
    TEST_ASSERT_TRUE(mask & LOAD_CHARGER);      /* and left it running */
}

/* The claim the whole project rests on. */
static void test_deadline_is_met_under_sustained_pressure(void)
{
    add("base", LOAD_BASE, LOAD_CLASS_FIXED, 5.0f, 0);
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);

    /* 0.3 Wh at 40 W = 27 s of charging, due at t = 90 s. */
    sched_set_job(&sch, ev, 0.3f, 90000);

    for (uint32_t t = 0; t <= 95000; t += 500) {
        uint8_t m = sched_mask(&sch);
        float   w = 5.0f + ((m & LOAD_CHARGER) ? 40.0f : 0.0f);
        sched_step(&sch, w, t);
    }

    TEST_ASSERT_EQUAL_UINT32(0, sch.deadline_misses);
    TEST_ASSERT_TRUE(sch.loads[ev].energy_delivered_wh >= 0.3f - 1e-3f);
}

/* Equipment protection outranks the demand target. */
static void test_min_on_time_blocks_an_immediate_shed(void)
{
    sched_load_t p;
    memset(&p, 0, sizeof(p));
    p.name = "thermal"; p.bit = LOAD_THERMAL; p.cls = LOAD_CLASS_FLEXIBLE;
    p.rated_w = 18.0f; p.min_on_ms = 20000; p.priority = 1;
    sched_add_load(&sch, &p);

    uint8_t mask = sched_step(&sch, 100.0f, 1000);
    TEST_ASSERT_TRUE(mask & LOAD_THERMAL);          /* too soon to shed */
    TEST_ASSERT_EQUAL_UINT32(0, sch.sheds);

    for (uint32_t t = 2000; t <= 25000; t += 1000)
        mask = sched_step(&sch, 100.0f, t);
    TEST_ASSERT_FALSE(mask & LOAD_THERMAL);         /* now it may go */
}

/* max_shed_ms is the crude proxy for buffer state: a freezer may be off for
 * ten minutes, not two hours. It must come back regardless of the target. */
static void test_max_shed_duration_forces_a_restore(void)
{
    sched_load_t p;
    memset(&p, 0, sizeof(p));
    p.name = "thermal"; p.bit = LOAD_THERMAL; p.cls = LOAD_CLASS_FLEXIBLE;
    p.rated_w = 18.0f; p.max_shed_ms = 10000; p.priority = 1;
    sched_add_load(&sch, &p);

    uint8_t mask = 0;
    for (uint32_t t = 0; t <= 30000; t += 1000)
        mask = sched_step(&sch, 100.0f, t);

    TEST_ASSERT_TRUE(sch.sheds > 0);
    TEST_ASSERT_TRUE(sch.forced_restores > 0);
    TEST_ASSERT_TRUE(mask & LOAD_THERMAL);
}

/* Releasing several loads together creates a brand-new peak -- the classic
 * way a naive controller makes things worse than doing nothing. */
static void test_restores_are_staggered(void)
{
    sch.stagger_ms = 10000;

    for (int k = 0; k < 3; k++) {
        sched_load_t p;
        memset(&p, 0, sizeof(p));
        p.name = "flex"; p.bit = (uint8_t)(1u << k);
        p.cls = LOAD_CLASS_FLEXIBLE; p.rated_w = 10.0f; p.priority = (uint8_t)k;
        sched_add_load(&sch, &p);
    }

    /* Push everything off. */
    for (uint32_t t = 0; t <= 20000; t += 1000) sched_step(&sch, 200.0f, t);
    TEST_ASSERT_TRUE(sch.sheds >= 2);

    uint32_t before = sch.restores;

    /* Now quiet. Only one load may come back per stagger interval. */
    for (uint32_t t = 21000; t <= 26000; t += 1000) sched_step(&sch, 0.0f, t);
    TEST_ASSERT_TRUE(sch.restores - before <= 1);
}

/* Without hysteresis the relay chatters at the threshold. */
static void test_restore_requires_headroom_below_target(void)
{
    int ev = add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    sched_set_job(&sch, ev, 0.1f, 600000);
    sch.stagger_ms = 0;

    for (uint32_t t = 0; t <= 20000; t += 1000) sched_step(&sch, 60.0f, t);
    TEST_ASSERT_TRUE(sch.sheds > 0);

    /* Sitting just under the target is not enough to justify bringing a 40 W
     * load back -- that would immediately breach it again. */
    uint8_t mask = 0;
    for (uint32_t t = 21000; t <= 30000; t += 1000)
        mask = sched_step(&sch, 39.0f, t);
    TEST_ASSERT_FALSE(mask & LOAD_CHARGER);
}

static void test_mask_reflects_commanded_state(void)
{
    add("base",    LOAD_BASE,    LOAD_CLASS_FIXED,      5.0f, 0);
    add("charger", LOAD_CHARGER, LOAD_CLASS_DEFERRABLE, 40.0f, 1);
    TEST_ASSERT_EQUAL_HEX8(LOAD_BASE | LOAD_CHARGER, sched_mask(&sch));
}

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_short_spike_barely_moves_the_average);
    RUN_TEST(test_sustained_load_fills_the_average);
    RUN_TEST(test_projection_sees_trouble_before_the_average_does);
    RUN_TEST(test_excess_grows_as_the_window_runs_out);
    RUN_TEST(test_no_excess_when_on_course);

    RUN_TEST(test_slack_is_spare_time_before_the_deadline);
    RUN_TEST(test_load_with_no_slack_is_critical);
    RUN_TEST(test_finished_job_has_infinite_slack);

    RUN_TEST(test_fixed_load_is_never_shed);
    RUN_TEST(test_deferrable_load_is_shed_when_over_target);
    RUN_TEST(test_critical_load_is_never_shed);
    RUN_TEST(test_slack_running_out_forces_a_restore);
    RUN_TEST(test_deadline_is_met_under_sustained_pressure);
    RUN_TEST(test_min_on_time_blocks_an_immediate_shed);
    RUN_TEST(test_max_shed_duration_forces_a_restore);
    RUN_TEST(test_restores_are_staggered);
    RUN_TEST(test_restore_requires_headroom_below_target);
    RUN_TEST(test_mask_reflects_commanded_state);

    return UNITY_END();
}
