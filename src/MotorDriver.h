#pragma once

#include <Arduino.h>

#include <BenchDrive.h>
#include <ImmediateSequence.h>
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

// ---------------------------------------------------------------------------
// What the pins are actually being held at (P-H0W Step 3)
// ---------------------------------------------------------------------------
//
// The duty this adapter last applied. It exists because a PWM duty cannot be read
// back from a pin: `digitalRead` on the EN pin reports the level it is being held
// at, which at 0 duty is LOW and at any other duty is toggling. So the readback
// below pairs the **observed** pin levels with the **written** duty and says which
// is which, rather than presenting the intent as a measurement.
inline uint16_t& lastDutyPermilleWritten() {
    static uint16_t duty = 0U;
    return duty;
}

namespace detail {

// The PH level this adapter last wrote. The single write path below needs it to
// apply one rule: PH may only move when the drive level has just been commanded
// to zero and the driver de-energized.
inline bool& lastPhaseHighWritten() {
    static bool high = false;
    return high;
}

// The full driver-input combination this adapter last wrote, and whether it
// has written anything yet. The single write path below skips the pin writes
// when the requested combination is identical to what is already on the pins:
// the main loop commands the output every pass, and rewriting an unchanged
// nSLEEP level and PWM duty once per pass is a glitch risk with no benefit -
// the pins hold their levels between writes. The skip is a pure output
// optimization: it never changes which combination is commanded, and any
// *change* (including every direction change, which moves PH) still goes
// through the de-energize-before-PH rule below.
struct LastWrittenLevels {
    bool valid = false;
    bool nSleepHigh = false;
    bool phaseHigh = false;
    uint16_t dutyPermille = 0U;
};

inline LastWrittenLevels& lastWrittenLevels() {
    static LastWrittenLevels levels;
    return levels;
}

inline void invalidateWrittenLevels() {
    lastWrittenLevels().valid = false;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The one write path - and the PH-before-EN ordering rule
// ---------------------------------------------------------------------------
//
// Every write of the three driver inputs goes through here, and the rule it
// enforces is the one P-H1A's required work asks to verify: PH is not changed
// while an energized drive output would make that unsafe.
//
// Without the rule, a command that goes from a powered drive straight to a stop
// writes PH before EN: the writes happen in level order (nSLEEP, PH, EN), so a
// `DriveForward` -> `Brake` transition - which the run's own FinalApproach ->
// FinalBrake handoff really produces - changes PH low while EN still carries
// the previous duty. That is an opposite-direction pulse the authority never
// authorized, and it is the one direction change the safety boundary cannot
// refuse, because it is created by write order rather than by a command.
//
// With the rule, a PH change first drives nSLEEP low and commands EN to zero
// duty, then writes PH, and only then restores the target's nSLEEP and EN. The
// rule is an ordering, not a timed guarantee: the driver's sleep-to-output-off
// interval (`tSLEEP`) is not measured in this project, so the interval between
// the de-energize write and the PH write is bounded by the part and is a
// measurement item for H1A/H3 rather than something this comment may claim.
inline void writeLevels(const ev::Drv8874Inputs& levels) {
    detail::LastWrittenLevels& last = detail::lastWrittenLevels();
    if (last.valid && last.nSleepHigh == levels.nSleepHigh &&
        last.phaseHigh == levels.phaseHigh && last.dutyPermille == levels.dutyPermille) {
        // Already commanding exactly this combination: the pins hold it, so
        // there is nothing to write. The duty record is refreshed so the
        // readback below still reports the intent.
        lastDutyPermilleWritten() = levels.dutyPermille;
        return;
    }
    if (levels.phaseHigh != detail::lastPhaseHighWritten()) {
        // De-energize before the direction input moves: nSLEEP low, then EN
        // commanded to zero duty, then PH.
        digitalWrite(kMotorSleepBar, LOW);
        analogWrite(kMotorEnable, 0);
        digitalWrite(kMotorPhase, levels.phaseHigh ? HIGH : LOW);
        detail::lastPhaseHighWritten() = levels.phaseHigh;
    }
    digitalWrite(kMotorSleepBar, levels.nSleepHigh ? HIGH : LOW);
    analogWrite(kMotorEnable, ev::pwmCountFromPermille(levels.dutyPermille,
                                                       ev::kPlannedPwmResolutionBits));
    lastDutyPermilleWritten() = levels.dutyPermille;
    last.nSleepHigh = levels.nSleepHigh;
    last.phaseHigh = levels.phaseHigh;
    last.dutyPermille = levels.dutyPermille;
    last.valid = true;
}

// The three driver inputs as the MCU itself sees them, plus the rule's verdict.
//
// Read through `digitalRead()` -> `R_IOPORT_PinRead()`: the level the MCU holds
// each pin at, not an external voltage measurement at the header. That distinction
// is the whole reason H0W can record this and still hand the electrical statement
// to H1A.
struct PinLevels {
    bool nSleepHigh;
    bool phaseHigh;
    bool enableHigh;  // meaningful at 0 duty; above that the pin is switching
    uint16_t dutyPermilleWritten;  // intent, not an observation
    bool deEnergized;  // the plan's own rule, over the observed levels + that duty
};

inline PinLevels pinLevels() {
    const bool nSleepHigh = digitalRead(kMotorSleepBar) == HIGH;
    const bool phaseHigh = digitalRead(kMotorPhase) == HIGH;
    const bool enableHigh = digitalRead(kMotorEnable) == HIGH;
    const uint16_t duty = lastDutyPermilleWritten();
    // The same pure predicate that judges a command, applied to what is on the
    // pins instead of to what was asked for: it requires nSLEEP low and a zero
    // duty, so a pin left driven the wrong way fails the check rather than being
    // described as safe.
    const ev::Drv8874Inputs asInputs{nSleepHigh, phaseHigh, duty};
    PinLevels levels{};
    levels.nSleepHigh = nSleepHigh;
    levels.phaseHigh = phaseHigh;
    levels.enableHigh = enableHigh;
    levels.dutyPermilleWritten = duty;
    levels.deEnergized = ev::drv8874InputsAreDeEnergized(asInputs);
    return levels;
}

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
    // Force the full write below through even if the cache holds the same
    // de-energized combination from before a reset: after a reset the
    // physical pins must be driven, not assumed.
    detail::invalidateWrittenLevels();
    // De-energize first, then configure: the sleep line is the driver's enable,
    // and it is driven low before anything else is touched.
    pinMode(kMotorSleepBar, OUTPUT);
    digitalWrite(kMotorSleepBar, LOW);

    pinMode(kMotorPhase, OUTPUT);
    pinMode(kMotorEnable, OUTPUT);

    // Pin the PWM scale explicitly instead of inheriting the core's default, so
    // the duty arithmetic in the plan and the timer's own resolution agree.
    analogWriteResolution(ev::kPlannedPwmResolutionBits);

    // The de-energized levels, through the one write path every other write
    // uses. Drive PH low explicitly here and sync `lastPhaseHighWritten()` so
    // the physical pin is guaranteed LOW even when `lastPhaseHighWritten()` was
    // already false at startup.
    digitalWrite(kMotorPhase, LOW);
    detail::lastPhaseHighWritten() = false;
    writeLevels(ev::Drv8874Inputs{false, false, 0U});
}

// Writes the driver's de-energized levels. Safe to call from anywhere, at any
// time, including from a control cycle that has already decided not to trust
// itself: it cannot produce powered output.
inline void failSafe() {
    writeLevels(ev::Drv8874Inputs{false, false, 0U});
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
    writeLevels(ev::drv8874InputsFor(command));
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
    writeLevels(ev::drv8874InputsForRequest(output.mode(), output.magnitudePermille()));
}

// Applies one bounded open-loop sequence output from `ev::ImmediateSequenceController`
// (`src/main.cpp`, the default `env:nano_r4` standalone movement image).
//
// `ImmediateDriveOutput` has a private constructor accessible only to
// `ev::ImmediateSequenceController`, so only the immediate state machine can
// construct a command for this entry point. All writes go through `writeLevels()`
// and `ev::drv8874InputsForRequest()`, preserving the DRV8874 PH/EN/nSLEEP
// direction-change de-energize ordering.
inline void applyImmediate(const ev::ImmediateDriveOutput& output) {
    writeLevels(ev::drv8874InputsForRequest(output.mode(), output.magnitudePermille()));
}

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
// ---------------------------------------------------------------------------
// TEMPORARY - H1A electrical-gate diagnostic (remove after the H1A evidence)
// ---------------------------------------------------------------------------
//
// Holds the plan's Brake level combination - the driver awake (nSLEEP high)
// with EN at zero duty - so the H1A session can measure PH/EN/nSLEEP with a
// meter while the motor is disconnected. It goes through `writeLevels` like
// every other write, so the PH rule above still holds, and its levels come from
// the same planned table (`ev::drv8874InputsForRequest`) as every authorized
// command, so measuring this measures the plan's Brake mapping.
//
// It commands no effort and no direction: EN is at zero duty, so it cannot
// drive. It is compiled only into `env:bench` (the build define), the console
// verb that reaches it lives in `src/bench/`, and `src/main.cpp` refuses to
// compile the vehicle image if the define ever leaks.
//
// It is not a shipping capability. The evidence it exists for is the H1A
// capture, and it is removed once that is recorded.
inline void applyH1aAwakeEnLow() {
    writeLevels(ev::drv8874InputsForRequest(ev::MotorMode::Brake, 0U));
}
#endif  // EV_BENCH_H1A_DIAGNOSTIC

}  // namespace motorDriver
}  // namespace evhw
