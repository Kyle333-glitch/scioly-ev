#pragma once

#include "Units.h"
#include "VehicleState.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The driver's nFAULT line - an interpretation that refuses to guess
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The one place the project decides what the planned driver's fault output
//   *means*. The raw level is sampled by the vehicle layer and reported as a raw
//   level; whether that level is a fault claim, and which polarity it uses, is a
//   decision, and this is where the decision lives.
// * A refusal by default. The polarity is a measured fact about the vehicle and
//   it is UNVERIFIED (`VERIFIED_STATE.md` #1, #2, #7): nobody has put a scope or
//   a meter on the line, and the driver is not even wired yet. An interpretation
//   invented now would be exactly the kind of assumption the project's rules
//   exist to stop: a fault input wired to the wrong polarity turns a healthy
//   driver into a latched fault, and an inverted one turns a real fault into a
//   log entry nobody acts on. So `DriverFaultPolarity::Unverified` - the zero
//   value - interprets nothing, and every level is reported as a level.
//
// What it is not
//
// * It does not sample anything. It takes a level that was already read - and
//   the capability, because a level from a signal this vehicle does not have is
//   not a reading at all (`DriverFaultCapability` below).
// * It is not a fault response. `FaultCode` is identity; what the motor should do
//   about it is `FaultStopPolicy` in `SafetyAuthority.h`, and the authority
//   latches the fault this module names exactly as it latches any other.
// * It does not decide severity, timing, or whether the driver latched the
//   condition itself. H1B establishes the level for each line state and H9 injects
//   a real fault; both are recorded in `docs/bringup/`, and neither may be
//   inferred from this header. On the assembled vehicle the external line is not
//   installed, so those measurements are not merely owed - they have no subject,
//   and the capability type below is what says so.

// Whether this vehicle's external nFAULT signal exists at all.
//
// This is a *capability* fact, separate from the polarity fact below, and it is
// separate on purpose: "which level means fault" and "is there a line to read"
// are different questions, and the assembled vehicle answers the second one
// **no** - the DRV8874's nFAULT output is not wired to the MCU. A firmware that
// collapsed the two would either keep demanding a polarity measurement for a
// line that does not exist, or read a floating pin and report its level as if it
// were driver-fault evidence. Neither is the truth, so the capability is its own
// type and every reader of the line states it.
//
// What this type is NOT: a measurement. Nobody measured anything; the fact comes
// from the assembled vehicle's wiring record (`src/PinMap.h` states it for the
// build). It is deliberately not a `Sourced` value, because there is no
// `ValueSource` for "stated by the team" and promoting it to `Measured` would be
// exactly the fabrication the project's rules exist to stop.
//
// What it does NOT change: the DRV8874's on-chip protection (overcurrent,
// thermal, undervoltage) is untouched and still exists. What changes is only
// whether the MCU can observe the driver's fault output.
//
// `Installed` is the zero value on purpose, and it is the conservative one: a
// forgotten assignment (or a default-constructed value) leaves the line
// *required* and its polarity unverified, which refuses arming - it never
// silently removes the external fault gate. Only an explicit statement that the
// signal does not exist may pass the configuration check without a polarity
// measurement.
enum class DriverFaultCapability : uint8_t {
    // The external nFAULT line exists on this vehicle and its polarity must be
    // measured before the vehicle may be armed.
    Installed = 0,
    // The external nFAULT line does not exist on this vehicle. No level is read,
    // no polarity is required, and external fault telemetry is unavailable.
    NotInstalled = 1,
};

inline const char* driverFaultCapabilityName(DriverFaultCapability capability) {
    switch (capability) {
        case DriverFaultCapability::Installed:
            return "Installed";
        case DriverFaultCapability::NotInstalled:
            return "NotInstalled";
    }
    return "?";
}

// How a level on the planned nFAULT line is to be read.
//
// `Unverified` is the zero value on purpose: a default-constructed configuration
// (and a forgotten assignment) interprets nothing rather than assuming the
// datasheet's expectation.
enum class DriverFaultPolarity : uint8_t {
    // Nobody has measured the line. No level is interpreted as a fault.
    Unverified = 0,
    // A low level is the driver reporting a fault (the planned wiring: open-drain
    // output with a pull-up, so the line is high when healthy).
    ActiveLow = 1,
    // A high level is the driver reporting a fault. Only a measurement can
    // establish this; it exists so a bench result has somewhere to go that is not
    // "rewrite the sign in the middle of the control tick".
    ActiveHigh = 2,
};

inline const char* driverFaultPolarityName(DriverFaultPolarity polarity) {
    switch (polarity) {
        case DriverFaultPolarity::Unverified:
            return "Unverified";
        case DriverFaultPolarity::ActiveLow:
            return "ActiveLow";
        case DriverFaultPolarity::ActiveHigh:
            return "ActiveHigh";
    }
    return "?";
}

enum class DriverFaultConfigError : uint8_t {
    None = 0,
    // The value is not Measured, or is Measured but still carries
    // `DriverFaultPolarity::Unverified`. Either way no level may be interpreted.
    PolarityNotVerified,
};

struct DriverFaultConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    DriverFaultConfigError error;
};

// True when the polarity has been measured and names a polarity. A measured
// `Unverified` is refused: "measured that it is unmeasured" is a contradiction,
// and accepting it would let a promotion to Measured stand in for a measurement
// that never happened. A cast-in unknown value is refused explicitly rather
// than accepted by "not Unverified" (S18A/P5).
inline bool driverFaultPolarityVerified(const Sourced<DriverFaultPolarity>& polarity) {
    if (!polarity.isMeasured()) {
        return false;
    }
    return polarity.value == DriverFaultPolarity::ActiveLow ||
           polarity.value == DriverFaultPolarity::ActiveHigh;
}

// Single entry point for this module's configuration validation, and it is
// capability-aware. The configuration-build check that gates arming reads this.
//
// The semantics, in full:
//
//   Installed + verified polarity    -> ok    (the line is read, its meaning known)
//   Installed + unverified polarity  -> refuse `PolarityNotVerified`
//                                             (a driver fault would be invisible)
//   NotInstalled + anything          -> ok    (polarity verification is not
//                                             applicable: there is no line to
//                                             interpret, so there is no polarity
//                                             to measure and none is required)
//
// `NotInstalled` does not "verify" anything and does not touch the polarity
// value: the shipped polarity stays `Unverified`, and this function simply
// reports that its verification is not a condition of this vehicle. The refusal
// on `Installed` is unchanged from what this check has always been.
inline DriverFaultConfigValidation validateDriverFaultConfig(
    DriverFaultCapability capability, const Sourced<DriverFaultPolarity>& polarity) {
    if (capability == DriverFaultCapability::NotInstalled) {
        return {true, DriverFaultConfigError::None};
    }
    if (!driverFaultPolarityVerified(polarity)) {
        return {false, DriverFaultConfigError::PolarityNotVerified};
    }
    return {true, DriverFaultConfigError::None};
}

// Does this raw level mean the driver is reporting a fault?
//
// `levelLow` is the raw level as sampled (`true` when the line reads low).
// An unverified polarity answers false for every level: the honest answer to
// "is this a fault?" when nobody has measured the line is "this build does not
// claim to know", and the caller keeps reporting the raw level.
inline bool driverFaultLevelIndicatesFault(bool levelLow, DriverFaultPolarity polarity) {
    switch (polarity) {
        case DriverFaultPolarity::ActiveLow:
            return levelLow;
        case DriverFaultPolarity::ActiveHigh:
            return !levelLow;
        case DriverFaultPolarity::Unverified:
            return false;
    }
    return false;
}

// The fault identity for one cycle's reading. The one runtime entry point, and it
// takes the capability because there is no honest way to interpret a level
// without knowing whether the signal exists.
//
//   NotInstalled: `FaultCode::None` for every level, unconditionally. There is no
//   line, so there is nothing to interpret - and this is deliberately *not* the
//   same as "the line read high": the caller must not feed a floating pin here
//   and call the result a healthy reading (`SensorInputs.h` does not sample the
//   pin at all while the capability is `NotInstalled`).
//
//   Installed, polarity unverified: `FaultCode::None` while the level is still
//   sampled, counted and reported as a raw level by the vehicle layer, but no
//   fault is invented from it.
//
//   Installed, polarity verified (H1B): the driver-fault identity, which the
//   authority latches exactly as it latches any other fault, and which H9's
//   injection step then observes end to end.
inline FaultCode observedDriverFault(DriverFaultCapability capability, bool levelLow,
                                     const Sourced<DriverFaultPolarity>& polarity) {
    if (capability == DriverFaultCapability::NotInstalled) {
        return FaultCode::None;
    }
    if (!driverFaultPolarityVerified(polarity)) {
        return FaultCode::None;
    }
    return driverFaultLevelIndicatesFault(levelLow, polarity.value) ? FaultCode::DriverFaultBar
                                                                   : FaultCode::None;
}

}  // namespace ev
