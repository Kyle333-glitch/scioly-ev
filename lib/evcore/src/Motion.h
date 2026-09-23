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

inline bool isDrivingMode(MotorMode mode) {
    return mode == MotorMode::DriveForward || mode == MotorMode::DriveReverse;
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
    uint16_t magnitudePermille() const { return magnitudePermille_; }

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

// A primitive motion intent for the controller: travel this far, in this
// direction, within this time budget.
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
