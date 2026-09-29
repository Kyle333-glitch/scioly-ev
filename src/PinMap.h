#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Provisional pin map - a PLACEHOLDER, not a wiring record
// ---------------------------------------------------------------------------
//
// **Every pin below is UNVERIFIED.** There is no wiring table in this project
// yet, so nothing here may be treated as a fact about the vehicle
// (`VERIFIED_STATE.md` #1). The names and numbers exist so the hardware boundary
// can be written at all, and they are centralized here so a wiring change is one
// edit in one file instead of a search through the firmware.
//
// How the numbers were chosen (a software fact, not a vehicle fact):
//
//   * each pin was checked against the pin table of the installed Renesas
//     framework for the Nano R4 (`variants/NANOR4/`) for the capability the role
//     needs - an interrupt channel for the encoder A channel, a PWM channel for
//     the driver's EN input, an analog channel for the two sense inputs;
//   * `attachInterrupt()` on this core silently attaches nothing when the pin has
//     no interrupt channel, so the encoder A pin must have one; which pins do is
//     visible in the framework's own pin table, and D2/D3 both do;
//   * A0 is the DAC pin on this board, so it is deliberately not used for a
//     plain analog read.
//
// What is still unknown, and is measured before the first powered test:
//
//   * whether the vehicle is wired this way at all (continuity check);
//   * the active level of every input below (nFAULT, the trigger, the encoder
//     channels' idle levels);
//   * the logic levels the driver sees, and which driver input combination
//     produces which direction (`docs/hardware/motor-driver-drv8874.md`).
//
// Nothing in this file may be cited as evidence for any of those.

namespace evhw {

// --- Encoder ---------------------------------------------------------------
// A-channel CHANGE interrupt plus the B level read at the transition, decoded by
// the existing x2 decoding module (`ev::QuadratureDecoder`). The idle levels and
// the pull requirements are UNVERIFIED wiring facts.
constexpr uint8_t kEncoderA = 2U;  // planned: D2, interrupt channel
constexpr uint8_t kEncoderB = 3U;  // planned: D3, read as a level only

// --- Motor driver: planned DRV8874 PH/EN interface -------------------------
// The only code allowed to write these is `MotorDriver.h`.
//
// `kMotorEnable` is the firmware's **only** PWM consumer. `analogWriteResolution()`
// (`MotorDriver.h`) is a core-wide setting, not a per-pin one, and the timer this
// pin lands on is not recorded - H1A has not yet measured the frequency it
// produces. So: **no second `analogWrite()` consumer, and no second
// `analogWriteResolution()` call, may be added on this board until H1A test 6
// records the switching frequency at `EN`.** A second consumer on the same timer
// silently changes the duty that reaches the driver, and nothing in this firmware
// would report it.
constexpr uint8_t kMotorPhase = 9U;        // planned DRV8874 PH
constexpr uint8_t kMotorEnable = 6U;       // planned DRV8874 EN, PWM-capable, the only PWM consumer
constexpr uint8_t kMotorSleepBar = 7U;     // planned DRV8874 nSLEEP, active low

// --- Inputs ----------------------------------------------------------------
// Planned DRV8874 nFAULT, active low and open-drain on the driver side. The
// internal pull-up is the plan for the bench; whether the board has its own
// pull-up, and what the line does when the driver is asleep, is UNVERIFIED.
constexpr uint8_t kMotorFaultBar = 4U;

// Planned start-go trigger. Its active level and its debounce are UNVERIFIED;
// the plan below reads it with the internal pull-up and treats LOW as active.
constexpr uint8_t kStartTrigger = 10U;

// Planned analog inputs. Both are read as raw counts only: no divider ratio, no
// shunt value and no ADC reference relationship is documented in this project,
// so no raw-to-engineering-unit conversion exists (`SOFTWARE_ARCHITECTURE.md`
// section 12, `VERIFIED_STATE.md` #7).
constexpr uint8_t kBatterySense = A1;
constexpr uint8_t kCurrentSense = A2;

}  // namespace evhw
