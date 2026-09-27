#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <MotionProfile.h>
#include <Units.h>

// Contract tests for the candidate distance-aware target shaping and for the
// timing intent arithmetic.
//
// Every cruise speed, acceleration limit, brake offset, distance and time below
// is a test fixture. None has been measured: the acceleration and deceleration
// limits come from a physical characterization that has not happened, and the
// brake offset comes from braking-distance testing that has not happened
// (VERIFIED_STATE.md #9). So no fixture here is a placeholder any firmware path
// may use, and a passing test says nothing about whether the shaped target is
// achievable by the real vehicle.
//
// The tests answer specific questions: does the target fall as the remaining
// distance shrinks, does it respect the acceleration and deceleration limits
// with the elapsed time it was given, does a reversal pass through zero, does a
// refusal ever shape a target, and is the duration arithmetic what it claims?

// Pinned at compile time: nothing that carries an answer may default-construct,
// and the safe zero values must stay where they are.
static_assert(!std::is_default_constructible<ev::MotionProfileCommand>::value,
              "a profile command must never default-construct into a shaped target");
static_assert(!std::is_default_constructible<ev::MotionProfileInputs>::value,
              "profile inputs must never default-construct");
static_assert(!std::is_default_constructible<ev::MotionDurationEstimate>::value,
              "a duration estimate must never default-construct");
static_assert(!std::is_default_constructible<ev::MotionTimingSummary>::value,
              "a timing summary must never default-construct");
static_assert(!std::is_default_constructible<ev::MotionProfileLimits>::value,
              "shaping limits must always state their evidence");
static_assert(ev::MotionProfileTarget{}.targetVelocityMps() == 0.0,
              "a fresh profile target must start at exactly zero");
static_assert(static_cast<int>(ev::MotionProfilePhase::Stopped) == 0,
              "the stopped phase must stay the zero value");
static_assert(static_cast<int>(ev::MotionProfileStatus::Ok) == 0,
              "the ok status must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Comparisons use explicit tolerances rather than double equality, so this
// target does not depend on optional Unity double support.
const double kVelocityToleranceMps = 1e-9;
const double kDurationToleranceS = 1e-9;

// Fixture limits: cruise 2.0 m/s under a 2.5 m/s ceiling, 0.5 m/s^2 of
// acceleration, 1.0 m/s^2 of deceleration, a 0.05 m brake offset, and a positive
// command sign meaning forward.
ev::MotionProfileLimits fixtureLimits() {
    return ev::MotionProfileLimits(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
}

// Fixture brake offset, in metres, matching fixtureLimits().
const double kFixtureBrakeOffsetM = 0.05;

ev::MotionProfileInputs travelForward(double currentTargetVelocityMps, double remainingDistanceM,
                                      uint32_t dtMs) {
    return ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward,
                                   remainingDistanceM, currentTargetVelocityMps, dtMs);
}

// The taper a fixture evaluation should produce: the speed from which 1.0 m/s^2
// still stops inside the remaining distance after the brake offset, capped by
// the cruise ceiling.
double expectedTaperMps(double remainingDistanceM) {
    const double beyondOffsetM =
        remainingDistanceM > kFixtureBrakeOffsetM ? remainingDistanceM - kFixtureBrakeOffsetM : 0.0;
    const double taperMps = std::sqrt(2.0 * 1.0 * beyondOffsetM);
    return taperMps < 2.0 ? taperMps : 2.0;
}

void expectTarget(double expectedMps, const ev::MotionProfileCommand& command) {
    TEST_ASSERT_TRUE(std::fabs(command.targetVelocityMps - expectedMps) <= kVelocityToleranceMps);
}

void expectRefusal(ev::MotionProfileStatus expectedStatus,
                   const ev::MotionProfileCommand& command) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStatus), static_cast<int>(command.status));
    TEST_ASSERT_TRUE(command.targetVelocityMps == 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Stopped),
                          static_cast<int>(command.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::None),
                          static_cast<int>(command.binding));
    TEST_ASSERT_TRUE(command.cruiseCeilingMps == 0.0);
    TEST_ASSERT_TRUE(command.taperCeilingMps == 0.0);
    TEST_ASSERT_TRUE(command.accelCeilingMps == 0.0);
    TEST_ASSERT_TRUE(command.decelCeilingMps == 0.0);
}

void expectDuration(double expectedS, const ev::MotionDurationEstimate& estimate) {
    TEST_ASSERT_TRUE(estimate.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionDurationStatus::Valid),
                          static_cast<int>(estimate.status));
    TEST_ASSERT_TRUE(std::fabs(estimate.durationS - expectedS) <= kDurationToleranceS);
}

void expectSummary(double expectedTravelS, double expectedTotalS, double expectedMarginS,
                   const ev::MotionTimingSummary& summary) {
    TEST_ASSERT_TRUE(summary.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionTimingStatus::Valid),
                          static_cast<int>(summary.status));
    TEST_ASSERT_TRUE(std::fabs(summary.travelDurationS - expectedTravelS) <= kDurationToleranceS);
    TEST_ASSERT_TRUE(std::fabs(summary.intendedTotalS - expectedTotalS) <= kDurationToleranceS);
    TEST_ASSERT_TRUE(std::fabs(summary.marginS - expectedMarginS) <= kDurationToleranceS);
}

// A shaped command with a stated target, for testing the along-leg gate on its
// own. Its ceilings play no part in the gate.
ev::MotionProfileCommand commandAt(double targetVelocityMps) {
    return ev::MotionProfileCommand(targetVelocityMps, ev::MotionProfileStatus::Ok,
                                    ev::MotionProfilePhase::Cruising,
                                    ev::MotionProfileLimit::CruiseCeiling, 2.0, 2.0, 2.0, 2.0);
}

}  // namespace

// --- Shaping limits ---------------------------------------------------------

void test_shaping_limits_require_measured_values(void) {
    const ev::MotionProfileLimits provisionalCruise(
        ev::Sourced<double>(2.0, ev::ValueSource::Provisional),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionProfileLimitsError::ValuesAreNotMeasured),
        static_cast<int>(ev::validateMotionProfileLimits(provisionalCruise).error));

    // The polarity is the UNVERIFIED fact (#4): a provisional value must stop
    // the profile from shaping anything rather than pick a sign.
    const ev::MotionProfileLimits provisionalPolarity(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Provisional));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionProfileLimitsError::ValuesAreNotMeasured),
        static_cast<int>(ev::validateMotionProfileLimits(provisionalPolarity).error));

    TEST_ASSERT_TRUE(ev::validateMotionProfileLimits(fixtureLimits()).ok);
}

void test_shaping_limits_reject_malformed_values(void) {
    const ev::MotionProfileLimits nanAccel(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured), ev::Sourced<double>(kNaN, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimitsError::NonFiniteValue),
                          static_cast<int>(ev::validateMotionProfileLimits(nanAccel).error));

    const ev::MotionProfileLimits zeroCruise(
        ev::Sourced<double>(0.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimitsError::NonPositiveLimit),
                          static_cast<int>(ev::validateMotionProfileLimits(zeroCruise).error));

    const ev::MotionProfileLimits negativeBrakeOffset(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(-0.01, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionProfileLimitsError::NegativeBrakeOffset),
        static_cast<int>(ev::validateMotionProfileLimits(negativeBrakeOffset).error));

    // A cruise above the measured ceiling is a configuration mistake: clamping
    // it silently would make the ceiling look like it was measured at the cruise
    // speed.
    const ev::MotionProfileLimits cruiseAboveCeiling(
        ev::Sourced<double>(3.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionProfileLimitsError::CruiseExceedsVelocityCeiling),
        static_cast<int>(ev::validateMotionProfileLimits(cruiseAboveCeiling).error));
}

void test_unusable_limits_never_shape_a_target(void) {
    const ev::MotionProfileLimits provisional(
        ev::Sourced<double>(2.0, ev::ValueSource::Provisional),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));

    expectRefusal(ev::MotionProfileStatus::LimitsAreNotUsable,
                  ev::nextTargetVelocity(travelForward(1.5, 3.0, 20U), provisional));
    // Limits are checked before the inputs, so an unusable configuration is
    // reported even when the inputs are also unusable.
    expectRefusal(ev::MotionProfileStatus::LimitsAreNotUsable,
                  ev::nextTargetVelocity(travelForward(1.5, kNaN, 0U), provisional));
}

void test_invalid_inputs_never_shape_a_target(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    expectRefusal(ev::MotionProfileStatus::InvalidDistance,
                  ev::nextTargetVelocity(travelForward(0.0, kNaN, 20U), limits));
    expectRefusal(ev::MotionProfileStatus::InvalidDistance,
                  ev::nextTargetVelocity(travelForward(0.0, -0.1, 20U), limits));
    expectRefusal(ev::MotionProfileStatus::InvalidTarget,
                  ev::nextTargetVelocity(travelForward(kNaN, 3.0, 20U), limits));
    expectRefusal(ev::MotionProfileStatus::InvalidTarget,
                  ev::nextTargetVelocity(travelForward(kInf, 3.0, 20U), limits));
}

// --- Ramping up -------------------------------------------------------------

void test_a_travel_from_rest_ramps_up_at_the_acceleration_limit(void) {
    const ev::MotionProfileCommand command =
        ev::nextTargetVelocity(travelForward(0.0, 8.0, 20U), fixtureLimits());

    // 0.5 m/s^2 for 20 ms is 0.01 m/s, far below the cruise ceiling.
    expectTarget(0.01, command);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::Ok),
                          static_cast<int>(command.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Accelerating),
                          static_cast<int>(command.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::RampLimit),
                          static_cast<int>(command.binding));
    TEST_ASSERT_TRUE(std::fabs(command.cruiseCeilingMps - 2.0) <= kVelocityToleranceMps);
    // The two rate brackets in force: 0.01 m/s reachable going up, and nothing
    // reachable going down because the target is already at zero.
    TEST_ASSERT_TRUE(std::fabs(command.accelCeilingMps - 0.01) <= kVelocityToleranceMps);
    TEST_ASSERT_TRUE(command.decelCeilingMps == 0.0);
}

void test_the_ramp_reaches_cruise_after_the_configured_time(void) {
    // 0 to 2.0 m/s at 0.5 m/s^2 takes 4.0 s; with 20 ms steps that is 200 steps.
    // The remaining distance is made large enough that the taper never binds.
    const ev::MotionProfileLimits limits = fixtureLimits();
    double targetMps = 0.0;
    for (int i = 0; i < 199; ++i) {
        targetMps = ev::nextTargetVelocity(travelForward(targetMps, 100.0, 20U), limits)
                        .targetVelocityMps;
    }
    TEST_ASSERT_TRUE(targetMps < 2.0);
    TEST_ASSERT_TRUE(targetMps > 1.9);

    // The next step reaches the cruise ceiling exactly rather than overshooting
    // it, because the ramp can only approach the smaller of the two.
    targetMps = ev::nextTargetVelocity(travelForward(targetMps, 100.0, 20U), limits)
                    .targetVelocityMps;
    TEST_ASSERT_TRUE(std::fabs(targetMps - 2.0) <= kVelocityToleranceMps);

    // Sitting at cruise with the taper out of the way: the ceiling is what the
    // target sits at, and the phase is neither rising nor falling.
    const ev::MotionProfileCommand cruise =
        ev::nextTargetVelocity(travelForward(2.0, 100.0, 20U), limits);
    expectTarget(2.0, cruise);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Cruising),
                          static_cast<int>(cruise.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::CruiseCeiling),
                          static_cast<int>(cruise.binding));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::Ok),
                          static_cast<int>(cruise.status));
}

void test_the_ramp_uses_the_elapsed_time_it_was_given(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();

    // A single 1000 ms step is not one 20 ms step: with the taper out of the way
    // (a taper of 0.4 m/s here) the target reaches the taper in one step.
    const ev::MotionProfileCommand longStep =
        ev::nextTargetVelocity(travelForward(0.0, 0.13, 1000U), limits);
    TEST_ASSERT_TRUE(std::fabs(longStep.taperCeilingMps - 0.4) <= kVelocityToleranceMps);
    expectTarget(0.4, longStep);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::DistanceTaper),
                          static_cast<int>(longStep.binding));

    // The same inputs with a 20 ms step only allow 0.01 m/s.
    const ev::MotionProfileCommand shortStep =
        ev::nextTargetVelocity(travelForward(0.0, 0.13, 20U), limits);
    expectTarget(0.01, shortStep);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::RampLimit),
                          static_cast<int>(shortStep.binding));
}

void test_an_uneven_step_sequence_is_measured_step_by_step(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    const uint32_t stepsMs[] = {100U, 7U, 500U};
    const double expectedMps[] = {0.05, 0.0535, 0.3035};

    double targetMps = 0.0;
    for (int i = 0; i < 3; ++i) {
        targetMps =
            ev::nextTargetVelocity(travelForward(targetMps, 50.0, stepsMs[i]), limits)
                .targetVelocityMps;
        TEST_ASSERT_TRUE(std::fabs(targetMps - expectedMps[i]) <= kVelocityToleranceMps);
    }
}

// --- The distance taper -----------------------------------------------------

void test_the_taper_is_monotonic_and_matches_the_deceleration_limit(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    const double remainingsM[] = {8.0, 4.0, 2.05, 1.55, 1.05, 0.55, 0.05};

    double targetMps = 2.0;  // at cruise, so every later step can only pull down
    double previousTaperMps = 2.0;
    for (double remainingM : remainingsM) {
        const ev::MotionProfileCommand command =
            ev::nextTargetVelocity(travelForward(targetMps, remainingM, 20U), limits);

        const double taperMps = expectedTaperMps(remainingM);
        TEST_ASSERT_TRUE(std::fabs(command.taperCeilingMps - taperMps) <= kVelocityToleranceMps);
        // As the remaining distance shrinks, the speed from which the vehicle
        // could still stop inside it never rises.
        TEST_ASSERT_TRUE(command.taperCeilingMps <= previousTaperMps + kVelocityToleranceMps);
        // The target is never jumped below what the distance allows, and never
        // raised while the distance is shrinking.
        TEST_ASSERT_TRUE(command.targetVelocityMps >= taperMps - kVelocityToleranceMps);
        TEST_ASSERT_TRUE(command.targetVelocityMps <= targetMps + kVelocityToleranceMps);

        previousTaperMps = command.taperCeilingMps;
        targetMps = command.targetVelocityMps;
    }
}

void test_the_taper_pulls_the_target_down_instead_of_snapping_it(void) {
    // At cruise with the target point 1.05 m away, the taper allows 1.414 m/s
    // but the deceleration limit only permits 0.02 m/s of change in 20 ms.
    const ev::MotionProfileCommand command =
        ev::nextTargetVelocity(travelForward(2.0, 1.05, 20U), fixtureLimits());

    TEST_ASSERT_TRUE(std::fabs(command.taperCeilingMps - std::sqrt(2.0)) <= kVelocityToleranceMps);
    expectTarget(1.98, command);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Decelerating),
                          static_cast<int>(command.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::RampLimit),
                          static_cast<int>(command.binding));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::Ok),
                          static_cast<int>(command.status));
}

void test_the_taper_subtracts_the_measured_brake_offset(void) {
    // With a 0.5 m brake offset, the last half metre of the travel is treated as
    // already spent: the taper is zero there.
    const ev::MotionProfileLimits limits(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));

    const ev::MotionProfileCommand outsideOffset =
        ev::nextTargetVelocity(travelForward(0.4472135955, 0.6, 20U), limits);
    TEST_ASSERT_TRUE(std::fabs(outsideOffset.taperCeilingMps - 0.4472135955) <= 1e-9);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::DistanceTaper),
                          static_cast<int>(outsideOffset.binding));

    // Inside the offset the geometry asks for zero, and the target is ramped
    // down at the deceleration limit rather than switched off.
    const ev::MotionProfileCommand insideOffset =
        ev::nextTargetVelocity(travelForward(0.4472135955, 0.5, 20U), limits);
    TEST_ASSERT_TRUE(insideOffset.taperCeilingMps == 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(insideOffset.status));
    TEST_ASSERT_TRUE(std::fabs(insideOffset.targetVelocityMps - 0.4272135955) <= 1e-9);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Decelerating),
                          static_cast<int>(insideOffset.phase));
}

void test_once_the_brake_offset_is_reached_the_target_goes_to_zero_and_stays(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    const ev::MotionProfileCommand command =
        ev::nextTargetVelocity(travelForward(0.1, kFixtureBrakeOffsetM, 100U), limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(command.status));
    TEST_ASSERT_TRUE(command.targetVelocityMps == 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Stopped),
                          static_cast<int>(command.phase));

    // Already at zero and still at the target point: the geometry is what keeps
    // the target at zero, and that is reported as such.
    const ev::MotionProfileCommand atRest =
        ev::nextTargetVelocity(travelForward(0.0, 0.0, 20U), limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(atRest.status));
    TEST_ASSERT_TRUE(atRest.targetVelocityMps == 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Stopped),
                          static_cast<int>(atRest.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::BrakeOffset),
                          static_cast<int>(atRest.binding));
}

// --- Stop requests and direction changes ------------------------------------

void test_a_stop_request_ramps_the_target_down_and_preserves_its_sign(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    const ev::MotionProfileInputs inputs(ev::MotionProfileIntent::Stop,
                                         ev::MotionDirection::Forward, 5.0, 1.0, 100U);

    const ev::MotionProfileCommand command = ev::nextTargetVelocity(inputs, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::StopRequested),
                          static_cast<int>(command.status));
    TEST_ASSERT_TRUE(std::fabs(command.targetVelocityMps - 0.9) <= kVelocityToleranceMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Decelerating),
                          static_cast<int>(command.phase));

    // A long enough step reaches zero, and the sign never flips on the way.
    const ev::MotionProfileCommand stopped(
        ev::nextTargetVelocity(ev::MotionProfileInputs(ev::MotionProfileIntent::Stop,
                                                      ev::MotionDirection::Forward, 5.0, 0.9, 1000U),
                               limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::StopRequested),
                          static_cast<int>(stopped.status));
    TEST_ASSERT_TRUE(stopped.targetVelocityMps == 0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Stopped),
                          static_cast<int>(stopped.phase));
}

void test_a_direction_change_is_ramped_through_zero_without_a_sign_flip(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();

    // Still moving forward, and the run now asks for reverse: the magnitude
    // falls at the deceleration limit and the sign stays forward.
    const ev::MotionProfileCommand holding = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                1.0, 100U),
        limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::DirectionChangeHold),
                          static_cast<int>(holding.status));
    TEST_ASSERT_TRUE(std::fabs(holding.targetVelocityMps - 0.9) <= kVelocityToleranceMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Decelerating),
                          static_cast<int>(holding.phase));

    // One step from zero in the new direction: now the sign may change.
    const ev::MotionProfileCommand rising = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                0.0, 20U),
        limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::Ok),
                          static_cast<int>(rising.status));
    TEST_ASSERT_TRUE(std::fabs(rising.targetVelocityMps - (-0.01)) <= kVelocityToleranceMps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Accelerating),
                          static_cast<int>(rising.phase));
}

void test_the_polarity_maps_the_requested_direction_to_the_command_sign(void) {
    const ev::MotionProfileLimits forwardIsPositive = fixtureLimits();
    const ev::MotionProfileLimits reverseIsPositive(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(0.05, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Reverse, ev::ValueSource::Measured));

    const ev::MotionProfileCommand forwardA =
        ev::nextTargetVelocity(travelForward(0.0, 5.0, 20U), forwardIsPositive);
    TEST_ASSERT_TRUE(forwardA.targetVelocityMps > 0.0);

    const ev::MotionProfileCommand reverseA =
        ev::nextTargetVelocity(
            ev::MotionProfileInputs(ev::MotionProfileIntent::Travel,
                                    ev::MotionDirection::Reverse, 5.0, 0.0, 20U),
            forwardIsPositive);
    TEST_ASSERT_TRUE(reverseA.targetVelocityMps < 0.0);

    const ev::MotionProfileCommand forwardB =
        ev::nextTargetVelocity(travelForward(0.0, 5.0, 20U), reverseIsPositive);
    TEST_ASSERT_TRUE(forwardB.targetVelocityMps < 0.0);

    const ev::MotionProfileCommand reverseB =
        ev::nextTargetVelocity(
            ev::MotionProfileInputs(ev::MotionProfileIntent::Travel,
                                    ev::MotionDirection::Reverse, 5.0, 0.0, 20U),
            reverseIsPositive);
    TEST_ASSERT_TRUE(reverseB.targetVelocityMps > 0.0);
}

// --- No elapsed time --------------------------------------------------------

void test_no_elapsed_time_holds_the_previous_target_exactly(void) {
    // The geometry would demand a much smaller target here, but no time passed,
    // so no change is authorized. The ceilings are still reported so a log can
    // show what the geometry was saying.
    const ev::MotionProfileCommand command =
        ev::nextTargetVelocity(travelForward(1.234, 0.1, 0U), fixtureLimits());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::TimeDidNotAdvance),
                          static_cast<int>(command.status));
    TEST_ASSERT_TRUE(command.targetVelocityMps == 1.234);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfilePhase::Cruising),
                          static_cast<int>(command.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileLimit::None),
                          static_cast<int>(command.binding));
    TEST_ASSERT_TRUE(std::fabs(command.cruiseCeilingMps - 2.0) <= kVelocityToleranceMps);
    TEST_ASSERT_TRUE(std::fabs(command.taperCeilingMps - std::sqrt(0.1)) <= kVelocityToleranceMps);
    TEST_ASSERT_TRUE(command.accelCeilingMps == 1.234);
    TEST_ASSERT_TRUE(command.decelCeilingMps == 1.234);
}

// --- Timing intent and duration ---------------------------------------------

void test_timing_intent_validation(void) {
    TEST_ASSERT_TRUE(ev::validateMotionTimingIntent(ev::MotionTimingIntent(13.5, 0.0)).ok);
    TEST_ASSERT_TRUE(ev::validateMotionTimingIntent(ev::MotionTimingIntent(13.5, 0.25)).ok);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::TimingIntentError::NonFiniteValue),
        static_cast<int>(
            ev::validateMotionTimingIntent(ev::MotionTimingIntent(kNaN, 0.0)).error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::TimingIntentError::NonFiniteValue),
        static_cast<int>(
            ev::validateMotionTimingIntent(ev::MotionTimingIntent(13.5, kInf)).error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::TimingIntentError::TargetTimeNotPositive),
        static_cast<int>(
            ev::validateMotionTimingIntent(ev::MotionTimingIntent(0.0, 0.0)).error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::TimingIntentError::DelayNegative),
        static_cast<int>(
            ev::validateMotionTimingIntent(ev::MotionTimingIntent(13.5, -0.1)).error));
}

void test_the_duration_is_the_trapezoid_profile(void) {
    // 20 m at 2.0 m/s under 0.5 m/s^2 of acceleration and 1.0 m/s^2 of
    // deceleration: 4 s accelerating, 7 s cruising 14 m, 2 s decelerating.
    const ev::MotionDurationEstimate estimate =
        ev::motionDurationS(20.0, ev::Sourced<double>(2.0, ev::ValueSource::Measured),
                            ev::Sourced<double>(0.5, ev::ValueSource::Measured),
                            ev::Sourced<double>(1.0, ev::ValueSource::Measured));
    expectDuration(13.0, estimate);
}

void test_the_duration_uses_the_triangular_profile_when_cruise_is_not_reached(void) {
    // 4 m at those limits: accelerate 4 m and decelerate 2 m would already be
    // 6 m, so cruise is never reached. The peak speed solves
    // 4 = v^2/(2*0.5) + v^2/(2*1.0), i.e. v = sqrt(8/3), and the duration is
    // v/0.5 + v/1.0.
    const double expectedS = std::sqrt(8.0 / 3.0) * 3.0;
    const ev::MotionDurationEstimate estimate =
        ev::motionDurationS(4.0, ev::Sourced<double>(2.0, ev::ValueSource::Measured),
                            ev::Sourced<double>(0.5, ev::ValueSource::Measured),
                            ev::Sourced<double>(1.0, ev::ValueSource::Measured));
    expectDuration(expectedS, estimate);
    TEST_ASSERT_TRUE(estimate.durationS < 13.0);
}

void test_the_duration_of_no_travel_is_zero_and_valid(void) {
    const ev::MotionDurationEstimate estimate =
        ev::motionDurationS(0.0, ev::Sourced<double>(2.0, ev::ValueSource::Measured),
                            ev::Sourced<double>(0.5, ev::ValueSource::Measured),
                            ev::Sourced<double>(1.0, ev::ValueSource::Measured));
    expectDuration(0.0, estimate);
}

void test_the_duration_refuses_unusable_limits_and_distances(void) {
    const ev::Sourced<double> measuredCruise(2.0, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredAccel(0.5, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredDecel(1.0, ev::ValueSource::Measured);

    // Not measured: no number at all, rather than a placeholder-shaped estimate.
    const ev::MotionDurationEstimate provisional =
        ev::motionDurationS(20.0, ev::Sourced<double>(2.0, ev::ValueSource::Provisional),
                            measuredAccel, measuredDecel);
    TEST_ASSERT_FALSE(provisional.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionDurationStatus::LimitsAreNotUsable),
                          static_cast<int>(provisional.status));
    TEST_ASSERT_TRUE(provisional.durationS == 0.0);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionDurationStatus::LimitsAreNotUsable),
        static_cast<int>(ev::motionDurationS(20.0, measuredCruise,
                                             ev::Sourced<double>(kNaN, ev::ValueSource::Measured),
                                             measuredDecel)
                             .status));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionDurationStatus::LimitsAreNotUsable),
        static_cast<int>(ev::motionDurationS(20.0, measuredCruise,
                                             ev::Sourced<double>(0.0, ev::ValueSource::Measured),
                                             measuredDecel)
                             .status));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionDurationStatus::LimitsAreNotUsable),
        static_cast<int>(ev::motionDurationS(20.0, measuredCruise, measuredAccel,
                                             ev::Sourced<double>(-1.0, ev::ValueSource::Measured))
                             .status));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionDurationStatus::DistanceNotFinite),
                          static_cast<int>(ev::motionDurationS(kNaN, measuredCruise, measuredAccel,
                                                               measuredDecel)
                                               .status));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotionDurationStatus::DistanceIsNegative),
        static_cast<int>(ev::motionDurationS(-1.0, measuredCruise, measuredAccel, measuredDecel)
                             .status));
}

void test_the_timing_summary_adds_the_delay_and_reports_the_margin(void) {
    const ev::Sourced<double> measuredCruise(2.0, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredAccel(0.5, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredDecel(1.0, ev::ValueSource::Measured);

    // 13.0 s of travel plus 0.25 s of deliberate delay against a 13.5 s target
    // time: the run intends to finish 0.25 s early.
    const ev::MotionTimingSummary summary =
        ev::motionTimingSummary(ev::MotionTimingIntent(13.5, 0.25), 20.0, measuredCruise,
                                measuredAccel, measuredDecel);
    expectSummary(13.0, 13.25, 0.25, summary);

    // A target time the limits cannot make: the margin is negative, and that is
    // all this module says about it. Acting on it is a run strategy decision and
    // is not made here.
    const ev::MotionTimingSummary late =
        ev::motionTimingSummary(ev::MotionTimingIntent(12.0, 0.0), 20.0, measuredCruise,
                                measuredAccel, measuredDecel);
    expectSummary(13.0, 13.0, -1.0, late);
}

void test_the_timing_summary_refuses_everything_it_cannot_compute(void) {
    const ev::Sourced<double> measuredCruise(2.0, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredAccel(0.5, ev::ValueSource::Measured);
    const ev::Sourced<double> measuredDecel(1.0, ev::ValueSource::Measured);

    const ev::MotionTimingSummary badIntent =
        ev::motionTimingSummary(ev::MotionTimingIntent(kNaN, 0.0), 20.0, measuredCruise,
                                measuredAccel, measuredDecel);
    TEST_ASSERT_FALSE(badIntent.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionTimingStatus::InvalidTimingIntent),
                          static_cast<int>(badIntent.status));
    TEST_ASSERT_TRUE(badIntent.travelDurationS == 0.0);
    TEST_ASSERT_TRUE(badIntent.intendedTotalS == 0.0);
    TEST_ASSERT_TRUE(badIntent.marginS == 0.0);

    const ev::MotionTimingSummary badDistance =
        ev::motionTimingSummary(ev::MotionTimingIntent(13.5, 0.0), -1.0, measuredCruise,
                                measuredAccel, measuredDecel);
    TEST_ASSERT_FALSE(badDistance.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionTimingStatus::InvalidDistance),
                          static_cast<int>(badDistance.status));

    const ev::MotionTimingSummary badLimits = ev::motionTimingSummary(
        ev::MotionTimingIntent(13.5, 0.0), 20.0,
        ev::Sourced<double>(2.0, ev::ValueSource::Provisional), measuredAccel, measuredDecel);
    TEST_ASSERT_FALSE(badLimits.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionTimingStatus::LimitsAreNotUsable),
                          static_cast<int>(badLimits.status));
}

// --- The caller-held handoff -------------------------------------------------

void test_the_profile_target_starts_at_zero_and_only_a_reset_clears_it(void) {
    ev::MotionProfileTarget target;
    TEST_ASSERT_TRUE(target.targetVelocityMps() == 0.0);

    const ev::MotionProfileLimits limits = fixtureLimits();

    // From rest the first step ramps at the 0.5 m/s^2 acceleration limit, and
    // the holder is where that ramp's memory lives.
    const ev::MotionProfileCommand first = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limits);
    TEST_ASSERT_TRUE(std::fabs(first.targetVelocityMps - 0.01) <= kVelocityToleranceMps);
    target.accept(first);
    TEST_ASSERT_TRUE(std::fabs(target.targetVelocityMps() - 0.01) <= kVelocityToleranceMps);

    const ev::MotionProfileCommand second = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limits);
    TEST_ASSERT_TRUE(std::fabs(second.targetVelocityMps - 0.02) <= kVelocityToleranceMps);
    target.accept(second);

    // The leak this holder exists to close: shaping from a leftover 1.0 m/s
    // target instead of the accepted 0.02 m/s produces a completely different
    // step (1.01 m/s instead of 0.03 m/s) with no compiler complaint.
    const ev::MotionProfileCommand leaked = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0,
                                1.0, 20U),
        limits);
    TEST_ASSERT_TRUE(std::fabs(leaked.targetVelocityMps - 1.01) <= kVelocityToleranceMps);
    const ev::MotionProfileCommand honest = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limits);
    TEST_ASSERT_TRUE(std::fabs(honest.targetVelocityMps - 0.03) <= kVelocityToleranceMps);

    // A refusal carries a zero target, so accepting one parks the holder at
    // zero rather than at a stale value.
    const ev::MotionProfileCommand refused = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, kNaN, 20U),
        limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::InvalidDistance),
                          static_cast<int>(refused.status));
    TEST_ASSERT_TRUE(refused.targetVelocityMps == 0.0);
    target.accept(refused);
    TEST_ASSERT_TRUE(target.targetVelocityMps() == 0.0);

    // The explicit new-run reset is the only thing a run boundary uses, and it
    // restarts the ramp from nothing.
    target.resetForNewRun();
    TEST_ASSERT_TRUE(target.targetVelocityMps() == 0.0);
    const ev::MotionProfileCommand restarted = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limits);
    TEST_ASSERT_TRUE(std::fabs(restarted.targetVelocityMps - 0.01) <= kVelocityToleranceMps);
}

void test_the_directional_limits_are_selected_by_direction(void) {
    // Two legs with deliberately different measured values, so attaching the
    // forward set to a reverse leg would be visible immediately.
    const ev::MotionProfileLimits forwardLimits = fixtureLimits();
    const ev::MotionProfileLimits reverseLimits(
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),   // cruise
        ev::Sourced<double>(1.5, ev::ValueSource::Measured),   // velocity ceiling
        ev::Sourced<double>(50.0, ev::ValueSource::Measured),  // acceleration limit
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),   // deceleration limit
        ev::Sourced<double>(0.02, ev::ValueSource::Measured),  // brake offset
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
    const ev::DirectionalMotionProfileLimits limits(forwardLimits, reverseLimits);

    // A reverse leg asked for a full step reaches the reverse cruise ceiling...
    const ev::MotionProfileCommand reverse = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                0.0, 20U),
        limits.forDirection(ev::MotionDirection::Reverse));
    TEST_ASSERT_TRUE(std::fabs(reverse.targetVelocityMps - (-1.0)) <= kVelocityToleranceMps);
    TEST_ASSERT_TRUE(std::fabs(reverse.cruiseCeilingMps - 1.0) <= kVelocityToleranceMps);

    // ...while a forward leg still sees the forward set. A wrong-leg selection
    // would show up here as the forward ramp step of 0.01 m/s.
    const ev::MotionProfileCommand forward = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0,
                                0.0, 20U),
        limits.forDirection(ev::MotionDirection::Forward));
    TEST_ASSERT_TRUE(std::fabs(forward.targetVelocityMps - 0.01) <= kVelocityToleranceMps);
    TEST_ASSERT_TRUE(std::fabs(forward.cruiseCeilingMps - 2.0) <= kVelocityToleranceMps);
}

// --- Property sweep ---------------------------------------------------------

void test_the_target_never_exceeds_the_ceilings_or_flips_itself(void) {
    const ev::MotionProfileLimits limits = fixtureLimits();
    // 1e-9 m is inside the brake offset; 1e300 m is large enough that
    // 2 * decelLimit * remaining overflows toward infinity before the square
    // root, so the cruise ceiling is the only thing that can keep the target
    // finite.
    const double remainingsM[] = {0.0, 1e-9, 0.05, 0.1, 1.0, 5.0, 100.0, 1e300};
    const double currentsMps[] = {-2.0, -0.5, 0.0, 0.5, 2.0};
    const uint32_t stepsMs[] = {1U, 20U, 1000U};
    const ev::MotionProfileIntent intents[] = {ev::MotionProfileIntent::Travel,
                                               ev::MotionProfileIntent::Stop};
    const ev::MotionDirection directions[] = {ev::MotionDirection::Forward,
                                              ev::MotionDirection::Reverse};

    for (double remainingM : remainingsM) {
        for (double currentMps : currentsMps) {
            for (uint32_t dtMs : stepsMs) {
                for (ev::MotionProfileIntent intent : intents) {
                    for (ev::MotionDirection direction : directions) {
                        const ev::MotionProfileInputs inputs(intent, direction, remainingM,
                                                             currentMps, dtMs);
                        const ev::MotionProfileCommand command =
                            ev::nextTargetVelocity(inputs, limits);

                        TEST_ASSERT_TRUE(std::isfinite(command.targetVelocityMps));
                        // No direction flip inside one step: while a change is
                        // being ramped, the sign belongs to the old direction.
                        const bool directionChange = currentMps != 0.0 &&
                                                     ((direction ==
                                                       ev::MotionDirection::Forward) !=
                                                      (currentMps > 0.0));
                        if (directionChange && command.targetVelocityMps != 0.0) {
                            TEST_ASSERT_TRUE((command.targetVelocityMps > 0.0) ==
                                             (currentMps > 0.0));
                        }
                        // The magnitude is never raised above the cruise
                        // ceiling and never above the current target.
                        const double magnitudeMps = std::fabs(command.targetVelocityMps);
                        TEST_ASSERT_TRUE(magnitudeMps <=
                                         std::fmax(std::fabs(currentMps),
                                                   command.cruiseCeilingMps) +
                                             kVelocityToleranceMps);
                        // Nothing the geometry computes can raise the target above
                        // the cruise ceiling; a target already above it may only
                        // decay. This is what keeps a 1e300 m remaining distance
                        // from turning into a 1e150 m/s target.
                        TEST_ASSERT_TRUE(magnitudeMps <=
                                             command.cruiseCeilingMps + kVelocityToleranceMps ||
                                         std::fabs(currentMps) > command.cruiseCeilingMps);
                        // A shaped non-zero target always names the ceiling that
                        // produced it.
                        if (command.status != ev::MotionProfileStatus::TimeDidNotAdvance &&
                            command.targetVelocityMps != 0.0) {
                            TEST_ASSERT_TRUE(command.binding != ev::MotionProfileLimit::None);
                        }
                        // A stop request or a reached target point never asks for
                        // a powered effort.
                        if (command.status == ev::MotionProfileStatus::StopRequested ||
                            command.status == ev::MotionProfileStatus::WithinBrakeOffset) {
                            TEST_ASSERT_TRUE(magnitudeMps <=
                                             std::fabs(currentMps) + kVelocityToleranceMps);
                        }
                    }
                }
            }
        }
    }
}

// --- The along-leg gate -------------------------------------------------------

void test_a_shaped_target_points_along_the_leg_it_is_for(void) {
    // The fixture polarity: a positive command sign means forward travel.
    const ev::MotionProfileLimits limits = fixtureLimits();

    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(commandAt(0.5),
                                                      ev::MotionDirection::Forward, limits));
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(commandAt(-0.5),
                                                       ev::MotionDirection::Forward, limits));
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(commandAt(-0.5),
                                                      ev::MotionDirection::Reverse, limits));
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(commandAt(0.5),
                                                       ev::MotionDirection::Reverse, limits));

    // A zero target asks for no powered motion, so it is along any leg.
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(commandAt(0.0),
                                                      ev::MotionDirection::Forward, limits));
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(commandAt(0.0),
                                                      ev::MotionDirection::Reverse, limits));

    // A non-finite target is not something the shaper produces (a refusal shapes
    // zero), so it is refused rather than given a sign.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(commandAt(nan),
                                                       ev::MotionDirection::Forward, limits));
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(commandAt(inf),
                                                       ev::MotionDirection::Forward, limits));
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(commandAt(-inf),
                                                       ev::MotionDirection::Reverse, limits));
}

void test_a_reversal_in_progress_does_not_point_along_the_new_leg(void) {
    // This is the case the gate exists for. A travel for the reverse leg arrives
    // while the target is still ramping down from the forward leg: the profile
    // reports DirectionChangeHold and the shaped target is still positive, so
    // presenting it as powered for the reverse leg would ask for effort in the
    // direction the run has just left.
    const ev::MotionProfileLimits limits = fixtureLimits();
    const ev::MotionProfileCommand holding = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                1.0, 100U),
        limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::DirectionChangeHold),
                          static_cast<int>(holding.status));
    TEST_ASSERT_TRUE(holding.targetVelocityMps > 0.0);
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(holding, ev::MotionDirection::Reverse, limits));
    // The same shape is still along the leg it is finishing, which is why the
    // gate is about the leg the run asked for and not about "moving".
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(holding, ev::MotionDirection::Forward, limits));

    // Reaching zero is what lets the sign change, and a zero target is along any
    // leg; the step after it carries the new sign and is along the new leg.
    const ev::MotionProfileCommand zeroed = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                0.01, 100U),
        limits);
    TEST_ASSERT_TRUE(zeroed.targetVelocityMps == 0.0);
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(zeroed, ev::MotionDirection::Reverse, limits));

    const ev::MotionProfileCommand rising = ev::nextTargetVelocity(
        ev::MotionProfileInputs(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 5.0,
                                0.0, 20U),
        limits);
    TEST_ASSERT_TRUE(rising.targetVelocityMps < 0.0);
    TEST_ASSERT_TRUE(ev::targetVelocityPointsAlongLeg(rising, ev::MotionDirection::Reverse, limits));
    TEST_ASSERT_FALSE(ev::targetVelocityPointsAlongLeg(rising, ev::MotionDirection::Forward, limits));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_shaping_limits_require_measured_values);
    RUN_TEST(test_shaping_limits_reject_malformed_values);
    RUN_TEST(test_unusable_limits_never_shape_a_target);
    RUN_TEST(test_invalid_inputs_never_shape_a_target);

    RUN_TEST(test_a_travel_from_rest_ramps_up_at_the_acceleration_limit);
    RUN_TEST(test_the_ramp_reaches_cruise_after_the_configured_time);
    RUN_TEST(test_the_ramp_uses_the_elapsed_time_it_was_given);
    RUN_TEST(test_an_uneven_step_sequence_is_measured_step_by_step);

    RUN_TEST(test_the_taper_is_monotonic_and_matches_the_deceleration_limit);
    RUN_TEST(test_the_taper_pulls_the_target_down_instead_of_snapping_it);
    RUN_TEST(test_the_taper_subtracts_the_measured_brake_offset);
    RUN_TEST(test_once_the_brake_offset_is_reached_the_target_goes_to_zero_and_stays);

    RUN_TEST(test_a_stop_request_ramps_the_target_down_and_preserves_its_sign);
    RUN_TEST(test_a_direction_change_is_ramped_through_zero_without_a_sign_flip);
    RUN_TEST(test_the_polarity_maps_the_requested_direction_to_the_command_sign);

    RUN_TEST(test_no_elapsed_time_holds_the_previous_target_exactly);

    RUN_TEST(test_timing_intent_validation);
    RUN_TEST(test_the_duration_is_the_trapezoid_profile);
    RUN_TEST(test_the_duration_uses_the_triangular_profile_when_cruise_is_not_reached);
    RUN_TEST(test_the_duration_of_no_travel_is_zero_and_valid);
    RUN_TEST(test_the_duration_refuses_unusable_limits_and_distances);
    RUN_TEST(test_the_timing_summary_adds_the_delay_and_reports_the_margin);
    RUN_TEST(test_the_timing_summary_refuses_everything_it_cannot_compute);

    RUN_TEST(test_the_profile_target_starts_at_zero_and_only_a_reset_clears_it);
    RUN_TEST(test_the_directional_limits_are_selected_by_direction);

    RUN_TEST(test_the_target_never_exceeds_the_ceilings_or_flips_itself);

    RUN_TEST(test_a_shaped_target_points_along_the_leg_it_is_for);
    RUN_TEST(test_a_reversal_in_progress_does_not_point_along_the_new_leg);

    return UNITY_END();
}
