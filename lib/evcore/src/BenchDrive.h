#pragma once

#include "Motion.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// BenchDrive - the bounded, latched, bench-only way to command effort
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The safety model of bench instrument B2 (`docs/bringup/README.md`): a way to
//   apply a conservative, *time-bounded* effort to the motor outside a run, for
//   the bring-up protocols that must measure what the vehicle does before any
//   competition behaviour exists to do it. It exists because the production
//   operator interface deliberately has no command that commands an effort (D14):
//   the first powered test needs one, and it must not be improvised at the bench.
// * Pure and host-testable. It reads no clock (every call takes `nowMs`), touches
//   no pin, allocates nothing, and is built from the same vocabulary the rest of
//   the core uses (`MotorMode`, `ActuationClass`).
// * A separate authority from `SafetyAuthority`, not a bypass of it. A bench
//   build contains both; production contains neither the bench code nor any way to
//   reach this type's output.
//
// The five properties that make it safe to have at all
//
//   1. **Bounded by construction.** Every command carries its own duration and
//      expires on its own. A lost host, a dropped serial line, a frozen console or
//      an operator who walks away cannot leave effort applied: the command ends
//      when its interval does, and the state machine de-energizes at that instant.
//   2. **Latched on abort.** An operator abort, or the encoder not changing while
//      the motor is commanded to drive, de-energizes the motor *and stays*
//      de-energized until a deliberate `reset()`. Nothing resumes by itself, which
//      is the same contract the shipped fault path has (`AGENTS.md` section 5).
//   3. **A deliberate arm.** A command does nothing until this object has been
//      armed in this session. Nothing arms at construction, and nothing survives a
//      reset.
//   4. **Every limit checked before anything is applied.** An out-of-range request
//      is refused with a named reason and changes no state: effort, duration and
//      direction all have hard ceilings, and a per-arm command budget means a
//      stuck console cannot loop commands forever.
//   5. **One drive direction per deliberate arm, and a stall watch that can
//      actually fire.** The first accepted command in an arm fixes the physical
//      direction (H2); the other direction is refused until `reset()` then
//      `arm()`, because a timed command's own expiry is a software fact and not
//      evidence that the drivetrain has stopped. The stall window is required to
//      be shorter than the longest command (H3), or the expiry would always fire
//      first and the latch would be unreachable while the motor is serviced
//      normally.
//
// What it is not
//
// * It is not the production safety boundary and it must never become a path the
//   run code can use: its output type is separate from
//   `AuthorizedMotorCommand` precisely so that no production module can accept it
//   by accident, and the driver adapter takes it through a separate call.
// * It does not replace the physical kill switch (the motor-supply disconnect).
//   That is the primary stop in every powered protocol, and this instrument is
//   never an excuse for leaving it out of reach.
// * It claims nothing about the electrical behavior of the effort it commands.
//   Direction, current, speed and braking are all UNVERIFIED facts that the
//   protocols using it are there to measure.

// The limits a bench command must fit inside. Supplied by the bench layer's own
// configuration rather than written here, so the numbers live with the bench that
// uses them and stay out of the pure core.
struct BenchDriveLimits {
    constexpr BenchDriveLimits(uint16_t maxEffortPermille_, uint32_t maxDurationMs_,
                               uint32_t stallWindowMs_, uint32_t maxCommandsPerArm_)
        : maxEffortPermille(maxEffortPermille_),
          maxDurationMs(maxDurationMs_),
          stallWindowMs(stallWindowMs_),
          maxCommandsPerArm(maxCommandsPerArm_) {}

    // The highest effort a bench command may ask for, permille of full scale.
    // Conservative by intent: the first powered test is the first time the
    // drivetrain moves, and the bench supply is current-limited for it.
    uint16_t maxEffortPermille;

    // The longest a single bench command may last, milliseconds.
    uint32_t maxDurationMs;

    // How long the encoder may show no change while a command is driving before
    // the command is aborted as a latched fault, milliseconds. This is the fault
    // H3 provokes deliberately; it also catches a stalled or unwired encoder.
    //
    // **It must be shorter than `maxDurationMs` (H3).** The expiry and this watch
    // are both evaluated by one `update()`; if the window is not shorter than the
    // longest command, the bounded expiry always fires first and the latch is
    // unreachable while the motor is serviced normally.
    // `benchStallProtectionReachable()` below states the relationship, the shipped
    // console asserts it at compile time, and the host suite pins it - so a future
    // limit set cannot silently lose the protection. The window is a software
    // budget, not a measured stopping behaviour: it bounds how long a stalled or
    // unwired motor may be driven, and a false abort at a very slow healthy
    // rotation is a safe-direction annoyance the protocol records.
    uint32_t stallWindowMs;

    // How many commands one arm may issue. A budget, not a rate: it bounds how
    // long a confused console can keep asking for effort without a fresh,
    // deliberate arm.
    uint32_t maxCommandsPerArm;
};

// Why a command was refused. Every one of these is a refusal that changed no
// state: nothing was energized, so there is nothing to abort or to latch.
enum class BenchCommandRefusal : uint8_t {
    None = 0,
    NotArmed = 1,              // no deliberate arm in this session
    FaultLatched = 2,          // an abort is latched; reset deliberately first
    CommandInProgress = 3,     // one command at a time; wait for the expiry or abort
    DirectionNotDrivable = 4,  // not DriveForward / DriveReverse
    EffortZero = 5,            // a zero-effort drive is not a bench command
    EffortAboveCeiling = 6,
    DurationZero = 7,          // a command with no duration is not bounded
    DurationAboveCeiling = 8,
    CommandBudgetExhausted = 9,
    // The instrument has already been commanded in the other direction during
    // this arm. A reversal needs evidence that the drivetrain has stopped, and
    // this path has none: a timed command's own expiry says only that the
    // software stopped asking (H2). `reset()` then `arm()` is the deliberate way
    // to change direction; this refusal is a pacing restriction, not a claim that
    // anything has stopped.
    DirectionChangeRequiresReset = 10,
};

inline const char* benchCommandRefusalName(BenchCommandRefusal refusal) {
    switch (refusal) {
        case BenchCommandRefusal::None:
            return "None";
        case BenchCommandRefusal::NotArmed:
            return "NotArmed";
        case BenchCommandRefusal::FaultLatched:
            return "FaultLatched";
        case BenchCommandRefusal::CommandInProgress:
            return "CommandInProgress";
        case BenchCommandRefusal::DirectionNotDrivable:
            return "DirectionNotDrivable";
        case BenchCommandRefusal::EffortZero:
            return "EffortZero";
        case BenchCommandRefusal::EffortAboveCeiling:
            return "EffortAboveCeiling";
        case BenchCommandRefusal::DurationZero:
            return "DurationZero";
        case BenchCommandRefusal::DurationAboveCeiling:
            return "DurationAboveCeiling";
        case BenchCommandRefusal::CommandBudgetExhausted:
            return "CommandBudgetExhausted";
        case BenchCommandRefusal::DirectionChangeRequiresReset:
            return "DirectionChangeRequiresReset";
    }
    return "?";
}

// Why a deliberate arm did not arm the instrument.
enum class BenchArmRefusal : uint8_t {
    None = 0,
    // An abort is latched. The latch exists so that re-arming is a separate
    // decision made after looking at why it stopped: `reset()` then `arm()`.
    FaultLatched = 1,
    // The instrument is already Armed or Driving. `arm()` is a transition from
    // Disarmed only, so a repeated `bench arm` can neither reset the per-arm
    // command budget nor cancel a command that is being applied (M3). Re-arming
    // is `reset()` first.
    NotDisarmed = 2,
};

inline const char* benchArmRefusalName(BenchArmRefusal refusal) {
    switch (refusal) {
        case BenchArmRefusal::None:
            return "None";
        case BenchArmRefusal::FaultLatched:
            return "FaultLatched";
        case BenchArmRefusal::NotDisarmed:
            return "NotDisarmed";
    }
    return "?";
}

// True when a limit set's stall watch can trip inside one command at all (H3).
//
// The expiry and the stall watch are both evaluated by `update()`; a command
// whose maximum duration is not longer than the stall window always expires
// first, so the latch is unreachable while the motor is serviced normally. The
// shipped limit set asserts this at compile time (`src/bench/BenchConsole.h`).
constexpr bool benchStallProtectionReachable(const BenchDriveLimits& limits) {
    return limits.stallWindowMs < limits.maxDurationMs;
}

// The bench instrument's state. Disarmed is the zero value: a default-constructed
// instrument commands nothing.
enum class BenchDriveState : uint8_t {
    Disarmed = 0,
    Armed = 1,    // a deliberate arm has happened; no command is applied
    Driving = 2,  // a bounded command is being applied
    Faulted = 3,  // an abort is latched; only reset() leaves this state
};

inline const char* benchDriveStateName(BenchDriveState state) {
    switch (state) {
        case BenchDriveState::Disarmed:
            return "Disarmed";
        case BenchDriveState::Armed:
            return "Armed";
        case BenchDriveState::Driving:
            return "Driving";
        case BenchDriveState::Faulted:
            return "Faulted";
    }
    return "?";
}

// What the last update() decided. Reported, never inferred by a caller.
enum class BenchDriveStatus : uint8_t {
    Disarmed = 0,
    Armed = 1,
    Driving = 2,
    Expired = 3,           // the command reached its own duration and stopped
    Aborted = 4,           // an operator abort: de-energized and latched
    EncoderStallFault = 5, // the encoder showed no change while driving: latched
    FaultLatched = 6,      // a latched abort is still latched
};

inline const char* benchDriveStatusName(BenchDriveStatus status) {
    switch (status) {
        case BenchDriveStatus::Disarmed:
            return "Disarmed";
        case BenchDriveStatus::Armed:
            return "Armed";
        case BenchDriveStatus::Driving:
            return "Driving";
        case BenchDriveStatus::Expired:
            return "Expired";
        case BenchDriveStatus::Aborted:
            return "Aborted";
        case BenchDriveStatus::EncoderStallFault:
            return "EncoderStallFault";
        case BenchDriveStatus::FaultLatched:
            return "FaultLatched";
    }
    return "?";
}

// What the bench path may hand the motor driver. Deliberately a different type
// from `AuthorizedMotorCommand`, and constructible only by `BenchDrive`: the
// production adapter cannot be made to accept one, and no production module can
// build one. It carries no arming, no run and no phase - it is one bounded
// command, or nothing.
class BenchDriveOutput {
public:
    MotorMode mode() const { return mode_; }

    // Permille of full scale, meaningful only for a drive mode. Always 0 for
    // every non-drive mode.
    uint16_t magnitudePermille() const { return magnitudePermille_; }

    ActuationClass actuationClass() const { return actuationClassOf(mode_); }
    bool isDriving() const { return isDrivingMode(mode_); }

private:
    friend class BenchDrive;

    explicit BenchDriveOutput(MotorMode mode, uint16_t magnitudePermille)
        : mode_(mode), magnitudePermille_(magnitudePermille) {}

    MotorMode mode_;
    uint16_t magnitudePermille_;
};

// The instrument. One instance, owned by the bench entry point.
class BenchDrive {
public:
    explicit BenchDrive(const BenchDriveLimits& limits) : limits_(limits) {}

    const BenchDriveLimits& limits() const { return limits_; }

    // Back to the post-construction state: disarmed, no fault, no command, no
    // history. This is the deliberate step that clears a latched abort - nothing
    // else does, and nothing here resumes anything.
    void reset() {
        state_ = BenchDriveState::Disarmed;
        faultLatched_ = false;
        status_ = BenchDriveStatus::Disarmed;
        hasCommand_ = false;
        mode_ = MotorMode::Disabled;
        permille_ = 0U;
        expiresAtMs_ = 0U;
        commandsIssued_ = 0U;
        stallFaults_ = 0U;
        armedDirection_ = MotorMode::Disabled;
    }

    // The deliberate arm: a transition from Disarmed only (M3).
    //
    // Refused while an abort is latched (re-arming is a separate decision made
    // after looking at why it stopped), and refused while already Armed or
    // Driving, so a repeated `bench arm` can neither reset the per-arm command
    // budget nor replace a command that is being applied. The instrument is
    // Disarmed only at construction and after `reset()`, both of which already
    // zero the command count, so the budget cannot be erased by an arm.
    BenchArmRefusal arm() {
        if (faultLatched_) {
            return BenchArmRefusal::FaultLatched;
        }
        if (state_ != BenchDriveState::Disarmed) {
            return BenchArmRefusal::NotDisarmed;
        }
        state_ = BenchDriveState::Armed;
        status_ = BenchDriveStatus::Armed;
        hasCommand_ = false;
        return BenchArmRefusal::None;
    }

    // An abort: whatever is applied stops now and stays stopped until `reset()`.
    // This is the operator's `bench stop`, and it is also what an arm that is
    // taken away must do - leaving a latched fault behind rather than a silently
    // disarmed instrument.
    void abort() {
        state_ = BenchDriveState::Faulted;
        faultLatched_ = true;
        status_ = BenchDriveStatus::Aborted;
        hasCommand_ = false;
        mode_ = MotorMode::Disabled;
        permille_ = 0U;
        expiresAtMs_ = 0U;
    }

    // One bounded command. Refuses (changing nothing) unless every limit is met.
    BenchCommandRefusal command(uint32_t nowMs, MotorMode mode, uint16_t magnitudePermille,
                               uint32_t durationMs) {
        if (faultLatched_) {
            return BenchCommandRefusal::FaultLatched;
        }
        if (state_ != BenchDriveState::Armed) {
            return state_ == BenchDriveState::Driving ? BenchCommandRefusal::CommandInProgress
                                                      : BenchCommandRefusal::NotArmed;
        }
        if (mode != MotorMode::DriveForward && mode != MotorMode::DriveReverse) {
            return BenchCommandRefusal::DirectionNotDrivable;
        }
        if (magnitudePermille == 0U) {
            return BenchCommandRefusal::EffortZero;
        }
        if (magnitudePermille > limits_.maxEffortPermille) {
            return BenchCommandRefusal::EffortAboveCeiling;
        }
        if (durationMs == 0U) {
            return BenchCommandRefusal::DurationZero;
        }
        if (durationMs > limits_.maxDurationMs) {
            return BenchCommandRefusal::DurationAboveCeiling;
        }
        if (commandsIssued_ >= limits_.maxCommandsPerArm) {
            return BenchCommandRefusal::CommandBudgetExhausted;
        }
        // One drive direction per deliberate arm (H2). The first accepted command
        // fixes it; a request for the other direction is refused until `reset()`
        // then `arm()`. This is the conservative first-bring-up policy: the bench
        // interface cannot establish standstill, so the only stop boundary a
        // reversal may cross is a deliberate one. It does not claim the drivetrain
        // has stopped - see `DirectionChangeRequiresReset` above.
        if (armedDirection_ != MotorMode::Disabled && mode != armedDirection_) {
            return BenchCommandRefusal::DirectionChangeRequiresReset;
        }

        state_ = BenchDriveState::Driving;
        status_ = BenchDriveStatus::Driving;
        hasCommand_ = true;
        mode_ = mode;
        armedDirection_ = mode;
        permille_ = magnitudePermille;
        expiresAtMs_ = nowMs + durationMs;
        ++commandsIssued_;
        // The stall window is measured from the command, and the first observation
        // is the caller's: `lastChangeMs_` is stamped here so the window is
        // "commanded, and nothing has moved for this long", not "moved a long time
        // ago".
        lastChangeMs_ = nowMs;
        hasLastTicks_ = false;
        return BenchCommandRefusal::None;
    }

    // One step, from the caller's clock reading and the encoder count. Advances the
    // expiry, watches for an encoder that is not changing while commanded to
    // drive, and reports what it decided.
    BenchDriveStatus update(uint32_t nowMs, int32_t encoderTicks) {
        if (state_ == BenchDriveState::Faulted) {
            status_ = BenchDriveStatus::FaultLatched;
            return status_;
        }
        if (state_ == BenchDriveState::Armed) {
            status_ = BenchDriveStatus::Armed;
            return status_;
        }
        if (state_ == BenchDriveState::Disarmed) {
            status_ = BenchDriveStatus::Disarmed;
            return status_;
        }

        // Driving. Remember when the count last changed, and detect a stall while
        // it has not.
        if (!hasLastTicks_ || encoderTicks != lastTicks_) {
            lastTicks_ = encoderTicks;
            hasLastTicks_ = true;
            lastChangeMs_ = nowMs;
        } else if (static_cast<int32_t>(nowMs - lastChangeMs_) >
                   static_cast<int32_t>(limits_.stallWindowMs)) {
            // The encoder has not moved for longer than the window while the motor
            // is commanded to drive. That is either a stalled drivetrain, an
            // unwired encoder or a command the drivetrain cannot follow - all
            // reasons to stop and to stay stopped.
            state_ = BenchDriveState::Faulted;
            faultLatched_ = true;
            status_ = BenchDriveStatus::EncoderStallFault;
            hasCommand_ = false;
            mode_ = MotorMode::Disabled;
            permille_ = 0U;
            expiresAtMs_ = 0U;
            ++stallFaults_;
            return status_;
        }

        // The bounded interval, which is the whole safety property: the command
        // ends here whether or not anyone is watching. Signed difference, so this
        // is correct across the clock's wraparound.
        if (static_cast<int32_t>(nowMs - expiresAtMs_) >= 0) {
            state_ = BenchDriveState::Armed;
            status_ = BenchDriveStatus::Expired;
            hasCommand_ = false;
            mode_ = MotorMode::Disabled;
            permille_ = 0U;
            expiresAtMs_ = 0U;
            return status_;
        }

        status_ = BenchDriveStatus::Driving;
        return status_;
    }

    // What may be applied right now. Nothing when not driving, always.
    BenchDriveOutput output() const {
        if (state_ != BenchDriveState::Driving || !hasCommand_) {
            return BenchDriveOutput(MotorMode::Disabled, 0U);
        }
        return BenchDriveOutput(mode_, permille_);
    }

    BenchDriveState state() const { return state_; }
    BenchDriveStatus status() const { return status_; }
    bool faultLatched() const { return faultLatched_; }
    bool hasCommand() const { return hasCommand_; }
    uint32_t commandsIssued() const { return commandsIssued_; }
    uint32_t stallFaults() const { return stallFaults_; }

    // The drive direction this arm has fixed, or Disabled when no command has been
    // accepted in it. Reported for diagnostics; it is the fact the
    // one-direction-per-arm refusal is made from.
    MotorMode armedDirection() const { return armedDirection_; }

    // Milliseconds until the command expires, or 0 when none is applied. Saturating
    // rather than wrapping, for the same reason the expiry test is signed.
    uint32_t remainingMs(uint32_t nowMs) const {
        if (state_ != BenchDriveState::Driving || !hasCommand_) {
            return 0U;
        }
        const int32_t remaining = static_cast<int32_t>(expiresAtMs_ - nowMs);
        return remaining > 0 ? static_cast<uint32_t>(remaining) : 0U;
    }

private:
    BenchDriveLimits limits_;
    BenchDriveState state_ = BenchDriveState::Disarmed;
    BenchDriveStatus status_ = BenchDriveStatus::Disarmed;
    bool faultLatched_ = false;
    bool hasCommand_ = false;
    bool hasLastTicks_ = false;
    MotorMode mode_ = MotorMode::Disabled;
    MotorMode armedDirection_ = MotorMode::Disabled;
    uint16_t permille_ = 0U;
    int32_t lastTicks_ = 0;
    uint32_t expiresAtMs_ = 0U;
    uint32_t lastChangeMs_ = 0U;
    uint32_t commandsIssued_ = 0U;
    uint32_t stallFaults_ = 0U;
};

}  // namespace ev
