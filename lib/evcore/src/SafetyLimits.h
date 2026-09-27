#pragma once

#include "Motion.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// Ceilings the vehicle must never exceed. Kept separate from
// VehicleCalibration (what the vehicle does measure) and from RunConfig (what
// the event supervisor announces).
//
// Like calibration, every value is supplied by the caller with its evidence.
// None can be filled in yet: no motor, driver or battery behavior has been
// measured. isFullyEstablished() is expected to stay false until real
// measurements exist, and a later arming authority must refuse to arm while it
// is false.
struct SafetyLimits {
    // Highest effort the firmware may ever command, permille of full scale
    // (0-1000).
    Sourced<uint16_t> maxEffortPermille;

    // Highest estimated speed allowed in each direction, metres per second.
    Sourced<double> maxForwardVelocityMps;
    Sourced<double> maxReverseVelocityMps;

    // Longest a single powered run may last, seconds.
    Sourced<double> maxRunDurationS;

    // Lowest battery voltage at which motion is allowed, volts.
    Sourced<double> minBatteryV;

    // What the chain does with these today: only maxEffortPermille is consumed,
    // and only through the vehicle layer that derives the regulator's own
    // ceiling from it (see effortCeilingsAgree() below). maxRunDurationS and
    // minBatteryV have no enforcement point: the run timer and the battery
    // check that will own them are not built, so they are data with no runtime
    // effect. They are deliberately not treated as satisfied - nothing has
    // consumed them to declare a run legal.

    // True only when every limit is backed by recorded measurement.
    //
    // This is a data-completeness check, not an arming decision; deciding when
    // the vehicle may be armed belongs to a later stage.
    constexpr bool isFullyEstablished() const {
        return maxEffortPermille.isMeasured() && maxForwardVelocityMps.isMeasured() &&
               maxReverseVelocityMps.isMeasured() && maxRunDurationS.isMeasured() &&
               minBatteryV.isMeasured();
    }
};

enum class SafetyLimitsError : uint8_t {
    None = 0,
    NotFullyMeasured,      // at least one limit is not ValueSource::Measured
    NonFiniteValue,        // NaN or infinity in a double limit
    MaxEffortOutOfRange,   // maxEffortPermille is not 0..1000
    NonPositiveLimit,      // a velocity, duration, or voltage ceiling is not > 0
};

struct SafetyLimitsValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    SafetyLimitsError error;
};

// Single entry point for safety-ceiling validation. Checks provenance first,
// then finiteness, then the Motion invariant (permille is 0..1000), then
// positivity of the remaining ceilings. The vehicle layer must derive
// SafetyReadiness::limitsUsable from this result so SafetyAuthority never
// has to interpret raw Sourced values.
inline SafetyLimitsValidation validateSafetyLimits(const SafetyLimits& limits) {
    if (!limits.isFullyEstablished()) {
        return {false, SafetyLimitsError::NotFullyMeasured};
    }
    if (!std::isfinite(limits.maxForwardVelocityMps.value) ||
        !std::isfinite(limits.maxReverseVelocityMps.value) ||
        !std::isfinite(limits.maxRunDurationS.value) ||
        !std::isfinite(limits.minBatteryV.value)) {
        return {false, SafetyLimitsError::NonFiniteValue};
    }
    if (limits.maxEffortPermille.value > 1000U) {
        return {false, SafetyLimitsError::MaxEffortOutOfRange};
    }
    if (!(limits.maxForwardVelocityMps.value > 0.0) ||
        !(limits.maxReverseVelocityMps.value > 0.0) ||
        !(limits.maxRunDurationS.value > 0.0) || !(limits.minBatteryV.value > 0.0)) {
        return {false, SafetyLimitsError::NonPositiveLimit};
    }
    return {true, SafetyLimitsError::None};
}

// The two effort ceilings that must agree: the regulator's own request ceiling
// (`VelocityControllerConfig::maxEffortPermille`) and this struct's
// `maxEffortPermille`. The vehicle layer derives the former from the latter, so
// the two must be numerically equal before the vehicle may be armed.
//
// A tighter safety ceiling is not harmless defence in depth here. The
// regulator's anti-windup and its `outputSaturated` report are computed against
// its own ceiling only: it would keep integrating headroom it does not have
// while SafetyAuthority clamped silently downstream, and one saturation would
// appear as `outputSaturated == false` in one log and `AuthorizedClamped` in
// another. Equality makes the regulator's own saturation report the whole story
// (the authority still clamps independently).
//
// Provenance is validated elsewhere - `validateVelocityControllerConfig()` and
// `validateSafetyLimits()` - so this is the numeric agreement only, and it is a
// configuration-build check, not a runtime clamp.
inline bool effortCeilingsAgree(const Sourced<uint16_t>& controllerMaxEffortPermille,
                                const Sourced<uint16_t>& safetyMaxEffortPermille) {
    return controllerMaxEffortPermille.value == safetyMaxEffortPermille.value;
}

// The two encodings of one physical fact must agree: which direction is
// "positive". `MotionProfileLimits::positiveCommandDirection` maps a physical
// `MotionDirection` to the command sign, and
// `VelocityControllerConfig::positiveEffortMode` maps that sign to a powered
// driver mode. They are the same UNVERIFIED polarity fact (`VERIFIED_STATE.md`
// #4) written down in two vocabularies, and the vehicle layer must refuse to
// build or arm the chain unless both name the same physical direction: a pair
// that is `Measured` but opposite passes every per-module validation, and the
// profile's taper and the regulator's effort would then drive the vehicle in
// opposite directions for the same command sign.
//
// The comparison is over the physical directions the two values name, never
// over their numeric values: `MotionDirection::Forward` agrees with
// `MotorMode::DriveForward` and `MotionDirection::Reverse` with
// `MotorMode::DriveReverse`, there is no relational ordering between the two
// enums, and a non-driving mode agrees with nothing.
//
// Provenance is validated elsewhere - `validateMotionProfileLimits()` and
// `validateVelocityControllerConfig()` - so this is the semantic agreement only,
// and it is a configuration-build check, not a runtime clamp.
inline bool polarityAgrees(const Sourced<MotionDirection>& profilePositiveCommandDirection,
                           const Sourced<MotorMode>& controllerPositiveEffortMode) {
    const PoweredDirection profileDirection =
        profilePositiveCommandDirection.value == MotionDirection::Forward
            ? PoweredDirection::Forward
            : PoweredDirection::Reverse;
    return poweredDirectionOf(controllerPositiveEffortMode.value) == profileDirection;
}

// The third encoding of the one physical polarity fact: which direction the
// decoder's positive tick convention corresponds to. `polarityAgrees()` above
// ties the command sign to the powered effort; this ties the measured sign to the
// command sign, and together the three must name one physical direction.
//
// The pairing matters because the regulator forms `target - measured` and demands
// effort from that error: a positive command sign must therefore mean the same
// physical direction as a positive measured speed, or the loop's error has the
// wrong sign and it drives away from its target instead of toward it - positive
// feedback, with no per-module validation able to see it. The run axis makes the
// same requirement from the other side: a run travelling its forward leg must see
// its position grow (`RunAxis.h`), which holds exactly when positive ticks are
// forward travel.
//
// The comparison is over the physical directions the two values name, never over
// their numeric values. Provenance is validated elsewhere -
// `validateMotionProfileLimits()` for the command direction, and the run-axis
// layer's own refusal for the tick direction - so this is the semantic agreement
// only, and it is a configuration-build check, not a runtime clamp.
inline bool tickPolarityAgrees(const Sourced<MotionDirection>& positiveTicksDirection,
                               const Sourced<MotionDirection>& profilePositiveCommandDirection) {
    return positiveTicksDirection.value == profilePositiveCommandDirection.value;
}

// Pin the provenance contract: a safety ceiling with an unstated source is a
// compile error. This matters most here, because a guessed limit cannot be
// distinguished from a measured one at runtime.
static_assert(!std::is_default_constructible<SafetyLimits>::value,
              "no silent defaults; every value must be supplied with its source");

}  // namespace ev
