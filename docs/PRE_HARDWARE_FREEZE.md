# Pre-hardware architecture freeze

**Status: FROZEN (S13 baseline, 2026-10-06).**

This record marks the point the repository stopped changing because of *software*
work and started waiting on *physical* evidence. It states what is frozen, the
exact revision that was validated, the validation itself, the checks that prove
the images cannot contain each other's behavior, the rule that applies from here,
and every remaining unknown with the protocol that resolves it.

It is a software record. **No physical measurement, no firmware upload and no
powered test produced or supports anything in it.** Every number below is a host
or compile fact. The vehicle's behavior is not described, claimed or predicted
anywhere in this file; `VERIFIED_STATE.md` remains the only place a physical fact
may become VERIFIED, and all twelve of its hardware rows are still UNVERIFIED.

---

## 1. Frozen revision

| Fact | Value |
| --- | --- |
| Branch | `main` |
| Validated revision (`git rev-parse HEAD`) | `93ea1ecd8325670b702291ca78398cb5080f6bb3` |
| Validated tree (`git rev-parse HEAD^{tree}`) | `60026bed69c23cfc0b2fe09f9be5d8725b3877b1` |
| Upstream | `origin/main` = `a0de094c7cf67b1b13a86682745fe75c1a31757b` (13 ahead / 0 behind, intentionally unpushed) |
| Working tree | clean (tracked and untracked) before the S13 change set |
| Agent pack | `EV-V5-current-to-competition-2026-10-04-v2` |

The **S13 change set itself is documentation and comments only**, and it is the
freeze baseline:

- this record;
- the `DEVELOPMENT_LOG.md` D50 entry;
- one new contract section, `SOFTWARE_ARCHITECTURE.md` §18 (the freeze rule);
- one sentence in `README.md` naming the freeze and this record;
- two software-fact corrections in `VERIFIED_STATE.md` (the bench-image symbol
  claim, and a row for the freeze itself);
- two comment corrections in `src/main.cpp` and `src/VehicleRuntime.cpp` that
  named the removed `EVE1` wire prefix instead of the live `EVE3`.

§5 shows that the three firmware images rebuild **byte-for-byte identical** after
those two comment corrections, so the frozen firmware content *is* the validated
`93ea1ec` content.

## 2. What is frozen

Frozen means: a change to any of the following is a deliberate act with a
`DEVELOPMENT_LOG.md` entry, not a refactor, and it may not be made to add a
capability that no measurement has asked for.

- **The contracts in `SOFTWARE_ARCHITECTURE.md` sections 1-18** — layering
  (`src/` → `lib/`, `lib/` free of Arduino headers), host-testability, nonblocking
  execution, the safety contract, units and naming, calibration/limits/provenance,
  rule sources, the telemetry contract, the encoder/odometry/velocity/run-policy
  contracts, the runtime and hardware boundary, the operator interface, and the
  bench instrumentation contract.
- **The module layout and the §11 module responsibilities** as they stand in
  `lib/evcore/src/` (38 headers) and `src/` (10 headers, 2 translation units,
  `src/bench/`).
- **The safety invariants** (unchanged by every software stage since D35):
  `SafetyAuthority` is the only producer of an authorized command; exactly one
  writer of the driver pins (`src/MotorDriver.h`); no direct powered direction
  flip; the near-zero reversal gate; faults latch; a reset never resumes motion;
  serial and telemetry are never participants in control; plan and calibration
  dependencies fail closed.
- **The telemetry wire schema**: `ev::kTelemetrySchemaVersion = 3`, prefixes
  `EVS3`/`EVE3`, `ev::kSerialLineCapacity = 576`.
- **The command and response vocabularies** in §16 (including the read-only
  `plan` report) and the `BENCH,...` bench grammar in §17.
- **The PlatformIO environment set**: `native`, `nano_r4`, `bench`,
  `bench-watchdog`. The tournament-only `[env:emergency]` does not exist in the
  active tree and is refused by name (§4).
- **The host test suite**: 35 suites, 628 cases, run by `pio test -e native`.

## 3. Validation (re-run from a clean build state on the frozen tree)

Clean build state = `.pio/build` removed before the run. Nothing was uploaded and
no board was connected.

| Check | Command | Result |
| --- | --- | --- |
| Host logic suite | `pio test -e native` | **628/628 cases in 35 suites**, exit 0, 0 warnings, 0 errors (91.3 s) |
| Vehicle image | `pio run -e nano_r4` | **SUCCESS**, 0 warnings, RAM 6772/32768 (20.7%), Flash 74540/262144 (28.4%) |
| Bench image | `pio run -e bench` | **SUCCESS**, 0 warnings, RAM 6972/32768 (21.3%), Flash 81380/262144 (31.0%) |
| Watchdog bench image | `pio run -e bench-watchdog` | **SUCCESS**, 0 warnings, RAM 6956/32768 (21.2%), Flash 80808/262144 (30.8%) |
| Analysis tooling | `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` | **119/119 OK**, exit 0 (25.7 s) |
| Removed image cannot be built | `pio run -e emergency` | **fails, exit 1**: `UnknownEnvNamesError: Unknown environment names 'emergency'. Valid names are 'bench-watchdog, nano_r4, native, bench'` (the trailing valid-name list is a set and reorders between runs; the environment name is not in it either way) |

These are the same numbers the earlier stages measured (native 628/628 since S11,
telemetry 119/119 since S8, the three RAM/Flash pairs since S8-S10). They are
re-measured here on the frozen tree rather than inherited.

## 4. Static separation and audit checks

All checks are searches over the live tree and symbol tables of the three built
images (`arm-none-eabi-nm`).

| Check | Evidence |
| --- | --- |
| The vehicle image contains no bench, H1A or emergency behavior | `nm .pio/build/nano_r4/firmware.elf` filtered for `bench|Bench` = **0**, `h1a|H1a` = **0**, `hang` = **0**, `emergency|Emergency` = **0** |
| The bench image has the instruments, and the H1A diagnostic with them | `nm .pio/build/bench/firmware.elf`: `bench|Bench` = 36, `h1a|H1a` = 5 (e.g. `_ZN4evhw12benchConsole11h1aHoldLineEm`, `_ZN4evhw12benchConsole17applyH1aAwakeHoldE...`), `emergency` = 0 |
| The watchdog bench image has the bench but **not** H1A | `nm .pio/build/bench-watchdog/firmware.elf`: `bench|Bench` = 31, `h1a|H1a` = **0**, `emergency` = 0. `[env:bench-watchdog]` sets its own `build_flags`, so it does not inherit `-DEV_BENCH_H1A_DIAGNOSTIC=1` from `[env:bench]` |
| The deliberate hang hook is compiled out at its shipped value | `kDeliberateHangAfterPasses = 0U` (`src/bench/BenchMain.cpp`) makes the hook's code unreachable, so `hang` = 0 in **all three** images; it is a bench-only source file either way. D35's record of a symbol match is preserved in the log |
| One entry point per image | Each of the three images has exactly one `T setup` and one `T loop`. The partition is by `build_src_filter`: `env:nano_r4` = `+<*> -<bench/>`, `env:bench` = `+<*> -<main.cpp>` |
| Nothing but the driver adapter writes the motor pins | `grep -rn "kMotorPhase\|kMotorEnable\|kMotorSleepBar" src/ lib/` matches only `src/PinMap.h` (the definitions plus comments) and `src/MotorDriver.h` (every `digitalWrite`/`analogWrite` of the three pins). The bench tree reaches the pins through `motorDriver::applyBench()` |
| `lib/` cannot touch hardware or the port | `lib/` contains no Arduino header, no `digitalWrite`/`analogWrite`, no `Serial.` and no `printf`; the only matches are explanatory comments. `CompetitionStateMachine.h` includes only pure modules |
| No blocking delay in production or the core | No `delay(`/`delayMicroseconds(` call in `src/` or `lib/`. The only waits are the two bounded 2 s boot waits (`while (!Serial && millis() - startMs < 2000)`) in `src/main.cpp` and `src/bench/BenchMain.cpp`, both before the watchdog opens; the only `for (;;)` is the bench-only hang hook |
| Every loop is bounded | Every `while` in `src/`/`lib/` exits on a counter, an index or a per-pass budget (`kReadBytesPerPass`, the help-table bound `kOperatorHelpLineCount`, the `BoundedText` digit loops) |
| Serial cannot influence control | The control tick reads no command and no session state; serial work is `serviceStagedText`/`serviceSerial`, called after the pass and the watchdog checkpoint. `test_serial_independence` re-runs a scripted run with and without the consumer hierarchy driven by a stalled host and asserts identical tick counts, dropped deadlines and per-tick authorization/mode/effort traces |
| No leftover task markers | No `TODO`, `FIXME`, `HACK`, `XXX` or `TBD` in `src/`, `lib/`, `test/`, `tools/`, `scripts/` or `platformio.ini` |
| No stale ownership alias in the code | `temporaryLegExecutionTargetM`, `turnAroundPositionM`, `PlannedDelayNotUsable` and `TargetTimeUnreachable` are absent; `plannedDelayS` survives only as `ev::RunTimingPlan`'s own diagnostic field, which is *not* the run's delay owner (`RunExecutionPlan::initialDelayS` is) |
| One forward execution target | The forward leg's shaping target (`legExecutionTargetM()`) and the forward progression threshold (`deriveBottleEvidence()`) both read the same stored `RunExecutionPlan::forwardStopMpPositionM`; the Bottle Line is never substituted for it |
| No impossible `NotInstalled` gate | Battery sensing is `NotInstalled`, so `minBatteryV`'s provenance is *not applicable* rather than outstanding (`validateSafetyLimits()`), and `maxEffortPermille`, both velocity ceilings and `maxRunDurationS` remain the checks that keep the report false for a reason this vehicle can address. The nFAULT line is `NotInstalled`: no pin is configured or sampled, and `static_assert(!driverFaultLineInstalled() \|\| kMotorFaultBar != kNoPin, ...)` refuses the half-edit. No raw A1/A2 count is labelled a voltage or a current |
| Hardware facts did not move | `VERIFIED_STATE.md` has 12 hardware-table rows, all `UNVERIFIED`; no calibration was promoted; `kMaxDwellGapMs` (28 ms) is still the only `Measured` production value |

## 5. The firmware images are byte-identical across the S13 delta

The S13 delta touches two compiled translation units (`src/main.cpp`,
`src/VehicleRuntime.cpp`) by comment only. Rebuilding all three images after the
change reproduces the pre-change images exactly:

| Image | `sha256` of `firmware.elf` (identical before and after S13) |
| --- | --- |
| `nano_r4` | `1ce0187475512f09f7704e19519fc2c0c8e1c132a7dcceea424f3faebaf605ff` |
| `bench` | `2bc3fe5df3002cb5cfe94e00823b131aad68ee171c610daa832c47d19dd21297` |
| `bench-watchdog` | `c78ef5b5f9738b50ed4012e3fadb15438154e6d3a61267600703dc3c663f9239` |

That is why the freeze can name a documentation commit as the baseline without
naming a different firmware: the executable content of the freeze is the content
that was validated at `93ea1ec`.

## 6. The freeze rule

**After this record, no speculative feature may be added.** Work on the software
side is limited to:

1. **Promoting a placeholder** to `Measured` from a recorded physical measurement
   through the `VERIFIED_STATE.md` procedure (row, raw capture under
   `data/evidence/`, log entry) — including the calibration values, the safety
   limits, the control gains and the bottle-interaction geometry;
2. **Fixing a defect** that physical testing or a reproduced failure has
   demonstrated, with its evidence recorded;
3. **Correcting a document or comment** that disagrees with the frozen code; and
4. **Enabling a capability the freeze left deliberately off**, each of which is
   named here: the production watchdog
   (`EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED` is a bench-only define), the
   driver-fault and battery-voltage capabilities (two edits each, stated in
   `src/VehicleConfig.h`), and the validated-timing flag
   (`kRunTimingModelValidated`, false until P-H14 records the comparison).

Everything else — a new control strategy, an additional sensor, a new estimate, a
new command, a new telemetry field, a new tuning constant, a "just in case" limit
— requires the measured problem it solves, and it is an escalation under
`AGENTS.md` §20 and §31, not an implementation task. Any change to a
`SOFTWARE_ARCHITECTURE.md` contract records its reason in `DEVELOPMENT_LOG.md` in
the same change, as that file already requires.

## 7. Known blockers: everything the software is waiting for

Each row is a fact the firmware must consume but cannot measure for itself. None
is a software defect; each is what the next phase produces. A row that is *not
applicable* is stated as such rather than left outstanding — that distinction is
deliberate and is the reason no check is permanently unsatisfiable.

| # | Unknown | Software consequence while unknown | Resolved by |
| --- | --- | --- | --- |
| 1 | Pin map / wiring continuity and logic levels | Every pin is a stated V5 harness assignment, not a wiring record; `VERIFIED_STATE.md` #1 | P-H1A step A0 (continuity), P-H1B (input levels), P-H2 (encoder pins) |
| 2 | Driver electrical behavior and the PH/EN truth table | `ev::drv8874InputsFor()` is a planned mapping only; no level combination for a drive is ever produced by an authorized command because the polarity is Provisional | P-H1A tests 1-6 |
| 3 | Which direction is positive (motor, decoder, and driver input) | The three encodings are Provisional, so no authorized drive is produced | P-H1A/H2/H3 |
| 4 | Encoder counts per revolution, gearing, repeatability, sign | No counts-to-distance conversion is trusted; the decoder scheme itself is fixed (x2, A-channel interrupt + B level) | P-H2 (manual rotations), P-H3/H4 |
| 5 | Effective distance per encoder tick (rollout) | `kPlannedDistancePerEncoderTickM` is a placeholder, so `CalibrationMeasured` is false | P-H4 (rollout) |
| 6 | Velocity-estimator window adequacy | The window (20 ms) and the no-edge timeout (100 ms) are policy, not measurement | P-H5 (velocity trace) |
| 7 | Feed-forward / PI gains and the effort ceiling | No gain is a tuning result; `maxEffortPermille` is Provisional, so the regulator cannot be enabled and `SafetyLimits` cannot be fully established | P-H6, P-H3 |
| 8 | Braking distance per direction | Both brake offsets are Provisional (0.05 m) and a different brake-distance-versus-speed relationship is unmeasured | P-H7 parts A/B |
| 9 | Bottle-interaction geometry: contact lead, capture advance, push margin, credible contact-window tolerance, forward-stop margin beyond the Bottle Line | `buildRunExecutionPlan()` refuses with `MissingForwardExecutionTarget`, so the run has **no** forward execution target and no forward phase can complete | P-H8 (five measurements) |
| 10 | Competition-strategy speeds for the approach, capture and push phases | The three ceiling placeholders equal the configured cruise and are Provisional, so `validateRunPhaseConfig()` refuses the run policy | P-H8 |
| 11 | Every `SafetyLimits` value (`maxEffortPermille`, both velocity ceilings, `maxRunDurationS`) | `safetyLimitsValid` is false, so no run may be armed; `maxRunDurationS` is inert while Provisional even though its consumer (S10) is live | P-H3/H4/H5/H7 |
| 12 | The internal Run Time has never been compared with the official rule 8.o Run Time | The scored number stays the timekeeper's/video's; the firmware's own number is a different believed quantity | P-H10, P-H14 step 12 |
| 13 | Whether the physical timing model is good enough to choose a run's cruise and delay | `kRunTimingModelValidated` is false, so the estimate is a labelled diagnostic only and cannot change cruise, delay or refuse a legal announcement | P-H14 step 12 |
| 14 | The watchdog margin under a loaded run, and the worst control-tick gap with the drivetrain loaded | The 114 ms timeout and the 28 ms dwell-gap bound are bench measurements; a loaded run adds work to the same loop | P-H0W step 8 |
| 15 | Battery voltage and pack-current conversion | **Not applicable on this vehicle**: no divider or shunt is installed, so `minBatteryV` is a capability-conditional arming precondition, the A1/A2 counts are raw counts, and no firmware path can stop the vehicle for a low battery | Not on this build; a later build wires the sensing and follows the two-edit procedure in `src/VehicleConfig.h` |
| 16 | The external nFAULT line | **Not applicable on this vehicle**: the line is not wired (`kMotorFaultBar = kNoPin`), so there is no polarity to measure and no driver fault the MCU can observe | Not on this build; a later build wires the line and measures the polarity (H1B) |
| 17 | Which behavior the drivetrain actually produces (motion, straightness, slip, repeatability, the bottle interaction) | Nothing in the firmware may claim it; the run layer is host-tested logic only | The whole powered protocol series, P-H3 onward |

One question recorded by earlier stages remains **open and unowned by this pack**:
the arming-gate question (whether the arming authority's set of preconditions is
the final one). It is a policy question, not a measurement, and no stage in this
pack resolves it. It is restated here so the freeze does not silently decide it.

## 8. What this record does not claim

- It is not physical evidence. It does not say the motor turns, the encoder
  counts, the distance conversion is accurate, the vehicle stops where it is
  commanded to, or the bottle interaction works.
- It does not say the firmware is complete. It says the deterministic software
  work the pack defined is finished and the remainder needs the vehicle.
- It does not promote anything. All twelve `VERIFIED_STATE.md` hardware rows are
  UNVERIFIED, and the 2026-10-02/03 observations remain OBSERVED, not evidence.
- It does not authorize hardware work. The bring-up protocols
  (`docs/bringup/`) run in their own order, each with a pre-registration and a
  recorded raw capture.

## 9. Reproducing the freeze checks

From the repository root:

```
rm -rf .pio/build                       # clean build state
pio test -e native                      # 628/628 in 35 suites
pio run -e nano_r4                      # SUCCESS, 0 warnings, 6772 / 74540
pio run -e bench                        # SUCCESS, 0 warnings, 6972 / 81380
pio run -e bench-watchdog               # SUCCESS, 0 warnings, 6956 / 80808
python -m unittest discover -s tools/telemetry/tests -t tools/telemetry   # 119/119 OK
pio run -e emergency                    # must fail: UnknownEnvNamesError
```

The symbol checks in §4 use the ARM toolchain's `nm` from the project's
PlatformIO packages:

```
<toolchain>/bin/arm-none-eabi-nm .pio/build/<env>/firmware.elf | grep -E "bench|hang|emergency|h1a"
```
