#pragma once

#include "Motion.h"
#include "SafetyLimits.h"
#include "VehicleState.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// Fault identity vs. fault response
// ---------------------------------------------------------------------------
//
// What electrical action a fault stop asks for. This is deliberately a
// different type from FaultCode: knowing *what* went wrong (fault identity)
// must not silently decide *what the motor should do* (fault response). The
// two are allowed to change on their own schedules.
//
// No real driver nFAULT/current/stall behavior is classified yet. The hardware
// facts behind those faults are UNVERIFIED (VERIFIED_STATE.md #2), so this
// stage only models the boundary, not the electrical strategies.
enum class FaultStopPolicy : uint8_t {
    Disable = 0,  // remove driver outputs; the safe default
    Coast = 1,    // leave the drive free to coast
    Brake = 2,    // command active braking
};

// The conservative response for the software faults that exist today. None of
// these describes a measured electrical condition, so Disable is the only
// response that is safe to assume without hardware evidence. That includes the
// run-policy faults: a phase timeout, an unverified reversal, a run that lost
// its arming and an inconsistent sequencing request are all software conditions
// with no measured electrical answer. A hardware fault gets a different policy
// only after measurement, and that change belongs here rather than in FaultCode.
inline FaultStopPolicy faultStopPolicyFor(FaultCode code) {
    switch (code) {
        case FaultCode::None:
        case FaultCode::InvalidRunConfig:
        case FaultCode::ProvisionalCalibration:
        case FaultCode::MissingSafetyLimits:
        case FaultCode::InvalidMotorEffort:
        case FaultCode::PhaseTimeout:
        case FaultCode::ReversalNotVerified:
        case FaultCode::DisarmedDuringRun:
        case FaultCode::RunSequencingError:
            return FaultStopPolicy::Disable;
        default:
            // New enumerators must be mapped explicitly; this default is the
            // safe response.
            return FaultStopPolicy::Disable;
    }
}

// ---------------------------------------------------------------------------
// System state supplied to the safety boundary
// ---------------------------------------------------------------------------
//
// Data-completeness, not an arming decision: these are the falsifiable
// preconditions a drive depends on. All default to false so a default-built
// state authorizes nothing powered.
struct SafetyReadiness {
    bool configurationValid = false;  // announced RunConfig passed validation
    bool calibrationUsable = false;   // required VehicleCalibration is measured
    bool limitsUsable = false;        // required SafetyLimits are established

    // `limitsUsable` must be derived from validateSafetyLimits(), so this class
    // never interprets raw Sourced values. Enforcing maxRunDurationS and
    // minBatteryV at runtime has no owner yet: the run timer and the battery
    // check that will own them are not built, and this class does not enforce
    // them.

    constexpr bool allReady() const {
        return configurationValid && calibrationUsable && limitsUsable;
    }
};

// The current safety/system state the authority transforms against. It carries
// no threshold values: `requiredDataFresh` is a policy decision made where the
// relevant data is actually known (no invented staleness timeout lives here),
// and `atZeroSpeed` is decided by the caller's motion estimate (no invented
// speed threshold lives here).
struct SafetySystemState {
    SafetyReadiness readiness;

    // Required measurement data is present and fresh this cycle.
    bool requiredDataFresh = false;

    // The motion estimate reports no net movement. Used only to permit a
    // powered direction change; it is not a control threshold.
    //
    // Producer contract (not built yet): whoever sets this field must combine a
    // fresh, valid velocity estimate with the estimator's explicit
    // `zeroSpeedClaimed` and with the interval over which the stop has been
    // observed, and must be able to say which of those facts it used.
    // `VelocityEstimate::zeroSpeedClaimed` alone is never sufficient: it is an
    // explicit software timeout on observed encoder non-change, not a
    // measurement that the vehicle stopped (VERIFIED_STATE.md #5, #9). Nothing
    // in lib/ may set this field today, and no component may derive it from a
    // window-derived zero alone.
    bool atZeroSpeed = false;

    // Clock reading from millis() used to stamp a newly latched fault. It must
    // be an actual millis() reading supplied by the vehicle layer; a forgotten
    // assignment is not detectable here, so a zero reading is accepted and
    // stamps raisedAtMs = 0 rather than suppressing the latch.
    uint32_t nowMs = 0;

    // A fault condition observed by the system this cycle (None if healthy).
    FaultCode detectedFault = FaultCode::None;
};

// Why the authority returned the command it did. Diagnostic only: the command
// itself is the safety-relevant output, and it is always a safe one.
enum class AuthzOutcome : uint8_t {
    AuthorizedDrive = 0,   // drive granted as requested
    AuthorizedClamped,     // drive granted, magnitude reduced to the safety ceiling
    AuthorizedStop,        // Disabled/Coast/Brake granted as requested
    ForcedFaultStop,       // a latched fault dictated the response
    DeniedNotArmed,        // not armed -> no DRIVE
    DeniedNotReady,        // configuration/calibration/limits not usable -> no drive
    DeniedStaleData,       // required data not fresh -> no drive
    DeniedIllegalEffort,   // malformed request (unknown mode / bad magnitude)
    DeniedUnsafeDirectionChange,  // direct powered direction flip refused
};

// The single output of the boundary: an authorized command plus why. This type
// has no default constructor, because an AuthorizedMotorCommand has none.
struct AuthorizationResult {
    AuthorizedMotorCommand command;
    AuthzOutcome outcome;
};

// ---------------------------------------------------------------------------
// SafetyAuthority
// ---------------------------------------------------------------------------
//
// The only code that can turn a request into an AuthorizedMotorCommand. It is
// the arming authority and the final permission boundary for powered motion.
//
// It is intentionally invariant-based and knows nothing about bottle phases,
// scoring strategy, event geometry, target-distance strategy, or normal
// competition sequencing. It reasons only about permission: arming, data
// completeness, a latched fault, effort bounds, and safe direction change.
//
// One authority, two request sources - and no arbiter yet. A run will have two
// kinds of request: the velocity controller's `MotorEffort`, and the run layer's
// own deliberate stop (Brake or Coast) as a MotorEffort handed to this same
// authorize(). This class arbitrates nothing between them and must not learn to:
// the run layer owns which single request is presented each cycle, and
// presenting two requests in one cycle and acting on the later one would make
// the direction memory and the fault response depend on call order. A latched
// fault overrides whatever request arrives. Until that run-layer arbitration is
// built, a caller must present exactly one request per cycle.
//
// `authorize()` is a deterministic transition: its result depends only on the
// request, the supplied state, the limits, and the authority's own small state
// (armed, latched fault, last authorized powered direction). There are no
// hidden timers and no GPIO.
class SafetyAuthority {
public:
    SafetyAuthority() = default;

    // Intentional transition into the armed/run state. It succeeds only when
    // the readiness data is complete and no fatal fault is latched. Returns the
    // resulting armed state, so a refused arm is visible to the caller.
    bool arm(const SafetyReadiness& readiness) {
        if (latchedFault_.code != FaultCode::None) {
            return false;
        }
        if (!readiness.allReady()) {
            return false;
        }
        armed_ = true;
        return true;
    }

    // Always available, always safe. Disarming does not clear the remembered
    // powered direction: the drivetrain may still be moving, so a later
    // reversal must still pass through the zero-speed path.
    void disarm() { armed_ = false; }

    bool isArmed() const { return armed_; }

    // Reinitialization. Clears the direction memory and any latched fault and
    // returns to disarmed. After reset, no previous command is remembered and
    // powered motion requires a fresh arming decision.
    void reset() {
        armed_ = false;
        latchedFault_ = FaultState{};
        lastDriveDirection_ = PoweredDirection::None;
    }

    // The fault of record. A latched fault blocks drive until clearFault().
    const FaultState& latchedFault() const { return latchedFault_; }

    // Clears a latched fault. Refuses while armed: removing the cause of a stop
    // without a fresh arming decision is not allowed. A fault clearance always
    // ends the current arming period: the authority is left disarmed, so
    // clearing a fault can never restore powered motion by itself and re-arming
    // is a fresh decision. Returns true when a latched fault was actually
    // cleared.
    //
    // Unlike reset(), clearFault() deliberately preserves the remembered powered
    // direction: the drivetrain may still be moving, so the path back to a
    // reversal still runs through the zero-speed stop. reset() is the only
    // operation that clears the direction memory, which is why a new run uses it
    // (see MotionChainReset.h).
    bool clearFault() {
        if (armed_) {
            return false;
        }
        if (latchedFault_.code == FaultCode::None) {
            return false;
        }
        latchedFault_ = FaultState{};
        armed_ = false;
        return true;
    }

    // The powered direction currently latched by an authorized drive, or None
    // when a direction change is permitted.
    PoweredDirection lastAuthorizedDirection() const { return lastDriveDirection_; }

    // The fault-stop command for a policy. Exposed so tests and later diagnostics
    // can verify that fault response is chosen independently from fault identity.
    // It can only build a stop command, never a Drive command.
    static AuthorizedMotorCommand stopCommandFor(FaultStopPolicy policy) {
        switch (policy) {
            case FaultStopPolicy::Disable:
                return commandFor(MotorMode::Disabled, 0U);
            case FaultStopPolicy::Coast:
                return commandFor(MotorMode::Coast, 0U);
            case FaultStopPolicy::Brake:
                return commandFor(MotorMode::Brake, 0U);
            default:
                // New enumerators must be mapped explicitly; this default is
                // the safe response.
                return commandFor(MotorMode::Disabled, 0U);
        }
    }

    // The single pure transformation: requested effort/intent plus current
    // safety/system state -> AuthorizedMotorCommand. It always returns a safe
    // command; a refusal is a stop, never "no command".
    AuthorizationResult authorize(const MotorEffort& requested,
                                  const SafetySystemState& state,
                                  const SafetyLimits& limits) {
        // Latch any newly detected fault before anything else. A fault must not
        // be ignored because the same cycle also asked to drive, and latching
        // disarms so powered motion cannot continue on a system that just
        // reported a fault. Fault severity is not classified yet, so every
        // detected fault is treated as fatal; this is deliberately conservative
        // and is revisited only when fault behavior is measured.
        if (state.detectedFault != FaultCode::None &&
            latchedFault_.code == FaultCode::None) {
            latchedFault_.code = state.detectedFault;
            latchedFault_.latched = true;
            latchedFault_.raisedAtMs = state.nowMs;
            armed_ = false;
        }

        // A latched fault dictates the response, independent of what was asked
        // for, so a drive request can never override a fault stop.
        if (latchedFault_.code != FaultCode::None) {
            noteStop(state.atZeroSpeed);
            return AuthorizationResult{stopCommandFor(faultStopPolicyFor(latchedFault_.code)),
                                       AuthzOutcome::ForcedFaultStop};
        }

        // A malformed request can never produce powered motion. It also does not
        // clear the direction memory: only a valid, authorized non-powered
        // command is a legitimate way to reach a direction change.
        if (!validateMotorEffort(requested).ok) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedIllegalEffort};
        }

        if (!isDrivingMode(requested.mode)) {
            // Disabled, Coast and Brake are always available: they cannot cause
            // powered motion, so they do not require arming. Braking in
            // particular must stay available to stop the vehicle.
            noteStop(state.atZeroSpeed);
            return AuthorizationResult{commandFor(requested.mode, 0U),
                                       AuthzOutcome::AuthorizedStop};
        }

        // From here the request is a powered drive, so every permission has to
        // hold. Each check returns a stop command, and none of them mutates the
        // direction memory.
        if (!armed_) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedNotArmed};
        }
        if (!state.readiness.allReady()) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedNotReady};
        }
        if (!state.requiredDataFresh) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedStaleData};
        }

        // Defensive invariant: the Motion contract is permille 0..1000 and the
        // vehicle layer must derive readiness.limitsUsable from
        // validateSafetyLimits(). If that contract is violated the authority
        // must never emit a command that would break it. This guard is
        // intentionally redundant with the readiness check and is ordered
        // before the direction check so a broken limit is never masked by
        // DeniedUnsafeDirectionChange.
        if (limits.maxEffortPermille.value > 1000U) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedNotReady};
        }

        // Independent prevention of a direct powered forward->reverse (or
        // reverse->forward) transition. The flip is refused whenever a powered
        // direction is still latched, even if the caller claims zero speed. The
        // latch is cleared only by an authorized non-powered command taken while
        // zero speed is reported, which is the deliberate legal path.
        const PoweredDirection requestedDirection = poweredDirectionOf(requested.mode);
        if (lastDriveDirection_ != PoweredDirection::None &&
            lastDriveDirection_ != requestedDirection) {
            return AuthorizationResult{commandFor(MotorMode::Disabled, 0U),
                                       AuthzOutcome::DeniedUnsafeDirectionChange};
        }

        // Effort bound: a request above the measured safety ceiling is clamped,
        // never granted as asked. `validateMotorEffort` already rejected
        // magnitudes above full scale, so this only ever reduces. After the
        // guard above the clamp cannot produce >1000.
        uint16_t commandedPermille = requested.magnitudePermille;
        AuthzOutcome outcome = AuthzOutcome::AuthorizedDrive;
        if (commandedPermille > limits.maxEffortPermille.value) {
            commandedPermille = limits.maxEffortPermille.value;
            outcome = AuthzOutcome::AuthorizedClamped;
        }

        lastDriveDirection_ = requestedDirection;
        return AuthorizationResult{commandFor(requested.mode, commandedPermille), outcome};
    }

private:
    // A stop is the only event that can permit a later direction change, and
    // only when the caller reports zero speed. Without that confirmation the
    // remembered direction survives, so the flip stays refused.
    //
    // This includes a stop a *refusal* produced: a controller that refused this
    // cycle emits a valid Disabled request, which the non-driving branch of
    // authorize() returns as an AuthorizedStop, and if the caller also reports
    // zero speed that stop clears the direction latch even though no one
    // deliberately commanded a stop. A malformed request (DeniedIllegalEffort)
    // deliberately does not do this. The fault path latches separately, so the
    // risk is low, but the run layer owns this: it must treat a refusal-derived
    // stop as a stop it knows about, not as an invisible no-op, and must decide
    // whether it wants the reversal door opened at that moment.
    void noteStop(bool atZeroSpeed) {
        if (atZeroSpeed) {
            lastDriveDirection_ = PoweredDirection::None;
        }
    }

    // The only place an AuthorizedMotorCommand is constructed. `mode`/`magnitude`
    // have already been validated by the caller of authorize(), or are a
    // hard-coded safe stop.
    static AuthorizedMotorCommand commandFor(MotorMode mode, uint16_t magnitudePermille) {
        return AuthorizedMotorCommand(MotorEffort(mode, magnitudePermille));
    }

    bool armed_ = false;
    FaultState latchedFault_{};
    PoweredDirection lastDriveDirection_ = PoweredDirection::None;
};

}  // namespace ev
