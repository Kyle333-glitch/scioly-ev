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

## Bench instrumentation that does not exist yet

Several protocols need the firmware to report something it currently cannot.
These are **software prerequisites, not built**; listing them here is the point
of pre-writing the protocols, so the gap is closed deliberately rather than
discovered on the bench. Each is bench-only and must be impossible to include in
a production image. The decided mechanism is a separate PlatformIO `bench`
environment: like `native`, it lives beside `nano_r4` and adds the bench sources,
so the production environment never compiles them. A build flag inside the
production translation units is not sufficient - the guarantee would then rest on
a macro being absent rather than on the build graph. None of these may become a
control input.

- **B1 - loop and watchdog health readout.** `WatchdogCheckpoint` already counts
  `passes()`, `refreshes()`, `withheldRefreshes()` and `lastRefreshMs()`, and the
  scheduler reports lateness and dropped deadlines, but no readout exposes them.
  Needed by H0 and H0W (cadence evidence and the watchdog timeout choice). It
  must not be a production path: the refresh stays in exactly one place.
- **B2 - bench-only diagnostic drive.** A bounded, time-limited way to command a
  conservative effort and a direction outside the run, for H3-H10. The operator
  interface deliberately has no such command (D14); this is a bench instrument,
  and the production image must not contain it. **Its safety model is part of
  what has to be built, not an afterthought**, because the first protocol that
  uses it (H3) is the first powered test and has no run to `disarm`:

  - B2 owns a **latched fault**: any abort - an effort or duration ceiling
    reached, an exceeded limit, or an encoder that stops changing while the
    motor is commanded to drive - de-energizes the motor *and stays
    de-energized* until a deliberate `reset`, exactly as the shipped fault path
    does (`AGENTS.md` section 5);
  - B2 is **bounded by construction**: the command expires on its own after a
    pre-registered short interval, so a lost host, a dropped serial line or an
    operator mistake cannot leave effort applied;
  - B2 is entered through a **deliberate bench arm**, never a command that
    resumes anything after boot or reset;
  - B2 does **not** replace the physical kill switch (the motor-supply
    disconnect), which remains the primary stop in every powered step.

  No protocol that uses B2 runs until the bounded expiration and the latched
  fault have themselves been demonstrated and recorded - that is H3's first step.
- **B3 - raw encoder tick readout.** The telemetry sample carries `pos_m` (which
  needs a scale that is not yet measured), not the raw tick count. H2, H3, H4 and
  H5 need raw signed ticks plus `countedEdges`/`ignoredUpdates`.
- **B4 - raw trigger level readout.** The telemetry sample carries
  `batt_counts`, `curr_counts` and `nfault_low`, but the start trigger's raw
  level is not reported anywhere. H1B needs it (active level and bounce).

Anything a protocol measures that the firmware cannot yet report is a defect to
close in code before the test, not something to approximate with a scale that is
itself unmeasured.

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
  HEAD` of the tree that was flashed;
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
   `VERIFIED_STATE.md`).
5. `docs/hardware/motor-driver-drv8874.md` tables updated if the measured driver
   behaviour contradicts the plan. Fix the plan or the wiring - never add a sign
   somewhere else.

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
| on-target watchdog behaviour and chosen timeout (**no row yet**) | H0W |
| host-drain behaviour of the serial link (**no row yet**) | H0 |
| observed control cadence and lateness (**no row yet**) | H0, H0W |

Items with no row yet are added only if the corresponding protocol passes; the
existing numbered rows are never relabelled by these protocols, only promoted
with their own recorded evidence.
