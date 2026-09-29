#include <cstdint>

#include <unity.h>

#include <WatchdogPlan.h>

// Contract tests for the watchdog timeout-qualification rule
// (lib/evcore/src/WatchdogPlan.h): the timeout has to cover BOTH the
// boot-to-first-refresh gap and the worst healthy refresh interval, plus margin.
//
// The bug this pins is the boring one: a rule written only in terms of
// "interval between consecutive refreshes" cannot see the boot gap, because the
// first refresh has no predecessor. A timeout chosen that way is long enough for
// every interval H0W measures and still short enough to reset the board during
// setup, which is a board that never boots rather than a board that fails a test.
//
// This is arithmetic verification. No timer, no target and no measurement is
// involved, and a pass here says nothing about how long anything takes on the
// real hardware.

void setUp(void) {}
void tearDown(void) {}

namespace {

// A qualification whose boot gap is the larger one.
constexpr ev::WatchdogQualification bootDominated{120U, 40U, 25U};

// A qualification whose steady-state interval is the larger one.
constexpr ev::WatchdogQualification refreshDominated{20U, 90U, 10U};

}  // namespace

// The rule takes the larger of the two gaps, not whichever one the caller
// happened to be thinking about.
void test_worst_gap_is_the_larger_of_the_two(void) {
    TEST_ASSERT_EQUAL_UINT32(120U, ev::worstWatchdogGapMs(bootDominated));
    TEST_ASSERT_EQUAL_UINT32(90U, ev::worstWatchdogGapMs(refreshDominated));
    TEST_ASSERT_EQUAL_UINT32(50U, ev::worstWatchdogGapMs(ev::WatchdogQualification{50U, 50U, 0U}));
}

// The required timeout is the larger gap plus the margin.
void test_required_timeout_adds_the_margin_to_the_larger_gap(void) {
    TEST_ASSERT_EQUAL_UINT32(145U, ev::requiredWatchdogTimeoutMs(bootDominated));
    TEST_ASSERT_EQUAL_UINT32(100U, ev::requiredWatchdogTimeoutMs(refreshDominated));
}

// A timeout that covers the steady-state interval but not the boot gap is NOT
// qualified. This is the failure mode the module exists for.
void test_a_timeout_covering_only_the_refresh_interval_is_refused(void) {
    // 60 ms is above the 40 ms worst refresh interval and far below the 120 ms
    // boot gap.
    TEST_ASSERT_TRUE(ev::timeoutCoversRefreshInterval(60U, bootDominated));
    TEST_ASSERT_FALSE(ev::timeoutCoversBoot(60U, bootDominated));
    TEST_ASSERT_FALSE(ev::timeoutIsQualified(60U, bootDominated));
}

// A timeout that covers the boot gap but is too short for the steady-state
// interval is refused just as firmly: both gaps have to hold.
void test_a_timeout_covering_only_the_boot_gap_is_refused(void) {
    // 40 ms equals the boot gap of refreshDominated and is below its 90 ms
    // interval.
    TEST_ASSERT_FALSE(ev::timeoutCoversRefreshInterval(40U, refreshDominated));
    TEST_ASSERT_FALSE(ev::timeoutIsQualified(40U, refreshDominated));
}

// Coverage is strict: a timeout exactly equal to a gap expires at the same
// instant the next refresh is due, and which the hardware does first is not
// something this firmware may assume.
void test_coverage_is_strict_and_equality_is_not_coverage(void) {
    TEST_ASSERT_FALSE(ev::timeoutCoversBoot(120U, bootDominated));
    TEST_ASSERT_FALSE(ev::timeoutCoversRefreshInterval(40U, bootDominated));
    TEST_ASSERT_TRUE(ev::timeoutCoversBoot(121U, bootDominated));
    TEST_ASSERT_TRUE(ev::timeoutCoversRefreshInterval(41U, bootDominated));
}

// The margin is part of the rule, not decoration: equal to the required value
// passes, one below it does not.
void test_the_margin_is_required_not_decorative(void) {
    TEST_ASSERT_TRUE(ev::timeoutIsQualified(145U, bootDominated));
    TEST_ASSERT_FALSE(ev::timeoutIsQualified(144U, bootDominated));
}

// A qualified timeout is always longer than both gaps, so a caller that applies
// this rule cannot produce a timeout that expires inside either of them.
void test_a_qualified_timeout_is_above_both_gaps(void) {
    const uint32_t chosen = ev::requiredWatchdogTimeoutMs(bootDominated);
    TEST_ASSERT_TRUE(ev::timeoutIsQualified(chosen, bootDominated));
    TEST_ASSERT_TRUE(chosen > bootDominated.bootToFirstRefreshMs);
    TEST_ASSERT_TRUE(chosen > bootDominated.worstRefreshIntervalMs);
}

// Arithmetic that would overflow saturates at the largest representable value
// instead of wrapping: a wrapped result would be a *shorter* timeout than the gap
// it is supposed to cover.
void test_margin_addition_saturates_instead_of_wrapping(void) {
    const ev::WatchdogQualification huge{0xFFFFFF00U, 10U, 0xFFFFU};
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, ev::requiredWatchdogTimeoutMs(huge));
}

// A zero margin is accepted and means "no margin": an explicit decision, not a
// default, and still strictly above both gaps.
void test_a_zero_margin_still_requires_strict_coverage(void) {
    const ev::WatchdogQualification noMargin{100U, 100U, 0U};
    TEST_ASSERT_EQUAL_UINT32(100U, ev::requiredWatchdogTimeoutMs(noMargin));
    TEST_ASSERT_FALSE(ev::timeoutIsQualified(100U, noMargin));
    TEST_ASSERT_TRUE(ev::timeoutIsQualified(101U, noMargin));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_worst_gap_is_the_larger_of_the_two);
    RUN_TEST(test_required_timeout_adds_the_margin_to_the_larger_gap);
    RUN_TEST(test_a_timeout_covering_only_the_refresh_interval_is_refused);
    RUN_TEST(test_a_timeout_covering_only_the_boot_gap_is_refused);
    RUN_TEST(test_coverage_is_strict_and_equality_is_not_coverage);
    RUN_TEST(test_the_margin_is_required_not_decorative);
    RUN_TEST(test_a_qualified_timeout_is_above_both_gaps);
    RUN_TEST(test_margin_addition_saturates_instead_of_wrapping);
    RUN_TEST(test_a_zero_margin_still_requires_strict_coverage);
    return UNITY_END();
}
