#pragma once

#include "Motion.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Candidate distance-aware target-velocity shaping and timing intent
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One job: given how much of the travel is left, what the previous target was
//   and how much time passed, produce the target velocity the velocity
//   controller should regulate toward. It supplies a target; it commands
//   nothing.
// * Distance-aware tapering: the target is capped by the speed from which the
//   vehicle could still stop inside the remaining distance, using the caller's
//   deceleration limit and the caller's measured brake offset. As the remaining
//   distance shrinks the cap shrinks with it, so the target can only fall.
// * Optional strategy ceiling: the run strategy may place one extra ceiling on
//   the target magnitude for the phase it is in (`StrategyVelocityCeiling`,
//   supplied per cycle). It joins the desired-magnitude calculation *before* the
//   rate limits - desired = min(cruise ceiling, distance taper, strategy
//   ceiling) - so it can only tighten a target, never raise one, and a sudden
//   reduction is still ramped down at the deceleration limit instead of
//   stepping. An inactive ceiling changes nothing, and an active malformed one
//   refuses the step.
// * Rate-limited ramping: the target magnitude never changes faster than the
//   configured acceleration limit going up or the deceleration limit coming
//   down, regardless of how the inputs jump. A commanded reversal is ramped to
//   zero and holds there for at least one step before the sign can change - the
//   deliberate decelerate -> brake -> settle -> reverse sequence
//   (AGENTS.md section 15) starts with this, but the settle and the brake are
//   the run state machine's decisions, not this module's.
// * Target Time and an optional intentional delay carried explicitly, and a
//   motion duration that can be estimated from the same limit values. The
//   margin between the announced Target Time and that estimate is reported; it
//   is never acted on here. Deciding to spend slack, wait, or go faster is a run
//   strategy decision.
//
// What this module is not
//
// * It owns no limits. Cruise speed, acceleration limit, deceleration limit,
//   brake offset, the safety velocity ceiling and the sign polarity all arrive
//   as `Sourced<T>` values. Anything not `ValueSource::Measured` produces a
//   zero target rather than a shaped one - no measured limit exists yet, so no
//   shaping value is written down here - with one exception: the cruise may be
//   a cited `ValueSource::Derived` plan output (`cruiseProvenanceIsEstablished()`),
//   because a validated run timing model computes it from measured inputs (D35).
//   The optional strategy ceiling is the module's one non-`Sourced` input: it is
//   a run-strategy choice, conveyed explicitly as active or inactive, and the
//   phase configuration that produces it (`RunPhaseConfig`) is where its
//   provenance is validated.
// * It has no friction, motor, driver or battery model. The only physics in it
//   are the kinematic relations between distance, speed, acceleration and time,
//   all evaluated with caller-supplied limits.
// * It is not a position controller and it has no final-position authority. It
//   does not decide that the target point has been reached, does not brake, does
//   not stop the vehicle and does not latch a direction. `WithinBrakeOffset` is
//   a statement about the distance it was handed, not a claim about where the
//   vehicle is.
// * A zero target is not a stop. It means no powered effort is being asked for,
//   and the run layer that owns the stop must command its own deliberate
//   decelerate/settle/brake sequence. Nothing here assumes the vehicle actually
//   followed the taper: a taper that reaches zero is a target, and the distance
//   the vehicle covers after it is the brake offset that a physical measurement
//   still has to establish (`VERIFIED_STATE.md` #9).
// * It declares no physical direction either. It takes the caller's
//   `MotionDirection` and converts it to a command sign through
//   `positiveCommandDirection`, which is the UNVERIFIED polarity fact
//   (VERIFIED_STATE.md #4) and must be measured before it may shape anything.
// * The estimated duration is not evidence about the vehicle. It is arithmetic
//   on configured limits, and it will be wrong until those limits come from
//   measured runs.

// ---------------------------------------------------------------------------
// Timing intent
// ---------------------------------------------------------------------------
//
// The announced Target Time and any deliberate delay the run intends to spend,
// made explicit rather than buried in sequencing code. Both are inputs; neither
// is enforced here.
//
// This type, together with `MotionProfileInputs` (the distance and elapsed-time
// side), is the single owner of a run's target/time contract. `MotionRequest`
// in Motion.h is vocabulary only and must not be wired into a run; nothing else
// may carry a second, competing time budget for the same travel.
struct MotionTimingIntent {
    constexpr MotionTimingIntent(double targetTimeS_, double intentionalDelayS_)
        : targetTimeS(targetTimeS_), intentionalDelayS(intentionalDelayS_) {}

    // The event's Target Time, seconds (rules 8.i, 10.0-20.0 s), taken from
    // RunConfig. Range and grid validation stays in validateRunConfig(); this
    // type only carries the value.
    double targetTimeS;

    // Extra time the run deliberately spends that is not travel: a settle wait,
    // a pause before release. 0 means none.
    double intentionalDelayS;
};

enum class TimingIntentError : uint8_t {
    None = 0,
    NonFiniteValue,
    TargetTimeNotPositive,
    DelayNegative,
};

struct TimingIntentValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    TimingIntentError error;
};

inline TimingIntentValidation validateMotionTimingIntent(const MotionTimingIntent& intent) {
    if (!std::isfinite(intent.targetTimeS) || !std::isfinite(intent.intentionalDelayS)) {
        return {false, TimingIntentError::NonFiniteValue};
    }
    if (!(intent.targetTimeS > 0.0)) {
        return {false, TimingIntentError::TargetTimeNotPositive};
    }
    if (intent.intentionalDelayS < 0.0) {
        return {false, TimingIntentError::DelayNegative};
    }
    return {true, TimingIntentError::None};
}

// Why an estimated motion duration does not exist.
enum class MotionDurationStatus : uint8_t {
    Valid = 0,
    DistanceNotFinite,
    DistanceIsNegative,
    LimitsAreNotUsable,   // not Measured, not finite, or not positive
    DurationIsNotFinite,  // the arithmetic overflowed
};

// An estimate of how long a travel takes under the supplied limits, from
// kinematics only. There is no default constructor: an estimate with no stated
// reason behind it must be impossible to write.
struct MotionDurationEstimate {
    constexpr MotionDurationEstimate(double durationS_, bool valid_, MotionDurationStatus status_)
        : durationS(durationS_), valid(valid_), status(status_) {}

    // Seconds. 0.0 when no estimate exists.
    double durationS;

    // Invariant: valid == (status == Valid).
    bool valid;

    MotionDurationStatus status;
};

// The time to travel `distanceM` under a trapezoidal (or, when the distance is
// too short to reach cruise speed, triangular) profile, from kinematics alone and
// with no provenance: the arithmetic `motionDurationS` and the diagnostic run
// timing estimate below both use this one formula, so "how long does this
// distance take" has a single answer in this module.
//
// Returns NaN when no duration exists: a non-finite or non-positive limit, a
// non-finite or negative distance, or an arithmetic result that is not finite.
// A caller that needs to report *why* there is no duration states the reason
// itself; this function only answers when it can.
inline double travelDurationS(double distanceM, double cruiseMps, double accelLimitMps2,
                              double decelLimitMps2) {
    if (!std::isfinite(distanceM) || distanceM < 0.0 || !std::isfinite(cruiseMps) ||
        !std::isfinite(accelLimitMps2) || !std::isfinite(decelLimitMps2) || !(cruiseMps > 0.0) ||
        !(accelLimitMps2 > 0.0) || !(decelLimitMps2 > 0.0)) {
        return std::nan("");
    }
    if (distanceM == 0.0) {
        return 0.0;
    }

    const double accelDistanceM = cruiseMps * cruiseMps / (2.0 * accelLimitMps2);
    const double decelDistanceM = cruiseMps * cruiseMps / (2.0 * decelLimitMps2);

    double durationS = 0.0;
    if (accelDistanceM + decelDistanceM <= distanceM) {
        const double cruiseDistanceM = distanceM - accelDistanceM - decelDistanceM;
        durationS = cruiseMps / accelLimitMps2 + cruiseDistanceM / cruiseMps +
                    cruiseMps / decelLimitMps2;
    } else {
        // Triangular profile: the peak speed solves
        // distance = v^2/(2a) + v^2/(2d).
        const double peakVelocityMps =
            std::sqrt(2.0 * distanceM /
                      (1.0 / accelLimitMps2 + 1.0 / decelLimitMps2));
        durationS = peakVelocityMps / accelLimitMps2 + peakVelocityMps / decelLimitMps2;
    }
    return std::isfinite(durationS) ? durationS : std::nan("");
}

// Time to travel `distanceM` with a trapezoidal speed profile: accelerate at
// `accelLimitMps2` to the cruise speed, cruise, decelerate at `decelLimitMps2`
// to rest. When the distance is too short to reach cruise speed, the triangular
// profile is used instead.
//
// The cruise value passed in is the speed actually to be used, so the caller is
// responsible for having capped it by the safety velocity ceiling. The brake
// offset is deliberately not subtracted here: this answers "how long does this
// distance take", and the intentional delay is added by the caller or by
// motionTimingSummary(). A non-measured limit or a non-finite or non-positive
// limit produces no estimate at all.
inline MotionDurationEstimate motionDurationS(double distanceM,
                                              const Sourced<double>& cruiseVelocityMps,
                                              const Sourced<double>& accelLimitMps2,
                                              const Sourced<double>& decelLimitMps2) {
    if (!std::isfinite(distanceM)) {
        return {0.0, false, MotionDurationStatus::DistanceNotFinite};
    }
    if (distanceM < 0.0) {
        return {0.0, false, MotionDurationStatus::DistanceIsNegative};
    }
    if (!cruiseVelocityMps.isMeasured() || !accelLimitMps2.isMeasured() ||
        !decelLimitMps2.isMeasured()) {
        return {0.0, false, MotionDurationStatus::LimitsAreNotUsable};
    }
    if (!std::isfinite(cruiseVelocityMps.value) || !std::isfinite(accelLimitMps2.value) ||
        !std::isfinite(decelLimitMps2.value) || !(cruiseVelocityMps.value > 0.0) ||
        !(accelLimitMps2.value > 0.0) || !(decelLimitMps2.value > 0.0)) {
        return {0.0, false, MotionDurationStatus::LimitsAreNotUsable};
    }

    const double durationS = travelDurationS(distanceM, cruiseVelocityMps.value, accelLimitMps2.value,
                                             decelLimitMps2.value);
    if (!std::isfinite(durationS)) {
        return {0.0, false, MotionDurationStatus::DurationIsNotFinite};
    }
    return {durationS, true, MotionDurationStatus::Valid};
}

// Why a timing summary does not exist.
enum class MotionTimingStatus : uint8_t {
    Valid = 0,
    InvalidTimingIntent,
    InvalidDistance,
    LimitsAreNotUsable,
    DurationIsNotFinite,
};

// The reported relationship between the announced Target Time and what the
// configured limits imply. Deliberately no default constructor.
struct MotionTimingSummary {
    constexpr MotionTimingSummary(double travelDurationS_, double intendedTotalS_, double marginS_,
                                  bool valid_, MotionTimingStatus status_)
        : travelDurationS(travelDurationS_),
          intendedTotalS(intendedTotalS_),
          marginS(marginS_),
          valid(valid_),
          status(status_) {}

    // Estimated travel time under the supplied limits, seconds: the trapezoid
    // over the whole distance, with the brake offset deliberately not subtracted
    // (see motionDurationS). The consequence belongs next to the number, because
    // the travel taper DOES subtract that offset: a vehicle following the taper
    // reaches a zero target about brakeOffsetM / cruiseVelocityMps seconds before
    // this value and still has the offset to cover under whatever brake, settle or
    // coast the run layer commands. This estimate describes the whole travel and
    // is intentionally not adjusted for either half of that; it is not a
    // prediction of when the vehicle stops.
    double travelDurationS;

    // travelDurationS plus the intentional delay, seconds.
    double intendedTotalS;

    // targetTimeS - intendedTotalS, seconds. Positive means the run intends to
    // finish before the announced Target Time. The sign of this number is the
    // only thing this module reports about timing strategy, and nothing acts on
    // it here.
    double marginS;

    // Invariant: valid == (status == Valid).
    bool valid;

    MotionTimingStatus status;
};

// Combines the announced timing intent with the estimated travel duration. All
// three outputs are 0.0 when the summary is not valid.
inline MotionTimingSummary motionTimingSummary(const MotionTimingIntent& intent, double distanceM,
                                               const Sourced<double>& cruiseVelocityMps,
                                               const Sourced<double>& accelLimitMps2,
                                               const Sourced<double>& decelLimitMps2) {
    if (!validateMotionTimingIntent(intent).ok) {
        return {0.0, 0.0, 0.0, false, MotionTimingStatus::InvalidTimingIntent};
    }
    if (!std::isfinite(distanceM) || distanceM < 0.0) {
        return {0.0, 0.0, 0.0, false, MotionTimingStatus::InvalidDistance};
    }
    const MotionDurationEstimate duration =
        motionDurationS(distanceM, cruiseVelocityMps, accelLimitMps2, decelLimitMps2);
    switch (duration.status) {
        case MotionDurationStatus::Valid:
            break;
        case MotionDurationStatus::DistanceNotFinite:
        case MotionDurationStatus::DistanceIsNegative:
            return {0.0, 0.0, 0.0, false, MotionTimingStatus::InvalidDistance};
        case MotionDurationStatus::LimitsAreNotUsable:
            return {0.0, 0.0, 0.0, false, MotionTimingStatus::LimitsAreNotUsable};
        case MotionDurationStatus::DurationIsNotFinite:
            return {0.0, 0.0, 0.0, false, MotionTimingStatus::DurationIsNotFinite};
    }
    const double intendedTotalS = duration.durationS + intent.intentionalDelayS;
    return {duration.durationS, intendedTotalS, intent.targetTimeS - intendedTotalS, true,
            MotionTimingStatus::Valid};
}

// ---------------------------------------------------------------------------
// The diagnostic run timing estimate - what an announced Target Time implies
// ---------------------------------------------------------------------------
//
// The announced Target Time is a *scored* event parameter (rule 8.i): a run that
// finishes far from it scores worse than one that lands on it, and the run may
// not exceed the configured safety limits to chase it. This is the one kinematic
// arithmetic that answers what an announced time would need, and it answers
// three questions at once:
//
//   1. **Which cruise speed?** The slowest cruise whose estimated travel fits
//      inside the announced time, so the run spends its slack by covering the
//      announced distance at a controlled speed instead of by crawling below the
//      speed at which the drivetrain is known to move.
//   2. **How much deliberate stationary delay?** Whatever the time budget has
//      left after the travel and the run's non-travel reserve, bounded by the
//      configured maximum delay and by the configured maximum run duration. This
//      is what stops a generous announced time from being "finished early":
//      instead of asking the drivetrain to travel absurdly slowly, the run waits
//      and then travels.
//   3. **Is the announcement possible at all?** A Target Time shorter than the
//      fastest admissible travel cannot be met without exceeding a limit, so it
//      is refused rather than silently violated. The refusal names the reason.
//
// What this is not: it is not a measurement, it is not a controller, and it
// commands nothing. The numbers are kinematic arithmetic on caller-supplied
// limits, the same way `motionTimingSummary()` is, and they are as good as those
// limits - which are placeholders until a physical test replaces them.
//
// **It is diagnostic until a physical timing model is validated (S7).** An
// unvalidated estimate must not control a run: it may not choose the run's
// cruise, its `plannedDelayS` may not become the run's deliberate delay, and it
// may not refuse an otherwise legal announcement. When a run's layer lets it
// influence a run at all, the value it uses is recorded in that run's immutable
// execution plan (`RunExecutionPlan::initialDelayS`); today no production run
// does, and every production flag behind that decision is false.
//
// Every limit arrives as a `Sourced` value and a value that is not `Measured`
// refuses the plan rather than shaping one; the announced Target Time is the one
// exception, because it is an announced *rule* parameter and is carried as
// `RuleDerived` with the rule cited (`RunConfig.targetTimeS`, rule 8.i).

// Why no plan exists, or that one does. The zero value is `Planned`, so a plan
// that was never produced cannot default into a valid-looking one: there is no
// default constructor below, and `refusedTimingPlan()` is how "no plan yet" is
// stated.
enum class RunTimingPlanStatus : uint8_t {
    Planned = 0,                 // a plan exists
    NotPlanned = 1,              // no run has been announced, so there is nothing to plan
    InvalidTargetTime,           // the announced Target Time is not a positive finite number
    InvalidGeometry,             // the announced travel distance is not a positive finite number
    LimitsAreNotUsable,          // a limit is not Measured, or is malformed
    TargetTimeTooShort,          // the fastest admissible travel cannot fit inside the Target Time
    PlanExceedsMaxRunDuration,   // travel + reserve + delay exceeds the configured maximum run duration
};

inline const char* runTimingPlanStatusName(RunTimingPlanStatus status) {
    switch (status) {
        case RunTimingPlanStatus::Planned:
            return "Planned";
        case RunTimingPlanStatus::NotPlanned:
            return "NotPlanned";
        case RunTimingPlanStatus::InvalidTargetTime:
            return "InvalidTargetTime";
        case RunTimingPlanStatus::InvalidGeometry:
            return "InvalidGeometry";
        case RunTimingPlanStatus::LimitsAreNotUsable:
            return "LimitsAreNotUsable";
        case RunTimingPlanStatus::TargetTimeTooShort:
            return "TargetTimeTooShort";
        case RunTimingPlanStatus::PlanExceedsMaxRunDuration:
            return "PlanExceedsMaxRunDuration";
    }
    return "?";
}

// The diagnostic kinematic estimate for one announced run. Deliberately no
// default constructor: an estimate with no announcement behind it must be
// impossible to write.
struct RunTimingPlan {
    constexpr RunTimingPlan(double plannedCruiseMps_, double plannedDelayS_, double travelDurationS_,
                            double reserveS_, double intendedTotalS_, double marginS_, bool valid_,
                            RunTimingPlanStatus status_)
        : plannedCruiseMps(plannedCruiseMps_),
          plannedDelayS(plannedDelayS_),
          travelDurationS(travelDurationS_),
          reserveS(reserveS_),
          intendedTotalS(intendedTotalS_),
          marginS(marginS_),
          valid(valid_),
          status(status_) {}

    // The cruise speed the estimate says the travel would need, metres per
    // second. 0.0 when no estimate exists. Diagnostic: it is the run's shaped
    // cruise only when a validated timing model says so.
    double plannedCruiseMps;

    // The deliberate stationary delay the estimate says the announced time has
    // left over, seconds. 0.0 when the announced time needs no delay (or when no
    // estimate exists). Diagnostic in the same sense as the cruise: the run's
    // own delay is its execution plan's `initialDelayS`.
    double plannedDelayS;

    // Estimated travel time at `plannedCruiseMps`, seconds, from `travelDurationS()`:
    // the same arithmetic `motionDurationS()` reports, for the same distance.
    double travelDurationS;

    // The non-travel time the plan sets aside inside the run's own time - the
    // reversal stop's dwell, which is inside Run Time (the final Complete Stop is
    // not, rule 8.p) - seconds. It is a caller-supplied reserve, not an estimate
    // this module makes.
    double reserveS;

    // travelDurationS + reserveS + plannedDelayS, seconds.
    double intendedTotalS;

    // TargetTime - intendedTotalS, seconds: how much of the announced time the
    // plan does not spend. Never negative on a valid plan.
    double marginS;

    // Invariant: valid == (status == Planned).
    bool valid;

    RunTimingPlanStatus status;
};

// What the plan is built from. No default constructor: a plan must state what it
// was planned from.
struct RunTimingPlanInputs {
    constexpr RunTimingPlanInputs(double travelDistanceM_, Sourced<double> targetTimeS_,
                                  Sourced<double> cruiseCeilingMps_,
                                  Sourced<double> velocityCeilingMps_,
                                  Sourced<double> accelLimitMps2_,
                                  Sourced<double> decelLimitMps2_,
                                  Sourced<double> minimumCruiseMps_,
                                  Sourced<double> nonTravelReserveS_,
                                  Sourced<double> maxRunDurationS_)
        : travelDistanceM(travelDistanceM_),
          targetTimeS(targetTimeS_),
          cruiseCeilingMps(cruiseCeilingMps_),
          velocityCeilingMps(velocityCeilingMps_),
          accelLimitMps2(accelLimitMps2_),
          decelLimitMps2(decelLimitMps2_),
          minimumCruiseMps(minimumCruiseMps_),
          nonTravelReserveS(nonTravelReserveS_),
          maxRunDurationS(maxRunDurationS_) {}

    // The whole travel the plan covers, metres: the run layer's announced
    // distance (`ev::plannedTravelDistanceM()` in RunAxis.h).
    double travelDistanceM;

    // The announced Target Time (rules 8.i). RuleDerived, with the rule cited.
    Sourced<double> targetTimeS;

    // The cruise speed the strategy would like to use, and the safety ceiling for
    // the travel. The lower of the two is the planning ceiling.
    Sourced<double> cruiseCeilingMps;
    Sourced<double> velocityCeilingMps;

    // Rate limits the travel must respect, metres per second squared.
    Sourced<double> accelLimitMps2;
    Sourced<double> decelLimitMps2;

    // The slowest cruise at which travel is known to be reliable, metres per
    // second. The plan does not go below it: a speed the drivetrain cannot follow
    // is not a way to spend time, and the remaining slack becomes delay instead.
    Sourced<double> minimumCruiseMps;

    // The run's non-travel time inside Run Time, seconds (the reversal stop's
    // dwell). Excluded from Run Time by the rules: the final Complete Stop.
    Sourced<double> nonTravelReserveS;

    // The configured maximum run duration, seconds. The planned total may not
    // exceed it, so an announcement that would need an unreasonable wait is
    // refused rather than planned.
    Sourced<double> maxRunDurationS;
};

// The plan with no announcement behind it: what a run layer holds before a run
// is announced, and what it returns to at a new-run boundary.
inline RunTimingPlan refusedTimingPlan(RunTimingPlanStatus status) {
    return RunTimingPlan(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, status);
}

// Builds the diagnostic estimate for one announced run.
//
// The algebra, and why it is exact rather than searched: for a cruise speed at
// or below the peak of the triangular profile, the travel time is
// `d/v + v*(1/(2a) + 1/(2d))`, which is decreasing in `v`. Setting it equal to
// the travel budget `B` gives `k*v^2 - B*v + d = 0` with `k = 1/(2a) + 1/(2d)`,
// whose smaller root is the *slowest* speed that still fits inside `B` - the
// whole point of the plan, because slower is the safe direction and the ceiling
// is what the run may not exceed. `B < 2*sqrt(k*d)` means even the triangular
// profile is longer than the budget, i.e. no admissible speed fits.
inline RunTimingPlan planRunTiming(const RunTimingPlanInputs& inputs) {
    const double cruiseCeilingMps = inputs.cruiseCeilingMps.value < inputs.velocityCeilingMps.value
                                        ? inputs.cruiseCeilingMps.value
                                        : inputs.velocityCeilingMps.value;

    if (!std::isfinite(inputs.targetTimeS.value) || !(inputs.targetTimeS.value > 0.0)) {
        return refusedTimingPlan(RunTimingPlanStatus::InvalidTargetTime);
    }
    if (!std::isfinite(inputs.travelDistanceM) || !(inputs.travelDistanceM > 0.0)) {
        return refusedTimingPlan(RunTimingPlanStatus::InvalidGeometry);
    }
    if (!inputs.cruiseCeilingMps.isMeasured() || !inputs.velocityCeilingMps.isMeasured() ||
        !inputs.accelLimitMps2.isMeasured() || !inputs.decelLimitMps2.isMeasured() ||
        !inputs.minimumCruiseMps.isMeasured() || !inputs.nonTravelReserveS.isMeasured() ||
        !inputs.maxRunDurationS.isMeasured()) {
        return refusedTimingPlan(RunTimingPlanStatus::LimitsAreNotUsable);
    }

    const double accelMps2 = inputs.accelLimitMps2.value;
    const double decelMps2 = inputs.decelLimitMps2.value;
    const double reserveS = inputs.nonTravelReserveS.value;
    const double minimumCruiseMps = inputs.minimumCruiseMps.value;
    const double maxRunDurationS = inputs.maxRunDurationS.value;
    if (!std::isfinite(cruiseCeilingMps) || !(cruiseCeilingMps > 0.0) ||
        !std::isfinite(accelMps2) || !(accelMps2 > 0.0) || !std::isfinite(decelMps2) ||
        !(decelMps2 > 0.0) || !std::isfinite(minimumCruiseMps) || !(minimumCruiseMps > 0.0) ||
        !std::isfinite(reserveS) || reserveS < 0.0 || !std::isfinite(maxRunDurationS) ||
        !(maxRunDurationS > 0.0)) {
        return refusedTimingPlan(RunTimingPlanStatus::LimitsAreNotUsable);
    }
    // A cruise floor above the planning ceiling is an inconsistent configuration,
    // not something to clamp: the floor would silently become the ceiling and the
    // run would travel at the safety limit.
    if (minimumCruiseMps > cruiseCeilingMps) {
        return refusedTimingPlan(RunTimingPlanStatus::LimitsAreNotUsable);
    }

    // Feasibility, stated with the speed the run may actually reach: a Target Time
    // the fastest admissible travel cannot fit is refused, never met by exceeding
    // a limit.
    const double fastestTravelS =
        travelDurationS(inputs.travelDistanceM, cruiseCeilingMps, accelMps2, decelMps2);
    if (!std::isfinite(fastestTravelS) || inputs.targetTimeS.value < fastestTravelS + reserveS) {
        return refusedTimingPlan(RunTimingPlanStatus::TargetTimeTooShort);
    }

    const double travelBudgetS = inputs.targetTimeS.value - reserveS;
    const double k = 0.5 / accelMps2 + 0.5 / decelMps2;
    const double discriminant =
        travelBudgetS * travelBudgetS - 4.0 * k * inputs.travelDistanceM;
    if (!std::isfinite(discriminant) || discriminant < 0.0) {
        // The budget is shorter than the shortest profile there is.
        return refusedTimingPlan(RunTimingPlanStatus::TargetTimeTooShort);
    }

    const double requiredCruiseMps =
        (travelBudgetS - std::sqrt(discriminant)) / (2.0 * k);
    const double plannedCruiseMps =
        requiredCruiseMps > minimumCruiseMps ? requiredCruiseMps : minimumCruiseMps;
    if (!std::isfinite(plannedCruiseMps) || plannedCruiseMps > cruiseCeilingMps) {
        return refusedTimingPlan(RunTimingPlanStatus::TargetTimeTooShort);
    }

    const double travelS =
        travelDurationS(inputs.travelDistanceM, plannedCruiseMps, accelMps2, decelMps2);
    if (!std::isfinite(travelS)) {
        return refusedTimingPlan(RunTimingPlanStatus::LimitsAreNotUsable);
    }

    // Whatever the budget has left after the travel is spent waiting, not by
    // crawling below the floor. It is never negative: the chosen cruise fits the
    // budget by construction.
    double plannedDelayS = inputs.targetTimeS.value - (travelS + reserveS);
    if (!(plannedDelayS > 0.0)) {
        plannedDelayS = 0.0;
    }

    const double intendedTotalS = travelS + reserveS + plannedDelayS;
    if (intendedTotalS > maxRunDurationS) {
        return refusedTimingPlan(RunTimingPlanStatus::PlanExceedsMaxRunDuration);
    }

    // The margin is reported at zero rather than at a negative tail. The chosen
    // cruise fits the budget by construction - it is solved for equality and the
    // feasibility check above already proved the ceiling fits - so a negative
    // margin could only be the last bits of a double, and a caller deciding "is
    // this run going to be late" must not see one.
    double marginS = inputs.targetTimeS.value - intendedTotalS;
    if (marginS < 0.0) {
        marginS = 0.0;
    }

    return RunTimingPlan(plannedCruiseMps, plannedDelayS, travelS, reserveS, intendedTotalS, marginS,
                         true, RunTimingPlanStatus::Planned);
}

// ---------------------------------------------------------------------------
// Target-velocity shaping
// ---------------------------------------------------------------------------

// What the run is asking the profile for.
enum class MotionProfileIntent : uint8_t {
    Travel = 0,  // move toward the target point in `direction`
    Stop = 1,    // bring the target velocity to zero, deliberately ramped
};

// The shaping limits for one travel. Every field is supplied by the caller for
// the direction being travelled: the forward and reverse brake offsets differ,
// and so do the two safety velocity ceilings, so the caller selects which pair
// applies instead of this module assuming one.
struct MotionProfileLimits {
    constexpr MotionProfileLimits(Sourced<double> cruiseVelocityMps_,
                                  Sourced<double> velocityCeilingMps_,
                                  Sourced<double> accelLimitMps2_,
                                  Sourced<double> decelLimitMps2_,
                                  Sourced<double> brakeOffsetM_,
                                  Sourced<MotionDirection> positiveCommandDirection_)
        : cruiseVelocityMps(cruiseVelocityMps_),
          velocityCeilingMps(velocityCeilingMps_),
          accelLimitMps2(accelLimitMps2_),
          decelLimitMps2(decelLimitMps2_),
          brakeOffsetM(brakeOffsetM_),
          positiveCommandDirection(positiveCommandDirection_) {}

    // Desired cruise speed, metres per second. Not a measured value yet.
    Sourced<double> cruiseVelocityMps;

    // Safety velocity ceiling for this direction, metres per second, taken from
    // the measured SafetyLimits. The lower of cruiseVelocityMps and this is the
    // cruise ceiling actually used.
    Sourced<double> velocityCeilingMps;

    // Magnitude of the positive acceleration limit, metres per second squared.
    Sourced<double> accelLimitMps2;

    // Magnitude of the deceleration limit, metres per second squared.
    Sourced<double> decelLimitMps2;

    // Measured extra travel after commanding a stop in this direction, metres
    // (VehicleCalibration::forwardBrakeOffsetM or reverseBrakeOffsetM). The
    // taper treats the last brakeOffsetM of the travel as already spent.
    Sourced<double> brakeOffsetM;

    // Which physical direction a positive command sign drives in. This is the
    // UNVERIFIED polarity fact (VERIFIED_STATE.md #4); a Provisional value stops
    // the profile from shaping anything.
    Sourced<MotionDirection> positiveCommandDirection;
};

// The brake offset that belongs to the direction being travelled, chosen from an
// explicit forward/reverse pair.
//
// This is the selection `src/VehicleConfig.h::profileLimitsForOneDirection()`
// makes when it builds one leg's limits, extracted here so a host test can prove
// it with a deliberately unequal fixture pair: the planned forward and reverse
// offsets are currently numerically equal, so a test restricted to the production
// constants could not tell a direction-specific selection from one that uses the
// same offset for both legs - the defect that once existed in the builder. The
// chosen value passes through untouched, provenance included; the selection is a
// function of the direction alone.
inline Sourced<double> brakeOffsetForDirection(MotionDirection direction,
                                               Sourced<double> forwardBrakeOffsetM,
                                               Sourced<double> reverseBrakeOffsetM) {
    return direction == MotionDirection::Forward ? forwardBrakeOffsetM
                                                 : reverseBrakeOffsetM;
}

// The forward and reverse limit sets for the two legs of a run, selected by the
// direction being travelled.
//
// The legs carry different measured values - the forward and reverse brake
// offsets differ, and so do the two safety velocity ceilings - so a caller that
// packs one leg's values by hand can silently attach the forward offset to a
// reverse leg, and nothing would notice: the profile is handed one set of limits
// and cannot know which leg it describes. This holder makes the selection a
// function of `MotionDirection`: build it once per run, then ask for the
// direction being travelled.
//
// The polarity field is a per-leg copy of the same global fact; a caller that
// builds both legs from the same measured `positiveCommandDirection` keeps them
// consistent.
struct DirectionalMotionProfileLimits {
    constexpr DirectionalMotionProfileLimits(MotionProfileLimits forward_,
                                             MotionProfileLimits reverse_)
        : forward(forward_), reverse(reverse_) {}

    // Limits for travel in MotionDirection::Forward / MotionDirection::Reverse.
    MotionProfileLimits forward;
    MotionProfileLimits reverse;

    const MotionProfileLimits& forDirection(MotionDirection direction) const {
        return direction == MotionDirection::Forward ? forward : reverse;
    }
};

enum class MotionProfileLimitsError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,  // cruise not Measured/cited-Derived, or another limit not Measured
    UnknownPositiveCommandDirection,  // a cast-in value that names no physical direction
    NonFiniteValue,
    NonPositiveLimit,               // cruise, ceiling, accel or decel not > 0
    NegativeBrakeOffset,
    CruiseExceedsVelocityCeiling,   // the desired cruise is above the measured ceiling
};

struct MotionProfileLimitsValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    MotionProfileLimitsError error;
};

// True when a cruise may shape a target: measured on this vehicle, or computed
// by a validated run timing model from measured inputs and cited as such
// (`ValueSource::Derived` with evidence). Mirrors
// `dwellProvenanceIsEstablished()` and `optionalDelayProvenanceIsEstablished()`:
// a computed number is never relabelled Measured, and an uncited Derived value
// is refused exactly like a placeholder. All other limits remain Measured-only.
inline bool cruiseProvenanceIsEstablished(const Sourced<double>& cruise) {
    if (cruise.isMeasured()) {
        return true;
    }
    return cruise.source == ValueSource::Derived && cruise.citesEvidence();
}

inline MotionProfileLimitsValidation validateMotionProfileLimits(
    const MotionProfileLimits& limits) {
    if (!cruiseProvenanceIsEstablished(limits.cruiseVelocityMps) ||
        !limits.velocityCeilingMps.isMeasured() ||
        !limits.accelLimitMps2.isMeasured() || !limits.decelLimitMps2.isMeasured() ||
        !limits.brakeOffsetM.isMeasured() || !limits.positiveCommandDirection.isMeasured()) {
        return {false, MotionProfileLimitsError::ValuesAreNotMeasured};
    }
    // A direction encoding that names no physical direction is malformed, not a
    // polarity: it must be refused rather than folded into Reverse by the
    // agreement checks (S18A/P5).
    if (!isKnownMotionDirection(limits.positiveCommandDirection.value)) {
        return {false, MotionProfileLimitsError::UnknownPositiveCommandDirection};
    }
    if (!std::isfinite(limits.cruiseVelocityMps.value) ||
        !std::isfinite(limits.velocityCeilingMps.value) ||
        !std::isfinite(limits.accelLimitMps2.value) ||
        !std::isfinite(limits.decelLimitMps2.value) || !std::isfinite(limits.brakeOffsetM.value)) {
        return {false, MotionProfileLimitsError::NonFiniteValue};
    }
    if (!(limits.cruiseVelocityMps.value > 0.0) || !(limits.velocityCeilingMps.value > 0.0) ||
        !(limits.accelLimitMps2.value > 0.0) || !(limits.decelLimitMps2.value > 0.0)) {
        return {false, MotionProfileLimitsError::NonPositiveLimit};
    }
    if (limits.brakeOffsetM.value < 0.0) {
        return {false, MotionProfileLimitsError::NegativeBrakeOffset};
    }
    // A cruise above the measured ceiling is a configuration mistake, not
    // something to silently clamp: it would otherwise look like the ceiling was
    // measured to be the cruise speed.
    if (limits.cruiseVelocityMps.value > limits.velocityCeilingMps.value) {
        return {false, MotionProfileLimitsError::CruiseExceedsVelocityCeiling};
    }
    return {true, MotionProfileLimitsError::None};
}

// ---------------------------------------------------------------------------
// The optional strategy ceiling
// ---------------------------------------------------------------------------
//
// Competition strategy may want a phase of the run - the bottle approach, the
// capture, the push - travelled slower than whatever the cruise ceiling and the
// distance taper currently ask for. That is one number for the cycle, and it
// belongs to the shaping input rather than to the limits: the limits describe the
// vehicle and are selected by direction, while a phase ceiling is a run-strategy
// choice that changes as the run advances through its phases.
//
// It is deliberately explicit about being optional. `active == false` is the
// value that changes nothing, and it is stated rather than implied: a ceiling of
// zero is a legitimate ACTIVE value (it asks for no shaped magnitude at all), so
// "zero" cannot double as "no ceiling". There is no default constructor and no
// silent fallback.
//
// The shaper's contract: an active ceiling participates in the desired magnitude
// BEFORE the acceleration/deceleration rate limits - desired = min(cruise
// ceiling, distance taper, strategy ceiling) - so it can only tighten a target
// and can never raise one, and a sudden reduction is a desired value the existing
// deceleration limit ramps down instead of a target step. An ACTIVE ceiling that
// is not a finite nonnegative number fails closed: the shaping step is refused
// (zero target) rather than shaped as if the ceiling were absent.
struct StrategyVelocityCeiling {
    constexpr StrategyVelocityCeiling(bool active_, double velocityMps_)
        : active(active_), velocityMps(velocityMps_) {}

    // False means no ceiling is supplied this cycle and shaping is untouched.
    bool active;

    // The ceiling, metres per second, when active. An inactive ceiling carries
    // exactly 0.0, so an accidental read cannot look like a stated ceiling.
    double velocityMps;
};

// No default constructor: whether a phase is slowed is a decision the caller
// states, not something a type may assume.
static_assert(!std::is_default_constructible<StrategyVelocityCeiling>::value,
              "a strategy ceiling must state whether it is active");

// The ceiling that changes nothing: no strategy limit is in force this cycle.
inline constexpr StrategyVelocityCeiling inactiveStrategyVelocityCeiling() {
    return StrategyVelocityCeiling(false, 0.0);
}

// A ceiling of `velocityMps` metres per second. Zero is legal (it asks for no
// shaped magnitude); a negative or non-finite value is caught by
// `validateStrategyVelocityCeiling()` when the shaper runs.
inline constexpr StrategyVelocityCeiling activeStrategyVelocityCeiling(double velocityMps) {
    return StrategyVelocityCeiling(true, velocityMps);
}

enum class StrategyVelocityCeilingError : uint8_t {
    None = 0,
    NonFiniteValue,  // active and NaN or infinite
    NegativeValue,   // active and below zero
};

struct StrategyVelocityCeilingValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    StrategyVelocityCeilingError error;
};

// Inactive is always valid: it is the absence of a ceiling, not a ceiling of
// zero. An active ceiling must be a finite number that is not negative; a
// nonnegative value including zero is legal.
inline StrategyVelocityCeilingValidation validateStrategyVelocityCeiling(
    const StrategyVelocityCeiling& ceiling) {
    if (!ceiling.active) {
        return {true, StrategyVelocityCeilingError::None};
    }
    if (!std::isfinite(ceiling.velocityMps)) {
        return {false, StrategyVelocityCeilingError::NonFiniteValue};
    }
    if (ceiling.velocityMps < 0.0) {
        return {false, StrategyVelocityCeilingError::NegativeValue};
    }
    return {true, StrategyVelocityCeilingError::None};
}

// Per-cycle inputs. No default constructor: an unstated travel is not a travel.
struct MotionProfileInputs {
    constexpr MotionProfileInputs(MotionProfileIntent intent_, MotionDirection direction_,
                                  double remainingDistanceM_, double currentTargetVelocityMps_,
                                  uint32_t dtMs_,
                                  StrategyVelocityCeiling strategyCeiling_ =
                                      inactiveStrategyVelocityCeiling())
        : intent(intent_),
          direction(direction_),
          remainingDistanceM(remainingDistanceM_),
          currentTargetVelocityMps(currentTargetVelocityMps_),
          dtMs(dtMs_),
          strategyCeiling(strategyCeiling_) {}

    // What the run is asking for this cycle.
    MotionProfileIntent intent;

    // Physical direction of the travel being shaped.
    MotionDirection direction;

    // Distance still to travel to the target point, metres, from the run's
    // distance estimate. This is a magnitude on the run axis: it does not
    // inherit the encoder's tick-counting convention, so the vehicle layer must
    // reconcile run-axis distances with tick-convention travel through the
    // measured polarity once - the reconciliation `positionFromOriginM()` in
    // Odometry.h documents - rather than per leg, or the forward and reverse
    // legs will disagree about which sign increases position. This module does
    // not decide what "remaining" means or whether the estimate behind it is
    // valid.
    double remainingDistanceM;

    // The target velocity this module produced last cycle, metres per second,
    // signed in the command convention. 0.0 at the start of a travel. The
    // caller-held side of this handoff is `MotionProfileTarget` below; do not
    // carry this value by hand across a run boundary.
    double currentTargetVelocityMps;

    // Elapsed time since the previous call, milliseconds. 0 means no time
    // passed and the target is held unchanged, with one exception
    // (`TimeDidNotAdvance`): when the requested direction contradicts the
    // current target's sign, the hold collapses to exactly zero instead of
    // handing back a target in the direction the run has just left.
    uint32_t dtMs;

    // The optional strategy ceiling for this cycle (`StrategyVelocityCeiling`
    // above). Inactive changes nothing; active can only lower the desired
    // magnitude, before the rate limits, and an active malformed value refuses
    // the step (`InvalidStrategyCeiling`) instead of being ignored. The default
    // is the explicit inactive value, never a silently chosen ceiling: a caller
    // that states nothing gets exactly the shaping that existed before phase
    // ceilings, and a caller that wants one states it.
    StrategyVelocityCeiling strategyCeiling;
};

// Why the target is what it is. Diagnostic only: the target is the output.
enum class MotionProfileStatus : uint8_t {
    Ok = 0,
    WithinBrakeOffset,      // the measured stopping distance already reaches the target point
    StopRequested,          // the run asked for a zero target
    DirectionChangeHold,    // a reversal is being ramped to zero before the sign may change
    LimitsAreNotUsable,     // not Measured, or malformed limits: no target is shaped
    InvalidDistance,        // non-finite or negative remaining distance
    InvalidTarget,          // non-finite current target
    InvalidStrategyCeiling, // the active strategy ceiling is non-finite or negative
    TimeDidNotAdvance,      // dtMs == 0: the previous target is held, or dropped to zero when the requested direction contradicts its sign
};

// Whether the target magnitude is rising, falling or unchanged this cycle.
// This describes the target, not the vehicle and not the run state machine.
enum class MotionProfilePhase : uint8_t {
    Stopped = 0,
    Accelerating,
    Cruising,
    Decelerating,
};

// Which ceiling set the target magnitude. Diagnostic only.
enum class MotionProfileLimit : uint8_t {
    None = 0,        // no ceiling applies (a held or refused target)
    CruiseCeiling,   // the cruise/velocity ceiling is what the target sits at
    StrategyCeiling, // the run strategy's phase ceiling is what the target sits at
    DistanceTaper,   // the remaining-distance taper is what caps the target
    RampLimit,       // the acceleration/deceleration rate limit is what caps it
    BrakeOffset,     // the brake offset reaches the target point
    StopRequest,     // the run asked for zero
};

// The shaped target for this cycle. Deliberately no default constructor.
struct MotionProfileCommand {
    constexpr MotionProfileCommand(double targetVelocityMps_, MotionProfileStatus status_,
                                   MotionProfilePhase phase_, MotionProfileLimit binding_,
                                   double cruiseCeilingMps_, double taperCeilingMps_,
                                   double accelCeilingMps_, double decelCeilingMps_,
                                   double strategyCeilingMps_, bool strategyCeilingActive_)
        : targetVelocityMps(targetVelocityMps_),
          status(status_),
          phase(phase_),
          binding(binding_),
          cruiseCeilingMps(cruiseCeilingMps_),
          taperCeilingMps(taperCeilingMps_),
          accelCeilingMps(accelCeilingMps_),
          decelCeilingMps(decelCeilingMps_),
          strategyCeilingMps(strategyCeilingMps_),
          strategyCeilingActive(strategyCeilingActive_) {}

    // Signed target velocity in the command convention, metres per second. 0.0
    // means "no powered motion is being asked for", which is the only thing this
    // module says about stopping.
    double targetVelocityMps;

    MotionProfileStatus status;
    MotionProfilePhase phase;
    MotionProfileLimit binding;

    // The ceilings behind the decision, reported for telemetry and for
    // comparing candidate configurations. All five are 0.0 on a refusal, and the
    // strategy ceiling is additionally inactive there.
    double cruiseCeilingMps;  // the lower of cruiseVelocityMps and velocityCeilingMps
    double taperCeilingMps;   // the taper, capped by cruiseCeilingMps
    double accelCeilingMps;   // current magnitude plus the acceleration allowed in dtMs
    double decelCeilingMps;   // current magnitude minus the deceleration allowed in dtMs

    // The strategy ceiling supplied this cycle, metres per second, and whether a
    // ceiling was supplied at all. 0.0 with `strategyCeilingActive == false` means
    // "no ceiling"; 0.0 with `true` is a legal active ceiling of zero - which is
    // why the flag exists rather than "0.0 means none". The value is reported as
    // supplied (validated); whether it actually tightened the target is
    // `binding == MotionProfileLimit::StrategyCeiling`.
    double strategyCeilingMps;
    bool strategyCeilingActive;
};

// The caller-held target velocity the stateless shaper needs.
//
// The optional strategy ceiling defaults to inactive (`MotionProfileInputs`), so
// the baseline shaping is unchanged for every caller that does not use strategy
// ceilings, and the explicit `inactiveStrategyVelocityCeiling()` remains
// available for a caller that wants to state the absence.
//
// MotionProfile holds no state, so the previous target is an argument on every
// call. This holder is that argument and nothing else: its zero value is exactly
// zero (no powered effort is being asked for), it can only change by accepting a
// shaped command, and `resetForNewRun()` is the explicit way a new run starts it
// over. Carrying the value by hand instead is how a stale target from a previous
// run silently reshapes the next ramp: a run that starts from 1.0 m/s of
// leftover target is not the run the profile was asked to shape.
//
// Deliberately default-constructible, because a fresh holder is a zero target,
// which is the safe value: a new run can start from a zero-initialized member
// without acting on anything.
class MotionProfileTarget {
public:
    constexpr MotionProfileTarget() : targetVelocityMps_(0.0) {}

    // The target the next shaping step starts from, metres per second, signed in
    // the command convention.
    constexpr double targetVelocityMps() const { return targetVelocityMps_; }

    // Stores the target of a shaped command. A refusal carries a zero target, so
    // accepting a refusal parks the holder at zero rather than at a stale value;
    // a non-finite target (which no profile path produces) is stored as zero for
    // the same fail-closed reason.
    void accept(const MotionProfileCommand& command) {
        targetVelocityMps_ =
            std::isfinite(command.targetVelocityMps) ? command.targetVelocityMps : 0.0;
    }

    // Explicit new-run reset: back to exactly zero. This is the only way a run
    // boundary clears the handoff, and `resetMotionChainForNewRun()` calls it.
    void resetForNewRun() { targetVelocityMps_ = 0.0; }

    // Builds this cycle's inputs with the held target already filled in, so the
    // handoff cannot be passed by hand.
    MotionProfileInputs inputsFor(MotionProfileIntent intent, MotionDirection direction,
                                  double remainingDistanceM, uint32_t dtMs,
                                  StrategyVelocityCeiling strategyCeiling =
                                      inactiveStrategyVelocityCeiling()) const {
        return MotionProfileInputs(intent, direction, remainingDistanceM, targetVelocityMps_,
                                   dtMs, strategyCeiling);
    }

private:
    double targetVelocityMps_;
};

static_assert(MotionProfileTarget{}.targetVelocityMps() == 0.0,
              "a fresh profile target must start at exactly zero");

// Whether the target magnitude is rising, falling or unchanged. This is
// diagnostics about the target itself: the vehicle's motion and the run's state
// are different things, and neither is inferred here.
inline MotionProfilePhase profilePhaseForMagnitude(double newMagnitudeMps,
                                                   double previousMagnitudeMps) {
    if (newMagnitudeMps <= 0.0) {
        return MotionProfilePhase::Stopped;
    }
    if (newMagnitudeMps > previousMagnitudeMps) {
        return MotionProfilePhase::Accelerating;
    }
    if (newMagnitudeMps < previousMagnitudeMps) {
        return MotionProfilePhase::Decelerating;
    }
    return MotionProfilePhase::Cruising;
}

// One shaping step.
//
// Deterministic for every input it refuses: a refusal returns a zero target,
// which the controller turns into no powered effort, never into a coasting or
// braking action.
inline MotionProfileCommand nextTargetVelocity(const MotionProfileInputs& inputs,
                                               const MotionProfileLimits& limits) {
    if (!validateMotionProfileLimits(limits).ok) {
        return MotionProfileCommand{0.0, MotionProfileStatus::LimitsAreNotUsable,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0, 0.0, false};
    }
    if (!std::isfinite(inputs.remainingDistanceM) || inputs.remainingDistanceM < 0.0) {
        return MotionProfileCommand{0.0, MotionProfileStatus::InvalidDistance,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0, 0.0, false};
    }
    if (!std::isfinite(inputs.currentTargetVelocityMps)) {
        return MotionProfileCommand{0.0, MotionProfileStatus::InvalidTarget,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0, 0.0, false};
    }
    // A malformed ACTIVE strategy ceiling is a refusal, never an excuse to shape
    // as if the ceiling were absent: a strategy that asked for a slower phase must
    // not get a faster one because its own input was unreadable. An inactive
    // ceiling is always valid and changes nothing.
    if (!validateStrategyVelocityCeiling(inputs.strategyCeiling).ok) {
        return MotionProfileCommand{0.0, MotionProfileStatus::InvalidStrategyCeiling,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0, 0.0, false};
    }

    const double cruiseCeilingMps = limits.cruiseVelocityMps.value < limits.velocityCeilingMps.value
                                        ? limits.cruiseVelocityMps.value
                                        : limits.velocityCeilingMps.value;
    const double currentMagnitudeMps = std::fabs(inputs.currentTargetVelocityMps);
    const int8_t currentSign = (inputs.currentTargetVelocityMps > 0.0)
                                   ? 1
                                   : (inputs.currentTargetVelocityMps < 0.0 ? -1 : 0);
    const int8_t requestedSign =
        (inputs.direction == limits.positiveCommandDirection.value) ? 1 : -1;
    const bool stopRequested = inputs.intent == MotionProfileIntent::Stop;

    // The taper: the speed from which the deceleration limit still stops the
    // vehicle inside what remains after the measured brake offset. Zero once the
    // offset already reaches the target point.
    const bool withinBrakeOffset = !(inputs.remainingDistanceM > limits.brakeOffsetM.value);
    const double taperCeilingMps =
        withinBrakeOffset
            ? 0.0
            : std::sqrt(2.0 * limits.decelLimitMps2.value *
                        (inputs.remainingDistanceM - limits.brakeOffsetM.value));
    const double cappedTaperCeilingMps =
        taperCeilingMps < cruiseCeilingMps ? taperCeilingMps : cruiseCeilingMps;

    // The strategy ceiling joins the desired magnitude BEFORE the rate limits: it
    // can only lower what the geometry asks for, and a sudden reduction produces
    // a desired value the deceleration ceiling below ramps down - never a target
    // step. An active zero ceiling therefore reaches exactly zero through the
    // same ramp a stop request takes.
    const bool strategyCeilingBinds =
        inputs.strategyCeiling.active && inputs.strategyCeiling.velocityMps < cappedTaperCeilingMps;

    if (inputs.dtMs == 0U) {
        // No time passed. The target is held exactly as it was, even if the
        // geometry now points elsewhere: a changed target with no elapsed time
        // behind it would be a step the rate limits never authorized. The
        // ceilings are still reported so the log shows what the geometry says.
        // With no elapsed time the rate brackets collapse onto the current
        // magnitude, which is exactly why the hold is the only legal answer.
        //
        // The one exception is a requested direction that contradicts the
        // target's own sign: holding that value would hand the caller a fully
        // signed target in the direction the run has just stopped travelling in,
        // which is the exact step the one-way reversal street exists to forbid.
        // The dt > 0 path refuses it through `directionChange`; this branch is
        // the one input combination that guard never sees, so the same refusal
        // has to live here. Zero is the value that street converges to anyway,
        // and a zero target asks for no powered effort.
        const bool directionChange = currentSign != 0 && currentSign != requestedSign;
        const double heldMagnitudeMps = directionChange ? 0.0 : currentMagnitudeMps;
        return MotionProfileCommand{directionChange ? 0.0 : inputs.currentTargetVelocityMps,
                                    MotionProfileStatus::TimeDidNotAdvance,
                                    profilePhaseForMagnitude(heldMagnitudeMps, currentMagnitudeMps),
                                    MotionProfileLimit::None, cruiseCeilingMps,
                                    cappedTaperCeilingMps, heldMagnitudeMps, heldMagnitudeMps,
                                    inputs.strategyCeiling.velocityMps,
                                    inputs.strategyCeiling.active};
    }

    const double dtS = static_cast<double>(inputs.dtMs) / 1000.0;

    // The magnitude the geometry alone would ask for, and why.
    double desiredMagnitudeMps;
    MotionProfileLimit desiredBinding;
    if (stopRequested) {
        desiredMagnitudeMps = 0.0;
        desiredBinding = MotionProfileLimit::StopRequest;
    } else if (withinBrakeOffset) {
        desiredMagnitudeMps = 0.0;
        desiredBinding = MotionProfileLimit::BrakeOffset;
    } else if (strategyCeilingBinds) {
        desiredMagnitudeMps = inputs.strategyCeiling.velocityMps;
        desiredBinding = MotionProfileLimit::StrategyCeiling;
    } else {
        desiredMagnitudeMps = cappedTaperCeilingMps;
        desiredBinding = cappedTaperCeilingMps < cruiseCeilingMps
                             ? MotionProfileLimit::DistanceTaper
                             : MotionProfileLimit::CruiseCeiling;
    }

    // Rate limits. The deceleration ceiling is never below zero, so the target
    // magnitude can approach zero but never cross it.
    const double decelCeilingRawMps = currentMagnitudeMps - limits.decelLimitMps2.value * dtS;
    const double decelCeilingMps = decelCeilingRawMps > 0.0 ? decelCeilingRawMps : 0.0;
    const double accelCeilingMps = currentMagnitudeMps + limits.accelLimitMps2.value * dtS;

    const bool directionChange = currentSign != 0 && currentSign != requestedSign;
    double magnitudeMps;
    MotionProfileLimit binding;
    if (directionChange) {
        // A reversal one-way street: the magnitude only falls this step. The
        // sign stays with the current direction while it is above zero, so a
        // step can never jump from forward drive to reverse drive.
        magnitudeMps = decelCeilingMps;
        binding = MotionProfileLimit::RampLimit;
    } else if (desiredMagnitudeMps < decelCeilingMps) {
        magnitudeMps = decelCeilingMps;
        binding = MotionProfileLimit::RampLimit;
    } else if (desiredMagnitudeMps > accelCeilingMps) {
        magnitudeMps = accelCeilingMps;
        binding = MotionProfileLimit::RampLimit;
    } else {
        magnitudeMps = desiredMagnitudeMps;
        binding = desiredBinding;
    }

    // While a reversal is ramping down, the sign belongs to the direction the
    // target is coming from; the new sign appears only once the magnitude has
    // reached zero and a later step ramps it up again.
    const int8_t outputSign = (directionChange && magnitudeMps > 0.0) ? currentSign : requestedSign;
    const double targetVelocityMps = magnitudeMps > 0.0 ? outputSign * magnitudeMps : 0.0;

    MotionProfileStatus status = MotionProfileStatus::Ok;
    if (withinBrakeOffset) {
        status = MotionProfileStatus::WithinBrakeOffset;
    } else if (stopRequested) {
        status = MotionProfileStatus::StopRequested;
    } else if (directionChange) {
        status = MotionProfileStatus::DirectionChangeHold;
    }

    return MotionProfileCommand{targetVelocityMps, status,
                                profilePhaseForMagnitude(magnitudeMps, currentMagnitudeMps),
                                binding, cruiseCeilingMps, cappedTaperCeilingMps,
                                accelCeilingMps, decelCeilingMps,
                                inputs.strategyCeiling.velocityMps,
                                inputs.strategyCeiling.active};
}

// Whether a shaped target points along the run's commanded leg.
//
// The profile ramps a commanded reversal to zero before it flips the sign
// (`DirectionChangeHold`), so the travel command that opens a new leg can be
// shaped at a small target still on the *previous* direction's side. Handing that
// target to the regulator makes it compute a powered effort in the direction the
// run has just stopped travelling in - and after a verified stop the authority
// has no latch left to refuse it with, because the stop that earned the reversal
// is exactly what cleared that latch. This is the single verdict a consumer uses
// to present a travel request as powered only once the shape agrees with the leg
// it is for; while it is false the consumer presents its own unpowered output.
//
// A zero target asks for no powered motion and is always along the leg. A
// non-finite target is not something this module produces (a refusal shapes
// exactly zero), so it is refused here rather than being given a sign.
//
// This answers the shape's direction and nothing else: whether a request may be
// presented at all also requires a current run intention
// (`runIntentionIsCurrentFor()`) and fresh position data, and those verdicts are
// owned elsewhere (SOFTWARE_ARCHITECTURE.md section 13).
inline bool targetVelocityPointsAlongLeg(const MotionProfileCommand& shaped,
                                         MotionDirection leg,
                                         const MotionProfileLimits& limits) {
    if (!std::isfinite(shaped.targetVelocityMps)) {
        return false;
    }
    if (shaped.targetVelocityMps == 0.0) {
        return true;
    }
    const bool positive = shaped.targetVelocityMps > 0.0;
    const bool positiveAlongLeg = leg == limits.positiveCommandDirection.value;
    return positive == positiveAlongLeg;
}

// Pin the "no silent answer" contracts and the safe zero values.
static_assert(!std::is_default_constructible<MotionProfileCommand>::value,
              "a profile command must never default-construct into a shaped target");
static_assert(!std::is_default_constructible<MotionProfileInputs>::value,
              "profile inputs must never default-construct");
static_assert(!std::is_default_constructible<MotionDurationEstimate>::value,
              "a duration estimate must never default-construct");
static_assert(!std::is_default_constructible<MotionTimingSummary>::value,
              "a timing summary must never default-construct");
static_assert(static_cast<int>(MotionProfilePhase::Stopped) == 0,
              "the stopped phase must stay the zero value");

}  // namespace ev
