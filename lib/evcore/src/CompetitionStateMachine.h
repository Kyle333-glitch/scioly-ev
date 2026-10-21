#pragma once

#include "BottleEvidence.h"
#include "Motion.h"
#include "MotionProfile.h"
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
//   on the forward progression - the travel reaching the credible capture window,
//   capture being guaranteed, the push reaching the run's forward execution target
//   - wait for that completion and are bounded by a configured timeout, exactly as
//   the stop phases wait for their verified stop. A phase that never gets its
//   completion faults instead of waiting forever or guessing.
// * A travel phase is additionally bounded by the run's *progress* once the leg
//   has reached brake entry, and not only by its clock (S18B). The target shaper
//   reaches zero inside the measured brake offset, so from brake entry the vehicle
//   is expected to coast the rest of the way to the landmark its completion needs;
//   a vehicle that comes to rest short of it would otherwise present a zero target
//   for the whole phase budget. The travel-progress bound watches the one position
//   fact the run already consumes and latches `FaultCode::TravelStalled` when that
//   fact has not advanced toward the landmark for
//   `RunPhaseConfig::travelStallTimeoutS`. It never claims the landmark was
//   reached and never commands motion: a run whose travel is going nowhere ends,
//   safely and with its own identity, rather than waiting out a bound that
//   measures the wrong thing.
// * It is blind to how a completion was produced. `RunInputs` carries three
//   completion booleans and nothing about their producer: today the vehicle layer
//   derives them from the run axis thresholded at the run's own execution-plan
//   landmarks (`BottleEvidence.h`, `RunExecutionPlan.h`), and a different producer
//   later - a sensor, a different estimator, an operator - would change nothing
//   here. No transition below may depend on which strategy is behind the fact, and
//   none may claim an observation the producer did not make: a completion is "the
//   travel has got this far", not "the bottle was detected".
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
//   state machine consumes their *results* as completion facts.
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
    // Forward travel toward the bottle. Completes when the travel enters the
    // credible capture window (the run's `approachComplete` fact) - a statement
    // about the travel, not an observation of the bottle.
    Approach = 4,
    // The capture window is open: the mechanism's capture behavior runs while the
    // travel continues, until capture is guaranteed (`captureComplete`).
    Capture = 5,
    // Driving the bottle forward until the push reaches the run's forward execution
    // target (`pushComplete`).
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

// True for the phases in which the run is asking the vehicle to travel toward the
// leg's execution target, and in which the position it acts on is expected to
// advance toward that target. These are exactly the phases the travel-progress
// bound guards; every other phase is either stationary on purpose (the delay, the
// deliberate stop, the final stop) or waiting for something else (arming, a
// configuration verdict), and a lack of progress there is not a travel fault.
inline bool runPhaseCarriesTravel(RunPhase phase) {
    switch (phase) {
        case RunPhase::Approach:
        case RunPhase::Capture:
        case RunPhase::Push:
        case RunPhase::Reverse:
        case RunPhase::FinalApproach:
            return true;
        case RunPhase::Idle:
        case RunPhase::ParameterValid:
        case RunPhase::Armed:
        case RunPhase::OptionalDelay:
        case RunPhase::ForwardDecel:
        case RunPhase::BrakeZeroSpeed:
        case RunPhase::FinalBrake:
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

// True for the commands that present a *ramp*: the commanded target is being
// brought to zero at the deceleration limit, and the run layer keeps regulating
// toward that shaped target while it falls. It exists so a ramp is not mistaken
// for a dead stop: mapping `RampDown*` straight to a non-powered request drops the
// commanded effort in one cycle, which is exactly the step the deceleration-limit
// ramp exists to avoid. The separation from `isRunStopCommand()` is deliberate -
// a ramp is still a regulated target, while a stop is a deliberate non-powered
// hold with no target at all (see `runStopEffort()`).
inline bool isRunRampDownCommand(RunMotionCommand command) {
    return command == RunMotionCommand::RampDownForward ||
           command == RunMotionCommand::RampDownReverse;
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

// The leg a run command names: a travel command names the leg it travels, and a
// stop command names the leg the stop belongs to (`RampDownForward` and
// `BrakeForward` both name the forward leg, and so on). `None` names no leg at
// all - the caller keeps the leg it had - which is why this returns false for it
// and why a caller must not apply the answer for `None`.
//
// This is the leg-tag interpretation and nothing more. It is never a powered
// direction: a stop has none, and a caller that turned this into a
// `PoweredDirection` would be deriving motion from a tag
// (SOFTWARE_ARCHITECTURE.md section 14). The run's own leg is the leg its current
// command names, so this is the one mapping every consumer uses.
inline bool runCommandNamesForwardLeg(RunMotionCommand command) {
    switch (command) {
        case RunMotionCommand::DriveForward:
        case RunMotionCommand::RampDownForward:
        case RunMotionCommand::BrakeForward:
            return true;
        case RunMotionCommand::DriveReverse:
        case RunMotionCommand::RampDownReverse:
        case RunMotionCommand::BrakeReverse:
            return false;
        case RunMotionCommand::None:
            break;
    }
    return false;
}

// The non-powered request a run stop command maps to, so every consumer maps the
// stop vocabulary the same way:
//
//   RampDown*  -> Disabled: the profile's own target is what ramps the travel to
//                 zero, and this request asks for no powered effort at all
//   Brake*     -> Brake: the deliberate non-powered hold
//
// The leg tag is dropped here, exactly as it must be: a stop has no powered
// direction, and neither mode above carries one. A command that is not a stop
// command maps to `Disabled` with zero effort - fail closed rather than inventing
// a stop - so a caller bug cannot turn a travel command into a second, competing
// request.
inline MotorEffort runStopEffort(RunMotionCommand command) {
    switch (command) {
        case RunMotionCommand::RampDownForward:
        case RunMotionCommand::RampDownReverse:
            return MotorEffort(MotorMode::Disabled, 0U);
        case RunMotionCommand::BrakeForward:
        case RunMotionCommand::BrakeReverse:
            return MotorEffort(MotorMode::Brake, 0U);
        case RunMotionCommand::None:
        case RunMotionCommand::DriveForward:
        case RunMotionCommand::DriveReverse:
            break;
    }
    return MotorEffort(MotorMode::Disabled, 0U);
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
    constexpr RunPhaseConfig(Sourced<double> maximumOptionalDelayS_,
                             Sourced<double> approachTimeoutS_, Sourced<double> captureTimeoutS_,
                             Sourced<double> pushTimeoutS_, Sourced<double> reverseTimeoutS_,
                             Sourced<double> finalApproachTimeoutS_,
                             Sourced<double> finalStopTimeoutS_,
                             Sourced<double> travelStallTimeoutS_,
                             Sourced<double> encoderProgressTimeoutS_,
                             Sourced<double> finalApproachDistanceM_,
                             NearZeroConfig stopVerification_,
                             Sourced<double> minimumPlannedCruiseMps_,
                             Sourced<double> approachCeilingMps_,
                             Sourced<double> captureCeilingMps_,
                             Sourced<double> pushCeilingMps_,
                             BottleEvidenceConfig bottleEvidence_)
        : maximumOptionalDelayS(maximumOptionalDelayS_),
          approachTimeoutS(approachTimeoutS_),
          captureTimeoutS(captureTimeoutS_),
          pushTimeoutS(pushTimeoutS_),
          reverseTimeoutS(reverseTimeoutS_),
          finalApproachTimeoutS(finalApproachTimeoutS_),
          finalStopTimeoutS(finalStopTimeoutS_),
          travelStallTimeoutS(travelStallTimeoutS_),
          encoderProgressTimeoutS(encoderProgressTimeoutS_),
          finalApproachDistanceM(finalApproachDistanceM_),
          stopVerification(stopVerification_),
          minimumPlannedCruiseMps(minimumPlannedCruiseMps_),
          approachCeilingMps(approachCeilingMps_),
          captureCeilingMps(captureCeilingMps_),
          pushCeilingMps(pushCeilingMps_),
          bottleEvidence(bottleEvidence_) {}

    // The *most* deliberate pre-travel delay a run may spend, seconds. This is a
    // legal/safety maximum, never the run's delay: the delay a run actually
    // spends is its own immutable execution plan's `initialDelayS`
    // (`RunExecutionPlan.h`), handed to `announceRun()` at the announcement
    // boundary, and this value only bounds what that plan may state, so no
    // announcement can turn into an unbounded wait. 0 means no delay is
    // permitted at all and the OptionalDelay phase is always skipped.
    //
    // The profile's `MotionTimingIntent::intentionalDelayS` must be fed from the
    // same per-run value (`initialDelayS()`), because a margin computed against a
    // delay the run did not spend is a number about a different run.
    Sourced<double> maximumOptionalDelayS;

    // Bounds, seconds. A phase that never receives its exit evidence ends at
    // Fault(PhaseTimeout) instead of waiting forever, and a bound is a deadline
    // rather than only a wait limit: exit evidence that arrives after it faults
    // the run instead of being accepted late, because a phase that overran its
    // bound is no longer the run that was announced. `ForwardDecel` and
    // `BrakeZeroSpeed` deliberately have no bound here: the reversal sequence
    // carries its own single timeout, which covers both.
    Sourced<double> approachTimeoutS;        // -> Capture, on approach complete
    Sourced<double> captureTimeoutS;         // -> Push, on capture complete
    Sourced<double> pushTimeoutS;            // -> ForwardDecel, on push complete
    Sourced<double> reverseTimeoutS;         // -> FinalApproach, on the closing distance
    Sourced<double> finalApproachTimeoutS;   // -> FinalBrake, on reaching the Target Point
    Sourced<double> finalStopTimeoutS;       // -> Done, on the stop being verified

    // How long a travel phase (Approach, Capture, Push, Reverse, FinalApproach)
    // may go without the position it acts on advancing toward the leg's required
    // execution target before the run latches `FaultCode::TravelStalled`, seconds.
    //
    // It is the bounded terminal outcome for the taper/landmark contract: once a
    // leg is inside its measured brake offset the target shaper has reached zero,
    // so the vehicle is expected to coast the rest of the way to the landmark; a
    // vehicle that comes to rest short of it would otherwise present a zero target
    // for the whole phase budget and only leave at `PhaseTimeout`. This bound
    // names that specific fact instead of waiting out the phase. It is not a
    // second phase timeout: it applies only from brake entry in a travel phase,
    // against the position evidence, and the phase's own bound (and the
    // run-scoped duration bound) remain the final fail-safe. It gates nothing it
    // cannot observe - an unusable or non-finite position pauses the check rather
    // than faulting - so a placeholder here can only add a bound the run cannot
    // satisfy, which is the fail-closed direction; it must be `Measured` before
    // any run may be armed.
    Sourced<double> travelStallTimeoutS;

    // How long accepted powered travel may go without the signed tick count
    // advancing in the expected direction before the run latches
    // `FaultCode::EncoderFeedbackLost`, seconds.
    //
    // It is the bounded response to lost encoder feedback during powered
    // travel: frozen counts, wrong-sign counts, or incoherent counts under an
    // authorized drive. The startup allowance is the same bound, baselined at
    // the first accepted powered authorization rather than at announce/arm;
    // ongoing travel re-baselines on every coherent tick advance. Cycles
    // without accepted powered travel (Disabled/brake/dwell/stop) neither
    // consume nor renew the allowance, so toggling cannot evade it and only
    // genuine progress resets it. A placeholder here can only add a bound the
    // run cannot satisfy, which is the fail-closed direction; it must be
    // `Measured` before any run may be armed. The real hardware value is
    // unmeasured: slowest reliable edge spacing, startup latency, both signs,
    // and loaded scheduling gaps (P-H5/H7/H9).
    Sourced<double> encoderProgressTimeoutS;

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

    // The slowest cruise at which travel is known to be reliable, metres per
    // second. The diagnostic timing model (`planRunTiming()`) does not go below
    // it: a speed the drivetrain cannot follow is not a way to spend time, and
    // the model spends the remaining slack as a deliberate delay instead. A run
    // may not be announced on a placeholder here, because a validated timing
    // model would either crawl or wait on a number nobody measured.
    Sourced<double> minimumPlannedCruiseMps;

    // The strategy velocity ceilings the run may travel the three bottle phases
    // at, metres per second: Approach, Capture, Push. Competition strategy uses
    // them to slow those phases without touching the cruise ceiling, the safety
    // velocity ceiling or any rate limit - they enter the target shaper's desired
    // magnitude before the acceleration/deceleration limits, so they can only
    // tighten a shaped target, never raise one, and a phase change is a ramped
    // transition rather than a target step (`MotionProfile.h`).
    //
    // `Measured`-only like everything else here, and placeholders today: no
    // per-phase speed has been established on this vehicle, so the run policy
    // stays refused and the ceilings are unusable until the bottle-integration
    // protocol establishes them (P-H8). A phase ceiling that is not a positive
    // speed is refused, because a phase carries travel and a zero ceiling would
    // be a standstill the run cannot complete.
    Sourced<double> approachCeilingMps;
    Sourced<double> captureCeilingMps;
    Sourced<double> pushCeilingMps;

    // The bottle-interaction distances the run's execution plan derives its forward
    // progression thresholds from (`BottleEvidence.h` distances,
    // `RunExecutionPlan.h` thresholds). The forward phases cannot exit without those
    // thresholds, so the distances are validated with the rest of the run policy's
    // configuration: a run may not be announced while any of its distances is a
    // placeholder, and none of them may be assumed from the mechanism's intended
    // geometry.
    BottleEvidenceConfig bottleEvidence;
};

enum class RunPhaseConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,             // at least one value is not ValueSource::Measured
    NonFiniteValue,                   // NaN or infinity
    NegativeDelay,                    // maximumOptionalDelayS below zero
    NonPositiveTimeout,               // a phase bound that is not > 0
    NonPositiveTravelStall,           // the travel-progress bound is not > 0
    NonPositiveEncoderProgress,       // the feedback-progress bound is not > 0
    NonPositiveFinalApproachDistanceM,  // the final approach distance is not > 0
    StopVerificationNotUsable,        // the NearZeroConfig failed its own validation
    NonPositiveMinimumCruise,         // the planned-cruise floor is not > 0
    NonPositiveStrategyCeiling,       // a bottle-phase strategy ceiling is not > 0
    BottleEvidenceNotUsable,          // the BottleEvidenceConfig failed its own validation
    FinalStopTimeoutWithoutDwellHeadroom,  // finalStopTimeoutS cannot span its dwell (R2/M3)
};

struct RunPhaseConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    RunPhaseConfigError error;
};

// True when the maximum optional delay may bound a run: it was measured on
// this vehicle, or the official rules fix it and the citing rule is recorded
// (`Sourced::citesEvidence()`). Mirrors `dwellProvenanceIsEstablished()` in
// `NearZeroVerifier.h`: the event's maximum Target Time (rule 8.i) is a rule
// value, never a measurement, and an uncited rule value is refused exactly like
// a placeholder.
inline bool optionalDelayProvenanceIsEstablished(const Sourced<double>& delay) {
    if (delay.isMeasured()) {
        return true;
    }
    return delay.source == ValueSource::RuleDerived && delay.citesEvidence();
}

// Single entry point for the run policy's validation. The stop-verification
// configuration is validated by `validateNearZeroConfig()` rather than
// re-checked here, so the threshold/dwell policy keeps one definition.
inline RunPhaseConfigValidation validateRunPhaseConfig(const RunPhaseConfig& config) {
    if (!optionalDelayProvenanceIsEstablished(config.maximumOptionalDelayS) || !config.approachTimeoutS.isMeasured() ||
        !config.captureTimeoutS.isMeasured() || !config.pushTimeoutS.isMeasured() ||
        !config.reverseTimeoutS.isMeasured() || !config.finalApproachTimeoutS.isMeasured() ||
        !config.finalStopTimeoutS.isMeasured() || !config.travelStallTimeoutS.isMeasured() ||
        !config.encoderProgressTimeoutS.isMeasured() ||
        !config.finalApproachDistanceM.isMeasured() ||
        !config.minimumPlannedCruiseMps.isMeasured() ||
        !config.approachCeilingMps.isMeasured() || !config.captureCeilingMps.isMeasured() ||
        !config.pushCeilingMps.isMeasured()) {
        return {false, RunPhaseConfigError::ValuesAreNotMeasured};
    }
    if (!std::isfinite(config.maximumOptionalDelayS.value) ||
        !std::isfinite(config.approachTimeoutS.value) ||
        !std::isfinite(config.captureTimeoutS.value) || !std::isfinite(config.pushTimeoutS.value) ||
        !std::isfinite(config.reverseTimeoutS.value) ||
        !std::isfinite(config.finalApproachTimeoutS.value) ||
        !std::isfinite(config.finalStopTimeoutS.value) ||
        !std::isfinite(config.travelStallTimeoutS.value) ||
        !std::isfinite(config.encoderProgressTimeoutS.value) ||
        !std::isfinite(config.finalApproachDistanceM.value) ||
        !std::isfinite(config.minimumPlannedCruiseMps.value) ||
        !std::isfinite(config.approachCeilingMps.value) ||
        !std::isfinite(config.captureCeilingMps.value) ||
        !std::isfinite(config.pushCeilingMps.value)) {
        return {false, RunPhaseConfigError::NonFiniteValue};
    }
    if (config.maximumOptionalDelayS.value < 0.0) {
        return {false, RunPhaseConfigError::NegativeDelay};
    }
    // A cruise floor that is not positive would let the plan divide by it or plan
    // a standstill; the floor exists so the plan never crawls.
    if (!(config.minimumPlannedCruiseMps.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveMinimumCruise};
    }
    // A phase ceiling must be a positive speed: the phases it belongs to carry
    // travel, and a zero (or negative) ceiling would configure a standstill the
    // run cannot complete. The shaper itself accepts an active zero as a legal
    // request for no shaped magnitude; the run policy is stricter on purpose.
    if (!(config.approachCeilingMps.value > 0.0) || !(config.captureCeilingMps.value > 0.0) ||
        !(config.pushCeilingMps.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveStrategyCeiling};
    }
    if (!validateBottleEvidenceConfig(config.bottleEvidence).ok) {
        return {false, RunPhaseConfigError::BottleEvidenceNotUsable};
    }
    if (!(config.approachTimeoutS.value > 0.0) || !(config.captureTimeoutS.value > 0.0) ||
        !(config.pushTimeoutS.value > 0.0) || !(config.reverseTimeoutS.value > 0.0) ||
        !(config.finalApproachTimeoutS.value > 0.0) || !(config.finalStopTimeoutS.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveTimeout};
    }
    if (!(config.travelStallTimeoutS.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveTravelStall};
    }
    if (!(config.encoderProgressTimeoutS.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveEncoderProgress};
    }
    if (!(config.finalApproachDistanceM.value > 0.0)) {
        return {false, RunPhaseConfigError::NonPositiveFinalApproachDistanceM};
    }
    if (!validateNearZeroConfig(config.stopVerification).ok) {
        return {false, RunPhaseConfigError::StopVerificationNotUsable};
    }
    // R2/M3: the FinalBrake phase verifies a continuous near-zero dwell and
    // otherwise ends in its phase timeout (`phaseTimeoutMs()` vs the
    // verifier's `dwellS`). A final-stop timeout at or below the dwell it has
    // to verify - compared in seconds, before any millisecond truncation - can
    // never complete verification before the bound expires: the outcome would
    // still be safe (a PhaseTimeout fault) but the configuration is
    // impossible, so it is refused here rather than armed. Malformed values
    // keep their fundamental verdicts above; this constrains only timeouts
    // that are otherwise usable. The one-tick cadence margin above equality
    // is operational guidance (the verification needs a second observation
    // after the dwell elapses), not a second gate: strict inequality is the
    // complete, measurement-free rule.
    if (!(config.finalStopTimeoutS.value > config.stopVerification.dwellS.value)) {
        return {false, RunPhaseConfigError::FinalStopTimeoutWithoutDwellHeadroom};
    }
    return {true, RunPhaseConfigError::None};
}

// The strategy velocity ceiling the run policy applies to one phase.
//
// Competition strategy is allowed to slow the three bottle phases - Approach,
// Capture and Push - and this is the one mapping from the phase the run itself
// named onto the ceiling its configuration carries for that phase. It decides
// nothing: the state machine never reads the ceilings, the vehicle layer hands
// the result to `MotionProfile`, and every other phase (the delay, the ramps,
// the brake, the reverse leg) is explicitly inactive - so a stop, a reversal or
// the final approach is never shaped by a bottle-phase ceiling, and no ceiling
// can reach the one-way reversal street.
//
// The ceilings are `Sourced` and validated with the rest of the run policy: while
// any of them is not `Measured`, `validateRunPhaseConfig()` refuses and no run can
// be announced or armed, so a placeholder phase ceiling is unusable rather than
// quietly ignored.
inline StrategyVelocityCeiling strategyVelocityCeilingForPhase(const RunPhaseConfig& config,
                                                               RunPhase phase) {
    switch (phase) {
        case RunPhase::Approach:
            return activeStrategyVelocityCeiling(config.approachCeilingMps.value);
        case RunPhase::Capture:
            return activeStrategyVelocityCeiling(config.captureCeilingMps.value);
        case RunPhase::Push:
            return activeStrategyVelocityCeiling(config.pushCeilingMps.value);
        case RunPhase::Idle:
        case RunPhase::ParameterValid:
        case RunPhase::Armed:
        case RunPhase::OptionalDelay:
        case RunPhase::ForwardDecel:
        case RunPhase::BrakeZeroSpeed:
        case RunPhase::Reverse:
        case RunPhase::FinalApproach:
        case RunPhase::FinalBrake:
        case RunPhase::Done:
        case RunPhase::Fault:
            return inactiveStrategyVelocityCeiling();
    }
    return inactiveStrategyVelocityCeiling();
}

// The deliberate pre-travel delay a *specific* run spends, in milliseconds, or 0
// when it spends none. Owned by that run's immutable execution plan
// (`RunExecutionPlan::initialDelayS`, consumed by `announceRun()` below); this
// is the one conversion from its seconds to the clock reading the OptionalDelay
// phase compares against.
inline uint32_t initialDelayMs(double initialDelayS) {
    return msFromSeconds(initialDelayS);
}

// Everything the vehicle layer observed this cycle. Every field defaults to the
// unusable value, so a default-built packet starts nothing and confirms nothing.
struct RunInputs {
    // The safety authority reports armed (`SafetyAuthority::isArmed()`). The run
    // never arms itself and this fact is what moves it out of ParameterValid.
    bool authorityArmed = false;

    // The go signal. This machine reads it as a level: an asserted value on the
    // update that reaches Armed starts the run. The **production producer** is
    // `ev::StartTriggerDetector`, which supplies a single debounced activation
    // edge per intentional press rather than a raw level, so a trigger already
    // held when the board booted starts nothing and a held pencil cannot start a
    // second run. The level reading here is what makes the machine reusable by a
    // scripted scenario; it is not a claim about what the vehicle layer feeds it.
    bool startTriggered = false;

    // Forward-progression completion, one fact per forward phase. Each says "the
    // travel has reached this run's threshold for that phase" and nothing about how
    // the answer was reached: the producer is the vehicle layer's own strategy
    // (today the run axis thresholded at the execution plan's landmarks), and this
    // machine consumes the fact without knowing or caring which strategy produced
    // it. None of them is an observation of the bottle.
    //
    // No production producer may leave one of these permanently false: the vehicle
    // layer derives all three from the run it announced, so they become available
    // exactly when the run's execution plan does. A phase whose completion can never
    // arrive is a wiring fault, not a policy, and each phase's configured bound
    // turns it into a `PhaseTimeout` instead of a silent stall.
    bool approachComplete = false;
    bool captureComplete = false;
    bool pushComplete = false;

    // Remaining distance along the run axis to the Target Point, metres, with
    // its own usability. The vehicle layer owns the reconciliation between the
    // run axis and the encoder's tick convention (Odometry.h,
    // MotionProfileInputs::remainingDistanceM); this machine only compares the
    // reconciled number against its configured final-approach distance.
    //
    // It is also the one position fact the run's travel-progress bound consumes.
    // The Target Point is fixed and, on the forward leg, sits before the forward
    // execution target and on the reverse leg *is* the execution target, so this
    // distance decreases strictly as the vehicle travels along either leg and a
    // run that reaches its landmark necessarily reduces it. The bound watches
    // that decrease and nothing else; it never re-derives a landmark or a speed
    // from it, and an unusable or non-finite value pauses the bound rather than
    // faulting on evidence the run does not have.
    bool remainingDistanceUsable = false;
    double remainingDistanceM = 0.0;

    // Whether the leg the run is travelling is within that leg's measured brake
    // offset: the distance from the current position to the leg's execution target
    // is at or inside the distance the vehicle is measured to travel after its
    // command reaches zero (`MotionProfileLimits::brakeOffsetM`, the same value
    // the shaping taper uses). This is the *commanded* travel reaching its
    // configured end - the point from which the run expects the vehicle to coast
    // the rest of the way to the landmark - and it is a travel fact, not a claim
    // that the landmark was reached.
    //
    // The vehicle layer owns it because only the vehicle layer holds both the
    // reconciled leg remaining distance and the direction's brake offset; the run
    // policy must not hold a second copy of that measured distance. False means
    // either the leg is still outside the offset or there is no usable leg target,
    // so the travel-progress bound does not apply.
    bool legInBrakeEntry = false;

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
    // The run's delay - its execution plan's `initialDelayS` - is negative, not
    // a number, or larger than the configured maximum. A run whose plan would
    // wait longer than the policy permits may not be sequenced, so nothing is
    // announced.
    InitialDelayNotUsable,
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

// The run's own recorded timeline: the clock readings that turn the event's Run
// Time (rule 8.o, actuation to the complete stop) and Complete Stop (rule 8.p,
// the stationary interval that is *not* part of Run Time) into numbers a log can
// carry.
//
// Every reading is a raw `millis()` reading taken from the clock reading the tick
// was given; a duration is an unsigned difference of two of them, which is the
// arithmetic the rest of the firmware uses and which stays correct across the
// `uint32` wrap. What each one is, exactly:
//
// * `actuatedAtMs` - the reading at which the start trigger actuated the run (the
//   cycle the run left `Armed`). The run's deliberate pre-travel delay is inside
//   the run, so the delay is counted, exactly as the event counts it. It is
//   stamped once per announced run and kept for the life of the run: a fault does
//   not erase the fact that the run was actuated.
// * `finalStopDwellStartMs` - the start of the final stop's verification
//   interval. While `FinalBrake` holds an open interval this is that candidate's
//   start, and a break in the near-zero evidence discards or restarts it (the
//   owned `NearZeroVerifier` owns the interval; this is its start, mirrored).
//   Once the interval has satisfied the rule it is the start of the interval that
//   did: only then does it mean "verified final stop start", and until
//   `finalStopVerified` is true it is a candidate that may still be discarded.
// * `finalStopVerifiedAtMs` - the reading at which the final stop's verification
//   completed: the cycle that entered `Done`, when the rule's Complete Stop
//   interval was satisfied. `finalStopVerified` is this record's own latch: it is
//   set by that verification and cleared only by `reset()`.
//
// The record is history, and is deliberately not the same fact as
// `RunStatus::stopVerified`: that verdict says which stop the run is presenting
// *right now* (and is false in `Fault`), while this record says what the run
// actually did. A fault therefore discards an in-progress dwell - a candidate
// that was never verified is not a fact - but it does not erase a final stop that
// was already verified: the run did complete its Complete Stop, and a log may not
// be made to forget that because a later fault ended the run. Only `reset()`
// clears the record, and the record keeps the actuation reading even through a
// fault.
//
// Nothing here changes what the run does: the readings are taken from facts the
// phases already consume, and they command nothing.
struct RunTimeline {
    // The run has been actuated (the start trigger was seen) and `actuatedAtMs`
    // is valid. False before the trigger and after a `reset()`.
    bool actuated = false;
    uint32_t actuatedAtMs = 0U;

    // A final-stop dwell start is known and `finalStopDwellStartMs` is valid:
    // either a candidate interval is open, or the interval that satisfied the
    // rule is latched. False with no run, in every phase before `FinalBrake`,
    // after a discarded candidate, and after a fault.
    bool dwellStartKnown = false;
    uint32_t finalStopDwellStartMs = 0U;

    // The rule's Complete Stop verification completed and `finalStopVerifiedAtMs`
    // is valid.
    bool finalStopVerified = false;
    uint32_t finalStopVerifiedAtMs = 0U;
};

// True while the run's own clock is *running*: it has been actuated, it has not
// finished (`Done`), and it has not faulted. This is the one definition of the
// interval a run-scoped bound applies to, and the run layer is handed it rather
// than re-deriving "is this run still going" for itself.
//
// The deliberate pre-travel delay is inside the interval, and deliberately so:
// the run's own time begins at actuation (rule 8.o, and the same convention
// `RunTimeline` records for the internal Run Time), so a delay cannot be used to
// escape a run-duration bound. `Done` and `Fault` end it: a completed run has no
// motion left to bound, and a faulted run is already stopped by the authority's
// own fault response, so a second bound firing there could only add noise.
//
// This reads no clock and holds no state - the run's own recorded facts are the
// input - so a run boundary (`reset()`) ends the interval and a new actuation
// begins a fresh one by construction.
inline bool runDurationClockActive(const RunTimeline& timeline, RunPhase phase,
                                   FaultCode fault) {
    return timeline.actuated && fault == FaultCode::None && phase != RunPhase::Done;
}

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

    // The deliberate pre-travel delay this specific run was announced with,
    // seconds. Set by `announceRun()` from the run's immutable execution plan
    // (`RunExecutionPlan::initialDelayS`, the one owner of the per-run delay) and
    // bounded by `config().maximumOptionalDelayS`, which is only the
    // legal/safety maximum; 0 when the run spends none, which is what an
    // unvalidated timing model produces by construction.
    // This is the value the profile's `MotionTimingIntent::intentionalDelayS`
    // must be fed with, so a reported margin is about the run that is actually
    // being sequenced.
    double initialDelayS() const { return initialDelayS_; }

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

    // The run's recorded timeline (see `RunTimeline`): when the run was
    // actuated, the start of the final stop's dwell interval (candidate, or the
    // one that satisfied the rule) and when its verification completed. Read-only,
    // and cleared by `reset()` - a new-run boundary therefore cannot inherit a
    // previous run's readings. The telemetry sample and any operator view report
    // the run's timing from here, so there is one owner of "when did this run
    // start and when did its final stop verify".
    RunTimeline timeline() const {
        RunTimeline recorded;
        recorded.actuated = runActuated_;
        recorded.actuatedAtMs = runActuatedAtMs_;
        recorded.dwellStartKnown = finalStopDwellStartKnown_;
        recorded.finalStopDwellStartMs = finalStopDwellStartMs_;
        recorded.finalStopVerified = finalStopVerifiedAt_;
        recorded.finalStopVerifiedAtMs = finalStopVerifiedAtMs_;
        return recorded;
    }

    // Returns the machine to its post-construction state: Idle (no run accepted,
    // so a new one must be announced), no fault of record, no reversal sequence,
    // no verified stop, no pending handoff, no run timeline. It never arms and
    // never commands motion; re-arming and re-announcing stay separate,
    // deliberate decisions. This is the only way out of Fault and Done.
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
        initialDelayS_ = 0.0;
        // No run exists after a reset, so no reading of a previous run survives
        // it: a new announcement starts from an empty timeline.
        runActuated_ = false;
        runActuatedAtMs_ = 0U;
        finalStopDwellStartKnown_ = false;
        finalStopDwellStartMs_ = 0U;
        finalStopVerifiedAt_ = false;
        finalStopVerifiedAtMs_ = 0U;
        travelProgressTracking_ = false;
        travelProgressBestRemainingM_ = 0.0;
        travelProgressImprovedAtMs_ = 0U;
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
    //
    // `initialDelayS` is the run's own deliberate pre-travel delay, taken from
    // that run's immutable execution plan (`RunExecutionPlan::initialDelayS`) -
    // the one owner of the per-run delay. The phase policy contributes only the
    // `maximumOptionalDelayS` bound checked below; it never decides the value.
    RunStartStatus announceRun(const RunConfig& config, const RunConfigValidation& validation,
                               double initialDelayS, uint32_t nowMs) {
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
        // The run's own deliberate delay, bounded by the policy maximum. Refused
        // without latching anything: this is a caller's plan that does not fit
        // the policy, not a run whose sequencing failed.
        if (!std::isfinite(initialDelayS) || initialDelayS < 0.0 ||
            initialDelayS > config_.maximumOptionalDelayS.value) {
            return RunStartStatus::InitialDelayNotUsable;
        }

        initialDelayS_ = initialDelayS;
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
            // The travel-progress bound (S18B): a travel phase that stops
            // advancing toward the landmark its completion needs ends with its own
            // identity rather than presenting a zero target until the phase bound
            // expires. It is evaluated after the phase bound, so the phase
            // deadline keeps precedence when both are due on the same cycle; it
            // never observes a fault that is already of record.
            checkTravelProgress(nowMs, inputs);
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
                    // The run's actuation, stamped once: this is the cycle the
                    // trigger asked the run to begin, whether or not the run has a
                    // deliberate pre-travel delay to spend first. The delay is
                    // inside the run's own time (rule 8.o counts from actuation),
                    // so it is not subtracted anywhere.
                    if (!runActuated_) {
                        runActuated_ = true;
                        runActuatedAtMs_ = nowMs;
                    }
                    enterPhase(initialDelayS_ > 0.0 ? RunPhase::OptionalDelay : RunPhase::Approach,
                               nowMs);
                }
                break;

            case RunPhase::OptionalDelay:
                if (phaseElapsedMs(nowMs) >= initialDelayMs(initialDelayS_)) {
                    enterPhase(RunPhase::Approach, nowMs);
                }
                break;

            case RunPhase::Approach:
                if (inputs.approachComplete) {
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
                // The dwell's start is the verifier's own interval start, read
                // from the verifier rather than reconstructed from the elapsed
                // span: an interval a broken observation discarded is simply gone
                // here, and one the gap rule restarted reports its new start. The
                // reading is only latched as the run's verified final-stop start
                // by the verification below.
                finalStopDwellStartKnown_ = stopVerifier_.intervalOpen();
                finalStopDwellStartMs_ = finalStopDwellStartKnown_
                                             ? stopVerifier_.intervalStartedAtMs()
                                             : 0U;
                if (verdict.verified) {
                    // The interval that just satisfied the rule is the verified
                    // final-stop start, and this cycle is the verification
                    // completion: the rule's Complete Stop is over, so Run Time
                    // ends here and not at `Done`'s later ticks. The timeline's
                    // latch is separate from the presentation latch above, so a
                    // later fault cannot erase what this run completed.
                    finalStopVerified_ = true;
                    finalStopVerifiedAt_ = true;
                    finalStopVerifiedAtMs_ = nowMs;
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
            // The handoff consumes only a CURRENT permission: the sequencer was
            // just re-stepped on this tick's evidence above, so anything but a
            // still-ready verdict means the evidence moved on while the stop
            // was being presented. Abort the handoff without flipping and stay
            // in BrakeZeroSpeed: the sequence timeout still bounds the wait,
            // and the stop can be re-earned (S18A/P1).
            handoffPending_ = false;
            if (reversal.phase != ReversalPhase::DirectionChangeReady) {
                return;
            }
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
    // dwell and the presentation of a verified stop are all discarded.
    //
    // The run's recorded timeline is handled differently on purpose (see
    // `RunTimeline`): an in-progress dwell candidate is discarded - it was never
    // verified, so there is no fact to keep - while an already-verified final stop
    // and the actuation reading stay. The run did those things; a fault after them
    // does not make the record untrue, and the log must still be able to say where
    // the run's own Run Time ended.
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
        if (!finalStopVerifiedAt_) {
            finalStopDwellStartKnown_ = false;
            finalStopDwellStartMs_ = 0U;
        }
        sequencer_.reset();
        stopVerifier_.reset();
    }

    void enterPhase(RunPhase phase, uint32_t nowMs) {
        phase_ = phase;
        phaseEnteredMs_ = nowMs;
        // A new phase re-baselines the travel-progress bound: the landmark it is
        // advancing toward belongs to the new phase, and the bound measures from
        // where the vehicle actually was when that phase began rather than
        // inheriting the previous phase's clock.
        travelProgressTracking_ = false;
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

    // The travel-progress bound (S18B). It applies only once the leg the run is
    // travelling has reached its brake entry - the commanded travel has reached
    // its configured end and the run now expects the vehicle to coast the rest of
    // the way to the landmark - because that is exactly where the taper/landmark
    // contract leaves the gap: the shaper commands zero inside the measured brake
    // offset, and only *after* that can a vehicle that has not coasted sit short
    // of the landmark with a zero target. Before brake entry the run is still
    // commanding travel and the phase's own bound is the right outcome; outside a
    // travel phase the run is stationary on purpose.
    //
    // While it applies, it watches the one position fact the run policy consumes
    // and faults with `TravelStalled` when that fact has not advanced toward the
    // leg's landmark for `config_.travelStallTimeoutS`. See
    // `RunPhaseConfig::travelStallTimeoutS` for why this is a distinct identity
    // from `PhaseTimeout`.
    //
    // The evidence is treated conservatively, which is the whole point of a
    // bounded *safety* outcome rather than a success claim:
    //
    //   * outside brake entry, or in a non-travel phase, the bound is inactive and
    //     its baseline is cleared, so entering brake entry always starts a fresh
    //     measurement window;
    //   * with no usable, finite position this cycle there is no progress fact at
    //     all, so the stall clock is paused (not reset to zero and not counted
    //     against the run) and no fault is claimed - the phase's own bound and the
    //     run-scoped duration bound remain the terminal outcome for that case;
    //   * progress is a strict decrease of the remaining distance below the best
    //     (smallest) value seen since brake entry, so any genuine coast toward the
    //     landmark counts and only a complete absence of it can fault;
    //   * the arithmetic is unsigned subtraction, so it stays correct across the
    //     `millis()` wrap exactly like every other bound here.
    //
    // It never claims the landmark was reached, never commands motion, and never
    // reads speed: it is a fault, and its response is the authority's existing
    // fault stop.
    void checkTravelProgress(uint32_t nowMs, const RunInputs& inputs) {
        if (!runPhaseCarriesTravel(phase_) || !inputs.legInBrakeEntry) {
            travelProgressTracking_ = false;
            return;
        }
        if (!inputs.remainingDistanceUsable || !std::isfinite(inputs.remainingDistanceM)) {
            // Uncertain evidence pauses the bound rather than faulting on it.
            if (travelProgressTracking_) {
                travelProgressImprovedAtMs_ = nowMs;
            }
            return;
        }
        if (!travelProgressTracking_) {
            travelProgressTracking_ = true;
            travelProgressBestRemainingM_ = inputs.remainingDistanceM;
            travelProgressImprovedAtMs_ = nowMs;
            return;
        }
        if (inputs.remainingDistanceM < travelProgressBestRemainingM_) {
            travelProgressBestRemainingM_ = inputs.remainingDistanceM;
            travelProgressImprovedAtMs_ = nowMs;
            return;
        }
        const uint32_t boundMs = boundMsFromSeconds(config_.travelStallTimeoutS.value);
        if (static_cast<uint32_t>(nowMs - travelProgressImprovedAtMs_) >= boundMs) {
            latchFault(FaultCode::TravelStalled, nowMs);
        }
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

    // The deliberate pre-travel delay this run was announced with, seconds: the
    // value consumed from the run's execution plan's `initialDelayS` (the plan is
    // the owner; this is the run's spent copy) and bounded by
    // `config_.maximumOptionalDelayS`. It is a run parameter, not geometry: no
    // phase transition derives geometry from it, and inside this machine only
    // `initialDelayS()` reads it.
    double initialDelayS_ = 0.0;

    // The travel-progress bound's state (S18B): whether the current travel phase
    // has a baseline yet, the smallest remaining distance seen since it began, and
    // the clock reading of the last improvement. Reset on every phase entry and by
    // `reset()`.
    bool travelProgressTracking_ = false;
    double travelProgressBestRemainingM_ = 0.0;
    uint32_t travelProgressImprovedAtMs_ = 0U;

    // The final stop has been verified by this machine's own `stopVerifier_`
    // (latched through Done). It is not the reversal gate's verdict: that latch
    // belongs to the sequencer and is never copied here.
    bool finalStopVerified_ = false;

    // The run's recorded timeline (see `RunTimeline`), each reading stamped from
    // the tick's own clock reading. Only `reset()` clears the record: a fault
    // discards an in-progress dwell candidate (nothing was verified) but keeps the
    // actuation reading and, once it exists, the verified final stop.
    bool runActuated_ = false;
    uint32_t runActuatedAtMs_ = 0U;
    bool finalStopDwellStartKnown_ = false;
    uint32_t finalStopDwellStartMs_ = 0U;
    // The timeline's own verification latch, separate from `finalStopVerified_`
    // above (which is the presentation's): this one is the run's record.
    bool finalStopVerifiedAt_ = false;
    uint32_t finalStopVerifiedAtMs_ = 0U;
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
