#pragma once

#include "CompetitionStateMachine.h"
#include "MotionProfile.h"
#include "SafetyAuthority.h"
#include "VehicleState.h"
#include "VelocityController.h"
#include "VelocityEstimator.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The new-run reset sequence for the whole chain - pure orchestration
// ---------------------------------------------------------------------------
//
// Starting a run is not one module's job. Every module already guarantees its
// own reset, but nothing owns the *sequence*, and prose is not a mechanism:
//
//   CompetitionStateMachine::reset(nowMs)  idle, no announcement, no fault of
//                                      record, no reversal sequence, no verified
//                                      stop - which also resets the
//                                      ReversalSequencer and finishing
//                                      verifier it owns; Idle is stamped with
//                                      the caller's clock reading, so
//                                      phaseElapsedMs counts from this boundary
//   VelocityController::reset()        disable() plus dropped integral, time base
//                                      and remembered direction
//   VelocityEstimator::reset()         no samples, no window, no claim, no counters
//   MotionProfileTarget::resetForNewRun()   target back to exactly zero
//   SafetyAuthority::reset()           disarmed, no latched fault; the remembered
//                                      powered direction is PRESERVED, because a
//                                      software reset is not a verified stop and
//                                      must not erase the direction barrier
//                                      (S18A/P2)
//
// Three things make the sequence more than a list:
//
// * `enable()` on a running regulator is a deliberate no-op, not a fresh start,
//   so a new run must go disable() -> enable() to get a fresh control state.
// * The profile holds no state, so it cannot zero the caller's target. That is
//   `MotionProfileTarget`, and nothing else may clear the stale target.
// * An authority reset must not be followed by an implicit arm. Arming stays a
//   separate, deliberate transition the vehicle layer makes after this call.
//
// What this module is not
//
// * It does not arm, drive, brake or decide anything about a run. It resets
//   per-run state and reports what it found and whether the regulator could be
//   re-enabled, and that is all.
// * It does not touch hardware and owns no clock, pin or Arduino type
//   (SOFTWARE_ARCHITECTURE.md section 2). The vehicle layer calls it, and the
//   encoder adapter alone resets the ISR-shared count and reference afterwards.
//   The decoder is deliberately not accepted here: a pure helper cannot protect
//   its multi-field mutation against the ISR (S18C-M1). Its one clock
//   argument, `nowMs`, is the caller's `millis()` reading: it is passed straight
//   to `CompetitionStateMachine::reset()` to stamp the new Idle phase, and
//   nothing here reads a clock.
// * It does not decide that the chain is safe: the regulator is left enabled so
//   the loop can regulate, but powered motion still requires an `arm()` that
//   only succeeds on complete readiness data, and a run that may travel still
//   requires a fresh announcement to the state machine.
//
// Caller obligation after the call: reset/re-reference the encoder through its
// owner before consuming new-run measurements. Production uses EncoderInput's
// single PRIMASK-preserving reset/reference operation; a synthetic local decoder
// can be reset directly by its test owner. This sequence does not prove physical
// standstill, discharge direction history, or preserve every edge during reset.

// What the reset found and what state it left behind. No default constructor:
// the report is the only evidence the caller has about a boundary that is
// otherwise silent.
struct MotionChainResetReport {
    constexpr MotionChainResetReport(bool controllerEnabled_, FaultCode clearedFault_,
                                     RunPhase clearedRunPhase_)
        : controllerEnabled(controllerEnabled_),
          clearedFault(clearedFault_),
          clearedRunPhase(clearedRunPhase_) {}

    // True when the regulator's fresh control state was enabled. False means the
    // configuration is not usable (unmeasured gains, malformed values) and
    // `enable()` refused; the chain must not be armed while this is false.
    bool controllerEnabled;

    // A fault that the reinitialization discarded, or FaultCode::None. The fault
    // is reported so the run log can record what ended the previous run before
    // `SafetyAuthority::reset()` clears it.
    FaultCode clearedFault;

    // The run phase the state machine was in, for the same reason: a run that
    // ended in Done and a run that was abandoned in Approach look identical
    // afterwards, and the log should say which one it was.
    RunPhase clearedRunPhase;
};

// Resets the pure per-run state (not the encoder adapter's measurement epoch),
// and leaves the regulator enabled on that fresh state.
//
// It never arms the authority and never commands motion. Returns the report
// above so a refusal to re-enable, a discarded fault, or the phase the previous
// run ended in is visible instead of silent.
inline MotionChainResetReport resetMotionChainForNewRun(VelocityEstimator& estimator,
                                                        VelocityController& controller,
                                                        MotionProfileTarget& target,
                                                        SafetyAuthority& authority,
                                                        CompetitionStateMachine& run,
                                                        uint32_t nowMs) {
    // Read what the boundary is about to discard, before discarding it.
    const FaultCode clearedFault = authority.latchedFault().code;
    const RunPhase clearedRunPhase = run.phase();

    // 1. End the run policy first, so no intention, no reversal permission and no
    //    verified stop survives into the next run. The sequencer and the final
    //    stop verifier are reset with it, because the machine owns them. The
    //    caller's clock reading stamps the new Idle phase.
    run.reset(nowMs);

    // 2. End the regulator's control state, so no integral, time base or
    //    remembered direction survives into the new run. This is why the
    //    sequence is disable() then enable() and not just enable().
    controller.disable();

    // 3. Discard the estimator history. The caller resets the encoder through
    //    its owner, never through this interrupt-unaware pure orchestration.
    estimator.reset();

    // 4. Zero the caller-held profile target. A stale target would silently
    //    reshape the new ramp; the profile cannot clear it because it holds no
    //    state.
    target.resetForNewRun();

    // 5. Reinitialize the safety boundary last: disarmed, no fault of record.
    //    The remembered powered direction survives on purpose (S18A/P2): the
    //    drivetrain may still be moving, so the next run's first opposite-
    //    direction drive stays refused until a fresh verified stop discharges
    //    it through the authority's zero-speed path. Arming remains a separate
    //    decision, and a reset never resumes motion by itself.
    authority.reset();

    // 6. Start the new regulator state. Refused when the configuration is not
    //    usable; the report carries that verdict instead of hiding it.
    const bool controllerEnabled = controller.enable();
    return MotionChainResetReport(controllerEnabled, clearedFault, clearedRunPhase);
}

}  // namespace ev
