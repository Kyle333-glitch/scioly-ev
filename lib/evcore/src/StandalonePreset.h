#pragma once

#include "OperatorPolicy.h"
#include "RunConfig.h"
#include "SerialCommandParser.h"

namespace ev {

// ---------------------------------------------------------------------------
// The standalone competition preset - event geometry, and nothing else
// ---------------------------------------------------------------------------
//
// What this is
//
//   The run configuration the standalone (laptop-free) image loads by itself at
//   boot, so `announce` no longer needs a human at a keyboard. It is the three
//   values the event supervisor announces plus the tournament level that fixes
//   the bottle's placement - **event geometry only**.
//
// What this is deliberately NOT
//
//   It is not calibration. The tick-to-distance scale, both brake offsets, the
//   velocity ceilings, the run duration bound, the gains and every bottle-phase
//   ceiling live in `src/VehicleConfig.h` and stay `Provisional` until a physical
//   measurement promotes them (`VERIFIED_STATE.md`). Loading this preset therefore
//   does *not* make a run armable: `announce` runs the same rule validation and
//   `arm` the same configuration-build checks a typed command does, and those
//   refuse while a required value is still a placeholder. That refusal is the
//   honest outcome and the diagnostic to fix next - it is never bypassed here.
//
//   The 20.0 s Target Time is a rule-legal announced value, not a validated run
//   duration: the firmware's timing model is unvalidated
//   (`vehicleConfig::kRunTimingModelValidated`), so this value does not bound,
//   shorten or extend a run.
//
// Changing the preset
//
//   Edit only this block. Target Distance must sit on the level's announced grid
//   (0.25 m at Regionals/Invitationals, 0.10 m at States, 0.01 m at Nationals),
//   Target Time on the 0.5 s grid, and Bottle Line Distance on the same grid as
//   the Target Distance, 1.00-3.00 m beyond the Target Point - the rules checked
//   by `validateRunConfig()`, which the host suite exercises against this preset.

namespace standalonePreset {

// Rule 4.b / 7.c.ii.(5): the bottle sits 2.00 m from the Start Point at
// Regional/Invitational tournaments. The Bottle Start Point is *derived* from
// this level (never typed), exactly as for a serial `config set`.
constexpr uint8_t kLevelIndex = 0U;  // RegionalInvitational

// Rule 8.h: 5.00 m from the Start Point, on the 0.25 m Regional grid.
constexpr double kTargetDistanceM = 5.00;

// Rule 8.i: 20.0 s, on the 0.5 s grid. Announced geometry, not a validated
// duration (see the note above).
constexpr double kTargetTimeS = 20.0;

// Rule 8.j: 1.00 m beyond the Target Point.
constexpr double kBottleLineDistanceM = 1.00;

// The preset as the operator-interface field type, so it travels through exactly
// the same `config set` path a typed configuration does. Built field by field
// rather than by aggregate initialisation: `RunConfigFields` carries default
// member initialisers, which makes it a non-aggregate in C++11 - the language
// level this project's host tests build with - so brace-initialising it with
// values is not portable across the target and host toolchains.
inline RunConfigFields fields() {
    RunConfigFields preset;
    preset.hasLevel = true;
    preset.levelIndex = kLevelIndex;
    preset.hasTargetDistanceM = true;
    preset.targetDistanceM = kTargetDistanceM;
    preset.hasTargetTimeS = true;
    preset.targetTimeS = kTargetTimeS;
    preset.hasBottleLineDistanceM = true;
    preset.bottleLineDistanceM = kBottleLineDistanceM;
    return preset;
}

// The complete rule configuration the preset describes (Bottle Start Point
// derived from the level).
inline RunConfig runConfig() { return completedRunConfig(fields()); }

}  // namespace standalonePreset

}  // namespace ev
