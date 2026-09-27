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
//   shaping value is written down here.
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
    if (distanceM == 0.0) {
        return {0.0, true, MotionDurationStatus::Valid};
    }

    const double accelDistanceM =
        cruiseVelocityMps.value * cruiseVelocityMps.value / (2.0 * accelLimitMps2.value);
    const double decelDistanceM =
        cruiseVelocityMps.value * cruiseVelocityMps.value / (2.0 * decelLimitMps2.value);

    double durationS = 0.0;
    if (accelDistanceM + decelDistanceM <= distanceM) {
        const double cruiseDistanceM = distanceM - accelDistanceM - decelDistanceM;
        durationS = cruiseVelocityMps.value / accelLimitMps2.value +
                    cruiseDistanceM / cruiseVelocityMps.value +
                    cruiseVelocityMps.value / decelLimitMps2.value;
    } else {
        // Triangular profile: the peak speed solves
        // distance = v^2/(2a) + v^2/(2d).
        const double peakVelocityMps =
            std::sqrt(2.0 * distanceM /
                      (1.0 / accelLimitMps2.value + 1.0 / decelLimitMps2.value));
        durationS = peakVelocityMps / accelLimitMps2.value +
                    peakVelocityMps / decelLimitMps2.value;
    }
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
    ValuesAreNotMeasured,
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

inline MotionProfileLimitsValidation validateMotionProfileLimits(
    const MotionProfileLimits& limits) {
    if (!limits.cruiseVelocityMps.isMeasured() || !limits.velocityCeilingMps.isMeasured() ||
        !limits.accelLimitMps2.isMeasured() || !limits.decelLimitMps2.isMeasured() ||
        !limits.brakeOffsetM.isMeasured() || !limits.positiveCommandDirection.isMeasured()) {
        return {false, MotionProfileLimitsError::ValuesAreNotMeasured};
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

// Per-cycle inputs. No default constructor: an unstated travel is not a travel.
struct MotionProfileInputs {
    constexpr MotionProfileInputs(MotionProfileIntent intent_, MotionDirection direction_,
                                  double remainingDistanceM_, double currentTargetVelocityMps_,
                                  uint32_t dtMs_)
        : intent(intent_),
          direction(direction_),
          remainingDistanceM(remainingDistanceM_),
          currentTargetVelocityMps(currentTargetVelocityMps_),
          dtMs(dtMs_) {}

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
    // passed and the target is held unchanged.
    uint32_t dtMs;
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
    TimeDidNotAdvance,      // dtMs == 0: the previous target is held exactly
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
                                   double accelCeilingMps_, double decelCeilingMps_)
        : targetVelocityMps(targetVelocityMps_),
          status(status_),
          phase(phase_),
          binding(binding_),
          cruiseCeilingMps(cruiseCeilingMps_),
          taperCeilingMps(taperCeilingMps_),
          accelCeilingMps(accelCeilingMps_),
          decelCeilingMps(decelCeilingMps_) {}

    // Signed target velocity in the command convention, metres per second. 0.0
    // means "no powered motion is being asked for", which is the only thing this
    // module says about stopping.
    double targetVelocityMps;

    MotionProfileStatus status;
    MotionProfilePhase phase;
    MotionProfileLimit binding;

    // The ceilings behind the decision, reported for telemetry and for
    // comparing candidate configurations. All four are 0.0 on a refusal.
    double cruiseCeilingMps;  // the lower of cruiseVelocityMps and velocityCeilingMps
    double taperCeilingMps;   // the taper, capped by cruiseCeilingMps
    double accelCeilingMps;   // current magnitude plus the acceleration allowed in dtMs
    double decelCeilingMps;   // current magnitude minus the deceleration allowed in dtMs
};

// The caller-held target velocity the stateless shaper needs.
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
                                  double remainingDistanceM, uint32_t dtMs) const {
        return MotionProfileInputs(intent, direction, remainingDistanceM, targetVelocityMps_,
                                   dtMs);
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
                                    0.0, 0.0};
    }
    if (!std::isfinite(inputs.remainingDistanceM) || inputs.remainingDistanceM < 0.0) {
        return MotionProfileCommand{0.0, MotionProfileStatus::InvalidDistance,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0};
    }
    if (!std::isfinite(inputs.currentTargetVelocityMps)) {
        return MotionProfileCommand{0.0, MotionProfileStatus::InvalidTarget,
                                    MotionProfilePhase::Stopped, MotionProfileLimit::None, 0.0, 0.0,
                                    0.0, 0.0};
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

    if (inputs.dtMs == 0U) {
        // No time passed. The target is held exactly as it was, even if the
        // geometry now points elsewhere: a changed target with no elapsed time
        // behind it would be a step the rate limits never authorized. The
        // ceilings are still reported so the log shows what the geometry says.
        // With no elapsed time the rate brackets collapse onto the current
        // magnitude, which is exactly why the hold is the only legal answer.
        return MotionProfileCommand{inputs.currentTargetVelocityMps,
                                    MotionProfileStatus::TimeDidNotAdvance,
                                    profilePhaseForMagnitude(currentMagnitudeMps,
                                                             currentMagnitudeMps),
                                    MotionProfileLimit::None, cruiseCeilingMps,
                                    cappedTaperCeilingMps, currentMagnitudeMps,
                                    currentMagnitudeMps};
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
                                accelCeilingMps, decelCeilingMps};
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
