# P-H5 - Low-speed velocity trace collection

**Primary class:** P (physical behaviour check).
**Also contains:** S (the estimator's own verdicts) and E (conservative effort).
**Order:** after H2, H3 and H4.
**Answers:** what the commanded effort produces in measured velocity, how long
the estimator takes to report a usable velocity, how long it takes to claim zero
speed again after the command is removed, and what the estimator does at zero
command. This is the input to tuning (H6), not tuning itself.
**Bench instrumentation required:** B2 (bench-only bounded diagnostic drive:
`bench arm`, `bench drive <fwd|rev> <permille> <ms>`, `bench stop`, `bench reset`)
and B3 (`bench ticks`).

Keep the drivetrain **unloaded** for this protocol. That isolates the
effort-to-velocity relationship from traction and from the vehicle's mass, and it
is safer. The loaded relationship is a different measurement (H7, H10); record
that this one is unloaded wherever these numbers appear.

## Prerequisites

- H4 passed, so a tick has a measured distance and `pos_m` is meaningful.
- Telemetry on at the highest admissible rate, captured to a file.
- Kill switch and current limit set; wheels off the floor.

## Setup

For each command level, hold the command for a fixed dwell, then de-energize for
a fixed recovery interval before the next. Record both intervals; keeping them
constant is what makes the samples comparable.

## Held constant

Supply voltage, unloaded condition, dwell and recovery intervals, capture rate,
firmware image, motor temperature (let it return to ambient between runs).

## Varied

- constant command level, one at a time, across a low range that stays inside the
  current limit and the pre-registered effort ceiling for the bench;
- repetition order: interleave the levels rather than sweeping monotonically, so
  battery and thermal drift show up as scatter rather than as a trend;
- the estimator policy values `kEstimatorWindowMs` and `kEstimatorNoEdgeTimeoutMs`,
  one at a time, at one fixed command level: the point is to record what a
  candidate value does to the `vel_ok` windows and the steady-state lag, so the
  value chosen later is chosen from evidence. These are policy values, not
  measurements - nothing measured here becomes a `Measured` fact. They are
  **compile-time constants**: each candidate is a rebuild and a reflash, so record
  the flashed image identity (`VERSION,fw=` and `git rev-parse HEAD`) with each
  candidate's series, or the series is unattributable (C16).

## Measurements and logs

Per run, from the `EVS3` sample train (schema 3; the H0/H0W-era captures recorded
`EVS1`), record (or keep the raw capture and note
the fields): `t_ms`, `dt_ms`, `phase`, `cmd`, `armed`, `pos_m`, `pos_ok`,
`vel_mps`, `vel_ok`, `vel_cmd_mps`, `rem_m`, `motor`, `effort_permille`,
`batt_counts`, `curr_counts`, `nfault_low`, `fault`, `authz`, `check`, and the
drop counters.

Derive, and keep the derivation separate from the raw capture:

- the steady-state measured velocity at each level (velocity after settling);
- the settling time from command to a stable velocity;
- the zero-command baseline (creep or offset) and the interval where `vel_ok` is
  0 (warming up or no-edge timeout);
- the relation effort -> steady-state velocity, with its scatter.

Also derive, because otherwise `vel_mps` is the only opinion in the room and it
is the thing under test:

- an **independent velocity from the raw tick count** over the same steady
  interval: `delta_ticks / delta_time * kPlannedDistancePerEncoderTickM`, using the
  B3 counter and the H4 scale. It does not use `vel_mps` at all, so a difference
  between the two is what tells an estimator-window artefact from a real speed.
  Report the difference per level;
- for at least one level, a **stopwatch-timed set of wheel revolutions** (from the
  wheel mark) converted with H4's measured distance per revolution - a slower but
  fully independent check of the powered scale, and the one that does not depend on
  the tick count that `vel_mps` is also built from;
- the **usable-estimate recovery latency**: after the command is removed, the
  interval until `vel_ok=1` again with a near-zero value, and the interval the
  near-zero dwell occupies inside it. Record it per level. The firmware's
  *internal* explicit zero-speed claim (`zeroSpeedClaimed`, a policy statement
  about observed non-change) is **not a distinct wire flag**, so a capture cannot
  time the claim itself; what a bench capture can time is this recovery, and the
  claim's adequacy for a real stop is bounded physically in H7. A stop is only as
  trustworthy as this latency.

Record the raw capture filename with every derived number.

## Repetitions

At least five runs per level, interleaved. If a level's scatter exceeds the
pre-registered criterion, find the cause (temperature, battery state, mechanical
drag) and record it; do not average a drifting set into one number.

## Safe-stop (abort) conditions

Kill switch primed. Abort on runaway, an unexpected direction, a sustained
current-limit trip, heat, or a command that produces no response at all when the
driver was expected to be awake.

## Acceptance / decision rule

The trace is accepted as H6's input when the effort-to-velocity relation is
repeatable within the **pre-registered** spread, the `vel_ok` windows are
understood, and the estimator's `vel_mps` agrees with the independent tick-count
velocity within the pre-registered tolerance (a disagreement is a finding about
the estimator, not scatter to average away). **No gain, offset or expected
velocity is prescribed here** - this protocol measures the plant; H6 chooses
numbers. If the relation is not repeatable, that is the finding, and the answer is
to fix the cause rather than to tune against unstable data.

## If it passes

- No `VERIFIED_STATE.md` row is promoted directly. This is evidence that feeds H6
  and informs the #5 (powered signal quality) and estimator-window questions.
- Record the trace as a named artifact (raw capture plus derived table) in
  `DEVELOPMENT_LOG.md`, marked unloaded, including the independent tick-count
  velocity and the usable-estimate recovery latency (the internal
  `zeroSpeedClaimed` field is not on the wire; do not report a "claim" time the
  capture cannot show).
- If the estimator's `vel_ok` windows are longer than a policy value expects
  (`kEstimatorWindowMs`, `kEstimatorNoEdgeTimeoutMs`), record that as a finding,
  together with which candidate value produced which window; those are policy
  values, not measurements.
- Record the **unloaded** usable-estimate recovery latency as the number a loaded
  stop has to be compared against: the loaded latency is measured in H7, and a
  zero-speed gate is only trusted where that measurement exists. (The claim
  itself is internal - see the note above.)

## If it fails or is inconclusive

In order: the unloaded condition is not actually unloaded; the supply sags under
load (record `batt_counts`); the encoder counts poorly under power (compare with
H2 and check `ignoredUpdates`); a mechanical drag; then a control/estimator bug.
Do not begin tuning until the plant data repeats, and do not paper over a
`vel_mps`-versus-tick-count disagreement by picking whichever one looks smoother.
