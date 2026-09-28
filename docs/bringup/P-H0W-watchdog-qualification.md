# P-H0W - Watchdog qualification and timeout choice

**Primary class:** S (on-target software check).
**Also contains:** E observation of the MCU pin levels (motor supply off; motor
disconnected).
**Order:** immediately after H0, before H1A.
**Answers:** does the prepared watchdog path actually start, count, refresh and
reset on this target; does a healthy loop run long enough without a false reset;
does a deliberately hung loop stop refreshing and reset the board; does the board
recover and re-flash reliably; what timeout is justified by the observed healthy
margin and the observed reset behaviour.
**Bench instrumentation required:** B1 (loop and watchdog counters), and a
bench-only hang hook the production image cannot contain - both supplied by the
separate PlatformIO `bench` environment (see the index), not by a flag inside the
production build.

**Scope limit that must not be crossed.** This protocol establishes that the MCU
resets and that the firmware re-initializes. It does **not** establish that a
reset de-energizes the motor output. The pin levels the firmware drives on reset
are observable here, but what those levels do at the driver outputs is a
different fact and is measured electrically in **H1A**. No statement about
motor-output safety may be recorded from H0W.

## Prerequisites

- H0 passed.
- `src/WatchdogCheckpoint.h` reviewed: `kWatchdogEnablementRequested` is the
  single enablement constant, `kWatchdogTimeoutMs` is the requested timeout, and
  refresh happens only in `endOfPass()`. Confirm by source inspection
  (`grep -rn "WDT" src/` matches only that file) that nothing else can refresh.
- The bench-only hang hook and B1 readout are present in the bench build and
  absent from the production build (verify with a build-flag check and a static
  search).
- Motor supply off and disconnected; drivetrain unloaded.
- (Pre-register) the soak duration, the margin that defines "exceeds the worst
  healthy refresh interval", and the number of hang/reset/recovery cycles.

## Setup

A bench build containing B1 and the hang hook; the board on USB with logging to
a file; the motor supply off and disconnected; the drivetrain unloaded. The
operator must be able to see the board restart (the `BOOT,...` line in the log)
and to power-cycle the board if the protocol asks for it.

## Held constant

- the firmware image under test (except `kWatchdogEnablementRequested` and, in
  Step 7, `kWatchdogTimeoutMs`, which are the things being varied);
- the host, cable and port; the telemetry rate within one load condition;
- the board, the supply and the ambient temperature.

## Varied

- whether the watchdog is enabled (Step 0 versus Step 1);
- the load condition (telemetry off, reading, unread);
- whether the loop is healthy or deliberately hung;
- the requested timeout, once, in Steps 6-7.

## Step 0 - Baseline, watchdog disabled

The shipped production setting is `kWatchdogEnablementRequested = false`. With it
disabled, establish upload and reboot reliability as in H0 but for the bench
tree: record the pre-registered number of successful uploads and power cycles.
Confirm healthy operation for the pre-registered soak: no resets, `refreshes()`
and `passes()` both increasing, `withheldRefreshes()` staying at zero. This is the
baseline the enabled build is compared against.

## Step 1 - Enable the prepared path

In a **bench build only**, set `kWatchdogEnablementRequested = true`. Record the
value of `kWatchdogTimeoutMs` and, through B1, whether `opened()` reports true
after `begin()`. `WDT.begin()` returns zero when the requested timeout exceeds
what the timer can produce; a refused open must be visible, not silent. Upload
the bench build.

Note the library's quantization, because the requested number is not the realized
one: the timer picks a prescaler and a reload value and rounds **up** to the next
achievable period, so the realized period is the requested value or slightly
longer. Record the realized period (from the library's own computation, or by
measurement in Step 3). Do not treat `kWatchdogTimeoutMs` as the realized period.
Worth recording too: on this library the timeout is configured as a reset, and
the refresh window is fully open (a refresh is accepted at any time in the
period), so this test is about the timeout alone, not a window.

## Step 2 - Healthy operation, no false resets

Soak the enabled build for the pre-registered duration. Record: reset count (must
be zero), `withheldRefreshes()` (must be zero), and the worst observed interval
between consecutive refreshes with B1. Repeat at three loads: telemetry off,
telemetry at the default rate with the host reading, and telemetry at the highest
admissible rate with the host reading. Each of these is a *healthy* condition and
contributes to the margin, so record the worst case across all of them, not the
average.

## Step 3 - Deliberate hang

Using the bench-only hook, hang the main loop after a recorded number of
completed passes (for example a compile-time constant that enters an endless loop
inside a pass, after B1 has recorded the pass count). Nothing else in the
firmware refreshes the watchdog, so refresh stops. Record:

- the time of the last refresh and the pass/refresh counters at the hang;
- the wall-clock latency from the hang to the board restarting;
- that the board *does* restart (the `BOOT,...` line reappears, the `EVE1` boot
  event recurs, the telemetry `t_ms` restarts near zero, and the counters restart
  from zero);
- the MCU pin levels `nSLEEP`/`PH`/`EN` after the restart, which must be the
  de-energized combination the firmware initializes to (levels only; H1A turns
  this into a statement about the driver outputs);
- whether the restart is attributable to the watchdog rather than to something
  else: the hang hook is the only difference from Step 2, the loop cannot refresh
  while hung, and the latency should match the realized timeout. If a reset-cause
  register is readable on this core, record it; do not assume one exists.

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

Choose `kWatchdogTimeoutMs` only now. The rule:

- it must exceed the **worst** healthy refresh interval observed across every
  healthy condition in Steps 2 and 5 (including the not-reading host case) by the
  pre-registered margin;
- it must be achievable by the timer's quantization - check that
  `WDT.begin()` actually opens with the chosen value, and record the realized
  period, which may be longer than requested;
- the reset latency observed in Steps 3 and 5 must be acceptable for the
  vehicle's stop behaviour; if the reset is too slow to be a credible stop, that
  is a finding, and the answer is physical (a brake path), not a shorter timeout
  that reintroduces false resets;
- do **not** inherit the current placeholder `250 ms` or any other number without
  this evidence.

## Step 7 - Finalize

Set `kWatchdogTimeoutMs` to the chosen value, re-run Steps 2, 3 and 5 at that
value, and record the realized period. Only after that may production enablement
be considered - and that is a separate, deliberate change, not part of this
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
is recorded.

Freezing a competition build requires this re-validation to exist; until then the
chosen timeout is a bench number and is recorded as such.

## Measurements and logs

Per step, record: the enablement constant and whether `opened()` is true; the
requested and the realized timeout; the pass/refresh/withheld counters and the
worst observed interval between refreshes, per load condition; the last-refresh
time and the pass count at the hang; the reset latency and the evidence of
restart (the `BOOT,...` line, the `EVE1` boot event, and `t_ms` with the counters
restarting); the pin levels after restart; and the upload result after each
reset. Keep the raw capture for every step.

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
