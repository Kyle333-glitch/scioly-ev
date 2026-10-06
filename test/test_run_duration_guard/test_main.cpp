#include <cmath>
#include <cstdint>
#include <limits>

#include <unity.h>

#include <RunDurationGuard.h>
#include <Units.h>
#include <VehicleState.h>

// Contract tests for the run-scoped maximum-duration bound. The numbers here are
// test fixtures only; they are deliberately not limit values for any real
// vehicle.

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

ev::Sourced<double> limit(double seconds, ev::ValueSource source) {
    return ev::Sourced<double>(seconds, source);
}

ev::Sourced<double> measuredLimit(double seconds) {
    return limit(seconds, ev::ValueSource::Measured);
}

}  // namespace

// 1. Only a measured, finite, positive duration bounds anything. A placeholder
// is data with no runtime effect, and a non-duration is not a bound at all.
void test_the_bound_is_usable_only_for_a_measured_positive_duration(void) {
    TEST_ASSERT_TRUE(ev::runDurationLimitUsable(measuredLimit(1.0)));
    TEST_ASSERT_TRUE(ev::runDurationLimitUsable(measuredLimit(0.001)));

    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(limit(1.0, ev::ValueSource::Provisional)));
    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(limit(1.0, ev::ValueSource::RuleDerived)));
    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(limit(1.0, ev::ValueSource::Derived)));

    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(measuredLimit(kNaN)));
    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(measuredLimit(kInf)));
    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(measuredLimit(0.0)));
    TEST_ASSERT_FALSE(ev::runDurationLimitUsable(measuredLimit(-1.0)));
}

// 2. An inactive run has no elapsed time, whatever start reading it was handed.
// This is what makes a run boundary (reset) and the interval after `Done` safe:
// no stale stamp can be turned into elapsed time once the run's clock is off.
void test_an_inactive_run_reports_no_elapsed_time_and_never_exceeds(void) {
    const ev::RunDurationVerdict verdict =
        ev::runDurationVerdict(false, 0U, 500000U, measuredLimit(0.001));
    TEST_ASSERT_FALSE(verdict.runActive);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.elapsedMs);
    TEST_ASSERT_TRUE(verdict.limitUsable);
    TEST_ASSERT_FALSE(verdict.exceeded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(ev::runDurationFault(verdict)));
}

// 3. The elapsed time is an unsigned difference, so it stays correct across the
// millis() wrap.
void test_the_elapsed_time_is_correct_across_the_clock_wrap(void) {
    const uint32_t start = 0xFFFFFF00U;  // 256 ms short of the wrap
    const uint32_t now = 0x00000100U;    // 256 ms past it
    const ev::RunDurationVerdict verdict =
        ev::runDurationVerdict(true, start, now, measuredLimit(1.0));
    TEST_ASSERT_TRUE(verdict.runActive);
    TEST_ASSERT_EQUAL_UINT32(512U, verdict.elapsedMs);
    TEST_ASSERT_FALSE(verdict.exceeded);

    // A 500 ms bound is already crossed by those same 512 ms.
    const ev::RunDurationVerdict crossed =
        ev::runDurationVerdict(true, start, now, measuredLimit(0.5));
    TEST_ASSERT_EQUAL_UINT32(512U, crossed.elapsedMs);
    TEST_ASSERT_TRUE(crossed.exceeded);
}

// 4. The bound is inclusive: a run is over its maximum at exactly the limit,
// never one cycle later.
void test_the_bound_is_crossed_at_exactly_the_limit(void) {
    const ev::RunDurationVerdict justUnder =
        ev::runDurationVerdict(true, 1000U, 1999U, measuredLimit(1.0));
    TEST_ASSERT_EQUAL_UINT32(999U, justUnder.elapsedMs);
    TEST_ASSERT_FALSE(justUnder.exceeded);

    const ev::RunDurationVerdict exactlyAt =
        ev::runDurationVerdict(true, 1000U, 2000U, measuredLimit(1.0));
    TEST_ASSERT_EQUAL_UINT32(1000U, exactlyAt.elapsedMs);
    TEST_ASSERT_TRUE(exactlyAt.exceeded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::RunDurationExceeded),
                          static_cast<int>(ev::runDurationFault(exactlyAt)));
}

// 5. A placeholder limit never bounds a run: a guessed duration must not stop a
// legal run, however long the run has been going. This is the production state
// of the limit until it is measured.
void test_a_placeholder_limit_never_bounds_a_run(void) {
    const ev::RunDurationVerdict verdict = ev::runDurationVerdict(
        true, 0U, 600000U, limit(0.001, ev::ValueSource::Provisional));
    TEST_ASSERT_TRUE(verdict.runActive);
    TEST_ASSERT_EQUAL_UINT32(600000U, verdict.elapsedMs);
    TEST_ASSERT_FALSE(verdict.limitUsable);
    TEST_ASSERT_FALSE(verdict.exceeded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(ev::runDurationFault(verdict)));
}

// 6. A start reading from the future (a reset clock) makes the unsigned
// difference enormous, which reads as over the bound. That is the same safe
// direction `sampleAge()` takes for a future stamp: an impossible interval is
// never mistaken for a fresh run.
void test_a_future_stamp_reads_as_over_the_bound(void) {
    const ev::RunDurationVerdict verdict =
        ev::runDurationVerdict(true, 5000U, 1000U, measuredLimit(30.0));
    TEST_ASSERT_TRUE(verdict.exceeded);
}

// 7. The fault this bound raises is its own identity, deliberately distinct from
// the per-phase fault, and it is not raised by a run that is within its bound.
void test_the_fault_identity_is_its_own_and_not_a_phase_timeout(void) {
    const ev::RunDurationVerdict within =
        ev::runDurationVerdict(true, 0U, 10U, measuredLimit(30.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(ev::runDurationFault(within)));

    const ev::RunDurationVerdict over =
        ev::runDurationVerdict(true, 0U, 30000U, measuredLimit(30.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::RunDurationExceeded),
                          static_cast<int>(ev::runDurationFault(over)));
    TEST_ASSERT_TRUE(ev::runDurationFault(over) != ev::FaultCode::PhaseTimeout);
}

// 8. Reset and re-arm: the verdict is a function of the start reading it is
// given, so a new-run boundary that re-stamps the run's actuation begins a fresh
// interval. Nothing from the previous run's elapsed time survives it.
void test_a_new_run_does_not_inherit_the_previous_runs_elapsed_time(void) {
    // A run that is well past a 1.0 s bound.
    const ev::RunDurationVerdict previous =
        ev::runDurationVerdict(true, 0U, 5000U, measuredLimit(1.0));
    TEST_ASSERT_TRUE(previous.exceeded);

    // The same evaluation with the run's own re-stamped actuation reading (the
    // reset/new-run boundary stamps a new start) is a fresh, short run.
    const ev::RunDurationVerdict freshRun =
        ev::runDurationVerdict(true, 5000U, 5000U, measuredLimit(1.0));
    TEST_ASSERT_TRUE(freshRun.runActive);
    TEST_ASSERT_EQUAL_UINT32(0U, freshRun.elapsedMs);
    TEST_ASSERT_FALSE(freshRun.exceeded);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_bound_is_usable_only_for_a_measured_positive_duration);
    RUN_TEST(test_an_inactive_run_reports_no_elapsed_time_and_never_exceeds);
    RUN_TEST(test_the_elapsed_time_is_correct_across_the_clock_wrap);
    RUN_TEST(test_the_bound_is_crossed_at_exactly_the_limit);
    RUN_TEST(test_a_placeholder_limit_never_bounds_a_run);
    RUN_TEST(test_a_future_stamp_reads_as_over_the_bound);
    RUN_TEST(test_the_fault_identity_is_its_own_and_not_a_phase_timeout);
    RUN_TEST(test_a_new_run_does_not_inherit_the_previous_runs_elapsed_time);
    return UNITY_END();
}
