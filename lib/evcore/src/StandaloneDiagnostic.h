#pragma once

#include <cstdint>

#include "BenchDrive.h"
#include "CompetitionStateMachine.h"
#include "MotorArbitration.h"
#include "StandaloneIndicator.h"
#include "StartTrigger.h"

namespace ev {

// ---------------------------------------------------------------------------
// StandaloneDiagnosticController - one bounded, trigger-driven motor pulse
// ---------------------------------------------------------------------------
//
// What this module is
//
// * A separate, explicitly named standalone diagnostic mode for the first
//   supervised, wheels-raised test of the *battery-powered* vehicle: no laptop,
//   no USB, no serial command. It proves only that the board boots safely, reads
//   and debounces the physical pencil trigger, applies one strictly bounded
//   low-effort motor activation, and returns the motor to its disabled state.
// * Reused machinery, not a new framework: `ev::BenchDrive` owns the bounded
//   duration, the effort ceiling, the per-arm command budget, the stall watch and
//   the latch; `ev::StartTriggerDetector` owns boot-held rejection, debounce and
//   single-edge pulses; `ev::benchApplyVerdict()` decides whether anything may
//   write the motor pins at all.
//
// Safety invariants
//
//   1. The motor is `Disabled` during boot, while the trigger has not been
//      stably released, after the bounded pulse expires, and in every fault
//      state.
//   2. The limits are the bench ceilings (`150` permille, `500` ms, one command
//      per arm) with a conservative default activation of `50` permille for
//      `250` ms.
//   3. One activation per power cycle. Holding the pencil, bouncing it or pressing
//      it again after the pulse completes cannot start a second activation: the
//      instrument's command budget is one, and the phase machine has no path back
//      to `Ready`.
//   4. A trigger held during boot cannot start the pulse; it must first be
//      stably released for the detector's qualification interval.
//   5. Any external fault, control-tick order violation, armed run or run
//      underway aborts the pulse immediately and latches `Faulted`.
//
// What this mode does not claim
//
//   It does not travel a distance, does not use the encoder scale, and is not a
//   competition run or a substitute for one. A completed pulse is evidence that
//   the trigger, the driver enable path and the battery supply work - nothing
//   about braking, distance or repeatability.

struct StandaloneDiagnosticConfig {
    constexpr StandaloneDiagnosticConfig(MotorMode directionMode_, uint16_t effortPermille_,
                                          uint32_t durationMs_, BenchDriveLimits limits_,
                                          StartTriggerConfig triggerConfig_)
        : directionMode(directionMode_),
          effortPermille(effortPermille_),
          durationMs(durationMs_),
          limits(limits_),
          triggerConfig(triggerConfig_) {}

    MotorMode directionMode;
    uint16_t effortPermille;
    uint32_t durationMs;
    BenchDriveLimits limits;
    StartTriggerConfig triggerConfig;
};

// Bench-referenced ceilings for this mode (the bench console's own numbers).
constexpr uint16_t kStandaloneDiagMaxEffortPermille = 150U;
constexpr uint32_t kStandaloneDiagMaxDurationMs = 500U;
// Shorter than the longest permitted activation, or the bounded expiry would
// always fire first and the stall latch would be unreachable (`BenchDrive.h`,
// `benchStallProtectionReachable()`).
constexpr uint32_t kStandaloneDiagStallWindowMs = 400U;
constexpr uint32_t kStandaloneDiagMaxCommandsPerArm = 1U;

constexpr BenchDriveLimits kStandaloneDiagLimits(kStandaloneDiagMaxEffortPermille,
                                                 kStandaloneDiagMaxDurationMs,
                                                 kStandaloneDiagStallWindowMs,
                                                 kStandaloneDiagMaxCommandsPerArm);

static_assert(benchStallProtectionReachable(kStandaloneDiagLimits),
              "the standalone diagnostic limits must keep bench stall protection reachable");

// Conservative default activation: 50/1000 effort for 250 ms forward. The pulse
// is shorter than the stall window, so it expires cleanly even on a vehicle whose
// encoder has not been verified yet - a raised wheel that does not turn produces
// no stall fault inside 250 ms, and the pulse still ends on its own timer.
constexpr uint16_t kStandaloneDiagDefaultEffortPermille = 50U;
constexpr uint32_t kStandaloneDiagDefaultDurationMs = 250U;

constexpr StandaloneDiagnosticConfig kDefaultStandaloneDiagnosticConfig(
    MotorMode::DriveForward, kStandaloneDiagDefaultEffortPermille,
    kStandaloneDiagDefaultDurationMs, kStandaloneDiagLimits, kDefaultStartTriggerConfig);

static_assert(kDefaultStandaloneDiagnosticConfig.effortPermille > 0U &&
                  kDefaultStandaloneDiagnosticConfig.effortPermille <=
                      kStandaloneDiagMaxEffortPermille,
              "the default standalone diagnostic effort must sit inside the bench ceiling");
static_assert(kDefaultStandaloneDiagnosticConfig.durationMs > 0U &&
                  kDefaultStandaloneDiagnosticConfig.durationMs <=
                      kStandaloneDiagMaxDurationMs,
              "the default standalone diagnostic duration must sit inside the bench ceiling");

enum class StandaloneDiagnosticPhase : uint8_t {
    // Waiting for the trigger to be stably released after boot.
    WaitingForTriggerRelease = 0,
    // Armed; waiting for exactly one intentional trigger press.
    Ready = 1,
    // Applying the single bounded pulse.
    ActivePulse = 2,
    // The pulse completed; the motor is disabled and will not retrigger.
    Completed = 3,
    // A fault or abort occurred; the motor is disabled and latched.
    Faulted = 4,
};

inline const char* standaloneDiagnosticPhaseName(StandaloneDiagnosticPhase phase) {
    switch (phase) {
        case StandaloneDiagnosticPhase::WaitingForTriggerRelease:
            return "WaitingForTriggerRelease";
        case StandaloneDiagnosticPhase::Ready:
            return "Ready";
        case StandaloneDiagnosticPhase::ActivePulse:
            return "ActivePulse";
        case StandaloneDiagnosticPhase::Completed:
            return "Completed";
        case StandaloneDiagnosticPhase::Faulted:
            return "Faulted";
    }
    return "?";
}

// One pass of the diagnostic, as the entry point must act on it.
struct StandaloneDiagnosticStep {
    StandaloneDiagnosticPhase phase = StandaloneDiagnosticPhase::WaitingForTriggerRelease;

    // What the instrument is asking the driver for this pass. `mode` is
    // `Disabled` whenever nothing may be applied, so a caller can write the
    // de-energized state from this alone.
    MotorMode mode = MotorMode::Disabled;
    uint16_t permille = 0U;

    // True when the caller must take the motor pins for this pass: the
    // instrument is driving AND `ev::benchApplyVerdict()` says nothing outranks
    // it. False means the caller must not write a drive, and must release the
    // pins it held on the previous pass.
    bool ownsMotorPins = false;

    bool pulseStartedThisPass = false;
    bool pulseFinishedThisPass = false;
    bool faultedThisPass = false;
    BenchApplyRefusal arbitrationRefusal = BenchApplyRefusal::None;
};

class StandaloneDiagnosticController {
public:
    explicit StandaloneDiagnosticController(
        const StandaloneDiagnosticConfig& config = kDefaultStandaloneDiagnosticConfig)
        : config_(config), drive_(config.limits), trigger_(config.triggerConfig) {}

    void reset() {
        drive_.reset();
        trigger_.reset();
        phase_ = StandaloneDiagnosticPhase::WaitingForTriggerRelease;
    }

    // Steps the diagnostic *before* the control tick, so the caller can hand the
    // pins over before anything else writes them.
    //
    // `runtimeArmed`, `runtimeFaultLatched`, `runtimeUnderway` and
    // `orderViolationDetected` are the vehicle runtime's own facts - this mode
    // runs beside the production chain, and every one of them outranks it.
    StandaloneDiagnosticStep update(uint32_t nowMs, bool rawTriggerActive, int32_t encoderTicks,
                                    bool runtimeArmed, bool runtimeFaultLatched,
                                    bool runtimeUnderway, bool orderViolationDetected) {
        StandaloneDiagnosticStep step;

        const bool externalFault =
            runtimeFaultLatched || orderViolationDetected || runtimeArmed || runtimeUnderway;
        if (externalFault && phase_ != StandaloneDiagnosticPhase::Faulted) {
            drive_.abort();
            phase_ = StandaloneDiagnosticPhase::Faulted;
            step.faultedThisPass = true;
        }

        const StartTriggerVerdict trigger = trigger_.update(nowMs, rawTriggerActive);
        const BenchDriveStatus driveStatus = drive_.update(nowMs, encoderTicks);

        if (driveStatus == BenchDriveStatus::EncoderStallFault ||
            driveStatus == BenchDriveStatus::Aborted ||
            driveStatus == BenchDriveStatus::FaultLatched) {
            if (phase_ != StandaloneDiagnosticPhase::Faulted) {
                phase_ = StandaloneDiagnosticPhase::Faulted;
                step.faultedThisPass = true;
            }
        } else if (phase_ == StandaloneDiagnosticPhase::ActivePulse &&
                   driveStatus == BenchDriveStatus::Expired) {
            phase_ = StandaloneDiagnosticPhase::Completed;
            step.pulseFinishedThisPass = true;
        }

        switch (phase_) {
            case StandaloneDiagnosticPhase::WaitingForTriggerRelease:
                if (trigger.inactiveQualified) {
                    if (drive_.arm() == BenchArmRefusal::None) {
                        phase_ = StandaloneDiagnosticPhase::Ready;
                    } else {
                        drive_.abort();
                        phase_ = StandaloneDiagnosticPhase::Faulted;
                        step.faultedThisPass = true;
                    }
                }
                break;

            case StandaloneDiagnosticPhase::Ready:
                if (trigger.activated) {
                    if (drive_.command(nowMs, config_.directionMode, config_.effortPermille,
                                       config_.durationMs) == BenchCommandRefusal::None) {
                        phase_ = StandaloneDiagnosticPhase::ActivePulse;
                        step.pulseStartedThisPass = true;
                    } else {
                        drive_.abort();
                        phase_ = StandaloneDiagnosticPhase::Faulted;
                        step.faultedThisPass = true;
                    }
                }
                break;

            case StandaloneDiagnosticPhase::ActivePulse:
            case StandaloneDiagnosticPhase::Completed:
            case StandaloneDiagnosticPhase::Faulted:
                // ActivePulse runs until its bounded timer expires. Completed and
                // Faulted are terminal for this power cycle: no trigger press
                // returns them to Ready.
                break;
        }

        const ev::BenchDriveOutput output = drive_.output();
        const BenchApplyVerdict verdict =
            benchApplyVerdict(output.isDriving(), runtimeArmed, runtimeFaultLatched,
                              runtimeUnderway, orderViolationDetected);
        if (!verdict.mayApply && output.isDriving()) {
            drive_.abort();
            phase_ = StandaloneDiagnosticPhase::Faulted;
            step.faultedThisPass = true;
        }

        const ev::BenchDriveOutput finalOutput = drive_.output();
        step.phase = phase_;
        step.mode = finalOutput.mode();
        step.permille = finalOutput.magnitudePermille();
        step.ownsMotorPins = verdict.mayApply && finalOutput.isDriving();
        step.arbitrationRefusal = verdict.refusal;
        return step;
    }

    StandaloneDiagnosticPhase phase() const { return phase_; }
    const StandaloneDiagnosticConfig& config() const { return config_; }
    const BenchDrive& drive() const { return drive_; }
    const StartTriggerDetector& trigger() const { return trigger_; }

    StandaloneIndicatorState indicatorState() const {
        switch (phase_) {
            case StandaloneDiagnosticPhase::WaitingForTriggerRelease:
                return StandaloneIndicatorState::Waiting;
            case StandaloneDiagnosticPhase::Ready:
                return StandaloneIndicatorState::Ready;
            case StandaloneDiagnosticPhase::ActivePulse:
                return StandaloneIndicatorState::Running;
            case StandaloneDiagnosticPhase::Completed:
                return StandaloneIndicatorState::Finished;
            case StandaloneDiagnosticPhase::Faulted:
                return StandaloneIndicatorState::Faulted;
        }
        return StandaloneIndicatorState::Faulted;
    }

private:
    StandaloneDiagnosticConfig config_;
    BenchDrive drive_;
    StartTriggerDetector trigger_;
    StandaloneDiagnosticPhase phase_ = StandaloneDiagnosticPhase::WaitingForTriggerRelease;
};

}  // namespace ev
