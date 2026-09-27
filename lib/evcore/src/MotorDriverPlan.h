#pragma once

#include "Motion.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The planned DRV8874 PH/EN driver interface - a PLAN, not a measurement
// ---------------------------------------------------------------------------
//
// This header holds the project's planned mapping from an authorized command to
// the driver's logic inputs. It is pure logic: it computes levels and a duty, it
// touches no pin, and it is host-testable. The pin writes themselves belong to
// the vehicle layer's single driver adapter.
//
// **Every electrical fact behind this mapping is UNVERIFIED.** It is a plan to
// bring up and measure, not a description of observed behavior
// (`VERIFIED_STATE.md` #1, #2 and #3 stay UNVERIFIED). The planned interface,
// with the datasheet-derived facts and the exact bench measurements that would
// confirm each one, is recorded at `docs/hardware/motor-driver-drv8874.md`.
//
// What is claimed here, and what is not
//
// * Claimed: which input pins the firmware intends to drive, and with what
//   levels, for each `MotorMode` - PH for direction, EN for drive magnitude,
//   nSLEEP for the driver's enable.
// * Not claimed: what the driver actually does with a level combination on this
//   vehicle. Whether the non-driven enabled state brakes or coasts, whether the
//   polarity of PH matches forward travel, the logic levels, the wake time, and
//   the electrical behavior of every mode are all measured facts that do not
//   exist yet. The first bench session settles them, and this table is the one
//   place the answer changes.
//
// Kind of change this is
//
//   SOFTWARE_ARCHITECTURE.md section 10 lists the driver interface and the
//   mapping of `MotorMode` onto it as deliberately undecided. Writing down a
//   *planned* mapping (a placeholder that refuses to be a measured fact) is what
//   makes the boundary implementable at all; section 15 records the decision and
//   `DEVELOPMENT_LOG.md` D13 records the reason. Nothing here may be used to
//   authorize motion: authorization stays with `SafetyAuthority`, and an
//   authorized command only reaches these inputs after every measured limit and
//   calibration value exists.

// The planned driver input levels for one authorized command. These are logic
// levels and a duty, not electrical outcomes.
struct Drv8874Inputs {
    // nSLEEP is active low. False drives it low, which is the driver's
    // low-power/sleep state and the de-energized state of the outputs: the safe
    // state, and the one every non-powered command asks for.
    bool nSleepHigh;
    // PH: the direction input. Held low whenever no direction is being asked
    // for, so the level is deterministic and no direction is presented.
    bool phaseHigh;
    // EN: the drive input. Always a duty (0-1000 permille of full scale); a duty
    // of 0 leaves EN low, which is what every non-powered command asks for.
    uint16_t dutyPermille;
};

// The planned mapping. Total over the `MotorMode` vocabulary: an unknown mode
// fails closed to the de-energized levels rather than to something powered.
//
// Coast is grouped with Disabled in this plan: the de-energized state (driver
// asleep, outputs off) is the one combination the plan can name without claiming
// an electrical behavior it has not measured. If the bench truth table shows a
// distinct coast combination, that is one row of this function to change - not a
// second place to add a sign (`VERIFIED_STATE.md` #2).
//
// Brake keeps the driver enabled with no drive level applied; whether that
// electrically brakes or coasts is one of the facts the bench session measures.
// The level is what the plan says it is; the behavior is not claimed.
//
// A Drive command with a duty of 0 is a legitimate level combination (an
// authorized zero-effort drive), and it is NOT a stop: `MotorMode::Disabled`,
// `Coast` and `Brake` are. Zero duty simply applies no drive.
inline Drv8874Inputs drv8874InputsFor(const AuthorizedMotorCommand& command) {
    switch (command.mode()) {
        case MotorMode::Disabled:
        case MotorMode::Coast:
            return Drv8874Inputs{false, false, 0U};
        case MotorMode::Brake:
            return Drv8874Inputs{true, false, 0U};
        case MotorMode::DriveForward:
            return Drv8874Inputs{true, true, command.magnitudePermille()};
        case MotorMode::DriveReverse:
            return Drv8874Inputs{true, false, command.magnitudePermille()};
    }
    // An unknown mode is never a powered request.
    return Drv8874Inputs{false, false, 0U};
}

// The PWM resolution the driver adapter programs into the Arduino Renesas core
// (`analogWriteResolution`), which is 8 bits by default. The plan is to state it
// explicitly rather than to depend on a default that another translation unit
// could change. The PWM frequency the core produces with that resolution is a
// driver-bring-up measurement item, not a fact this header may assume.
constexpr uint8_t kPlannedPwmResolutionBits = 8U;

// The planned PWM count for an effort of `permille` (0-1000) of full scale, at
// `resolutionBits` bits: the permille scale mapped onto the timer's counts, with
// rounding, and clamped so a corrupt value cannot wrap.
//
// This is an arithmetic mapping only. Whether the resulting duty produces the
// intended motor current, at what PWM frequency, and with what audible or
// thermal effect are all UNVERIFIED electrical facts.
inline uint16_t pwmCountFromPermille(uint16_t permille, uint8_t resolutionBits) {
    const uint16_t fullScale = static_cast<uint16_t>((1U << resolutionBits) - 1U);
    const uint32_t clamped = permille > 1000U ? 1000U : permille;
    return static_cast<uint16_t>((clamped * fullScale + 500U) / 1000U);
}

// True when the levels leave the driver de-energized: the single fact the
// vehicle layer's fail-safe path needs, stated here so the plan and its check
// stay in one place.
inline bool drv8874InputsAreDeEnergized(const Drv8874Inputs& inputs) {
    return !inputs.nSleepHigh && inputs.dutyPermille == 0U;
}

}  // namespace ev
