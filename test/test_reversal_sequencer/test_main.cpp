#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <NearZeroVerifier.h>
#include <ReversalSequencer.h>
#include <Units.h>

// Host tests for the controlled reversal sequence: ramp down, brake, verify the
// near-zero stop with a continuous dwell, and only then permit the direction
// change - or refuse deterministically when the near zero never arrives.
//
// Every threshold, dwell, timeout and speed below is a test fixture. None is a
// measured value and none may be used by a firmware path: the vehicle's stopping
// behavior is UNVERIFIED (VERIFIED_STATE.md #9), which is exactly why this
// module exists in front of the reversal.
//
// These tests prove sequencing logic only. They are not evidence about any
// drivetrain, and no test here measures whether a real vehicle comes to rest.

static_assert(!std::is_default_constructible<ev::ReversalStatus>::value,
              "a reversal status must never default-construct");
static_assert(!std::is_default_constructible<ev::ReversalConfig>::value,
              "no silent defaults; the verification policy and timeout must be supplied");
static_assert(static_cast<int>(ev::ReversalPhase::Idle) == 0,
              "the idle phase must stay the zero value");
static_assert(static_cast<int>(ev::ReversalRefusal::None) == 0,
              "no refusal must stay the zero value");
static_assert(static_cast<int>(ev::ReversalBeginStatus::Started) == 0,
              "a started reversal must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

// The stop-verification policy the fixtures use: threshold, dwell and a fixture
// update-gap limit wide enough that these tests stay about the reversal sequence.
ev::NearZeroConfig stopConfig(ev::Sourced<double> threshold, ev::Sourced<double> dwell) {
    return ev::NearZeroConfig(threshold, dwell,
                              ev::Sourced<uint32_t>(250U, ev::ValueSource::Measured));
}

// Fixture policy: within 0.05 m/s held for 0.2 s, whole sequence bounded at 3 s.
ev::ReversalConfig fixtureConfig() {
    return ev::ReversalConfig(stopConfig(measured(0.05), measured(0.2)), measured(3.0));
}

// A fresh, usable measurement of a vehicle still moving.
ev::NearZeroEvidence moving(double velocityMps) {
    ev::NearZeroEvidence evidence;
    evidence.velocityUsable = true;
    evidence.velocityMps = velocityMps;
    evidence.zeroSpeedClaimed = false;
    return evidence;
}

// A fresh, usable measurement within the threshold, with or without the
// estimator's explicit zero-speed claim.
ev::NearZeroEvidence nearZero(bool claimed) {
    ev::NearZeroEvidence evidence;
    evidence.velocityUsable = true;
    evidence.velocityMps = 0.0;
    evidence.zeroSpeedClaimed = claimed;
    return evidence;
}

void expectPhase(ev::ReversalPhase expected, const ev::ReversalStatus& status) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(status.phase));
}

void expectRefusal(ev::ReversalRefusal expected, const ev::ReversalStatus& status) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(status.refusal));
    TEST_ASSERT_EQUAL_INT(ev::ReversalPhase::StopRequired == status.phase ? 1 : 0,
                          expected != ev::ReversalRefusal::None ? 1 : 0);
}

// Drives a started sequence to the permission with the fixture times, and fails
// the calling test if the permission is not reached.
void earnPermission(ev::ReversalSequencer& sequencer, uint32_t& nowMs) {
    ev::ReversalStatus status = sequencer.update(nowMs, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);

    nowMs += 50U;
    status = sequencer.update(nowMs, nearZero(false));
    expectPhase(ev::ReversalPhase::Braking, status);

    nowMs += 50U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);

    nowMs += 200U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());
}

}  // namespace

// --- Configuration ----------------------------------------------------------

void test_config_refuses_placeholders_and_degenerate_values(void) {
    TEST_ASSERT_TRUE(ev::validateReversalConfig(fixtureConfig()).ok);

    const ev::ReversalConfig provisionalVerification(
        stopConfig(provisional(0.05), measured(0.2)), measured(3.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalConfigError::VerificationNotUsable),
                          static_cast<int>(
                              ev::validateReversalConfig(provisionalVerification).error));

    const ev::ReversalConfig provisionalTimeout(
        stopConfig(measured(0.05), measured(0.2)), provisional(3.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalConfigError::TimeoutNotMeasured),
                          static_cast<int>(ev::validateReversalConfig(provisionalTimeout).error));

    const ev::ReversalConfig zeroTimeout(stopConfig(measured(0.05), measured(0.2)),
                                         measured(0.0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalConfigError::NonPositiveTimeout),
                          static_cast<int>(ev::validateReversalConfig(zeroTimeout).error));

    const double infinity = std::numeric_limits<double>::infinity();
    const ev::ReversalConfig infiniteTimeout(stopConfig(measured(0.05), measured(0.2)),
                                             measured(infinity));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalConfigError::NonFiniteTimeout),
                          static_cast<int>(ev::validateReversalConfig(infiniteTimeout).error));
}

// --- Starting ---------------------------------------------------------------

void test_fresh_sequencer_is_idle_and_permits_nothing(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(sequencer.phase()));
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    TEST_ASSERT_FALSE(sequencer.stopVerified());

    // An update with no sequence running says exactly that, and never grows a
    // permission out of nothing.
    const ev::ReversalStatus status = sequencer.update(100U, nearZero(true));
    expectPhase(ev::ReversalPhase::Idle, status);
    expectRefusal(ev::ReversalRefusal::None, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_EQUAL_UINT32(0U, status.sequenceElapsedMs);
}

void test_a_reversal_to_the_same_direction_is_not_sequenced(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::NotNeeded),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Forward, 0U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(sequencer.phase()));

    const ev::ReversalStatus status = sequencer.update(1000U, nearZero(true));
    expectPhase(ev::ReversalPhase::Idle, status);
    TEST_ASSERT_FALSE(sequencer.stopVerified());
}

void test_an_unusable_configuration_refuses_immediately_and_latches(void) {
    ev::ReversalSequencer sequencer(ev::ReversalConfig(
        stopConfig(provisional(0.05), measured(0.2)), measured(3.0)));
    TEST_ASSERT_FALSE(sequencer.configUsable());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::ConfigIsNotUsable),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U)));

    // The refusal is immediate - no time is spent waiting for a verification
    // that could never be trusted - and it is latched, so a later begin cannot
    // restart it quietly.
    ev::ReversalStatus status = sequencer.update(0U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::ConfigIsNotUsable, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    status = sequencer.update(100000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::ConfigIsNotUsable, status);

    // The configuration is the cause of the refusal, so every later attempt
    // reports it rather than being masked by the latched stop.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::ConfigIsNotUsable),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 200000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::StopRequired),
                          static_cast<int>(sequencer.phase()));
}

// --- The nominal sequence ---------------------------------------------------

void test_nominal_sequence_reaches_the_permission(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::Started),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 1000U)));

    uint32_t nowMs = 1000U;
    ev::ReversalStatus status = sequencer.update(nowMs, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    expectRefusal(ev::ReversalRefusal::None, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    TEST_ASSERT_EQUAL_UINT32(0U, status.sequenceElapsedMs);

    // Near zero observed, but the count is still changing: brake and wait.
    nowMs = 1050U;
    status = sequencer.update(nowMs, nearZero(false));
    expectPhase(ev::ReversalPhase::Braking, status);
    TEST_ASSERT_TRUE(status.verification.velocityUsable);
    TEST_ASSERT_TRUE(status.verification.withinThreshold);
    TEST_ASSERT_FALSE(status.verification.zeroSpeedClaimed);
    TEST_ASSERT_FALSE(status.stopVerified);

    // The estimator declares the count stopped changing: the dwell starts.
    nowMs = 1100U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);
    TEST_ASSERT_EQUAL_UINT32(0U, status.verification.dwellElapsedMs);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    TEST_ASSERT_EQUAL_UINT32(100U, status.sequenceElapsedMs);

    nowMs = 1250U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);
    TEST_ASSERT_EQUAL_UINT32(150U, status.verification.dwellElapsedMs);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    nowMs = 1300U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    TEST_ASSERT_EQUAL_UINT32(200U, status.verification.dwellElapsedMs);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());
    TEST_ASSERT_EQUAL_UINT32(300U, status.sequenceElapsedMs);
}

void test_moving_evidence_after_the_stop_invalidates_the_permission(void) {
    // S18A/P1 correction: this test used to require that a verified stop
    // survive later moving evidence (`DirectionChangeReady` held with
    // `stopVerified` true after a +2 m/s update). That is the H4 defect -
    // historical completion consumed as current permission. The permission now
    // requires current evidence: fresh motion invalidates it, and re-earning
    // runs the full dwell again. History beyond the live verdict is the log's
    // job, never a permission.
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    uint32_t nowMs = 0U;
    earnPermission(sequencer, nowMs);

    ev::ReversalStatus status = sequencer.update(nowMs + 500U, moving(2.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.stopVerified());
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // One stopped sample does not revive the old permission: the verifier was
    // reset, so the dwell starts over.
    status = sequencer.update(nowMs + 550U, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // The full dwell re-earns it.
    status = sequencer.update(nowMs + 750U, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());
}

void test_unavailable_or_malformed_evidence_invalidates_the_permission(void) {
    // Absence of evidence is not evidence of a stop: a permission earned on
    // live evidence must not survive the evidence going away, and a malformed
    // speed must not gate it either.
    {
        ev::ReversalSequencer sequencer(fixtureConfig());
        sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse,
                                0U);
        uint32_t nowMs = 0U;
        earnPermission(sequencer, nowMs);
        ev::NearZeroEvidence unavailable;
        unavailable.velocityUsable = false;
        unavailable.velocityMps = 0.0;
        unavailable.zeroSpeedClaimed = true;
        const ev::ReversalStatus status = sequencer.update(nowMs + 50U, unavailable);
        expectPhase(ev::ReversalPhase::RampingDown, status);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }
    {
        // Within threshold but the estimator never claimed the count stopped:
        // a creeping vehicle, not a verified stop.
        ev::ReversalSequencer sequencer(fixtureConfig());
        sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse,
                                0U);
        uint32_t nowMs = 0U;
        earnPermission(sequencer, nowMs);
        const ev::ReversalStatus status = sequencer.update(nowMs + 50U, nearZero(false));
        expectPhase(ev::ReversalPhase::RampingDown, status);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }
    {
        // A non-finite speed with a claim is malformed evidence, not a stop.
        ev::ReversalSequencer sequencer(fixtureConfig());
        sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse,
                                0U);
        uint32_t nowMs = 0U;
        earnPermission(sequencer, nowMs);
        ev::NearZeroEvidence malformed = nearZero(true);
        malformed.velocityMps = std::numeric_limits<double>::quiet_NaN();
        const ev::ReversalStatus status = sequencer.update(nowMs + 50U, malformed);
        expectPhase(ev::ReversalPhase::RampingDown, status);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }
    {
        // Above the threshold even with a claim: fresh motion, not a stop.
        ev::ReversalSequencer sequencer(fixtureConfig());
        sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse,
                                0U);
        uint32_t nowMs = 0U;
        earnPermission(sequencer, nowMs);
        ev::NearZeroEvidence contradictory = moving(1.0);
        contradictory.zeroSpeedClaimed = true;
        const ev::ReversalStatus status = sequencer.update(nowMs + 50U, contradictory);
        expectPhase(ev::ReversalPhase::RampingDown, status);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }
}

void test_a_repeated_update_with_the_same_stamp_holds_the_permission(void) {
    // A frozen or repeated stamp is a timing artifact, not fresh motion: the
    // permission gates on the evidence facts, so re-reading the same stopped
    // evidence at the same stamp holds it.
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    uint32_t nowMs = 0U;
    earnPermission(sequencer, nowMs);
    const ev::ReversalStatus status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());
}

void test_an_expired_permission_refuses_even_with_perfect_evidence(void) {
    // The sequence timeout bounds an already-earned permission too: the
    // handoff must happen inside the budget, not whenever a historical latch
    // is read. Fixture budget is 3 s from the begin at 0.
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    uint32_t nowMs = 0U;
    earnPermission(sequencer, nowMs);

    // Just inside the bound with stopped evidence: still ready.
    ev::ReversalStatus status = sequencer.update(2999U, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());

    // Exactly at the bound: refused, even with perfect evidence.
    status = sequencer.update(3000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
}

void test_millis_wrap_keeps_a_short_interval_and_refuses_a_long_one(void) {
    // Unsigned wraparound arithmetic: a legitimate short interval across the
    // wrap holds the permission, while an interval past the budget refuses.
    const uint32_t startMs = 0xFFFFFF00U;
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse,
                            startMs);

    uint32_t nowMs = startMs;
    earnPermission(sequencer, nowMs);
    // nowMs wrapped past zero during the earn; the elapsed span is still 300.
    ev::ReversalStatus status = sequencer.update(nowMs + 10U, nearZero(true));
    expectPhase(ev::ReversalPhase::DirectionChangeReady, status);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());

    // 3000 ms past the begin is past the budget even across the wrap.
    status = sequencer.update(startMs + 3000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
}

// --- Refusals ---------------------------------------------------------------

void test_a_premature_request_ramps_down_first(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::Started),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U)));

    // Asking for the reversal while the vehicle is at speed buys nothing: the
    // sequence stays in the ramp-down phase for as long as it is still moving.
    uint32_t nowMs = 0U;
    for (uint32_t step = 0U; step < 20U; ++step) {
        nowMs += 50U;
        const ev::ReversalStatus status = sequencer.update(nowMs, moving(2.0));
        expectPhase(ev::ReversalPhase::RampingDown, status);
        TEST_ASSERT_FALSE(status.stopVerified);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }

    earnPermission(sequencer, nowMs);
    TEST_ASSERT_TRUE(sequencer.directionChangePermitted());
}

void test_a_second_request_does_not_restart_the_sequence(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::Started),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U)));

    uint32_t nowMs = 2900U;
    ev::ReversalStatus status = sequencer.update(nowMs, moving(2.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);

    // A caller that keeps asking cannot buy more time: the sequence is not
    // restarted, so its timeout still expires.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::AlreadyActive),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, nowMs)));

    nowMs = 3000U;
    status = sequencer.update(nowMs, moving(2.0));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
}

void test_a_never_reached_near_zero_times_out(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 1000U);

    uint32_t nowMs = 1000U;
    ev::ReversalStatus status = sequencer.update(nowMs, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    for (uint32_t step = 0U; step < 39U; ++step) {
        nowMs += 50U;
        status = sequencer.update(nowMs, moving(1.0));
        expectPhase(ev::ReversalPhase::RampingDown, status);
    }
    TEST_ASSERT_EQUAL_UINT32(1950U, status.sequenceElapsedMs);

    nowMs = 4000U;
    status = sequencer.update(nowMs, moving(1.0));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.stopVerified());
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // The refusal is latched: it does not clear because the vehicle later stops,
    // and a new attempt can only be made after an explicit reset.
    nowMs = 5000U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::StopLatched),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, nowMs)));
}

void test_stale_velocity_never_earns_the_reversal(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    // No fresh estimate at all, however long the sequence runs: the near zero is
    // never observed, so the reversal is never permitted.
    ev::NearZeroEvidence stale;
    stale.velocityUsable = false;
    stale.velocityMps = 0.0;
    stale.zeroSpeedClaimed = true;

    uint32_t nowMs = 0U;
    ev::ReversalStatus status = sequencer.update(nowMs, stale);
    expectPhase(ev::ReversalPhase::RampingDown, status);
    for (uint32_t step = 0U; step < 80U; ++step) {
        nowMs += 50U;
        status = sequencer.update(nowMs, stale);
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    }
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
}

void test_oscillation_around_the_threshold_times_out(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    uint32_t nowMs = 0U;
    ev::ReversalStatus status = sequencer.update(nowMs, nearZero(true));
    for (uint32_t step = 0U; step < 100U; ++step) {
        nowMs += 50U;
        const bool above = (step % 2U) == 0U;
        status = sequencer.update(nowMs, above ? moving(0.06) : nearZero(true));
        TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
        TEST_ASSERT_FALSE(sequencer.stopVerified());
    }

    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(status.stopVerified);
}

void test_the_timeout_covers_the_whole_sequence(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    // A slow ramp-down that leaves only a sliver of the configured bound: the
    // dwell cannot be finished inside the remaining time, so the sequence
    // refuses on schedule instead of extending its own deadline.
    uint32_t nowMs = 0U;
    ev::ReversalStatus status = sequencer.update(nowMs, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    nowMs = 2900U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);
    TEST_ASSERT_EQUAL_UINT32(0U, status.verification.dwellElapsedMs);

    nowMs = 3100U;
    status = sequencer.update(nowMs, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    TEST_ASSERT_FALSE(sequencer.stopVerified());
}

void test_a_stalled_loop_refuses_instead_of_verifying(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    // The dwell has started, and then the caller does not come back until well
    // past the configured bound. The timeout is evaluated before the
    // verification step, so the missing time is refused rather than credited to
    // a stop nobody watched.
    ev::ReversalStatus status = sequencer.update(0U, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);

    status = sequencer.update(5000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // The same is true of a gap inside the ramp-down phase: the reversal is
    // refused, never permitted from a single late update.
    ev::ReversalSequencer rampDown(fixtureConfig());
    rampDown.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);
    status = rampDown.update(0U, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    status = rampDown.update(9000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
}

void test_a_refusal_keeps_reporting_the_time_it_was_given(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 1000U);

    // Never near zero, so the sequence runs out its whole configured bound.
    ev::ReversalStatus status = sequencer.update(2000U, moving(1.5));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    TEST_ASSERT_EQUAL_UINT32(1000U, status.sequenceElapsedMs);

    status = sequencer.update(4000U, moving(1.5));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_EQUAL_UINT32(3000U, status.sequenceElapsedMs);

    // Reading the refusal later still reports how long the sequence was given,
    // rather than collapsing to nothing: the refusal is diagnostic evidence, and
    // evidence that evaporates on the next cycle is not evidence.
    status = sequencer.update(9000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, status);
    expectRefusal(ev::ReversalRefusal::TimedOut, status);
    TEST_ASSERT_EQUAL_UINT32(3000U, status.sequenceElapsedMs);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // A refusal raised before any sequence began reports no elapsed time,
    // because there is none to report.
    ev::ReversalSequencer unusable(
        ev::ReversalConfig(stopConfig(provisional(0.05), measured(0.2)), measured(3.0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::ConfigIsNotUsable),
                          static_cast<int>(unusable.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 1000U)));
    const ev::ReversalStatus noSequence = unusable.update(9000U, nearZero(true));
    expectPhase(ev::ReversalPhase::StopRequired, noSequence);
    expectRefusal(ev::ReversalRefusal::ConfigIsNotUsable, noSequence);
    TEST_ASSERT_EQUAL_UINT32(0U, noSequence.sequenceElapsedMs);
}

// --- Reset ------------------------------------------------------------------

void test_reset_ends_a_latched_refusal(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    const ev::ReversalStatus refused = sequencer.update(4000U, moving(1.0));
    expectPhase(ev::ReversalPhase::StopRequired, refused);

    sequencer.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(sequencer.phase()));
    TEST_ASSERT_FALSE(sequencer.stopVerified());
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());

    // A fresh sequence may now be started, and it must earn its own stop.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalBeginStatus::Started),
                          static_cast<int>(sequencer.beginReversal(
                              ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 5000U)));
    ev::ReversalStatus status = sequencer.update(5000U, moving(1.0));
    expectPhase(ev::ReversalPhase::RampingDown, status);
    expectRefusal(ev::ReversalRefusal::None, status);
}

void test_reset_clears_an_earned_permission(void) {
    ev::ReversalSequencer sequencer(fixtureConfig());
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 0U);

    uint32_t nowMs = 0U;
    earnPermission(sequencer, nowMs);
    TEST_ASSERT_TRUE(sequencer.stopVerified());

    sequencer.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(sequencer.phase()));
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
    TEST_ASSERT_FALSE(sequencer.stopVerified());

    // The permission does not survive a reset: the next reversal starts from the
    // ramp-down, whatever the vehicle did before.
    sequencer.beginReversal(ev::MotionDirection::Forward, ev::MotionDirection::Reverse, 6000U);
    const ev::ReversalStatus status = sequencer.update(6000U, nearZero(true));
    expectPhase(ev::ReversalPhase::VerifyingNearZero, status);
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
}

// --- R2/M3: reversal timeout/dwell headroom ----------------------------------
//
// The reversal timeout bounds the whole sequence - ramp-down, brake command
// and dwell verification together - and the timeout is evaluated before the
// verification step on every update. A timeout at or below the dwell it has
// to verify therefore refuses before the stop can ever be earned: the
// outcome stays safe (StopRequired) but the configuration is impossible, so
// validation must refuse it. The fixture dwell below is 0.2 s.

ev::ReversalConfig reversalConfigWithTimeout(double timeoutS) {
    return ev::ReversalConfig(stopConfig(measured(0.05), measured(0.2)), measured(timeoutS));
}

void test_a_reversal_timeout_at_or_below_its_dwell_is_rejected(void) {
    const double impossible[] = {0.2, 0.1, 0.05, 0.001};
    for (double timeoutS : impossible) {
        const ev::ReversalConfigValidation verdict =
            ev::validateReversalConfig(reversalConfigWithTimeout(timeoutS));
        TEST_ASSERT_FALSE(verdict.ok);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(ev::ReversalConfigError::TimeoutWithoutDwellHeadroom),
            static_cast<int>(verdict.error));
    }
}

void test_a_reversal_timeout_just_above_its_dwell_is_accepted(void) {
    TEST_ASSERT_TRUE(ev::validateReversalConfig(reversalConfigWithTimeout(0.201)).ok);
    TEST_ASSERT_TRUE(ev::validateReversalConfig(reversalConfigWithTimeout(3.0)).ok);
}

void test_reversal_timeout_headroom_keeps_existing_refusals_first(void) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::ReversalConfigError::NonFiniteTimeout),
        static_cast<int>(
            ev::validateReversalConfig(reversalConfigWithTimeout(std::nan(""))).error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::ReversalConfigError::NonPositiveTimeout),
        static_cast<int>(ev::validateReversalConfig(reversalConfigWithTimeout(0.0)).error));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::ReversalConfigError::NonPositiveTimeout),
        static_cast<int>(ev::validateReversalConfig(reversalConfigWithTimeout(-2.0)).error));
}

void test_an_impossible_reversal_timeout_cannot_be_sequenced(void) {
    // Belt and braces at the sequencer: a config the validator refuses also
    // refuses at begin, immediately and latched, rather than timing out.
    ev::ReversalSequencer sequencer(reversalConfigWithTimeout(0.1));
    TEST_ASSERT_FALSE(sequencer.configUsable());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::ReversalBeginStatus::ConfigIsNotUsable),
        static_cast<int>(sequencer.beginReversal(ev::MotionDirection::Forward,
                                                ev::MotionDirection::Reverse, 1000U)));
    expectPhase(ev::ReversalPhase::StopRequired, sequencer.update(1010U, nearZero(true)));
    TEST_ASSERT_FALSE(sequencer.directionChangePermitted());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_config_refuses_placeholders_and_degenerate_values);
    RUN_TEST(test_fresh_sequencer_is_idle_and_permits_nothing);
    RUN_TEST(test_a_reversal_to_the_same_direction_is_not_sequenced);
    RUN_TEST(test_an_unusable_configuration_refuses_immediately_and_latches);
    RUN_TEST(test_nominal_sequence_reaches_the_permission);
    RUN_TEST(test_moving_evidence_after_the_stop_invalidates_the_permission);
    RUN_TEST(test_unavailable_or_malformed_evidence_invalidates_the_permission);
    RUN_TEST(test_a_repeated_update_with_the_same_stamp_holds_the_permission);
    RUN_TEST(test_an_expired_permission_refuses_even_with_perfect_evidence);
    RUN_TEST(test_millis_wrap_keeps_a_short_interval_and_refuses_a_long_one);
    RUN_TEST(test_a_premature_request_ramps_down_first);
    RUN_TEST(test_a_second_request_does_not_restart_the_sequence);
    RUN_TEST(test_a_never_reached_near_zero_times_out);
    RUN_TEST(test_stale_velocity_never_earns_the_reversal);
    RUN_TEST(test_oscillation_around_the_threshold_times_out);
    RUN_TEST(test_the_timeout_covers_the_whole_sequence);
    RUN_TEST(test_a_stalled_loop_refuses_instead_of_verifying);
    RUN_TEST(test_a_refusal_keeps_reporting_the_time_it_was_given);
    RUN_TEST(test_reset_ends_a_latched_refusal);
    RUN_TEST(test_reset_clears_an_earned_permission);
    RUN_TEST(test_a_reversal_timeout_at_or_below_its_dwell_is_rejected);
    RUN_TEST(test_a_reversal_timeout_just_above_its_dwell_is_accepted);
    RUN_TEST(test_reversal_timeout_headroom_keeps_existing_refusals_first);
    RUN_TEST(test_an_impossible_reversal_timeout_cannot_be_sequenced);
    return UNITY_END();
}
