#pragma once

#include <Arduino.h>

#include <DriverFault.h>

#include "PinMap.h"
#include "VehicleConfig.h"

// ---------------------------------------------------------------------------
// Raw input acquisition - the other inputs the control tick samples
// ---------------------------------------------------------------------------
//
// What this module is
//
// * A thin read of the input lines this map names: the start trigger and the two
//   planned analog channels (battery and the driver's current-sense output -
//   **not installed on the assembled vehicle**; `PinMap.h`). It calls
//   `analogRead`/`digitalRead` and returns what they returned.
// * Deliberately raw. There is no divider ratio, no shunt value, no ADC reference
//   relationship and no active-level table recorded anywhere in this project, so
//   there is no raw-to-engineering-unit conversion here at all
//   (`SOFTWARE_ARCHITECTURE.md` section 12, `VERIFIED_STATE.md` #1, #7). A number
//   this module returns is an ADC count or a logic level, and nothing else.
// * **Capability-aware about the driver's fault line.** The external DRV8874
//   nFAULT signal is *not installed* on this vehicle
//   (`vehicleConfig::kPlannedDriverFaultCapability`), and the V5 pin map has no
//   pin for it (`kMotorFaultBar` is `kNoPin`), so this module does not configure
//   it and does not sample it. That is the whole point: a floating pin is not a
//   reading, and reporting one as a driver-fault level would claim a signal the
//   vehicle does not have. `RawSample` therefore carries the capability with the
//   level, and `driverFaultBarLow` is meaningful only when the capability is
//   `Installed`.
//
// What this module is NOT
//
// * It does not decide what an input means. Whether a level on a *wired* nFAULT
//   line is a fault, whether the trigger's low level is "pressed", and what either
//   line does when nothing is connected are all waiting on the wiring check and on
//   a measured truth table. The control cycle records the raw level and interprets
//   it from configuration (`lib/evcore/src/DriverFault.h`), which refuses to claim
//   a fault while the line's polarity is unverified, so nothing is interpreted
//   here and nothing is interpreted from a guess anywhere else either.
// * It does not run in an interrupt. Sampling analog channels belongs to the
//   control tick, which is also the only place its results are consumed.
// * It does not carry a timestamp. The cycle has one clock reading and passes it
//   to the telemetry snapshot, so a second stamp here could only disagree.

namespace evhw {
namespace sensorInputs {

// One cycle's raw readings. No field is an engineering quantity.
struct RawSample {
    uint16_t batteryCounts;   // planned battery divider, raw ADC counts
    uint16_t currentCounts;   // planned driver current-sense output, raw ADC counts
    // Whether an external nFAULT line exists to read. Carried with the level so a
    // consumer cannot mistake "not installed" for "read high": with
    // `NotInstalled`, `driverFaultBarLow` below is not a reading of anything and
    // must not be consumed as one.
    ev::DriverFaultCapability driverFaultCapability;
    bool driverFaultBarLow;   // true when a *wired* nFAULT line is low; undefined when not installed
    bool startTriggerActive;  // planned trigger: true when the line is low (pull-up)
};

// True when this build has an external driver-fault line to sample. Compile-time
// constant, so the pin is configured and read only on a vehicle that wires it.
constexpr bool driverFaultLineInstalled() {
    return vehicleConfig::kPlannedDriverFaultCapability ==
           ev::DriverFaultCapability::Installed;
}

// A build that flips the capability to `Installed` must first name a real pin:
// on this map the nFAULT line has none (`kMotorFaultBar` is `kNoPin`), and the
// two edits - the pin and the capability - go together or not at all. The
// refusal is at compile time, so the half-edit cannot reach a board.
static_assert(!driverFaultLineInstalled() || kMotorFaultBar != kNoPin,
              "the nFAULT capability is Installed but kMotorFaultBar is kNoPin: "
              "assign the line a pin in src/PinMap.h first");

// Configures the input pins. The two analog pins are read without pinMode; the
// digital inputs are configured with pull-ups, which is the bench plan and itself
// an UNVERIFIED wiring assumption. The nFAULT pin is deliberately left
// unconfigured when the vehicle does not have the line: configuring an
// INPUT_PULLUP on a pin that goes nowhere would not make it an input, and would
// leave a pull-up enabled on a pin with no connection for no reason.
inline void begin() {
    if (driverFaultLineInstalled()) {
        pinMode(kMotorFaultBar, INPUT_PULLUP);
    }
    pinMode(kStartTrigger, INPUT_PULLUP);
}

// Reads this vehicle's raw inputs. Cheap, non-blocking and interrupt-free.
//
// The nFAULT pin is read only when the line is installed. On this vehicle it is
// not, so no `digitalRead()` of it happens at all: the pin is not configured, its
// floating level is never sampled, and the sample carries `NotInstalled` with a
// level that is explicitly not a reading. That is a deliberate difference from
// "sampled it and it read high" - see `lib/evcore/src/DriverFault.h`.
inline RawSample sample() {
    RawSample raw;
    raw.batteryCounts = static_cast<uint16_t>(analogRead(kBatterySense));
    raw.currentCounts = static_cast<uint16_t>(analogRead(kCurrentSense));
    raw.driverFaultCapability = vehicleConfig::kPlannedDriverFaultCapability;
    raw.driverFaultBarLow = driverFaultLineInstalled() && digitalRead(kMotorFaultBar) == LOW;
    raw.startTriggerActive = digitalRead(kStartTrigger) == LOW;
    return raw;
}

}  // namespace sensorInputs
}  // namespace evhw
