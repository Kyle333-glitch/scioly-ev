#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <CompetitionStateMachine.h>
#include <Motion.h>
#include <MotionChainReset.h>
#include <MotionProfile.h>
#include <NearZeroVerifier.h>
#include <QuadratureDecoder.h>
#include <ReversalSequencer.h>
#include <RunConfig.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <Snapshots.h>
#include <Units.h>
#include <VehicleState.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

// Boundary tests for the chain the vehicle layer will wire together:
// VelocityEstimator -> MotionProfile -> VelocityController -> MotorEffort ->
// SafetyAuthority::authorize().
//
// The three modules are tested on their own elsewhere; what is pinned here is
// what none of those tests can see - that they agree on units and signs at the
// handoff, that a refusal at one boundary cannot become a drive at the next,
// and that the loop's output stays a request that only the safety boundary can
// turn into a command.
//
// Every gain, speed, distance and tick count below is a test fixture. Nothing
// here is a tuning value, and no fixture is a placeholder any firmware path may
// use. The trace-driven case simulates no vehicle: its motion is prescribed, not
// derived from the commanded effort, so it is not evidence that the vehicle
// would follow the profile (VERIFIED_STATE.md #4, #6, #8, #9, #10 all stay
// UNVERIFIED).

// Pinned at compile time: nothing outside the safety boundary can build an
// actuatable command, so the chain's output cannot be one.
static_assert(!std::is_constructible<ev::AuthorizedMotorCommand, ev::MotorEffort>::value,
              "the motion chain must not be able to authorize its own request");
static_assert(!std::is_default_constructible<ev::MotionProfileCommand>::value,
              "a profile command must never default-construct into a shaped target");
static_assert(!std::is_default_constructible<ev::VelocityEstimate>::value,
              "an estimate must never default-construct into a valid-looking value");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kTolerance = 1e-9;

ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<ev::MotorMode> measuredMode(ev::MotorMode mode) {
    return ev::Sourced<ev::MotorMode>(mode, ev::ValueSource::Measured);
}

ev::Sourced<ev::MotionDirection> measuredDirection(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Measured);
}

// Fixture scale: 0.0001 m per tick, the same fixture used by the module targets.
ev::Sourced<double> fixtureScale() { return measuredValue(0.0001); }

// The stop-verification policy the fixtures use: threshold, dwell and the fixture
// update-gap limit, so a new configuration field cannot be forgotten here.
ev::NearZeroConfig stopConfig(ev::Sourced<double> threshold, ev::Sourced<double> dwell) {
    return ev::NearZeroConfig(threshold, dwell,
                              ev::Sourced<uint32_t>(250U, ev::ValueSource::Measured));
}

ev::MotionProfileLimits profileLimits(ev::MotionDirection positiveCommandDirection) {
    return ev::MotionProfileLimits(measuredValue(2.0), measuredValue(2.5), measuredValue(0.5),
                                   measuredValue(1.0), measuredValue(0.05),
                                   measuredDirection(positiveCommandDirection));
}

// 300 permille per m/s of feed-forward, 200 per m/s of error, 100 per (m/s x s)
// of integrated error, the given effort ceiling so a proportional term stays
// visible, a 300 permille integral ceiling and at most 50 ms per integration
// step.
ev::VelocityControllerConfig controllerConfigWithCeiling(ev::MotorMode positiveEffortMode,
                                                        uint16_t maxEffortPermille) {
    return ev::VelocityControllerConfig(
        measuredValue(300.0), measuredValue(200.0), measuredValue(100.0),
        measuredMode(positiveEffortMode),
        ev::Sourced<uint16_t>(maxEffortPermille, ev::ValueSource::Measured), 300U, 0.05);
}

ev::VelocityControllerConfig controllerConfig(ev::MotorMode positiveEffortMode) {
    return controllerConfigWithCeiling(positiveEffortMode, 1000U);
}

ev::SafetyLimits limitsWithMaxEffort(uint16_t maxEffortPermille) {
    return ev::SafetyLimits{
        ev::Sourced<uint16_t>(maxEffortPermille, ev::ValueSource::Measured),
        measuredValue(2.5),
        measuredValue(2.5),
        measuredValue(30.0),
        measuredValue(9.0),
    };
}

// The run policy the new-run sequence also resets. Every value is a fixture, as
// everywhere else in this file: no bound here is measured.
ev::RunPhaseConfig chainPhaseConfig() {
    return ev::RunPhaseConfig(
        measuredValue(0.0), measuredValue(5.0), measuredValue(1.0), measuredValue(5.0),
        measuredValue(5.0), measuredValue(5.0), measuredValue(5.0), measuredValue(1.0),
        stopConfig(measuredValue(0.05), measuredValue(0.2)));
}

ev::ReversalConfig chainReversalConfig() {
    return ev::ReversalConfig(stopConfig(measuredValue(0.05), measuredValue(0.2)),
                              measuredValue(3.0));
}

ev::RunConfig chainRunConfig() {
    return ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
}

ev::RunInputs chainRunInputs() {
    ev::RunInputs inputs;
    inputs.authorityArmed = true;
    return inputs;
}

ev::SafetyReadiness fullyReady() {
    ev::SafetyReadiness readiness;
    readiness.configurationValid = true;
    readiness.calibrationUsable = true;
    readiness.limitsUsable = true;
    return readiness;
}

ev::SafetySystemState readyState() {
    ev::SafetySystemState state;
    state.readiness = fullyReady();
    state.requiredDataFresh = true;
    state.atZeroSpeed = true;
    state.nowMs = 1000U;
    state.detectedFault = ev::FaultCode::None;
    return state;
}

void expectEffort(ev::MotorMode expectedMode, uint16_t expectedPermille,
                  const ev::VelocityControlResult& result) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedMode), static_cast<int>(result.effort.mode));
    TEST_ASSERT_EQUAL_UINT16(expectedPermille, result.effort.magnitudePermille);
    TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
}

}  // namespace

// --- Units ------------------------------------------------------------------

void test_the_boundaries_agree_on_metres_per_second(void) {
    // 10 ticks per millisecond is 10000 ticks/s, which is 1.0 m/s at 0.0001
    // m/tick. The estimator's number and the profile's number then meet in the
    // regulator's error term.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, fixtureScale());
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), fixtureScale());
    }
    TEST_ASSERT_TRUE(estimate.valid);
    TEST_ASSERT_TRUE(std::fabs(estimate.velocityMps - 1.0) <= kTolerance);

    const ev::MotionProfileCommand command = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 8.0,
                                2.0, 20U),
        profileLimits(ev::MotionDirection::Forward));
    TEST_ASSERT_TRUE(std::fabs(command.targetVelocityMps - 2.0) <= kTolerance);

    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result =
        controller.update(1000U, command.targetVelocityMps, estimate.valid, estimate.velocityMps);

    // 2.0 m/s of target minus 1.0 m/s of measured speed is exactly 1.0 m/s of
    // error, so the proportional term is one gain and the feed-forward term is
    // two. A unit mix-up anywhere in the chain - a speed in millimetres per
    // second at one boundary, say - would make this error three orders of
    // magnitude larger and the demand would clamp away instead.
    TEST_ASSERT_TRUE(std::fabs(result.errorMps - 1.0) <= 1e-12);
    TEST_ASSERT_TRUE(std::fabs(result.feedForwardPermille - 600.0) <= kTolerance);
    TEST_ASSERT_TRUE(std::fabs(result.proportionalPermille - 200.0) <= kTolerance);
    expectEffort(ev::MotorMode::DriveForward, 800U, result);
}

// --- Sign chain -------------------------------------------------------------

void test_the_two_polarity_fields_are_independent_and_both_are_used(void) {
    // The profile maps a physical direction to a command sign through its own
    // field; the regulator maps that sign to a driver mode through its own. The
    // four combinations below are all legal configurations, and only a physical
    // measurement can say which pair describes the real vehicle
    // (VERIFIED_STATE.md #2, #3, #4). What is pinned here is that neither module
    // assumes the other's answer.
    const ev::MotionDirection positiveDirections[] = {ev::MotionDirection::Forward,
                                                      ev::MotionDirection::Reverse};
    const ev::MotorMode positiveModes[] = {ev::MotorMode::DriveForward,
                                           ev::MotorMode::DriveReverse};

    for (ev::MotionDirection positiveDirection : positiveDirections) {
        for (ev::MotorMode positiveMode : positiveModes) {
            const ev::MotionProfileCommand command = ev::nextTargetVelocity(
                ev::MotionProfileInputs(ev::MotionProfileIntent::Travel,
                                        ev::MotionDirection::Forward, 5.0, 0.0, 20U),
                profileLimits(positiveDirection));

            const int expectedSign =
                positiveDirection == ev::MotionDirection::Forward ? 1 : -1;
            TEST_ASSERT_TRUE(std::fabs(command.targetVelocityMps - expectedSign * 0.01) <= 1e-12);

            ev::VelocityController controller(controllerConfig(positiveMode));
            TEST_ASSERT_TRUE(controller.enable());
            const ev::VelocityControlResult result = controller.update(
                1000U, command.targetVelocityMps, true, command.targetVelocityMps);

            TEST_ASSERT_TRUE(std::fabs(result.errorMps) <= 1e-12);
            const ev::MotorMode expectedMode =
                expectedSign > 0
                    ? positiveMode
                    : (positiveMode == ev::MotorMode::DriveForward ? ev::MotorMode::DriveReverse
                                                                   : ev::MotorMode::DriveForward);
            expectEffort(expectedMode, 3U, result);
        }
    }
}

// --- Refusals at one boundary never become a drive at the next --------------

void test_a_warming_up_estimator_runs_the_regulator_open_loop(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());

    // One sample: the window is not covered, so there is no measurement yet.
    ev::VelocityEstimate estimate = estimator.update(0U, 0, fixtureScale());
    TEST_ASSERT_FALSE(estimate.valid);
    ev::VelocityControlResult result =
        controller.update(1000U, 2.0, estimate.valid, estimate.velocityMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::DriveForward, 600U, result);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);

    // The window fills, so the loop gains feedback and starts integrating.
    for (uint32_t ms = 10U; ms <= 40U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), fixtureScale());
    }
    TEST_ASSERT_TRUE(estimate.valid);

    result = controller.update(2000U, 2.0, estimate.valid, estimate.velocityMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    // 1000 ms since the last accepted cycle, integrated at most 50 ms of it.
    TEST_ASSERT_EQUAL_UINT32(1000U, result.dtMs);
    TEST_ASSERT_TRUE(result.integrationStepClamped);

    result = controller.update(2020U, 2.0, estimate.valid, estimate.velocityMps);
    TEST_ASSERT_TRUE(std::fabs(result.errorMps - 1.0) <= 1e-12);
    TEST_ASSERT_TRUE(controller.integralPermille() > 0.0);
}

void test_a_provisional_scale_never_becomes_feedback(void) {
    // The first value a real session will have is a placeholder, and a
    // placeholder must not become a measurement simply because it is the only
    // number available.
    const ev::Sourced<double> provisional(0.0001, ev::ValueSource::Provisional);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, provisional);
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), provisional);
    }
    TEST_ASSERT_FALSE(estimate.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityEstimateStatus::ScaleIsNotMeasured),
                          static_cast<int>(estimate.status));
    TEST_ASSERT_FALSE(estimate.zeroSpeedClaimed);

    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    ev::VelocityControlResult result =
        controller.update(1000U, 2.0, estimate.valid, estimate.velocityMps);
    result = controller.update(1020U, 2.0, estimate.valid, estimate.velocityMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(result.status));
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectEffort(ev::MotorMode::DriveForward, 600U, result);
}

void test_a_target_the_profile_shapes_to_zero_never_becomes_a_drive(void) {
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    controller.update(1000U, 1.0, true, 0.5);

    // The brake offset already reaches the target point, so the geometry asks
    // for zero. It is still a target, not a switch: a 20 ms step brings the
    // target down at the deceleration limit (1.0 m/s minus 1.0 m/s^2 times
    // 0.02 s), which is exactly the deliberate ramp the profile exists to do.
    const ev::MotionProfileCommand ramping = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 0.05,
                                1.0, 20U),
        profileLimits(ev::MotionDirection::Forward));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(ramping.status));
    TEST_ASSERT_TRUE(std::fabs(ramping.targetVelocityMps - 0.98) <= kTolerance);

    // A step long enough for the ramp to reach zero does reach it...
    const ev::MotionProfileCommand stopped = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 0.05,
                                1.0, 1000U),
        profileLimits(ev::MotionDirection::Forward));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(stopped.status));
    TEST_ASSERT_TRUE(stopped.targetVelocityMps == 0.0);

    // ...and the regulator turns that into no powered effort at all, dropping
    // the integral on the way through zero. Disabled, not Coast and not Brake:
    // the stop is the run layer's decision.
    const ev::VelocityControlResult result =
        controller.update(1020U, stopped.targetVelocityMps, true, 0.5);
    TEST_ASSERT_TRUE(result.integralResetForSignChange);
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    expectEffort(ev::MotorMode::Disabled, 0U, result);
}

// --- The output is a request ------------------------------------------------

void test_the_chain_output_is_a_request_the_boundary_decides_on(void) {
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result = controller.update(1000U, 2.0, true, 1.0);
    expectEffort(ev::MotorMode::DriveForward, 800U, result);

    ev::SafetyAuthority authority;
    const ev::SafetySystemState state = readyState();

    // Unarmed, the same effort the loop produced is refused outright, and the
    // refusal is a safe stop rather than no command.
    const ev::AuthorizationResult denied =
        authority.authorize(result.effort, state, limitsWithMaxEffort(600U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedNotArmed),
                          static_cast<int>(denied.outcome));
    TEST_ASSERT_TRUE(denied.command.isDisabled());
    TEST_ASSERT_EQUAL_UINT16(0U, denied.command.magnitudePermille());

    // Armed, it is authorized and clamped to the measured ceiling - 600, not the
    // loop's own 1000.
    TEST_ASSERT_TRUE(authority.arm(state.readiness));
    const ev::AuthorizationResult authorized =
        authority.authorize(result.effort, state, limitsWithMaxEffort(600U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedClamped),
                          static_cast<int>(authorized.outcome));
    TEST_ASSERT_EQUAL_UINT16(600U, authorized.command.magnitudePermille());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(authorized.command.mode()));
}

// --- The two effort ceilings --------------------------------------------------

void test_the_two_effort_ceilings_are_meant_to_be_equal(void) {
    // The vehicle layer derives the regulator's ceiling from SafetyLimits. When
    // the two agree, the regulator's own saturation report is the whole story.
    const ev::SafetyLimits safety = limitsWithMaxEffort(600U);
    const ev::VelocityControllerConfig fromSafety =
        controllerConfigWithCeiling(ev::MotorMode::DriveForward, 600U);
    TEST_ASSERT_TRUE(ev::effortCeilingsAgree(fromSafety.maxEffortPermille,
                                             safety.maxEffortPermille));

    ev::VelocityController agreedController(fromSafety);
    TEST_ASSERT_TRUE(agreedController.enable());
    // 2.0 m/s of target minus 1.0 m/s of measured speed demands 800 permille;
    // the regulator's own 600 permille ceiling is what binds, and it says so.
    const ev::VelocityControlResult agreed = agreedController.update(1000U, 2.0, true, 1.0);
    TEST_ASSERT_TRUE(agreed.outputSaturated);
    expectEffort(ev::MotorMode::DriveForward, 600U, agreed);

    ev::SafetyAuthority agreedAuthority;
    TEST_ASSERT_TRUE(agreedAuthority.arm(fullyReady()));
    const ev::AuthorizationResult agreedResult =
        agreedAuthority.authorize(agreed.effort, readyState(), safety);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(agreedResult.outcome));
    TEST_ASSERT_EQUAL_UINT16(600U, agreedResult.command.magnitudePermille());

    // A regulator ceiling above the safety ceiling is the mismatch the invariant
    // forbids: the same demand reports outputSaturated == false here while the
    // authority reports AuthorizedClamped downstream, splitting one saturation
    // across two logs (and the loop keeps its anti-windup headroom it does not
    // have). The helper exists so the vehicle layer can refuse to build this.
    const ev::VelocityControllerConfig aboveSafety =
        controllerConfigWithCeiling(ev::MotorMode::DriveForward, 1000U);
    TEST_ASSERT_FALSE(ev::effortCeilingsAgree(aboveSafety.maxEffortPermille,
                                              safety.maxEffortPermille));

    ev::VelocityController mismatchController(aboveSafety);
    TEST_ASSERT_TRUE(mismatchController.enable());
    const ev::VelocityControlResult mismatch = mismatchController.update(1000U, 2.0, true, 1.0);
    TEST_ASSERT_FALSE(mismatch.outputSaturated);
    expectEffort(ev::MotorMode::DriveForward, 800U, mismatch);

    ev::SafetyAuthority mismatchAuthority;
    TEST_ASSERT_TRUE(mismatchAuthority.arm(fullyReady()));
    const ev::AuthorizationResult mismatchResult =
        mismatchAuthority.authorize(mismatch.effort, readyState(), safety);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedClamped),
                          static_cast<int>(mismatchResult.outcome));
    TEST_ASSERT_EQUAL_UINT16(600U, mismatchResult.command.magnitudePermille());
}

// --- The two polarity encodings -----------------------------------------------

void test_the_two_polarity_encodings_must_agree(void) {
    // The profile and the regulator carry the same UNVERIFIED physical fact (#4)
    // in two vocabularies. Each pair below is individually valid - both fields
    // `Measured`, each module happy - so without this check an opposite pair
    // would arm and drive the taper and the effort in opposite directions for
    // one command sign.
    TEST_ASSERT_TRUE(ev::polarityAgrees(
        profileLimits(ev::MotionDirection::Forward).positiveCommandDirection,
        controllerConfig(ev::MotorMode::DriveForward).positiveEffortMode));
    TEST_ASSERT_TRUE(ev::polarityAgrees(
        profileLimits(ev::MotionDirection::Reverse).positiveCommandDirection,
        controllerConfig(ev::MotorMode::DriveReverse).positiveEffortMode));

    TEST_ASSERT_FALSE(ev::polarityAgrees(
        profileLimits(ev::MotionDirection::Forward).positiveCommandDirection,
        controllerConfig(ev::MotorMode::DriveReverse).positiveEffortMode));
    TEST_ASSERT_FALSE(ev::polarityAgrees(
        profileLimits(ev::MotionDirection::Reverse).positiveCommandDirection,
        controllerConfig(ev::MotorMode::DriveForward).positiveEffortMode));

    // A non-driving mode is not a polarity at all, so it agrees with nothing.
    // validateVelocityControllerConfig() already refuses it; this check must not
    // read it as a direction by accident.
    TEST_ASSERT_FALSE(ev::polarityAgrees(
        profileLimits(ev::MotionDirection::Forward).positiveCommandDirection,
        measuredMode(ev::MotorMode::Brake)));
}

void test_the_tick_polarity_must_agree_with_the_command_polarity(void) {
    // The third encoding of the same physical fact (#4): which direction the
    // decoder's positive tick convention corresponds to. A pair that disagrees
    // makes the regulator's error term the wrong sign - it would drive away from
    // its target instead of toward it - and makes the run axis shrink while the
    // run drives its forward leg.
    TEST_ASSERT_TRUE(ev::tickPolarityAgrees(
        measuredDirection(ev::MotionDirection::Forward),
        profileLimits(ev::MotionDirection::Forward).positiveCommandDirection));
    TEST_ASSERT_TRUE(ev::tickPolarityAgrees(
        measuredDirection(ev::MotionDirection::Reverse),
        profileLimits(ev::MotionDirection::Reverse).positiveCommandDirection));

    TEST_ASSERT_FALSE(ev::tickPolarityAgrees(
        measuredDirection(ev::MotionDirection::Forward),
        profileLimits(ev::MotionDirection::Reverse).positiveCommandDirection));
    TEST_ASSERT_FALSE(ev::tickPolarityAgrees(
        measuredDirection(ev::MotionDirection::Reverse),
        profileLimits(ev::MotionDirection::Forward).positiveCommandDirection));

    // Both checks together are exactly "all three encodings name one physical
    // direction": any single disagreement is refused, however the other two
    // pair up.
    const ev::MotionDirection directions[] = {ev::MotionDirection::Forward,
                                              ev::MotionDirection::Reverse};
    for (const ev::MotionDirection ticks : directions) {
        for (const ev::MotionDirection command : directions) {
            for (const ev::MotionDirection effort : directions) {
                const ev::MotorMode effortMode = effort == ev::MotionDirection::Forward
                                                     ? ev::MotorMode::DriveForward
                                                     : ev::MotorMode::DriveReverse;
                const bool chainAgrees =
                    ev::tickPolarityAgrees(measuredDirection(ticks),
                                           measuredDirection(command)) &&
                    ev::polarityAgrees(measuredDirection(command), measuredMode(effortMode));
                TEST_ASSERT_EQUAL_INT(
                    (ticks == command && command == effort) ? 1 : 0,
                    chainAgrees ? 1 : 0);
            }
        }
    }
}

// --- New-run reset -----------------------------------------------------------

void test_a_new_run_resets_every_module_and_never_arms(void) {
    ev::QuadratureDecoder decoder;
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    ev::MotionProfileTarget target;
    ev::SafetyAuthority authority;
    ev::CompetitionStateMachine run(chainPhaseConfig(), chainReversalConfig());

    // Use the chain so every module holds state: a decoder reference and count,
    // an estimator window, an enabled regulator with an integral, a non-zero
    // target and an armed authority with a latched forward direction.
    decoder.update(false, false);
    decoder.update(true, false);
    TEST_ASSERT_TRUE(decoder.hasReference());
    TEST_ASSERT_TRUE(decoder.ticks() == 1);

    ev::VelocityEstimate estimate = estimator.update(0U, 0, fixtureScale());
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), fixtureScale());
    }
    TEST_ASSERT_TRUE(estimate.valid);
    TEST_ASSERT_EQUAL_UINT32(7U, estimator.acceptedSamples());

    TEST_ASSERT_TRUE(controller.enable());
    controller.update(1000U, 2.0, estimate.valid, estimate.velocityMps);
    const ev::VelocityControlResult integrated =
        controller.update(1020U, 2.0, estimate.valid, estimate.velocityMps);
    TEST_ASSERT_TRUE(integrated.integralPermille > 0.0);
    TEST_ASSERT_TRUE(controller.integralPermille() > 0.0);

    const ev::MotionProfileCommand shaped = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        profileLimits(ev::MotionDirection::Forward));
    target.accept(shaped);
    TEST_ASSERT_TRUE(target.targetVelocityMps() > 0.0);

    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
    const ev::SafetySystemState state = readyState();
    const ev::AuthorizationResult granted = authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 500U), state, limitsWithMaxEffort(600U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(granted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    // The run policy holds state too: an announced run that has reached the
    // armed phase.
    const ev::RunConfig announced = chainRunConfig();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunStartStatus::Started),
        static_cast<int>(run.announceRun(announced, ev::validateRunConfig(announced), 1000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                          static_cast<int>(run.update(1020U, chainRunInputs()).phase));

    // The single new-run sequence resets every one of those states. The clock
    // reading is the caller's, passed through to stamp the new Idle phase.
    const ev::MotionChainResetReport report =
        ev::resetMotionChainForNewRun(decoder, estimator, controller, target, authority, run,
                                      1040U);

    TEST_ASSERT_TRUE(report.controllerEnabled);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(run.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(run.faultCode()));
    TEST_ASSERT_FALSE(run.stopVerified());
    TEST_ASSERT_FALSE(decoder.hasReference());
    TEST_ASSERT_EQUAL_INT32(0, decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, estimator.acceptedSamples());
    TEST_ASSERT_TRUE(controller.isEnabled());  // fresh control state, enabled
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
    TEST_ASSERT_TRUE(target.targetVelocityMps() == 0.0);
    TEST_ASSERT_FALSE(authority.isArmed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    // The first control cycle after the reset starts from nothing: the estimator
    // window is gone, and the regulator has no previous stamp, so nothing
    // integrates and there is no feedback.
    const ev::VelocityEstimate fresh = estimator.update(2000U, 0, fixtureScale());
    TEST_ASSERT_FALSE(fresh.valid);
    const ev::VelocityControlResult restarted =
        controller.update(2000U, 2.0, fresh.valid, fresh.velocityMps);
    TEST_ASSERT_EQUAL_UINT32(0U, restarted.dtMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(restarted.status));
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);

    // The regulator is enabled but the vehicle is not armed: a reset is not an
    // arming decision, and the deliberate arm() is still required.
    const ev::AuthorizationResult stillDenied =
        authority.authorize(restarted.effort, state, limitsWithMaxEffort(600U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedNotArmed),
                          static_cast<int>(stillDenied.outcome));
    TEST_ASSERT_TRUE(stillDenied.command.isDisabled());
    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
}

void test_a_new_run_reports_the_fault_it_discards(void) {
    ev::QuadratureDecoder decoder;
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    ev::MotionProfileTarget target;
    ev::SafetyAuthority authority;
    ev::CompetitionStateMachine run(chainPhaseConfig(), chainReversalConfig());

    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
    ev::SafetySystemState faulted = readyState();
    faulted.detectedFault = ev::FaultCode::ProvisionalCalibration;
    const ev::AuthorizationResult forced = authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 500U), faulted, limitsWithMaxEffort(600U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(forced.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(authority.latchedFault().code));
    TEST_ASSERT_FALSE(authority.isArmed());

    // The run policy records the same fault independently, and the reset clears
    // both records: the phase it discarded is reported before it is forgotten.
    ev::RunInputs runInputs = chainRunInputs();
    runInputs.detectedFault = ev::FaultCode::ProvisionalCalibration;
    const ev::RunStatus runStatus = run.update(1000U, runInputs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(runStatus.phase));

    const ev::MotionChainResetReport report =
        ev::resetMotionChainForNewRun(decoder, estimator, controller, target, authority, run,
                                      1000U);

    // The fault is reported before it is cleared, so the run log can record what
    // ended the previous run; the reset then discards it along with the arming.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(authority.latchedFault().code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(run.faultCode()));
    TEST_ASSERT_FALSE(authority.isArmed());

    // Discarding the fault buys no arm: readiness is still required.
    TEST_ASSERT_FALSE(authority.arm(ev::SafetyReadiness{}));
    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
}

void test_a_new_run_reports_a_regulator_that_cannot_be_re_enabled(void) {
    // An unmeasured gain means enable() refuses. The reset must not pretend the
    // chain is runnable: the report carries the refusal and the regulator stays
    // disabled, so no effort request can be produced at all.
    const ev::VelocityControllerConfig provisionalConfig(
        ev::Sourced<double>(300.0, ev::ValueSource::Provisional), measuredValue(200.0),
        measuredValue(100.0), measuredMode(ev::MotorMode::DriveForward),
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Provisional), 300U, 0.05);
    ev::VelocityController controller(provisionalConfig);
    TEST_ASSERT_FALSE(controller.enable());

    ev::QuadratureDecoder decoder;
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::MotionProfileTarget target;
    ev::SafetyAuthority authority;
    ev::CompetitionStateMachine run(chainPhaseConfig(), chainReversalConfig());

    const ev::MotionChainResetReport report =
        ev::resetMotionChainForNewRun(decoder, estimator, controller, target, authority, run,
                                      1000U);
    TEST_ASSERT_FALSE(report.controllerEnabled);
    TEST_ASSERT_FALSE(controller.isEnabled());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(report.clearedRunPhase));

    const ev::VelocityControlResult result = controller.update(1000U, 2.0, false, 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Disabled),
                          static_cast<int>(result.status));
    expectEffort(ev::MotorMode::Disabled, 0U, result);
}

// --- Velocity freshness across the handoff -----------------------------------

void test_a_reused_measurement_is_aged_out_before_the_regulator_sees_it(void) {
    // The regulator has no measurement clock of its own: the chain passes
    // velocityReadiness().usable as measurementUsable, and that verdict is what
    // makes a last-valid estimate reused across cycles visible.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, fixtureScale());
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), fixtureScale());
    }
    TEST_ASSERT_TRUE(estimate.valid);
    TEST_ASSERT_EQUAL_UINT32(60U, estimate.elapsedMs);

    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());

    // Inside the caller's limit the verdict is usable and the loop regulates
    // with feedback: 2.0 m/s of target minus 1.0 m/s of measured speed.
    const ev::VelocityReadiness fresh =
        ev::velocityReadiness(true, estimate, ev::sampleAge(80U, estimate.elapsedMs, 50U));
    TEST_ASSERT_TRUE(fresh.usable);
    ev::VelocityControlResult result =
        controller.update(80U, 2.0, fresh.usable, estimate.velocityMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::Ok),
                          static_cast<int>(result.status));
    TEST_ASSERT_TRUE(std::fabs(result.proportionalPermille - 200.0) <= kTolerance);

    // The same estimate held for 240 ms is stale. Nothing about the two facts
    // the regulator receives changed, so only this verdict can catch it.
    const ev::VelocityReadiness stale =
        ev::velocityReadiness(true, estimate, ev::sampleAge(300U, estimate.elapsedMs, 50U));
    TEST_ASSERT_FALSE(stale.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::SampleStale),
                          static_cast<int>(stale.reason));
    TEST_ASSERT_EQUAL_UINT32(240U, stale.ageMs);
    result = controller.update(300U, 2.0, stale.usable, estimate.velocityMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::FeedForwardOnly),
                          static_cast<int>(result.status));
    TEST_ASSERT_TRUE(controller.integralPermille() == 0.0);
}

void test_a_zero_speed_claim_is_not_a_measurement_the_chain_may_use(void) {
    // The estimator's claim is an explicit statement about observed non-change,
    // never a measurement of zero speed. It must not satisfy a freshness verdict
    // (so it cannot reach the regulator as feedback) and it must never be handed
    // to SafetySystemState::atZeroSpeed on its own.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 50U});
    ev::VelocityEstimate claim = estimator.update(0U, 0, fixtureScale());
    for (uint32_t ms = 10U; ms <= 50U; ms += 10U) {
        claim = estimator.update(ms, 0, fixtureScale());
    }
    TEST_ASSERT_TRUE(claim.zeroSpeedClaimed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityEstimateStatus::NoEdgeTimeout),
                          static_cast<int>(claim.status));
    TEST_ASSERT_FALSE(claim.valid);

    const ev::VelocityReadiness readiness =
        ev::velocityReadiness(true, claim, ev::sampleAge(50U, claim.elapsedMs, 50U));
    TEST_ASSERT_FALSE(readiness.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::EstimateInvalid),
                          static_cast<int>(readiness.reason));

    // And with a zero target the regulator asks for no powered effort at all, so
    // a claim can never turn into a request either.
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    const ev::VelocityControlResult result =
        controller.update(50U, 0.0, readiness.usable, claim.velocityMps);
    expectEffort(ev::MotorMode::Disabled, 0U, result);
}

// --- Trace-driven data flow -------------------------------------------------

void test_a_prescribed_trace_keeps_the_chain_inside_its_limits(void) {
    // Both traces are prescribed. The motion is not derived from the commanded
    // effort, so this is a data-flow and invariant check, not a simulation of the
    // vehicle: a passing case says nothing about whether the real drivetrain
    // follows the shaped target.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityController controller(controllerConfig(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(controller.enable());
    const ev::MotionProfileLimits limits = profileLimits(ev::MotionDirection::Forward);

    double targetMps = 0.0;
    double previousTargetMps = 0.0;
    for (uint32_t step = 0U; step <= 40U; ++step) {
        const uint32_t ms = step * 20U;
        // 1 tick per millisecond: 0.1 m/s at the fixture scale, whatever the loop
        // commands.
        const ev::VelocityEstimate estimate =
            estimator.update(ms, static_cast<int32_t>(ms), fixtureScale());
        const double remainingM = 1.0 + 0.01 * static_cast<double>(40U - step);
        const ev::MotionProfileCommand command = ev::nextTargetVelocity(
            ev::MotionProfileInputs(ev::MotionProfileIntent::Travel,
                                    ev::MotionDirection::Forward, remainingM, targetMps, 20U),
            limits);
        targetMps = command.targetVelocityMps;
        const ev::VelocityControlResult result =
            controller.update(ms, targetMps, estimate.valid, estimate.velocityMps);

        TEST_ASSERT_TRUE(ev::validateMotorEffort(result.effort).ok);
        TEST_ASSERT_TRUE(result.effort.magnitudePermille <= 1000U);
        TEST_ASSERT_TRUE(std::isfinite(command.targetVelocityMps));
        TEST_ASSERT_TRUE(std::isfinite(result.errorMps));
        TEST_ASSERT_TRUE(command.targetVelocityMps <= command.cruiseCeilingMps + kTolerance);
        // The shaped target only rises here: the remaining distance stays above
        // the taper, and the acceleration limit is what the target follows.
        TEST_ASSERT_TRUE(targetMps >= previousTargetMps - kTolerance);
        // The loop never asks for powered effort against its target, so with a
        // non-negative target the mode is never the reverse one.
        TEST_ASSERT_TRUE(result.effort.mode == ev::MotorMode::DriveForward ||
                         result.effort.mode == ev::MotorMode::Disabled);
        TEST_ASSERT_TRUE(controller.integralPermille() <= 300.0);
        previousTargetMps = targetMps;
    }
    // The ramp did move the target over these 40 steps: this trace is not
    // trivially stopped.
    TEST_ASSERT_TRUE(targetMps > 0.1);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_the_boundaries_agree_on_metres_per_second);

    RUN_TEST(test_the_two_polarity_fields_are_independent_and_both_are_used);

    RUN_TEST(test_a_warming_up_estimator_runs_the_regulator_open_loop);
    RUN_TEST(test_a_provisional_scale_never_becomes_feedback);
    RUN_TEST(test_a_target_the_profile_shapes_to_zero_never_becomes_a_drive);

    RUN_TEST(test_the_chain_output_is_a_request_the_boundary_decides_on);

    RUN_TEST(test_the_two_effort_ceilings_are_meant_to_be_equal);
    RUN_TEST(test_the_two_polarity_encodings_must_agree);
    RUN_TEST(test_the_tick_polarity_must_agree_with_the_command_polarity);

    RUN_TEST(test_a_new_run_resets_every_module_and_never_arms);
    RUN_TEST(test_a_new_run_reports_the_fault_it_discards);
    RUN_TEST(test_a_new_run_reports_a_regulator_that_cannot_be_re_enabled);

    RUN_TEST(test_a_reused_measurement_is_aged_out_before_the_regulator_sees_it);
    RUN_TEST(test_a_zero_speed_claim_is_not_a_measurement_the_chain_may_use);

    RUN_TEST(test_a_prescribed_trace_keeps_the_chain_inside_its_limits);

    return UNITY_END();
}
