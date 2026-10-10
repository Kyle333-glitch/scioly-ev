#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Units.h>
#include <VelocityEstimator.h>

// Contract tests for the candidate low-speed velocity estimators, driven by
// synthetic sample traces.
//
// Every tick count, sample time and scale below is a test fixture. None is a
// calibration value, a logged trace from a real encoder, or evidence that any
// estimator is good enough to use on the vehicle: no window candidate is
// declared adequate here, and no physical test is implied by a passing case.
// VERIFIED_STATE.md #4 (direction sign), #5 (signal quality), #6 (rollout) and
// #10 (wheel slip) all stay UNVERIFIED.
//
// The traces exist to answer one question per case: is the arithmetic and the
// refusal behavior of this module exactly what it claims to be, for constant
// speed, sparse low-speed ticks, stop/start, reversal, irregular sample spacing
// and edges that only appear in the sample that reports them?

// Pinned at compile time: an estimate must always state how it was produced,
// and the permissive zero values must stay where they are.
static_assert(!std::is_default_constructible<ev::VelocityEstimate>::value,
              "an estimate must never default-construct into a valid-looking value");
static_assert(!std::is_default_constructible<ev::VelocityEstimatorConfig>::value,
              "a configuration must always have to state its window and timeout");
static_assert(!std::is_default_constructible<ev::VelocityReadiness>::value,
              "a readiness verdict must never default-construct without a reason");
static_assert(static_cast<int>(ev::VelocityEstimateStatus::Valid) == 0,
              "the valid status must stay the zero value");
static_assert(static_cast<int>(ev::VelocityUsability::Usable) == 0,
              "the usable verdict must stay the zero value");
// The held defaults in the vehicle layer are built by
// `refusedVelocityEstimate(WarmingUp)`, so the enumerator's value is pinned
// explicitly: a reorder would otherwise silently change what those members mean.
static_assert(static_cast<int>(ev::VelocityEstimateStatus::WarmingUp) == 6,
              "the warm-up status must stay the value the held defaults assume");
// A default-built configuration is {0, 0}, which fails validation: a forgotten
// configuration must produce refusals, never numbers.
static_assert(ev::VelocityEstimatorConfig{0U, 0U}.windowMs == 0U,
              "the zero configuration must be the invalid one");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Speed comparisons use an explicit tolerance rather than double equality, so
// this target does not depend on optional Unity double support. Every compared
// speed here is a few tenths of a metre per second at most, where 1e-12 m/s is
// far below physical meaning and well above double rounding noise.
const double kSpeedToleranceMps = 1e-12;

// Fixture scale: 0.0001 m per tick. A real value can only come from a rollout
// measurement and lives in VehicleCalibration.
ev::Sourced<double> measuredScale(double metersPerEncoderTick) {
    return ev::Sourced<double>(metersPerEncoderTick, ev::ValueSource::Measured);
}

ev::Sourced<double> provisionalScale(double metersPerEncoderTick) {
    return ev::Sourced<double>(metersPerEncoderTick, ev::ValueSource::Provisional);
}

struct TraceSample {
    uint32_t elapsedMs;
    int32_t ticks;
};

// Feeds a whole trace and returns the last estimate. Requires at least one
// sample, which every caller here has by construction.
ev::VelocityEstimate feedAll(ev::VelocityEstimator& estimator, const TraceSample* trace,
                            std::size_t count, const ev::Sourced<double>& scale) {
    ev::VelocityEstimate estimate = estimator.update(trace[0].elapsedMs, trace[0].ticks, scale);
    for (std::size_t i = 1; i < count; ++i) {
        estimate = estimator.update(trace[i].elapsedMs, trace[i].ticks, scale);
    }
    return estimate;
}

// Every estimate a test inspects also has its documented invariants pinned:
// valid == (status == Valid), and the zero-speed claim only ever accompanies
// its own status.
void expectEstimate(ev::VelocityEstimateStatus expectedStatus, bool expectedValid,
                    bool expectedZeroSpeedClaim, const ev::VelocityEstimate& estimate) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedStatus), static_cast<int>(estimate.status));
    TEST_ASSERT_EQUAL_INT(expectedValid ? 1 : 0, estimate.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(estimate.status == ev::VelocityEstimateStatus::Valid ? 1 : 0,
                          estimate.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(expectedZeroSpeedClaim ? 1 : 0, estimate.zeroSpeedClaimed ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(estimate.status == ev::VelocityEstimateStatus::NoEdgeTimeout ? 1 : 0,
                          estimate.zeroSpeedClaimed ? 1 : 0);
}

void expectSpeed(double expectedMps, const ev::VelocityEstimate& estimate) {
    TEST_ASSERT_TRUE(std::fabs(estimate.velocityMps - expectedMps) <= kSpeedToleranceMps);
}

// A number is only ever reported from whole ticks over the covered span, times
// the supplied scale. This is the arithmetic contract: no filtering, no
// smoothing, no interpolation.
void expectExactTickArithmetic(const ev::VelocityEstimate& estimate,
                               const ev::Sourced<double>& scale) {
    const double expectedMps = static_cast<double>(estimate.effectiveWindowTicks) * scale.value /
                               (static_cast<double>(estimate.effectiveWindowMs) / 1000.0);
    TEST_ASSERT_TRUE(std::fabs(estimate.velocityMps - expectedMps) <= kSpeedToleranceMps);
}

}  // namespace

// --- Configuration ----------------------------------------------------------

void test_configuration_validation_rejects_the_zero_values(void) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityEstimatorConfigError::WindowNotPositive),
        static_cast<int>(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{0U, 50U}).error));
    TEST_ASSERT_FALSE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{0U, 50U}).ok);

    // A zero no-edge timeout would make every sample a zero-speed claim,
    // including the first one, so it is refused rather than interpreted.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityEstimatorConfigError::NoEdgeTimeoutNotPositive),
        static_cast<int>(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{20U, 0U}).error));
    TEST_ASSERT_FALSE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{20U, 0U}).ok);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityEstimatorConfigError::None),
        static_cast<int>(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{20U, 100U}).error));
    TEST_ASSERT_TRUE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{20U, 100U}).ok);
}

void test_configuration_validation_requires_the_window_below_the_timeout(void) {
    // S18A/P5: the claim is checked before window coverage in `update()`, so
    // at rest a window at or above the no-edge timeout fires the claim before
    // a window-derived zero can exist - leaving no retainable speed fact for
    // stop composition. The equality boundary is refused, not rounded.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::VelocityEstimatorConfigError::WindowNotBelowNoEdgeTimeout),
        static_cast<int>(
            ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{100U, 100U}).error));
    TEST_ASSERT_FALSE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{100U, 100U}).ok);
    TEST_ASSERT_FALSE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{200U, 100U}).ok);
    TEST_ASSERT_TRUE(ev::validateVelocityEstimatorConfig(ev::VelocityEstimatorConfig{99U, 100U}).ok);
    TEST_ASSERT_TRUE(
        ev::VelocityEstimator(ev::VelocityEstimatorConfig{20U, 100U}).configValid());
    TEST_ASSERT_FALSE(
        ev::VelocityEstimator(ev::VelocityEstimatorConfig{100U, 100U}).configValid());
}

void test_the_window_must_be_coverable_at_the_control_cadence(void) {
    // S18A/P5: sixteen retained samples at a 20 ms control period can cover at
    // most 300 ms. A wider window reports WindowExceedsHistory instead of a
    // number for as long as the cadence holds, so the combination is refused
    // at build time rather than left to time out.
    TEST_ASSERT_TRUE(ev::estimatorWindowCoverableAtCadence(20U, 20U));
    TEST_ASSERT_TRUE(ev::estimatorWindowCoverableAtCadence(300U, 20U));
    TEST_ASSERT_FALSE(ev::estimatorWindowCoverableAtCadence(301U, 20U));
    TEST_ASSERT_FALSE(ev::estimatorWindowCoverableAtCadence(80U, 5U));
    TEST_ASSERT_TRUE(ev::estimatorWindowCoverableAtCadence(75U, 5U));
    TEST_ASSERT_FALSE(ev::estimatorWindowCoverableAtCadence(20U, 0U));
}

void test_an_invalid_configuration_never_produces_a_number(void) {
    const TraceSample trace[] = {{0U, 0}, {10U, 5}, {20U, 10}, {30U, 15}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{0U, 100U});
    TEST_ASSERT_FALSE(estimator.configValid());

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 4, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::ConfigIsNotValid, false, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 0);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
}

// --- Window mechanics -------------------------------------------------------

void test_constant_speed_gives_the_windowed_speed(void) {
    // 5 ticks every 10 ms is 500 ticks/s. At 0.0001 m/tick that is 0.05 m/s.
    const TraceSample trace[] = {{0U, 0},   {10U, 5},  {20U, 10}, {30U, 15},
                                 {40U, 20}, {50U, 25}, {60U, 30}, {70U, 35}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{40U, 5000U});

    // The first samples only fill the window.
    for (std::size_t i = 0; i < 4; ++i) {
        const ev::VelocityEstimate warming =
            estimator.update(trace[i].elapsedMs, trace[i].ticks, measuredScale(0.0001));
        expectEstimate(ev::VelocityEstimateStatus::WarmingUp, false, false, warming);
        TEST_ASSERT_EQUAL_UINT32(0U, warming.effectiveWindowMs);
    }

    ev::VelocityEstimate estimate = feedAll(estimator, trace + 4, 1, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    expectSpeed(0.05, estimate);
    TEST_ASSERT_EQUAL_UINT32(40U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 20);
    expectExactTickArithmetic(estimate, measuredScale(0.0001));

    // Once the window is full it slides at the requested span, not a growing
    // one: every later estimate still covers exactly the configured span here.
    for (std::size_t i = 5; i < 8; ++i) {
        estimate = estimator.update(trace[i].elapsedMs, trace[i].ticks, measuredScale(0.0001));
        expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
        expectSpeed(0.05, estimate);
        TEST_ASSERT_EQUAL_UINT32(40U, estimate.effectiveWindowMs);
        TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 20);
    }
}

void test_the_realized_window_is_reported_when_samples_are_sparse(void) {
    // Samples every 25 ms cannot produce exactly a 20 ms window: the shortest
    // covering span is 25 ms. The estimate reports the span it really used
    // rather than pretending it was 20 ms.
    const TraceSample trace[] = {{0U, 0}, {25U, 3}, {50U, 6}, {75U, 9}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 4, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(25U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 3);
    // 3 ticks in 25 ms at 0.0001 m/tick is 0.012 m/s, not the 0.015 m/s a
    // 20 ms window would have produced.
    expectSpeed(0.012, estimate);
    expectExactTickArithmetic(estimate, measuredScale(0.0001));
}

void test_a_partial_span_never_produces_a_number(void) {
    const TraceSample trace[] = {{0U, 0}, {20U, 20}, {40U, 40}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{80U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 3, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::WarmingUp, false, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
}

void test_an_update_rate_too_fast_for_the_window_is_refused(void) {
    // 1 ms samples with an 80 ms window need 80 samples of history and only 16
    // are retained. The estimator says so instead of silently reporting a
    // shorter window.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{80U, 100000U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (int32_t i = 1; i <= 20; ++i) {
        estimate = estimator.update(static_cast<uint32_t>(i), i, measuredScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::WindowExceedsHistory, false, false, estimate);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ev::VelocityEstimator::kMaxRetainedSamples),
                            estimator.retainedSamples());
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
}

void test_the_too_fast_rate_refusal_is_not_latched(void) {
    // The refusal is a property of the retained history, not a state the object
    // enters: once the samples are spaced widely enough for the history to cover
    // the window, the estimator starts producing numbers again by itself.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{80U, 100000U});

    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (uint32_t ms = 1U; ms <= 40U; ++ms) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms), measuredScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::WindowExceedsHistory, false, false, estimate);

    // Same estimator, 20 ms samples now. Each one lengthens the retained span
    // until it covers the window again.
    bool recovered = false;
    for (uint32_t ms = 60U; ms <= 200U; ms += 20U) {
        const int32_t ticks = static_cast<int32_t>(1000U + (ms - 60U));
        estimate = estimator.update(ms, ticks, measuredScale(0.0001));
        if (estimate.status == ev::VelocityEstimateStatus::Valid) {
            recovered = true;
            TEST_ASSERT_TRUE(estimate.effectiveWindowMs >= 80U);
            TEST_ASSERT_TRUE(estimate.effectiveWindowTicks > 0);
            expectExactTickArithmetic(estimate, measuredScale(0.0001));
            break;
        }
        TEST_ASSERT_TRUE(estimate.status == ev::VelocityEstimateStatus::WindowExceedsHistory ||
                         estimate.status == ev::VelocityEstimateStatus::WarmingUp);
    }
    TEST_ASSERT_TRUE(recovered);
}

void test_zero_ticks_in_a_covered_window_is_a_valid_zero(void) {
    // No encoder change, but a full window of samples: that is a measured zero
    // over the window, and it is reported as valid rather than as a claim. The
    // explicit claim needs its own, longer horizon.
    const TraceSample trace[] = {{0U, 7}, {20U, 7}, {40U, 7}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 500U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 3, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(20U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 0);
}

void test_a_reversal_inside_the_window_reports_the_net_tick_change(void) {
    // Forward 100 ticks then back 100 ticks inside one window is a net zero.
    // The estimator reports the net, not the path: a sign convention it does not
    // own cannot be recovered from a net difference, and nothing here pretends
    // otherwise.
    const TraceSample trace[] = {{0U, 0}, {10U, 100}, {20U, 0}, {30U, 0}};
    // The window has to be at least as long as the round trip, otherwise the
    // sliding window has already dropped the starting sample and reports the
    // net over a shorter span.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{30U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 4, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(30U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 0);
}

void test_falling_ticks_are_a_negative_speed_with_no_direction_claim(void) {
    // The sign is the decoder's counting convention and nothing more: see the
    // module note and VERIFIED_STATE.md #4. A negative number here is not a
    // statement that the vehicle moved backwards.
    const TraceSample trace[] = {{0U, 400}, {50U, 300}, {100U, 200}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{50U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 3, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == -100);
    expectSpeed(-0.2, estimate);
}

void test_a_full_int32_tick_difference_inside_one_window_is_exact(void) {
    // Formed in 32 bits this difference is undefined behaviour; the module forms
    // it in 64 bits first, exactly as the odometry module does.
    const TraceSample trace[] = {{0U, std::numeric_limits<int32_t>::min()},
                                 {50U, std::numeric_limits<int32_t>::max()}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{50U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 2, measuredScale(1e-9));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 4294967295LL);
    expectSpeed(4294967295.0 * 1e-9 / (50.0 / 1000.0), estimate);
}

void test_an_irregular_sample_spacing_is_measured_not_assumed(void) {
    // 1 tick per millisecond is 1000 ticks/s, i.e. 0.1 m/s. The samples are
    // unevenly spaced and the last one jumps 51 ms; the window uses the real
    // span it retained, so the realized span is reported as 51 ms.
    const TraceSample trace[] = {{0U, 0}, {7U, 7}, {23U, 23}, {40U, 40}, {91U, 91}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{40U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 5, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(51U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 51);
    expectSpeed(0.1, estimate);
    expectExactTickArithmetic(estimate, measuredScale(0.0001));
}

// --- The explicit zero-speed claim ------------------------------------------

void test_a_silent_encoder_earns_the_claim_only_after_the_timeout(void) {
    // Samples every 10 ms, no tick change at all, window 20 ms, timeout 100 ms.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});

    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (uint32_t ms = 10U; ms <= 90U; ms += 10U) {
        estimate = estimator.update(ms, 0, measuredScale(0.0001));
    }
    // A covered window with no ticks is a valid zero, and the claim has not
    // been earned yet: the two statements are deliberately different.
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);

    estimate = estimator.update(100U, 0, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 0);
}

void test_the_claim_is_not_made_one_millisecond_early(void) {
    // The same silence as above, but the last sample lands at 99 ms. The claim
    // is withheld for that millisecond and the window-derived zero is reported
    // instead: exactly at the timeout, not before.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});

    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (uint32_t ms = 10U; ms <= 90U; ms += 10U) {
        estimate = estimator.update(ms, 0, measuredScale(0.0001));
    }
    estimate = estimator.update(99U, 0, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
}

void test_an_encoder_change_restarts_the_claim_clock(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});

    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (uint32_t ms = 10U; ms <= 100U; ms += 10U) {
        estimate = estimator.update(ms, 0, measuredScale(0.0001));
    }
    // At 100 ms the claim is earned...
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);

    // ...and one encoder change clears it, because the clock starts again from
    // the sample that observed the change.
    estimate = estimator.update(110U, 4, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_FALSE(estimate.zeroSpeedClaimed);

    // 100 ms after that change the claim is earned again.
    for (uint32_t ms = 120U; ms < 210U; ms += 10U) {
        estimate = estimator.update(ms, 4, measuredScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    estimate = estimator.update(210U, 4, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);
}

void test_a_claim_is_never_made_on_an_unusable_scale(void) {
    // The claim is control-relevant, so it never rests on a placeholder or a
    // broken number, however long the silence lasts.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 50U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, provisionalScale(0.0001));
    for (uint32_t ms = 10U; ms <= 200U; ms += 10U) {
        estimate = estimator.update(ms, 0, provisionalScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotMeasured, false, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);

    for (uint32_t ms = 10U; ms <= 200U; ms += 10U) {
        estimate = estimator.update(ms, 0, measuredScale(0.0));
    }
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotPositive, false, false, estimate);

    ev::VelocityEstimator invalidConfigEstimator(ev::VelocityEstimatorConfig{0U, 50U});
    for (uint32_t ms = 0U; ms <= 200U; ms += 10U) {
        estimate = invalidConfigEstimator.update(ms, 0, measuredScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::ConfigIsNotValid, false, false, estimate);
}

// --- Sample stamps ----------------------------------------------------------

void test_a_repeated_stamp_is_refused_and_never_enters_the_window(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    estimator.update(0U, 0, measuredScale(0.0001));
    estimator.update(20U, 10, measuredScale(0.0001));

    // The same stamp with different ticks: two samples at one instant is not a
    // measurement of anything, and it must not enter the window.
    const ev::VelocityEstimate repeated = estimator.update(20U, 25, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::TimeDidNotAdvance, false, false, repeated);
    TEST_ASSERT_TRUE(repeated.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(1U, estimator.rejectedStamps());
    TEST_ASSERT_EQUAL_UINT8(2U, estimator.retainedSamples());

    // The window is unaffected: it still spans the two accepted samples, and
    // the 10 ticks between them over 20 ms are 0.05 m/s.
    const ev::VelocityEstimate after = estimator.update(40U, 20, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, after);
    TEST_ASSERT_EQUAL_UINT32(20U, after.effectiveWindowMs);
    TEST_ASSERT_TRUE(after.effectiveWindowTicks == 10);
    expectSpeed(10.0 * 0.0001 / 0.02, after);
}

void test_a_rewound_stamp_is_refused(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    estimator.update(0U, 0, measuredScale(0.0001));
    estimator.update(20U, 10, measuredScale(0.0001));

    const ev::VelocityEstimate rewound = estimator.update(10U, 12, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::TimeDidNotAdvance, false, false, rewound);
    TEST_ASSERT_EQUAL_UINT32(1U, estimator.rejectedStamps());

    // And the refused stamp did not become the time base for the next one.
    const ev::VelocityEstimate after = estimator.update(40U, 20, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, after);
    TEST_ASSERT_EQUAL_UINT32(20U, after.effectiveWindowMs);
}

void test_the_window_works_across_millis_wraparound(void) {
    // The first stamp is 6 ms before the uint32 rollover, so the second sample
    // is 11 ms later even though its raw stamp is tiny.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{10U, 5000U});
    const uint32_t beforeWrapMs = 0xFFFFFFFAU;
    estimator.update(beforeWrapMs, 0, measuredScale(0.0001));

    const ev::VelocityEstimate estimate = estimator.update(5U, 10, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(11U, estimate.effectiveWindowMs);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 10);
    expectSpeed(10.0 * 0.0001 / 0.011, estimate);
}

// --- Scale rules ------------------------------------------------------------

void test_only_a_measured_scale_can_produce_a_valid_speed(void) {
    const TraceSample trace[] = {{0U, 0}, {50U, 50}, {100U, 100}};

    ev::VelocityEstimator provisional(ev::VelocityEstimatorConfig{50U, 5000U});
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotMeasured, false, false,
                   feedAll(provisional, trace, 3, provisionalScale(0.0001)));
    TEST_ASSERT_TRUE(provisional.update(150U, 150, provisionalScale(0.0001)).velocityMps == 0.0);

    ev::VelocityEstimator ruleDerived(ev::VelocityEstimatorConfig{50U, 5000U});
    const ev::Sourced<double> ruleScale(0.0001, ev::ValueSource::RuleDerived);
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotMeasured, false, false,
                   feedAll(ruleDerived, trace, 3, ruleScale));
}

void test_a_zero_or_negative_scale_is_refused_with_no_number(void) {
    const TraceSample trace[] = {{0U, 0}, {50U, 50}, {100U, 100}};

    ev::VelocityEstimator zero(ev::VelocityEstimatorConfig{50U, 5000U});
    const ev::VelocityEstimate zeroEstimate = feedAll(zero, trace, 3, measuredScale(0.0));
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotPositive, false, false, zeroEstimate);
    TEST_ASSERT_TRUE(zeroEstimate.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, zeroEstimate.effectiveWindowMs);

    ev::VelocityEstimator negative(ev::VelocityEstimatorConfig{50U, 5000U});
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotPositive, false, false,
                   feedAll(negative, trace, 3, measuredScale(-0.0001)));
}

void test_a_non_finite_scale_is_refused(void) {
    const TraceSample trace[] = {{0U, 0}, {50U, 50}, {100U, 100}};

    ev::VelocityEstimator nanScale(ev::VelocityEstimatorConfig{50U, 5000U});
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotFinite, false, false,
                   feedAll(nanScale, trace, 3, measuredScale(kNaN)));

    ev::VelocityEstimator infiniteScale(ev::VelocityEstimatorConfig{50U, 5000U});
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotFinite, false, false,
                   feedAll(infiniteScale, trace, 3, measuredScale(kInf)));
}

void test_a_measured_scale_whose_speed_overflows_is_refused(void) {
    // Finite, positive and measured: it passes every scale check, and the
    // window product is still not a number, so it must fail closed rather than
    // report an infinity as a speed.
    const TraceSample trace[] = {{0U, std::numeric_limits<int32_t>::min()},
                                 {50U, std::numeric_limits<int32_t>::max()}};
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{50U, 5000U});

    const ev::VelocityEstimate estimate = feedAll(estimator, trace, 2, measuredScale(1e300));
    expectEstimate(ev::VelocityEstimateStatus::SpeedIsNotFinite, false, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);
}

void test_the_scale_verdict_matches_the_odometry_scale_rule(void) {
    // The scale rule has one implementation, in Odometry.h; this checks the two
    // modules cannot drift apart silently.
    const TraceSample trace[] = {{0U, 0}, {50U, 50}, {100U, 100}};
    const ev::Sourced<double> scales[] = {measuredScale(0.0001), provisionalScale(0.0001),
                                          measuredScale(0.0), measuredScale(-0.0001),
                                          measuredScale(kNaN), measuredScale(kInf)};

    for (const ev::Sourced<double>& scale : scales) {
        ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{50U, 5000U});
        const ev::VelocityEstimate estimate = feedAll(estimator, trace, 3, scale);
        const bool scaleUsable =
            ev::distanceScaleStatus(scale) == ev::OdometryEstimateStatus::Valid;
        TEST_ASSERT_EQUAL_INT(scaleUsable ? 1 : 0, estimate.valid ? 1 : 0);
        if (scaleUsable) {
            expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
        } else {
            TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
            TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);
        }
    }
}

// --- Traces that look like a vehicle ----------------------------------------

void test_candidate_windows_can_be_compared_without_duplicating_an_algorithm(void) {
    // One trace, four configurations of the same module. At a constant 0.05 m/s
    // every window agrees on the speed while reporting its own realized window,
    // which is the property that makes the candidates comparable at all.
    const uint32_t windowsMs[] = {20U, 40U, 60U, 80U};
    for (uint32_t windowMs : windowsMs) {
        ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{windowMs, 5000U});
        ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
        for (uint32_t ms = 10U; ms <= 200U; ms += 10U) {
            estimate = estimator.update(ms, static_cast<int32_t>(ms / 2), measuredScale(0.0001));
        }
        expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
        expectSpeed(0.05, estimate);
        TEST_ASSERT_EQUAL_UINT32(windowMs, estimate.effectiveWindowMs);
        expectExactTickArithmetic(estimate, measuredScale(0.0001));
    }
}

void test_sparse_low_speed_ticks_quantize_differently_per_window(void) {
    // One tick every 40 ms is 0.0025 m/s at the fixture scale. A 20 ms window
    // contains either no tick or one tick, so it alternates between 0.0 and
    // 0.005 m/s and never reports the true value. A 40 ms window over this
    // perfectly regular 10 ms sampling keeps a span of exactly one tick spacing
    // and reports 0.0025 m/s every time. Both are valid readings of the same
    // trace, which is the point: the window changes what the number means, and
    // this fixture's regularity is not how a real encoder behaves. Which window
    // is usable is a question for a measured trace from a real encoder, and no
    // candidate is declared adequate here.
    const ev::Sourced<double> scale = measuredScale(0.0001);

    ev::VelocityEstimator shortWindow(ev::VelocityEstimatorConfig{20U, 100000U});
    ev::VelocityEstimator longWindow(ev::VelocityEstimatorConfig{40U, 100000U});

    bool shortSawZero = false;
    bool shortSawOneTick = false;
    int32_t longReadingsInTicks = 0;

    for (uint32_t ms = 0U; ms <= 400U; ms += 10U) {
        const int32_t ticks = static_cast<int32_t>(ms / 40U);
        const ev::VelocityEstimate shortEstimate = shortWindow.update(ms, ticks, scale);
        const ev::VelocityEstimate longEstimate = longWindow.update(ms, ticks, scale);

        // The first samples only fill each window, so this trace is inspected
        // once both are covered (the long one at 40 ms).
        if (ms < 50U) {
            continue;
        }
        expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, shortEstimate);
        expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, longEstimate);
        expectExactTickArithmetic(shortEstimate, scale);
        expectExactTickArithmetic(longEstimate, scale);

        // No negative reading: the ticks never fall.
        TEST_ASSERT_TRUE(shortEstimate.velocityMps >= 0.0);
        TEST_ASSERT_TRUE(longEstimate.velocityMps >= 0.0);

        if (shortEstimate.effectiveWindowTicks == 0) {
            shortSawZero = true;
        } else {
            shortSawOneTick = true;
            // A 20 ms window can hold at most one tick spaced 40 ms apart.
            TEST_ASSERT_TRUE(shortEstimate.effectiveWindowTicks == 1);
        }
        // The 40 ms window keeps a span of exactly one tick spacing here, so it
        // contains exactly one arrival every time and reports the true speed
        // without the alternation the short window shows.
        TEST_ASSERT_EQUAL_UINT32(40U, longEstimate.effectiveWindowMs);
        TEST_ASSERT_TRUE(longEstimate.effectiveWindowTicks == 1);
        longReadingsInTicks += longEstimate.effectiveWindowTicks;
    }

    TEST_ASSERT_TRUE(shortSawZero);
    TEST_ASSERT_TRUE(shortSawOneTick);
    TEST_ASSERT_TRUE(longReadingsInTicks > 0);
}

void test_a_stop_is_visible_in_the_window_before_the_claim_arrives(void) {
    // Moving, then stopped at 100 ms. The window-derived speed reaches zero one
    // window later; the explicit claim only arrives one timeout after the last
    // observed tick change. Both transitions are pinned.
    const ev::Sourced<double> scale = measuredScale(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, scale);

    for (uint32_t ms = 10U; ms <= 90U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms), scale);
    }
    // Still moving at 90 ms: the count rose 10 ticks per sample, so the window
    // covers 20 ticks in 20 ms, which is 0.1 m/s.
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    expectSpeed(0.1, estimate);

    const int32_t finalTicks = 90;
    estimate = estimator.update(100U, finalTicks, scale);
    // The count stopped rising between 90 ms and 100 ms, so the window now
    // spans 10 ticks in 20 ms and reports 0.05 m/s: the stop is partly visible,
    // not yet fully.
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    expectSpeed(0.05, estimate);
    TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 10);

    // Two sample periods later the window holds only stopped samples: a valid
    // zero over the window, with the explicit claim still not earned.
    for (uint32_t ms = 110U; ms <= 120U; ms += 10U) {
        estimate = estimator.update(ms, finalTicks, scale);
    }
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);

    // 100 ms after the last observed change of the count, the claim is earned.
    estimate = estimator.update(190U, finalTicks, scale);
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);
}

void test_an_edge_is_only_visible_in_the_sample_that_reports_it(void) {
    // The count changes in the sample stamped 80 ms. The estimator cannot know
    // when the edge physically happened between samples, so it attributes the
    // change to the stamp it was handed, and the claim clock runs from there.
    const ev::Sourced<double> scale = measuredScale(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{40U, 100U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, scale);
    for (uint32_t ms = 20U; ms <= 60U; ms += 20U) {
        estimate = estimator.update(ms, 0, scale);
    }
    estimate = estimator.update(80U, 5, scale);
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    expectSpeed(5.0 * 0.0001 / (40.0 / 1000.0), estimate);

    // 99 ms after the observing sample the claim is still withheld; only the
    // window remains, and it now holds no change at all.
    estimate = estimator.update(179U, 5, scale);
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);

    // 100 ms after it, the claim is earned.
    estimate = estimator.update(180U, 5, scale);
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);
}

// --- Counters and reset -----------------------------------------------------

void test_counters_report_what_the_estimator_did(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    estimator.update(0U, 0, measuredScale(0.0001));    // accepted, reference
    estimator.update(10U, 0, measuredScale(0.0001));   // accepted, no change
    estimator.update(20U, 3, measuredScale(0.0001));   // accepted, change
    estimator.update(20U, 4, measuredScale(0.0001));   // rejected stamp
    estimator.update(30U, 3, measuredScale(0.0001));   // accepted, same as the last accepted

    TEST_ASSERT_EQUAL_UINT32(4U, estimator.acceptedSamples());
    TEST_ASSERT_EQUAL_UINT32(1U, estimator.rejectedStamps());
    // Only one change: a rejected sample is not data, so the count of 4 it
    // carried was never part of the tick reference, and the accepted sample at
    // 30 ms repeats the last accepted count rather than returning to it.
    TEST_ASSERT_EQUAL_UINT32(1U, estimator.tickChanges());
}

void test_reset_discards_the_window_the_claim_and_the_counters(void) {
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 50U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, measuredScale(0.0001));
    for (uint32_t ms = 10U; ms <= 100U; ms += 10U) {
        estimate = estimator.update(ms, 0, measuredScale(0.0001));
    }
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);

    estimator.reset();
    TEST_ASSERT_EQUAL_UINT32(0U, estimator.acceptedSamples());
    TEST_ASSERT_EQUAL_UINT32(0U, estimator.rejectedStamps());
    TEST_ASSERT_EQUAL_UINT32(0U, estimator.tickChanges());
    TEST_ASSERT_EQUAL_UINT8(0U, estimator.retainedSamples());

    // Nothing resumes automatically: the next sample is a warm-up again, not a
    // resumed estimate or a resumed claim.
    estimate = estimator.update(0U, 0, measuredScale(0.0001));
    expectEstimate(ev::VelocityEstimateStatus::WarmingUp, false, false, estimate);
}

// --- Freshness ownership ----------------------------------------------------

void test_a_number_and_a_claim_carry_the_stamp_that_produced_them(void) {
    // A consumer cannot age an answer without knowing when it was produced. An
    // answer that carries a fact - a number, or the explicit zero-speed claim -
    // carries the update() stamp that produced it; a refusal carries none,
    // because there is nothing to age.
    const ev::Sourced<double> scale = measuredScale(0.0001);

    ev::VelocityEstimator moving(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityEstimate estimate = moving.update(0U, 0, scale);
    expectEstimate(ev::VelocityEstimateStatus::WarmingUp, false, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(0U, estimate.elapsedMs);
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = moving.update(ms, static_cast<int32_t>(ms * 10U), scale);
    }
    expectEstimate(ev::VelocityEstimateStatus::Valid, true, false, estimate);
    TEST_ASSERT_EQUAL_UINT32(60U, estimate.elapsedMs);

    ev::VelocityEstimator stopped(ev::VelocityEstimatorConfig{20U, 50U});
    estimate = stopped.update(0U, 0, scale);
    for (uint32_t ms = 10U; ms <= 50U; ms += 10U) {
        estimate = stopped.update(ms, 0, scale);
    }
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, estimate);
    TEST_ASSERT_EQUAL_UINT32(50U, estimate.elapsedMs);
}

void test_readiness_is_the_one_velocity_freshness_verdict(void) {
    const ev::Sourced<double> scale = measuredScale(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    ev::VelocityEstimate estimate = estimator.update(0U, 0, scale);
    for (uint32_t ms = 10U; ms <= 60U; ms += 10U) {
        estimate = estimator.update(ms, static_cast<int32_t>(ms * 10U), scale);
    }
    TEST_ASSERT_TRUE(estimate.valid);

    // Within the caller's limit the estimate is usable, and the age is reported.
    const ev::VelocityReadiness fresh =
        ev::velocityReadiness(true, estimate, ev::sampleAge(80U, estimate.elapsedMs, 50U));
    TEST_ASSERT_TRUE(fresh.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::Usable),
                          static_cast<int>(fresh.reason));
    TEST_ASSERT_EQUAL_UINT32(20U, fresh.ageMs);

    // The same estimate reused 240 ms later is stale, not silently trusted.
    const ev::VelocityReadiness stale =
        ev::velocityReadiness(true, estimate, ev::sampleAge(300U, estimate.elapsedMs, 50U));
    TEST_ASSERT_FALSE(stale.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::SampleStale),
                          static_cast<int>(stale.reason));
    TEST_ASSERT_EQUAL_UINT32(240U, stale.ageMs);

    // No estimate at all: there is nothing to judge, and the age is not read.
    const ev::VelocityReadiness none =
        ev::velocityReadiness(false, estimate, ev::sampleAge(300U, 0U, 50U));
    TEST_ASSERT_FALSE(none.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::NoEstimate),
                          static_cast<int>(none.reason));
    TEST_ASSERT_EQUAL_UINT32(0U, none.ageMs);

    // A stamp from the future is never fresh (unsigned subtraction), matching
    // sampleAge()'s contract on the position path.
    const ev::VelocityReadiness future =
        ev::velocityReadiness(true, estimate, ev::sampleAge(10U, estimate.elapsedMs, 50U));
    TEST_ASSERT_FALSE(future.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::SampleStale),
                          static_cast<int>(future.reason));
}

void test_readiness_reports_validity_before_age(void) {
    // Check order matches odometryReadiness(): an invalid answer is a
    // correctness problem, not a timing problem, so it reports EstimateInvalid
    // even when its (absent) stamp would look fresh.
    const ev::Sourced<double> scale = measuredScale(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    const ev::VelocityEstimate refused = estimator.update(0U, 0, scale);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityEstimateStatus::WarmingUp),
                          static_cast<int>(refused.status));
    TEST_ASSERT_EQUAL_UINT32(0U, refused.elapsedMs);

    const ev::VelocityReadiness readiness =
        ev::velocityReadiness(true, refused, ev::sampleAge(20U, refused.elapsedMs, 50U));
    TEST_ASSERT_FALSE(readiness.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::EstimateInvalid),
                          static_cast<int>(readiness.reason));
    TEST_ASSERT_EQUAL_UINT32(20U, readiness.ageMs);
}

void test_a_zero_speed_claim_is_never_a_usable_velocity(void) {
    // The claim is an explicit statement about observed non-change, never a
    // measurement of zero speed, so it can never satisfy a freshness verdict on
    // its own and can never become SafetySystemState::atZeroSpeed by itself
    // (SOFTWARE_ARCHITECTURE.md section 13).
    const ev::Sourced<double> scale = measuredScale(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 50U});
    ev::VelocityEstimate claim = estimator.update(0U, 0, scale);
    for (uint32_t ms = 10U; ms <= 50U; ms += 10U) {
        claim = estimator.update(ms, 0, scale);
    }
    expectEstimate(ev::VelocityEstimateStatus::NoEdgeTimeout, false, true, claim);

    const ev::VelocityReadiness readiness =
        ev::velocityReadiness(true, claim, ev::sampleAge(50U, claim.elapsedMs, 50U));
    TEST_ASSERT_FALSE(readiness.usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityUsability::EstimateInvalid),
                          static_cast<int>(readiness.reason));
    TEST_ASSERT_TRUE(claim.velocityMps == 0.0);
    TEST_ASSERT_FALSE(claim.valid);
}

// --- Property sweep ---------------------------------------------------------

void test_estimator_invariants_hold_across_a_trace_sweep(void) {
    const ev::Sourced<double> scales[] = {measuredScale(0.0001), measuredScale(1e300),
                                          provisionalScale(0.0001), measuredScale(0.0),
                                          measuredScale(kNaN)};
    const uint32_t windowsMs[] = {20U, 40U, 80U};
    const uint32_t timeoutsMs[] = {30U, 300U};

    for (uint32_t windowMs : windowsMs) {
        for (uint32_t noEdgeTimeoutMs : timeoutsMs) {
            for (const ev::Sourced<double>& scale : scales) {
                ev::VelocityEstimator estimator(
                    ev::VelocityEstimatorConfig{windowMs, noEdgeTimeoutMs});
                // A trace with motion, a stop, a reversal and irregular spacing.
                const TraceSample trace[] = {{0U, 0},    {11U, 20},  {25U, 45}, {37U, 45},
                                             {52U, 30},  {68U, 10},  {83U, 0},  {95U, 0},
                                             {140U, 0},  {200U, -15}};
                for (const TraceSample& sample : trace) {
                    const ev::VelocityEstimate estimate =
                        estimator.update(sample.elapsedMs, sample.ticks, scale);
                    // Invariants, every cycle.
                    TEST_ASSERT_EQUAL_INT(
                        estimate.status == ev::VelocityEstimateStatus::Valid ? 1 : 0,
                        estimate.valid ? 1 : 0);
                    TEST_ASSERT_EQUAL_INT(
                        estimate.status == ev::VelocityEstimateStatus::NoEdgeTimeout ? 1 : 0,
                        estimate.zeroSpeedClaimed ? 1 : 0);
                    if (estimate.valid || estimate.zeroSpeedClaimed) {
                        TEST_ASSERT_TRUE(std::isfinite(estimate.velocityMps));
                        // A fact-carrying answer states when it was produced, so
                        // a consumer can age it instead of trusting it forever.
                        TEST_ASSERT_TRUE(estimate.elapsedMs > 0U);
                    } else {
                        TEST_ASSERT_TRUE(estimate.velocityMps == 0.0);
                        TEST_ASSERT_EQUAL_UINT32(0U, estimate.effectiveWindowMs);
                        TEST_ASSERT_TRUE(estimate.effectiveWindowTicks == 0);
                        // A refusal carries neither a number nor a claim, so it
                        // carries no stamp to age.
                        TEST_ASSERT_EQUAL_UINT32(0U, estimate.elapsedMs);
                    }
                    if (estimate.valid) {
                        TEST_ASSERT_TRUE(scale.isMeasured());
                        TEST_ASSERT_TRUE(scale.value > 0.0);
                        TEST_ASSERT_TRUE(std::isfinite(scale.value));
                        TEST_ASSERT_TRUE(estimate.effectiveWindowMs >= windowMs);
                        TEST_ASSERT_TRUE(estimate.effectiveWindowMs <= 200U);
                        TEST_ASSERT_FALSE(estimate.zeroSpeedClaimed);
                    }
                }
            }
        }
    }
}

void test_a_forgotten_configuration_fails_closed(void) {
    // VelocityEstimatorConfig has no default constructor, so a configuration
    // cannot be forgotten into existence. The explicit zero configuration is
    // also invalid, so zero-initialized storage can never be mistaken for a
    // usable one.
    const ev::VelocityEstimatorConfig zero{0U, 0U};
    TEST_ASSERT_EQUAL_UINT32(0U, zero.windowMs);
    TEST_ASSERT_EQUAL_UINT32(0U, zero.noEdgeTimeoutMs);
    TEST_ASSERT_FALSE(ev::validateVelocityEstimatorConfig(zero).ok);
}

void test_the_refused_estimate_shape_is_owned_in_one_place(void) {
    // The one definition of the value the vehicle layer's held members use: an
    // absent number is this shape with a status that says why, and the
    // estimator's own refusals are the same value.
    const ev::VelocityEstimate held =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    expectEstimate(ev::VelocityEstimateStatus::WarmingUp, false, false, held);
    TEST_ASSERT_TRUE(held.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, held.elapsedMs);
    TEST_ASSERT_EQUAL_UINT32(0U, held.effectiveWindowMs);
    TEST_ASSERT_TRUE(held.effectiveWindowTicks == 0);

    // Only the status differs, and it is the caller's reason, not a measurement.
    const ev::VelocityEstimate refusedScale =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::ScaleIsNotMeasured);
    expectEstimate(ev::VelocityEstimateStatus::ScaleIsNotMeasured, false, false, refusedScale);
    TEST_ASSERT_TRUE(refusedScale.velocityMps == 0.0);

    // A real refusal from the estimator carries exactly that shape.
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 5000U});
    const ev::VelocityEstimate fromEstimator = estimator.update(0U, 0, provisionalScale(0.0001));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityEstimateStatus::ScaleIsNotMeasured),
                          static_cast<int>(fromEstimator.status));
    TEST_ASSERT_FALSE(fromEstimator.valid);
    TEST_ASSERT_FALSE(fromEstimator.zeroSpeedClaimed);
    TEST_ASSERT_TRUE(fromEstimator.velocityMps == 0.0);
    TEST_ASSERT_EQUAL_UINT32(0U, fromEstimator.elapsedMs);
    TEST_ASSERT_EQUAL_UINT32(0U, fromEstimator.effectiveWindowMs);
    TEST_ASSERT_TRUE(fromEstimator.effectiveWindowTicks == 0);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_configuration_validation_rejects_the_zero_values);
    RUN_TEST(test_configuration_validation_requires_the_window_below_the_timeout);
    RUN_TEST(test_the_window_must_be_coverable_at_the_control_cadence);
    RUN_TEST(test_the_refused_estimate_shape_is_owned_in_one_place);
    RUN_TEST(test_an_invalid_configuration_never_produces_a_number);
    RUN_TEST(test_a_forgotten_configuration_fails_closed);

    RUN_TEST(test_constant_speed_gives_the_windowed_speed);
    RUN_TEST(test_the_realized_window_is_reported_when_samples_are_sparse);
    RUN_TEST(test_a_partial_span_never_produces_a_number);
    RUN_TEST(test_an_update_rate_too_fast_for_the_window_is_refused);
    RUN_TEST(test_the_too_fast_rate_refusal_is_not_latched);
    RUN_TEST(test_zero_ticks_in_a_covered_window_is_a_valid_zero);
    RUN_TEST(test_a_reversal_inside_the_window_reports_the_net_tick_change);
    RUN_TEST(test_falling_ticks_are_a_negative_speed_with_no_direction_claim);
    RUN_TEST(test_a_full_int32_tick_difference_inside_one_window_is_exact);
    RUN_TEST(test_an_irregular_sample_spacing_is_measured_not_assumed);

    RUN_TEST(test_a_silent_encoder_earns_the_claim_only_after_the_timeout);
    RUN_TEST(test_the_claim_is_not_made_one_millisecond_early);
    RUN_TEST(test_an_encoder_change_restarts_the_claim_clock);
    RUN_TEST(test_a_claim_is_never_made_on_an_unusable_scale);

    RUN_TEST(test_a_repeated_stamp_is_refused_and_never_enters_the_window);
    RUN_TEST(test_a_rewound_stamp_is_refused);
    RUN_TEST(test_the_window_works_across_millis_wraparound);

    RUN_TEST(test_only_a_measured_scale_can_produce_a_valid_speed);
    RUN_TEST(test_a_zero_or_negative_scale_is_refused_with_no_number);
    RUN_TEST(test_a_non_finite_scale_is_refused);
    RUN_TEST(test_a_measured_scale_whose_speed_overflows_is_refused);
    RUN_TEST(test_the_scale_verdict_matches_the_odometry_scale_rule);

    RUN_TEST(test_candidate_windows_can_be_compared_without_duplicating_an_algorithm);
    RUN_TEST(test_sparse_low_speed_ticks_quantize_differently_per_window);
    RUN_TEST(test_a_stop_is_visible_in_the_window_before_the_claim_arrives);
    RUN_TEST(test_an_edge_is_only_visible_in_the_sample_that_reports_it);

    RUN_TEST(test_counters_report_what_the_estimator_did);
    RUN_TEST(test_reset_discards_the_window_the_claim_and_the_counters);

    RUN_TEST(test_a_number_and_a_claim_carry_the_stamp_that_produced_them);
    RUN_TEST(test_readiness_is_the_one_velocity_freshness_verdict);
    RUN_TEST(test_readiness_reports_validity_before_age);
    RUN_TEST(test_a_zero_speed_claim_is_never_a_usable_velocity);

    RUN_TEST(test_estimator_invariants_hold_across_a_trace_sweep);

    return UNITY_END();
}
