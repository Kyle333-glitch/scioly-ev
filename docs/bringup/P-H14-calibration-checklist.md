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

- Do not raise the bench effort ceiling (`kPlannedMaxEffortPermille`, currently
  **300 permille**) to make a step easier. If a step needs more, stop and say so.
- One behavior at a time. Drivetrain unloaded (wheels off the floor) for
  steps 1-4; on the floor only from step 5, with a clear runout and a kill switch.
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
**Reps:** 5 boots.
**Acceptance:** the same de-energized combination every boot; no powered output.
**Establishes:** #2 (driver truth table, de-energized direction).

## Step 2 - Command polarity (H1A/H3) - **unlocks everything below**

**Setup:** drivetrain unloaded, wheels free, conservative effort only.
**Image:** `env:bench`. **Commands:** `bench drive <permille>` at the lowest value
that moves the motor, both signs, one direction at a time.
**Measure:** which physical direction the wheel turns for a positive command.
**Reps:** 5 each direction.
**Acceptance:** one repeatable direction per sign, no stutter, no unexpected
direction.
**Calculation:** none - it is an observation.
**Establishes:** `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode`
(then `CommandPolarityAgrees` can pass).

## Step 3 - Encoder direction sign (H2)

**Setup:** motor supply off, drivetrain unloaded.
**Image:** `env:bench`. **Commands:** `bench ticks` (raw count readout) while
turning the wheel by hand in each direction.
**Measure:** raw tick count sign for hand-forward and hand-reverse travel.
**Reps:** 10 forward, 10 reverse, at least one full revolution each.
**Acceptance:** sign is consistent and opposite between directions; no double
counts or missed counts (compare counted edges with revolutions x expected
counts/turn).
**Establishes:** `kPlannedPositiveTicksDirection` (then `TickPolarityAgrees`), and
`VERIFIED_STATE.md` #4, #5.

## Step 4 - Rollout (distance per tick) (H4)

**Setup:** on the floor, straight line, no bottle, loaded drivetrain.
**Image:** `env:bench` (datalog on: `telemetry on`).
**Commands:** drive a fixed distance at low effort; or push the vehicle by hand
over a measured distance with the encoder counting.
**Measure:** physically measured travel (tape, >= 2 m, mm resolution) and the tick
delta from the log.
**Reps:** 5 in each direction.
**Calculation:** `distancePerEncoderTickM = travelM / ticks` per run; take the
mean and report the spread.
**Acceptance:** spread within the pre-registered tolerance and the mean within 1%
of the tape measurement; both directions agree.
**Establishes:** `kPlannedDistancePerEncoderTickM` (`CalibrationMeasured` can now
pass), #6.

## Step 5 - Minimum reliable cruise and velocity ceilings (H5)

**Setup:** on the floor, straight line, runout clear, telemetry on, one direction
per series.
**Image:** `env:bench` first (bounded effort), `env:nano_r4` once step 4 is in.
**Commands:** `telemetry on`, `telemetry rate 50`, then a bounded drive at a series
of commanded speeds from the lowest to the highest admissible.
**Measure:** the commanded speed, the measured speed (`vel_mps`), the tick delta
and the travel time; the lowest commanded speed that produced steady travel (no
stall, no stop-start).
**Reps:** 5 per speed point.
**Calculation:** the lowest speed with zero stalls is `kPlannedMinimumCruiseMps`.
The highest speed at which the vehicle still tracked its command without
saturation is the ceiling; keep the configured ceilings at or below it.
**Acceptance:** a clean monotone speed series with the stall point identified.
**Establishes:** `kPlannedMinimumCruiseMps`, `kPlannedCruiseMps`,
`kPlannedMaxForwardVelocityMps`, `kPlannedMaxReverseVelocityMps` (then
`SafetyLimitsValid` and `ProfileLimitsValid`).

## Step 6 - Regulator gains (FF/P/I) (H6)

**Setup:** as step 5, several speed steps up and down.
**Image:** `env:bench`/`env:nano_r4`.
**Measure:** the per-step response: rise time, overshoot, steady-state error,
effort (`effort_permille`), saturation flags.
**Reps:** 5 per step.
**Calculation:** feed-forward from the effort that holds a steady speed; P from the
error-vs-effort relationship; I small enough not to oscillate. Keep
`kPlannedMaxIntegralPermille` and `kPlannedMaxIntegrationStepS` as the anti-windup
bounds they are.
**Acceptance:** each step settles inside the pre-registered band without sustained
oscillation.
**Establishes:** `kPlannedFeedForwardPermillePerMps`,
`kPlannedProportionalPermillePerMps`, `kPlannedIntegralPermillePerMpsSecond`
(then `ControllerConfigValid`); #8.

## Step 7 - Acceleration, and braking per direction (H7 part 1)

**Setup:** on the floor, straight line, runout >= 2 m past the stop point, one
direction per series.
**Measure:** for each commanded deceleration/stop: entry speed, distance travelled
after the stop command, time to near zero.
**Reps:** 10 per direction and speed point.
**Calculation:** `brakeOffsetM = mean(coast + brake distance after the stop
command)` per direction; acceleration limit from the measured ramp-up.
**Acceptance:** the per-direction spread is reported, not just the mean; stops
completed without a fault.
**Establishes:** `kPlannedForwardBrakeOffsetM`, `kPlannedReverseBrakeOffsetM`,
`kPlannedAccelMps2`, `kPlannedDecelMps2`; #9.

## Step 8 - Near-zero verification (reversal gate) (H7 part 2)

**Setup:** as step 7.
**Measure:** at several entry speeds, the speed and time at which the vehicle is
genuinely stopped (independent observation: the wheels are not turning), and what
the firmware reported (`vel_mps`, phase, `authz`).
**Reps:** 10.
**Calculation:** the threshold is the largest measured speed that is still "no
travel" (below it the vehicle does not move a measurable distance in the dwell);
the dwell is long enough to cover the longest observed settle at the highest entry
speed.
**Acceptance:** the reversal never starts before the vehicle is physically stopped,
and never times out on a legal stop.
**Establishes:** `kPlannedReversalStopThresholdMps`, `kPlannedReversalStopDwellS`,
`kPlannedReversalTimeoutS` (then `ReversalConfigValid` and
`StopEvidenceSpansReversalStop`).

## Step 9 - Final stop and stop-evidence limits

**Setup:** as step 8, full stop at a target.
**Measure:** the same as step 8 for the final stop, plus the age of the
window-derived speed fact the packet pairs with the claim (`composeStopEvidence`'s
`speedMaxAgeMs` against the dwell).
**Reps:** 10.
**Calculation:** the final-stop threshold from the measured stop; the evidence age
limit must span the dwell (`stopEvidenceLimitSpansDwell()` refuses one that does
not). The dwell itself is rule 8.p and is not tunable.
**Acceptance:** `FinalBrake` reaches `Done` with no `PhaseTimeout`, and the
`Complete Stop` is held for the whole rule interval.
**Establishes:** `kStopEvidenceMaxAgeMs`, `kPlannedFinalStopTimeoutS`,
`StopEvidenceSpansFinalStop`; confirms the rule dwell.

## Step 10 - Phase bounds and the final approach (H7 part 3)

**Setup:** a full announced run, no bottle first.
**Measure:** the time each phase actually takes, and the largest legal excursion.
**Reps:** 5 per configuration.
**Calculation:** each timeout is the measured worst case plus the pre-registered
margin; the final-approach distance is the distance at which the run can still stop
inside the reversing leg's measured stopping distance.
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
- believed: `pos_m` at the tick the phase changed (`EVE2` `PhaseChange=Capture` /
  `Push`), and the position at which the run entered `ForwardDecel`;
- observed: the bottle's start and end positions from the tape, whether the
  witness mark moved, and where contact and release were seen.

**Reps:** enough runs per condition to decide the pre-registered success fraction,
at least three conditions (approach distance, approach speed, push criterion).
**Calculation:**
- `kPlannedBottleContactLeadM` = `bottleStartM - pos_m(contact)` from the runs
  where contact was observed at the witness mark;
- `kPlannedBottleCaptureAdvanceM` = the advance beyond contact over which the
  bottle is observed to be carried (believed and observed must agree; a systematic
  disagreement is an estimator or mechanism problem, not a threshold to move);
- `kPlannedBottlePushMarginM` = the observed overshoot of the bottle past the
  bottle line, minus the announced margin the run already has.
**Acceptance:** the pre-registered success fraction of runs finish with the bottle
past the line, judged from the *observed* evidence.
**Establishes:** the three bottle distances (then `PhaseConfigValid`'s bottle
clause), and only then `VERIFIED_STATE.md` #11.

## Step 12 - Plan verification (Target Time)

**Setup:** a marked course for one tournament level, the bottle in place,
telemetry on.
**Commands:** announce the same geometry with several announced Target Times (e.g.
10.0, 15.0, 20.0 s), `status` between runs.
**Measure:** the announced target time, the realised Run Time (from the `Done`
tick or an external stopwatch), the planned cruise and delay from the
`OK,cmd=announce` line, and the final position error.
**Reps:** 5 per announced time.
**Calculation:** the realised run time minus the announced Target Time (mean and
spread); the same for the final position.
**Acceptance:** pre-registered: each announced time produces a distinct planned
cruise/delay and the realised times separate in the direction predicted by the
plan.
**Establishes:** confidence in `kPlannedCruiseMps`/accel/decel and the plan's
knot-freedom. Nothing new is promoted here; this step is what turns the timing
plan from arithmetic into a measured behavior.

## Step 13 - Repeatability (H10)

**Setup:** the frozen competition configuration.
**Commands:** the competition sequence, 20+ times.
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
| 4 | `kPlannedDistancePerEncoderTickM` |
| 5 | `kPlannedMinimumCruiseMps`, `kPlannedCruiseMps`, both velocity ceilings |
| 6 | `kPlannedFeedForwardPermillePerMps`, `kPlannedProportionalPermillePerMps`, `kPlannedIntegralPermillePerMpsSecond` |
| 7 | `kPlannedForwardBrakeOffsetM`, `kPlannedReverseBrakeOffsetM`, `kPlannedAccelMps2`, `kPlannedDecelMps2` |
| 8 | `kPlannedReversalStopThresholdMps`, `kPlannedReversalStopDwellS`, `kPlannedReversalTimeoutS` |
| 9 | `kStopEvidenceMaxAgeMs`, `kPlannedFinalStopTimeoutS` |
| 10 | the six phase timeouts, `kPlannedFinalApproachDistanceM`, `kPlannedMaxRunDurationS` |
| 11 | `kPlannedBottleContactLeadM`, `kPlannedBottleCaptureAdvanceM`, `kPlannedBottlePushMarginM` |
| 12-13 | nothing new: these verify the behavior the values produce |

`kPlannedMaxEffortPermille` is deliberately absent: it is a **safety ceiling**, not
a calibration value. Raise it only by a separate, deliberate decision - never to
make a step pass.
