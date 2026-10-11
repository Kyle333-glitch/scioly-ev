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

---

## D17 - 2026-09-27 - P9A - Pre-hardware readiness validation

**Entry gate**

P0-P8 are implemented and reconciled (D0-D16). No vehicle hardware is available
and none was used. Nothing was committed.

**The question this stage answers**

Is the repository waiting on hardware, or on unfinished deterministic software?
It is waiting on hardware. Every check that can be run without a board passes
from a fresh build, and the only outstanding items are the physical measurements
`VERIFIED_STATE.md` already lists plus the bench-only instruments D16 named.

**What was run (all from the repository root, no hardware)**

- `pio run -t clean` then `pio run` (nano_r4) - SUCCESS, RAM **17.8%**
  (5848 / 32768 bytes), Flash **24.8%** (65028 / 262144 bytes), and **no
  compiler warning or error** anywhere in the clean build at the project's
  `-Wall -Wextra`. The figures are identical to D15/D16, which is the expected
  result: this stage changed no firmware source.
- `pio test -e native` - **456 test cases, 456 succeeded** (unchanged from D14).
  The whole-run and fault harness is part of it: `test_scenario` drives the
  nominal run through the real chain to `Done`, replays that trace
  byte-identically, latches every fault identity in every phase, exercises the
  stale-odometry / stale-stop-evidence / stalled-clock / rewound-clock cases,
  a mid-run disarm, an illegal direct reverse, and a reset followed by a second
  run; `test_serial_independence` pins that a stalled or abusing host cannot
  change what the chain decides.
- ARM cross-check over the pure core: `arm-none-eabi-g++ -std=gnu++11 -Wall
  -Wextra -fsyntax-only` on every `lib/evcore/src/*.h` - no diagnostic except the
  expected `#pragma once in main file` artifacts (one per header). The same
  check D1 recorded.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -
  **111 test cases, OK** (see the correction below).
- `python tools/telemetry/analyze.py synth --force` then `analyze` over the
  three committed fixtures - exit 0, report tree written, and the regenerated
  fixtures are byte-identical to the committed ones (`git status` clean), so the
  generator and the fixtures still agree.

**Static audit of the tree**

- **Debug leftovers:** no `TODO`/`FIXME`/`XXX`/`HACK`/`TEMP` marker in `src/`,
  `lib/`, `test/` or `tools/`.
- **Motor-output access:** a search for the three planned pin names matches only
  their definitions (`src/PinMap.h`) and the single adapter (`src/MotorDriver.h`)
  that writes them. No other file writes them, and `lib/` cannot - it contains no
  Arduino header at all.
- **Blocking:** no `delay()`/`delayMicroseconds` in `src/` or `lib/`. The only
  unbounded-looking loops are the bounded 2000 ms boot wait in `setup()` (the one
  initialization wait the nonblocking contract allows) and bounded per-pass
  cursors; every serial write is gated on `availableForWrite()` in
  `SerialTransport.h`, and `Serial` is referenced only from `main.cpp` and that
  transport.
- **Allocation:** no `new`/`malloc`/`calloc`/`realloc`/`std::vector`/`std::string`
  in `src/` or `lib/`.
- **Duplicated state:** `RunPhase` is the one phase vocabulary (`VehicleState.h`
  keeps no second list); the run policy's fault latch and the authority's are two
  deliberate latches, both read by `systemState()`; the watchdog's counters live
  only in `WatchdogCheckpoint.h`.
- **Provisional constants:** no `ValueSource::Measured` appears in `src/` or
  `lib/` outside comments and the enum itself. Every calibration value, safety
  limit, gain, threshold, dwell, timeout and polarity encoding in
  `src/VehicleConfig.h` is `Provisional`; the one rule-derived value (the
  Complete Stop dwell) is `RuleDerived`. No physical constant is spelled inline
  in firmware logic.
- **Watchdog:** `kWatchdogEnablementRequested` is `false`, the adapter compiles
  and links, and nothing claims it starts, refreshes or resets. Correct and
  unchanged pending H0W.
- **Tool/protocol correspondence:** the schema mirror is not a free-standing
  copy - `test_schema_mirror` re-reads the firmware's `TelemetryRecord.h`,
  `TelemetryFormat.h`, `ConfigurationChecks.h` and `BoundedText.h` and fails on
  any drift; and the `docs/bringup/` protocols use the interfaces the firmware
  actually implements (`version`/`status`/`checks`/`config show`/`config set`/
  `announce`/`arm`, `EVS1`/`EVE1`, `BOOT,`).

**Correction to a recorded evidence figure (append-only, per this file's rule)**

D15 recorded "91 test cases, 91 succeeded" for `tools/telemetry/` and D16
carried that figure forward. The reproducible count is **111**:
`test_schema_mirror` 9, `test_parse` 34, `test_cli` 25, `test_metrics` 23,
`test_distribution` 20. D15's own breakdown (9 + 34 + 23 + 25 = 91) shows the
omission: the `test_distribution` suite was never counted. Nothing failed and
nothing is skipped; the tooling is correct and only the tally was wrong. D15 and
D16 are left unedited, as this file requires.

**What was fixed**

No firmware or tooling defect was found, so no source was changed. The one
concrete defect this stage found was the stale evidence tally above, corrected
by recording it here rather than by rewriting D15/D16; `README.md` now lists P9A
as the current stage. No feature was added, no tuning value was chosen and no
hardware status was promoted.

**Not verified**

Everything about the vehicle, and the bench paths the protocols need. No
hardware was powered, `VERIFIED_STATE.md` #1-#12 all remain UNVERIFIED and no row
was added or modified, and the watchdog's on-target behavior remains unobserved.
A clean build, a passing host test and a passing analysis run prove logic and
tooling only; they are never evidence about the vehicle.

**Hard stop honored**

The stage stops before the independent P9 audits, and no external model was used.

**Pending physical work**

Unchanged: build the bench instrumentation (B1-B4) several protocols require,
then run the `docs/bringup/` protocols in order from P-H0. H3 remains the first
powered step, and every direction, speed and distance result depends on it.

---

## D18 - 2026-09-27 - P9A audit fixes - One-way street on the zero-step path, and one owner for the refused estimate

**Why**

A follow-up audit of P9A (D17) returned two findings (F1, F2) and one recorded
observation with no fix. F1 was a latent correctness hole in the distance-aware
target shaper; F2 was a maintainability hazard in how the vehicle layer declares
the absent-`VelocityEstimate` value. The observation concerned how much near-zero
dwell can be credited across a control-loop stall. The scope was deliberately
limited to those three: no behavior outside the shaper's zero-elapsed-time path
changed, no safety contract moved, no hardware fact was invented, and nothing in
`VERIFIED_STATE.md` was touched.

**The findings and their fixes**

1. **F1 - the `dtMs == 0` early return could hand back the previous direction.**
   `ev::nextTargetVelocity()` holds the previous target when no time has passed,
   and the one-way reversal street (decelerate to zero before the sign may
   change) was enforced only on the `dt > 0` path. A caller that re-entered the
   profile with a zero step after a requested direction change therefore received
   a fully signed target of the old direction - the exact step the rate limits
   exist to forbid, admitted through the one input combination the guard never
   saw. It is not reachable from `ControlScheduler` today (a frozen clock makes
   the tick not-due, and a rewound one is refused) and the vehicle layer's
   `targetVelocityPointsAlongLeg()` gate would refuse the leaked sign anyway, but
   it becomes reachable as soon as bench tooling hand-builds profile inputs. The
   fix is the three-line guard the audit asked for: when the current target's
   sign is non-zero and contradicts the requested sign, the hold collapses to
   exactly `0.0` (`MotionProfileStatus::TimeDidNotAdvance` unchanged, the
   ceilings still reported, and both rate brackets reported as the held
   magnitude). Zero asks for no powered effort and is the value the reversal
   street converges to anyway. The `MotionProfileInputs::dtMs` comment and the
   `TimeDidNotAdvance` status comment now state the exception.
2. **F2 - the zero-value shape of `VelocityEstimate` was spelled by hand.**
   `VelocityEstimator.h` pins `Valid == 0` but left `WarmingUp` (value 6)
   unpinned, and the vehicle layer's held members wrote the refusal shape out
   field by field. A new field or a reordered enumerator would have compiled
   cleanly and left a garbage-looking default estimate in the one place no test
   inspects. The fix is the audit's alternative and the stronger of the two: the
   shape now has one owner, `ev::refusedVelocityEstimate(status)`, which
   `VelocityEstimator::refuse()` also uses, so the estimator and the held
   defaults cannot drift apart. `WarmingUp == 6` is pinned by a `static_assert`
   in `VelocityEstimator.h` (mirrored in `test_velocity_estimator`), so a
   reorder fails the build. Both hand-written initializers in
   `src/VehicleRuntime.h`, and the two mirrored copies in
   `test/test_scenario/ScenarioHarness.h`, now call the factory.
3. **The recorded observation (no fix, by design) - dwell credit across a
   control-loop stall.** `NearZeroVerifier` credits a gap between two advancing
   stamps as continuous near-zero evidence, because two stamps cannot say whether
   the loop ran in between. That is bounded for a reversal (the sequence
   evaluates its own timeout before each verification step), but the final stop
   has no such owner, so the bound there is how long the control loop can stall.
   This is not a defect in the frozen architecture - the documented owner of a
   stalled loop is the command-timeout/watchdog path - so no code was changed.
   Instead the missing number was made owed rather than assumed: the H0W protocol
   now requires the **maximum control-loop stall** to be recorded per load
   condition (Steps 2 and 5), with a section stating why it matters, and
   `NearZeroVerifier.h` and the B1 instrumentation entry cross-reference it.
   Until that measurement exists, the bound on dwell credited without
   observation is an open fact and is recorded as one.

**Evidence (software verification; no physical test is applicable)**

- `pio test -e native` - **458 test cases, 458 succeeded** (was 456). The two new
  cases are `test_no_elapsed_time_never_hands_back_the_previous_direction`
  (`test_motion_profile`) and
  `test_the_refused_estimate_shape_is_owned_in_one_place`
  (`test_velocity_estimator`).
- The F1 proving test was verified to **fail before the fix**: with the guard
  temporarily removed, the case reported
  `test_no_elapsed_time_never_hands_back_the_previous_direction: Expected TRUE
  Was FALSE` and `test_motion_profile` ran 1 failed / 28 succeeded. The guard was
  then restored and the target passes. The property sweep in `test_motion_profile`
  now includes a `0U` step in its cross product, so "no direction flip inside one
  step" is exercised on the zero-elapsed-time path too rather than only by the
  one case.
- `pio run` - nano_r4 **SUCCESS**, RAM **17.8%** (5848 / 32768 bytes), Flash
  **24.8%** (65044 / 262144 bytes; +16 bytes from D17, from the guard and the
  shared factory). No compiler warning or error at `-Wall -Wextra`.
- ARM cross-check over the pure core: `arm-none-eabi-g++ -std=gnu++11 -Wall
  -Wextra -fsyntax-only -Ilib/evcore/src` on every `lib/evcore/src/*.h` - no
  diagnostic other than the expected `#pragma once in main file` artifacts, one
  per header. This is what pins that the new `constexpr` factory compiles for the
  target and not only for the host.
- `git diff` reviewed: `MotionProfile.h`, `VelocityEstimator.h`,
  `NearZeroVerifier.h`, `src/VehicleRuntime.h`,
  `test/test_motion_profile/test_main.cpp`,
  `test/test_velocity_estimator/test_main.cpp`,
  `test/test_scenario/ScenarioHarness.h`,
  `docs/bringup/P-H0W-watchdog-qualification.md`, `docs/bringup/README.md`,
  `README.md` and this file. No unrelated cleanup, no reformatting, and no change
  to a frozen safety contract or to `src/VehicleConfig.h`.

**Not verified**

Nothing about the vehicle. The F1 path is a pure-logic branch and the F2 change
is compile-time plumbing; both are proven by the native suite and the ARM
cross-check, which say nothing about whether a real vehicle follows the shaped
target, brakes, or stops. Every `VERIFIED_STATE.md` row (#1-#12) stays UNVERIFIED
and no row was added or modified. The two new tests are not evidence about
hardware, and the maximum control-loop stall remains unmeasured - it is a
required H0W output, not a number.

**Next stage**

Unchanged from D17: build the bench instrumentation (B1-B4) several protocols
require, then run the `docs/bringup/` protocols in order from P-H0. H3 remains
the first powered step, and every direction, speed and distance result depends on
it.

## D19 - 2026-09-28 - P9D - Adversarial first-power review fixes, and the bench instrumentation B1-B4

**Why**

P9D was an independent, read-only review of the live tree, aimed at the ways this
software could make the first powered test unsafe, misleading or unnecessarily
slow. It found one latent firmware defect, closed the two instrument gaps the
bring-up protocols had been written around, and turned recorded-but-open
observations into enforced configuration facts. Production behavior that reaches
the driver was not loosened anywhere: the vehicle image still cannot arm, command
an effort or start a run, and every new capability that can apply effort lives in
a separate image that the production build does not compile.

**The findings and their fixes**

1. **M1 - the watchdog could expire during boot.** `WatchdogCheckpoint::begin()`
   opened the timer inside `setup()`, and the only refresh point is the end of a
   main-loop pass, so an enabled build's first refresh sat behind the rest of
   `setup()` including the bounded (up to 2 s) wait for a serial host. With the
   requested 250 ms placeholder that is a board that reset-loops before it ever
   boots, and the rule H0W used - "exceed the worst interval between consecutive
   refreshes" - cannot see it, because the first refresh has no predecessor. The
   fix is chronological and then mechanical: `begin()` now only records the boot
   instant, a separate `openIfRequested()` opens the timer at the **end** of
   `setup()` after the bounded wait, the measured gap is exposed
   (`timeToFirstRefreshMs()`) for H0W, and the rule itself became pure logic
   (`lib/evcore/src/WatchdogPlan.h`) that the requested constants are asserted
   against at compile time - a build that requests enablement with a timeout that
   cannot cover both gaps no longer compiles. H0W's prerequisites, Step 1, Step 2
   and Step 6 were updated to the new entry point, the boot-gap measurement and the
   rule, and to require a build-graph and symbol check that the bench tree is
   absent from the production image rather than a macro check.
2. **M2/M3 - the instrumentation the protocols depend on did not exist.** H3 could
   not run as written (its motion comes from B2), H0W had no B1 readout, H1B had no
   raw input readout and H2/H3/H4/H5 had no raw tick readout. All four now exist:
   `src/bench/BenchConsole.h` (the `bench` verb set and the readouts),
   `src/bench/BenchMain.cpp` (the bench entry point, keeping the production boot
   order and pass order), and `lib/evcore/src/BenchDrive.h` (the bench drive's
   safety model as pure, host-tested logic). Exclusion is a property of the build
   graph: `env:nano_r4` compiles `src/` without `bench/` and `env:bench` compiles
   `src/` without the production `main.cpp`. B2's model: a deliberate arm, a
   bounded duration that expires on its own, effort/duration/direction ceilings, a
   per-arm command budget, a latched fault on an operator abort or on an encoder
   that stops changing while driving, and no output at all while the production run
   is armed. Its output is a type no production module can construct, it is written
   through a separate adapter entry (`motorDriver::applyBench()`), and it is applied
   after the tick - so the tick's own de-energized write is the backstop if the
   bench layer stops running. A refusal that never energized anything (an
   over-ceiling or zero command) is a named refusal and latches nothing, which keeps
   "latched fault" meaning "something was applied and had to be aborted".
3. **S1 - the driver's fault output was sampled and never interpreted.** It is now
   interpreted by one pure module, `lib/evcore/src/DriverFault.h`, from a configured
   polarity, and the shipped polarity is `Unverified`: no level becomes a
   `FaultCode`, the raw level is still reported and counted, and the new
   `DriverFaultPolarityVerified` configuration check keeps the vehicle unarmable
   until H1B measures the line. Promoting the polarity gives the line the identity
   `FaultCode::DriverFaultBar` (added to the fault vocabulary, mirrored in
   `tools/telemetry/evtelemetry/schema.py`), which the authority latches exactly as
   it latches any other fault.
4. **S2 - the final stop's dwell could be credited across a loop stall.** The D18
   observation is now enforced rather than owed: `NearZeroConfig` carries
   `maxUpdateGapMs`, a gap wider than it restarts the observed interval and reports
   `NearZeroStatus::UpdateGapExceeded` instead of being credited as continued
   evidence, and the limit is a `Sourced` value whose measured source is the H0W
   maximum control-loop stall. An unmeasured limit is refused by
   `validateNearZeroConfig()`, so no stop can be verified until the stall has been
   measured - the safe direction, and it fails loudly rather than silently
   crediting.
5. **S5 - a promoted value could authorize motion without naming its evidence.**
   `Sourced` now carries an evidence pointer, `citesEvidence()` requires a
   non-placeholder value to name one, and the new `MeasuredValuesCiteEvidence`
   configuration-build check reports the verdict (it passes while everything is
   provisional and bites on the first promotion that cites nothing).
   `VERIFIED_STATE.md`'s promotion steps and `docs/bringup/README.md`'s record
   procedure now require the citation.
6. **S3/S4/S6 (documentation).** `kStopEvidenceMaxAgeMs` now states that its honest
   value comes from the H5 trace and the H7 characterization, and why it is not
   derived from the cadence. H3's provoked fault is now a **fixture** on the output
   shaft rather than a hand on it, with the kill switch in reach, and H3 records the
   bench ceilings in force before it starts. H0's host-not-reading load case
   (already present) is now named as one of the loads the watchdog margin depends
   on; H0W's Step 5 already covers it.

**Evidence (software verification; no physical test applies or was performed)**

- `pio test -e native` - **497 test cases, 497 succeeded** (was 458). The new
  targets are `test_watchdog_plan` (9 cases: the qualification arithmetic,
  including that a timeout covering only the refresh interval is refused),
  `test_bench_drive` (15 cases: bounded expiry and its wraparound, the latched
  abort, the encoder-stall fault, every limit refusal, the per-arm budget and the
  output shape) and `test_bench_command` (13 cases: the tokenizer, verb
  recognition and the `bench drive` argument grammar). `test_near_zero_verifier`
  gained three gap cases and lost the case that pinned the old
  credit-across-a-gap behavior, which is the behavior this stage deliberately
  changed.
- **Two defects the console's own tests caught, which is why the grammar moved
  out of the console.** The first version parsed the tokens inside
  `src/bench/BenchConsole.h`, where nothing host-side could reach it: it indexed
  `tokens[1]` as the drive direction (it is the verb, `drive`), so every well-formed
  `bench drive` would have been refused as a bad direction; and its
  overflow check for an unsigned decimal was off by one digit, so `4294967296`
  parsed as a wrapped `0`. Both are pure-logic bugs, both are now impossible to
  ship unnoticed: the grammar is `lib/evcore/src/BenchCommand.h` and
  `test_bench_command` pins the indexing, the exact token count, over-long lines
  and the overflow boundary.
- `pio run -e nano_r4` - **SUCCESS**, RAM **18.2%** (5948 / 32768 bytes), Flash
  **25.2%** (66020 / 262144 bytes). `pio run -e bench` - **SUCCESS**, RAM **18.7%**
  (6132 bytes), Flash **26.7%** (70016 bytes). No compiler warning or error at
  `-Wall -Wextra` in either image. The vehicle image's size is unchanged by the
  bench work, which is the point: `BenchCommand.h` and the console are not compiled
  into it at all.
- Bench exclusion, checked against the linked images rather than asserted:
  `arm-none-eabi-nm` over `.pio/build/nano_r4/firmware.elf` matches **0** symbols
  for `benchConsole|BenchDrive|BenchCommand|BenchTokens`, and over
  `.pio/build/bench/firmware.elf` matches **24** (including
  `evhw::benchConsole::healthLine`). The build graph is the guarantee; this is the
  check that it holds, and the two numbers are the ones a later reader can re-run.
- Static checks re-run: `grep -rn "WDT" src/` matches only
  `src/WatchdogCheckpoint.h`; the three planned motor-pin names appear only in
  `src/PinMap.h` (definitions) and `src/MotorDriver.h` (writes), with the bench tree
  reaching them through `applyBench()`; `digitalWrite`/`analogWrite` appear in
  `src/` only in `MotorDriver.h`, `main.cpp` and `bench/BenchMain.cpp`, the last for
  the status LED.
- ARM cross-check over the pure core: `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra
  -fsyntax-only` on every `lib/evcore/src/*.h`, including the three new headers - no
  diagnostic other than the expected `#pragma once in main file` artifacts, one per
  header.
- `python -m unittest discover` in `tools/telemetry` - **111 tests, OK**. The schema
  mirror passes with the added fault name, the two added configuration checks in
  their report order, and the widened check-report bound.
- `git diff` reviewed. Files: `platformio.ini`; `src/main.cpp`,
  `src/WatchdogCheckpoint.h`, `src/MotorDriver.h`, `src/SensorInputs.h`,
  `src/VehicleConfig.h`, `src/VehicleRuntime.h`, `src/VehicleRuntime.cpp`;
  `src/bench/BenchConsole.h`, `src/bench/BenchMain.cpp`;
  `lib/evcore/src/WatchdogPlan.h`, `DriverFault.h`, `BenchDrive.h`,
  `BenchCommand.h`, `Units.h`,
  `ConfigurationChecks.h`, `NearZeroVerifier.h`, `MotorDriverPlan.h`,
  `VehicleState.h`, `TelemetryRecord.h`, `OperatorResponse.h`, `BoundedText.h`;
  `test/test_watchdog_plan/`, `test/test_bench_drive/`, `test/test_bench_command/`,
  `test/test_near_zero_verifier/test_main.cpp`,
  `test/test_operator_response/test_main.cpp`,
  `test/test_competition_state/test_main.cpp`,
  `test/test_motion_chain/test_main.cpp`,
  `test/test_reversal_sequencer/test_main.cpp`,
  `test/test_scenario/ScenarioHarness.h`, `test/test_scenario/test_main.cpp`;
  `tools/telemetry/evtelemetry/schema.py`; `docs/bringup/` (the index, H0, H0W,
  H1B, H2, H3, H4, H5, H6, H7, H9); `SOFTWARE_ARCHITECTURE.md` (sections 14, 15, 16
  and a new section 17), `README.md`, `VERIFIED_STATE.md` and this file. No
  unrelated cleanup and no reformatting.

**Not verified**

Everything about the vehicle. The bench readouts, the bench drive and every
ceiling in it are **built and never run**: no instrument has been flashed, no
command has been sent, no motor has been powered, and no watchdog behavior has
been observed. The `env:bench` and compile-time-rule rows added to
`VERIFIED_STATE.md` are build-and-source facts and say nothing about the board.
Both watchdog placeholder numbers (the boot budget and the worst refresh interval)
are still guesses, as is the near-zero gap limit they will inform; H0W replaces
them with measurements and is now the only path to them. Every hardware row
(#1-#12) stays UNVERIFIED, and the bench image's existence does not change that:
it is a way to measure, not a measurement. In particular the review's own
observation stands: the dwell-credit bound is now enforced, but its *value* is
still an unmeasured placeholder, so a stop cannot be verified until H0W has run.

**Next stage**

Execute the protocols in `docs/bringup/` in order, starting with P-H0 against the
vehicle image. H0W (bench image) follows, and its Step 6 is now the only place the
watchdog timeout and the near-zero gap limit may be chosen from. H3 remains the
first powered step and its Step 0 must pass before any powered command, in a bench
build whose ceilings are recorded.
## D20 - 2026-09-28 - P9D (second pass) - the bench/production composition: one writer, one arbitration, one hang hook, one environment

**Why**

A second independent review of the same post-D19 tree - targeting the newest,
still-uncommitted layer, the seam between the bench instrument and the
production chain - reported four MUST-FIX and five SHOULD-FIX items. Every one
was checked against the live tree by symbol rather than by line number (the
report predates the move of the bench grammar into `BenchCommand.h`, so its line
numbers are approximate). Seven were real and are fixed here; two did not hold up
as written and are recorded as such below. Production reach to the driver is not
loosened anywhere: the production image still cannot arm, command an effort or
start a run, and the new arbitration only ever *removes* the bench layer's access
to the pins.

**Validation of the report**

Real, fixed here:

- **MF-1 (the serious one).** Two writers commanded the motor pins in every bench
  pass: the tick's stage 9 wrote the authority's de-energized command
  (`nSLEEP` low) and, microseconds later, `applyOutput` wrote the bench command
  (`nSLEEP` high + PH + duty), so a bench command put a sleep/wake edge on
  `nSLEEP` once per 20 ms control period. Worse, the gate was
  `instrument.output().isDriving() && !runtime.armed()`, and `!armed()` is true
  precisely in the states that need the motor off - a latched fault clears arming
  (`SafetyAuthority::authorize()`) and a violated tick order calls
  `motorDriver::failSafe()` then disarms - so the bench could write a powered
  command over a fail-safe write. `VehicleRuntime` had no way to express the
  correct test.
- **MF-2.** H0W Step 3 requires a deliberate hang, and no hook existed.
- **MF-3.** Watchdog enablement was one literal in a header compiled by **both**
  images, so enabling it for the H0W bench qualification would have silently
  changed the vehicle build's decision too.
- **MF-4, SF-1, SF-2, SF-4.** All four as described (details below).

Not as written:

- **SF-3's citation.** The claim that `src/PinMap.h:66` "records an unresolved
  D9/D10 shared-timer conflict" is wrong: no such note exists anywhere in
  `src/PinMap.h`. The underlying concern is real and is now documented there (a
  core-wide `analogWriteResolution()` plus an unrecorded timer assignment means a
  second PWM consumer can silently change the frequency at `EN`), but the fix is
  new text, not a follow-through on text that was there.
- **MF-4's premise needed a qualifier.** H3 already requires H1B to have passed,
  and H1B is what promotes the polarity to `Measured` - so "the firmware ignores
  the driver fault" is the state *before* that promotion, not the state H3 runs
  in. What was genuinely missing is that H3 never told the operator how to tell
  which state they are in, or that the firmware's fault response cannot be the
  stop authority for the first overcurrent event either way. That is what was
  added.
- **The report's own retraction is correct and worth recording**: the 2000 ms
  `while (!Serial)` wait does not race the watchdog, because `openIfRequested()`
  runs after it (`src/main.cpp`). That ordering is D19's M1 fix and it holds.

**The findings and their fixes**

1. **MF-1/SF-4 - one writer per pass, and a host-tested arbitration that is not
   `!armed`.** The decision was extracted into pure logic,
   `lib/evcore/src/MotorArbitration.h`: `ev::benchApplyVerdict(benchDriving,
   runArmed, faultLatched, runUnderway, orderViolationDetected)` refuses (with a
   named reason, most severe first) on a latched fault, a violated tick order, a
   run under way or an armed run, and permits a write only when the instrument is
   driving and nothing outranks it. `VehicleRuntime` gained exactly the three
   facts it could not previously express - `faultLatched()` (either latch; the
   same fact `status` reports, now with one definition), `runUnderway()`
   (`ev::isRunPhaseUnderway`) and `orderViolationDetected()` (the order guard's
   since-boot violation/abandonment counters).
   The single-writer half: `MotorDriver` gained a pins-ownership flag
   (`motorPinsOwned()` / `setMotorPinsOwned()`, neutral names on purpose so the
   vehicle image contains no bench-named symbol). The bench layer now decides
   ownership **before** the tick (`benchConsole::beginPass()`) and writes **after**
   it (`benchConsole::applyOutput()`), so a bench pass has exactly one writer and
   the pins see one intent. `motorDriver::apply()` yields while the flag is set;
   `failSafe()` deliberately does **not** yield, so a de-energize always reaches
   the pins. `applyOutput()` also refuses to write when the pass's tick ran out of
   order - it aborts the instrument instead - so a fail-safe write can never be
   overwritten by a bench command in the same pass.
   A refusal while driving now **aborts and latches** the instrument rather than
   leaving it silently disarmed, so the bench cannot resume applying effort when
   the blocking condition clears. `test_bench_arbitration` (9 tests) covers the
   predicate, including the two regression cases the old gate got wrong (a
   fault-latched vehicle and an order-violated tick are both `!armed` while the
   motor is not free) and an exhaustive 32-combination invariant check.
2. **MF-2 - the hang hook H0W Step 3 requires.**
   `kDeliberateHangAfterPasses` in `src/bench/BenchMain.cpp`: a compile-time pass
   count after which the loop hangs inside a pass, staging one
   `BENCH,hang=1,image=bench,after_passes=<n>` line first. It is a constant and
   **not** a console verb, deliberately - no serial input can hang the board - and
   it lives in the bench tree, so the vehicle image cannot contain it. H0W's
   instrumentation list, prerequisites and Step 3 now name it, and the protocol
   states that the hook must never be set with the motor supply on, because B2's
   bounded expiry is evaluated by the same loop.
3. **MF-3 - enablement is per-environment, and production refuses the define.**
   `kWatchdogEnablementRequested` is now
   `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED != 0`, defaulted off in
   `src/WatchdogCheckpoint.h`. The new `[env:bench-watchdog]` sets the define
   (`pio run -e bench-watchdog`), so enabling the watchdog for H0W cannot touch
   what `pio run` or `pio run -e bench` decides, and `src/main.cpp` carries a
   `static_assert` that refuses to compile the vehicle image if the define is ever
   present - which keeps the production flip a separate, deliberate change. H0W's
   Step 0/1/Setup/held-constant text now names the two environments and forbids
   editing the header to enable it.
4. **MF-4 - H3 says who stops the motor.** H3 gained an explicit prerequisite:
   while the polarity is `Unverified` the firmware interprets no level as a fault
   (the level is sampled, counted and reported, but nothing is latched and no
   automatic stop happens), `checks`'
   `DriverFaultPolarityVerified=…` is how the operator tells whether the firmware
   is watching the line, and the stop authority for the protocol is the bench
   supply's current limit and the physical kill switch. The Safe-stop section says
   the same in one sentence, so an operator does not wait for a fault report that
   cannot come.
5. **SF-1 - `Brake` is not a brake until it is measured.** The plan has only two
   distinct level combinations for four mode names (`Coast` and `Disabled` are
   pin-identical; `Brake` is a drive with zero duty), and whether the `Brake`
   combination shorts the bridge or leaves it high-Z is unmeasured. Both candidate
   mappings are now pre-registered as a table in
   `docs/hardware/motor-driver-drv8874.md` with the consequence of each, and
   `P-H7` gained a prerequisite forbidding `Brake` (which is what
   `RunMotionCommand::BrakeForward`/`BrakeReverse` request, and therefore the
   run's deliberate stop dwell) from being credited as a hold until H1A test 3 is
   recorded. `P-H7` also gained the dwell-credibility bound from H0W's stall
   measurement.
6. **SF-2 - the driver-fault-low counter is per run.**
   `cyclesWithDriverFaultLow_` is cleared on a successful `arm` and by `reset`, so
   H1B Part 3 and H9 read their own test's activity rather than a since-boot
   total.
7. **SF-3 - the PWM reservation is written down.** `src/PinMap.h` and
   `docs/hardware/motor-driver-drv8874.md` now state that `analogWriteResolution()`
   is core-wide, that the EN pin's timer assignment is unrecorded, and that **no
   second PWM consumer or resolution change may be added until H1A test 6 records
   the switching frequency at `EN`**.

**SF-5 (process) - not actioned here.** The report is right that `P9A` and the
bench work are uncommitted and that `git rev-parse HEAD` therefore does not
describe the tree H1A/H3 would flash. `AGENTS.md` section 23 forbids committing
without being asked, so this is surfaced for a decision rather than done.

**Files changed**

New: `lib/evcore/src/MotorArbitration.h`,
`test/test_bench_arbitration/test_main.cpp`.
Changed: `src/VehicleRuntime.h`, `src/VehicleRuntime.cpp` (three accessors, one
definition for `status`'s fault flag, the counter reset), `src/MotorDriver.h`
(ownership flag, `apply()` yield, comments), `src/bench/BenchConsole.h`
(`beginPass`/`applyOutput`, the pass verdict, the hang report line),
`src/bench/BenchMain.cpp` (pass order, the hang hook), `src/WatchdogCheckpoint.h`
(per-environment enablement), `src/main.cpp` (production guard), `src/PinMap.h`,
`platformio.ini` (`env:bench-watchdog`), `docs/bringup/README.md`,
`docs/bringup/P-H0W-watchdog-qualification.md`,
`docs/bringup/P-H3-first-low-duty-drive.md`,
`docs/bringup/P-H7-distance-braking-reversal.md`,
`docs/hardware/motor-driver-drv8874.md`, `SOFTWARE_ARCHITECTURE.md` (sections 15,
16, 17), `VERIFIED_STATE.md` and this file. No unrelated cleanup.

**Verification**

- `pio test -e native`: **506/506** (was 497; +9 from `test_bench_arbitration`).
- `pio run -e nano_r4`: SUCCESS, RAM 18.2% (5956 B), flash 25.2% (65964 B).
- `pio run -e bench`: SUCCESS, RAM 18.7% (6140 B), flash 26.8% (70316 B).
- `pio run -e bench-watchdog`: SUCCESS, same RAM, flash 26.9% (70644 B) - the
  define changes one constant and links the timer calls.
- No compiler warnings in any of the three builds.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry`: 111
  tests, OK.
- `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over every
  `lib/evcore/src/*.h`: clean apart from the expected
  `#pragma once in main file` artifacts.
- `arm-none-eabi-nm` symbol isolation: `.pio/build/nano_r4/firmware.elf` filtered
  for `bench|hang` matches **nothing**; `.pio/build/bench/firmware.elf` matches 26
  symbols. (The first cut of the ownership accessor was named
  `benchOwnership()` and *did* put one bench-named symbol in the vehicle image;
  renaming it `motorPinsOwned()` - which is what the production tick legitimately
  reads - restored the isolation, and the docs' symbol-search check should use the
  `bench|hang` filter.)

**Not verified**

Everything about the vehicle, unchanged from D19. The bench instruments, the hang
hook, the arbitration and every ceiling are **built and never run**: no image has
been flashed in this change, no command has been sent, no motor has been powered,
and no watchdog behavior has been observed. The three new `VERIFIED_STATE.md` rows
are build-and-source facts. Every hardware row (#1-#12) stays UNVERIFIED, and MF-1
in particular is a defect that was found by reading, not by measuring: that one
writer now commands the pins per pass is a source fact, and whether the DRV8874
does what the planned levels intend (including whether a wake delay is required at
all) remains H1A's measurement.

**Next stage**

Unchanged, with one ordering constraint now explicit: H0 (vehicle image), then
H0W in **both** bench environments - `env:bench` for its Step 0 baseline, then
`env:bench-watchdog` for Steps 1-7 and the hang hook - then H1A, H1B, H2, then H3
as the first powered step. Nothing may be flashed from a tree that is not a
recorded checkpoint (SF-5).

---

## D21 - 2026-09-28 - H0 - First physical bring-up: Nano R4 upload, boot and serial link (not passed)

**What was run**

The Nano R4 was connected over USB for the first time and `P-H0` was executed.
No firmware source was changed in this session: the tree is the committed
`8e42d13` that D19/D20 left behind, and the point of the session was to find out
what it does on the board rather than to adjust it.

- detection: `pio device list` -> `COM3`, `USB VID:PID=2341:0074`,
  `SER=350D331C5A333131021133354B573462`, `LOCATION=1-3:x.0`.
- build: `pio run` SUCCESS, RAM 18.2% (5956 B), flash 25.2% (65964 B).
- uploads: eight attempts in total, one of them the first (driverless) failure -
  see the tally in `data/evidence/P-H0-2026-09-28/README.md`.
- serial: seven boot captures, a command/refusal/ malformed-input capture, a
  telemetry-rate capture, two `bench health` (B1) readouts, and one physical
  power cycle.
- tests: `pio test -e native` 506/506 (the acceptance gate's regression check;
  unchanged because nothing was changed).

Raw captures are in `data/evidence/P-H0-2026-09-28/` (copied unchanged from
`data/raw/`); the counts this session was judged against are in
`data/prereg/P-H0-2026-09-28.md`, which is **labelled retrospective** because it
was written after the first uploads rather than before.

**What was observed**

Everything below is firmware or host output, not an inference.

- The board enumerates in application mode as `USB VID:PID=2341:0074` and, in
  bootloader mode, as `USB\VID_2341&PID_0374&REV_0100` (`Nano R4 DFU`).
- Seven boots were observed. Each printed exactly one
  `EVE1,ver=1,t_ms=0,phase=Idle,armed=0,event=Boot,...` and answered
  `VERSION,fw=ev-nano-r4-p7b,telemetry_schema=1`.
- The control period on the board is 20 ms. Telemetry is **enabled at boot** at
  stride 1 (50 Hz): `telemetry status` reported `enabled=1,stride=1,rate_hz=50.0000`
  before any command was typed, which is `TelemetrySession`'s documented default
  and means P-H0's "leave telemetry off for the boot step" is an operator action
  rather than a default.
- `telemetry rate 10` was accepted (`stride=5,rate_hz=10.0000`) and the log then
  shows 90 consecutive samples exactly 100 ms apart, i.e. a whole number of
  control ticks; `telemetry rate 100` -> `ERR,...,reason=AboveControlCadence`
  and `telemetry rate 0.5` -> `ERR,...,reason=BelowMinimum`, each with an
  `EVE1,...,event=CommandRejected` record.
- With no host reading the port, records are dropped rather than waited on: the
  first capture opens with `drop_s=1790` accumulated over ~37 s of no host, and
  reconnecting flushed only the queue's own 8-record capacity of stale samples
  (the oldest: `t_ms=2000..2140`) before current ones.
- The command set answered in the shapes `help` documents. Refusals observed
  verbatim: with no held configuration `announce` ->
  `ERR,cmd=announce,reason=ConfigIncomplete,error=-,first_fail=-`; with a
  complete, rules-valid configuration
  (`config set level=state distance=7.50 time=15.0 bottle_line=2.00` ->
  `complete=1,rules=1`, `bottle_start_m=3.0000`) it becomes
  `ERR,cmd=announce,reason=PolicyNotUsable,error=-,first_fail=RunPolicyUsable`,
  which is what `announceVerdictFor()` deliberately names; `arm` ->
  `ERR,cmd=arm,reason=NoRunAnnounced`. `checks` reported
  `all=0,count=11,first=ConfigurationValid,mask=63427` - 11 of the 16 checks
  failing, the mask equal to the set of those 11 bits, and
  `ConfigurationValid` first because it means "an announced RunConfig passed rule
  validation" and nothing had been announced.
- Malformed input: `config set distance=abc` -> `InvalidValue`, `config se` ->
  `UnknownSubcommand`, `syntax junk here` -> `UnknownCommand`, `status extra
  tokens` -> `WrongTokenCount`, `config set distance=999999999999999999999` ->
  `ValueOutOfRange`, `config set level=galactic ...` -> `UnknownLevel`. Each was
  a named `ERR` with no `OK`, each carried `cmd=none` (a parse failure has no
  command id), and `status` stayed `phase=Idle` through all of them.
- `bench health` (B1) on the `env:bench` image reported `image=bench`,
  `enable=0,open_attempted=1,opened=0,open_refused=0,timeout_ms=250`,
  `dt_ms=20,late_ms=0,dropped=0,violations=0` at ~10 s and again at ~35 s
  (`control_ticks` 503 -> 1880, `passes` 468874 -> 1747855).
- The physical power cycle returned COM3 with the same VID/PID/serial and a
  single fresh boot record whose first sample is at `t_ms=2000`: with no host
  attached, `setup()`'s bounded wait runs its full 2000 ms before the loop's
  first pass.

**Findings**

F-1. **`pio run -t upload` cannot enter the bootloader on this board, by
  itself.** `builder/main.py` in platform `renesas-ra` 1.9.0 wires its
  `BeforeUpload` (which is what performs a `use_1200bps_touch`) only into the
  `sam-ba` branch; the `dfu` branch runs `dfu-util` with nothing before it. The
  core does implement the touch (`CheckSerialReset()` -> `goBootloader()` on
  `bps == 1200 && !dtr`), so a host-issued touch works: it was used for seven of
  the eight attempts. Consequence: `AGENTS.md`'s `pio run -t upload` is not a
  complete instruction for this board. The operator deferred the choice between
  a pre-upload hook in this repository and a documented touch-then-upload
  procedure until after the first successful upload, then left it open - it is
  still open.

F-2. **The bootloader needs a Windows driver that PlatformIO does not install.**
  With the board in bootloader mode Windows reported `Nano R4 DFU`,
  problem code **28**, and `dfu-util` failed with `LIBUSB_ERROR_NOT_SUPPORTED`.
  Arduino's own signed package was already on disk inside the framework package
  (`framework-arduinorenesas-uno/drivers/renesas.inf`, whose
  `DeviceIDNanoR4DFU` is exactly the `USB\VID_2341&PID_0374&REV_0100` this board
  presents), and the operator approved an elevated run of that package's own
  `dpinst-amd64.exe`, after which `dfu-util -l` listed `@CodeFlash`/`@DataFlash`
  and the download succeeded. This is a machine setup fact, not a defect in this
  repository, but it is why the first attempt failed and it belongs in the next
  person's instructions.

F-3. **One upload left the board on the bus but not enumerating.** After attempt
  7 (`pio run -e bench -t upload`), Windows showed
  `Unknown USB Device (Device Descriptor Request Failed)` on
  `USB\VID_0000&PID_0002\5&28FFA4CE&0&3`, no `VID_2341` device was present, and
  `pio device list` was empty. No software path could reach it; a physical USB
  power cycle was the only recovery, and it worked. The immediate cause is **not
  determined**. In `AGENTS.md` section 31 order the untested suspects are: the
  host link itself (a USB enumeration race between the CDC device leaving and
  the DFU device arriving, or a marginal cable/port - both held constant this
  session), then the bootloader's own USB initialisation, and only then a
  firmware defect. The retry of the same command immediately afterwards
  succeeded, so it is not deterministic. H0's decision rule makes this the
  reason the stage did not pass.

F-4. **The `BOOT,...` line never appeared.** `VehicleRuntime::serviceStagedText()`
  stages exactly one `BOOT` line per boot (`bootLinePending_`, set in `begin()`),
  `src/main.cpp`'s comment and `P-H0` Step 1 both expect it, and it did not
  appear in any of the seven boots - while the `EVE1,...,event=Boot` record did,
  and would have been sent *after* the staged line had the transport still been
  holding it. Read-and-reason says the staged line cannot be lost: the body is
  ~120 chars against `kSerialLineCapacity = 448`, `stage()` only fails when a
  line is already staged, and `pump()` leaves the line staged while
  `write()` returns 0 (which it does when DTR is not asserted, since
  `connected()` is `tud_cdc_connected() || ignore_dtr` and `ignore_dtr` is false
  in this firmware). That reasoning did not survive contact with the board, so
  the cause is **not determined**. The consequence is concrete: the operator loses
  the boot-time view the protocol asks for (`checks_all`, `regulator`, cadence,
  telemetry state), because the `EVE1` boot record carries none of those fields.
  Next diagnostic: expose the transport's own counters
  (`linesStaged_`/`linesSent_`/`linesAbandoned_`/`responsesLost_` are already
  reachable through `linesSent()` and `telemetryLinesSent()`), then capture a
  boot with a host attached from before the reset.

F-5. **An out-of-range `config set` changes the held state.** `P-H0` Step 3 asks
  that an out-of-range field be "refused with a named error, never silently
  clamped", and its acceptance rule asks for "every malformed input named and
  rejected with no state change". Observed: `config set distance=9.50` (the rule
  range is 5.00-9.00, `RunConfig.h`) answered **`OK,cmd=config set`** with the
  value held (`target_m=9.5000`, echoed by `config show`), `complete=1`,
  `rules=0`, `error=TargetDistanceOutOfRange`. It is named, not clamped, and the
  vehicle stays unarmable (`ConfigurationValid=0` and `RunPolicyUsable=0` keep
  both `announce` and `arm` refused), so the safety boundary holds; but the state
  *did* change, which is what the protocol's wording does not allow. Either the
  firmware should refuse an out-of-range value outright or P-H0 should say what
  it actually means here - that is a decision, not a fix to make silently.

F-6. **B1 cannot answer P-H0 Step 7 as written.** `bench health`'s `late_ms` and
  `dropped` come from `telemetry_.lateByMs`/`droppedDeadlines`, which
  `VehicleRuntime::runPass` copies from the current `ControlTickDecision`, so they
  describe the **last** control tick; the scheduler's cumulative worst case
  (`ControlScheduler::droppedDeadlines()`) is not reported. P-H0 Step 7 asks for
  "the worst case ... never the average", and `first_refresh_ms` is 0 while
  enablement is off, so the boot-to-first-refresh gap is not obtainable from
  `env:bench` either. This session therefore reports two samples 25 s apart
  (both clean), not a worst case, and leaves the boot gap to H0W.

**Files changed**

New: `data/prereg/P-H0-2026-09-28.md` (labelled retrospective),
`data/evidence/P-H0-2026-09-28/` (README plus twelve captures and one tool
transcript). Changed: `VERIFIED_STATE.md` (an on-target section and the
availability paragraph) and this file. **No firmware or tooling source was
changed**, no `platformio.ini` change, and nothing under `data/raw/` was added to
the repository (`data/raw/` is scratch and gitignored).

**Verification**

- `pio test -e native`: **506/506 succeeded** in 00:02:13 (the H0 acceptance
  gate's regression check).
- `pio run` (vehicle image): SUCCESS, RAM 18.2% (5956 B), flash 25.2%
  (65964 B); `pio run -e bench`: SUCCESS, RAM 18.7% (6140 B), flash 26.8%
  (70316 B). No warnings.
- Uploads: 7 successes, 1 failure (F-3) out of 8 attempts. After the restore
  upload the board is running the **vehicle** image and `h0-cycle-7.log` shows no
  `BENCH` line in it.
- The `env:bench` image left on the board mid-session was replaced; the last
  state of the board is the vehicle image, booted and answering.

**Not verified**

- **H0 did not pass.** Its own decision rule says a failed upload blocks the
  stage, and one did (F-3); the pre-registered counts were not met either
  (`data/prereg/P-H0-2026-09-28.md`). The honest reading: the upload/boot/serial
  path demonstrably works, and it is not yet reliable enough to be called
  verified.
- Nothing about the vehicle. No motor supply was connected; the motor, driver,
  encoder, drivetrain, battery and every rule-#1-#12 row are exactly as
  unverified as they were at D20. `bench drive` (B2) was not touched, and the
  deliberate-hang hook was left at its inert shipped value (`0U`).
- The two rows this session could have added - host-drain behaviour and observed
  cadence - are added only in the scoped form the captures support, and the
  boot-to-first-refresh gap is not among them.

**Next stage**

H0W stays blocked until H0 passes. Before repeating H0: decide F-5 and F-6 (fix
or reword), decide F-1 (pre-upload hook versus a documented touch), and write the
repeat's pre-registration **before** its first upload. The repeat should run the
upload series P-H0 asks for (at least 10) with the touch and cable/port held
constant, and it should try to reproduce F-3 and F-4 - a second occurrence of
F-3 with the same cable/port, and a capture with a host attached across the
reset, are what turn either from an anomaly into a diagnosis.

---

## D22 - 2026-09-28 - Post-H0 fixes - The five H0 findings, as changes

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13` ("feat: pre-hardware stuff"). The D21 session was
  uncommitted: `DEVELOPMENT_LOG.md` and `VERIFIED_STATE.md` modified,
  `data/evidence/P-H0-2026-09-28/` and `data/prereg/P-H0-2026-09-28.md` untracked.
  Nothing was committed and no unrelated work was touched.
- The five findings are D21's F-1, F-3, F-4, F-5 and F-6. Their code sites were
  read before editing (`platformio.ini`, `src/VehicleRuntime.cpp`/`.h`,
  `src/SerialTransport.h`, `src/bench/BenchConsole.h`, `lib/evcore/src/`
  `ControlScheduler.h`, `OperatorPolicy.h`, `OperatorResponse.h`, `RunConfig.h`)
  and the installed `renesas-ra` builder and Arduino core were inspected for the
  uploader and USB facts the fixes depend on.

**What changed**

1. **F-1 - the pre-upload touch is now part of the upload.** New
   `scripts/nano_upload_touch.py`, registered as `extra_scripts = post:` in
   `[env:nano_r4]` (inherited by `env:bench` and `env:bench-watchdog`, never by
   `env:native`). `post:` is load-bearing rather than stylistic: attaching an
   action to the `upload` target only works once that target exists, and a `pre:`
   script runs before the platform's builder creates it - SCons then binds the
   action to a same-named filesystem node that is never built, and the touch
   would silently never run. That was found by measuring, not by reading: a
   standalone SCons probe with the same ordering (`data/raw/scons_alias_probe/`,
   scratch and gitignored) shows the `pre:` ordering returning an `Entry` whose
   pre-action never fired and the `post:` ordering returning an `Alias` that did.
   The script therefore re-checks the node type at load time and prints a warning
   naming the cause if it ever stops being an `Alias`. Because it is attached to
   the `upload` target, it runs after a successful build and before `dfu-util`, so
   a failed build never touches the board. It finds the board's application-mode
   port (the board's own `build.hwids` cover `2341:0074` and
   `2341:0374`), opens it at 1200 bps with DTR and RTS low and closes it - the
   touch the core watches for (`CheckSerialReset()` -> `goBootloader()` on
   `bps == 1200 && !dtr`) - then waits up to 10 s for `dfu-util -l` to report
   `Found DFU`, retrying the touch once. It is a no-op when the bootloader is
   already present, when no port exists, and for any protocol other than `dfu`
   (so a `sam-ba` upload keeps the platform's own touch). `pio run -t upload` is
   therefore a complete instruction on this machine, as `AGENTS.md` says.
2. **F-3 - robustness at the upload boundary, and an honest record.** The cause
   is still **not determined** and no root cause is claimed. What the hook adds is
   a *classification* and a *race removal*: it distinguishes "no `VID_2341`
   device at all" (the F-3 state, with the exact remedy - a physical USB power
   cycle) from "a `VID_2341&PID_0374` device that `dfu-util` cannot use" (the F-2
   driver state, with the driver-package remedy), and the upload no longer starts
   `dfu-util` until the bootloader is actually enumerating, which removes the
   "touch, then race the enumeration" ordering from the sequence. Neither is
   evidence of a fix: F-3 may recur, and only a repeat session can say. The hook
   never fails the upload - `dfu-util` stays the authority on whether the device
   is there.
3. **F-4 - the boot line is now owed to a host that is actually reading.**
   `SerialTransport.h` gains `hostAttached()`, the core's own `connected()`
   predicate (the one `write()` gates on) - deliberately *not* `Serial.dtr()`,
   which flips the core's `ignore_dtr` and would make `write()` succeed into a
   void. `VehicleRuntime::serviceStagedText()` samples it at a bounded rate
   (`kHostPollPasses = 32`) and (a) re-arms `bootLinePending_` every time a host
   attaches, (b) stages the boot line only while a host is attached, so it cannot
   be committed to a port nothing is holding. `drainTelemetry()` also refuses to
   pop a record while no host is attached, so a staged-but-undeliverable record
   can no longer be mistaken for a record the queue still holds. The observable
   consequence: a host that attaches after a boot nobody was reading sees
   `BOOT,...` first, which is what the on-target captures below show. That is a
   behavioural change to what the H0 capture expects, so P-H0's Step 1 wording is
   updated to match. Note the deliberate limitation, established by those same
   captures: the attach signal is the CDC `connected()` predicate, and a detach is
   not always observed, so the greeting is guaranteed for the first attaching host
   only; a later attach may or may not be greeted. **Superseded in part by D23**,
   below, which stops the re-greeting from depending on an observed detach at all.
4. **F-5 - an out-of-range `config set` is refused, and stores nothing.** New
   `ev::validateSuppliedConfigFields()` (`OperatorPolicy.h`) checks only what one
   field can fail alone: a non-finite number, the rule ranges (rules 8.h, 8.i,
   8.j) and a real tournament level index. `VehicleRuntime::handleCommandLine()`
   runs it for `ConfigSet` before the command is counted as accepted, answers with
   the new `formatConfigSetRefusal()` - `ERR,cmd=config set,reason=ValueOutOfRange,
   error=<rule error>`, with no configuration echo, because an echo of the
   unchanged held state next to an error reads as "held" - and pushes a
   `CommandRejected` event carrying the rule error name. The announced grid and
   the cross-field geometry checks (Bottle Start before Target, Bottle Start fixed
   by the level) deliberately stay with `heldRunConfigVerdict()` at `config show`
   and `announce`: they need the level and the whole configuration.
5. **F-6 - the cadence readouts can answer "worst case".**
   `ControlScheduler` now keeps the session's extremes (`worstLateByMs()`, and
   `droppedDeadlines()` already accumulated) and `TelemetrySnapshot` carries them
   as `worstLateByMs`/`droppedDeadlinesTotal` alongside the last tick's own
   values. B1's `bench health` line reports them as `worst_late_ms` and
   `dropped_total`, next to the unchanged `late_ms`/`dropped`, which is what P-H0
   Step 7 asks for. The telemetry sample schema (`EVS1`) is deliberately
   unchanged: it is versioned, and this is a diagnostic readout.

**Files changed**

New: `scripts/nano_upload_touch.py`, plus two on-target captures in
`data/evidence/P-H0-2026-09-28/` (`d22-f1-upload-verify.log`,
`d22-f4-attach-observations.txt`; the third attach's raw output is
`d22-f4-attach3.txt`). Changed: `platformio.ini`,
`src/SerialTransport.h`, `src/VehicleRuntime.cpp`, `src/VehicleRuntime.h`,
`src/bench/BenchConsole.h`, `lib/evcore/src/ControlScheduler.h`,
`lib/evcore/src/OperatorPolicy.h`, `lib/evcore/src/OperatorResponse.h`,
`test/test_control_scheduler/test_main.cpp`, `test/test_operator_policy/test_main.cpp`,
`test/test_operator_response/test_main.cpp`,
`docs/bringup/P-H0-nano-upload-serial.md`, `docs/bringup/README.md` and this file.
No hardware fact was invented, no provisional value was promoted, no rule value
changed, and nothing was committed.

**Verification (code only - no board was touched in this entry)**

- `pio run`: SUCCESS, RAM 18.2% (5964 B), flash 25.4% (66612 B). No warnings.
- `pio run -e bench`: SUCCESS; `pio run -e bench-watchdog`: SUCCESS.
- `pio test -e native`: **510 cases, 510 succeeded** (four new cases: one
  scheduler, two policy, one response; the previous baseline was 506).
- `pio project config` shows `extra_scripts = post:scripts/nano_upload_touch.py`
  resolved for `env:nano_r4`, `env:bench` and `env:bench-watchdog`, and only
  `post:scripts/native_toolchain.py` for `env:native`.
- The attachment was confirmed **in this project's own environment graph**, not
  only in the probe: `NANO_TOUCH_REPORT=1 pio run` and
  `NANO_TOUCH_REPORT=1 pio run -e bench` each print
  `nano_touch: attached the touch to a Alias target` (the script's opt-in
  diagnostic line; unset, it prints nothing). No upload was attempted.
- The touch script's own control flow was exercised on the host without touching
  the board (`data/raw/nano_upload_touch_harness.py`, scratch and gitignored):
  the non-dfu protocol path is a no-op, a missing port prints guidance and does
  not raise, an already-present bootloader short-circuits the touch, and a
  registration that did not land on the `upload` alias is reported rather than
  swallowed. That run also confirmed the detection predicate against this
  machine's real state - with the board in application mode, `dfu-util -l` lists
  only the DFU *runtime* interface, so `Found DFU` is correctly absent.
- The `pre:` versus `post:` attachment question was settled by measurement, not
  by reading SCons: `data/raw/scons_alias_probe/` (scratch, gitignored) runs the
  same ordering with both forms and shows the `pre:`-ordered action attaching to
  an `Entry` and never firing, and the `post:`-ordered action attaching to an
  `Alias` and firing.

**On-target verification (same day, after the fixes - USB only, no motor supply)**

The board was connected in application mode (`COM3`, `USB VID:PID=2341:0074`,
`dfu-util -l` listing only the DFU runtime interface). Three uploads and three
late-attach captures were made with the fixed firmware. No other hardware was
connected and nothing was armed.

- **F-1: verified.** `pio run -t upload` was run three times. Each run's hook
  touched `COM3` at 1200 bps, detected the bootloader, and `dfu-util` flashed
  66612 bytes to `Download done.` / `Done!` with exit status 0 - **three of three,
  with no manual touch**, which is the thing this repository's documented upload
  instruction could not do before. Evidence: `d22-f1-upload-verify.log` (the last
  of the three; the two before it behaved identically). The observed detail worth
  keeping: the port tears down while the touch is being applied, so pyserial
  reports `OSError(22, 'A device which does not exist was specified.')` at that
  moment - the hook prints it and continues, and the board is in its bootloader
  immediately afterwards. Nothing in this repository should be read as claiming
  that message means the touch failed.
- **Image identity.** The flashed image is byte-identical to this tree's build:
  `sha256 .pio/build/nano_r4/firmware.bin` = `6cfc65dcebbe555304d65b9f18f4e6bd8e58efac45a89c5b275809369f082af2`
  before and after the last (comment-only) source edits, so the verified upload
  behaviour belongs to exactly this source.
- **F-4: verified for the case that failed, with one honest caveat.** Three
  captures attached to a board that had booted with **nobody reading** (each
  capture's first sample is `t_ms=2000, dt_ms=2000`, proving `setup()`'s bounded
  wait ran to its full 2000 ms). Attach 1 and attach 3 opened with
  `BOOT,fw=ev-nano-r4-p7b,schema=1,cadence_ms=20,...` as the **first line** -
  the loss D21 recorded is gone. Attach 2 did **not** receive one: the stream
  resumed mid-flight with current records, i.e. no attach transition was observed,
  so no greeting was re-armed. Why the detach between attach 1 and attach 2 was
  not seen is **not determined** (the port was closed by the capturing process;
  whether and when the host deasserted DTR was not measured). The claim is
  therefore stated narrowly in code, in `VERIFIED_STATE.md`, in the H0 protocol
  and here: the boot line is sent **once per boot, to the first host that
  attaches, whenever it attaches**; a greeting for every later attach is the
  code's intent and is not demonstrated. All three captures are recorded verbatim
  in `d22-f4-attach-observations.txt`, including the one that did not behave.

**Not verified (needs the board)**

- F-5: the logic is host-tested, but no board has echoed the new refusal yet. The
  repeat must record `config set distance=9.50` verbatim and then `config show` to
  show the held configuration did not change.
- F-6: the arithmetic is host-tested, but `worst_late_ms`/`dropped_total` have
  not been read from a board (B1 needs the bench image, which was not flashed in
  this session).
- F-3: unchanged - one occurrence, no root cause, one recovery (a physical power
  cycle). This session's three uploads all succeeded, which is a second data
  point but not a reproduction and not a fix.
- The boot-line re-greeting on a later attach (attach 2 above).
- Nothing about the vehicle: still no motor supply, no drivetrain, and no
  motion-related row changes.

**Next stage**

H0W stays blocked until H0 passes. The H0 repeat should be pre-registered (as
D21's "Next stage" requires), and it now has one extra thing to check: that a
plain `pio run -e bench -t upload` reaches the bootloader with no manual touch,
and that a reader attaching after boot is greeted with `BOOT,...`.

---

## D23 - 2026-09-28 - F-4 follow-up - The greeting no longer depends on an attach edge

**Baseline inspected before any edit**

- Branch `main`. The D21 and D22 work was uncommitted, as it still is; nothing was
  committed and no unrelated work was touched.
- The F-4 decision was read from the sources that own it before editing:
  `src/VehicleRuntime.cpp`/`.h` (`serviceStagedText()`, `drainTelemetry()`),
  `src/SerialTransport.h`, and the installed Arduino core's `usb/SerialUSB.cpp` and
  `tinyusb/class/cdc/cdc_device.{h,c}`. None of this is inference about the core:
  `tud_cdc_n_connected(0)` is `tud_ready() && (line_state & 1)` (`cdc_device.c`),
  i.e. **the DTR bit**, stored by `tud_cdc_line_state_cb` from the
  `SET_CONTROL_LINE_STATE` handler (`cdc_device.c`), which runs under
  `tud_task()` in the USB interrupt (`usb/USB.cpp`, `_usbfs_interrupt_handler`).
  `SerialUSB::connected()` is `tud_cdc_connected() || ignore_dtr`, and no file in
  `src/` calls `Serial.dtr()`, so `ignore_dtr` is never set and `connected()` is
  exactly DTR. `SerialUSB::write()` returns 0 when disconnected and otherwise
  loops until every byte is accepted - which is why the transport stages and
  pumps instead.

**The finding this addresses**

D22 re-armed the greeting on a host-attach **transition**, and recorded honestly
that one of three attaches showed no transition at all: the presence flag stayed
true, nothing was re-armed, and no greeting was sent. D23 states the cause as far
as the evidence supports: the only presence signal the firmware can see is the USB
CDC line state, and a detach is not guaranteed to appear in it - a host that
closes a port and reopens it for another reader process does not always deassert
DTR. The remedy therefore cannot be "notice the detach better"; the greeting has
to be re-owed on evidence that does not require one.

**What changed**

1. **The presence signal is both control lines, read without side effects.**
   `SerialTransport.h` replaces `hostAttached()` with `hostLineState()` ->
   `{dtr, rts}`. `dtr` is still the core's own `connected()`, the predicate
   `Serial.write()` gates on, so it keeps its exact meaning - "a staged line can
   move now". `rts` is read through `Serial.rts()`, a plain read of the value the
   line-state callback stored. `Serial.dtr()` is still never called, because
   reading it sets `ignore_dtr` and would make `write()` succeed into a void.
2. **`ev::HostGreetPolicy` - the greeting decision, host-tested.** New
   `lib/evcore/src/HostGreetPolicy.h`: pure logic over `(dtr, rts, nowMs)` that
   answers whether a greeting is owed. Three observations owe it - a control line
   that newly goes high (**either** bit), any byte from the host (which also
   restarts the interval, so a typed session is greeted once rather than once per
   command), and host silence at or past `kSilenceMs = 10000` (which repeats every
   interval, so the one attach no edge can report is bounded instead of removed).
   `VehicleRuntime` supplies the two inputs it cannot see - the sampled control
   lines and `nowMs` - and stages the line only while `dtr` is high.
3. **The greeting is callable: `boot`.** A new `boot` operator command re-sends the
   boot line on demand - parser, `operatorCommandName()`, the help table, and the
   inspection policy, so it is admissible in every state including mid-run. The
   answer *is* the boot line: `VehicleRuntime::stageBootLine()` is now shared by
   the automatic greeting and the command, so the two cannot drift apart.
   `serviceStagedText()` takes `nowMs` (the bench console passes its own).
4. **Nothing on the control path changed.** The greeting is still outside the
   control tick, still non-blocking, and still staged only when a write can
   actually leave; `drainTelemetry()`'s refusal to pop a record without a host is
   unchanged, and the `EVS1`/`EVE1` schema is untouched.

**Verified here**

- `pio run` succeeds: 5972 B RAM (18.2%), 66836 B flash (25.5%), no warnings.
  `pio run -e bench` and `-e bench-watchdog` succeed as well (6156 B RAM;
  71252 B and 71580 B flash). `pio test -e native`: **517/517** - the D22
  baseline's 510 plus 7 new cases in `test_host_greet_policy`.
- The image for this source:
  `sha256 .pio/build/nano_r4/firmware.bin` =
  `cfd288d455c2cf6c4d15f4b236827ee3e217f40ffe623b71ef301c265b38b788` (two
  clean rebuilds of the final source agree, so the hash identifies it).

**Not verified (needs the board)**

- **Nothing here was flashed.** The D22 upload was `6cfc65dc...` / 66612 bytes,
  which is no longer this tree's image, so D22's on-target F-4 observations belong
  to the D22 source. The new image's greeting behaviour on a late attach is
  **unobserved**.
- Specifically unobserved: the silence rule firing for a host that presents no
  attach edge, and `boot` typed at a board. The D22 captures were 4-6 s long,
  which is shorter than `kSilenceMs`, so a repeat has to capture for **longer than
  10 s** and record whether a `BOOT,...` line appears mid-capture. A capture whose
  first line is `BOOT,...` shows the attach edge, not the beacon, and the two must
  be recorded separately.
- The residual limit is unchanged and is now bounded rather than hidden: a host
  that presents no control-line edge **and** never writes cannot be told apart
  from the previous reader. The silence rule is what puts a number on that case.

**Next stage**

The H0 repeat keeps its D22 additions and gains two: a late-attach capture longer
than `kSilenceMs`, and `boot` typed verbatim alongside the other inspection
commands. Everything else about H0 - the pre-registered upload count, F-3, and
H0W behind it - is unchanged.

---

## D24 - 2026-09-28 - Documentation - The limitations register, and one claim worded stronger than the code

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13`. The D21-D23 work is uncommitted, as it was; no
  unrelated work was touched and nothing was committed.
- The limitations existed but were scattered: D21's findings F-1..F-6, D22's and
  D23's "Not verified" blocks, the caveats inside `VERIFIED_STATE.md` rows, the
  H0 session README's "What this session does NOT establish", and
  `SOFTWARE_ARCHITECTURE.md` sections 7 and 10. There was no single place to read
  what the project cannot yet claim.
- The greeting decision was re-read before writing it down, and that read is what
  produced the correction below: `lib/evcore/src/HostGreetPolicy.h`,
  `src/VehicleRuntime.cpp` (`serviceStagedText()`, the byte loop in
  `serviceSerial()`), and `test/test_host_greet_policy/test_main.cpp`.

**What changed**

1. **New `docs/limitations.md` - the limitations register.** One file, grouped:
   (A) nothing about the vehicle itself (rows #1-#12, plus the placeholder pin
   map, the uninterpreted driver fault line, the unknown encoder mechanics, the
   placeholder encoder pull-up, and the absent bottle mechanism); (B) implemented
   and host-tested but never observed on the board (F-5's refusal, F-6's B1
   extremes, the current image's greeting, the silence rule, `boot`); (C) known
   defects and unexplained behavior (F-3 with no root cause; the byte-rule wording
   corrected below; the greeting a typing no-edge reader postpones; the
   `Serial.dtr()` trap; Unity's disabled double assertions); (D) upload and
   toolchain limits on this machine (physical power cycle as the only F-3
   recovery, the `renesas.inf` install, the monitor must be closed, the expected
   `OSError(22)`, the hook deliberately never failing the upload, the single USB
   port and the single board); (E) the watchdog being completely unqualified;
   (F) placeholders and unbuilt enforcement points; (G) evidence and process
   limits. Every entry carries where the limit is evidenced and what would remove
   it, and the file states that it is a reading list, not evidence, and changes no
   `VERIFIED_STATE.md` row.
2. **Three records corrected, no code changed.** Three places said a byte from
   the host *owes* the greeting: the header comment in `HostGreetPolicy.h`, D23,
   and the `VERIFIED_STATE.md` boot-line row. The implemented and pinned behavior
   is that `observeHostByte()` only **restarts the silence interval**
   (`test_a_host_byte_restarts_the_silence_interval`, and the method's own
   comment), which is also the only reading consistent with D23's own next clause
   ("a typed session is greeted once rather than once per command" - owing on
   every byte would greet before every command). The header comment and the
   `VERIFIED_STATE.md` row were reworded to match the code and its test; D23 is
   append-only and is corrected here instead. The behavioral consequence is now
   recorded as a limitation rather than a property: a reader that presents no
   control-line edge **and** types continuously postpones the greeting by
   `kSilenceMs` per byte and may see no `BOOT,...` line at all while it types,
   with `boot` (or stopping for ten seconds) as the manual remedy.
3. **Pointers added so the register is not a dead end.** `VERIFIED_STATE.md`
   gains a paragraph naming `docs/limitations.md` beside its own rule, and its
   "How to promote a fact" list gains a step that narrows or deletes the matching
   limitation in the same change; `docs/bringup/README.md` gains the same step in
   "How a result is recorded".

**H0W's entry gate, adjudicated**

The H0W protocol's entry gate reads "H0 passed with reliable baseline upload,
reboot, and serial behavior", and this repository records the opposite (D21: H0
not passed, F-3 unexplained, 7/8 uploads; D22's "Next stage": H0W stays blocked
until H0 passes). The gate is not re-interpreted here: **the gate stands, and the
way to close it is the H0 repeat**, whose pre-registration must exist before its
first upload (D21's "Next stage", `data/prereg/README.md`). The repeat runs the
pre-registered upload series with the cable and port held constant, and it
carries the additions D22 and D23 asked for (a late-attach capture longer than
`kSilenceMs`, `boot` typed verbatim, F-5's refusal echoed by a board, B1's
`worst_late_ms`/`dropped_total`). Only if it passes is H0W started, at its own
Step 0.

**Files changed**

New: `docs/limitations.md`. Changed: `DEVELOPMENT_LOG.md`,
`VERIFIED_STATE.md`, `docs/bringup/README.md`, `lib/evcore/src/HostGreetPolicy.h`
(comments only), and the D23 note in `data/evidence/P-H0-2026-09-28/README.md`.
No behavior change, no configuration change, no provisional value promoted, no
`VERIFIED_STATE.md` row status changed, and nothing committed.

**Verification (code only - no board was touched in this entry)**

- `pio run`: SUCCESS, RAM 18.2% (5972 B), flash 25.5% (66836 B), no warnings.
- `sha256 .pio/build/nano_r4/firmware.bin` =
  `cfd288d455c2cf6c4d15f4b236827ee3e217f40ffe623b71ef301c265b38b788` - **the same
  hash D23 recorded**, which is the check that the `HostGreetPolicy.h` edit is
  comment-only: the image D23's statements belong to is still this source's image.
- `pio test -e native`: **517/517 succeeded**.

**Not verified (needs the board)**

- Everything in section B of `docs/limitations.md`, and every limitation whose
  owner is a later stage. This entry adds no hardware fact and removes none.
- The greeting limitation in item 2 above is **unobserved**: whether a typing
  reader actually misses the greeting on this board has never been measured, and
  it is on the H0 repeat's list rather than fixed.

---

## D25 - 2026-09-28 - H0 repeat - Upload, boot and serial link, pre-registered (passed)

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13`; the D21-D24 work is uncommitted, as it was. No
  firmware source and no configuration changed in this session: it is a
  measurement session, and its only new files are records and evidence.
- The pre-registration was written **before the first upload**
  (`data/prereg/P-H0-repeat-2026-09-28.md`, recorded at `2026-09-28T22:07:04-07:00`),
  which is the gap D21's retrospective file left open. 13 uploads were fixed in
  advance (10 vehicle, 3 bench), with a failed upload ending the series, plus the
  greeting/beacon capture rules, the F-5 refusal, the F-6 extremes read twice, and
  a >= 1500-tick soak.
- Why this session existed: H0W's entry gate requires a passed H0 and H0 was
  recorded as not passed (D21 F-3, 7/8 uploads). The operator chose to close the
  gate with the repeat rather than waive it.

**What was run**

USB only, no motor supply, wheels unloaded, board on `COM3`
(`USB VID:PID=2341:0074`, `SER=350D331C5A333131021133354B573462`). Vehicle image
`sha256=cfd288d4...` (66836 B), bench image `sha256=71f42e40...`. All captures were
written verbatim by `data/raw/h0repeat_serial.py` (scratch, gitignored), which
records when each `BOOT,...` line arrived and the observed telemetry spacing.

**What was observed**

- **14 uploads, 14 successes, no manual touch**: 10x `pio run -t upload`, 3x
  `pio run -e bench -t upload`, 1 restoring upload. Each showed
  `nano_touch: touching COM3 at 1200 bps (attempt 1/2)` -> `bootloader present;
  uploading` -> `Download done.`/`Done!` with exit status 0; the hook never needed
  its second attempt, and `COM3` re-enumerated with the same VID/PID/serial after
  every one. The expected `OSError(22, ...)` appeared on each touch. **F-3 did not
  recur**, and no attempt left the board non-enumerating.
- **The greeting is verified on the current source.** A host attaching to a board
  that had booted with nobody reading saw `BOOT,...` as its **first line in 7 of 7
  attaches** (six vehicle captures, one bench capture), and the **silence rule
  fired and repeated** at ~10.0 s cadence in every capture long enough to contain
  a beacon - the D23 behavior that had never been seen on hardware. The **byte
  rule is restart-only**: with commands at 1.55-10.02 s in one 26 s capture, no
  beacon appeared near the 10.06 s deadline the attach alone would have produced,
  and the next beacon arrived 10 s after the last host byte - which is the
  behavior D24 corrected the prose to, now observed. **`boot` was answered by the
  same line the greeting sends, verbatim.**
- **The boot line is a live status line**: beacons later in the same boot reported
  `stride=5` after `telemetry rate 10` was accepted, while the boot record at
  `t_ms=0` had reported `stride=1`.
- **F-5 is verified on target**: three identical rounds of `config set
  distance=9.50` answered
  `ERR,cmd=config set,reason=ValueOutOfRange,error=TargetDistanceOutOfRange`, and
  each following `config show` still reported `target_m=7.5000,...,rules=1`. The
  held configuration did not change - the opposite of D21's observation.
- **F-6 is answered**: two `bench health` readings 25.5 s apart both reported
  `worst_late_ms=1980,dropped_total=99` with `late_ms=0,dropped=0,violations=0`,
  so the extremes were readable **and flat** during 25.5 s of healthy 50 Hz
  telemetry with the host reading (`control_ticks` 773 -> 2046). They are the
  boot's bounded wait: every unhosted boot's first sample is `dt_ms=2000`,
  `t_ms=2000`.
- **The whole documented command set answered in shape**, and the malformed set
  (three identical rounds) was named 3/3 with `status` still `phase=Idle`,
  `motor=Disabled`, `authz=AuthorizedStop`; each rejection carried its `EVE1`
  `CommandRejected` event. The remaining verbs (`config clear`, `disarm`,
  `reset`, `status`, `config check`) were exercised in a second capture, so every
  verb in `help` has now been answered on a board.
- **Host stops reading**: after 20 s unhosted, reattaching opened with `BOOT,...`,
  flushed exactly 8 stale records (the lane capacity), then jumped 19.7 s to
  current records; `drop_s=365` and rising only while nobody read, `drop_e=0`.
- **Telemetry spacing**: 2305 consecutive 20 ms intervals at the default rate and
  180 intervals of exactly 100 ms after `rate 10`, `dt_ms=20` on all 2488 samples;
  `rate 100`/`rate 0.5` refused by name.
- **A bench-image finding**: on `env:bench` the greeting's byte input never fires.
  `src/bench/BenchMain.cpp` runs the console's read loop and the console forwards
  non-bench lines to `runtime.handleCommandLine(...)`
  (`src/bench/BenchConsole.h`) instead of running `VehicleRuntime::serviceSerial()`,
  where `observeHostByte()` is called. Observed as timing: a `bench health` typed
  at t=27.02 was followed by a beacon at 30.03, exactly 10.0 s after the previous
  one. Operator verbs still work in that image (the console forwards them). H0W
  runs the bench image, so a beacon there is evidence about the silence rule only.

**H0's decision rule**

Every pre-registered criterion was met. Two notes are recorded rather than
smoothed: the "attaching while the firmware already believed a host was attached"
condition was **not observed** this session (the protocol counts it neither way,
and D21 saw it once), and the cadence clause's extremes are **non-zero**, all of
it the bounded boot wait before the loop starts. **H0 passes**; F-3 stays an open
limitation (one occurrence ever, unreproduced in 14 uploads, cause undetermined).

**Files changed**

New: `data/prereg/P-H0-repeat-2026-09-28.md`,
`data/evidence/P-H0-repeat-2026-09-28/` (README plus eight captures). Changed:
`DEVELOPMENT_LOG.md`, `VERIFIED_STATE.md` (the upload row, the F-5 and F-6 rows,
and a new on-target section for this session), and `docs/limitations.md`. No
firmware source, no `platformio.ini`, no configuration value, and nothing
committed.

**Verification**

- `pio run` (before the session) SUCCESS, 5972 B RAM / 66836 B flash, no warnings;
  `sha256 .pio/build/nano_r4/firmware.bin` = `cfd288d4...` - the image the
  session's on-target claims belong to.
- `pio test -e native`: **517/517** (run in D24 on this same source; this session
  changed no source, and no test was skipped).
- The session's own gates: 10/10 and 3/3 uploads, 7/7 attaches greeted,
  beacons in five vehicle captures, F-5 refusal three times, extremes read twice.

**Not verified**

- **Nothing about the vehicle**: no motor supply, no driver, no encoder, no
  battery, no drivetrain. Rows #1-#12 are untouched, and no motor-output safety
  statement follows from a reset or from anything here (H1A owns that).
- **No root cause for F-3**, and the greeting's one counted-neither-way condition.
- **The watchdog**: the vehicle image ships it disabled and the bench capture is
  the disabled build. Nothing here says the watchdog starts, counts, refreshes or
  resets - that is H0W, which this session unblocks.

**Next stage**

H0W, per `docs/bringup/P-H0W-watchdog-qualification.md`, starting at its Step 0
with the now-closed gate. What this session hands it: the boot gap is ~1980 ms of
lateness and ~99 dropped deadlines (so a timeout opened before `setup()`'s wait
could not survive a boot), `first_refresh_ms` is still 0 in the disabled build so
the boot-to-first-refresh gap is Step 1's measurement, and B1's extremes must be
read from the bench image where the greeting's byte input does not fire.

---

## D26 - 2026-09-28 - H0W (Steps 0 and 1) - The prepared watchdog path opens and runs without a false reset

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13`. The entry gate was checked first and is
  satisfied: H0 passed in its pre-registered repeat (D25). The protocol's other
  prerequisites were checked before the first upload - `grep -rn "WDT" src/`
  matches only `src/WatchdogCheckpoint.h` (so there is exactly one refresh path),
  `arm-none-eabi-nm` over `.pio/build/nano_r4/firmware.elf` filters for
  `bench|hang` to **0** matches against **25** in the bench image (the vehicle
  image cannot contain the hook), and `kDeliberateHangAfterPasses` is at its
  shipped inert `0U`.
- `data/prereg/P-H0W-2026-09-28.md` was written before the Step 0 upload, fixing
  the margin (100 ms), the soak (60 s per load condition), the hang cycles (5),
  the recovery cycles (5), the hook value (500 passes) and the timeout rule
  (never the 250 ms placeholder).

**What was run**

USB only, **motor supply off and disconnected**. Two uploads, 60 s soaks, host
reading at the default rate, `bench health` at each end. `env:bench`
`sha256=71f42e40...` (watchdog disabled, Step 0) and `env:bench-watchdog`
`sha256=dc5a1bcb...` (enablement requested, Step 1). No header was edited to
enable anything: the define is `[env:bench-watchdog]`'s.

**What was observed**

- **Step 0 (disabled)**: `enable=0,open_attempted=1,opened=0,open_refused=0`,
  `refreshes=0`, `control_ticks` +2799 in 56 s (50.0/s), telemetry `t_ms`
  monotonic 2000 -> 65042, one boot, no resets, and the extremes
  `worst_late_ms=1980`/`dropped_total=99` identical at both ends.
- **Step 1 (enabled)**: **`opened=1`, `open_refused=0`** - the requested 250 ms was
  accepted - and **`refreshes == passes` exactly** (+2576103 each, `withheld=0`),
  i.e. one refresh per completed loop pass and no second refresh path. Zero
  unexplained resets over 60 s, `violations=0`, `late_ms=0`/`dropped=0` at both
  readings, and the extremes unchanged from the disabled baseline: enablement did
  not change the loop's health.
- **`first_refresh_ms=0`**: the boot-to-first-refresh gap is below a millisecond at
  `millis()` resolution, because the open is at the end of `setup()` after the
  bounded wait. The 50 ms placeholder is at least 50x too large.
- **The main loop is free-running**: ~46,000-47,600 iterations per second against
  exactly 50 control ticks per second, so `passes` counts iterations and the
  healthy refresh interval is one iteration (~21 us), not the 20 ms control
  period. What a timeout must survive is the gap before the first pass and a stall
  *inside* a pass. This is the measurement Step 6 needs, and it contradicts the
  assumption the placeholders were written with.
- **B1 has no worst-refresh-interval field**, as the pre-registration anticipated;
  `worst_late_ms` is used as the proxy and the missing field is recorded.
- **16 consecutive uploads with no manual touch** since F-3 (14 in D25, 2 here).

**What this is not**

Steps 2-5 (three load conditions, the deliberate hang, the recovery cycles), Step
6 (choosing the timeout) and Step 7 (re-running at it) have **not** run, and Step 8
(loaded-run re-validation) remains owed. H0W's acceptance rule has not been
exercised: in particular no hang has been produced, so "a deliberate main-loop
hang reliably causes a watchdog reset" is untested, the realized timeout period is
unmeasured, and the post-reset pin levels have not been recorded. **No row is
promoted from "partial".**

**Files changed**

New: `data/prereg/P-H0W-2026-09-28.md`,
`data/evidence/P-H0W-2026-09-28/` (README plus three captures). Changed: this file,
`VERIFIED_STATE.md` (one partial row) and `docs/limitations.md` (section E). No
firmware source changed, no configuration value changed, no timeout changed, the
hook was left at `0U`, and nothing was committed.

**Verification**

- Both uploads exit 0 with no manual touch (`h0w-uploads.log`).
- Soak arithmetic is in the captures: 3007-3008 samples, all 20 ms apart except the
  read-window jump; `control_ticks` deltas of 2799 over 56 s.
- No source changed in this entry, so the D24 verification stands: `pio run`
  SUCCESS with `sha256 cfd288d4...`, `pio test -e native` **517/517**.

**Next stage**

H0W Steps 2-5 at the enabled build, then the timeout choice, then a re-run at it -
done in D27.

---

## D27 - 2026-09-28 - H0W Steps 2-7 - The timeout is measured, the hang resets the board, and the numbers replace the placeholders

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13`; D21-D26 uncommitted, as they were. No motor
  supply, wheels unloaded, motor driver/encoder/battery disconnected throughout.
- Two gaps in the instruments were identified before the campaign, from the
  protocol's own wording: (a) H0W Step 2 asks for "the worst observed interval
  between consecutive refreshes" and B1 had no such field; (b) the hang hook's
  pass count had been sized in D26's pre-registration against the 20 ms control
  period, and D26 had just measured the loop at ~46,000 passes/s.

**What changed in the source (four edits, none of them behavioural)**

1. **`src/WatchdogCheckpoint.h` - `worstRefreshGapMs()`.** The longest interval
   between two consecutive refreshes, computed from the `nowMs` `endOfPass()`
   already receives: a comparison per pass, no added timer or peripheral read, and
   no change to when or whether the watchdog is refreshed. The first refresh is
   excluded (the boot gap has its own field, so the two numbers cannot hide each
   other), and the value is reset in `begin()`. Millisecond resolution, because
   the timeout is in hundreds of milliseconds and a sub-millisecond gap is
   honestly 0 rather than invented.
2. **`src/bench/BenchConsole.h` - the field is reported.** `worst_refresh_gap_ms`
   joins the B1 `bench health` line, next to `first_refresh_ms`, and the readout's
   header comment says which quantity it is and why it is not `worst_late_ms`.
3. **`src/WatchdogCheckpoint.h` - the placeholders are the measurements.**
   `kBootToFirstRefreshBudgetMs` 50 -> **0** (measured: `first_refresh_ms` reports
   0, i.e. sub-millisecond), `kWorstRefreshIntervalBudgetMs` 40 -> **14**
   (measured, identical across every load condition), `kWatchdogPlanMarginMs` 25 ->
   **100** (pre-registered in `data/prereg/P-H0W-2026-09-28.md` before any
   measurement existed), and `kWatchdogTimeoutMs` 250 -> **114**, which is
   `ev::requiredWatchdogTimeoutMs()`'s own answer (14 + 100). The compile-time
   assertion now applies the **whole** rule (`ev::timeoutIsQualified`, margin
   included, where it previously asserted only the two coverage clauses), plus a
   second assertion that the request equals the rule's requirement - so a
   hand-edited timeout cannot leave the measured margin behind.
4. **`src/bench/BenchMain.cpp` - the hang hook was set to 460000 for the campaign
   and returned to `0U`.** The comment now records the unit trap: a "pass" is one
   loop iteration at ~46,000/s, not a 20 ms control tick.

**What was run and observed (13 uploads, all exit 0, no manual touch)**

- **Step 2, three load conditions, 60 s each, enabled build:** telemetry off;
  the highest admissible rate (`telemetry rate 50` accepted as `stride=1`, and
  so identical to the default) with the host reading; the same rate with the host
  deliberately not reading. **`worst_refresh_gap_ms = 14` in every reading of
  every condition**, `late_ms=0`/`dropped=0`, `refreshes == passes`, `withheld=0`,
  `violations=0`, and **no reset in any of them** - one boot each, `t_ms` monotonic.
- **Step 3, the deliberate hang:** the hook's first campaign produced **8 hangs and
  8 resets in 95 s**, each cycle being `BENCH,hang=1,...`, the USB device vanishing
  171-344 ms later, a reconnect, `BOOT,...`, and ~10 s of healthy telemetry. The
  first attempt's capture died with `ClearCommError failed (PermissionError(13, ...))`
  - the reset seen from the host side - which is why the campaign uses a
  disconnect-surviving capture (`data/raw/h0w_cyclic_capture.py`, scratch).
- **Step 4, recovery:** **5/5 uploads into a board that was looping hang/reset**
  succeeded with no manual intervention and no manual bootloader entry, 8-9 s each;
  the 1200-bps touch reaches the board while the main loop is hung.
- **Step 5, under load:** the hang behaves the same with telemetry off
  (hangs at 1.36 s, 13.13 s, 24.80 s; device gone 235-297 ms later), and with the
  host not reading at all - re-attaching after 40 s found a fresh `EVE1 event=Boot`
  and `opened=1` with `refreshes == passes`, i.e. **the watchdog re-arms and resets
  with nobody watching**.
- **Step 6, the choice:** margin 100 (pre-registered), boot gap 0, worst refresh
  interval 14 -> **114 ms**, the rule's answer, not the placeholder.
- **Step 7, at 114 ms:** a 63 s healthy soak with `opened=1`, `open_refused=0`,
  `worst_refresh_gap_ms=14`, `refreshes == passes` and **no reset**; then **6 hangs
  and 6 resets** with hang -> device-gone latencies of **0-141 ms**, against
  171-344 ms at the 250 ms request. The latency scaling with the request is the
  attribution H0W asks for.
- **The hook is back at `0U`** in the tree and on the board, and the board was left
  running the vehicle image (`7fc8d3b5...`) with the watchdog disabled.

**Acceptance: everything met except one clause, and Step 8 owed**

No reset while healthy across four healthy conditions; a hung loop reset 8/8 times
at one timeout and 6/6 at another, well inside 3x; recovery 5/5 without hands; the
build and the 517 native tests pass. **The post-reset pin levels are NOT measured** -
no meter or logic analyzer was on the header - so H0W is **not recorded as passed**,
and Step 8's loaded-run re-validation remains owed. The chosen 114 ms is therefore
bench-qualified and provisional for the loaded run path.

**Files changed**

Source: `src/WatchdogCheckpoint.h`, `src/bench/BenchConsole.h`,
`src/bench/BenchMain.cpp` (hook value only). Records: this file,
`data/prereg/P-H0W-2026-09-28.md` (deviations and result),
`data/evidence/P-H0W-2026-09-28/` (README plus fourteen captures and their arrival
indexes), `VERIFIED_STATE.md` and `docs/limitations.md`. Nothing committed.

**Verification**

- `pio run`, `pio run -e bench`, `pio run -e bench-watchdog`: all SUCCESS, no
  warnings. Images: vehicle `7fc8d3b5...`, bench `14018e96...`, bench-watchdog
  `ba50cfb4...` (114 ms, hook `0U`); the campaign's hang image at 250 ms was
  `2566610c...` and at 114 ms `47813b0e...`.
- `pio test -e native`: **517/517**. The `WatchdogPlan` rule is host-tested and
  unchanged; the asserted constants are compile-time, so a build that violated the
  rule would not have produced an image at all.
- No test covers `worstRefreshGapMs()` directly (it lives in the Arduino layer,
  which host tests cannot compile, `test_build_src = no`). Its evidence is the
  on-target readout, which is recorded - and that absence is stated rather than
  papered over.

**Not verified / next stage**

- The post-reset pin levels (needs an instrument) and Step 8 (needs a loaded run).
- `kPlannedMaxDwellGapMs` stays `Provisional`: the capture gives `late_ms=0` for
every healthy tick and `worst_late_ms=1980` for the boot gap, so the honest bound
is still the boot gap and H0W did not produce a better one.
- H1A and any powered protocol are untouched: no driver, encoder or motor was
connected at any point in this entry.

---

## D28 - 2026-09-28 - H0W Steps 2/3 and 5 closed - the pins after a reset are read, the loop's worst stall is measured, and the dwell bound is promoted

**Baseline inspected before any edit**

- Branch `main`, HEAD `8e42d13`; D21-D27 uncommitted, as they were. Motor supply
  off and disconnected, drivetrain unloaded, nothing armed, nothing connected.
- D27's closing line said the post-reset pin levels "need an instrument", which
  left H0W's fifth acceptance clause open. Two gaps in the instruments were
  identified before any edit, both from the protocol's own wording:
  (a) Step 3 asks to record "the MCU pin levels `nSLEEP`/`PH`/`EN` after the
  restart" and no readout existed - `bench health` reports counters, not pins;
  (b) Step 2 and Step 5 ask for "the **maximum control-loop stall**", "the
  largest gap between two consecutive control-loop stamps", and B1 reported only
  `late_ms`/`worst_late_ms` - lateness against a deadline, which is a *different
  quantity* from the gap between two ticks.

**What changed in the source (four edits, none of them behavioural)**

1. **`lib/evcore/src/ControlScheduler.h` - `worstStallMs()`.** The longest
   interval between two *consecutive committed control ticks*, since `start()`.
   The first tick is **excluded**, by definition - a stall needs two ticks, and
   the interval before the first one is `dtMs`' own boot gap, which is a different
   fact and a much larger number (1980 ms, D25) that would otherwise dominate the
   field and make the real stall invisible. Reset in `start()` alongside the other
   session extremes. Deliberately separate from `worstLateByMs()`: lateness is
   measured against the deadline, a stall against the previous tick, and a passing
   tick can be late without the loop stalling.
2. **`src/VehicleRuntime.h`/`.cpp` and `src/bench/BenchConsole.h` - the value is
   reported.** `TelemetrySnapshot::worstStallMs` is filled from the scheduler in
   `runPass` and B1's line carries `worst_stall_ms`, next to `worst_late_ms`, with
   the readout's header comment saying why the two are not the same number.
3. **`src/MotorDriver.h` and `src/bench/BenchConsole.h` - `bench pins`.**
   `motorDriver::pinLevels()` reads the three driver inputs with
   `digitalRead()` -> `R_IOPORT_PinRead()` and pairs them with
   `lastDutyPermilleWritten()`, the duty this adapter last applied
   (`lastDutyPermilleWritten()` is set in `beginDisabled()`, `failSafe()`,
   `apply()` and `applyBench()`). `bench pins` prints them with
   `source=pin_read` and `deenergized=` from
   `ev::drv8874InputsAreDeEnergized()` - the same pure predicate that judges a
   command, applied to the pins instead of to the request. `lib/evcore/src/
   BenchCommand.h` gains `BenchVerb::Pins = 11` and its parse branch, and the
   `help` verb list gains `pins`.

   **A PWM duty cannot be read back from a pin.** `digitalRead` on the EN pin
   reports the level it is being held at, which at zero duty is LOW and above it
   is toggling. So the readout separates the two kinds of fact in the line itself:
   the levels are **observed**, the duty is **written**, and the header comment and
   the `source=` field say so. That distinction is the whole reason this can be
   recorded without claiming H1A's electrical measurement.
4. **`src/VehicleConfig.h` - `kPlannedMaxDwellGapMs` (40 ms) is replaced by
   `kMaxDwellGapMs = 28U`, with a citation.** The symbol's old comment said its
   honest value was "the measured maximum control-loop stall from H0W", and
   D27 could not supply one; `worst_stall_ms` can. A new `measuredGapMs(value,
   citation)` builder marks it `Measured`, `kMaxDwellGapEvidence` names the capture
   and the log entry, and both `NearZeroConfig` builders (`reversalStopVerification()`
   and `finalStopVerification()`) use it. The citation is not decoration:
   `Sourced::citesEvidence()` is what the `MeasuredValuesCiteEvidence`
   configuration check reads. The old symbol is gone from the tree, and the two
   bring-up documents that named it (`docs/bringup/P-H0W-watchdog-qualification.md`,
   `docs/bringup/P-H7-distance-braking-reversal.md`) now name the new one.

   **The promotion changed no verdict that matters, and nothing became armable.**
   `MeasuredValuesCiteEvidence` (`allPromotedValuesCiteEvidence()`) passed vacuously
   while every value was provisional and passes on the citation now - it bites only
   when a promoted value cites nothing, which is exactly the edit this avoids.
   `validateNearZeroConfig()` still refuses both near-zero configurations, because
   it requires the threshold, the dwell **and** the gap to be `Measured` and only
   the gap is, so `stopEvidenceLimitSpansDwell()` and the reversal/phase config
   validations still fail. The in-flight telemetry agrees: the D28 captures still
   report the first failing check as `check=ConfigurationValid`.

**What was run and observed (6 uploads, all exit 0, no manual touch)**

- **The pins, before and after a reset** (`env:bench-watchdog`, 114 ms, hook
  set): a capture that issues `bench pins` on every reconnect shows, per cycle,
  the hang, the device vanishing, the reconnect and the readout. In
  `h0w-step8-pins-after-reset.log`: **6 hang -> reset cycles and 7 readings - the
  first before any hang, then 6/6 after a reset** - and every one of them read
  `nsleep_high=0,phase_high=0,enable_high=0,duty_permille_written=0,deenergized=1`.
  The final hang campaign on the current source adds 3 further post-reset reads
  with the same values (`h0w-step7-final-hang.log`), for **9 observed post-reset
  reads, all the de-energized combination**.
- **The stall, per load condition** (`h0w-step8-pins-baseline.log`,
  `h0w-step8-stall-loads.log`, `h0w-step8-stall-unread.log`): telemetry off and
  the highest admissible rate with the host reading both reported
  **`worst_stall_ms=21`**, the host-not-reading condition **22**, and a 57 s
  baseline soak **28** at both ends of the file. The value does not drift with the
  load, and the worst of it stayed within 1.4x of the 20 ms period the loop is
  asked for.
- **The final soak on the current source** (`h0w-step7-final-soak.log`, hook
  `0U`): 45 s, `opened=1`, `open_refused=0`, `refreshes == passes`,
  **no reset**, `worst_refresh_gap_ms=15` (14 in the earlier campaigns - the
  quantity is alive, not pinned at a constant) and `worst_stall_ms=27`.
- **The final hang on the current source**: 3 hangs, 3 resets, the device gone
  **47-78 ms** after each `BENCH,hang=1` line, each followed by a reconnect and a
  de-energized pin readout.

**The pin clause: met, and why this reverses D27's wording**

The protocol is explicit about what the clause is, and D27 (and D26's record)
read it more strictly than it is written:

- the header lists the protocol's second class as "**E** observation of the MCU
  pin levels (motor supply off; motor disconnected)";
- Step 3 asks for "the MCU pin levels `nSLEEP`/`PH`/`EN` after the restart, which
  must be the de-energized combination the firmware initializes to (**levels
  only**; H1A turns this into a statement about the driver outputs)";
- the scope limit says "the pin levels the firmware drives on reset are
  observable here, but what those levels do at the driver outputs is a different
  fact and is measured electrically in H1A".

The clause is therefore about the levels the *firmware drives*, which is exactly
what `bench pins` reads, and the electrical statement is H1A's by the protocol's
own construction. D27's "no instrument was on the header" was stricter than the
clause; this entry corrects it by appending rather than rewriting, as D24 did for
D23's greeting wording. The statements this supersedes, each left in place as the
record of what was believed before D28: D27's "Not verified / next stage" line
("The post-reset pin levels (needs an instrument)") and its acceptance paragraph
("H0W is **not recorded as passed**"), the same wording in the H0W evidence README's
scope paragraph and clause table, the `VERIFIED_STATE.md` H0W row, and
`docs/limitations.md` section E.

**What the readout is, and what it is not.** It is `digitalRead()` of the same
three pins the firmware drives, i.e. the MCU's own view. It establishes that the
reset re-runs initialization to the de-energized combination; it says **nothing
about the driver outputs**, which H1A owns. Two limits travel with it:

1. it is the firmware's own instrument, not an independent one: a pin driven low
   but shorted high at the header would still read as the MCU sees it;
2. every observed reset followed a **drive-free** hang - the motor supply is off
   in this protocol by design - so "a reset taken *while an effort is commanded*
   returns to de-energized" is **not** established here and cannot be.

**The hook slip, and its correction**

The upload log's last block, labelled "final: hook back to `0U`", uploaded an
image (`858a5a32...`) that **still contained `kDeliberateHangAfterPasses = 460000U`**:
the source edit had not been made before the build, and the build cannot see that -
it compiled exactly what it was given. It was found by reading the tree afterwards,
not by the build or the tests. `src/bench/BenchMain.cpp` now genuinely says `0U`,
the rebuild is `ddba3b9d...`, and an appended `!!! CORRECTION` block in
`data/evidence/P-H0W-2026-09-28/h0w-uploads.log` records what was uploaded versus
what the log claimed. The vehicle upload in that same block is unaffected: the
vehicle image cannot contain the hook (`src/main.cpp` asserts the define is absent
and the bench tree is not compiled there).

**Acceptance: H0W's rule is met, as a bench qualification**

All five clauses of the protocol's acceptance rule now have recorded evidence:
`opened()` true at the chosen timeout; zero resets while healthy across four load
conditions; a hung loop reset 8/8 times at 250 ms and 6/6 at 114 ms with the
latency scaling with the request; **the pin levels after restart are the
MCU-driven de-energized combination, 9/9 observations**; and 5/5 (plus the D28
campaign's) uploads into a looping board with no hands. **Step 8 is not one of the
acceptance clauses** - the protocol pre-registers it as "owed, not done here" - so
H0W is recorded as **passed for the bench load**. The chosen **114 ms remains a
bench number**, provisional for the loaded run path until Step 8 is repeated
there, and `kMaxDwellGapMs = 28` is likewise a bench measurement to re-take then.

**The cadence margin against the policy values**

H0W's "if it passes" list asks for the measured cadence margin to be recorded as
evidence for the cadence policy values, so it was checked against them. The
observed interval between healthy ticks is the requested 20 ms (`dt_ms=20`,
`late_ms=0`, `dropped=0` in every healthy reading) and the worst stall was 28 ms,
which sits inside `kMaxSampleAgeMs = 60` - the longest age a control-path sample
may have, three periods - with room to spare, and far inside
`kStopEvidenceMaxAgeMs = 3200`. **No policy value was contradicted, so none was
changed.** That is a consistency check, not a validation: the estimator window
(`kEstimatorWindowMs = 20`) and the no-edge timeout (`kEstimatorNoEdgeTimeoutMs =
100`) remain policy, because no window has been shown adequate for this encoder -
that needs an encoder and H5/H10, not a bench soak with nothing attached.

**Files changed**

Source: `lib/evcore/src/ControlScheduler.h`, `lib/evcore/src/BenchCommand.h`,
`src/VehicleRuntime.h`, `src/VehicleRuntime.cpp`, `src/MotorDriver.h`,
`src/VehicleConfig.h`, `src/bench/BenchConsole.h`, `src/bench/BenchMain.cpp` (the
hook back to `0U`). Tests: `test/test_control_scheduler/test_main.cpp` (one new
case), `test/test_bench_command/test_main.cpp` (the `pins` verb). Records: this
file, `data/evidence/P-H0W-2026-09-28/` (README, six captures and their arrival
indexes), `data/prereg/P-H0W-2026-09-28.md` (two deviations and the result),
`VERIFIED_STATE.md`, `docs/limitations.md`, and the symbol rename plus the
consistency pass in `docs/bringup/P-H0W-watchdog-qualification.md`,
`docs/bringup/P-H7-distance-braking-reversal.md`, `docs/bringup/README.md`,
`SOFTWARE_ARCHITECTURE.md` and `README.md`. Nothing committed.

**Verification**

- `pio run`, `pio run -e bench`, `pio run -e bench-watchdog`: SUCCESS, no
  warnings. Images on disk: vehicle `eb440b31...` (67020 B), bench `1f775f89...`,
  bench-watchdog (114 ms, hook `0U`) `ddba3b9d...`.
- `pio test -e native`: **518/518**, including the new
  `test_the_stall_extreme_is_the_gap_between_consecutive_ticks`, which pins that a
  later smaller stall does not lower the extreme and that the first tick is
  excluded.
- `worstStallMs()` is host-tested (the scheduler is pure); `pinLevels()` and
  `worstRefreshGapMs()` are not - they live in the Arduino layer, which host tests
  cannot compile (`test_build_src = no`) - so their evidence is the on-target
  readouts above, and that absence is stated rather than papered over.

**Not verified / next stage**

- **Step 8** (loaded-run re-validation): needs a run path, which does not exist.
  Until it exists the timeout and the dwell bound are bench numbers.
- **The driver outputs**: whether the pins' levels de-energize the DRV8874 is
  H1A's electrical measurement, and no statement about motor-output safety is
  recorded here.
- **A reset while driving**: never observed, and not observable with the motor
  supply off (see above).
- **The realized timeout period**: still bounded, not clocked - the timer rounds
  the request up and the latency measured includes the host noticing a vanished
  device.
- H1A and any powered protocol are untouched: no driver, encoder or motor was
  connected at any point in this entry.

---

## D29 - 2026-09-29 - Repository - The D21-D28 span is committed and pushed, as four scoped commits

**Objective**

Leave the tree recoverable. Everything from D21 to D28 - the five H0 fixes, the
H0 repeat, the whole H0W campaign, the new instruments and the records that
describe them - was working tree only, and `docs/limitations.md` listed that as a
limit: a lost checkout would have taken the evidence with it. Commit it, and get
it onto `origin/main`.

**What was done**

The span was first committed as a single commit (`7a928dc`) with the subject
`feat: H0` and pushed. That subject named one of the eight days the commit
carried, so it was rewritten as four, split by what a reader would come looking
for:

- `002b215` - `fix: enter the Nano R4 bootloader before upload (H0 finding F-1)`:
  `platformio.ini` and `scripts/nano_upload_touch.py`.
- `354d8b1` - `feat: watchdog qualification, bench diagnostics and the H0 operator
  fixes (D22-D28)`: `src/`, `lib/evcore/src/`, `test/`.
- `eb41d8e` - `docs: record H0 and H0W, and add the limitations register
  (D21-D28)`: this file, `VERIFIED_STATE.md`, `SOFTWARE_ARCHITECTURE.md`,
  `README.md`, `docs/`.
- `b70cd56` - `data: H0/H0W captures and their pre-registrations (D21-D28)`:
  `data/evidence/`, `data/prereg/`.

**The rewrite changed no content**, and that is checkable rather than asserted.
The commit was unwound with `git reset --soft 8e42d13` plus an unstage - which
touches HEAD and the index and never the worktree - and the index was then
compared against `7a928dc` (`git add -A`, then
`git diff --cached 7a928dc --stat`), which was empty: all 86 files byte-identical
to the tree the campaign was validated against. After the four commits
`git diff 7a928dc HEAD` was empty again. Same tree, different scissors.

**Nothing recorded depended on the discarded hash.** Every record that names a
build identity names `8e42d13` - the commit *below* the work - not `7a928dc`; a
grep for `7a928dc` across the tree returns nothing. The rewrite therefore
invalidated no caption in the evidence, which is why the records are left as they
are.

**Push.** The remote was at `a595118` (P5), so the first push fast-forwarded it
over P7, P8, `8e42d13` and `feat: H0`. Rewriting an already-pushed commit then
needs a force, which was done as `--force-with-lease` against `7a928dc` - so the
push would have been refused if any other push had landed in the meantime.
`7a928dc` no longer exists on `main`.

**One thing found on the way.** `core.autocrlf=true` and there is no
`.gitattributes`, so every text blob is stored with LF however it arrived -
including the serial captures, which arrive CRLF (`git ls-files --eol` reports
`i/lf w/crlf` for them, while the worktree files are still the originals). It is
recorded in `docs/limitations.md` section G rather than fixed, because
`data/** -text` plus a re-add is a repository-wide decision.

**Files changed**

`docs/limitations.md` - the "uncommitted" bullet is replaced by what the commit
does and does not buy, and the line-ending bullet is added - and this entry.
Markdown only; no source, test, configuration or evidence file was touched.

**Verification**

- `git diff 7a928dc HEAD`: empty. `git status --short`: empty.
- `pio run`, `pio run -e bench`, `pio run -e bench-watchdog`: SUCCESS. Images on
  disk hash exactly as D28 left them - vehicle `eb440b31...`, bench `1f775f89...`,
  bench-watchdog `ddba3b9d...` - which is the evidence that this entry's edits
  are markdown only.
- `pio test -e native`: **518/518**.
- `git ls-remote origin refs/heads/main` equals local `HEAD`.

**What this supersedes**

The `Nothing committed` footer and the "uncommitted, as they were" notes inside
D21-D28 describe those sessions accurately and are left alone; from this entry
on, the D21-D28 span is committed. The one claim that had gone stale - the
`docs/limitations.md` bullet above - is updated here.

**Not verified / next stage**

- **H0W Step 8** is still owed and still blocked: it needs a loaded run path.
- **H1A** is the next stage, and it was not started.
- The board was not touched in this entry: no upload, no serial session, no motor
  supply, and no hardware connected.

---

## D30 - 2026-09-29 - H0W Steps 3 and 7 closed - the reset is attributed by the silicon, the realized period is measured, and Step 8's criteria are written down

**Objective**

Two H0W clauses were being met by inference rather than measurement, and one of them
is the clause the protocol exists for:

- Step 3 asks for the restart to be attributed to the watchdog, and adds "if a
  reset-cause register is readable on this core, record it; do not assume one
  exists". D27/D28 attributed resets by the latency scaling with the request and by
  there being a single refresh path. That is inference.
- Step 7 asks for the **realized** period to be recorded, and Step 1 warns that the
  requested timeout is not it. D28 recorded the period as "bounded, not clocked".

The board was available for one last session, so both were measured instead of
carried forward.

**Baseline inspected before any edit**

- Branch `main`, HEAD `48598ea` (the D29 commit); working tree clean.
- Board on `COM3` (`USB VID:PID=2341:0074`); motor supply off and disconnected. No
  driver, encoder, motor or battery was connected at any point in this entry.
- `src/WatchdogCheckpoint.h`: `kWatchdogTimeoutMs = 114`, enablement false in the
  vehicle image; `src/bench/BenchMain.cpp` hook at `0U`.

**What was added**

Four things, all **bench-only** - they live in `src/bench/`, which `env:nano_r4` does
not compile, and the vehicle image contains none of their symbols:

- `src/bench/ResetCause.h` and `bench resetsrc [clear]`: reads
  `R_SYSTEM->RSTSR0`/`RSTSR1` and decodes the masks the RA4M1 device header defines,
  reporting the raw values alongside the flags. `clear` writes zero to both,
  which is how this family's reset flags are cleared, and the write is
  self-verifying - the readout after it shows whether it took effect.
- `src/bench/WatchdogRegisters.h` and `bench wdt`: reads `WDTCR` (TOPS/CKS/RPES/RPSS,
  with the vendor's masks), `WDTSR` (the live `CNTVAL` down-counter and its two
  flags), and `pclkbHz()` from the FSP's own clock accessor. It deliberately does
  **not** decode a period from those fields: that mapping is hardware-manual
  knowledge, not a header this build can see, so the record decodes against the
  vendor's own `wdt_timeout_t`/`wdt_clock_division_t` enumerations instead.
- `wdt_realized_ms`, a field on B1's health line: the WDT library's own computed
  period (`WDTimer::getTimeout()`), which returns 0 when the timer was never opened.
- Two verbs in `lib/evcore/src/BenchCommand.h` (`ResetCause`, `Wdt`), with a host
  test pinning that they are matched by their whole names, so `bench reset` (the
  drive latch) and `bench resetsrc` cannot alias.

**The property that had to be found before any reading meant anything: the flags latch**

The first capture read, on a boot that followed nothing but an upload:

```
BENCH,resetsrc=1,...,rstsr0=1,rstsr1=6,porf=1,wdtrf=1,swrf=1,...
```

`wdtrf=1` on an ordinary boot, and not because that upload set it: it was a leftover
from the D27/D28 hang campaign that had survived every later upload, including the
D30 upload of a different instrument. Two consecutive plain reads returned identical
values (the read is passive), and `bench resetsrc clear` returned `rstsr0=0,
rstsr1=0`, confirmed by the next read - so the clear works, and without it **no
reading can attribute a *new* reset**. That is why the instrument has one.

The control that completed the decode was a genuine power cycle (the operator
unplugged and replugged the board): `rstsr0=1, rstsr1=0` - `porf=1` and nothing else.
**A power-on reset clears RSTSR1**; software resets clear nothing.

| reset source | `rstsr0` | `rstsr1` | decoded |
|---|---|---|---|
| power-on (USB unplugged and replugged) | 1 | 0 | `porf=1`, nothing else |
| upload / DFU (a software reset) | 0 | 4 | `swrf=1`, nothing else |
| deliberate hang (the watchdog) | 0 | 2 | `wdtrf=1`, nothing else |

**The attribution, measured**

Cleared to zero, then 70 s of deliberate hangs (hook `460000`): **5 hangs, 5 resets,
and every post-reset read was `rstsr1=2`** - `wdtrf=1` with `porf=0`, `swrf=0`,
`iwdtrf=0` and no bus, parity or security flag - with the device vanishing and
re-enumerating each time. The reset that follows a hung loop is the watchdog's as the
silicon reports it, not merely as the latency suggested.

**The realized period, measured**

```
BENCH,wdt=1,...,wdtcr=13139,tops=3,cks=5,rpes=3,rpss=3,wdtsr=16371,cntval=16371,
     underflow=0,refresh_error=0,pclkb_hz=24000000,wdt_realized_ms=174
```

- `tops=3` -> 16384 cycles and `cks=5` -> /256, per the vendor's `wdt_timeout_t` and
  `wdt_clock_division_t`. Those enumerations were checked **entry by entry** against
  the WDT library's own lookup tables, because a mismatch there would have made the
  library's computed period wrong; they agree (index 5 is /256 and index 15 is /128
  in both).
- `pclkb_hz=24000000` -> **16384 x 256 / 24 MHz = 174.76 ms**, and `wdt_realized_ms`
  reports the same 174 from the library. The live counter reads 16370-16371 against
  that 16384 reload, across two boots and a power cycle.
- **The request is 114 ms.** The margin the rule applied (100 ms over the worst
  measured refresh gap of 14 ms) was therefore applied to the **request**, and the
  realized margin is **160 ms**. The timeout value is unchanged: the rule chooses it,
  and this rounding only lengthens the period.

A correction to the host-side number, recorded because it changes how the earlier
campaigns should be read: the capture helper reads 4096 bytes or times out, so its
reset latency is quantized by ~0.2 s - the same order as the period itself. A 62 ms
hang-to-device-gone gap was observed in the same run that reported a 174 ms realized
period, because a hang marker read late makes the gap look short. The D27/D28
latencies (171-344 ms and 0-141 ms) still show what they were used for - the latency
*scales with the request* - but they never clocked a period, and any record that read
them as a duration is corrected here.

**Two findings the audit turned up while writing this down**

- **The `BOOT,...` line is not evidence of a restart in a bench image.** The same
  line is re-sent as the silence beacon every `kSilenceMs`: a 20 s capture showed
  three `BOOT` lines against one connect, and the 70 s hang capture showed 12 `BOOT`
  lines but only 7 `EVE1,...,event=Boot` events for 5 hangs. Step 3's clause listed
  the `BOOT` line first; the protocol now says the primary evidence is the device
  vanishing and re-enumerating, the `EVE1` boot event, `t_ms` restarting near zero
  and the counters restarting - and that the beacon does not re-send `EVE1`, which is
  what makes that line the usable one.
- **`src/WatchdogCheckpoint.h` contradicted both itself and the record.** Its header
  still said the watchdog "has never been observed on this target: H0W ... has not
  been run", and that `kWatchdogTimeoutMs` "is UNVERIFIED, and the two gaps ... are
  placeholders too (no measurement of either exists yet)" - while the constants
  directly below it were D27's measurements. Corrected to what is true: H0W has been
  run and recorded for the **bench** load, the timeout is provisional for a loaded
  run, and the numbers are measurements. The header also said the realized period
  comes from the reset latency; it now says it comes from the registers and the
  library, with the reason.

**Step 8, written down**

Step 8 requires its criteria to be written down *now*, while the trial is owed.
`data/prereg/P-H0W-step8-2026-09-29.md` now holds the loaded conditions, the 60 s
soaks, the unchanged 100 ms margin, 5 hang cycles and 5 recovery uploads, the bench
numbers the trial is compared against (including the realized 174 ms), and what
would make the timeout change. The protocol's Step 8 points at it.

**Files changed**

Source: `src/bench/ResetCause.h` (new), `src/bench/WatchdogRegisters.h` (new),
`src/bench/BenchConsole.h` (two readout lines, a verb dispatch, the
realized-period field on B1, the help list), `src/bench/BenchMain.cpp` (the hook,
used and restored), `src/WatchdogCheckpoint.h` (comments only),
`lib/evcore/src/BenchCommand.h` (two verbs). Tests:
`test/test_bench_command/test_main.cpp` (the new verbs and the `reset`/`resetsrc`
aliasing case). Records: this file, the H0W evidence README and its upload ledger,
both pre-registrations, `VERIFIED_STATE.md`, `docs/limitations.md`, and
`docs/bringup/P-H0W-watchdog-qualification.md`.

**Verification**

- `pio run`, `pio run -e bench`, `pio run -e bench-watchdog`: SUCCESS, no warnings.
  Images: vehicle `eb440b31...` (unchanged from D28 through every D30 build), bench
  `d627ab9f...`, bench-watchdog `9e1281b5...` (hook `0U`).
- **The `WatchdogCheckpoint.h` edit changed no code**: all three images hash
  identically before and after it, which is how a comments-only change to a header
  compiled into the vehicle image is shown to have altered nothing.
- `pio test -e native`: **519/519**, including the new verb case.
- `arm-none-eabi-nm`: the vehicle image still matches nothing for `bench|hang` and
  nothing for the new `resetCause|resetsrc` symbols; the bench image matches both.
- On target: read/clear/read agrees with itself; 5/5 hang resets attributed to
  `wdtrf` alone; `bench wdt` reproduced across two boots and a power cycle.

**Not verified / next stage**

- **H0W Step 8** is still owed, and is now pre-registered rather than only named. It
  cannot run until H7/H10 produce a loaded run path.
- **The driver outputs** remain H1A's electrical measurement. The reset-cause
  register says which reset happened; it says nothing about whether the vehicle was
  safe when it did.
- **A reset while an effort is commanded** was not observed here either - every hang
  was drive-free, as the protocol requires.
- **The board was left running the vehicle image** (`eb440b31`, watchdog disabled)
  with the hook's source value back at `0U`.

## D31 - 2026-09-29 - Review - the widest line in the firmware had no bound, and the build was not pinned

A review of the whole tree, asked for with the instruction to look outside the
usual places. Six findings, four of which are changes here; the session then tried
to verify them on target and **the board was taken away mid-session**, which is
recorded below rather than glossed.

**Finding 1 - the bench readouts had no worst-case bound, and losing one was silent.**

Every consumer of the one serial line buffer states its worst case against that
buffer except the bench console: `ev::kOperatorResponseWorstCaseChars`,
`kTelemetryLineCapacity`, and `BoundedText.h`'s own note naming 418 characters for
a telemetry sample and 443 for the configuration-check report.
`benchConfig::kReadoutLineCapacity` was simply `ev::kSerialLineCapacity` with no
derivation behind it. Derived now, field by field from `healthLine()` itself, the
widest readout's worst case is **534 characters** - every `uint32` field at ten
digits, which the free-running counters really can reach (`passes` reaches ten
digits after roughly 26 h at the measured ~46,000 passes/s, `last_refresh_ms` at
the `millis()` wrap) - against a capacity of **448**. That is **86 characters
over**, and the widest line any capture has actually contained is 389, so the
gap is not reachable in a bench session but is reachable in the limit.

The failure it would have produced is the one this console must not have:
`respond()` drops a readout that does not fit **whole** (deliberately - a truncated
line hides its tail, and the tail is where a diagnosis lives) and counts it in
`detail::lostResponses()`, and `responses_lost` is a **field of the health line**.
So a health line too long to fit would have taken its own evidence of the loss
off the wire, and the interface would have looked healthy while going silent.

Fixed: `stageLostReadoutNotice()` composes a constant, 36-character
`BENCH,lost=1,reason=ReadoutDidNotFit` into the same buffer a dropped readout
vacated, so a loss is visible on the wire. It is not routed through `respond()` -
the drop is already counted there, and a notice that could itself be dropped would
be no notice. The owed fix is still to **split `health`** into its watchdog/loop
half and its console/interface half; that is a schema change, it is recorded with
its number in `BenchConsole.h` and in `docs/limitations.md`, and it was not made
here. Also removed: a stray comment on `kReadoutLineCapacity` that described the
bench trigger readout's *hold time*, which is a different fact living with the
trigger readout.

**Finding 2 - dead code in `src/WatchdogCheckpoint.h`.** `hasTimeToFirstRefresh()`
had no callers in `src/`, `lib/` or `test/`, and `bootMs_` was written in
`begin()` and never read by anything. Both removed, and `begin()`'s comment -
which claimed it "records the boot instant" - corrected to describe what it does:
clear the counters, the refresh history, and the refresh interval's start. The two
instants the class is actually asked about are the open (`openedAtMs_`) and the
first refresh (`timeToFirstRefreshMs_`); a third that nothing reads is a place for
a stale value to look like a measurement.

**Finding 3 - the platform was not pinned.** `platform = renesas-ra` resolved to
whatever the registry offered on the day of the build, so the tree was not
reproducible and a future build could compile a different Arduino core under the
same `platformio.ini`. Now `platform = renesas-ra@1.9.0`, which is what this
target was built and tested with; a platform release declares the package versions
it needs, so pinning it pins the framework (1.6.0 here) with it. The reason it
matters more than usual on this target is recorded in the comment: the Renesas FSP
ships as a **prebuilt** `libfsp.a` inside the core's variant, with no C sources in
the tree, so its reset and clock setup cannot be read from source at all - the
version pin is the only record of what the firmware was built against, and
`src/bench/ResetCause.h` depends on that. `env:native` stays unpinned on purpose,
and says why.

**Finding 4 - checked and refuted, not a defect.** The two entry points' loop
orders differ on inspection, so the equivalence was verified rather than assumed.
`src/main.cpp` runs `runPass -> endOfPass -> serviceSerial -> updateStatusLed`;
`serviceSerial` is `serviceStagedText` + the byte read + `handleCommandLine` +
`drainTelemetry`. `src/bench/BenchMain.cpp` runs `beginPass -> runPass ->
endOfPass -> serviceStagedText -> console read -> applyOutput -> drainTelemetry ->
updateStatusLed -> serviceDeliberateHang`, which is the same relative order with
the bench instruments around it. The one substantive difference is real and
already recorded: the bench console's own reader never calls
`greeter_.observeHostByte()`, so the byte-restarts-the-silence-interval rule is
inert on the bench image (`VERIFIED_STATE.md`, D25). No change.

**Finding 5 - one claim checked against the library, and it holds.**
`healthLine()` reports `wdt_realized_ms` from `WDT.getTimeout()` and claims the
read "is a pure member read and touches no timer register". Read:
`libraries/WDT/src/WDT.cpp` returns the `_timeout` member and 0 when the timer was
never opened, and `_timeout` is set in `getReload()` to the selected
reload/prescaler pair's ceiling - so it is the realized period, it is not a
register read, and "0 when not opened" is true. It agrees with D30's independent
register reading (174 ms). No change.

**Finding 6 - `docs/limitations.md` had two `## C.` sections and two F-3 entries.**
`## C. Known defects and unexplained behavior` appeared **twice**, and the
second one carried a **stale duplicate of F-3** that disagreed with the first:
it said "D22's three uploads all succeeded, which is a second data point" and
assigned ownership to "the H0 repeat and H0W Step 4", where the current copy has
the 17-successful-upload tally and names H0W Step 4's recovery cycles. Two copies
of one known defect, one of them out of date, is the register failing at the one
thing it exists for. The duplicate heading and the stale entry are removed; the
remaining items (the postponed greeting, `Serial.dtr()`, Unity's doubles) were
numbered 2-4 in the second section and keep those numbers, so the merged section
reads 1-4 with one F-3. No content was lost - the removed copy's every fact is in
the kept one.

**The sweep for overstated claims is not exhaustive and is not claimed to be.**
The codebase uses "never"/"must not" pervasively as design-contract language,
which is not the defect class being hunted; the class that matters is a comment
asserting an observation the project does not have, and D30 corrected the last
known instance of that in `WatchdogCheckpoint.h`. A full pass over every
`never`-claim in `src/`, `lib/` and `docs/` was **not** performed and remains owed.

**Files changed**

Source: `src/bench/BenchConsole.h` (the lost-readout notice, the derived bound and
its comment, the stray comment removed), `src/WatchdogCheckpoint.h` (both dead
members removed, one comment corrected), `platformio.ini` (the pin, with the
rationale). Records: this file, `VERIFIED_STATE.md` (one row),
`docs/limitations.md` (the readout bound, the pin, the handoff, and the duplicated
section C merged), `data/evidence/D31-2026-09-29/` (new: the README and the one
capture). No file under `lib/` was touched, which is why the host-test count is
unchanged at 519.

**Verification**

- `pio run`, `pio run -e bench`, `pio run -e bench-watchdog`: SUCCESS, no warnings,
  with the pinned platform.
- **The vehicle image hash changed**: `eb440b31...` -> `91f74a04...` (67020 B,
  the same size). This is *not* a comment-only change - the
  `WatchdogCheckpoint.h` edit removes a member and a store and is compiled into
  the vehicle image - so the D28-D30 "identical hash proves the edit changed no
  code" argument does not apply here and is not made. The behavior is unchanged
  for the same reason the member was dead: nothing read it.
- Bench `b6b1c455...` (72924 B), bench-watchdog `271e8eba...` (73252 B).
- `pio test -e native`: **519/519**, unchanged. No test was added, and none can be: the notice and the bounded readouts live in the ARM-only bench tree (`test_build_src = no`).
- `arm-none-eabi-nm`: the rebuilt vehicle image still matches nothing for
  `bench|hang` and nothing for `resetCause|resetsrc`; the bench image matches 31
  and 1. The source-split guarantee is unchanged.
- **Not verified: anything about the notice path.** No test exercises
  `stageLostReadoutNotice()` (it is in the ARM-only bench tree, so a host test
  cannot reach it) and no on-target readout ever contained it - the line that
  would trigger it is 86 characters from being reachable. It is reviewed code,
  not verified code, and is recorded as such.

**On target - the session that was cut short**

- Before changing anything, 13.0 s of the **pre-change vehicle image** was
  captured: `t_ms` continuous `1521430 -> 1534430`, `dt_ms=20`, `phase=Idle`,
  `armed=0`, `motor=Disabled`, `effort_permille=0` on all 651 `EVS1` samples, board
  up 25.4 min. It also corroborates D30's `BOOT`-line caveat independently: one
  `BOOT,...` line arrived mid-stream at 9.699 s between samples whose `t_ms` are
  20 ms apart, with **zero `EVE1` lines** and no reset. Committed as
  `data/evidence/D31-2026-09-29/d31-prechange-vehicle.log`.
- `pio run -e bench -t upload` SUCCESS (`nano_touch` -> `Download done.` ->
  `Done!`), and then **the board disappeared**: the capture of that image's
  readouts raised `ClearCommError ... PermissionError(13, 'The device does not
  recognize the command.')` on its first read and the operator reported the device
  gone. So **the D31 bench image's readouts were never observed**, and no statement
  rests on them.
- **The board was left holding the bench image** (`b6b1c455...`), not the vehicle
  image, and can no longer be reflashed by this project. That is a change from
  every previous handoff and it is safety-relevant - the bench image contains
  `bench drive`, a command that applies effort outside a run - so it is recorded
  in `data/evidence/D31-2026-09-29/README.md` and in `docs/limitations.md`.

**Not verified / next stage**

- **Any D31 source on target.** The changed vehicle image was never flashed; the
  changed bench image was flashed and its readouts were lost with the port.
- **`stageLostReadoutNotice()` on hardware**, and therefore the claim that a
  dropped readout is visible. Unowned until there is hardware again.
- **Splitting `health`** is owed and unowned. With the board gone this is a
  maintenance fix, not a bring-up fix: nothing can be measured against it.
- **The full overstated-claim sweep** (the paragraph above Finding 5) is owed.
- **H0W Step 8, H1A's driver-output measurement, and every loaded-run protocol
  are now permanently blocked on hardware.** They are not deferred; they are
  unperformable with the equipment this project has, and no amount of further
  source work changes that.

---
## D32 - 2026-10-03 - Configuration - The external DRV8874 nFAULT signal is not installed, and the firmware now says so

**The fact this entry records**

The assembled vehicle does **not** connect the DRV8874's `nFAULT` output to the
Nano. The pin the firmware had planned for it (`kMotorFaultBar`, D4) is therefore
not an input at all. This is a statement by the team about the vehicle's wiring,
**not a measurement**: nobody put a meter on anything, no polarity was ever
established, and no fabricated measurement was introduced to make the build pass.

**Why the previous state was wrong**

The firmware had been built around the assumption that the line would eventually
be measured. Two consequences followed:

1. `DriverFaultPolarityVerified` (the configuration-build check) required a
   `Measured` polarity before the vehicle could be armed. On a vehicle with no
   line that requirement is **impossible to satisfy** - there is nothing to
   measure - so production arming was blocked forever by a fact about a signal the
   vehicle does not have. The check was not reporting a missing measurement; it
   was asserting a requirement that did not apply.
2. `SensorInputs::sample()` read D4, and `VehicleRuntime` incremented
   `cyclesWithDriverFaultLow_` from that level, so a floating pin's toggles were
   recorded as driver-fault activity - a fabricated measurement rather than a
   missing one.

The deeper problem was the modelling, not the value: "which level means fault" and
"is there a line to read" are different facts, and the code had only the first.

**The design**

A capability type, separate from the polarity type:

```cpp
enum class DriverFaultCapability : uint8_t {
    Installed = 0,     // the line exists; polarity must be measured before arming
    NotInstalled = 1,  // the line does not exist; polarity verification is not applicable
};
```

`Installed` is the **zero value** on purpose: a forgotten assignment leaves the
line required and unverified, which refuses arming. The unsafe direction - a build
that silently drops the external fault gate because a field was never set - is not
reachable by default.

**Validation semantics** (`ev::validateDriverFaultConfig(capability, polarity)`):

| Capability | Polarity | Verdict |
|---|---|---|
| `Installed` | `Measured` + `ActiveLow`/`ActiveHigh` | pass |
| `Installed` | `Unverified` (or `Measured` + `Unverified`) | refuse: `PolarityNotVerified` |
| `NotInstalled` | any value | pass - verification is **not applicable** |

**What "NotInstalled" is, and is not**

It is a wiring fact stated by the team for this build, not a measurement, and it
is deliberately **not** a `Sourced` value: there is no `ValueSource` for "stated",
and marking it `Measured` would have been the exact fabrication this project's
rules exist to prevent. `kPlannedDriverFaultPolarity` stays `Unverified`; it was
**not** relabelled. The DRV8874's on-chip overcurrent, thermal and undervoltage
protections are untouched - what is absent is only the MCU's ability to observe a
fault the driver reports.

**Runtime behavior on this vehicle**

- `SensorInputs::begin()` does not configure `kMotorFaultBar`, and `sample()` does
  not `digitalRead` it. The pin is left alone rather than pulled up so something
  can still be read.
- `RawSample` carries the capability with the level, so a consumer cannot mistake
  "not installed" for "read high".
- `cyclesWithDriverFaultLow_` is incremented only when the capability is
  `Installed`. On this vehicle it stays zero - which is **not** evidence of a
  healthy driver, which is why the capability travels beside it.
- `ev::observedDriverFault(capability, levelLow, polarity)` returns
  `FaultCode::None` unconditionally when `NotInstalled`, and is unchanged when
  `Installed`: the same call still refuses to claim a fault from an unverified
  polarity and still returns `FaultCode::DriverFaultBar` once the polarity is
  measured.

**Telemetry (schema version 1 -> 2)**

A sample gained `nfault` (`Installed` / `NotInstalled`) next to `nfault_low`, and
`nfault_low` is no longer always a boolean: when the line is not installed it
carries the schema's absent marker `-`. A `0` would have read as "sampled and
high", i.e. as a healthy driver - the exact claim this vehicle cannot make. The
parser refuses a record that says the line is installed with no level, and refuses
one that says it is not installed while carrying a level. `VERSION` now reports
`telemetry_schema=2`, and `EVS2`/`EVE2` prefixes make an old log unmistakable.
The bench `bench inputs` readout reports the same two fields.

**Configuration check rename**

`DriverFaultPolarityVerified` -> `DriverFaultConfigurationValid`. A column named
"polarity verified" would have claimed a measurement that does not exist on this
vehicle; the capability-aware name reports what is actually required. It is a
schema-visible rename and it travelled with the version bump, so no version-1 log
can be read as if it carried the new name. Its position in the check ordering is
unchanged, so no other check's `firstFailure()` ordering moved.

**Why the design is the one chosen**

1. **Capability, not a deleted gate.** Deleting the check would have made a
   vehicle that *does* wire the line armable with an uninterpreted fault input -
   the exact hazard the check exists for. The gate is now conditioned on a fact
   instead of removed.
2. **The polarity value was not touched.** It stays `Unverified`/`Provisional`.
   Nothing was promoted, and no citation was invented.
3. **The pin is not sampled, not merely ignored.** Reading D4 and discarding the
   value would still be a firmware that samples a signal it does not have; leaving
   it unconfigured is the honest version.
4. **A capability type, not a `bool`.** The codebase's configuration style is
   strongly typed and documented; a bare `bool driverFaultWired` would have been a
   second vocabulary for a fact that already had one.

**Files changed**

- `lib/evcore/src/DriverFault.h` - `DriverFaultCapability`, its name table,
  capability-aware `validateDriverFaultConfig(capability, polarity)`, and
  `observedDriverFault(capability, levelLow, polarity)`. The polarity-level
  predicate `driverFaultLevelIndicatesFault()` is unchanged.
- `lib/evcore/src/ConfigurationChecks.h` - the check id renamed to
  `DriverFaultConfigurationValid`, with the rename documented in the header.
- `lib/evcore/src/TelemetryRecord.h` - `kTelemetrySchemaVersion = 2`, prefixes
  `EVS2`/`EVE2`, `TelemetrySample::driverFaultCapability`, and
  `telemetryDriverFaultLevelName()`.
- `lib/evcore/src/TelemetryFormat.h` - writes `nfault` then `nfault_low`;
  worst-case sample bound 418 -> 438 characters, re-derived in the comment.
- `lib/evcore/src/OperatorResponse.h` - CHECK worst case 443 -> 445.
- `src/VehicleConfig.h` - `kPlannedDriverFaultCapability = NotInstalled`,
  `plannedDriverFaultCapability()`, and `configurationBuildChecks()` reading the
  capability-aware validation. No calibration value changed.
- `src/SensorInputs.h` - the pin is configured and sampled only when the line is
  installed; `RawSample` carries the capability.
- `src/PinMap.h` - `kMotorFaultBar` documented as a spare on this build.
- `src/VehicleRuntime.h` / `.cpp` - the guarded counter, the capability-aware
  `observedDriverFault` call, and the snapshot/sample fields.
- `src/bench/BenchConsole.h` - `bench inputs` reports `nfault` and an absent
  level rather than a floating pin's level.
- `test/test_driver_fault/test_main.cpp` - new, 8 cases.
- `test/test_telemetry_format/test_main.cpp`, `test/test_operator_response/`,
  `test/test_parse.py`, `test/test_metrics.py`, `test/test_cli.py`,
  `test/test_schema_mirror.py` (unchanged logic, new vocabulary),
  `tools/telemetry/evtelemetry/{schema,parse,metrics,report,synthetic}.py`,
  regenerated `tools/telemetry/samples/*.csv`.
- `README.md`, `SOFTWARE_ARCHITECTURE.md`, `VERIFIED_STATE.md`,
  `docs/limitations.md`, `docs/hardware/motor-driver-drv8874.md`,
  `docs/bringup/P-H1B-sense-inputs.md`, `docs/bringup/P-H3-first-low-duty-drive.md`,
  `docs/bringup/P-H9-fault-injection.md` - superseding notes; historical
  pre-registration text was preserved and annotated rather than rewritten.

**Verification**

- `pio test -e native`: **529/529** (was 519; `test_driver_fault` adds 8, and the
  two new telemetry cases add 2). `test_driver_fault` pins installed+verified,
  installed+unverified, not-installed+unverified, the conservative default, the
  vocabulary, and that not-installed can never observe a fault from either level -
  including when the polarity happens to be verified, which is the case a check
  that forgot to consult the capability would let through.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry`:
  **116/116**, including the schema-mirror drift check that re-reads the firmware
  headers.
- `pio run -e nano_r4`, `pio run -e bench`: SUCCESS with no warnings. The vehicle
  image hash changed, as expected - this is a code change, not a comment.
- **Not verified: anything on hardware.** The board is gone (`docs/limitations.md`)
  and no physical test was performed or is possible. Every row in
  `VERIFIED_STATE.md` keeps the status it had; the wiring fact above is stated,
  not promoted.

**Remaining production blockers (unchanged by this work)**

`ConfigurationValid`, `CalibrationMeasured`, `SafetyLimitsValid`,
`ControllerConfigValid`, `ProfileLimitsValid`, `PhaseConfigValid`,
`ReversalConfigValid` and `RunPolicyUsable` are all still false with the shipped
provisional configuration. `DriverFaultConfigurationValid` is the one check this
work changes, and it changes by making the requirement match the hardware rather
than by weakening it. Nothing here was done to "make production pass", and no
other gate was touched.

---
## D33 - 2026-10-03 - Configuration - The pin map is standardized to the stated V5 harness

**The fact this entry records**

The team reports the vehicle is standardized on the reviewed V5 wiring map. That
is a statement about the intended/as-built harness, **not a measurement**: no
continuity check has been performed, row #1 of `VERIFIED_STATE.md` stays
UNVERIFIED, and nothing here is promoted.

| Role | Previous planned pin | V5 pin |
|---|---|---|
| Motor encoder A | D2 | D2 (unchanged), interrupt channel 0 |
| Motor encoder B | D3 | **D4** (level read only; no interrupt channel in this core's table) |
| DRV8874 PH | D9 | **D8** (plain digital output) |
| DRV8874 EN/PWM | D6 | **D9** (the firmware's only PWM consumer) |
| DRV8874 nSLEEP | D7 | **D10** |
| DRV8874 nFAULT | D4 (spare; line not installed) | **no pin** (`kNoPin`); capability stays `NotInstalled` |
| Start trigger | D10 | **D12** |
| DRV8874 PMODE | - | not MCU-controlled; tied to GND in the harness |

**No pin-role conflicts.** D4 is no longer any kind of nFAULT input (it is
encoder B, and the nFAULT line has no pin at all - `kMotorFaultBar` is the
explicit `kNoPin` sentinel, with a `static_assert` in `src/SensorInputs.h` that
refuses a build with `kPlannedDriverFaultCapability == Installed` and no pin
behind it). D10 is no longer the trigger (it is nSLEEP), D12 is the trigger, D9
is the PWM/EN output and D8 is a plain PH output.

**Framework capability check** (installed Renesas core 1.9.0,
`variants/NANOR4/`, from the framework's own pin table): D2 = P105 carries
`PIN_INTERRUPT|CHANNEL_0` (encoder A interrupt valid); D4 = P103 has no
interrupt entry (fine for the level-only B read, and a warning that an interrupt
attached there would silently attach nothing); D9 = P303 carries PWM channel
7-B (valid `analogWrite`); D8 = P304, D10 = P112 and D12 = P110 are plain
digital I/O. This is a framework-table reading, not a board measurement.

**Deliberately unchanged:** the `DriverFaultCapability::NotInstalled` design
(`lib/evcore/src/DriverFault.h` untouched); every calibration value and its
provenance (`src/VehicleConfig.h` untouched); the PH/EN plan
(`lib/evcore/src/MotorDriverPlan.h` untouched); the temporary 300-permille bench
ceiling; all production safety/configuration gates. Battery and current sensing
remain not installed: their constants stay planned raw-count placeholders (no
conversion, no hardware presence claimed).

**Files changed:** `src/PinMap.h` (the map), `src/SensorInputs.h` (the
compile-time guard and the not-installed wording), `docs/limitations.md`,
`VERIFIED_STATE.md`, `SOFTWARE_ARCHITECTURE.md`, `README.md`,
`docs/bringup/P-H1A-driver-electrical.md`, `docs/bringup/P-H1B-sense-inputs.md`,
`docs/hardware/motor-driver-drv8874.md`, `DEVELOPMENT_LOG.md` (this entry).

**Verification** (host/build only - the board is gone, no physical test was
performed or possible):

- `pio test -e native`: **529/529**.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry`:
  **116/116** (without `-t tools/telemetry` the package-relative imports do not
  resolve).
- `pio run -e nano_r4`: SUCCESS, clean rebuild, no warnings (67152 B).
- `pio run -e bench`: SUCCESS, no warnings (74004 B).
- `pio run -e bench-watchdog`: SUCCESS, no warnings (73432 B).

**Not verified / next stage**

- **Nothing on hardware.** No continuity check, no idle levels, no driver truth
  table; the map is what H1A step A0 would verify. Production and the bench
  images compile the same `src/PinMap.h`, so both now carry the V5 map; neither
  was uploaded (the board is not available).

---

## D34 - 2026-10-03 - Production closure - five audit findings, and the run now
plans its own time

Phase 13 (production competition-readiness closure). Bench bring-up is done
(motor moves under bench control, both directions tested, the encoder counts, the
trigger works), so the work is software closure against the current tree. Five
findings were re-checked against the tree first; all five were still present.
Nothing was promoted: no value was relabelled `Measured`, and no calibration
number was invented.

**1. The final stop's dwell was unvalidatable.** `validateNearZeroConfig()`
required `ValueSource::Measured` for all three of its values, while the run's
final stop is the event's Complete Stop (rule 8.p) and arrives as `RuleDerived` -
so with an otherwise complete calibration the run policy could never be usable and
no run could start. Fixed in `NearZeroVerifier.h`:
`dwellProvenanceIsEstablished()` accepts a measured dwell **or** a `RuleDerived`
dwell that cites its rule (`citesEvidence()`), and the dwell gets its own refusal
(`NearZeroConfigError::DwellIsNotEstablished`). The threshold and the update-gap
limit are still measured-only. The dwell was **not** relabelled `Measured`, and an
*uncited* rule value is refused exactly like a placeholder.

**2. The bottle phases could never exit.**
`bottleContactDetected`/`captureComplete`/`pushComplete` were hard-coded `false` in
the vehicle layer, so `Approach` could only end in `PhaseTimeout`. Implemented in
the one place that can honestly produce it: `lib/evcore/src/BottleEvidence.h`
derives the three facts from the run-axis position and the announced geometry -
contact at a configured lead before the Bottle Start Point (rule 7.c.ii.(5)),
capture complete after a configured advance beyond contact, push complete at the
turn-around position (the Target Point plus the announced Bottle Line Distance,
rule 8.j) plus a configured margin. **This is a deliberate deviation from section
14's wording**, recorded in that section and in the module header: the mechanism
exists and has no detection of its own, so the inference is implemented with the
claim stated plainly - it is the firmware's position-derived *belief*, every
distance must be `Measured` before a run may be announced, and the bottle's real
behavior still has to be checked against an independent observation (P-H8) before
`VERIFIED_STATE.md` #11 may be promoted. No sensor was invented and no sensor
evidence is faked.

**3. Target Time did nothing.** It was stored, validated and echoed, and no motion
path read it. Now the announcement boundary plans the run:
`ev::planRunTiming()` (`MotionProfile.h`) turns the announced Target Time, the
announced geometry (the whole course: out to the turn-around and back) and the
measured limits into a **planned cruise** - the slowest cruise whose estimated
travel fits the announced time, never above the configured/safety ceiling - and a
**deliberate delay** that spends the slack the run cannot spend by travelling,
bounded by the policy's maximum delay and by the configured maximum run duration.
An announced time shorter than the fastest admissible travel is **refused**
(`AnnounceRefusal::TargetTimeUnreachable`, with the plan's own status as the
detail) rather than met by exceeding a limit. The plan reaches the control path
through `runProfileLimits_` (the per-run cruise) and
`CompetitionStateMachine::plannedDelayS()` (the OptionalDelay phase), so a
changing Target Time changes the commanded motion. The Target Time and the
geometry remain operator inputs (`config set`), never source constants.

**4. Both legs used the forward brake offset.** `profileLimitsForOneDirection()`
now takes the direction and selects `kPlannedForwardBrakeOffsetM` or
`kPlannedReverseBrakeOffsetM`; the per-run planned-cruise builder
(`profileLimitsWithCruise()`) does the same.

**5. A ramp-down was a dead stop.** `RunMotionCommand::RampDown*` was mapped
straight onto a non-powered `Disabled` request, so the profile's computed
deceleration ramp was never commanded and the "ramp-down" was a coast. The vehicle
layer now keeps regulating toward the shaped target for a ramp
(`ev::isRunRampDownCommand()`), while `Brake*` still maps to the deliberate
non-powered hold and every fault/stop path still fails to `Disabled`. The
regulator's one-sided effort limit keeps a ramp from ever asking for powered effort
the other way.

**Operator workflow (no recompilation).** `reset` -> `config set level=<...>
distance=<m> time=<s> bottle_line=<m>` -> `config check` -> `announce` -> `arm` ->
physical trigger -> run -> `status`. `announce` now answers with the plan:
`OK,cmd=announce,plan=Planned,cruise_mps=...,delay_s=...,total_s=...,margin_s=...`.

**Configuration gates.** No new check id (`ConfigurationCheckId` and the uint16
mask are unchanged) and no telemetry schema change: the new values live in
`RunPhaseConfig` (minimum planned cruise, the three bottle distances) and are
validated by `validateRunPhaseConfig()`, so `PhaseConfigValid`/`RunPolicyUsable`
still refuse arming while they are placeholders. The maximum deliberate delay is
RuleDerived (the rule's maximum Target Time).

**New placeholders (all Provisional, none promoted):**
`kPlannedMinimumCruiseMps = 0.10`,
`kPlannedBottleContactLeadM = 0.0`, `kPlannedBottleCaptureAdvanceM = 0.0`,
`kPlannedBottlePushMarginM = 0.0`. The Phase-14 procedure that establishes them,
and every other value, is `docs/bringup/P-H14-calibration-checklist.md`.

**Files changed:** `lib/evcore/src/NearZeroVerifier.h`, `MotionProfile.h`,
`RunAxis.h`, `BottleEvidence.h` (new), `CompetitionStateMachine.h`,
`OperatorPolicy.h`, `OperatorResponse.h`; `src/VehicleConfig.h`,
`src/VehicleRuntime.h`, `src/VehicleRuntime.cpp`; `SOFTWARE_ARCHITECTURE.md`
(sections 13 and 14), `docs/limitations.md`,
`docs/bringup/P-H14-calibration-checklist.md` (new), the test fixtures
(`test_competition_state`, `test_motion_chain`, `test_scenario`,
`test_near_zero_verifier`, `test_operator_policy`, `test_operator_response`) and
two new suites (`test_bottle_evidence`, `test_run_timing`), and this entry.

**Not verified / next stage.** Nothing on hardware: every number above is still a
placeholder, `checks` still reports `all=0`, and the plan's arithmetic has never
been compared with a realised Run Time. The first physical step is step 0 of
`docs/bringup/P-H14-calibration-checklist.md` (continuity and the pin map), then
step 2 (command polarity), which is what unlocks the rest.

---

## D35 - 2026-10-03 - Post-tournament cleanup - emergency removed, D32-D34 repaired and rebuilt

**Preservation first.** The dirty post-tournament tree was snapshotted before any
cleanup, as required. `git switch -c recovery/d32-d34-v5-snapshot`, `git add -A`
(staged 57 files; `.pio/`, `data/raw/`, `.freebuff/`, `__pycache__/` verified
unstaged via ignore rules), commit `2b1f45c5aa4f644670335eeb781a0ba1bd6684ad`
`snapshot: working tree at post-tournament audit (D32-D34 + emergency image)`.
Cleanup branch `cleanup/d35-post-tournament` was created from that snapshot; all
work below is on it. `main` untouched, nothing pushed, nothing uploaded. D34 had
been **unverified** until this cleanup/rebuild: it did not compile and had no
green host run.

**1. Emergency image removed (tournament-only, recoverable from the snapshot).**
Deleted `src/emergency/EmergencyConfig.h`, `src/emergency/EmergencyRun.h`,
`src/emergency/EmergencyMain.cpp` (whole `src/emergency/` directory) and
`test/test_emergency_run/test_main.cpp` (whole suite). Removed `[env:emergency]`
plus its comment block and `build_src_filter = +<emergency/> +<EncoderInput.cpp>`
/ `build_flags = -DEV_EMERGENCY_IMAGE=1` from `platformio.ini`; the `env:native`
comment block now directly precedes `[env:native]`. `pio run -e emergency` now
reports unknown env. Added a defensive guard in `src/main.cpp` refusing
`EV_EMERGENCY_IMAGE`/`DEV_EMERGENCY_IMAGE` in the vehicle image, mirroring the
H1A guard. No emergency env, source, test, or define remains in the active tree.

**2. Compile fix (smallest correct).** `src/VehicleConfig.h`
`runTimingPlanInputs()` took `double announcedTargetTimeS`, shadowing the helper
`announcedTargetTimeS(double)` it calls on the next line, so
`announcedTargetTimeS(announcedTargetTimeS)` could not compile. Renamed the
parameter to `announcedTargetTimeSValue` and call
`announcedTargetTimeS(announcedTargetTimeSValue)`. D34 Target-Time planning
semantics unchanged.

**3. Rule-derived optional-delay gate.** `maximumOptionalDelayS` is legitimately
`RuleDerived` (rule 8.i maximum Target Time, cited), but
`validateRunPhaseConfig()` required `isMeasured()`, so `PhaseConfigValid` could
never pass. Added `optionalDelayProvenanceIsEstablished()` in
`lib/evcore/src/CompetitionStateMachine.h`, mirroring
`dwellProvenanceIsEstablished()` for the final-stop dwell: Measured passes;
cited RuleDerived passes; Provisional or uncited RuleDerived fails. No value
relabeled Measured. Regression tests in `test/test_competition_state`:
`test_cited_rule_derived_optional_delay_passes` and
`test_uncited_and_provisional_optional_delay_fail` (plus measured still passes).

**4. Derived-cruise provenance (choice documented).** `plannedCruiseValue()` had
labeled the computed timing-plan cruise `Measured`, which is false: it is
arithmetic from measured inputs, not a vehicle measurement. Chose the dedicated
`ValueSource::Derived = 3` solution (minimal, explicit): added `Derived` plus
`isDerived()` in `lib/evcore/src/Units.h` (`citesEvidence()` already requires a
citation for it); added `cruiseProvenanceIsEstablished()` in
`lib/evcore/src/MotionProfile.h` (Measured or cited Derived) and made only the
cruise field of `validateMotionProfileLimits()` use it - every other limit stays
Measured-only, and `planRunTiming()` inputs still require Measured (a Derived
input is refused). `src/VehicleConfig.h::plannedCruiseValue()` now returns
`Derived` with the derivation cited. The alternative (keeping plan outputs
outside `Sourced<T>`) would have retyped `MotionProfileLimits` and every caller;
Derived keeps the provenance in the type with a two-line predicate. Regression
tests in `test/test_run_timing`: `test_a_computed_cruise_is_derived_never_measured`,
`test_profile_limits_accept_a_cited_derived_cruise_only`,
`test_plan_inputs_still_require_measured_limits`. No calibration gate weakened:
shipped `profileLimits()` cruise is still Provisional, so production still
refuses to arm.

**5. Hygiene.** Split the merged comment in
`lib/evcore/src/ConfigurationChecks.h` (`failureMask() ... for.` now ends its
line; the frozen-vocabulary bullet starts its own). No other formatting.

**6. Bench ceiling.** Reverted the temporary tournament override in
`src/bench/BenchConsole.h` from 300 back to `kMaxEffortPermille = 150U`. No test
or documented requirement needs 300; production ceiling
`kPlannedMaxEffortPermille` untouched. H1A `bench h1a-brake` retained: strictly
bench-only (`src/bench/` + `EV_BENCH_H1A_DIAGNOSTIC` guard), bounded
(`kH1aHoldMaxMs = 30000U`, expiry on pass clock, `bench stop`/`reset` end it),
production leakage guard intact (`src/main.cpp` `#error`), and removal now would
discard a still-useful preregistered diagnostic. No H1A code in production.

**7. D32/D33/D34 preserved, subject to tests.** D32 DriverFaultCapability
NotInstalled + capability-aware telemetry/validation untouched except via the
provenance fixes above; D33 V5 pin map (D2/D4/PH D8/EN D9/nSLEEP D10/trigger D12,
nFAULT `kNoPin`) and MotorDriver single-write-path with de-energize-before-PH
untouched; D34 final-stop dwell RuleDerived, BottleEvidence, planRunTiming,
run-scoped cruise/delay, separate brake offsets, RampDown, PlannedDelayNotUsable
all preserved. Only cleanup-attributable defects fixed, each with regression
coverage.

**8. Environment separation (D35 evidence, not assertion).**
`pio project config` shows `nano_r4: +<*> -<bench/>`, `bench: +<*> -<main.cpp>`,
`bench-watchdog: +<*> -<main.cpp>`, no `emergency`. Objects:
`nano_r4/src` = EncoderInput/main/VehicleRuntime only;
`bench/src` = EncoderInput/VehicleRuntime/bench/BenchMain only. `nm`:
`nano_r4/firmware.elf` matches nothing for `bench|hang|emergency|Emergency|h1a`;
`bench/firmware.elf` matches bench console/drive/hang/grammar plus retained H1A,
no emergency, single `setup`/`loop`; `bench-watchdog/firmware.elf` matches bench,
no H1A, no emergency, single `setup`/`loop`.

**Verification (measured, host/build only; no hardware, board gone).**

- `pio test -e native`: **552/552 SUCCESS** (32 suites; D35 adds 5).
- `pio run -e nano_r4`: **SUCCESS**, RAM 19.2% (6292/32768), Flash 27.4%
  (71836/262144), no warnings.
- `pio run -e bench`: **SUCCESS**, RAM 19.8% (6492/32768), Flash 30.0%
  (78680/262144), no warnings.
- `pio run -e bench-watchdog`: **SUCCESS**, RAM 19.8% (6476/32768), Flash 29.8%
  (78108/262144), no warnings.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry`:
  **116/116 OK**.
- No `emergency` env remains. Nothing uploaded.

**Remaining production blockers (no evidence fabricated).** All calibration still
Provisional: distance per tick, forward/reverse brake offsets, velocity ceilings,
run duration, battery, cruise/accel/decel, feedforward/P/I/integral, reversal and
final-stop thresholds (final dwell is cited RuleDerived, threshold still
Provisional), approach/capture/push/reverse/final timeouts, final-approach
distance, minimum cruise, bottle lead/advance/margin, polarity facts. Hence
`ConfigurationValid`, `CalibrationMeasured`, `SafetyLimitsValid`,
`ControllerConfigValid`, `ProfileLimitsValid`, `PhaseConfigValid`,
`ReversalConfigValid`, `RunPolicyUsable` remain false; `checks all=0`. First
physical steps per `P-H14`: continuity/pin map, then command polarity. Structurally
clean and build/test clean; **not yet competition-ready** pending measured
calibration.

## D36 - 2026-10-04 - Documentation - Reconcile post-tournament physical bring-up evidence

**Trigger.** The final report on the clean integration (`integration/d35-clean`)
was read against documentation still written for the pre-hardware era, and three
present-tense claims were false as of 2026-10-02:

- `docs/limitations.md` - "**no motor supply was ever connected**, so the board has
  never moved anything under firmware control";
- `VERIFIED_STATE.md` - "No motor, motor driver, encoder, battery or drivetrain
  hardware was connected or tested, so every hardware fact below is still
  UNVERIFIED";
- `README.md` - "The vehicle still has no measured behavior of its own: no motor,
  encoder, sensor, trigger, battery or bottle mechanism behavior has been observed
  on hardware".

This entry records what the 2026-10-02 bring-up and the 2026-10-02/03 tournament
actually established. The accompanying edits correct **only** factual status
claims: no firmware source, no test, no configuration constant and no provenance
gate was touched.

**Observed informally - physically witnessed, but NOT evidence (confirmed by the
team after the event).** Everything below is recorded so the fact of observation is
not lost; none of it changes a row.

1. The physical harness was rewired to the stated V5 assignment: encoder A D2,
   encoder B D4, DRV PH D8, DRV EN/PWM D9, DRV nSLEEP D10, trigger D12, PMODE tied
   to GND, nFAULT not installed.
2. The motor hummed and spun under firmware control at low effort - the first
   motion of this vehicle under firmware control.
3. Forward and reverse motor commands were both physically exercised.
4. Encoder ticks changed during actual wheel/motor movement: forward produced
   increasing ticks, reverse produced decreasing ticks.
5. The physical pencil trigger worked.
6. The tournament-only emergency image (its own entry point and build
   environment, present in the post-tournament tree and deleted in D35) was
   flashed and **physically drove the vehicle autonomously forward and backward
   at the tournament**.

**Why none of this can mark a row VERIFIED - the rule, quoted.**
`VERIFIED_STATE.md` `## The rule`: "A hardware or physical fact may be marked
**VERIFIED** only when it is supported by direct **on-target or physical
evidence** that has been recorded (command or test procedure plus raw
observation/data), and the promotion is logged in `DEVELOPMENT_LOG.md`." The same
file excludes, verbatim, "**recollection or verbal description**", sets
"**Default status is UNVERIFIED**", and its `## How to promote a fact` step 1
says the protocols "change no row until they pass", step 2 says "Record the raw
measurement or observation, not just a conclusion", and step 3 requires an
"evidence pointer". For 2026-10-02/03 there is no capture, no pre-registration,
no `data/evidence/` directory and no protocol run to an acceptance rule, so the
raw observation/data does not exist, no pointer can be carried, and what survives
is the team's account - the excluded category. Being logged here (requirement 3)
does not satisfy requirements 1 and 2. **Decision: rows #3 and #4 stay
UNVERIFIED**, and the observations are recorded as OBSERVED / informal physical
observations. `AGENTS.md` line 186 ("Never report hardware behavior as verified
unless it was actually observed or measured") states a necessary condition only;
`VERIFIED_STATE.md`'s rule is the stricter, governing one.

**Provisional - observed, but not a formal calibration.**

- A manual forward rollout counted **~1494 encoder ticks over an *intended* ~2.0 m**,
  giving **~1.339 mm/tick** as a rough figure. The travel was intended, not
  measured; the push was not repeated; no spread exists. It promotes nothing.
- Low effort was enough to move the vehicle; the minimum useful effort was never
  measured or recorded.
- Speed, braking and reversal calibration remain untouched guesses.

**Still unverified - all twelve rows, including #3 and #4.** #1 continuity of every
harness connection (no test performed); #2 the driver's level-to-output truth table;
#3 motor polarity (sign witnessed informally, uncaptured); #4 encoder direction
sign (sign witnessed informally, uncaptured); #5 encoder signal quality; #6 rollout
as a calibration; #7 sensor scaling (battery and current sensing are not
installed); #8 control gains; #9 braking; #10 wheel slip; #11 bottle interaction;
#12 dead-wheel question. Also still unverified: formal mm/tick calibration, Target
Time accuracy, repeatability, any loaded behavior of the production run state
machine, and nFAULT external telemetry (the line is not installed).

**Row changes (`VERIFIED_STATE.md`): none.** #3 and #4 stay **UNVERIFIED**; their
Evidence cells now carry the observation labelled "**OBSERVED, informal - not
evidence under 'The rule'**", and their "What would verify it" cells keep the
original acceptance methods (H3 for #3, H2 hand-rotation for #4). #1, #2, #5 and
#6 gain the same informal-observation notes and all stay UNVERIFIED (#6 records
the ~1494-tick / intended-~2.0 m push as provisional). The availability paragraph
now says what was seen is "observed, not verified" and that no row was promoted. A
new section, "Informal physical observations, 2026-10-02 bring-up and tournament
(D36) - OBSERVED, NOT VERIFIED", opens with "This section promotes nothing" and
closes with "All twelve hardware rows remain UNVERIFIED". Nothing was promoted to
`Measured`: every `Sourced` value in `src/VehicleConfig.h` is unchanged and still
`Provisional`, so `arm` still refuses.

**Documentation corrected (status claims only).**

- `VERIFIED_STATE.md` - the availability paragraph (D31's "board gone" reframed as
  a period fact and a *current* availability constraint, and what was seen there
  labelled "observed, not verified"); evidence cells for rows #1-#6 annotated as
  informal observations with **no status change**; the new
  "OBSERVED, NOT VERIFIED" observations section; the emergency-image note on the
  "one writer" row; and one false absolute ("no bench instrument has been run" -
  the same file records `bench pins`, `bench resetsrc` and `bench wdt` as
  on-target readings from H0/H0W). The `#1-#3` cross-references were checked and
  left as written: with #3 still UNVERIFIED they are correct.
- `docs/limitations.md` - section A (all twelve rows stated UNVERIFIED, the
  observations labelled informal, and the false "never moved" sentence superseded),
  the board-availability paragraph (the owed stages are owed because they were not
  done, not because they are unperformable), the encoder entry (direction observed
  informally; row #4 UNVERIFIED; counts/rev and gearing still open), a new bullet
  on the tournament's emergency image, section F (the provisional rollout; the
  polarity encodings observed but not evidence and not a promotion; the
  production-path scope of the shaping/braking/reversal entry), the H0W Step 8
  entry ("cannot be run" / "owed and unperformable" -> owed and blocked on a
  loaded run path), and section G (H0/H0W scope, the image-identity "cannot be
  reflashed" wording, and the missing evidence directory with "not enough to mark
  any row VERIFIED").
- `README.md` - the `docs/bringup/` line, the "Next" stage entry (observation, not
  evidence; no row moved), "It cannot move" -> the *production image* cannot be
  commanded to move, and the current-state paragraph ("every hardware row in
  `VERIFIED_STATE.md` still reads UNVERIFIED").
- `SOFTWARE_ARCHITECTURE.md` - sections 10 and 11: a note added in each that the
  uncaptured 2026-10-02 observation is OBSERVED-not-verified and changes nothing.
  The original status wording (encoder direction and polarity facts UNVERIFIED,
  `#1-#3` for the driver levels, no `lib/` assumption, no `Measured` without a
  citation, the arm refusal) was restored byte-for-byte and stands unchanged.
- `docs/bringup/README.md` - "once the vehicle hardware exists" and "when the
  board arrives" wording, a note beside the H0/H0W paragraph recording the
  2026-10-02 informal entry and that all twelve rows stay UNVERIFIED, and a note
  under the coverage table that #3/#4 carry only an uncaptured observation until
  H3/H2 record one.
- `docs/hardware/motor-driver-drv8874.md` - #3 stated as having only an uncaptured
  observation and **staying UNVERIFIED**; the `DriveForward`/`DriveReverse` rows
  keep "**Unverified.**" and add the observation as an observation, not a level
  measurement.

**Unchanged deliberately.** `data/prereg/` (a pre-registration is fixed before
its measurement), `data/evidence/` (the D31 and H0/H0W records were true of those
sessions), the protocol files under `docs/bringup/` (procedures, not status),
`AGENTS.md`, and all firmware source and tests. D31's and D35's in-log sentences
stay as history; this entry supersedes them only where they were read as claims
about hardware history: D31's "the board is gone ... unperformable with the
equipment the project has" (out of hand then, in hand again on 2026-10-02), D35's
"(no hardware, board gone)" parenthetical as a statement about hardware, and
D35's "First physical steps per `P-H14`: continuity/pin map, then command
polarity" (command polarity has only an informal observation, still UNVERIFIED;
continuity does not even have that). D35's
own host/build verification stands.

**Process note on this entry.** This entry was drafted, then corrected within the
same uncommitted task, before any commit: the draft first recorded rows #3 and #4
as promoted to VERIFIED, which the quoted rule does not allow. Amending an
uncommitted draft is not a rewrite of history; had it been committed, the
correction would have been appended as a new entry instead.

**Emergency image, stated once.** It was tournament-only, it **did** physically
operate the vehicle forward and backward at the tournament, and D35 removed it
from the active tree (source, test, `[env:emergency]`). It is recoverable only
from `recovery/d32-d34-v5-snapshot`, and `src/main.cpp` refuses to compile its
define into the vehicle image. It is not the production run state machine, and
no telemetry, run record or result from it exists in this repository.

**Evidence limit, stated once.** There is **no** `data/evidence/` directory, no
pre-registration and no serial capture for 2026-10-02/03 - there is no raw record
to re-read. Everything above is a single un-instrumented sighting, recorded from
the team's confirmed account. It is enough to record that it was seen (and so to
stop the documentation asserting the opposite as fact); it is **not enough to mark
any row VERIFIED**, not a promotion of any calibration, not a distribution, and
not a measurement. The protocols that would produce evidence (H1A, H1B, H2, H4,
H0W Step 8, H5+) remain owed, and until one runs and is recorded, every hardware
row stays UNVERIFIED.

**Verification (documentation only).** `git diff --check` clean; `git status`
shows only `.md` files; no code file changed, so no rebuild was run. The tree
intentionally no longer equals `cleanup/d35-post-tournament`'s tree - it differs
by exactly these documentation files. Nothing committed, merged or pushed.

---

## D37 / S0 - 2026-10-04 - Live-source reconciliation and stage-status matrix (S0R corrected)

**Kind of change.** Documentation only. No firmware source, test, configuration,
calibration or provenance change was made; no hardware action was performed; nothing
was committed, merged, pushed or uploaded by this stage. This entry is the
repository-side record of the S0 stage of the pack
`EV-V5-current-to-competition-2026-10-04-v2`; the full stage report is kept in the
pack's local agent workspace (not committed).

**S0R correction (same day, bookkeeping only).** The first version of this entry
used an S1-S12 numbering taken from the pack's re-check lists, which does not match
the pack's authoritative stage mapping. The entry has been corrected in place (only
this current, uncommitted entry; no older history was touched): the mapping below is
authoritative, the two "blocked on user architecture decision" rows are replaced by
recorded Decisions A (rule geometry vs vehicle execution geometry) and B (absent
battery/current sensing), and the S7 row records the timing decision. No engineering
stage was started and no code changed.

**Live starting point, re-verified rather than assumed.** `main` = `origin/main` =
`a0de094c7cf67b1b13a86682745fe75c1a31757b`; working tree clean before this entry
(`## main...origin/main`); active PlatformIO environments are `nano_r4`, `bench`,
`bench-watchdog`, `native`; `env:emergency` is absent (`platformio.ini` has no such
environment, `pio run -e emergency` reports an unknown environment name, and
`src/main.cpp` refuses the removed defines with `#error`).

**Software baseline, re-run at this HEAD (all green).**

- `pio test -e native`: **552/552 test cases succeeded** (32 suites).
- `pio run -e nano_r4`: SUCCESS, RAM 6292/32768 (19.2%), Flash 71836/262144 (27.4%).
- `pio run -e bench`: SUCCESS, RAM 6492/32768 (19.8%), Flash 78680/262144 (30.0%).
- `pio run -e bench-watchdog`: SUCCESS, RAM 6476/32768 (19.8%), Flash 78108/262144 (29.8%).
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry`: **116/116 OK**.
- No compiler warnings in any of the three builds.

**Roadmap artifact and orchestration authority.** No standalone "2026-10-04
engineering roadmap" document exists at any repository path or in the pack
workspace. Until a repository roadmap is deliberately added, this pack's S1-S12
mapping and `.agent-work/MASTER_CONTEXT.md` are the orchestration authority for the
stages below. Classification is software-evidence only and carries no hardware claim.

**Architecture decisions already made (no stage is blocked on these).**

- **Decision A - S2 geometry.** Bottle Line is an official rule landmark, not
  inherently the vehicle's physical forward stop target. `RunAxisGeometry` keeps
  owning rule geometry (`targetPointPositionM`, `bottleStartPositionM`,
  `bottleLinePositionM = Target Point + the announced Bottle Line Distance`). A
  later `RunExecutionPlan` owns the explicit vehicle execution target, including
  `forwardStopMpPositionM`. Do not guess that physical target before its
  dependencies are established; host-test fixtures may use synthetic explicit
  targets; production must fail closed while required physical calibration is
  unavailable. S2 is STILL NEEDED, not blocked.
- **Decision B - S10 absent sensing.** The assembled V5 has nFAULT, battery sensing
  and current sensing all `NotInstalled`; A1/A2 floating/raw counts are not
  measurements. Do not fabricate a `Measured` `minBatteryV`; do not treat floating
  counts as engineering values. `minBatteryV` must not permanently block arming
  merely because this hardware has no battery-sense circuit; when battery sensing is
  `NotInstalled`, the voltage-based runtime gate is N/A for that build and the
  limitation stays explicit. If battery sensing is later installed, its conversion
  and threshold must be physically validated before the gate can authorize motion.
  Current sensing stays optional telemetry/anomaly information unless later evidence
  justifies a control/safety dependency. S10 is not blocked.
- **Timing decision - S7.** D34 established one timing-plan owner and
  per-announcement cruise/delay plumbing. An unvalidated idealised physical-time
  prediction must not control production timing; the intended later behaviour is
  `timingReady = false`, `initialDelayS = 0` until physical timing evidence
  validates the model. The live tree still lets the idealised plan control
  cruise/delay, so S7 is PARTIALLY DONE. Do not implement here.

**Capability matrix (S1-S12, authoritative mapping; S0R corrected).**

| Stage: capability | Classification | Live evidence (at a0de094) |
|---|---|---|
| S1 Direction-specific brake-offset truth and stale-document reconciliation | PARTIALLY DONE | `profileLimitsForOneDirection()` selects `kPlannedForwardBrakeOffsetM`/`kPlannedReverseBrakeOffsetM` by travelled direction and `profileLimitsWithCruise()` does the same (D35-era fix recorded in this log). `test_run_timing/test_the_leg_limits_carry_that_legs_own_brake_offset` uses deliberately unequal fixtures (forward 0.05 vs reverse 0.40) and asserts both selection and behaviour (reverse `WithinBrakeOffset` at 0.30 m; forward still shapes a target). Remaining: the test covers the limits container, not the `VehicleConfig` builder (`test/` has zero `VehicleConfig` references), and `docs/bringup/P-H7-distance-braking-reversal.md` still says the reverse offset "currently has no consumer" and that the forward offset is passed to both legs - stale as of the fix. |
| S2 Separate official rule geometry from vehicle execution geometry | STILL NEEDED | `lib/evcore/src/RunAxis.h` conflates the rule landmark with the forward execution target: `turnAroundPositionM() = targetPointPositionM + bottleLineDistanceM`, test-pinned by `test_run_axis/test_the_turn_around_is_the_target_plus_the_announced_bottle_line`; `RunConfig` carries no separate execution target. Decision A recorded above; not blocked. |
| S3 Immutable RunExecutionPlan with dependency validity | STILL NEEDED | No `RunExecutionPlan` type exists (zero live hits; the only occurrence is this log text). Per-run decided state is mutable and spread across `VehicleRuntime` (`runProfileLimits_`, `timingPlan_`, `axisGeometry_`, `legForward_`) and `CompetitionStateMachine` (`plannedDelayS_`); announce/reset replace fields. |
| S4 One forward physical stop target everywhere | STILL NEEDED | No execution-plan forward target exists. Motion shaping consumes `legRemainingM()` (taper to `turnAroundPositionM()`); forward progression consumes `BottleEvidence::pushComplete` at `turnAroundPositionM() + pushMarginDistanceM`. Both share the rule landmark today but neither consumes one explicit execution-plan target. |
| S5 Truthful bottle-phase progression semantics | PARTIALLY DONE | `BottleEvidence.h` derives `contactDetected`/`captureComplete`/`pushComplete` from run-axis position and states it is a position-derived belief (D34 deviation recorded in `SOFTWARE_ARCHITECTURE.md` section 14 and `docs/limitations.md`); sensor-sounding names unchanged; `test_bottle_evidence` 7 cases; `VERIFIED_STATE.md` #11 UNVERIFIED. |
| S6 Strategy velocity ceiling inside the existing target shaper | PARTIALLY DONE | `planRunTiming()` caps the planning ceiling at the lower of the strategy cruise and the safety ceiling (`TargetTimeTooShort` refusal); per-direction `velocityCeilingMps` feeds the shaper; no separate strategy-ceiling parameter; all inputs `Provisional`. |
| S7 Per-run delay ownership AND disabling unvalidated automatic timing behavior | PARTIALLY DONE | D34 ownership live (`planRunTiming()` single owner; `runProfileLimits_`; `plannedDelayS()`; `test_run_timing` 14 cases + `test_competition_state` delay cases); the idealised model still controls cruise/delay automatically. Timing decision recorded above; do not implement here. |
| S8 Official Run Time telemetry semantics at successful final-stop dwell start | STILL NEEDED | No run/actuation timestamp (`CompetitionStateMachine` stores `phaseEnteredMs_`/`faultRaisedAtMs_` only; `ReversalSequencer::startedAtMs_` is the reversal timer); telemetry schema v2 has no Run Time field or run-complete event; `STATUS` reports no time fields; rules 8.o/8.p recorded in `docs/rules/README.md` only. |
| S9 Read-only plan observability command | PARTIALLY DONE | `announce` echoes the plan once (`OK,cmd=announce,plan=...,cruise_mps=...,delay_s=...,total_s=...,margin_s=...`, `formatAnnounceAck`); no `plan` command in the frozen vocabulary; `STATUS` has no plan fields; `timingPlan_` is never read back. |
| S10 Global maxRunDuration runtime consumer AND capability-aware absent-sensing policy | PARTIALLY DONE | Plan-time consumer exists (`kPlannedMaxRunDurationS`; `PlanExceedsMaxRunDuration` refusal, pinned by test); no runtime run timer (`SafetyLimits::maxRunDurationS` data-only; `docs/limitations.md` F "run timer ... not built"). Battery/current sensing not installed (A1/A2 raw counts only; `batteryReadingValid` never true); arming currently requires a `Measured` `minBatteryV` (`isFullyEstablished()`). Decision B recorded above; not blocked. |
| S11 Full legal-parameter host sweep and deterministic scenario coverage | PARTIALLY DONE | Deterministic scenario harness exists and is green (`test/test_scenario`, incl. identical-trace replay); no full legal-parameter sweep artifact in `tools/`/`scripts/`/`test/`; nearest coverage `test_run_config` boundary/grid cases. |
| S12 Architecture / limitations / bring-up documentation synchronization | PARTIALLY DONE | D36 status wording and D34 section-14 deviation are live; remaining: `SOFTWARE_ARCHITECTURE.md` section 7 / `docs/limitations.md` F say velocity ceilings and max run duration have "no enforcement point ... no runtime effect" while the D34 announcement boundary consumes both (only the battery check and the runtime run timer remain genuinely unbuilt); stale P-H7 brake-offset wording tracked under S1. |

**Post-audit changed facts, re-verified live (none re-broken).**

| Fact | Live verification |
|---|---|
| Direction-specific brake-offset truth | Code fixed (`profileLimitsForOneDirection()`, `profileLimitsWithCruise()`); unequal-fixture regression test `test_the_leg_limits_carry_that_legs_own_brake_offset` (0.05 forward vs 0.40 reverse, plus behavioural assertion) covers the limits container; no native test reaches the `VehicleConfig` builder; `docs/bringup/P-H7-distance-braking-reversal.md` still carries the stale "no consumer" claim. |
| D34 timing work | `planRunTiming()`, `RunTimingPlanInputs`, `plannedDelayS()`, `runProfileLimits_` wiring; the announce acknowledgement carries the plan. |
| `Derived` provenance | `ValueSource::Derived` in `lib/evcore/src/Units.h`; `cruiseProvenanceIsEstablished()` accepts Measured or a cited Derived value for the cruise only; `plannedCruiseValue()` returns Derived with its derivation cited; tests `test_a_computed_cruise_is_derived_never_measured`, `test_profile_limits_accept_a_cited_derived_cruise_only`, `test_plan_inputs_still_require_measured_limits`. |
| nFAULT `NotInstalled` semantics | `DriverFaultCapability::NotInstalled` in `lib/evcore/src/DriverFault.h`; `kPlannedDriverFaultCapability` is `NotInstalled`; `kMotorFaultBar` is `kNoPin`; `src/SensorInputs.h` does not configure or sample the pin; telemetry writes `nfault=NotInstalled,nfault_low=-` (pinned by `test_telemetry_format`); the capability-aware check passes without a polarity measurement while `Installed` still refuses (`test_driver_fault`). |
| Final V5 pin map | `src/PinMap.h`: encoder A D2 (interrupt channel 0), encoder B D4, PH D8, EN D9 (the only PWM consumer), nSLEEP D10, trigger D12, nFAULT has no pin, PMODE is not an MCU pin; it remains a stated harness map, so `VERIFIED_STATE.md` #1 stays UNVERIFIED. |
| D36 evidence wording | `VERIFIED_STATE.md` "OBSERVED, NOT VERIFIED" section and row annotations; `docs/limitations.md` section A; `README.md` current state; `SOFTWARE_ARCHITECTURE.md` sections 10/11 notes; "all twelve hardware rows remain UNVERIFIED". |

**Contradictions found (recorded, not fixed in S0/S0R).**

1. `SOFTWARE_ARCHITECTURE.md` section 7 and `docs/limitations.md` section F
   ("maximum velocity per direction, maximum run duration ... have no enforcement
   point ... no runtime effect") are narrower than the D34 tree: the announcement
   boundary now consumes both through `runTimingPlanInputs()` - a plan refusal and
   a shaped cruise bound are behaviour. Only the battery check and the runtime run
   timer remain genuinely unbuilt.
2. The pack's 2026-10-04 roadmap document is absent from the repository; per S0R,
   the pack mapping and `.agent-work/MASTER_CONTEXT.md` are the orchestration
   authority unless a repository roadmap is deliberately added later.
3. `docs/bringup/P-H7-distance-braking-reversal.md` still claims the reverse brake
   offset has no consumer and that the forward offset is passed to both legs -
   stale as of the D35-era fix (S1 scope).

**No promotions, no behavior change.** Every calibration and limit value stays
`Provisional` (only `kMaxDwellGapMs` remains the cited `Measured` exception); no
`VERIFIED_STATE.md` row changed; the configuration gates still report every arming
precondition false, exactly as D34/D35 left them.

**Stage bookkeeping.** S0 remains PASS; the S0R stage-map correction is PASS. The
next eligible stage is **S1** (direction-specific brake-offset truth and
stale-document reconciliation). This entry remains uncommitted pending a checkpoint.

---

## D38 / S1 - 2026-10-04 - Brake-offset direction selection: host regression coverage and stale bring-up text corrected

**Kind of change.** Software + documentation. S1 of the pack
`EV-V5-current-to-competition-2026-10-04-v2`: direction-specific brake-offset
truth and stale-document reconciliation. The D35-era direction-specific selection
is confirmed live and was **not** reimplemented; the production change is a
behaviour-preserving extraction that creates the host-test seam, and the rest is
regression coverage and documentation. No physical fact was promoted; no hardware
action (no upload, no powered test, no measurement); nothing committed, merged,
pushed or uploaded by this stage. This entry is the repository-side record; the
full stage report is kept in the pack's local agent workspace (not committed).

**What the live tree already had (re-verified, not assumed).**
`src/VehicleConfig.h::profileLimitsForOneDirection()` selected
`kPlannedForwardBrakeOffsetM` for the forward leg and
`kPlannedReverseBrakeOffsetM` for the reverse leg, and `profileLimits()` /
`profileLimitsWithCruise()` both routed through it. The unequal-fixture test
`test_the_leg_limits_carry_that_legs_own_brake_offset` exercised the
`DirectionalMotionProfileLimits` container, not the builder's selection step, and
`docs/bringup/P-H7-distance-braking-reversal.md` still claimed the reverse offset
had no consumer and that the forward offset was passed to both legs.

**Code change (behaviour-preserving seam).**
`ev::brakeOffsetForDirection(direction, forwardBrakeOffsetM, reverseBrakeOffsetM)`
now owns the per-direction selection in `lib/evcore/src/MotionProfile.h`, and the
`VehicleConfig` builder calls it with the two planned constants. The selection
result is unchanged; no constant, gate, provenance label, safety path or motion
behaviour changed. The extraction follows `SOFTWARE_ARCHITECTURE.md` section 3
(the native environment never compiles `src/`, `test_build_src = no`) and the D14
precedent (`ConfigurationChecks.h` moved out of `VehicleConfig.h` so it could be
host-tested).

**Regression coverage.**
`test/test_run_timing/test_main.cpp::test_the_builder_selects_the_brake_offset_by_direction`
drives the selection with a deliberately unequal fixture pair (forward 0.011,
reverse 0.077 - synthetic host fixtures, never vehicle measurements): each
direction receives its own value and provenance, and a swapped pair swaps the
answer. Verified negatively: with the historical defect temporarily reintroduced
(the reverse case returning the forward value), the new test fails while the older
container-level test still passes; the simulation was reverted immediately and the
restored tree re-passed.

**Documentation correction.** `docs/bringup/P-H7-distance-braking-reversal.md` no
longer claims `kPlannedReverseBrakeOffsetM` has no consumer or that the forward
offset is passed to both legs. The corrected text separates the software fact
(both offsets have their own direction-specific consumer, pinned by the host test)
from the physical fact (both numbers remain `Provisional` until Part B records
ground-truth braking data, and neither leg may be reported as calibrated before
then). The order-of-work item, acceptance rule B and the "If it passes" bullets
were corrected to match. Historical log entries were not rewritten.

**Software verification (not physical verification).**

- `pio test -e native`: **553/553 test cases succeeded** (552 at D37 plus this
  entry's one new test). The only compiler warning (`test_scenario` switch over
  `FaultCode` missing `DriverFaultBar`, `-Wswitch`) reproduces identically at
  `91409bc` in a clean worktree, so it is pre-existing, not new.
- `pio run -e nano_r4`: SUCCESS, RAM 6292/32768 (19.2%), Flash 71884/262144 (27.4%).
- `pio run -e bench`: SUCCESS, RAM 6492/32768 (19.8%), Flash 78728/262144 (30.0%).
- `pio run -e bench-watchdog`: SUCCESS, RAM 6476/32768 (19.8%), Flash 78156/262144 (29.8%).
- `git diff --check`: clean.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration/limit
  value remains `Provisional`; the two brake-offset numbers still require the P-H7
  physical characterization.

---

## D39 / S2 - 2026-10-05 - Rule geometry separated from execution geometry

**Kind of change.** Software + documentation. S2 of the pack
`EV-V5-current-to-competition-2026-10-04-v2`. This entry is the repository-side
record of the S2 milestone; the full stage report is kept in the pack's local
agent workspace (not committed). No physical fact was promoted, no hardware
action was taken (no upload, no powered test, no measurement), and nothing was
committed, merged, pushed or uploaded by this stage.

**The problem.** `ev::RunAxisGeometry` conflated the event's official rule
geometry with the vehicle's execution geometry: `turnAroundPositionM()` returned
the Target Point plus the announced Bottle Line Distance and was named and
treated as the vehicle's forward-leg endpoint and turn-around position, so the
Bottle Line rule landmark was architecture-truth for the physical forward-stop
target. Decision A separates the two.

**What changed.**

- `RunAxisGeometry` now owns **rule landmarks only**: `targetPointPositionM`
  (rule 8.h), `bottleStartPositionM` (rule 7.c.ii.(5)) and the new
  `bottleLinePositionM()` (rule 8.j, the Target Point plus the announced Bottle
  Line Distance). `turnAroundPositionM()` is gone; the Bottle Line is documented
  as an official rule landmark and explicitly **not** the vehicle's physical
  turn-around, forward-leg endpoint or forward stop target.
- The run-axis remaining-distance arithmetic is now generic and takes an
  **explicit, caller-supplied target**: `ev::remainingDistanceTo(targetPositionM,
  currentPositionM, forwardLeg)` (signed, deliberately unclamped) and
  `ev::remainingDistanceToClamped(...)` (the taper distance, never negative). The
  old geometry-coupled `remainingToTargetM()` / `legRemainingM()` were removed,
  so the arithmetic can no longer choose an execution target, and it cannot
  substitute a rule landmark (such as the Bottle Line) for the target a consumer
  asked about.
- `ev::deriveBottleEvidence()` consumes the Bottle Line rule landmark truthfully
  (`geometry.bottleLinePositionM()`) for the push-complete position; its comments
  now name the rule landmark rather than the "turn-around position".
- `plannedTravelDistanceM()` is expressed from the rule landmarks; behaviour is
  unchanged.

**Temporary pre-S3/S4 compatibility seam (debt).**
`ev::temporaryLegExecutionTargetM(geometry, forwardLeg)` in
`lib/evcore/src/RunAxis.h` is the ONE place that preserves the old behaviour: the
forward leg's execution target is taken to be the Bottle Line rule landmark (the
reverse leg's target is the rule Target Point and is not temporary). It is a
compatibility mapping, not a physical claim, and it contains no calibration
constant. `src/VehicleRuntime.cpp` and the host scenario harness route their leg
distance through it, so there is a single seam, not several copies.
**S3/S4 must remove it**: S3 owns the immutable `RunExecutionPlan` and S4 owns the
single forward physical stop target everywhere. **No physical forward-stop value
was invented.**

**Callers migrated.** `src/VehicleRuntime.cpp`,
`test/test_scenario/ScenarioHarness.h`, `test/test_scenario/test_main.cpp`,
`test/test_operator_policy/test_main.cpp`, `test/test_run_timing/test_main.cpp`,
`test/test_bottle_evidence/test_main.cpp` (naming), and the affected
documentation (`SOFTWARE_ARCHITECTURE.md`).

**Tests.** `test/test_run_axis/test_main.cpp` was rebuilt for the S2 contract: the
Target Point, Bottle Start Point and Bottle Line positions; the Bottle Line
identity `bottleLinePositionM() == targetPointPositionM + bottleLineDistanceM`;
the explicit-target arithmetic (two different explicit targets give two different
remaining distances while the rule geometry is unchanged; the Bottle Line does
not override the caller's target; leg/direction sign behaviour), and the
compatibility seam pinned **as compatibility behaviour, not physical truth**. No
test asserts that the vehicle's turnaround inherently equals the Bottle Line.

**Software verification (not physical verification).**

- `pio test -e native`: **556/556 test cases succeeded** (553 at D38 plus 3 net
  new run-axis cases), 0 warnings.
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog`: 3 SUCCESS, 0 warnings,
  RAM/Flash identical to the D38 baseline (6292/71884, 6492/78728, 6476/78156) -
  behaviour- and footprint-preserving.
- `git diff --check`: clean.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration/limit
  value remains `Provisional`; the `Provisional` lockout still refuses to arm.
- No gate, provenance label or safety path was changed, and no blocking `delay()`
  was added.

**Next.** S3 - an immutable `RunExecutionPlan` with dependency validity, which
owns the explicit vehicle execution target (including the forward stop position);
S4 then makes motion shaping and forward progression consume that one target and
removes the S2 compatibility seam. This entry does not start S3.

---

## D40 / S3 - 2026-10-05 - The execution plan owns the run's execution targets, or names why it cannot

**Kind of change.** Software + documentation. S3 of the pack
`EV-V5-current-to-competition-2026-10-04-v2`. Adds
`lib/evcore/src/RunExecutionPlan.h` and `test/test_execution_plan/test_main.cpp`. No
runtime wiring, no calibration value, no telemetry change, no hardware action.

**Why.** S2 separated the announced *rule* geometry (`RunAxisGeometry`: Target
Point, Bottle Start Point, Bottle Line) from the vehicle's execution geometry and
left exactly one temporary compatibility seam (`temporaryLegExecutionTargetM()`)
that maps the forward leg onto the Bottle Line. The vehicle's physical forward stop
target is not the Bottle Line and cannot be guessed: it depends on pusher reach, the
contact window, capture/transfer behaviour, the push margin and braking, none of
which is established on this vehicle. S3 creates the abstraction that will
eventually own those execution targets, without pretending the missing physical
facts exist.

**What the plan is.** `ev::RunExecutionPlan` (with `ev::RunExecutionDependencies`,
`ev::buildRunExecutionPlan()` and `ev::RunExecutionPlanStatus`) is a pure, immutable
value describing ONE announced run: the preserved validated `RunAxisGeometry`; the
forward-axis interaction landmarks (earliest and latest credible contact, capture
guarantee, push complete); the explicit `forwardStopMpPositionM` and
`reverseStopMpPositionM`; and timing state (`timingReady`, `predictedMotionTimeS`,
`initialDelayS`). No Arduino headers, no I/O, no serial formatting, no allocation,
no globals, no `VehicleConfig` dependency and no motor command. Every field is
`const`, there is no default constructor and there are no setters, so a plan cannot
be adjusted as a run progresses: an announcement produces one plan and a new
announcement produces a new plan.

**Forward stop rule.** `forwardStopMpPositionM = bottleLinePositionM() +
forwardStopMarginBeyondBottleLineM`, where the margin is a required `Measured`
dependency. The builder never equates the forward stop with the Bottle Line: an
unchanged Bottle Line with a different measured margin produces a different forward
stop (host-tested), and with no margin established the construction refuses with
`MissingForwardExecutionTarget`.

**Reverse stop rule.** `reverseStopMpPositionM = targetPointPositionM` (rule 8.h) -
a rule-derived relationship pinned by host test, and not labelled `Measured`
anywhere.

**Ordering.** A valid plan must satisfy `earliestContact <= latestContact <=
captureGuaranteed <= forwardStop`, `pushComplete <= forwardStop` and
`earliestContact >= 0` (contact cannot precede the Start Point). Internally
contradictory geometry is refused as `ContradictoryExecutionGeometry` rather than
carried into a run. The contact/capture positions are derived from the existing
`BottleEvidenceConfig` semantics (nominal contact = Bottle Start Point minus the
contact lead; capture = nominal contact plus the capture advance; push complete =
Bottle Line plus the push margin), reused rather than restated.

**Dependency / provenance contract.** Construction reuses `validateRunConfig()` for
the announcement and `validateBottleEvidenceConfig()` for the bottle distances
instead of restating either; only `ValueSource::Measured` authorizes a physical
position, so a `Provisional` placeholder - or a `Derived` planning value - refuses,
and nothing is promoted. Refusals are named: `InvalidAnnouncement`,
`MissingBottleInteractionCalibration`, `MissingForwardExecutionTarget`,
`NonFiniteValue` and `ContradictoryExecutionGeometry`. The zero value stays
`Planned`, and a refusal carries no derived position at all.

**Timing.** The plan records timing honestly and does not make the idealized timing
model control production behavior: with `timingReady` false it carries
`predictedMotionTimeS = 0.0` and `initialDelayS = 0.0` regardless of the caller's
idealized numbers, and `timingReady` true requires finite non-negative inputs and is
exercised only by a synthetic fixture. `planRunTiming()`, the run's delay and the
run's cruise are untouched.

**Production.** With the shipped configuration a production construction refuses:
every bottle-interaction distance in `src/VehicleConfig.h` is `provisionalValue(...)`
(`kPlannedBottleContactLeadM`, `kPlannedBottleCaptureAdvanceM`,
`kPlannedBottlePushMarginM`), and no measured forward-stop margin exists anywhere in
the tree - there is no forward-stop constant at all. Both refusals are host-tested
with production-shaped provenance.

**Not done (scope).** No runtime integration: `src/VehicleRuntime.cpp`/`.h`,
`CompetitionStateMachine`, `MotionProfile`, `OperatorPolicy`, the serial commands and
the telemetry schema are untouched, and no consumer reads the plan yet. The S2
compatibility seam `ev::temporaryLegExecutionTargetM()` remains exactly as S2 left
it, still mapping the forward leg onto the Bottle Line rule landmark, and is
identified for removal by S4. S4 owns the wiring.

**Tests.** `test/test_execution_plan/test_main.cpp`, 12 cases: a valid synthetic plan
(all fields and the ordering invariant); illegal announcements (off-grid,
out-of-range, another level's bottle placement, non-finite); non-finite established
dependencies (NaN tolerance, infinite margin, non-finite bottle distances);
unestablished bottle dependencies (each distance and the contact-window tolerance, as
`Provisional` and as `Derived`); unestablished forward execution target;
contradictory geometry (capture window not covered by the advance, push not complete
by the stop, negative margin, contact behind the Start Point); reverse stop = Target
Point for two announcements; the forward stop follows the margin and is not the
Bottle Line at an unchanged Bottle Line; two plans from two announcements are
independent (building plan B does not mutate plan A); timing readiness recorded
honestly; no hidden valid defaults (`static_assert`s on default-constructibility and
copy-assignability, plus a refusal that carries zero positions and a named status);
and the production-shaped refusal. Every value is a synthetic fixture, not a
measurement: a passing host test is arithmetic and gate verification only, and is
not evidence about the bottle, the stops or the vehicle.

**Software verification (not physical verification).**

- `pio test -e native -f test_execution_plan`: **12/12 succeeded**.
- `pio test -e native`: **568/568 succeeded** (556 at S2 plus 12 new), 0 warnings.
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog`: 3 SUCCESS, 0 warnings,
  RAM/Flash byte-identical to the S2 baseline (6292/71884, 6492/78728, 6476/78156) -
  the new header is compiled into no image, so no image behaviour changed.
- `git diff --check`: clean. Telemetry was not touched and was not re-run.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration and limit
  value remains `Provisional`; the `Provisional` lockout still refuses to arm.

**Next.** S4 - make motion shaping and forward progression consume the plan's one
forward stop target everywhere, and delete `ev::temporaryLegExecutionTargetM()` and
its two call sites (`src/VehicleRuntime.cpp`, `test/test_scenario/ScenarioHarness.h`).
This entry does not start S4.

---

## D41 / S4 - 2026-10-05 - One forward stop target: the travel taper and the push objective read the same stored value

**Kind of change.** Software + documentation. S4 of the pack
`EV-V5-current-to-competition-2026-10-04-v2`. Wires the immutable execution plan
into the vehicle layer and the scenario harness, deletes the S2 compatibility seam,
and makes one stored forward stop value own both the forward travel's shaping target
and its completion. No calibration value, no telemetry wire-schema change, no
hardware action.

**Why.** S2 separated the announced *rule* geometry from the vehicle's execution
geometry (`D39`), and S3 created the plan that owns the execution targets (`D40`) -
but nothing consumed it. Until this change the forward travel ended in two places,
both built on the same rule landmark: the taper was sized to the Bottle Line, and
`Push` completed at the Bottle Line plus the configured push margin. Neither was the
vehicle's execution target, and the two could disagree with each other. This change
gives both consumers one owner and one stored value.

**What consumes the plan now.**

- **The leg's execution target** - `ev::legExecutionTargetM(plan, legForward)`
  (`RunExecutionPlan.h`): the forward leg ends at `forwardStopMpPositionM`, the
  reverse leg at the announced Target Point the plan carries. The vehicle layer's
  leg distance (`legRemainingM`, the taper input) and the harness's mirror of it are
  computed from this, and `remainingDistanceToClamped()` is unchanged arithmetic
  done to an explicitly named target.
- **The push objective** - `ev::deriveBottleEvidence(plan, ...)`: contact and
  capture keep their existing meaning, owner and configuration; the push counts as
  complete at the plan's forward stop. Every valid plan's ordering invariant already
  requires `pushCompleteMpPositionM <= forwardStopMpPositionM`, so completing at the
  stop is a completed push rather than a shortened one, and the plan's rule-derived
  landmark stays as the bound that invariant checks.
- **The run layer's storage** - `ev::RunExecutionPlanHolder`: a plan is immutable
  and not assignable, so the holder *adopts* one announcement's plan in place
  (construct-in-place, no allocation) and `clear()`s back to `NotPlanned` at a
  new-run boundary. `RunExecutionPlanStatus::NotPlanned` was added for exactly that
  state, so "no announcement yet" is distinguishable in a log from "the
  announcement was refused".
- **Telemetry** - the tick snapshot now carries `legTargetUsable`, `legTargetM`,
  `executionPlanUsable`, `executionPlanStatus` and `forwardStopMpPositionM`, so a run
  that could not shape a forward target is attributed to the missing plan and to its
  named refusal. The wire schema (`TelemetrySample`/`TelemetryFormat`) is
  deliberately untouched: extending it is S8/S9 work.
- **Configuration** - `kPlannedContactWindowToleranceM` and
  `kPlannedForwardStopMarginBeyondBottleLineM` in `src/VehicleConfig.h`, both
  `provisionalValue(...)` placeholders, plus `runExecutionDependencies()`. No value
  was invented: they exist so the refusal paths can run, and the plan refuses while
  they are Provisional.

**Fail-closed behaviour.** With no usable plan there is no leg target at all: the
leg distance is zero (nothing to shape toward) and no forward evidence is derived,
so a run cannot travel forward or claim a completed push. No rule landmark is
substituted for the missing execution target, and the plan's refused positions
(zeros) are never read as landmarks. In production this is the live state: the
forward-stop margin has never been measured, so the plan refuses with
`MissingForwardExecutionTarget` and a run has no forward execution target. The
`Provisional` calibration lockout already refuses to arm such a run.

**Removed duplication.** `ev::temporaryLegExecutionTargetM()` (`RunAxis.h`) and its
two call sites (`src/VehicleRuntime.cpp`, `test/test_scenario/ScenarioHarness.h`) are
gone, together with the scenario driver's own
`position >= bottleLinePositionM()` push threshold - that script now asks the
harness for the production derivation instead of repeating the arithmetic.

**Not changed (owned elsewhere).** Bottle-phase semantics (S5): the phases, their
order, their timeouts and their exit evidence are untouched. Speed strategy and the
timing model (S6/S7): `plannedTravelDistanceM()` still sizes the timing plan from
rule geometry, the planned cruise still comes from `planRunTiming()`, and the plan's
timing inputs are recorded honestly (`timingReady = false`, both numbers 0.0) rather
than letting an unvalidated prediction travel beside the execution targets -
reconciling the rule course with the execution targets for the timing estimate is a
separate, owned decision. Operator `plan` output (S9) and the telemetry wire schema
(S8/S9) are not built here. The arming gate does not yet require a usable execution
plan: a run announced with one cannot drive, but nothing stops such a run being
announced, and it would end in a phase timeout rather than in motion.

**Tests.** `test_execution_plan` (+6 cases): the leg target and the push objective
are the same stored forward stop; changing *only* the measured forward-stop margin
(fixture margins 0.15 and 0.40, same announcement, same bottle
calibration, same rule geometry) moves the leg target and the push objective together
by the same 0.25 m while the rule-derived push landmark and the reverse stop do not
move; the push completes at the forward stop and not at the Bottle Line; the reverse
leg ends at the announced Target Point for two announcements and is unaffected by the
forward margin; no usable plan means no leg target and no forward evidence (including
a production-shaped refusal); the holder adopts, reads and clears one plan. The
scenario suite gains one case asserting the same identity end to end through the
harness (including that the run stayed in `Push` past the Bottle Line and short of
the forward stop), and `test_run_axis` drops the seam case. Every value is a fixture,
not a measurement.

**Software verification (not physical verification).**

- `pio test -e native`: **574/574 test cases succeeded** (568 at S3, +7 new cases,
  -1 seam case removed), 0
  warnings from changed files (the one `-Wswitch` on `FaultCode::DriverFaultBar` in
  `test_scenario`'s `faultName()` reproduces at the D38 baseline and is untouched by
  this diff).
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog` after `-t clean`: 3
  SUCCESS, 0 warnings. RAM/Flash grew by a deliberate **+136 bytes RAM** on every
  image (nano_r4 6292 -> 6428, bench 6492 -> 6628, bench-watchdog 6476 -> 6612) and
  +536/+528/+528 bytes Flash: the run layer now *holds* this run's plan and reports
  its state in the tick snapshot, which is the point of the change.
- `git diff --check`: clean. Telemetry tooling untouched and not re-run.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration and limit
  value remains `Provisional`; the `Provisional` lockout still refuses to arm.

**Next.** S5 owns truthful bottle-phase progression semantics; S6/S7 own the speed
strategy and the timing model; S8/S9 own Run Time telemetry and plan observability.
Until the forward-stop margin and the bottle-interaction distances are measured
(P-H8 plus a braking characterization), no run can travel forward. This entry does
not start any of those.
## D42 / S5 - 2026-10-05 - Truthful bottle-phase progression: the plan owns the thresholds, and no phase claims a contact

**Baseline inspected before any edit**

- Branch `main`, HEAD `c840fc2d475a293fe6f2c6740c049aa1f4fbf364` (the U1-S4
  checkpoint), tracked tree clean, 5 ahead / 0 behind `origin/main`
  (intentionally unpushed).
- S5 of the corrected stage matrix: *"position-derived belief documented;
  sensor-sounding names remain"*.

**The finding.** The run's three forward-phase exits were named like observations of
a bottle this vehicle cannot make. `RunInputs::bottleContactDetected` said a contact
had been detected, while the producer was odometry: the run axis compared against a
derived position. Nothing was dishonest about *where* the number came from
(`BottleEvidence.h` and section 14 already said it is a position-derived belief), but
the name - and therefore any log, review or reader - claimed a sensor that does not
exist. The progression was also derived twice: once by the geometry/config
derivation inside `BottleEvidence.h` and once by the run's execution plan, which
already owns the forward stop.

**What changed.**

- **Names that state what happened.** `BottleEvidence` now reports
  `approachComplete` / `captureComplete` / `pushComplete`, and the run policy
  consumes `RunInputs::approachComplete` / `captureComplete` / `pushComplete`. The
  three are *progression completions*: "the travel has got this far". None is an
  observation of the bottle, and `approachComplete` in particular is the credible
  capture window **opening** - never a claim that the bottle was touched.
- **One owner per threshold, and it is the plan.** `ev::deriveBottleEvidence(plan,
  ...)` thresholds at the plan's own `earliestContactMpPositionM`,
  `captureGuaranteedMpPositionM` and `forwardStopMpPositionM`. The geometry-coupled
  derivation and its `BottleEvidenceConfig` argument are **deleted**: with the
  thresholds coming from the plan, the configuration no longer produced them, and a
  parameter that looks like it does is the second source this stage exists to
  remove. `BottleEvidence.h` keeps the config type and its validation, which the
  plan consumes; `ev::deriveForwardProgression()` is the one comparison (leg,
  position usability, finiteness, three thresholds) and is pure.
- **The state machine is producer-agnostic.** Its contract now states that it never
  learns which strategy produced a completion and that no transition may depend on
  one: a sensor-driven or operator-driven producer later changes nothing in
  `CompetitionStateMachine.h`. Timeouts, first-fault latching, phase ownership,
  reset behavior and the stale-data refusal are untouched - a phase whose completion
  never arrives still faults with `PhaseTimeout`, and an unusable (stale, unmeasured
  polarity, unmeasured scale) position still produces no completion at all.
- **Behavioural delta, stated plainly.** `Approach` now completes at the *earliest*
  credible contact position (nominal contact minus the plan's measured window
  tolerance) instead of at nominal contact: the run begins its capture behaviour when
  the credible window opens rather than committing to one nominal position. On the
  fixture course that is 0.12 m earlier (1.88 m instead of 2.00 m, with a 0.02 m
  window and a 0.10 m lead). Capture and push completion are numerically unchanged;
  no constant in `VehicleConfig.h` moved, and no speed, timing, safety or arming
  behaviour changed.
- **No production input is permanently false.** All three completions are produced
  by the strategy from the announced run's own plan, so they become live exactly when
  the plan does (`RunExecutionPlanStatus`). Today the plan refuses - the
  bottle-interaction distances and the forward-stop margin are all `Provisional`
  placeholders - so the completions are unavailable, which is the intended
  fail-closed state and not a hard-coded `false`.
- **Wiring and mirrors.** `src/VehicleRuntime.{h,cpp}` (snapshot field
  `approachComplete`, the plan-aware derivation call) and
  `test/test_scenario/ScenarioHarness.h` follow the new names; the scenario driver
  now states all three completions from the production derivation
  (`harness.forwardBottleEvidence()`) instead of carrying its own threshold, so the
  scripted scenarios exercise the production strategy rather than a copy of it.
- **Telemetry wire schema unchanged.** `kTelemetrySchemaVersion` stays **2** and no
  wire field was added, renamed or reinterpreted: `TelemetrySample`,
  `TelemetryRecord`, `TelemetryFormat` and the analysis tooling carry no
  bottle-contact field (they carry `phase`, `position_m` and the rest), and none is
  touched by this entry. The renamed facts live in the runtime's internal tick
  snapshot only.

**Tests.**

- `test_bottle_evidence` rewritten for the new API (7 cases): one fact per threshold,
  appearing exactly at its own threshold and staying as the travel continues; the
  thresholds echoed back for diagnostics; an unusable position reporting nothing; a
  non-finite position *or* threshold reporting nothing; the reverse leg reporting
  nothing; a placeholder interaction distance producing no usable configuration; a
  negative distance refused.
- `test_execution_plan` (+3 cases): the approach completes exactly at the plan's
  earliest credible contact position, and that position is the window opening rather
  than a contact claim (earlier than nominal contact, earlier than the capture
  guarantee, capture still not complete); the capture completes exactly at the plan's
  capture guarantee, which covers the whole credible window; and the thresholds
  follow the plan's *measured* window - widening the window tolerance moves the
  approach threshold earlier while the rule landmarks and the forward stop do not
  move, and a window the capture guarantee cannot cover is refused rather than
  sequenced.
- `test_scenario` (+1 case): end to end through the real chain, the run enters
  `Capture` when the capture window opens - before the announced Bottle Start Point
  and before the capture guarantee - and the production derivation at that position
  reports approach complete, capture not complete and push not complete.
- Renames across `test_competition_state` (the fixture's `approach` fact),
  `test_scenario` and its harness; the S4-era `pushPositionM` name is gone with the
  struct field it named.

**Software verification (not physical verification).**

- `pio test -e native`: **578/578 test cases succeeded** (574 at S4, +3 plan cases,
  +1 scenario case), 0 warnings from changed files (the one `-Wswitch` on
  `FaultCode::DriverFaultBar` in `test_scenario`'s `faultName()` is pre-existing and
  untouched by this diff).
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog` after `-t clean`: 3
  SUCCESS, 0 warnings. RAM unchanged (6428 / 6628 / 6612); Flash +100/+108/+108
  bytes (nano_r4 72520, bench 79364, bench-watchdog 78792) - the three-threshold
  finiteness checks and the threshold value carried into the one derivation.
- Telemetry tooling untouched and re-run green (`116 tests OK`). `git diff --check`
  clean.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration and limit
  value remains `Provisional`; no hardware was touched and no physical behaviour is
  claimed.

**Documents corrected for truthfulness (not rewritten history).**

- `SOFTWARE_ARCHITECTURE.md` section 14: the forward phases wait for a *completion*,
  the state machine never learns its producer, and the thresholds are the execution
  plan's; section 7's "producers of the bottle-interaction evidence" item now names
  the unmeasured values that actually gate a plan; the "what the bottle line means
  for the mechanism" sentence points at the plan.
- `docs/limitations.md`: the D34 entry now says *forward progression* rather than
  "bottle evidence", states that approach complete is the window opening and not a
  contact, and says no code path reads it as one.
- `docs/bringup/P-H8-bottle-integration.md` and
  `docs/bringup/P-H14-calibration-checklist.md`: the believed values to record are
  the three progression completions, and the nominal contact position must come from
  the **observed** contact (the witness mark at its tick) - never from the `Capture`
  transition, which is now the window opening. P-H14 Step 11 also lists the execution
  plan's two own dependencies (credible contact-window tolerance and forward-stop
  margin beyond the Bottle Line) as measurements that step must produce, since no
  plan can exist without them.

**Not changed (owned elsewhere).** Speed strategy and the timing model (S6/S7):
`plannedTravelDistanceM()` still sizes the timing plan from rule geometry, the
planned cruise still comes from `planRunTiming()`, and the plan's timing inputs stay
honest (`timingReady = false`, both numbers 0.0). Run Time telemetry and plan
observability (S8/S9), `maxRunDuration` (S10) and the legal-parameter sweep (S11) are
untouched; the arming gate still does not require a usable plan, so a run announced
while the plan refuses ends in a phase timeout rather than in motion. Bottle-phase
*ordering*, timeouts and phase ownership are unchanged.

**Next.** H8 (bottle integration) must produce five measured facts before this
progression can do anything: the three bottle-interaction distances, the credible
contact-window tolerance and the forward-stop margin beyond the Bottle Line - with
the observed contact position recorded independently of the firmware. S6/S7 own the
speed/timing decision; S8/S9 the Run Time telemetry and the `plan` command.

---

## D43 / S6 - 2026-10-05 - The target shaper's optional strategy ceiling: per-phase speeds that can only tighten

**Scope.** Competition strategy may want the bottle phases (Approach, Capture,
Push) travelled slower than the announced cruise. S6 adds that as one explicit
optional input to the existing target shaper - no second controller, no bypassed
taper and no bypassed acceleration/deceleration limit. No calibration value was
tuned and no speed was invented: every production bottle-phase ceiling is a
`Provisional` placeholder and the run policy refuses while they are.

**The shaper contract (`lib/evcore/src/MotionProfile.h`).**

- New `ev::StrategyVelocityCeiling`: `active` plus `velocityMps`, with
  `ev::inactiveStrategyVelocityCeiling()` / `ev::activeStrategyVelocityCeiling(v)`,
  no default construction, and `ev::validateStrategyVelocityCeiling()` separating
  `NonFiniteValue` from `NegativeValue`. Inactive is always valid and carries
  exactly 0.0; a ceiling of zero is a legal ACTIVE value ("ask for no shaped
  magnitude"), which is why the active flag exists.
- `MotionProfileInputs` gained the ceiling as its last field, defaulted to the
  explicit inactive value so every existing call site keeps exactly its former
  shaping; `MotionProfileTarget::inputsFor()` passes it through.
  `MotionProfileCommand` reports `strategyCeilingMps` / `strategyCeilingActive`
  beside the other ceilings, with a new `MotionProfileLimit::StrategyCeiling`
  binding and a new `MotionProfileStatus::InvalidStrategyCeiling` refusal.
- `nextTargetVelocity()` computes the desired magnitude BEFORE the rate limits:
  desired = min(cruise ceiling, distance taper, strategy ceiling if active), then
  the existing acceleration/deceleration brackets produce the target. So the
  ceiling can only tighten a target, a sudden reduction is a deceleration-limited
  ramp and never a step, and an active NaN/infinite/negative ceiling refuses with a
  zero target rather than shaping as if the ceiling were absent. A stop request and
  the reversal street are untouched: both were already zero/ramp paths, and a
  reversal still reaches zero before the sign may flip.

**The run policy side (`CompetitionStateMachine.h`, `VehicleConfig.h`).**

- `RunPhaseConfig` carries three `Sourced<double>` ceilings (approach / capture /
  push), validated with the rest of the policy: `Measured`-only, finite and
  strictly positive (`NonPositiveStrategyCeiling`). They are placeholders in
  `VehicleConfig.h` (`kPlannedApproachCeilingMps` / `kPlannedCaptureCeilingMps` /
  `kPlannedPushCeilingMps`, each the configured cruise today), so `PhaseConfigValid`
  stays false and no run can be announced or armed on them - the
  unusable/provisional state the bottle-integration work must earn its way past.
- New free function `ev::strategyVelocityCeilingForPhase(config, phase)`: Approach
  -> the approach ceiling, Capture -> capture, Push -> push, every other phase ->
  explicitly inactive. The state machine never reads the ceilings; the vehicle
  layer and the scenario harness select through this one mapping, and the harness
  mirrors the production call exactly.
- Production wiring: `VehicleRuntime` passes the phase's ceiling into the shaper
  every cycle (stage 6), so an active ceiling tightens through the same taper and
  rate limits every other target goes through, and `SafetyAuthority` and the FF+PI
  regulator are untouched (the ceiling limits the target, never the effort path).

**Proof that strategy only tightens motion (host tests, not physical).**

- `test_motion_profile` +9 cases: inactive changes nothing (default form and
  explicit form identical); a ceiling above the desired target changes nothing
  (identical to the inactive baseline at the same state); a lower ceiling only
  tightens (deceleration-limited first step, then the target sits at the ceiling
  and the binding names it); NaN/inf/negative ACTIVE ceilings fail closed with
  `InvalidStrategyCeiling` and a zero target while zero stays legal; a sudden
  reduction is ramped at the deceleration limit, never stepped; a sweep of
  ceilings x currents x distances never raises a target above the no-ceiling
  baseline and never above the previous target when descending; a zero ceiling
  ramps to exactly no powered effort; a stop request is unchanged by an active
  ceiling; and a full reversal with an active ceiling still ramps to zero before
  the sign may flip and is then capped on the new side.
- `test_competition_state` +2 cases: the per-phase selection maps only the three
  bottle phases (distinct fixtures prove which ceiling each phase gets); and a
  provisional, zero or NaN phase ceiling is refused by `validateRunPhaseConfig()`
  with its own named reason, so no run can be armed on a placeholder.
- `test_scenario` +1 case: two identical scripted runs, one with a 0.30 m/s
  Capture ceiling, produce the same trace length and the same phase every cycle;
  every cycle the slowed run's shaped target is at or below the nominal one;
  Capture cycles are tightened while Approach is not; the entry into Capture is
  bounded by one cycle of deceleration (no step); and the slowed run still reaches
  Push - the ceiling delays a phase instead of wedging the run. The harness gained
  one recorded field (`CycleRecord::shapedTargetVelocityMps`) for that proof.

**Software verification (not physical verification).**

- `pio test -e native`: **590/590 test cases succeeded** (578 at S5, +12: nine
  `test_motion_profile` cases, two `test_competition_state` cases and one
  `test_scenario` case), no new warnings (the one `-Wswitch` on
  `FaultCode::DriverFaultBar` in the scenario helper is pre-existing and
  untouched).
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog`: 3 SUCCESS, 0 warnings.
  RAM +48 bytes on all three (nano_r4 6476, bench 6676, bench-watchdog 6660 of
  32768) and Flash +548/+540/+540 bytes (nano_r4 73068, bench 79904,
  bench-watchdog 79332): the three per-phase `Sourced` ceilings held by the run
  policy, the optional input carried in the shaping inputs/command, and the
  mapping/validation code - deliberate and small.
- Telemetry tooling untouched and re-run green (`116 tests OK`); the telemetry
  wire schema is unchanged (`kTelemetrySchemaVersion` stays 2; no check name,
  field or event changed). `git diff --check` clean.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; every calibration and
  limit value remains `Provisional`; no hardware was touched and no physical
  behaviour is claimed.

**Documents.** `SOFTWARE_ARCHITECTURE.md` section 7 now states the strategy
ceiling in the target-shaping contract (desired-min before the rate limits, the
per-phase mapping, the explicit inactive phases, the provisional production
values). `docs/bringup/P-H8-bottle-integration.md` and its P-H14 Step 11 list the
three ceilings among the facts that step must establish before a run may be armed,
not only among the bottle distances and the plan's two dependencies.

**Not changed (owned elsewhere).** Timing ownership and the idealized timing model
(S7): `planRunTiming()` still controls cruise/delay at the announcement boundary
and the `timingReady` policy is untouched. Run Time telemetry and plan
observability (S8/S9), `maxRunDuration` (S10), the legal-parameter sweep (S11) and
the remaining documentation synchronisation (S12) are untouched. No stop, reversal,
fault, arming or telemetry behaviour changed.

**Next.** H8 must also establish the three bottle-phase strategy ceilings (with the
five facts it already owes). They remain unusable - the policy refuses and no run
may be armed - until each is measured and promoted through the normal evidence
procedure. S7 owns the timing decision; S8/S9 the Run Time telemetry and the
`plan` command.

## D44 / S7 - 2026-10-05 - The deliberate delay belongs to the run's execution plan, and the kinematic estimate is diagnostic

**Scope.** S7 moves the run's deliberate start delay into the immutable per-run
`RunExecutionPlan` and stops the unvalidated idealized kinematic model
(`planRunTiming()`) from automatically controlling competition timing. No
calibration value was tuned, no timing constant was invented, and no physical
behaviour is claimed: the physical timing model remains unvalidated
(`kRunTimingModelValidated = false`), so production's cruise stays the configured
strategy cruise and its delay is 0.

**The delay has one owner: the run's execution plan.**

- `RunExecutionPlan::initialDelayS` is now the one per-run delay value. The
  announcement boundary builds the run's plan **before** announcing it and hands
  the plan's own `initialDelayS` to `CompetitionStateMachine::announceRun()`; the
  `OptionalDelay` phase spends exactly that value.
- The phase policy's `RunPhaseConfig::maximumOptionalDelayS` is documented - and
  now used - as only the legal/safety maximum that bounds the plan's value. It no
  longer names a plan, and nothing reads it as a delay.
- The delay's vocabulary moved with it: `CompetitionStateMachine::plannedDelayS()`
  is `initialDelayS()`, the announce parameter and member are `initialDelayS`, and
  `RunStartStatus::PlannedDelayNotUsable` is `InitialDelayNotUsable` (the operator
  reason string is `InitialDelayNotUsable`). `VehicleRuntime` no longer keeps a
  second copy of a decided delay.

**The idealized estimate is diagnostic until measured.**

- `MotionProfile.h` labels `planRunTiming()` and its `RunTimingPlan` output as the
  **diagnostic** kinematic estimate: it says what the announced Target Time,
  geometry and limits imply, and it commands nothing.
- The announcement gate no longer refuses a legal run for an unreachable Target
  Time. `AnnounceRefusal::TargetTimeUnreachable`, its name, and its refusal branch
  in `formatAnnounceRefusal()` are **deleted**: an unvalidated estimate must not
  invalidate an otherwise legal run. The estimate still travels in
  `AnnounceVerdict::plan` and is echoed by `OK,cmd=announce` (the wire shape of the
  acknowledgement is unchanged; its comment now says it is a diagnostic, not what
  the run will do - the run's delay is the plan's and its cruise is the configured
  one).
- One production readiness flag, `vehicleConfig::kRunTimingModelValidated = false`,
  gates everything: `applyAnnounce()` sets `timingReady` only when the flag is set
  **and** the estimate is valid, so it uses the estimate's cruise and delay only
  then; otherwise it uses `profileLimits()` and records
  `timingReady = false` / `initialDelayS = 0` in the run's plan. The estimate is
  still computed and stored in `timingPlan_` for the announcement acknowledgement.
- The one new production value is a `constexpr bool`; no `Sourced` calibration
  value was added or moved.

**Tests (software verification only; every number is a synthetic fixture).**

- `test_execution_plan` +1 case: two synthetic **validated** timing-model fixtures
  (measured limits, 25.0 s and 12.0 s Target Times on the 9.50 m fixture course)
  produce different model delays, and each lands in the plan as its own
  `initialDelayS`; the same estimates with timing unvalidated produce
  `timingReady = false` and `initialDelayS = 0.0`; a negative or NaN delay with
  timing ready is refused (`NonFiniteValue`); valid delays are nonnegative.
- `test_run_timing`: the delay test now builds two plans (1.0 s and 1.5 s), shows
  the over-maximum and a negative value are refused with `InitialDelayNotUsable`
  and latch nothing, and shows the `OptionalDelay` phase runs for exactly the
  plan's 1.0 s and no longer - the policy maximum is only a bound. Its header
  comment now describes the estimate as diagnostic and the delay as plan-owned.
- `test_operator_policy`: the unreachable-Target-Time test is rewritten to assert
  the run is **not** refused (`ok`, `None`) while the estimate's status still
  travels with the verdict. That case existed but was never registered in
  `main()`; it is registered now, so the acceptance property is actually executed.
- `test_operator_response`: the same property in the response layer - the gate
  accepts the announcement, whose estimate may still say `TargetTimeTooShort`.
- `test_scenario` harness: the fixture announcement now builds its plan first
  (timing-ready, fixture values) and announces the plan's `initialDelayS`, so the
  nominal scenarios keep their `OptionalDelay` phase exactly as before while
  modelling the validated-timing case rather than a raw policy number.
- `test_competition_state`: comments only (the fixture's announced delay is the
  plan's value; the policy maximum is a bound).

**Software verification.**

- `pio test -e native`: **592/592 test cases succeeded** (590 at S6; +1 new
  `test_execution_plan` case and +1 previously-unregistered operator-policy case
  now running), 33 suites, no new warnings.
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog`: 3 SUCCESS, 0 warnings.
  RAM 6476 / 6676 / 6660 (unchanged from S6) and Flash 72852 / 79700 / 79128
  (S6 values minus 216 / 204 / 204 bytes: the removed unreachable refusal branch
  and its status name cost more than the added plan gating).
- Telemetry tooling re-run green (**116 tests OK**); the telemetry wire schema is
  unchanged (`kTelemetrySchemaVersion` stays 2; no check name, field or event
  changed). `git diff --check` clean.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; no hardware was touched
  and no physical behaviour is claimed.

**Documents.** `SOFTWARE_ARCHITECTURE.md` section 7 now states the diagnostic
estimate and the plan-owned delay; `docs/limitations.md` entry A records that the
timing model is disabled until a realised-Run-Time comparison exists; and
`docs/bringup/P-H14-calibration-checklist.md` Step 12 gained the enablement
condition - the step's records plus promoted limits come first, and
`kRunTimingModelValidated` is set true afterwards as its own reviewed change.

**Not changed (owned elsewhere).** Official Run Time telemetry (S8), the `plan`
observability command (S9), the `maxRunDuration` runtime consumer (S10), the
legal-parameter host sweep (S11) and the remaining documentation synchronisation
(S12) are untouched. No stop, reversal, fault, arming, safety-authority or
telemetry behaviour changed.

**What would set `timingReady = true`.** Not a constant edit. P-H14 step 12 must
record several announced Target Times on a marked course together with the
realised Run Time and the raw data preserved, and the acceleration, deceleration
and cruise limits the estimate uses must be promoted from `Provisional` to
`Measured` through the normal evidence procedure. Only then may
`kRunTimingModelValidated` be set true, and the estimate's cruise/delay may
influence a run.
## D45 / S8 - 2026-10-05 - The run's own Run Time ends at the verified final-stop dwell start, not at Done

**Scope.** S8 records the run's own timeline so the event's Run Time (rule 8.o,
actuation to the complete stop) and Complete Stop (rule 8.p, the stationary
interval that is not part of Run Time) can be read from a log without
reconstructing either from the armed flag. The internal Run Time is defined as
`dwell_start_ms - run_start_ms`: actuation to the start of the **verified**
final-stop dwell, so the rule's three-second confirmation interval is
deliberately outside it. The firmware records readings; it derives no duration
in any control path. No calibration value was tuned, no physical behaviour is
claimed, and the external timekeeper or video remains the official Run Time.

**Timestamp ownership: one record, in the run policy.**

- `ev::RunTimeline` (`lib/evcore/src/CompetitionStateMachine.h`) is the single
  owner of the three readings. `CompetitionStateMachine::timeline()` returns a
  copy; `VehicleRuntime` copies it into `TelemetrySnapshot` and then into the
  telemetry sample, so the sample cannot disagree with the run that produced it.
- **Actuation** is stamped once, in the tick the start trigger is seen, before
  the phase moves to `OptionalDelay` or `Approach`. The run's deliberate
  pre-travel delay is therefore inside the run's own time, as the event counts
  it. A fault keeps it: the run did begin.
- **The dwell start** is read from the verifier that owns the interval, not
  reconstructed from an elapsed span. `NearZeroVerifier` gained two read-only
  accessors - `intervalOpen()` (`hasNearZeroSince_`) and `intervalStartedAtMs()`
  (`nearZeroSinceMs_`) - so a broken observation clears the reading and the gap
  rule's restart reports its new start, both without the state machine
  re-deriving "is an interval open" from a status it might classify wrongly.
- **The verified start** is latched only by the cycle that earned the verdict,
  together with the verification completion reading for that same cycle (which is
  also the tick that entered `Done`). `RunTimeline::finalStopVerified` is a
  separate latch from the presentation's `RunStatus::stopVerified`: a later fault
  discards an in-progress **candidate** (nothing was verified, so there is no fact
  to keep) but cannot erase a final stop the run completed. Only `reset()` clears
  the record, so a new run cannot inherit a previous run's readings.

**Schema impact: version 3, and the run timeline on the sample.**

- `ev::kTelemetrySchemaVersion` is 3 and the prefixes are `EVS3`/`EVE3`. Each
  sample gained six fields in the `pos_m`/`pos_ok` shape: `run_start_ms` /
  `run_start_ok`, `dwell_start_ms` / `dwell_start_ok`, `stop_verified_ms` /
  `stop_verified_ok`. The fields are always written, so "no run has been
  actuated" is a zero next to a `0` flag rather than a missing column, and a
  version-2 log's missing columns are not readable as "the run never verified a
  final stop" - which is what the bump states.
- The sample's derived worst case grew from 438 to 567 characters, so
  `ev::kSerialLineCapacity` is 576 (was 448). The bench console's own derived
  worst case (534, D31) now fits with 42 characters to spare; its comment in
  `src/bench/BenchConsole.h` and the entry in `docs/limitations.md` say so, and
  the owed `health` split is still owed because that line still has no
  compile-time bound behind it.
- The operator wire shape is otherwise untouched: `STATUS` carries no time
  fields, and the help table's records line no longer names a schema digit it
  would have to be edited to keep true.

**Telemetry tooling follows the firmware, and stops substituting the armed window.**

- `tools/telemetry` mirrors version 3: the six fields parse (`parse.Sample`), the
  schema pins them (`schema.py`, `test_schema_mirror.py`), and the synthetic
  generator writes them from its own modelled final-stop interval
  (`SYNTHETIC_COMPLETE_STOP_MS`), exposing `final_stop_timeline()` so a test can
  compare tool numbers against the fixture's own readings.
- `run_window.internal_run_time_s` is the unsigned difference actuation to
  verified dwell start (taken modulo 2**32, the same arithmetic the firmware
  uses, so a wrap cannot produce a negative duration), and
  `final_stop_verification_completed_s` is the same difference to the completion
  reading. A run with no verified final stop reports **both as `None`**: the
  endpoint comparison no longer substitutes `armed_duration_s` (or the log
  duration) for the Run Time, which was the gap still live on `main`. The
  comparison table's `dur_s` column is now `run_s` and carries the internal Run
  Time, and the JSON report carries both new numbers.

**Tests (software verification only; every number is a synthetic fixture).**

- `test_competition_state` +5 cases: the timeline records actuation, the dwell
  start and the verification completion; a broken final-stop dwell is discarded
  and restarted with a new start reading; the readings stay correct across the
  `uint32` wrap; `reset()` clears the record (and a new run starts empty); a
  fault discards a candidate but keeps a verified record.
- `test_near_zero_verifier` + cases for `intervalOpen()`/`intervalStartedAtMs()`
  on a fresh interval, a held (frozen-stamp) interval, a discarded interval and a
  restarted one.
- `test_telemetry_format` pins the six new fields and their spelling, and the
  sample's declared worst case against the line capacity.
- `test_operator_response` re-pins the version line to `telemetry_schema=3`
  (the expected strings had stayed at 2 while the constant moved).
- Telemetry tooling: a parse case for the new readings and flags (including a
  reading wider than its firmware type and a misspelled flag), and metrics cases
  pinning the internal Run Time against the fixture's own timeline - including
  that `final_stop_verification_completed_s - internal_run_time_s` is exactly
  3.0 s, which is the acceptance property that the rule's confirmation interval
  is not counted - plus that a faulted run reports no Run Time at all.

**Software verification.**

- `pio test -e native`: **599/599 test cases succeeded** (592 at S7), 33 suites,
  no new warnings.
- `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog` from clean: 3 SUCCESS,
  0 warnings. RAM 6772 / 6972 / 6956 (+296 each: the shared line buffer grew by
  129 bytes and each image holds two of them) and Flash 73244 / 80088 / 79516
  (+392 / +388 / +388: the six field names, the timeline record and the new
  accessors).
- Telemetry tooling: **119 tests OK** (116 at S7), including the schema mirror
  that reads `TelemetryRecord.h` itself. Synthetic fixtures regenerated and
  pinned byte-for-byte by the drift test. `git diff --check` clean; every
  changed file is 100% CRLF.
- All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED; no hardware was touched
  and no physical behaviour is claimed.

**Documents.** `SOFTWARE_ARCHITECTURE.md` section 9 states the version, the run
timeline on the sample and where the internal Run Time ends;
`docs/limitations.md` records the metric as recorded but never compared against
the official one; and P-H14's Step 11 no longer names a record prefix the
firmware stopped emitting (the remaining pinned `EVS1`/`EVE1` mentions in the
older bring-up documents are documentation synchronisation, owned by S12).

**Not changed (owned elsewhere).** The operator `plan` output (S9), the
`maxRunDuration` runtime consumer (S10), the legal-parameter host sweep (S11) and
the remaining documentation synchronisation (S12) are untouched. No stop,
reversal, fault, arming, safety-authority or control behaviour changed: `Done`
is entered exactly as before, and the rule's 3-second confirmation interval was
not shortened, lengthened or made configurable.

**Why external timing is still required in H10.** The internal Run Time is what
the firmware believed on its own `millis()` clock: it is a `millis()` interval,
not a measurement of the vehicle, and it cannot see a start trigger pressed late,
a delay in the drivetrain, or how the officials time the run. H10 must record the
official time (timekeeper or video) **and** the internal metric for the same
runs, side by side, and only a series of those pairs can say whether the internal
number is usable - the same evidentiary rule the timing model itself is held to.
## D46 / S9 - 2026-10-05 - A read-only plan report: the operator can read the exact plan the run holds, or the named reason there is none

**Scope.** S9 adds one inspection command, `plan`, that renders the run layer's
current immutable `RunExecutionPlan` as a single bounded `PLAN` line: the announced
rule inputs and landmarks, the plan's execution targets, contact window and capture
point, its timing state and its deliberate start delay - or the plan's own named
status when there is no usable plan. It is read-only in the strongest sense: the
formatter is handed a value copy of facts the run layer already holds, the accessor
is `const`, nothing in the path adopts, clears or adjusts a plan, and the command
is admissible in every state - including while a run is under way - so reading the
plan can never wait on control or gate it. No calibration value changed, no
physical behaviour is claimed, and no hardware was touched.

**The command, in the three modules that own it.**

- Parser (`SerialCommandParser.h`): `OperatorCommandId::Plan`, spelled `plan`, one
  token exactly (`plan extra` is `WrongTokenCount`), and `operatorCommandName()`
  returns `plan`.
- Policy (`OperatorPolicy.h`): `isInspectionCommand(Plan)` is true, so
  `admissibilityOf()` admits it in every phase, armed or faulted, exactly like
  `status` and `checks`. It is not a telemetry command and it is not in the
  vocabulary that changes run-control state.
- Runtime (`VehicleRuntime.h/.cpp`): `planView()`, a `const` method, gathers the
  view from `executionPlan_.plan()` and the announced rule geometry and Target
  Time; `applyCommand()` adds the one case that formats and stages the line. The
  report goes out through the same bounded, non-blocking `respondLine()` every
  other response uses.

**The output contract** (one line, prefix `PLAN`, fixed field order; a field that
moves breaks a test):

```
PLAN,ready=<0|1>,reason=<name|none>,target_m=<v>,time_s=<v>,line_dist_m=<v>,
bottle_start_m=<v>,bottle_line_m=<v>,window_lo_m=<v>,window_hi_m=<v>,
capture_m=<v>,fwd_stop_m=<v>,rev_stop_m=<v>,timing_ready=<0|1>,
predicted_s=<v|->,delay_s=<v|->
```

- `ready`/`reason`: whether a usable execution plan exists and, when it does not,
  the named refusal in the plan module's own vocabulary
  (`runExecutionPlanStatusName()`), with `reason=none` when a plan exists - the
  `first=none` convention the check report already uses. `ready` is plan
  readiness, not arming readiness: a run can be `ready=1` and unarmable, and that
  is what `CHECK`/`arm` report.
- The rule inputs (`target_m`, `time_s`, `line_dist_m`) and the two rule landmarks
  (`bottle_start_m`, `bottle_line_m`, the latter derived as the Target Point plus
  the announced Bottle Line Distance) are reported whenever a run has been
  announced, **even when the plan refused**: the geometry is a fact about the
  announcement, so "the geometry is what you typed, and the plan refuses for this
  named reason" reads off one line.
- `window_lo_m`/`window_hi_m` are the credible contact window (the earliest
  approach completion and the latest credible contact), `capture_m` is the
  capture guarantee, `fwd_stop_m` is the run's single forward execution target
  (Bottle Line plus the measured margin) and `rev_stop_m` is the announced Target
  Point the reverse leg ends at.
- Everything that describes the plan is a one-character absent marker when there
  is no usable plan, never a zero: a zero position would read as a landmark at the
  Start Point, which is the one reading a diagnostic must not invent.
- `predicted_s` is marked absent when the plan is not timing-ready, because the
  plan's own contract says its `0.0` means "not available"; `delay_s` is a real
  value whenever a plan exists, including the production `0.0000` - the plan does
  state the delay the run will spend, and it is zero.

**Boundedness.** `kOperatorPlanLineWorstCaseChars` is **340**, derived field by
field in the header: twelve numeric fields at `BoundedText::appendFixed()`'s
13-character bound plus the keys and the `PLAN,ready=1,reason=none` summary. It is
an upper bound *and* it is reachable - all three geometry fields can render at 13
characters at once (`target_m=-6000000.0000`, `line_dist_m=-3000000.0000`, so
`bottle_line_m=-9000000.0000`) - while the widest refusal form is far narrower
despite carrying the 35-character `MissingBottleInteractionCalibration`. A
`static_assert` holds it under `kSerialLineCapacity` (576), and the worst-case
test pins the number, the reachable form and both refusal forms.

**Tests (+6 test cases; native 599 -> 605).**

- `test_serial_command_parser`: `plan` is in the accepted-vocabulary pin, and
  `plan extra` is `WrongTokenCount`.
- `test_operator_policy`: the dedicated
  `test_the_plan_report_is_read_only_and_ungated` pins that `plan` is inspection
  (not telemetry) and that every phase, armed and faulted, admits it - the state
  restriction answered for the new command rather than assumed.
- `test_operator_response` (+4): the ready form pinned byte for byte (including
  the derived Bottle Line), the refusal forms pinned with the named reason and the
  absent markers (announced-and-refused, and nothing announced), the separation of
  an absent predicted time from a real zero delay, output stability (formatting a
  view twice produces identical bytes, and a line that does not fit produces
  nothing), the field order asserted for every form, the widest form pinned to
  `kOperatorPlanLineWorstCaseChars`, and the help table checked to list `plan`.
- `test_serial_independence` (+1): the scenario types `plan` before any
  announcement and again while the run's phases are under way, and asserts both
  were admitted (a complete `PLAN` line) while the recorded control trace stayed
  bit-identical to the run with no serial consumer at all - the S9 acceptance
  evidence that disconnecting serial cannot change an outcome, now with the new
  command in the stream.

**Validation (clean build, on the frozen tree).** Native **605/605** in 33 suites,
exit 0; `nano_r4`, `bench` and `bench-watchdog` all SUCCESS with 0 warnings from a
clean build, RAM 6772/6972/6956 (unchanged from S8) and Flash 74404/81236/80664
(+1160/+1148/+1148: the new formatter and its string table); `tools/telemetry`
**119/119 OK** (untouched by this stage); `git diff --check` clean; every changed
file 100% CRLF.

**Not changed (owned elsewhere).** `maxRunDuration` (S10), the legal-parameter
sweep (S11) and the remaining documentation synchronisation (S12) are untouched,
as is every safety, stop, reversal, fault and arming behaviour. The telemetry wire
schema stays **v3** - the plan report is an operator response, not a sample field.
No `.agent-work` material, no evidence promotion: all twelve `VERIFIED_STATE.md`
rows remain UNVERIFIED.

**What this stage does not claim.** The report says what the firmware's plan is;
it does not make that plan usable. With the provisional configuration the plan
still refuses with `MissingForwardExecutionTarget`, and `plan` is exactly the
command that says so from the vehicle. Physical validation of anything the plan
describes remains H8's work.

## D47 / S10 - 2026-10-05 - The maximum run duration has a runtime consumer, and an absent battery sensor is a capability, not a missing measurement

S10 closes two real safety-policy gaps without fabricating a sensor or weakening a
gate. Neither gap could be closed by relabelling a value: the first had no runtime
consumer at all, and the second could not be satisfied by any measurement this
vehicle is able to take.

### A. The global run-duration bound

**The gap.** `SafetyLimits::maxRunDurationS` had only a plan-time consumer:
`planRunTiming()` refuses a plan whose intended total exceeds it
(`PlanExceedsMaxRunDuration`). That estimate is diagnostic (S7), and no runtime
path bounded a run that was already going. The per-phase bounds
(`RunPhaseConfig`) catch a phase that never received its evidence; nothing caught
the run as a whole, so a run that changed phase often enough could outlast any
single phase's bound with no global limit at all.

**The change.** One new pure module, `ev::RunDurationGuard`
(`lib/evcore/src/RunDurationGuard.h`), is the **run-scoped elapsed timer**:

- `ev::runDurationLimitUsable()` makes the bound conditional on a `Measured`,
  finite, positive duration. A placeholder is data with no runtime effect, so a
  guessed limit cannot stop a legal run - and it cannot be armed on either
  (`SafetyLimitsValid` still requires `maxRunDurationS` to be measured).
- `ev::runDurationVerdict()` is the whole evaluation: an unsigned
  `nowMs - runStartedAtMs` (so it is correct across the ~49.7-day `millis()` wrap),
  the bound converted through the one shared `msFromSeconds()` conversion, and an
  inclusive `>=` comparison. A stamp from the future reads as over the bound - the
  same safe direction `sampleAge()` takes.
- `ev::runDurationFault()` maps the verdict onto one identity,
  `FaultCode::RunDurationExceeded`, and nothing else.

The module holds **no start of its own**. The run's actuation reading has one
owner, `RunTimeline::actuatedAtMs` (S8), and the interval the bound applies to has
one definition, `ev::runDurationClockActive()` (`CompetitionStateMachine.h`):
actuated, not `Done`, not faulted. That interval deliberately includes the
deliberate pre-travel delay (the run's own time begins at actuation, rule 8.o)
and ends at `Done`/`Fault`, so a boundary (`reset()`) ends it and a new actuation
begins a fresh one by construction - there is no path by which one run inherits
another's elapsed time.

**The wiring.** The vehicle layer evaluates the verdict in stage 4
(`FaultEvaluation`) and merges it into this cycle's observed fault
(`ev::RunDurationGuard.h` is why the arithmetic lives in `lib/`): the run policy
latches it as its fault of record through the ordinary `RunInputs::detectedFault`
path, and `SafetyAuthority` latches it too and answers with its single
deterministic fault stop (`Disable`, `ForcedFaultStop`). A driver fault is kept
first so a real electrical fault is never masked by a run that also overran. The
bound is computed inside the control tick, in every phase - it is independent of
the per-phase timeouts and of the serial interface by construction, not by
discipline. `faultStopPolicyFor()` maps the new identity to `Disable`.

### B. The absent battery sensor is a capability

**The audit.** On this hardware `SafetyLimitsValid` (`validateSafetyLimits()`) was
**permanently impossible**: it required `SafetyLimits::isFullyEstablished()`, which
required `minBatteryV.isMeasured()`, and the assembled vehicle has no battery-sense
circuit - `safetyLimits()` supplies `provisionalValue(kPlannedMinBatteryV)`, so the
value could only ever become `Measured` by fabricating a measurement. Since
`SafetyReadiness::limitsUsable` is `checks.safetyLimitsValid`, the arming gate was
permanently impossible for a quantity the vehicle cannot sense. (`RunPolicyUsable`
is a separate fact - `CompetitionStateMachine::configUsable()` - and does not
depend on the battery value at all; its own refusal comes from the provisional
phase bounds.)

**The policy.** `ev::BatteryVoltageCapability` (`SafetyLimits.h`) states whether a
battery-sense circuit exists, exactly as `DriverFaultCapability` (D32) states the
same kind of fact about the external nFAULT line. `validateSafetyLimits()` is now
capability-aware:

| Capability | `minBatteryV` | Verdict |
| --- | --- | --- |
| `Installed` | `Measured` | pass (unchanged from before) |
| `Installed` | placeholder | refuse `NotFullyMeasured` (unchanged from before) |
| `NotInstalled` | any value | provenance **not applicable** - no circuit to read, so no voltage is required |

`Installed` is the zero value and the default argument, so a forgotten assignment
keeps the voltage gate *required*; only an explicit statement that the circuit does
not exist may pass without a measured voltage. The numeric checks stay
unconditional (a non-positive or non-finite ceiling is malformed whatever the
capability) and no other limit is relaxed: `maxRunDurationS`, the velocity
ceilings and the effort ceiling are still required. Nothing is relabelled and no
A1/A2 count becomes a measurement; `vehicleConfig::kPlannedBatteryVoltageCapability`
is `NotInstalled`, and a future build that installs the sensing must set it back to
`Installed`, measure the pack, and build the runtime comparison separately
(`docs/limitations.md`).

### Telemetry, vocabulary and wire schema

`FaultCode::RunDurationExceeded` is added to the existing fault vocabulary, with its
name added to `telemetryFaultName()` and to the mirrored Python `schema.FAULTS`
(pinned by `test_schema_mirror`). The wire schema stays **v3**: the change *gains* a
value of an existing field, it renames nothing, and it adds no field - the
project's rule is that a vocabulary may gain entries but not rename one.
`kTelemetrySchemaVersion` is therefore unchanged.

### Tests

- **New suite `test_run_duration_guard` (+8 cases, pure module):** usability of the
  limit (Measured/Provisional/RuleDerived/Derived, NaN/inf/zero/negative); an
  inactive run reports no elapsed time and never exceeds; the elapsed time across a
  `uint32` wrap (512 ms across `0xFFFFFF00 -> 0x00000100`); the bound is inclusive
  at exactly the limit; a placeholder never bounds a run; a future stamp reads as
  over the bound; the identity is its own and not `PhaseTimeout`; and a re-stamped
  start (reset/re-arm) reports a fresh, short run.
- **`test_core_types` (+2):** the capability-aware `isFullyEstablished()` /
  `validateSafetyLimits()` - `Installed` refuses a placeholder battery voltage and
  `NotInstalled` does not; the default is the conservative `Installed`; the other
  limits stay required; malformed numeric values still refuse; and both capability
  spellings are stable.
- **`test_scenario` (+1, whole chain):** a run whose fixture maximum is a
  `Measured` 1.0 s (twenty times shorter than the 5.0 s approach bound) is driven
  into `Approach` and then starved; it must fault with `RunDurationExceeded` (not
  `PhaseTimeout`), with the authority's own `ForcedFaultStop`, no effort and no
  direction, and the fault must arrive far inside the approach bound's window. The
  fault-identity sweep also injects the new identity in every phase.
- **`test_competition_state` (+1 assertion):** `faultStopPolicyFor(RunDurationExceeded)`
  is `Disable`, alongside the other run-policy faults.
- **`test_telemetry_format`:** the fault-name loop now covers the whole vocabulary
  (it previously stopped at `RunSequencingError`, leaving the last two unexamined);
  the longest-name pin (22, `ProvisionalCalibration`) is unchanged.
- **`tools/telemetry/tests/test_schema_mirror`** passes with the new fault name -
  the C++ vocabulary and the Python mirror agree.

### Validation (clean build, on the frozen tree)

Native **616/616** test cases in 34 suites, exit 0, 0 warnings (605 at S9; +11: the
8 new guard cases, 2 core-types cases and 1 scenario case). `nano_r4`, `bench` and
`bench-watchdog` all SUCCESS with 0 warnings from a clean build: RAM
6772/6972/6956 (unchanged) and Flash 74540/81380/80808 (+136/+144/+144: the guard,
the new fault name and the capability name). `tools/telemetry` **119/119 OK**.
`git diff --check` clean; every changed file 100% CRLF.

### Not changed

The legal-parameter host sweep (S11) and the remaining documentation
synchronisation (S12) are untouched. The telemetry wire schema stays **v3**. No
calibration or limit value moved, no evidence was promoted (all twelve
`VERIFIED_STATE.md` rows remain UNVERIFIED), no hardware was touched and no
measurement was taken.

### What this stage does not claim

The run-duration bound has **never been observed on a vehicle**: while
`kPlannedMaxRunDurationS` is a placeholder the bound is inert, and no run can be
armed to test it. The battery-voltage gate is **N/A on this build, not enforced**:
nothing can read the pack voltage, and the runtime comparison a real gate would
need does not exist. What S10 establishes is that the run-duration bound has a
runtime path, that a run-duration overrun is a named fault with a deterministic
stop, and that the arming gate is no longer permanently impossible because of a
quantity this vehicle cannot sense.

---

## D48 / S11 - 2026-10-06 - The whole legal announced grid is swept on the host, and the real chain is driven to Done at every level

S11 closes the gap between "the arithmetic is unit-tested at its boundaries" and
"the arithmetic holds over the whole space the rules allow". It is a test-only
stage: no production file changed, no calibration or limit value moved, no
evidence was promoted and no hardware was touched.

### What was built

A new host suite `test/test_legal_sweep` (the 35th native suite, 12 cases) with
three parts.

**1. A full sweep of the announced space.** The grids come from the repository's
rules source (`ev::rules2027`, `lib/evcore/src/RunConfig.h`, the code's copy of
the official 2027 Division C Electric Vehicle rules): Target Distance 5.00-9.00 m
on the level's own interval (0.25 m Regional/Invitational, 0.10 m State, 0.01 m
National), Bottle Line Distance 1.00-3.00 m on the same interval, Target Time
10.0-20.0 s on the 0.5 s grid, and the level's fixed bottle placement (rule
7.c.ii.(5)). Every combination of all three is built through the production
`ev::buildRunExecutionPlan()` with the scenario harness's own calibrated fixtures:

- Regional 17 x 9 x 21 = 3,213 cases
- State    41 x 21 x 21 = 18,081 cases
- National 401 x 201 x 21 = 1,692,621 cases
- total **1,713,915**, plus 20,000 deterministic random on-grid intermediates

Each case asserts: the configuration validates; the plan is produced; every value
is finite; no distance is negative and the delay is nonnegative; the landmark
ordering invariant holds; the credible contact window is the announced tolerance
either side of the nominal contact position and is not inverted; the forward
execution target lies beyond the required capture region and at or beyond the push
objective; the reverse target is exactly the announced Target Point; the derived
positions are the documented arithmetic on their inputs; the plan's geometry
carries the announced rule values; **one forward endpoint** (the motion shaper's
leg target and the state progression's completion threshold are the same stored
value, and the progression really completes there and not a millimetre earlier);
no value exceeds a sane bound; and a second construction is bit-for-bit identical.

**2. The real chain, not a second copy of a formula.** Fifteen representative
configurations (the minimum, maximum and a shared interior point of every level's
grid, plus two deterministic intermediates per level) are announced and armed,
then driven through `evscenario::ScenarioHarness` - decoder, odometry, estimator,
run policy, target shaping, regulator, safety boundary, actuator boundary - to
`Done`. Each asserts the one phase order the run policy allows, no fault, a
verified final stop, no dropped sample, the forward travel reaching the plan's own
forward stop (and not far past it), the reverse leg returning to the announced
Target Point within one control cycle, the boundary's forward-then-reverse
direction order, and the whole-trace architecture invariants (one request per
cycle, every drive authorized for its own cycle, every drive along the run's own
leg, no powered reversal without a presented stop at a reported zero, `Fault` and
`Done` safe). Four of those configurations are run twice and every recorded field
compared.

**3. Invalid and fuzz-style cases.** Named illegal announcements (off-grid,
out-of-range, non-finite, another level's bottle placement, bottle at the Target
Point) are refused with `InvalidAnnouncement`; the execution dependencies are
refused by name (`MissingBottleInteractionCalibration`,
`MissingForwardExecutionTarget`, `NonFiniteValue`,
`ContradictoryExecutionGeometry`) and accepted at their exact boundaries (a window
exactly as wide as the capture advance, a zero-width window, a forward stop
exactly at the push objective, a zero push margin); 3,000 deterministic fuzzed
configurations and 2,000 fuzzed dependency sets never produce a usable plan; and a
refused plan is proven to carry no landmark, no leg target and no progression
evidence, with the holder's `NotPlanned` lifecycle pinned.

### Measured coverage and runtime

The sweep prints its own totals: grid **1,713,915** cases in **5.3-7.8 s** across
two runs on this machine, 20,000 random cases in **66-118 ms**, 5,000 fuzz cases
in **10-14 ms**, and 23 end-to-end scenario runs in **193-293 ms**. The suite
itself takes about 11 s and the full native run 63-73 s, so the sweep runs with
every native run rather than behind a flag.

### Not changed

No production file, configuration or calibration value, telemetry wire schema
(still **v3**), `VERIFIED_STATE.md` row or hardware was touched. S11 produces **no
physical evidence**: every "measured" number in the sweep is a test fixture, the
sweep reports only what the production arithmetic and the state machine do with
stated evidence, and no synthetic result may be promoted. The documentation
synchronisation (S12) and the open arming-gate question were not touched.

### Validation (on the frozen tree)

Native **628/628** in 35 suites (616 at S10; +12 new cases), `nano_r4`, `bench`
and `bench-watchdog` all SUCCESS with 0 warnings, RAM unchanged at
6772/6972/6956 and Flash unchanged at 74540/81380/80808 (a test-only change),
`tools/telemetry` **119/119 OK**, `git diff --check` clean, every changed file
100% CRLF.

## D49 / S12 - 2026-10-06 - The documents are brought back into agreement with the code; no claim moves

S12 is a documentation-synchronisation stage. It exists because the architecture
stages S3-S11 changed what the run layer is and does, and prose written in the
D34-D36 era still described the pre-S3 world in places. The stage changed no code,
no configuration, no calibration, no telemetry schema and no evidence row: it
corrected stale *claims* and records the corrections here rather than rewriting the
earlier entries.

### Stale claims found and corrected

- **`SOFTWARE_ARCHITECTURE.md` section 10 said "Schema version 1 ... frozen in
  sections 9 and 16".** The live schema is version **3** (`EVS3`/`EVE3`, S8) and
  the command set has included `plan` since S9. The sentence now names
  `ev::kTelemetrySchemaVersion` and points at section 9.
- **`README.md`'s "Current state" named the serial reports without `plan`**, and
  explained the arming refusal with the placeholder configuration alone. It now
  names `plan` and adds the two further live dependencies: the immutable execution
  plan cannot be built without P-H8's five measurements, and the three bottle-phase
  strategy ceilings are `Provisional`, so `validateRunPhaseConfig()` refuses the
  run policy.
- **`README.md`'s staged-workflow list stopped at P9E.** A new bullet records the
  architecture-sync stages S0-S12 (D37-D49) - rule landmarks vs execution targets,
  the immutable `RunExecutionPlan`, the truthful `approachComplete` /
  `captureComplete` / `pushComplete` vocabulary, the strategy ceiling, the
  plan-owned `initialDelayS` with the timing estimate as a labelled diagnostic, the
  internal Run Time ending at the verified final-stop dwell start, the run-duration
  consumer, the absent-sensor capability and the `plan` command - and states that
  no physical fact's status changed.
- **`VERIFIED_STATE.md`'s software rows still carried D35's numbers** (native
  552/552 in 32 suites; `nano_r4` RAM 6292 / Flash 71836; `bench` RAM 6492 / Flash
  78680). Each D35 record is preserved and now also carries the re-measured current
  numbers (native **628/628** in 35 suites; `nano_r4` 6772/74540; `bench`
  6972/81380; `bench-watchdog` 6956/80808), plus a new row for the analysis
  tooling's own suite (**119/119 OK**). No hardware row moved: all twelve remain
  **UNVERIFIED**, and the battery/current installed-capability truth (D32/S10) is
  now stated in the hardware section as well.
- **`docs/bringup/P-H0`, `P-H5`, `P-H6`, `P-H7`, `P-H8`, `P-H9` and `P-H0W`
  instructed the operator to read `EVS1`/`EVE1`.** The live wire prefix is
  `EVS3`/`EVE3`; each procedure now names the current prefix and notes that the
  H0/H0W/D30 captures recorded the then-current one, so the historical captures are
  not misread as current data products.
- **`docs/bringup/P-H0`'s inspection list omitted `plan`**, and **`P-H9`'s fault
  table omitted `RunDurationExceeded`** (S10) while its low-battery sub-test still
  said the battery enforcement point was merely "not built". P-H9 now names the
  run-duration fault (and that it is unexercisable while the limit is
  `Provisional`), marks the low-battery step not applicable on this vehicle (no
  sense circuit) with the runtime comparison still unbuilt even on one that has it,
  and its "If it passes" bullet no longer claims the nFAULT question owes a
  measurement - D32 resolved it as not installed.
- **`docs/bringup/P-H1A` and `docs/hardware/motor-driver-drv8874.md` still told the
  operator to measure `nFAULT` levels**, contradicting D32's `NotInstalled`
  capability and each file's own later "not applicable" note. Both now scope that
  measurement to a vehicle that wires the line.
- **`docs/bringup/P-H10` never named the official Run Time.** It now states that
  the scored number is rule 8.o's Run Time (actuation to complete stop, measured by
  timekeeper or video) and that the firmware's own `internal_run_time_s` (actuation
  to the *start* of the verified final-stop dwell, S8) is a different quantity to
  record beside it, never to substitute for it.
- **`docs/bringup/P-H14` step 12 measured "the realised Run Time (from the `Done`
  tick ...)"**, which S8 superseded. The step now records the official measurement
  and the internal metric separately, and step 10 states that
  `kPlannedMaxRunDurationS` gained a runtime consumer (S10) that is inert while the
  value is `Provisional`.
- **`tools/telemetry/README.md` described "run duration: the log window and the
  armed window"**, which S8 deleted. It now describes the run timeline the tooling
  actually derives (`run_start_ms`, `final_stop_dwell_start_ms`, `stop_verified_ms`,
  `internal_run_time_s` and `final_stop_verification_completed_s`, both `None`
  without a verified final stop) and states what `measured_run_time_s` is.
- **`docs/limitations.md` did not name the run's execution-plan dependency or the
  three strategy ceilings.** A new section-F entry records that no plan exists
  until P-H8 measures its five dependencies, that with no plan there is no forward
  leg target and no progression completion, and that the three ceilings keep
  `validateRunPhaseConfig()` refusing the run policy.

### Deliberately not changed

- **`AGENTS.md` is untouched**: its invariants (safety, one-writer driver, fault
  latching, nonblocking behaviour, host tests never being physical evidence,
  physical testing as the source of truth) all still hold exactly as written, and
  the stage contract authorizes a change there only if an invariant actually
  changed.
- **The twelve hardware rows in `VERIFIED_STATE.md` remain UNVERIFIED**, and the
  informal 2026-10-02 observations remain recorded as OBSERVED-not-evidence. This
  stage promotes nothing and measures nothing.
- **The emergency tournament image's history is preserved as written.** It remains
  absent from the active tree (D35), the vehicle image still refuses to compile its
  define, and the statements that it is not the production image and supplied no
  run data are unchanged.

### Files changed

`SOFTWARE_ARCHITECTURE.md`, `README.md`, `VERIFIED_STATE.md`,
`docs/limitations.md`, `docs/bringup/P-H0-nano-upload-serial.md`,
`docs/bringup/P-H0W-watchdog-qualification.md`,
`docs/bringup/P-H1A-driver-electrical.md`, `docs/bringup/P-H5-velocity-trace.md`,
`docs/bringup/P-H6-ff-pi-tuning.md`,
`docs/bringup/P-H7-distance-braking-reversal.md`,
`docs/bringup/P-H8-bottle-integration.md`, `docs/bringup/P-H9-fault-injection.md`,
`docs/bringup/P-H10-full-run-repeatability.md`,
`docs/bringup/P-H14-calibration-checklist.md`, `docs/bringup/README.md`,
`docs/hardware/motor-driver-drv8874.md`, `tools/telemetry/README.md` and this
entry. No `src/`, `lib/`, `test/`, `platformio.ini`, calibration or schema file was
touched, and no `.agent-work` file is committed.

### Validation (on the frozen tree)

Native **628/628** in 35 suites; `nano_r4`, `bench` and `bench-watchdog` all
SUCCESS with 0 warnings (RAM 6772/6972/6956; Flash 74540/81380/80808);
`tools/telemetry` **119/119 OK**; `git diff --check` clean; every changed file
100% CRLF. These are the same numbers the stage recorded in `VERIFIED_STATE.md`,
measured on this tree rather than inherited from an earlier entry.

## D50 / S13 - 2026-10-06 - Pre-hardware architecture freeze: the deterministic software phase is closed at a validated revision

S13 is the gate between software work and physical work. It re-ran the whole
validation from a clean build state, audited the tree for the defects a freeze must
not inherit, and recorded the frozen revision, the evidence, the separation checks
and every remaining unknown in **`docs/PRE_HARDWARE_FREEZE.md`** (new). It changed
no behavior: the two source edits below are comments, and all three firmware images
rebuild **byte-for-byte identical** to the pre-change images
(`nano_r4` `1ce01874...`, `bench` `2bc3fe5d...`, `bench-watchdog` `c78ef5b5...`).

### Validation (clean build state - `.pio/build` removed first; nothing uploaded)

- `pio test -e native` -> **628/628** cases in **35** suites, exit 0, 0 warnings.
- `pio run -e nano_r4` -> SUCCESS, 0 warnings, RAM 6772/32768, Flash 74540/262144.
- `pio run -e bench` -> SUCCESS, 0 warnings, RAM 6972/32768, Flash 81380/262144.
- `pio run -e bench-watchdog` -> SUCCESS, 0 warnings, RAM 6956/32768, Flash 80808/262144.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -> **119/119 OK**.
- `pio run -e emergency` -> fails, exit 1: `UnknownEnvNamesError: Unknown environment names 'emergency'. Valid names are 'bench-watchdog, nano_r4, native, bench'` (the trailing valid-name list is a set and reorders between runs).

### Separation and audit checks (all green)

`arm-none-eabi-nm` over the three images: the vehicle image matches **0** for
`bench`, `hang`, `h1a` and `emergency`; `bench` matches 36 bench and 5 H1A symbols
with no emergency; `bench-watchdog` matches 31 bench symbols with **0** H1A and no
emergency (it overrides `build_flags`, so it does not inherit
`-DEV_BENCH_H1A_DIAGNOSTIC=1`). Each image has exactly one `T setup` and one
`T loop`. The three motor pin names appear only in `src/PinMap.h` (definitions) and
`src/MotorDriver.h` (writes). `lib/` has no Arduino header, no
`digitalWrite`/`analogWrite`, no `Serial.` and no `printf`. No `delay(` in `src/`
or `lib/`; the only waits are the two bounded 2 s boot waits, and the only
`for (;;)` is the bench-only hang hook (compiled out at its shipped `0U`). No
`TODO`/`FIXME`/`HACK`/`XXX`/`TBD` anywhere in source, tests, tooling or
`platformio.ini`. The stale ownership vocabulary
(`temporaryLegExecutionTargetM`, `turnAroundPositionM`, `PlannedDelayNotUsable`,
`TargetTimeUnreachable`) is absent; `plannedDelayS` survives only as
`ev::RunTimingPlan`'s diagnostic field, not as the run's delay owner. The forward
leg's shaping target and its progression threshold read the same stored
`RunExecutionPlan::forwardStopMpPositionM`. The `NotInstalled` battery and nFAULT
capabilities relax exactly one provenance requirement each and leave every numeric
check and the `kMotorFaultBar == kNoPin` `static_assert` unconditional.

### Concrete defects found and fixed

1. **Two source comments named a removed wire prefix.** `src/main.cpp` (the boot
   line note) and `src/VehicleRuntime.cpp` (the host-attach note in
   `serviceStagedText`) described the live boot event as `EVE1`. The prefix has been
   `EVE3` since S8 (`kTelemetrySchemaVersion = 3`), so both comments would have
   sent a reader to a token the firmware no longer emits. Corrected to `EVE3`;
   no code line changed, and the images are byte-identical (above).
2. **The bench-symbol row overstated what reproduces.** `VERIFIED_STATE.md`
   claimed the `bench` image's `nm` filter matches "the hang hook". The hook is a
   compile-time constant at its inert shipped value (`kDeliberateHangAfterPasses
   = 0U`), so the compiler removes its code and `hang` matches nothing in any
   image. The row now records the D35 result **and** the S13 re-measurement, and
   states why the hook symbol is absent. A new software row records the freeze
   itself (validated revision, validation, separation checks, and that it moves no
   hardware row).

### Deliberately unchanged

- **`AGENTS.md`** - no invariant it states changed. The freeze adds a *stage* rule
  (which the new `SOFTWARE_ARCHITECTURE.md` **§18** carries as a contract), not a
  change to the safety, ownership, provenance or evidence rules it already fixes.
- **Every hardware row.** All twelve `VERIFIED_STATE.md` hardware rows remain
  **UNVERIFIED**; no calibration was promoted; the only `Measured` production value
  is still `kMaxDwellGapMs` (28 ms); `nFAULT` and battery/current sensing remain
  `NotInstalled`; no raw count was relabelled.
- **The recovery material.** `recovery/s2-overscope-snapshot` and
  `recovery/d32-d34-v5-snapshot` are untouched, and nothing was pushed, merged,
  tagged or uploaded.

### Result

The repository is waiting on **physical evidence and calibration**, not on
unfinished deterministic software. Every remaining unknown is listed in
`docs/PRE_HARDWARE_FREEZE.md` §7 with the protocol that resolves it (P-H1A through
P-H14, and P-H0W step 8), and the one question no stage in this pack owns - the
arming gate - is restated there rather than silently decided. **READY FOR
INDEPENDENT PRE-HARDWARE AUDITS. No hardware work was started.**

## D51 / S14 - 2026-10-07 - The bench first-power surface is repaired against five audited safety defects, with a failing-before reproduction

S14 (`POST-S13-REPAIR-ROADMAP.md` stage A) repairs the five bench-control defects
registered as B1, B2, B3, B4 and B6 — **H1** (an ordinary `disarm`/`reset` left a
bench drive applied and acknowledged it as a stop), **H2** (opposite-direction
commands admitted with no stop evidence), **H3** (the 1000 ms stall window was
unreachable inside the 500 ms command ceiling), **M1** (ownership could be released
on a pass on which no writer wrote OFF: 18 ms of measured residual duty, unbounded
under a stalled loop), and **M3** (malformed no-argument bench lines executed, and a
repeated arm erased the per-arm command budget). Each is a demonstrated-defect fix
under the freeze rule (`docs/PRE_HARDWARE_FREEZE.md` §6): reproduced against the
pre-repair revision, repaired, and pinned by focused host regression tests.
**B5/M2 (serial framing) is deliberately not in this stage** — the roadmap's
stage-A findings bucket names it but its validation criteria do not, and the stage
instruction deferred it; it remains "MUST fix before F" for diagnosis/log integrity.
**The first powered bench test therefore stays BLOCKED.** No hardware action was
taken: no upload, no serial port opened, no arming, no motor command.

### Validation (final content; the whole list re-run after the last edit; nothing uploaded)

- `pio test -e native` -> **643/643** cases in **35** suites, exit 0 (628 + 15 new, all registered).
- `pio run -e nano_r4` -> SUCCESS, 0 warnings, RAM 6772/32768, Flash 74540/262144.
- `pio run -e bench` -> SUCCESS, 0 warnings, RAM 6972/32768, Flash 81700/262144.
- `pio run -e bench-watchdog` -> SUCCESS, 0 warnings, RAM 6964/32768, Flash 81036/262144.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -> untouched by this stage; inherits **119/119 OK** (no `tools/` file changed).
- `git diff --check` -> clean, exit 0; all seven changed files 100 % CRLF.

Byte-identity: the vehicle image's `firmware.elf` `sha256 1ce0187475512f09...` is
**identical to the S13 freeze image**, so the repair is confined to the bench path.
The bench images changed as intended (`bench` `55f82aee...`, previously `2bc3fe5d...`;
`bench-watchdog` `063508af...`, previously `c78ef5b5...`). Deltas against S13: native
**+15 cases**; RAM **+0 / +0 / +8**; Flash **+0 / +320 / +228**.

### Reproduction and regression evidence

`.agent-work/reviews/s14-2026-10-07/` holds a real-source mock-I/O probe that
compiles the actual bench dispatch header plus the actual `src/VehicleRuntime.cpp`
and `src/EncoderInput.cpp` against the mock Arduino headers, and mirrors the frozen
pass order of `src/bench/BenchMain.cpp::loop()`. One identical 33-check program was
run twice: against a pristine `git archive HEAD` extract (revision `370435d...`,
pre-repair) it reports **19 failed checks and exits 1**; against the repaired
working tree it reports **0 failed checks and exits 0**. `run_probe.sh` and
`run_probe_pristine.sh` reproduce both runs; both compiler logs are empty. The
fifteen new host cases (10 in `test_bench_drive`, 3 in `test_bench_command`, 2 in
`test_bench_arbitration`) pin each repaired behavior; the one pre-existing case that
had *endorsed* re-arming as a budget reset was corrected rather than left as the
defect's acceptance test.

### Corrections

- `lib/evcore/src/BenchDrive.h`: `BenchCommandRefusal::DirectionChangeRequiresReset`
  and one drive direction per arm (`armedDirection_`, cleared only by `reset()`);
  `BenchArmRefusal` (`None`/`FaultLatched`/`NotDisarmed`) with `arm()` as a
  Disarmed-only transition that no longer resets `commandsIssued_`;
  `benchStallProtectionReachable()`.
- `lib/evcore/src/BenchCommand.h`: `benchVerbMaximumTokenCount()` — the exact token
  bound of every verb in one table.
- `lib/evcore/src/MotorArbitration.h`: `benchRevokesForProductionCommand()` — exactly
  `disarm` and `reset`.
- `src/bench/BenchConsole.h`: `kStallWindowMs` 1000 -> 400 ms, the shipped numbers as
  one `constexpr kDriveLimits` with `static_assert(benchStallProtectionReachable(...))`;
  the revoke-before-forward intercept in `handleLine()` (`abort()` + `failSafe()` +
  `driveLine()` in the same dispatch); the pre-dispatch token-bound refusal
  (`TooManyTokens`); the named arm refusal; `failSafe()` on **every** ownership-release
  pass in `beginPass()` (own expiry, stall latch, arbitration revocation, H1A hold
  lapse); an additive `direction` field on the `BENCH,drive` line.

### Deliberately unchanged

- The powered duration ceiling (500 ms), the effort ceiling (150 permille), the
  per-arm budget (8), the `disarm`/`reset` acknowledgments, every non-revoking
  forwarded command, the production image, the telemetry schema, all calibration and
  `VERIFIED_STATE.md` statuses, and `AGENTS.md`.
- The two reversed alternatives: the powered duration was **not** lengthened to make
  the old 1000 ms window reachable, and no settling time was invented — the direction
  barrier is procedural (`reset()` + a fresh arm) and claims no standstill.

### Result

S14 PASS for the five findings. The first powered bench test is **not yet unblocked**:
it still requires M2/B5 and the S17 paper corrections C3/C4, on top of physical
block E. The two pre-existing orphaned tests
(`test/test_near_zero_verifier/test_main.cpp:159`,
`test/test_operator_response/test_main.cpp:354`) remain unregistered and are S15's
P9. Two observations are recorded rather than silently decided: a refused arm is not
counted in the refusal diagnostic counter, and the `BENCH,drive` line gained an
additive field (`BENCH` is ignored by the analyzer, so no tooling change is needed).
## D52 / S15 - 2026-10-08 - Measurement and analysis integrity: one endpoint, one run, one record per line

S15 (`POST-S13-REPAIR-ROADMAP.md` stage B) repairs the analysis-integrity findings
T1-T8 and the assigned micro items P8 (the serial-framing seam it owns), P9, P10,
P11, P12 and P13. Each is a demonstrated-defect fix under the freeze rule
(`docs/PRE_HARDWARE_FREEZE.md` section 6): reproduced against the pre-repair
revision, repaired, and pinned by focused regressions. **No hardware action**: no
upload, no serial port opened, no arming, no motor command, no measurement. All
twelve `VERIFIED_STATE.md` hardware rows remain UNVERIFIED and no calibration value
moved.

### The serial framing defect (M2/B5) - the largest repair here
**Before**: one buffer held both the line being formatted and the line being sent,
so `stage(urgent)` overwrote a partially emitted telemetry record; the host read a
telemetry prefix and an operator reply as one malformed record with no terminator
between them ("64 bytes of EVS3 then `OK,cmd=disarm` concatenated without CRLF"),
the same priority could destroy an earlier reply, and an over-capacity line was
staged with no bound check. **After**: the decision is a pure module,
`ev::LineFramer` (`lib/evcore/src/SerialLineFraming.h`), and
`src/SerialTransport.h` is a thin adapter over the board's port. Two line slots:
the formatter composes the next line in the slot that is not transmitting; an
urgent response staged while a line is in flight waits for that line's terminator;
a second urgent replaces only the line still waiting (counted, nothing emitted); a
telemetry line is refused and counted while either slot is busy; an over-capacity
line is refused and counted (`oversizeLinesRefused`). Every write is gated on
`availableForWrite()` and a short or zero write resumes where it stopped -
`Serial.write` is never assumed to send every byte. Static cost only: +577 bytes of
`.bss` per image (the single buffer *was* the defect); no allocation, no unbounded
buffer, no wait, and the emergency path (the supply cut) is unchanged.

### The analyzer (T1-T7)
- **T1/M7**: `Boot` opens a session and belongs to it; every record carries its
  `session_index`; backwards readings are classified as `wrap`, `reordered` or
  `restart_suspected` instead of all being read as 49.7 days; metrics analyze the
  last run of the last session with samples, report what was excluded
  (`mixed_sessions`/`mixed_runs`), compute the window per session, and refuse the
  elapsed duration for an unlabelled restart; cross-lane phase comparison uses the
  raw `t_ms` within one session. The audit reproducer (1000 ms, Boot 0, 0/20 ms)
  no longer produces a 4 294 967 s capture.
- **T2/M8**: the endpoint is the position at the **verified final stop**
  (`end_position_source`), with the last sample, the after-stop travel and the
  sampling window reported separately and never substituted for it; the believed
  target uses the sign of the leg the run stopped on (`MotionPlan::remainingDistanceTo`),
  and completion/abort is explicit.
- **T3/M9**: measurement headers are normalized *and used*, duplicate columns and
  wrong-width rows are refused, numbers must be finite and inside their column's
  domain, failures are documented CLI errors with exit 2 and no partial report, and
  JSON is written strictly finite by rename (`allow_nan=False`).
- **T4/M10**: every run id's output directory is preflighted for collisions
  (punctuation folding **and** case-insensitive filesystem behaviour) before
  anything is written; every input is validated before the first report; the
  manifest is published atomically.
- **T5/M11**: the capture's own `# SYNTHETIC` declaration travels through the parse
  into metrics, the report, the plots and the manifest; a name may add a warning,
  never remove one.
- **T6/L2**: `synth` refuses to replace a differing existing fixture without
  `--force` (checked for all fixtures before any write).
- **T7/L3**: a rerun reconciles its run directory (no stale `parse_errors.txt`,
  no stale plots); phase durations are transition-bounded, with the observed sample
  span reported separately and the still-in-effect final phase marked censored.
- **T8/L1**: `citesEvidence()` requires a non-null **and non-empty** citation.

### Runtime and test items
- **P9**: both orphans are registered (native baseline is 643 after S14; final
  count **658**).
- **P10**: `EncoderInput::resetAndReestablishReference()` performs the decoder
  reset and the reference sample as one PRIMASK-preserving critical section, and
  `reestablishReference()` itself reads both pins and updates the decoder with the
  interrupt masked; the mask spans a handful of instructions and is restored at
  once. The stationary precondition is unchanged; the wider reset barrier is S18's.
- **P11**: `applyReset()` invalidates the runtime-held estimate/odometry caches
  (current and last-valid velocity estimates, odometry estimate and stamp,
  position-usable), so no stop verification can pair a fresh zero-speed claim with
  the previous run's speed fact. The direction-safety barriers are untouched.
- **P12/OBS-FWD**: `OK,cmd=announce` now carries `exec=<plan status>` and
  `fwd_stop_m=<metres|->`, and the `AnnounceAccepted` event carries the same status
  as its `detail` - the commanded forward stop now exists in the durable evidence
  instead of only in a live `plan` query. The ACK's derived worst case is re-derived
  as 192 (the execution section's two renderings are exclusive) and pinned by a host
  test that renders both.
- **P13**: one end-to-end scenario crosses the uint32 `millis()` wrap through the
  real chain.

### Validation (final content; the whole list re-run after the last edit; nothing uploaded)
- `pio test -e native` -> **658/658** in **36** suites, exit 0 (643 + 15 new, all registered).
- `pio run -e nano_r4` -> SUCCESS, 0 warnings, RAM 7364/32768, Flash 75232/262144.
- `pio run -e bench` -> SUCCESS, 0 warnings, RAM 7564/32768, Flash 82740/262144.
- `pio run -e bench-watchdog` -> SUCCESS, 0 warnings, RAM 7556/32768, Flash 82080/262144.
- `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` -> **142/142 OK** (119 at S14).
- `git diff --check` -> clean, exit 0.
- Real-source probe `.agent-work/reviews/s15-2026-10-08/` (the **actual**
  `src/SerialTransport.h` against a mock USB-CDC sink, same 11-check program both
  ways): pristine `git archive HEAD` extract -> **4 of 11 checks fail, exit 1**;
  repaired tree -> **0 of 11 fail, exit 0**.

Resource deltas against S14: native **+15 cases**; RAM **+592 / +592 / +592**
(the framer's second line slot); Flash **+692 / +1040 / +1044**.

### Deliberately unchanged
- The telemetry wire schema (**still v3**, `EVS3`/`EVE3`), every sample and event
  field, the `PLAN` line, the help table, the bench image's behavior, all
  calibration values and `Sourced` provenances, and every `VERIFIED_STATE.md` row.
- The direction-safety barriers, the reversal permission rules, the taper/short-stop
  semantics, the encoder-loss response, the configuration cross-checks and the
  timing model: S18's, not this stage's.
- `AGENTS.md` (no invariant changed).
## D53 / S16 - 2026-10-08 - Arming requires the announced run's own usable execution plan, and the refusal is named

S16 (`POST-S13-REPAIR-ROADMAP.md` stage C) implements the user-approved **Option D**
of `ARMING-PLAN-GATE-REVIEW.md` for the arming-gate finding (`GATE`): the October 6
audit **M6**, S13R **I-1**, S13I **I-1** and S13X **F3** are one root cause, folded
here with S13R **M-1** and **M-2**. **No hardware action**: no upload, no serial
port opened, no arming, no motor command, no measurement. All twelve
`VERIFIED_STATE.md` hardware rows remain UNVERIFIED; no calibration value was
promoted and neither `runExecutionDependencies()` value became `Measured`.

### The defect

`announceVerdictFor()` reads only the rules, the held configuration and
`checks.runPolicyUsable`; `armVerdictFor()` reads only the 16-check
configuration-build report; nothing on the announce-and-arm path read
`runExecutionDependencies()` or the run's adopted plan. So once a run could be
announced (a state reachable as soon as the phase configuration is measured),
`announce` -> OK and `arm` -> OK could be followed by a run whose plan is refused:
the plan has no forward execution target, no leg target and no progression fact, so
the run could only end `PhaseTimeout` at the 20 s approach bound after the trigger -
fail-safe (no motion) but a configuration fact the gate was positioned to state
before a human pulled the trigger.

### The decision: the recorded 17th check was evaluated and not selected

`ARMING-PLAN-GATE-REVIEW.md` section 5.2 proposed a 17th `ConfigurationCheckId`
(`ExecutionPlanEstablished`) appended last, with `failureMask()` widened to
`uint32_t`. Two shapes were compared before editing, as the stage contract required:

* **the check shape** - append the check, widen the mask, move
  `ArmVerdict::failedCheckMask` and the `CHECK` line's worst case, and extend the
  telemetry `check` vocabulary (which is mirrored in
  `tools/telemetry/evtelemetry/schema.py` and pinned by
  `test_schema_mirror.py`); and
* **the narrow state-based refusal** - one new `CommandRefusal` name, one boolean
  on `OperatorSystemState`, one gate in `admissibilityOf()`, next to
  `NoRunAnnounced`.

The narrow shape was chosen. The reason is not that the check shape is wrong; it is
that the requirements it was proposed to satisfy are already carried elsewhere and
the narrower shape satisfies them with no wire change:

1. **Announcement diagnostics already exist and are already pinned** (`P12`/OBS-FWD,
   S15): `OK,cmd=announce` ends with `exec=<RunExecutionPlanStatus>` and
   `fwd_stop_m=<metres|->`, the `AnnounceAccepted` event carries the plan's status
   as its `detail`, and `plan` is the live view. A 17th check would restate a fact
   the command response and the event already carry, which this stage's contract
   explicitly discourages.
2. **Arming already has a run-state refusal vocabulary**, and this is exactly that
   kind of fact: `NoRunAnnounced` says there is no run to arm for; the new name says
   the run that was announced has no usable execution plan. Both are read from the
   state the command arrived in, and both are reported through
   `ERR,cmd=arm,reason=<name>`.
3. **There is no cache to go stale.** `configurationBuildChecks()` is computed on
   control ticks, so a check-based gate would need care at the arm boundary;
   `systemState()` derives the plan fact from `executionPlan_.plan().valid` on every
   command, so the gate reads the plan the layer is holding at the moment the arm is
   typed.
4. **No mask, no schema, no formatter capacity moves.** `ConfigurationChecks.h`,
   `ArmVerdict`, the `CHECK` line (445 characters), the width of the mask, the
   telemetry schema (still v3) and the Python mirror are untouched.

Recorded as an explicit deviation from the roadmap's proposed mechanism, satisfying
the roadmap's own acceptance criteria in substance: a refused plan refuses `arm`
with a named fact and accepts once both dependencies are `Measured` (the plan is
then `Planned`); the announcement stays admissible and names the plan's status and
forward stop; `CHECK`/`ERR,cmd=arm` stay within capacity; and the new cases are
executed (see the validation below). The current lockout's refusal reason is
unchanged in practice - with every phase configuration value still `Provisional` no
run can be announced at all, so `arm` still reports `NoRunAnnounced`, and the plan
refusal is reached only once a run *has* been announced. The one wording in the
roadmap that no longer holds literally is "the 17th check + `uint32_t` mask"; it is
recorded here as declined in favour of the state-refusal shape, which is what this
entry is for.

### What changed

* `lib/evcore/src/OperatorPolicy.h` - `CommandRefusal::ExecutionPlanNotEstablished`
  (appended after `NoRunAnnounced`, with its `commandRefusalName()` spelling);
  `OperatorSystemState::executionPlanEstablished` (default `false`, the fail-closed
  value); and the `Arm` case of `admissibilityOf()` refuses when it is false, after
  `AlreadyArmed` and `NoRunAnnounced` and after the fault gate that precedes the
  switch. The safety-significant precedence is therefore unchanged: `FaultLatched`
  still outranks everything, "no run announced" outranks "no plan", and the new
  refusal outranks the configuration-build verdict because a refused plan is a fact
  about the run being armed, not about a configuration value.
* `src/VehicleRuntime.cpp` - `systemState()` fills the fact from the adopted plan
  (`executionPlan_.plan().valid`); `applyArm()` re-asserts the same condition with
  the same named refusal immediately before `SafetyAuthority::arm()`, so an unusable
  plan cannot reach the authority even if a future caller reached that private
  method another way.
* `src/VehicleConfig.h` - **M-1**: the comment above `runExecutionDependencies()`
  no longer claims plan refusal alone is "why this vehicle cannot be armed"; it now
  records that the plan's refusal fail-closes travel *and* is itself an arming
  precondition. **M-2**: `allPromotedValuesCiteEvidence()` now folds the two
  `Sourced` dependencies (`contactWindowToleranceM`,
  `forwardStopMarginBeyondBottleLineM`) into the aggregate evidence-citation check
  with the existing `provenance::cited()` convention, so a promotion of either must
  name its evidence like every other promoted value.
* `SOFTWARE_ARCHITECTURE.md` (sections 15 and 16), `docs/limitations.md`, and the
  three `docs/bringup` deadline sentences (README, P-H14 Step 11, P-H7): the
  behaviour is documented and the S16 promotion deadline is recorded as satisfied,
  naming the mechanism and the operator's remaining per-trial discipline.

### Not changed, deliberately

The 16-check vocabulary, `failureMask()`'s width, `ArmVerdict`, every formatter and
its worst-case constants, the telemetry wire schema (v3), the timing model
(`kRunTimingModelValidated` stays false and is never an arming dependency),
`AnnounceRefusal` (a legal configuration is still announced - Option C was
rejected), the bench image, the reversal/taper/encoder-loss policies (S18's), every
calibration value and `Sourced` provenance, and every `VERIFIED_STATE.md` row.

### The F3 deadline

S17 already landed the procedure side of F3 (P-H14 Step 11 lists the five plan
constants and carries the deadline sentence; README and P-H7 state it too). S16
satisfies that deadline: the gate exists. The three sentences now say so and name
`ExecutionPlanNotEstablished` rather than "a named plan-usability check" that was
still to come.

### Validation (final content; nothing uploaded, no board connected)

* `pio test -e native` -> **664/664** in 36 suites, exit 0 (658 + 6 new, all
  registered: five in `test_operator_policy`, one in `test_operator_response`).
* **Failing-before evidence**: with only the `admissibilityOf()` gate removed from
  the same tree, `pio test -e native -f test_operator_policy` reports **5 failed,
  23 succeeded** - the five new/extended cases that assert the gate - captured at
  `.agent-work/reviews/s16-2026-10-08/pre-gate-test_operator_policy.log`.
* `pio run -e nano_r4` / `-e bench` / `-e bench-watchdog` (clean rebuilds) -> 3
  SUCCESS, **0 warnings**, RAM 7364/7564/7556, Flash 76120/83628/82968.
* `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` ->
  **142/142 OK** (unchanged; no analyzer or schema input moved).
* `git diff --check` -> clean. Resource delta against S15: Flash **+888** bytes per
  image, RAM **unchanged** - the new refusal name, its switch case and the gate at
  two call sites; no allocation and no new `.bss`.

### What the host tests do not cover

`applyArm()` and `systemState()` are Arduino-side (`src/`), which the host suite
cannot compile. The gate's decision function, the production plan builder and the
response formatter are tested as real production logic; the runtime *wiring* (one
assignment, one guarded call) is verified by the three firmware builds and by the
invariant re-assertion inside `applyArm()`, and is stated as a residual gap rather
than claimed as host-verified. `allPromotedValuesCiteEvidence()`'s new citations
cannot change its value while every production value is `Provisional`, so M-2 is a
review-verified correction with no executable regression; exercising it would
require the promotion S16 is forbidden to make.

## D54 / S18 - 2026-10-08 - Scope gate blocks the combined run-path safety repair

**S18 BLOCKED before production edits; checkpoint NOT ATTEMPTED.** The requested
pre-edit complexity gate applies: P1/H4 (stale reversal permission), P2/H5 (reset
direction-history loss), P3/H6 (forward/reverse short-stop), P4/H7 (missing travel
feedback), P5/M5 (stop-evidence/polarity cross-checks), P7/M16 (disabled diagnostic
timing approximation) and P8/M13 (remaining lifecycle/liveness/runtime fidelity)
span distinct safety mechanisms and frozen behavioral contracts. No partial repair
is presented as PASS. S16 Option D is preserved without reconsideration.

Proposed reviewable sequence: **S18A** current reversal permission, reset recovery
and stop/sign cross-checks; **S18B** explicit brake-entry and bounded short-stop
outcomes with output-coupled forward/reverse tests; **S18C** bounded feedback-loss
response, diagnostic-timing limitations and final actual-source integration closure.
Each requires its own complete validation and scope review before checkpointing;
overall S18 cannot PASS until every assigned finding and cumulative review passes.
The split is a proposal, not an approved roadmap change or an executed next stage.

Current-source host probes reproduce stale permission after +1 m/s and a 2000 ms
gap, reset/re-arm allowing the opposite direction without stop evidence, the ideal
target-coupled plant stopping at 6.059898 m before its 6.100000 m target, reverse
zero target inside the offset, frozen feedback authorizing 50/50 powered travel
ticks over 1000 ms, and invalid window/timeout and reverse-sign combinations.
An additional actual `VehicleRuntime` reset-command probe confirms remembered
Reverse becomes None without stop evidence, while S15 caches clear and S16's plan
clears. These are defect reproductions, not physical or post-repair safety evidence.
Original October 6 audit/probes/raw logs are preserved byte-identically.

Baseline revalidation on unchanged firmware: native **664/664 in 36 suites**;
telemetry **142/142**; clean `nano_r4`, `bench`, `bench-watchdog` builds SUCCESS,
**0 warnings**, RAM **7364/7564/7556 B**, Flash **76120/83628/82968 B** (zero delta
from S16); every command exit 0; `git diff --check` clean. Production still cannot
announce (`PolicyNotUsable`) or arm; physical dependencies remain Provisional,
absent sensors NotInstalled and empirical timing disabled. No source/test repair,
hardware operation, upload, push or evidence promotion occurred. Block E remains
separately eligible only when authorized; block F still requires E's physical
evidence. Full production runs remain blocked by S18 and physical calibration.

Full ledger, proposed paths/contracts, raw evidence, remaining physical gates and
validation: `.agent-work/stages/S18.md` and `.agent-work/reviews/s18-2026-10-08/`.
No commit is authorized by this BLOCKED verdict. Stop pending approval of the split.

## D55 / S18A - 2026-10-10 - Reversal permission, reset-direction safety and stop-evidence coherence repaired

**S18A PASS under the user-approved S18A/S18B/S18C split; checkpoint commit**
**authorized on full pass only (see below).** S18A covers P1/H4 (stale reversal
permission), P2/H5 (reset erases the direction barrier) and P5/M5 (invalid
stop-evidence and reverse-polarity configurations), plus only the P8
lifecycle/integration regressions those need. S18B (P3 short-stop/liveness) and
S18C (P4 feedback loss, P7 timing model, remaining P8) are not started and S18
as a whole stays open. No S16 arming-policy change, no 16-check mask change,
no timing change, no new sensors, no calibration promotion, no hardware work.

P1: `ReversalSequencer::update()` re-checks the sequence timeout and the live
stop evidence on every call, including while `DirectionChangeReady`; fresh
contradictory/unavailable evidence or an expired budget invalidates the
permission (verifier reset, latch cleared, full dwell to re-earn), and
`CompetitionStateMachine::advanceReversal()` consumes a pending handoff only
against the current tick's still-ready verdict, else holds `BrakeZeroSpeed`.
Historical verification is diagnostic only; opposite-direction power requires
current, in-budget evidence at consumption. The old latch test was corrected
with explanation.

P2: `SafetyAuthority::reset()` preserves the remembered powered direction - a
software reset is not a verified mechanical stop - and only an authorized
non-powered command taken while zero speed is reported discharges the barrier.
Both `VehicleRuntime` and the scenario harness step a disarmed stop-recovery
verifier (the reversal gate's own stop definition) on live evidence each
tick/cycle, contributing to `atZeroSpeed` only while disarmed through the
single existing `authorize()` call; it is reset at every new-run boundary and
never verifies on the shipped Provisional stop definition, so production
behavior is unchanged. Operator procedure: hold still while disarmed for one
fresh verification before announce/arm. Power-cycle RAM loss and the resulting
operator standstill requirement are documented, not sensed. The old
reset-forgets-direction test was corrected with explanation; completed runs
self-discharge through their verified final stop, which the unchanged
second-run test now proves.

P5, all derived from how the consumers operate and wired into the existing 16
checks: estimator `windowMs < noEdgeTimeoutMs` (the claim is checked before
window coverage, so a wider window can never yield a window-derived zero at
rest); the speed-fact age must span the claim lead plus the dwell (new
`stopEvidenceSpansClaimAndDwell()`, production 3200 ms still spans both stops);
the window must be coverable by the 16-sample history at the control cadence;
both travel legs must name the one global positive-command direction (new
`directionalCommandPolarityAgrees()`); cast-in unknown direction/polarity
enums are refused (`isKnownMotionDirection()`, unknown command direction,
explicit driver-fault polarity, hardened agreement functions). Production
verdicts are unchanged (all fail-closed conjuncts); placeholders stay
Provisional and no physical adequacy is claimed.

Regression evidence: 21 new Unity tests, all registered (script-audited, zero
orphans), plus 2 corrected contract tests and a trace-audit correction that
credits both verified-stop sources the authority itself clears on. Native
**685/685 in 36 suites** (baseline 664 + 21 reconciled per suite); `nano_r4`,
`bench`, `bench-watchdog` SUCCESS with **0 warnings** (RAM +64 B for the one
recovery verifier, Flash +352 B); telemetry **142/142**; `git diff --check`
clean. S14 bench safety, S15 integrity and S16 Option D behavior preserved;
encoder-loss response and short-stop policy remain honestly identified as
S18B/S18C work. Full ledger, per-case lists, resource analysis and remaining
gates: `.agent-work/stages/S18A.md`. D54 preserved byte-identically above.

## D56 / S18B - 2026-10-10 - A travel phase that reaches brake entry and stops advancing ends with its own identity

**S18B PASS under the user-approved S18A/S18B/S18C split; checkpoint commit
authorized on full pass only (see below).** S18B covers P3 (the shaper's zero
inside the brake offset versus a state machine that cannot complete until the
landmark is crossed) plus the P8 output-coupled liveness regressions that
finding needs. S18A is the checkpointed input revision (`a049e49`); S18C
(P4/H7 encoder-loss response, P7 timing model, remaining P8) is not started and
S18 as a whole stays open. No S16 arming-policy change, no timing change, no
new sensor, no calibration promotion, no hardware work.

**The defect, reproduced on the pre-repair revision.** A positive brake offset
makes the target shaper reach zero *inside* the measured offset, while the run
layer cannot leave a travel phase until the vehicle reaches that phase's
landmark. A vehicle that tracks its command and comes to rest short of the
landmark therefore presents a zero target for the whole phase budget and leaves
only at `PhaseTimeout`. Reproduced on `a049e49` with an output-coupled synthetic
plant against the real `RunExecutionPlan` / `MotionProfile` /
`CompetitionStateMachine` chain (`.agent-work/reviews/s18b-2026-10-10/`):
forward settled **6.059502 m** against a forward execution target of
**6.100000 m** (short by **0.040498 m**, inside the fixture's 50 mm brake
offset) - **391 cycles** with a zero shaped target while still short,
**11 980 ms** spent in `Push`, ending `PhaseTimeout` with no identity of its
own; reverse settled **5.040629 m** against a Target Point of **5.000000 m**
(short by **0.040600 m**) and ended `PhaseTimeout` in a phase other than
`Reverse`. These are software counterexamples, not measured stopping distances,
and no such number enters production.

**Chosen policy.** The narrowest option that satisfies the invariants without
inventing calibration: name brake entry explicitly as its own input fact and
give the short-stop one bounded terminal outcome with its own identity.
Rejected: a "stop-short fault" triggered by a zero-speed claim alone (a single
motor encoder cannot prove standstill, so a claim derived from encoder silence
is not independent evidence); any creep or powered correction drive (the
physical endpoint stays the only source of truth and the firmware must not
manufacture motion to satisfy a threshold); and renaming `PhaseTimeout`, which
measures a different thing (waiting for evidence, not travelling).

**Implementation.** `RunInputs::legInBrakeEntry` (derived in
`src/VehicleRuntime.cpp` from the plan-owned leg remaining distance and the
direction's measured brake offset - the same values the shaper uses, so the run
policy never holds a second copy of a measured distance).
`RunPhaseConfig::travelStallTimeoutS` (production
`kPlannedTravelStallTimeoutS` is a `Provisional` placeholder, so
`validateRunPhaseConfig()` still refuses the run policy and production remains
unarmable). `CompetitionStateMachine::checkTravelProgress()` latches the new
`FaultCode::TravelStalled` when a travel phase (`runPhaseCarriesTravel()`:
`Approach`, `Capture`, `Push`, `Reverse`, `FinalApproach`) has been at brake
entry for `travelStallTimeoutS` without the remaining distance strictly
decreasing below the best value seen since the phase began. It is evaluated
after the phase bound so the phase deadline keeps precedence, re-baselines on
every phase entry, pauses (never faults) on unusable or non-finite position
evidence, uses wrap-safe unsigned subtraction, never reads speed, never
re-derives a landmark, commands no motion, and its response is the authority's
existing single fault stop (`FaultStopPolicy::Disable`, like every other
non-electrical fault). The wire schema stays **v3**: `telemetryFaultName()`
gains the `TravelStalled` spelling and the vocabulary list widens, exactly as
`RunDurationExceeded` did (S10). A semantic fault identity required the
corresponding test, diagnostic-contract and minimal documentation updates:
`docs/bringup/P-H9-fault-injection.md` gains its injection row and
`SOFTWARE_ARCHITECTURE.md` section 14 names the identity and the bound.

**Output-coupled liveness tests (P8).** The scenario harness gains a
deterministic synthetic plant that couples commanded output to movement -
position advances only while the boundary authorized a drive, so a phase whose
command has reached zero produces no further travel - with explicit limitations
(one scalar velocity, first-order lag, no torque, friction, slip, gearbox,
battery or tyre model), labelled in the header. Five new `test_scenario` cases:
a coasting plant still reaches `Done` (the success path the bound must not
break); an ideal follower stops short and faults `TravelStalled` - never
`PhaseTimeout`, never a push/reversal/Done claim, `ForcedFaultStop`, zero
permille, no direction; a frozen plant makes no progress outside brake entry and
the phase bound still ends it; a controlled overshoot still reaches `Done`; and
a reverse leg that stops short faults `TravelStalled` without ever claiming the
final stop. Six new `test_competition_state` cases cover the bound directly:
every travel phase without progress, inactivity outside brake entry, the pause
on unusable evidence, continuous progress never faulted, phase-bound
precedence, and `uint32` wrap safety. Two historical expectations were extended
rather than corrected: the whole-fault-identity latch sweep and the
phase-timeout cases now state that they are outside brake entry.

**Validation (exit codes recorded).** Native **696/696 in 36 suites**, exit 0
(baseline at `a049e49` 685 - `test_competition_state` +6, `test_scenario` +5,
every other suite unchanged; all new tests `RUN_TEST`-registered, zero orphans
by script audit). `nano_r4` / `bench` / `bench-watchdog`: 3 clean rebuilds
SUCCESS, **0 warnings**, RAM **7468 / 7668 / 7660** (+40 B each: the new
`Sourced` phase bound, the run-input flag and the machine's three progress
members), Flash **76968 / 84480 / 83812** (+496 / +500 / +492 B: the progress
bound, the new identity and its vocabulary). Telemetry **142/142 OK**, exit 0.
`git diff --check` clean, exit 0, and all 15 changed tracked files are 100 %
CRLF. Preserved: S18A reversal/stop-evidence safety (its suites pass
unchanged), S16 Option D arming (untouched), S14 bench safety, S15 integrity,
empirical timing still disabled (`kRunTimingModelValidated` false), production
still unarmable on Provisional calibration.

**Still open.** S18C: P4/H7 (frozen encoder counts leave production
feed-forward drive requests active - the phase bound ends such a run, but there
is still no encoder-progress watch), P7/M16 timing model, remaining P8. The
2026-10-08 audit's **M1 remains present** (`resetMotionChainForNewRun()` still
resets the ISR-shared decoder with interrupts enabled at
`src/VehicleRuntime.cpp:1133`, with only the adapter's second
`resetAndReestablishReference()` masked) and is recorded here as an outstanding
safety finding rather than cleaned up inside S18B. The other 2026-10-08
findings (M2-M18, L1-L6) stay open except H3, which this stage repairs. Every
physical placeholder stays `Provisional`, no stopping distance is claimed, and
the brake offset that defines brake entry stays unmeasured until the physical
braking trials set it. Full record: `.agent-work/stages/S18B.md`.

## D57 / S18C - 2026-10-10 - Encoder-feedback stage blocked by the unprotected reset dependency

**S18C BLOCKED; checkpoint NOT ATTEMPTED.** Live entry matched the authorized
S18B checkpoint: main at afc98a55869390be13ac822963757b04dcde98cb,
20 ahead / 0 behind origin/main, tracked worktree and index clean, only the two
untracked October 6/8 audit documents. Both audits remain byte-identical.
S18A and S18B remain checkpointed PASS; S18 as a whole is not complete.

Recovered mapping: P4 = October 6 H7 / October 8 H4, missing bounded production
encoder-progress supervision; P7 = October 6 M16 / October 8 M7, incomplete
one-trip timing diagnostic; remaining P8 = October 6 M13 / October 8 M17,
actual-source lifecycle/composition coverage. October 8 M16 instead concerns
reply-loss accounting. Prior 50/50-powered frozen-count probe logs were reviewed,
but no independent S18B-HEAD reproduction or post-repair result is claimed.

The user-required M1 dependency check found the defect still present:
VehicleRuntime::applyReset passes encoderInput::decoder() into the pure
resetMotionChainForNewRun helper, whose decoder.reset() mutates ISR-shared
fields with interrupts enabled. Only the subsequent adapter reset/reference is
PRIMASK-protected. That second reset does not synchronize the first mutation.
Reset is admitted in Fault without proof of stationary mechanics; S18A retains
the direction barrier for exactly that reason. A sequential host mock cannot
establish this required fault/reset/re-arm measurement boundary as coherent.
No physical race or specific downstream powered behavior was observed.

Under the explicit dependency STOP, no source/test/configuration repair, guard,
new fault identity, timeout, timing-output migration, or unrelated analyzer or
watchdog fix was introduced. The prerequisite is a separately authorized narrow
M1 reset-ownership repair and deterministic interrupt-injection review, then
resumed S18C reproduction/design/implementation/validation. Option B (visibly
unsupported diagnostic estimates) is recommended for P7, not implemented;
kRunTimingModelValidated remains false and S16's plan gate is unchanged.

No full native/telemetry suite or firmware build was rerun after the dependency
stop. S18B's recorded 696/696 native cases in 36 suites, 142/142 telemetry,
three successful zero-warning builds and RAM/Flash 7468/76968, 7668/84480,
7660/83812 B remain historical baseline, not S18C regression evidence.
Git diff hygiene, audit hashes and append-only history are checked after this
record. D54, D55 and D56 are not rewritten; only this D57 is appended.

Full finding ledger, partial versus remaining P8 coverage, October 8 backlog
reconciliation, physical uncertainty and unmet acceptance gates are in
.agent-work/stages/S18C.md; checkpoint-not-attempted record:
.agent-work/utilities/U1-S18C-checkpoint.md. No staging or commit, no push,
merge, tag, upload, port access, hardware action or calibration promotion.
Production remains unarmable. E is separately unexecuted; F waits for E and
separate authorization plus applicable safety/instrument review. Independent
cumulative S18 review is still needed after eventual C PASS. STOP after S18C.

## D58 / S18C-M1 - 2026-10-10 - Sole protected ownership of the ISR-shared decoder reset

**S18C-M1 PASS (software prerequisite); conditional checkpoint authorized after
complete staged review. Parent S18C remains unfinished, not PASS.** Entry was
main at afc98a55869390be13ac822963757b04dcde98cb, 20 ahead / 0 behind;
the known +49/-0 D57 carryover was the only tracked edit and is preserved
unchanged. Both untracked October 6/8 audits remain byte-identical and unstaged.
No recovery import, hardware action, upload, calibration promotion or push.

Live M1 confirmed: VehicleRuntime.cpp:1133 passed the ISR-owned decoder to the
pure chain; MotionChainReset.h:125 reset six shared fields without masking.
Only the subsequent adapter reset/reference at runtime:1142 was protected.
That second reset did not synchronize the first. A new actual-source host
regression sees the old count already erased at the FIRST adapter mask boundary
(expected 2, actual 0), giving 1 failed / 7 passed before repair, exit 1.
This is deterministic software evidence, not an observed physical race.

Option A chosen: resetMotionChainForNewRun no longer accepts/resets a decoder.
EncoderInput alone owns production mutations, and its mutable decoder accessor
is removed. The existing single saved-PRIMASK reset/reference guard is retained;
local pure-test decoders are reset explicitly by their synthetic owners. Runtime
cache invalidation, plan/origin reset, S18A direction-history barrier, S16 arm
gate and fault precedence remain intact. Architecture documents that ownership.
No P4 feedback policy, P7 timing correction or cumulative S18C integration began.

Twelve new registered Unity cases link the actual EncoderInput.cpp and
VehicleRuntime.cpp with suite-local fake Arduino I/O. They cover enabled and
disabled initial masks, outer guard preservation, pending callback injection at
both pin reads and the guard boundaries, all four initial A/B phases and both
software signs, before/after-reference moving edges, nonzero-count/fault/cache
reset, no false standstill, timer wrap without restarting the schedule, retained
direction protection, production refusal and synthetic reannouncement with the
S16 execution-plan refusal. The mock coalesces pending requests and cannot admit
an ISR while masked. NANOR4 CMSIS memory clobbers and compiled ARM disassembly
confirm MRS PRIMASK / CPSID / all reset stores and reference / MSR saved PRIMASK.
All reset/access callers in vehicle, bench, watchdog and synthetic tests reviewed;
each firmware has one decoder, encoder ISR, setup and loop, no escaping accessor.

Final checks after last source/test edits: focused **67/67**, native **708/708 in
37 suites** (696 + 12), three clean firmware builds **SUCCESS / 0 warnings**,
telemetry **142/142 OK**, registration **37 suites / 0 problems**, diff check
clean; each command exit 0. RAM/Flash B: nano_r4 **7468/76928**, bench
**7668/84440**, bench-watchdog **7660/83780**. RAM unchanged; Flash -40/-40/-32
from S18B. No performance speedup claimed. S14/S15/S16/S18A/S18B suites green;
VehicleConfig untouched, timing validation false, physical values Provisional,
production unarmable. No warning suppressed and no safety assertion weakened.

Interrupt exclusion establishes software consistency, not mechanical standstill:
sequential reads are not simultaneous, pending physical edges may coalesce or be
absorbed into the reset reference, and prior counts are deliberately discarded.
Encoder direction, repeatability, edge-capture reliability, distance calibration
and stopping remain physical gates. No physical evidence is promoted.

Full source/caller/coverage ledger and exact command statuses/resources:
.agent-work/stages/S18C-M1.md; raw logs and preservation manifest:
.agent-work/reviews/s18c-m1-2026-10-10/; checkpoint verification:
.agent-work/utilities/U1-S18C-M1-checkpoint.md. Original D54-D57 unchanged.
Next eligible action is resume S18C; NOT started here. STOP after prerequisite.

## D59 / S18C-RESUME - 2026-10-11 - Bounded travel on lost encoder feedback

**S18C-RESUME PASS; conditional checkpoint authorized after complete staged
review. Parent S18C is therefore complete; cumulative S18 (A/B/C) is complete
as software.** Entry was main at c5553aa67266aaf5aac9712aa5a10a8eb53c09e7
(S18C-M1, ISR-owned decoder reset atomic), 21 ahead / 0 behind origin/main,
tracked tree clean except the authorized work below. Both untracked October
6/8 audits remain byte-identical and unstaged. No recovery import, hardware
action, upload, calibration promotion or push.

P4 (October 6 H7 / October 8 H4) reproduced on entry HEAD with a temporary
actual-composition suite (real decoder/estimator/controller/run/authority via
ScenarioHarness, synthetic Measured fixtures): moving history, then frozen
counts for 1000 ms (50 cycles) sustained 50/50 powered forward authorizations
with the zero-speed claim present and no fault - 1/1 passed as the unsafe
behavior. The temporary suite was deleted after the log was preserved
(.agent-work/reviews/s18c-resume/p4-prerepair-repro.log); the committed
regressions assert the bounded fault instead. A standalone MinGW probe binary
could not be built in this sandbox (even a trivial file fails there), so the
Unity reproduction is the before-evidence, not a re-run of old probe logs.

Feedback-loss contract: ACTIVE POWERED TRAVEL is the authority's returned
driving command (previous cycle's fact at evaluation); REQUEST (regulator
effort/intention) never suffices; progress is a net signed tick advance >= 1
in the leg's expected direction through the measured polarity; dropouts,
placeholders, and unmeasured polarity pause rather than fault; the interval
starts at the first accepted authorization (startup allowance = full bound)
and re-baselines on every coherent advance; non-powered cycles neither
consume nor renew it (no toggle bypass); violation latches
FaultCode::EncoderFeedbackLost via the existing observed-fault path with
driver-first/feedback-second/duration-last priority and the single Disable
stop; reset clears the monitor without discharging the S18A barrier.

Minimum policy: new pure EncoderProgressMonitor.h (accumulated powered time
without progress, dtMs-based and wrap-safe); RunPhaseConfig gains measured
encoderProgressTimeoutS with NonPositiveEncoderProgress refusal (no new check
ID, 16-check vocabulary and mask unchanged); production placeholder
kPlannedEncoderProgressTimeoutS = 0.5 s Provisional (25 periods; a software
exposure budget, not a physical claim; P-H5/H7/H9 own the measurement);
runtime evaluates before the run policy and stores the authorized-powered
fact; harness mirrors the ordering; schema stays v3 with one new fault
spelling. Three older frozen-count expectations corrected with explanation
(two scripted starvation cases and the S18B planted frozen case now end at
the feedback bound, which supersedes the S18B-era deliberate non-detection).

P7 (October 6 M16 / October 8 M7) resolved by Option B without a model
rewrite or wire change: kRunTimingModelValidated stays false, timingReady
false owns no cruise/delay/refusal, and the structural omissions (one ramp
pair over rule distance vs two stopped execution legs with margins, second
ramps, ceilings, dwells, interaction timing) plus the two-gate enablement
rule are now stated at the flag, the plan inputs, the estimator arithmetic,
the architecture, and the limitations. Existing timing-independence and
serial-independence suites pin the behavior.

New regressions: 22-case test_encoder_progress_monitor (startup, forward,
reverse, dwell exemption, fault/recovery, timing boundaries, config) plus 10
scenario integration cases (frozen forward/reverse, wrong sign, intermittent
progress, dropout pause, untriggered exemption, wrap, large gap, stalled
clock, fault/reset/barrier) and the fault sweep/vocabulary/policy extensions.
All registered, zero orphans (38 suites / 0 problems).

Final checks on the exact staged content: native **740/740 in 38 suites**
(708 + 32), three clean firmware builds **SUCCESS / 0 warnings** at RAM/Flash
nano_r4 **7500/77152** (+32/+224), bench **7700/84664** (+32/+224),
bench-watchdog **7692/84004** (+32/+224), telemetry **142/142 OK**, diff check
clean, each command exit 0. S14/S15/S16/S18A/S18B/S18C-M1 suites green;
production still unarmable on Provisional values; no warning suppressed and
no safety assertion weakened. No physical evidence is promoted; the timeout,
scale, polarity, braking, and timing facts stay unmeasured.

Full ledger, contract, and October 8 dispositions:
.agent-work/stages/S18C-RESUME.md; raw logs:
.agent-work/reviews/s18c-resume/; checkpoint verification:
.agent-work/utilities/U1-S18C-RESUME-checkpoint.md. Original D54-D58 and
S18C.md (BLOCKED) unchanged. Next eligible action is independent cumulative
S18 review; no next stage started here. STOP after this stage.
