# P-H7 - Distance, braking and reversal

**Primary class:** P (physical behaviour check).
**Also contains:** E (runout, kill switch, `disarm` abort) and S (the phase
timeline).
**Order:** after H4 and H6. This is the first protocol with the vehicle loaded,
on the surface, travelling under the real run path.
**Answers:** what the stopping distance is as a function of speed (#9); how well
the vehicle stops at a target distance; whether the reversal sequence actually
stops before reversing; and whether the unloaded H6 gains still track once the
vehicle is carrying its own mass.
**Bench instrumentation required:** B2 for the bench characterization in parts
A0, B and L; the biddable run path for part A and the end-to-end accuracy check.

## Prerequisites

- H4 passed (measured rollout) and H6 passed (tuned, unloaded). If H6's gains
  were tuned unloaded, expect the loaded response to differ - measure it (part
  L), do not assume it.
- A straight course with a marked start, target point and a **clear runout**
  beyond the target; no person in the path.
- Tape and stopwatch, or a measured course with position marks; record the
  instrument and resolution. Pre-place tape marks along the runout (every 0.5 m
  is a reasonable default) so a physical position at an instant can be bracketed
  rather than estimated.
- Kill switch within reach of a second person or positioned so the operator can
  reach it while watching; `disarm` known to end a run.
- A multimeter for the pack reading, and the measurement CSV prepared once from
  the tool's own template (see `tools/telemetry/README.md`): `pack_voltage_v`,
  `recharged`, `accepted`, `exclusion_reason`, `target_distance_m` and
  `tournament_level` are part of this protocol's record, not extras.
- (Pre-register) in `data/prereg/`, before the first trial: the accuracy and
  repeatability criteria, the target set, the speeds used in part B and part C,
  and the exclusion rule.

## Setup

Mark the course: start point, target point, and a clear runout beyond it. Tape
marks along the runout; tape and stopwatch on hand; telemetry captured for every
run; kill switch primed; no person in the path. Confirm the runout is comfortably
longer than the longest stopping distance expected from part B, and re-mark it if
it is not.

## Order of work (this order matters)

The arming path refuses while any calibration value is provisional, and the brake
offsets are calibration. That is a bootstrap problem, and the honest way through
it is not to pretend a distance-targeted run exists before the numbers do:

1. **Part A0 - bootstrap scale, open-loop.** B2 applies a bounded conservative
   effort for a bounded time. There is **no distance target**, because a distance
   target is exactly what is not calibrated yet and the run path will not arm
   until it is. The scale check compares ground-truth travel against believed
   travel, which needs no offset.
2. **Part B - braking offsets.** Still with B2, off the run path, against
   ground-truth marks.
3. **Write the measured values** into `src/VehicleConfig.h` with
   `ValueSource::Measured`, including only the offsets the firmware actually
   consumes, and re-run the configuration checks.
4. **Part L - loaded tracking re-check**, then, only now, a run may be announced
   and armed.
5. **Part A - distance-targeted accuracy** through the real run path; then
   **part C - reversal**; then the end-to-end accuracy check.

Record this bootstrap explicitly. A number measured off the run path and then used
by the run path must be identified as such, and the distance-targeted accuracy
result is not reported as if it had been available from the start.

## Part A0 - bootstrap scale, open-loop (before the offsets exist)

Through B2, apply a bounded conservative effort for a bounded duration and let the
vehicle roll over a measured span. Record, per trial:

- the measured ground travel between two marks (tape), with the instrument's
  resolution;
- the raw tick delta over that same span (B3), and the believed travel from those
  ticks (`delta_ticks x kPlannedDistancePerEncoderTickM`);
- the direction, the command level, the supply reading, and the run time.

Use **at least three spans**, not one, so a scale error (which grows with
distance) is separable from a zero error (which does not). This is the check that
says whether H4's scale survives being applied under power and load.

## Part A - distance accuracy at low speed (after the offsets exist)

Through the real run path, command travel to a measured target and measure where
the vehicle actually comes to rest, from the start line. Do this at **at least
three targets** spread across the rules' target range (rule 8.h), not only the
event target - one target cannot separate a scale error from a zero error.

Record per trial: the commanded target, the believed position at the end of the
commanded motion (`pos_m`), the measured resting position, the direction, the run
time, and any lateral drift. Keep the A0 and A physical measurements separate:
A0 says whether the scale is right, A says whether the whole run path lands where
it is sent.

## Part B - braking distance vs speed

At each of at least three recorded speeds (the lowest speed the vehicle will hold,
a mid speed, and the planned cruise, staying inside the current limit), command a
stop and measure the distance travelled after the stop command, from a
**ground-truth mark** to the resting position. Repeat per speed.

### What "after the stop command" means

There is no discrete stop command in the motion profile: the distance taper drives
the target velocity to zero on its own before the endpoint
(`lib/evcore/src/MotionProfile.h`). Left undefined, the number is unreproducible,
so the instant is defined here, not assumed:

- the **command instant** is the last tick whose commanded velocity is non-zero
  (`vel_cmd_mps` != 0) before the vehicle comes to rest;
- the **speed the offset is set for** is the measured `vel_mps` at that tick,
  with `vel_ok`. An offset with no stated speed is meaningless, because the whole
  question is how the stopping distance varies with speed;
- the **residual** is the measured resting position minus the profile's planned
  endpoint; it is the quantity an offset compensates, and it is reported with the
  speed it was measured at;
- both the believed position and the ground-truth position at the command instant
  are recorded, so the residual can be computed from each and the difference
  between them is itself a finding.

### Taking the ground-truth measurement

The believed position is coupled to H4's scale, so a braking number expressed
against it cannot tell an offset error from a scale error. Take the physical
measurement independently:

1. a second person watches the vehicle against the pre-placed tape marks and
   records which mark the vehicle is nearest when the forward motion ends (the
   last non-zero command tick, visible on the capture);
2. tape from that mark to where the vehicle came to rest;
3. record the mark spacing as the measurement's resolution.

If a second person is not available, film the run against the marks and read the
instant from the video against the capture's timestamps; record that the reading
is from video and its frame rate.

### Setting the offsets

Set `kPlannedForwardBrakeOffsetM` with `ValueSource::Measured` from the measured
data. **`kPlannedReverseBrakeOffsetM` currently has no consumer in the firmware:**
`profileLimitsForOneDirection()` passes the forward offset to both legs, so
changing the reverse value changes the arming checks, not the motion. Record the
reverse measurement if it differs, and record "reverse offset measured but not yet
consumed" as a **pending software change** - do not report the reverse leg as
calibrated until the value is actually read by the profile.

If the distance depends materially on speed, say so and record the relationship
and the range over which a single constant offset is acceptable. The design
currently carries one constant per direction, so a velocity-dependent result is a
recorded limitation, not something to average away.

## Part L - loaded tracking re-check (the gains were tuned unloaded)

H6 tuned the feed-forward and PI with the drivetrain unloaded, and the loaded
plant is different. Before any accuracy or reversal result is used to judge the
vehicle, run H6's fixed command profile once with the vehicle loaded (B2, clear
runout) and record the tracking error, the fraction of time the effort sits at the
ceiling, and the pinned-effort duration.

If the loaded response degrades materially, that is the finding: retune loaded and
record which gains the loaded tuning produced, rather than assuming the unloaded
gains transfer. The loaded numbers are the ones the competition run uses.

## Part C - reversal sequence

Run the stop-and-reverse sequence through the real path and verify, from the
`EVE1`/`EVS1` timeline **and from watching the vehicle**, that:

- forward travel ramps down before any reverse torque is applied;
- there is a verified stop (the phase timeline shows the ramp-down and the
  stop-hold before the reverse phase);
- the reverse phase begins only after that stop;
- no single event commands a large forward command directly into a large reverse
  one.

Do this at **three speeds**: the lowest speed that still triggers a reversal (the
near-threshold case, where the stop logic is most marginal), the planned cruise,
and the maximum speed the safety ceilings allow. A reversal that verifies at one
speed and not another is the finding.

The phase timeline and the estimated velocity are the firmware's *belief*.
Confirm the stop independently as well: mark the vehicle's position at the moment
the forward ramp ends and again when reverse begins, and measure the two marks. A
software-verified stop with visible motion between the marks is a failed stop.

Record the observed stop dwell, the reversal latency, the independent stop
measurement, and the measured `kPlannedReversalStopThresholdMps` /
`kPlannedReversalStopDwellS` behaviour. If the vehicle is still moving when
reverse is requested, that is a finding.

## Held constant

Surface, tire pressure, build and mass, course, the pre-registered profile. The
pack is **recorded, not held flat**: for every run record a multimeter reading of
the pack (`pack_voltage_v`), the firmware's `batt_counts`, and whether the pack was
recharged (`recharged`), in the measurement CSV. The two voltage numbers are kept
side by side because `batt_counts` has no documented conversion, so the meter is
the only number in volts anyone can act on.

## Varied

One thing at a time: the commanded span (part A0); the commanded target (part A);
the speed at the stop command (part B); the reversal speed (part C); the
integral-limiting values (part L, if H6's were revisited).

Within one series, exactly one thing varies and the pack state is not pooled
across: a low-battery run and a fresh-pack run must not be averaged together, or
neither is explainable. If pack sensitivity is itself the question, that is a
**separate series with the pack as the deliberate variable**, reported separately
- not interleaved into the accuracy series.

## Measurements and logs

Per trial, record in the measurement CSV (or beside the capture with the same
fields): the commanded target, the believed position and the ground-truth position
at the stop command, the measured resting position, the run time, the direction,
the lateral drift, the speed at the stop command (`vel_mps` with `vel_ok`), the
commanded mode, the meter pack voltage, `batt_counts`, `recharged`, and - for any
run not accepted - `accepted=no` with its `exclusion_reason`.

Record the raw capture filename with every derived number, and record every
aborted trial with its reason. The capture is copied to `data/evidence/` and
committed; the measurement CSV goes with it.

## Repetitions

At least five trials per condition. A0: at least three spans, both directions. A:
at least three targets. B: at least three speeds. L: at least three runs at the
profile. C: at least five reversals per speed, three speeds. The count is derived
in the pre-registration from the smallest effect the series must detect
(`AGENTS.md` section 28), not chosen because five looks thorough.

## Safe-stop (abort) conditions

Kill switch primed and tested; abort on overshoot past the runout, veer off the
lane toward a boundary or a person, an unexpected direction, a phase timeout that
leaves the vehicle moving, or any run whose stop is not verified. Stop with the
kill switch, then `disarm`. Repeatedly failing to stop is a hard block - do not
continue to H8.

## Acceptance / decision rule

- **A0:** ground-truth travel and believed travel agree within the pre-registered
  band across the spans; a disagreement that grows with distance is a scale
  finding, a constant one is a zero finding.
- **A:** characterize the errors as systematic or random (`AGENTS.md` section 8):
  report mean and spread per target. A systematic offset is steerable through
  calibration; a large random spread is not a software problem and must be
  reported as a mechanism or traction issue.
- **B:** the forward offset is set from the measured data; the speed dependence
  is recorded either way; the reverse offset is recorded as measured-but-unused.
- **L:** the loaded tracking error is recorded; the gains in force are the ones
  the measured data supports.
- **C:** the stop-before-reverse order is confirmed with the observed dwell and
  with the independent mark measurement, at three speeds.

**No expected error is prescribed.** "Pass" means the recorded behaviour meets the
pre-registered criteria, not that it matches a predicted number. Use
`python tools/telemetry/analyze.py distribution <measurements.csv>` for the
series statistics - the systematic offset and random spread come from there, and
excluded runs stay in the record with their reason.

## If it passes

- #9 (braking behaviour and stopping distance) promoted, with the speed each
  offset was set for and the ground-truth braking measurements recorded.
- The **forward** brake-offset placeholder replaced with a measured value; the
  reverse measurement recorded as a pending software change until it is consumed.
- The reversal stop-verification values set from the measured stop behaviour.
- Safety ceilings may only now be set from evidence: the maximum forward/reverse
  velocity the vehicle can control and stop within the runout, and the minimum
  battery voltage at which that still holds. Record the evidence with each.
- Log in `DEVELOPMENT_LOG.md`.

## If it fails or is inconclusive

Escalation order: a measurement mistake (course, tape, stopwatch, the mark-spacing
resolution); a configuration mistake (the wrong scale or offset now live); a
wiring/electrical issue (supply sag under load - compare the meter reading with
`batt_counts`); a mechanical issue (binding, braking asymmetry, hysteresis wider
than H4 part C); a basic firmware bug; then calibration; then the control
strategy. Do not add sensing to fix a stopping problem before these are exhausted.
