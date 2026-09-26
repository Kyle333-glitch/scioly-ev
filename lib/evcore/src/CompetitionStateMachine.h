#pragma once

#include "Motion.h"
#include "NearZeroVerifier.h"
#include "ReversalSequencer.h"
#include "RunConfig.h"
#include "Units.h"
#include "VehicleState.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// The competition run state machine - run intent only
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One explicit state machine for one competition run, with the frozen phases:
//
//       Idle -> ParameterValid -> Armed -> OptionalDelay -> Approach -> Capture
//            -> Push -> ForwardDecel -> BrakeZeroSpeed -> Reverse
//            -> FinalApproach -> FinalBrake -> Done        (or Fault, from
//                                                            anywhere)
//
// * It consumes: the announced, rule-validated `RunConfig` (once, at the
//   announcement boundary), the safety authority's armed fact, trigger/event
//   evidence, the run-axis remaining distance, and fault evidence. It emits one
//   thing: what the run layer should ask the motion chain for this cycle
//   (`RunMotionCommand`). The vehicle layer maps that onto
//   `MotionProfileIntent`/`MotionDirection`/the regulator and presents exactly
//   one request per cycle to `SafetyAuthority`, as section 13 requires.
// * Every phase has an understandable exit condition. Phases whose exit depends
//   on a physical event the firmware has not measured - a bottle contact, a
//   captured bottle, a completed push, a verified stop - wait for that evidence
//   and are bounded by a configured timeout. A phase that never gets its exit
//   evidence faults instead of waiting forever or guessing.
// * The reversal is requested through the reversal contract, not re-implemented:
//   entering `ForwardDecel` begins a `ReversalSequencer`, `BrakeZeroSpeed`
//   consumes its permission, and a refusal from it faults. The steady-state
//   "ramp to zero, brake, hold, verify near zero, allow the direction change" is
//   owned by that module.
//
// What this module is not
//
// * It knows no hardware: no pin, no PWM, no driver truth table, no encoder
//   counts or counts-per-revolution, no gain, no window, no regulator internals.
//   Its outputs are commands from `Motion.h` vocabulary, never an effort and
//   never an authorized command.
// * It is not the safety authority and must never become one. It does not arm,
//   does not authorize, does not know `SafetyAuthority`'s direction latch, and
//   its own permission to reverse is not a substitute for the authority's
//   independent refusal of a direct flip (SOFTWARE_ARCHITECTURE.md section 5).
// * It does not decide geometry or strategy. The announced `RunConfig` is
//   consumed at the announcement boundary (validated, and then not stored),
//   and no phase transition is derived from those distances: where the vehicle
//   is on the run axis, what "the bottle line" means for the mechanism, and how
//   far the vehicle must travel before the push is complete are run-layer
//   calculations built on measured, still-UNVERIFIED facts (#4, #6, #11). The
//   state machine consumes their *results* as evidence.
// * It does not ramp, regulate, brake or verify by itself. The ramp belongs to
//   `MotionProfile`, the near-zero policy to `NearZeroVerifier`, the reversal
//   sequencing to `ReversalSequencer`, the permission to `SafetyAuthority`.
// * It contains no `delay()`, no clock read and no timeout literal: every bound
//   and every distance arrives in `RunPhaseConfig` as a `Sourced` value, and a
//   policy configuration that is not `ValueSource::Measured` is refused at the
//   announcement boundary rather than used to gate a run.

// The frozen behavioral states of a competition run. Names and semantics are
// fixed by the run plan; `Idle` is the zero value, so a default-built state
// machine is the safe one.
enum class RunPhase : uint8_t {
    // No run announced. The run layer keeps its own unpowered idle output.
    Idle = 0,
    // The announced parameters have been accepted. The run layer may arm.
    ParameterValid = 1,
    // The safety authority reports armed; waiting for the start trigger.
    Armed = 2,
    // The deliberate pre-travel delay is being spent.
    OptionalDelay = 3,
    // Forward travel toward the bottle.
    Approach = 4,
    // Bottle contact observed; the mechanism engages it.
    Capture = 5,
    // Driving the bottle forward until the push objective is reported complete.
    Push = 6,
    // Bringing forward travel to zero: the reversal sequence is ramping down.
    // This is the run's *presentation* (`RampDown*`), not the evidence: the
    // owned `ReversalSequencer`'s `RampingDown` sub-phase is where the ramp-down
    // actually is, and `RunStatus::reversalPhase` reports it. The split is
    // deliberate and frozen - see `advanceReversal()` for the full mapping.
    ForwardDecel = 7,
    // The deliberate stop is being held while the near-zero stop is verified.
    // Also a presentation: underneath, the sequencer holds `Braking`,
    // `VerifyingNearZero` or `DirectionChangeReady`. The run presents `Brake*`
    // for all three and keeps presenting it for one extra handoff cycle after
    // the permission is earned, so the stop cycle is explicit rather than
    // coincidental.
    BrakeZeroSpeed = 8,
    // The verified direction change has been taken; travelling back.
    Reverse = 9,
    // The last stretch of the reverse leg, before the Target Point.
    FinalApproach = 10,
    // The final stop is being held while it is verified.
    FinalBrake = 11,
    // The run is complete: stopped, verified, and no further drive is asked for.
    Done = 12,
    // A fault of record. Only reset() leaves this phase.
    Fault = 13,
};

// True for the phases in which the run has started and must stay armed. A disarm
// in one of these ends the run; a disarm earlier is a normal abort, and a disarm
// after Done is the post-run shutdown and is not a fault.
inline bool isRunPhaseUnderway(RunPhase phase) {
    switch (phase) {
        case RunPhase::OptionalDelay:
        case RunPhase::Approach:
        case RunPhase::Capture:
        case RunPhase::Push:
        case RunPhase::ForwardDecel:
        case RunPhase::BrakeZeroSpeed:
        case RunPhase::Reverse:
        case RunPhase::FinalApproach:
        case RunPhase::FinalBrake:
            return true;
        case RunPhase::Idle:
        case RunPhase::ParameterValid:
        case RunPhase::Armed:
        case RunPhase::Done:
        case RunPhase::Fault:
            return false;
    }
    return false;
}

// What the run asks the motion chain for this cycle.
//
// The travel direction is part of the command, not a separate argument, so a
// stop can never be applied to the wrong leg by a caller that forgot to pass a
// direction. `None` is not a stop command: it means the run makes no motion
// request at all, and the vehicle layer keeps its own unpowered output.
//
// The Forward/Reverse suffix on a stop command is a *leg tag*, not a powered
// direction. A non-powered stop has no powered direction, and the tag exists
// only so a stop cannot be attached to the wrong leg. The vehicle layer must
// map `RampDown*` and `Brake*` onto a non-powered request - a zero target for
// the profile, a non-Driving mode for the authority - and must never derive a
// `PoweredDirection` from the tag. A stop command is therefore never something
// to "complete" with a drive in the tagged direction; the tag is invisible to
// `SafetyAuthority`, which will refuse a direct flip independently.
enum class RunMotionCommand : uint8_t {
    None = 0,
    DriveForward = 1,
    DriveReverse = 2,
    RampDownForward = 3,
    RampDownReverse = 4,
    BrakeForward = 5,
    BrakeReverse = 6,
};

// True for the commands that ask for powered travel (`MotionProfileIntent::
// Travel`). A consumer must never emit one that is not current for its cycle
// (`runIntentionIsCurrentFor`).
inline bool isRunTravelCommand(RunMotionCommand command) {
    return command == RunMotionCommand::DriveForward || command == RunMotionCommand::DriveReverse;
}

// True for the commands that ask for a deliberate non-powered stop: a target
// ramp to zero (`RampDown*`) or an active brake hold (`Brake*`).
inline bool isRunStopCommand(RunMotionCommand command) {
    switch (command) {
        case RunMotionCommand::RampDownForward:
        case RunMotionCommand::RampDownReverse:
        case RunMotionCommand::BrakeForward:
        case RunMotionCommand::BrakeReverse:
            return true;
        case RunMotionCommand::None:
        case RunMotionCommand::DriveForward:
        case RunMotionCommand::DriveReverse:
            return false;
    }
    return false;
}

// One cycle's intent, stamped with the clock reading that produced it.
struct RunMotionIntention {
    constexpr RunMotionIntention(RunMotionCommand command_, uint32_t emittedAtMs_)
        : command(command_), emittedAtMs(emittedAtMs_) {}

    RunMotionCommand command;

    // The `millis()` reading of the update that produced this intention. Not a
    // physical duration.
    uint32_t emittedAtMs;
};

// An intention is valid for the cycle it was produced in and for no other.
//
// This is the single verdict a consumer uses to refuse a stale run command: a
// Travel intention held across a skipped cycle, a stalled control loop, or a run
// that has since ended must not be driven on. A run that reaches Done or Fault
// never emits a travel command again, so a stale Travel intention can only come
// from a caller that kept one from an earlier cycle - and this is how it finds
// out.
inline bool runIntentionIsCurrentFor(const RunMotionIntention& intention, uint32_t nowMs) {
    return intention.emittedAtMs == nowMs;
}

// The run policy: every bound and distance the phase transitions use. Nothing
// here has a default, because nothing here has been measured; the vehicle layer
// supplies each value with its source.
struct RunPhaseConfig {
    constexpr RunPhaseConfig(Sourced<double> optionalDelayS_, Sourced<double> approachTimeoutS_,
                             Sourced<double> captureTimeoutS_, Sourced<double> pushTimeoutS_,
                             Sourced<double> reverseTimeoutS_, Sourced<double> finalApproachTimeoutS_,
                             Sourced<double> finalStopTimeoutS_,
                             Sourced<double> finalApproachDistanceM_,
                             NearZeroConfig stopVerification_)
        : optionalDelayS(optionalDelayS_),
          approachTimeoutS(approachTimeoutS_),
          captureTimeoutS(captureTimeoutS_),
          pushTimeoutS(pushTimeoutS_),
          reverseTimeoutS(reverseTimeoutS_),
          finalApproachTimeoutS(finalApproachTimeoutS_),
          finalStopTimeoutS(finalStopTimeoutS_),
          finalApproachDistanceM(finalApproachDistanceM_),
          stopVerification(stopVerification_) {}

    // Deliberate pre-travel delay, seconds. 0 means the OptionalDelay phase is
    // skipped. This is a run strategy value, not a rule value; the profile's
    // `MotionTimingIntent::intentionalDelayS` must be fed from the same source,
    // because a margin computed against a delay the run did not spend is a
    // number about a different run.
    Sourced<double> optionalDelayS;

    // Bounds, seconds. A phase that never receives its exit evidence ends at
    // Fault(PhaseTimeout) instead of waiting forever, and a bound is a deadline
    // rather than only a wait limit: exit evidence that arrives after it faults
    // the run instead of being accepted late, because a phase that overran its
    // bound is no longer the run that was announced. `ForwardDecel` and
    // `BrakeZeroSpeed` deliberately have no bound here: the reversal sequence
    // carries its own single timeout, which covers both.
    Sourced<double> approachTimeoutS;        // -> Capture, on bottle contact
    Sourced<double> captureTimeoutS;         // -> Push, on capture complete
    Sourced<double> pushTimeoutS;            // -> ForwardDecel, on push complete
    Sourced<double> reverseTimeoutS;         // -> FinalApproach, on the closing distance
    Sourced<double> finalApproachTimeoutS;   // -> FinalBrake, on reaching the Target Point
    Sourced<double> finalStopTimeoutS;       // -> Done, on the stop being verified

    // The last stretch of the reverse leg, metres of remaining distance at which
    // the run moves into the final approach phase.
    Sourced<double> finalApproachDistanceM;

    // How the final stop is verified (threshold and dwell). The reversal gate is
    // configured separately (`ReversalConfig::verification`) because the two
    // asks are different: the reversal needs enough evidence to permit a
    // powered flip, while the final stop is the event's Complete Stop
    // (rule 8.p) and is expected to be held much longer. Feeding both from one
    // value would silently make one of them wrong.
    NearZeroConfig stopVerification;
};

enum class RunPhaseConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,             // at least one value is not ValueSource::Measured
    NonFiniteValue,                   // NaN or infinity
    NegativeDelay,                    // optionalDelayS below zero
    NonPositiveTimeout,               // a phase bound that is not > 0
    NonPositiveFinalApproachDistanceM,  // the final approach distance is not > 0
    StopVerificationNotUsable,        // the NearZeroConfig failed its own validation
};

struct RunPhaseConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    RunPhaseConfigError error;
};

// Single entry point for the run policy's validation. The stop-verification
// configuration is validated by `validateNearZeroConfig()` rather than
// re-checked here, so the threshold/dwell policy keeps one definition.
inline RunPhaseConfigValidation validateRunPhaseConfig(const RunPhaseConfig& config) {
    if (!config.optionalDelayS.isMeasured() || !config.approachTimeoutS.isMeasured() ||
        !config.captureTimeoutS.isMeasured() || !config.pushTimeoutS.isMeasured() ||
        !config.reverseTimeoutS.isMeasured() || !config.finalApproachTimeoutS.isMeasured() ||
        !config.finalStopTimeoutS.isMeasured() || !config.finalApproachDistanceM.isMeasured()) {
        return {false, RunPhaseConfigError::ValuesAreNotMeasured};
    }
    if (!std::isfinite(config.optionalDelayS.value) ||
        !std::isfinite(config.approachTimeoutS.value) ||
        !std::isfinite(config.captureTimeoutS.value) || !std::isfinite(config.pushTimeoutS.value) ||
        !std::isfinite(config.reverseTimeoutS.value) ||
        !std::isfinite(config.finalApproachTimeoutS.value) ||
        !std::isfinite(config.finalStopTimeoutS.value) ||
        !std::isfinite(config.finalApproachDistanceM.value)) {
        return {false, RunPhaseConfigError::NonFiniteValue};
    }
    if (config.optionalDelayS.value < 0.0) {
        return {false, RunPhaseConfigError::NegativeDelay};
    }
    if (!(config.approachTimeoutS.value > 0.0) || !(config.captureTimeoutS.value > 0.0) ||
        !(config.pushTimeoutS.value > 0.0) || !(config.reverseTimeoutS.value > 0.0) ||
        !(config.finalApproachTimeoutS.value > 0.0) || !(config.finalStopTimeoutS.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveTimeout};
    }
    if (!(config.finalApproachDistanceM.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveFinalApproachDistanceM};
    }
    if (!validateNearZeroConfig(config.stopVerification).ok) {
        return {false, RunPhaseConfigError::StopVerificationNotUsable};
    }
    return {true, RunPhaseConfigError::None};
}

// Everything the vehicle layer observed this cycle. Every field defaults to the
// unusable value, so a default-built packet starts nothing and confirms nothing.
struct RunInputs {
    // The safety authority reports armed (`SafetyAuthority::isArmed()`). The run
    // never arms itself and this fact is what moves it out of ParameterValid.
    bool authorityArmed = false;

    // The go signal. A level, not an edge: a trigger that is already asserted
    // when the run reaches Armed starts it on that update.
    bool startTriggered = false;

    // Bottle-interaction evidence. None of these is derivable from the announced
    // geometry yet: the mechanism and its contact behavior are UNVERIFIED
    // (VERIFIED_STATE.md #11). The run layer reports what it actually observed,
    // each phase waits with a configured bound, and a bound that expires is a
    // fault rather than a guessed transition.
    bool bottleContactDetected = false;
    bool captureComplete = false;
    bool pushComplete = false;

    // Remaining distance along the run axis to the Target Point, metres, with
    // its own usability. The vehicle layer owns the reconciliation between the
    // run axis and the encoder's tick convention (Odometry.h,
    // MotionProfileInputs::remainingDistanceM); this machine only compares the
    // reconciled number against its configured final-approach distance.
    bool remainingDistanceUsable = false;
    double remainingDistanceM = 0.0;

    // The evidence the near-zero verification rests on. The reversal sequence
    // consumes it while the run is bringing the forward leg to a stop, and the
    // final stop consumes it after the reverse leg.
    NearZeroEvidence stopEvidence;

    // A fault the vehicle layer observed this cycle (None when healthy).
    FaultCode detectedFault = FaultCode::None;
};

// Why an announcement did not start a run.
enum class RunStartStatus : uint8_t {
    // Accepted: the run is now in ParameterValid.
    Started = 0,
    // The announced parameters failed rule validation; the run faults.
    ConfigInvalid,
    // This machine's own phase policy is not usable; the run faults. A run may
    // not be sequenced with placeholder bounds.
    PolicyConfigNotUsable,
    // A run has already been announced and not yet reset.
    RunAlreadyActive,
    // A fault is of record. reset() is required before a new announcement.
    FaultLatched,
};

// The phase's answer for one cycle. Deliberately no default constructor: a phase
// with no facts behind it must be impossible to write.
struct RunStatus {
    constexpr RunStatus(RunPhase phase_, RunMotionIntention intention_, FaultCode fault_,
                        uint32_t faultRaisedAtMs_, bool stopVerified_,
                        ReversalPhase reversalPhase_, uint32_t phaseElapsedMs_)
        : phase(phase_),
          intention(intention_),
          fault(fault_),
          faultRaisedAtMs(faultRaisedAtMs_),
          stopVerified(stopVerified_),
          reversalPhase(reversalPhase_),
          phaseElapsedMs(phaseElapsedMs_) {}

    RunPhase phase;

    // What the run asks the vehicle layer for this cycle.
    RunMotionIntention intention;

    // The fault of record. Non-None exactly when phase == Fault.
    FaultCode fault;

    // Clock reading of the update that latched the fault, milliseconds. 0 when
    // no fault is of record.
    uint32_t faultRaisedAtMs;

    // The near-zero verdict for the stop the run is presenting right now: true
    // while `BrakeZeroSpeed` holds a stop the reversal gate verified, and while
    // `FinalBrake`/`Done` hold the verified final stop. False in every phase that
    // asks for travel, so it is never a claim about a vehicle that is still
    // moving and a later leg cannot inherit an earlier leg's stop. This is the
    // fact the vehicle layer maps onto `SafetySystemState::atZeroSpeed`, together
    // with the interval it was observed over - which is what the verifier
    // combined.
    bool stopVerified;

    // The reversal sequencer's phase. Diagnostic: it says how far the reversal
    // sequencing has got when the run is in ForwardDecel/BrakeZeroSpeed.
    ReversalPhase reversalPhase;

    // Time in the current phase, milliseconds, measured from the clock reading
    // that entered it. A machine that has never been reset counts from its
    // construction, which is where `Idle` was entered; a reset stamps `Idle`
    // with the reading passed to it.
    uint32_t phaseElapsedMs;
};

// Maps a reversal refusal onto the fault it raises. This is the one place the
// two vocabularies meet; the sequencer itself never names a fault.
inline FaultCode faultForReversalRefusal(ReversalRefusal refusal) {
    switch (refusal) {
        case ReversalRefusal::None:
            return FaultCode::None;
        case ReversalRefusal::ConfigIsNotUsable:
            // A verification that rests on a placeholder may not gate a
            // direction change; the missing measurement is the fault.
            return FaultCode::ProvisionalCalibration;
        case ReversalRefusal::TimedOut:
            return FaultCode::ReversalNotVerified;
    }
    return FaultCode::ReversalNotVerified;
}

// ---------------------------------------------------------------------------
// CompetitionStateMachine
// ---------------------------------------------------------------------------
//
// `nowMs` is the caller's `millis()` reading on every call; the class never
// reads a clock. It holds the run's phase, the fault of record, and the reversal
// sequence and final-stop verifier it owns - nothing else, and no announced
// parameters. It commands nothing: every output is an intention for the vehicle
// layer, which is the only place that talks to the motion chain and the
// authority.
class CompetitionStateMachine {
public:
    CompetitionStateMachine(const RunPhaseConfig& config, const ReversalConfig& reversalConfig)
        : config_(config), sequencer_(reversalConfig), stopVerifier_(config.stopVerification) {}

    const RunPhaseConfig& config() const { return config_; }

    bool configUsable() const {
        return validateRunPhaseConfig(config_).ok && sequencer_.configUsable();
    }

    RunPhase phase() const { return phase_; }

    // The fault of record, or FaultCode::None.
    FaultCode faultCode() const { return faultCode_; }

    ReversalPhase reversalPhase() const { return sequencer_.phase(); }

    // The near-zero fact the vehicle layer maps onto `atZeroSpeed`, derived from
    // the phase the run is in rather than OR-ed across the run's history: a stop
    // that was verified for one leg must not be reported as the vehicle being
    // stopped during the next one. It is true exactly while the run is presenting
    // a stop whose evidence has been verified - the reversal gate's stop in
    // `BrakeZeroSpeed`, the verified final stop in `FinalBrake` and `Done` - so
    // the vehicle layer may present it while it presents the stop, and never
    // while the run asks for travel.
    //
    // One stop, one verifier, one latch, at the level that can see the evidence:
    // this is not a third copy of anyone's boolean. The reversal gate's stop is
    // verified by the `NearZeroVerifier` inside `ReversalSequencer` and latched
    // there (`sequencer_.stopVerified()`); the final stop is verified by this
    // machine's own `stopVerifier_` and latched here (`finalStopVerified_`, which
    // survives into `Done`). This accessor only reports the latch of the stop the
    // run is presenting right now; it never re-derives or OR-s them.
    bool stopVerified() const {
        switch (phase_) {
            case RunPhase::BrakeZeroSpeed:
                return sequencer_.stopVerified();
            case RunPhase::FinalBrake:
            case RunPhase::Done:
                return finalStopVerified_;
            case RunPhase::Idle:
            case RunPhase::ParameterValid:
            case RunPhase::Armed:
            case RunPhase::OptionalDelay:
            case RunPhase::Approach:
            case RunPhase::Capture:
            case RunPhase::Push:
            case RunPhase::ForwardDecel:
            case RunPhase::Reverse:
            case RunPhase::FinalApproach:
            case RunPhase::Fault:
                return false;
        }
        return false;
    }

    // Returns the machine to its post-construction state: Idle (no run accepted,
    // so a new one must be announced), no fault of record, no reversal sequence,
    // no verified stop, no pending handoff. It never arms and never commands
    // motion; re-arming and re-announcing stay separate, deliberate decisions.
    // This is the only way out of Fault and Done.
    //
    // `nowMs` is the caller's clock reading, not a duration: it stamps the Idle
    // phase, so `phaseElapsedMs` measures from the reset that entered Idle rather
    // than from machine construction. The class still reads no clock.
    void reset(uint32_t nowMs) {
        phase_ = RunPhase::Idle;
        faultCode_ = FaultCode::None;
        faultRaisedAtMs_ = 0U;
        phaseEnteredMs_ = nowMs;
        legDirection_ = MotionDirection::Forward;
        handoffPending_ = false;
        finalStopVerified_ = false;
        sequencer_.reset();
        stopVerifier_.reset();
    }

    // Announces the run's parameters, with the validation verdict the vehicle
    // layer derived from `ev::validateRunConfig()` - the single validation entry
    // point, which this machine calls again as a cross-check rather than
    // trusting a verdict that arrived separately from the configuration it was
    // about. Both must agree, so a caller bug cannot announce an unvalidated run.
    //
    // The announced parameters are the vehicle layer's to hold: they are
    // consumed at this boundary and not stored, because no phase transition is
    // allowed to derive geometry from them. A run that cannot be validated, or a
    // machine whose own policy is not usable, faults here and requires an
    // explicit reset(): a half-corrected announcement must not leave a run
    // waiting that a later call can start.
    RunStartStatus announceRun(const RunConfig& config, const RunConfigValidation& validation,
                               uint32_t nowMs) {
        if (phase_ == RunPhase::Fault) {
            return RunStartStatus::FaultLatched;
        }
        if (phase_ != RunPhase::Idle) {
            return RunStartStatus::RunAlreadyActive;
        }
        if (!validation.ok || !validateRunConfig(config).ok) {
            latchFault(FaultCode::InvalidRunConfig, nowMs);
            return RunStartStatus::ConfigInvalid;
        }
        if (!configUsable()) {
            // A policy that rests on a placeholder (or is malformed) may not
            // sequence a run: the missing measurement is the fault of record.
            latchFault(FaultCode::ProvisionalCalibration, nowMs);
            return RunStartStatus::PolicyConfigNotUsable;
        }

        enterPhase(RunPhase::ParameterValid, nowMs);
        return RunStartStatus::Started;
    }

    // One run step.
    RunStatus update(uint32_t nowMs, const RunInputs& inputs) {
        // Fault evidence first, from every phase: nothing is acted on in a cycle
        // that reports a fault, and the response does not depend on what the
        // run was doing.
        if (inputs.detectedFault != FaultCode::None) {
            latchFault(inputs.detectedFault, nowMs);
        }

        // An underway run requires the armed state it started with. Losing it
        // means the authority is refusing drive, so the run is over; waiting for
        // evidence that can no longer arrive would be a silent stall.
        if (phase_ != RunPhase::Fault && isRunPhaseUnderway(phase_) && !inputs.authorityArmed) {
            latchFault(FaultCode::DisarmedDuringRun, nowMs);
        }

        // The phase's configured bound, where it has one. A phase entered this
        // cycle has an elapsed time of zero, so a bound cannot fire on entry.
        if (phase_ != RunPhase::Fault && phase_ != RunPhase::Done) {
            const uint32_t boundMs = phaseTimeoutMs();
            if (boundMs != 0U && phaseElapsedMs(nowMs) >= boundMs) {
                latchFault(FaultCode::PhaseTimeout, nowMs);
            }
        }

        if (phase_ != RunPhase::Fault) {
            advance(nowMs, inputs);
        }

        return statusFor(nowMs);
    }

private:
    // The per-phase transition logic. Entered only when no fault is of record.
    void advance(uint32_t nowMs, const RunInputs& inputs) {
        switch (phase_) {
            case RunPhase::Idle:
                // Only announceRun() leaves Idle.
                break;

            case RunPhase::ParameterValid:
                if (inputs.authorityArmed) {
                    enterPhase(RunPhase::Armed, nowMs);
                }
                break;

            case RunPhase::Armed:
                if (!inputs.authorityArmed) {
                    // A disarm before the run has started is an abort, not a
                    // fault: nothing has moved and the announcement still
                    // stands.
                    enterPhase(RunPhase::ParameterValid, nowMs);
                    break;
                }
                if (inputs.startTriggered) {
                    enterPhase(config_.optionalDelayS.value > 0.0 ? RunPhase::OptionalDelay
                                                                  : RunPhase::Approach,
                               nowMs);
                }
                break;

            case RunPhase::OptionalDelay:
                if (phaseElapsedMs(nowMs) >= msFromSeconds(config_.optionalDelayS.value)) {
                    enterPhase(RunPhase::Approach, nowMs);
                }
                break;

            case RunPhase::Approach:
                if (inputs.bottleContactDetected) {
                    enterPhase(RunPhase::Capture, nowMs);
                }
                break;

            case RunPhase::Capture:
                if (inputs.captureComplete) {
                    enterPhase(RunPhase::Push, nowMs);
                }
                break;

            case RunPhase::Push:
                if (inputs.pushComplete) {
                    enterPhase(RunPhase::ForwardDecel, nowMs);
                    beginReversalForFinalLeg(nowMs);
                }
                break;

            case RunPhase::ForwardDecel:
            case RunPhase::BrakeZeroSpeed:
                advanceReversal(nowMs, inputs);
                break;

            case RunPhase::Reverse:
                if (distanceEvidenceUsable(inputs) &&
                    inputs.remainingDistanceM <= config_.finalApproachDistanceM.value) {
                    enterPhase(RunPhase::FinalApproach, nowMs);
                }
                break;

            case RunPhase::FinalApproach:
                if (distanceEvidenceUsable(inputs) && inputs.remainingDistanceM <= 0.0) {
                    enterPhase(RunPhase::FinalBrake, nowMs);
                }
                break;

            case RunPhase::FinalBrake: {
                const NearZeroVerdict verdict = stopVerifier_.update(nowMs, inputs.stopEvidence);
                if (verdict.status == NearZeroStatus::ConfigIsNotUsable) {
                    latchFault(FaultCode::ProvisionalCalibration, nowMs);
                    break;
                }
                if (verdict.verified) {
                    finalStopVerified_ = true;
                    enterPhase(RunPhase::Done, nowMs);
                }
                break;
            }

            case RunPhase::Done:
            case RunPhase::Fault:
                // Terminal phases. Only reset() leaves either one.
                break;
        }
    }

    // The braking/reversal half of the run: ForwardDecel ramps the forward leg
    // to zero and BrakeZeroSpeed consumes the earned direction change.
    //
    // Presentation and evidence are deliberately separate here, and this is the
    // single place that maps one onto the other:
    //
    //   run presents   run phase        evidence (owned sequencer sub-phase)
    //   RampDown*      ForwardDecel     RampingDown
    //   Brake*         BrakeZeroSpeed   Braking | VerifyingNearZero |
    //                                    DirectionChangeReady
    //
    // `ForwardDecel` and `BrakeZeroSpeed` are what the run asks the vehicle layer
    // for. What has actually been established about the stop is the sequencer's
    // sub-phase, reported for diagnostics, and the run keeps no second copy of
    // that sequence: the only thing it latches at this boundary is the one-cycle
    // stop handoff below (`handoffPending_`), and the stop verdict it reports in
    // `BrakeZeroSpeed` is the sequencer's own latch.
    void advanceReversal(uint32_t nowMs, const RunInputs& inputs) {
        const ReversalStatus reversal = sequencer_.update(nowMs, inputs.stopEvidence);

        // ForwardDecel and BrakeZeroSpeed deliberately have no bound of their own:
        // the reversal sequence's single timeout is what bounds this half of the
        // run. A sequence that is neither running nor refusing therefore cannot be
        // waited on - there would be nothing left to expire and the run would
        // present a ramp-down forever - so it is a sequencing inconsistency and
        // faults. Both cases below are unreachable while the sequencer's own
        // invariants hold, which is exactly why they must not be silent.
        if (reversal.phase == ReversalPhase::Idle) {
            latchFault(FaultCode::RunSequencingError, nowMs);
            return;
        }
        if (reversal.phase == ReversalPhase::StopRequired) {
            const FaultCode refusalFault = faultForReversalRefusal(reversal.refusal);
            latchFault(refusalFault == FaultCode::None ? FaultCode::RunSequencingError : refusalFault,
                       nowMs);
            return;
        }

        if (phase_ == RunPhase::ForwardDecel) {
            // The ramp-down is over once the vehicle is within the near-zero
            // threshold: from here the deliberate stop is what the run presents.
            if (reversal.phase == ReversalPhase::Braking ||
                reversal.phase == ReversalPhase::VerifyingNearZero ||
                reversal.phase == ReversalPhase::DirectionChangeReady) {
                enterPhase(RunPhase::BrakeZeroSpeed, nowMs);
            }
            return;
        }

        // BrakeZeroSpeed. The earned change is consumed only after one full
        // cycle of the deliberate stop has been presented, because a
        // non-powered stop taken while zero speed is reported is what clears the
        // authority's remembered powered direction; a travel command in the same
        // cycle would be refused by the authority, correctly and invisibly to
        // this machine. The run therefore presents the stop for one cycle, then
        // asks to travel, and the authority independently refuses if anything
        // about the handoff was not true.
        if (handoffPending_) {
            handoffPending_ = false;
            sequencer_.reset();
            legDirection_ = oppositeOf(legDirection_);
            enterPhase(RunPhase::Reverse, nowMs);
            return;
        }
        if (reversal.phase == ReversalPhase::DirectionChangeReady) {
            handoffPending_ = true;
        }
    }

    // Whether the run's distance evidence may be acted on: the vehicle layer's
    // usability verdict plus a finite number. A non-finite remaining distance is
    // not a distance, and acting on it would end the travel early on arithmetic
    // that produced nothing - a negative infinity satisfies "at or inside the
    // final approach" and "at or past the Target Point". It is therefore treated
    // as no evidence at all, and the phase's own configured bound is what faults
    // the run instead of a comparison being satisfied by an infinity.
    static bool distanceEvidenceUsable(const RunInputs& inputs) {
        return inputs.remainingDistanceUsable && std::isfinite(inputs.remainingDistanceM);
    }

    // Begins the reversal sequence for the run's final leg. Once per run, from
    // Push.
    void beginReversalForFinalLeg(uint32_t nowMs) {
        const MotionDirection from = legDirection_;
        const MotionDirection to = oppositeOf(legDirection_);
        switch (sequencer_.beginReversal(from, to, nowMs)) {
            case ReversalBeginStatus::Started:
                break;
            case ReversalBeginStatus::AlreadyActive:
                // A sequence is already sequencing this reversal; it is not
                // restarted, because restarting would restart its timeout.
                break;
            case ReversalBeginStatus::ConfigIsNotUsable:
                latchFault(FaultCode::ProvisionalCalibration, nowMs);
                break;
            case ReversalBeginStatus::StopLatched:
            case ReversalBeginStatus::NotNeeded:
                // Neither is reachable from here: the run resets the sequencer
                // at every boundary, and the final reversal always changes
                // direction. Observing one means the sequencing itself is
                // inconsistent, which stops the run rather than being papered
                // over.
                latchFault(FaultCode::RunSequencingError, nowMs);
                break;
        }
    }

    static MotionDirection oppositeOf(MotionDirection direction) {
        return direction == MotionDirection::Forward ? MotionDirection::Reverse
                                                     : MotionDirection::Forward;
    }

    // The fault of record is the first one raised: a later fault does not
    // replace it, so the run log keeps the cause that actually stopped the run.
    // Nothing from before the fault is resumeable - the reversal permission, the
    // dwell and the verified stop are all discarded.
    void latchFault(FaultCode code, uint32_t nowMs) {
        if (code == FaultCode::None || phase_ == RunPhase::Fault) {
            return;
        }
        phase_ = RunPhase::Fault;
        faultCode_ = code;
        faultRaisedAtMs_ = nowMs;
        phaseEnteredMs_ = nowMs;
        handoffPending_ = false;
        finalStopVerified_ = false;
        sequencer_.reset();
        stopVerifier_.reset();
    }

    void enterPhase(RunPhase phase, uint32_t nowMs) {
        phase_ = phase;
        phaseEnteredMs_ = nowMs;
    }

    uint32_t phaseElapsedMs(uint32_t nowMs) const {
        return static_cast<uint32_t>(nowMs - phaseEnteredMs_);
    }

    // A configured bound is at least one millisecond: a positive duration that
    // rounds to zero milliseconds must expire immediately, never turn into "no
    // bound at all".
    static uint32_t boundMsFromSeconds(double seconds) {
        const uint32_t ms = msFromSeconds(seconds);
        return ms == 0U ? 1U : ms;
    }

    // The phase's configured bound in milliseconds, or 0 when the phase has none.
    // ForwardDecel and BrakeZeroSpeed deliberately return 0: the reversal
    // sequence's single timeout covers both, and a second bound would give one
    // failure two owners.
    uint32_t phaseTimeoutMs() const {
        switch (phase_) {
            case RunPhase::Approach:
                return boundMsFromSeconds(config_.approachTimeoutS.value);
            case RunPhase::Capture:
                return boundMsFromSeconds(config_.captureTimeoutS.value);
            case RunPhase::Push:
                return boundMsFromSeconds(config_.pushTimeoutS.value);
            case RunPhase::Reverse:
                return boundMsFromSeconds(config_.reverseTimeoutS.value);
            case RunPhase::FinalApproach:
                return boundMsFromSeconds(config_.finalApproachTimeoutS.value);
            case RunPhase::FinalBrake:
                return boundMsFromSeconds(config_.finalStopTimeoutS.value);
            case RunPhase::Idle:
            case RunPhase::ParameterValid:
            case RunPhase::Armed:
            case RunPhase::OptionalDelay:
            case RunPhase::ForwardDecel:
            case RunPhase::BrakeZeroSpeed:
            case RunPhase::Done:
            case RunPhase::Fault:
                break;
        }
        return 0U;
    }

    // What the phase asks for. The direction comes from `legDirection_`, so this
    // mapping cannot attach a stop to the wrong leg: the leg flips once, in the
    // handoff, and every command below follows it.
    RunMotionCommand motionCommandForPhase() const {
        const bool forwardLeg = legDirection_ == MotionDirection::Forward;
        switch (phase_) {
            case RunPhase::Approach:
            case RunPhase::Capture:
            case RunPhase::Push:
                return forwardLeg ? RunMotionCommand::DriveForward : RunMotionCommand::DriveReverse;
            case RunPhase::ForwardDecel:
                return forwardLeg ? RunMotionCommand::RampDownForward
                                  : RunMotionCommand::RampDownReverse;
            case RunPhase::Reverse:
            case RunPhase::FinalApproach:
                return forwardLeg ? RunMotionCommand::DriveForward : RunMotionCommand::DriveReverse;
            case RunPhase::BrakeZeroSpeed:
            case RunPhase::FinalBrake:
            case RunPhase::Done:
                // A deliberate hold. Done is not "no command": the event's
                // Complete Stop is the vehicle being held stationary, and no
                // further drive is ever asked for after it.
                return forwardLeg ? RunMotionCommand::BrakeForward : RunMotionCommand::BrakeReverse;
            case RunPhase::Idle:
            case RunPhase::ParameterValid:
            case RunPhase::Armed:
            case RunPhase::OptionalDelay:
            case RunPhase::Fault:
                // No motion request from the run. The vehicle layer keeps its own
                // unpowered output, and a fault response is the fault policy's
                // decision (SafetyAuthority), not this machine's.
                return RunMotionCommand::None;
        }
        return RunMotionCommand::None;
    }

    RunStatus statusFor(uint32_t nowMs) const {
        return RunStatus(phase_, RunMotionIntention(motionCommandForPhase(), nowMs), faultCode_,
                         faultRaisedAtMs_, stopVerified(), sequencer_.phase(),
                         phaseElapsedMs(nowMs));
    }

    RunPhaseConfig config_;
    ReversalSequencer sequencer_;
    NearZeroVerifier stopVerifier_;

    RunPhase phase_ = RunPhase::Idle;
    FaultCode faultCode_ = FaultCode::None;
    uint32_t faultRaisedAtMs_ = 0U;
    uint32_t phaseEnteredMs_ = 0U;

    // The direction of the leg being travelled. The run starts forward; the
    // reversal handoff flips it once.
    MotionDirection legDirection_ = MotionDirection::Forward;

    // The earned direction change has been observed and the deliberate stop is
    // being presented for one cycle before the reverse travel is asked for.
    bool handoffPending_ = false;

    // The final stop has been verified by this machine's own `stopVerifier_`
    // (latched through Done). It is not the reversal gate's verdict: that latch
    // belongs to the sequencer and is never copied here.
    bool finalStopVerified_ = false;
};

// Pin the safe zero values and the "no silent answer" contract.
static_assert(static_cast<int>(RunPhase::Idle) == 0,
              "the idle phase must stay the zero value");
static_assert(static_cast<int>(RunMotionCommand::None) == 0,
              "no motion request must stay the zero value");
static_assert(!std::is_default_constructible<RunStatus>::value,
              "a run status must never default-construct");
static_assert(!std::is_default_constructible<RunMotionIntention>::value,
              "a run intention must never default-construct");

}  // namespace ev
