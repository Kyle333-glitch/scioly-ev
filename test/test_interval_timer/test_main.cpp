#include <unity.h>

#include <IntervalTimer.h>

// Smoke test for real production code (lib/evcore/src/IntervalTimer.h).
// Runs in the host `native` environment; no Arduino dependency.

void setUp(void) {}
void tearDown(void) {}

void test_first_call_establishes_reference_without_firing(void) {
    IntervalTimer timer(500);
    TEST_ASSERT_FALSE(timer.elapsed(0));
    TEST_ASSERT_FALSE(timer.elapsed(499));
}

void test_fires_at_exactly_one_interval(void) {
    IntervalTimer timer(500);
    TEST_ASSERT_FALSE(timer.elapsed(1000));
    TEST_ASSERT_TRUE(timer.elapsed(1500));
}

void test_fires_once_per_interval(void) {
    IntervalTimer timer(500);
    TEST_ASSERT_FALSE(timer.elapsed(0));
    TEST_ASSERT_TRUE(timer.elapsed(500));
    // Immediately after firing, the timer must not fire again.
    TEST_ASSERT_FALSE(timer.elapsed(500));
    TEST_ASSERT_FALSE(timer.elapsed(999));
    TEST_ASSERT_TRUE(timer.elapsed(1000));
}

void test_firing_is_wraparound_safe(void) {
    // 0xFFFFFFF0 + 32 ms wraps past the uint32_t boundary.
    IntervalTimer timer(32);
    TEST_ASSERT_FALSE(timer.elapsed(0xFFFFFFF0u));
    TEST_ASSERT_FALSE(timer.elapsed(0x0000000Fu));
    TEST_ASSERT_TRUE(timer.elapsed(0x00000010u));
}

void test_reset_rearms_from_new_timestamp(void) {
    IntervalTimer timer(500);
    TEST_ASSERT_FALSE(timer.elapsed(0));
    TEST_ASSERT_TRUE(timer.elapsed(500));

    timer.reset(5000);
    TEST_ASSERT_FALSE(timer.elapsed(5499));
    TEST_ASSERT_TRUE(timer.elapsed(5500));
}

void test_interval_can_be_changed(void) {
    IntervalTimer timer(500);
    timer.setIntervalMs(100);
    TEST_ASSERT_EQUAL_UINT32(100, timer.intervalMs());

    TEST_ASSERT_FALSE(timer.elapsed(0));
    TEST_ASSERT_FALSE(timer.elapsed(99));
    TEST_ASSERT_TRUE(timer.elapsed(100));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_call_establishes_reference_without_firing);
    RUN_TEST(test_fires_at_exactly_one_interval);
    RUN_TEST(test_fires_once_per_interval);
    RUN_TEST(test_firing_is_wraparound_safe);
    RUN_TEST(test_reset_rearms_from_new_timestamp);
    RUN_TEST(test_interval_can_be_changed);
    return UNITY_END();
}
