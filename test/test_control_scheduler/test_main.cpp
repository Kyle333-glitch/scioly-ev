#include <cstdint>

#include <unity.h>

#include <ControlScheduler.h>

// Contract tests for the pure control-loop scheduler
// (lib/evcore/src/ControlScheduler.h): the deadline decision, the measured dt,
// the dropped-deadline policy and uint32 wraparound.
//
// The clock here is a plain integer sequence supplied by the test: there is no
// vehicle, no Arduino and no timer mock, because the module reads no clock at
// all. A passing test says nothing about how long anything takes on the real
// board - it is arithmetic verification of the scheduling logic.

void setUp(void) {}
void tearDown(void) {}

namespace {

const uint32_t kPeriodMs = 20U;  // 50 Hz: the initial planned cadence

// Asserts the parts of a decision that are not the dt, so a test that is about
// timing does not have to restate the whole struct.
void expectNotDue(uint32_t expectedWaitMs, const ev::ControlTickDecision& decision) {
    TEST_ASSERT_FALSE(decision.due);
    TEST_ASSERT_EQUAL_UINT32(0U, decision.lateByMs);
    TEST_ASSERT_EQUAL_UINT32(0U, decision.droppedTicks);
    TEST_ASSERT_EQUAL_UINT32(expectedWaitMs, decision.waitMs);
    // A pass that runs no tick claims no interval.
    TEST_ASSERT_EQUAL_UINT32(0U, decision.dtMs);
}

void expectDue(uint32_t expectedDtMs, uint32_t expectedLateByMs, uint32_t expectedDropped,
               const ev::ControlTickDecision& decision) {
    TEST_ASSERT_TRUE(decision.due);
    TEST_ASSERT_EQUAL_UINT32(expectedDtMs, decision.dtMs);
    TEST_ASSERT_EQUAL_UINT32(expectedLateByMs, decision.lateByMs);
    TEST_ASSERT_EQUAL_UINT32(expectedDropped, decision.droppedTicks);
    TEST_ASSERT_EQUAL_UINT32(0U, decision.waitMs);
}

}  // namespace

// A scheduler that was never started is inert: no deadline exists, so no pass is
// ever due. A layer that forgets to start the loop does no control work rather
// than doing it at millisecond zero.
void test_unstarted_scheduler_is_never_due(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    const ev::ControlTickDecision decision = scheduler.decide(123456U);
    expectNotDue(0U, decision);
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.ticks());

    // Committing without a start must not arm anything either.
    scheduler.advance(123456U);
    TEST_ASSERT_FALSE(scheduler.decide(123500U).due);
}

// A period of zero is not a cadence: it is treated as 1 ms so the deadline
// arithmetic and the dropped-deadline division stay defined.
void test_zero_period_is_defined_as_one_millisecond(void) {
    ev::ControlScheduler scheduler(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.periodMs());
    scheduler.start(0U);
    TEST_ASSERT_FALSE(scheduler.decide(0U).due);
    TEST_ASSERT_TRUE(scheduler.decide(1U).due);
}

// The first deadline is one period after start, and the first tick's dt is the
// measured interval since the start boundary - not the nominal period.
void test_first_deadline_is_one_period_after_start(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(1000U);

    expectNotDue(1U, scheduler.decide(1019U));

    const ev::ControlTickDecision decision = scheduler.decide(1020U);
    expectDue(20U, 0U, 0U, decision);

    scheduler.advance(1020U);
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.droppedDeadlines());
}

// A period is not assumed: every dt is the measured interval between the passes
// that actually ran, so an uneven loop reports uneven intervals.
void test_variable_dt_is_measured_not_nominal(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(0U);

    // First tick right on its deadline.
    const ev::ControlTickDecision first = scheduler.decide(20U);
    expectDue(20U, 0U, 0U, first);
    scheduler.advance(20U);

    // 25 ms pass: the next deadline is 40, so the tick is 5 ms late.
    const ev::ControlTickDecision second = scheduler.decide(45U);
    expectDue(25U, 5U, 0U, second);
    scheduler.advance(45U);

    // The late tick is followed by a full period, never by a short catch-up
    // interval: the deadline is 65, so 60 ms is still 5 ms early.
    expectNotDue(5U, scheduler.decide(60U));
    const ev::ControlTickDecision third = scheduler.decide(65U);
    expectDue(20U, 0U, 0U, third);
    scheduler.advance(65U);

    // 2 ms pass: the deadline is 85, so this pass is only 2 ms later.
    expectNotDue(18U, scheduler.decide(67U));
    const ev::ControlTickDecision fourth = scheduler.decide(85U);
    expectDue(20U, 0U, 0U, fourth);
    scheduler.advance(85U);

    TEST_ASSERT_EQUAL_UINT32(4U, scheduler.ticks());
}

// A pass that overruns by a whole period drops the deadlines it went through:
// they are counted and reported, never run back to back.
void test_missed_deadlines_are_dropped_and_counted(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(0U);

    const ev::ControlTickDecision first = scheduler.decide(20U);
    expectDue(20U, 0U, 0U, first);
    scheduler.advance(20U);

    // The pass that should have run at 40 ms returns at 75 ms: one deadline
    // (40) is unreachable and one tick is dropped.
    const ev::ControlTickDecision late = scheduler.decide(75U);
    expectDue(55U, 35U, 1U, late);
    scheduler.advance(75U);
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.droppedDeadlines());
    TEST_ASSERT_EQUAL_UINT32(2U, scheduler.ticks());

    // The next deadline is a full period after the tick that did run, so the
    // late tick is not answered with a short interval.
    expectNotDue(1U, scheduler.decide(94U));
    const ev::ControlTickDecision afterStall = scheduler.decide(95U);
    expectDue(20U, 0U, 0U, afterStall);
    scheduler.advance(95U);

    const ev::ControlTickDecision hugeStall = scheduler.decide(400U);
    expectDue(305U, 285U, 14U, hugeStall);  // 285 / 20 = 14 whole deadlines
    scheduler.advance(400U);
    TEST_ASSERT_EQUAL_UINT32(15U, scheduler.droppedDeadlines());
    TEST_ASSERT_EQUAL_UINT32(4U, scheduler.ticks());
}

// Exactly one period late is the boundary: the deadline that arrived with this
// pass is the one it satisfies, so dropping starts above it.
void test_dropping_starts_above_one_whole_period(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(0U);
    scheduler.decide(20U);
    scheduler.advance(20U);

    const ev::ControlTickDecision onTimeBoundary = scheduler.decide(40U);
    expectDue(20U, 0U, 0U, onTimeBoundary);

    const ev::ControlTickDecision oneTickOver = scheduler.decide(60U);
    expectDue(40U, 20U, 1U, oneTickOver);
}

// A clock that does not advance runs no tick twice for one millisecond, and a
// stalled loop simply stops ticking.
void test_stalled_clock_does_not_retick(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(0U);

    const ev::ControlTickDecision first = scheduler.decide(20U);
    expectDue(20U, 0U, 0U, first);
    scheduler.advance(20U);

    // Same millisecond again: the deadline has moved on, so nothing is due.
    expectNotDue(20U, scheduler.decide(20U));
    expectNotDue(20U, scheduler.decide(20U));

    // Still 20 ms before the deadline, so the same decision repeats.
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.ticks());

    // The next tick's dt is the measured interval since the last tick that ran,
    // so the modules downstream see real elapsed time rather than a nominal
    // period.
    const ev::ControlTickDecision afterStall = scheduler.decide(40U);
    expectDue(20U, 0U, 0U, afterStall);
}

// A reading older than the previous one is reported, and cannot make a tick
// happen: the deadline is in the past relative to the old reading, so the new
// one waits instead. Nothing here latches a fault - the vehicle layer decides
// what a rewound clock means.
void test_rewound_clock_is_reported_and_not_due(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(1000U);

    TEST_ASSERT_FALSE(scheduler.decide(1020U).clockRewind);
    scheduler.advance(1020U);

    const ev::ControlTickDecision rewound = scheduler.decide(1010U);
    TEST_ASSERT_TRUE(rewound.clockRewind);
    expectNotDue(30U, rewound);

    // The reading catches back up: it is due on the deadline, with the dt
    // measured from the last tick that actually ran.
    const ev::ControlTickDecision caughtUp = scheduler.decide(1050U);
    TEST_ASSERT_FALSE(caughtUp.clockRewind);
    expectDue(30U, 10U, 0U, caughtUp);

    // A rewound reading is still a reading: the decision after one is not itself
    // reported as a rewind.
    scheduler.advance(1050U);
    TEST_ASSERT_FALSE(scheduler.decide(1070U).clockRewind);
}

// The scheduling arithmetic is the same across the uint32 wrap at ~49.7 days,
// and the dt of a tick that straddles it is a real interval.
void test_deadline_and_dt_are_wraparound_safe(void) {
    ev::ControlScheduler scheduler(32U);
    scheduler.start(0xFFFFFFF0u);  // 16 ms before the wrap

    expectNotDue(1U, scheduler.decide(0x0000000Fu));

    const ev::ControlTickDecision acrossWrap = scheduler.decide(0x00000010u);
    expectDue(32U, 0U, 0U, acrossWrap);
    TEST_ASSERT_FALSE(acrossWrap.clockRewind);
    scheduler.advance(0x00000010u);

    const ev::ControlTickDecision next = scheduler.decide(0x00000030u);
    expectDue(32U, 0U, 0U, next);
    scheduler.advance(0x00000030u);

    TEST_ASSERT_EQUAL_UINT32(2U, scheduler.ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.droppedDeadlines());

    // An overrun that crosses the wrap drops deadlines the same way.
    // The tick that ran last straddled the wrap at 0x30; this pass returns at
    // 0xB4, 100 ms past the deadline at 0x50.
    const ev::ControlTickDecision overrun = scheduler.decide(0x000000B4u);
    expectDue(132U, 100U, 3U, overrun);
}

// Waiting for a deadline that sits on the far side of the wrap reports a small
// wait, not a huge one.
void test_wait_across_the_wrap_is_a_small_interval(void) {
    ev::ControlScheduler scheduler(1000U);
    scheduler.start(0xFFFFFC18u);  // 1000 ms before the wrap

    expectNotDue(1U, scheduler.decide(0xFFFFFFFFu));
    const ev::ControlTickDecision decision = scheduler.decide(0x00000000u);
    expectDue(1000U, 0U, 0U, decision);
}

// Restarting re-arms the schedule from a new boundary and forgets the counters:
// it is the loop's own boundary, not a vehicle event.
void test_start_rearms_from_a_new_boundary(void) {
    ev::ControlScheduler scheduler(kPeriodMs);
    scheduler.start(0U);
    scheduler.decide(20U);
    scheduler.advance(20U);
    TEST_ASSERT_EQUAL_UINT32(1U, scheduler.ticks());

    scheduler.start(10000U);
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, scheduler.droppedDeadlines());
    expectNotDue(19U, scheduler.decide(10001U));
    const ev::ControlTickDecision decision = scheduler.decide(10020U);
    expectDue(20U, 0U, 0U, decision);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unstarted_scheduler_is_never_due);
    RUN_TEST(test_zero_period_is_defined_as_one_millisecond);
    RUN_TEST(test_first_deadline_is_one_period_after_start);
    RUN_TEST(test_variable_dt_is_measured_not_nominal);
    RUN_TEST(test_missed_deadlines_are_dropped_and_counted);
    RUN_TEST(test_dropping_starts_above_one_whole_period);
    RUN_TEST(test_stalled_clock_does_not_retick);
    RUN_TEST(test_rewound_clock_is_reported_and_not_due);
    RUN_TEST(test_deadline_and_dt_are_wraparound_safe);
    RUN_TEST(test_wait_across_the_wrap_is_a_small_interval);
    RUN_TEST(test_start_rearms_from_a_new_boundary);
    return UNITY_END();
}
