#pragma once

#include "Units.h"

#include <cmath>
#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The forward progression evidence - where the run's forward phases get their exits
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One mapping from a position on the run axis to the three **progression
//   completion** facts the run policy waits for on its forward leg:
//
//       the forward travel has reached the earliest position at which a touch
//       could credibly be reported (the capture window opens) -> approachComplete
//       ... and the configured capture advance past nominal contact -> captureComplete
//       ... and the run's own forward execution target            -> pushComplete
//
//   Read each one for what it says: the *travel* has got this far. Not "a sensor
//   saw the bottle", and not "the bottle did something". The run's word for the
//   event is that a phase is *complete*, and that is exactly what these booleans
//   are named after.
//
// * The three threshold positions are **arguments**, not arithmetic done here. The
//   run's execution plan (`RunExecutionPlan.h`) owns them: it derived them once,
//   at the announcement boundary, from the validated rule geometry and the measured
//   interaction calibration. This module applies them to a position. There is
//   deliberately no path here that derives a threshold from the rule geometry,
//   because a second derivation of one threshold is how a phase exit and the travel
//   taper start to disagree about where an event is.
//
// * Deterministic, monotone in the vehicle's own position, and pure: no clock,
//   no pin, no interrupt, no allocation. It takes the run-axis position with its
//   own usability verdict, the three thresholds, and answers three booleans.
//
// * Producer-agnostic at the consumer's end. `CompetitionStateMachine` consumes
//   the three booleans and never learns how they were produced; the strategy behind
//   them (today, the run axis thresholded at the plan's landmarks) is the vehicle
//   layer's business, and replacing that strategy is not a change to the run policy.
//
// What this module is not
//
// * **It is not a sensor, and these facts are not observations of the bottle.**
//   This vehicle has no bottle sensor: no microswitch, no contact strip, no
//   current threshold, no operator watching the mechanism. `approachComplete` says
//   the travel has entered the credible contact window - it is emphatically *not* a
//   claim that the bottle was touched, and no code path may read it as one.
//   `captureComplete` and `pushComplete` are statements about how far the vehicle
//   has travelled, nothing else.
// * It is not a measurement of the mechanism. The distances behind the thresholds
//   arrive as `Sourced` configuration inside the plan, and a value that is not
//   `Measured` refuses the *plan* instead of producing evidence, so no phase exit
//   rests on a placeholder. What they mean physically - how far ahead of the
//   vehicle's measurement point the pusher actually touches the bottle, how far the
//   bottle must be carried before it is captured, how far past the line it must end
//   up - is established by the bottle-integration protocol
//   (`docs/bringup/P-H8-bottle-integration.md`) against independently observed
//   bottle positions, never by tuning this module.
// * It does not decide that a phase may advance, does not stop anything and does
//   not know what a phase is. It reports completion facts; the run policy consumes
//   them, and the phase's own configured bound is what faults a run whose completion
//   never arrives.
//
// **Deviation from the frozen wording in `SOFTWARE_ARCHITECTURE.md` section 14,
// recorded here and in `DEVELOPMENT_LOG.md`:** that section says the bottle
// interaction's evidence is "not inferred from distance or from elapsed time",
// because when it was written no mechanism existed and inferring a contact would
// have been inventing one. The mechanism now exists, it has no detection of its
// own, and the run cannot complete without an exit from `Approach`, `Capture` and
// `Push` - so the inference is implemented, in this one module, with every
// distance required to be measured before it may be used, and with the honesty of
// the claim stated in the names themselves: these are progression facts derived
// from the vehicle's own measured travel, not observations of the bottle. The
// bottle's actual behavior still has to be verified against an independent
// observation (P-H8) before the bottle-interaction row (`VERIFIED_STATE.md` #11)
// may be promoted.

// The three distances that establish the progression thresholds. Every field is
// supplied by the caller; none has a default, because none has been measured.
//
// This module does not apply them itself: the run's execution plan consumes them
// (`buildRunExecutionPlan()`, `RunExecutionPlan.h`) and hands this module the three
// positions they produced.
struct BottleEvidenceConfig {
    constexpr BottleEvidenceConfig(Sourced<double> contactLeadDistanceM_,
                                   Sourced<double> captureAdvanceDistanceM_,
                                   Sourced<double> pushMarginDistanceM_)
        : contactLeadDistanceM(contactLeadDistanceM_),
          captureAdvanceDistanceM(captureAdvanceDistanceM_),
          pushMarginDistanceM(pushMarginDistanceM_) {}

    // How far *before* the Bottle Start Point the *nominal* contact position lies,
    // metres: the run's datum is its Measurement Point, and a pusher that reaches
    // ahead of it would touch the bottle before the Measurement Point arrives, so
    // this is that offset, measured from the mechanism. 0 means "the Measurement
    // Point reaching the bottle's announced position is nominal contact". The
    // credible window around this nominal position is the plan's
    // (`RunExecutionDependencies::contactWindowToleranceM`).
    Sourced<double> contactLeadDistanceM;

    // How much further the forward travel must advance past the nominal contact
    // position before the bottle counts as captured rather than merely touched,
    // metres. The capture guarantee is the nominal position plus this advance, and
    // a valid plan requires it to cover the whole credible window.
    Sourced<double> captureAdvanceDistanceM;

    // How far past the Bottle Line rule landmark the bottle must be carried
    // before the push counts as complete, metres. Pushing the bottle to the line
    // means the vehicle has to reach the line at least; 0 means "the vehicle
    // reaching the Bottle Line is the push". This is the event's rule landmark,
    // not the vehicle's physical forward stop target: that is execution geometry
    // owned by the immutable `RunExecutionPlan` (`RunExecutionPlan.h`), and the run
    // layer's push completes at the plan's own forward stop, which every valid plan
    // places at or beyond this landmark.
    Sourced<double> pushMarginDistanceM;
};

enum class BottleEvidenceConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,  // a distance is not ValueSource::Measured
    NonFiniteValue,        // NaN or infinity
    NegativeDistance,      // a distance below zero: a lead or an advance is not a retreat
};

struct BottleEvidenceConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    BottleEvidenceConfigError error;
};

// Single entry point for this module's configuration validation. The distances
// are positions along the run axis, so a negative one is refused rather than
// reinterpreted: a negative lead would claim the pusher touches the bottle after
// the vehicle's datum has passed it.
inline BottleEvidenceConfigValidation validateBottleEvidenceConfig(
    const BottleEvidenceConfig& config) {
    if (!config.contactLeadDistanceM.isMeasured() ||
        !config.captureAdvanceDistanceM.isMeasured() || !config.pushMarginDistanceM.isMeasured()) {
        return {false, BottleEvidenceConfigError::ValuesAreNotMeasured};
    }
    if (!std::isfinite(config.contactLeadDistanceM.value) ||
        !std::isfinite(config.captureAdvanceDistanceM.value) ||
        !std::isfinite(config.pushMarginDistanceM.value)) {
        return {false, BottleEvidenceConfigError::NonFiniteValue};
    }
    if (config.contactLeadDistanceM.value < 0.0 || config.captureAdvanceDistanceM.value < 0.0 ||
        config.pushMarginDistanceM.value < 0.0) {
        return {false, BottleEvidenceConfigError::NegativeDistance};
    }
    return {true, BottleEvidenceConfigError::None};
}

// The three positions at which the forward progression completes, metres on the
// run axis, signed (positive is forward travel).
//
// Owned by the run's execution plan: this is the plan's own
// `earliestContactMpPositionM`, `captureGuaranteedMpPositionM` and
// `forwardStopMpPositionM`, handed over as a value so this module needs no
// knowledge of how they were derived and cannot derive them a second time.
struct ForwardProgressionThresholds {
    constexpr ForwardProgressionThresholds(double approachCompletePositionM_,
                                           double captureCompletePositionM_,
                                           double pushCompletePositionM_)
        : approachCompletePositionM(approachCompletePositionM_),
          captureCompletePositionM(captureCompletePositionM_),
          pushCompletePositionM(pushCompletePositionM_) {}

    // The earliest MP position at which the bottle's touch could credibly be
    // reported: the start of the credible capture window. Reaching it means the
    // window is open - the travel has entered the stretch where contact is possible
    // - and never that contact happened.
    double approachCompletePositionM;

    // The MP position at which capture is guaranteed: nominal contact plus the
    // measured capture advance, which a valid plan requires to cover the whole
    // credible window.
    double captureCompletePositionM;

    // The MP position at which the push counts as complete: the run's forward
    // execution target (the Bottle Line rule landmark plus the measured forward-stop
    // margin).
    double pushCompletePositionM;
};

// The three progression facts, for one cycle. Every field defaults to the
// unusable value, so a packet that was never filled reports nothing observed.
struct BottleEvidence {
    // Each boolean is "the travel has reached this threshold", never "something was
    // detected". A phase that has been entered stays entered: the facts are monotone
    // in the position while the leg travels forward.
    bool approachComplete = false;
    bool captureComplete = false;
    bool pushComplete = false;

    // The positions the answer was computed against, metres on the run axis, for
    // diagnostics: what the firmware believed the three thresholds were. They are
    // reported even when the position verdict was unusable, so a log can show the
    // geometry a run was working with next to the facts it produced.
    double approachCompletePositionM = 0.0;
    double captureCompletePositionM = 0.0;
    double pushCompletePositionM = 0.0;

    // True when the position was usable and the thresholds were usable numbers,
    // i.e. when the booleans above mean anything at all. False is "nothing observed",
    // which is exactly what a run phase must wait on.
    bool derived = false;
};

// Derives the forward-leg progression facts from the run-axis position.
//
// `positionUsable` is the run layer's combined position verdict
// (`odometryReadiness().usable && RunAxisPosition::usable`): an unusable position
// produces no evidence at all, so a run whose scale or polarity is still a
// placeholder simply waits - and, with the configuration this firmware ships, arms
// nowhere near this code path. A stale sample arrives here as an unusable position
// for the same reason and with the same effect, so the staleness verdict is not
// re-derived in this module.
//
// The reverse leg reports nothing: the bottle interaction is a forward-leg event,
// and a leg that is travelling back toward the Target Point is not driving into the
// bottle. The leg arrives as an argument rather than being re-derived from the
// motion command, so the caller keeps the one leg decision it already made
// (`runCommandNamesForwardLeg()`).
inline BottleEvidence deriveForwardProgression(bool forwardLeg, double positionM,
                                               bool positionUsable,
                                               const ForwardProgressionThresholds& thresholds) {
    BottleEvidence evidence;
    evidence.approachCompletePositionM = thresholds.approachCompletePositionM;
    evidence.captureCompletePositionM = thresholds.captureCompletePositionM;
    evidence.pushCompletePositionM = thresholds.pushCompletePositionM;

    // A threshold that is not a number is no threshold: refusing reports nothing
    // rather than comparing a position against an infinity, which would report every
    // fact at once.
    if (!std::isfinite(evidence.approachCompletePositionM) ||
        !std::isfinite(evidence.captureCompletePositionM) ||
        !std::isfinite(evidence.pushCompletePositionM)) {
        return evidence;
    }
    if (!forwardLeg || !positionUsable || !std::isfinite(positionM)) {
        return evidence;
    }

    evidence.derived = true;
    evidence.approachComplete = positionM >= evidence.approachCompletePositionM;
    evidence.captureComplete = positionM >= evidence.captureCompletePositionM;
    evidence.pushComplete = positionM >= evidence.pushCompletePositionM;
    return evidence;
}

}  // namespace ev
