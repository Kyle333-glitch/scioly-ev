# P-H0W - Watchdog qualification and timeout choice

**Primary class:** S (on-target software check).
**Also contains:** E observation of the MCU pin levels (motor supply off; motor
disconnected).
**Order:** immediately after H0, before H1A.
**Answers:** does the prepared watchdog path actually start, count, refresh and
reset on this target; does a healthy loop run long enough without a false reset;
does a deliberately hung loop stop refreshing and reset the board; does the board
recover and re-flash reliably; what timeout is justified by the observed healthy
margin and the observed reset behaviour; what is the worst observed control-loop
stall (see "Why the maximum control-loop stall is recorded here" below), which
bounds how much near-zero dwell could be credited without observation.
**Bench instrumentation required:** B1 (loop and watchdog counters), a driver-pin
readout (`bench pins`, which reports the three driver inputs as the MCU itself
holds them and pairs them with the duty the adapter last wrote - a PWM duty cannot
be read back from a pin), a reset-cause readout (`bench resetsrc [clear]`, added in
D30 for Step 3's attribution clause), a watchdog-register readout (`bench wdt`,
added in D30 for Step 7's realized period), and a bench-only hang hook the
production image cannot contain - all supplied by the separate PlatformIO `bench` environment (see the
index), not by a flag inside the production build. The hook is `kDeliberateHangAfterPasses` in
`src/bench/BenchMain.cpp`; it is a compile-time constant, not a console verb, so
no serial input can hang the board by accident. This protocol uses **two** bench
environments: `env:bench` (watchdog disabled) for Step 0, and
`env:bench-watchdog` (built with `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED=1`) for
Steps 1-7. Neither is the vehicle image.

**Scope limit that must not be crossed.** This protocol establishes that the MCU
resets and that the firmware re-initializes. It does **not** establish that a
reset de-energizes the motor output. The pin levels the firmware drives on reset
are observable here, but what those levels do at the driver outputs is a
different fact and is measured electrically in **H1A**. No statement about
motor-output safety may be recorded from H0W.

## Prerequisites

- H0 passed.
- `src/WatchdogCheckpoint.h` reviewed: `kWatchdogEnablementRequested` is the
  single enablement expression, `kWatchdogTimeoutMs` is the requested timeout, and
  refresh happens only in `endOfPass()`. Confirm by source inspection
  (`grep -rn "WDT" src/` matches only that file) that nothing else can refresh.
- `kWatchdogEnablementRequested` is derived from the build define
  `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`, which only `[env:bench-watchdog]` sets
  (`platformio.ini`). Enabling the watchdog for this protocol is therefore a choice
  of environment, not an edit to a header both images share - and `src/main.cpp`
  refuses to compile if the vehicle image ever receives the define, so production
  enablement cannot be flipped by accident here.
- `src/WatchdogCheckpoint.h` reviewed for **when** the timer is opened: the open
  is `openIfRequested()`, called once at the **end** of `setup()` after the bounded
  serial wait, not in `begin()`. The boot-to-first-refresh gap this leaves is
  measured (`timeToFirstRefreshMs()`) and reported by B1, and the requested
  constants are asserted against the rule at compile time
  (`lib/evcore/src/WatchdogPlan.h`): a build that requests enablement with a
  timeout that cannot cover that gap and the worst refresh interval does not
  compile. Note this in the record - it is why Step 1 cannot silently produce a
  board that resets during boot.
- The bench-only hang hook and B1 readout are present in the bench build and
  absent from the production build. Verify by build graph and by symbol, not by a
  macro: `pio run -e bench` compiles `src/bench/`, `pio run` does not
  (`platformio.ini`), and a symbol search over the two `firmware.elf` files must
  find the console's symbols in the bench image and none in the vehicle image. A
  working check (with `arm-none-eabi-nm` on `PATH`):

  ```
  arm-none-eabi-nm .pio/build/nano_r4/firmware.elf | grep -Ec "bench|hang"     # must be 0
  arm-none-eabi-nm .pio/build/bench/firmware.elf   | grep -Ec "bench|hang"     # must be > 0
  ```
- Motor supply off and disconnected; drivetrain unloaded.
- (Pre-register) the soak duration, the margin that defines "exceeds the worst
  healthy refresh interval", and the number of hang/reset/recovery cycles.

## Setup

A bench build containing B1 and the hang hook - `env:bench` for Step 0,
`env:bench-watchdog` for Steps 1-7, both uploaded with
`pio run -e <env> -t upload`; the board on USB with logging to a file; the motor
supply off and disconnected; the drivetrain unloaded. B1's readout is `bench health`, and the boot-to-first-refresh
gap is one of the fields it reports. The operator must be able to see the board
restart (the `BOOT,...` line in the log) and to power-cycle the board if the
protocol asks for it.

## Held constant

- the firmware image under test (except the enablement define, which is what
  distinguishes Step 0's `env:bench` from Steps 1-7's `env:bench-watchdog`, and
  `kWatchdogTimeoutMs`, which Step 7 varies);
- the host, cable and port; the telemetry rate within one load condition;
- the board, the supply and the ambient temperature.

## Varied

- whether the watchdog is enabled (Step 0 versus Step 1);
- the load condition (telemetry off, reading, unread);
- whether the loop is healthy or deliberately hung;
- the requested timeout, once, in Steps 6-7.

## Step 0 - Baseline, watchdog disabled

The shipped setting is `kWatchdogEnablementRequested = false`, which is what
`env:bench` builds (the define is absent). With it disabled, establish upload and
reboot reliability as in H0 but for the bench tree: record the pre-registered
number of successful uploads and power cycles. Confirm healthy operation for the
pre-registered soak: no resets, `refreshes()` and `passes()` both increasing,
`withheldRefreshes()` staying at zero. This is the baseline the enabled build is
compared against.

## Step 1 - Enable the prepared path

Enablement is a build choice, not a source edit: build
`env:bench-watchdog` (`pio run -e bench-watchdog -t upload`), which is `env:bench`
with `-DEV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED=1`. Do **not** edit
`kWatchdogEnablementRequested` or the header it lives in - that header is compiled
into the vehicle image too, and editing it to enable a bench qualification would
change the vehicle's decision in the same commit. Confirm from the uploaded image
that `bench health` reports `enable=true`.

Record the value of `kWatchdogTimeoutMs` and, through B1 (`bench health`), whether
`opened()` reports true, whether `open_attempted` is true at all - a build whose
boot path never reaches the open point shows up here rather than as a mystery -
and whether `open_refused` is true. `WDT.begin()` returns zero when the requested
timeout exceeds what the timer can produce; a refused open must be visible, not
silent.

Because the constants are asserted against the qualification rule at compile time,
a requested timeout that cannot cover the boot gap and the worst refresh interval
is a **compile error**, not a silent boot loop. If Step 1 reports a board that
resets before it finishes booting, that is a finding about this protocol's numbers
(the placeholders in `WatchdogCheckpoint.h`), and the answer is the measured
values, not a shorter timeout. Upload
the bench build.

Note the library's quantization, because the requested number is not the realized
one: the timer picks a prescaler and a reload value and rounds **up** to the next
achievable period, so the realized period is the requested value or slightly
longer. Record the realized period (from the library's own computation, or by
measurement in Step 3). Do not treat `kWatchdogTimeoutMs` as the realized period.
Worth recording too: on this library the timeout is configured as a reset, and
the refresh window is fully open (a refresh is accepted at any time in the
period), so this test is about the timeout alone, not a window.

**Measured (D30):** for the 114 ms request the realized period is **174 ms** -
`WDTCR.TOPS=3` (16384 cycles), `CKS=5` (/256), PCLKB 24 MHz, i.e.
16384 x 256 / 24 MHz = 174.76 ms. `bench wdt` reports the register fields and the
clock, `wdt_realized_ms` reports the library's own computation, and the two agree.
That is 60 ms above the request, **not** "slightly longer", so the margin a record
states must be the realized one (174 - 14 = 160 ms over the worst measured gap),
not the requested one (114 - 14 = 100 ms).

## Step 2 - Healthy operation, no false resets

Soak the enabled build for the pre-registered duration. Record: reset count (must
be zero), `withheldRefreshes()` (must be zero), and the worst observed interval
between consecutive refreshes with B1. Repeat at three loads: telemetry off,
telemetry at the default rate with the host reading, and telemetry at the highest
admissible rate with the host reading. Each of these is a *healthy* condition and
contributes to the margin, so record the worst case across all of them, not the
average.

Record also the **maximum control-loop stall** per load condition: the longest
observed pass the loop took, i.e. the largest gap between two consecutive
control-loop stamps (the largest `millis()` delta between two completed passes),
not merely the worst refresh interval. These are close but not the same number -
the refresh happens once per pass while the loop also does transport work - and
the stall is the one that bounds a dwell, so report both. B1 reports the stall as
`worst_stall_ms`, which excludes the first tick so the bounded boot wait cannot
dominate the number (D28); it is deliberately **not** `worst_late_ms`, which is
lateness measured against the deadline rather than the gap between two ticks.

Record too the **boot-to-first-refresh gap**: the interval between the watchdog
being opened (the end of `setup()`) and the first refresh (the end of the first
completed pass), reported as `first_refresh_ms` by `bench health`. This is the
gap H0W Step 6 has to cover along with the refresh interval, and it is not a
"consecutive refresh" interval, so a soak alone cannot produce it.

## Why the maximum control-loop stall is recorded here

The near-zero verifier (`lib/evcore/src/NearZeroVerifier.h`) credits near-zero
evidence only across gaps no longer than its configured `maxUpdateGapMs`: a wider
gap restarts the observed interval and reports `UpdateGapExceeded`, so a stall
cannot silently complete a dwell. For a direction reversal there is also a
sequence-level timeout, evaluated before each verification step.

The value that bounds that credit is a **measured** one, and it is the maximum
control-loop stall measured here (`kMaxDwellGapMs` in
`src/VehicleConfig.h`, `Provisional` until this protocol passes). It is not the
timeout: the timeout is chosen from the margin over the worst healthy refresh
interval, while the stall is the longest single healthy pass itself. Record the
maximum stall per load condition in Step 2 and Step 5, and promote the gap limit
from those numbers together with the timeout - a stop cannot be verified until
the limit is `Measured`, which is deliberate.

## Step 3 - Deliberate hang

Using the bench-only hook, hang the main loop after a recorded number of
completed passes. The hook is `kDeliberateHangAfterPasses` in
`src/bench/BenchMain.cpp`: set it to the pre-registered pass count, rebuild
`env:bench-watchdog`, upload, and leave the console alone. It enters an endless
loop inside a pass, after that many more passes have completed, and it is the only
way to produce this condition. Nothing else in the firmware refreshes the
watchdog, so refresh stops.

**The motor supply must be off and disconnected for this step.** The bounded
expiry in `ev::BenchDrive` (B2) is evaluated by the loop itself, so a hang
suspends it too - which is why H0W's hang step is drive-free and why the hook must
never be set non-zero during a powered protocol.

The hook stages one line just before it hangs
(`BENCH,hang=1,image=bench,after_passes=<n>`), so the capture shows the pass the
hang happened at rather than only the last earlier readout. Record:

- the time of the last refresh and the pass/refresh counters at the hang;
- the wall-clock latency from the hang to the board restarting;
- that the board *does* restart. The primary evidence is the **device vanishing
  and re-enumerating** (the capture's connect/disconnect rows), together with the
  `EVE1,...,event=Boot` line, the telemetry `t_ms` restarting near zero, and the
  counters restarting from zero. The `BOOT,...` line on its own is **not** evidence
  of a restart: the same line is re-sent as the silence beacon every `kSilenceMs`,
  so a capture can show several `BOOT` lines with no reset at all (measured, D30 -
  three `BOOT` lines against one connect in a 20 s capture, and a 70 s hang capture
  with 12 `BOOT` lines but only 7 `EVE1` boot events for 5 hangs). The beacon does
  not re-send the `EVE1` boot event, which is what makes that line the usable one;
- the **reset cause from the MCU's own reset-status registers**, which is direct
  evidence rather than inference. Read `bench resetsrc`, clear with
  `bench resetsrc clear`, then read again after the reset: a watchdog reset reports
  `wdtrf=1` with `porf=0` and `swrf=0`, and no other reset source measured on this
  board produces that combination (D30: 5/5 hang cycles, against `swrf`-only for an
  upload and `porf`-only for a power-on). The clear is not optional - the flags
  latch across software resets, so a bare read cannot attribute a *new* reset;
- the MCU pin levels `nSLEEP`/`PH`/`EN` after the restart, which must be the
  de-energized combination the firmware initializes to (`bench pins` reads them:
  levels only, the MCU's own view of the pins it drives, and it will not see a pin
  driven low but held high at the header; H1A turns this into a statement about the
  driver outputs);
- whether the restart is attributable to the watchdog rather than to something
  else: the hang hook is the only difference from Step 2 and the loop cannot refresh
  while hung. Do **not** expect the host-side latency to settle this - it is
  quantized by the capture's read window (~0.2 s), the same order as the period, so
  a hang marker read late makes the gap look short (D30 measured a 62 ms gap against
  a 174 ms period). A reset-cause register **is** readable on this core, and it was
  recorded in D30 with `bench resetsrc` / `bench resetsrc clear`, which is the
  evidence this clause asks for; the latency is kept only as corroboration that it
  scales with the request.

Repeat Step 3 at least the pre-registered number of times per candidate timeout.

## Step 4 - Recovery and re-flash cycles

After every reset from Step 3, re-upload and repeat. A board that requires a
manual power cycle or a manual bootloader entry to accept an upload after a
watchdog reset is a **failure of this protocol** - the recovery behaviour is part
of the thing being qualified. Record the upload success rate across these cycles.

## Step 5 - Representative load

Repeat Steps 2 and 3 with the serial path under load: telemetry at the default
rate and at the highest admissible rate, with the host reading and with the host
deliberately not reading (the condition H0 Step 6 exercised). This is the load
that can make a healthy pass long, so the margin recorded here is the margin that
matters. Also confirm that a withheld telemetry line or a full staging buffer
never causes a reset while the loop is otherwise healthy.

## Step 6 - Choose the timeout from the data

Choose `kWatchdogTimeoutMs` only now, and choose it with the rule rather than by
hand: `ev::requiredWatchdogTimeoutMs()`
(`lib/evcore/src/WatchdogPlan.h`) takes an `ev::WatchdogQualification` - the
**boot-to-first-refresh gap**, the **worst healthy refresh interval** and the
margin - and returns the smallest timeout that covers both. Then, with the chosen
value in place, `kWatchdogTimeoutMs` has to satisfy the compile-time assertion in
`src/WatchdogCheckpoint.h` - which is the point of the rule having one owner. The
rule:

- it must exceed **both** gaps observed in Steps 2 and 5 - the worst healthy
  refresh interval across every healthy condition (including the not-reading host
  case) *and* the boot-to-first-refresh gap - by the pre-registered margin;
- it must be achievable by the timer's quantization - check that
  `WDT.begin()` actually opens with the chosen value, and record the realized
  period, which may be longer than requested (done in D30: 114 requested, 174
  realized, with `bench wdt` showing the registers behind it);
- the reset latency observed in Steps 3 and 5 must be acceptable for the
  vehicle's stop behaviour; if the reset is too slow to be a credible stop, that
  is a finding, and the answer is physical (a brake path), not a shorter timeout
  that reintroduces false resets;
- do **not** inherit the current placeholder `250 ms` or any other number without
  this evidence.

## Step 7 - Finalize

Set `kWatchdogTimeoutMs` to the chosen value, re-run Steps 2, 3 and 5 at that
value, and record the realized period. Read it from the hardware and the library
(`bench wdt`, `wdt_realized_ms`) rather than from the reset latency: the host's
latency measurement is quantized by its serial read window (up to ~200 ms in the
H0W captures), which is the same order as the period being measured (D30). Only
after that may production enablement be considered - and that is a separate, deliberate change, not part of this
protocol.

## Step 8 - re-validation with the real run path (owed, not done here)

The margin behind the chosen timeout is measured in Steps 2 and 5 with **no run
path and no motor**: the telemetry load is real, but the control work of a
competition run is not. A loaded run adds estimator and control work to the same
loop, so the timeout is justified for the bench load and **provisional for the
loaded run path** until this step is repeated there.

This step cannot run in H0W - the run path does not exist yet - so it is
**pre-registered here and owed later**. Write down now: the loaded condition (the
competition profile at the competition telemetry rate, plus the host-not-reading
case), the soak duration, the margin, and the number of hang/reset cycles to
repeat. Re-run Steps 2, 3 and 5 under that load once H7/H10 can produce it, and
confirm the timeout still exceeds the worst healthy refresh interval by the
margin. If it does not, the timeout is changed from the loaded data and the change
is recorded. **It has been written down:** `data/prereg/P-H0W-step8-2026-09-29.md`
holds the criteria, the bench numbers the trial is compared against (including the
realized 174 ms), and what would make the timeout change.

Freezing a competition build requires this re-validation to exist; until then the
chosen timeout is a bench number and is recorded as such.

## Measurements and logs

Per step, record: the enablement constant and whether `opened()` is true; the
requested and the realized timeout; the pass/refresh/withheld counters, the worst
observed interval between refreshes and the **maximum control-loop stall**, per
load condition; the last-refresh time and the pass count at the hang; the reset
latency and the evidence of restart (the `BOOT,...` line, the `EVE1` boot event,
and `t_ms` with the counters restarting); the pin levels after restart; and the
upload result after each reset. Keep the raw capture for every step.

## Repetitions

Pre-registered hang/reset cycles (suggest at least 5) at each candidate timeout;
the recovery/re-flash cycle repeated at least 5 times; each healthy soak long
enough to cover at least 3x the realized timeout at each load condition.

## Safe-stop (abort) conditions

The hang is intentional, so the abort is about the board, not motion: remove the
motor supply and USB if the reset does not occur within 3x the realized timeout,
if the board restarts repeatedly on its own, or if the pin levels after restart
are not the de-energized combination. Any unexpected motion is an immediate abort
and a finding (the motor supply should be off in this protocol).

## Acceptance / decision rule

Pass requires all of: `opened()` true with the chosen timeout; zero resets while
healthy across every load condition; a hung loop reliably resets and the restart
is attributable to the timeout by latency and by the absence of any other refresh
path; the pin levels after restart are the de-energized combination; and a reset
board re-uploads without manual intervention, at the pre-registered rate. If the
hang never resets, the watchdog did not start - record `opened()` and do not
proceed. If resets occur while healthy, the timeout is too short for the real
loop or something else refreshes the watchdog; find which before changing a
number.

## If it passes

- Add a **new** on-target row recording: the watchdog starts, counts, refreshes
  only from the central checkpoint, resets on a stalled loop, the restart is
  followed by the de-energized pin levels, and the chosen timeout plus its
  realized period and the margin evidence behind it.
- Record the cadence margin as evidence for the cadence policy values (the
  sample-age limit is a policy guess until this exists); if the observed margin
  contradicts a policy value, fix the value, not the measurement.
- Record the maximum control-loop stall per load condition as the bound on
  near-zero dwell credited without observation (see "Why the maximum control-loop
  stall is recorded here"). It is a measurement, not a new timeout: it does not
  change `kWatchdogTimeoutMs`, which Step 6 chooses from the margin.
- Record explicitly that the chosen timeout is **provisional for the loaded run
  path** until Step 8 is repeated under a loaded run, and that freezing the
  competition build depends on it.
- Log in `DEVELOPMENT_LOG.md`. No numbered row #1-#12 changes, and no claim about
  motor-output safety is recorded here.

## If it fails or is inconclusive

Do not flip production enablement. Diagnose in order: a refused `WDT.begin()`
(the request is unachievable or the timer was already opened); another code path
touching `WDT` (a static search answers this); a timeout below the real worst
pass; a boot/recovery problem after reset. Record exactly which of these it was.
