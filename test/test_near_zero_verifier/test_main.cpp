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
// below the threshold, and the estimator's explicit zero-speed claim), plus the
// bound on the gap between two updates that may still be credited as continued
// evidence.
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

ev::Sourced<uint32_t> measuredGap(uint32_t value) {
    return ev::Sourced<uint32_t>(value, ev::ValueSource::Measured);
}

// A value the official rules fix, with the rule it came from. The citation is
// what makes it usable for a dwell; a `RuleDerived` value with no citation is
// refused exactly like a placeholder.
ev::Sourced<double> ruleDerived(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::RuleDerived, "rules2027 8.p");
}

ev::Sourced<double> ruleDerivedWithoutCitation(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::RuleDerived);
}

// Fixture policy: within 0.05 m/s, held continuously for 0.2 s, with an update
// gap limit of 250 ms.
//
// The limit is deliberately wider than every cadence the tests below step at
// (20, 50 and 100 ms), so those tests stay about the cadence and the dwell rather
// than about the gap. The gap rule has its own tests, which use a narrow limit so
// the behavior is visible rather than incidental.
ev::NearZeroConfig fixtureConfig() {
    return ev::NearZeroConfig(measured(0.05), measured(0.2), measuredGap(250U));
}

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

    // A placeholder threshold or gap limit can never verify anything.
    const ev::NearZeroConfig provisionalThreshold(provisional(0.05), measured(0.2),
                                                  measuredGap(250U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateNearZeroConfig(provisionalThreshold).error));
    // A placeholder dwell is refused for its own reason: it is neither measured
    // nor a cited rule value, and a stop that rests on a guess may not gate one.
    const ev::NearZeroConfig provisionalDwell(measured(0.05), provisional(0.2),
                                              measuredGap(250U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::DwellIsNotEstablished),
                          static_cast<int>(ev::validateNearZeroConfig(provisionalDwell).error));
    // The gap limit is what bounds how much dwell can be credited without
    // observation, so an unmeasured one is not usable either.
    const ev::NearZeroConfig provisionalGapLimit(measured(0.05), measured(0.2),
                                                 ev::Sourced<uint32_t>(
                                                     250U, ev::ValueSource::Provisional));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateNearZeroConfig(provisionalGapLimit).error));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const ev::NearZeroConfig nanThreshold(measured(nan), measured(0.2), measuredGap(250U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonFiniteValue),
                          static_cast<int>(ev::validateNearZeroConfig(nanThreshold).error));

    // A zero threshold could never be observed on a real measurement, and a zero
    // dwell would verify on the first observation, which is not an interval.
    const ev::NearZeroConfig zeroThreshold(measured(0.0), measured(0.2), measuredGap(250U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonPositiveThreshold),
                          static_cast<int>(ev::validateNearZeroConfig(zeroThreshold).error));
    const ev::NearZeroConfig zeroDwell(measured(0.05), measured(0.0), measuredGap(250U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonPositiveDwell),
                          static_cast<int>(ev::validateNearZeroConfig(zeroDwell).error));
    // A zero gap limit would refuse every continued interval, which is not a
    // policy anyone states from a stall measurement.
    const ev::NearZeroConfig zeroGap(measured(0.05), measured(0.2), measuredGap(0U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::NonPositiveUpdateGap),
                          static_cast<int>(ev::validateNearZeroConfig(zeroGap).error));
}

// The final stop's dwell is the event's Complete Stop (rule 8.p) - a rule value,
// never a measurement - so it must be accepted where a measured dwell is, while a
// placeholder still is not. This is the check that decides whether a run with an
// otherwise complete calibration can ever leave FinalBrake.
void test_a_cited_rule_dwell_gates_a_stop_and_an_uncited_one_does_not(void) {
    const ev::NearZeroConfig completeStop(measured(0.05), ruleDerived(3.0), measuredGap(3200U));
    TEST_ASSERT_TRUE(ev::validateNearZeroConfig(completeStop).ok);
    TEST_ASSERT_TRUE(ev::dwellProvenanceIsEstablished(completeStop.dwellS));

    // A rule value that does not say which rule is not established: the citation
    // is the difference between a rule and an unmeasured number.
    const ev::NearZeroConfig uncited(measured(0.05), ruleDerivedWithoutCitation(3.0),
                                     measuredGap(3200U));
    TEST_ASSERT_FALSE(ev::validateNearZeroConfig(uncited).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::NearZeroConfigError::DwellIsNotEstablished),
                          static_cast<int>(ev::validateNearZeroConfig(uncited).error));
    TEST_ASSERT_FALSE(ev::dwellProvenanceIsEstablished(uncited.dwellS));

    // The measurement may still be what a dwell is (the reversal gate's is).
    TEST_ASSERT_TRUE(ev::dwellProvenanceIsEstablished(measured(0.2)));
    TEST_ASSERT_FALSE(ev::dwellProvenanceIsEstablished(provisional(0.2)));

    // And the rule dwell actually verifies a stop: the config is usable, so the
    // verifier produces verdicts instead of refusing.
    ev::NearZeroVerifier verifier(completeStop);
    TEST_ASSERT_TRUE(verifier.configUsable());
    const ev::NearZeroEvidence evidence = evidenceAt(0.0, true);
    expectStatus(ev::NearZeroStatus::DwellInProgress, verifier.update(1000U, evidence));
    expectStatus(ev::NearZeroStatus::Verified, verifier.update(4000U, evidence));
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
    const ev::NearZeroConfig dwell200(measured(0.05), measured(0.2), measuredGap(250U));
    TEST_ASSERT_TRUE(ev::stopEvidenceLimitSpansDwell(200U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(199U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(0U, dwell200));

    // An unusable configuration has no dwell to span, so no limit spans it - a
    // check that answered true here would look like approval of a placeholder.
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(
        1000U, ev::NearZeroConfig(measured(0.05), provisional(0.2), measuredGap(250U))));

    // The fixture policy the scenarios use: a 0.1 s dwell with a 200 ms limit.
    TEST_ASSERT_TRUE(ev::stopEvidenceLimitSpansDwell(
        200U, ev::NearZeroConfig(measured(0.05), measured(0.1), measuredGap(250U))));
}

void test_the_packet_limit_must_span_the_claim_lead_plus_the_dwell(void) {
    // S18A/P5: the claim appears only after the estimator's no-edge interval,
    // and the paired speed keeps aging through the dwell. A limit that spans
    // the dwell alone (200 ms here) but not the 100 ms lead cannot let the
    // pair coincide, so it is refused.
    const ev::NearZeroConfig dwell200(measured(0.05), measured(0.2), measuredGap(250U));
    TEST_ASSERT_TRUE(ev::stopEvidenceSpansClaimAndDwell(300U, 100U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(299U, 100U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(200U, 100U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(0U, 100U, dwell200));

    // With no claim lead the check degrades to the dwell span, boundary for
    // boundary with the older check above.
    TEST_ASSERT_TRUE(ev::stopEvidenceSpansClaimAndDwell(200U, 0U, dwell200));
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(199U, 0U, dwell200));

    // An unusable configuration has nothing to span.
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(
        1000U, 100U,
        ev::NearZeroConfig(measured(0.05), provisional(0.2), measuredGap(250U))));

    // The fixture policy the scenarios use: a 0.1 s dwell, a 100 ms claim
    // lead, and a 200 ms limit - exactly spanning.
    TEST_ASSERT_TRUE(ev::stopEvidenceSpansClaimAndDwell(
        200U, 100U, ev::NearZeroConfig(measured(0.05), measured(0.1), measuredGap(250U))));
    TEST_ASSERT_FALSE(ev::stopEvidenceSpansClaimAndDwell(
        199U, 100U, ev::NearZeroConfig(measured(0.05), measured(0.1), measuredGap(250U))));
}

void test_a_composed_packet_from_a_real_estimator_can_earn_a_verified_stop(void) {
    // The estimator never reports the claim and a number together, so the packet
    // is the only way the verifier's three facts hold at once. This drives the
    // real estimator through a stop and asserts the composed packet verifies
    // under the harness's policy: a 0.1 s dwell and a 200 ms speed-fact limit.
    const ev::Sourced<double> scale = measured(0.0001);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{20U, 100U});
    ev::NearZeroVerifier verifier(
        ev::NearZeroConfig(measured(0.05), measured(0.1), measuredGap(250U)));

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
    ev::NearZeroVerifier verifier(
        ev::NearZeroConfig(provisional(0.05), measured(0.2), measuredGap(250U)));
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

void test_a_gap_beyond_the_limit_is_not_credited(void) {
    // Two advancing stamps do not say whether the loop ran in between, so a gap
    // wider than the configured limit does not grow the dwell. The evidence holds
    // now - the interval simply restarts here, and the verdict says why.
    //
    // The dwell here is 50 ms and the gap limit 60 ms, so "the restarted interval
    // can still be earned" is observable within the test rather than after 200 ms
    // of fixture time.
    ev::NearZeroVerifier verifier(
        ev::NearZeroConfig(measured(0.05), measured(0.05), measuredGap(60U)));
    verifier.update(0U, evidenceAt(0.0, true));

    const ev::NearZeroVerdict verdict = verifier.update(2000U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::UpdateGapExceeded, verdict);
    TEST_ASSERT_FALSE(verdict.verified);
    TEST_ASSERT_EQUAL_UINT32(0U, verdict.dwellElapsedMs);
    TEST_ASSERT_TRUE(verdict.velocityUsable);
    TEST_ASSERT_TRUE(verdict.withinThreshold);
    TEST_ASSERT_TRUE(verdict.zeroSpeedClaimed);
    expectVerdictInvariant(verdict);

    // The restarted interval is real: the dwell is earned from the observation
    // that ended the gap, not from the time before it.
    TEST_ASSERT_FALSE(verifier.update(2030U, evidenceAt(0.0, true)).verified);
    const ev::NearZeroVerdict earned = verifier.update(2060U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, earned);
    TEST_ASSERT_EQUAL_UINT32(60U, earned.dwellElapsedMs);
}

void test_a_gap_of_exactly_the_limit_is_still_credited(void) {
    // The limit is inclusive: a gap equal to it was observed at the cadence the
    // limit describes. One millisecond more is not.
    ev::NearZeroVerifier verifier(
        ev::NearZeroConfig(measured(0.05), measured(0.2), measuredGap(60U)));
    verifier.update(0U, evidenceAt(0.0, true));

    expectStatus(ev::NearZeroStatus::DwellInProgress,
                 verifier.update(60U, evidenceAt(0.0, true)));
    expectStatus(ev::NearZeroStatus::UpdateGapExceeded,
                 verifier.update(121U, evidenceAt(0.0, true)));
}

void test_a_gap_does_not_credit_an_interval_across_a_stall(void) {
    // The whole point of the bound: a stalled loop cannot silently earn the dwell
    // a stop is verified from. Without the gap rule this sequence verifies at the
    // second update.
    ev::NearZeroVerifier verifier(
        ev::NearZeroConfig(measured(0.05), measured(0.2), measuredGap(60U)));
    verifier.update(0U, evidenceAt(0.0, true));

    for (uint32_t ms = 5000U; ms <= 5000U + 100U; ms += 50U) {
        TEST_ASSERT_FALSE_MESSAGE(verifier.update(ms, evidenceAt(0.0, true)).verified,
                                  "a stall was credited as an observed dwell");
    }
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

// The interval's own start is what a run's final-stop timeline is stamped with,
// so the interval's owner reports it rather than leaving a caller to recover it
// from the elapsed span: open exactly while the evidence holds, a new reading
// after every discard or restart, and unchanged while the stamp is frozen.
void test_the_open_interval_reports_its_own_start(void) {
    ev::NearZeroVerifier verifier(fixtureConfig());

    TEST_ASSERT_FALSE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(0U, verifier.intervalStartedAtMs());

    // The first observation opens the interval at this reading.
    verifier.update(1000U, evidenceAt(0.0, true));
    TEST_ASSERT_TRUE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(1000U, verifier.intervalStartedAtMs());

    // A non-advancing stamp holds the dwell: it is the same interval, so its
    // start does not move.
    verifier.update(1000U, evidenceAt(0.0, true));
    TEST_ASSERT_TRUE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(1000U, verifier.intervalStartedAtMs());

    // Broken evidence closes the interval: there is no start left to report.
    verifier.update(1050U, evidenceAt(0.5, true));
    TEST_ASSERT_FALSE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(0U, verifier.intervalStartedAtMs());

    // The restart is a new interval with its own start, not the old one.
    verifier.update(1100U, evidenceAt(0.0, true));
    TEST_ASSERT_TRUE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(1100U, verifier.intervalStartedAtMs());

    // Verified: the interval stays open, reporting the start it was earned from.
    const ev::NearZeroVerdict verdict = verifier.update(1300U, evidenceAt(0.0, true));
    expectStatus(ev::NearZeroStatus::Verified, verdict);
    TEST_ASSERT_TRUE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(1100U, verifier.intervalStartedAtMs());

    // A reset ends the interval with the rest of the observation.
    verifier.reset();
    TEST_ASSERT_FALSE(verifier.intervalOpen());
    TEST_ASSERT_EQUAL_UINT32(0U, verifier.intervalStartedAtMs());
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
    RUN_TEST(test_a_gap_beyond_the_limit_is_not_credited);
    RUN_TEST(test_a_gap_of_exactly_the_limit_is_still_credited);
    RUN_TEST(test_a_gap_does_not_credit_an_interval_across_a_stall);
    RUN_TEST(test_verdict_invariants_hold_over_a_varied_sequence);
    RUN_TEST(test_the_packet_pairs_the_current_claim_with_the_retained_window_estimate);
    RUN_TEST(test_each_packet_fact_expires_on_its_own_limit);
    RUN_TEST(test_an_absent_answer_contributes_nothing_to_the_packet);
    RUN_TEST(test_a_window_derived_zero_is_only_a_speed_fact_not_a_stop);
    RUN_TEST(test_the_packet_limit_must_span_the_stop_dwell);
    RUN_TEST(test_the_packet_limit_must_span_the_claim_lead_plus_the_dwell);
    RUN_TEST(test_a_composed_packet_from_a_real_estimator_can_earn_a_verified_stop);
    RUN_TEST(test_the_open_interval_reports_its_own_start);
    // S13R I-2 / P9: this case existed but was never registered, so Unity never
    // ran it and the suite counted it nowhere. It pins the cited-`RuleDerived`
    // dwell provenance the final stop's Complete Stop interval depends on.
    RUN_TEST(test_a_cited_rule_dwell_gates_a_stop_and_an_uncited_one_does_not);
    return UNITY_END();
}
