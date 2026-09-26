#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <NearZeroVerifier.h>
#include <Units.h>

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
    return UNITY_END();
}
