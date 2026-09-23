# Development Log

Append-only record of accepted milestones and important team/engineering
decisions. Add new entries at the **bottom**. Do not rewrite or delete earlier
entries; if a decision changes, append a new entry that supersedes it.

This is a history file. Architecture contracts live in
`SOFTWARE_ARCHITECTURE.md`. Hardware/physical facts and their evidence live in
`VERIFIED_STATE.md`.

Entry heading format: `## D<n> - <date> - <stage> - <short title>`

---

## D0 - 2026-09-22 - Stage 0 (pre-P1) - Truth, provenance, and host-test path

**Baseline inspected before any edit**

- Branch `main`, commit `3ce4e85` ("Initialize Nano R4 EV firmware project").
- Working tree clean; no unrelated user changes present.
- Only firmware source was `src/main.cpp` (LED blink plus a serial banner).

**Baseline build (before changes)**

- Command: `pio run`
- Result: SUCCESS. RAM 7.8% (2548 / 32768 bytes), Flash 13.0%
  (34116 / 262144 bytes), ~10.9 s. No warnings reported.
- Conclusion: the pre-edit baseline was healthy. No baseline failure to diagnose.

**Accepted milestones**

- The Nano R4 environment builds after the changes: `pio run` SUCCESS,
  RAM 7.8% (2564 / 32768 bytes), Flash 13.0% (34152 / 262144 bytes).
  `default_envs = nano_r4` was added so `pio run` builds the vehicle firmware
  only.
- Host logic tests build and run: `pio test -e native` reports
  `6 test cases: 6 succeeded`.

**Decisions**

1. **Authority files established.** `SOFTWARE_ARCHITECTURE.md` holds frozen
   contracts, `VERIFIED_STATE.md` holds hardware facts with their evidence
   status, and this file holds history. Process rules remain in `AGENTS.md`.
2. **Two-layer source split.** `src/` is vehicle firmware and is the only place
   allowed to use Arduino headers. `lib/` is pure logic with no Arduino
   dependency, so it can be tested on the host. Dependency direction is
   `src/` -> `lib/` only. See `SOFTWARE_ARCHITECTURE.md` sections 2-3.
3. **Host tests must not compile Arduino code.** The `native` environment sets
   `test_build_src = no`, so `src/main.cpp` is never forced through a host
   compiler.
4. **First pure-logic production module.** `lib/evcore/src/IntervalTimer.h`
   provides nonblocking, wraparound-safe interval logic. It was extracted from
   the timing pattern already used by `main.cpp`, and `main.cpp` now uses it for
   the LED blink. Vehicle behavior is unchanged; this keeps the module real
   production code rather than test-only code.
5. **Host toolchain handling.** A stock Windows machine has no host C/C++
   compiler, so `[env:native]` declares the PlatformIO-managed MinGW toolchain
   (`toolchain-gccmingw32`) and `scripts/native_toolchain.py` uses it only when
   no system compiler is present. Host test binaries are linked statically so
   the test runner can execute them without the MinGW runtime DLLs on PATH.
6. **Stage boundary respected.** No motor, encoder, controller, state machine,
   telemetry, CLI, configuration, or simulator code was created. This stage
   stops before P1.

**Software verification (not physical verification)**

- `pio run` - Nano R4 firmware builds.
- `pio test -e native` - `test_interval_timer` smoke test passes 6/6, exercising
  the production `IntervalTimer` module.

**Pending physical checks**

The LED blink and serial banner were not observed on hardware after this change.
When a board is available, upload and confirm the serial banner appears and the
onboard LED toggles at roughly 0.5 s. This is a convenience check, not a safety
or motion test.

**Remains unverified**

Every hardware fact listed in `VERIFIED_STATE.md` is UNVERIFIED. No vehicle
hardware is available yet, so nothing in that table has been measured.

**Next stage**

P1 (motor driver bring-up with the drivetrain unloaded), using the documented
baseline as the known-good starting point.

---

## D1 - 2026-09-22 - P1 - Pure-core data contracts and rule-valid run configuration

**Correction to D0**

D0 predicted that P1 would be motor-driver bring-up. That was wrong: P1 is the
pure-core data contract stage, and hardware bring-up moves later. The stage
description here supersedes the D0 prediction; D0 is left unedited.

**Official rule source**

The event rules were added at `docs/rules/2027-div-c-electric-vehicle.pdf`
(official 2027 Division C rules; the Electric Vehicle event is on pages C26-C31).
The rule values used by the firmware, with rule numbers, are recorded in
`docs/rules/README.md`. The rules file is currently untracked; whether to commit
a third-party copyrighted PDF is a team decision that has not been made.

Values taken from the rules:

- rule 8.h Target Distance 5.00-9.00 m, interval 0.25/0.10/0.01 m by level;
- rule 8.i Target Time 10.0-20.0 s, interval 0.5 s;
- rule 8.j Bottle Line Distance 1.00-3.00 m beyond the Target Point;
- rule 7.c.ii.(5) bottle placement 2.0/3.0/4.0 m from the Start Point by level.

**Accepted milestones**

- Pure-core contracts added under `lib/evcore/src`: `Units.h`, `RunConfig.h`,
  `VehicleCalibration.h`, `SafetyLimits.h`, `Motion.h`, `Snapshots.h`,
  `VehicleState.h`.
- Run-configuration validation centralized in `ev::validateRunConfig()` against
  the official rule values.
- Nano R4 build still succeeds.
- `pio test -e native`: 35 test cases, 35 succeeded.

**Decisions**

1. **Units are SI in the pure core**: metres, seconds, metres per second, volts,
   with the unit in every field name. `millis()` clock readings stay in
   milliseconds with an `Ms` suffix because that is what the platform provides.
   This follows the explicit "prefer SI internally" requirement; it is a
   departure from the millimetre examples in `AGENTS.md` section 10, and
   `SOFTWARE_ARCHITECTURE.md` section 6 now records the convention.
2. **Provenance is part of the type.** `Sourced<T>` carries a `ValueSource`
   (`Measured`, `RuleDerived`, `Provisional`) and has no default constructor, so
   an unmeasured quantity cannot be silently filled in. No calibration or
   safety-limit numbers were invented: those types have no default values at
   all, and tests supply their own fixtures.
3. **Nothing can command motion yet.** `AuthorizedMotorCommand` has a private
   constructor and a single `friend` declaration for the future arming
   authority, so no code present today can construct one. Native tests assert
   that it is not default-constructible and not constructible from a
   `MotorEffort`.
4. **Four categories stay separate**: fixed hardware configuration (not yet
   written), `VehicleCalibration`, `SafetyLimits`, and per-run `RunConfig`.
5. **Rule citations are not physical evidence.** Rule-derived numbers are
   labelled `RuleDerived`, never `Measured`, and `VERIFIED_STATE.md` now records
   the rule source alongside the hardware facts.

**Evidence**

- `pio run` - nano_r4 SUCCESS, RAM 7.8% (2564 / 32768 bytes), Flash 13.0%
  (34152 / 262144 bytes). The pure core is not yet referenced by `main.cpp`, so
  the firmware size is unchanged.
- `pio test -e native` - `test_interval_timer` 6/6, `test_run_config` 13/13,
  `test_core_types` 16/16: 35 test cases, 35 succeeded.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over every
  `lib/evcore/src` header: no warnings. This was run as an extra check because
  `main.cpp` does not include the new headers, so the normal firmware build does
  not compile them.

**Deferred because hardware evidence is required**

- A fixed hardware configuration type. Pin assignments, wiring and logic levels
  are UNVERIFIED, so no such values may be written yet.
- Every `VehicleCalibration` value: distance per encoder tick, and the forward
  and reverse brake offsets.
- Every `SafetyLimits` value: maximum effort, maximum velocity in each
  direction, maximum run duration, and minimum battery voltage.
- The mapping from `MotorMode` onto real driver inputs.
- Whether `batteryMv` is ever a reading at all, i.e. whether battery sensing
  will exist.

**Still unverified**

Every hardware fact in `VERIFIED_STATE.md` remains UNVERIFIED. No physical test
has been performed at any point in this project so far.

**Next stage**

P2: the arming authority and motor-driver bring-up with the drivetrain
unloaded, still with the wheel off the floor and conservative effort.
