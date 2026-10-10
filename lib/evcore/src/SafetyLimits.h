#pragma once

#include "Motion.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// Whether this vehicle has a battery-voltage sensing circuit at all.
//
// This is a *capability* fact, separate from the `minBatteryV` limit's value and
// provenance, and it is separate on purpose: "what voltage is too low" and "is
// there a circuit that could ever read the voltage" are different questions, and
// the assembled V5 vehicle answers the second one **no** - battery sensing is
// not installed (`src/PinMap.h`; the A1 counts this firmware reports are raw ADC
// counts of an unconnected input, not a voltage).
//
// A firmware that collapsed the two would either keep demanding a measured
// `minBatteryV` for a quantity nothing can measure - making production arming
// permanently impossible for a limit the vehicle cannot sense - or relabel the
// floating counts as a measurement. Neither is the truth, so the capability is
// its own type and every reader of the battery voltage states it.
//
// What this type is NOT: a measurement. Nobody measured anything; the fact comes
// from the assembled vehicle's sensing record (`src/PinMap.h`). It is
// deliberately not a `Sourced` value, because there is no `ValueSource` for
// "stated by the team" and promoting it to `Measured` would be exactly the
// fabrication the project's rules exist to stop. It mirrors
// `DriverFaultCapability` (`DriverFault.h`, D32), which states the same kind of
// fact about the external nFAULT line.
//
// What it does NOT change: an `Installed` vehicle still requires a `Measured`
// `minBatteryV` before it may be armed, exactly as before. What changes is only
// whether that requirement is a condition of *this* vehicle.
//
// `Installed` is the zero value on purpose, and it is the conservative one: a
// forgotten assignment (or a default-constructed value) leaves the voltage gate
// *required*, which refuses arming while `minBatteryV` is a placeholder - it
// never silently drops the gate. Only an explicit statement that the circuit
// does not exist may pass validation without a measured voltage.
enum class BatteryVoltageCapability : uint8_t {
    // The battery divider exists on this vehicle and `minBatteryV` must be
    // measured before the vehicle may be armed.
    Installed = 0,
    // No battery-sense circuit exists on this vehicle. Nothing can read the pack
    // voltage, so no level is a measurement, and the arming-time voltage
    // requirement is *not applicable* rather than satisfied.
    NotInstalled = 1,
};

inline const char* batteryVoltageCapabilityName(BatteryVoltageCapability capability) {
    switch (capability) {
        case BatteryVoltageCapability::Installed:
            return "Installed";
        case BatteryVoltageCapability::NotInstalled:
            return "NotInstalled";
    }
    return "?";
}

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

    // What the chain does with these today: maxEffortPermille is consumed through
    // the vehicle layer that derives the regulator's own ceiling from it (see
    // effortCeilingsAgree() below); maxRunDurationS is consumed at run time by the
    // vehicle layer's run-scoped bound (see `RunDurationGuard.h`); and
    // minBatteryV is a capability-aware arming precondition, not a runtime check
    // (`validateSafetyLimits()` below): the voltage comparison it would gate does
    // not exist, because no raw-to-volts conversion is documented on this
    // project.

    // True only when every limit that *this build requires* is backed by recorded
    // measurement.
    //
    // The `capability` argument is what makes the battery-voltage requirement
    // conditional: on a build with no battery-sense circuit (NotInstalled), a
    // measured `minBatteryV` is not a condition of the vehicle and the value may
    // stay a placeholder. The default is `Installed`, the conservative direction -
    // a caller that says nothing gets the strict requirement, so the voltage gate
    // is never dropped by omission. Every other limit is always required.
    //
    // This is a data-completeness check, not an arming decision; deciding when
    // the vehicle may be armed belongs to the arming authority, which reads it
    // through `validateSafetyLimits()`.
    constexpr bool isFullyEstablished(
        BatteryVoltageCapability batteryCapability =
            BatteryVoltageCapability::Installed) const {
        return maxEffortPermille.isMeasured() && maxForwardVelocityMps.isMeasured() &&
               maxReverseVelocityMps.isMeasured() && maxRunDurationS.isMeasured() &&
               (batteryCapability == BatteryVoltageCapability::NotInstalled ||
                minBatteryV.isMeasured());
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

// Single entry point for safety-ceiling validation, and it is capability-aware
// about the battery-voltage limit. Checks provenance first, then finiteness,
// then the Motion invariant (permille is 0..1000), then positivity of the
// remaining ceilings. The vehicle layer must derive SafetyReadiness::
// limitsUsable from this result so SafetyAuthority never has to interpret raw
// Sourced values.
//
// The battery-voltage semantics, mirroring `validateDriverFaultConfig()`:
//
//   Installed + measured minBatteryV    -> ok    (the circuit exists and the
//                                                 limit is a recorded value)
//   Installed + placeholder minBatteryV -> refuse `NotFullyMeasured`
//                                                 (the voltage gate would rest on
//                                                 a guess, and no run may be armed
//                                                 on it)
//   NotInstalled + anything             -> the provenance of `minBatteryV` is
//                                                 **not applicable**: there is no
//                                                 circuit to read, so no voltage
//                                                 is required and none can be
//                                                 measured. `NotInstalled` does
//                                                 not "verify" the value and does
//                                                 not touch it - the placeholder
//                                                 stays a placeholder.
//
// The numeric checks below stay unconditional, and that is deliberate rather than
// an oversight: they are configuration hygiene (a non-finite or non-positive
// ceiling is a malformed value whatever the capability), they are not a claim
// that the voltage was measured, and every production placeholder for this limit
// is finite and positive, so they change nothing on a NotInstalled build. Only
// the *provenance* requirement is relaxed - the one requirement a vehicle with no
// sensing circuit cannot ever meet.
//
// The capability defaults to `Installed` for the same reason the field default is
// `Installed`: a caller that forgets it gets the strict requirement.
inline SafetyLimitsValidation validateSafetyLimits(
    const SafetyLimits& limits,
    BatteryVoltageCapability batteryCapability = BatteryVoltageCapability::Installed) {
    if (!limits.isFullyEstablished(batteryCapability)) {
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
    // An unknown profile direction is not a polarity at all: without this guard
    // the mapping below would fold it into Reverse and let it agree with a
    // reverse effort (S18A/P5). A non-driving controller mode agrees with
    // nothing; `poweredDirectionOf()` already maps one to `None`, which can
    // never equal a known profile direction.
    if (!isKnownMotionDirection(profilePositiveCommandDirection.value)) {
        return false;
    }
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
    // Unknown encodings agree with nothing (S18A/P5): equality alone already
    // refuses a one-sided unknown, and this makes the refusal explicit rather
    // than accidental.
    if (!isKnownMotionDirection(positiveTicksDirection.value) ||
        !isKnownMotionDirection(profilePositiveCommandDirection.value)) {
        return false;
    }
    return positiveTicksDirection.value == profilePositiveCommandDirection.value;
}

// The two travel legs' encodings of the one global polarity fact must agree:
// the positive-command direction is a property of the vehicle, not of the leg
// being travelled, so the forward and reverse `MotionProfileLimits` must name
// the same physical direction (S18A/P5). Without this, each leg validates on
// its own while the legs disagree, and a contradictory reverse leg passes every
// per-module check yet shapes reverse travel the regulator drives forward.
//
// The comparison is over the physical directions the two values name, and
// unknown encodings agree with nothing. Provenance is validated per leg by
// `validateMotionProfileLimits()`; this is the cross-leg semantic agreement
// only, and it is a configuration-build check, not a runtime clamp.
inline bool directionalCommandPolarityAgrees(
    const Sourced<MotionDirection>& forwardPositiveCommandDirection,
    const Sourced<MotionDirection>& reversePositiveCommandDirection) {
    if (!isKnownMotionDirection(forwardPositiveCommandDirection.value) ||
        !isKnownMotionDirection(reversePositiveCommandDirection.value)) {
        return false;
    }
    return forwardPositiveCommandDirection.value == reversePositiveCommandDirection.value;
}

// Pin the provenance contract: a safety ceiling with an unstated source is a
// compile error. This matters most here, because a guessed limit cannot be
// distinguished from a measured one at runtime.
static_assert(!std::is_default_constructible<SafetyLimits>::value,
              "no silent defaults; every value must be supplied with its source");

}  // namespace ev
