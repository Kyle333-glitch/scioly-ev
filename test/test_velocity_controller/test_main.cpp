#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <Units.h>
#include <VelocityController.h>

// Contract tests for the candidate feed-forward + PI velocity regulator.
//
// Every gain, ceiling and velocity below is a test fixture. None is a tuning
// value for the vehicle: no gain has been measured (VERIFIED_STATE.md #8), so
// nothing here is a recommendation and no fixture is a placeholder that any
// firmware path may use. The controller itself owns no gains at all.
//
// The tests answer specific questions: does the regulator saturate where it
// says it does, does it stop integrating when the error pushes further into a
// limit, does it use the elapsed time it was given rather than a nominal
// period, does it refuse to drive on unmeasured or corrupt data, and does it
// ever ask for powered effort against the commanded direction?

// Pinned at compile time.
static_assert(!std::is_default_constructible<ev::VelocityControlResult>::value,
              "a control result must never default-construct into a valid-looking answer");
static_assert(!std::is_default_constructible<ev::VelocityControllerConfig>::value,
              "a configuration must always have to state its gains and polarity");
static_assert(static_cast<int>(ev::VelocityControlStatus::Ok) == 0,
              "the ok status must stay the zero value");
static_assert(static_cast<int>(ev::MotorMode::Disabled) == 0,
              "the disabled mode must stay the zero value");
// The regulator must not be able to authorize its own request: the only type
// that can reach the motor driver has a private constructor with SafetyAuthority
// as its friend.
static_assert(!std::is_constructible<ev::AuthorizedMotorCommand, ev::MotorEffort>::value,
              "no module outside the safety boundary may build an authorized command");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Term comparisons use an explicit tolerance rather than double equality, so
// this target does not depend on optional Unity double support.
const double kPermilleTolerance = 1e-9;

ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisionalValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

ev::Sourced<ev::MotorMode> measuredMode(ev::MotorMode mode) {
    return ev::Sourced<ev::MotorMode>(mode, ev::ValueSource::Measured);
}

// Fixture: 300 permille per m/s of feed-forward, 200 per m/s of error, 100 per
// (m/s x s) of integrated error, a 600 permille ceiling, a 300 permille integral
// ceiling, and at most 50 ms of one integration step.
ev::VelocityControllerConfig fixtureConfig() {
    return ev::VelocityControllerConfig(measuredValue(300.0), measuredValue(200.0),
                                        measuredValue(100.0),
                                        measuredMode(ev::MotorMode::DriveForward),
                                        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U,
                                        0.05);
}

// Same ceiling and step limit as the fixture, with only integral action, so an
// integral accumulation can be read without the other terms in the way.
ev::VelocityControllerConfig integralOnlyConfig(double integralGain, uint16_t integralCeiling) {
    return ev::VelocityControllerConfig(measuredValue(0.0), measuredValue(0.0),
                                        measuredValue(integralGain),
                                        measuredMode(ev::MotorMode::DriveForward),
                                        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured),
                                        integralCeiling, 0.05);
}

// A refusal carries no computed demand, no elapsed time and no flags. The
// reported integral is the state at the moment of refusal - 0 for most
// refusals, and the preserved value for an invalid target - so each caller that
// cares checks it explicitly.
void expectRefused(ev::VelocityControlStatus expectedStatus,
                   const ev::VelocityControlResult& result) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStatus), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(result.effort.mode));
    TEST_ASSERT_EQUAL_UINT16(0U, result.effort.magnitudePermille);
    TEST_ASSERT_TRUE(result.feedForwardPermille == 0.0);
    TEST_ASSERT_TRUE(result.proportionalPermille == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, result.dtMs);
    TEST_ASSERT_TRUE(!result.outputSaturated && !result.integrationStepClamped &&
                     !result.antiWindupActive && !result.integralResetForSignChange);
    // A refusal must always be a legal effort, never something the boundary has
    // to reject for being malformed.
    TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
}

void expectEffort(ev::MotorMode expectedMode, uint16_t expectedPermille,
                  const ev::VelocityControlResult& result) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedMode), static_cast<int>(result.effort.mode));
    TEST_ASSERT_EQUAL_UINT16(expectedPermille, result.effort.magnitudePermille);
    TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
}

void expectTerm(double expectedPermille, double actualPermille) {
    TEST_ASSERT_TRUE(std::fabs(actualPermille - expectedPermille) <= kPermilleTolerance);
}

}  // namespace

// --- Configuration ----------------------------------------------------------

void test_configuration_validation_requires_measured_values(void) {
    const ev::VelocityControllerConfig provisionalFeedForward(
        provisionalValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::ValuesAreNotMeasured),
        static_cast<int>(ev::validateVelocityControllerConfig(provisionalFeedForward).error));

    // The polarity is the UNVERIFIED fact (#4): a provisional value must stop
    // the regulator rather than guess a direction.
    const ev::VelocityControllerConfig provisionalPolarity(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        ev::Sourced<ev::MotorMode>(ev::MotorMode::DriveForward, ev::ValueSource::Provisional),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::ValuesAreNotMeasured),
        static_cast<int>(ev::validateVelocityControllerConfig(provisionalPolarity).error));

    const ev::VelocityControllerConfig provisionalCeiling(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Provisional), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::ValuesAreNotMeasured),
        static_cast<int>(ev::validateVelocityControllerConfig(provisionalCeiling).error));
}

void test_configuration_validation_rejects_malformed_values(void) {
    const ev::VelocityControllerConfig nanFeedForward(
        measuredValue(kNaN), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::NonFiniteValue),
        static_cast<int>(ev::validateVelocityControllerConfig(nanFeedForward).error));

    const ev::VelocityControllerConfig negativeGain(
        measuredValue(300.0), measuredValue(-1.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::NegativeGain),
        static_cast<int>(ev::validateVelocityControllerConfig(negativeGain).error));

    const ev::VelocityControllerConfig effortOutOfRange(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(1001U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::EffortCeilingOutOfRange),
        static_cast<int>(ev::validateVelocityControllerConfig(effortOutOfRange).error));

    const ev::VelocityControllerConfig integralOutOfRange(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 1001U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::IntegralCeilingOutOfRange),
        static_cast<int>(ev::validateVelocityControllerConfig(integralOutOfRange).error));

    // A non-driving polarity is not a polarity.
    const ev::VelocityControllerConfig notDriving(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::Brake),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::PositiveEffortModeIsNotDriving),
        static_cast<int>(ev::validateVelocityControllerConfig(notDriving).error));

    const ev::VelocityControllerConfig zeroStep(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.0);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityControllerConfigError::IntegrationStepNotPositive),
        static_cast<int>(ev::validateVelocityControllerConfig(zeroStep).error));

    TEST_ASSERT_TRUE(ev::validateVelocityControllerConfig(fixtureConfig()).ok);
}

void test_an_unusable_configuration_cannot_be_enabled_and_never_drives(void) {
    const ev::VelocityControllerConfig unusable(provisionalValue(300.0), measuredValue(200.0),
                                                measuredValue(100.0),
                                                measuredMode(ev::MotorMode::DriveForward),
                                                ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured),
                                                300U, 0.05);
    ev::VelocityController controller(unusable);
    TEST_ASSERT_FALSE(controller.configUsable());
    TEST_ASSERT_FALSE(controller.enable());
    TEST_ASSERT_FALSE(controller.isEnabled());

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, true, 0.0);
    expectRefused(ev::VelocityControlStatus::Disabled, result);
}

void test_nothing_drives_before_enable_and_disabling_drops_the_state(void) {
    ev::VelocityController controller(fixtureConfig());
    const ev::VelocityControlResult beforeEnable = controller.update(1000U, 1.0, true, 1.0);
    expectRefused(ev::VelocityControlStatus::Disabled, beforeEnable);

    TEST_ASSERT_TRUE(controller.enable());
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    TEST_ASSERT_TRUE(controller.integralPermille() > 0.0);

    controller.disable();
    TEST_ASSERT_FALSE(controller.isEnabled());
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectRefused(ev::VelocityControlStatus::Disabled, controller.update(1040U, 1.0, true, 0.0));
}

// --- Feed-forward and proportional terms ------------------------------------

void test_while_the_estimator_warms_up_the_regulator_runs_feed_forward_only(void) {
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, false, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(result.status));
    expectTerm(300.0, result.feedForwardPermille);
    expectTerm(0.0, result.proportionalPermille);
    // No measurement, no error, and nothing integrated: a missing estimate is
    // never treated as a measurement of zero.
    TEST_ASSERT_TRUE(result.errorMps == 0.0);
    TEST_ASSERT_TRUE(result.integralPermille == 0.0);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectEffort(ev::MotorMode::DriveForward, 300U, result);
}

void test_an_unusable_measurement_that_is_not_a_number_still_runs_feed_forward_only(void) {
    // Absence and corruption are different things: absence means open-loop,
    // corruption means refuse.
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, false, kNaN);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::DriveForward, 300U, result);
}

void test_zero_error_holds_the_feed_forward_term(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, true, 1.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    expectTerm(300.0, result.feedForwardPermille);
    expectTerm(0.0, result.proportionalPermille);
    TEST_ASSERT_TRUE(result.errorMps == 0.0);
    TEST_ASSERT_FALSE(result.outputSaturated);
    expectEffort(ev::MotorMode::DriveForward, 300U, result);
}

void test_positive_error_adds_proportional_effort(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, true, 0.5);
    TEST_ASSERT_TRUE(std::fabs(result.errorMps - 0.5) <= 1e-12);
    expectTerm(300.0, result.feedForwardPermille);
    expectTerm(100.0, result.proportionalPermille);
    expectEffort(ev::MotorMode::DriveForward, 400U, result);
}

void test_negative_error_reduces_effort_toward_zero(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult result = controller.update(1000U, 1.0, true, 1.5);
    TEST_ASSERT_TRUE(std::fabs(result.errorMps + 0.5) <= 1e-12);
    expectTerm(-100.0, result.proportionalPermille);
    expectEffort(ev::MotorMode::DriveForward, 200U, result);
}

void test_an_overspeed_never_asks_for_powered_effort_the_other_way(void) {
    // Target 0.3 m/s while moving 3.0 m/s: the demand is negative. This
    // regulator has no authority to reverse; it asks for no powered effort, and
    // the run state machine's deliberate decelerate/brake/settle path handles
    // the stop. Note that the request is a refusal, not a DriveReverse.
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult result = controller.update(1000U, 0.3, true, 3.0);
    expectTerm(90.0, result.feedForwardPermille);
    expectTerm(-540.0, result.proportionalPermille);
    TEST_ASSERT_TRUE(result.outputSaturated);
    expectEffort(ev::MotorMode::Disabled, 0U, result);
}

void test_the_polarity_is_configuration_not_an_assumption(void) {
    const ev::VelocityControllerConfig forwardPositive(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    const ev::VelocityControllerConfig reversePositive(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(ev::MotorMode::DriveReverse),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);

    ev::VelocityController controllerA(forwardPositive);
    controllerA.enable();
    const ev::VelocityControlResult negativeA = controllerA.update(1000U, -1.0, true, -1.0);
    expectEffort(ev::MotorMode::DriveReverse, 300U, negativeA);

    ev::VelocityController controllerB(reversePositive);
    controllerB.enable();
    const ev::VelocityControlResult negativeB = controllerB.update(1000U, -1.0, true, -1.0);
    expectEffort(ev::MotorMode::DriveForward, 300U, negativeB);
}

void test_effort_is_clamped_to_the_configured_ceiling(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult result = controller.update(1000U, 3.0, true, 0.0);
    expectTerm(900.0, result.feedForwardPermille);
    expectTerm(600.0, result.proportionalPermille);
    TEST_ASSERT_TRUE(result.outputSaturated);
    expectEffort(ev::MotorMode::DriveForward, 600U, result);
}

void test_a_zero_target_asks_for_no_powered_effort(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    // A zero target collapses the effort interval to {0}: there is no powered
    // effort to ask for, and the demand is exactly at that interval rather than
    // outside it.
    const ev::VelocityControlResult atRest = controller.update(1000U, 0.0, true, 0.0);
    TEST_ASSERT_FALSE(atRest.outputSaturated);
    expectEffort(ev::MotorMode::Disabled, 0U, atRest);

    const ev::VelocityControlResult stillRolling = controller.update(1020U, 0.0, true, 0.5);
    expectEffort(ev::MotorMode::Disabled, 0U, stillRolling);
    TEST_ASSERT_TRUE(stillRolling.errorMps < 0.0);
}

void test_an_effort_that_quantizes_to_zero_is_disabled_not_a_zero_drive(void) {
    // 1 permille per m/s: 0.4 m/s rounds to 0 permille, which must be emitted as
    // Disabled rather than as a Drive with magnitude 0, because a zero-effort
    // Drive still latches a direction in the safety boundary.
    const ev::VelocityControllerConfig tiny(
        measuredValue(1.0), measuredValue(0.0), measuredValue(0.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 300U, 0.05);
    ev::VelocityController controller(tiny);
    controller.enable();

    expectEffort(ev::MotorMode::Disabled, 0U, controller.update(1000U, 0.4, true, 0.4));
    expectEffort(ev::MotorMode::DriveForward, 1U, controller.update(1020U, 0.5, true, 0.5));
    expectEffort(ev::MotorMode::Disabled, 0U, controller.update(1040U, -0.4, true, -0.4));
    expectEffort(ev::MotorMode::DriveReverse, 1U, controller.update(1060U, -0.5, true, -0.5));
}

// --- Integral action, measured time and anti-windup --------------------------

void test_the_integral_accumulates_over_the_measured_elapsed_time(void) {
    ev::VelocityController controller(integralOnlyConfig(100.0, 300U));
    controller.enable();

    // First cycle: no previous stamp, so there is nothing to measure against and
    // nothing integrates.
    const ev::VelocityControlResult first = controller.update(1000U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, first.dtMs);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);

    const ev::VelocityControlResult second = controller.update(1020U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(20U, second.dtMs);
    expectTerm(2.0, controller.integralPermille());
    expectEffort(ev::MotorMode::DriveForward, 2U, second);

    const ev::VelocityControlResult third = controller.update(1040U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(20U, third.dtMs);
    expectTerm(4.0, controller.integralPermille());
    expectEffort(ev::MotorMode::DriveForward, 4U, third);
}

void test_the_actual_dt_is_used_never_a_nominal_period(void) {
    ev::VelocityController controller(integralOnlyConfig(100.0, 300U));
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);

    // 37 ms, not 20 ms: the integral grows by 100 * 1.0 * 0.037.
    const ev::VelocityControlResult longStep = controller.update(1037U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(37U, longStep.dtMs);
    expectTerm(3.7, controller.integralPermille());

    // 5 ms, not 20 ms either.
    const ev::VelocityControlResult shortStep = controller.update(1042U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(5U, shortStep.dtMs);
    expectTerm(4.2, controller.integralPermille());
}

void test_a_stalled_loop_is_clamped_to_the_max_integration_step(void) {
    ev::VelocityController controller(integralOnlyConfig(100.0, 300U));
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);

    // 500 ms of elapsed time is reported honestly, but only 50 ms of it is
    // integrated: one late cycle cannot wind the integral up by a whole stall.
    const ev::VelocityControlResult stalled = controller.update(1500U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(500U, stalled.dtMs);
    TEST_ASSERT_TRUE(stalled.integrationStepClamped);
    expectTerm(5.0, controller.integralPermille());
}

void test_integration_stops_while_the_demand_is_saturated_and_the_error_pushes(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 2.0, true, 0.0);

    // Demand is 600 (feed-forward) + 400 (proportional) = 1000 against a ceiling
    // of 600, and the error is positive: integrating would only wind up.
    const ev::VelocityControlResult saturated = controller.update(1020U, 2.0, true, 0.0);
    TEST_ASSERT_TRUE(saturated.outputSaturated);
    TEST_ASSERT_TRUE(saturated.antiWindupActive);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectEffort(ev::MotorMode::DriveForward, 600U, saturated);
}

void test_integration_resumes_once_the_error_no_longer_pushes_into_the_limit(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 2.0, true, 0.0);
    controller.update(1020U, 2.0, true, 0.0);

    // A smaller target leaves room under the ceiling, so the same positive error
    // now integrates normally.
    const ev::VelocityControlResult resumed = controller.update(1040U, 0.2, true, 0.0);
    TEST_ASSERT_FALSE(resumed.outputSaturated);
    TEST_ASSERT_FALSE(resumed.antiWindupActive);
    expectTerm(0.4, controller.integralPermille());
    expectEffort(ev::MotorMode::DriveForward, 100U, resumed);
}

void test_the_integral_term_is_clamped_to_its_own_ceiling(void) {
    ev::VelocityController controller(integralOnlyConfig(1000.0, 3U));
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);

    // One 50 ms step of this gain would add 50 permille; the term ceiling stops
    // it at 3, and it stays there on every later step.
    ev::VelocityControlResult result = controller.update(1050U, 1.0, true, 0.0);
    expectTerm(3.0, controller.integralPermille());
    expectEffort(ev::MotorMode::DriveForward, 3U, result);

    result = controller.update(1100U, 1.0, true, 0.0);
    expectTerm(3.0, controller.integralPermille());
}

void test_an_integral_ceiling_of_zero_disables_integral_action(void) {
    // A deliberate way to run feed-forward + proportional only.
    ev::VelocityController controller(integralOnlyConfig(100.0, 0U));
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
}

// --- Direction changes ------------------------------------------------------

void test_a_target_sign_change_drops_the_integral_accumulated_for_the_old_one(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());

    const ev::VelocityControlResult reversed = controller.update(1060U, -1.0, true, 0.0);
    TEST_ASSERT_TRUE(reversed.integralResetForSignChange);
    // The reset happens before this cycle integrates, so the new integral is
    // only this cycle's contribution: 100 * (-1.0) * 0.02.
    expectTerm(-2.0, controller.integralPermille());
    expectTerm(-300.0, reversed.feedForwardPermille);
    expectTerm(-200.0, reversed.proportionalPermille);
    // -300 - 200 - 2 = -502 permille, all of it in the new direction.
    expectEffort(ev::MotorMode::DriveReverse, 502U, reversed);
}

void test_a_transition_to_a_zero_target_also_drops_the_integral(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);

    const ev::VelocityControlResult stopping = controller.update(1060U, 0.0, true, 0.0);
    TEST_ASSERT_TRUE(stopping.integralResetForSignChange);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectEffort(ev::MotorMode::Disabled, 0U, stopping);
}

void test_a_same_sign_target_never_drops_the_integral(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    expectTerm(2.0, controller.integralPermille());

    const ev::VelocityControlResult sameSign = controller.update(1040U, 0.9, true, 0.0);
    TEST_ASSERT_FALSE(sameSign.integralResetForSignChange);
    // 2 permille from before, plus 100 * 0.9 * 0.02 for this cycle.
    expectTerm(3.8, controller.integralPermille());
}

// --- Invalid input and clock behavior ---------------------------------------

void test_a_non_finite_target_is_refused_and_preserves_the_integral(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());

    const ev::VelocityControlResult invalidTarget =
        controller.update(1060U, kNaN, true, 0.0);
    expectRefused(ev::VelocityControlStatus::InvalidTarget, invalidTarget);
    // A refusal for a bad target reports the integral it is still holding, so a
    // log shows the loop state rather than a zero that was never true.
    expectTerm(4.0, invalidTarget.integralPermille);
    TEST_ASSERT_TRUE(controller.integralPermille() == 4.0);
    expectRefused(ev::VelocityControlStatus::InvalidTarget,
                  controller.update(1080U, kInf, true, 0.0));
    expectRefused(ev::VelocityControlStatus::InvalidTarget,
                  controller.update(1100U, -kInf, true, 0.0));
    TEST_ASSERT_TRUE(controller.integralPermille() == 4.0);

    // The regulator is still sound, so a valid target resumes from where it was
    // rather than from a state the refusal invented. The refused cycles did not
    // advance the time base, so this step sees the whole 80 ms since the last
    // accepted cycle; the integration step limit caps that at 50 ms, and the
    // preserved 4 permille becomes 4 + 100 * 1.0 * 0.05.
    const ev::VelocityControlResult resumed = controller.update(1120U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(resumed.status));
    TEST_ASSERT_EQUAL_UINT32(80U, resumed.dtMs);
    TEST_ASSERT_TRUE(resumed.integrationStepClamped);
    expectTerm(9.0, controller.integralPermille());
}

void test_a_corrupt_measurement_is_refused_and_drops_the_integral(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());

    // Usable, but not a number: the feedback path contradicted itself.
    expectRefused(ev::VelocityControlStatus::InvalidMeasurement,
                  controller.update(1060U, 1.0, true, kNaN));
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);

    // A refused cycle does not advance the time base, so the next accepted cycle
    // integrates the whole interval since the last accepted step - here 40 ms -
    // starting from the dropped integral rather than from the corrupt history.
    // A long refusal stretch is bounded by maxIntegrationStepS.
    controller.update(1080U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());
}

void test_a_non_advancing_stamp_freezes_integration_but_still_regulates(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.5);

    const ev::VelocityControlResult repeated = controller.update(1000U, 1.0, true, 0.5);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::TimeDidNotAdvance),
                          static_cast<int>(repeated.status));
    TEST_ASSERT_EQUAL_UINT32(0U, repeated.dtMs);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    // Feed-forward and proportional do not need time, so the effort is still
    // computed; only the integral is frozen.
    expectEffort(ev::MotorMode::DriveForward, 400U, repeated);

    // A rewound stamp is refused the same way and is not adopted as the time
    // base either.
    const ev::VelocityControlResult rewound = controller.update(900U, 1.0, true, 0.5);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::TimeDidNotAdvance),
                          static_cast<int>(rewound.status));
    expectEffort(ev::MotorMode::DriveForward, 400U, rewound);

    const ev::VelocityControlResult advancing = controller.update(1010U, 1.0, true, 0.5);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(advancing.status));
    TEST_ASSERT_EQUAL_UINT32(10U, advancing.dtMs);
    expectTerm(0.5, controller.integralPermille());
}

void test_enabling_an_already_enabled_regulator_does_not_disturb_the_loop(void) {
    // enable() on a running regulator is a no-op rather than a reset: a caller
    // that enables every cycle must not silently wipe the integral and the time
    // base. Only enable() after disable() or reset() starts a fresh state.
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());

    TEST_ASSERT_TRUE(controller.enable());
    TEST_ASSERT_TRUE(controller.isEnabled());
    expectTerm(4.0, controller.integralPermille());

    // The time base survived too, so this step integrates 20 ms and not nothing.
    const ev::VelocityControlResult continued = controller.update(1060U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(20U, continued.dtMs);
    expectTerm(6.0, controller.integralPermille());
    expectEffort(ev::MotorMode::DriveForward, 506U, continued);
}

void test_the_time_base_survives_millis_wraparound(void) {
    ev::VelocityController controller(integralOnlyConfig(100.0, 300U));
    controller.enable();
    const uint32_t beforeWrapMs = 0xFFFFFFF0U;
    controller.update(beforeWrapMs, 1.0, true, 0.0);

    // 41 ms later, across the uint32 rollover.
    const ev::VelocityControlResult result = controller.update(0x19U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(41U, result.dtMs);
    expectTerm(4.1, controller.integralPermille());
}

// --- Reset ------------------------------------------------------------------

void test_reset_returns_the_regulator_to_its_construction_state(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    TEST_ASSERT_TRUE(controller.integralPermille() > 0.0);

    controller.reset();
    TEST_ASSERT_FALSE(controller.isEnabled());
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectRefused(ev::VelocityControlStatus::Disabled, controller.update(1040U, 1.0, true, 0.0));

    // Nothing resumes: a later enable starts with no integral and no time base.
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult firstAfterReset = controller.update(2000U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(firstAfterReset.status));
    TEST_ASSERT_EQUAL_UINT32(0U, firstAfterReset.dtMs);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
}

void test_re_enabling_never_resumes_a_previous_control_state(void) {
    ev::VelocityController controller(fixtureConfig());
    controller.enable();
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.disable();

    TEST_ASSERT_TRUE(controller.enable());
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    const ev::VelocityControlResult result = controller.update(5000U, 1.0, true, 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, result.dtMs);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
}

// --- Property sweeps --------------------------------------------------------

void test_every_output_is_a_legal_effort_within_the_ceiling(void) {
    const double targets[] = {-3.0, -0.5, 0.0, 0.25, 1.0, 3.0, kNaN, kInf};
    const double measurements[] = {-2.0, 0.0, 0.5, 1.0, 2.5, kNaN, kInf};
    const bool usableFlags[] = {false, true};
    const uint32_t stamps[] = {1000U, 1020U, 1020U, 1600U};

    for (double target : targets) {
        for (double measurement : measurements) {
            for (bool usable : usableFlags) {
                ev::VelocityController controller(fixtureConfig());
                controller.enable();
                ev::VelocityControlResult result =
                    controller.update(1000U, target, usable, measurement);
                for (uint32_t stamp : stamps) {
                    result = controller.update(stamp, target, usable, measurement);
                    TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
                    TEST_ASSERT_TRUE(result.effort.magnitudePermille <= 600U);
                    if (!ev::isDrivingMode(result.effort.mode)) {
                        TEST_ASSERT_EQUAL_UINT16(0U, result.effort.magnitudePermille);
                    }
                }
            }
        }
    }
}

void test_the_effort_never_asks_for_the_opposite_direction_of_the_target(void) {
    const double targets[] = {-3.0, -0.5, 0.0, 0.25, 1.0, 3.0};
    const double measurements[] = {-3.0, -0.5, 0.0, 0.5, 3.0};

    for (double target : targets) {
        for (double measurement : measurements) {
            ev::VelocityController controller(fixtureConfig());
            controller.enable();
            ev::VelocityControlResult result =
                controller.update(1000U, target, true, measurement);
            for (uint32_t stamp = 1020U; stamp <= 1100U; stamp += 20U) {
                result = controller.update(stamp, target, true, measurement);
                if (target > 0.0) {
                    TEST_ASSERT_TRUE(result.effort.mode == ev::MotorMode::DriveForward ||
                                     result.effort.mode == ev::MotorMode::Disabled);
                } else if (target < 0.0) {
                    TEST_ASSERT_TRUE(result.effort.mode == ev::MotorMode::DriveReverse ||
                                     result.effort.mode == ev::MotorMode::Disabled);
                } else {
                    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                                          static_cast<int>(result.effort.mode));
                }
                // Never Coast or Brake: choosing a non-powered motion action is
                // the run state machine's decision, not this module's.
                TEST_ASSERT_TRUE(result.effort.mode != ev::MotorMode::Coast);
                TEST_ASSERT_TRUE(result.effort.mode != ev::MotorMode::Brake);
            }
        }
    }
}

void test_a_measurement_moving_the_wrong_way_asks_for_effort_in_the_commanded_direction(void) {
    // Deliberate, not accidental: with a measured velocity opposing the target,
    // the error is simply large and the regulator asks for full effort in the
    // commanded direction. It does not detect the wrong-way condition and it has
    // no authority to reverse; what a stalled, slipping or backwards-moving
    // drivetrain means is the run state machine's and the fault path's problem.
    ev::VelocityController controller(fixtureConfig());
    controller.enable();

    const ev::VelocityControlResult opposite =
        controller.update(1000U, 1.0, true, -1.0);
    TEST_ASSERT_TRUE(std::fabs(opposite.errorMps - 2.0) <= 1e-12);
    expectTerm(300.0, opposite.feedForwardPermille);
    expectTerm(400.0, opposite.proportionalPermille);
    TEST_ASSERT_TRUE(opposite.outputSaturated);
    expectEffort(ev::MotorMode::DriveForward, 600U, opposite);

    const ev::VelocityControlResult oppositeReverse =
        controller.update(1020U, -1.0, true, 1.0);
    TEST_ASSERT_TRUE(std::fabs(oppositeReverse.errorMps + 2.0) <= 1e-12);
    expectEffort(ev::MotorMode::DriveReverse, 600U, oppositeReverse);

    // And with a zero target the interval is {0}: the wrong-way motion is not
    // answered with powered effort at all.
    const ev::VelocityControlResult zeroTarget = controller.update(1040U, 0.0, true, 1.0);
    expectEffort(ev::MotorMode::Disabled, 0U, zeroTarget);
}

void test_a_corrupt_measurement_never_asks_for_any_powered_effort(void) {
    const ev::VelocityControllerConfig patterns[] = {
        fixtureConfig(),
        integralOnlyConfig(100.0, 300U),
        ev::VelocityControllerConfig(measuredValue(0.0), measuredValue(0.0), measuredValue(0.0),
                                     measuredMode(ev::MotorMode::DriveReverse),
                                     ev::Sourced<uint16_t>(0U, ev::ValueSource::Measured), 0U, 0.05)};
    for (const ev::VelocityControllerConfig& config : patterns) {
        ev::VelocityController controller(config);
        TEST_ASSERT_TRUE(controller.enable() == controller.configUsable());
        for (uint32_t stamp = 1000U; stamp <= 1200U; stamp += 10U) {
            const ev::VelocityControlResult result = controller.update(stamp, 1.0, true, kNaN);
            TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                                  static_cast<int>(result.effort.mode));
            TEST_ASSERT_EQUAL_UINT16(0U, result.effort.magnitudePermille);
            TEST_ASSERT_TRUE(result.integralPermille == 0.0);
        }
    }
}

// --- R2/M18: explicit arithmetic refusal ------------------------------------
//
// Finite configuration can still produce non-finite intermediate arithmetic:
// a very large finite gain times a finite target or error overflows its term
// to an infinity, and opposite infinities cancel to NaN. The regulator must
// name that failure and refuse the cycle rather than convert it into an
// effort. Every value below is a synthetic host-only fixture, never a tuning
// recommendation: no gain has been measured.

// A config whose gains are extreme but finite and measured: validation
// accepts it (finiteness is not a cap), so the runtime arithmetic is what
// has to refuse.
ev::VelocityControllerConfig extremeGainConfig() {
    const double huge = 1.0e308;
    return ev::VelocityControllerConfig(measuredValue(huge), measuredValue(huge),
                                        measuredValue(0.0),
                                        measuredMode(ev::MotorMode::DriveForward),
                                        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 0U,
                                        0.05);
}

void test_opposite_infinite_terms_are_refused_as_arithmetic_not_as_no_request(void) {
    ev::VelocityController controller(extremeGainConfig());
    TEST_ASSERT_TRUE(controller.enable());
    // Feed-forward overflows to +inf (1e308 * 10) while the proportional term
    // overflows to -inf (1e308 * (10 - 20)): the demand is NaN.
    const ev::VelocityControlResult result = controller.update(1000U, 10.0, true, 20.0);
    expectRefused(ev::VelocityControlStatus::ArithmeticNotFinite, result);
    // The refusal must not advertise computed terms that were never finite.
    TEST_ASSERT_TRUE(result.feedForwardPermille == 0.0);
    TEST_ASSERT_TRUE(result.proportionalPermille == 0.0);
}

void test_a_negative_infinite_demand_never_commands_power(void) {
    // Fixture gains with an extreme but finite measurement: the proportional
    // term overflows to -inf while the other terms stay finite, so the demand
    // is -inf. Clamping that value would yield full reverse power; the
    // arithmetic refusal must fire before any conversion.
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result =
        controller.update(1000U, -10.0, true, 1.0e308);
    expectRefused(ev::VelocityControlStatus::ArithmeticNotFinite, result);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(result.effort.mode));
    TEST_ASSERT_EQUAL_UINT16(0U, result.effort.magnitudePermille);
}

void test_a_positive_infinite_demand_is_refused_not_saturated(void) {
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());
    // +inf demand with a positive target: saturation logic would otherwise
    // report a bounded powered effort as though the loop had regulated there.
    const ev::VelocityControlResult result =
        controller.update(1000U, 10.0, true, -1.0e308);
    expectRefused(ev::VelocityControlStatus::ArithmeticNotFinite, result);
    TEST_ASSERT_TRUE(!result.outputSaturated);
}

void test_large_representable_terms_still_saturate_at_the_ceiling(void) {
    // A demand that is merely large - every term finite - keeps the
    // intentional bounded-saturation behavior: Ok, powered, clamped.
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result = controller.update(1000U, 10.0, true, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::DriveForward, 600U, result);
    TEST_ASSERT_TRUE(result.outputSaturated);
    expectTerm(3000.0, result.feedForwardPermille);
    expectTerm(2000.0, result.proportionalPermille);
}

void test_a_demand_exactly_at_the_ceiling_is_not_reported_saturated(void) {
    ev::VelocityControllerConfig config(
        measuredValue(60.0), measuredValue(0.0), measuredValue(0.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 0U, 0.05);
    ev::VelocityController controller(config);
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result = controller.update(1000U, 10.0, true, 10.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::DriveForward, 600U, result);
    TEST_ASSERT_TRUE(!result.outputSaturated);
}

void test_zero_gains_ask_for_no_power_but_stay_ok(void) {
    ev::VelocityControllerConfig config(
        measuredValue(0.0), measuredValue(0.0), measuredValue(0.0),
        measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured), 0U, 0.05);
    ev::VelocityController controller(config);
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result = controller.update(1000U, 5.0, true, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(result.effort.mode));
    expectTerm(0.0, result.feedForwardPermille);
    expectTerm(0.0, result.proportionalPermille);
}

void test_negative_targets_drive_the_configured_reverse_mode(void) {
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result = controller.update(1000U, -2.0, true, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::DriveReverse, 600U, result);
}

void test_arithmetic_refusal_preserves_the_integral_like_an_invalid_target(void) {
    ev::VelocityController controller(fixtureConfig());
    TEST_ASSERT_TRUE(controller.enable());
    controller.update(1000U, 1.0, true, 0.0);
    controller.update(1020U, 1.0, true, 0.0);
    controller.update(1040U, 1.0, true, 0.0);
    expectTerm(4.0, controller.integralPermille());
    // An extreme finite measurement overflows only the proportional term;
    // the demand is -inf and the held integral must survive the refusal.
    const ev::VelocityControlResult refused =
        controller.update(1060U, 1.0, true, 1.0e308);
    expectRefused(ev::VelocityControlStatus::ArithmeticNotFinite, refused);
    expectTerm(4.0, refused.integralPermille);
    TEST_ASSERT_TRUE(controller.integralPermille() == 4.0);
}

void test_invalid_inputs_keep_precedence_over_the_arithmetic_check(void) {
    ev::VelocityController controller(extremeGainConfig());
    TEST_ASSERT_TRUE(controller.enable());
    // A non-finite target is the caller's fault, not the arithmetic's: the
    // target verdict wins even when the gains would also overflow.
    expectRefused(ev::VelocityControlStatus::InvalidTarget,
                  controller.update(1000U, kInf, true, 0.0));
    expectRefused(ev::VelocityControlStatus::InvalidTarget,
                  controller.update(1020U, -kInf, true, 0.0));
    // A self-contradicting measurement is refused as corrupt feedback first.
    expectRefused(ev::VelocityControlStatus::InvalidMeasurement,
                  controller.update(1040U, 1.0, true, kNaN));
    expectRefused(ev::VelocityControlStatus::InvalidMeasurement,
                  controller.update(1060U, 1.0, true, kInf));
}

void test_arithmetic_failure_overrides_a_frozen_stamp(void) {
    ev::VelocityController controller(extremeGainConfig());
    TEST_ASSERT_TRUE(controller.enable());
    controller.update(1000U, 10.0, true, 20.0);
    // The stamp does not advance, but the terms are still computed - and they
    // are still non-finite, so the refusal must not degrade into a regulating
    // TimeDidNotAdvance cycle carrying infinite diagnostics.
    const ev::VelocityControlResult result = controller.update(1000U, 10.0, true, 20.0);
    expectRefused(ev::VelocityControlStatus::ArithmeticNotFinite, result);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_configuration_validation_requires_measured_values);
    RUN_TEST(test_configuration_validation_rejects_malformed_values);
    RUN_TEST(test_an_unusable_configuration_cannot_be_enabled_and_never_drives);
    RUN_TEST(test_nothing_drives_before_enable_and_disabling_drops_the_state);

    RUN_TEST(test_while_the_estimator_warms_up_the_regulator_runs_feed_forward_only);
    RUN_TEST(test_an_unusable_measurement_that_is_not_a_number_still_runs_feed_forward_only);
    RUN_TEST(test_zero_error_holds_the_feed_forward_term);
    RUN_TEST(test_positive_error_adds_proportional_effort);
    RUN_TEST(test_negative_error_reduces_effort_toward_zero);
    RUN_TEST(test_an_overspeed_never_asks_for_powered_effort_the_other_way);
    RUN_TEST(test_the_polarity_is_configuration_not_an_assumption);
    RUN_TEST(test_effort_is_clamped_to_the_configured_ceiling);
    RUN_TEST(test_a_zero_target_asks_for_no_powered_effort);
    RUN_TEST(test_an_effort_that_quantizes_to_zero_is_disabled_not_a_zero_drive);

    RUN_TEST(test_the_integral_accumulates_over_the_measured_elapsed_time);
    RUN_TEST(test_the_actual_dt_is_used_never_a_nominal_period);
    RUN_TEST(test_a_stalled_loop_is_clamped_to_the_max_integration_step);
    RUN_TEST(test_integration_stops_while_the_demand_is_saturated_and_the_error_pushes);
    RUN_TEST(test_integration_resumes_once_the_error_no_longer_pushes_into_the_limit);
    RUN_TEST(test_the_integral_term_is_clamped_to_its_own_ceiling);
    RUN_TEST(test_an_integral_ceiling_of_zero_disables_integral_action);

    RUN_TEST(test_a_target_sign_change_drops_the_integral_accumulated_for_the_old_one);
    RUN_TEST(test_a_transition_to_a_zero_target_also_drops_the_integral);
    RUN_TEST(test_a_same_sign_target_never_drops_the_integral);

    RUN_TEST(test_a_non_finite_target_is_refused_and_preserves_the_integral);
    RUN_TEST(test_a_corrupt_measurement_is_refused_and_drops_the_integral);
    RUN_TEST(test_a_non_advancing_stamp_freezes_integration_but_still_regulates);
    RUN_TEST(test_enabling_an_already_enabled_regulator_does_not_disturb_the_loop);
    RUN_TEST(test_the_time_base_survives_millis_wraparound);

    RUN_TEST(test_reset_returns_the_regulator_to_its_construction_state);
    RUN_TEST(test_re_enabling_never_resumes_a_previous_control_state);

    RUN_TEST(test_every_output_is_a_legal_effort_within_the_ceiling);
    RUN_TEST(test_the_effort_never_asks_for_the_opposite_direction_of_the_target);
    RUN_TEST(test_a_measurement_moving_the_wrong_way_asks_for_effort_in_the_commanded_direction);
    RUN_TEST(test_a_corrupt_measurement_never_asks_for_any_powered_effort);

    RUN_TEST(test_opposite_infinite_terms_are_refused_as_arithmetic_not_as_no_request);
    RUN_TEST(test_a_negative_infinite_demand_never_commands_power);
    RUN_TEST(test_a_positive_infinite_demand_is_refused_not_saturated);
    RUN_TEST(test_large_representable_terms_still_saturate_at_the_ceiling);
    RUN_TEST(test_a_demand_exactly_at_the_ceiling_is_not_reported_saturated);
    RUN_TEST(test_zero_gains_ask_for_no_power_but_stay_ok);
    RUN_TEST(test_negative_targets_drive_the_configured_reverse_mode);
    RUN_TEST(test_arithmetic_refusal_preserves_the_integral_like_an_invalid_target);
    RUN_TEST(test_invalid_inputs_keep_precedence_over_the_arithmetic_check);
    RUN_TEST(test_arithmetic_failure_overrides_a_frozen_stamp);

    return UNITY_END();
}
