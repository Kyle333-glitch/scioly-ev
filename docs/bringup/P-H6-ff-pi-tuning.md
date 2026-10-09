# P-H6 - Feed-forward and PI tuning

**Primary class:** P (physical behaviour check).
**Order:** after H5, whose recorded trace is the input.
**Answers:** the feed-forward effort per velocity (#8, first half), from H5's
**open-loop** effort-to-velocity relation; and a **conservative P/I bootstrap**
(#8, second half) whose closed-loop adequacy is measured where the regulator
actually runs - the loaded run path, P-H7 Part L. The bench cannot close the
loop: `bench drive` applies typed effort through the bench adapter and never
invokes `VelocityController::update()`, so no gain can change a bench command's
response.
**Bench instrumentation required:** B2 (`bench drive`) for the open-loop effort
trials; the real run path (P-H7 Part L) for the closed-loop series.

**No gain value is prescribed here, and none may be adopted from this document.**
The placeholder gains in `src/VehicleConfig.h` are not starting guesses to keep;
they exist so the types are complete. The numbers come from this protocol's data.

## What the bench can and cannot measure (C1)

- **Open-loop plant characterization is available.** `bench drive <fwd|rev>
  <permille> <ms>` applies a *typed effort* (the triple you type) through the
  bench adapter `ev::BenchDrive`, with its own bounded duration; the `BENCH`
  replies plus the typed triple and the capture timestamps are the applied-command
  record.
- **The production telemetry sample does not describe bench actuation.** `EVS3`'s
  `motor`/`effort_permille` fields report the *production requested* effort -
  `Disabled`/`0` while the bench owns the motor. Never read them as the applied
  bench command and never correlate a measured rotation against them. The record
  must state the active owner (bench, via B2) and the applied triple alongside any
  production sample fields it quotes.
- **The closed loop is not available on the bench.** No bench command runs the
  controller, so P/I and the integral limits cannot be characterized here. The
  honest split is: part 1 below measures the feed-forward from H5's open-loop
  data; part 2 sets a conservative P/I **bootstrap**; the closed-loop criterion is
  met where the loop runs - **P-H7 Part L** on the loaded run path. If the loaded
  refinement cannot meet its criterion, the escalation is a bounded closed-loop
  bench capability designed as the smallest instrumentation needed to execute
  this experiment safely (`INTERACTIVE-CALIBRATION-DESIGN.md` section 5.4,
  option 2) - never a placeholder promotion or a gate bypass.

## Prerequisites

- H5 passed, with the unloaded effort-to-velocity trace recorded.
- A repeatable command profile (a fixed sequence of command levels with fixed
  dwells) that exercises the low speeds used in competition - defined before
  running, and identical for every candidate. It is executed through the run path
  in P-H7 Part L, where the regulator is in the loop.
- Telemetry at the highest admissible rate, capture per run.
- Kill switch, current limit, wheels off the floor (unloaded tuning).

## Setup, part 1 - feed-forward

From the H5 steady-state relation, take the effort that produces each steady
velocity and set the feed-forward slope `kPlannedFeedForwardPermillePerMps` in
`src/VehicleConfig.h` from that measured relation, with `ValueSource::Measured`.
This is open-loop data and it is valid for a feed-forward. Record the raw pairs
it was derived from. If the relation is not linear over the range, say so and
record the deviation - a single slope is then an approximation with a stated
valid range.

## Setup, part 2 - the P/I bootstrap (the bench cannot tune the loop)

The fixed profile cannot be run *through the regulator* on the bench, so part 2
establishes a **conservative bootstrap**, not a tuning result:

1. From H5's recorded effort-to-velocity relation, derive a deliberately
   conservative initial pair under a **pre-registered bootstrap rule** (recorded
   before the derivation is applied): a small proportional fraction of the
   measured feed-forward slope, and integral action at or near zero. Record the
   derivation arithmetic and the raw H5 series it came from.
2. Promote the pair as `Measured` **with that citation**, and record explicitly
   that this is a **bootstrap**: the values are evidence-backed, but their
   closed-loop adequacy is untested until the loaded runs of P-H7 Part L, which
   refine or confirm them. A value promoted without a named capture is not a
   bootstrap - it is the fabrication the promotion procedure exists to prevent.
3. The anti-windup values (`kPlannedMaxIntegralPermille`,
   `kPlannedMaxIntegrationStepS`) are policy bounds, recorded as policy, and
   bounded by part 3's characterization when it runs.

## Setup, part 3 - integral limiting (the windup candidate) - executed in P-H7 Part L

Windup is a *candidate cause* of a pinned effort, and the two values that bound it
are `kPlannedMaxIntegralPermille` (how much integral authority is allowed) and
`kPlannedMaxIntegrationStepS` (the largest step the accumulator may take in one
tick). These cannot be characterized on the bench either (no loop). This section
remains the **authoritative definition** of the windup measurement: with a
closed-loop profile (in P-H7 Part L), a step deliberately saturates the effort,
and the **pinned-effort duration after the error changes sign** is reported as a
number. In the loaded series, vary the two limiting values **one at a time** -
against the same profile - and record which combination shortens that duration.
If no change does, the cause is elsewhere and that is the finding; it is not
licence to add a feedback sensor.

Like the estimator's window values, these are policy values: a candidate is
chosen from the recorded behaviour, not adopted because it appears in
`src/VehicleConfig.h`.

## Held constant

Supply voltage, load condition (unloaded for the bench parts; loaded for the
P-H7 Part L series), profile, capture rate, temperature, and the firmware image
except the one gain being changed - each candidate is a rebuild, so record the
flashed image identity (`VERSION,fw=`, `git rev-parse HEAD`) with its series.

## Varied

One gain per iteration, plus one integral-limiting value per iteration in part 3.
At the end, a small set of the best candidates is re-run at a second supply
voltage (a fresh pack versus a depleted one) so battery sensitivity is visible
rather than assumed.

## Measurements and logs

Per bench run, record the typed bench triple and its timestamps with the capture
(`bench drive <fwd|rev> <permille> <ms>`; `BENCH` replies), and note that `EVS3`'s
`motor`/`effort_permille` describe the production requested effort, not the bench
command. For any part-L run through the real path, record from the `EVS3` train
(schema 3): `vel_mps`, `vel_cmd_mps`, `vel_ok`, `motor`, `effort_permille`,
`dt_ms`, `batt_counts`, `nfault_low`, `authz`. Record the raw capture for both.

Derive, per candidate: the tracking error (`vel_cmd_mps - vel_mps`), overshoot
and rise time at each step, the amplitude and period of any oscillation, the
fraction of time the effort sits at the ceiling, and the **pinned-effort duration
after the error changes sign** - the windup measurement, reported as a number so a
limiting change in part 3 can be credited or rejected by it. Also record `dt_ms`
so a gain change is not credited with an effect that is really a cadence change.

## Repetitions

For the P/I series (executed in P-H7 Part L): at least three per candidate; a
candidate is adopted only when its repetitions agree within the pre-registered
band. Re-run the adopted candidate at the second supply voltage; if the response
changes materially, record the sensitivity and do not pretend one gain set is
supply-independent. The counts are floors derived in the pre-registration
(`docs/bringup/README.md`).

## Safe-stop (abort) conditions

As H5, plus: abort if the effort reaches the ceiling for a sustained period (the
command has no more authority and the loop is no longer doing what it reports),
if the response diverges, or if the motor overheats. Cut the supply, then
de-energize.

## Acceptance / decision rule

Part 1 is accepted when the feed-forward relation repeats and the slope is
recorded; its promotion is a real (open-loop) measurement. The P/I pair is
accepted as a **bootstrap** when it cites its H5 series and the pre-registered
derivation and is recorded as unrefined. A **candidate gain set** is accepted
when it meets the **pre-registered** closed-loop criterion at every step of the
profile in every repetition and at both supply voltages (or with a recorded
sensitivity), with no sustained oscillation and with the pinned-effort duration
recorded either way - and that acceptance happens in **P-H7 Part L**, where the
regulator is in the loop. On acceptance there, set
`kPlannedProportionalPermillePerMps` and
`kPlannedIntegralPermillePerMpsSecond` (and keep the feed-forward from part 1) as
`Measured` from the loaded records, set the integral-limiting values from part 3's
data (recording their provenance as the policy values they are), and re-run the
configuration checks so the arming path can see the change. If no candidate meets
the criterion, the finding is recorded and the next step is to re-examine H5's
data or the mechanism - not to loosen the criterion after seeing the results.

## If it passes

- #8 (control gains / tuning values) promoted in two parts: the feed-forward from
  H5's open-loop relation, and the P/I **bootstrap** (citation above), recorded
  as a bootstrap until P-H7 Part L refines it from loaded closed-loop records.
- Record explicitly that the bootstrap was set without a closed loop, and that
  the loaded verification is P-H7 Part L, H7 and H10; the gains may need
  revisiting there for reasons that are measured, not assumed.
- Log in `DEVELOPMENT_LOG.md`.

## If it fails or is inconclusive

In order: a closed-loop claim was made from a bench command (the bench cannot run
the regulator - re-run the trial through the run path in P-H7 Part L); the plant
data from H5 was not repeatable (go back, do not tune against it); the effort
ceiling is binding (raise the mechanism, not the constant); a cadence problem
(`dt_ms` shows lateness or drops, so the loop is not the fixed period the tuning
assumed); a windup or clamping defect in the regulator; then the regulator's
structure. Do not add a second feedback sensor to fix a tuning problem until
this list is exhausted.
