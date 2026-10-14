#include <cmath>
#include <cstdint>
#include <limits>

#include <unity.h>

#include <CompetitionStateMachine.h>
#include <EncoderProgressMonitor.h>
#include <Motion.h>
#include <Units.h>
#include <VehicleState.h>

// Contract tests for the encoder-progress supervision of accepted powered
// travel (S18C/P4). Every timeout and polarity here is a synthetic host-only
// fixture: `Measured` so the production validators accept it, never a
// production promotion and never a physical claim. The real hardware timeout
// is unmeasured (P-H5/H7/H9).

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Synthetic fixture bound: 0.5 s, i.e. 25 control periods at the 20 ms
// cadence. Well above cadence jitter and the dwell-gap limit, well below any
// phase bound: a pure software exposure budget, not a physical claim.
const double kFixtureTimeoutS = 0.5;
const uint32_t kPeriodMs = 20U;

ev::Sourced<double> measuredTimeout(double seconds) {
    return ev::Sourced<double>(seconds, ev::ValueSource::Measured);
}

ev::Sourced<double> provisionalTimeout(double seconds) {
    return ev::Sourced<double>(seconds, ev::ValueSource::Provisional);
}

ev::Sourced<ev::MotionDirection> measuredTicks(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Measured);
}

ev::Sourced<ev::MotionDirection> provisionalTicks(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Provisional);
}

const ev::Sourced<double> kTimeout = measuredTimeout(kFixtureTimeoutS);
const ev::Sourced<ev::MotionDirection> kForwardTicks =
    measuredTicks(ev::MotionDirection::Forward);

// Steps the monitor through `cycles` powered forward cycles with the same tick
// count and returns the last violation verdict.
bool stepFrozen(ev::EncoderProgressMonitor& monitor, int32_t ticks, uint32_t cycles,
                uint32_t dtMs = kPeriodMs) {
    bool violated = false;
    for (uint32_t cycle = 0U; cycle < cycles; ++cycle) {
        violated = monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, dtMs);
    }
    return violated;
}

}  // namespace

// 1. Only a measured, finite, positive timeout bounds anything. A placeholder
// is data with no runtime effect, and a non-duration is not a bound at all.
void test_the_progress_bound_is_usable_only_for_a_measured_positive_duration(void) {
    TEST_ASSERT_TRUE(ev::encoderProgressLimitUsable(measuredTimeout(0.5)));
    TEST_ASSERT_TRUE(ev::encoderProgressLimitUsable(measuredTimeout(0.001)));

    TEST_ASSERT_FALSE(ev::encoderProgressLimitUsable(provisionalTimeout(0.5)));
    TEST_ASSERT_FALSE(
        ev::encoderProgressLimitUsable(ev::Sourced<double>(0.5, ev::ValueSource::RuleDerived)));
    TEST_ASSERT_FALSE(
        ev::encoderProgressLimitUsable(ev::Sourced<double>(0.5, ev::ValueSource::Derived)));

    TEST_ASSERT_FALSE(ev::encoderProgressLimitUsable(measuredTimeout(kNaN)));
    TEST_ASSERT_FALSE(ev::encoderProgressLimitUsable(measuredTimeout(kInf)));
    TEST_ASSERT_FALSE(ev::encoderProgressLimitUsable(measuredTimeout(0.0)));
    TEST_ASSERT_FALSE(ev::encoderProgressLimitUsable(measuredTimeout(-1.0)));
}

// 2. Only a measured, known tick polarity decides an expected direction. An
// unmeasured or unknown encoding agrees with nothing, so the monitor pauses
// rather than guessing a sign.
void test_the_progress_polarity_is_usable_only_for_a_measured_known_direction(void) {
    TEST_ASSERT_TRUE(
        ev::encoderProgressPolarityUsable(measuredTicks(ev::MotionDirection::Forward)));
    TEST_ASSERT_TRUE(
        ev::encoderProgressPolarityUsable(measuredTicks(ev::MotionDirection::Reverse)));

    TEST_ASSERT_FALSE(
        ev::encoderProgressPolarityUsable(provisionalTicks(ev::MotionDirection::Forward)));
    TEST_ASSERT_FALSE(ev::encoderProgressPolarityUsable(ev::Sourced<ev::MotionDirection>(
        static_cast<ev::MotionDirection>(255), ev::ValueSource::Measured)));
}

// 3. STARTUP: a newly accepted forward drive without an edge owns the full
// allowance from the first powered cycle, then faults when it expires with no
// progress. 25 periods of 20 ms make 500 ms; the 25th powered cycle without
// progress is already over the inclusive bound.
void test_a_new_powered_attempt_without_an_edge_faults_at_the_bound(void) {
    ev::EncoderProgressMonitor monitor;
    // First powered cycle takes the reference; 23 more stay inside the bound.
    TEST_ASSERT_FALSE(stepFrozen(monitor, 1000, 24U));
    TEST_ASSERT_EQUAL_UINT32(23U * kPeriodMs, monitor.poweredMsWithoutProgress());
    // The 26th powered cycle without progress reaches the 500 ms bound.
    TEST_ASSERT_FALSE(stepFrozen(monitor, 1000, 1U));
    TEST_ASSERT_TRUE(stepFrozen(monitor, 1000, 1U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(ev::encoderProgressFault(true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(ev::encoderProgressFault(false)));
}

// 4. STARTUP: a delayed first edge before expiry discharges the interval: the
// accumulation restarts and the run continues without a fault.
void test_a_first_edge_before_expiry_discharges_the_startup_allowance(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(stepFrozen(monitor, 1000, 20U));
    TEST_ASSERT_FALSE(monitor.update(1001, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_EQUAL_INT32(1001, monitor.referenceTicks());
    TEST_ASSERT_FALSE(stepFrozen(monitor, 1001, 24U));
}

// 5. TIMING: one millisecond before the bound holds, the bound itself faults.
// A 499 ms timeout against 499 ms of powered time without progress is still
// inside; the next millisecond crosses it.
void test_the_bound_is_inclusive_at_the_configured_timeout(void) {
    ev::EncoderProgressMonitor narrow;
    const ev::Sourced<double> timeout = measuredTimeout(0.499);
    TEST_ASSERT_FALSE(narrow.update(0, true, true, true, kForwardTicks, timeout, 0U));
    for (uint32_t cycle = 0U; cycle < 24U; ++cycle) {
        TEST_ASSERT_FALSE(
            narrow.update(0, true, true, true, kForwardTicks, timeout, kPeriodMs));
    }
    // 480 ms accumulated: inside the 499 ms bound.
    TEST_ASSERT_FALSE(
        narrow.update(0, true, true, true, kForwardTicks, timeout, 18U));
    TEST_ASSERT_EQUAL_UINT32(498U, narrow.poweredMsWithoutProgress());
    // One more millisecond reaches it.
    TEST_ASSERT_TRUE(narrow.update(0, true, true, true, kForwardTicks, timeout, 1U));
}

// 6. TIMING: a large scheduling gap consumes the allowance at once. A 5 s gap
// with no progress faults on the next powered cycle rather than being
// forgiven.
void test_a_large_scheduling_gap_without_progress_faults_at_once(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(500, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_TRUE(monitor.update(500, true, true, true, kForwardTicks, kTimeout, 5000U));
}

// 7. TIMING: a repeated same-timestamp observation (dt 0) accumulates nothing
// and faults nothing. A stalled clock is not motion evidence in either
// direction.
void test_a_repeated_stamp_accumulates_no_powered_time(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(700, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    for (uint32_t cycle = 0U; cycle < 100U; ++cycle) {
        TEST_ASSERT_FALSE(monitor.update(700, true, true, true, kForwardTicks, kTimeout, 0U));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
}

// 8. FORWARD: healthy correct-direction ticks never fault, however long the
// travel takes. 200 ticks per cycle for 1000 cycles is continuous progress.
void test_healthy_forward_progress_never_faults(void) {
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 0;
    TEST_ASSERT_FALSE(monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 1000U; ++cycle) {
        ticks += 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
}

// 9. FORWARD: frozen counts with fresh reads fault. Fresh samples of an
// unchanged count are not progress: 50 cycles at 20 ms reach the 500 ms
// software reproduction window with the bound already crossed at 25.
void test_frozen_counts_with_fresh_reads_fault(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(9000, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    bool violated = false;
    uint32_t cyclesToFault = 0U;
    for (uint32_t cycle = 0U; cycle < 50U; ++cycle) {
        violated = monitor.update(9000, true, true, true, kForwardTicks, kTimeout, kPeriodMs);
        if (violated) {
            cyclesToFault = cycle + 1U;
            break;
        }
    }
    TEST_ASSERT_TRUE(violated);
    TEST_ASSERT_EQUAL_UINT32(25U, cyclesToFault);
}

// 10. FORWARD: intermittent real progress keeps the run alive. One tick every
// 20 cycles is slow but genuine, and each advance restarts the allowance.
void test_intermittent_real_progress_keeps_the_run_alive(void) {
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 0;
    TEST_ASSERT_FALSE(monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        if (cycle % 20U == 19U) {
            ++ticks;
        }
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
}

// 11. FORWARD: stale previously healthy evidence is not unlimited permission.
// Old movement stops protecting the run once the allowance since the last
// progress expires.
void test_old_movement_does_not_grant_unlimited_future_travel(void) {
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 0;
    TEST_ASSERT_FALSE(monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 100U; ++cycle) {
        ticks += 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_TRUE(stepFrozen(monitor, ticks, 25U));
}

// 12. FORWARD: contradictory direction evidence never resets the interval. A
// backwards tick move under forward power is not progress; only the expected
// sign counts.
void test_wrong_sign_ticks_never_reset_the_interval(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(
        monitor.update(1000, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 24U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(1000 - static_cast<int32_t>(cycle + 1), true, true, true,
                           kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_INT32(1000, monitor.referenceTicks());
    TEST_ASSERT_TRUE(
        monitor.update(1000 - 25, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
}

// 13. REVERSE: healthy negative-direction progress never faults.
void test_healthy_reverse_progress_never_faults(void) {
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 50000;
    TEST_ASSERT_FALSE(
        monitor.update(ticks, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 500U; ++cycle) {
        ticks -= 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
}

// 14. REVERSE: a frozen reverse count faults with the same identity.
void test_a_frozen_reverse_count_faults(void) {
    ev::EncoderProgressMonitor reverse;
    TEST_ASSERT_FALSE(
        reverse.update(3000, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
    bool violated = false;
    for (uint32_t cycle = 0U; cycle < 25U; ++cycle) {
        violated =
            reverse.update(3000, true, true, false, kForwardTicks, kTimeout, kPeriodMs);
    }
    TEST_ASSERT_TRUE(violated);
}

// 15. REVERSE: forward ticks during a reverse leg are wrong-sign evidence and
// never reset the interval.
void test_forward_ticks_during_reverse_never_reset_the_interval(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(
        monitor.update(0, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 24U; ++cycle) {
        TEST_ASSERT_FALSE(monitor.update(static_cast<int32_t>(cycle + 1), true, true, false,
                                         kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_TRUE(
        monitor.update(25, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
}

// 16. REVERSAL: a valid handoff with movement continues without a fault. The
// new leg's first advance in its own direction re-baselines the monitor.
void test_a_moving_reversal_handoff_continues_without_fault(void) {
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 0;
    TEST_ASSERT_FALSE(monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 10U; ++cycle) {
        ticks += 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    // Dwell: unpowered, no accumulation, reference follows.
    for (uint32_t cycle = 0U; cycle < 10U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, false, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    // Reverse leg moves immediately.
    for (uint32_t cycle = 0U; cycle < 50U; ++cycle) {
        ticks -= 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, false, kForwardTicks, kTimeout, kPeriodMs));
    }
}

// 17. DWELL: intentional stationary periods never fault. Disabled cycles,
// brake holds and verification dwells pause the monitor however long they
// last, and the next powered travel owns a full allowance only after genuine
// progress - while a stationary resume keeps the old accumulation.
void test_intentional_stationary_periods_pause_without_fault(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(0, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 1000U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, true, false, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    // Resume powered and stationary: the old accumulation continues, so the
    // remaining allowance - not a fresh one - is what is left.
    TEST_ASSERT_FALSE(stepFrozen(monitor, 0, 24U));
    TEST_ASSERT_TRUE(stepFrozen(monitor, 0, 1U));
}

// 18. CONFIG: an unusable bound pauses instead of faulting. A provisional,
// zero, negative, NaN or infinite timeout never stops a run; neither does an
// unmeasured polarity or a dropped sample.
void test_an_unusable_configuration_pauses_instead_of_faulting(void) {
    ev::EncoderProgressMonitor monitor;
    const ev::Sourced<double> badTimeouts[] = {
        provisionalTimeout(0.5), measuredTimeout(0.0), measuredTimeout(-1.0),
        measuredTimeout(kNaN), measuredTimeout(kInf)};
    for (const ev::Sourced<double>& bad : badTimeouts) {
        for (uint32_t cycle = 0U; cycle < 100U; ++cycle) {
            TEST_ASSERT_FALSE(
                monitor.update(0, true, true, true, kForwardTicks, bad, kPeriodMs));
        }
    }
    for (uint32_t cycle = 0U; cycle < 100U; ++cycle) {
        TEST_ASSERT_FALSE(monitor.update(0, true, true, true,
                                         provisionalTicks(ev::MotionDirection::Forward),
                                         kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
}
// R2/S18R-M1: an unavailable sample with a usable bound is sustained-invalid
// exposure, not a missing configuration - the case the old suite paused on is
// now bounded below. A placeholder bound or polarity still pauses (above).
void test_persistently_invalid_feedback_faults_at_the_bound(void) {
    // The first invalid powered cycle baselines the startup allowance (no
    // instant fault on invalidity immediately after arming); every one after
    // it consumes the bound exactly like a frozen count.
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    for (uint32_t cycle = 0U; cycle < 24U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(480U, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_TRUE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
}

void test_a_single_invalid_sample_does_not_fault(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_FALSE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(20U, monitor.poweredMsWithoutProgress());
}

void test_alternating_invalid_and_frozen_feedback_still_faults(void) {
    // Invalid and frozen are both powered cycles without progress evidence:
    // they share one accumulator, so alternating them cannot renew power.
    ev::EncoderProgressMonitor monitor;
    bool violated = false;
    for (uint32_t cycle = 0U; cycle < 26U; ++cycle) {
        const bool fresh = (cycle % 2U) == 0U;
        violated = monitor.update(0, fresh, true, true, kForwardTicks, kTimeout, kPeriodMs);
    }
    TEST_ASSERT_TRUE(violated);
}

void test_genuine_progress_after_invalid_discharges_the_allowance(void) {
    ev::EncoderProgressMonitor monitor;
    for (uint32_t cycle = 0U; cycle < 10U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_FALSE(monitor.update(1, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_FALSE(stepFrozen(monitor, 1, 24U));
    TEST_ASSERT_TRUE(stepFrozen(monitor, 1, 1U));
}

void test_unpowered_invalid_feedback_never_faults(void) {
    // A refused request or a deliberate stop is not accepted power: invalid
    // observations while stationary accumulate nothing and fault nothing.
    ev::EncoderProgressMonitor monitor;
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, false, false, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
}

void test_a_direction_flip_does_not_evict_the_invalid_exposure(void) {
    ev::EncoderProgressMonitor monitor;
    for (uint32_t cycle = 0U; cycle < 15U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
    for (uint32_t cycle = 0U; cycle < 10U; ++cycle) {
        TEST_ASSERT_FALSE(
            monitor.update(0, false, true, false, kForwardTicks, kTimeout, kPeriodMs));
    }
    TEST_ASSERT_TRUE(
        monitor.update(0, false, true, false, kForwardTicks, kTimeout, kPeriodMs));
}

void test_an_invalid_gap_longer_than_the_bound_faults_at_once(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(0, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_TRUE(monitor.update(0, false, true, true, kForwardTicks, kTimeout, 5000U));
}

void test_repeated_short_dropouts_without_progress_fault(void) {
    // Three missed samples and one frozen-but-valid read, repeated: no cycle
    // carries progress, so the allowance drains and the monitor faults.
    ev::EncoderProgressMonitor monitor;
    bool violated = false;
    for (uint32_t round = 0U; round < 10U && !violated; ++round) {
        for (uint32_t cycle = 0U; cycle < 3U; ++cycle) {
            violated = monitor.update(0, false, true, true, kForwardTicks, kTimeout, kPeriodMs);
        }
        violated = monitor.update(0, true, true, true, kForwardTicks, kTimeout, kPeriodMs);
    }
    TEST_ASSERT_TRUE(violated);
}

void test_short_dropouts_with_real_progress_between_them_never_fault(void) {
    // The complementary case: dropouts that genuine tick advances separate
    // are renewed by evidence, not by time, and never fault.
    ev::EncoderProgressMonitor monitor;
    int32_t ticks = 0;
    for (uint32_t round = 0U; round < 50U; ++round) {
        for (uint32_t cycle = 0U; cycle < 3U; ++cycle) {
            TEST_ASSERT_FALSE(
                monitor.update(ticks, false, true, true, kForwardTicks, kTimeout, kPeriodMs));
        }
        ++ticks;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    }
}

void test_invalid_exposure_saturates_instead_of_wrapping(void) {
    // The accumulator measures powered exposure with dtMs only - no clock
    // stamp enters it, so the uint32 millis wrap cannot disturb it - and it
    // saturates at the largest representable count rather than wrapping to a
    // small one that would renew power.
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(
        monitor.update(0, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_TRUE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_TRUE(
        monitor.update(0, false, true, true, kForwardTicks, kTimeout, 0xFFFFFFFFu));
}

// 19. RECOVERY: reset() clears the accumulation and the reference. A new run
// after a fault owns a fresh allowance and proves progress anew.
void test_reset_clears_the_accumulation_and_the_reference(void) {
    ev::EncoderProgressMonitor monitor;
    TEST_ASSERT_FALSE(monitor.update(0, true, true, true, kForwardTicks, kTimeout, kPeriodMs));
    TEST_ASSERT_TRUE(stepFrozen(monitor, 0, 25U));
    monitor.reset();
    TEST_ASSERT_EQUAL_UINT32(0U, monitor.poweredMsWithoutProgress());
    TEST_ASSERT_FALSE(stepFrozen(monitor, 5000, 25U));
    TEST_ASSERT_TRUE(stepFrozen(monitor, 5000, 1U));
}

// 20. CONFIG: the run policy refuses a placeholder, zero, negative,
// non-finite, or unknown-direction feedback bound, and accepts a synthetic
// measured one. Production stays unarmable on the placeholder.
void test_the_run_policy_refuses_an_unusable_feedback_bound(void) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunPhaseConfigError::NonPositiveEncoderProgress),
        static_cast<int>(ev::validateRunPhaseConfig(ev::RunPhaseConfig(
                                                       ev::Sourced<double>(
                                                           1.0, ev::ValueSource::Measured),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(1.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(2.0),
                                                       measuredTimeout(0.0),
                                                       measuredTimeout(1.0),
                                                       ev::NearZeroConfig(
                                                           measuredTimeout(0.05),
                                                           measuredTimeout(0.2),
                                                           ev::Sourced<uint32_t>(
                                                               250U, ev::ValueSource::Measured)),
                                                       measuredTimeout(0.10),
                                                       measuredTimeout(0.30),
                                                       measuredTimeout(0.30),
                                                       measuredTimeout(0.30),
                                                       ev::BottleEvidenceConfig(
                                                           measuredTimeout(0.10),
                                                           measuredTimeout(0.20),
                                                           measuredTimeout(0.05))))
                          .error));
    // A measured zero is refused as non-positive rather than as unmeasured.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunPhaseConfigError::NonPositiveEncoderProgress),
        static_cast<int>(ev::validateRunPhaseConfig(ev::RunPhaseConfig(
                                                       ev::Sourced<double>(
                                                           1.0, ev::ValueSource::Measured),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(1.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(5.0),
                                                       measuredTimeout(2.0),
                                                       measuredTimeout(0.0),
                                                       measuredTimeout(1.0),
                                                       ev::NearZeroConfig(
                                                           measuredTimeout(0.05),
                                                           measuredTimeout(0.2),
                                                           ev::Sourced<uint32_t>(
                                                               250U, ev::ValueSource::Measured)),
                                                       measuredTimeout(0.10),
                                                       measuredTimeout(0.30),
                                                       measuredTimeout(0.30),
                                                       measuredTimeout(0.30),
                                                       ev::BottleEvidenceConfig(
                                                           measuredTimeout(0.10),
                                                           measuredTimeout(0.20),
                                                           measuredTimeout(0.05))))
                          .error));
    // A fully measured synthetic fixture - the shape every host test uses -
    // is accepted, so the new field adds no refusal to valid configurations.
    TEST_ASSERT_TRUE(ev::validateRunPhaseConfig(ev::RunPhaseConfig(
                                                    ev::Sourced<double>(
                                                        1.0, ev::ValueSource::Measured),
                                                    measuredTimeout(5.0),
                                                    measuredTimeout(1.0),
                                                    measuredTimeout(5.0),
                                                    measuredTimeout(5.0),
                                                    measuredTimeout(5.0),
                                                    measuredTimeout(5.0),
                                                    measuredTimeout(2.0),
                                                    measuredTimeout(0.5),
                                                    measuredTimeout(1.0),
                                                    ev::NearZeroConfig(
                                                        measuredTimeout(0.05),
                                                        measuredTimeout(0.2),
                                                        ev::Sourced<uint32_t>(
                                                            250U, ev::ValueSource::Measured)),
                                                    measuredTimeout(0.10),
                                                    measuredTimeout(0.30),
                                                    measuredTimeout(0.30),
                                                    measuredTimeout(0.30),
                                                    ev::BottleEvidenceConfig(
                                                        measuredTimeout(0.10),
                                                        measuredTimeout(0.20),
                                                        measuredTimeout(0.05))))
                     .ok);
}

// 21. The expected tick sign follows the leg through the measured polarity:
// forward with forward-positive ticks expects rising counts; every flip of
// leg or polarity flips the expectation.
void test_the_expected_tick_sign_follows_leg_and_polarity(void) {
    TEST_ASSERT_EQUAL_INT(1, ev::expectedTickSignForLeg(true, kForwardTicks));
    TEST_ASSERT_EQUAL_INT(-1, ev::expectedTickSignForLeg(false, kForwardTicks));
    TEST_ASSERT_EQUAL_INT(-1, ev::expectedTickSignForLeg(
                                  true, measuredTicks(ev::MotionDirection::Reverse)));
    TEST_ASSERT_EQUAL_INT(1, ev::expectedTickSignForLeg(
                                 false, measuredTicks(ev::MotionDirection::Reverse)));
}

// 22. A reverse-positive polarity build behaves symmetrically: forward travel
// expects falling counts and gets them without a fault.
void test_a_reverse_positive_polarity_build_counts_symmetrically(void) {
    ev::EncoderProgressMonitor monitor;
    const ev::Sourced<ev::MotionDirection> reverseTicks =
        measuredTicks(ev::MotionDirection::Reverse);
    int32_t ticks = 100000;
    TEST_ASSERT_FALSE(monitor.update(ticks, true, true, true, reverseTicks, kTimeout, kPeriodMs));
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        ticks -= 200;
        TEST_ASSERT_FALSE(
            monitor.update(ticks, true, true, true, reverseTicks, kTimeout, kPeriodMs));
    }
    // Frozen under the same polarity still faults.
    ev::EncoderProgressMonitor frozen;
    TEST_ASSERT_FALSE(
        frozen.update(ticks, true, true, true, reverseTicks, kTimeout, kPeriodMs));
    bool violated = false;
    for (uint32_t cycle = 0U; cycle < 25U; ++cycle) {
        violated = frozen.update(ticks, true, true, true, reverseTicks, kTimeout, kPeriodMs);
    }
    TEST_ASSERT_TRUE(violated);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_progress_bound_is_usable_only_for_a_measured_positive_duration);
    RUN_TEST(test_the_progress_polarity_is_usable_only_for_a_measured_known_direction);
    RUN_TEST(test_a_new_powered_attempt_without_an_edge_faults_at_the_bound);
    RUN_TEST(test_a_first_edge_before_expiry_discharges_the_startup_allowance);
    RUN_TEST(test_the_bound_is_inclusive_at_the_configured_timeout);
    RUN_TEST(test_a_large_scheduling_gap_without_progress_faults_at_once);
    RUN_TEST(test_a_repeated_stamp_accumulates_no_powered_time);
    RUN_TEST(test_healthy_forward_progress_never_faults);
    RUN_TEST(test_frozen_counts_with_fresh_reads_fault);
    RUN_TEST(test_intermittent_real_progress_keeps_the_run_alive);
    RUN_TEST(test_old_movement_does_not_grant_unlimited_future_travel);
    RUN_TEST(test_wrong_sign_ticks_never_reset_the_interval);
    RUN_TEST(test_healthy_reverse_progress_never_faults);
    RUN_TEST(test_a_frozen_reverse_count_faults);
    RUN_TEST(test_forward_ticks_during_reverse_never_reset_the_interval);
    RUN_TEST(test_a_moving_reversal_handoff_continues_without_fault);
    RUN_TEST(test_intentional_stationary_periods_pause_without_fault);
    RUN_TEST(test_an_unusable_configuration_pauses_instead_of_faulting);
    RUN_TEST(test_persistently_invalid_feedback_faults_at_the_bound);
    RUN_TEST(test_a_single_invalid_sample_does_not_fault);
    RUN_TEST(test_alternating_invalid_and_frozen_feedback_still_faults);
    RUN_TEST(test_genuine_progress_after_invalid_discharges_the_allowance);
    RUN_TEST(test_unpowered_invalid_feedback_never_faults);
    RUN_TEST(test_a_direction_flip_does_not_evict_the_invalid_exposure);
    RUN_TEST(test_an_invalid_gap_longer_than_the_bound_faults_at_once);
    RUN_TEST(test_repeated_short_dropouts_without_progress_fault);
    RUN_TEST(test_short_dropouts_with_real_progress_between_them_never_fault);
    RUN_TEST(test_invalid_exposure_saturates_instead_of_wrapping);
    RUN_TEST(test_reset_clears_the_accumulation_and_the_reference);
    RUN_TEST(test_the_run_policy_refuses_an_unusable_feedback_bound);
    RUN_TEST(test_the_expected_tick_sign_follows_leg_and_polarity);
    RUN_TEST(test_a_reverse_positive_polarity_build_counts_symmetrically);
    return UNITY_END();
}
