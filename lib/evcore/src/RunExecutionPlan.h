#pragma once

#include "BottleEvidence.h"
#include "RunAxis.h"
#include "RunConfig.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// The run execution plan - the execution targets of ONE announced run
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The one production owner of a run's **execution geometry**: where the
//   vehicle must actually stop and where the bottle interaction landmarks sit on
//   the run axis, derived once, at the announcement boundary, from
//
//     - the announced *rule* geometry (`RunAxisGeometry`, `RunAxis.h`), which
//       this module consumes and never redefines;
//     - the bottle-interaction calibration the evidence model already uses
//       (`BottleEvidenceConfig`, `BottleEvidence.h`); and
//     - the physical dependencies that establish the forward execution target
//       (`RunExecutionDependencies` below), which are exactly the facts that do
//       not exist yet.
//
// * A pure value: no Arduino headers, no I/O, no serial formatting, no
//   allocation, no clock, no pin, no globals and no knowledge of `VehicleConfig`.
//   It reads nothing but its arguments and it commands nothing.
//
// * Immutable once constructed. There are no setters and every field is `const`,
//   so a plan cannot be adjusted as a run progresses: an announcement produces
//   one plan, and a new announcement produces a new plan.
//
// What this module is not
//
// * **It is not rule geometry.** S2 separated the two: `RunAxisGeometry` owns the
//   rule landmarks (Target Point, Bottle Start Point, Bottle Line) and this module
//   owns the execution targets. The Bottle Line is an official rule landmark and
//   is **not** assumed to be the vehicle's physical forward stop.
// * **It owns the forward progression's thresholds, and it is not the evidence.**
//   `BottleEvidence.h` turns a position into the three progression facts the run
//   waits for (`approachComplete`, `captureComplete`, `pushComplete`); the
//   *positions* those facts are thresholded at are this plan's
//   (`earliestContactMpPositionM`, `captureGuaranteedMpPositionM`,
//   `forwardStopMpPositionM`), handed to that module through
//   `ev::deriveBottleEvidence(plan, ...)`. The plan derives them once, from the
//   announced rule geometry and the measured calibration, so there is no second
//   derivation that could disagree with the travel taper about where an event is.
//   The plan does not re-implement the comparison and does not decide that a phase
//   may advance.
// * **It is not a motion profile and it is not a state machine.**
//   `MotionProfile.h` owns shaping and the diagnostic timing estimate;
//   `CompetitionStateMachine.h` owns phase transitions. This plan owns neither,
//   and it carries no motion command.
// * **It is not a timing model.** It does not estimate travel time, but it is
//   the one owner of the run's **deliberate start delay**: `initialDelayS` is
//   what the run policy consumes at the announcement boundary
//   (`CompetitionStateMachine::announceRun()`, the `OptionalDelay` phase), and no
//   static phase policy holds a second copy of it (the phase policy contributes
//   only the legal/safety maximum that bounds it). It carries `timingReady`,
//   `predictedMotionTimeS` and `initialDelayS` because timing is per-run state,
//   and it records them honestly: until empirical physical timing evidence
//   exists, `timingReady` is false and both numbers are 0.0 ("not meaningfully
//   available"), which overrides even a non-zero argument.
// * **The run layer consumes it, and only through the accessors below.** S4 wired
//   the plan into `VehicleRuntime` and the scenario harness: each leg's execution
//   target (`legExecutionTargetM()`), the forward-leg progression thresholds
//   (`deriveBottleEvidence()`) and the shaping distance the run travels are all
//   read from the plan, and the temporary pre-S3/S4 compatibility seam
//   (`temporaryLegExecutionTargetM()`) is gone. There is one forward stop value in
//   an active run, and the taper and the phase progression both read it.
// * **It invents no physical value.** The forward stop is the Bottle Line plus a
//   *measured* margin; with no margin established, construction refuses with a
//   named reason instead of quietly equating the forward stop with the rule
//   landmark.
//
// Why the refusal is the point
//
// The physical facts behind the forward stop - pusher reach, the contact window,
// bottle capture and transfer behaviour, the push margin and braking behaviour -
// are not established on this vehicle. `VERIFIED_STATE.md` and `P-H8` own that
// work. Until it exists, a production plan is *unavailable*, and this module's
// job is to say so with a name rather than to produce a plausible-looking plan
// from the Bottle Line.

// Why an execution plan does not exist. The zero value is `Planned`, so a plan
// that was never produced cannot default into a valid-looking one: there is no
// default constructor below, and `refusedExecutionPlan()` is how "no plan" is
// stated. The names are the wire/diagnostic spelling of the refusal.
enum class RunExecutionPlanStatus : uint8_t {
    // A complete, immutable plan exists. This is the only status a plan may be
    // consumed under, and the zero value.
    Planned = 0,
    // The announced `RunConfig` failed `validateRunConfig()`: the announcement is
    // not a legal one, so no execution arithmetic may run on it at all.
    InvalidAnnouncement = 1,
    // A bottle-interaction distance, or the contact window's own tolerance, is
    // not an established physical fact (`ValueSource::Measured`). `Provisional`
    // never authorizes an execution plan that depends on it.
    MissingBottleInteractionCalibration = 2,
    // The forward execution target's own physical dependency - the margin by
    // which the commanded forward stop passes the Bottle Line rule landmark - is
    // not established. With no such fact, there is no forward stop to plan to.
    MissingForwardExecutionTarget = 3,
    // A supplied or derived value is NaN or infinite. Provenance says where a
    // number came from; it does not say it is a number.
    NonFiniteValue = 4,
    // The derived interaction landmarks are not in a physically possible order
    // (see the ordering invariant in `buildRunExecutionPlan()`).
    ContradictoryExecutionGeometry = 5,
    // No announcement has produced this plan yet: the value a run layer holds
    // before its first announcement and returns to at a new-run boundary. It is a
    // separate status from `InvalidAnnouncement` because "no announcement at all"
    // and "an announcement that was refused" are different facts, and a
    // diagnostic that could not tell them apart could not name the cause.
    NotPlanned = 6,
};

inline const char* runExecutionPlanStatusName(RunExecutionPlanStatus status) {
    switch (status) {
        case RunExecutionPlanStatus::Planned:
            return "Planned";
        case RunExecutionPlanStatus::InvalidAnnouncement:
            return "InvalidAnnouncement";
        case RunExecutionPlanStatus::MissingBottleInteractionCalibration:
            return "MissingBottleInteractionCalibration";
        case RunExecutionPlanStatus::MissingForwardExecutionTarget:
            return "MissingForwardExecutionTarget";
        case RunExecutionPlanStatus::NonFiniteValue:
            return "NonFiniteValue";
        case RunExecutionPlanStatus::ContradictoryExecutionGeometry:
            return "ContradictoryExecutionGeometry";
        case RunExecutionPlanStatus::NotPlanned:
            return "NotPlanned";
    }
    return "?";
}

// The physical dependencies that establish the forward execution target, and the
// credibility window of the contact landmark. Every field is supplied by the
// caller and is required to be a measured fact; none has a default, because none
// has been measured on this vehicle.
struct RunExecutionDependencies {
    constexpr RunExecutionDependencies(Sourced<double> contactWindowToleranceM_,
                                        Sourced<double> forwardStopMarginBeyondBottleLineM_)
        : contactWindowToleranceM(contactWindowToleranceM_),
          forwardStopMarginBeyondBottleLineM(forwardStopMarginBeyondBottleLineM_) {}

    // How far either side of the nominal contact position the bottle's actual
    // touch may credibly be reported, metres (so the credible window is twice
    // this). The nominal contact position itself comes from the announced Bottle
    // Start Point and the configured contact lead; this is the measured spread
    // around it - the pusher's own mechanical window and the repeatability of the
    // approach - and it is what makes the earliest and latest credible contact
    // positions two different landmarks. `P-H8` measures it against independently
    // observed bottle contact positions. 0 is a legitimate value for a mechanism
    // whose touch position is exactly repeatable, and it is still a measurement.
    Sourced<double> contactWindowToleranceM;

    // How far **beyond the Bottle Line rule landmark** the commanded forward stop
    // position lies, metres. This is the one dependency that establishes the
    // forward execution target, and it exists because the Bottle Line is rule
    // geometry rather than a physical stop: driving to the line is not the same
    // fact as having pushed and captured the bottle, and a commanded stop is not
    // the physical resting position either (`AGENTS.md` sections 10 and 15).
    // Pusher reach, the contact window, capture/transfer behaviour, the push
    // margin and braking all fold into this distance, and it is measured from
    // observed bottle positions and a braking characterization - never from the
    // rule landmark alone.
    Sourced<double> forwardStopMarginBeyondBottleLineM;
};

static_assert(!std::is_default_constructible<RunExecutionDependencies>::value,
              "execution dependencies must state their provenance, never default");

// One announced run's execution plan. Deliberately no default constructor: a plan
// with no announcement behind it must be impossible to write. Every field is
// `const`, so a plan is immutable after construction - there is no `setDelay()`,
// no `setForwardStop()` and no path that adjusts a plan as a run progresses.
//
// Invariant, pinned by every test: `valid == (status == Planned)`, and a refused
// plan carries no derived position at all.
struct RunExecutionPlan {
    constexpr RunExecutionPlan(RunAxisGeometry geometryM_,
                               double earliestContactMpPositionM_,
                               double latestContactMpPositionM_,
                               double captureGuaranteedMpPositionM_,
                               double pushCompleteMpPositionM_, double forwardStopMpPositionM_,
                               double reverseStopMpPositionM_, bool timingReady_,
                               double predictedMotionTimeS_, double initialDelayS_, bool valid_,
                               RunExecutionPlanStatus status_)
        : geometry(geometryM_),
          earliestContactMpPositionM(earliestContactMpPositionM_),
          latestContactMpPositionM(latestContactMpPositionM_),
          captureGuaranteedMpPositionM(captureGuaranteedMpPositionM_),
          pushCompleteMpPositionM(pushCompleteMpPositionM_),
          forwardStopMpPositionM(forwardStopMpPositionM_),
          reverseStopMpPositionM(reverseStopMpPositionM_),
          timingReady(timingReady_),
          predictedMotionTimeS(predictedMotionTimeS_),
          initialDelayS(initialDelayS_),
          valid(valid_),
          status(status_) {}

    // The announced **rule** geometry the plan was built from, preserved by value
    // so a consumer reads the rule landmarks from the plan rather than re-deriving
    // them. Rule geometry stays owned by `RunAxisGeometry`; this is the validated
    // value, not a second definition of it.
    const RunAxisGeometry geometry;

    // --- Forward-axis interaction landmarks, metres from the Start Point ---
    // Measurement Point (MP) positions: where the vehicle's own measurement point
    // is on the axis when each event happens. Positive is forward travel.

    // The earliest MP position at which the bottle's touch can credibly be
    // reported: the nominal contact position minus the credible tolerance. The
    // forward travel reaching it is where the run's `approachComplete` fact becomes
    // true - the credible capture window opening is a statement about the travel,
    // never a claim that the bottle was touched.
    const double earliestContactMpPositionM;

    // The latest MP position at which the bottle's touch can credibly be
    // reported: the nominal contact position plus the credible tolerance. A plan
    // whose agent has not touched the bottle by here cannot claim a credible
    // contact at all.
    const double latestContactMpPositionM;

    // The MP position at which the bottle counts as captured, given the
    // configured advance past the *nominal* contact position. This is the position
    // the run's `captureComplete` fact is thresholded at, and a valid plan requires
    // it to cover the whole credible contact window. Deliberately not named
    // "measured": it is derived arithmetic on measured inputs.
    const double captureGuaranteedMpPositionM;

    // The MP position at which the push counts as complete: the Bottle Line rule
    // landmark plus the configured push margin. This is the plan's *rule-derived*
    // push landmark - the objective the event asks for - and it is carried here for
    // the forward stop ordering check and for diagnostics. The run layer's own
    // completion threshold is `forwardStopMpPositionM` (see the consumption
    // section below), which every valid plan places at or beyond this landmark.
    const double pushCompleteMpPositionM;

    // The commanded forward stop position, metres: the Bottle Line rule landmark
    // plus the measured forward-stop margin. This is the explicit forward
    // execution target the run layer consumes everywhere
    // (`ev::legExecutionTargetM()`, and the `pushComplete` threshold of
    // `ev::deriveBottleEvidence(plan, ...)`), and it is **not** the Bottle Line
    // unless the measured margin is exactly zero.
    const double forwardStopMpPositionM;

    // The commanded reverse stop position, metres: the announced Target Point
    // (rule 8.h). A rule-derived relationship, not a measurement - the reverse
    // leg stops where the rules put the Target Point - so it is deliberately not
    // labelled `Measured` anywhere.
    const double reverseStopMpPositionM;

    // --- Timing state ---

    // True only when empirical physical timing evidence has established this
    // run's predicted motion time and delay. False is the honest production value
    // until that evidence exists, and it is the only value the firmware's
    // configuration currently produces.
    const bool timingReady;

    // The predicted motion time, seconds, when `timingReady` is true. 0.0
    // otherwise, which means "not meaningfully available" - never a claim that the
    // run takes no time.
    const double predictedMotionTimeS;

    // The run's deliberate stationary delay before travel, seconds: the one value
    // the run policy consumes as its per-run delay (the `OptionalDelay` phase),
    // bounded there by the phase policy's legal/safety maximum. Non-zero only when
    // `timingReady` is true; 0.0 otherwise, which is the honest production value
    // until a physical timing model is validated.
    const double initialDelayS;

    // Invariant: `valid == (status == RunExecutionPlanStatus::Planned)`.
    const bool valid;

    const RunExecutionPlanStatus status;
};

static_assert(!std::is_default_constructible<RunExecutionPlan>::value,
              "no silent defaults; an execution plan must be built from an announcement");
static_assert(!std::is_copy_assignable<RunExecutionPlan>::value,
              "an execution plan must be immutable: every field is const");
static_assert(static_cast<int>(RunExecutionPlanStatus::Planned) == 0,
              "the planned status must stay the zero value");

// The plan with no announcement behind it: what a run layer holds before a run is
// announced, and what a refused construction carries. Every position is zero and
// `valid` is false, exactly like `refusedTimingPlan()`; `status` must not be
// `Planned`, because that would break the invariant above.
inline RunExecutionPlan refusedExecutionPlan(RunExecutionPlanStatus status) {
    return RunExecutionPlan(RunAxisGeometry(0.0, 0.0, 0.0), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, 0.0,
                            0.0, false, status);
}

// The result of one plan-construction attempt: either a complete immutable plan,
// or a named refusal. A caller must check `valid` (equivalently `plan.valid`)
// before reading any position - a refusal carries none.
struct RunExecutionPlanResult {
    constexpr RunExecutionPlanResult(RunExecutionPlan plan_, bool valid_,
                                     RunExecutionPlanStatus status_)
        : plan(plan_), valid(valid_), status(status_) {}

    // The plan, meaningful only when `valid` is true. When `valid` is false this
    // is the refused value: every position 0.0 and `plan.valid` false.
    const RunExecutionPlan plan;

    // Invariant: `valid == (status == Planned) == plan.valid`.
    const bool valid;

    const RunExecutionPlanStatus status;
};

inline RunExecutionPlanResult refusedExecutionPlanResult(RunExecutionPlanStatus status) {
    return RunExecutionPlanResult(refusedExecutionPlan(status), false, status);
}

// Builds the execution plan for one announced run.
//
// The gates are ordered from the most fundamental problem to the most specific,
// so a refusal names one attributable reason (the same convention as
// `firstFailure()` in `ConfigurationChecks.h`):
//
//   1. the announced rule geometry must be legal (`validateRunConfig()`);
//   2. the bottle-interaction calibration must be an established, finite,
//      non-negative fact (`validateBottleEvidenceConfig()`, reused rather than
//      restated);
//   3. the contact window's tolerance and the forward execution target's margin
//      must be established facts;
//   4. every input and every derived position must be finite; and
//   5. the derived landmarks must be in a physically possible order.
//
// Ordering invariant, all four terms required:
//
//   earliestContact <= latestContact <= captureGuaranteed <= forwardStop
//   pushComplete <= forwardStop
//
// Read each on its own:
//
//   * the credible contact window must not be inverted (a negative tolerance is
//     not a window);
//   * the capture guarantee must cover the **whole** credible contact window -
//     otherwise the plan could declare the bottle captured while contact is still
//     uncertain, which is exactly the contradiction that must not reach a run;
//   * the forward stop must not be commanded before capture is guaranteed, and
//     not before the push is complete, because stopping short of either leaves the
//     event's own interaction unfinished; and
//   * the earliest credible contact must not lie behind the Start Point (the axis
//     origin), because the vehicle cannot touch the bottle before it starts.
//
// This is the project's *current* bottle-interaction semantics, read from
// `BottleEvidenceConfig`; S5 owns any redesign of them. Nothing here invents a
// physical constant.
//
// Timing inputs are accepted and recorded honestly: with `timingReady` false the
// plan carries `predictedMotionTimeS = 0.0` and `initialDelayS = 0.0` regardless
// of the arguments, because an unvalidated idealized prediction is not available
// evidence for a run's delay; with `timingReady` true both must be finite and
// non-negative. The recorded `initialDelayS` is then the run's one per-run delay
// - the run layer hands it to `CompetitionStateMachine::announceRun()` - so
// "who decides the delay" has a single answer in an active run. Nothing here
// makes idealized timing control production behavior: the readiness flag is the
// caller's, and the production configuration never sets it.
inline RunExecutionPlanResult buildRunExecutionPlan(
    const RunConfig& announcedConfig, const BottleEvidenceConfig& bottleConfig,
    const RunExecutionDependencies& dependencies, bool timingReady, double predictedMotionTimeS,
    double initialDelayS) {
    // 1. The announcement. An illegal announced value is a rule problem, not an
    //    execution one, and no execution arithmetic may run on it.
    if (!validateRunConfig(announcedConfig).ok) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::InvalidAnnouncement);
    }
    const RunAxisGeometry geometry = RunAxisGeometry::fromRunConfig(announcedConfig);

    // 2. The bottle-interaction calibration, through the bottle module's own
    //    single validation entry point - reused, never restated - with its reasons
    //    mapped onto this plan's names so one refusal has one spelling.
    const BottleEvidenceConfigValidation bottleEvidence = validateBottleEvidenceConfig(bottleConfig);
    if (bottleEvidence.error == BottleEvidenceConfigError::ValuesAreNotMeasured) {
        return refusedExecutionPlanResult(
            RunExecutionPlanStatus::MissingBottleInteractionCalibration);
    }
    if (bottleEvidence.error == BottleEvidenceConfigError::NonFiniteValue) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::NonFiniteValue);
    }
    if (bottleEvidence.error == BottleEvidenceConfigError::NegativeDistance) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    }

    // 3. The contact window and the forward execution target. `Provisional` is a
    //    placeholder and never authorizes a plan that depends on it; `Derived` is
    //    a computed planning value and is not a measurement of the mechanism
    //    either, so only `Measured` passes.
    if (!dependencies.contactWindowToleranceM.isMeasured()) {
        return refusedExecutionPlanResult(
            RunExecutionPlanStatus::MissingBottleInteractionCalibration);
    }
    if (!dependencies.forwardStopMarginBeyondBottleLineM.isMeasured()) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::MissingForwardExecutionTarget);
    }

    const double contactWindowToleranceM = dependencies.contactWindowToleranceM.value;
    const double forwardStopMarginM = dependencies.forwardStopMarginBeyondBottleLineM.value;
    if (!std::isfinite(contactWindowToleranceM) || !std::isfinite(forwardStopMarginM)) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::NonFiniteValue);
    }

    // 4. Derive the execution landmarks. The nominal contact position is the
    //    announced Bottle Start Point advanced by the pusher's measured lead; the
    //    credible window is the measured tolerance either side of it; capture is
    //    the configured advance past it; and the forward stop is the Bottle Line
    //    plus the measured margin - never the Bottle Line itself. The window's
    //    earliest position is where `BottleEvidence.h` reports `approachComplete`
    //    (the window opening), the capture position where it reports
    //    `captureComplete`, and the forward stop where it reports `pushComplete`.
    const double nominalContactMpPositionM =
        geometry.bottleStartPositionM - bottleConfig.contactLeadDistanceM.value;
    const double earliestContactMpPositionM = nominalContactMpPositionM - contactWindowToleranceM;
    const double latestContactMpPositionM = nominalContactMpPositionM + contactWindowToleranceM;
    const double captureGuaranteedMpPositionM =
        nominalContactMpPositionM + bottleConfig.captureAdvanceDistanceM.value;
    const double pushCompleteMpPositionM =
        geometry.bottleLinePositionM() + bottleConfig.pushMarginDistanceM.value;
    const double forwardStopMpPositionM = geometry.bottleLinePositionM() + forwardStopMarginM;
    const double reverseStopMpPositionM = geometry.targetPointPositionM;

    if (!std::isfinite(earliestContactMpPositionM) || !std::isfinite(latestContactMpPositionM) ||
        !std::isfinite(captureGuaranteedMpPositionM) || !std::isfinite(pushCompleteMpPositionM) ||
        !std::isfinite(forwardStopMpPositionM) || !std::isfinite(reverseStopMpPositionM)) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::NonFiniteValue);
    }

    // 5. The ordering invariant. Internally contradictory execution geometry is
    //    refused here rather than carried into a run.
    const bool ordered = earliestContactMpPositionM <= latestContactMpPositionM &&
                         latestContactMpPositionM <= captureGuaranteedMpPositionM &&
                         captureGuaranteedMpPositionM <= forwardStopMpPositionM &&
                         pushCompleteMpPositionM <= forwardStopMpPositionM &&
                         earliestContactMpPositionM >= 0.0;
    if (!ordered) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    }

    // 6. Timing state, recorded honestly. A non-finite timing input on a
    //    timing-ready plan is still no plan; a plan that is not timing-ready
    //    carries 0.0 for both numbers and never the caller's idealized guess.
    if (timingReady && (!std::isfinite(predictedMotionTimeS) || predictedMotionTimeS < 0.0 ||
                        !std::isfinite(initialDelayS) || initialDelayS < 0.0)) {
        return refusedExecutionPlanResult(RunExecutionPlanStatus::NonFiniteValue);
    }
    const double planPredictedMotionTimeS = timingReady ? predictedMotionTimeS : 0.0;
    const double planInitialDelayS = timingReady ? initialDelayS : 0.0;

    const RunExecutionPlan plan(geometry, earliestContactMpPositionM, latestContactMpPositionM,
                                captureGuaranteedMpPositionM, pushCompleteMpPositionM,
                                forwardStopMpPositionM, reverseStopMpPositionM, timingReady,
                                planPredictedMotionTimeS, planInitialDelayS, true,
                                RunExecutionPlanStatus::Planned);
    return RunExecutionPlanResult(plan, true, RunExecutionPlanStatus::Planned);
}

// ---------------------------------------------------------------------------
// Consuming a plan - the run layer's one source for each leg's target
// ---------------------------------------------------------------------------
//
// A plan answers one question per leg: **where does this leg end?**
//
//   * the forward leg ends at `forwardStopMpPositionM`, the explicit execution
//     target (the Bottle Line rule landmark plus the measured forward-stop
//     margin) - never at the Bottle Line itself;
//   * the reverse leg ends at `reverseStopMpPositionM`, the announced Target Point
//     (rule 8.h), which is what the reverse leg ended at before the plan existed.
//
// Both answers come from the plan and from nowhere else. The vehicle layer holds a
// plan and reads it with these functions, so the taper that shapes the travel and
// the phase progression that ends it cannot be working to two different endpoints:
// both are arithmetic on the same stored value.
//
// With no usable plan there is no leg target at all. Every position of a refused
// plan is zero, and a zero is not a landmark: the run layer must treat "no plan" as
// "no target", shape nothing, and claim no forward completion, rather than
// substituting a rule landmark for the execution target the plan was supposed to
// own. That is the fail-closed rule `usable` carries.

// Where one leg of the run ends, metres on the run axis, with its own usability.
// There is deliberately no default: an absent target must be stated as absent.
struct LegExecutionTarget {
    constexpr LegExecutionTarget(double positionM_, bool usable_)
        : positionM(positionM_), usable(usable_) {}

    // The target position, metres from the Start Point, signed (positive is
    // forward travel). Meaningful only when `usable` is true.
    double positionM;

    // False means "this leg has no execution target", which is a different fact
    // from "the target is at zero". A consumer must not shape travel against an
    // unusable target, and must not report its position as a landmark.
    bool usable;
};

static_assert(!std::is_default_constructible<LegExecutionTarget>::value,
              "no silent defaults; an absent leg target must be stated as absent");

// The execution target of the leg the run is travelling. The plan owns it: there
// is no second argument, no fallback and no rule landmark here, so a caller cannot
// ask "where does the forward leg end?" and be handed the Bottle Line because the
// execution target is missing.
inline LegExecutionTarget legExecutionTargetM(const RunExecutionPlan& plan, bool forwardLeg) {
    return forwardLeg ? LegExecutionTarget(plan.forwardStopMpPositionM, plan.valid)
                      : LegExecutionTarget(plan.reverseStopMpPositionM, plan.valid);
}

// The forward-leg progression facts, with every threshold taken from the plan.
//
// The plan is the one owner of the three positions at which the forward progression
// completes, and this function is where the run layer reads them:
//
//   * `approachComplete` at `earliestContactMpPositionM` - the credible capture
//     window opening, which is a statement about the travel and never a claim that
//     the bottle was touched;
//   * `captureComplete` at `captureGuaranteedMpPositionM` - the capture guarantee,
//     which a valid plan places beyond the whole credible window; and
//   * `pushComplete` at `forwardStopMpPositionM` - the run's own forward execution
//     target, so the run leaves `Push` exactly where its shaping target reaches zero
//     and cannot end its push short of the event's objective (the ordering invariant
//     requires `pushCompleteMpPositionM <= forwardStopMpPositionM`).
//
// Nothing is derived a second time here: this function does not consult the
// interaction configuration, because the plan already did - a valid plan exists only
// when the calibration was measured, so taking the thresholds from it carries that
// gate, and there is no configuration argument a caller could pass that would
// disagree with the plan it is asking about.
//
// A refused plan produces no evidence at all: its positions are zeros, and a zero
// would read as "the vehicle is past every threshold".
inline BottleEvidence deriveBottleEvidence(const RunExecutionPlan& plan, bool forwardLeg,
                                           double positionM, bool positionUsable) {
    if (!plan.valid) {
        return BottleEvidence{};
    }

    return deriveForwardProgression(
        forwardLeg, positionM, positionUsable,
        ForwardProgressionThresholds(plan.earliestContactMpPositionM,
                                     plan.captureGuaranteedMpPositionM,
                                     plan.forwardStopMpPositionM));
}

// The run layer's current execution plan.
//
// A holder, and not an assignment, because a plan is immutable: it has no default
// constructor, no setters and no assignment operator (every field is `const`), so
// "the run's current plan" cannot be produced by copying over a previous one.
// `adopt()` ends the previous plan's lifetime and begins the new one's in place,
// which is the same "one announcement, one plan" rule the type states. A plan owns
// no resources, so the replacement is a plain construct-in-place: no allocation, no
// ownership transfer, and no partially updated plan - a reader sees either the whole
// previous plan or the whole new one.
class RunExecutionPlanHolder {
public:
    // Before any announcement there is no plan, and that is stated rather than
    // implied by zeros.
    RunExecutionPlanHolder() : plan_(refusedExecutionPlan(RunExecutionPlanStatus::NotPlanned)) {}

    // A holder is the run layer's storage, not a value to copy around: copying one
    // would be a second, unnamed owner of the current plan.
    RunExecutionPlanHolder(const RunExecutionPlanHolder&) = delete;
    RunExecutionPlanHolder& operator=(const RunExecutionPlanHolder&) = delete;

    const RunExecutionPlan& plan() const { return plan_; }

    // Takes on one announcement's plan, ending the previous one. Refused plans are
    // adopted too: a refusal is a fact about the current run, and its named status
    // is what a diagnostic reports.
    void adopt(const RunExecutionPlan& plan) {
        plan_.~RunExecutionPlan();
        new (&plan_) RunExecutionPlan(plan);
    }

    // Back to "no announcement", for a new-run boundary.
    void clear() { adopt(refusedExecutionPlan(RunExecutionPlanStatus::NotPlanned)); }

private:
    RunExecutionPlan plan_;
};

}  // namespace ev
