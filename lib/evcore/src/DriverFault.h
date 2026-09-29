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
// * It does not sample anything. It takes a level that was already read.
// * It is not a fault response. `FaultCode` is identity; what the motor should do
//   about it is `FaultStopPolicy` in `SafetyAuthority.h`, and the authority
//   latches the fault this module names exactly as it latches any other.
// * It does not decide severity, timing, or whether the driver latched the
//   condition itself. H1B establishes the level for each line state and H9 injects
//   a real fault; both are recorded in `docs/bringup/`, and neither may be
//   inferred from this header.

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
// that never happened.
inline bool driverFaultPolarityVerified(const Sourced<DriverFaultPolarity>& polarity) {
    return polarity.isMeasured() && polarity.value != DriverFaultPolarity::Unverified;
}

// Single entry point for this module's configuration validation. The
// configuration-build check that gates arming reads this, so a vehicle with an
// unverified fault polarity cannot be armed - a driver fault would be invisible.
inline DriverFaultConfigValidation validateDriverFaultConfig(
    const Sourced<DriverFaultPolarity>& polarity) {
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

// The fault identity for one cycle's reading.
//
// Returns `FaultCode::None` while the polarity is unverified: the level is still
// sampled, counted and reported as a raw level by the vehicle layer, but no fault
// is invented from it. Once the polarity is measured (H1B) this returns the
// driver-fault identity, which the authority latches exactly as it latches any
// other fault, and which H9's injection step then observes end to end.
inline FaultCode observedDriverFault(bool levelLow, const Sourced<DriverFaultPolarity>& polarity) {
    if (!driverFaultPolarityVerified(polarity)) {
        return FaultCode::None;
    }
    return driverFaultLevelIndicatesFault(levelLow, polarity.value) ? FaultCode::DriverFaultBar
                                                                   : FaultCode::None;
}

}  // namespace ev
