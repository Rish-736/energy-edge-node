#include <unity.h>
#include <string.h>
#include <math.h>
#include "nilm.h"
#include "verify.h"

/* ---------------------------------------------------------------------------
 * Phase 5 verified without a sensor, a relay, or a board.
 *
 * nilm_update() takes a power reading and a timestamp and returns a bitmask;
 * verify_step() takes two bitmasks and returns verdicts. Neither touches
 * hardware, so a test can drive a whole switching sequence -- inrush, droop,
 * ripple, a welded contact -- in microseconds.
 *
 * Two tests carry most of the weight:
 *
 *   test_inrush_tells_two_identical_loads_apart
 *       Proves the classifier identifies a load by the SHAPE of its turn-on
 *       and not merely by its size. Without that, NILM only ever works on
 *       loads whose wattages happen not to collide, which is not a method.
 *
 *   test_welded_contact_is_detected
 *       The reason this phase exists. A relay that reports "off" while
 *       current still flows is invisible to the controller, and this is the
 *       test that says we would catch it.
 * -------------------------------------------------------------------------*/

#define BASE_W     5.0f
#define CHARGER_W 40.0f
#define THERMAL_W 18.0f

static nilm_t    n;
static verifier_t v;
static uint32_t  t_ms;

void setUp(void)
{
    nilm_init(&n);
    nilm_add_load(&n, "base",    LOAD_BASE,    BASE_W,    false);
    nilm_add_load(&n, "charger", LOAD_CHARGER, CHARGER_W, false);
    nilm_add_load(&n, "thermal", LOAD_THERMAL, THERMAL_W, true);
    t_ms = 0;
}
void tearDown(void) {}

/* Feed `count` samples of `w` at the 10 Hz sample rate the windows assume. */
static void feed(float w, int count)
{
    for (int k = 0; k < count; k++) { nilm_update(&n, w, t_ms); t_ms += 100; }
}

/* Long enough for the smoothing filter to flush and a level to be believed. */
static void settle(float w) { feed(w, 20); }

/* =========================================================================
 * Anchoring on an absolute level
 * =======================================================================*/

static void test_silence_means_no_loads(void)
{
    settle(0.0f);
    TEST_ASSERT_TRUE(nilm_confident(&n));
    TEST_ASSERT_EQUAL_UINT8(LOAD_NONE, nilm_mask(&n));
}

static void test_startup_anchors_on_whatever_is_already_running(void)
{
    /* No edge to observe -- the gateway rebooted mid-shift and the site was
     * already drawing 23 W. Only the absolute anchor can recover this. */
    settle(23.0f);
    TEST_ASSERT_TRUE(nilm_confident(&n));
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_THERMAL, nilm_mask(&n));
}

static void test_no_answer_is_claimed_before_a_level_settles(void)
{
    feed(45.0f, 2);
    TEST_ASSERT_FALSE(nilm_confident(&n));
}

static void test_expected_power_sums_the_estimates(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f,
        nilm_expected_w(&n, LOAD_NONE));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, BASE_W + CHARGER_W,
        nilm_expected_w(&n, LOAD_BASE | LOAD_CHARGER));
}

/* =========================================================================
 * Attributing edges
 * =======================================================================*/

static void test_switch_on_is_attributed_to_one_load(void)
{
    settle(0.0f);
    settle(5.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE, nilm_mask(&n));
    TEST_ASSERT_EQUAL_UINT32(1, n.edges);
}

static void test_switch_off_is_attributed_too(void)
{
    settle(45.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_CHARGER, nilm_mask(&n));
    settle(5.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE, nilm_mask(&n));
}

static void test_a_big_step_is_the_charger_not_three_small_ones(void)
{
    settle(5.0f);
    settle(45.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_CHARGER, nilm_mask(&n));
}

static void test_simultaneous_switch_is_a_candidate_not_a_special_case(void)
{
    settle(0.0f);
    /* base + charger together: no single load explains a 45 W step. */
    settle(45.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_CHARGER, nilm_mask(&n));
}

static void test_the_simple_explanation_wins(void)
{
    /* 23 W is both "thermal + base" and, from 5 W, "a lone 18 W step".
     * Starting from base already on, one load changing must win. */
    settle(5.0f);
    settle(23.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_THERMAL, nilm_mask(&n));
}

/* =========================================================================
 * Inrush as a fingerprint -- the discriminator that makes this a method
 * =======================================================================*/

static void test_inrush_is_recognised_on_the_motor(void)
{
    settle(5.0f);
    /* 3.5x overshoot decaying over ~250 ms, then the running level. */
    nilm_update(&n, 5.0f + THERMAL_W * 3.5f, t_ms); t_ms += 100;
    nilm_update(&n, 5.0f + THERMAL_W * 1.7f, t_ms); t_ms += 100;
    nilm_update(&n, 5.0f + THERMAL_W * 1.2f, t_ms); t_ms += 100;
    settle(23.0f);

    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_THERMAL, nilm_mask(&n));
    TEST_ASSERT_EQUAL_UINT32(1, n.inrush_seen);
}

static void test_inrush_tells_two_identical_loads_apart(void)
{
    /* THE test. Two loads, same 18 W rating, so their steps are
     * indistinguishable by size. Only the turn-on shape separates them. */
    nilm_t m;
    nilm_init(&m);
    nilm_add_load(&m, "pump", LOAD_THERMAL, 18.0f, true);   /* has inrush */
    nilm_add_load(&m, "heat", LOAD_CHARGER, 18.0f, false);  /* clean step */

    uint32_t t = 0;
    for (int k = 0; k < 20; k++) { nilm_update(&m, 0.0f, t); t += 100; }

    /* A clean 18 W step must be the heater. */
    for (int k = 0; k < 20; k++) { nilm_update(&m, 18.0f, t); t += 100; }
    TEST_ASSERT_EQUAL_UINT8(LOAD_CHARGER, nilm_mask(&m));

    /* Back to nothing, then the same 18 W step WITH an inrush: the pump. */
    for (int k = 0; k < 20; k++) { nilm_update(&m, 0.0f, t); t += 100; }
    TEST_ASSERT_EQUAL_UINT8(LOAD_NONE, nilm_mask(&m));

    nilm_update(&m, 18.0f * 3.5f, t); t += 100;
    nilm_update(&m, 18.0f * 1.7f, t); t += 100;
    nilm_update(&m, 18.0f * 1.2f, t); t += 100;
    for (int k = 0; k < 20; k++) { nilm_update(&m, 18.0f, t); t += 100; }
    TEST_ASSERT_EQUAL_UINT8(LOAD_THERMAL, nilm_mask(&m));
}

/* =========================================================================
 * Not being fooled by the things that actually happen on a bench
 * =======================================================================*/

static void test_thermal_ripple_does_not_fake_an_edge(void)
{
    settle(23.0f);
    uint32_t edges_before = n.edges;

    /* 8% commutation ripple on an 18 W motor, aliased by 10 Hz sampling --
     * the real waveform from ina226_sim. It must not look like switching. */
    for (int k = 0; k < 100; k++) {
        float ripple = 23.0f + 1.44f * sinf((float)k * 6.28318531f * 0.3f);
        nilm_update(&n, ripple, t_ms);
        t_ms += 100;
    }
    TEST_ASSERT_EQUAL_UINT32(edges_before, n.edges);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_THERMAL, nilm_mask(&n));
}

static void test_a_warming_element_does_not_read_as_switching(void)
{
    settle(45.0f);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_CHARGER, nilm_mask(&n));
    uint32_t edges_before = n.edges;

    /* 6% droop as the element heats: 45 W -> 42.6 W over several seconds.
     * Bigger than the stable band, so only the slow-drift tracking keeps
     * this from being misread as a load switching off. */
    for (int k = 0; k < 80; k++) {
        float warm = 1.0f - expf(-(float)k * 0.1f / 2.0f);
        nilm_update(&n, 5.0f + CHARGER_W * (1.0f - 0.06f * warm), t_ms);
        t_ms += 100;
    }
    TEST_ASSERT_EQUAL_UINT32(edges_before, n.edges);
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_CHARGER, nilm_mask(&n));
}

static void test_a_stale_belief_is_never_reported_as_confident(void)
{
    /* A step begins but has not yet crossed the edge threshold. The old mask
     * is still the best guess, and it is now wrong -- so confidence must be
     * withheld. verify.h gates on this, so a confidently wrong mask here
     * would surface as a phantom relay fault. */
    settle(0.0f);
    TEST_ASSERT_TRUE(nilm_confident(&n));

    nilm_update(&n, 5.0f, t_ms);        /* smoothed: only ~1.7 W so far */
    TEST_ASSERT_FALSE(nilm_confident(&n));
}

/* =========================================================================
 * Adaptive ratings
 * =======================================================================*/

static void test_adaptation_learns_the_real_wattage(void)
{
    /* Nameplate says 40 W; the element actually draws 37.6 W warm. Repeated
     * edges should pull the estimate toward what was measured. */
    for (int cycle = 0; cycle < 6; cycle++) {
        settle(5.0f);
        settle(5.0f + 37.6f);
    }
    float est = n.loads[1].est_w;
    TEST_ASSERT_TRUE(est < CHARGER_W);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 37.6f, est);
}

static void test_adaptation_cannot_run_away(void)
{
    /* Even fed edges wildly off the nameplate, the estimate stays clamped
     * near it -- one bad attribution must not destroy the model. */
    n.adapt_limit = 0.30f;
    for (int cycle = 0; cycle < 20; cycle++) {
        settle(0.0f);
        settle(5.0f);
    }
    TEST_ASSERT_TRUE(n.loads[0].est_w >= BASE_W * 0.70f - 0.01f);
    TEST_ASSERT_TRUE(n.loads[0].est_w <= BASE_W * 1.30f + 0.01f);
}

static void test_adaptation_can_be_disabled(void)
{
    n.adapt_alpha = 0.0f;
    for (int cycle = 0; cycle < 6; cycle++) {
        settle(5.0f);
        settle(5.0f + 37.6f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, CHARGER_W, n.loads[1].est_w);
}

/* =========================================================================
 * Admitting ignorance
 * =======================================================================*/

static void test_an_unexplainable_step_is_rejected_not_guessed(void)
{
    settle(0.0f);
    /* 200 W is no combination of 5, 18 and 40. Committing to the least-bad
     * fit would corrupt both the belief and the working estimates. */
    settle(200.0f);
    TEST_ASSERT_EQUAL_UINT32(1, n.edges_rejected);
    TEST_ASSERT_EQUAL_UINT32(0, n.edges);
}

static void test_the_anchor_corrects_a_belief_that_drifted(void)
{
    settle(0.0f);
    nilm_resync(&n);                 /* belief dropped; level unchanged */
    TEST_ASSERT_FALSE(nilm_confident(&n));
    settle(23.0f);                   /* re-anchors absolutely, no edge seen */
    TEST_ASSERT_TRUE(nilm_confident(&n));
    TEST_ASSERT_EQUAL_UINT8(LOAD_BASE | LOAD_THERMAL, nilm_mask(&n));
}

static void test_duplicate_and_empty_load_bits_are_rejected(void)
{
    /* A duplicate bit would make two candidate states identical and silently
     * halve the search space. */
    TEST_ASSERT_EQUAL_INT(-1, nilm_add_load(&n, "dup", LOAD_BASE, 5.0f, false));
    TEST_ASSERT_EQUAL_INT(-1, nilm_add_load(&n, "zero", 0, 5.0f, false));
    TEST_ASSERT_EQUAL_INT(3, n.count);
}

/* =========================================================================
 * Actuation verification
 * =======================================================================*/

#define GRACE_MS 3000u
#define CONFIRM  5

static void verify_setup(bool charger_on)
{
    verify_init(&v, GRACE_MS, CONFIRM);
    verify_add_load(&v, "base",    LOAD_BASE,    true);
    verify_add_load(&v, "charger", LOAD_CHARGER, charger_on);
    verify_add_load(&v, "thermal", LOAD_THERMAL, false);
}

/* Hold a command/measurement pair for `ms`, evaluating every 100 ms. */
static void hold(uint8_t commanded, uint8_t measured, uint32_t ms)
{
    uint32_t end = t_ms + ms;
    while (t_ms < end) {
        verify_step(&v, commanded, measured, true, t_ms);
        t_ms += 100;
    }
}

static void test_agreement_is_reported_ok(void)
{
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    TEST_ASSERT_EQUAL_INT(ACT_OK, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT32(0, v.faults_total);
}

static void test_the_grace_window_blocks_an_early_verdict(void)
{
    /* Commanding a shed and judging it immediately would report a fault every
     * time the controller did its job -- NILM needs time to see the change. */
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);

    t_ms += 0;
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS - 500);
    TEST_ASSERT_EQUAL_INT(ACT_PENDING, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT32(0, v.faults_total);
}

static void test_welded_contact_is_detected(void)
{
    /* Commanded off. Still drawing. The controller cannot see this, and it
     * means the demand target is being defended by a load that never
     * stopped. This is the whole point of Phase 5. */
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS + 2000);

    TEST_ASSERT_EQUAL_INT(ACT_STUCK_ON, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT8(LOAD_CHARGER, verify_fault_mask(&v));
    TEST_ASSERT_EQUAL_UINT32(1, v.faults_total);
}

static void test_failed_open_contact_is_detected(void)
{
    verify_setup(false);
    hold(LOAD_BASE, LOAD_BASE, 5000);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE, GRACE_MS + 2000);

    TEST_ASSERT_EQUAL_INT(ACT_STUCK_OFF, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT8(LOAD_CHARGER, verify_fault_mask(&v));
}

static void test_a_single_disagreement_is_not_a_fault(void)
{
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS);

    /* One confident disagreement past the grace window. */
    verify_step(&v, LOAD_BASE, LOAD_BASE | LOAD_CHARGER, true, t_ms);
    t_ms += 100;
    TEST_ASSERT_EQUAL_UINT32(0, v.faults_total);
}

static void test_an_unconfident_classifier_is_not_evidence(void)
{
    /* NILM says "I don't know" throughout. That is not a relay fault, and
     * treating it as one would alarm on every transition. */
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);

    uint32_t end = t_ms + GRACE_MS + 5000;
    while (t_ms < end) {
        verify_step(&v, LOAD_BASE, LOAD_BASE | LOAD_CHARGER, false, t_ms);
        t_ms += 100;
    }
    TEST_ASSERT_EQUAL_UINT32(0, v.faults_total);
    TEST_ASSERT_NOT_EQUAL(ACT_STUCK_ON, verify_state(&v, LOAD_CHARGER));
}

static void test_a_new_command_discards_old_evidence(void)
{
    /* Evidence gathered against the previous command must not be carried
     * into a verdict about the new one. */
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS + 200);

    /* Command it back on; the measurement now agrees immediately. */
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 500);
    TEST_ASSERT_EQUAL_INT(ACT_PENDING, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT32(0, v.faults_total);
}

static void test_the_verifier_recovers_when_the_fault_clears(void)
{
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS + 2000);
    TEST_ASSERT_EQUAL_INT(ACT_STUCK_ON, verify_state(&v, LOAD_CHARGER));

    /* The contact frees itself, or somebody reseated the wire. */
    hold(LOAD_BASE, LOAD_BASE, 1000);
    TEST_ASSERT_EQUAL_INT(ACT_OK, verify_state(&v, LOAD_CHARGER));
    TEST_ASSERT_EQUAL_UINT8(LOAD_NONE, verify_fault_mask(&v));
    TEST_ASSERT_EQUAL_UINT32(1, v.faults_total);   /* history is kept */
}

static void test_latency_is_measured_from_the_command(void)
{
    /* The operationally useful number: how long after ordering a shed do you
     * learn it never happened. */
    verify_setup(true);
    hold(LOAD_BASE | LOAD_CHARGER, LOAD_BASE | LOAD_CHARGER, 5000);
    hold(LOAD_BASE, LOAD_BASE | LOAD_CHARGER, GRACE_MS + 2000);

    uint32_t lat = 0;
    for (int i = 0; i < v.count; i++)
        if (v.loads[i].bit == LOAD_CHARGER) lat = v.loads[i].detect_latency_ms;

    TEST_ASSERT_TRUE(lat >= GRACE_MS);
    TEST_ASSERT_TRUE(lat <= GRACE_MS + CONFIRM * 100 + 200);
}

static void test_an_unwatched_load_reports_unknown(void)
{
    verify_setup(true);
    TEST_ASSERT_EQUAL_INT(ACT_UNKNOWN, verify_state(&v, 0x40));
}

/* =========================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_silence_means_no_loads);
    RUN_TEST(test_startup_anchors_on_whatever_is_already_running);
    RUN_TEST(test_no_answer_is_claimed_before_a_level_settles);
    RUN_TEST(test_expected_power_sums_the_estimates);

    RUN_TEST(test_switch_on_is_attributed_to_one_load);
    RUN_TEST(test_switch_off_is_attributed_too);
    RUN_TEST(test_a_big_step_is_the_charger_not_three_small_ones);
    RUN_TEST(test_simultaneous_switch_is_a_candidate_not_a_special_case);
    RUN_TEST(test_the_simple_explanation_wins);

    RUN_TEST(test_inrush_is_recognised_on_the_motor);
    RUN_TEST(test_inrush_tells_two_identical_loads_apart);

    RUN_TEST(test_thermal_ripple_does_not_fake_an_edge);
    RUN_TEST(test_a_warming_element_does_not_read_as_switching);
    RUN_TEST(test_a_stale_belief_is_never_reported_as_confident);

    RUN_TEST(test_adaptation_learns_the_real_wattage);
    RUN_TEST(test_adaptation_cannot_run_away);
    RUN_TEST(test_adaptation_can_be_disabled);

    RUN_TEST(test_an_unexplainable_step_is_rejected_not_guessed);
    RUN_TEST(test_the_anchor_corrects_a_belief_that_drifted);
    RUN_TEST(test_duplicate_and_empty_load_bits_are_rejected);

    RUN_TEST(test_agreement_is_reported_ok);
    RUN_TEST(test_the_grace_window_blocks_an_early_verdict);
    RUN_TEST(test_welded_contact_is_detected);
    RUN_TEST(test_failed_open_contact_is_detected);
    RUN_TEST(test_a_single_disagreement_is_not_a_fault);
    RUN_TEST(test_an_unconfident_classifier_is_not_evidence);
    RUN_TEST(test_a_new_command_discards_old_evidence);
    RUN_TEST(test_the_verifier_recovers_when_the_fault_clears);
    RUN_TEST(test_latency_is_measured_from_the_command);
    RUN_TEST(test_an_unwatched_load_reports_unknown);

    return UNITY_END();
}
