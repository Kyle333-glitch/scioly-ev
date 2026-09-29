# P-H0W session - watchdog qualification, Steps 0-7 and the post-reset pin read

- protocol: `docs/bringup/P-H0W-watchdog-qualification.md`
- pre-registration: `data/prereg/P-H0W-2026-09-28.md` (written before the Step 0
  upload; margin 100 ms, soak 60 s, 5 hang cycles, 5 recovery cycles, hook = 500
  passes, all fixed in advance)
- date: 2026-09-28, continuing the same session as the H0 repeat
- entry gate: H0 **passed** in its pre-registered repeat
  (`data/evidence/P-H0-repeat-2026-09-28/`, D25)
- repository: `git rev-parse HEAD` = `8e42d13b69e6fcbb79275ff7699ffe4c9608fcc3`
- images: the session's **first** pair was `env:bench`
  `sha256 = 71f42e40...` (watchdog disabled) and `env:bench-watchdog`
  `dc5a1bcb...` (enablement requested, 250 ms). The campaign rebuilt them twice -
  D27 added the worst-refresh-gap field, D28 added the pin readout and the stall
  field - so the captures below span three image generations. Every upload and its
  hash is in `h0w-uploads.log`. The **final** images on disk are the vehicle
  `eb440b319a8a09aeb4ee82f0aa37f24a80f3f4a011b49d14639d895bc5d88d50`, the bench
  `1f775f89fad691b6ba9796da0916eea5a58efd4d16eaf9690fe223018dc7a973`, and the
  bench-watchdog (114 ms, hook `0U`)
  `ddba3b9d9c0f4fbfd1f5a05051e81d943d4e442fc73bfcea798f2245957f2714`. The
  campaign's deliberate-hang images were `2566610c...` (250 ms), `47813b0e...`
  (114 ms), `27449ca1...` (D28, hook `0U`) and `858a5a32...` (D28, hook
  `460000`).
- board: Arduino Nano R4 on `COM3`; **motor supply off and disconnected**, wheels
  unloaded, nothing armed. No motor, driver, encoder or battery connected.
- **Scope of this session: H0W Steps 0-7, with the post-reset pin clause closed in
  D28.** Every clause of the protocol's acceptance rule now has recorded evidence
  (clause table at the end), so **H0W is recorded as passed for the bench load**.
  Two things stay open, and neither is an acceptance clause: **Step 8** (the
  loaded-run re-validation, which the protocol pre-registers as owed and which
  cannot run without a run path) and any statement about the **driver outputs**
  (H1A's electrical measurement). The chosen 114 ms is therefore a **bench** number,
  provisional for the loaded run path.

## Captures

All captures are copied unchanged from `data/raw/`; a `*.times.txt` beside a
capture is its derived arrival index (`elapsed_s<TAB>kind<TAB>line`), used for the
reset latencies - the raw file itself stays verbatim.

| file | what it contains |
|---|---|
| `h0w-uploads.log` | every upload this session with its output and exit code, and every image hash |
| `h0w-step0-baseline.log` | 60 s on `env:bench`, watchdog disabled, host reading, `bench health` at 2 s and 58 s |
| `h0w-step1-enabled.log` | 60 s on `env:bench-watchdog` (250 ms), same load, same two readings |
| `h0w-step2-telemetry-off.log` | 62 s enabled, telemetry **off**, host reading |
| `h0w-step2-50hz.log` | 62 s enabled, highest admissible rate (50 Hz) with the host reading |
| `h0w-step2-unread.log` | 60 s with **nobody reading**, then attach and read the counters |
| `h0w-step3-hang.log` | the first hang attempt, and the capture dying when the device vanished |
| `h0w-step3-hang-cycles.log` (+`.times.txt`) | 95 s, hook set: **8 hang -> reset cycles**, latencies and reconnects |
| `h0w-step5-hang-telemetry-off.log` (+`.times.txt`) | hang cycles with telemetry off |
| `h0w-step5-hang-unread.log` (+`.times.txt`) | hang cycles with nobody reading, then attach |
| `h0w-step7-timeout114-soak.log` | 63 s healthy at the **chosen 114 ms**, host reading |
| `h0w-step7-hang-114ms.log` (+`.times.txt`) | hang campaign at 114 ms: **6 cycles** |
| `h0w-step7-restored-hook0.log` | 30 s healthy after the hook was restored to `0U` |
| `h0w-final-state-vehicle.log` | the vehicle image after D27's last upload (beacon pair, 14 s) |
| `h0w-step8-pins-baseline.log` | D28: 57 s on the 114 ms image with the hook inert, `bench pins` read three times and `worst_stall_ms` at both ends |
| `h0w-step8-pins-after-reset.log` (+`.times.txt`) | D28: the hang campaign with `bench pins` issued on every reconnect - **6 hangs, 6 resets, a pin readout after each** |
| `h0w-step8-stall-loads.log` | D28: `worst_stall_ms` under the two read load conditions |
| `h0w-step8-stall-unread.log` | D28: `worst_stall_ms` with nobody reading (one reading) |
| `h0w-step7-final-soak.log` | D28: 45 s healthy on the final source, hook `0U` |
| `h0w-step7-final-hang.log` (+`.times.txt`) | D28: 3 hangs, 3 resets and a pin readout after each, on the final source |

**A naming note.** The D28 captures are named `h0w-step8-*` for the step they were
*preparing* - they close Step 3's post-restart pin clause and Step 2/5's stall
measurement, **not** Step 8. Step 8 is the loaded-run re-validation and has no
captures, because it has no run path to run.

## Step 0 - baseline, watchdog disabled

```
BENCH,health=1,image=bench,enable=0,open_attempted=1,opened=0,open_refused=0,timeout_ms=250,
passes=259536,refreshes=0,withheld=0,last_refresh_ms=0,first_refresh_ms=0,control_ticks=253,
dt_ms=20,late_ms=0,dropped=0,worst_late_ms=1980,dropped_total=99,violations=0,armed=0
... 56 s later ...
passes=2923918,refreshes=0,withheld=0,control_ticks=3052,late_ms=0,dropped=0,
worst_late_ms=1980,dropped_total=99,violations=0
```

- The disabled build's expected shape: `enable=0, open_attempted=1, opened=0,
  open_refused=0` - the boot path reached its open point and did not open.
- `refreshes=0` (nothing refreshes), `control_ticks` +2799 in 56 s = **50.0
  ticks/s**, and the telemetry `t_ms` ran monotonically from 2000 to 65042 with a
  single read-window jump and no restart: **one boot, no resets, 60 s.**
- The session extremes are identical at both ends (`worst_late_ms=1980`,
  `dropped_total=99`) - they are the boot's bounded wait, and nothing accrued
  during the soak.

## Step 1 - the prepared path enabled

`pio run -e bench-watchdog -t upload` (the define is the environment's, no header
was edited) and the same 60 s soak:

```
BENCH,health=1,image=bench,enable=1,open_attempted=1,opened=1,open_refused=0,timeout_ms=250,
passes=232099,refreshes=232099,withheld=0,last_refresh_ms=6700,first_refresh_ms=0,
control_ticks=235,dt_ms=20,late_ms=0,dropped=0,worst_late_ms=1980,dropped_total=99,
violations=0,armed=0
... 56 s later ...
passes=2808202,refreshes=2808202,withheld=0,last_refresh_ms=62681,control_ticks=3034,
late_ms=0,dropped=0,worst_late_ms=1980,dropped_total=99,violations=0
```

- **`opened()=1`, `open_refused=0`: the requested 250 ms was accepted by the
  timer.** The adapter's open point ran and opened; nothing was refused silently.
- **`refreshes == passes` exactly (+2576103 passes, +2576103 refreshes,
  `withheld=0`)**: one refresh per completed loop iteration, none withheld, and
  no second refresh path.
- **Zero unexplained resets in 60 s**: one boot (`t_ms` 2000 -> 64669,
  monotonic), the six `BOOT,...` lines are the silence beacons, and
  `violations=0`.
- `late_ms=0, dropped=0` at both readings and the extremes unchanged from the
  disabled baseline: **enablement did not change the loop's health.**
- `first_refresh_ms=0` = the boot-to-first-refresh gap is **below one millisecond
  at `millis()` resolution** - the watchdog is opened at the end of `setup()`,
  after the bounded serial wait, and the first pass follows immediately. The 50 ms
  placeholder in `WatchdogCheckpoint.h` is therefore at least 50x too large.

## Findings

1. **The main loop is free-running, and `passes` counts loop iterations, not
   control ticks.** Both soaks show ~46,000-47,600 loop iterations per second
   against exactly 50 control ticks per second. With enablement on, the watchdog
   is therefore refreshed ~46,000 times/s, so the *healthy refresh interval* is one
   loop iteration (~21 us), not the 20 ms control period. What a timeout actually
   has to survive is (a) the gap before the first pass and (b) a stall **inside**
   a pass - not the period. This is the single most important measurement for
   Step 6, and it points at the opposite of the placeholder's assumption.
2. **`first_refresh_ms=0`**: the boot-to-first-refresh gap is sub-millisecond,
   because the open is deliberately after the bounded wait (which is itself the
   ~1980 ms stall D25 measured, and the reason for opening late).
3. **B1 cannot report the worst refresh interval**, as pre-registered: it had
   `passes`, `refreshes`, `withheld`, `last_refresh_ms`, `first_refresh_ms` and the
   lateness extremes, but no worst-interval field. `worst_late_ms` was used as the
   proxy (the refresh happens at the end of a pass, so tick lateness is the
   refresh spacing up to one period), and the missing field was recorded as a
   finding rather than worked around silently - **closed in D27**, when
   `worst_refresh_gap_ms` was added and every later reading reports it.
4. **No reset-cause register was needed and none was observed**: no reset occurred,
   so there was nothing to attribute.
5. **Two uploads, both with no manual touch** (one per environment), exit 0, the
   hook's first touch attempt each time - bringing the project to 16 consecutive
   successful uploads since F-3.

## Step 2 - the three load conditions, 60 s each (watchdog enabled, 250 ms)

A B1 field for the quantity Step 2 actually asks for was added first (D27, below):
`worst_refresh_gap_ms`, the longest interval between two consecutive refreshes,
recorded in the checkpoint from the `nowMs` the refresh already uses - a
comparison per pass, no timer read, and no change to when the watchdog refreshes.

| condition | file | `worst_refresh_gap_ms` | `late_ms`/`dropped` | resets |
|---|---|---|---|---|
| telemetry **off**, host reading | `h0w-step2-telemetry-off.log` | 14 | 0/0 | none (one boot, `t_ms` to 66 s) |
| **highest admissible rate** (50 Hz; `telemetry rate 50` accepted as `stride=1`) with the host reading | `h0w-step2-50hz.log` | 14 | 0/0 | none |
| same rate, host **deliberately not reading** for 60 s | `h0w-step2-unread.log` | 14 | 0/0 | none (re-attach showed one boot and `drop_s` rising only while nobody read) |

`worst_refresh_gap_ms = 14` in every reading of every condition, and it never grew
over ~190 s of soaking. `refreshes == passes` throughout with `withheld=0`.

The highest admissible rate turned out to be the default: `telemetry rate 50` was
accepted and reported `stride=1, rate_hz=50.0000`, so condition (c) is condition
(b) with the command sent explicitly. That is recorded rather than pretended to be
a second load.

## Step 3 - the deliberate hang (250 ms requested)

The hook counts **loop iterations**, not control ticks (D26): `460000` passes is
about 10 s after the first pass. With the hook set and the image uploaded, the
loop hangs, the port stops answering, and the board resets. The first attempt's
capture died on `ClearCommError failed (PermissionError(13, ...))` - which is
itself the reset, seen from the host side - so the campaign uses
`data/raw/h0w_cyclic_capture.py`, which **reopens** the port after each
disconnect and keeps a `.<name>.times.txt` index of when each line arrived.

`h0w-step3-hang-cycles.log` (95 s): **8 hangs, 8 resets**, no cycle failed to
reset. Each cycle: `BENCH,hang=1,image=bench,after_passes=460000`, the device
disappearing **171-344 ms** later, a reconnect ~0.5 s after that, then `BOOT,...`
and ~10 s of healthy telemetry before the next hang. The device disappearing is
the reset; the host's own detection is part of the measured latency, so this
bounds the realized period rather than clocking it.

## Step 4 - recovery uploads (5)

Five `pio run -e bench-watchdog -t upload` runs **while the board was in its
hang/reset loop**: all five succeeded with no manual intervention and no manual
bootloader entry, 8-9 s each. The 1200-bps touch reaches the board even while the
main loop is hung (the USB stack runs in interrupts). No occurrence of the F-3
non-enumerating state, and no power cycle was needed at any point in this session.

## Step 5 - the hang under load

- **Telemetry off** (`h0w-step5-hang-telemetry-off.log`): hangs at 1.36 s, 13.13 s
  and 24.80 s, each followed by the device vanishing 235-297 ms later and a
  healthy ~10 s run. The hang behaves the same with the serial path idle.
- **Host not reading** (`h0w-step5-hang-unread.log`): the board was left unread for
  40 s while looping. Re-attaching found `EVE1,...,event=Boot` at `t_ms=0` (a reset
  had just happened with nobody watching) and then `bench health` reporting
  `enable=1,opened=1,open_refused=0,refreshes==passes,worst_refresh_gap_ms=14` -
  **the watchdog re-arms and resets with no host attached**, and the reset loop
  continues while unread.

## Step 6 - the timeout, chosen from the measured numbers

The pre-registered margin is 100 ms (five nominal control periods), the measured
boot-to-first-refresh gap is **0 ms** (`first_refresh_ms` reports 0: sub-millisecond
at `millis()` resolution) and the worst healthy refresh interval is **14 ms**
(identical across all four conditions). `ev::requiredWatchdogTimeoutMs()` therefore
returns **114 ms**, and that is what `kWatchdogTimeoutMs` was set to - the placeholder
250 ms was not inherited. The placeholders behind it were replaced with the
measurements (`kBootToFirstRefreshBudgetMs = 0`, `kWorstRefreshIntervalBudgetMs = 14`,
`kWatchdogPlanMarginMs = 100`), and the compile-time assertion now applies the
**whole** rule (`ev::timeoutIsQualified`, margin included) plus a new assertion that
the request equals `requiredWatchdogTimeoutMs()` so a hand-edited value cannot
drift away from the rule that chose it (D27).

## Step 7 - re-run at the chosen timeout

| step | file | result |
|---|---|---|
| healthy soak, 114 ms, hook inert | `h0w-step7-timeout114-soak.log` | `opened=1, timeout_ms=114, open_refused=0`, `refreshes == passes` (+2,416,028, `withheld=0`), `worst_refresh_gap_ms=14`, `first_refresh_ms=0`, **no reset in 63 s**, `violations=0` |
| hang campaign, 114 ms | `h0w-step7-hang-114ms.log` | **6 hangs, 6 resets**; hang -> device-gone latency **0-141 ms** (mean 36 ms) |
| hook restored to `0U`, 114 ms | `h0w-step7-restored-hook0.log` | 30 s healthy, `opened=1`, `worst_refresh_gap_ms=14`, no reset |

The latency difference between the two campaigns is the attribution evidence H0W
asks for: **171-344 ms at a 250 ms request against 0-141 ms at a 114 ms request**,
with the loop healthy and refreshing in between. The measured latency also
includes the host noticing the device is gone (bounded by the capture's ~0.2 s read
window), so it tracks the realized period without clocking it.

**A unit correction worth recording:** the pre-registration said the hook should be
left "non-zero" with `kDeliberateHangAfterPasses = 500` ("about 10 s after boot").
That estimate assumed one pass = one 20 ms control tick; D26 measured ~46,000
passes/s, so 500 passes would have hung the loop ~11 ms after boot and the campaign
used **460000**. The hook counted by iterations all along - the number in the
pre-registration was wrong, not the hook.

## The post-reset pin levels and the loop's worst stall (D28)

Two instruments were missing for clauses the protocol already asked for, and D28
added them rather than working around them.

**`bench pins` - the pins as the MCU holds them.** A PWM duty cannot be read back
from a pin, so the readout separates the two kinds of fact in the line itself: the
three levels are **observed** (`digitalRead()` -> `R_IOPORT_PinRead()`) and the duty
is the value the adapter last **wrote**, and `source=pin_read` says which is which.
`deenergized=` is `ev::drv8874InputsAreDeEnergized()` - the same pure predicate that
judges a command - applied to those levels instead of to the request, so a pin held
the wrong way reports `0` rather than reading as safe.

| capture | readings | result |
|---|---|---|
| `h0w-step8-pins-baseline.log` | 3, no hang | every one `nsleep_high=0,phase_high=0,enable_high=0,duty_permille_written=0,deenergized=1` |
| `h0w-step8-pins-after-reset.log` | 7 - the first before any hang, then one after each reset | **6/6 post-reset readings de-energized**, all fields as above |
| `h0w-step7-final-hang.log` | 4 (1 before the first hang, 3 after resets) | same values; each reset 47-78 ms after the `BENCH,hang=1` line |

Nine readings after a reset, all the de-energized combination. **What that is and
is not:** it is the MCU's own view of the pins it drives, which is the "levels only"
the protocol asks for and explicitly hands the electrical statement to H1A. It
would *not* see a pin driven low but held high at the header, and every observed
reset followed a **drive-free** hang - so a reset taken while an effort is
commanded is not covered, and cannot be with the motor supply off.

**`worst_stall_ms` - the largest gap between two consecutive control ticks.**
Lateness (`worst_late_ms`) is measured against the deadline and is a different
quantity; a passing tick can be late without the loop stalling. The stall is the
interval between two committed ticks, with the **first tick excluded** so the
~1980 ms boot gap cannot dominate it.

| condition | file | `worst_stall_ms` |
|---|---|---|
| 57 s baseline, host reading | the D28 baseline capture | **28** (identical at both ends) |
| telemetry off, host reading | the D28 load capture | **21** |
| highest admissible rate, host reading | the D28 load capture | **21** |
| highest admissible rate, host not reading | the D28 unread capture | **22** |
| 45 s soak, final source | `h0w-step7-final-soak.log` | **27** |

The value does not track the load, sat at 21-28 ms against a 20 ms period, and is
the measurement behind `kMaxDwellGapMs = 28` - the bound on how much near-zero
dwell may be credited without fresh evidence, replacing the 40 ms placeholder. It
is a **bench** measurement: a loaded run adds work to the same loop (Step 8).

## H0W's acceptance rule, clause by clause

| Clause | Result |
|---|---|
| `opened()` true at the chosen timeout | **met** - 114 ms accepted, `open_refused=0` |
| zero resets while healthy, every load condition | **met** - telemetry off, 50 Hz read, 50 Hz unread, and a 63 s soak at the chosen timeout, one boot each, `violations=0` |
| a hung loop reliably resets, within 3x the realized timeout | **met** - 8/8 cycles at 250 ms and 6/6 at 114 ms, the longest observed latency 344 ms (3x 250 ms = 750 ms) |
| the restart is attributable to the timeout | **met** - latency scales with the request (171-344 ms at 250 ms vs 0-141 ms at 114 ms), the hang hook is the only difference, one refresh path (`grep -rn "WDT" src/` matches only `WatchdogCheckpoint.h`), and the healthy soaks never reset |
| **the pin levels after restart are the de-energized combination** | **met, at the MCU level (D28)** - the pins to check are the ones `src/PinMap.h` names, read at rest after a watchdog reset. `bench pins` after each of 6 resets reported `nsleep_high=0,phase_high=0,enable_high=0,duty_permille_written=0,deenergized=1`, 6/6, plus 3 more in the final hang campaign; the readings before any hang are identical. This is the MCU's own `digitalRead()` of the three pins it drives - the "levels only" the protocol asks for (its own scope limit hands the electrical statement to H1A), not an independent measurement at the header. Every observed reset followed a **drive-free** hang, so a reset taken while an effort is commanded is not covered |
| a reset board re-uploads without manual intervention, at the pre-registered rate | **met** - 5/5 recovery uploads into a looping board, plus the Step 7a/7b/7c uploads |
| native tests and the Nano build still pass | **met** - `pio run`, `-e bench`, `-e bench-watchdog` all SUCCESS (re-confirmed at D28 with the same three hashes); `pio test -e native` 517/517 at D27 and **518/518** after D28's added stall test |

**H0W is recorded as passed for the bench load (D28)**: every clause above is met
with recorded evidence, including the pin clause, which the protocol asks for at the
MCU level. **Step 8's loaded-run re-validation is still owed** - but the protocol
pre-registers it as "owed, not done here" rather than as an acceptance clause, so
it does not block the pass. The chosen timeout is therefore a **bench-qualified**
number: justified for the bench load, and provisional for the loaded run path until
Step 8.

## What this does NOT establish

- **Anything external about the reset pin levels.** The D28 readout is the MCU's
  own `digitalRead()` of the pins it drives: it shows the firmware re-initializes
  them to the de-energized combination after a reset, but it would not see a pin
  driven low yet held high at the header, and whether the levels de-energize the
  **driver outputs** is H1A's electrical measurement.
- **A reset taken while an effort is commanded**: every observed reset followed a
  drive-free hang, because this protocol keeps the motor supply off by design.
- **The realized timeout period**, precisely: the timer rounds the 114 ms request up
  and the latency measured is the host noticing a vanished device, not a timer
  capture.
- **Anything about the loaded run path**: the soak has no run path and no motor, so
  the margin behind 114 ms is a bench margin (Step 8, owed).
- **Anything about the vehicle or motor-output safety.** No motor supply, no driver,
  no encoder: what the driver pins *do* at a reset is H1A's question.
- **`kMaxDwellGapMs = 28` is a bench measurement, not a ceiling.** It is the worst
  `worst_stall_ms` read across the D28 loads (28 with the host reading, 21-22 under
  the other two), and it now bounds credited near-zero dwell in place of the 40 ms
  placeholder. Lower is the safe direction - a longer stall only restarts the
  verifier's interval - but Step 8 has to re-take it once a loaded run adds work to
  the same loop.
