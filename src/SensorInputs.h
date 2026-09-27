#pragma once

#include <Arduino.h>

#include "PinMap.h"

// ---------------------------------------------------------------------------
// Raw input acquisition - the other inputs the control tick samples
// ---------------------------------------------------------------------------
//
// What this module is
//
// * A thin read of the planned inputs: two analog channels (battery and the
//   driver's current-sense output), the driver's nFAULT line, and the start
//   trigger. It calls `analogRead`/`digitalRead` and returns what they returned.
// * Deliberately raw. There is no divider ratio, no shunt value, no ADC reference
//   relationship and no active-level table recorded anywhere in this project, so
//   there is no raw-to-engineering-unit conversion here at all
//   (`SOFTWARE_ARCHITECTURE.md` section 12, `VERIFIED_STATE.md` #1, #7). A number
//   this module returns is an ADC count or a logic level, and nothing else.
//
// What this module is NOT
//
// * It does not decide what an input means. Whether the nFAULT line's low level is
//   a fault, whether the trigger's low level is "pressed", and what either line
//   does when nothing is connected are all waiting on the wiring check and on a
//   measured truth table. The control cycle records the raw level and, until that
//   measurement exists, interprets nothing.
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
    bool driverFaultBarLow;   // planned DRV8874 nFAULT: true when the line is low
    bool startTriggerActive;  // planned trigger: true when the line is low (pull-up)
};

// Configures the input pins. The two analog pins are read without pinMode; the
// two digital inputs are configured with pull-ups, which is the bench plan and
// itself an UNVERIFIED wiring assumption.
inline void begin() {
    pinMode(kMotorFaultBar, INPUT_PULLUP);
    pinMode(kStartTrigger, INPUT_PULLUP);
}

// Reads the four raw inputs. Cheap, non-blocking and interrupt-free.
inline RawSample sample() {
    RawSample raw;
    raw.batteryCounts = static_cast<uint16_t>(analogRead(kBatterySense));
    raw.currentCounts = static_cast<uint16_t>(analogRead(kCurrentSense));
    raw.driverFaultBarLow = digitalRead(kMotorFaultBar) == LOW;
    raw.startTriggerActive = digitalRead(kStartTrigger) == LOW;
    return raw;
}

}  // namespace sensorInputs
}  // namespace evhw
