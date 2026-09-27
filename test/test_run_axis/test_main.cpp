#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <RunAxis.h>
#include <RunConfig.h>
#include <Units.h>

// Contract tests for the run axis: the announced geometry mapped from
// RunConfig, the tick-sign polarity applied exactly once, and the two remaining
// distances the run layer consumes.
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
// The turn-around is the Target Point plus the announced Bottle Line Distance.
static_assert(ev::RunAxisGeometry(5.0, 1.0, 2.0).turnAroundPositionM() == 6.0,
              "the turn-around must be the Target Point plus the Bottle Line Distance");
static_assert(ev::RunAxisGeometry(0.0, 0.0, 0.0).turnAroundPositionM() == 0.0,
              "the zero geometry must be exactly the axis origin");

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

// --- The announced geometry --------------------------------------------------

void test_the_announced_configuration_maps_onto_the_run_axis(void) {
    // The Regional fixture announcement: 5.00 m Target Distance, 1.00 m Bottle
    // Line Distance, the Regional 2.0 m bottle placement.
    const ev::RunConfig config(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(config);

    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 5.0);
    TEST_ASSERT_TRUE(geometry.bottleLineDistanceM == 1.0);
    TEST_ASSERT_TRUE(geometry.bottleStartPositionM == 2.0);
    TEST_ASSERT_TRUE(geometry.turnAroundPositionM() == 6.0);
}

void test_a_different_announcement_moves_every_position_with_it(void) {
    // A State-grid announcement: 5.25 m and 1.25 m, with the State 3.0 m bottle
    // placement. The turn-around is 6.50 m, not the 6.00 m of the Regional case.
    const ev::RunConfig config(ev::TournamentLevel::State, 5.25, 10.0, 1.25, 3.0);
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(config);

    TEST_ASSERT_TRUE(geometry.targetPointPositionM == 5.25);
    TEST_ASSERT_TRUE(geometry.bottleLineDistanceM == 1.25);
    TEST_ASSERT_TRUE(geometry.bottleStartPositionM == 3.0);
    TEST_ASSERT_TRUE(geometry.turnAroundPositionM() == 6.5);
}

void test_the_turn_around_is_the_target_plus_the_announced_bottle_line(void) {
    const ev::RunAxisGeometry geometry(7.0, 2.5, 4.0);
    TEST_ASSERT_TRUE(geometry.turnAroundPositionM() == 9.5);
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

// --- Remaining distances ------------------------------------------------------

void test_the_remaining_distance_to_the_target_point_is_signed_by_the_leg(void) {
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(
        ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0));

    // Forward leg: positive while short of the point, zero at it, negative past
    // it - an overshoot is evidence, so it is not clamped away.
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, true, 4.0) == 1.0);
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, true, 5.0) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, true, 5.5) == -0.5);

    // Reverse leg: the same point, approached from the far side.
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, false, 5.5) == 0.5);
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, false, 5.0) == 0.0);
    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, false, 4.5) == -0.5);
}

void test_the_leg_remaining_is_never_below_zero(void) {
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(
        ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0));

    // Forward leg ends at the turn-around, 6.0 m.
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, true, 5.0) == 1.0);
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, true, 6.0) == 0.0);
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, true, 7.0) == 0.0);

    // Reverse leg ends at the Target Point, 5.0 m.
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, false, 6.5) == 1.5);
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, false, 5.0) == 0.0);
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, false, 4.0) == 0.0);
}

void test_a_standstill_at_the_origin_has_the_whole_first_leg_ahead(void) {
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(
        ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0));

    TEST_ASSERT_TRUE(ev::remainingToTargetM(geometry, true, 0.0) == 5.0);
    TEST_ASSERT_TRUE(ev::legRemainingM(geometry, true, 0.0) == 6.0);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_the_announced_configuration_maps_onto_the_run_axis);
    RUN_TEST(test_a_different_announcement_moves_every_position_with_it);
    RUN_TEST(test_the_turn_around_is_the_target_plus_the_announced_bottle_line);

    RUN_TEST(test_a_positive_tick_delta_is_forward_travel_under_a_measured_forward_polarity);
    RUN_TEST(test_a_measured_reverse_polarity_turns_the_tick_sign_around);
    RUN_TEST(test_the_axis_zero_is_the_origin_reading_not_tick_zero);
    RUN_TEST(test_a_standstill_is_exactly_the_axis_origin);

    RUN_TEST(test_an_unmeasured_tick_polarity_refuses_the_position_but_reports_a_diagnostic);
    RUN_TEST(test_a_position_never_survives_an_unusable_scale);
    RUN_TEST(test_the_usable_invariant_holds_across_the_input_combinations);

    RUN_TEST(test_the_remaining_distance_to_the_target_point_is_signed_by_the_leg);
    RUN_TEST(test_the_leg_remaining_is_never_below_zero);
    RUN_TEST(test_a_standstill_at_the_origin_has_the_whole_first_leg_ahead);

    return UNITY_END();
}
