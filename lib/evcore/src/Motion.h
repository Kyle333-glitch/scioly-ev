#pragma once

#include <cmath>
#include <cstdint>

namespace ev {

// What the firmware intends the motor to do.
//
// The mapping from these intents to actual driver inputs is UNVERIFIED: the
// driver truth table has not been measured (VERIFIED_STATE.md #2). Disabled is
// the safe state and is intentionally the zero value.
enum class MotorMode : uint8_t {
    Disabled = 0,  // driver outputs off / disabled
    Coast = 1,
    Brake = 2,
    DriveForward = 3,
    DriveReverse = 4,
};

inline bool isKnownMotorMode(MotorMode mode) {
    switch (mode) {
        case MotorMode::Disabled:
        case MotorMode::Coast:
        case MotorMode::Brake:
        case MotorMode::DriveForward:
        case MotorMode::DriveReverse:
            return true;
    }
    return false;
}

// True for DriveForward/DriveReverse at any magnitude, including 0.
// A zero-effort Drive still carries a PoweredDirection and, when
// authorized, still latches direction in SafetyAuthority. A true
// non-powered stop is Disabled/Coast/Brake with 0; do not use
// Drive-with-zero as a stop.
inline bool isDrivingMode(MotorMode mode) {
    return mode == MotorMode::DriveForward || mode == MotorMode::DriveReverse;
}

// The coarse electrical-action class a mode belongs to. This is the vocabulary
// the safety boundary reasons about: whether an action is DISABLED, BRAKE or
// DRIVE. Direction and effort are meaningful only for a Drive action.
//
// Coast is grouped with Disabled here because both remove powered output; the
// difference between them is a driver detail that is not yet verified
// (VERIFIED_STATE.md #2), so the safety boundary must not depend on it.
//
// A future MotorDriver must NOT switch on this class: it must switch on
// MotorMode directly so Disabled (outputs off) and Coast (high-Z) stay
// distinct even though the safety boundary groups them.
enum class ActuationClass : uint8_t {
    Disabled = 0,  // no powered output (Disabled or Coast)
    Brake = 1,     // active braking, no direction, no effort
    Drive = 2,     // powered drive; direction and effort are meaningful
};

inline ActuationClass actuationClassOf(MotorMode mode) {
    switch (mode) {
        case MotorMode::Disabled:
        case MotorMode::Coast:
            return ActuationClass::Disabled;
        case MotorMode::Brake:
            return ActuationClass::Brake;
        case MotorMode::DriveForward:
        case MotorMode::DriveReverse:
            return ActuationClass::Drive;
    }
    // An unknown mode is never a powered request.
    return ActuationClass::Disabled;
}

// Which way the motor is powered. None means no powered output; it is
// intentionally the zero value so a default-initialized direction is the
// non-powered one. These are distinct identities, not a scale: order carries
// no meaning; never compare relationally.
enum class PoweredDirection : uint8_t {
    None = 0,
    Forward = 1,
    Reverse = 2,
};

inline PoweredDirection poweredDirectionOf(MotorMode mode) {
    switch (mode) {
        case MotorMode::DriveForward:
            return PoweredDirection::Forward;
        case MotorMode::DriveReverse:
            return PoweredDirection::Reverse;
        case MotorMode::Disabled:
        case MotorMode::Coast:
        case MotorMode::Brake:
            return PoweredDirection::None;
    }
    return PoweredDirection::None;
}

// A requested motor effort before authorization. Magnitude is a fraction of
// full commanded effort in permille (0-1000). It is dimensionless on purpose:
// it is an intent, not a hardware duty cycle or a current.
struct MotorEffort {
    constexpr MotorEffort(MotorMode mode_, uint16_t magnitudePermille_)
        : mode(mode_), magnitudePermille(magnitudePermille_) {}

    MotorMode mode;
    uint16_t magnitudePermille;
};

enum class MotorEffortError : uint8_t {
    None = 0,
    UnknownMode,            // mode is not a defined MotorMode value
    MagnitudeOutOfRange,    // above full scale
    MagnitudeOnNonDrivingMode,
};

struct MotorEffortValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    MotorEffortError error;
};

inline MotorEffortValidation validateMotorEffort(const MotorEffort& effort) {
    if (!isKnownMotorMode(effort.mode)) {
        return {false, MotorEffortError::UnknownMode};
    }
    if (effort.magnitudePermille > 1000U) {
        return {false, MotorEffortError::MagnitudeOutOfRange};
    }
    if (!isDrivingMode(effort.mode) && effort.magnitudePermille != 0U) {
        return {false, MotorEffortError::MagnitudeOnNonDrivingMode};
    }
    return {true, MotorEffortError::None};
}

// A motor effort that a safety authority has explicitly authorized.
//
// Nothing in this stage can create one: the constructor is private and reserved
// for the arming authority, which is a later stage. The deliberate effect is
// that no code present today can command powered motion, and no hardware
// wrapper can be written to accept anything weaker than this type.
class AuthorizedMotorCommand {
public:
    MotorMode mode() const { return mode_; }

    // Effective effort, permille of full scale (0-1000). Meaningful only for a
    // Drive command; every non-Drive command is constructed with magnitude 0.
    uint16_t magnitudePermille() const { return magnitudePermille_; }

    ActuationClass actuationClass() const { return actuationClassOf(mode_); }
    bool isDisabled() const { return mode_ == MotorMode::Disabled; }
    bool isCoasting() const { return mode_ == MotorMode::Coast; }
    bool isBraking() const { return mode_ == MotorMode::Brake; }
    // True for DriveForward/DriveReverse even when magnitude is 0. A
    // zero-effort Drive still latches PoweredDirection in SafetyAuthority
    // and still requires a zero-speed stop to reverse.
    bool isDriving() const { return isDrivingMode(mode_); }

    // None unless this is a Drive command.
    PoweredDirection poweredDirection() const { return poweredDirectionOf(mode_); }

private:
    // The authority that may arm the vehicle. Declared here so the boundary is
    // explicit; implemented in a later stage.
    friend class SafetyAuthority;

    // Precondition: the caller must have accepted this effort via
    // validateMotorEffort() (mode known, magnitude in range, magnitude rules
    // respected). Enforcing that here is a SafetyAuthority (P2)
    // responsibility.
    explicit AuthorizedMotorCommand(const MotorEffort& effort)
        : mode_(effort.mode), magnitudePermille_(effort.magnitudePermille) {}

    MotorMode mode_;
    uint16_t magnitudePermille_;
};

enum class MotionDirection : uint8_t {
    Forward = 0,
    Reverse = 1,
};

// True only for the two physical directions above. A cast-in unknown value is
// not a direction: agreement checks must refuse it rather than fold it into
// Reverse by accident (S18A/P5). Mirrors `isKnownMotorMode()` above.
inline bool isKnownMotionDirection(MotionDirection direction) {
    switch (direction) {
        case MotionDirection::Forward:
        case MotionDirection::Reverse:
            return true;
    }
    return false;
}

// VOCABULARY ONLY - no firmware path may consume this type.
//
// The single owner of a run's target and time contract is
// `MotionProfileInputs` together with `MotionTimingIntent`, both in
// MotionProfile.h: the remaining distance and elapsed time are profile inputs,
// and Target Time and the intentional delay are timing intent. This struct is a
// leftover primitive intent that nothing constructs and nothing consumes; it is
// validated and tested because it exists, not because a run uses it, and its
// `timeBudgetS` duplicates `MotionTimingIntent::targetTimeS`.
//
// Do not wire it into a run without superseding the target/time ownership
// recorded in SOFTWARE_ARCHITECTURE.md section 13 and logging the change in
// DEVELOPMENT_LOG.md. When that decision is made, this type should be deleted
// rather than kept as a second, competing time budget.
struct MotionRequest {
    constexpr MotionRequest(MotionDirection direction_, double targetDistanceM_,
                            double timeBudgetS_)
        : direction(direction_), targetDistanceM(targetDistanceM_), timeBudgetS(timeBudgetS_) {}

    MotionDirection direction;
    double targetDistanceM;  // > 0
    double timeBudgetS;      // > 0
};

enum class MotionRequestError : uint8_t {
    None = 0,
    NonFiniteValue,          // NaN or infinity
    TargetDistanceNotPositive,
    TimeBudgetNotPositive,
};

struct MotionRequestValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    MotionRequestError error;
};

inline MotionRequestValidation validateMotionRequest(const MotionRequest& request) {
    if (!std::isfinite(request.targetDistanceM) || !std::isfinite(request.timeBudgetS)) {
        return {false, MotionRequestError::NonFiniteValue};
    }
    if (!(request.targetDistanceM > 0.0)) {
        return {false, MotionRequestError::TargetDistanceNotPositive};
    }
    if (!(request.timeBudgetS > 0.0)) {
        return {false, MotionRequestError::TimeBudgetNotPositive};
    }
    return {true, MotionRequestError::None};
}

}  // namespace ev
