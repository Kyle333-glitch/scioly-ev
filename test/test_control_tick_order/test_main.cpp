#include <cstdint>

#include <unity.h>

#include <ControlTickOrder.h>

// Contract tests for the frozen control-tick stage order and its guard
// (lib/evcore/src/ControlTickOrder.h).
//
// The tests pin the order itself - the enum values, one by one - and then every
// way a tick can fail to present it: a skipped stage, a repeated stage, a
// completion with stages missing, a stage with no tick begun, and a second tick
// begun while the first was unfinished.
//
// This is bookkeeping verification only. It says nothing about what the stages
// do on the vehicle, and it is not evidence that any measurement, run or motor
// command behaves correctly.

// Pinned at compile time: the vocabulary's ends and its zero value.
static_assert(static_cast<int>(ev::ControlTickStage::EncoderSnapshot) == 0,
              "the first stage must stay the zero value");
static_assert(static_cast<int>(ev::ControlTickStage::TelemetryCapture) == 9,
              "the last stage must stay the last");
static_assert(ev::kControlTickStageCount == 10U, "the stage count must match the vocabulary");
static_assert(static_cast<int>(ev::ControlTickOrderError::None) == 0,
              "no violation must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

// The ten stages in the frozen order, so a test walks the vocabulary the
// production runtime walks.
const ev::ControlTickStage kOrder[ev::kControlTickStageCount] = {
    ev::ControlTickStage::EncoderSnapshot,     ev::ControlTickStage::OdometryAndVelocity,
    ev::ControlTickStage::SensorSnapshot,      ev::ControlTickStage::FaultEvaluation,
    ev::ControlTickStage::RunStateMachine,     ev::ControlTickStage::RunMotionRequest,
    ev::ControlTickStage::MotionControl,       ev::ControlTickStage::SafetyAuthorization,
    ev::ControlTickStage::MotorCommand,        ev::ControlTickStage::TelemetryCapture,
};

// Runs one complete, in-order tick.
void runCleanTick(ev::ControlTickOrderGuard& guard) {
    guard.begin();
    for (uint8_t i = 0U; i < ev::kControlTickStageCount; ++i) {
        guard.enter(kOrder[i]);
    }
    guard.finish();
}

void expectVerdict(bool expectedOk, ev::ControlTickOrderError expectedError,
                   ev::ControlTickStage expectedStage, const ev::ControlTickOrderGuard& guard) {
    TEST_ASSERT_EQUAL_INT(expectedOk, guard.tickInOrder());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedOk), static_cast<int>(guard.tickVerdict().ok));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedError),
                          static_cast<int>(guard.tickVerdict().error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStage),
                          static_cast<int>(guard.tickVerdict().stage));
}

}  // namespace

// The stage order is the meaning of the vocabulary: the enum values are pinned
// here one by one so a reordering is a test failure, not a silent refactor.
void test_stage_order_is_pinned(void) {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(ev::ControlTickStage::EncoderSnapshot));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(ev::ControlTickStage::OdometryAndVelocity));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(ev::ControlTickStage::SensorSnapshot));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(ev::ControlTickStage::FaultEvaluation));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(ev::ControlTickStage::RunStateMachine));
    TEST_ASSERT_EQUAL_INT(5, static_cast<int>(ev::ControlTickStage::RunMotionRequest));
    TEST_ASSERT_EQUAL_INT(6, static_cast<int>(ev::ControlTickStage::MotionControl));
    TEST_ASSERT_EQUAL_INT(7, static_cast<int>(ev::ControlTickStage::SafetyAuthorization));
    TEST_ASSERT_EQUAL_INT(8, static_cast<int>(ev::ControlTickStage::MotorCommand));
    TEST_ASSERT_EQUAL_INT(9, static_cast<int>(ev::ControlTickStage::TelemetryCapture));
    TEST_ASSERT_EQUAL_UINT8(10U, ev::kControlTickStageCount);
}

// The nominal tick: every stage once, in order.
void test_complete_tick_in_order_is_accepted(void) {
    ev::ControlTickOrderGuard guard;
    runCleanTick(guard);

    expectVerdict(true, ev::ControlTickOrderError::None, ev::ControlTickStage::EncoderSnapshot,
                  guard);
    TEST_ASSERT_FALSE(guard.tickInProgress());
    TEST_ASSERT_EQUAL_UINT8(ev::kControlTickStageCount, guard.stagesEntered());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(0U, guard.violatedTicks());
    TEST_ASSERT_EQUAL_UINT32(0U, guard.abandonedTicks());
}

// Several ticks in a row stay independent: the guard is not a one-shot.
void test_repeated_ticks_are_counted(void) {
    ev::ControlTickOrderGuard guard;
    for (uint32_t i = 0U; i < 5U; ++i) {
        runCleanTick(guard);
    }
    TEST_ASSERT_EQUAL_UINT32(5U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(0U, guard.violatedTicks());
    TEST_ASSERT_TRUE(guard.tickInOrder());
}

// A skipped stage is a violation naming the stage that arrived, and the tick
// cannot become clean again afterwards.
void test_skipped_stage_is_out_of_order(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(ev::ControlTickStage::EncoderSnapshot);
    guard.enter(ev::ControlTickStage::OdometryAndVelocity);
    // SensorSnapshot is skipped: FaultEvaluation arrives where it was expected.
    guard.enter(ev::ControlTickStage::FaultEvaluation);

    expectVerdict(false, ev::ControlTickOrderError::StageOutOfOrder,
                  ev::ControlTickStage::FaultEvaluation, guard);
    TEST_ASSERT_TRUE(guard.tickInProgress());

    // Presenting the remaining stages cannot repair the tick.
    guard.enter(ev::ControlTickStage::RunStateMachine);
    guard.enter(ev::ControlTickStage::RunMotionRequest);
    guard.enter(ev::ControlTickStage::MotionControl);
    guard.enter(ev::ControlTickStage::SafetyAuthorization);
    guard.enter(ev::ControlTickStage::MotorCommand);
    guard.enter(ev::ControlTickStage::TelemetryCapture);
    guard.finish();

    expectVerdict(false, ev::ControlTickOrderError::StageOutOfOrder,
                  ev::ControlTickStage::FaultEvaluation, guard);
    TEST_ASSERT_EQUAL_UINT32(0U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.violatedTicks());
}

// A repeated stage is out of order: each stage happens exactly once per tick.
void test_repeated_stage_is_out_of_order(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(ev::ControlTickStage::EncoderSnapshot);
    guard.enter(ev::ControlTickStage::OdometryAndVelocity);
    guard.enter(ev::ControlTickStage::OdometryAndVelocity);

    expectVerdict(false, ev::ControlTickOrderError::StageOutOfOrder,
                  ev::ControlTickStage::OdometryAndVelocity, guard);
}

// A value outside the vocabulary - a corrupted stage, a stale cast - is out of
// order rather than accepted as a stage.
void test_stage_outside_the_vocabulary_is_refused(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(static_cast<ev::ControlTickStage>(200U));

    expectVerdict(false, ev::ControlTickOrderError::StageOutOfOrder,
                  static_cast<ev::ControlTickStage>(200U), guard);
    TEST_ASSERT_EQUAL_UINT8(0U, guard.stagesEntered());
}

// Completing a tick with stages still missing is a violation naming the first
// stage that never arrived.
void test_incomplete_tick_names_the_first_missing_stage(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(ev::ControlTickStage::EncoderSnapshot);
    guard.enter(ev::ControlTickStage::OdometryAndVelocity);
    guard.finish();

    expectVerdict(false, ev::ControlTickOrderError::TickIncomplete,
                  ev::ControlTickStage::SensorSnapshot, guard);
    TEST_ASSERT_FALSE(guard.tickInProgress());
    TEST_ASSERT_EQUAL_UINT32(0U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.violatedTicks());
}

// A tick that never began: a stage or a completion with no begin() is refused.
void test_stage_without_begin_is_refused(void) {
    ev::ControlTickOrderGuard guard;
    guard.enter(ev::ControlTickStage::EncoderSnapshot);

    expectVerdict(false, ev::ControlTickOrderError::TickNotStarted,
                  ev::ControlTickStage::EncoderSnapshot, guard);
    TEST_ASSERT_EQUAL_UINT8(0U, guard.stagesEntered());
}

void test_finish_without_begin_is_refused(void) {
    ev::ControlTickOrderGuard guard;
    guard.finish();

    expectVerdict(false, ev::ControlTickOrderError::TickNotStarted,
                  ev::ControlTickStage::EncoderSnapshot, guard);
    TEST_ASSERT_EQUAL_UINT32(0U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(0U, guard.violatedTicks());
}

// A second begin() while the first tick is unfinished is counted as abandoned,
// and the resuming tick's record starts dirty so the abandonment cannot be
// forgotten. The tick after that one starts clean, so the guard reports both the
// history (the counter) and the current cycle truthfully.
void test_second_begin_abandons_the_unfinished_tick(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(ev::ControlTickStage::EncoderSnapshot);
    guard.begin();  // the first tick never finished

    expectVerdict(false, ev::ControlTickOrderError::TickWasAbandoned,
                  ev::ControlTickStage::EncoderSnapshot, guard);
    TEST_ASSERT_TRUE(guard.tickInProgress());

    for (uint8_t i = 0U; i < ev::kControlTickStageCount; ++i) {
        guard.enter(kOrder[i]);
    }
    guard.finish();

    TEST_ASSERT_EQUAL_UINT32(0U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.violatedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.abandonedTicks());

    // The next tick is a fresh record.
    runCleanTick(guard);
    TEST_ASSERT_TRUE(guard.tickInOrder());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.violatedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.abandonedTicks());
}

// A violated tick does not poison the next one: the verdict is per tick, and the
// counters keep the history.
void test_a_violation_does_not_poison_the_next_tick(void) {
    ev::ControlTickOrderGuard guard;
    guard.begin();
    guard.enter(ev::ControlTickStage::MotorCommand);
    guard.finish();
    TEST_ASSERT_FALSE(guard.tickInOrder());

    runCleanTick(guard);
    expectVerdict(true, ev::ControlTickOrderError::None, ev::ControlTickStage::EncoderSnapshot,
                  guard);
    TEST_ASSERT_EQUAL_UINT32(1U, guard.completedTicks());
    TEST_ASSERT_EQUAL_UINT32(1U, guard.violatedTicks());
}

// `stagesEntered()` is the progress of the tick in progress, and the stage count
// is what a complete tick reports.
void test_stage_progress_is_reported(void) {
    ev::ControlTickOrderGuard guard;
    TEST_ASSERT_EQUAL_UINT8(0U, guard.stagesEntered());

    guard.begin();
    TEST_ASSERT_TRUE(guard.tickInProgress());
    guard.enter(ev::ControlTickStage::EncoderSnapshot);
    TEST_ASSERT_EQUAL_UINT8(1U, guard.stagesEntered());
    for (uint8_t i = 1U; i < ev::kControlTickStageCount; ++i) {
        guard.enter(kOrder[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(ev::kControlTickStageCount, guard.stagesEntered());
    guard.finish();
    TEST_ASSERT_FALSE(guard.tickInProgress());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_order_is_pinned);
    RUN_TEST(test_complete_tick_in_order_is_accepted);
    RUN_TEST(test_repeated_ticks_are_counted);
    RUN_TEST(test_skipped_stage_is_out_of_order);
    RUN_TEST(test_repeated_stage_is_out_of_order);
    RUN_TEST(test_stage_outside_the_vocabulary_is_refused);
    RUN_TEST(test_incomplete_tick_names_the_first_missing_stage);
    RUN_TEST(test_stage_without_begin_is_refused);
    RUN_TEST(test_finish_without_begin_is_refused);
    RUN_TEST(test_second_begin_abandons_the_unfinished_tick);
    RUN_TEST(test_a_violation_does_not_poison_the_next_tick);
    RUN_TEST(test_stage_progress_is_reported);
    return UNITY_END();
}
