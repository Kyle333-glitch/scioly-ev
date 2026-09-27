#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <NearZeroVerifier.h>
#include <Units.h>
#include <VelocityEstimator.h>

// Host tests for the near-zero verification policy: threshold + continuous dwell
// over three independently supplied facts (a fresh valid estimate, the speed
// below the threshold, and the estimator's explicit zero-speed claim).
//
// Every threshold, dwell and speed below is a test fixture. None is a measured
// vehicle value and none is a tuning suggestion: the vehicle has no verified
// stopping behavior (VERIFIED_STATE.md #5, #9), so no firmware path may use
// these numbers.
//
// These tests prove logic only. A verified stop here is not evidence that any
// vehicle stops, and it is not a claim that a real encoder's zero-speed claim
// arrives when the wheels have stopped turning.

static_assert(!std::is_default_constructible<ev::NearZeroVerdict>::value,
              "a verification verdict must never default-construct");
static_assert(static_cast<int>(ev::NearZeroStatus::Verified) == 0,
              "the verified status must stay the zero value");
static_assert(!std::is_default_constructible<ev::NearZeroConfig>::value,
              "no silent defaults; threshold and dwell must be supplied with their source");

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

// Fixture policy: within 0.05 m/s, held continuously for 0.2 s.
ev::NearZeroConfig fixtureConfig() { return ev::NearZeroConfig(measured(0.05), measured(0.2)); }

// A fresh, usable measurement at `velocityMps`, with the explicit claim either
// present or absent.
ev::NearZeroEvidence evidenceAt(double velocityMps, bool claimed) {
    ev::NearZeroEvidence evidence;
    evidence.velocityUsable = true;
    evidence.velocityMps = velocityMps;
    evidence.zeroSpeedClaimed = claimed;
    return evidence;
}

void expectStatus(ev::NearZeroStatus expected, const ev::NearZeroVerdict& verdict) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(verdict.status));
    TEST_ASSERT_EQUAL_INT(expected == ev::NearZeroStatus::Verified ? 1 : 0,
                          verdict.verified ? 1 : 0);
}

// The invariant every verdict must carry, whatever its inputs were.
void expectVerdictInvariant(const ev::NearZeroVerdict& verdict) {
    TEST_ASSERT_EQUAL_INT(verdict.status == ev::NearZeroStatus::Verified ? 1 : 0,
                          verdict.verified ? 1 : 0);
    if (verdict.verified) {
        TEST_ASSERT_TRUE(verdict.velocityUsable);
        TEST_ASSERT_TRUE(verdict.withinThreshold);
        TEST_ASSERT_TRUE(verdict.zeroSpeedClaimed);
    }
}

// One estimator answer, built by hand so the packet composition can be tested
// against stated facts instead of against a live estimator. The fields are the
// production type's; only the values are fixtures.
ev::VelocityEstimate syntheticEstimate(double velocityMps, bool valid, bool claimed,
                                       ev::VelocityEstimateStatus status, uint32_t elapsedMs) {
    return ev::VelocityEstimate(velocityMps, valid, claimed, status, elapsedMs, 20U,
                                static_cast<int64_t>(velocityMps * 200.0));
}

}  // namespace

// --- Configuration ----------------------------------------------------------

void test_config_refuses_unmeasured_and_degenerate_values(void) {
    TEST_ASSERT_TRUE(ev::validateNearZeroConfig(fixtureConfig()).ok);

    // A placeholder threshold or dwell can never verify anything.
    const ev::NearZeroConfig provisionalThreshold(provisional(0.05), measured(0.2));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateNearZeroConfig(provisionalThreshold).error));
    const ev::NearZeroConfig provisionalDwell(measured(0.05), provisional(0.2));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateNearZeroConfig(provisionalDwell).error));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const ev::NearZeroConfig nanThreshold(measured(nan), measured(0.2));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonFiniteValue),
                          static_cast<int>(ev::validateNearZeroConfig(nanThreshold).error));

    // A zero threshold could never be observed on a real measurement, and a zero
    // dwell would verify on the first observation, which is not an interval.
    const ev::NearZeroConfig zeroThreshold(measured(0.0), measured(0.2));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonPositiveThreshold),
                          static_cast<int>(ev::validateNearZeroConfig(zeroThreshold).error));
    const ev::NearZeroConfig zeroDwell(measured(0.05), measured(0.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonPositiveDwell),
                          static_cast<int>(ev::validateNearZeroConfig(zeroDwell).error));
}

// --- The composed evidence packet -------------------------------------------

void test_the_packet_pairs_the_current_claim_with_the_retained_window_estimate(void) {
    // The two answers the estimator actually produces: the current cycle's
    // explicit claim (no number), and the last window-derived estimate (a number,
    // no claim). Neither is the verifier's evidence on its own.
    const ev::VelocityEstimate claim = syntheticEstimate(
        0.0, false, true, ev::VelocityEstimateStatus::NoEdgeTimeout, 1000U);
    const ev::VelocityEstimate window = syntheticEstimate(
        0.02, true, false, ev::VelocityEstimateStatus::Valid, 980U);

    const ev::NearZeroEvidence packet =
        ev::composeStopEvidence(claim, true, window, true, 1000U, 60U, 200U);
    TEST_ASSERT_TRUE(packet.velocityUsable);
    TEST_ASSERT_TRUE(packet.velocityMps == 0.02);
    TEST_ASSERT_TRUE(packet.zeroSpeedClaimed);

    // Its three facts are exactly the verifier's, so the composed packet is what
    // earns the stop - and a stop is not earned on the first observation.
    ev::NearZeroVerifier verifier(fixtureConfig());
    TEST_ASSERT_FALSE(verifier.update(1000U, packet).verified);
    TEST_ASSERT_FALSE(verifier.update(1199U, packet).verified);
    TEST_ASSERT_TRUE(verifier.update(1200U, packet).verified);
}

void test_each_packet_fact_expires_on_its_own_limit(void) {
    const ev::VelocityEstimate claim = syntheticEstimate(
        0.0, false, true, ev::VelocityEstimateStatus::NoEdgeTimeout, 1000U);
    const ev::VelocityEstimate window = syntheticEstimate(
        0.02, true, false, ev::VelocityEstimateStatus::Valid, 980U);

    // The speed fact is the packet's own, longer limit: usable at 200 ms old,
    // not at 201.
    TEST_ASSERT_TRUE(ev::composeStopEvidence(claim, true, window, true, 1180U, 60U, 200U)
                         .velocityUsable);
    TEST_ASSERT_FALSE(ev::composeStopEvidence(claim, true, window, true, 1181U, 60U, 200U)
                          .velocityUsable);

    // The claim is the control path's limit: the claim interval must be current,
    // and the packet's longer speed limit must not stretch it.
    TEST_ASSERT_TRUE(ev::composeStopEvidence(claim, true, window, true, 1060U, 60U, 2000U)
                         .zeroSpeedClaimed);
    TEST_ASSERT_FALSE(ev::composeStopEvidence(claim, true, window, true, 1061U, 60U, 2000U)
                          .zeroSpeedClaimed);
}

void test_an_absent_answer_contributes_nothing_to_the_packet(void) {
    const ev::VelocityEstimate claim = syntheticEstimate(
        0.0, false, true, ev::VelocityEstimateStatus::NoEdgeTimeout, 1000U);
    const ev::VelocityEstimate window = syntheticEstimate(
        0.02, true, false, ev::VelocityEstimateStatus::Valid, 980U);

    // No current answer this cycle: nothing claims a zero, however good the
    // retained speed fact is.
    const ev::NearZeroEvidence noClaim =
        ev::composeStopEvidence(claim, false, window, true, 1000U, 60U, 200U);
    TEST_ASSERT_FALSE(noClaim.zeroSpeedClaimed);
    TEST_ASSERT_TRUE(noClaim.velocityUsable);

    // No retained window answer: no usable speed, so the packet can never earn a
    // stop however good the claim is.
    const ev::NearZeroEvidence noSpeed =
        ev::composeStopEvidence(claim, true, window, false, 1000U, 60U, 200U);
    TEST_ASSERT_FALSE(noSpeed.velocityUsable);
    TEST_ASSERT_TRUE(noSpeed.zeroSpeedClaimed);
}

void test_a_window_derived_zero_is_only_a_speed_fact_not_a_stop(void) {
    // A covered window with no change is a valid zero and never a claim, so a
    // packet built from it carries no zero-speed claim: the verifier must not
    // earn a stop from a window alone.
    const ev::VelocityEstimate windowZero = syntheticEstimate(
        0.0, true, false, ev::VelocityEstimateStatus::Valid, 1000U);
    const ev::NearZeroEvidence packet =
        ev::composeStopEvidence(windowZero, true, windowZero, true, 1000U, 60U, 200U);
    TEST_ASSERT_TRUE(packet.velocityUsable);
    TEST_ASSERT_FALSE(packet.zeroSpeedClaimed);

    ev::NearZeroVerifier verifier(fixtureConfig());
    for (uint32_t ms = 1000U; ms <= 1600U; ms += 20U) {
        TEST_ASSERT_FALSE_MESSAGE(verifier.update(ms, packet).verified,
                                  "a window zero alone earned a stop");
    }
}

void test_the_packet_limit_must_span_the_stop_dwell(void) {
    const ev::NearZeroConfig dwell200(measured(0.05), measured(0.2));
    TEST_ASSERT_TRUE(ev::stopEvidenceLimitSpansDwell(200U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(199U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(0U, dwell200));

    // An unusable configuration has no dwell to span, so no limit spans it - a
    // check that answered true here would look like approval of a placeholder.
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(
        1000U, ev::NearZeroConfig(measured(0.05), provisional(0.2))));

    // The fixture policy the scenarios use: a 0.1 s dwell with a 200 ms limit.
    TEST_ASSERT_TRUE(ev::stopEvidenceLimitSpansDwell(
        200U, ev::NearZeroConfig(measured(0.05), measured(0.1))));
}

void test_a_composed_packet_from_a_real_estimator_can_earn_a_verified_stop(void) {
    // The estimator never reports the claim and a number together, so the packet
    // is the only way the verifier's three facts hold at once. This drives the
    // real estimator through a stop and asserts the composed packet verifies
    // under the harness's policy: a 0.1 s dwell and a 200 ms speed-fact limit.
    const ev::Sourced<double> scale = measured(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});
    ev::NearZeroVerifier verifier(ev::NearZeroConfig(measured(0.05), measured(0.1)));

    ev::VelocityEstimate current = estimator.update(0U, 0, scale);
    ev::VelocityEstimate lastValid = current;
    bool hasLastValid = false;
    bool bothInOneAnswer = false;
    bool verified = false;
    uint32_t verifiedAtMs = 0U;

    for (uint32_t ms = 20U; ms <= 800U; ms += 20U) {
        // Moving at 1.0 m/s until 100 ms, then the count holds.
        const int32_t ticks = ms < 100U ? static_cast<int32_t>(ms * 10U) : 1000;
        current = estimator.update(ms, ticks, scale);
        bothInOneAnswer = bothInOneAnswer || (current.valid && current.zeroSpeedClaimed);
        if (current.valid) {
            lastValid = current;
            hasLastValid = true;
        }
        if (verifier
                .update(ms, ev::composeStopEvidence(current, true, lastValid, hasLastValid, ms,
                                                    60U, 200U))
                .verified) {
            verified = true;
            verifiedAtMs = ms;
            break;
        }
    }

    TEST_ASSERT_FALSE_MESSAGE(bothInOneAnswer,
                              "one estimator answer carried a window number and the claim");
    TEST_ASSERT_TRUE_MESSAGE(verified, "the composed packet never earned a stop");
    // The dwell is a real observed interval, so the stop cannot be verified
    // before the claim has existed for it: the count stops at 100 ms, the
    // estimator's no-edge interval is 100 ms, and the dwell is another 100 ms.
    TEST_ASSERT_TRUE_MESSAGE(verifiedAtMs >= 300U,
                             "a stop was verified before its dwell could have elapsed");
}

// --- Refusals ---------------------------------------------------------------

void test_default_built_evidence_observes_nothing(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());
    const ev::NearZeroEvidence nothing;
    const ev::NearZeroVerdict verdict = verifier.update(1000U, nothing);

    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    TEST_ASSERT_FALSE(verdict.velocityUsable);
    TEST_ASSERT_FALSE(verdict.withinThreshold);
    TEST_ASSERT_FALSE(verdict.zeroSpeedClaimed);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
}

void test_unusable_config_never_verifies(void) {
    ev::NearZeroVerifier verifier(ev::NearZeroConfig(provisional(0.05), measured(0.2)));
    TEST_ASSERT_FALSE(verifier.configUsable());

    ev::NearZeroVerdict verdict = verifier.update(1000U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::ConfigIsNotUsable, verdict);
    for (uint32_t ms = 1000U; ms <= 5000U; ms += 50U) {
        verdict = verifier.update(ms, evidenceAt(0.0, true));
        expectStatus(ev::NearZeroStatus::ConfigIsNotUsable, verdict);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
}

void test_stale_velocity_never_verifies(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // A last-known zero with no fresh estimate is not a stop.
    ev::NearZeroEvidence stale;
    stale.velocityUsable = false;
    stale.velocityMps = 0.0;
    stale.zeroSpeedClaimed = true;

    ev::NearZeroVerdict verdict = verifier.update(1000U, stale);
    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    for (uint32_t ms = 1050U; ms <= 4000U; ms += 50U) {
        verdict = verifier.update(ms, stale);
        expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
        expectVerdictInvariant(verdict);
    }
    TEST_ASSERT_FALSE(verdict.verified);
    TEST_ASSERT_FALSE(verdict.velocityUsable);
}

void test_a_small_window_number_without_the_claim_is_not_a_stop(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // The window reads below the threshold, but the encoder is still ticking
    // slowly: the claim is what rules the creep out.
    ev::NearZeroVerdict verdict = verifier.update(1000U, evidenceAt(0.01, false));
    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    TEST_ASSERT_TRUE(verdict.velocityUsable);
    TEST_ASSERT_TRUE(verdict.withinThreshold);
    TEST_ASSERT_FALSE(verdict.zeroSpeedClaimed);

    for (uint32_t ms = 1050U; ms <= 5000U; ms += 50U) {
        verdict = verifier.update(ms, evidenceAt(0.01, false));
        expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    }
    TEST_ASSERT_FALSE(verdict.verified);
}

void test_a_claim_with_an_above_threshold_speed_is_not_a_stop(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // Contradictory evidence: the claim says nothing changed, while a fresh
    // window says the vehicle is moving. The threshold wins, because a stop is
    // the thing being verified.
    const ev::NearZeroVerdict verdict = verifier.update(1000U, evidenceAt(0.5, true));
    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    TEST_ASSERT_FALSE(verdict.withinThreshold);
    TEST_ASSERT_TRUE(verdict.zeroSpeedClaimed);
}

// --- Verification -----------------------------------------------------------

void test_verified_only_after_the_configured_dwell(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // First observation starts the interval; it does not verify.
    ev::NearZeroVerdict verdict = verifier.update(1000U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);

    verdict = verifier.update(1100U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(100U, verdict.dwellElapsedMs);

    verdict = verifier.update(1199U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(199U, verdict.dwellElapsedMs);

    verdict = verifier.update(1200U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_EQUAL_UINT32(200U, verdict.dwellElapsedMs);
    expectVerdictInvariant(verdict);

    // Staying near zero keeps verifying, with a growing interval.
    verdict = verifier.update(1400U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_EQUAL_UINT32(400U, verdict.dwellElapsedMs);
}

void test_a_negative_speed_within_magnitude_verifies(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // The threshold is a magnitude: which counting convention is physical
    // forward travel is unverified (#4), so a near-zero stop must be recognized
    // on either sign.
    ev::NearZeroVerdict verdict = verifier.update(0U, evidenceAt(-0.02, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    verdict = verifier.update(200U, evidenceAt(-0.02, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
}

void test_the_dwell_must_be_continuous(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    verifier.update(1000U, evidenceAt(0.0, true));
    ev::NearZeroVerdict verdict = verifier.update(1180U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(180U, verdict.dwellElapsedMs);

    // One cycle above the threshold clears the whole interval.
    verdict = verifier.update(1200U, evidenceAt(0.4, true));
    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);

    // The next near zero starts a brand-new interval and does not inherit the
    // 180 ms that were observed before the movement.
    verdict = verifier.update(1250U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
    verdict = verifier.update(1400U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(150U, verdict.dwellElapsedMs);
    verdict = verifier.update(1450U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_EQUAL_UINT32(200U, verdict.dwellElapsedMs);
}

void test_oscillation_around_the_threshold_never_verifies(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // A vehicle bouncing on the threshold accumulates nothing, however long it
    // is watched: the dwell is a continuous interval, not a total.
    ev::NearZeroVerdict verdict = verifier.update(0U, evidenceAt(0.0, true));
    const double aboveThreshold = 0.06;
    for (uint32_t ms = 50U; ms <= 4000U; ms += 50U) {
        const bool nearZero = (ms / 50U) % 2U == 0U;
        verdict = verifier.update(
            ms, evidenceAt(nearZero ? 0.0 : aboveThreshold, true));
        TEST_ASSERT_FALSE(verdict.verified);
        expectVerdictInvariant(verdict);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
}

void test_precision_at_the_threshold_boundary(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    // Exactly at the threshold is inside it (the comparison is inclusive); one
    // step beyond is not.
    ev::NearZeroVerdict verdict = verifier.update(0U, evidenceAt(0.05, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    verdict = verifier.update(200U, evidenceAt(0.05, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);

    ev::NearZeroVerifier strict(fixtureConfig());
    verdict = strict.update(0U, evidenceAt(0.05 + 1e-9, true));
    expectStatus(ev::NearZeroStatus::AwaitingEvidence, verdict);
}

// --- Clock behavior ---------------------------------------------------------

void test_a_non_advancing_stamp_holds_the_dwell(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    verifier.update(1000U, evidenceAt(0.0, true));
    ev::NearZeroVerdict verdict = verifier.update(1100U, evidenceAt(0.0, true));
    TEST_ASSERT_EQUAL_UINT32(100U, verdict.dwellElapsedMs);

    // A repeated stamp neither grows nor clears the interval: nothing was
    // observed over the missing time, so nothing may be credited to it.
    verdict = verifier.update(1100U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::TimeDidNotAdvance, verdict);
    TEST_ASSERT_EQUAL_UINT32(100U, verdict.dwellElapsedMs);
    TEST_ASSERT_FALSE(verdict.verified);

    // The next advancing stamp still yields the true elapsed span.
    verdict = verifier.update(1200U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_EQUAL_UINT32(200U, verdict.dwellElapsedMs);
}

void test_a_rewound_stamp_cannot_verify_early(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    verifier.update(10000U, evidenceAt(0.0, true));
    const ev::NearZeroVerdict verdict = verifier.update(500U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::TimeDidNotAdvance, verdict);
    TEST_ASSERT_FALSE(verdict.verified);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
}

void test_reset_clears_the_observed_interval(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    verifier.update(0U, evidenceAt(0.0, true));
    ev::NearZeroVerdict verdict = verifier.update(180U, evidenceAt(0.0, true));
    TEST_ASSERT_EQUAL_UINT32(180U, verdict.dwellElapsedMs);

    verifier.reset();
    TEST_ASSERT_EQUAL_UINT32(0U, verifier.dwellElapsedMs());

    verdict = verifier.update(200U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::DwellInProgress, verdict);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
    TEST_ASSERT_FALSE(verdict.verified);
}

void test_an_update_gap_is_credited_as_continued_evidence(void) {
    // Pinned deliberate behavior, not a defect: two advancing stamps are all
    // this module sees, so the time between them is credited as if the evidence
    // had continued across it. What keeps that honest is elsewhere on purpose: a
    // stalled control loop is a command-timeout question with its own owner
    // (AGENTS.md section 5), and every sequence that consumes a verdict carries a
    // timeout evaluated before the verification step. Pinning it here means a
    // future change to it has to be a decision.
    ev::NearZeroVerifier verifier(fixtureConfig());
    verifier.update(0U, evidenceAt(0.0, true));

    const ev::NearZeroVerdict verdict = verifier.update(2000U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_EQUAL_UINT32(2000U, verdict.dwellElapsedMs);
}

// --- The invariant over a varied evidence sequence ---------------------------

void test_verdict_invariants_hold_over_a_varied_sequence(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    const double speeds[] = {0.0, 0.01, 0.05, 0.06, -0.02, 1.5, 0.0};
    const bool claims[] = {true, true, false, true, true, false, true};
    const bool usable[] = {true, true, true, true, true, true, false};

    uint32_t nowMs = 0U;
    for (uint32_t step = 0U; step < 60U; ++step) {
        const uint32_t index = step % 7U;
        ev::NearZeroEvidence evidence;
        evidence.velocityUsable = usable[index];
        evidence.velocityMps = speeds[index];
        evidence.zeroSpeedClaimed = claims[index];
        nowMs += 50U;

        const ev::NearZeroVerdict verdict = verifier.update(nowMs, evidence);
        expectVerdictInvariant(verdict);
        if (verdict.verified) {
            TEST_ASSERT_TRUE(std::fabs(evidence.velocityMps) <= 0.05);
            TEST_ASSERT_TRUE(verdict.dwellElapsedMs >= 200U);
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_config_refuses_unmeasured_and_degenerate_values);
    RUN_TEST(test_default_built_evidence_observes_nothing);
    RUN_TEST(test_unusable_config_never_verifies);
    RUN_TEST(test_stale_velocity_never_verifies);
    RUN_TEST(test_a_small_window_number_without_the_claim_is_not_a_stop);
    RUN_TEST(test_a_claim_with_an_above_threshold_speed_is_not_a_stop);
    RUN_TEST(test_verified_only_after_the_configured_dwell);
    RUN_TEST(test_a_negative_speed_within_magnitude_verifies);
    RUN_TEST(test_the_dwell_must_be_continuous);
    RUN_TEST(test_oscillation_around_the_threshold_never_verifies);
    RUN_TEST(test_precision_at_the_threshold_boundary);
    RUN_TEST(test_a_non_advancing_stamp_holds_the_dwell);
    RUN_TEST(test_a_rewound_stamp_cannot_verify_early);
    RUN_TEST(test_reset_clears_the_observed_interval);
    RUN_TEST(test_an_update_gap_is_credited_as_continued_evidence);
    RUN_TEST(test_verdict_invariants_hold_over_a_varied_sequence);
    RUN_TEST(test_the_packet_pairs_the_current_claim_with_the_retained_window_estimate);
    RUN_TEST(test_each_packet_fact_expires_on_its_own_limit);
    RUN_TEST(test_an_absent_answer_contributes_nothing_to_the_packet);
    RUN_TEST(test_a_window_derived_zero_is_only_a_speed_fact_not_a_stop);
    RUN_TEST(test_the_packet_limit_must_span_the_stop_dwell);
    RUN_TEST(test_a_composed_packet_from_a_real_estimator_can_earn_a_verified_stop);
    return UNITY_END();
}
