# Verified State

This file records what the project **knows**, as opposed to what it assumes.
It exists to stop guesses from being treated as measured facts.

## The rule

A hardware or physical fact may be marked **VERIFIED** only when it is supported
by direct **on-target or physical evidence** that has been recorded (command or
test procedure plus raw observation/data), and the promotion is logged in
`DEVELOPMENT_LOG.md`.

None of the following make a fact verified:

- code that compiles, links, or uploads;
- a datasheet, product listing, wiring diagram, or vendor example;
- a host-side test, simulation, or unit test;
- an assumption, convention, or value that seems obvious;
- a value copied from another project;
- recollection or verbal description.

**Default status is UNVERIFIED.** A fact that is not listed here is unverified.

What this file cannot yet say is collected separately in `docs/limitations.md`:
the rows above that are still unverified, the changes that are code-only and
have never been on a board, the defects with no root cause, the constraints the
upload path imposes, and the placeholders that still carry weight. That file is a
reading list, not evidence - it changes no row here, and a row and its limitation
are updated together.

Downstream rule: code must not silently depend on an UNVERIFIED fact. Where a
value depends on one, it is configurable and clearly marked as a placeholder
(see `SOFTWARE_ARCHITECTURE.md` section 7).

## Current hardware availability

The Nano R4 controller was used for the on-target sessions recorded at the end of
this file (H0, 2026-09-28 and the H0W sessions, 2026-09-28/29: USB only, no motor
supply connected). Those sessions still say nothing about the vehicle.

The unit was taken away at the end of D31 (2026-09-29) and was in hand again on
**2026-10-02, for the pre-tournament bring-up and the tournament itself
(2026-10-02/03)**. That is when the motor supply was first connected, the harness
was reworked to the stated V5 assignment, and the vehicle was moved under firmware
control. What was seen there is recorded in "Informal physical observations,
2026-10-02 bring-up and tournament" below - **observed, not verified**: no row was
promoted, because nothing from those sessions satisfies "The rule" at the top of
this file (no recorded command or procedure, no raw observation/data, no evidence
pointer). Every hardware row below is therefore still UNVERIFIED, as listed in
`docs/limitations.md`.

**Whether the board is in hand *now* is a present-day availability constraint** to
check before planning any stage. A board that is not available limits *when* a
stage can be run; it is never evidence that the hardware work recorded above did
not happen. The earlier "the board is gone" wording (D31, D35) meant only "not in
hand at that moment", and it is superseded on that point (D36).

## Hardware facts

| # | Fact | Status | Evidence | What would verify it |
|---|------|--------|----------|----------------------|
| 1 | Wiring and pin assignments | UNVERIFIED | none recorded. Functionally exercised 2026-10-02 (informal observation, D36 - not evidence under "The rule"): the harness was physically rewired to the stated V5 assignment and the roles that were exercised behaved as their roles require (the encoder counted, the motor drove, the trigger input responded). Functional observation only - **no continuity test was performed** | Continuity check against a recorded wiring table; observed correct behavior per pin |
| 2 | Motor driver modes / truth table | UNVERIFIED | none. Drive forward/reverse were exercised end to end on 2026-10-02 (informal observation, D36 - not evidence), but no input combination was metered | Driver powered with the drivetrain unloaded; each input combination observed and tabulated |
| 3 | Motor polarity (sign of a positive command) | UNVERIFIED | **OBSERVED, informal - not evidence under "The rule"** (D36): on 2026-10-02 at low effort a forward command moved the vehicle forward and a reverse command moved it backward, both directions exercised; no capture, no procedure, no raw data and no evidence pointer exist for it, so the row is unchanged. No PH/EN/nSLEEP level was metered either (that remains row #2) | Low conservative PWM command with the observed wheel direction recorded (H3); a recorded procedure plus raw observation/data, with the evidence pointer this row can name |
| 4 | Encoder direction sign (forward vs reverse counting) | UNVERIFIED | **OBSERVED, informal - not evidence under "The rule"** (D36): on 2026-10-02, during wheel/motor movement, forward travel produced increasing ticks and reverse produced decreasing ticks, and a forward manual push counted ~1494 ticks; witnessed but uncaptured, so the row is unchanged | Hand-rotated wheel in each direction with the raw count sign recorded (H2); a recorded procedure plus raw observation/data, with the evidence pointer this row can name. Signal quality (#5), counts per revolution and gearing also remain open |
| 5 | Encoder signal quality (noise, missed/extra counts) | UNVERIFIED | Ticks were produced while the wheel turned (2026-10-02, informal observation, D36), but no noise, double-count or missed-count assessment was made | Repeated manual revolutions with count comparison; scope or count-delta check |
| 6 | Rollout (effective distance per encoder tick) | UNVERIFIED | None measured. **OBSERVED, informal - not evidence** (2026-10-02, D36): ~1494 ticks against an *intended* ~2.0 m forward push, i.e. ~1.339 mm/tick as a rough figure - one unrepeated push with no measured travel and no preserved capture, so it is not a calibration and nothing was promoted | Measured travel over multiple revolutions on a real surface, averaged |
| 7 | Sensor scaling (any sensor to engineering units) | UNVERIFIED | none | Reference measurement compared against reported value at several points |
| 8 | Control gains / tuning values | UNVERIFIED | none | Tuning against recorded closed-loop response data |
| 9 | Braking behavior and stopping distance | UNVERIFIED | none | Repeated measured stopping distance at recorded speeds |
| 10 | Wheel slip (does the motor encoder track ground travel?) | UNVERIFIED | none | Motor-encoder distance compared against physically measured travel |
| 11 | Bottle interaction behavior | UNVERIFIED | none | Recorded runs observing contact, push distance, and outcome |
| 12 | Benefit of a ground / dead-wheel encoder | UNVERIFIED | none | Measured slip magnitude large enough to justify the added hardware |

The planned interface and the bench procedure that would verify rows #1-#3 are
recorded in `docs/hardware/motor-driver-drv8874.md`. That file is a plan, not
evidence: it changes no status in the table above, and the pins it names
(`src/PinMap.h`) follow the team's stated V5 harness but remain UNVERIFIED
(row #1) - a statement of intent, not a continuity record.

**One wiring fact is now stated rather than pending (D32):** the assembled
vehicle does not connect the DRV8874's nFAULT output to the Nano. That is a fact
the team reports about the vehicle's wiring, **not a measurement**, and it is not
promoted to a row here - it is stated in `src/VehicleConfig.h`
(`kPlannedDriverFaultCapability`) and in `docs/limitations.md`, and it is why the
external driver-fault rows above have no measurement path on this hardware. No
`Measured` label was applied to anything by that change.

**The V5 pin map is likewise a statement, not a measurement (D33):**
`src/PinMap.h` now names the pins the team reports the V5 harness connects
(encoder A D2, encoder B D4, PH D8, EN D9, nSLEEP D10, trigger D12; PMODE tied
to GND in hardware; the nFAULT line has no pin). Row #1 above is not verified by
it - the map is what a continuity check (H1A step A0) would verify, and it stays
UNVERIFIED until that check is recorded. The harness **was** physically reworked
to this assignment on 2026-10-02 and the exercised roles behaved accordingly
(functional observation, D36), which is still not continuity.

**Battery and current sensing are also not installed, and that is a capability rather than an
outstanding measurement (D32, S10).** The assembled vehicle has no battery-divider or current-shunt
circuit, so `vehicleConfig::kPlannedBatteryVoltageCapability` is `NotInstalled`, the arming-time
requirement for a `Measured` `minBatteryV` is **not applicable** rather than unbuilt-and-required
(`ev::validateSafetyLimits()`), and `batt_counts`/`curr_counts` are raw ADC counts of unconnected or
uncalibrated inputs - never a voltage and never a current. Row **#7** (sensor scaling) therefore has
no measurement path on this vehicle: no divider ratio, shunt value or ADC reference relationship is
documented anywhere in the project, so P-H1B Parts 1-2 can record raw pairs but cannot establish a
conversion, and no firmware path compares a voltage or a current at run time. Nothing absent is
labelled measured, and no raw count is relabelled as engineering units.

### Informal physical observations, 2026-10-02 bring-up and tournament (D36) - OBSERVED, NOT VERIFIED

**This section promotes nothing.** It records what was seen so the fact of
observation is not lost, and it satisfies no row. Under "The rule" at the top of
this file a VERIFIED mark needs recorded evidence - a command or test procedure
plus raw observation/data - with an evidence pointer to carry, and these sessions
have **no capture, no pre-registration and no `data/evidence/` directory**: there
is no raw serial log to re-read. What exists is a single un-instrumented sighting
recorded from the team's confirmed account, which the rule names in its excluded
set ("recollection or verbal description"), and no protocol in `docs/bringup/` was
run to its acceptance rule ("change no row until they pass"). Nothing in `src/` was
changed by it either: every calibration constant and `Sourced` value is still
`Provisional`, no `Measured` label was applied anywhere, and the arm gate still
refuses.

| Observed (informal; changes no row) | Bearing on rows |
|---|---|
| The harness was rewired to the stated V5 assignment: encoder A D2, encoder B D4, DRV PH D8, DRV EN/PWM D9, DRV nSLEEP D10, trigger D12, PMODE tied to GND, nFAULT not installed | Row #1 stays UNVERIFIED - functional observation only, **no continuity testing was performed** |
| The motor hummed and spun under firmware control at low effort | No numbered row. Replaces the "no motor supply / never moved" wording with an observation; it is still not evidence |
| Forward and reverse motor commands were both physically exercised | **Row #3 stays UNVERIFIED** - observed informally, uncaptured |
| Encoder ticks changed while the wheel/motor moved: forward increased them, reverse decreased them | **Row #4 stays UNVERIFIED** - observed informally, uncaptured |
| A manual forward rollout counted ~1494 ticks over an *intended* ~2.0 m | Row #6 stays UNVERIFIED - provisional only, not a calibration |
| The physical pencil trigger actuated (it worked) | No numbered row; the trigger's raw idle/active *levels* were not recorded, so H1B Part 4 is still owed |
| The tournament-only emergency image - a separate standalone entry point, deleted from the active tree in D35 - was flashed and drove the vehicle autonomously forward and backward at the tournament | Not the production image, and not evidence: it says nothing about the run state machine, and no telemetry or run data from it exists here |

**Provisional / observed but not formal:** the ~1494 ticks over an intended ~2.0 m
and the resulting ~1.339 mm/tick estimate; the fact that low effort sufficed to move
the vehicle (the minimum useful effort was never measured); speed, braking and
reversal calibration.

**All twelve hardware rows remain UNVERIFIED** after those sessions: #1
(continuity of every connection), #2 (level-to-output truth table), #3 (sign
observed informally, no recorded raw data), #4 (sign observed informally, no
recorded raw data), #5 (signal quality), #6 (rollout as a calibration),
#7 (sensor scaling - battery and current sensing are not installed), #8 (gains),
#9 (braking), #10 (wheel slip), #11 (bottle interaction), #12 (dead-wheel
question); plus formal mm/tick calibration, Target Time accuracy, repeatability,
and any loaded behavior of the production run state machine. nFAULT external
telemetry remains unavailable (line not installed). Collected in
`docs/limitations.md`.

## Software and toolchain facts

These are verified by reproducible commands, **not** by physical testing. They
say nothing about the vehicle.

| Fact | Status | Evidence |
|------|--------|----------|
| `env:nano_r4` firmware builds | VERIFIED | `pio run -e nano_r4` SUCCESS (D35: RAM 6292/32768, Flash 71836/262144, no warnings). D34 had failed to compile (`announcedTargetTimeS` shadowing in `src/VehicleConfig.h`) and was unverified until this cleanup/rebuild (see `DEVELOPMENT_LOG.md` D35). **Re-measured at S12/D49 (2026-10-06, not the current tree): `pio run` SUCCESS, 0 warnings, RAM 6772/32768, Flash 74540/262144. Later stages re-measured on their own trees (S18: 740/740 native with the same three images rebuilt; R1U/R3: see `DEVELOPMENT_LOG.md` and `.agent-work/stages/`); this row's numbers are that session's record, not today's.** |
| Host logic tests build and run with the `native` environment | VERIFIED | `pio test -e native` 552/552 SUCCESS (D35; 32 suites, including 2 new optional-delay and 3 new derived-cruise regression tests; `test/test_emergency_run` removed with the emergency image). D34 had no green host run and was unverified until this cleanup/rebuild (see `DEVELOPMENT_LOG.md` D35). **Re-measured at S12/D49 (2026-10-06, not the current tree): `pio test -e native` 628/628 SUCCESS in 35 suites; the architecture-sync stages S3-S11 added 76 cases, including the S11 sweep of the whole legal announced parameter space. Later stages re-measured on their own trees (S18: 740/740 in 38 suites; R3: see its stage record); this row's numbers are that session's record, not today's.** |
| The analysis tooling's own test suite runs | VERIFIED | `python -m unittest discover -s tools/telemetry/tests -t tools/telemetry` **119/119 OK** (S12/D49, 2026-10-06 - that session's record, not the current tree; S7: 116/116, S8 onward: 119/119; R1U/R3 re-measured on their own trees, see their stage records). Host-side only: it parses the mirrored telemetry schema and never runs on the vehicle. |
| The vehicle-layer runtime shell and the hardware boundary compile and link into the firmware (driver adapter, encoder interrupt, raw inputs, scheduler, tick order, watchdog adapter) | VERIFIED | `pio run` succeeds with `src/` compiled (see `DEVELOPMENT_LOG.md` D13). This is a build fact only. |
| The watchdog adapter's library links into the firmware image | VERIFIED | `arm-none-eabi-nm` on `firmware.elf` lists the WDT symbols (see `DEVELOPMENT_LOG.md` D13). Linking is **not** evidence that the watchdog starts, counts, refreshes or resets. |
| Nothing but the driver adapter writes the planned motor pins | VERIFIED | A static search over `src/` for the three planned pin names matches only `src/PinMap.h` (the definitions) and `src/MotorDriver.h` (the writes), and the bench tree reaches the pins through `MotorDriver.h`'s `applyBench()` rather than writing them itself. The vehicle image does not contain the bench tree at all - see the `env:bench` row below (see `DEVELOPMENT_LOG.md` D13, D19 and D35). The tournament-only `src/emergency/EmergencyMain.cpp` (which called `writeLevels()` directly - a second writer while it existed) was deleted in D35; it had been flashed for the 2026-10-02/03 tournament and did physically drive the vehicle forward and backward there (`DEVELOPMENT_LOG.md` D36). A search is evidence about the source, not about the hardware. |
| The driver's fault output is interpreted only from a measured polarity, and an absent signal is never interpreted at all | VERIFIED | `lib/evcore/src/DriverFault.h` maps a raw level to `FaultCode::DriverFaultBar` only when the capability is `Installed` **and** the configured polarity is `Measured`; the shipped `kPlannedDriverFaultCapability` is `NotInstalled` (the assembled vehicle does not wire the line), `kPlannedDriverFaultPolarity` stays `Unverified`, and the capability-aware `DriverFaultConfigurationValid` check requires a measured polarity only on a vehicle that has the line (see `DEVELOPMENT_LOG.md` D19 and D32). This is a logic fact: **which level the line drives for each state is UNVERIFIED and, on this vehicle, unmeasurable** (rows #1, #2 above). |
| The pure core compiles for the ARM target | VERIFIED | `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over all `lib/evcore/src` headers: exit 0. The only diagnostics are the expected `#pragma once in main file` artifacts of checking headers standalone (one per header); any other diagnostic is a real warning to investigate. |
| The bench image builds, and the vehicle image contains none of it | VERIFIED | `pio run -e bench` SUCCESS (D35: RAM 6492/32768, Flash 78680/262144; re-measured at S12/D49, not the current tree: RAM 6972/32768, Flash 81380/262144, and `pio run -e bench-watchdog` SUCCESS at RAM 6956/32768, Flash 80808/262144; R3 re-measured on its own tree, see its stage record) and `pio run -e nano_r4` SUCCESS (D35, above), and a symbol search over the two `firmware.elf` files finds the bench console's symbols in the bench image and none in the vehicle image (see `DEVELOPMENT_LOG.md` D19 and D35); the two environments are disjoint by `build_src_filter` in `platformio.ini`. The tournament-only `[env:emergency]` no longer exists (`pio run -e emergency` reports unknown env; `src/emergency/` and `test/test_emergency_run/` deleted in D35, recoverable from the preservation snapshot). This is a build-and-link fact only: this row claims no instrument reading (the `bench pins`, `bench resetsrc` and `bench wdt` readings recorded in the on-target sections below came from the H0/H0W-era images), and nothing here says the readouts or the bench drive behave correctly on the board. |
| The vehicle image contains no bench-named symbol, and enabling the watchdog for a bench qualification cannot change it | VERIFIED | `arm-none-eabi-nm` over `.pio/build/nano_r4/firmware.elf` filtered for `bench|hang|emergency|Emergency|h1a` matches nothing (D35; re-measured identically at S13: 0 bench, 0 hang, 0 H1A, 0 emergency); over `.pio/build/bench/firmware.elf` it matches the console, the bench drive and the grammar plus the retained bench-only H1A diagnostic (D35; re-measured at S13: 36 bench, 5 H1A, 0 emergency), and over `.pio/build/bench-watchdog/firmware.elf` it matches bench but no H1A and no emergency (D35; re-measured at S13: 31 bench, 0 H1A, 0 emergency). The deliberate hang hook is a compile-time constant at its inert shipped value (`kDeliberateHangAfterPasses = 0U`, `src/bench/BenchMain.cpp`), so the compiler removes its code and a `hang` filter matches nothing in any of the three images; D35's symbol record and the H0W campaign build that set the hook to `460000U` (D27, D30) are preserved. Watchdog enablement is derived from the build define `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`, which only `[env:bench-watchdog]` sets, and `src/main.cpp` has a `static_assert` that refuses to compile the vehicle image if the define is ever present (see `DEVELOPMENT_LOG.md` D20) plus a D35 `#error` guard refusing `EV_EMERGENCY_IMAGE`/`DEV_EMERGENCY_IMAGE` in the vehicle image. Build facts only: nothing here says the bench instruments or the watchdog behave correctly on the board. |
| Only one writer commands the motor pins per pass, and the bench writer is gated on a host-tested predicate | VERIFIED | `src/MotorDriver.h` skips the tick's write while the bench layer holds the pins (`motorDriver::setMotorPinsOwned()`), and the bench layer sets that only where `ev::benchApplyVerdict()` (`lib/evcore/src/MotorArbitration.h`) permits - refusing on a latched fault, a violated control-tick order, an armed run or a run under way; `VehicleRuntime` exposes `faultLatched()`, `runUnderway()` and `orderViolationDetected()` for it, and `test_bench_arbitration` covers the predicate including the two states that are `!armed` while the motor is *not* free (see `DEVELOPMENT_LOG.md` D20). This is a logic-and-build fact. `failSafe()` deliberately ignores the flag, and whether the DRV8874's pins are commanded as intended is UNVERIFIED (rows #1-#3; the sign of a commanded forward/reverse was only observed informally on 2026-10-02 and is not evidence, so #3 remains in this set, and no level was metered). |
| The driver-fault-low cycle counter is per run, not since boot, and counts nothing while the line is absent | VERIFIED | `VehicleRuntime::cyclesWithDriverFaultLow_` (`src/VehicleRuntime.cpp`) is cleared on a successful `arm` and by `reset`, so H1B Part 3 and H9 read their own test's activity rather than a since-boot total (see `DEVELOPMENT_LOG.md` D20); it is incremented only when the capability is `Installed`, so an unread pin's toggles are never counted as fault evidence (D32). A source fact only: the counter's value on hardware has not been observed, and on this vehicle it stays zero because there is no signal. |
| The watchdog timeout rule is enforced at compile time | VERIFIED | `lib/evcore/src/WatchdogPlan.h` holds the rule (a timeout must be strictly longer than both the boot-to-first-refresh gap and the worst healthy refresh interval, plus margin) and `src/WatchdogCheckpoint.h` asserts the requested constants against it, so a build that requests enablement with a timeout that cannot survive boot fails to compile; `test_watchdog_plan` pins the arithmetic. **The two gaps are now measurements, not placeholders** (`kBootToFirstRefreshBudgetMs = 0`, `kWorstRefreshIntervalBudgetMs = 14`, `kWatchdogPlanMarginMs = 100` pre-registered, `kWatchdogTimeoutMs = 114` - D27), and the assertion applies the whole rule plus an equality check against `requiredWatchdogTimeoutMs()`. This row stays a source-and-logic fact; the measurements themselves are the H0W row below. |
| The operator interface and the telemetry consumer compile and link into the firmware (command parser, operator policy, response formatter, telemetry schema/formatter/queue, line transport) | VERIFIED | `pio run` succeeds with `src/` compiled (see `DEVELOPMENT_LOG.md` D14), and the pure modules are covered by host tests (`test_serial_command_parser`, `test_operator_policy`, `test_operator_response`, `test_telemetry_format`, `test_telemetry_queue`). This is a build-and-logic fact only; no host was connected. |
| No control path depends on the operator interface or the telemetry session, so ignoring or abusing serial cannot change what the chain decides | VERIFIED | `test_serial_independence` runs the same scripted run twice - once with the consumer hierarchy fully driven by a stalled host and a stream of admitted, rejected and refused commands, once without it - and asserts equal tick counts, equal dropped deadlines and an identical per-tick trace of authorization outcome, motor mode and effort; separately that no tick ever produced `AuthorizedDrive`/`AuthorizedClamped`, that the sample lane holds exactly `kTelemetrySampleLaneCapacity` before counting drops, and that every queued-plus-dropped sample is accounted for. This is evidence about the logic, not about a real serial link. |
| A `config set` whose supplied value is non-finite or outside the rules' range changes nothing: it is refused with the rule error it broke, and the held configuration keeps the value it had | VERIFIED (host tests) | `ev::validateSuppliedConfigFields()` (`lib/evcore/src/OperatorPolicy.h`) checks only what one field can fail alone (finiteness, rules 8.h/8.i/8.j ranges, a real level index); `VehicleRuntime::handleCommandLine()` runs it for `ConfigSet` before the command counts as accepted, answers with `formatConfigSetRefusal()` (`ERR,cmd=config set,reason=ValueOutOfRange,error=<rule error>`, no configuration echo) and pushes a `CommandRejected` event. Pinned by the new cases in `test_operator_policy` and `test_operator_response` (D22, fixing D21 F-5). **Confirmed on the board in the H0 repeat (D25)**: three identical rounds of `config set distance=9.50` (rule range 5.00-9.00) each answered `ERR,cmd=config set,reason=ValueOutOfRange,error=TargetDistanceOutOfRange`, and each following `config show` still reported `target_m=7.5000,...,rules=1` - the held configuration did not change, which is what the first H0 session failed on. Evidence: `data/evidence/P-H0-repeat-2026-09-28/h0repeat-config-vehicle.log`. In-range-but-off-grid values are deliberately still accepted at the door and judged by the whole-configuration check. |
| The control scheduler reports the session's cadence extremes, not only the last tick's values | VERIFIED (host tests + on target) | `ControlScheduler::worstLateByMs()` tracks the maximum lateness over every committed tick and `droppedDeadlines()` the cumulative drop count (`lib/evcore/src/ControlScheduler.h`); `test_control_scheduler`'s `test_session_extremes_are_not_the_last_tick` shows a later smaller lateness does not lower the extreme and that the dropped count accumulates (D22, fixing D21 F-6). **Read from a board in the H0 repeat (D25)**: two `bench health` readings 25.5 s apart on `env:bench` both reported `worst_late_ms=1980,dropped_total=99` with the last tick's `late_ms=0,dropped=0` and `violations=0`, so the extremes were visible **and flat** while the loop ran healthily at 50 Hz with the host reading (`control_ticks` 773 -> 2046). Both are produced by the bounded boot wait before the loop starts - every unhosted-boot capture opens with a sample at `dt_ms=2000`/`t_ms=2000`, i.e. a first tick ~1980 ms late and ~99 deadlines elapsed - and this is why the watchdog is opened after that wait rather than before it. Evidence: `data/evidence/P-H0-repeat-2026-09-28/h0repeat-bench-health.log`. |
| `pio run -t upload` performs the board's 1200-bps touch itself, so the upload enters the bootloader with no manual step | VERIFIED (on target, 14 runs) | `scripts/nano_upload_touch.py` is registered as `extra_scripts = post:` in `[env:nano_r4]` and inherited by `env:bench`/`env:bench-watchdog` while `env:native` keeps only its own `post:` script - `pio project config` output recorded in D22. `post:` is load-bearing: attached from a `pre:` script the action binds to a same-named stand-in filesystem node and never runs (measured with a standalone SCons probe, D22), and the script now checks the node type at load time so that failure cannot be silent. The attachment was confirmed **in this project's own environment graph**: `NANO_TOUCH_REPORT=1 pio run` (and `-e bench`) prints `nano_touch: attached the touch to a Alias target`. The touch was then run three times against the board (application mode, `COM3`, `USB VID:PID=2341:0074`) - `pio run -t upload` each time touched the port, detected the bootloader, and `dfu-util` flashed 66612 bytes to `Download done.`/`Done!` with exit status 0, three times out of three, with no manual touch: `d22-f1-upload-verify.log`. The script's own control flow was also exercised on the host without a board: non-dfu is a no-op, a missing port prints guidance and does not raise, an already-present bootloader short-circuits, the touch issues 1200 bps with DTR/RTS low, the retry fires once when no bootloader appears, and a registration that did not land on the `upload` alias is reported. One caveat on the observed run: the port tears down while the touch is being applied, so pyserial reports `OSError(22, 'A device which does not exist was specified.')` at that point; the hook prints it and continues, and the board did enter its bootloader both times it appeared. **The H0 repeat (D25) then ran 14 uploads end to end (10 vehicle, 3 bench, 1 restore), all succeeding on the hook's first touch attempt with no manual step** (`data/evidence/P-H0-repeat-2026-09-28/h0repeat-uploads.log`). The hook still never fails an upload by design, and a port held by another program is still indistinguishable from the expected teardown (`docs/limitations.md`). |
| The boot line is owed to whatever reader is attached, and re-owed for a reader that (re)appears or stays silent, so a board that booted with nobody reading is still greeted | VERIFIED (host tests + on target, D25) | The decision is the pure `ev::HostGreetPolicy` (`lib/evcore/src/HostGreetPolicy.h`, D23): a control line that newly goes high (**either** DTR or RTS, via `serialTransport::hostLineState()` - never `Serial.dtr()`, which flips `ignore_dtr` and would let `write()` succeed with no host) and host silence at `kSilenceMs = 10000` each owe the greeting, with silence re-owing it every interval; a byte from the host **restarts** that interval rather than owing the greeting (corrected in D24 - the code and `test_a_host_byte_restarts_the_silence_interval` always did the latter, and the earlier wording here, in D23 and in the header comment overstated it), so a typed session is greeted once rather than once per command, and a reader that presents no edge and types continuously postpones the greeting (`docs/limitations.md`). `serviceStagedText(nowMs)` samples the lines at a bounded rate and stages the line only while `dtr` is high - the predicate `Serial.write()` gates on - so a greeting is never committed to a port that cannot move it; `drainTelemetry()` still refuses to pop a record with no host. `boot` re-sends the same line on demand (`stageBootLine()`, shared with the automatic greeting; admissible in every state). Pinned by `test_host_greet_policy` (7 cases: level versus edge, greeted-once-per-reader, the byte restarting the interval, the silence boundary and its repetition) plus the parser, policy and help-table cases for `boot`. **All of that is now observed on the board (H0 repeat, D25; current source, vehicle image hash `cfd288d4...`)**: a host attaching to a board that had booted with nobody reading received `BOOT,...` as its **first line in 7 of 7 attaches** across both images (D22 saw 2 of 3); the **silence rule fired and repeated** - a beacon ~10.0 s after the last delivery or host byte in every capture long enough to contain one, in five vehicle captures and the bench capture; the **byte rule restarted the interval without owing a greeting** (commands typed at 1.55-10.02 s in one capture produced no beacon near the 10.06 s deadline the attach alone would have produced, the next arriving 10 s after the last byte); and **`boot` was answered by the same line the greeting sends, verbatim**. On the **bench image** the byte input never fires - `src/bench/BenchMain.cpp` runs the console's own read loop and the console forwards lines to `runtime.handleCommandLine(...)` rather than running `VehicleRuntime::serviceSerial()`, where `observeHostByte()` is called - so beacons there are evidence about the silence rule only. A host that presents no control-line edge and never writes is still indistinguishable from the previous reader: the silence rule bounds that case rather than removing it, and a host that presents no edge but writes continuously defers it. D22's three attaches remain recorded verbatim in `d22-f4-attach-observations.txt`, including the one that was not greeted.
| The repository is frozen at a validated pre-hardware revision, and the freeze is not a physical claim | VERIFIED | `docs/PRE_HARDWARE_FREEZE.md` (S13/D50) records the validated revision (`git rev-parse HEAD` = `93ea1ecd8325670b702291ca78398cb5080f6bb3`, tree `60026bed69c23cfc0b2fe09f9be5d8725b3877b1`), the full validation re-run from a clean build state (native **628/628** in 35 suites; `nano_r4`/`bench`/`bench-watchdog` SUCCESS with 0 warnings at RAM 6772/6972/6956 and Flash 74540/81380/80808; telemetry **119/119 OK**; `pio run -e emergency` failing with `UnknownEnvNamesError`), the static separation checks (the vehicle image's symbol table contains no bench, H1A, hang or emergency symbol, each image has exactly one `setup`/`loop`, and the only writer of the three motor pins is `src/MotorDriver.h`), and all seventeen remaining physical-unknown groups with the protocol that resolves each. The three images rebuild **byte-for-byte identical** across the documentation-and-comment-only S13 delta (`sha256` listed in the record). This row moves no hardware row: all twelve remain UNVERIFIED, and nothing in the record was observed on hardware - it is a software/compile fact only. |

### On-target observations (H0 session 1, 2026-09-28)

These rows are read from the **board**, not from source: the vehicle image was
flashed over USB DFU and its own output captured. They are still not statements
about the vehicle, which has no drivetrain connected. Captures and the full
tally: `data/evidence/P-H0-2026-09-28/`; log entry D21. This first session recorded
what it saw as **not a qualified stage** - its upload-reliability clause failed -
and the findings it raised (F-1, F-4, F-5) were then addressed in code (D22) and
re-tested; D23 changed the greeting again. **The repeat session (D25) met H0's
pre-registered criteria and H0 now passes** - see "On-target observations (H0
repeat)" below, which supersedes the last row here and the "unverified" notes in
the rows above. D22's on-target F-4 observations belong to the **D22** image; the
current image's greeting behaviour was observed in D25.

| Fact | Status | Evidence |
|------|--------|----------|
| The board is detected as `USB VID:PID=2341:0074 SER=350D331C5A333131021133354B573462` on `COM3`, and its bootloader mode presents `USB\VID_2341&PID_0374&REV_0100` (`Nano R4 DFU`) | VERIFIED (on target) | `pio device list` and `Get-PnpDevice` output in `data/evidence/P-H0-2026-09-28/README.md` and `toolchain.session.txt`. |
| `dfu-util` can open that bootloader **only** after Arduino's own `renesas.inf` WinUSB driver is installed; without it Windows reports problem code 28 for `USB\VID_2341&PID_0374&REV_0100`, which is exactly the hardware ID that INF matches, and the upload fails with `Error 74` | VERIFIED (on target) | Same transcript, plus the successful download that followed (`Download done.`, 65964 bytes) - see D21 F-2. |
| PlatformIO's `renesas-ra` DFU uploader performs **no** 1200-bps touch, so a bare `pio run -t upload` cannot put this board into bootloader mode; a host-issued touch is required | VERIFIED (source + observed) | `builder/main.py` wires `BeforeUpload` (the only caller of the touch) into the `sam-ba` branch only; the first upload failed with `No DFU capable USB device available` and the same command succeeded immediately after a host touch. D21 F-1. **Since D22 the repository performs that touch as a pre-upload step**, and three uploads on target entered the bootloader with no manual touch (row in the table above). |
| The vehicle image boots and reports its identity: seven observed boots each produced exactly one `EVE1,ver=1,t_ms=0,phase=Idle,armed=0,event=Boot,...` and answered `VERSION,fw=ev-nano-r4-p7b,telemetry_schema=1` | VERIFIED (on target, 7 boots) | `h0-boot1.log`, `h0-cycle-2..5.log`, `h0-powercycle.log`, `h0-cycle-7.log`. The `BOOT,...` line `P-H0` Step 1 expects did **not** appear in any of them, cause undetermined: D21 F-4. Addressed in D22 and **verified on target** the same day: a reader attaching after a boot that had no host received `BOOT,...` as its first line (attach 1 and attach 3 of three; `d22-f4-attach-observations.txt`), while a second attach was not greeted. That observation belongs to the D22 source - D23 changed the greeting again (the boot-line row in the table above is current). |
| On target the control period is 20 ms; telemetry is enabled at boot at stride 1 (50 Hz); `telemetry rate 10` gives exactly 100 ms between samples (a whole number of ticks); `rate 100` and `rate 0.5` are refused as `AboveControlCadence` and `BelowMinimum` | VERIFIED (on target) | `h0-rate.log`: 70 consecutive samples 20 ms apart, then 90 consecutive samples 100 ms apart, then both named refusals with `CommandRejected` records. |
| With no host reading the port, records are dropped rather than waited on, and reconnecting flushes only the queue's own capacity of stale records | VERIFIED (on target) | `h0-boot1.log` opens with `drop_s=1790` after ~37 s with no host and shows the 8 stale `t_ms=2000..2140` samples (the `TelemetryQueue` lane capacity) before current ones; no new drops while the host kept up. |
| The operator interface answers in the documented shapes on the board, and refuses malformed input by name with no `OK` and no state change | VERIFIED (on target) | `h0-config.log`: `ConfigIncomplete` and `PolicyNotUsable`/`RunPolicyUsable` announce refusals, `NoRunAnnounced` for `arm`, `checks` `all=0,count=11,first=ConfigurationValid,mask=63427`, and `InvalidValue`/`UnknownSubcommand`/`UnknownCommand`/`WrongTokenCount`/`ValueOutOfRange`/`UnknownLevel` for the malformed set, with `status` still `phase=Idle`. One caveat: on that session an **out-of-range value was not refused but held** (`OK,cmd=config set` with `rules=0`), so that one did change state - D21 F-5, since changed in D22 (refused with no state change) and not yet re-tested on the board. |
| **Upload reliability was NOT established by this session.** Seven of eight attempts succeeded; one left the board present on the USB bus but not enumerating (`Unknown USB Device (Device Descriptor Request Failed)`, `USB\VID_0000&PID_0002\5&28FFA4CE&0&3`), reachable by no software path and recovered only by a physical power cycle. Cause not determined | SUPERSEDED by the H0 repeat (D25) for the reliability clause; **F-3 itself remains an open limitation** | The tally and the Windows state are in `data/evidence/P-H0-2026-09-28/README.md`; D21 F-3. The repeat ran 14 consecutive successful uploads with no recurrence (`data/evidence/P-H0-repeat-2026-09-28/h0repeat-uploads.log`); one unreproduced occurrence is not a root cause, so F-3 is carried in `docs/limitations.md`. |

### On-target observations (H0 repeat, 2026-09-28, log entry D25)

Second on-target session, same day, same board, USB only, **no motor supply**. Its
pre-registration is `data/prereg/P-H0-repeat-2026-09-28.md` and its captures are
in `data/evidence/P-H0-repeat-2026-09-28/`. Vehicle image
`sha256=cfd288d455c2cf6c4d15f4b236827ee3e217f40ffe623b71ef301c265b38b788`
(66836 B), bench image `sha256=71f42e40417d2dcd780607f0e0d6ab04715bea1f992ae37ab46c29e22773e9a7`.
The H0W rows below continued the same session (log entries D26-D28, both bench
images rebuilt twice as instrumentation was added); their pre-registration and
captures are in `data/prereg/P-H0W-2026-09-28.md` and
`data/evidence/P-H0W-2026-09-28/`.
These are still not statements about the vehicle.

| Fact | Status | Evidence |
|------|--------|----------|
| Uploading is reliable over a pre-registered series: 10 of 10 `pio run -t upload` and 3 of 3 `pio run -e bench -t upload`(plus one restoring upload) all entered the bootloader with **no manual touch** and flashed successfully | VERIFIED (on target, 14 runs) | `h0repeat-uploads.log`: every attempt shows `nano_touch: touching COM3 at 1200 bps (attempt 1/2)`, `bootloader present; uploading`, `Download done.`/`Done!`, exit 0, and `COM3` re-enumerating with the same VID/PID/serial. The hook never needed its second attempt. The expected `OSError(22, ...)` printed on each touch. **F-3 did not recur** (see `docs/limitations.md`). |
| Telemetry on this board is enabled at boot at `stride=1`/50 Hz, `telemetry off`/`on` stop and resume it, and `telemetry rate <hz>` moves the sample spacing to a whole number of control ticks | VERIFIED (on target) | `h0repeat-telemetry-vehicle.log`: `TELEMETRY,enabled=1,stride=1,rate_hz=50.0000,drop_s=1785` at boot; over 2488 samples, 2305 consecutive 20 ms intervals and then 180 intervals of exactly 100 ms after `rate 10` (`stride=5`), every sample `dt_ms=20`; `rate 100` -> `reason=AboveControlCadence`, `rate 0.5` -> `reason=BelowMinimum`. The 46 s default-rate soak is the >= 1500-tick soak P-H0 asks for. |
| With no host reading, records are dropped rather than waited on, and reattaching flushes only the queue's own lane of stale records | VERIFIED (on target) | `h0repeat-reattach-vehicle.log` (20 s unhosted): reattach opened with `BOOT,...` first, then exactly **8** stale records (`t_ms=264253..264953`, the lane capacity, 100 ms apart at the then-current stride), then jumped 19.7 s to current records; `drop_s=365` at that point and rising only while nobody read; `drop_e=0`. |
| The boot line is a live status line, not a frozen boot record | VERIFIED (on target) | After `telemetry rate 10` was accepted, beacons later in the **same boot** reported `stride=5` (`h0repeat-telemetry-vehicle.log`), while the boot record at `t_ms=0` had reported `stride=1`. |
| Every verb in `help` is answered in the documented shape, and no malformed or refused input changes the state | VERIFIED (on target) | This session plus D21 cover `version`, `status`, `checks`, `boot`, `config show/check/set/clear`, `announce`, `arm`, `disarm`, `reset`, `telemetry on/off/status/rate`, `help`. `h0repeat-inspect2-vehicle.log` adds `config clear` -> `OK,cmd=config clear` with an empty `CONFIG,...complete=0`, `disarm`, `reset` (`event=ChainReset`), `status`, `config check`. Three identical rounds of the malformed set (`ValueOutOfRange`, `InvalidValue`, `UnknownSubcommand`, `UnknownCommand`, `WrongTokenCount`, `UnknownLevel`) each produced a named `ERR`, an `EVE1` `CommandRejected` event, and `status` still `phase=Idle,armed=0,motor=Disabled,effort_permille=0,fault=None,authz=AuthorizedStop`. `announce` -> `PolicyNotUsable,first_fail=RunPolicyUsable`; `arm` -> `NoRunAnnounced`. |
| The bounded boot wait is a ~2 s stall that the loop cannot hide: the first control tick after an unhosted boot is ~1980 ms late and ~99 deadlines elapse in it | VERIFIED (on target) | `late_ms`/`dropped` extremes `worst_late_ms=1980`/`dropped_total=99` in `h0repeat-bench-health.log`, and every unhosted-boot capture opening at `dt_ms=2000`/`t_ms=2000`. `setup()`'s bounded host wait is the cause; this is the measured reason the watchdog is opened at the **end** of `setup()` (`src/WatchdogCheckpoint.h`), and it is an input to H0W's timeout choice. |
| The firmware re-initializes the driver pins to the de-energized combination after a watchdog reset, as the MCU itself reports them | VERIFIED (on target, D28; **MCU level only**) | `bench pins` (D28) reads the three pins the firmware drives with `digitalRead()` -> `R_IOPORT_PinRead()` and pairs them with the duty the adapter last wrote, because a PWM duty cannot be read back; `deenergized` is `ev::drv8874InputsAreDeEnergized()` applied to those levels. After each of **6 resets** in the D28 hang campaign the readout was `nsleep_high=0,phase_high=0,enable_high=0,duty_permille_written=0,deenergized=1` (6/6), and the final hang campaign added 3 more with the same values (9/9); readings taken before any hang are identical. This is the "MCU pin levels" H0W's Step 3 asks for - **not** a voltage measurement at the header, so it would not see a pin driven low but held high, and whether these levels de-energize the **driver outputs** is H1A's question. Every observed reset followed a drive-free hang, so a reset taken while an effort is commanded is not covered. Evidence: `data/evidence/P-H0W-2026-09-28/h0w-step8-pins-after-reset.log`, `h0w-step7-final-hang.log`. |
| The control loop's worst stall between two consecutive control ticks is 28 ms, and that measurement - not the 20 ms period - bounds credited near-zero dwell | VERIFIED (on target, D28) | `ControlScheduler::worstStallMs()` (host-tested by `test_control_scheduler`) tracks the longest gap between two committed ticks, excluding the first so the ~1980 ms boot gap cannot dominate it, and B1 reports it as `worst_stall_ms`. Measured **28 ms** in a 57 s baseline soak (identical at both ends), **21 ms** under telemetry-off and under the highest admissible rate with the host reading, **22 ms** with the host not reading, and **27 ms** in a 45 s soak on the final source. `kMaxDwellGapMs = 28U` (`src/VehicleConfig.h`), marked `Measured` and cited through `kMaxDwellGapEvidence` (`Sourced::citesEvidence()`, which is what `MeasuredValuesCiteEvidence` reads - that check passed vacuously before and passes on the citation now, and it bites only when a promoted value cites nothing), replaces the 40 ms `kPlannedMaxDwellGapMs` placeholder in both `NearZeroConfig` builders. **Nothing became armable**: `validateNearZeroConfig()` still refuses both near-zero configurations because their threshold and dwell remain `Provisional`, and the telemetry still reports the first failing check as `ConfigurationValid` in the D28 captures. It is a **bench** measurement and a re-measurement target for H0W Step 8, not a ceiling. |
| The main loop is free-running: it iterates ~46,000-47,600 times per second while the control tick fires exactly 50 times per second, so B1's `passes` counts loop iterations, and an enabled watchdog is refreshed per iteration (~21 us), not per control period | VERIFIED (on target) | `h0w-step0-baseline.log` (+2,664,382 passes and +2,799 control ticks in 56 s) and `h0w-step1-enabled.log` (+2,576,103 passes, +2,799 control ticks in the same window; with enablement `refreshes` rose by the same 2,576,103 and `withheld=0`). This is the measurement a watchdog timeout has to be chosen against: what it must survive is the gap before the first pass and a stall inside a pass, not the 20 ms period. D26. |
| **H0W Steps 0-7 - the prepared watchdog path, on target, with every acceptance clause met (the pin clause closed by the MCU pin read in D28).** With enablement requested through `[env:bench-watchdog]`, the timer opened at both 250 ms and the chosen 114 ms; the loop refreshed it once per pass; **no reset occurred in any healthy soak across four load conditions**; a deliberately hung loop **reset 8/8 times at 250 ms and 6/6 at 114 ms**, with the hang-to-device-gone latency scaling with the request (171-344 ms vs 0-141 ms) and the port re-enumerating each time; **5/5 recovery uploads succeeded into a looping board with no manual intervention**. The worst healthy refresh interval measured **14 ms** (identical across telemetry off, 50 Hz read, 50 Hz unread, and after a reset) and the boot-to-first-refresh gap is **sub-millisecond** (`first_refresh_ms=0`) | VERIFIED (on target) for every clause of the acceptance rule, so H0W is recorded as **passed for the bench load** (D28). The last clause - the post-reset pin levels - is met by an **MCU-level** read: `bench pins` (D28) reported the de-energized combination after **6/6** resets in the D28 hang campaign, plus 3 more in the final hang campaign. That is the "MCU pin levels ... (levels only)" the protocol's Step 3 asks for; it is not an electrical measurement at the header and says nothing about the driver outputs (H1A) | Evidence: `data/evidence/P-H0W-2026-09-28/` (`h0w-step0-baseline.log`, `h0w-step1-enabled.log`, `h0w-step2-*.log`, `h0w-step3-hang-cycles.log`, `h0w-step5-*.log`, `h0w-step7-*.log`, `h0w-step8-*.log` with their `.times.txt` arrival indexes), D26/D27/D28 and the preregistration. The chosen timeout is **114 ms = max(gap 14, boot 0) + the pre-registered 100 ms margin**, asserted at compile time; it is a **bench** number and provisional for the loaded run path until **Step 8** is repeated there (owed, and now pre-registered in `data/prereg/P-H0W-step8-2026-09-29.md`). The realized period is not the requested value; it is its own row below, measured from the hardware in D30. `opened()` is reported, so a refused open cannot be silent. |
| A deliberately hung loop's reset is attributable to the **watchdog** from the MCU's own reset-status registers rather than by inference | VERIFIED (on target, D30) | `bench resetsrc` reads `R_SYSTEM->RSTSR0/RSTSR1` and decodes the vendor header's masks; `bench resetsrc clear` clears both. The controlled signatures on this board: **power-on** `porf=1, wdtrf=0, swrf=0` (`rstsr0=1, rstsr1=0`); **upload/DFU** `porf=0, wdtrf=0, swrf=1` (`rstsr1=4`); **watchdog** `porf=0, wdtrf=1, swrf=0` (`rstsr1=2`). After clearing everything to zero, **5/5** deliberate-hang resets read `rstsr1=2` - the WDT flag alone, with no power-on, software, independent-watchdog, bus, parity or security flag set. The clear is load-bearing, because the flags latch across software resets: a `wdtrf=1` left by the D27/D28 campaign survived every later upload (including a different image) until D30 cleared it, while a genuine power cycle **clears** RSTSR1. Before this, H0W's attribution rested on the reset latency matching the request plus the absence of any other refresh path; this is the silicon stating which reset occurred. Evidence: `data/evidence/P-H0W-2026-09-28/h0w-step3-resetsrc-*.log` (with `.times.txt`). |
| The realized watchdog period for the **114 ms** request is **174 ms** - the request is not what the timer programs | VERIFIED (on target, D30) | `bench wdt` reads the hardware rather than the request: `WDTCR.TOPS=3` (16384 cycles), `CKS=5` (/256), `RPSS=3`/`RPES=3`, `pclkb_hz=24000000`, with the live down-counter sitting at `cntval=16370-16371` against a 16384 reload. The period follows as 16384 x 256 / 24 MHz = **174.76 ms**; `wdt_realized_ms` (the WDT library's own computation, whose lookup tables were checked against the vendor's `wdt_timeout_t` and `wdt_clock_division_t`) reports the same 174. Reproduced across two boots and a power cycle. **The pre-registered 100 ms margin was applied to the request**, so the realized margin over the worst measured refresh gap (14 ms) is 160 ms rather than 100; the timeout keeps its value because the rule chooses it, and the rounding only lengthens the period. The host-observed reset latency cannot measure this quantity - the capture's serial read window quantizes it to ~200 ms, the same order as the period - which is why the registers and the library are the evidence. Whether 174 ms is a credible stop for the vehicle is a loaded-run question and belongs to Step 8. Evidence: `h0w-step7-wdt-registers.log`, `h0w-step3-resetsrc-powercycle.log`. |
| A `BOOT,...` line in a running image is a **beacon, not evidence of a restart**: it arrives mid-stream with the sample clock continuous and no boot event | VERIFIED (on target, D31 - ad-hoc, not pre-registered) | `data/evidence/D31-2026-09-29/d31-prechange-vehicle.log`: 651 consecutive `EVS1` samples span `t_ms=1521430 -> 1534430` with `dt_ms=20` on every one and **zero** `EVE1` lines, while a single `BOOT,fw=ev-nano-r4-p7b,...` line arrived between two samples 20 ms apart (line 486, host time 9.699 s). The board had been up 1521 s and did not reset. This is the D30 caveat (`BOOT` is re-sent as the silence beacon every `kSilenceMs`, so only `EVE1,...,event=Boot` marks a restart) confirmed from the opposite direction - a beacon observed with **no** restart to explain it. Same capture: `phase=Idle`, `armed=0`, `motor=Disabled`, `effort_permille=0` on all 651 samples, and `pos_ok=0`/`vel_ok=0` throughout, which is the expected refusal while the distance scale is unmeasured. |

### Behavior this build depends on

The first two rows are read from the **installed** Renesas core sources this build
actually compiles, so they are statements about the code the firmware will run
rather than about a datasheet or an example; the third is about the host test
build. None of them is a bench observation: how a *real* host drains the port was
not measured.

| Fact | Status | Evidence |
|------|--------|----------|
| On this target `Serial` is USB-CDC, and `_SerialUSB::write(const uint8_t*, size_t)` **blocks** until the host drains the buffer; it also disables the USB interrupt while it waits and returns 0 when no host is connected. `availableForWrite()` does not block. | VERIFIED (source inspection) | `~/.platformio/packages/framework-arduinorenesas-uno/cores/arduino/usb/SerialUSB.cpp`: `write(const uint8_t *buf, size_t size)` loops on `tud_cdc_write_available()` until every byte is sent, with `usbd_int_set(false)` around the loop. `src/SerialTransport.h` therefore gates every write on `availableForWrite()`, as `SOFTWARE_ARCHITECTURE.md` section 9 requires. |
| The USB-CDC endpoint buffers on this board are 256 bytes each way, with a 64-byte endpoint packet size. | VERIFIED (source inspection) | `~/.platformio/packages/framework-arduinorenesas-uno/variants/NANOR4/tusb_config.h` defines `CFG_TUD_CDC_RX_BUFSIZE` and `CFG_TUD_CDC_TX_BUFSIZE` as `(TUD_OPT_HIGH_SPEED ? 512 : 64) * 4` and `CFG_TUD_CDC_EP_BUFSIZE` as `(TUD_OPT_HIGH_SPEED ? 512 : 64)`. The high-speed branch does not apply here: the variant sets `CFG_TUD_MAX_SPEED` to `OPT_MODE_DEFAULT_SPEED`, which is `0x0000` (`tusb_option.h`), so `TUD_OPT_HIGH_SPEED` falls back to `TUP_RHPORT_HIGHSPEED`, and the RA family is not one of the MCUs that define it as `1` (`tusb_mcu.h`). The result is a small window, which is why a record is dropped rather than waited on. |
| Unity's double assertions are disabled in this project's test build, so `TEST_ASSERT_EQUAL_DOUBLE` cannot be used and host tests compare doubles with an explicit tolerance and `fabs`. | VERIFIED | The vendored Unity (`Unity/src/unity_internals.h`) defines `UNITY_EXCLUDE_DOUBLE` unless `UNITY_INCLUDE_DOUBLE` is defined, and this project's `unity_config.h` does not define it. |

## Rule source

The event rules are an official document, not a measurement. A rule citation
establishes what the event requires and is recorded as `ValueSource::RuleDerived`,
never as `Measured`.

| Item | Status | Evidence |
|------|--------|----------|
| Official event rules for the 2027 season | AVAILABLE | `docs/rules/2027-div-c-electric-vehicle.pdf` (Division C rules; Electric Vehicle event on pages C26-C31). Extracted values are recorded in `docs/rules/README.md`; the authoritative copy in code is `ev::rules2027`. |

The rule-derived facts used by the firmware are Target Distance (rule 8.h),
Target Time (rule 8.i), Bottle Line Distance (rule 8.j), and bottle placement
(rule 7.c.ii.(5)). They constrain the *event*, not the vehicle, so they do not
appear in the hardware table above.

## How to promote a fact

1. Perform the test on real hardware, following the safety rules in `AGENTS.md`
   (unloaded drivetrain, conservative PWM, one behavior at a time). The exact
   procedures - prerequisites, variables, measurements, repetitions, safe-stop
   conditions and the acceptance rule - are in `docs/bringup/`. They are written
   ahead of the hardware, are not evidence, and change no row until they pass.
2. Record the raw measurement or observation, not just a conclusion.
3. Update the row: status **VERIFIED**, evidence pointer, and date.
4. Add an entry to `DEVELOPMENT_LOG.md` describing what was tested and observed.
5. Narrow or delete the matching entry in `docs/limitations.md`, so the register
   keeps matching what the project can claim.
6. If the result contradicts an assumption, fix the assumption; do not adjust
   the numbers to preserve it.
7. Carry the evidence pointer into the code: a value promoted to `Measured` (or
   `RuleDerived`) must name where its evidence is recorded, through
   `Sourced::citesEvidence()`, or the `MeasuredValuesCiteEvidence`
   configuration-build check reports false and the arm gate refuses. A promotion
   without a citation is not a promotion - it is the one edit that lets a
   provisional number start authorizing motion while looking like every other
   measured one.
