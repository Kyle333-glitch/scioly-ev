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

---

## D2 - 2026-09-22 - P1 audit fixes - Compile-time contracts and evidence accuracy

**Why**

An entry-gate audit of the P1 commit (`7693a9f`) found one IMPORTANT and four
MINOR issues. All five were in the P1 stage boundary: documentation and pure-core
types. No P2+ behavior was added (no arming logic, no `SafetyAuthority`
implementation, no state machine).

The IMPORTANT finding was that the "no silent defaults" contract in
`SOFTWARE_ARCHITECTURE.md` section 7 held only incidentally: it depended on the
fact that `Sourced<T>` happens to have no default constructor. Nothing failed if
someone later added one. The MINOR findings were: an undocumented ordering
assumption in `validateRunConfig()`, an unpinned `ok`/`error` invariant on the
validation result structs, an undocumented precondition on the
`AuthorizedMotorCommand` constructor, and an inaccurate evidence row in
`VERIFIED_STATE.md`.

**Fixes**

1. **IMPORTANT - pinned the provenance contract at compile time.** Added
   `#include <type_traits>` and a `static_assert(!std::is_default_constructible<...>)`
   to `Units.h` (`Sourced<double>`), `VehicleCalibration.h`, `SafetyLimits.h` and
   `RunConfig.h`. A future change that makes any of these default-constructible
   now fails the build instead of silently weakening the contract. The same
   assertions are mirrored in `test/test_core_types` and `test/test_run_config`,
   so a regression stops a host test target from compiling.
2. **MINOR - documented the range-before-grid ordering.** `validateRunConfig()`
   reports a range violation before an off-grid violation. That order is only
   correct because every range boundary (5.00/9.00 m Target Distance,
   1.00/3.00 m Bottle Line Distance) is an integer multiple of every level's
   interval (0.25/0.10/0.01 m). The invariant is now stated at the range checks,
   and `test_range_boundaries_sit_on_every_levels_grid` fails if a future Rules
   Correction moves an interval so a legal boundary falls off the grid.
3. **MINOR - pinned the `ok`/`error` invariant.** `RunConfigValidation`,
   `MotorEffortValidation` and `MotionRequestValidation` now document
   "error == None exactly when ok is true", and every existing valid/invalid case
   asserts it through the `expectValid` / `expectInvalid` / `expectEffortInvalid`
   / `expectMotionRequestInvalid` helpers rather than new tests.
4. **MINOR - documented the `AuthorizedMotorCommand` precondition.** The private
   constructor enforces *who* may authorize an effort, not that the effort is
   valid; that is stated at the constructor as a P2 (`SafetyAuthority`)
   responsibility. No `SafetyAuthority` code or validation call was added, and
   the deferred P2 regression test (refusing an out-of-range `MotorEffort`) was
   deliberately not written now.
5. **MINOR - corrected an evidence row.** `VERIFIED_STATE.md` claimed the ARM
   syntax check produced "no warnings". Checking a header standalone necessarily
   emits one `#pragma once in main file` artifact per header; the row now states
   the exact expected output and that any other diagnostic is a real warning.

**Evidence (software verification; no physical test is applicable)**

- `pio run` - nano_r4 SUCCESS, RAM 7.8% (2564 / 32768 bytes), Flash 13.0%
  (34152 / 262144 bytes). Unchanged from D1, as expected: the pure core is still
  not referenced by `main.cpp`, and no new warnings were produced.
- `pio test -e native` - 36 test cases, 36 succeeded (was 35; the single new case
  is `test_range_boundaries_sit_on_every_levels_grid`). Breakdown:
  `test_interval_timer` 6/6, `test_run_config` 14/14, `test_core_types` 16/16.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all eight `lib/evcore/src` headers: exit 0 for every header. The only
  diagnostics are the expected `#pragma once in main file` artifacts, one per
  header. This remains the only check that compiles the pure core, because
  `main.cpp` does not include it yet.
- `git diff` reviewed: it touches only the four headers named in fix 1,
  `Motion.h`, the two test translation units, `VERIFIED_STATE.md` and this file.
  No unrelated cleanup or reformatting.

**Not verified**

Nothing about the vehicle. These are compile-time and documentation changes
only, so there is no pending physical test and no hardware behavior is claimed.
Every hardware fact in `VERIFIED_STATE.md` stays UNVERIFIED.

**Next stage**

Unchanged from D1: P2 is the arming authority and motor-driver bring-up with the
drivetrain unloaded.

---

## D3 - 2026-09-23 - P2 - Final permission boundary for powered motion

**Why**

P2 was requested as the pure final-permission safety boundary that future motor
actuation must pass through. The required behavior was: explicit
DISABLED/BRAKE/DRIVE semantics, one pure transformation from a requested
effort/intent plus safety/system state to an authorized command, independent
prevention of a direct powered forward/reverse transition, and a fault-stop
policy that is separate from fault identity. The boundary had to be
invariant-based and deliberately ignorant of bottle phases, scoring, event
geometry, target-distance strategy and competition sequencing, and it had to
contain no GPIO and no driver nFAULT/current/stall classification.

The stage was explicitly scoped to stop before a GPIO motor driver, motion
controller, state machine or physical threshold. That hard stop was honored: no
`src/` file changed, nothing new touches Arduino headers, and no hardware fact
was invented to make the code work.

**Decisions**

1. **Scope is permission, not motion.** `SafetyAuthority` reasons only about
   arming, data completeness, a latched fault, effort bounds and safe direction
   change. It contains no bottle, scoring, geometry, distance-targeting or
   sequencing knowledge, and its behavior depends only on the request, the
   supplied state, the supplied limits and its own three pieces of state
   (armed, latched fault, last authorized powered direction). There are no
   hidden timers.
2. **DISABLED / BRAKE / DRIVE is now explicit vocabulary.** `Motion.h` gained
   `ActuationClass {Disabled, Brake, Drive}` with `actuationClassOf()` and
   `PoweredDirection {None, Forward, Reverse}` with `poweredDirectionOf()`;
   `AuthorizedMotorCommand` exposes both. Direction and effort are meaningful
   only for a Drive action, and every non-Drive command is built with magnitude
   0. `Coast` is classified with `Disabled` because both remove powered output
   and the difference between them is a driver detail that is still UNVERIFIED,
   so the boundary must not depend on it. An unknown mode is never classified as
   powered.
3. **One construction site for authorized commands.** `SafetyAuthority` is the
   only friend of `AuthorizedMotorCommand`, and its private static `commandFor()`
   is the only place a command is built. `AuthorizationResult` therefore has no
   default constructor either; it is pinned by a compile-time assertion in the
   new test target.
4. **A denial is always a concrete stop.** `authorize()` never returns "no
   command". Every refusal returns `Disabled` with magnitude 0, and non-powered
   requests (Disabled/Coast/Brake) are always granted without arming, because
   they cannot add energy and braking must remain available to stop the vehicle.
5. **Fault identity is separate from fault response.** A new `FaultStopPolicy`
   type with `faultStopPolicyFor(FaultCode)` decides the electrical response.
   Every software fault that exists today maps to `Disable`, the only response
   that is safe to assume while driver truth tables, nFAULT behavior and current
   limits are UNVERIFIED. A hardware fault gets a different policy only after
   measurement, and that change lands in the policy function rather than in
   `FaultCode`. No stall/current/nFAULT condition is classified yet.
6. **Faults are latched, and latching is conservative.** A detected fault is
   latched once (stamped with the caller's `nowMs`), it disarms, it blocks drive
   until explicitly cleared, and a later different fault does not silently
   replace the fault of record. `clearFault()` refuses while armed; today that
   guard is defensive and unreachable, because latching a fault always disarms,
   but it keeps the rule true if another mutation path is ever added. `reset()`
   returns to disarmed with no fault and no remembered direction, so nothing
   resumes automatically after a reset.
7. **No invented thresholds.** `SafetySystemState` carries `requiredDataFresh`,
   `atZeroSpeed` and `nowMs` as caller-supplied facts; no staleness timeout and
   no zero-speed threshold live in the boundary. Staleness and zero speed are
   measurement/policy questions the vehicle layer has to answer.
8. **Direction reversal is refused independently of claimed speed.** The last
   authorized powered direction is remembered and a drive in the opposite
   direction is refused (`DeniedUnsafeDirectionChange`) even when the caller
   reports zero speed. Only an authorized non-powered command taken while zero
   speed is reported clears the memory, which is the deliberate legal path
   (`decelerate -> brake/stop -> settle at zero -> reverse`). Disarming,
   re-arming and denied flips do not clear it, so arming is not a shortcut
   around the zero-speed path.
9. **Effort bounds only ever reduce.** A magnitude above full scale is an illegal
   request; a legal request above `limits.maxEffortPermille` is clamped and
   reported as `AuthorizedClamped`, never granted as asked.
10. **Reasons are diagnostics, not the output.** `AuthzOutcome` exists so a test
    or a future telemetry line can explain a decision; the safety-relevant
    output is always the returned command.

**Evidence (software verification; no physical test is applicable)**

- `pio run` - nano_r4 SUCCESS, RAM 7.8% (2564 / 32768 bytes), Flash 13.0%
  (34152 / 262144 bytes). Unchanged from D2, as expected: `main.cpp` still does
  not include the pure core, so nothing new is linked into the firmware.
- `pio test -e native` - 62 test cases, 62 succeeded (was 36). The new
  `test_safety_authority` contributes 26 cases; breakdown is
  `test_interval_timer` 6/6, `test_run_config` 14/14, `test_core_types` 16/16,
  `test_safety_authority` 26/26. No compiler warnings were produced.
- The new cases cover: refused and granted arming, disarm denying drive, fault
  latching plus forced stop, fault-latch persistence across later drive
  requests, clear/re-arm for every fault code, `clearFault()` refusing when
  nothing is latched, fault response independent of fault identity, stop
  commands per policy never producing a drive, DISABLED/BRAKE/DRIVE
  classification, non-powered commands without arming, every drive
  precondition (not armed / not ready / stale data), illegal efforts, full-scale
  effort, clamping at and below the ceiling, 200 repeated identical commands,
  direct flips refused in both directions (including when zero speed is
  claimed and across 50 consecutive attempts), the legal zero-speed direction
  change, a stop without zero speed not clearing the memory, disarm/re-arm not
  forgetting the direction, and reinitialization after a fault.
- One property-style case drives 5000 pseudo-random steps over the cross product
  of modes (including an unknown mode), magnitudes (0/1/350/1000/1001),
  readiness combinations, freshness, zero-speed claims, fault codes, two effort
  ceilings, and interleaved arm/disarm/clear/reset actions. It asserts that a
  powered command can only continue or establish the remembered direction,
  never flip away from it; that a drive is only ever issued when armed, ready,
  fresh and unfaulted; that a non-Drive command always carries magnitude 0; and
  that the direction memory is cleared only by a stop taken at reported zero
  speed.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all nine `lib/evcore/src` headers: no errors and no warnings other than
  the expected `#pragma once in main file` artifact, one per header. This is
  still the only check that compiles the pure core, because `main.cpp` does not
  include it yet.
- The first test run failed three cases; all three were wrong test fixtures, not
  implementation defects (re-arming while a fault is latched is correctly
  refused, a latched fault correctly overrides a requested brake with the policy
  stop, and an in-range effort is correctly not reported as clamped). The test
  file was corrected and the implementation was not changed. A latched fault
  therefore does not allow braking through this boundary yet; that is the
  conservative consequence of policy 5 and is revisited when braking is
  measured.
- `git diff` reviewed: `Motion.h` (classification helpers and command
  accessors), the new `SafetyAuthority.h`, the new
  `test/test_safety_authority/test_main.cpp`, and this file. No unrelated
  cleanup, no reformatting, no `src/` changes.

**Not verified**

Nothing about the vehicle. No physical test was performed and no hardware
behavior is claimed. `SafetyAuthority` has never driven a motor; no driver truth
table, nFAULT behavior, current limit, braking behavior or encoder fact is
implied by this stage. Every hardware fact in `VERIFIED_STATE.md` stays
UNVERIFIED, and no `VERIFIED_STATE.md` row was added or modified.

**Next stage**

P3 is the motor-driver layer and the first physical bring-up: a hardware
configuration type written from measurements rather than assumptions, then a
GPIO driver brought up with the drivetrain unloaded, conservative effort, one
behavior at a time, and verified direction before any higher power. The
`SafetyAuthority` boundary stays the only path to an authorized command.

---

## D4 - 2026-09-23 - P2 audit fixes - Pinned fault-clearance disarm and hardened boundary tests

**Why**

An external P2 audit of the `SafetyAuthority` boundary returned "PASS AFTER
SPECIFIC FIXES" with six findings (F1-F6). The scope was deliberately limited to
those findings: no authorization behavior was to change beyond one pinned line,
and the P2 contracts (`AuthzOutcome` order, denial = Disabled/0, the zero-speed
direction-change path, clamping, fault-latch semantics) stay frozen. No `src/`
change, no Arduino header in `lib/`, no GPIO, no competition-phase policy and no
new hardware fact. `VERIFIED_STATE.md` is untouched.

**Decisions**

1. **F1 (IMPORTANT) - clearing a fault can never restore powered motion.**
   `clearFault()` now sets `armed_ = false;` immediately after
   `latchedFault_ = FaultState{};`, and its comment states that a fault clearance
   always ends the current arming period, so re-arming is a fresh decision. Today
   this is idempotent because latching a fault already disarms; it is pinned so a
   future mutation path cannot make fault clearance a shortcut back to powered
   motion. Two new proving tests: `test_clear_fault_while_armed_is_refused`
   (a faulted authority disarms, refuses to re-arm while the fault is latched,
   and a separate never-faulted armed authority refuses `clearFault()`, so the
   armed guard is reachable and load-bearing rather than dead code) and
   `test_clear_fault_ends_the_arming_period` (armed -> forward granted -> fault
   latches -> `clearFault()` succeeds -> explicit `arm(fullyReady())` succeeds ->
   forward is granted again while reverse is still refused, proving the direction
   memory survived; `isArmed()` is asserted false between `clearFault()` and the
   explicit arm).
2. **F2 (IMPORTANT) - `SafetySystemState::nowMs` documented, name kept.** The
   field name is unchanged so existing tests are not churned. Its comment now
   states that it must be an actual `millis()` reading supplied by the vehicle
   layer, and that a forgotten assignment is not detectable at this boundary.
   New test `test_zero_timestamp_does_not_suppress_fault_latching` pins that a
   zero stamp still latches (`latched == true`, `raisedAtMs == 0`) rather than
   silently weakening the fault latch.
3. **F3 (MINOR) - arming refusal test fails for the right reason.**
   `test_arm_is_refused_while_a_fault_is_latched` now asserts the latching call
   returned `ForcedFaultStop` and that `isArmed()` is false *before* the arm
   refusal assert, so a future refactor cannot make this test pass on an
   unrelated denial.
4. **F4 (MINOR) - property search is seed-parameterized.** The 5000-step
   randomized search was extracted into `runDirectionFlipPropertySearch(seed)`
   and is now run for the fixed seeds `{0x5EED1234U, 0xDEADBEEFU, 1U}`. Seeds
   stay fixed for CI determinism, and every per-step invariant must hold for
   every seed. The test count is unchanged by this finding (one test case).
5. **F5 (MINOR) - `PoweredDirection` ordering is documented as meaningless.**
   The enum comment in `Motion.h` now states that these are distinct identities,
   not a scale, and that order carries no meaning and must never be compared
   relationally. Comment-only; no code or behavior change.
6. **F6 (MINOR) - unreachable tails replaced by an explicit safe default.**
   `faultStopPolicyFor(FaultCode)` and `stopCommandFor(FaultStopPolicy)` now end
   their switch with a `default:` returning the safe `Disable` response, commented
   that new enumerators must be mapped explicitly and that this default is the
   safe response. The build stays `-Wall -Wextra` clean.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **65 test cases, 65 succeeded** (was 62).
  `test_safety_authority` grew from 26 to 29 cases; the other targets are
  unchanged: `test_core_types` 16/16, `test_interval_timer` 6/6,
  `test_run_config` 14/14, `test_safety_authority` 29/29. No compiler warnings.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D3: the pure core is still not
  linked into `main.cpp`, and these fixes are comments, tests and one in-method
  assignment inside a header `src/` does not include yet.
- ARM syntax check:
  `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all nine `lib/evcore/src/*.h` headers - no errors and no warnings other
  than the expected `#pragma once in main file` artifact, one per header.
- `git diff` reviewed: `SafetyAuthority.h`, the `PoweredDirection` comment block
  in `Motion.h`, and `test/test_safety_authority/test_main.cpp` only. No
  unrelated cleanup, no reformatting, no `src/` change.

**Not verified**

Software verification only; no physical test is applicable; no hardware fact is
claimed. `SafetyAuthority` has never actuated a motor, and no driver truth
table, nFAULT behavior, current limit, braking behavior or encoder fact is
implied. Every `VERIFIED_STATE.md` row stays UNVERIFIED and no row was added or
modified.

**Next stage**

Unchanged from D3: P3 is the motor-driver layer and the first physical bring-up
with the drivetrain unloaded. The `SafetyAuthority` boundary remains the only
path to an authorized command.

---

## D5 - 2026-09-24 - P3 (decoding only) - A-channel CHANGE x2 quadrature decoding as pure logic

**Why**

The next layer above the accepted P2 boundary is the encoder, but no encoder is
physically available, so nothing about a real signal can be claimed yet. The
part of encoder handling that is *not* a hardware question was therefore built
now, on its own, as pure deterministic logic the host can prove: the decoding
rule from two channel levels to one signed tick. This keeps the later pin/ISR
bring-up from also having to invent and debug the decoding arithmetic at the
same time as the wiring.

The stage was deliberately scoped to the decoder alone. It stops before pin
reads, interrupt attachment, sampling policy, filtering/debounce, odometry,
velocity estimation and any ticks-to-distance conversion. No `src/` file
changed, nothing new includes an Arduino header, and no hardware fact was
invented to make the code look complete.

**Decisions**

1. **One pure decoder owns the decoding rule.**
   `lib/evcore/src/QuadratureDecoder.h` contains `ev::QuadratureTickSense`
   (`{-1, 0, +1}`), `ev::tickSenseForLandingState(aLevel, bLevel)` and
   `ev::QuadratureDecoder`. It is integer-only, has no Arduino header, no clock,
   no filtering, no allocation and no float, so it satisfies the host-testability
   contract for `lib/` (section 3) before any hardware exists.
2. **The scheme is A-channel CHANGE x2, and the sign comes from both channels.**
   One count per transition of channel A, on both of its edges, and only on A.
   The sign is a function of the whole `(A, B)` state the A edge lands in, split
   by parity: equal levels (`00`, `11`) are one sense and different levels (`10`,
   `01`) are the other. B alone is explicitly **not** the direction source.
   Reading B alone (or B plus the edge type) is the classic defect this avoids:
   it inverts one of the two counted A edges, so a full revolution cancels to
   zero while still looking like a plausible count.
3. **The software sense convention is stated, and stated as a convention.** The
   Gray cycle `00 -> 10 -> 11 -> 01 -> 00` is defined as `Positive` *inside this
   module*. That is a counting convention, not a physical fact: nothing in the
   module, and nothing in `lib/`, may assume this sense is forward travel. The
   mapping from `Positive`/`Negative` to physical forward remains UNVERIFIED
   (`VERIFIED_STATE.md` #4) and is settled only by hand-rotating a real encoder.
4. **First sample only establishes a reference.** A fresh decoder (or one just
   `reset()`) returns `None` and counts nothing on its first `update()`, because
   there is no previous state to compare against and no power-on channel level
   may be assumed (both channels can legitimately rest high).
5. **A sample that is not an A transition can never move the count.** `update()`
   with A unchanged returns `None`, increments `ignoredUpdates()`, and — this is
   the load-bearing part — still refreshes the tracked B level so a later real A
   edge is decoded against the current state rather than a stale one. A caller
   that repeats a sample or also reports B transitions is therefore harmless.
6. **The invalid and the unreachable are reported, not hidden.** `countedEdges()`
   and `ignoredUpdates()` are exposed so a bring-up log can distinguish a count
   that is progressing from one that is oscillating. Whether a large
   ignored-update count means a noisy signal is a hardware question
   (`VERIFIED_STATE.md` #5); the decoder does not adjudicate signal quality.
7. **Tick count type matches the existing contract.** `ticks()` is `int32_t`,
   matching `SensorSnapshot::motorEncoderTicks`, whose sign is likewise
   documented UNVERIFIED. The decoder is not yet wired into any snapshot, and no
   odometry layer consumes it.
8. **Contracts updated in the same change.** `SOFTWARE_ARCHITECTURE.md` gains
   section 11, which freezes the decoding scheme, the sense-as-convention rule
   and the ban on conversion factors in this module; section 10 narrows its
   encoder bullet to what genuinely remains undecided (counts per revolution,
   gearing, and the physical forward sign, plus the ticks-to-displacement
   conversion).

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **80 test cases, 80 succeeded** (was 65). All five
  targets PASSED: `test_core_types`, `test_interval_timer`,
  `test_quadrature_decoder` (new, 15 cases), `test_run_config`,
  `test_safety_authority`. No compiler warnings.
- The new target pins the sense values at compile time (`-1/0/+1`), decodes
  against an independent reference model of the quadrature ring rather than
  against the implementation, and covers: fresh-decoder state, the first-sample
  reference, A-only counting, B-only samples, repeated samples, both channels
  changing between counted edges, the explicit proof that direction is not a
  function of B alone, every reachable edge transition, all four landing states,
  repeated cycles accumulating linearly, reversal from every state (both the
  single-edge inversion and the mid-run cancel), a bounce oscillation cancelling
  to zero net count, `reset()` forgetting reference/count/levels, and a seeded pseudo-random walk
  (fixed seeds `0x5EED1234`, `0xDEADBEEF`, `1`; 4000 steps each, with duplicate
  samples injected) asserting that no sample sequence can
  break the decoder.
- The first full run failed two cases, and both were wrong test fixtures, not
  implementation defects: one assumed every step was an A edge, the other
  assumed a fixed ignored-update count. The fixtures were corrected to derive
  expected counted edges from the reference ring; **the implementation was not
  changed**. Recorded here because a fixture that is wrong in the direction of
  passing is exactly the failure mode this file exists to prevent.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D4, as expected: `main.cpp`
  does not include the pure core, so the decoder is not linked into the
  firmware and its size is not yet evidence about the firmware.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **ten** `lib/evcore/src/*.h` headers, including the new
  `QuadratureDecoder.h`: exit 0 for every header, no warnings other than the
  expected `#pragma once in main file` artifact, one per header. This remains
  the only check that compiles the pure core, because `main.cpp` does not
  include it yet.
- A forbidden-dependency scan of `QuadratureDecoder.h` for `Arduino`, `Serial`,
  `pinMode`, `digitalRead`, `attachInterrupt`, `millis`, `distance`, `CPR`,
  `gear`, `wheel`, `radius`, `circumference`, `float` and `double` matches only
  the doc comments that name what the module is *not*. No such dependency is
  present, so no CPR, gearing or wheel-distance fact can have leaked into this
  layer.
- `git diff` reviewed: `QuadratureDecoder.h` (new), `test/test_quadrature_decoder/`
  (new), `SOFTWARE_ARCHITECTURE.md` (sections 10-11) and this file. No unrelated
  cleanup, no reformatting, no `src/` change.

**Not verified**

Nothing about the vehicle. Software verification only; no physical test is
applicable because no encoder is available. The decoder has never seen a real
signal. In particular the following stay UNVERIFIED and no row was added or
modified: encoder direction sign (#4), encoder signal quality (#5) and rollout
(#6), plus driver truth table (#2), motor polarity (#3) and braking (#9). The
x2 decision above is a decoding convention the project has chosen and can host-
test; it is not evidence that any encoder produces those two channels, those
counts, that sign, or a clean signal.

**Pending physical test (first thing that would settle #4)**

When an encoder is on the bench, hand-rotate the shaft by a known number of
revolutions in each direction with the drivetrain unloaded, feed the two channel
levels to `update()` and record the signed `ticks()` and `countedEdges()` for
each direction. Expected evidence: repeatable magnitudes for equal revolutions,
opposite signs for the two directions, and a `ticks()` per revolution that is
consistent with the encoder's documented resolution once CPR is measured. The
sign that corresponds to physically forward travel then promotes
`VERIFIED_STATE.md` #4, not before.

**Next stage**

Still P3, and still blocked on hardware for the parts that matter: the channel
sampling layer (pin reads and `attachInterrupt ... CHANGE`) that calls
`update()`, then filter/debounce policy only if the measured signal justifies it,
then odometry and velocity estimation. The `SafetyAuthority` boundary remains the
only path to an authorized command, and no decoder output may reach a motor
command path before braking and direction behavior are measured.

---

## D6 - 2026-09-24 - P3A - Tick-to-distance math with an explicit scale, and odometry validity semantics

**Correction to D5**

D5 predicted that odometry would come *after* the channel sampling layer. P3A
takes the opposite order on purpose: the only part of odometry that is not a
hardware question is the arithmetic and its validity semantics, and settling it
now means the later bring-up does not have to debug conversion math and wiring in
the same breath. Nothing about sampling, signals or velocity was added. D5 is
left unedited.

**Why**

Two questions could be settled before any encoder exists: what a signed tick count
means in metres once a rollout measurement exists, and what makes a derived
estimate usable at all. Both are pure logic, and neither may be answered by
inventing a wheel circumference or a conversion factor. The entry gate holds:
rollout and sensor scaling are still UNVERIFIED, so no physical value was written
in this stage.

**Decisions**

1. **Odometry is a pure module with no scale of its own.**
   `lib/evcore/src/Odometry.h` holds `ev::distanceScaleStatus()`,
   `ev::distanceTravelledM()`, `ev::positionFromOriginM()` and
   `ev::odometryReadiness()`. The effective distance per encoder tick is an
   argument on every call, typed as the `Sourced<double>` that
   `VehicleCalibration::distancePerEncoderTickM` already carries. The module
   contains no wheel circumference, gear ratio, counts-per-revolution or
   x1/x2/x4 factor, so a theoretical geometry cannot be mistaken for measured
   rollout (`VERIFIED_STATE.md` #6).
2. **The result separates "the number" from "may I use it".** `OdometryEstimate`
   carries `distanceM`, `valid` and a status, with
   `valid == (status == Valid)` an invariant that every test pins. `valid` is the
   only field a control path may act on; the status exists to explain a refusal.
3. **A provisional scale is diagnostic only.** A `Provisional` (or
   `RuleDerived`) scale still produces a number so a bring-up log can print it,
   but the estimate is invalid. A placeholder therefore cannot reach control
   merely because a consumer forgot to check calibration provenance.
4. **Zero and negative scales are refused, not multiplied through.** A zero
   scale would produce a plausible-looking `0.0 m`; a negative scale would
   silently invert a sign convention this layer does not own. Both report
   `ScaleIsNotPositive` and report no number at all, so an invalid estimate never
   carries a value that looks like a measurement.
5. **Non-finite input and non-finite output fail closed.** A NaN or infinite
   scale reports `ScaleIsNotFinite`; a finite scale times a large delta that
   still overflows reports `DistanceIsNotFinite`; a non-finite run-origin
   position reports `OriginIsNotFinite`. None of the three reports a number, and
   none reports an infinity as a distance.
6. **Tick differences are formed in 64 bits first.** `INT32_MIN` to `INT32_MAX`
   is undefined behaviour in 32-bit subtraction, and a large accumulated count is
   exactly what a long-lived vehicle produces. Every int32 reading pair is exact
   in `int64_t`, and that value is exact as a `double` (well inside 2^53).
7. **The run origin is an argument, not a constant.** `originPositionM` is in
   metres and supplied by the caller: where a run starts (the Start Point, a
   post-reversal turn-around, an origin re-zeroed after a stall) is the vehicle
   layer's decision, and this layer has no opinion on it.
8. **No velocity and no zero-speed estimate.** The hard stop is honored:
   `OdometrySnapshot::velocityMps` is still filled by nothing, and `atZeroSpeed`
   remains a caller-supplied fact in `SafetyAuthority`. Both need a time base and
   a measured rollout, so neither belongs here.
9. **Sample freshness is one rule, in `Snapshots.h`.** `SampleAge` and
   `ev::sampleAge()` compute `ageMs = nowMs - sampleMs` with unsigned
   subtraction, so the arithmetic survives `millis()` wraparound, and a sample is
   fresh up to a caller-supplied `maxAgeMs` inclusive. No staleness timeout lives
   in `lib/`, because none has been measured; the caller supplies the limit, the
   same way `SafetySystemState::requiredDataFresh` is the caller's verdict. A
   stamp from the future reads as ancient and is never fresh, which is the safe
   direction for a reset or mixed clock. `SampleAge{}` is `{0, false}`, so a
   forgotten age computation fails closed.
10. **`odometryReadiness()` is the single verdict a consumer needs.** It combines
    sample existence, estimate validity and freshness, checking in that order and
    reporting the most fundamental problem first, and returns `usable`, a reason
    and the sample age. Motion and safety consume that one answer (mapped onto
    `SafetySystemState::requiredDataFresh`) instead of each re-deriving it. The
    invariant `usable == (reason == Usable)` is pinned in every test.
11. **No battery or current raw-conversion helper was written.** The requirement
    allowed one only where the circuit relationship is authoritative in project
    sources. There is no ADC reference, divider ratio or shunt value anywhere in
    `docs/`, `src/` or `lib/`: no circuit is documented and `VERIFIED_STATE.md`
    #7 (sensor scaling) is UNVERIFIED. Such a helper would be a guess wearing a
    function signature. `SensorSnapshot::batteryMv` stays a raw reading guarded
    by `batteryReadingValid`, and `SOFTWARE_ARCHITECTURE.md` section 12 records
    the condition under which a conversion may be added later.
12. **Contracts updated in the same change.** `SOFTWARE_ARCHITECTURE.md` gains
    section 12 (odometry and sample-age contract); section 10's
    ticks-to-displacement bullet now says the arithmetic is fixed while the
    factor is not; section 11's closing bullet no longer claims odometry is
    unbuilt. The frozen P2 contracts (`Motion.h`, `SafetyLimits.h`,
    `SafetyAuthority.h`, `RunConfig.h`, `Units.h`) were not touched, and
    `Snapshots.h` gained only `SampleAge`/`sampleAge()` with no change to any
    existing type.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **111 test cases, 111 succeeded** (was 80). The new
  target `test_odometry` contributes 31 cases; the others are unchanged:
  `test_core_types` 16/16, `test_interval_timer` 6/6,
  `test_quadrature_decoder` 15/15, `test_run_config` 14/14,
  `test_safety_authority` 29/29. No compiler warnings were produced.
- The new cases cover: signed travel in both directions; a difference that is
  independent of the absolute count and of a negative origin; a zero tick delta
  staying valid at exactly `0.0 m`; the metre convention being metres and not
  millimetres; linearity in the scale; an `INT32_MIN`-to-`INT32_MAX` step being
  exact and reversible; extreme origins; a large accumulated count converting
  without precision loss; a zero scale and a negative scale both refused with no
  number; NaN and ±infinity scales refused; a finite measured scale whose product
  overflows refused; `Provisional` and `RuleDerived` scales producing a real
  number that is never valid; `distanceScaleStatus()` reported on its own; a
  property sweep over the cross product of scale numerics and provenance that
  only ever marks `Measured` + finite + positive as valid; position as origin
  plus travel in both directions; position matching travel at a zero origin; a
  standstill reporting the origin; non-finite origins refused; a position never
  surviving an unusable scale; a finite origin plus finite travel whose sum
  overflows refused; freshness at and one millisecond past the limit; freshness
  across `millis()` wraparound; a future stamp never fresh; a default-built
  `SampleAge` failing closed; `NoSample`, `EstimateInvalid` and `SampleStale`
  each reported; the check order (a stale provisional estimate reports the
  correctness problem); and a cross-product case asserting that `usable` implies
  a sample, a valid estimate and freshness all at once.
- The first run failed one case, and it was a **wrong test fixture, not an
  implementation defect**: the overflow case used 1e6 ticks at 1e300 m/tick
  "expecting" an infinity, but that product is 1e306, still finite, so the module
  correctly reported `Valid`. The fixture was corrected to 2e9 ticks (1e300 x
  2e9 = 2e309, above the largest finite double, and reachable from one int32
  reading pair); **the implementation was not changed**. Recorded because a
  fixture that is wrong in the direction of passing is what this file exists to
  prevent.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D5, as expected: `main.cpp`
  does not include the pure core, so the new module is not linked into the
  firmware and its size is not yet evidence about the firmware.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **eleven** `lib/evcore/src/*.h` headers, including the new
  `Odometry.h`: exit 0 for every header, with no diagnostic other than the
  expected `#pragma once in main file` artifact, one per header. This remains the
  only check that compiles the pure core, because `main.cpp` does not include it
  yet.
- A forbidden-dependency scan of `Odometry.h` and `Snapshots.h` for `Arduino`,
  `Serial`, `pinMode`, `digitalRead`, `digitalWrite`, `attachInterrupt`,
  `analogRead`, `millis`, `delay`, `CPR`, `wheel`, `circumference`, `gear`,
  `velocity` and `alloc` matches only the doc comments that name what the module
  is *not*, plus `Snapshots.h`'s pre-existing `millis()` clock comments. No
  dependency, no clock read and no wheel fact is present. `nowMs` is an argument
  to `sampleAge()` for exactly that reason.
- `git diff` reviewed: `Odometry.h` (new), `test/test_odometry/` (new),
  `Snapshots.h` (additive), `SOFTWARE_ARCHITECTURE.md` (sections 10-12) and this
  file. No `src/` change, no reformatting, no unrelated cleanup.

**Not verified**

Nothing about the vehicle. Software verification only; no physical test is
applicable because no encoder and no drivetrain are available. The module has
never converted a real tick. Every hardware row in `VERIFIED_STATE.md` stays
UNVERIFIED and no row was added or modified, in particular rollout (#6),
encoder direction sign (#4), encoder signal quality (#5), sensor scaling (#7),
wheel slip (#10) and braking (#9). These tests prove what the arithmetic and its
fail-safe behavior do with a supplied number; they are not evidence that any
encoder exists, that it counts, that the counting convention is forward travel,
or that the effective distance per tick is known.

**Pending physical test (what would settle #6, rollout)**

Prerequisite: encoder direction sign, per D5 (#4). Then, with the vehicle able to
roll on the real competition surface: mark a start line, move the vehicle through
a measured straight distance inside the event's own range (5.00-9.00 m), and
record the accumulated signed tick count before and after. Repeat at least five
times in each direction. Expected evidence: a consistent effective distance per
tick across runs, reported as a spread rather than a single mean, with the sign
matching the direction convention measured for #4. Only that measurement fills
`VehicleCalibration::distancePerEncoderTickM` as `ValueSource::Measured`, and
only that flips an `OdometryEstimate` from invalid to valid. If the motor encoder
is found to disagree with ground travel, that is wheel slip (#10): a separate
measured problem for the section 20 escalation question, not a reason to tune the
scale until it agrees.

**Next stage**

Still P3, and still blocked on hardware: the channel sampling layer (pin reads and
`attachInterrupt ... CHANGE`) plus the first physical bring-up. The first thing
odometry may legitimately be wired to after that is a diagnostic that prints
distance and validity for a hand-driven or low-power run - not a motion
controller, and not a stop command, until braking and direction behavior (#9) are
measured. The `SafetyAuthority` boundary remains the only path to an authorized
command.
## D7 - 2026-09-25 - P3B - Candidate velocity estimation, feed-forward + PI control, and distance-aware target shaping

**Why**

With the tick-to-displacement arithmetic settled in D6, three motion questions
were still open, and all three are pure logic that can be written and pinned down
before any hardware exists: what speed do the ticks imply, what effort should the
regulator ask for, and what target should it be asked to reach as the target point
approaches? This stage answers all three as *candidates*: the estimator is
parameterized rather than chosen, the regulator owns no gains, and the profile
shapes a target without deciding a stop, a brake or a state change. Nothing here
was tuned, and no candidate is called adequate.

The entry gate held: encoder direction sign (#4), signal quality (#5), rollout
(#6), gains (#8), braking (#9) and wheel slip (#10) are all still UNVERIFIED, so
no physical value was written down and every unmeasured value is an argument the
caller must supply as `Measured` before anything can move.

**Decisions**

1. **Three modules, three responsibilities, no framework.** `VelocityEstimator.h`,
   `VelocityController.h` and `MotionProfile.h` in `lib/evcore/src/`. There is no
   estimator base class, no candidate registry, no plugin selection and no
   controller subclasses: comparing candidate windows is *configuring one
   estimator several ways*, and the tests do exactly that.
2. **The estimator owns no scale**, exactly like Odometry. The effective distance
   per tick arrives on every `update()` as the caller's `Sourced<double>`, and the
   scale verdict comes from odometry's `distanceScaleStatus()` rather than a
   second copy of the rule. A test feeds the same scales to both modules and pins
   that their usability verdicts agree, so the two cannot drift.
3. **A sliding window, and the realized span is reported.** Samples are retained
   in a fixed 16-slot ring and pruned to the shortest span that still covers
   `windowMs`; the span actually used, `effectiveWindowMs`, travels with every
   number, together with the signed `effectiveWindowTicks`. Tick differences are
   formed in 64 bits. There is no filtering, interpolation, extrapolation or
   prediction.
4. **Edge-period timing was refused, with the reason recorded.** `SensorSnapshot`
   carries one stamp per sample and no per-edge timestamps, so an edge-period
   estimator would have to invent timestamps. The requirement allowed it only if
   the existing timestamp contract supported it cleanly; it does not.
5. **"No ticks" and "no change for a long time" are two different facts.** A
   covered window containing no tick change is a valid zero over that window. An
   encoder that has not changed for `noEdgeTimeoutMs` earns
   `zeroSpeedClaimed = true` with status `NoEdgeTimeout`, no number and no window,
   because it is a statement about observed non-change over a longer horizon, not
   a measurement. Neither fact may be wired to `SafetySystemState::atZeroSpeed`
   in this stage.
6. **No estimator configuration is adequate, and the tests say so.** The sparse
   low-speed case pins the real behavior: with one tick every 40 ms and a 20 ms
   window, the estimate alternates between 0.0 and 0.005 m/s and never reports the
   true 0.0025 m/s, while the same module configured at 40 ms happens to report
   the true value for that perfectly regular fixture. That is quantization caused
   by the window, not a bug, and choosing a window needs a real trace (#5, #6,
   #10). Nothing in the module or the tests selects one.
7. **Stamps: refuse anything that does not advance.** A repeated or rewound stamp
   is refused (`TimeDidNotAdvance`), counted, and never enters the window or
   becomes the time base, so the next advancing stamp still yields the true
   elapsed time. Advancement uses a signed 32-bit difference, so it is correct
   across the `uint32` wraparound and a rewound clock stays refused.
8. **Configuration zero values fail closed.** `VelocityEstimatorConfig{0, 0}` is
   invalid (no default constructor exists at all), and an invalid configuration
   produces refusals with no number for the life of the object.
9. **The regulator always uses the measured dt.** `nowMs` is an argument; the
   first cycle after enabling or resetting has no previous stamp, so it does not
   integrate at all, and a stall reports its real elapsed time while integrating
   at most `maxIntegrationStepS`. Nothing assumes a 20 ms period.
10. **Powered effort is only ever requested in the commanded direction.** The
    effort interval is one-sided: `[0, ceiling]` while the target is positive,
    `[-ceiling, 0]` while it is negative, and `{0}` for a zero target. An
    overspeed therefore reduces effort toward zero and is never answered with
    powered effort the other way. Reversal stays the run state machine's
    deliberate decelerate/brake/settle sequence (AGENTS.md section 15), which is
    the hard stop for this stage.
11. **An effort that quantizes to zero is `Disabled`,** never a Drive with
    magnitude 0: a zero-effort Drive still latches a powered direction in
    `SafetyAuthority` (Motion.h), and no direction may be latched that is not
    actually being powered.
12. **Anti-windup is conditional integration plus a clamp**, and all three facts
    are reported: `outputSaturated`, `antiWindupActive`, `integrationStepClamped`.
    Integration is skipped only while the demand is already at a limit *and* the
    error pushes further into it - no back calculation, no tracking time constant,
    nothing to tune. A zero integral ceiling is a deliberate way to run
    feed-forward + proportional only.
13. **Refusals are three different behaviors, and each is pinned.** A non-finite
    target is refused with the integral *preserved* and reported as it stands (the
    loop is sound; the target is the problem). A measurement that claims to be
    usable but is not a number is refused with the integral *dropped* (the
    feedback path contradicted itself). A measurement that is merely *absent* runs
    feed-forward only with nothing integrated, because that is what start-up looks
    like while the estimator window is still filling. A refused cycle does not
    advance the time base, so the next accepted cycle integrates the whole
    interval since the last accepted step, bounded by the step limit.
14. **Direction changes drop the integral.** Any change of the commanded sign,
    including a transition to or from zero, zeroes the integral and reports
    `integralResetForSignChange`: the integral was accumulated for the previous
    direction, and unwinding it slowly is exactly the delay a deliberate reversal
    must not inherit.
15. **Nothing resumes.** The regulator is disabled on construction and after
    `reset()`; `enable()` is explicit and refused when the configuration is not
    usable; `disable()` and `reset()` drop the integral, the time base and the
    remembered direction.
16. **One sign chain with two explicit, provenance-carrying boundaries.**
    `MotionProfileLimits::positiveCommandDirection` maps a physical
    `MotionDirection` to the command sign;
    `VelocityControllerConfig::positiveEffortMode` maps the command sign to a
    powered driver mode. Both are the UNVERIFIED polarity fact (#4) and must be
    `Measured` before the regulator will drive; a `Provisional` polarity yields a
    non-powered output. The estimator never declares a physical direction, so no
    part of `lib/` pretends to know which way is forward.
17. **The profile shapes a target and decides nothing else.** The target
    magnitude is the smallest of the cruise ceiling, the distance taper
    `sqrt(2 * decelLimit * max(0, remaining - brakeOffset))` and the rate brackets
    reachable in the measured dt. A commanded reversal and an explicit stop
    request are ramped to zero at the deceleration limit and never flip sign
    inside one step. `WithinBrakeOffset` is a statement about the distance it was
    handed, not about where the vehicle is, and not a stop command. The module
    holds no state, so the caller owns the previous target and the run origin.
18. **Timing intent is carried and reported, never acted on.**
    `MotionTimingIntent` holds the announced Target Time and an optional
    intentional delay; `motionDurationS()` computes trapezoidal/triangular
    kinematics from the supplied limits only, and `motionTimingSummary()` reports
    the intended total and the margin against Target Time. Spending slack,
    waiting, or going faster is a run strategy decision that has not been made,
    and the module has no friction or motor model to make it with.
19. **A test found a real defect in the profile's telemetry.** The reported
    "ramp ceiling" field actually carried the *deceleration* ceiling even when the
    target was accelerating upward. It was replaced by the two rate brackets that
    are actually in force, `accelCeilingMps` and `decelCeilingMps`, and the tests
    pin both. The implementation changed; the test was right.
20. **Test fixtures and helpers were wrong in eleven places, and are recorded as
    such.** Nine were assertion failures: the reversal fixture (a 20 ms window had
    already slid past the start of the round trip), an expected speed after a
    rejected stamp, the sparse-tick long-window expectation, the tick-change
    count after a rejected sample, the reversal-cycle integral (the reset happens
    before that cycle integrates), the dt-clamp arithmetic after three refusals,
    an `expectRefused` helper that asserted the integral was always zero while an
    invalid target deliberately preserves it, the same-sign integral (the error
    changes with the target, so it is 3.8 and not 4.0), and - in the boundary
    target added by the audit below - the assumption that the profile snaps its
    target to zero at the brake offset when it ramps it down at the deceleration
    limit. Two were compile errors: a zero-initialized `VelocityEstimatorConfig`
    (the type deliberately has no default constructor, which is the contract
    working) and a brace-enclosed range-for that needs `<initializer_list>` on
    this toolchain. Every one was a wrong test expectation rather than an
    implementation fault, and each was corrected by recomputing the trace rather
    than by weakening the assertion. This is logged for the same reason D6 logged
    its overflow fixture: a fixture that is wrong in the direction of passing is
    what this file exists to prevent.

**Audit pass (same change)**

Re-reading the three modules against their own claims - rather than against the
passing tests - found six things worth fixing. None of them changed a decision
above. Three were documentation that was wrong or silent about a deliberate
choice, one removed a redundant condition, and two added tests for behavior that
was deliberate but unstated or unpinned.

1. **A documentation claim was wrong, and it was the kind that matters.** The
   estimator's capacity comment said 16 retained samples cover every window this
   stage considers "at any ordinary control period (5 ms or slower)". With samples
   P milliseconds apart, 16 samples span at most 15 * P, so an 80 ms window needs
   P >= 5.33 ms: at a 5 ms control period the largest candidate would have been
   refused on the bench. The comment now states the arithmetic bound, and the new
   `test_the_too_fast_rate_refusal_is_not_latched` pins that the refusal is a
   property of the retained history rather than a latched state - once the sample
   spacing is wide enough, the estimator produces numbers again on its own.
2. **"No filtering" overstated the module.** A window averages over its span;
   that is what a window is, and it is why the sparse-tick case shows quantization
   at all. The doc now says the only processing is the net tick difference over
   the covered span times the supplied scale, instead of implying that the module
   adds nothing to the samples.
3. **The regulator's missing wrong-way guard is now stated and pinned.** If the
   measured velocity opposes the target, the error is simply large and the
   regulator asks for full effort in the commanded direction. That is deliberate -
   a wrong wheel direction, a stall or a slip is the run state machine's and the
   fault path's problem, not a velocity loop's - but it was implicit. It is now in
   the module's "what this is not" list and in a test, together with the
   zero-target case where the effort interval collapses to {0} and no powered
   effort is asked for at all.
4. **A redundant condition was removed.** The anti-windup test carried
   `demandAtLimit &&` on both branches of a comparison that already implies it.
   Behavior-neutral; removed so the condition reads as what it is.
5. **The boundary between the three modules had no test at all.** Each module was
   tested in isolation, so a unit or sign error *between* them would have passed
   everything. `test_motion_chain` now pins: that the three modules agree on
   metres per second (2.0 m/s of target minus 1.0 m/s of measured speed is exactly
   1.0 m/s of error, so a millimetres-per-second boundary anywhere in the chain
   would be visible immediately); that the two polarity fields are independent and
   both are used; that a warming-up estimator, or a provisional rollout scale,
   produces feed-forward-only with nothing integrated; that a target shaped to
   zero becomes no powered effort rather than a drive; that the loop's effort is
   refused by an unarmed `SafetyAuthority` and authorized-and-clamped by an armed
   one; and that a prescribed trace stays inside every limit. None of it simulates
   a vehicle: the trace's motion is prescribed rather than derived from the
   commanded effort, and the test says so in its own header.
6. **The timing estimate's deliberate omission is now stated where it is read.**
   The travel taper subtracts the measured brake offset; the duration estimate
   does not. `motionDurationS` answers "how long does this distance take" over the
   whole distance and said so, but the field a caller actually reads - the
   summary's `travelDurationS` - did not carry the consequence: a vehicle
   following the taper reaches a zero target about
   `brakeOffsetM / cruiseVelocityMps` seconds before that value and then still has
   the offset to cover under whatever brake or settle the run layer commands. That
   is now written next to the field, so a future reader cannot quietly "correct"
   the estimate into a different meaning. No number and no behavior changed.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **209 test cases, 209 succeeded** (was 111). The new
  targets are `test_velocity_estimator` 32/32, `test_velocity_controller` 35/35,
  `test_motion_profile` 24/24 and `test_motion_chain` 7/7 (the last one added by
  the audit pass above); the existing targets are unchanged:
  `test_core_types` 16/16, `test_interval_timer` 6/6,
  `test_quadrature_decoder` 15/15, `test_odometry` 31/31,
  `test_run_config` 14/14, `test_safety_authority` 29/29.
- No compiler warnings were produced anywhere in this change. The `nano_r4` build
  output contains no `warning` or `error` line, and all **ten**
  `test/*/test_main.cpp` files compile with the host toolchain under the project's
  exact flags (`-std=gnu++11 -Wall -Wextra -fsyntax-only`, the same include paths
  the runner uses) with **zero diagnostics** - a stronger check than reading the
  test runner's quiet output.
- **The modules were compiled for the real target, not only syntax-checked.** A
  single translation unit that instantiates the estimator, the controller and the
  profile - with a provisional (non-measured) configuration, so that the refusal
  paths are part of the emitted code - builds with the platform's own architecture
  flags (`arm-none-eabi-g++ -std=gnu++11 -Os -mcpu=cortex-m4 -mthumb
  -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Wall -Wextra`) with **no diagnostics**, at
  **2108 bytes of text and 424 bytes of static data** for one loop step across all
  three modules, with an **88-byte stack frame** (44 bytes of pushed registers,
  including one double-precision register, plus 44 bytes of locals). Two facts
  worth keeping from it: (a) there is no 64-bit division helper in the object - the
  64-bit tick difference costs registers, not a library divide; (b) even with the
  FPU enabled the object still needs `__aeabi_dadd`, `__aeabi_dmul`,
  `__aeabi_ddiv`, the double comparisons and `sqrt`, because this core's
  floating-point unit is single-precision only, so the `double` convention used
  throughout the pure core is library-call arithmetic on this MCU. That is a
  property of the convention rather than a defect in these modules, and nothing in
  this change optimizes against it - it is recorded so that a future flash or
  loop-time problem is diagnosed from a number instead of from a guess. It is also
  why `src/main.cpp`, which still includes none of these modules, gives no size
  evidence about them.
- Estimator cases cover: the zero configuration being invalid; a constant-speed
  trace giving exactly the windowed speed with exactly the requested span; the
  realized span being reported when samples are sparse; a partial span producing
  no number; an update rate too fast for the window reported as
  `WindowExceedsHistory` instead of a silently shorter window; a valid zero over a
  covered window; a reversal inside a window reporting the net tick change; a
  negative speed carrying no direction claim; a full-range int32 tick difference
  in one window; an irregular 51 ms realized span on uneven samples; the claim
  being earned exactly at the timeout and not one millisecond early; the claim
  clock restarting at the observing sample; the claim never being made on an
  unusable scale or an invalid configuration; repeated and rewound stamps refused
  without entering the window; the window working across the `millis()`
  wraparound; provisional, rule-derived, zero, negative, NaN and infinite scales
  refused with no number; a measured scale whose product overflows refused;
  agreement with `distanceScaleStatus()`; four window configurations agreeing on a
  constant-speed trace while reporting their own spans; sparse low-speed
  quantization differing by window; a stop becoming visible one window before the
  claim; an edge visible only in the sample that reports it; counters; reset; and
  an invariant sweep over windows, timeouts and scales.
- Controller cases cover: measured-value and malformed-value configuration
  validation; an unusable configuration that cannot be enabled; nothing driving
  before `enable()` and `disable()` dropping the state; feed-forward-only during
  estimator warm-up and for an unusable-but-non-finite measurement; zero, positive
  and negative error; an overspeed never asking for powered effort the other way;
  the polarity being configuration rather than assumption; the effort ceiling
  clamping; a zero target asking for no powered effort; an effort that quantizes
  to zero being `Disabled`; the integral accumulating over the measured dt (20 ms,
  37 ms, 5 ms); a stalled loop clamped to the maximum integration step while
  reporting the real 500 ms; integration stopping under saturation and resuming
  when the error no longer pushes into the limit; the integral clamp; a zero
  integral ceiling disabling integral action; a sign change dropping the integral
  and a transition to zero doing the same while a same-sign target never does; a
  non-finite target preserving the integral and a corrupt measurement dropping it;
  a non-advancing stamp freezing integration while still regulating; wraparound in
  the time base; `reset()`; re-enabling never resuming; and three property sweeps
  (every output a legal effort within the ceiling, never a mode opposite the
  target, never `Coast` or `Brake`, and no powered effort from a corrupt
  measurement).
- Profile cases cover: measured-value and malformed-value limit validation; a
  cruise above the measured ceiling refused rather than clamped; unusable limits
  and invalid inputs never shaping a target; a travel from rest ramping at the
  acceleration limit with both rate brackets reported; the ramp reaching cruise
  after exactly the configured time; a 1000 ms step reaching the taper in one step
  where a 20 ms step only moves 0.01 m/s; an uneven step sequence measured step by
  step; the taper being monotonic in the remaining distance and matching
  `sqrt(2*a*s)` after the brake offset; the taper pulling the target down instead
  of snapping it; the measured brake offset being subtracted; the target going to
  zero and staying there once the offset reaches the target point; a stop request
  ramping down without a sign flip; a direction change ramping through zero; the
  polarity mapping; no elapsed time holding the previous target exactly with the
  ceilings still reported; timing-intent validation; the trapezoid and triangular
  durations; zero distance; every unusable-limit and bad-distance refusal; the
  timing summary adding the delay and reporting a positive and a negative margin;
  and a property sweep over remaining distances, current targets, step lengths,
  intents and directions pinning that the target never exceeds its ceilings and
  never flips sign inside a step.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash 13.0%
  (**34152 / 262144 bytes**). Unchanged from D6, as expected: `main.cpp` still
  does not include the pure core, so the new modules are not linked into the
  firmware and nothing about firmware size is evidence about them.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over all
  **fourteen** `lib/evcore/src/*.h` headers: **0 unexpected diagnostics** (only the
  expected `#pragma once in main file` artifact). This remains the only check that
  compiles the pure core for the target, because `main.cpp` does not include it
  yet.
- A forbidden-dependency scan of the three new headers for `Arduino`, `Serial`,
  `pinMode`, `digitalRead`, `digitalWrite`, `attachInterrupt`, `analogRead`,
  `millis`, `delay`, `CPR`, `wheel`, `circumference`, `gear`, `alloc` and
  `malloc`: every match is inside a doc comment that names what the module is
  *not*; there is no include, no clock read and no allocation. `nowMs` is an
  argument for exactly that reason.
- `git diff` reviewed: three new headers (`VelocityEstimator.h`,
  `VelocityController.h`, `MotionProfile.h`), four new test targets
  (`test_velocity_estimator`, `test_velocity_controller`, `test_motion_profile`,
  `test_motion_chain`), `SOFTWARE_ARCHITECTURE.md` (new section 13 plus the
  section 10 bullet and the section 12 closing bullet that said velocity
  estimation was unbuilt) and this file. No `src/` change, no reformatting, no
  unrelated cleanup, and none of the frozen P2/P3A modules were modified.

**Not verified**

Nothing about the vehicle. Software verification only; no physical test is
applicable because no motor, driver, encoder or drivetrain is available. No
estimator window is declared adequate, no gain is a recommendation, no
acceleration or brake-offset fixture is a measurement, and the duration estimate
has never been compared with a real run. Every hardware row in `VERIFIED_STATE.md`
stays UNVERIFIED and no row was added or modified. Specifically still open:
encoder direction sign (#4, which the two polarity fields now depend on), encoder
signal quality (#5), rollout (#6, which the estimator's scale depends on), gains
(#8), braking (#9, which the taper's limits and brake offset depend on) and wheel
slip (#10, which decides whether a motor-encoder window is usable at low speed at
all). These tests prove the arithmetic, the refusals and the reset semantics with
supplied numbers; they are not evidence that any of it is achievable by the
vehicle.

**Pending physical test (the sequence that would let any of this be evaluated)**

1. **Direction polarity (#4), still first.** With the drivetrain unloaded and the
   motor driver outputs disabled, hand-turn the wheel through the positive
   direction and record the sign of the decoded tick count, per D5. Only that
   measurement may fill `positiveCommandDirection` and `positiveEffortMode` as
   `Measured`, and only then can the regulator drive at all.
2. **An open-loop low-power straight run with logging**, one behavior at a time,
   conservative effort, wheels unloaded where possible: log `SensorSnapshot`
   `(elapsedMs, motorEncoderTicks)` at the control period for at least 10 s of
   steady motion, then run the same trace through the 20/40/60/80 ms
   configurations offline and compare quantization, lag and the zero-speed claim
   timing against the measured trace. Expected evidence: a window whose readings
   are stable enough to distinguish, and a claim timeout that does not fire while
   the vehicle is creeping. That comparison - not this stage - is what selects an
   estimator.
3. **Braking-distance runs (#9)** in each direction at two or three speeds, on the
   real surface: mark the stop position, command the stop, measure the extra
   travel, and repeat at least five times per speed. Expected evidence: a spread
   rather than a single number, filling `forwardBrakeOffsetM` and
   `reverseBrakeOffsetM` and giving a first deceleration limit. Until then the
   taper's limits stay unfilled.
4. **Gains (#8) only after the above**, on the bench with the wheels unloaded and
   the effort ceiling kept low, starting from feed-forward + proportional only
   (integral ceiling 0) and adding integral action only if a steady-state error is
   actually observed. Every value recorded with the run that produced it.

Reversal, settling and the competition state machine stay out of scope until #4
and #9 are measured: this stage's tests pin that a reversal request is *held at
zero*, not that the vehicle can perform one.

**Next stage**

Still P3, and still blocked on hardware: the channel sampling layer
(`attachInterrupt ... CHANGE` in `src/`, handing levels to `QuadratureDecoder`)
plus the first bench bring-up, with the estimator and the regulator wired to
telemetry only - not to a stop decision and not to an armed run. The
`SafetyAuthority` boundary remains the only path to an authorized command, and the
controlled reversal and competition state machine remain out of scope.

---

## D8 - 2026-09-25 - P4I audit fixes - Motion-chain wiring contracts, freshness ownership, and a new-run reset sequence

**Why**

A read-only integration audit (P4I) of the motion chain - `QuadratureDecoder` ->
`Odometry` -> `VelocityEstimator` -> `MotionProfile` -> `VelocityController` ->
`SafetyAuthority` - returned "COHERENT AFTER SPECIFIC FIXES". The ownership
boundaries, the sign chain, the unit handling and every refusal path passed;
every finding was vehicle-layer wiring or documentation debt, currently
fail-closed. This change addresses the five required fixes plus the findings the
audit raised alongside them (the directional limit-set selection, the
refusal-derived stop, and the two velocity-carrying orphans).

No control behavior changed: no gain, limit, window, threshold or state
transition was added, and no hardware fact was written down. The two pure
additions to runtime behavior are a stamp on an existing answer and an
orchestration function that only resets state.

**Decisions**

1. **The target/time contract has one named owner, and `MotionRequest` is
   explicitly vocabulary only.** `MotionRequest` (`Motion.h`) duplicates
   `MotionTimingIntent::targetTimeS` in its `timeBudgetS`, is constructed by
   nothing and consumed by nothing. It is now documented as vocabulary only,
   with `MotionProfileInputs` plus `MotionTimingIntent` nominated as the single
   owner of a run's distance/time contract and an instruction to delete the
   orphan rather than wire it in when that ownership decision is superseded.
   Existing validation and tests of the type are kept because its `ok`/`error`
   invariant is pinned there; no firmware path may consume it.
2. **The two effort ceilings are one invariant, not two tuning knobs.**
   `VelocityControllerConfig::maxEffortPermille` must equal
   `SafetyLimits::maxEffortPermille` before arming, and
   `ev::effortCeilingsAgree()` (SafetyLimits.h) is the check the vehicle layer
   runs while building the configuration. The controller field now documents
   why: anti-windup and `outputSaturated` are computed against the controller's
   own ceiling only, so a looser safety ceiling would let the loop integrate
   headroom it does not have and split one saturation across
   `outputSaturated == false` and `AuthorizedClamped`. The chain test pins both
   the agreement case (the controller's own saturation is the whole story and
   the authority returns `AuthorizedDrive`) and the mismatch case (800 permille
   reported unsaturated by the loop, clamped to 600 by the authority).
3. **A new run is one sequence, not a checklist in prose.**
   `ev::resetMotionChainForNewRun()` in `lib/evcore/src/MotionChainReset.h`
   resets the decoder, the estimator, the regulator (disable-then-enable,
   because `enable()` on a running regulator is a deliberately pinned no-op),
   the caller-held target and the authority, in that order, and returns a report
   carrying `controllerEnabled` and the `FaultCode` it discarded. It never arms
   and never commands motion; the chain test pins that after it, the regulator
   is enabled on a fresh state, every module's counters and reference are gone,
   the authority is disarmed with no direction memory, and an unarmed
   `authorize()` still refuses the regulator's effort.
4. **The profile-target handoff is a type, not an argument passed by hand.**
   `ev::MotionProfileTarget` (`MotionProfile.h`) owns the previous target that
   the stateless shaper needs: zero value exactly zero, `accept()` for a shaped
   command (a refusal or a non-finite target parks it at zero), `inputsFor()` so
   the handoff cannot be passed by hand, and `resetForNewRun()` as the only
   run-boundary clear. Profile tests pin the ripple a stale target causes
   (1.01 m/s instead of 0.03 m/s for the same geometry) and that the reset
   restores the from-rest ramp.
5. **Directional limit sets are selected by direction once.**
   `ev::DirectionalMotionProfileLimits` holds the forward and reverse
   `MotionProfileLimits` and returns the right one from `forDirection()`, so the
   forward brake offset or ceiling cannot be attached to a reverse leg by hand
   with no compiler complaint.
6. **Velocity freshness now has an owner on the velocity path.**
   `VelocityEstimate::elapsedMs` carries the clock reading of the `update()`
   that produced an answer (0 for a refusal, which has nothing to age), and
   `ev::velocityReadiness()` (`VelocityEstimator.h`) mirrors
   `odometryReadiness()`: existence, then validity, then age, with the same
   fail-closed `SampleAge`. The vehicle layer maps `readiness.usable` onto the
   regulator's `measurementUsable`, and the controller documents that it cannot
   detect a reused estimate itself. This closes the audit's "no owner" finding
   without inventing a threshold: no staleness limit lives in `lib/`.
   The chain test pins a valid estimate going `SampleStale` at 240 ms and the
   regulator falling back to feed-forward only.
7. **`zeroSpeedClaimed` can never become `atZeroSpeed` on its own.** The
   producer contract is now stated at `SafetySystemState::atZeroSpeed`: a fresh
   valid estimate, the explicit claim and the observed interval, with the facts
   it used stated. Tests pin that a claim reports `EstimateInvalid` rather than a
   usable velocity and that a zero target still produces no powered effort from
   it. Nothing in `lib/` sets the safety field.
8. **The tick-convention -> physical sign reconciliation is documented where it
   is missing.** `positionFromOriginM()` adds a caller-supplied run-axis origin
   to travel that inherits the tick-counting convention and applies no polarity;
   `Odometry.h` and `MotionProfile.h` (`remainingDistanceM`) now state that the
   vehicle layer must reconcile the two conventions once through the measured
   polarity, and that until it is measured a physical origin must not be
   combined with tick-derived travel. The architecture contract in section 12
   records the same rule.
9. **One authority, two request sources, arbitration point named.**
   `SafetyAuthority` now documents that a run will present both the regulator's
   `MotorEffort` and the run layer's own Brake/Coast stop to the same
   `authorize()`, that the run layer owns which single request is presented each
   cycle, and that calling `authorize()` twice and acting on the later result
   would make the direction memory and fault response depend on call order.
10. **A refusal-derived stop is a documented stop.** `noteStop()` now states
    that a Disabled request (which is what a refusing regulator emits) taken
    while zero speed is reported clears the direction latch exactly like a
    deliberate stop, while a malformed request (`DeniedIllegalEffort`) does not,
    and that the run layer owns that consequence. `clearFault()` keeps its
    direction memory and `reset()` is the only clear, now stated at both call
    sites.
11. **The velocity duplication orphan is resolved by ownership.**
    `OdometrySnapshot::velocityMps` stays deliberately unfilled by the odometry
    layer; speed is owned by `VelocityEstimate` plus its freshness verdict, and
    the field comment plus architecture section 12 forbid copying a second
    velocity there for telemetry convenience.
12. **What was deliberately not done.** The audit's "existing ceilings are
    unenforced" finding (`minBatteryV`, `maxRunDurationS`) is documented as
    data with no enforcement point yet - building a run timer or battery check
    now would be a new control behavior the audit did not ask for. No arbiter
    was written for the two request sources, and no `atZeroSpeed` producer was
    written: both need the run state machine, which stays out of scope.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **221 test cases, 221 succeeded** (was 209). Changed
  targets: `test_motion_chain` 13/13 (was 7), `test_motion_profile` 26/26 (was
  24), `test_velocity_estimator` 36/36 (was 32). Unchanged targets:
  `test_core_types` 16/16, `test_interval_timer` 6/6, `test_odometry` 31/31,
  `test_quadrature_decoder` 15/15, `test_run_config` 14/14,
  `test_safety_authority` 29/29, `test_velocity_controller` 35/35.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D7: `main.cpp` still does not
  include the pure core, so none of this change is linked into the firmware.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over all
  **fifteen** `lib/evcore/src/*.h` headers (the new `MotionChainReset.h`
  included): **0 unexpected diagnostics** (only the expected `#pragma once in
  main file` artifact, one per header).
- A single translation unit that instantiates the new paths - decoder update,
  `velocityReadiness()`, a provisional-config regulator refusal, the profile
  target handoff and `resetMotionChainForNewRun()` - compiled with the
  platform's own architecture flags (`-std=gnu++11 -Os -mcpu=cortex-m4 -mthumb
  -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Wall -Wextra`) with **no diagnostics**, at
  **1156 bytes of text and 312 bytes of static data** (the static probe objects).
  The temporary file was removed after the check.
- A forbidden-dependency scan of the new header for `Arduino`, `Serial`,
  `pinMode`, `digitalRead`, `digitalWrite`, `attachInterrupt`, `analogRead`,
  `millis`, `delay`, `CPR`, `wheel`, `circumference`, `gear`, `alloc` and
  `malloc`: the only match is a doc comment naming what the module is *not*.
- `git diff` reviewed: `Motion.h`, `SafetyLimits.h`, `Snapshots.h`,
  `Odometry.h`, `MotionProfile.h`, `VelocityEstimator.h`, `VelocityController.h`,
  `SafetyAuthority.h`, the new `MotionChainReset.h`, the three test targets,
  `SOFTWARE_ARCHITECTURE.md` (sections 7, 12, 13) and this file. No `src/`
  change, no reformatting, no unrelated cleanup, and no hardware fact added or
  modified. `VERIFIED_STATE.md` is untouched.

**Not verified**

Nothing about the vehicle. Software verification only; no physical test is
applicable because no motor, driver, encoder or drivetrain is available. The
new tests prove that an answer's stamp ages, that a stale or claimed answer is
refused as feedback, that the reset sequence clears every per-run state, and
that the ceiling-agreement check detects a mismatch; they are not evidence that
any of it is achievable by the vehicle. Every hardware row in `VERIFIED_STATE.md`
stays UNVERIFIED, and no row was added or modified.

**Pending physical test**

Unchanged from D7, and the new contracts add nothing physical to it: direction
polarity (#4) first, then the open-loop logged run for estimator window
selection (#5, #6, #10), then braking-distance runs (#9), then gains (#8). The
new `atZeroSpeed` producer contract stays unimplemented until #5 and #9 give it
something measured to combine; until then the legal reversal path remains
fail-closed, which is the intended state.

**Next stage**

Unchanged from D7: the channel sampling layer and the first bench bring-up, with
the estimator and the regulator wired to telemetry only. The `SafetyAuthority`
boundary remains the only path to an authorized command, and the run state
machine, the request arbiter and the competition sequence remain out of scope.

---

## D9 - 2026-09-25 - P5 - Controlled reversal and the competition run state machine

**Entry gate**

P4 was accepted: `RunConfig` and the motion contracts exist. Every physical
tuning value in them remains UNVERIFIED - the near-zero threshold, the braking
behavior, backlash, dwell and the rest have never been measured on a vehicle.
This entry therefore adds sequencing and policy, and no measured fact.

**Why**

The run needs two things that did not exist: a way to earn a powered direction
change instead of commanding one, and an explicit state machine for a whole
competition run instead of a sequence of delays. They were built together because
they share exactly one boundary - the earned direction change - and that boundary
had to be built once, in one place, rather than half in a reversal helper and
half in the run logic.

The work was deliberately scoped to stop before the scripted scenario/fault
harness, before any vehicle-layer wiring and before any hardware: `src/` is
unchanged, `main.cpp` still does not include the pure core, and no pin, driver
truth table, effort, gain, geometry or threshold value was written down.

**Decisions**

1. **Near-zero verification is its own module, and it consumes three named
   facts.** `ev::NearZeroVerifier` (`lib/evcore/src/NearZeroVerifier.h`) answers
   one question - has the vehicle been within the threshold, continuously, for
   the dwell - from exactly the facts the safety contract names: a fresh, valid
   velocity estimate (`velocityReadiness().usable`), the measured speed within
   the threshold, and the estimator's explicit `zeroSpeedClaimed`. The verdict
   reports which of the three held, so a refused reversal can be attributed to
   the missing fact instead of being guessed at. This is the producer section 13
   said would be needed for `SafetySystemState::atZeroSpeed`; the module returns
   a verdict, and the vehicle layer maps it onto that field.
2. **The dwell must be continuous, so oscillation can never verify.** Any update
   whose evidence does not hold clears the observed interval, a stamp that does
   not advance holds it rather than growing or clearing it, and a negative speed
   within the threshold counts (which sign is physical forward travel is still
   UNVERIFIED, #4). A vehicle bouncing on the threshold therefore never earns a
   reversal, however long it is watched.
3. **No verification value lives in `lib/`.** The threshold and the dwell arrive
   as `Sourced` `NearZeroConfig` values; a configuration that is not `Measured`
   produces `ConfigIsNotUsable` instead of a verdict, and a non-positive
   threshold or dwell is refused rather than reinterpreted. Until a stop is
   measured (#5, #9), the honest configuration is a placeholder and the honest
   result is a refusal that authorizes nothing.
4. **Reversal sequencing is its own module with one bound.**
   `ev::ReversalSequencer` (`lib/evcore/src/ReversalSequencer.h`) possesses the
   sequence `RampingDown -> Braking -> VerifyingNearZero -> DirectionChangeReady`
   and the deterministic `StopRequired` refusal. `reversalTimeoutS` covers the
   ramp-down and the verification together, because a second bound would give one
   failure two owners. `Braking` is deliberately distinct from
   `VerifyingNearZero`: the first is "within the threshold but the count is still
   changing", the second is "the claim arrived and the dwell is accumulating",
   which is exactly the sequence decelerate -> brake -> verify -> permit.
5. **The permission is latched, restarts are refused, and refusals are
   immediate.** Once earned, `DirectionChangeReady` survives later moving
   evidence (a verified stop does not stop being verified), and only `reset()`
   clears it. A second request while a sequence runs is `AlreadyActive` and does
   not restart the timeout, so a vehicle that keeps stalling cannot wait forever.
   An unusable configuration refuses at `beginReversal()` before any time passes,
   and that refusal is latched: the configuration is the cause, so later attempts
   keep reporting it rather than being masked by the latched stop.
6. **The run phases are frozen, and there is only one phase vocabulary.**
   `ev::RunPhase` (`Idle`, `ParameterValid`, `Armed`, `OptionalDelay`,
   `Approach`, `Capture`, `Push`, `ForwardDecel`, `BrakeZeroSpeed`, `Reverse`,
   `FinalApproach`, `FinalBrake`, `Done`, `Fault`) is the frozen behavioral
   sequence, with `Idle` as the zero value. `BRAKE/ZERO_SPEED_VERIFY` is one
   state, `BrakeZeroSpeed`, because it is one stage of the sequence: the stop is
   presented and the stop is verified in the same state, and the sequencer's
   phase distinguishes braking from verifying. `CompetitionState` was deleted
   from `VehicleState.h` rather than left beside it - the same reasoning D8
   applied to `MotionRequest`: a second, competing vocabulary is a name that will
   disagree with itself.
7. **`Fault` is reachable from every phase, `Done` and `Fault` are terminal, and
   reset is explicit.** Fault evidence is processed before anything else in an
   update, so nothing is acted on in a cycle that reports a fault, from any phase
   including `Idle` and `Done`. The fault of record is the first one raised (a
   later, different fault does not replace it), latching discards the reversal
   sequence, the dwell and the verified stop, and only `reset()` leaves `Fault`
   or `Done`. After a reset, a run needs a fresh announcement and a fresh arming
   decision; `announceRun()` refuses while a fault is of record, and the disarm
   path is explicit: before the trigger a disarm returns to `ParameterValid`
   (nothing has moved and the announcement still stands), during an underway run
   it faults (`DisarmedDuringRun`), and after `Done` it is the normal post-run
   shutdown and is not a fault.
8. **`Done` never permits travel.** `Done` presents a deliberate brake hold and
   no travel command, and no evidence can move it: the acceptance test drives a
   whole nominal run through `Done` with every flag asserted and asserts that no
   travel command is ever emitted again.
9. **Every phase exit is evidence, and every bound is an input.** Contact,
   capture, push, the closing distance and the verified final stop come from the
   vehicle layer's evidence under `Sourced` bounds; none is inferred from
   distance or elapsed time. ForwardDecel and BrakeZeroSpeed have no separate
   bound on purpose (the reversal timeout owns both), and a positive bound that
   converts to zero whole milliseconds expires immediately rather than becoming
   an unbounded wait - caught while writing the timeout conversion and pinned by
   a test.
10. **The reversal is requested through the reversal contract, never
    re-implemented.** `Push` calls `beginReversal()` on the way into
    `ForwardDecel`; `ForwardDecel` ramps the forward leg down and moves to
    `BrakeZeroSpeed` once the vehicle is within the near-zero threshold;
    `BrakeZeroSpeed` consumes the earned permission. A refusal from the sequencer
    faults through one mapping function (`faultForReversalRefusal()`), so the
    sequencer never names a fault and the state machine never keeps a second copy
    of the sequence.
11. **The direction handoff presents one full stop cycle first.** A non-powered
    stop taken while zero speed is reported is what clears the authority's
    remembered powered direction (section 13), so the run stays in
    `BrakeZeroSpeed`, keeps asking for the stop and reports `stopVerified` for
    one cycle after the permission is earned, and asks for the reverse travel on
    the next update. Presenting the travel in the same cycle would be refused by
    the authority - correctly, and invisibly to the run - so the deliberate cycle
    exists to make the handoff explicit rather than accidental. The authority
    still refuses independently: if the handoff was not true, the run's own phase
    bound faults it instead of letting it hang.
12. **An intention is valid for one cycle.** `RunMotionIntention` carries the
    clock reading that produced it and `runIntentionIsCurrentFor()` is the single
    verdict a consumer uses, so a Travel command held across a skipped cycle, a
    stalled loop or an ended run is refused rather than driven on.
13. **Run faults are identity, and the response stays the policy's.** `FaultCode`
    gained `PhaseTimeout`, `ReversalNotVerified`, `DisarmedDuringRun` and
    `RunSequencingError`; all four are software conditions with no measured
    electrical answer, so `faultStopPolicyFor()` maps every one of them to
    `Disable` explicitly (rather than relying on its safe default).
14. **The final stop is the event's Complete Stop.** `FinalBrake` waits for the
    near-zero verdict under `finalStopTimeoutS`, and rule 8.p's 3 s interval is
    now the authoritative constant `ev::rules2027::kCompleteStopS` so the vehicle
    layer configures the final dwell from the rule rather than retyping a number.
    The reversal gate's dwell is a separate value on purpose: enough evidence to
    permit a powered flip is a different ask from holding the Complete Stop.
15. **The announcement is consumed, validated and not stored.** `announceRun()`
    takes the `RunConfig` with the vehicle layer's validation verdict
    (`validateRunConfig()`) and calls the same single entry point again as a
    cross-check, so a caller bug cannot announce a run that was never validated.
    The parameters are not kept: `RunConfig` has no default constructor and
    nothing in the run policy may derive geometry from it, so the vehicle layer
    remains the owner of the announced run and of the run-axis bookkeeping.
16. **A new run resets the run policy too.** `resetMotionChainForNewRun()` now
    also resets the state machine (and, through it, the reversal sequencer and
    the final-stop verifier it owns) and reports the phase it discarded, exactly
    as it reports the fault it discarded. A run that ended in `Done` and a run
    abandoned in `Approach` used to look identical after a reset; now the log can
    say which one it was.
17. **One conversion from seconds to milliseconds, with explicit saturation.**
    `msFromSeconds()` (`Units.h`) is the single place a duration becomes a
    `millis()`-comparable interval: a non-positive or non-finite duration becomes
    0, and a duration longer than the clock can express saturates instead of
    wrapping. The run policy treats a configured bound as at least 1 ms.
18. **What was deliberately not done.** No vehicle-layer wiring, no request
    arbiter, no telemetry line, no serial diagnostics, no scenario or fault
    harness, no hardware configuration and no `src/` change. No value was
    invented to make a phase reachable: with every policy value still a
    placeholder, `announceRun()` refuses the run outright, which is the intended
    fail-closed state until the measurements exist.

**Corrections made while working**

- The reversal test expected `StopLatched` from a second `beginReversal()` on an
  unusable configuration, but the implementation checks the configuration first
  and reported `ConfigIsNotUsable`. The implementation was kept (a refusal should
  name its cause, not the latched stop it left behind), the ordering was
  documented at `beginReversal()`, and the test was corrected.
- Two FSM test fixtures were wrong rather than the code: one asserted the phase
  after an update whose evidence (an armed fact) legitimately moved the run from
  `ParameterValid` to `Armed`, and one asserted "no travel command" inside a loop
  whose last iteration could legitimately reach `Reverse`. Both tests were
  corrected.

**Self-audit findings**

After the suite was first green and the build and the platform syntax check had
passed, the new code was re-read against its own doc comments and against the
acceptance criteria. Each finding below is a real discrepancy the first green run
did not expose, and each is now fixed and pinned by a test.

- **A claim in the near-zero header that the code did not make.** The
  documentation implied a skipped update could not credit dwell, but an update
  with no new clock reading credits the elapsed lead-in time. The behavior is the
  one the contract wants - the dwell is measured against the clock, and a caller
  that stops calling is not evidence of motion, so a stop stays a stop until
  something reports otherwise - so the claim was withdrawn and the semantics were
  written down at the update and pinned by
  `test_an_update_gap_is_credited_as_continued_evidence`. A doc comment that
  overstates a guard is worse than no comment: it teaches a reader to trust a
  refusal that will not come.
- **A stalled loop could have verified.** The sequencer evaluated the timeout
  after the verification step, so a caller that stopped advancing its clock could
  reach a state that the timeout should have refused. The timeout is now
  evaluated first, and `test_a_stalled_loop_refuses_instead_of_verifying` pins
  it. This is the same class of defect the phase bounds were audited for: a bound
  that can be outlived by simply not making progress is not a bound.
- **A non-finite remaining distance could have advanced a phase.** The distance
  evidence only required a usable `Sourced` value; `distanceEvidenceUsable()` now
  also requires `isfinite()`, and
  `test_a_non_finite_remaining_distance_does_not_end_the_final_leg` pins it. A
  NaN compares false against every bound, so it could otherwise have starved a
  phase rather than ending it.
- **A phase bound documented as a floor rather than a deadline.** The bound is a
  deadline: evidence that arrives after it faults the phase. This is now stated
  at the conversion and pinned by
  `test_the_phase_bound_is_inclusive_and_late_evidence_is_refused`, which also
  asserts the inclusive boundary - evidence exactly on the bound is accepted,
  one millisecond past it is refused.
- **A disarm could have lost to the trigger.** In `Armed`, an update that
  disarms and triggers at once had one ordering; the disarm now wins outright
  (nothing is underway, so the announcement stands and the phase returns to
  `ParameterValid`), and the test that covered the path was strengthened to
  assert it in the same update.
- **A verified stop could have been reported as the vehicle being stopped during
  the next leg.** `stopVerified()` was `sequencer_.stopVerified() ||
  finalStopVerified_`, which reads as "this run has verified a stop at some
  point". It happened to be correct only because the handoff resets the sequencer
  on the one path into `Reverse`, so the honesty of a safety-facing verdict rested
  on a side effect of a different step. It is now derived from the phase the run
  is presenting - the reversal gate's stop in `BrakeZeroSpeed`, the final-stop
  verifier's in `FinalBrake` and `Done`, false in every travel phase - so "the
  vehicle is stopped" is a property of the stop being presented and a later leg
  structurally cannot inherit an earlier leg's stop. Today's behavior is
  unchanged (the existing assertions on the reverse cycle, `FinalBrake` and
  `Done` all still hold), which is the point: an invariant that only happens to
  hold is not an invariant.
- **Two phases with no bound could have been waited on forever.** `ForwardDecel`
  and `BrakeZeroSpeed` deliberately have no timeout of their own - the reversal
  sequence's single bound owns both - so a sequence that is neither running nor
  refusing while the run sits in them (a sequencer that is `Idle`, or a
  `StopRequired` refusal that names no cause) would have presented a ramp-down
  forever with nothing left to expire. Both are now `RunSequencingError` faults.
  Neither is reachable while the sequencer's own invariants hold, which is
  precisely why they must not be silent: a state that is impossible today and
  unasserted is a state that is merely unreached.
- **A refusal's evidence evaporated on the next read.** `ReversalStatus::
  sequenceElapsedMs` returned 0 on every update after the one that refused, so a
  log reading the refusal a cycle later lost how long the sequence had been
  given. The refusal now latches the elapsed time it was raised at, the field is
  documented per phase, and the new
  `test_a_refusal_keeps_reporting_the_time_it_was_given` pins both halves of it,
  including the config-refusal path where no sequence began and 0 is the honest
  answer.
- **Dead surface removed.** The unused `reversal()` accessor and the
  announcement fields that were stored but never read were deleted. Storing the
  announced configuration would have created the second copy of the run
  parameters that decision 15 exists to avoid.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **289 test cases, 289 succeeded** (was 221 before this
  stage). New targets: `test_competition_state` 36,
  `test_near_zero_verifier` 16, `test_reversal_sequencer` 16. Changed targets:
  `test_run_config` 15 (was 14), `test_core_types` 15 (was 16: the superseded
  phase-vocabulary case moved to `test_competition_state`). Unchanged:
  `test_interval_timer` 6, `test_motion_chain` 13, `test_motion_profile` 26,
  `test_odometry` 31, `test_quadrature_decoder` 15, `test_safety_authority` 29,
  `test_velocity_controller` 35, `test_velocity_estimator` 36. No compiler
  warnings were produced.
- The new cases cover, in the verifier: unusable and degenerate configurations,
  default evidence, stale evidence, a small window number with no claim, a claim
  with an above-threshold speed, the dwell boundary (one millisecond early and
  exactly on time), negative speeds, a broken dwell, threshold oscillation,
  boundary precision, a repeated stamp, a rewound stamp, reset, and a varied
  sequence that re-checks the verdict invariant every step.
- In the sequencer: the same-direction refusal, the unusable-configuration
  refusal and its latch, the nominal sequence, the latched permission, a
  premature request that only ramps down, a restart that does not restart the
  timeout, a never-reached near zero, stale evidence, threshold oscillation, the
  timeout that covers the whole sequence, and both reset paths.
- In the state machine: every legal transition in order with its command and the
  authority-facing verdict at each step, the one-cycle stop handoff, no
  evidence-driven phase skipping, the reversal timeout, stale and oscillating
  stop evidence, fault during a reversal, disarm during the run and during the
  reversal, every bounded phase timing out, `Done` refusing further travel, a
  fault after `Done`, reset and re-announcement, fault reachability from every
  phase (including `Idle` and `Done`), the underway-phase classification, the
  one-cycle intention verdict, rule 8.p's constant, and two integration cases:
  the authority refusing a direct reverse independently while zero speed is
  claimed, and a whole nominal run replayed through the authority asserting that
  no direction flip is ever granted without an intervening stop taken at a
  reported zero. The self-audit added the non-finite remaining distance, the
  inclusive bound with late evidence, and the disarm-in-`Armed` precedence.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D8, as expected: `main.cpp`
  still does not include the pure core, so none of this is linked into the
  firmware yet.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **eighteen** `lib/evcore/src/*.h` headers: **0 unexpected
  diagnostics** (only the expected `#pragma once in main file` artifact, one per
  header).
- A single translation unit that instantiates the new paths - verifier update,
  sequencer begin/update, decoder and estimator update, regulator step, run
  announcement and update, the intention verdict and
  `resetMotionChainForNewRun()` - compiled with the platform's own architecture
  flags (`-std=gnu++11 -Os -mcpu=cortex-m4 -mthumb -mfloat-abi=hard
  -mfpu=fpv4-sp-d16 -Wall -Wextra`) with **no diagnostics**, at **6336 bytes of
  text, 4 bytes of data and 848 bytes of bss** (the static probe objects). The
  temporary file was removed after the check.
- A forbidden-dependency scan of the new and modified headers for `Arduino`,
  `Serial`, `pinMode`, `digitalRead`, `digitalWrite`, `attachInterrupt`,
  `analogRead`, `analogWrite`, `millis`, `delay`, `malloc`, `alloc`, `wheel`,
  `circumference`, `gear`, `CPR` and `radius`: every match is a doc comment
  naming what the module consumes or is *not*. No hardware dependency and no
  geometry, gearing or encoder-interpretation fact is present.
- `git diff` reviewed: the new `NearZeroVerifier.h`, `ReversalSequencer.h`,
  `CompetitionStateMachine.h` and their three test targets; `Units.h`
  (`msFromSeconds`), `VehicleState.h` (fault codes, `CompetitionState` removed),
  `RunConfig.h` (rule 8.p constant), `SafetyAuthority.h` (explicit policy mapping
  for the new faults), `MotionChainReset.h` (the run policy in the reset sequence
  and the phase in the report), `test_motion_chain`, `test_core_types`,
  `test_run_config`, `SOFTWARE_ARCHITECTURE.md` (sections 10, 13 and the new 14)
  and this file. No `src/` change, no reformatting, no unrelated cleanup, and no
  hardware fact added or modified. `VERIFIED_STATE.md` is untouched.

**Not verified**

Nothing about the vehicle. No physical test is applicable because no motor,
driver, encoder, mechanism or drivetrain is available. The tests prove
sequencing, refusal and verdict logic over stated evidence; they are not evidence
that any vehicle decelerates, stops, verifies, reverses or reaches any phase, and
the trace they follow is prescribed, not derived from commanded effort. Every
threshold, dwell, timeout and distance used here is a fixture. Every hardware row
in `VERIFIED_STATE.md` stays UNVERIFIED and no row was added or modified.

**Pending physical test**

Unchanged from D8, and this stage adds requirements rather than removing any.
Direction polarity (#4) first, then the open-loop logged run for estimator window
selection (#5, #6, #10), then braking-distance runs (#9), then gains (#8). Newly
required by this stage: the near-zero threshold and its dwell (from the same
braking runs), the reversal timeout, each phase bound, the bottle contact,
capture and push evidence and its producers (#11), and the Complete Stop dwell
against rule 8.p. Until those exist, every run policy configuration is a
placeholder: `announceRun()` refuses the run, the reversal gate refuses the
direction change, and the authority refuses the flip independently. That
fail-closed path is the intended state, not an obstacle to work around.

**Next stage**

Stop here, before the scripted scenario/fault harness, as instructed. The next
stage is the vehicle layer that wires these modules into one loop: the
single-request-per-cycle arbitration between the regulator's effort and the run
layer's own stop (section 13), telemetry that reads the run phase together with
the estimator's and the verifier's verdicts, and the diagnostic entry points -
still with the drivetrain unloaded, and still with the `SafetyAuthority`
boundary as the only path to an authorized command.

---

## D10 - 2026-09-26 - P5I audit fixes - Polarity agreement, phase ownership, stop leg tags, reset clock, vehicle-layer obligations

**Entry gate**

The P5 work (D9) was audited for FSM / motion / safety integration coherence.
The verdict was "coherent after specific fixes": five issues, each with a named
fix. This entry implements those five only. No measured value is written down,
no hardware fact is added or modified, `src/` is untouched, and every row in
`VERIFIED_STATE.md` stays UNVERIFIED. Nothing was committed.

**Fixes**

1. **The two polarity encodings now have an agreement check.**
   `ev::polarityAgrees()` (`lib/evcore/src/SafetyLimits.h`) mirrors
   `ev::effortCeilingsAgree()` for the one UNVERIFIED polarity fact
   (`VERIFIED_STATE.md` #4), which is stored twice:
   `MotionProfileLimits::positiveCommandDirection`
   (`Sourced<MotionDirection>`) and
   `VelocityControllerConfig::positiveEffortMode` (`Sourced<MotorMode>`).
   Both fields could be `Measured` and opposite - each module valid on its own -
   and the taper and the regulator would then drive in opposite directions for
   one command sign. The check compares the physical directions the two values
   name (`Forward`/`DriveForward`, `Reverse`/`DriveReverse`), never their numeric
   values, and a non-driving mode agrees with nothing. The vehicle layer runs it
   before arming; like the effort ceilings it is a configuration-build check,
   not a runtime clamp, and provenance stays with
   `validateMotionProfileLimits()` / `validateVelocityControllerConfig()`.
2. **Presentation and evidence are frozen apart, not de-duplicated.**
   `ForwardDecel` / `BrakeZeroSpeed` are what the run *presents* the vehicle
   layer (`RampDown*` / `Brake*`); `RampingDown` / `Braking` /
   `VerifyingNearZero` / `DirectionChangeReady` are what the owned
   `ReversalSequencer` has *established*. Collapsing the pair would undo the
   frozen phase vocabulary (D9 decision 6), so the mapping is instead documented
   at the enum, at `advanceReversal()` (the single mapping site) and in
   SOFTWARE_ARCHITECTURE.md section 14, and pinned by a new test that walks the
   whole reversal half and asserts both vocabularies at every step. The three
   `stopVerified` sites are likewise frozen as one verifier and one latch per
   stop, at the level that sees the evidence: the reversal gate's verifier and
   latch inside `ReversalSequencer`, the final stop's inside
   `CompetitionStateMachine`; `RunStatus::stopVerified` only reports the latch of
   the stop being presented.
3. **Stop commands are leg tags, invisible to the authority.**
   `BrakeForward` / `BrakeReverse` (and `RampDownForward` / `RampDownReverse`)
   name the leg a stop belongs to, never a powered direction: a non-powered stop
   has none. The enum doc and SOFTWARE_ARCHITECTURE.md section 14 state the
   obligation, and a new test pins the reference mapping - every stop command
   maps to a non-powered request with zero effort and `PoweredDirection::None`,
   the two brake tags are indistinguishable to the authority, and only travel
   commands carry a direction. `RampDown*` maps to no powered effort (a
   zero-shaped target), `Brake*` to the active non-powered hold.
4. **`reset()` is stamped with the caller's clock.**
   `CompetitionStateMachine::reset(nowMs)` now sets `phaseEnteredMs_ = nowMs`
   instead of 0, so `RunStatus::phaseElapsedMs` for a fresh `Idle` measures from
   the boundary that entered it rather than from machine construction, matching
   the field's own documentation. `resetMotionChainForNewRun()` takes the
   caller's `nowMs` as its one clock argument and passes it to `run.reset()`; it
   still reads no clock. Both reset call sites in the tests pass the harness
   reading, and the reset test now asserts Idle's phase clock starts at the
   reset (50 ms after one 50 ms tick).
5. **The vehicle-layer obligations are recorded as build-time acceptance
   criteria.** The single-`authorize()`-per-cycle rule and the intention
   freshness gate (`runIntentionIsCurrentFor()`) cannot be enforced by a layer
   that does not exist, so sections 13 and 14 now state them as obligations the
   vehicle layer must satisfy when built - not conventions a later implementer
   may reinterpret. No code enforcement was invented where there is no caller.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **292 test cases, 292 succeeded** (was 289).
  `test_competition_state` **38** (was 36: the reversal
  presentation/evidence mapping and the stop-leg-tag mapping),
  `test_motion_chain` **14** (was 13: the polarity agreement). Unchanged: all
  other targets. No compiler warnings were produced.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash
  13.0% (**34152 / 262144 bytes**). Unchanged from D9: `main.cpp` still does not
  include the pure core, so none of this is linked into the firmware yet.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **eighteen** `lib/evcore/src/*.h` headers: **0 unexpected
  diagnostics** (only the expected `#pragma once in main file` artifact, one per
  header). `SafetyLimits.h` now includes `Motion.h`, which is acyclic.
- A forbidden-dependency scan of the changed headers and tests for `Arduino`,
  `Serial`, `pinMode`, `digitalRead`, `digitalWrite`, `attachInterrupt`,
  `analogRead`, `analogWrite`, `millis`, `delay`, `malloc`, `alloc`, `wheel`,
  `circumference`, `gear`, `CPR` and `radius`: every match is a doc comment or a
  test name naming what the module consumes or is *not*. No hardware dependency
  and no geometry, gearing or encoder-interpretation fact is present.
- `git diff` reviewed: `SafetyLimits.h` (the `Motion.h` include and
  `polarityAgrees()`), `CompetitionStateMachine.h` (documentation and
  `reset(nowMs)`), `MotionChainReset.h` (the `nowMs` pass-through),
  `test_competition_state/test_main.cpp`, `test_motion_chain/test_main.cpp`,
  `SOFTWARE_ARCHITECTURE.md` (sections 13 and 14) and this entry. No `src/`
  change, no reformatting, no unrelated cleanup, and no hardware fact added or
  modified. `VERIFIED_STATE.md` is untouched.

**Not verified**

Nothing about the vehicle, as in D9. The new polarity check is a check, not a
measurement: which of the two encodings describes the real vehicle is still
UNVERIFIED (`VERIFIED_STATE.md` #4), both fields stay `Provisional`, and the
agreement verdict is only a refusal to arm a chain that contradicts itself. The
phase/evidence mapping and the leg-tag mapping are pinned over stated evidence,
not over a moving vehicle.

**Pending physical test**

Unchanged from D9, with one addition: direction polarity (#4) must be measured
once on the bench, and that single measurement must set **both**
`MotionProfileLimits::positiveCommandDirection` and
`VelocityControllerConfig::positiveEffortMode`, after which
`ev::polarityAgrees()` is expected to read true. Until then both stay
placeholders: `announceRun()` refuses the run, the reversal gate refuses the
direction change, and the authority refuses the flip independently.

---

## D11 - 2026-09-26 - P6 - Full scripted scenario and fault harness

**Entry gate**

P5 and the P5I audit (D9, D10) were accepted, so the pure core's decision
pipeline is mature enough to be exercised end to end with scripted evidence.
This stage adds host tests only: no `lib/` and no `src/` file changed, no
hardware fact and no measured value was added or modified, and nothing was
committed. The stage stops before Arduino runtime/scheduler integration - there
is still no scheduler, driver layer or pin code in the repository.

**Why**

Every module and every boundary had been tested on its own, and the handoffs had
been pinned by `test_motion_chain` and `test_competition_state`, but nothing had
driven the whole chain with stated evidence for a *whole run*. Whole-run
scenarios are also where contract *composition* problems appear: each module can
be individually correct and the composition still not reach the end of a run.

**What was added**

- `test/test_scenario/ScenarioHarness.h` - test support, never compiled into the
  firmware. It feeds predetermined timestamps, an announced `RunConfig`, trigger
  and bottle evidence, scripted encoder edge sequences, sensor dropouts and
  observed faults into the real production modules (`QuadratureDecoder`,
  `distanceTravelledM`, `VelocityEstimator`, the readiness verdicts,
  `CompetitionStateMachine`, `MotionProfile`, `VelocityController`,
  `SafetyAuthority`), and implements the vehicle-layer obligations D10 recorded
  as build-time acceptance criteria: exactly one request per cycle, the
  intention-freshness gate, stop commands mapped onto non-powered requests, the
  run's verified-stop verdict mapped onto `atZeroSpeed`, one observed fault
  reported to both consumers in the same cycle, and the configuration-build
  checks (`effortCeilingsAgree`, `polarityAgrees`, per-module validation) before
  arming.
- The single authorized command reaches an `ActuatorBoundary`, which models the
  boundary's *type* only: it accepts an `AuthorizedMotorCommand` and records it,
  and performs no physical action.
- `test/test_scenario/test_main.cpp` - 18 cases: nominal IDLE -> DONE; trace
  replay determinism; only-authorized-commands; no powered reversal without a
  verified stop; FAULT/DONE safety at the boundary; a fault injected from every
  reachable phase; fault-latch persistence and clearance; a disarm during an
  underway run; seven invalid configurations; `RunConfig` changes at illegal
  times; a sensor dropout; stale odometry on the reverse leg; stale stop evidence
  during the reversal; six phase-bound timeouts; a vehicle that never comes near
  zero; a vehicle that never moves again after the reversal; a hostile
  out-of-band reverse request; a fault in the same cycle as a travel request; and
  a chain reset followed by a second full run.

It is deliberately not a simulator. No effort is integrated into a velocity, and
no torque, friction, braking distance, bottle dynamics, wheel slip or sensor
noise is modelled. Encoder readings are prescribed by the scenario, never derived
from a commanded effort, and the chain's response to that prescription is what is
asserted.

**Findings - composition problems the whole-run scenarios exposed**

1. **The stop-verification packet cannot be derived from one estimator answer.**
   `VelocityEstimator` reports either a window-derived number with
   `zeroSpeedClaimed == false` or the explicit zero-speed claim with
   `valid == false`, and never both (both behaviours are pinned by
   `test_velocity_estimator`). `NearZeroVerifier` needs a fresh *valid* estimate,
   an in-threshold speed and the claim at the same time. The harness therefore
   composes the packet from two production answers - the current cycle's claim
   and the last window-derived estimate - and that composition only satisfies the
   verifier when the estimate is aged with a limit at least as long as the stop
   dwell. Pinned by
   `test_the_stop_evidence_packet_is_composed_from_two_production_answers`: with
   the control path's own limit the package never verifies. The zero the packet
   rests on is always produced *before* the claim interval it is paired with,
   which is a policy statement the vehicle layer must make explicitly.
2. **A travel request can arrive while the profile is still ramping the previous
   direction down.** `MotionProfile` ramps a commanded reversal to zero before it
   flips the sign (`MotionProfileStatus::DirectionChangeHold`), so the output that
   opens the next leg can still be on the old side. The regulator then computes a
   powered effort in the *old* direction for the new leg's request (feed-forward
   plus an errored proportional term - an overspeed in the new direction), and
   the authority has nothing to refuse it with: its direction latch was
   legitimately cleared by the verified stop that earned the reversal. The first
   version of this harness presented that effort and the scenarios caught it. The
   harness now presents a travel request as a powered request only while the
   shaped target points along the commanded leg, and presents its own unpowered
   output otherwise. Pinned by `expectEveryDriveFollowsTheRunLeg`, asserted over
   every scenario's whole trace.
3. **`requiredDataFresh` is the position verdict, not the speed verdict.** A
   stationary vehicle's estimate is the explicit zero-speed claim, whose `valid`
   is false by design, so `velocityReadiness().usable` is false at every
   standstill. Section 12 maps `odometryReadiness()` onto `requiredDataFresh` and
   section 13 maps `velocityReadiness()` onto the regulator's `measurementUsable`;
   had the speed verdict gated `requiredDataFresh` instead, no run could start
   moving again from a verified stop. The harness wires it that way, and the
   nominal scenario exercises exactly that restart.

**Obligations recorded**

`SOFTWARE_ARCHITECTURE.md` now records findings 1-3 where they belong: the
stop-evidence pairing in section 13's zero-speed bullet, the travel-presentation
gate as a new section 13 obligation, and the `requiredDataFresh` clarification in
section 12. All three are vehicle-layer obligations, which the modules themselves
cannot enforce, and the harness implements them.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **310 test cases, 310 succeeded** (was 292).
  `test_scenario` contributes 18; every other target is unchanged. No compiler
  warnings were produced by the new target.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash 13.0%
  (**34152 / 262144 bytes**). Unchanged from D10, as expected: no `lib/` or
  `src/` file changed, so the firmware build is the same one.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **eighteen** `lib/evcore/src/*.h` headers: **0** unexpected
  diagnostics (only the expected `#pragma once in main file` artifact, one per
  header).
- `git status`: `test/test_scenario/` is the only new path; this entry and the
  section 12/13 notes are the only edits to tracked files, and no `lib/`, `src/`
  or `platformio.ini` change is present.
- The scenarios assert the architecture properties the stage asked for: one
  request per cycle, every drive carrying the authorization preconditions of its
  own cycle, every drive in the leg the run names, no powered direction change
  without a stop presented at a reported verified zero, `Fault` cycles carrying
  only the fault policy's `Disabled`, `Done` never asking for travel, no
  announcement starting a run on an invalid configuration, and byte-identical
  traces on replay of the same scripted evidence.

**Not verified**

Nothing about the vehicle. Every scenario is driven by prescribed encoder
readings and stated evidence, so a passing scenario is software verification
only: it is not evidence that the vehicle moves, counts, stops, tracks straight,
reverses, pushes a bottle or reaches any phase physically. No hardware fact is
claimed and no `VERIFIED_STATE.md` row was added or modified - in particular
polarity (#4), signal quality (#5), rollout (#6), gains (#8), braking (#9),
wheel slip (#10) and the bottle mechanism (#11) all stay UNVERIFIED.

**Pending physical test**

Unchanged from D10. The first physical test is still the low-power, unloaded
bring-up with conservative effort, one behavior at a time, and the first
measurement that unblocks any of this remains encoder direction sign (#4).

**Next stage**

The Arduino runtime/scheduler integration that will own these obligations for
real: the pin/ISR sampling layer, the vehicle-layer wiring the harness now
implements in test form, and telemetry. It is deliberately not started here.

## D12 - 2026-09-26 - P6I audit fixes - Vehicle-layer rules promoted to production, and the evidence the scenarios were missing

**Entry gate**

P6 and the P6I audit were reviewed together. The audit accepted the harness's
scope but found that it was the *only* owner of several vehicle-layer decisions,
that the fault and clock paths were injected but not exercised in every reachable
state, that two asserted properties were circular, and that a few gates could not
be observed at all. This stage fixes those findings: it promotes the decisions to
`lib/evcore`, adds the missing scenarios, and leaves no rule living only in test
support. It changes `lib/`, `test/` and the two architecture documents and no
`src/` file: the firmware's only entry point is still the sketch, neither a
runtime, a scheduler, a pin/ISR layer nor telemetry exists yet, and nothing was
committed.

**Why**

A rule that exists only in test support is not a firmware contract. When the
vehicle layer is finally written, every such rule would have to be re-derived
from prose, and the harness's copy would silently drift from whatever that layer
implemented - the drift being exactly the failure the rule was written down to
prevent. Whole-run scenarios also cannot see a gate they never take, so a gate
that is correct but unreachable in the nominal trace must be driven by a stated
hostile or held request, or it is untested code.

**What was added - production ownership (`lib/evcore`)**

- `lib/evcore/src/RunAxis.h` (new) - `ev::RunAxisGeometry` (the announced
  geometry, built from a valid `RunConfig` through one mapping),
  `ev::runAxisPositionM()` (the production tick-to-position conversion, with the
  measured tick-sign polarity applied exactly once, to travel, and its own
  `RunAxisStatus` refusal set), and `ev::remainingToTargetM()` /
  `ev::legRemainingM()` (the signed and clamped remaining distances). The state
  machine deliberately stores no geometry, so this is the run layer's single
  owner of both the competition axis and the vehicle's place on it. A polarity
  that is not `Measured` refuses control while still reporting a diagnostic
  number, exactly like a `Provisional` scale in `Odometry.h`.
- `lib/evcore/src/NearZeroVerifier.h` - `ev::composeStopEvidence()` composes the
  stop-verification packet from the two production estimator answers (the current
  cycle's claim under the control path's limit, the retained window-derived
  estimate under the packet's own longer limit), and
  `ev::stopEvidenceLimitSpansDwell()` is the configuration-build check for that
  limit. D11 recorded the composition as an obligation; it is now a function.
- `lib/evcore/src/MotionProfile.h` - `ev::targetVelocityPointsAlongLeg()` is the
  along-leg verdict the D11 scenarios discovered: a travel request may be
  presented as powered only once the shaped target points along the leg the run
  asked for.
- `lib/evcore/src/CompetitionStateMachine.h` - `ev::runCommandNamesForwardLeg()`
  (the one leg-tag mapping, including the stop tags) and `ev::runStopEffort()`
  (the one stop-vocabulary-to-request mapping, fail-closed for a command that is
  not a stop).
- `lib/evcore/src/SafetyLimits.h` - `ev::tickPolarityAgrees()` ties the measured
  tick sign to the command sign, making the polarity fact one agreement across
  all three of its encodings instead of two plus an assumption.

**What was added - test coverage**

- `test/test_run_axis/` (new target, 13 cases): the geometry mapping, the
  polarity applied once under both signs, an origin that is not zero, the two
  refusals (`TickTravelUnusable`, `PolarityIsNotMeasured`), the invariant
  `usable == (status == Usable)`, the signed/unclamped and clamped remaining
  distances, and the geometry's own arithmetic.
- `test/test_scenario/test_main.cpp` (18 -> 27 cases). The nine additions are the
  audit's missing evidence: (1) a *held* stale intention, presented cycle after
  cycle by the caller while the run keeps asking, carrying no powered request;
  (2) a hostile reverse request injected past the vehicle layer's gates, refused
  by the authority as `DeniedUnsafeDirectionChange` with the forward latch and
  the run untouched; (3) eight incomplete configurations (provisional
  calibration, safety limit, regulator ceiling and profile limit; a
  stop-evidence limit shorter than the dwell; disagreeing ceilings; a command
  polarity opposite the effort polarity; a tick polarity opposite the command
  polarity) that each announce a valid `RunConfig` and still cannot arm, with
  every piece of run evidence for sixty cycles producing no drive and no phase
  beyond `ParameterValid`; (4) a disarm in each of the four phases around the
  direction change (`ForwardDecel`, `BrakeZeroSpeed`, `Reverse`, `FinalBrake`),
  each latching `DisarmedDuringRun` and continuing to stop; (5) a stalled clock,
  which yields no speed evidence (`TimeDidNotAdvance`), consumes no phase time
  and never grows the phase's elapsed measurement, while the chain keeps
  travelling on position evidence; (6) an uneven clock, which finishes the whole
  run with strictly increasing stamps; (7) a rewound clock, which stops the run
  and latches, followed by the chain boundary and a second run to `Done` (the
  fault-reset-second-run path); (8) raw channel levels fed straight to the
  production decoder - duplicate, B-only and simultaneous double-channel samples,
  the two-count Gray cycle and an oscillating pair whose counted edges differ
  from its signed sum; and (9) an independent oracle over the nominal trace that
  recomputes every position from the raw tick count, the run origin and the
  fixture scale instead of reading the harness's own field, checks both legs for
  monotone travel and the reverse leg for a monotonically closing distance, and
  repeats the position conversion under the opposite measured tick polarity.
- Existing targets extended rather than duplicated: `test_near_zero_verifier`
  (16 -> 22, the composition and its dwell-limit check), `test_motion_profile`
  (26 -> 28, the along-leg gate), `test_motion_chain` (14 -> 15, the three-way
  polarity agreement), `test_competition_state` (38 -> 39, the leg-tag mapping).

**Findings and decisions worth recording**

1. **The along-leg gate is unreachable in the nominal trace, and that is worth
   stating.** The reversal path ramps the target to exactly zero before the leg
   flips, and a zero target is along any leg, so a nominal run never needs the
   gate to refuse. It is therefore pinned twice: as a whole-trace property of
   every scenario (`expectEveryDriveFollowsTheRunLeg`) and as a unit contract on
   the refusal case (`DirectionChangeHold` in `test_motion_profile`). A gate
   whose only evidence was "the run finished" would be untested code.
2. **The stale-intention gate can only be observed by a caller that holds an
   intention.** The run stamps its intention with the cycle that produced it and
   never re-presents one, so no scenario built from the run's own requests can
   reach the gate. The harness therefore exposes a held-intention hook, which
   presents a *previous* cycle's intention through the same production gate; the
   run's own record stays truthful throughout, so the trace still shows what the
   run asked for and what the caller presented side by side.
3. **A rewound clock reads as an overrun.** The phase timer is an unsigned
   elapsed measurement, so a stamp that moves backwards makes the measured
   elapsed time larger than any bound and the run latches `PhaseTimeout`. That is
   deliberate: a reading the run's own elapsed measurement cannot support is not
   evidence it may spend, and stopping is the safe answer. It is recorded here
   because the fault identity is a consequence of the timer arithmetic rather
   than a dedicated clock fault, so a later reader does not mistake it for an
   accident. A stalled clock is the opposite case and is handled without a
   fault: elapsed time does not grow, so no bound can expire.
4. **The distance a cycle consumes belongs to the previous cycle's leg.** The
   run's command for a cycle does not exist yet at the moment the vehicle layer
   must state the remaining distance, so the leg used is the one the run named in
   the *previous* cycle. At a leg change the cycle that changes legs therefore
   still carries the previous leg's distance - it is the cycle the run consumed
   while still in its previous phase - and the reverse-leg monotonicity oracle
   starts from the next cycle. This is a wiring order the vehicle layer must
   keep, not a harness artifact: a distance computed from a leg the run has not
   named yet would be the wrong number.
5. **A valid announcement is not a usable chain.** The arm-refusal matrix pins
   that separately: all eight variants announce a valid `RunConfig` and are still
   refused before arming, because the configuration-build checks are about the
   chain's own measured facts (calibration, limits, ceilings, polarity, the three
   polarity encodings and the stop-evidence limit), not about the event's
   parameters.
6. **Fault identity and fault response stay separate, now across the whole
   matrix.** The expanded fault case injects all eight observable identities in
   all thirteen phases (104 combinations) and asserts the same response for every
   one: the identity of record is latched and reported unchanged, the boundary
   carries `Disabled`, zero effort and no direction, the authority's own latched
   fault carries the same identity, and five further cycles of every run evidence
   move nothing.

**Evidence (software verification only; no physical test is applicable)**

- `pio test -e native` - **342 test cases, 342 succeeded** (was 310). The new
  `test_run_axis` target contributes 13; `test_scenario` 27 (was 18);
  `test_near_zero_verifier` 22 (was 16); `test_motion_profile` 28 (was 26);
  `test_motion_chain` 15 (was 14); `test_competition_state` 39 (was 38).
- A clean native rebuild (`-Wall -Wextra`) produced **no** compiler warnings or
  errors.
- `pio run` - nano_r4 **SUCCESS**, RAM 7.8% (**2564 / 32768 bytes**), Flash 13.0%
  (**34152 / 262144 bytes**). Unchanged from D11: the promoted functions are
  inline in `lib/evcore` headers, and nothing in the firmware's current entry
  point calls them, so the linked image is the same one.
- `git status`: `lib/evcore/src/RunAxis.h` and `test/test_run_axis/` are new
  paths; `test/test_scenario/` is still uncommitted from D11; the four existing
  `lib/evcore` headers and five existing test targets carry the additions. No
  `src/` or `platformio.ini` change is present.
- The scenarios now assert, in addition to the D11 properties: no powered request
  from a held intention, no powered request from a stale one, an independent
  authority refusal of a direct flip, a refusal to arm on each incomplete
  configuration, a latched stop in every reversal phase, no speed evidence from a
  stalled or rewound clock, a completed run on an uneven one, the production
  decoder's own verdict on duplicate, B-only, diagonal and oscillating samples,
  and a position oracle that does not read the field it checks.

**Not verified**

Nothing about the vehicle. Every scenario is still driven by prescribed encoder
readings and stated evidence, and the rewound-clock and hostile-request cases are
statements about the chain's logic, not about any physical timing or electrical
behavior. No hardware fact is claimed and no `VERIFIED_STATE.md` row was added or
modified - polarity (#4), signal quality (#5), rollout (#6), gains (#8), braking
(#9), wheel slip (#10) and the bottle mechanism (#11) all stay UNVERIFIED.

**Pending physical test**

Unchanged from D10 and D11. The first physical test is still the low-power,
unloaded bring-up with conservative effort, one behavior at a time, and the first
measurement that unblocks any of this remains encoder direction sign (#4).

**Next stage**

The Arduino runtime/scheduler integration, and in particular the vehicle-layer
wiring these rules now belong to: the harness's ordering (sample, readiness,
position, run update, effort, boundary) is the order that layer must implement,
and the promoted functions are the ones it should call rather than re-derive.

## D13 - 2026-09-26 - P7A - Cooperative runtime shell and the Nano R4 hardware boundary

**Entry gate**

P6 and the P6I audit (D11, D12) were accepted, so the pure core's decision
pipeline was complete enough to be orchestrated on the real target. This stage
adds the production runtime shell and the thinnest hardware boundary for the Nano
R4, so the complete firmware builds and is ready for physical bring-up. No
hardware is available: no upload was attempted, no electrical behavior was
observed, and nothing in this entry is a measurement. `src/` is compiled for the
first time in this project's history. Nothing was committed.

**Why**

Everything before this stage was pure logic the firmware's own entry point did
not call: `main.cpp` blinked an LED and printed a banner, and the chain was
exercised only by host tests. The obligations D10 recorded as build-time
acceptance criteria - exactly one `authorize()` per cycle, the intention
freshness gate, the leg-tag and stop mapping, the position verdict gating
`requiredDataFresh`, the configuration-build checks before arming - had no
production owner. Until a vehicle layer owned them, "the firmware" and "the
tested logic" were two different things.

**What was added - pure logic (`lib/evcore`)**

- `lib/evcore/src/ControlScheduler.h` (new) - the deadline decision for one pass:
  due when the reading reaches the next deadline (signed difference of two
  `uint32` readings, so the ~49.7 day wrap is handled), with the measured `dtMs`,
  lateness, dropped deadlines, remaining wait and a rewound reading reported. No
  clock, no hardware, no waiting.
- `lib/evcore/src/ControlTickOrder.h` (new) - the frozen stage vocabulary
  (`EncoderSnapshot` ... `TelemetryCapture`) and the guard that reports a tick
  which skipped, repeated, abandoned or failed to complete a stage.
- `lib/evcore/src/MotorDriverPlan.h` (new) - the planned DRV8874 PH/EN input
  levels per `MotorMode`, the permille-to-PWM-count arithmetic and the
  de-energized predicate. Pure, host-tested, and the only command-to-levels
  mapping in the project.

**What was added - vehicle layer (`src/`)**

- `src/PinMap.h` - the centralized provisional pin set, with why each pin was
  chosen (framework pin-table capability) kept apart from the wiring fact that
  remains UNVERIFIED.
- `src/MotorDriver.h` - the only code that writes PH/EN/nSLEEP. It applies one
  authorized command through the pure plan, and it has its own de-energized path
  (`beginDisabled()`, `failSafe()`) that does not need a command.
- `src/EncoderInput.h/.cpp` - the one interrupt: two digital reads and one pure
  decoder update, plus a coherent tick snapshot taken with PRIMASK saved and
  restored.
- `src/SensorInputs.h` - raw battery/current/nFAULT/trigger acquisition; no
  conversion and no interpretation.
- `src/WatchdogCheckpoint.h` - the single refresh path, gated on a completed pass,
  behind one centralized enablement constant that is **false**.
- `src/VehicleConfig.h` - cadence and staleness policy, `Provisional` placeholders
  and the rule-derived Complete Stop dwell, kept apart, plus the
  `ConfigurationBuildChecks` report every arming decision will read.
- `src/VehicleRuntime.h/.cpp` - the control tick in the frozen stage order, the
  one `authorize()` per cycle, the leg-tag and intention gates, the stop mapping,
  the driver fail-safe on an untrustworthy tick, and the telemetry snapshot and
  hook.
- `src/main.cpp` - the cooperative shell: pass, watchdog checkpoint, then
  low-priority work (status LED), with the boot banner describing the safe state.

**Decisions**

1. **A planned driver mapping is written down, and it supersedes nothing about
   the hardware.** `SOFTWARE_ARCHITECTURE.md` section 10 listed the driver
   interface as deliberately undecided. Writing a *plan* is what made the boundary
   implementable; the electrical behavior, the truth table, the polarity and the
   wiring all stay UNVERIFIED, and the plan says so in the code, in
   `docs/hardware/motor-driver-drv8874.md`, and in `VERIFIED_STATE.md`. The PH
   level -> direction sense is recorded there as the fourth encoding of the one
   polarity fact, measurable once.
2. **A missed deadline is dropped, counted and reported - never caught up.**
   Running a burst of back-to-back ticks after an overrun would sample several
   times with almost no interval between samples and push the following work
   later still. The schedule re-arms from the tick that actually ran, so the
   average cadence is the period plus the loop's real cost, and that cost is
   visible as lateness instead of hidden inside a burst.
3. **The watchdog gate is a completed pass with an in-order tick.** Withheld
   refreshes are counted, so the mechanism is observable before it is enabled.
   Refresh lives in exactly one function in one file and is called from exactly
   one place (the loop, after the pass returns). Production enablement is a
   centralized constant and it is off: no watchdog behavior has been observed on
   this target, and linking the adapter is not validation.
4. **A raw nFAULT level is not invented into a fault.** The pin, its polarity and
   the driver's fault behavior are all UNVERIFIED, so the level is sampled,
   counted and reported, and no `FaultCode` is produced. The reason this is safe
   today is that no drive can be authorized at all: calibration, limits and gains
   are provisional. Mapping that input onto a fault identity and a stop policy is
   recorded as pending bench work, and it is the one place in this stage where a
   deliberate omission is the answer rather than an implementation.
5. **The run's final stop dwell is the rule-derived Complete Stop** (rule 8.p,
   `RuleDerived`), while the reversal gate's own dwell is a separate, much shorter
   provisional policy value - the two are different asks, and the stop-evidence
   limit must span the longer one, which `ConfigurationBuildChecks` reports.
6. **Arming, announcement and the run boundary are deliberately not built.** They
   are deliberate transitions owned by the command layer, and building them now
   would mean inventing where the announced parameters come from. Until then the
   runtime computes the readiness verdicts, runs the tick, and stays in `Idle`
   with every drive refused.
7. **The encoder snapshot is a coherence measure, not a tearing fix.** An aligned
   `int32` load is already atomic on this core; the saved-PRIMASK section exists
   so the count and the decoder's own counters are read as one instant, and it is
   short enough that nothing else is delayed. The ISR itself does two digital
   reads and one pure decode.
8. **The telemetry snapshot is not a schema.** It is one RAM record plus one
   optional non-blocking hook, with no transport, and the boot banner is not a
   telemetry stream. The schema stays undecided (section 10) and P7B owns it.

**Evidence (software verification only; no physical test is applicable)**

- `pio run` (clean rebuild, `nano_r4`) - SUCCESS, RAM **12.4%** (4064 / 32768
  bytes), Flash **19.0%** (49772 / 262144 bytes). No compiler warnings. The
  watchdog library is found and linked: `arm-none-eabi-nm` lists `WDTimer` /
  `R_WDT_Open` / `R_WDT_Refresh` in `firmware.elf`. Linking is not watchdog
  validation.
- `pio test -e native` - **372 test cases, 372 succeeded** (was 342).
  `test_control_scheduler` contributes 11 (deadline and due logic, measured
  variable dt, the drop-instead-of-catch-up rule, the stalled and rewound clocks,
  wraparound on both sides, re-arming); `test_control_tick_order` 12 (the pinned
  stage order and every way a tick can fail it); `test_motor_driver_plan` 7 (the
  planned levels per command, the duty following the authorized magnitude, every
  refusal leaving the driver de-energized, the PWM mapping). No compiler
  warnings.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **22** `lib/evcore/src/*.h` headers: **0 unexpected diagnostics** (only
  the expected `#pragma once in main file` artifact, one per header).
- Static searches over `src/`: the three planned motor pin names appear only in
  `src/PinMap.h` (definitions) and `src/MotorDriver.h` (the writes); `WDT` appears
  only in `src/WatchdogCheckpoint.h`; there is no `delay()`, no unbounded loop and
  no per-cycle serial output; `Serial` appears only in the boot banner in
  `src/main.cpp`. The interrupt handler body is two `digitalRead`s and one
  `decoder.update()`. No test target includes a `src/` header, so the `native`
  environment still never compiles the Arduino tree.
- `git status`: `docs/hardware/`, the three new `lib/evcore` headers, all new
  `src/` files and the three new test targets are new paths; `src/main.cpp` and
  the four authority documents are modified. No `platformio.ini` change.

**Not verified**

Everything about the vehicle. The firmware's startup state, the de-energized
driver levels, the encoder interrupt, the raw input readings, the watchdog and
every timing number are code paths that have not been run on hardware. Rows #1
(wiring), #2 (driver truth table), #3 (motor polarity), #4 (encoder sign), #5
(signal quality), #6 (rollout), #7 (sensor scaling), #9 (braking), #11 (bottle
mechanism) stay UNVERIFIED, and a build, a passing host test, a static search and
a linked library are none of them evidence about the vehicle.

**Pending physical tests**

In order, each following the safety rules in `AGENTS.md` section 5 (drivetrain
unloaded, conservative effort, one behavior at a time):

1. **Wiring continuity (#1)** against `src/PinMap.h`, recorded before anything is
   powered.
2. **Driver idle levels and truth table (#2)** with the motor supply current
   limited: the de-energized levels at rest, whether EN low with the driver awake
   brakes or coasts, both PH levels, the duty at 10/50/100%, and what nFAULT does
   when tripped. Procedure and rows to record:
   `docs/hardware/motor-driver-drv8874.md`.
3. **Direction and encoder sign (#3, #4)**: which PH level turns the shaft in the
   vehicle's forward direction, and which tick sign that produces. One measurement
   sets `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode` and
   `kPlannedPositiveTicksDirection` together, after which the polarity agreements
   are expected to read true.
4. **Encoder counting (#5)**: hand-rotate the wheel; confirm the interrupt counts
   at all (a pin with no interrupt channel attaches nothing, which is why the pin
   choice matters), then check duplicate and noise behavior over repeated
   revolutions.
5. **Rollout (#6)** and **ADC scaling (#7)**: measured travel per tick, and raw
   counts against a reference meter. Both replace placeholders and nothing else.
6. **Trigger active level** and its debounce, before the trigger is allowed to
   start anything.
7. **H0W, the watchdog check**: with the enablement constant flipped in a bench
   build only, confirm that the adapter starts the watchdog, that a stalled pass
   leads to a reset, and that a reset returns the pins to the de-energized state.
   Record it before production enablement is flipped.
8. **Loop cadence**: the measured control-tick interval, lateness and dropped
   deadlines, which is also the evidence needed to choose the watchdog timeout.

**Next stage**

P7B: the command and telemetry transport (serial), the run announcement with the
announced `RunConfig`, and the deliberate arming path that reads
`ConfigurationBuildChecks` - plus the fault identity for a measured nFAULT input
once test 2 exists.

## D14 - 2026-09-27 - P7B - Operator interface, versioned telemetry, and the non-blocking transport

**Entry gate**

P7A and its build-time obligations were accepted (D13), so the control chain had
one production owner. This stage adds the two things an operator and a
post-mortem can use: a bounded serial command line and a machine-readable
telemetry stream. Still no hardware: no upload, no port connected, and nothing in
this entry is a measurement. Nothing was committed.

**Why**

D13 left arming, the run announcement and telemetry unbuilt on purpose, because
each needs a decision about where its inputs come from. Without them the firmware
could run its tick and refuse every drive, but a team member standing next to the
vehicle had no way to ask what it believed, no way to hand it the event's
parameters, and no way to read a run afterwards. The three questions this stage
answers are: what did the chain believe, what parameters was it given, and what
can an operator ask it to do without ever being able to make it move.

**What was added - pure logic (`lib/evcore`)**

- `lib/evcore/src/BoundedText.h` (new) - the shared bounded text writer every
  formatter is built on: capacity-checked `appendChar`/`appendText`/`appendUnsigned`/
  `appendBool`/`appendFixed`, a `field(key)` helper for `key=value`,
  fixed-point double formatting with no `printf` and no allocation, explicit
  `nan`/`inf`/`ovf` sentinels, and a truncation flag. It also owns the line
  vocabulary: `kBoundedLineDidNotFit`, `finishBoundedLine()`, and
  `kSerialLineCapacity`/`kSerialLineBufferBytes`.
- `lib/evcore/src/ConfigurationChecks.h` (new) - the configuration-build check
  vocabulary (`ConfigurationCheckId`, 14 checks + `Count`, `configurationCheckName()`)
  and the report type `ConfigurationBuildChecks`. It moved out of
  `src/VehicleConfig.h` so the operator policy can be host-tested against it.
- `lib/evcore/src/SerialCommandParser.h` (new) - the bounded line reader
  (`CommandLineReader`), the command vocabulary (17 `OperatorCommandId` values),
  the `config set` field set (`RunConfigFields`), the named `CommandError` set, the
tournament-level spelling, and the single entry point `parseOperatorCommand()`.
- `lib/evcore/src/OperatorPolicy.h` (new) - the only place a command is admitted or
  refused: `admissibilityOf()` over `OperatorSystemState`, the named
  `CommandRefusal`s, the announcement and arming verdicts
  (`announceVerdictFor()`, `armVerdictFor()`, `heldRunConfigVerdict()`), and the
  telemetry-rate to stride mapping
  (`telemetryStrideForRate()`).
- `lib/evcore/src/TelemetryRecord.h` (new) - the schema owner: `kTelemetrySchemaVersion`,
  the `EVS`/`EVE` prefixes, the field set and enumerator spellings of one sample
  `TelemetrySample` and one `TelemetryEvent`, the missing-field marker, the drop
  counters, the event batch, the baseline-primed `TelemetryEventDetector`, and the
  `TelemetryRecord`/`TelemetryRecordKind` union the queue carries.
- `lib/evcore/src/TelemetryQueue.h` (new) - two fixed lanes (8 samples, 16 events),
  drop-newest with the count stamped into the records that do go out, events
  drained first, and `TelemetrySession` holding enable/disable and the due-stride.
- `lib/evcore/src/TelemetryFormat.h` (new) - the one place a record becomes text
  (`EVS<v>,...`, `EVE<v>,...`), with the worst-case sizes statically checked
  against the line capacity (418 / 180 characters).
- `lib/evcore/src/OperatorResponse.h` (new) - every response spelling: the prefix
  vocabulary (`OK`, `ERR`, `STATUS`, `CONFIG`, `CHECK`, `TELEMETRY`, `VERSION`),
  the ack/refusal/arm-refusal/announce-refusal text, the configuration echo, the
  checks line, the status line, the telemetry state and rate acknowledgements, the
  version line, and the help table (15 lines). Its worst case is 384 characters,
  statically checked against the line capacity like the telemetry records.

**What was added - vehicle layer (`src/`)**

- `src/SerialTransport.h` (new) - the non-blocking line transport: one staging
  buffer, `stage()` for a line (an operator response displaces a staged telemetry
  line and counts it as abandoned), `pump()` writing only what
  `availableForWrite()` reports room for, and counters for lines sent, staged,
  abandoned and refused.
- `src/VehicleRuntime.h/.cpp` - `serviceSerial(nowMs)` in the loop's low-priority
  section: move staged bytes out, read at most `kReadBytesPerPass`, apply and
  answer at most one command, then drain at most four telemetry records; the held
  configuration, the operator effects routed through the chain's own entry points,
  the boot line, the tick's telemetry capture, and the operator/telemetry
  counters.
- `src/VehicleConfig.h` - `kFirmwareId` and `using ConfigurationBuildChecks =
  ev::ConfigurationBuildChecks` (one owner of the check report, no second
  definition).
- `src/main.cpp` - the boot banner is gone; startup is LED, runtime, watchdog,
  transport with a bounded 2000 ms host wait, and the loop is pass, watchdog
  checkpoint, `serviceSerial`, status LED.
- `test/test_serial_independence/test_main.cpp` (new) - the acceptance-gate
  scenario: the same scripted run twice, once with the consumer hierarchy fully
  driven by a stalled host and a stream of rejected and refused commands, with the
  control traces compared tick by tick.

**Decisions**

1. **The operator interface is three modules, not one.** Parsing, admissibility
   and formatting have three separate owners, and none of them holds run-control
   state. A single "CLI module" would have put the arming refusal next to a string
   formatter, and the format of a reply would have been the place a safety rule
   lived. Splitting them also made every one of them host-testable, which is what
   lets the refusal paths be enumerated in tests before any operator ever types
   them.
2. **The transport had to be built around a blocking `write()`.** Reading the
   installed Renesas core (`usb/SerialUSB.cpp`) shows `_SerialUSB::write(buf,size)`
   looping until the host drains the buffer, with the USB interrupt disabled while
   it waits. A bare `Serial.print()` of one record would therefore hand the control
   loop's schedule to whoever is - or is not - reading the port, which is exactly
   the failure mode `AGENTS.md` section 13 forbids. Every write is gated on
   `availableForWrite()`, the endpoint window is 256 bytes
   (`variants/NANOR4/tusb_config.h`), and a line that does not fit is kept for the
   next pass rather than split. Both facts are recorded in `VERIFIED_STATE.md`.
3. **Telemetry is SI, one line per record, and all-or-nothing.** Values are metres
   and metres per second (section 6), a record that does not fit is dropped and
   counted rather than truncated, and the drop counters ride in the records that do
   go out. Truncation would eat exactly the tail fields a post-mortem needs - the
   fault, the authorization outcome, the failing check - which is the opposite of
   what a log is for. Schema version 1, the `EVS`/`EVE` prefixes and every
   enumerator spelling are frozen in `TelemetryRecord.h`; adding or renaming one is
   a schema change, not a refactor (section 10).
4. **A run that is underway is closed to the operator, except for three things.**
   Inspection commands, `disarm` and the telemetry commands stay admissible; every
   other command is refused as `RunUnderway`. Putting the restriction in one
   function is what makes "serial input cannot alter an in-progress run" checkable
   at a single place, and `test_serial_independence` checks it from outside by
   driving the whole consumer chain during a run and comparing the resulting
   control trace with the run that had no operator at all.
5. **`reset` is admitted before the fault gate, and the gate is what refuses
   everything else.** Implementing the refusal paths made the ordering visible: a
   latched fault must be clearable, and `reset` is the only command that clears it,
   so checking the fault first left no way out of a fault. It stays refused while a
   run's phases are underway, because discarding a fault is not abandoning a moving
   run - that is `disarm`'s job, and it is the operator's decision.
6. **Arming reads the configuration-build report before it asks the authority.**
   `armVerdictFor()` runs first and names the first failing check, which is how the
   placeholder configuration in `VehicleConfig.h` produces an *attributable*
   refusal instead of an armed state with unverified values behind it. The check
   report itself moved into `lib/` so the policy that reads it is host-testable;
   `src/VehicleConfig.h` still owns which values fill it.
7. **The Start Point is the arming decision.** A successful `arm` takes the
   encoder tick reading as the run origin, because the event's Start Point is
   defined by a deliberate decision and not by whenever the vehicle happened to
   stop moving. Nothing before arming writes an origin, and a reset re-establishes
   the decoder reference instead of remembering one.
8. **The operator's answer outranks the log.** A staged response displaces a
   staged telemetry line, and `serviceSerial()` answers before it drains samples.
   A person waiting for an answer must not queue behind a log stream they cannot
   see, and the displaced line is counted rather than silently lost.
9. **The boot line is staged text, and the boot event is a record.** Startup does
   not need a host and does not print a banner: one `BOOT,...` line is staged for
   whenever a host appears, and the `Boot` event goes through the same queue and
   schema as every other transition. The 2000 ms host wait in `setup()` is a
   bounded initialization wait (section 4), not a control-loop wait, and nothing
   downstream depends on whether it expired.
10. **A refusal with nothing to refuse is its own outcome.** `CommandRefusal::NothingToApply`
    was added so a caller bug - presenting a request that was never parsed - cannot
    be read as a policy refusal of a real command. The refusal set is diagnostic
    vocabulary, and collapsing two different causes into one name would hide the
    bug it is meant to expose.
11. **A `Provisional` configuration is not a reason to invent a diagnostic motion
    mode.** `AGENTS.md` section 17 lists `motor <value>` and `test run <profile>` as
    examples. Neither is built: with every gain, scale and limit provisional, a
    command that could drive the motor would be a second, unaudited path around
    `SafetyAuthority`, and the chain would refuse it anyway. A diagnostic motion
    mode with control authority is a new contract, not a convenience.

**Evidence (software verification only; no physical test is applicable)**

- `pio run` (clean rebuild, `nano_r4`) - SUCCESS, RAM **17.8%** (5848 / 32768
  bytes), Flash **24.8%** (65028 / 262144 bytes). No compiler warnings.
- Host suites, each run individually (`pio test -e native -f <suite>`) - **24
  suites, 456 test cases, 456 succeeded**. New this stage: `test_serial_command_parser`
  20 (bounds, token limits, every error name, the field set, number spelling),
  `test_operator_policy` 19 (every refusal, the reset-before-fault ordering, the
  arming and announcement verdicts, the stride mapping), `test_operator_response`
  16 (every response shape, the worst-case bound, the help table),
  `test_telemetry_format` 11 (both record shapes, the SI field order, the
  all-or-nothing behaviour, the worst-case bound), `test_telemetry_queue` 14 (both
  lanes, drop-newest with the counters stamped, events draining first, disable
  emptying the queue), and `test_serial_independence` 4 (the acceptance gate above).
  Existing 18 suites unchanged and passing: `test_competition_state` 39,
  `test_control_scheduler` 11, `test_control_tick_order` 12, `test_core_types` 15,
  `test_interval_timer` 6, `test_motion_chain` 15, `test_motion_profile` 28,
  `test_motor_driver_plan` 7, `test_near_zero_verifier` 22, `test_odometry` 31,
  `test_quadrature_decoder` 15, `test_reversal_sequencer` 16, `test_run_axis` 13,
  `test_run_config` 15, `test_safety_authority` 29, `test_scenario` 27,
  `test_velocity_controller` 35, `test_velocity_estimator` 36.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only -Ilib/evcore/src`
  over all **30** `lib/evcore/src/*.h` headers: **0 unexpected diagnostics** (only
  the expected `#pragma once in main file` artifact, one per header).
- Static searches over `src/`: there is no `delay(` in any code path (the only
  match is a comment); `Serial` appears only in `src/main.cpp` (the bounded boot
  wait) and `src/SerialTransport.h` (the gated writes); each planned pin name
  appears only in `src/PinMap.h` and its one adapter (`kEncoderA`/`kEncoderB` in
  `EncoderInput.cpp`, the three motor pins in `MotorDriver.h`, `kMotorFaultBar`,
  `kStartTrigger`, `kBatterySense`, `kCurrentSense` in `SensorInputs.h`). No
  `printf`/`sprintf`/allocation in `lib/`.
- `git status`: `SOFTWARE_ARCHITECTURE.md`, `DEVELOPMENT_LOG.md`, `README.md` and
  `VERIFIED_STATE.md` are modified; all of `lib/evcore/src`, `src/` and the new
  test targets remain untracked from D13 and D14. No `platformio.ini` change.

**Not verified**

Everything about the vehicle, and one thing about the host link. The firmware's
startup state, the driver levels, the encoder interrupt, the raw inputs, the
watchdog and every timing number are still code paths that have not run on
hardware (rows #1-#12 stay UNVERIFIED). Added this stage: no serial link has ever
been opened to this firmware. The blocking behaviour of `Serial.write()` is a
source-level fact about the installed core, not an observation; the endpoint
buffer size is a configuration fact, not a measurement; and the claim that a host
which stops reading cannot disturb the control cadence is proved by the logic test
and has not been demonstrated on the board.

**Pending physical tests**

After the D13 list, and each following the safety rules in `AGENTS.md` section 5.
This stage's additions need no motion at all, so they can be done first and with
the drivetrain unloaded:

1. **The port comes up at all.** Upload, open `pio device monitor`, confirm the
   staged `BOOT,...` line arrives once. If it does not, the boot wait and the
   transport's write gating are the first things to suspect.
2. **Inspection and refusal paths, remembered verbatim.** Send `version`, `status`,
   `checks`, `config show`, then `announce` and `arm`. Record the exact lines. With
   the placeholder configuration, `arm` must be refused and the refusal must name
   the first failing check; a refusal that names the wrong check is a configuration
   bug, not a labelling bug.
3. **`config set` and the rule validation.** Set a level, distance, time and bottle
   line, echo them, and confirm a malformed or out-of-range field is refused with a
   named error rather than silently clamped.
4. **Malformed input never reaches the chain.** Send junk, an over-long line, too
   many tokens, a huge number and a half-finished command. Confirm each produces a
   named error, that no `OK` is produced for a command that was not applied, and
   that nothing about the run state changed.
5. **Telemetry shape and rate.** Confirm `EVS1` sample lines and `EVE1` event
   lines appear, that `telemetry off` stops the samples and empties the queue, that
   `telemetry rate <hz>` changes the observed spacing to a whole number of ticks,
   and that the drop counters stay at zero when the host keeps up.
6. **The host stops reading.** Disconnect the monitor (or unplug the data line)
   mid-stream with telemetry on. The cadence must not change and the run must not
   alter: `status` should show the drop counters rising, which is the intended
   behaviour, and reconnecting must not produce a burst of stale lines.
7. **Cadence with the transport busy.** The measured control-tick interval,
   lateness and dropped deadlines with telemetry at its default rate, which feeds
   the same cadence evidence D13's test 8 asks for.

**Next stage**

Physical bring-up. Nothing in the current design is blocked on new software: the
next useful result is a measurement, starting with wiring continuity (#1), the
driver's idle levels and truth table (#2), and then the polarity measurement that
unblocks every gain, scale and limit in `src/VehicleConfig.h`. The nFAULT fault
identity and every motion command stay deliberately absent until those exist.

---

## D15 - 2026-09-27 - P8A - Telemetry analysis tooling: a schema mirror, a strict parser, and reproducible reports

**Entry gate**

The entry gate was that the P7B telemetry schema is stable enough to build
tooling on. It is: `lib/evcore/src/TelemetryRecord.h` freezes the prefixes, the
field set and every vocabulary spelling, `TelemetryFormat.h` freezes the field
order and the numeric rendering, and the host tests pin both against the tables.
Nothing in this entry changes any of that, and no firmware source was touched.

**Why**

There is no vehicle hardware yet, so there is no real log - and a parser and a
chart writer that have never read a record prove nothing. The gap this stage
closes is *interpretation latency*: when hardware arrives, the first capture has
to be readable immediately, in the same minute it is taken, without installing a
toolchain or writing a notebook. The tooling therefore had to exist, be validated
against something, and be unable to confuse what the firmware believed with what
the vehicle did.

**What was added (`tools/`)** - host-side Python only, no firmware change

- `tools/telemetry/analyze.py` (new) - the single entry point.
- `tools/telemetry/evtelemetry/schema.py` (new) - the mirror of the frozen
  schema: the version, both prefixes, the missing-field marker and the four
  fixed-point sentinels, the field set and order of both record kinds, and every
  enumerator's wire spelling.
- `tools/telemetry/evtelemetry/parse.py` (new) - one-line and whole-stream
  parsing, with the named failures `SchemaVersionError`, `FieldShapeError` and
  `FieldValueError`; per-lane `millis()` unwrapping using the firmware's own
  unsigned-difference rule; and an optional continue-on-error mode that records
  bad lines instead of hiding them.
- `tools/telemetry/evtelemetry/metrics.py` (new) - descriptive metrics: the
  transition timeline from both the event lane and the sample train with their
  disagreements, position, measured vs commanded velocity and the tracking error,
  effort overall and per motor mode, raw battery/current counts, the reported
  control `dt`, drops and their monotonicity, phase durations, braking/reversal
  *state* timings, the armed window, and believed endpoints.
- `tools/telemetry/evtelemetry/endpoints.py` (new) - the measured-vs-believed
  comparison from a small hand-measurement CSV, with a strict header.
- `tools/telemetry/evtelemetry/svgplot.py` (new) - a dependency-free,
  deterministic SVG line-chart writer with phase bands, reference lines and event
  markers.
- `tools/telemetry/evtelemetry/report.py` (new) - the report writer, the plot
  composition and the guard that refuses to write any path naming a project
  authority file.
- `tools/telemetry/evtelemetry/synthetic.py` (new) - the deterministic fixture
  generator, including a variant with a dropped burst across a phase boundary and
  a variant that faults mid-run.
- `tools/telemetry/evtelemetry/cli.py` (new) - `synth` and `analyze`.
- `tools/telemetry/samples/*.csv` (new, committed) - the three synthetic
  fixtures, each headed by `# SYNTHETIC` lines and a "never cite in
  VERIFIED_STATE.md" warning.
- `tools/telemetry/tests/` (new) - 91 host tests (see Evidence).
- `tools/telemetry/README.md` (new) and `README.md` (project layout, the `tools/`
  entry, and the P8A stage line). `.gitignore` gains `data/reports/`.

**Decisions**

1. **The schema is mirrored, and the mirror is tested against the firmware.**
   `schema.py` is a copy, and a hand-maintained copy is exactly the kind of thing
   that goes stale silently. `test_schema_mirror.py` re-reads
   `TelemetryRecord.h`, `TelemetryFormat.h`, `ConfigurationChecks.h` and
   `BoundedText.h`, extracts the field order from the formatter calls, extracts
   each vocabulary from its `switch`, and fails if the mirror disagrees. It skips
   - never silently passes - when the headers are absent, and an extraction that
   finds nothing is a failure, because "could not verify" must not read like
   "verified".
2. **A line that claims to be telemetry must parse, or the parse fails.**
   Anything beginning with the `EV` root is telemetry; a wrong version, a renamed
   field, a reordered field set, a value outside its firmware type, a
   non-schema number spelling or an unknown vocabulary entry is refused with the
   file, the line number, the offending line and the expected shape. Accepting a
   renamed column is how an analysis ends up describing a record that was never
   logged. Non-telemetry lines (operator responses, `BOOT,...`, comments) are
   counted as ignored, not rejected, because a capture is not required to be pure
   telemetry.
3. **Time is unwrapped per lane, with the firmware's own arithmetic.** `millis()`
   wraps, and `SOFTWARE_ARCHITECTURE.md` section 4 fixes unsigned subtraction as
   the rule for elapsed time, so a duration computed here cannot disagree with
   the firmware. The two lanes are unwrapped independently because the event lane
   drains first: an event for a later tick legitimately precedes a sample for an
   earlier one, and one shared counter would read that as a jump of nearly 2^32 ms.
4. **`ovf` is "no value", not infinity.** The formatter writes `ovf` for a finite
   value beyond its range, which is a bug or uninitialised data. Representing it
   as infinity would let it enter a mean as a huge measurement; it parses to
   `None` and stays out of every statistic.
5. **Descriptive metrics only, and the names say so.** `batt_counts` and
   `curr_counts` are reported as raw ADC counts with the words "no conversion
   documented" attached (`SOFTWARE_ARCHITECTURE.md` section 12 records that no
   raw-to-engineering-unit relationship exists in project sources, so inventing
   one here would turn a guess into something that looks like a measurement).
   Braking and reversal are reported as phase-boundary timings, each labelled "not
   stopping distances". "Believed" prefixes every firmware-derived endpoint. A
   log says what the chain believed; it is not evidence about the vehicle.
6. **Reproducible bytes, so a chart can be cited.** No timestamps, no host names,
   no random ids, sorted JSON keys, rounded numbers before printing, fixed
   element order, and a fixed `sha256` of each input in the manifest. Two runs
   over the same log produce identical files - which is what the acceptance gate
   asked for and what a diff between two sessions needs.
7. **Standard library only.** `AGENTS.md` section 25 requires a reason before a
   library is added, and here there was none: a plot writer of about 300 lines
   removed the only real dependency (matplotlib) and removed the question of
   whether a Windows machine without it could run the tool at all. SVG is text, so
   a reviewer can diff two charts.
8. **The synthetic fixtures are generated, committed, and labelled.** The parser
   and the plots are validated against `EVS1`/`EVE1` lines written by a mirror of
   the firmware formatter - not a convenient paraphrase - and a fixture-drift test
   asserts the committed files are byte-identical to what the generator produces.
   Each file opens with `# SYNTHETIC` and an explicit "never cite it in
   `VERIFIED_STATE.md`". The generator also produces the two conditions that are
   hard to get on purpose from real hardware: a dropped burst straddling a phase
   boundary (which is what makes the sample lane and the event lane disagree) and
   a fault latched mid-run.
9. **The tool refuses to write an authority file.** `report.py` checks every
   component of an output path against `VERIFIED_STATE.md`,
   `DEVELOPMENT_LOG.md`, `SOFTWARE_ARCHITECTURE.md` and `AGENTS.md`, and raises
   `ReportError` if one matches. A directory inside a report tree named after the
   evidence record is refused too - it is a trap for whoever reads the tree next.
   A test additionally asserts the repository's `VERIFIED_STATE.md` is unchanged
   after a full `synth` + `analyze` run.
10. **Defaults are anchored to the repository root, not the current directory.**
    A relative default would have quietly written a second
    `tools/telemetry/samples` wherever the tool was started from; this was found
    by running the command from the wrong directory, which is recorded here
    because it is exactly the kind of defect a test would not have caught.

**Evidence (software verification only; no physical test is applicable)**

- `pio run` (nano_r4) - **SUCCESS**, RAM **17.8%** (5848 / 32768 bytes), Flash
  **24.8%** (65028 / 262144 bytes). Unchanged from D14, which is the point: this
  stage added no firmware code, so the image must be identical, and it is.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -
  **91 test cases, 91 succeeded**: `test_schema_mirror` 9, `test_parse` 34,
  `test_metrics` 23, `test_cli` 25. The analyzer runs end to end on the three
  committed synthetic fixtures, from a plain `python` with nothing installed.
- Acceptance gate, item by item:
  1. *runs locally on synthetic samples* - `analyze` on
     `tools/telemetry/samples/*.csv` exits 0 and writes a per-run report
     (`metrics.json`, `summary.txt`, six SVGs) plus `comparison.json/.txt` and
     `manifest.json`. All 18 SVGs parse as XML; all JSON files load.
  2. *parsing fails clearly on incompatible or corrupt input* - a bumped schema
     version, a truncated field set, a renamed column, a non-numeric
     measurement and a missing file each exit 2 with a message naming the file,
     the line number, the offending line and what was expected, and no manifest
     is written. `--continue-on-error` instead records the bad line in
     `parse_errors.txt` and states in that file that the metrics describe a
     partial log.
  3. *metrics and plots are reproducible* - analysing the same log twice into two
     directories produces a byte-identical file tree, asserted by comparing every
     file. The synthetic generator is likewise byte-stable.
  4. *synthetic data is never written into `VERIFIED_STATE.md`* - the write guard
     is unit-tested for all four authority files, and `VERIFIED_STATE.md`'s
     `sha256` is asserted unchanged across a full `synth` + `analyze` run.
- Cross-checks worth recording: the parser is exercised against the literal
  `EVS1`/`EVE1` lines that the firmware's own `test_telemetry_format` pins, so it
  reads a record a board actually emits; and the synthetic fixtures are asserted
  byte-identical to the generator's output, so a fixture cannot drift from the
  code that claims to produce it.
- `git status`: `.gitignore` and `README.md` modified; `tools/` untracked. No
  `src/`, `lib/`, `include/`, `platformio.ini` or test-target change. No commit
  was made. `data/reports/` was added to `.gitignore` so a generated report never
  lands in the repository - the raw capture is the record, not the report.

**Not verified**

Everything about the vehicle, and one thing about the tool: it has never read a
byte that came off a board. Every hardware row in `VERIFIED_STATE.md` stays
UNVERIFIED, and **no row was added or modified** - in particular no row claims
that the tooling works, because a passing host test is not evidence about the
vehicle and this stage produced no measurement of any kind. The tooling's own
limits are part of the contract: it cannot detect a wheel that slips, a brake
that grabs, a driver that ignores an input or an encoder that counts wrongly. It
can only say what the firmware believed and commanded.

**Hard stop honored**

No physical conclusion is drawn anywhere in this stage, and none is available to
draw: the fixtures are generated, their positions are arithmetic over a velocity
model, and their phase timings are a fiction written by the generator. The
numbers in a synthetic report describe the generator, not a vehicle. The tool
exists so that a *real* capture can be read the day hardware arrives; it does not
substitute for that capture.

**Pending physical work**

Unchanged from D14 and D13, and now the first thing a real capture feeds: wiring
continuity (#1), the driver's idle levels and truth table (#2), then the polarity
measurement (#3, #4). Once a session has been captured, the immediate use of this
tooling is to read `control_dt`, the drop counters and the phase timeline from
that capture, and to record the measured endpoints by hand alongside it - while
remembering that the comparison it prints is arithmetic, not a conclusion.

---

## D16 - 2026-09-27 - P8B - Exact physical bring-up and experiment protocols

**Entry gate**

The P7 runtime and hardware boundary (D13), the operator interface and telemetry
(D14) and the P8A analysis tooling (D15) are available. No vehicle hardware is
available, and none was used. Nothing was committed.

**Why**

The pending physical tests recorded in D13, D14 and `VERIFIED_STATE.md` were a
list of *topics*: "wiring continuity (#1), the driver's truth table (#2), the
polarity measurement (#3, #4)". A list of topics is not a procedure, and the gap
between the two is exactly the delay that appears the day the board arrives:
someone has to decide what to hold constant, what to vary, what to record, how
many times, when to stop, and what result would count. This stage writes those
procedures ahead of the hardware so the first hour with the vehicle is
measurement, not test design. It also makes visible, before the bench, the
bench-only software several of the procedures need - which is the other half of
the same delay.

**What was added - documentation only, no firmware change**

- `docs/bringup/README.md` (new) - the index: the three check classes, the
  protocol order and the blocking graph, the common safety preconditions, the
  record every session leaves, how a result reaches `VERIFIED_STATE.md`, the
  protocol format, and the coverage matrix that maps every planned
  hardware-verification field to the protocol that would verify it.
- `docs/bringup/P-H0-nano-upload-serial.md` (new) - upload/boot reliability, the
  operator interface on a real host, malformed input, the boot line, and the
  observed control cadence with the transport idle, busy and unread.
- `docs/bringup/P-H0W-watchdog-qualification.md` (new) - baseline reliability
  with the watchdog disabled, enabling the prepared path in a bench build,
  healthy soaks at three load conditions, a deliberate main-loop hang, the
  reset and the re-initialization, recovery and re-flash cycles, and the timeout
  chosen from the observed margin. It states its own scope limit: it establishes
  that the MCU resets, **not** that a reset de-energizes the motor output.
- `docs/bringup/P-H1A-driver-electrical.md` (new) - wiring continuity with power
  off, the driver's idle levels, the level-to-output truth table with the motor
  disconnected (including the awake-`EN`-low state the plan deliberately does
  not name), and the PWM behaviour. It explicitly cannot establish motor
  polarity, because no motor is connected.
- `docs/bringup/P-H1B-sense-inputs.md` (new) - battery raw counts against a metered
  supply with an explicit established/not-established verdict, the quiescent
  current count with the scaling marked unestablished, the `nFAULT` input and
  idle level with the trip behaviour deferred, and the trigger's active level
  and bounce.
- `docs/bringup/P-H2-encoder-manual.md` (new) - hand-driven tick sign (#4),
  ticks per revolution and counting quality (#5), with an explicit statement
  that the acceptance is repeatability, not agreement with a predicted count.
- `docs/bringup/P-H3-first-low-duty-drive.md` (new) - the first powered step: one
  PH level and one duty increment at a time, drivetrain unloaded, the minimum
  duty that moves from rest, the de-energized behaviour, and the polarity fact
  (#3) once the forward convention is defined mechanically.
- `docs/bringup/P-H4-rollout.md` (new) - the kinematic scale hand-rolled
  (unpowered) and the powered scale separately, so the difference between them is
  the first #10 slip evidence instead of being averaged away; sets #6.
- `docs/bringup/P-H5-velocity-trace.md` (new) - the unloaded effort-to-velocity
  trace and the estimator's own `vel_ok` windows, recorded as H6's input.
- `docs/bringup/P-H6-ff-pi-tuning.md` (new) - feed-forward from the measured
  trace, then one gain at a time against a fixed profile at two supply voltages;
  no gain value is prescribed.
- `docs/bringup/P-H7-distance-braking-reversal.md` (new) - the bootstrap order
  (measure the brake offsets off the run path, then arm), distance accuracy at
  three targets so a scale error and a zero error are distinguishable, braking
  distance against speed with the relationship recorded rather than averaged,
  and the stop-before-reverse verification.
- `docs/bringup/P-H8-bottle-integration.md` (new) - one mechanism variable at a
  time, the bottle's measured final position over repeats, and the failure modes;
  rule-derived line and placement distances stay rule-derived.
- `docs/bringup/P-H9-fault-injection.md` (new) - one injected fault at a time,
  mapped to the `FaultCode` vocabulary, plus the electrical `nFAULT` trip and a
  low battery, which the firmware does not yet enforce and which are recorded as
  "enforcement pending" rather than as passing.
- `docs/bringup/P-H10-full-run-repeatability.md` (new) - ten or more identical
  complete runs, hand measurements in the tooling's own CSV alongside the
  captures, the residual classified as systematic or random, and the #12
  verdict recorded from the measured slip.
- `README.md` - the `docs/bringup/` layout entry, the P8B stage line, and P8A
  moved out of "(current)".
- `VERIFIED_STATE.md` - the promotion step now points at `docs/bringup/` for the
  exact procedure, while stating that the procedures change no row until one
  passes. No row, and no status, was changed.

**Decisions**

1. **Three check classes, kept apart.** Every protocol is labelled electrical
   gate (**E**), on-target software check (**S**) or physical behaviour check
   (**P**), with the classes it also contains listed. An electrical fact, a
   software fact and a physical fact have different evidence, and the point of
   the label is that nobody promotes one as another.
2. **One unknown per protocol.** The list was split rather than compressed: the
   driver's truth table is measured with the motor disconnected (H1A), so a
   wrong-direction result in H3 cannot be blamed on an unmapped driver; the
   rollout is measured hand-rolled and powered separately (H4), so slip is a
   measurement instead of an error term. Where a protocol could not isolate a
   variable it says so and defers it (the current scaling, the `nFAULT` trip).
3. **No target values are prescribed.** Where the point is to measure a number -
   rollout, minimum drive duty, braking distance, gains, the watchdog timeout -
   the protocol states a repeatability or decision criterion to be
   *pre-registered* and records whatever the hardware produces. The existing
   `Provisional` constants are named as placeholders to replace, never as
   starting values to keep.
4. **The watchdog protocol does not claim motor-output safety.** Its scope limit
   is written into it: H0W establishes that the MCU resets and re-initializes and
   observes the pin levels afterwards; what those levels do at the driver outputs
   is H1A's measurement. This is the one place where it would have been easy to
   write one sentence too many.
5. **The timeout is chosen from the realized timer, not the requested number.**
   The library quantizes the timeout up to a prescaler x reload period and
   returns 0 when the request is unachievable; the protocol requires recording
   the realized period and refusing an unachievable request, and forbids
   inheriting the current `250 ms` placeholder without evidence.
6. **Outstanding bench instrumentation is named, not stubbed.** Four bench-only
   instruments are listed in the index and referenced by the protocols that need
   them (loop/watchdog counters, a bounded diagnostic drive, a raw tick readout,
   a raw trigger level), together with the requirement that none can exist in a
   production image and none can become a control input. They are   not built; the point of the list is that they are software work identified
   before the bench, and that the diagnostic drive must not become a second
   unaudited path around `SafetyAuthority`. The isolation mechanism was decided
   rather than left open: a separate PlatformIO `bench` environment beside
   `nano_r4` and `native`, so the production environment never compiles the bench
   sources at all. A build flag inside the production translation units was
   rejected because a flag can be flipped in the image that ships. The `bench`
   environment itself is **not** added yet; it is the first piece of that
   software work and part of the same deferral.
7. **The coverage matrix is the acceptance gate.** Every hardware fact
   `VERIFIED_STATE.md` lists, and the three on-target facts that have no row yet
   (the watchdog and its timeout, the host-drain behaviour of the link, the
   observed cadence), is mapped to the protocol that would produce it. A fact
   with no protocol is a gap; a protocol with no fact is scope creep, and the
   index says which is which.
8. **Armed-run bootstrapping is written down.** `arm` is refused while any
   calibration value is provisional, and the brake offsets *are* calibration, so
   the braking measurement cannot itself use the armed run path. H7 records the
   order: characterize off the run path, write the measured values, then arm and
   run the real path - and marks any number that crossed that boundary.

**Evidence (documentation and tooling verification only; no physical test is applicable)**

- No firmware source changed: `src/`, `lib/`, `include/`, `test/`,
  `platformio.ini` and `tools/` are untouched by this stage. The image is
  therefore expected to be identical, and it is: `pio run` (nano_r4) - SUCCESS,
  RAM **17.8%** (5848 / 32768 bytes), Flash **24.8%** (65028 / 262144 bytes),
  the same figures D15 recorded.
- `pio test -e native` - **456 test cases, 456 succeeded** (unchanged from D14),
  confirming nothing outside `docs/` was disturbed.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -
  **91 test cases, OK.** The tooling the protocols cite still works.
- The one command this stage cites from the tooling was executed to confirm the
  documentation is exact: `analyze --print-measurement-template` prints the
  header and exits, but the `analyze` subcommand still requires a log positional,
  so the working invocation is
  `python tools/telemetry/analyze.py analyze --print-measurement-template <any-log>`.
  P-H10 records that invocation, because a procedure that cannot be run as
  written is not a procedure.
- `docs/bringup/` contains **14 files**: the index and the thirteen protocols
  (H0, H0W, H1A, H1B, H2, H3, H4, H5, H6, H7, H8, H9, H10). Every link in the
  index resolves to a file on disk.
- `git status`: `docs/bringup/` is new and untracked; `README.md`,
  `VERIFIED_STATE.md` and `DEVELOPMENT_LOG.md` are modified. No commit was made.

**Not verified**

Everything about the vehicle, and the procedures themselves. No protocol has
been run, no hardware was powered, no measurement exists, and no row in
`VERIFIED_STATE.md` was added or modified - the hardware table still reads
UNVERIFIED #1-#12. The protocols are a plan, and a plan is not evidence. The
bench instrumentation several of them require does not exist, so those protocols
cannot be executed as written until it is built. In particular, this stage makes
**no** claim that the watchdog starts, refreshes or resets, that the driver
levels are correct, that the encoder counts, or that any distance conversion is
right.

**Hard stop honored**

The stage stops where it must: before any hardware is powered. Writing a
procedure is not running a test, and nothing here is an observation.

**Pending physical work**

Execute the protocols in order, starting with P-H0 (upload, boot and serial
link), then its watchdog qualification P-H0W, then the driver electrical checks
P-H1A and P-H1B. The first four protocols need no motion and can run as soon as
a board exists; H3 is the first powered step and every direction, speed and
distance result depends on it.

