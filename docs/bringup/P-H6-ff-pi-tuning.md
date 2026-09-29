# P-H6 - Feed-forward and PI tuning

**Primary class:** P (physical behaviour check).
**Order:** after H5, whose recorded trace is the input.
**Answers:** the feed-forward effort per velocity (#8, first half), and a PI gain
set that meets a pre-registered closed-loop criterion on recorded data (#8,
second half).
**Bench instrumentation required:** B2 (`bench drive`) for the repeatable command
profile.

**No gain value is prescribed here, and none may be adopted from this document.**
The placeholder gains in `src/VehicleConfig.h` are not starting guesses to keep;
they exist so the types are complete. The numbers come from this protocol's data.

## Prerequisites

- H5 passed, with the unloaded effort-to-velocity trace recorded.
- A repeatable command profile (a fixed sequence of command levels with fixed
  dwells) that exercises the low speeds used in competition - defined before
  running, and identical for every candidate.
- Telemetry at the highest admissible rate, capture per run.
- Kill switch, current limit, wheels off the floor (unloaded tuning).

## Setup, part 1 - feed-forward

From the H5 steady-state relation, take the effort that produces each steady
velocity and set the feed-forward slope `kPlannedFeedForwardPermillePerMps` in
`src/VehicleConfig.h` from that measured relation, with `ValueSource::Measured`.
Record the raw pairs it was derived from. If the relation is not linear over the
range, say so and record the deviation - a single slope is then an approximation
with a stated valid range.

## Setup, part 2 - proportional and integral

Run the fixed profile with the feed-forward in place and the integral gain at
zero first, then vary **one gain at a time** - never two together. For each
candidate gain set, run the profile at least three times.

## Setup, part 3 - integral limiting (the windup candidate)

Windup is a *candidate cause* of the pinned effort part 2 will report, and the two
values that bound it are `kPlannedMaxIntegralPermille` (how much integral
authority is allowed) and `kPlannedMaxIntegrationStepS` (the largest step the
accumulator may take in one tick). Once part 2 has a proportional/integral
candidate, vary these **one at a time** - against the same profile, with a step
that deliberately saturates the effort - and record which combination shortens
the pinned-effort period after the error changes sign. If no change does, the
cause is elsewhere and that is the finding; it is not licence to add a feedback
sensor.

Like the estimator's window values, these are policy values: a candidate is
chosen from the recorded behaviour, not adopted because it appears in
`src/VehicleConfig.h`.

## Held constant

Supply voltage, unloaded condition, profile, capture rate, temperature, and the
firmware image except the one gain being changed.

## Varied

One gain per iteration, plus one integral-limiting value per iteration in part 3.
At the end, a small set of the best candidates is re-run at a second supply
voltage (a fresh pack versus a depleted one) so battery sensitivity is visible
rather than assumed.

## Measurements and logs

Per run, from the `EVS1` train: `vel_mps`, `vel_cmd_mps`, `vel_ok`, `motor`,
`effort_permille`, `dt_ms`, `batt_counts`, `nfault_low`, `authz`. Record the raw
capture.

Derive, per candidate: the tracking error (`vel_cmd_mps - vel_mps`), overshoot
and rise time at each step, the amplitude and period of any oscillation, the
fraction of time the effort sits at the ceiling, and the **pinned-effort duration
after the error changes sign** - the windup measurement, reported as a number so a
limiting change in part 3 can be credited or rejected by it. Also record `dt_ms`
so a gain change is not credited with an effect that is really a cadence change.

## Repetitions

At least three per candidate; a candidate is adopted only when its repetitions
agree within the pre-registered band. Re-run the adopted candidate at the second
supply voltage; if the response changes materially, record the sensitivity and
do not pretend one gain set is supply-independent.

## Safe-stop (abort) conditions

As H5, plus: abort if the effort reaches the ceiling for a sustained period (the
command has no more authority and the loop is no longer doing what it reports),
if the response diverges, or if the motor overheats. Cut the supply, then
de-energize.

## Acceptance / decision rule

A candidate gain set is accepted when it meets the **pre-registered** criterion
at every step of the profile in every repetition and at both supply voltages (or
with a recorded sensitivity), with no sustained oscillation and with the
pinned-effort duration recorded either way. On acceptance, set
`kPlannedProportionalPermillePerMps` and
`kPlannedIntegralPermillePerMpsSecond` (and keep the feed-forward from part 1) as
`Measured`, set the integral-limiting values from part 3's data (recording their
provenance as the policy values they are), and re-run the configuration checks so
the arming path can see the change. If no candidate meets the criterion, the finding is recorded and the
next step is to re-examine H5's data or the mechanism - not to loosen the
criterion after seeing the results.

## If it passes

- #8 (control gains / tuning values) promoted, with the raw captures, the
  criterion, and the supply-voltage sensitivity recorded.
- Record explicitly that this tuning was done **unloaded**; the loaded
  verification is H7 and H10, and the gains may need revisiting there for reasons
  that are measured, not assumed.
- Log in `DEVELOPMENT_LOG.md`.

## If it fails or is inconclusive

In order: the plant data from H5 was not repeatable (go back, do not tune against
it); the effort ceiling is binding (raise the mechanism, not the constant); a
cadence problem (`dt_ms` shows lateness or drops, so the loop is not the fixed
period the tuning assumed); a windup or clamping defect in the regulator; then
the regulator's structure. Do not add a second feedback sensor to fix a tuning
problem until this list is exhausted.
