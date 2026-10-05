#pragma once

#include "RunAxis.h"
#include "Units.h"

#include <cmath>
#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The bottle-interaction evidence - where the run's forward phases get their exits
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One mapping, from facts the vehicle already has to the three pieces of
//   evidence the run policy waits for on its forward leg:
//
//       the forward travel has reached the Bottle Start Point  -> bottleContactDetected
//       ... and a configured advance beyond that point          -> captureComplete
//       ... and the Bottle Line rule landmark (rule 8.j)        -> pushComplete
//
// * Deterministic, monotone in the vehicle's own position, and pure: no clock,
//   no pin, no interrupt, no allocation. It takes the run-axis position with its
//   own usability verdict and the announced geometry, and answers three booleans.
// * Distance-derived, and that is a stated decision rather than an oversight
//   (see the deviation note below).
//
// What this module is not
//
// * It is not a sensor. This vehicle has no bottle sensor: no microswitch, no
//   contact strip, no current threshold. The evidence below is the firmware's
//   *belief* about where the vehicle is on an axis, computed from the encoder, and
//   a belief is not an observation.
// * It is not a measurement of the mechanism. The three distances arrive as
//   `Sourced` configuration and a value that is not `Measured` refuses the
//   evidence instead of producing it, so no phase exit rests on a placeholder.
//   What they mean physically - how far ahead of the vehicle's measurement point
//   the pusher actually touches the bottle, how far the bottle must be carried
//   before it is captured, how far past the line it must end up - is established
//   by the bottle-integration protocol (`docs/bringup/P-H8-bottle-integration.md`)
//   against independently observed bottle positions, never by tuning this module.
// * It does not decide that a phase may advance, does not stop anything and does
//   not know what a phase is. It reports evidence; `CompetitionStateMachine`
//   consumes it, and the phase's own configured bound is what faults a run whose
//   evidence never arrives.
//
// **Deviation from the frozen wording in `SOFTWARE_ARCHITECTURE.md` section 14,
// recorded here and in `DEVELOPMENT_LOG.md`:** that section says the bottle
// interaction's evidence is "not inferred from distance or from elapsed time",
// because when it was written no mechanism existed and inferring a contact would
// have been inventing one. The mechanism now exists, it has no detection of its
// own, and the run cannot complete without an exit from `Approach`, `Capture` and
// `Push` - so the inference is implemented, in this one module, with every
// distance required to be measured before it may be used, and with the honesty
// of the claim stated: this is the firmware's position-derived belief, and the
// bottle's actual behavior still has to be verified against an independent
// observation (P-H8) before the bottle-interaction row (`VERIFIED_STATE.md` #11)
// may be promoted. A belief is not evidence about the bottle.

// The three distances that turn a position into evidence. Every field is
// supplied by the caller; none has a default, because none has been measured.
struct BottleEvidenceConfig {
    constexpr BottleEvidenceConfig(Sourced<double> contactLeadDistanceM_,
                                   Sourced<double> captureAdvanceDistanceM_,
                                   Sourced<double> pushMarginDistanceM_)
        : contactLeadDistanceM(contactLeadDistanceM_),
          captureAdvanceDistanceM(captureAdvanceDistanceM_),
          pushMarginDistanceM(pushMarginDistanceM_) {}

    // How far *before* the Bottle Start Point contact is reported, metres. The
    // vehicle's own datum is its Measurement Point; a pusher that reaches ahead
    // of it touches the bottle before the Measurement Point arrives, and this is
    // that offset, measured from the mechanism. 0 means "contact when the
    // measurement point reaches the bottle's announced position".
    Sourced<double> contactLeadDistanceM;

    // How much further the forward travel must advance past the contact point
    // before the bottle counts as captured rather than merely touched, metres.
    Sourced<double> captureAdvanceDistanceM;

    // How far past the Bottle Line rule landmark the bottle must be carried
    // before the push counts as complete, metres. Pushing the bottle to the line
    // means the vehicle has to reach the line at least; 0 means "the vehicle
    // reaching the Bottle Line is the push". This is the event's rule landmark,
    // not the vehicle's physical forward stop target (that is execution geometry
    // owned by S3/S4).
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

// The three pieces of evidence, for one cycle. Every field defaults to the
// unusable value, so a packet that was never filled reports nothing observed.
struct BottleEvidence {
    bool contactDetected = false;
    bool captureComplete = false;
    bool pushComplete = false;

    // The positions the answer was computed against, metres on the run axis, for
    // diagnostics: what the firmware believed the three landmarks were. They are
    // reported even when the position verdict was unusable, so a log can show the
    // geometry a run was working with next to the evidence it produced.
    double contactPositionM = 0.0;
    double capturePositionM = 0.0;
    double pushPositionM = 0.0;

    // True when the position was usable and the configuration was measured, i.e.
    // when the booleans above mean anything at all. False is "nothing observed",
    // which is exactly what a run phase must wait on.
    bool derived = false;
};

// Derives the forward-leg evidence from the run-axis position.
//
// `positionUsable` is the run layer's combined position verdict
// (`odometryReadiness().usable && RunAxisPosition::usable`): an unusable position
// produces no evidence at all, so a run whose scale or polarity is still a
// placeholder simply waits - and, with the configuration this firmware ships,
// arms nowhere near this code path.
//
// The reverse leg reports nothing: contact, capture and push are forward-leg
// events, and a leg that is travelling back toward the Target Point is not
// touching the bottle. The leg arrives as an argument rather than being re-derived
// from the motion command, so the caller keeps the one leg decision it already
// made (`runCommandNamesForwardLeg()`).
inline BottleEvidence deriveBottleEvidence(const RunAxisGeometry& geometry, bool forwardLeg,
                                           double positionM, bool positionUsable,
                                           const BottleEvidenceConfig& config) {
    BottleEvidence evidence;
    evidence.contactPositionM = geometry.bottleStartPositionM - config.contactLeadDistanceM.value;
    evidence.capturePositionM =
        evidence.contactPositionM + config.captureAdvanceDistanceM.value;
    evidence.pushPositionM = geometry.bottleLinePositionM() + config.pushMarginDistanceM.value;

    if (!validateBottleEvidenceConfig(config).ok) {
        return evidence;
    }
    if (!forwardLeg || !positionUsable || !std::isfinite(positionM)) {
        return evidence;
    }

    evidence.derived = true;
    evidence.contactDetected = positionM >= evidence.contactPositionM;
    evidence.captureComplete = positionM >= evidence.capturePositionM;
    evidence.pushComplete = positionM >= evidence.pushPositionM;
    return evidence;
}

}  // namespace ev
