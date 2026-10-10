#include <unity.h>
#include <string.h>
#include "forwarder.h"

/* ---------------------------------------------------------------------------
 * Store-and-forward, verified without WiFi, a broker, or a board.
 *
 * The whole feature is "a reader that refuses to advance while the link is
 * down", so it can be driven entirely by calling forwarder_set_link() and
 * counting what comes out. These tests are the proof that the outage demo
 * works -- the flaky part (actual WiFi) is not where the logic lives.
 * -------------------------------------------------------------------------*/

#define BUDGET     16
#define THRESHOLD  8

static telemetry_ring_t ring;
static forwarder_t      fwd;
static telemetry_sample_t batch[BUDGET];

void setUp(void)
{
    ring_init(&ring);
    forwarder_init(&fwd, &ring, THRESHOLD);
}
void tearDown(void) {}

static void push_n(uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        telemetry_sample_t s;
        memset(&s, 0, sizeof(s));
        s.p = (float)k;          /* payload we can assert ordering on */
        ring_push(&ring, &s);
    }
}

/* Drain everything available, in batches, returning the total. */
static size_t drain_all(void)
{
    size_t total = 0, n;
    while ((n = forwarder_take(&fwd, batch, BUDGET)) > 0) total += n;
    return total;
}

/* --- the link must start closed ----------------------------------------- */

/* A forwarder that assumed the link was up would publish into a void and
 * lose the first samples before the first connection even completes. */
static void test_link_starts_down(void)
{
    push_n(5);
    TEST_ASSERT_EQUAL_UINT32(0, forwarder_take(&fwd, batch, BUDGET));
    TEST_ASSERT_EQUAL_UINT32(5, forwarder_backlog(&fwd));
}

/* --- normal operation --------------------------------------------------- */

static void test_live_samples_are_not_flagged_backfill(void)
{
    forwarder_set_link(&fwd, true);
    push_n(5);

    size_t n = forwarder_take(&fwd, batch, BUDGET);
    TEST_ASSERT_EQUAL_UINT32(5, n);

    for (size_t k = 0; k < n; k++) {
        TEST_ASSERT_FALSE(batch[k].flags & TLM_FLAG_BACKFILL);
        TEST_ASSERT_EQUAL_FLOAT((float)k, batch[k].p);   /* oldest first */
    }
    TEST_ASSERT_EQUAL_UINT32(5, fwd.published);
    TEST_ASSERT_EQUAL_UINT32(0, fwd.backfilled);
}

/* --- the outage ---------------------------------------------------------- */

/* The core of the feature: nothing is published while down, nothing is lost,
 * and the backlog is exactly what accumulated. */
static void test_outage_buffers_instead_of_discarding(void)
{
    forwarder_set_link(&fwd, true);
    push_n(3);
    TEST_ASSERT_EQUAL_UINT32(3, forwarder_take(&fwd, batch, BUDGET));

    forwarder_set_link(&fwd, false);
    push_n(50);

    /* Repeated attempts must all refuse, not drain a little each time. */
    for (int k = 0; k < 5; k++)
        TEST_ASSERT_EQUAL_UINT32(0, forwarder_take(&fwd, batch, BUDGET));

    TEST_ASSERT_EQUAL_UINT32(50, forwarder_backlog(&fwd));
    TEST_ASSERT_EQUAL_UINT32(0,  forwarder_dropped(&fwd));
    TEST_ASSERT_EQUAL_UINT32(1,  fwd.outages);
}

/* Reconnect: the gap fills in, oldest first, tagged as late. */
static void test_reconnect_backfills_in_order(void)
{
    forwarder_set_link(&fwd, false);
    push_n(50);
    forwarder_set_link(&fwd, true);

    size_t total = 0;
    float expect = 0.0f;
    size_t n;
    while ((n = forwarder_take(&fwd, batch, BUDGET)) > 0) {
        TEST_ASSERT_TRUE(n <= BUDGET);          /* budget respected */
        for (size_t k = 0; k < n; k++) {
            TEST_ASSERT_EQUAL_FLOAT(expect, batch[k].p);   /* strict order */
            expect += 1.0f;
        }
        total += n;
    }

    TEST_ASSERT_EQUAL_UINT32(50, total);
    TEST_ASSERT_EQUAL_UINT32(50, fwd.published);
    TEST_ASSERT_EQUAL_UINT32(0,  forwarder_dropped(&fwd));
    TEST_ASSERT_EQUAL_UINT32(50, fwd.max_backlog);
}

/* The backlog is large, so the batch must be marked as history, not live. */
static void test_backfilled_samples_are_flagged(void)
{
    forwarder_set_link(&fwd, false);
    push_n(THRESHOLD + 20);
    forwarder_set_link(&fwd, true);

    size_t n = forwarder_take(&fwd, batch, BUDGET);
    TEST_ASSERT_TRUE(n > 0);
    for (size_t k = 0; k < n; k++)
        TEST_ASSERT_TRUE(batch[k].flags & TLM_FLAG_BACKFILL);
}

/* A small backlog is just normal jitter, not an outage -- do not cry wolf. */
static void test_small_backlog_is_not_called_backfill(void)
{
    forwarder_set_link(&fwd, true);
    push_n(THRESHOLD);                 /* at the threshold, not above it */

    size_t n = forwarder_take(&fwd, batch, BUDGET);
    TEST_ASSERT_EQUAL_UINT32(THRESHOLD, n);
    for (size_t k = 0; k < n; k++)
        TEST_ASSERT_FALSE(batch[k].flags & TLM_FLAG_BACKFILL);
}

/* --- the limit, stated honestly ----------------------------------------- */

/* An outage longer than the buffer is real data loss. What must NOT happen is
 * pretending otherwise: every sample is either published or counted as
 * dropped, and the two must add up. */
static void test_outage_beyond_buffer_reports_exact_loss(void)
{
    forwarder_set_link(&fwd, false);

    const uint32_t pushed = TELEMETRY_RING_SIZE + 100;
    push_n(pushed);

    forwarder_set_link(&fwd, true);
    size_t published = drain_all();

    uint32_t dropped = forwarder_dropped(&fwd);

    TEST_ASSERT_EQUAL_UINT32(100, dropped);                 /* the overflow */
    TEST_ASSERT_EQUAL_UINT32(TELEMETRY_RING_SIZE, published);
    TEST_ASSERT_EQUAL_UINT32(pushed, published + dropped);  /* nothing vanishes
                                                             * unaccounted for */
}

/* After a drop, what survives must still be the NEWEST window, oldest-first.
 * Backfilling stale data while discarding recent data would be worse than
 * dropping cleanly. */
static void test_after_loss_survivors_are_the_newest_window(void)
{
    forwarder_set_link(&fwd, false);
    const uint32_t pushed = TELEMETRY_RING_SIZE + 100;
    push_n(pushed);
    forwarder_set_link(&fwd, true);

    size_t n = forwarder_take(&fwd, batch, BUDGET);
    TEST_ASSERT_TRUE(n > 0);

    /* First survivor is sample #100 (0..99 were overwritten). */
    TEST_ASSERT_EQUAL_FLOAT(100.0f, batch[0].p);
}

/* --- repeated outages --------------------------------------------------- */
static void test_multiple_outages_are_counted(void)
{
    forwarder_set_link(&fwd, true);
    for (int k = 0; k < 3; k++) {
        push_n(4);
        drain_all();
        forwarder_set_link(&fwd, false);
        push_n(4);
        forwarder_set_link(&fwd, true);
        drain_all();
    }
    TEST_ASSERT_EQUAL_UINT32(3,  fwd.outages);
    TEST_ASSERT_EQUAL_UINT32(24, fwd.published);
    TEST_ASSERT_EQUAL_UINT32(0,  forwarder_dropped(&fwd));
}

static void test_zero_budget_takes_nothing(void)
{
    forwarder_set_link(&fwd, true);
    push_n(5);
    TEST_ASSERT_EQUAL_UINT32(0, forwarder_take(&fwd, batch, 0));
    TEST_ASSERT_EQUAL_UINT32(5, forwarder_backlog(&fwd));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_link_starts_down);
    RUN_TEST(test_live_samples_are_not_flagged_backfill);
    RUN_TEST(test_outage_buffers_instead_of_discarding);
    RUN_TEST(test_reconnect_backfills_in_order);
    RUN_TEST(test_backfilled_samples_are_flagged);
    RUN_TEST(test_small_backlog_is_not_called_backfill);
    RUN_TEST(test_outage_beyond_buffer_reports_exact_loss);
    RUN_TEST(test_after_loss_survivors_are_the_newest_window);
    RUN_TEST(test_multiple_outages_are_counted);
    RUN_TEST(test_zero_budget_takes_nothing);
    return UNITY_END();
}
