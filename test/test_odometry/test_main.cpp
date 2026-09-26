#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Odometry.h>
#include <Snapshots.h>
#include <Units.h>

// Contract tests for the pure tick -> displacement/position conversion and for
// the sample-age and odometry-usability semantics that a later motion or safety
// layer consumes.
//
// The scales and tick counts below are test fixtures, not calibration values or
// logged counts for any real vehicle. Every conversion here is arithmetic on a
// supplied number: a passing test is not evidence that any encoder produces
// those ticks, that the effective distance per tick is known, or that a real
// run measures the distance this module computes (VERIFIED_STATE.md #4, #5, #6,
// #10 all stay UNVERIFIED).

// Pinned at compile time: an estimate or a readiness verdict must always state
// how it was produced. If either ever gains a default constructor, a caller
// could obtain a valid-looking answer with no reason behind it and this target
// stops compiling.
static_assert(!std::is_default_constructible<ev::OdometryEstimate>::value,
              "an estimate must never default-construct into a valid-looking value");
static_assert(!std::is_default_constructible<ev::OdometryReadiness>::value,
              "a readiness verdict must never default-construct");
// The zero values are pinned so a future reorder cannot silently remap them:
// "usable" and "valid" are the permissive members, so they are the ones that a
// caller must never reach without a check.
static_assert(static_cast<int>(ev::OdometryUsability::Usable) == 0,
              "the usable reason must stay the zero value");
static_assert(static_cast<int>(ev::OdometryEstimateStatus::Valid) == 0,
              "the valid status must stay the zero value");
// A sample age that was never computed must fail closed.
static_assert(!ev::SampleAge{}.fresh, "a default-built sample age must not be fresh");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Comparisons are done with an explicit absolute tolerance rather than a double
// equality assert, so this target does not depend on optional Unity double
// support. Every compared value here is a few metres at most, where 1e-9 m is
// far below any physical meaning and well above double rounding noise.
const double kDistanceToleranceM = 1e-9;

ev::Sourced<double> sourcedScale(double metersPerEncoderTick, ev::ValueSource source) {
    return ev::Sourced<double>(metersPerEncoderTick, source);
}

// Fixture scale: 0.0001 m per tick. A real value can only come from a rollout
// measurement and lives in VehicleCalibration.
ev::Sourced<double> measuredScale(double metersPerEncoderTick) {
    return sourcedScale(metersPerEncoderTick, ev::ValueSource::Measured);
}

// Every estimate a test inspects also has its documented invariant pinned:
// valid == (status == Valid).
void expectEstimate(ev::OdometryEstimateStatus expectedStatus, bool expectedValid,
                    const ev::OdometryEstimate& estimate) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStatus), static_cast<int>(estimate.status));
    TEST_ASSERT_EQUAL_INT(expectedValid ? 1 : 0, estimate.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(estimate.status == ev::OdometryEstimateStatus::Valid ? 1 : 0,
                          estimate.valid ? 1 : 0);
}

void expectDistance(double expectedM, const ev::OdometryEstimate& estimate) {
    TEST_ASSERT_TRUE(std::fabs(estimate.distanceM - expectedM) <= kDistanceToleranceM);
}

// Every readiness verdict a test inspects also has its documented invariant
// pinned: usable == (reason == Usable).
void expectReadiness(ev::OdometryUsability expectedReason, bool expectedUsable,
                     uint32_t expectedAgeMs, const ev::OdometryReadiness& readiness) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedReason), static_cast<int>(readiness.reason));
    TEST_ASSERT_EQUAL_INT(expectedUsable ? 1 : 0, readiness.usable ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(readiness.reason == ev::OdometryUsability::Usable ? 1 : 0,
                          readiness.usable ? 1 : 0);
    TEST_ASSERT_EQUAL_UINT32(expectedAgeMs, readiness.ageMs);
}

}  // namespace

// --- Sign, zero, and what the tick difference means -------------------------

void test_forward_and_reverse_tick_deltas_give_signed_metres(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);

    const ev::OdometryEstimate forward = ev::distanceTravelledM(0, 1000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, forward);
    expectDistance(0.1, forward);

    const ev::OdometryEstimate reverse = ev::distanceTravelledM(0, -1000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, reverse);
    expectDistance(-0.1, reverse);
}

void test_only_the_tick_difference_matters_not_the_absolute_count(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::OdometryEstimate fromZero = ev::distanceTravelledM(0, 1000, scale);

    // The same thousand-tick step from a large origin, and from a negative one.
    const ev::OdometryEstimate fromLargeOrigin = ev::distanceTravelledM(500000, 501000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, fromLargeOrigin);
    expectDistance(fromZero.distanceM, fromLargeOrigin);

    const ev::OdometryEstimate fromNegativeOrigin = ev::distanceTravelledM(-40000, -39000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, fromNegativeOrigin);
    expectDistance(fromZero.distanceM, fromNegativeOrigin);

    // And the step between two large readings is not their sum.
    const ev::OdometryEstimate betweenLarge = ev::distanceTravelledM(-1000000, 1000000, scale);
    expectDistance(200.0, betweenLarge);
}

void test_zero_tick_delta_is_zero_distance_and_still_valid(void) {
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(12345, 12345, measuredScale(0.0001));
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 0.0);
}

// --- Units ------------------------------------------------------------------

void test_the_distance_is_metres_not_millimetres(void) {
    // The scale is metres per tick, so 4000 ticks at 0.0001 m/tick is 0.4 m,
    // which is 400 mm. A millimetre convention would report 400 here.
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 4000, measuredScale(0.0001));
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, estimate);
    expectDistance(0.4, estimate);
    TEST_ASSERT_TRUE(std::fabs(estimate.distanceM - 400.0) > 1.0);
}

void test_scaling_is_linear_in_the_scale(void) {
    const ev::OdometryEstimate single = ev::distanceTravelledM(0, 3000, measuredScale(0.0002));
    expectDistance(0.6, single);

    const ev::OdometryEstimate doubled = ev::distanceTravelledM(0, 3000, measuredScale(0.0004));
    expectDistance(1.2, doubled);
    expectDistance(single.distanceM * 2.0, doubled);
}

// --- Large counts -----------------------------------------------------------

void test_a_delta_spanning_the_whole_int32_range_is_exact(void) {
    // INT32_MIN to INT32_MAX is a 4294967295-tick step. Formed in 32 bits that
    // subtraction is undefined behaviour; the module forms it in 64 bits first.
    const int32_t lowest = std::numeric_limits<int32_t>::min();
    const int32_t highest = std::numeric_limits<int32_t>::max();
    const ev::Sourced<double> scale = measuredScale(1e-9);

    const ev::OdometryEstimate span = ev::distanceTravelledM(lowest, highest, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, span);
    TEST_ASSERT_TRUE(std::fabs(span.distanceM - 4294967295.0 * 1e-9) <= 1e-6);

    const ev::OdometryEstimate back = ev::distanceTravelledM(highest, lowest, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, back);
    TEST_ASSERT_TRUE(std::fabs(back.distanceM + span.distanceM) <= 1e-6);
}

void test_extreme_origins_still_convert_exactly(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const int32_t lowest = std::numeric_limits<int32_t>::min();
    const int32_t highest = std::numeric_limits<int32_t>::max();

    const ev::OdometryEstimate nearLowest = ev::distanceTravelledM(lowest, lowest + 10, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, nearLowest);
    expectDistance(0.001, nearLowest);

    const ev::OdometryEstimate nearHighest = ev::distanceTravelledM(highest, highest - 10, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, nearHighest);
    expectDistance(-0.001, nearHighest);
}

void test_a_large_accumulated_count_converts_without_precision_loss(void) {
    // An accumulated count this large is a long-lived vehicle, not an event run,
    // but it must not quietly lose precision or overflow.
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 2000000000, measuredScale(0.0001));
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, estimate);
    expectDistance(200000.0, estimate);
}

// --- Invalid calibration ----------------------------------------------------

void test_a_zero_scale_cannot_produce_a_distance(void) {
    // A zero scale would multiply out to a plausible-looking zero, so it is
    // refused rather than reported as a valid "no movement" measurement.
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 10000, measuredScale(0.0));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotPositive, false, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 0.0);
}

void test_a_negative_scale_is_refused(void) {
    // A negative scale would secretly invert the tick sign convention, which
    // this module does not own, so it is not accepted as a direction reversed.
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 10000, measuredScale(-0.0001));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotPositive, false, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 0.0);
}

void test_a_non_finite_scale_is_refused(void) {
    const ev::OdometryEstimate nanScale = ev::distanceTravelledM(0, 1000, measuredScale(kNaN));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotFinite, false, nanScale);
    TEST_ASSERT_TRUE(nanScale.distanceM == 0.0);

    const ev::OdometryEstimate infScale = ev::distanceTravelledM(0, 1000, measuredScale(kInf));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotFinite, false, infScale);

    // Negative infinity is both non-finite and non-positive; finiteness is
    // checked first, so the reported reason is the more fundamental one.
    const ev::OdometryEstimate negativeInfScale = ev::distanceTravelledM(0, 1000, measuredScale(-kInf));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotFinite, false, negativeInfScale);
}

void test_provisional_and_rule_derived_scales_are_diagnostic_only(void) {
    // A placeholder still produces a number so a bench log can print it, but the
    // estimate is never valid: nothing unmeasured may reach control.
    const ev::OdometryEstimate provisional =
        ev::distanceTravelledM(0, 1000, sourcedScale(0.0004, ev::ValueSource::Provisional));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotMeasured, false, provisional);
    expectDistance(0.4, provisional);

    const ev::OdometryEstimate ruleDerived =
        ev::distanceTravelledM(0, 1000, sourcedScale(0.0004, ev::ValueSource::RuleDerived));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotMeasured, false, ruleDerived);
    expectDistance(0.4, ruleDerived);
}

void test_a_measured_scale_whose_product_overflows_is_refused(void) {
    // Finite, positive and Measured: it passes every scale check, and the
    // product is still not a number, so it must fail closed instead of
    // reporting an infinity as a distance. 2e9 ticks at 1e300 m/tick is 2e309,
    // above the largest finite double, and 2e9 ticks is reachable from a single
    // int32 reading pair.
    const ev::OdometryEstimate estimate =
        ev::distanceTravelledM(0, 2000000000, measuredScale(1e300));
    expectEstimate(ev::OdometryEstimateStatus::DistanceIsNotFinite, false, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 0.0);
}

void test_the_scale_check_is_reported_on_its_own(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OdometryEstimateStatus::Valid),
                          static_cast<int>(ev::distanceScaleStatus(measuredScale(0.0001))));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::OdometryEstimateStatus::ScaleIsNotMeasured),
        static_cast<int>(
            ev::distanceScaleStatus(sourcedScale(0.0001, ev::ValueSource::Provisional))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OdometryEstimateStatus::ScaleIsNotPositive),
                          static_cast<int>(ev::distanceScaleStatus(measuredScale(0.0))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OdometryEstimateStatus::ScaleIsNotPositive),
                          static_cast<int>(ev::distanceScaleStatus(measuredScale(-0.0001))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OdometryEstimateStatus::ScaleIsNotFinite),
                          static_cast<int>(ev::distanceScaleStatus(measuredScale(kNaN))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OdometryEstimateStatus::ScaleIsNotFinite),
                          static_cast<int>(ev::distanceScaleStatus(measuredScale(kInf))));
}

void test_only_a_measured_finite_positive_scale_yields_a_valid_estimate(void) {
    // Property-style sweep of the cross product of scale numerics and
    // provenance: nothing but Measured + finite + positive may come out valid,
    // and a valid estimate must always carry a finite number.
    const double scaleValues[] = {0.0001, 0.0, -0.0001, kNaN, kInf, 1e300};
    const ev::ValueSource sources[] = {ev::ValueSource::Measured, ev::ValueSource::Provisional,
                                      ev::ValueSource::RuleDerived};

    for (double value : scaleValues) {
        for (ev::ValueSource source : sources) {
            const ev::OdometryEstimate estimate =
                ev::distanceTravelledM(-12345, 54321, sourcedScale(value, source));

            TEST_ASSERT_EQUAL_INT(estimate.status == ev::OdometryEstimateStatus::Valid ? 1 : 0,
                                  estimate.valid ? 1 : 0);
            if (estimate.valid) {
                TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ValueSource::Measured),
                                      static_cast<int>(source));
                TEST_ASSERT_TRUE(std::isfinite(value));
                TEST_ASSERT_TRUE(value > 0.0);
                TEST_ASSERT_TRUE(std::isfinite(estimate.distanceM));
            }
        }
    }
}

// --- Position on the run axis ----------------------------------------------

void test_position_is_the_origin_plus_the_travel(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);

    const ev::OdometryEstimate forward = ev::positionFromOriginM(1.25, 0, 1000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, forward);
    expectDistance(1.35, forward);

    const ev::OdometryEstimate reverse = ev::positionFromOriginM(1.25, 0, -1000, scale);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, reverse);
    expectDistance(1.15, reverse);
}

void test_position_with_a_zero_origin_is_exactly_the_travel(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    const ev::OdometryEstimate travel = ev::distanceTravelledM(2000, 7000, scale);
    const ev::OdometryEstimate position = ev::positionFromOriginM(0.0, 2000, 7000, scale);
    expectDistance(travel.distanceM, position);
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, position);
}

void test_a_standstill_reports_the_origin_position(void) {
    const ev::OdometryEstimate estimate =
        ev::positionFromOriginM(2.5, 4000, 4000, measuredScale(0.0001));
    expectEstimate(ev::OdometryEstimateStatus::Valid, true, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 2.5);
}

void test_a_non_finite_origin_position_is_refused(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    expectEstimate(ev::OdometryEstimateStatus::OriginIsNotFinite, false,
                   ev::positionFromOriginM(kNaN, 0, 1000, scale));
    expectEstimate(ev::OdometryEstimateStatus::OriginIsNotFinite, false,
                   ev::positionFromOriginM(kInf, 0, 1000, scale));
    expectEstimate(ev::OdometryEstimateStatus::OriginIsNotFinite, false,
                   ev::positionFromOriginM(-kInf, 0, 1000, scale));
    TEST_ASSERT_TRUE(ev::positionFromOriginM(kNaN, 0, 1000, scale).distanceM == 0.0);
}

void test_a_position_never_survives_an_unusable_scale(void) {
    const ev::OdometryEstimate diagnostic =
        ev::positionFromOriginM(1.0, 0, 1000, sourcedScale(0.0004, ev::ValueSource::Provisional));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotMeasured, false, diagnostic);
    expectDistance(1.4, diagnostic);

    // With no usable scale there is no travel, so the origin alone must not be
    // reported: it would look like a legitimate "has not moved yet" reading.
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotPositive, false,
                   ev::positionFromOriginM(1.0, 0, 1000, measuredScale(0.0)));
    expectEstimate(ev::OdometryEstimateStatus::ScaleIsNotFinite, false,
                   ev::positionFromOriginM(1.0, 0, 1000, measuredScale(kNaN)));
    TEST_ASSERT_TRUE(ev::positionFromOriginM(1.0, 0, 1000, measuredScale(0.0)).distanceM == 0.0);
}

void test_a_position_that_overflows_the_sum_is_refused(void) {
    // The origin is finite and the travel is finite, but their sum is not.
    const ev::OdometryEstimate estimate =
        ev::positionFromOriginM(1e308, 0, 1000, measuredScale(1e305));
    expectEstimate(ev::OdometryEstimateStatus::DistanceIsNotFinite, false, estimate);
    TEST_ASSERT_TRUE(estimate.distanceM == 0.0);
}

// --- Sample age and freshness ----------------------------------------------

void test_a_sample_is_fresh_up_to_the_limit_inclusive(void) {
    const ev::SampleAge atZeroAge = ev::sampleAge(1000U, 1000U, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, atZeroAge.ageMs);
    TEST_ASSERT_TRUE(atZeroAge.fresh);

    const ev::SampleAge atLimit = ev::sampleAge(1050U, 1000U, 50U);
    TEST_ASSERT_EQUAL_UINT32(50U, atLimit.ageMs);
    TEST_ASSERT_TRUE(atLimit.fresh);

    const ev::SampleAge pastLimit = ev::sampleAge(1051U, 1000U, 50U);
    TEST_ASSERT_EQUAL_UINT32(51U, pastLimit.ageMs);
    TEST_ASSERT_FALSE(pastLimit.fresh);
}

void test_sample_age_is_correct_across_millis_wraparound(void) {
    // The stamp is 6 ms before the uint32 rollover, so by nowMs = 5 the sample
    // is 11 ms old even though the raw stamp is enormous.
    const ev::SampleAge age = ev::sampleAge(5U, 0xFFFFFFFAU, 11U);
    TEST_ASSERT_EQUAL_UINT32(11U, age.ageMs);
    TEST_ASSERT_TRUE(age.fresh);
    TEST_ASSERT_FALSE(ev::sampleAge(5U, 0xFFFFFFFAU, 10U).fresh);
    TEST_ASSERT_FALSE(ev::sampleAge(15U, 0xFFFFFFFAU, 11U).fresh);
}

void test_a_sample_stamped_in_the_future_is_never_fresh(void) {
    const ev::SampleAge age = ev::sampleAge(1000U, 2000U, 1000U);
    TEST_ASSERT_FALSE(age.fresh);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU - 999U, age.ageMs);

    // A generous limit does not rescue it either: the wrapped difference is
    // enormous rather than negative, so the sample reads as ancient.
    TEST_ASSERT_FALSE(ev::sampleAge(1000U, 1001U, 100000U).fresh);
}

void test_a_default_built_sample_age_fails_closed(void) {
    const ev::SampleAge age{};
    TEST_ASSERT_EQUAL_UINT32(0U, age.ageMs);
    TEST_ASSERT_FALSE(age.fresh);
}

// --- Odometry usability -----------------------------------------------------

void test_readiness_requires_a_sample_to_exist(void) {
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0001));
    const ev::SampleAge fresh = ev::sampleAge(1000U, 1000U, 50U);

    const ev::OdometryReadiness readiness = ev::odometryReadiness(false, estimate, fresh);
    expectReadiness(ev::OdometryUsability::NoSample, false, 0U, readiness);
}

void test_readiness_accepts_a_valid_estimate_from_a_fresh_sample(void) {
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0001));
    const ev::SampleAge fresh = ev::sampleAge(1037U, 1000U, 50U);

    const ev::OdometryReadiness readiness = ev::odometryReadiness(true, estimate, fresh);
    expectReadiness(ev::OdometryUsability::Usable, true, 37U, readiness);
}

void test_readiness_refuses_an_invalid_estimate_even_when_the_sample_is_fresh(void) {
    const ev::OdometryEstimate provisional =
        ev::distanceTravelledM(0, 1000, sourcedScale(0.0004, ev::ValueSource::Provisional));
    const ev::SampleAge fresh = ev::sampleAge(1000U, 1000U, 50U);

    expectReadiness(ev::OdometryUsability::EstimateInvalid, false, 0U,
                    ev::odometryReadiness(true, provisional, fresh));

    // A stale provisional estimate reports the correctness problem, not the age
    // problem: estimate validity is checked before age.
    const ev::SampleAge stale = ev::sampleAge(9000U, 1000U, 50U);
    expectReadiness(ev::OdometryUsability::EstimateInvalid, false, 8000U,
                    ev::odometryReadiness(true, provisional, stale));
}

void test_readiness_refuses_a_stale_sample_even_when_the_estimate_is_valid(void) {
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0001));
    const ev::SampleAge stale = ev::sampleAge(9000U, 1000U, 50U);

    expectReadiness(ev::OdometryUsability::SampleStale, false, 8000U,
                    ev::odometryReadiness(true, estimate, stale));
}

void test_readiness_treats_a_wrapped_sample_as_stale(void) {
    // The vehicle layer's freshness verdict is what readiness consumes, so a
    // sample whose age wrapped into staleness stays unusable without this module
    // re-deriving the age itself.
    const ev::OdometryEstimate estimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0001));
    const ev::SampleAge wrapped = ev::sampleAge(5U, 0xFFFFFFFAU, 10U);

    expectReadiness(ev::OdometryUsability::SampleStale, false, 11U,
                    ev::odometryReadiness(true, estimate, wrapped));
}

void test_the_readiness_invariant_holds_for_every_combination(void) {
    const ev::OdometryEstimate validEstimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0001));
    const ev::OdometryEstimate provisionalEstimate =
        ev::distanceTravelledM(0, 1000, sourcedScale(0.0004, ev::ValueSource::Provisional));
    const ev::OdometryEstimate noScaleEstimate = ev::distanceTravelledM(0, 1000, measuredScale(0.0));

    const ev::OdometryEstimate* estimates[] = {&validEstimate, &provisionalEstimate,
                                               &noScaleEstimate};
    const bool sampleFlags[] = {false, true};
    const ev::SampleAge ages[] = {ev::sampleAge(1000U, 1000U, 50U), ev::sampleAge(9000U, 1000U, 50U),
                                  ev::sampleAge(1000U, 2000U, 50U)};

    for (bool hasSample : sampleFlags) {
        for (const ev::OdometryEstimate* estimate : estimates) {
            for (const ev::SampleAge& age : ages) {
                const ev::OdometryReadiness readiness =
                    ev::odometryReadiness(hasSample, *estimate, age);

                TEST_ASSERT_EQUAL_INT(
                    readiness.reason == ev::OdometryUsability::Usable ? 1 : 0,
                    readiness.usable ? 1 : 0);
                if (readiness.usable) {
                    // Usable demands all three facts at once.
                    TEST_ASSERT_TRUE(hasSample);
                    TEST_ASSERT_TRUE(estimate->valid);
                    TEST_ASSERT_TRUE(age.fresh);
                }
            }
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_forward_and_reverse_tick_deltas_give_signed_metres);
    RUN_TEST(test_only_the_tick_difference_matters_not_the_absolute_count);
    RUN_TEST(test_zero_tick_delta_is_zero_distance_and_still_valid);

    RUN_TEST(test_the_distance_is_metres_not_millimetres);
    RUN_TEST(test_scaling_is_linear_in_the_scale);

    RUN_TEST(test_a_delta_spanning_the_whole_int32_range_is_exact);
    RUN_TEST(test_extreme_origins_still_convert_exactly);
    RUN_TEST(test_a_large_accumulated_count_converts_without_precision_loss);

    RUN_TEST(test_a_zero_scale_cannot_produce_a_distance);
    RUN_TEST(test_a_negative_scale_is_refused);
    RUN_TEST(test_a_non_finite_scale_is_refused);
    RUN_TEST(test_provisional_and_rule_derived_scales_are_diagnostic_only);
    RUN_TEST(test_a_measured_scale_whose_product_overflows_is_refused);
    RUN_TEST(test_the_scale_check_is_reported_on_its_own);
    RUN_TEST(test_only_a_measured_finite_positive_scale_yields_a_valid_estimate);

    RUN_TEST(test_position_is_the_origin_plus_the_travel);
    RUN_TEST(test_position_with_a_zero_origin_is_exactly_the_travel);
    RUN_TEST(test_a_standstill_reports_the_origin_position);
    RUN_TEST(test_a_non_finite_origin_position_is_refused);
    RUN_TEST(test_a_position_never_survives_an_unusable_scale);
    RUN_TEST(test_a_position_that_overflows_the_sum_is_refused);

    RUN_TEST(test_a_sample_is_fresh_up_to_the_limit_inclusive);
    RUN_TEST(test_sample_age_is_correct_across_millis_wraparound);
    RUN_TEST(test_a_sample_stamped_in_the_future_is_never_fresh);
    RUN_TEST(test_a_default_built_sample_age_fails_closed);

    RUN_TEST(test_readiness_requires_a_sample_to_exist);
    RUN_TEST(test_readiness_accepts_a_valid_estimate_from_a_fresh_sample);
    RUN_TEST(test_readiness_refuses_an_invalid_estimate_even_when_the_sample_is_fresh);
    RUN_TEST(test_readiness_refuses_a_stale_sample_even_when_the_estimate_is_valid);
    RUN_TEST(test_readiness_treats_a_wrapped_sample_as_stale);
    RUN_TEST(test_the_readiness_invariant_holds_for_every_combination);

    return UNITY_END();
}
