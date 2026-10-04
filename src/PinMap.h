#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pin map - the team's stated V5 harness map, not a verified wiring record
// ---------------------------------------------------------------------------
//
// The numbers below are the team's statement of what the vehicle's V5 harness
// connects to the Nano R4. They are a statement about the intended/as-built
// harness, **not a measurement**: no continuity check has been performed, so
// every pin below is still UNVERIFIED and `VERIFIED_STATE.md` #1 stays
// UNVERIFIED. Nothing in this file may be cited as a wiring fact.
//
// Why each pin can carry its role (checked against the pin table of the
// installed Renesas framework for the Nano R4, core 1.9.0, `variants/NANOR4/`;
// these are framework facts, not vehicle facts):
//
//   * encoder A (D2 / port P105) needs an interrupt channel - P105 carries
//     external interrupt channel 0, and `attachInterrupt()` on this core silently
//     attaches nothing when the pin has no channel, so the encoder A pin must
//     keep one;
//   * encoder B (D4 / P103) is only ever read as a level by the x2 decoder -
//     P103 has no interrupt entry in this core's table, so a later attempt to
//     attach a B-channel interrupt to D4 would silently do nothing; D2 is the
//     channel that has one;
//   * the driver's EN (D9 / P303) needs a PWM channel - P303 carries PWM channel
//     7-B, and EN is the firmware's only `analogWrite()` consumer;
//   * PH (D8 / P304), nSLEEP (D10 / P112) and the trigger (D12 / P110) are plain
//     digital I/O; D8 does list a PWM channel in the table and is still
//     deliberately used as a plain output;
//   * A0 is the DAC pin on this board, so it is deliberately not used for a
//     plain analog read.
//
// What is still unknown, and is measured before the first powered test:
//
//   * whether the vehicle is wired this way at all (continuity check);
//   * the active level of every input below (the trigger, the encoder channels'
//     idle levels; the nFAULT line is not installed on this vehicle - see the
//     driver-fault capability note under "Inputs");
//   * the logic levels the driver sees, and which driver input combination
//     produces which direction (`docs/hardware/motor-driver-drv8874.md`).

namespace evhw {

// --- Encoder ---------------------------------------------------------------
// A-channel CHANGE interrupt plus the B level read at the transition, decoded by
// the existing x2 decoding module (`ev::QuadratureDecoder`). The idle levels and
// the pull requirements are UNVERIFIED wiring facts.
constexpr uint8_t kEncoderA = 2U;  // V5 harness: D2, interrupt channel 0
constexpr uint8_t kEncoderB = 4U;  // V5 harness: D4, read as a level only

// --- Motor driver: DRV8874 PH/EN interface --------------------------------
// The only code allowed to write these is `MotorDriver.h`.
//
// PMODE is tied to GND in the stated V5 harness, which selects the driver's
// PH/EN control mode at power-up. It is not an MCU pin: the firmware neither
// drives nor reads it, so there is no constant for it here.
//
// `kMotorEnable` is the firmware's **only** PWM consumer. `analogWriteResolution()`
// (`MotorDriver.h`) is a core-wide setting, not a per-pin one, and the timer this
// pin lands on is not recorded - H1A has not yet measured the frequency it
// produces. So: **no second `analogWrite()` consumer, and no second
// `analogWriteResolution()` call, may be added on this board until H1A test 6
// records the switching frequency at `EN`.** A second consumer on the same timer
// silently changes the duty that reaches the driver, and nothing in this firmware
// would report it.
constexpr uint8_t kMotorPhase = 8U;      // V5 harness: DRV8874 PH, plain digital output
constexpr uint8_t kMotorEnable = 9U;     // V5 harness: DRV8874 EN, PWM-capable, the only PWM consumer
constexpr uint8_t kMotorSleepBar = 10U;  // V5 harness: DRV8874 nSLEEP, active low

// --- Inputs ----------------------------------------------------------------
// The DRV8874 nFAULT line: **there is no pin for it on this vehicle.** The V5
// harness does not connect the driver's fault output to the Nano, and the pin
// this map once planned for it (D4) is now the encoder B channel.
// `kMotorFaultBar` is the explicit no-pin value, kept only so the
// capability-aware input module can compile its not-installed path without
// naming a pin (`vehicleConfig::kPlannedDriverFaultCapability` is
// `NotInstalled`).
//
// A build that wires the line makes two edits together, never one: replace
// `kNoPin` below with a real pin, and set `kPlannedDriverFaultCapability` back
// to `Installed`. `SensorInputs.h` has a `static_assert` that refuses the second
// edit alone, so a vehicle cannot be built with the capability `Installed` and
// no pin behind it. While the capability is `NotInstalled`, `SensorInputs.h`
// does not configure or sample any nFAULT pin, so no floating level ever enters
// fault logic or telemetry (`data/evidence/D31-2026-09-29/README.md`,
// `docs/limitations.md`).
constexpr uint8_t kNoPin = 0xFFU;  // not a pin: no physical connection exists
constexpr uint8_t kMotorFaultBar = kNoPin;

// Start-go trigger (V5 harness: D12). Its active level and its debounce are
// UNVERIFIED; the plan below reads it with the internal pull-up and treats LOW
// as active.
constexpr uint8_t kStartTrigger = 12U;

// Planned analog inputs. Battery and current sensing are not installed on the
// assembled vehicle (team statement), so these two constants are placeholders
// for a build that installs the sensing; the counts this firmware reports for
// them are raw ADC counts of an unconnected input - not battery or current
// measurements - and may not be recorded as such
// (`data/evidence/D31-2026-09-29/README.md`).
//
// Both are read as raw counts only: no divider ratio, no shunt value and no ADC
// reference relationship is documented in this project, so no
// raw-to-engineering-unit conversion exists (`SOFTWARE_ARCHITECTURE.md`
// section 12, `VERIFIED_STATE.md` #7).
constexpr uint8_t kBatterySense = A1;
constexpr uint8_t kCurrentSense = A2;

}  // namespace evhw
