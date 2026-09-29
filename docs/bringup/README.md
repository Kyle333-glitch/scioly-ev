# Physical bring-up and experiment protocols (P8B)

**Status: PRE-WRITTEN PROCEDURES. Not evidence.**

These files are the plan for what will be measured once the vehicle hardware
exists. Nothing here is an observation, nothing here changes a status in
`VERIFIED_STATE.md`, and no number in this directory describes the vehicle. A
protocol becomes evidence only when a person has run it on the real hardware and
its raw observations are recorded.

**Hard stop.** Every protocol below is executed by a person with the hardware in
front of them. None is executed by a coding agent, and this stage stops before
anything is powered. Writing a procedure is not running a test.

**As of D25-D28, H0 and H0W have been executed** on the real board - USB only,
motor supply off and disconnected - by the operator, and their raw captures are in
`data/evidence/P-H0-2026-09-28/`, `data/evidence/P-H0-repeat-2026-09-28/` and
`data/evidence/P-H0W-2026-09-28/`; the rows they earned are in `VERIFIED_STATE.md`.
The protocol files themselves remain the plan, and nothing in this directory is
evidence.

## Why this exists

The firmware, the operator interface and the analysis tooling are built and
unverified on hardware. When the board arrives, the first hour should be spent
measuring, not designing tests. Each protocol here isolates one unknown, names
the exact commands and readings, and says what may be recorded if it passes.

## The three check classes

Every protocol is labelled with one **primary class** and lists the classes it
also contains. Keeping them apart is deliberate: an electrical fact, a software
fact and a physical fact have different evidence and must not be confused.

- **E - electrical gate.** Establishes or confirms an electrical fact while the
  drivetrain cannot produce dangerous motion: supply off or current-limited,
  motor disconnected or outputs unloaded. A failed E check blocks every later
  protocol that depends on it. E is not a synonym for "safe forever": it means
  this specific test cannot move the vehicle in a way that matters.
- **S - on-target software check.** Exercises the firmware on the real board and
  records what it did. No motion is intended. A pass here says the code behaves;
  it says nothing about the vehicle.
- **P - physical behaviour check.** Measures how the drivetrain, vehicle or
  mechanism actually responds, whether hand-driven or powered. A P protocol may
  run only after the E gates it names have passed.

## Order and blocking

Run them in order. Each protocol states which earlier protocols must have passed.
The series extends the program's own names (`H0`, `H0W`, `H1A`); the rest follow
the same pattern.

| # | Protocol | Primary class | Blocked by |
|---|----------|---------------|------------|
| H0 | [Upload, boot and serial link](P-H0-nano-upload-serial.md) | S | - |
| H0W | [Watchdog qualification and timeout choice](P-H0W-watchdog-qualification.md) | S (+E observation) | H0 |
| H1A | [Driver electrical logic, motor disconnected](P-H1A-driver-electrical.md) | E | H0 |
| H1B | [Battery, current, nFAULT and trigger](P-H1B-sense-inputs.md) | E | H0, H1A |
| H2 | [Encoder sign and counting, hand-driven](P-H2-encoder-manual.md) | P | H0 |
| H3 | [First powered drive, drivetrain unloaded](P-H3-first-low-duty-drive.md) | P | H1A, H1B, H2 |
| H4 | [Rollout: effective distance per tick, and slip](P-H4-rollout.md) | P | H2, H3 |
| H5 | [Low-speed velocity trace](P-H5-velocity-trace.md) | P | H2, H3, H4 |
| H6 | [Feed-forward and PI tuning](P-H6-ff-pi-tuning.md) | P | H5 |
| H7 | [Distance, braking and reversal](P-H7-distance-braking-reversal.md) | P | H4, H6 |
| H8 | [Bottle integration](P-H8-bottle-integration.md) | P | H7 |
| H9 | [Fault injection](P-H9-fault-injection.md) | P (+E, +S) | H3, H7 |
| H10 | [Full-run repeatability](P-H10-full-run-repeatability.md) | P | H7, H8, H9 |

## Bench instrumentation (built; B1, the pin, reset-cause and watchdog readouts and the hang hook exercised)

Several protocols need the firmware to report something the production image
deliberately cannot, one needs a bounded way to command effort outside a run, and
H0W needs a deliberate way to stall the loop. These instruments exist **in code**,
and an instrument is a prerequisite for a measurement, never a measurement itself.

**Status: BUILT.** B1, the driver-pin readout, the reset-cause readout, the
watchdog-register readout and the deliberate-hang hook have all been run on the
board in the H0 and H0W sessions (D25-D30): the counters, the cadence extremes, the
worst refresh interval and the worst control-loop stall were read, the hook produced
real watchdog resets, and the reset that follows one was attributed from the MCU's
own reset-status registers. Their evidence is in
`data/evidence/P-H0W-2026-09-28/` and the rows in `VERIFIED_STATE.md`. **B2, B3 and
B4 have not been run on hardware**, so nothing about the bench drive, the raw tick
readout or the raw input readout is observed yet, and no protocol that uses B2 runs
until H3's first step has demonstrated its bounded expiration and latched fault.

The mechanism is the separate PlatformIO `bench` environment described in
`SOFTWARE_ARCHITECTURE.md` section 17: `env:bench` compiles `src/bench/` and the
bench entry point, and `env:nano_r4` (the vehicle image, and what `pio run`
builds) does not compile either. The guarantee is in the build graph rather than
in a macro, because a macro that is merely absent could be defined by accident,
while a source tree the production environment does not compile cannot run. It is
verifiable without running anything: a symbol search over the two `firmware.elf`
files finds the console's symbols in the bench image and none in the vehicle
image.

Build and use it with `pio run -e bench -t upload`; every line it prints starts
with `BENCH`, and `bench health` reports `image=bench`. The analyzer treats a
`BENCH` line as an ignored line, not as telemetry. None of these may become a
control input.

H0W needs the watchdog **enabled**, which is a separate environment rather than an
edit to a header both images compile: `pio run -e bench-watchdog -t upload` builds
the same bench tree with `-DEV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED=1`, which is the
define `kWatchdogEnablementRequested` reads. H0W Step 0 uses `env:bench` (disabled,
the shipped value); Steps 1-7 use `env:bench-watchdog`. The vehicle image never
receives the define, and `src/main.cpp` refuses to compile if it ever does.

- **B1 - loop and watchdog health readout.** `bench health`. Reports the
  checkpoint's `passes`, `refreshes`, `withheld`, `last_refresh_ms`, the
  **measured boot-to-first-refresh gap** (`first_refresh_ms`), whether the open was
  attempted/opened/refused, whether enablement is requested, the requested timeout,
  and the scheduler's `control_ticks`/`dt_ms`/`late_ms`/`dropped`/`violations`.
  `late_ms` and `dropped` describe the **last control tick**; `worst_late_ms` and
  `dropped_total` (D22) are the session's running extremes, which is the pair
  H0 Step 7's "worst case, never the average" needs. `worst_refresh_gap_ms` (D27)
  is the longest interval between two consecutive refreshes (the quantity H0W
  Step 2 asks for), and `worst_stall_ms` (D28) the longest gap between two
  consecutive control ticks, with the first tick excluded so the bounded boot wait
  cannot dominate it - the stall, not the period, is the number that bounds credited
  near-zero dwell.
  Needed by H0 and H0W (cadence evidence, the watchdog timeout choice, and the
  maximum control-loop stall that bounds the near-zero dwell credited without
  observation - see the H0W protocol). It is read-only, and the refresh stays in
  exactly one place: a readout cannot feed the watchdog.
- **B2 - bench-only diagnostic drive.** `bench arm`, `bench drive <fwd|rev>
  <permille> <ms>`, `bench stop`, `bench reset`, `bench drive-status`. A bounded,
  time-limited way to command a conservative effort and a direction outside the
  run, for H3-H10. The operator interface deliberately has no such command (D14);
  the production image does not contain this one either (see above). Its safety
  model is `ev::BenchDrive` (`lib/evcore/src/BenchDrive.h`), which is pure logic
  and host-tested (`test_bench_drive`):

  - a **latched fault** on an abort. An operator `bench stop`, or an encoder that
    stops changing while the motor is commanded to drive, de-energizes the motor
    *and stays* de-energized until a deliberate `bench reset`, exactly as the
    shipped fault path does (`AGENTS.md` section 5). A refusal that never applied
    anything - an effort or duration above the ceiling, a zero-effort or
    zero-duration command - is refused with a named reason and latches nothing:
    nothing was energized, so there is nothing to abort;
  - **bounded by construction**: every command carries its own duration and
    expires on its own, so a lost host, a dropped serial line, a frozen console or
    an operator mistake cannot leave effort applied;
  - entered through a **deliberate bench arm**, never a command that resumes
    anything after boot or reset;
  - refused while **anything outranks it**, which is `ev::benchApplyVerdict()`
    (`lib/evcore/src/MotorArbitration.h`, pure and host-tested): a latched fault, a
    violated control-tick order, an armed run, or a run under way. The test is
    deliberately *not* `!armed` - a latched fault clears arming and an order
    violation disarms, so `!armed` cannot tell a free motor from a faulted one.
    When the verdict refuses, the instrument is **aborted** (latched), not silently
    disarmed;
  - bounded by a **hard effort ceiling, a hard duration ceiling and a per-arm
    command budget** (the planned values are constants in `src/bench/BenchConsole.h`
    and H3 records the ones used);
  - **the pass's single writer** while it is driving: the bench takes ownership of
    the motor pins before the tick (`motorDriver::setMotorPinsOwned()`), so the pins
    see one intent instead of a sleep/wake toggle once per control period. The
    write happens **after** the control tick, and ownership is released on the next
    pass (or immediately if the tick ran out of order), so the tick's own
    de-energized command is the backstop if the bench layer stops running;
  - it does **not** replace the physical kill switch (the motor-supply
    disconnect), which remains the primary stop in every powered step.

  No protocol that uses B2 runs until the bounded expiration and the latched
  fault have themselves been demonstrated and recorded - that is H3's first step.
- **B3 - raw encoder tick readout.** `bench ticks`. Reports the raw signed tick
  count plus `counted_edges`/`ignored_updates`. The telemetry sample carries
  `pos_m` instead (which needs a scale nobody has measured). H2, H3, H4 and H5
  need these raw numbers.
- **B4 - raw input readout.** `bench inputs` (also `bench trigger`). Reports the
  raw trigger level (`trigger_low`), the driver fault line's raw level, and
  `batt_counts`/`curr_counts`. H1B needs it (active level and bounce).
- **The driver-pin readout (H0W Step 3).** `bench pins`. Reports the three driver
  inputs as the MCU itself holds them (`digitalRead()`), plus the duty the adapter
  last wrote, with `source=pin_read` saying which field is an observation and which
  is written intent - a PWM duty cannot be read back from a pin. Its `deenergized=`
  is the pure predicate that judges a command (`ev::drv8874InputsAreDeEnergized`)
  applied to those levels, so a pin held the wrong way reports `0` instead of
  reading as safe. It is the MCU's own view of the pins it drives: whether those
  levels de-energize the driver outputs is H1A's electrical measurement, and a pin
  driven low but held high at the header would still read as the MCU sees it.
- **The reset-cause readout (H0W Step 3).** `bench resetsrc [clear]`. Reports
  `R_SYSTEM->RSTSR0`/`RSTSR1` as the MCU reads them - the raw values plus the flags
  the vendor device header's masks name - with `source=register_read` saying so.
  `clear` writes zero to both (this family's clear-a-flag idiom) and the same line
  shows the result, so the write cannot fail silently. **The flags latch** across
  software resets, so reading without clearing cannot attribute a *new* reset; that
  is why the argument exists and why the protocol reads, clears, then reads. It says
  which reset happened, never whether the vehicle was safe when it did (H1A), and
  the decode was measured on this one board rather than read out of a datasheet.
- **The watchdog-register readout (H0W Steps 6 and 7).** `bench wdt`. Reports
  `WDTCR`'s TOPS/CKS/RPES/RPSS fields (vendor masks), `WDTSR`'s live `CNTVAL`
  down-counter with its two flags, and `pclkb_hz` from the FSP's own clock
  accessor. Together with B1's `wdt_realized_ms` - the library's computed period -
  this is how the **requested** timeout and the **realized** period are told apart:
  114 ms requested against **174 ms** realized (D30). It deliberately does not
  decode a period from those fields, because that mapping is hardware-manual
  knowledge rather than something a header in this build states.
- **The deliberate-hang hook (H0W Step 3).** `kDeliberateHangAfterPasses` in
  `src/bench/BenchMain.cpp`: a compile-time pass count after which the loop hangs
  inside a pass, and it stages one `BENCH,hang=1,...` line first. It is a constant
  and **not** a console verb, deliberately - no serial input can hang the board -
  and it lives in the bench tree, so the vehicle image cannot contain it. H0W
  Step 3 sets it, rebuilds, and observes the resulting watchdog reset. It must
  only ever be set with the motor supply off: B2's bounded expiry is evaluated by
  the same loop, so a hang suspends it too. **Note the unit:** a "pass" here is one
  loop *iteration* and the loop is free-running at ~46,000/s (D26), so `460000U`
  hangs about 10 s after the first pass, not 2.5 hours. The hook has now been used
  (D27/D28) and is back at its shipped `0U`.

Anything a protocol measures that the firmware cannot report is a defect to close
in code before the test, not something to approximate with a scale that is itself
unmeasured.

## Common safety preconditions

These apply to every powered protocol and come from `AGENTS.md` section 5. A
protocol lists only its *additional* hazards.

- A person is present, with the physical kill switch - the motor supply
  disconnect - within reach and tested before any powered step.
- Powered motion happens only with the drivetrain unloaded (wheels off the
  floor) until a protocol explicitly requires the vehicle on the surface, and
  then only with a clear runout and no one in the path.
- The bench supply is current-limited conservatively for every first test, and
  the limit is recorded.
- One behaviour at a time: one command, one direction, one gain, one variable.
- Eye protection and nothing loose near the drivetrain.
- No code path may resume a previous command after a reset; a reset must be
  re-armed deliberately.

## The record every session leaves

A session that is not recorded is not evidence. For every protocol, capture:

- the build identity - `VERSION,fw=...` from the interface, plus `git rev-parse
  HEAD` of the tree that was flashed, **and which image it was**: `BOOT,...` for
  the vehicle image, or `bench health`'s `image=bench` for a bench session. A
  capture that does not say which image produced it cannot be re-read later;
- date, operator, board and vehicle identity, supply voltage and current limit;
- the exact commands typed, and the exact responses, verbatim;
- the raw serial capture saved verbatim as a file. `data/raw/` is scratch and is
  gitignored, so any capture that backs a result is copied **unchanged** into
  `data/evidence/<protocol>-<session>/` and committed (see its README): the
  capture is the record, the generated report is not;
- hand measurements with their instrument and resolution;
- every deviation from the protocol, and every rejected run **with the reason**.

Pre-register, before the first trial, any acceptance or repeatability criterion a
protocol asks for, in a committed `data/prereg/<protocol>-<date>.md` (template in
its README). A criterion invented after seeing the data is not a criterion, and a
criterion that exists only in the operator's memory cannot be shown to have come
first.

**Repetition counts are derived, not chosen.** Where a protocol says "at least
five", the pre-registration records the smallest effect the series must detect
and the criterion it is judged against, and the count follows from those
(`AGENTS.md` section 28). "Enough runs for one outlier to be visible" is an
acceptable derivation; "five because the protocol said five" is not one.

## How a result is recorded

1. Raw observation or measurement, written down first.
2. `DEVELOPMENT_LOG.md` entry describing what was run and what was observed.
3. `VERIFIED_STATE.md` row set to VERIFIED with the evidence pointer and date -
   only for the rows the protocol names, and only if it passed.
4. Replacing a `Provisional` placeholder in `src/VehicleConfig.h` is one edit
   there plus the log entry, with `ValueSource::Measured` (the promotion steps in
   `VERIFIED_STATE.md`) **and the evidence pointer**: a promoted value has to pass
   `Sourced::citesEvidence()`, or the configuration-build check
   `MeasuredValuesCiteEvidence` reports false and the vehicle cannot be armed. The
   citation is the `VERIFIED_STATE.md` row and the capture under
   `data/evidence/`, not a comment.
5. `docs/hardware/motor-driver-drv8874.md` tables updated if the measured driver
   behaviour contradicts the plan. Fix the plan or the wiring - never add a sign
   somewhere else.
6. The matching entry in `docs/limitations.md` narrowed or deleted, so the
   register of what the project cannot yet claim keeps matching the record. A
   limitation that is still listed after its evidence exists is as misleading as
   a row promoted without evidence.

A report from `tools/telemetry/analyze.py` says what the firmware believed. It is
never evidence of vehicle behavior, and it is never pasted into
`VERIFIED_STATE.md`.

The hand measurements are the evidence. `analyze.py distribution` computes the
series statistics (mean, sample standard deviation, and the systematic-versus-
random residual against the recorded target) straight from the measurement CSV,
so the arithmetic is reproducible and every excluded run stays in the record with
its reason; the statistics are derived from the measurements, not evidence on
their own.

## Protocol format

Each protocol file has: prerequisites, setup, held constant, varied,
measurements and logs, repetitions, safe-stop conditions, acceptance/decision
rule, and what may be recorded if it passes - plus what to do if it fails or is
inconclusive.

## Coverage: every planned hardware fact

| `VERIFIED_STATE.md` item | Verified by |
|---|---|
| #1 wiring and pin assignments | H1A (continuity), H1B (input levels), H2 (encoder pins) |
| #2 motor driver modes / truth table | H1A |
| #3 motor polarity (sign of a positive command) | H3 |
| #4 encoder direction sign | H2 |
| #5 encoder signal quality | H2 hand-driven, H5 powered |
| #6 rollout (effective distance per tick) | H4 |
| #7 sensor scaling | H1B battery (and only if the fit criterion is met); current scaling is deferred to H3/H9 with a load |
| #8 control gains / tuning values | H6 |
| #9 braking behaviour and stopping distance | H7 |
| #10 wheel slip | H4 (hand-rolled vs powered), H7, H10 |
| #11 bottle interaction behaviour | H8 |
| #12 benefit of a ground / dead-wheel encoder | H4 (low plus cruise-near slip, deadband/coast separated) + H7 + H10: the measured slip distribution, with an explicit justified/not-justified verdict |
| on-target watchdog behaviour and chosen timeout | H0W - **passed for the bench load**, D28 (Step 8 owed under a loaded run) |
| host-drain behaviour of the serial link | H0 - **passed**, D25 |
| observed control cadence and lateness | H0 (D25), H0W (D26-D28) |

These rows were added when the corresponding protocol passed; the existing numbered
rows are never relabelled by these protocols, only promoted with their own recorded
evidence (`VERIFIED_STATE.md`).
