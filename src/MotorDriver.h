#pragma once

#include <Arduino.h>

#include <BenchDrive.h>
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
// `src/bench/` reaches the pins through this file's `applyBench()` and does not
// write them itself, and the production build does not compile `src/bench/` at
// all, so the production image's list of motor-pin writers is still this one
// file.

namespace evhw {
namespace motorDriver {

// ---------------------------------------------------------------------------
// Who owns the pins this pass
// ---------------------------------------------------------------------------
//
// There are two writers to the three driver inputs, and only one of them may
// write in a given pass. The production tick's stage 9 is the default writer; the
// bench layer takes the pins for a pass when its bounded instrument is driving and
// `ev::benchApplyVerdict()` says nothing outranks it (`MotorArbitration.h`).
//
// Why the flag exists rather than "write both, last one wins": a bench command
// written after the tick's de-energized write would put a sleep/wake edge on
// `nSLEEP` once per control period (20 ms). The pin state the driver actually sees
// would then be neither writer's intent, and whether a wake delay is required
// before a drive is an UNVERIFIED fact (`docs/hardware/motor-driver-drv8874.md`) -
// so a 50 Hz sleep/wake toggle is exactly the state no measurement bounds. Setting
// this before the tick means one writer per pass and a pin state that is somebody's
// intent.
//
// `failSafe()` deliberately ignores the flag: a de-energize must always reach the
// pins. Only the authority's ordinary per-pass command yields.
inline bool& motorPinsOwned() {
    static bool pinsOwned = false;
    return pinsOwned;
}

// Hands the pins to the bench layer (`true`) or back to the production tick
// (`false`). Only `src/bench/` ever passes `true`; the production image has no
// caller, so its value is always false there and the tick's write is never
// skipped. The accessor's name is neutral on purpose: the production image must
// contain no bench-named symbol, and `apply()` legitimately reads this flag.
inline void setMotorPinsOwned(bool pinsOwned) { motorPinsOwned() = pinsOwned; }

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
//
// Yields the pins for a pass when the bench layer owns them (`setMotorPinsOwned()`).
// The authority's command is still what the tick recorded in its telemetry
// snapshot; only the pin write is skipped, so that the pass has exactly one writer
// instead of two writes one instruction apart. The write resumes on the first pass
// the bench no longer owns the driver.
inline void apply(const ev::AuthorizedMotorCommand& command) {
    if (motorPinsOwned()) {
        return;
    }
    const ev::Drv8874Inputs levels = ev::drv8874InputsFor(command);
    digitalWrite(kMotorSleepBar, levels.nSleepHigh ? HIGH : LOW);
    digitalWrite(kMotorPhase, levels.phaseHigh ? HIGH : LOW);
    analogWrite(kMotorEnable, ev::pwmCountFromPermille(levels.dutyPermille,
                                                       ev::kPlannedPwmResolutionBits));
}

// Applies one bounded bench command (B2).
//
// This is a separate entry point, and it takes a separate type, on purpose. The
// production path can reach it in exactly one way - by calling it - and the only
// caller in the repository is `src/bench/`, which the production build does not
// compile (`platformio.ini`). A production module cannot build a
// `BenchDriveOutput` (its constructor is private to `ev::BenchDrive`), so there is
// no type it could pass to this function by accident.
//
// The levels come from the same pure table as every other write here, so the plan
// still has exactly one definition. What the bench layer owns is different: when it
// is allowed to be called at all, and that is `ev::benchApplyVerdict()`
// (`MotorArbitration.h`) - a latched fault, a violated tick order, an armed run or
// a run under way all outrank the bench, and `!armed()` alone is *not* that test.
//
// Called after the control tick, and only while the bench layer holds ownership
// for the pass (`setMotorPinsOwned()`), so this write is the pass's only write to the
// pins. Continuing to hold ownership is what keeps a bench command from being
// toggled off and on once per control period by the tick's own write; releasing it
// - which happens on the pass after the instrument stops driving, and immediately
// if the tick ran out of order - hands the driver back to the production tick,
// whose next write de-energizes a bench command the bench code can no longer
// reach.
inline void applyBench(const ev::BenchDriveOutput& output) {
    const ev::Drv8874Inputs levels =
        ev::drv8874InputsForRequest(output.mode(), output.magnitudePermille());
    digitalWrite(kMotorSleepBar, levels.nSleepHigh ? HIGH : LOW);
    digitalWrite(kMotorPhase, levels.phaseHigh ? HIGH : LOW);
    analogWrite(kMotorEnable, ev::pwmCountFromPermille(levels.dutyPermille,
                                                       ev::kPlannedPwmResolutionBits));
}

}  // namespace motorDriver
}  // namespace evhw
