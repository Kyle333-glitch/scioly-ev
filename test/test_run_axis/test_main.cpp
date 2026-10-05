#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <RunAxis.h>
#include <RunConfig.h>
#include <Units.h>

// Contract tests for the run axis: the announced *rule* geometry mapped from
// RunConfig, the tick-sign polarity applied exactly once, and the generic
// remaining-distance arithmetic that takes an explicit caller-supplied target.
//
// S2 separates official rule geometry from vehicle execution geometry. Every
// landmark below is a rule landmark (Target Point, Bottle Start Point, Bottle
// Line); no physical forward stop target exists here and none is invented. The
// one pre-S3/S4 compatibility mapping for the forward leg's execution target is
// pinned as compatibility behaviour, never as physical truth.
//
// Every value below is a test fixture, not a measurement and not an announced
// competition configuration. A passing test is arithmetic verification only: it
// is not evidence that any encoder produces those ticks, that the tick-sign
// polarity is known, or that an announced run exists (VERIFIED_STATE.md #4, #6
// stay UNVERIFIED).

// Pinned at compile time: a geometry, a position and a status must not arrive
// without being built from something.
static_assert(!std::is_default_constructible<ev::RunAxisGeometry>::value,
              "a geometry must be built from announced parameters, never defaulted");
static_assert(!std::is_default_constructible<ev::RunAxisPosition>::value,
              "a position must state how it was produced");
// The permissive members are the zero values, so a caller must never reach them
// without a check.
static_assert(static_cast<int>(ev::RunAxisStatus::Usable) == 0,
              "the usable status must stay the zero value");
// The Bottle Line is the official rule landmark: the Target Point plus the
// announced Bottle Line Distance (rule 8.j). It is not the vehicle's physical
// turnaround and this file asserts no such thing.
static_assert(ev::RunAxisGeometry(5.0, 1.0, 2.0).bottleLinePositionM() == 6.0,
              "the Bottle Line must be the Target Point plus the Bottle Line Distance");
static_assert(ev::RunAxisGeometry(0.0, 0.0, 0.0).bottleLinePositionM() == 0.0,
              "the zero geometry's Bottle Line must be exactly the axis origin");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Comparisons use an explicit absolute tolerance rather than a double equality
// assert, so this target does not depend on optional Unity double support.
const double kToleranceM = 1e-9;

ev::Sourced<double> sourcedScale(double metersPerEncoderTick, ev::ValueSource source) {
    return ev::Sourced<double>(metersPerEncoderTick, source);
}

// Fixture scale: 0.0001 m per tick, the same bench-candidate value the other
// targets use. A real value can only come from a rollout measurement.
ev::Sourced<double> measuredScale(double metersPerEncoderTick) {
    return sourcedScale(metersPerEncoderTick, ev::ValueSource::Measured);
}

ev::Sourced<ev::MotionDirection> effectivePolarity(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Measured);
}

ev::Sourced<ev::MotionDirection> placeholderPolarity(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Provisional);
}

// The Regional fixture announcement: 5.00 m Target Distance, 1.00 m Bottle Line
// Distance, the Regional 2.0 m bottle placement. Its rule landmarks are the
// Target Point at 5.0 m, the Bottle Start Point at 2.0 m and the Bottle Line at
// 6.0 m.
ev::RunAxisGeometry regionalGeometry() {
    return ev::RunAxisGeometry::fromRunConfig(
        ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0));
}

void expectPosition(ev::RunAxisStatus expectedStatus, bool expectedUsable,
                    double expectedPositionM, const ev::RunAxisPosition& position) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStatus), static_cast<int>(position.status));
    TEST_ASSERT_EQUAL_INT(expectedUsable ? 1 : 0, position.usable ? 1 : 0);
    // The documented invariant, checked on every inspection: usable is exactly
    // the Usable status, never an independent flag.
    TEST_ASSERT_EQUAL_INT(position.status == ev::RunAxisStatus::Usable ? 1 : 0,
                          position.usable ? 1 : 0);
    TEST_ASSERT_TRUE(std::fabs(position.positionM - expectedPositionM) <= kToleranceM);
}

}  // namespace

// --- The announced rule geometry ---------------------------------------------

void test_the_announced_configuration_maps_onto_the_run_axis(void) {
    // The Regional fixture announcement: 5.00 m Target Distance, 1.00 m Bottle
    // Line Distance, the Regional 2.0 m bottle placement.
    const ev::RunConfig config(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(config);

    // The three rule landmarks, each on its own.
    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 5.0);
    TEST_ASSERT_TRUE(geometry.bottleStartPositionM == 2.0);
    TEST_ASSERT_TRUE(geometry.bottleLineDistanceM == 1.0);
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() == 6.0);
}

void test_a_different_announcement_moves_every_position_with_it(void) {
    // A State-grid announcement: 5.25 m and 1.25 m, with the State 3.0 m bottle
    // placement. The Bottle Line is 6.50 m, not the 6.00 m of the Regional case.
    const ev::RunConfig config(ev::TournamentLevel::State, 5.25, 10.0, 1.25, 3.0);
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(config);

    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 5.25);
    TEST_ASSERT_TRUE(geometry.bottleStartPositionM == 3.0);
    TEST_ASSERT_TRUE(geometry.bottleLineDistanceM == 1.25);
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() == 6.5);
}

void test_the_bottle_line_is_the_target_plus_the_announced_bottle_line_distance(void) {
    const ev::RunAxisGeometry geometry(7.0, 2.5, 4.0);
    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 7.0);
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() == 9.5);
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() ==
                     geometry.targetPointPositionM + geometry.bottleLineDistanceM);
}

// --- Position on the run axis ------------------------------------------------

void test_a_positive_tick_delta_is_forward_travel_under_a_measured_forward_polarity(void) {
    const ev::RunAxisPosition position = ev::runAxisPositionM(
        1000, 4000, measuredScale(0.0001), effectivePolarity(ev::MotionDirection::Forward));
    expectPosition(ev::RunAxisStatus::Usable, true, 0.3, position);
}

void test_a_measured_reverse_polarity_turns_the_tick_sign_around(void) {
    // The same ticks, the other measured polarity: the vehicle travelled the
    // other way on the run axis.
    const ev::RunAxisPosition position = ev::runAxisPositionM(
        1000, 4000, measuredScale(0.0001), effectivePolarity(ev::MotionDirection::Reverse));
    expectPosition(ev::RunAxisStatus::Usable, true, -0.3, position);
}

void test_the_axis_zero_is_the_origin_reading_not_tick_zero(void) {
    // A run that starts from an accumulated counter is still at position zero on
    // its own axis, and only the difference of the two readings is converted.
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::Sourced<ev::MotionDirection> polarity = effectivePolarity(ev::MotionDirection::Forward);

    expectPosition(ev::RunAxisStatus::Usable, true, 0.0,
                   ev::runAxisPositionM(123456, 123456, scale, polarity));
    expectPosition(ev::RunAxisStatus::Usable, true, 0.1,
                   ev::runAxisPositionM(123456, 124456, scale, polarity));

    const ev::RunAxisPosition large = ev::runAxisPositionM(-2000000000, -1999999000, scale, polarity);
    expectPosition(ev::RunAxisStatus::Usable, true, 0.1, large);
}

void test_a_standstill_is_exactly_the_axis_origin(void) {
    const ev::RunAxisPosition position = ev::runAxisPositionM(
        4000, 4000, measuredScale(0.0001), effectivePolarity(ev::MotionDirection::Forward));
    expectPosition(ev::RunAxisStatus::Usable, true, 0.0, position);
    TEST_ASSERT_TRUE(position.positionM == 0.0);
}

// --- What the polarity provenance does ---------------------------------------

void test_an_unmeasured_tick_polarity_refuses_the_position_but_reports_a_diagnostic(void) {
    // A placeholder polarity still yields a diagnostic number - the same
    // convention as a Provisional scale in Odometry.h - and it may not reach
    // control: no run may be driven on a position whose sign is a guess.
    const ev::RunAxisPosition placeholderForward = ev::runAxisPositionM(
        0, 3000, measuredScale(0.0001), placeholderPolarity(ev::MotionDirection::Forward));
    expectPosition(ev::RunAxisStatus::PolarityIsNotMeasured, false, 0.3, placeholderForward);

    const ev::RunAxisPosition placeholderReverse = ev::runAxisPositionM(
        0, 3000, measuredScale(0.0001), placeholderPolarity(ev::MotionDirection::Reverse));
    expectPosition(ev::RunAxisStatus::PolarityIsNotMeasured, false, -0.3, placeholderReverse);
}

void test_a_position_never_survives_an_unusable_scale(void) {
    const ev::Sourced<ev::MotionDirection> polarity = effectivePolarity(ev::MotionDirection::Forward);

    // A Provisional scale is a diagnostic conversion: a number exists, and it is
    // not usable.
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.3,
                   ev::runAxisPositionM(0, 3000, sourcedScale(0.0001, ev::ValueSource::Provisional),
                                        polarity));

    // No number at all for a refused scale, so nothing can be mistaken for a
    // legitimate "has not moved yet" reading.
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.0,
                   ev::runAxisPositionM(0, 3000, measuredScale(0.0), polarity));
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.0,
                   ev::runAxisPositionM(0, 3000, measuredScale(-0.0001), polarity));
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.0,
                   ev::runAxisPositionM(0, 3000, measuredScale(kNaN), polarity));
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.0,
                   ev::runAxisPositionM(0, 3000, measuredScale(kInf), polarity));

    // A measured, finite, positive scale whose product overflows is still no
    // position: 2e9 ticks at 1e300 m/tick is above the largest finite double.
    expectPosition(ev::RunAxisStatus::TickTravelUnusable, false, 0.0,
                   ev::runAxisPositionM(0, 2000000000, measuredScale(1e300), polarity));
}

void test_the_usable_invariant_holds_across_the_input_combinations(void) {
    const ev::Sourced<double> scales[] = {
        measuredScale(0.0001), sourcedScale(0.0001, ev::ValueSource::Provisional),
        measuredScale(0.0), measuredScale(kNaN), measuredScale(1e300)};
    const ev::Sourced<ev::MotionDirection> polarities[] = {
        effectivePolarity(ev::MotionDirection::Forward),
        effectivePolarity(ev::MotionDirection::Reverse),
        placeholderPolarity(ev::MotionDirection::Forward),
        placeholderPolarity(ev::MotionDirection::Reverse)};

    for (const ev::Sourced<double>& scale : scales) {
        for (const ev::Sourced<ev::MotionDirection>& polarity : polarities) {
            const ev::RunAxisPosition position =
                ev::runAxisPositionM(-5000, 5000, scale, polarity);

            TEST_ASSERT_EQUAL_INT(position.status == ev::RunAxisStatus::Usable ? 1 : 0,
                                  position.usable ? 1 : 0);
            if (position.usable) {
                // A usable position demands every fact at once.
                TEST_ASSERT_TRUE(scale.isMeasured());
                TEST_ASSERT_TRUE(polarity.isMeasured());
                TEST_ASSERT_TRUE(std::isfinite(scale.value));
                TEST_ASSERT_TRUE(scale.value > 0.0);
                TEST_ASSERT_TRUE(std::isfinite(position.positionM));
            }
        }
    }
}

// --- Explicit-target remaining distance --------------------------------------

void test_the_generic_arithmetic_uses_the_explicit_target_not_the_bottle_line(void) {
    // The rule geometry is 5.0 m Target Point / 6.0 m Bottle Line, and it must
    // not leak into the arithmetic: the caller names the target.
    const ev::RunAxisGeometry geometry = regionalGeometry();
    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 5.0);
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() == 6.0);

    const double positionM = 4.0;
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(geometry.targetPointPositionM, positionM, true) == 1.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(geometry.bottleLinePositionM(), positionM, true) == 2.0);
    // A target that is neither landmark is still honoured exactly, so the Bottle
    // Line (or any other landmark) cannot secretly stand in for it.
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(7.0, positionM, true) == 3.0);
}

void test_two_different_explicit_targets_give_two_different_remaining_distances(void) {
    // Same position, same rule geometry, two caller-supplied targets: the answer
    // tracks the target the caller named.
    const ev::RunAxisGeometry geometry = regionalGeometry();
    const double positionM = 4.0;

    const double toNearTargetM = ev::remainingDistanceTo(5.0, positionM, true);
    const double toFarTargetM = ev::remainingDistanceTo(6.5, positionM, true);
    TEST_ASSERT_TRUE(toNearTargetM == 1.0);
    TEST_ASSERT_TRUE(toFarTargetM == 2.5);
    TEST_ASSERT_TRUE(toFarTargetM != toNearTargetM);
    // The rule geometry is unchanged by either call.
    TEST_ASSERT_TRUE(geometry.bottleLinePositionM() == 6.0);
}

void test_the_signed_remaining_distance_follows_the_leg(void) {
    // Forward leg: positive while short of the target, zero at it, negative past
    // it - an overshoot is evidence, so it is not clamped away.
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 4.0, true) == 1.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 5.0, true) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 5.5, true) == -0.5);

    // Reverse leg: the same explicit target, approached from the far side.
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 5.5, false) == 0.5);
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 5.0, false) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceTo(5.0, 4.5, false) == -0.5);
}

void test_the_clamped_remaining_distance_is_never_below_zero(void) {
    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 4.0, true) == 1.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 5.0, true) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 6.0, true) == 0.0);

    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 6.5, false) == 1.5);
    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 5.0, false) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingDistanceToClamped(5.0, 4.0, false) == 0.0);
}

// --- The temporary pre-S3/S4 compatibility seam -------------------------------

void test_the_compatibility_seam_maps_the_forward_leg_onto_the_bottle_line_landmark(void) {
    // This pins TEMPORARY pre-S3/S4 COMPATIBILITY BEHAVIOUR so that changing it
    // is a deliberate act. It is NOT an assertion that the Bottle Line is the
    // vehicle's physical turnaround, forward-leg endpoint or forward stop target.
    const ev::RunAxisGeometry geometry = regionalGeometry();
    TEST_ASSERT_TRUE(ev::temporaryLegExecutionTargetM(geometry, true) ==
                     geometry.bottleLinePositionM());
    // The reverse leg's target is the rule Target Point and is not temporary.
    TEST_ASSERT_TRUE(ev::temporaryLegExecutionTargetM(geometry, false) ==
                     geometry.targetPointPositionM);
    // A different announcement moves the seam's forward target with the rule
    // landmark it stands in for - the behaviour S3/S4 must replace.
    const ev::RunAxisGeometry wider(5.0, 2.5, 2.0);
    TEST_ASSERT_TRUE(ev::temporaryLegExecutionTargetM(wider, true) == 7.5);
}

void test_the_planned_travel_is_the_rule_course_out_and_back(void) {
    // Forward out to the Bottle Line (6.0 m) then the reverse leg back to the
    // Target Point (1.0 m): a rule quantity used to size the timing plan.
    const ev::RunAxisGeometry geometry = regionalGeometry();
    TEST_ASSERT_TRUE(ev::plannedTravelDistanceM(geometry) == 7.0);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_the_announced_configuration_maps_onto_the_run_axis);
    RUN_TEST(test_a_different_announcement_moves_every_position_with_it);
    RUN_TEST(test_the_bottle_line_is_the_target_plus_the_announced_bottle_line_distance);

    RUN_TEST(test_a_positive_tick_delta_is_forward_travel_under_a_measured_forward_polarity);
    RUN_TEST(test_a_measured_reverse_polarity_turns_the_tick_sign_around);
    RUN_TEST(test_the_axis_zero_is_the_origin_reading_not_tick_zero);
    RUN_TEST(test_a_standstill_is_exactly_the_axis_origin);

    RUN_TEST(test_an_unmeasured_tick_polarity_refuses_the_position_but_reports_a_diagnostic);
    RUN_TEST(test_a_position_never_survives_an_unusable_scale);
    RUN_TEST(test_the_usable_invariant_holds_across_the_input_combinations);

    RUN_TEST(test_the_generic_arithmetic_uses_the_explicit_target_not_the_bottle_line);
    RUN_TEST(test_two_different_explicit_targets_give_two_different_remaining_distances);
    RUN_TEST(test_the_signed_remaining_distance_follows_the_leg);
    RUN_TEST(test_the_clamped_remaining_distance_is_never_below_zero);

    RUN_TEST(test_the_compatibility_seam_maps_the_forward_leg_onto_the_bottle_line_landmark);
    RUN_TEST(test_the_planned_travel_is_the_rule_course_out_and_back);

    return UNITY_END();
}
