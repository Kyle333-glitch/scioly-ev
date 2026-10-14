# P-H14 - Phase-14 calibration checklist (ordered, one experiment unlocks the next)

**Class:** P (physical calibration procedure).
**Purpose:** take the firmware from "every calibration value is a placeholder" to
"the production image can be armed", in the fewest short, safe experiments, with
each result establishing named constants and unlocking the next step.

**Status of every number in this file: NONE.** This is a procedure, not evidence.
Nothing here changes a `VERIFIED_STATE.md` row, and no value below may be written
into `src/VehicleConfig.h` as `Measured` until the step's raw data is recorded and
the change is logged (`VERIFIED_STATE.md`, "How to promote a fact").

**Rules that apply to every step**

- **Do not raise any ceiling to make a step pass.** The bench ceiling is
  `benchConfig::kMaxEffortPermille` = **150 permille**, with
  `kMaxCommandDurationMs` = **500 ms** and a per-arm budget of **8 commands**
  (`src/bench/BenchConsole.h`); `kPlannedMaxEffortPermille` (300 permille) is the
  *production* placeholder safety ceiling, **not** a bench ceiling. If a step
  needs more than the bench ceiling, stop and say so (C4/C7).
- One behavior at a time. Drivetrain unloaded (wheels off the floor) for
  steps 1-4; on the floor only from step 5, with a clear runout and a kill switch.
- **The execution order is the bootstrap section of `docs/bringup/README.md`**
  ("Calibration bootstrap order"): levels 0/1, then the conservative bootstrap
  that makes the run path armable, then the first run-path tests. This checklist
  is the per-step detail of that order; where the two disagree, the bootstrap
  order is what makes the gates pass without fabricating a value (C2).
- Repetition counts below are **floors**: the pre-registration records the
  smallest effect the series must detect and derives the count from it
  (`docs/bringup/README.md`); "five because the step said five" is not a
  derivation (C18).
- Record raw data, not conclusions: serial captures under `data/evidence/`, and
  the arithmetic you used. Keep rejected runs with their reason.
- The acceptance rule is a pre-registered pass/fail, written before the runs.
- After each step: set the constants, rebuild, and re-run `checks` - the
  `CHECK,all=0,...,first=<check>` line tells you which gate is still open.

**Images**

- `env:bench` - `pio run -e bench -t upload`. Bench-only instruments (B1-B4). It
  contains a command that applies effort outside a run; it is never the vehicle
  image.
- `env:nano_r4` - `pio run -e nano_r4 -t upload`. The vehicle image. Use it for
  steps 7+ once the calibration constants are in; every line it prints is
  production output.
- Every command below is typed at 115200 baud (`pio device monitor`).

---

## Step 0 - Continuity and the pin map (no power to the motor)

**Setup:** battery disconnected from the driver; Nano on USB only.
**Commands:** `version`, `checks`, `status`.
**Measure:** continuity from each Nano pin named in `src/PinMap.h` to the driver
header / encoder pin it is supposed to reach (encoder A→D2, encoder B→D4, PH→D8,
EN→D9, nSLEEP→D10, PMODE→GND, trigger→D12).
**Acceptance:** every role reaches the pin the map names, and no two roles share a
pin.
**Establishes:** `VERIFIED_STATE.md` #1 (wiring), the pin map.

## Step 1 - Driver de-energized state (H1A)

**Setup:** motor supply on, drivetrain unloaded, meter on PH / EN / nSLEEP.
**Image:** `env:bench`. **Commands:** `bench health`, `bench pins`, then the H1A
gate diagnostic if it is still built into the bench image.
**Measure:** the levels on PH, EN, nSLEEP at boot and after `bench pins`.
**Reps:** 5 boots (floor; derive the count in the pre-registration).
**Acceptance:** the same de-energized combination every boot; no powered output.
**Establishes:** #2 (driver truth table, de-energized direction).

## Step 2 - Command polarity (H1A/H3) - **unlocks everything below**

**Setup:** drivetrain unloaded, wheels free, conservative effort only.
**Image:** `env:bench`. **Commands:** `bench arm`, then `bench drive <fwd|rev>
<permille> <ms>` at the lowest value that moves the motor, one direction at a
time - the live grammar takes direction, permille and duration together, and B2
refuses a direction change until `bench reset` + a fresh `bench arm` (S14). Record
the exact triple per trial.
**Measure:** which physical direction the wheel turns for a positive command.
**Reps:** 5 each direction (floor; derive the count in the pre-registration).
**Acceptance:** one repeatable direction per sign, no stutter, no unexpected
direction.
**Calculation:** none - it is an observation.
**Establishes:** `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode`
(then `CommandPolarityAgrees` can pass).

## Step 3 - Encoder direction sign (H2)

**Setup:** motor supply off, drivetrain unloaded.
**Image:** `env:bench`. **Commands:** `bench ticks` (raw count readout) while
turning the wheel by hand in each direction.
**Measure:** raw tick count sign for hand-forward and hand-reverse travel, and
the ticks-per-revolution count.
**Reps:** 10 forward, 10 reverse, at least one full revolution each (floor; derive
the count in the pre-registration).
**Acceptance:** sign is consistent and opposite between directions; the
ticks-per-revolution count is **repeatable run-to-run within the pre-registered
spread** (P-H2's rule - the expected count is what this measurement establishes,
so comparing against a predicted number would be circular; C12).
**Establishes:** `kPlannedPositiveTicksDirection` (then `TickPolarityAgrees`), and
`VERIFIED_STATE.md` #4, #5.

## Step 4 - Rollout (distance per tick) (H4)

**Setup:** on the floor, straight line, no bottle.
**Image:** `env:bench` (raw tick readout B3; telemetry if it helps the record).
**Commands:** part A - push the vehicle by hand over a measured span with the
encoder counting (motor de-energized). Part B - bounded `bench drive` commands,
still between **ground marks**. B2 takes effort and duration only, never a
distance: the span is defined by the marks (C12).
**Measure:** physically measured travel between line crossings (tape, >= 2 m,
mm resolution) and the raw tick delta over the same mark-to-mark span.
**Reps:** 5 in each direction (floor; derive the count in the pre-registration).
**Calculation:** `distancePerEncoderTickM = travelM / ticks` per run, per part;
take the Part A mean and report the spread; subtract the deadband and coast parts
from Part B before reading its residual.
**Acceptance:** run-to-run spread within the pre-registered tolerance and both
directions inside the pre-registered band - the tape is the *input*, so "the mean
is within 1% of the tape" is not a criterion (C12). The promoted value is the
**Part A mean**; Part B never enters the scale - its residual is the #10 slip
record (C8).
**Establishes:** `kPlannedDistancePerEncoderTickM` (`CalibrationMeasured` can now
pass), #6, and the first #10 slip evidence.

## Step 5 - Minimum reliable cruise and velocity ceilings (H5)

**Setup:** on the floor, straight line, runout clear, telemetry on, one direction
per series.
**Image:** `env:bench` (bounded effort; the run path cannot arm yet).
**Commands:** `telemetry on`, `telemetry rate 50`, then a series of bounded bench
**efforts** (typed `bench drive <fwd|rev> <permille> <ms>` triples), from the
lowest to the highest admissible. This is H5's effort series: B2 takes effort, not
commanded speed, so the speeds below are *measured* steady-state velocities, and
"the vehicle tracked its command" can only mean "it travelled steadily at the
recorded effort" here (C12).
**Measure:** the effort triple, the measured steady-state speed (`vel_mps` with
`vel_ok`), the independent tick-count velocity, the tick delta and the travel
time; the lowest effort whose steady travel showed no stall or stop-start.
**Reps:** 5 per effort point (floor; derive the count in the pre-registration).
**Calculation:** the lowest steady speed with zero stalls is
`kPlannedMinimumCruiseMps`. Set the configured ceilings at or below the highest
steady speed the series actually recorded, reduced by the pre-registered margin,
and record them as **conservative bootstrap** values (evidence: this series) to
be confirmed once the run path tracks (P-H7 Part L) - a "highest speed the loop
can hold" claim needs the loop, and Step 7+ carries it (C12).
**Acceptance:** a clean monotone effort-to-speed series with the stall point
identified and the spread reported.
**Establishes:** `kPlannedMinimumCruiseMps`, `kPlannedCruiseMps`,
`kPlannedMaxForwardVelocityMps`, `kPlannedMaxReverseVelocityMps` (bootstrap
values; then `SafetyLimitsValid` and `ProfileLimitsValid`).

## Step 6 - Regulator gains (FF/P/I) (H6) - pointer

**Run `P-H6-ff-pi-tuning.md` (as amended, C1/C10).** Summary: the feed-forward is
measured from H5's open-loop effort-to-speed relation; P/I are a **conservative
candidate** derived from it under a pre-registered rule and recorded as a
Provisional derived candidate with that citation - never promoted as
`Measured` before a closed-loop test - because no bench command runs the
regulator; the closed-loop criterion and the windup characterization
(`kPlannedMaxIntegralPermille`, `kPlannedMaxIntegrationStepS`, the
pinned-effort duration after sign change) are executed in **P-H7 Part L**, and
the gains are promoted from those loaded records. Until that experiment
exists, the pair stays Provisional and everything armed downstream stays
blocked (P-H6 "Why the bootstrap cannot self-qualify"). P-H6 is authoritative
for the method; this checklist does not restate a weaker variant (C10).
**Establishes:** `kPlannedFeedForwardPermillePerMps`,
`kPlannedProportionalPermillePerMps`, `kPlannedIntegralPermillePerMpsSecond`
(feed-forward measured + Provisional candidate; promotion at P-H7 Part L)
(then `ControllerConfigValid`); #8.

## Step 7 - Acceleration, and braking per direction (H7 part 1) - pointer

**Run `P-H7-distance-braking-reversal.md` Part B. That protocol is authoritative
for the brake-offset measurement** (the command instant is the last non-zero
`vel_cmd_mps` tick, the speed is `vel_mps` at that tick, the residual is
ground-truth rest minus the planned endpoint; C9). The one-line formula this
checklist used to give is superseded: do not compute `brakeOffsetM` independently
here - two procedures would produce two irreconcilable numbers.
**Setup:** on the floor, straight line, runout >= 2 m past the stop point, one
direction per series.
**Measure (per P-H7 Part B):** entry speed, ground-truth distance after the
command instant, time to near zero, per direction.
**Reps:** 10 per direction and speed point (floor; derive the count in the
pre-registration).
**Acceptance:** the per-direction spread is reported, not just the mean; stops
completed without a fault; both offsets promoted from their own direction's
series.
**Establishes:** `kPlannedForwardBrakeOffsetM`, `kPlannedReverseBrakeOffsetM`,
`kPlannedAccelMps2`, `kPlannedDecelMps2`; #9.

## Step 8 - Near-zero verification (reversal gate) (H7 part 2)

**Setup:** as step 7; bounded bench stop trials through B2 for the physical
bootstrap, the real run path for the verification.
**Measure:** at several entry speeds, the speed and time at which the vehicle is
genuinely stopped (independent observation: the wheels are not turning), and what
the firmware reported (`vel_mps`, phase, `authz`). The physical settle quantities
are measurable off-path with B2 and a stopwatch; the firmware's own stop
verification runs only on the run path.
**Reps:** 10 (floor; derive the count in the pre-registration).
**Calculation:** the threshold is the largest measured speed that is still "no
travel" (below it the vehicle does not move a measurable distance in the dwell);
the dwell is long enough to cover the longest observed settle at the highest entry
speed. The bootstrap values come from those bounded bench stop trials through
B2; the first run-path series confirms or refines them.
**Acceptance:** the reversal never starts before the vehicle is physically stopped,
and never times out on a legal stop.
**Establishes:** `kPlannedReversalStopThresholdMps`, `kPlannedReversalStopDwellS`,
`kPlannedReversalTimeoutS` (then `ReversalConfigValid` and
`StopEvidenceSpansReversalStop`).

## Step 9 - Final stop and stop-evidence limits

**Setup:** as step 8, full stop at a target. The bootstrap values come from the
bounded bench stop trials through B2 and the arithmetic limit relationships; the
full-stop behaviour itself is confirmed on the run path.
**Measure:** the same as step 8 for the final stop, plus the age of the
window-derived speed fact the packet pairs with the claim (`composeStopEvidence`'s
`speedMaxAgeMs` against the dwell).
**Reps:** 10 (floor; derive the count in the pre-registration).
**Calculation:** the final-stop threshold from the measured stop; the evidence age
limit must span the dwell (`stopEvidenceLimitSpansDwell()` refuses one that does
not). The dwell itself is rule 8.p and is not tunable.
**Acceptance:** `FinalBrake` reaches `Done` with no `PhaseTimeout`, and the
`Complete Stop` is held for the whole rule interval.
**Establishes:** `kPlannedFinalStopTimeoutS` (promoted); the
`kStopEvidenceMaxAgeMs` relationship, adjusted only if the recorded dwell
requires it - it is a time limit checked against the dwell, never a
provenance-marked value; `StopEvidenceSpansFinalStop`; confirms the rule dwell.

## Step 10 - Phase bounds and the final approach (H7 part 3)

**Setup:** a full announced run, no bottle first. The plan's two dependencies
are the conservative bootstraps from `P-H7` Part B's braking series plus the
placement range plus the margin (see the README bootstrap section); **no promotion
touching `runExecutionDependencies()` before the S16 arming-gate check exists or
is explicitly declined** (the deadline sentence in Step 11).
**Measure:** the time each phase actually takes, and the largest legal excursion.
**Reps:** 5 per configuration (floor; derive the count in the pre-registration).
**Bootstrap (the first run cannot wait for its own measurement, C2).** The first
announced run cannot exist while its own timeouts are placeholders, so the initial
values are set conservatively from recorded bounded-trial evidence plus a
pre-registered margin, and promoted as **Measured bootstraps** citing that
evidence: each timeout = the longest bounded trial actually recorded for
that phase (B2 bounded-trial duration plus the pre-registered margin);
`kPlannedFinalApproachDistanceM` from the measured stopping distance at the
bootstrap ceiling speed; `kPlannedMaxRunDurationS` from the longest bounded trial
run plus the same margin. The run path is then armed and this step **refines each
value from the first run-path series** through the normal promotion procedure,
superseding the bootstrap. No gate is weakened, and no value is promoted without
the record it cites.

A conservative `Measured` bootstrap of this kind is honest only because the
quantity itself was observed: a worst-case duration was measured and the margin
runs in the safe direction, and the value stays flagged as untested on the run
path until the run-path series supersedes it. That is the line P-H6's
provenance contract draws: a computed controller-gain pair was never executed
anywhere, so no margin and no flag can make it `Measured` - gains are excluded
from bootstrapping, full stop (R3/M6). The first armed run additionally waits
for the gain bootstrap's own resolution (P-H6), so these timeout values enable
nothing by themselves.
**Calculation:** each timeout is the measured worst case plus the pre-registered
margin; the final-approach distance is the distance at which the run can still stop
inside the reversing leg's measured stopping distance; `kPlannedMaxRunDurationS` is
the whole-run bound, set from the measured worst-case full run plus the same margin
- the vehicle layer evaluates it every control tick **once it is `Measured`** and
faults the run as `RunDurationExceeded` past it (S10), while a `Provisional` limit
is inert and can never stop a run.
**Acceptance:** every phase exits on its evidence, and no phase timeout fires.
**Establishes:** `kPlannedApproachTimeoutS`, `kPlannedCaptureTimeoutS`,
`kPlannedPushTimeoutS`, `kPlannedReverseTimeoutS`, `kPlannedFinalApproachTimeoutS`,
`kPlannedFinalApproachDistanceM`, `kPlannedMaxRunDurationS` (then
`PhaseConfigValid`).

## Step 11 - Bottle interaction (H8) - the three evidence distances

**Setup:** official bottle placed per the tournament level, witness mark on the
bottle, independent observer or camera, kill switch, runout clear.
**Image:** `env:nano_r4` with everything above measured.
**Commands:** `config set level=... distance=... time=... bottle_line=...`,
`config check`, `announce`, `arm`, then the physical trigger. After the run:
`status` (and the captured log).
**Measure, per run, believed AND observed:**
- believed: `pos_m` at the tick the phase changed (the `PhaseChange=Capture` /
  `Push` event), and the position at which the run entered `ForwardDecel`. Those three
  are the run's *progression* completions: `Capture` begins at the earliest
  credible contact position (the window opening - **not** a contact observation),
  `Push` begins at the capture guarantee, and `ForwardDecel` at the forward
  execution target;
- observed: the bottle's start and end positions from the tape, whether the
  witness mark moved, and where contact and release were seen. The observed
  contact position and tick are what establish the nominal contact position;
  never derive it from the `Capture` transition.

**Reps:** enough runs per condition to decide the pre-registered success fraction,
at least three conditions (approach distance, phase speeds - the three strategy
ceilings - and push criterion).
**Calculation:**
- `kPlannedBottleContactLeadM` = `bottleStartM - pos_m` at the tick the observer
  saw contact at the witness mark (from the log, at the observed contact tick - not
  at the `Capture` transition, which is the window opening);
- `kPlannedBottleCaptureAdvanceM` = the advance beyond contact over which the
  bottle is observed to be carried (believed and observed must agree; a systematic
  disagreement is an estimator or mechanism problem, not a threshold to move);
- `kPlannedBottlePushMarginM` = the observed overshoot of the bottle past the
  bottle line, minus the announced margin the run already has;
- `kPlannedContactWindowToleranceM` = half the observed spread (max - min) of the
  witnessed contact position around the nominal contact position across the runs -
  the credible window the execution plan needs;
- `kPlannedForwardStopMarginBeyondBottleLineM` = how far beyond the Bottle Line
  the vehicle's *commanded* forward stop must lie: from the observed bottle
  overshoot together with the measured braking distance at the forward-stop speed
  (a braking-characterization run, not a bottle run);
- `kPlannedApproachCeilingMps` / `kPlannedCaptureCeilingMps` / `kPlannedPushCeilingMps`
  = the phase speed each bottle phase was shown to perform correctly at, from the
  runs recorded here. They are the run policy's three strategy ceilings: each can
  only slow its own phase (the target shaper takes the smallest of the cruise
  ceiling, the distance taper and the active phase ceiling), they are
  `Measured`-only, and while they are placeholders the whole run policy is refused
  - so no run may be armed until they too come from these measurements.
**Acceptance:** the pre-registered success fraction of runs finish with the bottle
past the line, judged from the *observed* evidence.
**Establishes:** the three bottle distances, the three bottle-phase strategy
ceilings and the execution plan's two own physical dependencies (the credible
contact-window tolerance and the forward-stop margin beyond the Bottle Line).
`ev::buildRunExecutionPlan()` cannot produce a plan without its five, so no run
can complete a forward phase until then; the three ceilings keep
`PhaseConfigValid` false until they are measured, so no run may be armed on
placeholders either. Only then `VERIFIED_STATE.md` #11.

**Promotion deadline (F3/S16).** **No promotion touching
`runExecutionDependencies()` may happen before the arming gate that requires a
usable execution plan exists.** That gate **landed in S16** (2026-10-08, D53;
Option D in `ARMING-PLAN-GATE-REVIEW.md`): a run announced with a refused plan has
`arm` refused with the named `ExecutionPlanNotEstablished` refusal before
`SafetyAuthority::arm()` is asked, so a refused plan can no longer be discovered
only at the trigger. Announce still reports the plan status and its forward stop
on its acknowledgement and event (`exec=`, `fwd_stop_m=`), and `plan` remains the
live view. The promotion this deadline covers is therefore unblocked; the per-trial
discipline is not - still confirm `plan` reads `Planned` before a bootstrap run's
trigger, because the refusal is *reported* by `arm`, not repaired by it.

## Step 12 - Plan verification (Target Time)

**Setup:** a marked course for one tournament level, the bottle in place,
telemetry on.
**Commands:** announce the same geometry with several announced Target Times (e.g.
10.0, 15.0, 20.0 s), `status` between runs.
**Measure:** the announced target time; the **official** Run Time (external
stopwatch or video, actuation to complete stop - the scored measurement, rule
8.o); the firmware's own believed Run Time for the same run, derived from the
sample's own timeline as `internal_run_time_s = dwell_start_ms - run_start_ms`
(actuation to the start of the verified final-stop dwell - it excludes the rule's
Complete Stop interval and no longer ends at the `Done` tick, so it is not the
official number); the diagnostic cruise/delay from the `OK,cmd=announce` line; and
the final position error.
**Reps:** 5 per announced time (floor; derive the count in the pre-registration).
**Calculation:** the realised run time minus the announced Target Time (mean and
spread); the same for the final position.
**Acceptance:** pre-registered: each announced time produces a distinct planned
cruise/delay and the realised times separate in the direction predicted by the
plan.
**Establishes:** confidence in `kPlannedCruiseMps`/accel/decel and the plan's
knot-freedom. Nothing new is promoted here; this step is what turns the timing
plan from arithmetic into a measured behavior.

**Enablement (S7):** running this step changes nothing by itself. The run's
deliberate delay is its execution plan's `initialDelayS`, and while
`vehicleConfig::kRunTimingModelValidated` is false the plan carries
`timingReady = false`/`initialDelayS = 0`, the run's cruise is the configured
strategy cruise, and the announcement gate does not consult the kinematic
estimate for a refusal. Only after this step's records exist (raw data preserved),
the accel/decel/cruise limits are promoted from `Provisional` to `Measured`
through the normal procedure, and the realised-Run-Time comparison is reviewed may
`kRunTimingModelValidated` be set true - as its own reviewed change, with the
evidence cited.

## Step 13 - Repeatability (H10)

**Setup:** the frozen competition configuration. **Pre-gate (C11): P-H0W Step 8
(loaded watchdog/dwell-gap re-validation) must be re-run and recorded first** -
freezing a competition build requires it.
**Commands:** the competition sequence, 20+ times (floor; derive the count in the
pre-registration).
**Measure:** distance error, time error, fault count, lateral deviation.
**Acceptance:** the pre-registered median/percentile error, with every outlier
kept and explained.
**Establishes:** the competition build's claimed accuracy (and nothing else).

---

## Which constants each step closes

| Step | Constants established |
|---|---|
| 0 | pin map (wiring fact) |
| 2 | `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode` |
| 3 | `kPlannedPositiveTicksDirection` |
| 4 | `kPlannedDistancePerEncoderTickM` (the Part A mean; Part B is the #10 slip record) |
| 5 | `kPlannedMinimumCruiseMps`, `kPlannedCruiseMps`, both velocity ceilings (bootstrap values; confirmed at P-H7 Part L) |
| 6 | `kPlannedFeedForwardPermillePerMps` (open-loop, H5), `kPlannedProportionalPermillePerMps`, `kPlannedIntegralPermillePerMpsSecond` (conservative bootstrap; refined at P-H7 Part L) |
| 7 | `kPlannedForwardBrakeOffsetM`, `kPlannedReverseBrakeOffsetM`, `kPlannedAccelMps2`, `kPlannedDecelMps2` (P-H7 Part B is authoritative, C9) |
| 8 | `kPlannedReversalStopThresholdMps`, `kPlannedReversalStopDwellS`, `kPlannedReversalTimeoutS` (B2 bootstrap; confirmed on the run path) |
| 9 | `kPlannedFinalStopTimeoutS` (bootstrap from bounded bench stop trials through B2; confirmed on the run path); the `kStopEvidenceMaxAgeMs` relationship spans the recorded dwell |
| 10 | the six phase timeouts, `kPlannedFinalApproachDistanceM`, `kPlannedMaxRunDurationS` (bootstrap first, then refined from the first run-path series) |
| 11 | `kPlannedBottleContactLeadM`, `kPlannedBottleCaptureAdvanceM`, `kPlannedBottlePushMarginM`, plus the five plan/policy dependencies: `kPlannedContactWindowToleranceM`, `kPlannedForwardStopMarginBeyondBottleLineM`, and the three strategy ceilings `kPlannedApproachCeilingMps`/`kPlannedCaptureCeilingMps`/`kPlannedPushCeilingMps` (bootstrap values superseded here; C2/F3) |
| 12-13 | nothing new: these verify the behavior the values produce |

**Before Step 13 (the freeze):** `P-H0W` Step 8 (loaded watchdog/dwell-gap
re-validation) must be re-run and recorded under the loaded run path - it is
scheduled in the README order table after H7 and is a hard pre-gate for freezing
a competition build (C11).

**Before any promotion touching `runExecutionDependencies()`:** the S16 arming
gate must exist or be explicitly declined (Step 11's deadline sentence;
`ARMING-PLAN-GATE-REVIEW.md`, Option D). It exists as of S16/D53.

`kPlannedMaxEffortPermille` is deliberately absent: it is a **safety ceiling**, not
a calibration value. Raise it only by a separate, deliberate decision - never to
make a step pass.
