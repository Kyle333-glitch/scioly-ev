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

The promotion carries the normal evidence, not just the arithmetic: the H5
series it came from (capture under `data/evidence/`), the `VERIFIED_STATE.md`
row, and the log entry - a `Measured` value without a citation fails
`MeasuredValuesCiteEvidence` and the vehicle cannot be armed on it
(`Sourced::citesEvidence()`). An uncited slope is a placeholder no matter how
carefully it was computed.

## Provenance contract for everything below (R3/M6)

Five different things, five different labels. The labels are the
`ValueSource` vocabulary (`lib/evcore/src/Units.h`), and the rule is that a
computation never becomes a measurement by acquiring a citation:

1. **Provisional placeholder.** Exists so the types are complete. Authorizes
   nothing. Needs no citation. This is what every gain below starts as, and
   what the P/I pair stays until the closed-loop experiment in P-H7 Part L
   (or an authorized equivalent) evaluates it.
2. **Analytically derived candidate.** Computed offline from measured inputs
   under a pre-registered rule, with the derivation arithmetic and the input
   series recorded alongside. Cited as a derivation (`ValueSource::Derived`
   exists for exactly such numbers), never relabelled `Measured`. The P/I
   bootstrap pair of part 2 lives here: evidence-backed as a computation,
   untested as a controller.
3. **Experimentally estimated parameter.** A quantity observed in a physical
   experiment that evaluated that configuration: the feed-forward slope from
   H5's open-loop regression (part 1), a timeout from the longest bounded
   trial actually recorded plus the pre-registered margin (P-H14 Step 10).
   Promotable to `Measured` with the trial record cited - but only for
   quantities with a defined safe direction (a bound), and only flagged as
   untested on the run path until the run path confirms them.
4. **Physically validated controller configuration.** A gain set that met the
   pre-registered closed-loop criterion (§Acceptance) where the regulator
   runs, with repetitions, spread and supply-voltage sensitivity recorded.
   This - and only this - is what promotes P/I to `Measured`.
5. **Physically observed vehicle performance.** What the vehicle did on the
   course (tape, stopwatch, marks), recorded independently of what the
   firmware believed. Never derived from a log alone.

What `Measured` means for a gain, then: the pair ran in closed loop against
the fixed profile, met the criterion at every step in every repetition at
both supply voltages, and the promotion cites the loaded records - the
`VERIFIED_STATE.md` row, the captures, the log entry. A cited formula, a
pre-registered safety margin, or an honest "adequacy untested" flag changes
nothing about that: none of them ran the pair.

What prohibits a promotion: a formula alone; a bootstrap never executed in
closed loop; a single trial; pooled battery states; synthetic or uncaptured
data; a missing capture; a changed configuration without re-validation. The
independent check is a second reviewer walking the
capture-derivation-promotion chain plus the holdout series (H10, and H7 Part
A holdout targets) that never enters tuning. Physical-test permission is
separately authorized per trial (human in the loop, kill switch, current
limit); this document grants none.

## Setup, part 2 - the P/I bootstrap (the bench cannot tune the loop)

The fixed profile cannot be run *through the regulator* on the bench, so part 2
establishes a **conservative candidate**, not a tuning result and not a
measurement:

1. From H5's recorded effort-to-velocity relation, derive a deliberately
   conservative initial pair under a **pre-registered bootstrap rule** (recorded
   before the derivation is applied): a small proportional fraction of the
   measured feed-forward slope, and integral action at or near zero. Record the
   derivation arithmetic and the raw H5 series it came from.
2. Record the pair as a **Provisional candidate with that derivation cited**
   (the offline candidate record - the trial-record `derived` field of
   `INTERACTIVE-CALIBRATION-DESIGN.md` section 5.2 - not a promotion): the
   values are evidence-backed as a computation, but their closed-loop adequacy
   is untested until the loaded runs of P-H7 Part L, which refine or confirm
   them. A value promoted without a named capture is not a bootstrap - it is
   the fabrication the promotion procedure exists to prevent - and a computed
   pair promoted **as `Measured` before any closed-loop test is the same
   fabrication with a citation attached**: no formula, however conservative
   and however honestly flagged, measures a gain pair or establishes its
   closed-loop adequacy. The previous revision of this section instructed
   exactly that promotion; it was wrong, and it is removed here (R3/M6).
3. The anti-windup values (`kPlannedMaxIntegralPermille`,
   `kPlannedMaxIntegrationStepS`) are policy bounds, recorded as policy, and
   bounded by part 3's characterization when it runs.

### Why the bootstrap cannot self-qualify, and what is blocked

The controller's configuration check is `Measured`-only
(`validateVelocityControllerConfig()`), and the run path that could supply
the missing closed-loop evidence requires arming, which requires that same
check. No bench command runs the regulator (C1 above), so the pair cannot be
evaluated off the run path with the instruments that exist today. Promoting
the pair as `Measured` to break that circle does not resolve it - it
relables the circle as evidence.

The honest state, until a closed-loop experiment exists, is therefore:

- the P/I pair stays `Provisional` (a cited derived candidate);
- `ControllerConfigValid` fails, and the production image stays unarmable -
  which is the gate working, not an obstacle to work around;
- P-H7 Part L, H8, and every armed run built on them are **blocked on the
  gain bootstrap**, alongside their other unmeasured dependencies (strategy
  ceilings, plan dependencies, timeouts).

Two candidate resolutions, both future work, neither executed here:

a. a bounded closed-loop bench capability - the bench-only speed target of
   `INTERACTIVE-CALIBRATION-DESIGN.md` section 5.4 option 2 - built as a
   reviewed escalation with the same abort/ownership rules, validating
   candidates unloaded before Part L refines them loaded; or
b. a reviewed provenance contract admitting a narrower claim than full
   `Measured` for an unloaded-validated pair, with the loaded Part L
   refinement still mandatory before competition tuning.

What does NOT resolve it: rewording the promotion, citing the formula more
thoroughly, or flagging the values as a bootstrap while marking them
`Measured`. Do not solve a bootstrap cycle by changing the meaning of
`Measured`.

Block F prerequisites missing for all of the above, in order: H1A/H1B (hand
and meter), H2 (hand-driven encoder), H3 Step 0 (B2 bounded-expiry and
latched-abort demonstration) and H3 polarity/smallest-effort, H4 scale, H5
effort-to-speed relation - plus the R2 invalid-feedback bound (S18R-M1)
before bounded powered running. No powered trial in this protocol may run
until those are recorded.

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
flashed image identity (`VERSION` with its `rev=`/`dirty=`/`variant=`/`cfg=`
fields, plus `git rev-parse HEAD`) with its series.

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
recorded; its promotion is a real (open-loop) measurement, with the H5
series, the `VERIFIED_STATE.md` row and the log entry cited. The P/I pair is
accepted as a **candidate** when it cites its H5 series and the pre-registered
derivation and is recorded as a Provisional derived candidate - unrefined,
unpromoted, and unarmable. A **candidate gain set** is accepted when it meets
the **pre-registered** closed-loop criterion at every step of the profile in
every repetition and at both supply voltages (or with a recorded
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
  H5's open-loop relation (a real measurement, cited), and the P/I **candidate**
  (derivation cited, Provisional) - recorded as a candidate until P-H7 Part L
  evaluates it in closed loop and promotes it from loaded records.
- Record explicitly that the candidate was set without a closed loop, that it
  authorizes nothing, and that the loaded verification is P-H7 Part L, H7 and
  H10; the gains may need revisiting there for reasons that are measured, not
  assumed. Until the closed-loop experiment exists, Part L and everything
  armed downstream of it stay blocked (see "Why the bootstrap cannot
  self-qualify" above).
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
