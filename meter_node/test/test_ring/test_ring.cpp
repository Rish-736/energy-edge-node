#include <unity.h>
#include "ring_buffer.h"

static telemetry_ring_t   ring;
static telemetry_reader_t rd;

static void push_n(uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        telemetry_sample_t s = {};
        s.p = (float)k;          /* payload we can assert on */
        ring_push(&ring, &s);
    }
}

void setUp(void)    { ring_init(&ring); }
void tearDown(void) {}

static void test_empty_reader_gets_nothing(void)
{
    telemetry_sample_t s;
    ring_reader_attach(&ring, &rd, "t");
    TEST_ASSERT_FALSE(ring_read(&ring, &rd, &s));
    TEST_ASSERT_EQUAL_UINT32(0, ring_available(&ring, &rd));
}

static void test_fifo_order_and_seq(void)
{
    ring_reader_attach(&ring, &rd, "t");
    push_n(5);

    TEST_ASSERT_EQUAL_UINT32(5, ring_available(&ring, &rd));

    telemetry_sample_t s;
    for (uint32_t k = 0; k < 5; k++) {
        TEST_ASSERT_TRUE(ring_read(&ring, &rd, &s));
        TEST_ASSERT_EQUAL_UINT32(k, s.seq);       /* writer stamps seq */
        TEST_ASSERT_EQUAL_FLOAT((float)k, s.p);   /* and in order      */
    }
    TEST_ASSERT_FALSE(ring_read(&ring, &rd, &s));
}

/* A reader that attaches late must get live data, not a stale backlog. */
static void test_late_reader_starts_live(void)
{
    push_n(10);
    ring_reader_attach(&ring, &rd, "late");

    telemetry_sample_t s;
    TEST_ASSERT_FALSE(ring_read(&ring, &rd, &s));

    push_n(1);
    TEST_ASSERT_TRUE(ring_read(&ring, &rd, &s));
    TEST_ASSERT_EQUAL_UINT32(10, s.seq);
}

/* The case that matters: a stalled consumer gets lapped. It must resync to
 * the oldest surviving sample and report exactly how many it lost. */
static void test_overrun_is_counted_not_hidden(void)
{
    ring_reader_attach(&ring, &rd, "slow");

    const uint32_t extra = 10;
    push_n(TELEMETRY_RING_SIZE + extra);

    telemetry_sample_t s;
    TEST_ASSERT_TRUE(ring_read(&ring, &rd, &s));

    TEST_ASSERT_EQUAL_UINT32(extra, rd.overruns);
    TEST_ASSERT_EQUAL_UINT32(extra, s.seq);   /* oldest still in the buffer */
}

/* Two readers must not interfere -- draining one leaves the other untouched. */
static void test_readers_are_independent(void)
{
    telemetry_reader_t a, b;
    ring_reader_attach(&ring, &a, "a");
    ring_reader_attach(&ring, &b, "b");

    push_n(3);

    telemetry_sample_t s;
    while (ring_read(&ring, &a, &s)) { }

    TEST_ASSERT_EQUAL_UINT32(0, ring_available(&ring, &a));
    TEST_ASSERT_EQUAL_UINT32(3, ring_available(&ring, &b));
}

static void test_truth_string_rendering(void)
{
    char buf[32];
    telemetry_truth_str(LOAD_NONE, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("off", buf);

    telemetry_truth_str(LOAD_HEATER, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("heater", buf);

    telemetry_truth_str(LOAD_LIGHTS | LOAD_MOTOR, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("lights+motor", buf);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_reader_gets_nothing);
    RUN_TEST(test_fifo_order_and_seq);
    RUN_TEST(test_late_reader_starts_live);
    RUN_TEST(test_overrun_is_counted_not_hidden);
    RUN_TEST(test_readers_are_independent);
    RUN_TEST(test_truth_string_rendering);
    return UNITY_END();
}
