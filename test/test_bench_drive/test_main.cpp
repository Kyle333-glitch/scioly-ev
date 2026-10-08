#include <cstdint>

#include <unity.h>

#include <BenchDrive.h>

// Contract tests for bench instrument B2's safety model
// (lib/evcore/src/BenchDrive.h).
//
// This is the one code path in the project that commands effort outside the run,
// and the first protocol that uses it (H3) is the first powered test. The five
// properties under test are the five that make it acceptable to have at all:
//
//   * bounded by construction - a command ends on its own, with nobody watching;
//   * latched on abort - an abort stops the motor and stays stopped until a
//     deliberate reset;
//   * a deliberate arm - nothing is applied without one, nothing survives a
//     reset, and a repeated arm is refused rather than silently re-arming (M3);
//   * every limit checked before anything is applied, with a named refusal;
//   * one drive direction per deliberate arm, and a stall watch that can actually
//     fire inside one command (H2, H3).
//
// Every limit below is a test fixture. None is a recommendation for the bench
// session, and none was measured on a vehicle. The *shipped* limit set (its
// values, and the stall-window/duration relationship that makes the latch
// reachable) is pinned twice elsewhere: `src/bench/BenchConsole.h` asserts the
// relationship at compile time for all three firmware images, and the real-source
// mock-I/O probe (.agent-work/reviews/s14-2026-10-07) drives the actual console
// dispatch with the shipped numbers.
//
// These tests prove logic only. A "Driving" state here is not evidence that any
// motor turns, and no voltage was ever applied.

void setUp(void) {}
void tearDown(void) {}

namespace {

const uint16_t kCeilingPermille = 150U;
const uint32_t kMaxDurationMs = 500U;
const uint32_t kStallWindowMs = 200U;
const uint32_t kMaxCommandsPerArm = 3U;

ev::BenchDriveLimits limits() {
    return ev::BenchDriveLimits(kCeilingPermille, kMaxDurationMs, kStallWindowMs,
                                kMaxCommandsPerArm);
}

// An armed instrument, which is the state every command test starts from.
ev::BenchDrive armed() {
    ev::BenchDrive drive(limits());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::None),
                          static_cast<int>(drive.arm()));
    return drive;
}

// Asserts that nothing powered can be applied right now.
void expectNothingApplied(const ev::BenchDrive& drive) {
    TEST_ASSERT_FALSE(drive.output().isDriving());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(drive.output().mode()));
    TEST_ASSERT_EQUAL_UINT16(0U, drive.output().magnitudePermille());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                          static_cast<int>(drive.output().actuationClass()));
}

}  // namespace

// --- The deliberate arm ------------------------------------------------------

// A fresh instrument commands nothing, and a command without an arm is refused
// rather than applied.
void test_a_command_without_an_arm_is_refused(void) {
    ev::BenchDrive drive(limits());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Disarmed),
                          static_cast<int>(drive.state()));
    expectNothingApplied(drive);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::NotArmed),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 100U, 100U)));
    drive.update(0U, 0);
    expectNothingApplied(drive);
}

// Nothing is applied by an arm on its own: the arm is permission, not a command.
void test_an_arm_applies_nothing_by_itself(void) {
    ev::BenchDrive drive = armed();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Armed),
                          static_cast<int>(drive.update(0U, 0)));
    expectNothingApplied(drive);
}

// M3: the arm is a transition from Disarmed only. A repeated arm is refused with
// its own reason, and - the point of the refusal - it cannot reset the per-arm
// budget or drop a command that is being applied.
void test_an_arm_is_only_admissible_from_disarmed(void) {
    ev::BenchDrive drive = armed();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::NotDisarmed),
                          static_cast<int>(drive.arm()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Armed),
                          static_cast<int>(drive.state()));

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 100U, 200U)));
    drive.update(0U, 0);
    TEST_ASSERT_TRUE(drive.output().isDriving());

    // While a command is applied, an arm is refused and the command survives.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::NotDisarmed),
                          static_cast<int>(drive.arm()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Driving),
                          static_cast<int>(drive.state()));
    TEST_ASSERT_TRUE(drive.hasCommand());
    TEST_ASSERT_TRUE(drive.output().isDriving());
    TEST_ASSERT_EQUAL_UINT16(100U, drive.output().magnitudePermille());
    TEST_ASSERT_EQUAL_UINT32(1U, drive.commandsIssued());
}

// M3: a repeated arm must not clear the per-arm command budget. The budget is
// reset only by the deliberate `reset()`, which is a new session by definition.
void test_a_repeated_arm_does_not_reset_the_per_arm_budget(void) {
    ev::BenchDrive drive = armed();
    for (uint32_t index = 0U; index < kMaxCommandsPerArm; ++index) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(ev::BenchCommandRefusal::None),
            static_cast<int>(drive.command(1000U * index, ev::MotorMode::DriveForward, 100U, 100U)));
        drive.update(1000U * index, static_cast<int32_t>(index));
        drive.update(1000U * index + 100U, static_cast<int32_t>(index));
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxCommandsPerArm, drive.commandsIssued());

    // The arm that used to erase the budget is now refused, and the budget holds.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::NotDisarmed),
                          static_cast<int>(drive.arm()));
    TEST_ASSERT_EQUAL_UINT32(kMaxCommandsPerArm, drive.commandsIssued());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::CommandBudgetExhausted),
        static_cast<int>(drive.command(9000U, ev::MotorMode::DriveForward, 100U, 100U)));
    expectNothingApplied(drive);

    // The deliberate new session - reset then arm - does start with a fresh budget.
    drive.reset();
    TEST_ASSERT_EQUAL_UINT32(0U, drive.commandsIssued());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::None),
                          static_cast<int>(drive.arm()));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(9100U, ev::MotorMode::DriveForward, 100U, 100U)));
}

// --- Bounded by construction -------------------------------------------------

// The command expires on its own, at its own duration, with nobody watching, and
// the expiry is not a fault: the instrument is still armed and can be commanded
// again deliberately.
void test_a_command_expires_on_its_own_and_latches_nothing(void) {
    ev::BenchDrive drive = armed();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(1000U, ev::MotorMode::DriveForward, 100U, 200U)));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(1000U, 0)));
    TEST_ASSERT_TRUE(drive.output().isDriving());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(drive.output().mode()));
    TEST_ASSERT_EQUAL_UINT16(100U, drive.output().magnitudePermille());

    // One millisecond before the bound it is still driving; at the bound it is not.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(1199U, 10)));
    TEST_ASSERT_TRUE(drive.output().isDriving());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(1200U, 20)));
    expectNothingApplied(drive);
    TEST_ASSERT_FALSE(drive.faultLatched());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Armed),
                          static_cast<int>(drive.state()));
}

// A gap longer than the command's own duration is still an expiry, not a stall:
// the bound is what ends it, whatever happened to the loop in between.
void test_a_long_gap_past_the_expiry_stops_rather_than_faults(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 100U);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(60000U, 0)));
    expectNothingApplied(drive);
    TEST_ASSERT_FALSE(drive.faultLatched());
}

// The clock's wraparound does not extend a command: the expiry test is a signed
// difference, so a deadline that wrapped is still a deadline.
void test_the_expiry_is_wraparound_safe(void) {
    ev::BenchDrive drive = armed();
    const uint32_t startMs = 0xFFFFFFFFu - 50U;  // 50 ms before the wrap
    drive.command(startMs, ev::MotorMode::DriveReverse, 100U, 100U);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(startMs + 50U, 5)));
    TEST_ASSERT_TRUE(drive.output().isDriving());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(startMs + 100U, 5)));
    expectNothingApplied(drive);
}

// The remaining interval counts down and never reports a stale positive number
// after the expiry.
void test_the_remaining_interval_counts_down_and_saturates(void) {
    ev::BenchDrive drive = armed();
    drive.command(1000U, ev::MotorMode::DriveForward, 100U, 300U);
    drive.update(1000U, 0);

    TEST_ASSERT_EQUAL_UINT32(300U, drive.remainingMs(1000U));
    TEST_ASSERT_EQUAL_UINT32(100U, drive.remainingMs(1200U));
    TEST_ASSERT_EQUAL_UINT32(0U, drive.remainingMs(1500U));
    drive.update(1300U, 10);
    TEST_ASSERT_EQUAL_UINT32(0U, drive.remainingMs(1300U));
}

// --- Latched on abort --------------------------------------------------------

// An abort de-energizes immediately and stays latched: a later command is refused
// until a deliberate reset, and the arm cannot be re-taken.
void test_an_abort_latches_until_a_deliberate_reset(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 120U, 400U);
    drive.update(0U, 0);
    TEST_ASSERT_TRUE(drive.output().isDriving());

    drive.abort();
    expectNothingApplied(drive);
    TEST_ASSERT_TRUE(drive.faultLatched());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Faulted),
                          static_cast<int>(drive.state()));

    // It is still latched after more updates, and every further attempt is
    // refused with the reason rather than silently ignored.
    for (uint32_t ms = 20U; ms <= 5000U; ms += 20U) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::FaultLatched),
                              static_cast<int>(drive.update(ms, 0)));
        expectNothingApplied(drive);
    }
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::FaultLatched),
        static_cast<int>(drive.command(5000U, ev::MotorMode::DriveForward, 100U, 100U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::FaultLatched),
                          static_cast<int>(drive.arm()));

    // The deliberate reset is the only way out, and it leaves the instrument
    // disarmed rather than armed: re-arming is a separate decision.
    drive.reset();
    TEST_ASSERT_FALSE(drive.faultLatched());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Disarmed),
                          static_cast<int>(drive.state()));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::NotArmed),
        static_cast<int>(drive.command(5100U, ev::MotorMode::DriveForward, 100U, 100U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::None),
                          static_cast<int>(drive.arm()));
}

// An encoder that stops changing while the motor is commanded to drive is the
// provoked fault H3 demonstrates. It latches and de-energizes.
void test_an_encoder_stall_faults_and_latches(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 500U);

    // The count advances for the first 100 ms, then holds.
    int32_t ticks = 0;
    for (uint32_t ms = 0U; ms <= 100U; ms += 20U) {
        ++ticks;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                              static_cast<int>(drive.update(ms, ticks)));
    }

    // From there nothing moves. The window is 200 ms, so the fault lands just past
    // the last observed change - and it comes before the command's own expiry.
    ev::BenchDriveStatus status = ev::BenchDriveStatus::Driving;
    for (uint32_t ms = 120U; ms <= 320U && status == ev::BenchDriveStatus::Driving; ms += 20U) {
        status = drive.update(ms, ticks);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::EncoderStallFault),
                          static_cast<int>(status));
    TEST_ASSERT_TRUE(drive.faultLatched());
    TEST_ASSERT_EQUAL_UINT32(1U, drive.stallFaults());
    expectNothingApplied(drive);

    // Latched, so it does not clear as the clock runs past the original duration.
    drive.update(2000U, ticks);
    TEST_ASSERT_TRUE(drive.faultLatched());
    expectNothingApplied(drive);
}

// A command that never produces a tick change at all is caught too, and one that
// keeps changing is never blamed.
void test_a_command_the_encoder_never_follows_faults(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveReverse, 100U, 500U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(0U, 7)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(200U, 7)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::EncoderStallFault),
                          static_cast<int>(drive.update(221U, 7)));
}

// H3: the stall window's boundary is exact. At the window the command is still
// running; one millisecond later, with no edge seen, it latches - and it latches
// strictly inside the command's own envelope, which is the property the shipped
// 1000 ms window against a 500 ms command did not have.
void test_the_stall_window_boundary_is_exact_and_inside_the_command(void) {
    ev::BenchDrive drive = armed();
    TEST_ASSERT_TRUE(ev::benchStallProtectionReachable(limits()));
    drive.command(0U, ev::MotorMode::DriveForward, 100U, kMaxDurationMs);

    // First observation establishes the "nothing has moved since" instant.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(0U, 5)));
    // Exactly the window: not yet a fault.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                          static_cast<int>(drive.update(kStallWindowMs, 5)));
    TEST_ASSERT_TRUE(drive.output().isDriving());
    // One millisecond later: the latched fault, still inside the command bound.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::EncoderStallFault),
                          static_cast<int>(drive.update(kStallWindowMs + 1U, 5)));
    TEST_ASSERT_TRUE(kStallWindowMs + 1U < kMaxDurationMs);
    expectNothingApplied(drive);
}

// H3: the reachability predicate itself, on both sides of the boundary. This is
// the assertion that would have failed for the shipped (1000, 500) limit set.
void test_the_stall_reachability_predicate(void) {
    TEST_ASSERT_TRUE(ev::benchStallProtectionReachable(ev::BenchDriveLimits(150U, 500U, 400U, 8U)));
    TEST_ASSERT_TRUE(ev::benchStallProtectionReachable(ev::BenchDriveLimits(150U, 500U, 499U, 8U)));
    TEST_ASSERT_FALSE(
        ev::benchStallProtectionReachable(ev::BenchDriveLimits(150U, 500U, 500U, 8U)));
    // The shipped pre-repair limit set: 1000 ms window against a 500 ms command.
    TEST_ASSERT_FALSE(
        ev::benchStallProtectionReachable(ev::BenchDriveLimits(150U, 500U, 1000U, 8U)));
}

void test_a_moving_encoder_never_faults(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 400U);
    int32_t ticks = 0;
    for (uint32_t ms = 0U; ms < 400U; ms += 20U) {
        ticks += 3;  // a slow but real rotation
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                              static_cast<int>(drive.update(ms, ticks)));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(400U, ticks)));
    TEST_ASSERT_FALSE(drive.faultLatched());
    TEST_ASSERT_EQUAL_UINT32(0U, drive.stallFaults());
}

// H3: a genuinely slow rotation - one edge per stall window, which is the
// pessimistic end of "healthy" - is not blamed. This is a lower bound on the
// window's tolerance, not a physical claim: whether a real rotor at the smallest
// duty produces edges this slowly is a bench question (H3), still outstanding.
void test_a_very_slow_but_real_rotation_is_not_blamed(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, kMaxDurationMs);
    int32_t ticks = 0;
    for (uint32_t ms = 0U; ms <= kMaxDurationMs; ms += 20U) {
        if (ms % kStallWindowMs == 0U) {
            ++ticks;  // one edge exactly per stall window
        }
        const ev::BenchDriveStatus status = drive.update(ms, ticks);
        TEST_ASSERT_TRUE(status == ev::BenchDriveStatus::Driving ||
                         status == ev::BenchDriveStatus::Expired);
    }
    TEST_ASSERT_FALSE(drive.faultLatched());
    TEST_ASSERT_EQUAL_UINT32(0U, drive.stallFaults());
}

// A reset clears the stall history as well as the fault, so the next session's
// counters describe the next session.
void test_reset_clears_counters_and_history(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 400U);
    drive.update(0U, 0);
    drive.update(300U, 0);
    TEST_ASSERT_TRUE(drive.faultLatched());

    drive.reset();
    TEST_ASSERT_EQUAL_UINT32(0U, drive.stallFaults());
    TEST_ASSERT_EQUAL_UINT32(0U, drive.commandsIssued());
    TEST_ASSERT_FALSE(drive.hasCommand());
    TEST_ASSERT_EQUAL_UINT32(0U, drive.remainingMs(0U));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(drive.armedDirection()));
    expectNothingApplied(drive);
}

// --- H2: one drive direction per deliberate arm ------------------------------

// The first accepted command fixes the direction for the arm. The opposite
// direction is refused with its own reason and changes nothing - no state, no
// budget, no output. This is the conservative first-bring-up policy: a timed
// command's own expiry is not evidence that the drivetrain has stopped.
void test_the_arm_fixes_one_direction(void) {
    ev::BenchDrive drive = armed();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(drive.armedDirection()));

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 100U, 100U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(drive.armedDirection()));
    drive.update(0U, 0);
    TEST_ASSERT_TRUE(drive.output().isDriving());
    TEST_ASSERT_EQUAL_UINT32(100U, drive.remainingMs(0U));

    // The command ends on its own, and the direction it fixed survives it: this
    // is the defect H2 is about - expiry is a software fact, not a stop.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(100U, 1)));
    expectNothingApplied(drive);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DirectionChangeRequiresReset),
        static_cast<int>(drive.command(120U, ev::MotorMode::DriveReverse, 100U, 100U)));
    TEST_ASSERT_EQUAL_UINT32(1U, drive.commandsIssued());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(drive.armedDirection()));
    expectNothingApplied(drive);

    // The same direction is still admitted, so the restriction is on reversal
    // only, not on further commands in the direction this arm chose.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(120U, ev::MotorMode::DriveForward, 100U, 100U)));
}

// Expiry is not a stop boundary: an arm that has driven forward still refuses
// reverse after the command it drove has ended on its own.
void test_an_expiry_does_not_release_the_direction(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 100U);
    drive.update(0U, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(100U, 1)));
    expectNothingApplied(drive);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DirectionChangeRequiresReset),
        static_cast<int>(drive.command(120U, ev::MotorMode::DriveReverse, 100U, 100U)));
    expectNothingApplied(drive);
}

// An abort does not release the direction either, and neither does the arm that
// follows a release-by-reset until the reset has actually happened: the direction
// record is cleared by `reset()` alone.
void test_an_abort_does_not_release_the_direction(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 400U);
    drive.update(0U, 0);
    drive.abort();
    drive.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(drive.armedDirection()));
}

// The deliberate direction-change procedure: reset (which leaves the instrument
// disarmed), then a fresh arm. Only then may the other direction be commanded.
void test_reset_then_a_fresh_arm_admits_the_other_direction(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveForward, 100U, 100U);
    drive.update(0U, 0);

    // A reset on its own leaves the instrument disarmed, so the next command is
    // refused for the arm it does not have - not for the direction.
    drive.reset();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::NotArmed),
        static_cast<int>(drive.command(200U, ev::MotorMode::DriveReverse, 100U, 100U)));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArmRefusal::None),
                          static_cast<int>(drive.arm()));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(220U, ev::MotorMode::DriveReverse, 100U, 100U)));
    drive.update(220U, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveReverse),
                          static_cast<int>(drive.output().mode()));
    // And the new arm is fixed to the new direction, once its command ends.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                          static_cast<int>(drive.update(320U, 1)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DirectionChangeRequiresReset),
        static_cast<int>(drive.command(340U, ev::MotorMode::DriveForward, 100U, 100U)));
}

// --- Limits, each checked before anything is applied --------------------------

void test_every_limit_is_refused_with_its_own_reason(void) {
    ev::BenchDrive drive = armed();

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DirectionNotDrivable),
        static_cast<int>(drive.command(0U, ev::MotorMode::Brake, 100U, 100U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DirectionNotDrivable),
        static_cast<int>(drive.command(0U, ev::MotorMode::Disabled, 100U, 100U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::EffortZero),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 0U, 100U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::EffortAboveCeiling),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward,
                                       static_cast<uint16_t>(kCeilingPermille + 1U), 100U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DurationZero),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 100U, 0U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::DurationAboveCeiling),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 100U,
                                       kMaxDurationMs + 1U)));

    // None of those refusals changed anything.
    expectNothingApplied(drive);
    TEST_ASSERT_EQUAL_UINT32(0U, drive.commandsIssued());
    TEST_ASSERT_FALSE(drive.faultLatched());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveState::Armed),
                          static_cast<int>(drive.state()));
    // A refused command does not fix a direction either.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(drive.armedDirection()));

    // The boundaries themselves are allowed: at the ceiling, at the maximum
    // duration.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, kCeilingPermille,
                                       kMaxDurationMs)));
}

// One command at a time: a second command while one is applied is refused rather
// than replacing it, so what is applied is always what was asked for and checked.
void test_a_second_command_while_driving_is_refused(void) {
    ev::BenchDrive drive = armed();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::None),
        static_cast<int>(drive.command(0U, ev::MotorMode::DriveForward, 80U, 300U)));
    drive.update(0U, 0);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::CommandInProgress),
        static_cast<int>(drive.command(20U, ev::MotorMode::DriveReverse, 120U, 300U)));

    // The original command is untouched.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(drive.output().mode()));
    TEST_ASSERT_EQUAL_UINT16(80U, drive.output().magnitudePermille());
    TEST_ASSERT_EQUAL_UINT32(280U, drive.remainingMs(20U));
}

// The per-arm budget bounds a console that keeps asking, within one arm.
void test_the_per_arm_command_budget_is_finite(void) {
    ev::BenchDrive drive = armed();

    for (uint32_t index = 0U; index < kMaxCommandsPerArm; ++index) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(ev::BenchCommandRefusal::None),
            static_cast<int>(drive.command(1000U * index, ev::MotorMode::DriveForward, 100U,
                                           100U)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Driving),
                              static_cast<int>(drive.update(1000U * index, 20)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchDriveStatus::Expired),
                              static_cast<int>(drive.update(1000U * index + 100U, 40)));
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxCommandsPerArm, drive.commandsIssued());

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::BenchCommandRefusal::CommandBudgetExhausted),
        static_cast<int>(drive.command(9000U, ev::MotorMode::DriveForward, 100U, 100U)));
    expectNothingApplied(drive);
}

// --- Names -------------------------------------------------------------------

// Every refusal and every arm refusal has its own non-empty, non-placeholder
// name: a refusal an operator cannot tell from another is not a refusal.
void test_every_refusal_has_its_own_name(void) {
    const ev::BenchCommandRefusal refusals[] = {
        ev::BenchCommandRefusal::NotArmed,
        ev::BenchCommandRefusal::FaultLatched,
        ev::BenchCommandRefusal::CommandInProgress,
        ev::BenchCommandRefusal::DirectionNotDrivable,
        ev::BenchCommandRefusal::EffortZero,
        ev::BenchCommandRefusal::EffortAboveCeiling,
        ev::BenchCommandRefusal::DurationZero,
        ev::BenchCommandRefusal::DurationAboveCeiling,
        ev::BenchCommandRefusal::CommandBudgetExhausted,
        ev::BenchCommandRefusal::DirectionChangeRequiresReset,
    };
    for (uint8_t index = 0U; index < 10U; ++index) {
        const char* name = ev::benchCommandRefusalName(refusals[index]);
        TEST_ASSERT_TRUE(name != nullptr);
        TEST_ASSERT_TRUE(name[0] != '\0');
        TEST_ASSERT_TRUE(name[0] != '?');
    }
    for (uint8_t first = 0U; first < 10U; ++first) {
        for (uint8_t second = first + 1U; second < 10U; ++second) {
            TEST_ASSERT_TRUE(ev::benchCommandRefusalName(refusals[first]) !=
                             ev::benchCommandRefusalName(refusals[second]));
        }
    }
    TEST_ASSERT_EQUAL_STRING("DirectionChangeRequiresReset",
                             ev::benchCommandRefusalName(
                                 ev::BenchCommandRefusal::DirectionChangeRequiresReset));

    const ev::BenchArmRefusal armRefusals[] = {ev::BenchArmRefusal::FaultLatched,
                                               ev::BenchArmRefusal::NotDisarmed};
    for (uint8_t index = 0U; index < 2U; ++index) {
        const char* name = ev::benchArmRefusalName(armRefusals[index]);
        TEST_ASSERT_TRUE(name != nullptr);
        TEST_ASSERT_TRUE(name[0] != '?');
        TEST_ASSERT_TRUE(name[0] != '\0');
    }
    TEST_ASSERT_EQUAL_STRING("None", ev::benchArmRefusalName(ev::BenchArmRefusal::None));
    TEST_ASSERT_EQUAL_STRING("FaultLatched",
                             ev::benchArmRefusalName(ev::BenchArmRefusal::FaultLatched));
    TEST_ASSERT_EQUAL_STRING("NotDisarmed",
                             ev::benchArmRefusalName(ev::BenchArmRefusal::NotDisarmed));
}

// --- The output type itself ---------------------------------------------------

// The output never carries a drive mode with zero effort, and never carries a
// non-zero effort with a non-drive mode: the adapter that writes pins can rely on
// that, and the assertions above pin it from the other side.
void test_the_output_carries_a_drivable_mode_or_nothing(void) {
    ev::BenchDrive drive = armed();
    drive.command(0U, ev::MotorMode::DriveReverse, 120U, 200U);
    drive.update(0U, 0);
    const ev::BenchDriveOutput applied = drive.output();
    TEST_ASSERT_TRUE(applied.isDriving());
    TEST_ASSERT_EQUAL_UINT16(120U, applied.magnitudePermille());

    drive.update(200U, 10);
    const ev::BenchDriveOutput stopped = drive.output();
    TEST_ASSERT_FALSE(stopped.isDriving());
    TEST_ASSERT_EQUAL_UINT16(0U, stopped.magnitudePermille());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_command_without_an_arm_is_refused);
    RUN_TEST(test_an_arm_applies_nothing_by_itself);
    RUN_TEST(test_an_arm_is_only_admissible_from_disarmed);
    RUN_TEST(test_a_repeated_arm_does_not_reset_the_per_arm_budget);
    RUN_TEST(test_a_command_expires_on_its_own_and_latches_nothing);
    RUN_TEST(test_a_long_gap_past_the_expiry_stops_rather_than_faults);
    RUN_TEST(test_the_expiry_is_wraparound_safe);
    RUN_TEST(test_the_remaining_interval_counts_down_and_saturates);
    RUN_TEST(test_an_abort_latches_until_a_deliberate_reset);
    RUN_TEST(test_an_encoder_stall_faults_and_latches);
    RUN_TEST(test_a_command_the_encoder_never_follows_faults);
    RUN_TEST(test_the_stall_window_boundary_is_exact_and_inside_the_command);
    RUN_TEST(test_the_stall_reachability_predicate);
    RUN_TEST(test_a_moving_encoder_never_faults);
    RUN_TEST(test_a_very_slow_but_real_rotation_is_not_blamed);
    RUN_TEST(test_reset_clears_counters_and_history);
    RUN_TEST(test_the_arm_fixes_one_direction);
    RUN_TEST(test_an_expiry_does_not_release_the_direction);
    RUN_TEST(test_an_abort_does_not_release_the_direction);
    RUN_TEST(test_reset_then_a_fresh_arm_admits_the_other_direction);
    RUN_TEST(test_every_limit_is_refused_with_its_own_reason);
    RUN_TEST(test_a_second_command_while_driving_is_refused);
    RUN_TEST(test_the_per_arm_command_budget_is_finite);
    RUN_TEST(test_every_refusal_has_its_own_name);
    RUN_TEST(test_the_output_carries_a_drivable_mode_or_nothing);
    return UNITY_END();
}
