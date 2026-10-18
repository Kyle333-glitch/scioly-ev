#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <MotionProfile.h>
#include <Odometry.h>
#include <RunAxis.h>
#include <Units.h>
#include <VelocityEstimator.h>

// S22 cross-module distance-consistency contract tests.
//
// These tests do NOT duplicate the per-module suites (test_odometry,
// test_run_axis, test_velocity_estimator, test_motion_profile, ...). They pin
// the seams between those modules: that one fixture scale fed to the
// odometry conversion, the run-axis position (polarity applied once) and the
// windowed velocity estimator agree with each other, and that a single
// encoder tick is the quantization step that flips the travel taper's
// WithinBrakeOffset boundary.
//
// Every tick count, scale and distance below is a SYNTHETIC fixture. Nothing
// here is a calibration value, a logged count from a real encoder, or
// evidence about the vehicle. VERIFIED_STATE.md #4 (direction sign), #5
// (signal quality), #6 (rollout) and #10 (wheel slip) all stay UNVERIFIED.
// In particular the fractional scale 0.001339 m/tick is an arbitrary
// arithmetic fixture, not a measurement and not the informal ~1.339 mm/tick
// observation from 2026-10-02 (which was one unrepeated push with no measured
// travel and is explicitly not a calibration).

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kToleranceM = 1e-9;
const double kToleranceMps = 1e-9;

ev::Sourced<double> measuredScale(double metersPerEncoderTick) {
    return ev::Sourced<double>(metersPerEncoderTick, ev::ValueSource::Measured);
}

ev::Sourced<double> provisionalScale(double metersPerEncoderTick) {
    return ev::Sourced<double>(metersPerEncoderTick, ev::ValueSource::Provisional);
}

ev::Sourced<ev::MotionDirection> measuredPolarity(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Measured);
}

ev::MotionProfileLimits fixtureLimits(double brakeOffsetM) {
    return ev::MotionProfileLimits(
        ev::Sourced<double>(2.0, ev::ValueSource::Measured),
        ev::Sourced<double>(2.5, ev::ValueSource::Measured),
        ev::Sourced<double>(0.5, ev::ValueSource::Measured),
        ev::Sourced<double>(1.0, ev::ValueSource::Measured),
        ev::Sourced<double>(brakeOffsetM, ev::ValueSource::Measured),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward, ev::ValueSource::Measured));
}

}  // namespace

// One fixture scale through odometry, the run axis and the velocity
// estimator must agree: 1000 ticks at 0.1 mm/tick is 0.1 m of travel, the
// run-axis position under a measured forward polarity, and a 1000-tick
// window over 1.0 s is 0.1 m/s.
void test_same_scale_agrees_across_odometry_run_axis_and_velocity(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);

    const ev::OdometryEstimate travel = ev::distanceTravelledM(0, 1000, scale);
    TEST_ASSERT_TRUE(travel.valid);
    TEST_ASSERT_TRUE(std::fabs(travel.distanceM - 0.1) <= kToleranceM);

    const ev::RunAxisPosition position = ev::runAxisPositionM(
        0, 1000, scale, measuredPolarity(ev::MotionDirection::Forward));
    TEST_ASSERT_TRUE(position.usable);
    TEST_ASSERT_TRUE(std::fabs(position.positionM - travel.distanceM) <= kToleranceM);

    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig(100U, 1000U));
    estimator.update(0U, 0, scale);
    estimator.update(500U, 500, scale);
    const ev::VelocityEstimate estimate = estimator.update(1000U, 1000, scale);
    TEST_ASSERT_TRUE(estimate.valid);
    TEST_ASSERT_TRUE(std::fabs(estimate.velocityMps - 0.1) <= kToleranceMps);
    // The estimator prunes to the shortest span still covering the 100 ms
    // window, so the realized span here is 500 ms / 500 ticks (500 -> 1000),
    // not the full 1000-tick trace. The velocity still agrees with odometry.
    // (No TEST_ASSERT_EQUAL_INT64: Unity 64-bit asserts are disabled here.)
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 500);
}

// Polarity is applied exactly once, in the run axis: flipping the measured
// polarity inverts the run-axis sign while the odometry magnitude is
// unchanged.
void test_polarity_flip_inverts_run_axis_but_not_odometry_magnitude(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::OdometryEstimate travel = ev::distanceTravelledM(0, 1000, scale);

    const ev::RunAxisPosition forward = ev::runAxisPositionM(
        0, 1000, scale, measuredPolarity(ev::MotionDirection::Forward));
    const ev::RunAxisPosition reverse = ev::runAxisPositionM(
        0, 1000, scale, measuredPolarity(ev::MotionDirection::Reverse));
    TEST_ASSERT_TRUE(forward.usable);
    TEST_ASSERT_TRUE(reverse.usable);
    TEST_ASSERT_TRUE(std::fabs(forward.positionM + reverse.positionM) <= kToleranceM);
    TEST_ASSERT_TRUE(std::fabs(forward.positionM - travel.distanceM) <= kToleranceM);
}

// A fractional (sub-micrometre-unrepresentable) synthetic scale converts
// linearly: 1494 synthetic ticks at 0.001339 m/tick is 2.000466 m. The tick
// count is an arbitrary arithmetic fixture, not a vehicle measurement.
void test_fractional_scale_converts_linearly(void) {
    const ev::Sourced<double> scale = measuredScale(0.001339);
    const ev::OdometryEstimate travel = ev::distanceTravelledM(0, 1494, scale);
    TEST_ASSERT_TRUE(travel.valid);
    TEST_ASSERT_TRUE(std::fabs(travel.distanceM - 2.000466) <= 1e-6);
    const ev::OdometryEstimate reverse = ev::distanceTravelledM(0, -1494, scale);
    TEST_ASSERT_TRUE(reverse.valid);
    TEST_ASSERT_TRUE(std::fabs(reverse.distanceM + travel.distanceM) <= 1e-9);
}

// Splitting one tick span at an intermediate origin is exact: counter
// continuity across a run-origin change loses nothing.
void test_origin_change_preserves_total_travel(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::OdometryEstimate whole = ev::distanceTravelledM(100000, 105000, scale);
    const ev::OdometryEstimate first = ev::distanceTravelledM(100000, 102500, scale);
    const ev::OdometryEstimate second = ev::distanceTravelledM(102500, 105000, scale);
    TEST_ASSERT_TRUE(whole.valid && first.valid && second.valid);
    TEST_ASSERT_TRUE(std::fabs((first.distanceM + second.distanceM) - whole.distanceM) <=
                     kToleranceM);
}

// One encoder tick is the quantization step at the taper boundary: with a
// 0.1 mm/tick scale and a 0.05 m brake offset, 500 ticks of remaining travel
// sits exactly on the boundary (WithinBrakeOffset) while 501 ticks is still
// outside it.
void test_single_tick_flips_the_brake_offset_boundary(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const double brakeOffsetM = 0.05;
    const ev::MotionProfileLimits limits = fixtureLimits(brakeOffsetM);

    // Remaining distances derived from tick counts, the way the vehicle layer
    // derives legRemainingM from the run-axis position.
    const double remainingOnBoundaryM = static_cast<double>(500) * scale.value;
    const double remainingOneTickOutsideM = static_cast<double>(501) * scale.value;
    TEST_ASSERT_TRUE(std::fabs(remainingOnBoundaryM - brakeOffsetM) <= kToleranceM);

    const ev::MotionProfileInputs onBoundary(ev::MotionProfileIntent::Travel,
                                             ev::MotionDirection::Forward,
                                             remainingOnBoundaryM, 0.0, 20U,
                                             ev::inactiveStrategyVelocityCeiling());
    const ev::MotionProfileInputs oneTickOutside(ev::MotionProfileIntent::Travel,
                                                 ev::MotionDirection::Forward,
                                                 remainingOneTickOutsideM, 0.0, 20U,
                                                 ev::inactiveStrategyVelocityCeiling());
    const ev::MotionProfileCommand atBoundary = ev::nextTargetVelocity(onBoundary, limits);
    const ev::MotionProfileCommand outside = ev::nextTargetVelocity(oneTickOutside, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotionProfileStatus::WithinBrakeOffset),
                          static_cast<int>(atBoundary.status));
    TEST_ASSERT_TRUE(atBoundary.targetVelocityMps == 0.0);
    TEST_ASSERT_TRUE(outside.status != ev::MotionProfileStatus::WithinBrakeOffset);
}

// A zero tick delta is zero distance and still valid; a position with a zero
// origin is exactly the travel.
void test_zero_delta_is_zero_distance_and_still_valid(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::OdometryEstimate travel = ev::distanceTravelledM(12345, 12345, scale);
    TEST_ASSERT_TRUE(travel.valid);
    TEST_ASSERT_TRUE(travel.distanceM == 0.0);
    const ev::RunAxisPosition position = ev::runAxisPositionM(
        0, 0, scale, measuredPolarity(ev::MotionDirection::Forward));
    TEST_ASSERT_TRUE(position.usable);
    TEST_ASSERT_TRUE(position.positionM == 0.0);
}

// One bad scale is refused the same way everywhere: odometry reports no
// valid estimate, the velocity estimator reports the matching refusal with
// no number, and the run axis reports no usable position.
void test_invalid_scale_is_refused_consistently_across_modules(void) {
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    const ev::Sourced<double> bad[] = {
        measuredScale(0.0), measuredScale(-0.0001), measuredScale(kNaN),
        provisionalScale(0.0001),
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const ev::OdometryEstimate travel = ev::distanceTravelledM(0, 1000, bad[i]);
        TEST_ASSERT_FALSE(travel.valid);
        const ev::RunAxisPosition position = ev::runAxisPositionM(
            0, 1000, bad[i], measuredPolarity(ev::MotionDirection::Forward));
        TEST_ASSERT_FALSE(position.usable);
        ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig(20U, 200U));
        estimator.update(0U, 0, bad[i]);
        estimator.update(20U, 10, bad[i]);
        const ev::VelocityEstimate estimate = estimator.update(40U, 20, bad[i]);
        TEST_ASSERT_FALSE(estimate.valid);
        TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_same_scale_agrees_across_odometry_run_axis_and_velocity);
    RUN_TEST(test_polarity_flip_inverts_run_axis_but_not_odometry_magnitude);
    RUN_TEST(test_fractional_scale_converts_linearly);
    RUN_TEST(test_origin_change_preserves_total_travel);
    RUN_TEST(test_single_tick_flips_the_brake_offset_boundary);
    RUN_TEST(test_zero_delta_is_zero_distance_and_still_valid);
    RUN_TEST(test_invalid_scale_is_refused_consistently_across_modules);
    return UNITY_END();
}
