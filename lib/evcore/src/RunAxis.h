#pragma once

#include "Motion.h"
#include "Odometry.h"
#include "RunConfig.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// The run axis - the competition geometry and where the vehicle is on it
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The one production owner of the run-axis arithmetic the run layer needs:
//
//     - the announced *rule* geometry of a run, on a single axis measured in
//       metres from the Start Point: the Target Point (rule 8.h), the Bottle
//       Start Point (rule 7.c.ii.(5)) and the Bottle Line rule landmark (the
//       Target Point plus the announced Bottle Line Distance, rule 8.j);
//     - the vehicle's position on that axis from the production
//       tick-to-distance conversion, with the measured tick-sign polarity
//       applied exactly once;
//     - the generic remaining-distance arithmetic the run layer consumes, which
//       takes an explicit, caller-supplied target rather than choosing one
//       itself. The target is supplied by the consumer's own execution plan
//       (`RunExecutionPlan.h`), not by this module: this arithmetic must not be
//       able to substitute a rule landmark for it.
//
// * Pure arithmetic. No clock, no pin, no interrupt, no allocation, no
//   filtering, and no decision about a run.
//
// Why it exists as its own module
//
// * `Odometry.h` converts ticks to travel and deliberately applies no polarity:
//   its sign inherits the encoder's counting convention, and reconciling that
//   convention with a physical run axis is documented there as the vehicle
//   layer's job. This module is that job, written down once -
//   `runAxisPositionM()` is the only place the tick-sign polarity is applied.
// * `CompetitionStateMachine` deliberately stores no geometry: the announced
//   values are validated at the announcement boundary and then not kept, and no
//   phase transition derives geometry from them. The run layer therefore has to
//   hold them, and holding them without an owner is how two places start
//   disagreeing about where a rule landmark is.
//   `RunAxisGeometry` is that owner, built from the announced `RunConfig`
//   through one mapping.
//
// What this module is not
//
// * It owns no measured fact. The distance per tick and the tick-sign polarity
//   arrive as `Sourced` arguments; a value that is not `ValueSource::Measured`
//   produces an unusable answer instead of a number that could reach control.
//   The polarity is UNVERIFIED (`VERIFIED_STATE.md` #4) and stays a placeholder
//   until it is measured, so no run may be armed on a position whose sign is a
//   guess.
// * It does not decide the run's leg. It takes the leg the run named
//   (`runCommandNamesForwardLeg()` in CompetitionStateMachine.h) and answers for
//   that leg; a stop command names a leg, never a powered direction.
// * It does not decide that a position has been reached and it stops nothing.
//   "Remaining distance zero" is a statement about the estimate, not about the
//   vehicle - the same warning as `MotionProfile`'s `WithinBrakeOffset`.
// * It does not re-derive sample freshness. A stale sample is a readiness
//   question owned by `odometryReadiness()` (Odometry.h), and the consumer
//   combines that verdict with this module's usability verdict; neither fact is
//   folded into the other.
// * It does not validate the announced values. Range, grid and cross-field
//   checks stay in `validateRunConfig()` (RunConfig.h), the single entry point;
//   an unvalidated `RunConfig` must not be turned into a geometry at all.

// The announced geometry of one run, in metres from the Start Point.
//
// Built from the announced `RunConfig` so the mapping from the event's values to
// the run axis is written once. The Start Point is the axis origin by definition
// of the event (rules 8.h, 8.j, 7.c.ii.(5)), which is why the geometry carries
// absolute positions rather than offsets.
struct RunAxisGeometry {
    constexpr RunAxisGeometry(double targetPointPositionM_, double bottleLineDistanceM_,
                              double bottleStartPositionM_)
        : targetPointPositionM(targetPointPositionM_),
          bottleLineDistanceM(bottleLineDistanceM_),
          bottleStartPositionM(bottleStartPositionM_) {}

    // The one mapping from the announced configuration to the run axis. Rule
    // validation stays in `validateRunConfig()`.
    static constexpr RunAxisGeometry fromRunConfig(const RunConfig& config) {
        return RunAxisGeometry(config.targetDistanceM, config.bottleLineDistanceM,
                               config.bottleStartDistanceM);
    }

    // Position of the Target Point, metres from the Start Point: the announced
    // Target Distance (rule 8.h).
    double targetPointPositionM;

    // The announced Bottle Line Distance (rule 8.j): how far the Bottle Line
    // landmark sits beyond the Target Point.
    double bottleLineDistanceM;

    // Position of the Bottle Start Point, metres from the Start Point (rule
    // 7.c.ii.(5)). Announced rather than derived; the run waits for the
    // mechanism's contact evidence, not for this number. Official rule landmark.
    double bottleStartPositionM;

    // Position of the **Bottle Line**, metres from the Start Point: the event's
    // official rule landmark (rule 8.j), the Target Point plus the announced
    // Bottle Line Distance.
    //
    // This is rule geometry only. It is **not** the vehicle's physical
    // turn-around point, its forward-leg endpoint or its forward stop target -
    // those are execution geometry, owned by the immutable `RunExecutionPlan`
    // (`RunExecutionPlan.h`), whose single forward stop target the run layer reads
    // through `ev::legExecutionTargetM()`. Nothing in this module maps a leg onto
    // this landmark.
    constexpr double bottleLinePositionM() const {
        return targetPointPositionM + bottleLineDistanceM;
    }
};

// Why a run-axis position cannot be used for control.
enum class RunAxisStatus : uint8_t {
    // The tick-to-distance conversion produced a measured, finite travel and the
    // tick-sign polarity is a measured fact. This is the only usable status, and
    // the zero value.
    Usable = 0,
    // The conversion refused: the scale is not a measured, finite, positive fact,
    // or the product overflowed. The detailed reason is `distanceTravelledM()`'s
    // (Odometry.h); this reports that no run-axis number exists at all.
    TickTravelUnusable = 1,
    // The tick-sign polarity is not `ValueSource::Measured`, so no physical sign
    // may be applied to tick travel. A diagnostic number is still reported under
    // the placeholder polarity - the same convention as a Provisional scale,
    // which still reports a diagnostic distance - but it may not reach control,
    // and no run may be driven on a position whose sign is a guess.
    PolarityIsNotMeasured = 2,
};

// The vehicle's position on the run axis. Deliberately no default constructor:
// a position with no conversion behind it must be impossible to write.
struct RunAxisPosition {
    constexpr RunAxisPosition(double positionM_, bool usable_, RunAxisStatus status_)
        : positionM(positionM_), usable(usable_), status(status_) {}

    // Metres from the Start Point, signed: positive is forward travel. 0.0
    // whenever no position exists, so a refusal never reports the origin as if
    // the vehicle were still there.
    double positionM;

    // Invariant, pinned by every call: usable == (status == Usable). False means
    // "do not use this for control", not "this number is narrow enough to be
    // harmless".
    bool usable;

    RunAxisStatus status;
};

static_assert(!std::is_default_constructible<RunAxisPosition>::value,
              "no silent defaults; a run-axis position must state how it was produced");
static_assert(static_cast<int>(RunAxisStatus::Usable) == 0,
              "the usable status must stay the zero value");

// Position on the run axis, from the production tick-to-distance conversion.
//
// The axis's zero is the Start Point, which is where the run's origin tick
// reading is taken, so the origin position is not a parameter: a run is defined
// relative to the Start Point, and a second, caller-supplied origin would be a
// second convention for the same axis. `originTicks` is that reading - an
// argument, never a constant - and only the difference between the two readings
// is used (Odometry.h), so an ordinary zeroed counter and a large accumulated
// one both work.
//
// The tick-sign polarity is applied here, once, and it is applied to the travel
// rather than to a position, so the sign cannot be inverted by an origin
// position that is not zero. `positiveTicksDirection` states which physical
// direction a positive tick delta corresponds to; it is the encoder-direction
// fact (`VERIFIED_STATE.md` #4), which is a separate measurement from the two
// command-sign encodings, and until it is measured the position is unusable.
//
// Check order is deliberate: no number at all is reported when the scaling
// arithmetic refused (a position of zero would look like "has not moved yet"),
// then provenance (an unmeasured polarity refuses the sign), then the scale's own
// diagnostic case.
inline RunAxisPosition runAxisPositionM(int32_t originTicks, int32_t currentTicks,
                                        const Sourced<double>& distancePerEncoderTickM,
                                        const Sourced<MotionDirection>& positiveTicksDirection) {
    const OdometryEstimate travel =
        distanceTravelledM(originTicks, currentTicks, distancePerEncoderTickM);

    // A Provisional scale still produces a diagnostic number (`ScaleIsNotMeasured`);
    // every other refusal means no meaningful travel exists at all.
    if (travel.status != OdometryEstimateStatus::Valid &&
        travel.status != OdometryEstimateStatus::ScaleIsNotMeasured) {
        return RunAxisPosition(0.0, false, RunAxisStatus::TickTravelUnusable);
    }

    const double signedTravelM = positiveTicksDirection.value == MotionDirection::Forward
                                     ? travel.distanceM
                                     : -travel.distanceM;

    if (!positiveTicksDirection.isMeasured()) {
        return RunAxisPosition(signedTravelM, false, RunAxisStatus::PolarityIsNotMeasured);
    }
    if (!travel.valid) {
        return RunAxisPosition(signedTravelM, false, RunAxisStatus::TickTravelUnusable);
    }
    return RunAxisPosition(signedTravelM, true, RunAxisStatus::Usable);
}

// The whole rule-mandated travel an announced run covers, metres: forward out to
// the Bottle Line rule landmark (rule 8.j) and back to the Target Point (rule
// 8.h). This is the distance a timing plan is sized against - the run must
// travel it, not just the announced Target Distance - and it is derived from the
// announced rule geometry, so there is no second, competing distance for a run's
// budget. It is a rule quantity used to size the plan, not an execution target:
// the vehicle travels to its execution plan's own targets, which are further out
// than this rule course (`RunExecutionPlan.h`), and reconciling the two for the
// timing estimate is a separate, owned decision rather than something this
// arithmetic may assume.
inline double plannedTravelDistanceM(const RunAxisGeometry& geometry) {
    return geometry.bottleLinePositionM() + geometry.bottleLineDistanceM;
}

// Remaining distance from the current position to an **explicit,
// caller-supplied target**, metres, along the leg being travelled: positive
// while the target is still ahead, zero at it, and negative once it has been
// passed.
//
// The target is an argument and is never chosen here. This is the generic
// arithmetic the run layer consumes: a caller measuring to the Target Point
// passes `targetPointPositionM`, and a caller measuring to the forward leg's
// execution target passes whatever its execution plan named - so this arithmetic
// cannot substitute a rule landmark (such as the Bottle Line) for the target a
// consumer actually asked about.
//
// Deliberately not clamped. An overshoot is evidence, and clamping it to zero
// would hide it from the caller and from a log, exactly as clamping a stale
// number to something plausible would.
inline double remainingDistanceTo(double targetPositionM, double currentPositionM,
                                  bool forwardLeg) {
    return forwardLeg ? (targetPositionM - currentPositionM)
                      : (currentPositionM - targetPositionM);
}

// The same explicit-target arithmetic, never below zero.
//
// This is the distance a travel taper is sized from. A leg that has finished its
// distance reports exactly zero rather than a negative number, because a
// negative "remaining" handed to a taper is arithmetic about a journey that no
// longer exists.
inline double remainingDistanceToClamped(double targetPositionM, double currentPositionM,
                                         bool forwardLeg) {
    const double remainingM = remainingDistanceTo(targetPositionM, currentPositionM, forwardLeg);
    return remainingM > 0.0 ? remainingM : 0.0;
}

}  // namespace ev
