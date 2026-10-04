# Known limitations

This file is the single place to read what this project **cannot yet claim**.
`VERIFIED_STATE.md` records what is known; this records the other half - the
limits of that knowledge, the defects with no root cause, the behavior that has
never been observed on hardware, and the placeholders that still carry real
weight.

## The rule

A limitation is removed by **evidence**, never by editing this file. Every entry
names where its evidence (or its absence) is recorded and what would remove it.
Nothing here is evidence, nothing here changes a `VERIFIED_STATE.md` row, and an
entry marked "unobserved" must not be read as "probably fine because the tests
pass". Host tests and a clean compile are explicitly *not* enough for any
hardware claim (`AGENTS.md` sections 6 and 29; `VERIFIED_STATE.md` "The rule").

Where an entry has an owner stage, the stage is named: the entry survives until
that stage records its evidence.

---

## A. What the vehicle itself still does not establish

The 2026-10-02 bring-up and the 2026-10-02/03 tournament were **observed** to move
the vehicle under firmware control: forward and reverse were both exercised, the
encoder's ticks rose forward and fell in reverse, the physical trigger actuated,
and a separate tournament-only emergency image drove the vehicle forward and
backward at the tournament. These are **informal physical observations (OBSERVED),
not evidence**: nothing was captured, no protocol was run to its acceptance rule,
and no raw data or evidence pointer exists - all of which `VERIFIED_STATE.md`'s
rule requires, while its excluded set names "recollection or verbal description".
**All twelve rows of `VERIFIED_STATE.md` therefore remain UNVERIFIED**, including
#3 motor polarity and #4 encoder direction sign, whose signs were witnessed
informally (D36); also #1 wiring and pin assignments (no continuity check), #2 the
motor driver's truth table, #5 encoder signal quality, #6 rollout, #7 sensor
scaling, #8 control gains, #9 braking behavior, #10 wheel slip, #11 bottle
interaction and #12 the dead-wheel question.

The earlier wording here - that no motor supply had ever been connected, so the
board had never moved anything under firmware control - was written in the H0/H0W
era and is superseded. Those sessions *were* USB only with the supply off, and they
still say nothing about the vehicle; but the supply was connected and the vehicle
was driven on 2026-10-02, so the general claim was false from that date.

**Board availability is a current constraint, not a claim about history.** The
Nano R4 was taken away at the end of the D31 session (2026-09-29), where it was
left holding the **bench** image (`b6b1c455...`) and could not be reflashed by this
project - that is what `data/evidence/D31-2026-09-29/README.md` records, and it was
true then. The unit was in hand again on 2026-10-02/03 for the bring-up and the
tournament (`VERIFIED_STATE.md`, D36), so "the board is gone" must never be read as
"no hardware work happened". Whether it is in hand *now* is the thing to check
before planning a stage; while it is not, source can still be changed, reviewed,
host-tested and committed, but no `VERIFIED_STATE.md` row can be promoted.

What the 2026-10-02/03 sessions did **not** do is still owed, and it is owed
because it was not done - not because it cannot be done: H1A's continuity and
driver-output measurement, H1B's input-level record, H2's recorded repetitions,
H4's formal rollout, H0W Step 8 under a loaded run, and every loaded-run protocol
from H5 on. No capture from those sessions exists in this repository (section G).

- Evidence of the limit: `VERIFIED_STATE.md` hardware table; `data/evidence/P-H0-2026-09-28/README.md` ("What this session does NOT establish").
- Removed by: H1A onward, row by row, each with recorded raw observations.

Consequences that are easy to forget:

- **`src/PinMap.h` carries the team's stated V5 harness map, and it is not a
  verified wiring record.** The role-to-pin numbers come from the team's
  statement of the intended/as-built harness; each role's framework capability
  was checked against the installed framework's pin table; and the map has never
  been checked against a physical harness (`VERIFIED_STATE.md` #1,
  `SOFTWARE_ARCHITECTURE.md` section 10).
- **The external DRV8874 nFAULT signal is NOT INSTALLED on this vehicle.** The
  driver's fault output is not wired to the Nano, so there is no line to read, no
  polarity to measure and no external driver fault the MCU can observe. The
  firmware states this explicitly: `vehicleConfig::kPlannedDriverFaultCapability`
  is `NotInstalled`, the V5 pin map has no pin for the line (`kMotorFaultBar` is
  `kNoPin`), `SensorInputs.h` does not configure or sample any nFAULT pin, and
  the capability-aware `DriverFaultConfigurationValid` check treats polarity
  verification as *not applicable* rather than outstanding
  (`src/VehicleConfig.h`, `lib/evcore/src/DriverFault.h`).
  **Consequence: external fault telemetry is unavailable.** No firmware path can
  stop the vehicle for a driver fault on this hardware, and the raw level is not
  reported as a level at all - telemetry writes `nfault=NotInstalled` and marks
  the level field absent, because a floating pin is not a healthy driver. This is
  a wiring fact stated by the team, **not** a measurement: no polarity was ever
  established, `kPlannedDriverFaultPolarity` stays `Unverified`, and nothing may
  be relabelled as measured.
  **What still exists independently:** the DRV8874's on-chip overcurrent, thermal
  and undervoltage protections are untouched by this. The driver still protects
  itself; the MCU simply cannot observe that it did.
  H1B Part 3 and H9's driver-fault trip have **no subject** on this vehicle
  (`docs/bringup/P-H1B-sense-inputs.md`, `docs/bringup/P-H9-fault-injection.md`).
- **The encoder's mechanical interpretation is only partly known**: counts per
  revolution and gearing are still unknown, while the count *direction* was
  **observed informally** on 2026-10-02 - forward travel increases the count,
  reverse decreases it (row #4 stays UNVERIFIED; `VERIFIED_STATE.md` D36,
  `SOFTWARE_ARCHITECTURE.md` section 10). The decoding *scheme* is fixed in
  section 11; its numbers are not.
- **The encoder input's pull-up setting is a bench placeholder** and one of the
  first things H2 must check (`src/EncoderInput.h`).
- **The bottle-pushing mechanism does not exist yet**, so how a contact or a
  finished push is detected - and whether it can be detected at all - is
  undecided (`VERIFIED_STATE.md` #11, `SOFTWARE_ARCHITECTURE.md` section 10)
  *(superseded in part by D34: the run's forward phases now exit on
  position-derived evidence from `BottleEvidence.h`, with every distance a
  `Measured`-required placeholder - see the entry below. The mechanism's real
  behavior, and therefore row #11, is still unverified.)*
- **The run's bottle evidence is the firmware's belief, not an observation** (D34).
  This vehicle has no bottle sensor, so contact, capture and push are derived from
  the run-axis position and the announced geometry (`lib/evcore/src/BottleEvidence.h`).
  The three distances are Provisional placeholders, so no run may be announced
  until they are measured, and even then the derivation says only where the
  firmware believed the vehicle was. Removed by: P-H8 recording independently
  observed bottle positions (witness mark, observer or camera) against the
  believed ones, and the constants promoted with that evidence.
- **The run's timing plan has never been compared with a realised Run Time** (D34).
  `aim: planRunTiming()` chooses the run's cruise and deliberate delay from the
  announced Target Time and geometry, but every limit it uses is still a
  placeholder, so nothing has been observed. Removed by: P-H14 step 12 - several
  announced Target Times on a marked course, with the planned cruise/delay from the
  `OK,cmd=announce` line and the realised Run Time recorded together.
- **The regulated ramp-down has never been observed on a vehicle** (D34). The run
  now keeps regulating toward the shaped target while the profile ramps it to zero
  (`isRunRampDownCommand()`), instead of coasting. What that does to a real
  drivetrain - and how far it travels - is H7's measurement, not something the
  code change establishes.
- **The tournament ran on an emergency image that is no longer in the tree (D36).**
  A standalone, tournament-only firmware image (its own entry point and build
  environment) was flashed and physically drove the vehicle autonomously forward
  and backward at the 2026-10-02/03 tournament. D35 deleted it from the active tree
  (source, test and `[env:emergency]`); it is recoverable only from the
  preservation snapshot (`recovery/d32-d34-v5-snapshot`), and `src/main.cpp`
  refuses to compile its define into the vehicle image. What it did was observed,
  not instrumented - no telemetry or run record from it exists in this repository -
  it is not the production run state machine, and nothing about production
  behavior, calibration or repeatability follows from it.

## B. Implemented, host-tested, but never observed on the board

These are code-complete and unit-tested, and no board has ever produced their
output. They are the cheapest entries to remove: each needs one capture.

**This section was emptied of its first six entries by the H0 repeat (D25):** the
out-of-range `config set` refusal, B1's `worst_late_ms`/`dropped_total` extremes,
the greeting policy on the current source, the silence beacon, the `boot` command
and the `BOOT,...` first line on attach were all observed on the board that
session, and `VERIFIED_STATE.md` now carries each of them with its capture. The
entries that follow are the ones the repeat could **not** reach.

| Limitation | Evidence of the limit | Removed by |
|---|---|---|
| **A host that attaches while the firmware already believes one is attached** was not observed on the current source: every attach in the repeat was an attach edge the firmware could see (`BOOT,...` first line in 7 of 7) | `data/evidence/P-H0-repeat-2026-09-28/README.md`; D22 observed the condition once, on the D22 image | A capture in which a reader attaches mid-stream (telemetry joining the queue's head) without a `BOOT,...` line. D22 has one; the current policy is meant to cover it by the silence rule, so a failed greeting there would be a finding |
| The **bench image's byte rule never fires**: `src/bench/BenchMain.cpp` runs the console's own read loop, and the console forwards lines to `runtime.handleCommandLine(...)` (`src/bench/BenchConsole.h`) rather than running `VehicleRuntime::serviceSerial()`, where `greeter_.observeHostByte()` is called - so a beacon on `env:bench` is evidence about the silence rule only | D25 (observed as timing: a `bench health` at t=27.02 did not defer the beacon at 30.03); `src/bench/BenchMain.cpp`, `src/bench/BenchConsole.h` | Nothing pending: this is a bench-image property. It matters only in that bench captures must not be read as evidence about the byte rule, and H0W runs the bench image |
| The greeting has never been exercised with **two reader processes in sequence within one capture** (the D22 condition), so "the second reader is greeted" is still only the policy's intent | `data/evidence/P-H0-2026-09-28/d22-f4-attach-observations.txt`; D22, D23 | A capture closing and reopening the port inside one session, recording whether a `BOOT,...` line appears for the second reader |

## C. Known defects and unexplained behavior

1. **F-3 - the upload failure with no root cause.** One of eight uploads in the
   first H0 session left the board present on the USB bus but not enumerating
   (`Unknown USB Device (Device Descriptor Request Failed)`,
   `USB\VID_0000&PID_0002\5&28FFA4CE&0&3`), reachable by no software path and
   recovered only by a **physical power cycle**. The cause is **not determined**.
   D22 added a classification (no `VID_2341` device at all versus a
   `VID_2341&PID_0374` device `dfu-util` cannot use) and removed one race, which
   is not a fix. D22's three uploads and the H0 repeat's **14 consecutive uploads
   (D25)** all succeeded, which is 17 successes in a row since the one failure -
   a strong indication that it is not routine, and still not a root cause, a
   reproduction, or a reason to stop carrying this entry. Ownership: a repeat
   occurrence, and H0W Step 4's recovery cycles.
   Evidence: D21 F-3, D22 item 2, D25,
   `data/evidence/P-H0-2026-09-28/README.md`,
   `data/evidence/P-H0-repeat-2026-09-28/h0repeat-uploads.log`.

2. **The greeting can be postponed by the reader it is meant for.** Because a
   byte restarts the silence interval, a reader that appears with **no
   control-line edge** and then types continuously pushes the silence deadline
   out by `kSilenceMs` on every byte, so it may see no `BOOT,...` line at all
   while it is typing. This is the tested and now on-target-observed behavior of
   the byte rule (the H0 repeat saw the byte rule defer a beacon), not a defect:
   owing the greeting on every byte would greet before every command. The
   remedies are deliberate and manual - stop typing for `kSilenceMs`, or type
   `boot` (owed immediately, admissible in every state, and answered by the same
   line). Evidence: `lib/evcore/src/HostGreetPolicy.h`, D23, D24, D25.

3. **`Serial.dtr()` must never be called.** On this core it *sets* the
   `ignore_dtr` flag, after which `connected()` and `write()` succeed with no
   host attached - a silent way to stage output into a void. Nothing in `src/`
   calls it today (`hostLineState()` reads the lines another way), and this is
   recorded to keep it that way. Evidence: `src/SerialTransport.h`, D23.

4. **Unity's double assertions are disabled** in this project's test build, so
   `TEST_ASSERT_EQUAL_DOUBLE` cannot be used and host tests compare doubles with
   an explicit tolerance (`VERIFIED_STATE.md`, behavior this build depends on).
   A limitation of the test instrument, not of the firmware.

## D. Upload and toolchain limits on this machine

- **A failed F-3-state board needs hands.** The only known recovery is a physical
  USB power cycle; there is no software path. This is why an upload series can
  only be run where someone can reach the board - the H0 repeat's 14/14 (D25) was
  run with the operator present for exactly that reason, and it is also the abort
  path H0W Steps 3-4 depend on.
- **The bootloader needs a Windows driver PlatformIO does not install.** Without
  Arduino's signed `renesas.inf` (installed once, elevated, from the framework
  package's `drivers/`), Windows reports problem code 28 and `dfu-util` fails
  with Error 74. This is machine setup, not repository state: a clean machine
  repeats F-2. Evidence: D21 F-2, tool transcript in the H0 evidence README.
- **The serial monitor must be closed to upload.** The pre-upload touch has to
  open the application-mode port; if another program holds it (e.g. a running
  `pio device monitor`), the touch cannot be issued and the upload fails to find
  a device. The hook cannot tell that case apart from the expected port teardown.
- **`OSError(22, 'A device which does not exist was specified.')` is expected and
  is printed on every successful touch**, because the CDC port goes away while
  the board resets. It must not be read as a failure - and, conversely, a real
  port-in-use failure looks similar in the log.
- **The hook never fails the upload.** If no bootloader appears it prints what it
  saw and continues, so `dfu-util` gives the authoritative error; a device that
  appears a moment late is still worth uploading to. A failed `pio run -t upload`
  is therefore still possible, deliberately.
- **The hook only applies to the `dfu` protocol**; a `sam-ba` upload keeps the
  platform's own touch. It is also not attached to `env:native`.
- **One USB port, one board.** Captures and uploads are serialized on the same
  port, so a session cannot read the log and upload at the same instant; and
  every observation in this repository comes from a single physical Nano R4
  (serial `350D331C5A333131021133354B573462`). Nothing here is evidence about a
  second unit, a different cable, or a different host.

## E. The watchdog is qualified only in part

The prepared path exists, links, has compile-time guards, and as of H0W Steps 0-7
(D26/D27/D28) has been observed on target to **open, refresh, and reset a
deliberately hung loop** - 8/8 hang cycles at 250 ms and 6/6 at the chosen 114 ms,
no false reset across four healthy load conditions, 5/5 recovery uploads with no
hands, and the de-energized pin levels read back from the MCU after each reset.
**H0W's acceptance rule is met and it is recorded as passed for the bench load**
(D28); the entries below are what the protocol's acceptance rule does **not** cover,
or what remains provisional about the numbers it justified.

- Enablement is off in the vehicle image (`kWatchdogEnablementRequested` derives
  from `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`, which only
  `[env:bench-watchdog]` sets), so nothing is currently protected by it.
- **A timeout opened before the boot wait could not survive a boot.** The H0
  repeat measured the board's bounded wait for a host as a ~1980 ms first-tick
  lateness with ~99 deadlines elapsing inside it (D25: `worst_late_ms=1980`,
  `dropped_total=99`, and every unhosted boot opening at `dt_ms=2000`/`t_ms=2000`).
  The firmware opens the watchdog at the **end** of `setup()`, after that wait, for
  exactly this reason (`src/WatchdogCheckpoint.h`). The gap that remains once it
  is opened **is** measured: `first_refresh_ms = 0` with enablement on, i.e.
  sub-millisecond at `millis()` resolution (D27).
- **The four numbers in the decision are measurements plus one pre-registered
  policy value**, not placeholders: `kBootToFirstRefreshBudgetMs = 0` and
  `kWorstRefreshIntervalBudgetMs = 14` measured, `kWatchdogPlanMarginMs = 100`
  fixed before the first trial, and `kWatchdogTimeoutMs = 114` - the rule's own
  answer (D27). The rule they must satisfy (`lib/evcore/src/WatchdogPlan.h`) is
  asserted at compile time, the **whole** rule plus an equality check against
  `requiredWatchdogTimeoutMs()`, so a build cannot request an unachievable timeout
  or a hand-edited one that drifted away from the margin that chose it.
- `WDT.begin()` is documented to return zero for a period the timer cannot
  produce, and the timer **rounds the requested period up**, so the requested
  `kWatchdogTimeoutMs` is not the realized period. The open itself has been seen -
  `opened=1, open_refused=0` at both 250 ms and 114 ms - and no refusal has ever
  been observed. **The realized period is now measured rather than bounded** (D30):
  174 ms for the 114 ms request, read from the hardware's own `WDTCR` fields with
  PCLKB from the vendor clock API, and agreeing with the library's computation.
  What stays open is not the number but its consequence - whether 174 ms is a
  credible stop - and that belongs to Step 8.
- **The reset cause is read from the MCU's own registers, measured on this board
  only.** D30 added `bench resetsrc` - `R_SYSTEM->RSTSR0`/`RSTSR1` with the vendor
  header's masks - plus a `clear` (write 0), because the flags **latch** across
  software resets: a `wdtrf=1` left by the D27/D28 campaign survived every later
  upload, including one of a different image, until it was cleared. With the clear,
  the three sources separated cleanly - a power-on reset reads `porf=1` with
  `rstsr1=0`, an upload `swrf=1` alone, a deliberate hang `wdtrf=1` alone, 5/5. Two
  limits remain: the decode was measured on **this one board**, so it is not a
  datasheet claim about the part family, and the register is status only - it says
  which reset happened, never whether the vehicle was safe when it did, which is
  H1A's question. Before D30 the attribution rested on the latency scaling with the
  request and on there being a single refresh path (`grep -rn "WDT" src/` matches
  only `WatchdogCheckpoint.h`); those still hold, and now the silicon agrees.
- **The post-reset pin levels are read at the MCU, not at the header.** D28's
  `bench pins` reports the de-energized combination 6/6 times after a reset (9/9
  including the final hang campaign), which is the "MCU pin levels ... (levels
  only)" the protocol's Step 3 asks for, and closes that acceptance clause. It
  cannot substitute for an **electrical** measurement: it is `digitalRead()` of the
  same pins the firmware drives, so a pin driven low but held high at the header
  would still read as the MCU sees it, and whether these levels de-energize the
  **driver outputs** is H1A's question. Related: every observed reset followed a
  **drive-free** hang (the motor supply is off in this protocol by design), so a
  reset taken while an effort is commanded is not covered by this observation.
- **The host cannot clock the period, which is why the registers are the evidence.**
  `WDT.begin(114)` was accepted and the library rounds a request **up** to the next
  achievable period; the capture's reset latency is quantized by its own ~0.2 s read
  window - the same order as the 174 ms period - so a hang marker read late makes the
  gap look short (D30 saw 62 ms against 174 ms). The realized period is therefore
  taken from `WDTCR` and the library, and the latency is kept only as evidence that
  it *scales with the request*.
- **The chosen 114 ms is a bench number, and its margin was applied to the
  request.** The soak has no run path and no motor; a loaded run adds estimator and
  control work to the same loop, so the margin is provisional until H0W **Step 8** is
  repeated under a loaded run - which does not exist yet, and which freezing a
  competition build depends on. Those criteria are now written down rather than only
  owed (`data/prereg/P-H0W-step8-2026-09-29.md`). The 100 ms margin was applied to
  the **request** (114 ms) while the timer programs 174 ms, so the realized margin
  over the worst measured gap is 160 ms: the rule held with more room than it
  claimed, and Step 8 must re-state it in realized terms.
- **Two of the instruments have no host test.** B1 could not report the worst
  refresh interval H0W Step 2 asks for, so `worst_refresh_gap_ms` was added (D27) -
  a comparison per pass, from the clock the refresh already receives, changing
  nothing about when the watchdog is refreshed - and D28 added the `bench pins`
  readout. Both are Arduino-layer (`test_build_src = no`), so their evidence is the
  on-target readouts and not a unit test. The quantity behind D28's stall field,
  `ControlScheduler::worstStallMs()`, **is** host-tested.
- **The qualified margin would be a bench margin only.** H0W's soak has no run
  path and no motor, so the timeout it justifies is *provisional for the loaded
  run path* until H0W Step 8 is repeated under a loaded run; freezing a
  competition build depends on that
  (`docs/bringup/P-H0W-watchdog-qualification.md`, Step 8, criteria in
  `data/prereg/P-H0W-step8-2026-09-29.md`).
- **A reset is not a motor-off.** H0W can observe the MCU resetting and the
  firmware re-initializing; whether the driver outputs de-energize is an
  electrical fact owned by H1A. No statement about motor-output safety may be
  recorded from H0W.
- **`kMaxDwellGapMs = 28` bounds credited near-zero dwell, and it is a bench
  number.** It is the worst stall measured across the D28 loads (28 ms with the
  host reading, 21-22 ms under the other two) and replaces the 40 ms placeholder,
  so the gap limit is now `Measured` with a citation. It is not a ceiling, and a
  loaded run adds work to the same loop: H0W **Step 8** has to re-take it. Being
  measured does not make a stop verifiable on its own either - the rest of the
  near-zero configuration (threshold, dwell) is still provisional, and nothing has
  run on a wheel (`src/VehicleConfig.h`, `lib/evcore/src/NearZeroVerifier.h`).

- **H0W Step 8 has not been run.** It needs a loaded run path to soak under -
  which H7/H10 have not produced, because the production image has never run
  loaded - and it needs the board in hand (section A: availability is a
  constraint, not a verdict). Its criteria are pre-registered
  (`data/prereg/P-H0W-step8-2026-09-29.md`) and its status is **owed**, blocked
  on a loaded run path that does not exist yet. The timeout therefore stays
  **bench-qualified and provisional** for the loaded run path, at the unchanged
  `kWatchdogTimeoutMs = 114` (realized 174 ms, D30).

## F. Placeholders and unbuilt enforcement points

- **Every calibration, limit, run-policy and control value is `Provisional`**
  (`src/VehicleConfig.h` section 2): distance per encoder tick, brake offsets,
  effort ceiling, cruise/accel/decel, every regulator gain, every phase timeout,
  every near-zero threshold and dwell, the final-approach distance, and the three
  encodings of the polarity fact. None is a measurement
  (`VERIFIED_STATE.md` #6, #8, #9). The polarity sign behind those three
  encodings was **observed informally** on 2026-10-02 (#3, #4) but is **not
  evidence and not a promotion**: no constant changed and no row moved - both
  rows stay UNVERIFIED because nothing from that session was captured - so the
  configuration still reports them as placeholders (D36). The single exception is
  `kMaxDwellGapMs` (D28), the near-zero verifier's gap limit, which is now a cited
  `Measured` value: it bounds how much dwell may be credited, it does not by itself
  make a stop verifiable, and H0W Step 8 has to re-take it under load.
- **The rollout observation of 2026-10-02 is provisional, not a calibration (D36).**
  A manual forward push counted roughly **1494 encoder ticks over an *intended*
  ~2.0 m**, i.e. ~1.339 mm/tick as a rough figure. The travel was intended rather
  than measured, the push was not repeated and no spread exists, so
  `kPlannedDistancePerEncoderTickM` stays `Provisional`, row #6 stays UNVERIFIED,
  and the figure may not be used as a calibration. Removed by: P-H14 step 4 (H4) -
  tape-measured travel, five runs each direction, spread and mean within the
  pre-registered tolerance.
- **A "Measured" promotion requires a citation** (`Sourced::citesEvidence()`), and
  `MeasuredValuesCiteEvidence` reports false while placeholders remain. This is a
  deliberate limit: it is the check that stops a provisional number from
  authorizing motion while looking like a measured one.
- **Three safety ceilings have no enforcement point yet.** Maximum velocity per
  direction, maximum run duration, and minimum battery voltage are data with no
  runtime effect; the profile/ceiling selection, the run timer and the battery
  check that would own them are **not built**
  (`SOFTWARE_ARCHITECTURE.md` section 7). Only `maxEffortPermille` is enforced
  today, through the regulator's own ceiling.
- **No battery sensing exists.** Battery-condition correlations (a listed
  analysis goal) cannot be produced yet.
- **Several policy values are explicitly "policy, not measurement"**: the
  estimator window (20 ms), the no-edge timeout (100 ms) and the stop-evidence
  age limit (3200 ms) have no demonstration that they suit this encoder
  (`src/VehicleConfig.h`; `VERIFIED_STATE.md` #5, #10). H0W's measured cadence
  margin (a 20 ms tick and a 28 ms worst stall, D28) is *consistent* with them -
  well inside the 60 ms sample-age limit - and the check is recorded in
  `DEVELOPMENT_LOG.md` D28; but consistency under a bench load with no encoder
  attached is not the encoder-specific demonstration H5 and H10 owe.
- **The control configuration is unproven**: section 13 fixes the estimator's,
  regulator's and shaper's responsibilities and refusal behavior, but not which
  estimator window is usable, not one gain, and not whether any candidate is good
  enough for an event run (`SOFTWARE_ARCHITECTURE.md` section 10).
- **Target shaping, braking and reversal have never run on a wheel through the
  production image**, so no runway, stopping distance or reversal behavior is known;
  the braking offsets that would compensate for it are placeholders
  (`VERIFIED_STATE.md` #9). What did move on the floor was the tournament's separate
  emergency image, which drove the vehicle forward and backward autonomously (D36) -
  a standalone entry point, not the run state machine, since removed in D35 - and
  the low-effort motion of the 2026-10-02 bring-up (which image commanded that is
  not recorded here). Neither is a production run, and neither established a
  stopping distance.

- **The bench console's readouts had no compile-time bound, and its widest one does
  not fit.** Derived field by field from `healthLine()` in D31, the worst case is
  **534 characters** against `ev::kSerialLineCapacity = 448` - 86 over - because
  the free-running counters reach ten digits (`passes` after roughly 26 h at the
  measured ~46,000 passes/s). The widest line any capture has contained is 389, so
  the overflow is unreachable in a bench session but reachable in the limit. Every
  other consumer of the shared buffer states a derived worst case against it
  (`ev::kOperatorResponseWorstCaseChars`, and `BoundedText.h`'s own 418/443); this
  one stated none until D31, when the number was derived and recorded in
  `src/bench/BenchConsole.h`. What D31 fixed is the **silence**: a readout that
  does not fit is dropped whole and was counted only in `responses_lost`, a field
  of the health line - so the loss hid on the line that was lost. A dropped readout
  now leaves a constant `BENCH,lost=1,reason=ReadoutDidNotFit` in its place. What
  stays open is the schema: **the owed fix is to split `health`** into its
  watchdog/loop half and its console/interface half, after which the bound can be
  asserted at compile time the way the operator path's is. Owner: unowned. With the
  board gone this is maintenance, not bring-up, and nothing can be measured
  against it.
- **`stageLostReadoutNotice()` is reviewed, not verified.** It lives in the
  ARM-only bench tree, so no host test reaches it (`test_build_src = no`), and the
  86 characters of headroom mean no bench session could have produced a line long
  enough to trigger it. Its claim - that a dropped readout is visible on the wire -
  has never been exercised on hardware.

## G. Evidence and process limits

- **H0 passed only in its second session.** The first session (D21) recorded what
  it saw and did not pass - its decision rule blocked on upload reliability. The
  repeat (D25) met every pre-registered criterion and passes, with two notes: one
  condition (attaching while a host was already believed attached) was not
  observed and is counted neither way, and the cadence extremes are non-zero
  because of the boot gap. Stages that H0 gates are unblocked from D25 onward;
  the first session's rows describe the **D21/D22** images.
- **P-H0's first pre-registration is retrospective** - it was written after the
  first uploads, so its counts were not fixed in advance
  (`data/prereg/P-H0-2026-09-28.md`; kept, labelled). The repeat has a real one
  (`data/prereg/P-H0-repeat-2026-09-28.md`, written before its first upload).
- **The record is committed; the images it describes are not reproducible from
  it.** As of 2026-09-29 the D21-D28 span is committed on `main` in four scoped
  commits above `8e42d13`, so the log entries, the `VERIFIED_STATE.md` rows, the
  evidence directories, the pre-registrations, the touch script, this register
  and the source changes all have a home. What that does **not** buy is a
  rebuild: the flashed images were built from intermediate working trees that
  were never committed (placeholder watchdog budgets, the deliberate-hang hook at
  `460000U`), so no commit reproduces the D22-D27 images. Where a record names
  `8e42d13` as its build identity, that is the last commit *below* the tree that
  was actually flashed; the uncommitted delta is the D22-D28 work, now committed
  above it. **D31 fixed one half of this and not the other.** Fixed:
  `platform = renesas-ra@1.9.0` (`platformio.ini`), so the toolchain can no longer
  drift under an unchanged file. That matters more than usual here because the
  Renesas FSP ships as a **prebuilt** `libfsp.a` inside the core's variant with no
  C sources in the tree, so the version pin is the only record of what the firmware
  was built against (`src/bench/ResetCause.h`; D30). Not fixed: the images. The
  earlier records describe images built from intermediate working trees
  (placeholder watchdog budgets, the deliberate-hang hook at `460000U`) that were
  never committed, so no commit reproduces them - and D31 added the reverse case, a
  commit whose images were built and hashed but **never observed on target**
  (`91f74a04...` vehicle, `b6b1c455...` bench, `271e8eba...` bench-watchdog; the
  bench image was flashed and its readouts were lost with the port). Both
  directions are in `data/evidence/D31-2026-09-29/README.md`.
- **The captures are stored line-ending normalized.** `core.autocrlf=true` and
  there is no `.gitattributes`, so every text blob is stored with LF however it
  arrived - including the serial captures, which arrive CRLF (`git ls-files --eol`
  reports `i/lf w/crlf` for them). A fresh checkout restores CRLF on Windows and
  not elsewhere, and `git show` always yields LF, so a byte-exact comparison
  against the original capture needs the worktree rather than the blob. Keeping
  the evidence verbatim would take `data/** -text` plus a re-add; that is a
  repository-wide decision and it has not been made.
- **Host tests prove logic, never hardware.** `pio test -e native` covers pure
  modules because `src/` cannot compile on a host toolchain
  (`test_build_src = no`); it cannot reach the Arduino layer that talks to the
  board, and simulation may never be reported as physical validation
  (`AGENTS.md` section 29).
- **Single observations are not repeatability.** Where a behavior has been seen
  once or twice, the record says so; none of it is a distribution, and no
  worst-case claim may be read out of it. The H0 repeat's counts are the largest
  series this project has (14 uploads, 7 attaches, three refusal rounds, two B1
  readouts); everything else is smaller, and the condition D22 saw once (a
  greeting missed) has still been seen only once.
- **Image identity must be checked before believing an on-target claim.** The
  board is left running whichever image was last flashed; a record's statements
  belong to the image whose hash that record names, not to the current source
  (D22's greeting observations belong to the D22 image; D23's source was never
  flashed; the H0 repeat's are the `cfd288d4...` vehicle image and the
  `71f42e40...` bench image). **Every handoff before D31 left the vehicle image
  running; D31 left the bench image** (`b6b1c455...`) - the image whose own
  documentation says it must never be mistaken for the vehicle image, and which
  contains `bench drive`, a command that applies effort outside a run. The board is
  reachable only intermittently - it left the project after D31 and was flashed
  again for the 2026-10-02/03 sessions (section A) - so a unit that is ever powered
  again should be treated as an unknown image until `bench health` reports
  `image=bench`, which settles it. Note that the tournament's emergency image has
  since been deleted from this tree: an image that is no longer in the source can
  never be confirmed from the source.
- **The H0/H0W sessions were measured with the board alone: no motor supply, no
  driver, no encoder and no drivetrain connected**, so the highest load those
  sessions applied to the firmware is its own serial path. The 2026-10-02/03
  sessions did connect the drivetrain (section A), but produced no captures.
- **The 2026-10-02/03 bring-up and tournament left no evidence directory.** There is
  no `data/evidence/`, no pre-registration, no serial capture and no repetition
  count for them; what they established is recorded once, in `DEVELOPMENT_LOG.md`
  D36, from the team's confirmed account after the event. Each observation there is
  therefore a single un-instrumented sighting: enough to record that it was seen
  (and so to stop this register asserting the opposite as fact), but **not enough
  to mark any row VERIFIED, not enough to promote a calibration**, and no number
  from those sessions may be read as a distribution. The formal protocols that
  would produce the evidence - H1A, H1B, H2, H4 and the loaded-run stages - are
  still owed, and until one of them runs and is recorded, **every hardware row in
  `VERIFIED_STATE.md` stays UNVERIFIED**.

## How to remove an entry

1. Run the stage that owns it (column or owner named in the entry), following
   `docs/bringup/` and the safety rules in `AGENTS.md`.
2. Record the raw observation, not the conclusion, in `data/evidence/`.
3. Promote or change the corresponding `VERIFIED_STATE.md` row (with its evidence
   pointer and date) and log the change in `DEVELOPMENT_LOG.md`.
4. Delete or narrow the entry **here**, in the same change, so this file keeps
   matching what the project can actually claim.
