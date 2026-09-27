#pragma once

#include <Arduino.h>

#include <MotorDriverPlan.h>

#include "PinMap.h"

// ---------------------------------------------------------------------------
// The motor driver adapter - the only code that writes PH / EN / nSLEEP
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The single place where the three planned driver inputs are written. No other
//   file in this firmware calls `digitalWrite`/`analogWrite` on them, and no
//   module in `lib/` can: `lib/` has no Arduino headers at all. A search for the
//   three pin names (see below) is therefore a complete list of the motor-output
//   writers.
// * The only consumer of `ev::drv8874InputsFor()`. The mapping from an
//   authorized command to input levels is pure logic in
//   `lib/evcore/src/MotorDriverPlan.h`; this adapter only turns those levels into
//   pin writes, so the plan stays host-testable and there is exactly one place
//   the plan and the pins can disagree.
// * Its own fail-safe path. `failSafe()` writes the driver's de-energized
//   levels, and it does not take an `AuthorizedMotorCommand` - nothing about the
//   safe state depends on the rest of the firmware having authorized anything.
//   Startup uses it before any other logic exists, and a broken control cycle
//   uses it instead of a command.
//
// What this module is NOT
//
// * It is not an authorization. It cannot build an `AuthorizedMotorCommand`
//   (the constructor is private to `ev::SafetyAuthority`), so the only powered
//   values that can reach these pins come from the safety boundary.
// * It knows nothing about runs, phases, targets or faults. It applies one
//   command and it de-energizes.
// * It makes no electrical claim. What each level combination does on the
//   vehicle - direction, brake, coast, timing, current - is UNVERIFIED
//   (`VERIFIED_STATE.md` #1, #2, #3) and is the first thing the bench session
//   measures. See `docs/hardware/motor-driver-drv8874.md`.
//
// Static check (from the repository root):
//
//     grep -rn "kMotorPhase\|kMotorEnable\|kMotorSleepBar" src/
//
// must match only `src/PinMap.h` (the definitions) and this file (the writes).

namespace evhw {
namespace motorDriver {

// Configures the three pins and leaves the driver de-energized: nSLEEP low,
// PH low, EN at zero duty. This is the first hardware action the firmware takes,
// before the encoder, the sensors, the serial port or any control state, so the
// driver cannot be enabled by a pin left floating in an undefined state.
//
// It also establishes the EN pin as a PWM output once, here, rather than in the
// control tick: the core's `analogWrite()` creates and caches the PWM channel on
// its first call for a pin, and paying for that inside the first drive command
// would put an allocation inside the control loop's timing.
inline void beginDisabled() {
    // De-energize first, then configure: the sleep line is the driver's enable,
    // and it is driven low before anything else is touched.
    pinMode(kMotorSleepBar, OUTPUT);
    digitalWrite(kMotorSleepBar, LOW);

    pinMode(kMotorPhase, OUTPUT);
    digitalWrite(kMotorPhase, LOW);

    pinMode(kMotorEnable, OUTPUT);
    digitalWrite(kMotorEnable, LOW);

    // Pin the PWM scale explicitly instead of inheriting the core's default, so
    // the duty arithmetic in the plan and the timer's own resolution agree.
    analogWriteResolution(ev::kPlannedPwmResolutionBits);
    analogWrite(kMotorEnable, 0);
}

// Writes the driver's de-energized levels. Safe to call from anywhere, at any
// time, including from a control cycle that has already decided not to trust
// itself: it cannot produce powered output.
inline void failSafe() {
    digitalWrite(kMotorSleepBar, LOW);
    digitalWrite(kMotorPhase, LOW);
    analogWrite(kMotorEnable, 0);
}

// Applies one authorized command. `ev::drv8874InputsFor()` decides the levels;
// this function writes them.
inline void apply(const ev::AuthorizedMotorCommand& command) {
    const ev::Drv8874Inputs levels = ev::drv8874InputsFor(command);
    digitalWrite(kMotorSleepBar, levels.nSleepHigh ? HIGH : LOW);
    digitalWrite(kMotorPhase, levels.phaseHigh ? HIGH : LOW);
    analogWrite(kMotorEnable, ev::pwmCountFromPermille(levels.dutyPermille,
                                                       ev::kPlannedPwmResolutionBits));
}

}  // namespace motorDriver
}  // namespace evhw
