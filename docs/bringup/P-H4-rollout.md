# P-H4 - Rollout: effective distance per tick, and the first slip evidence

**Primary class:** P (physical behaviour check).
**Order:** after H2 and H3.
**Answers:** what ground distance one encoder tick corresponds to (#6); how much
the motor encoder disagrees with hand-rolled travel (#10, first evidence).
**Bench instrumentation required:** B3 (raw signed tick readout).

This protocol deliberately separates two measurements that are easy to conflate:

- **A - kinematic scale, unpowered.** Hand-rolled on the real surface. No motor
  torque, so no slip is introduced by the drivetrain; this is the geometric
  scale.
- **B - powered scale, slip check.** Driven a measured distance at a low constant
  command, and then at a level near the planned cruise. A systematic difference
  between A and B is slip, deadband, coast or a braking artefact - not a reason to
  average the two.
- **C - tick hysteresis.** The backlash, measured in ticks, that bounds how
  precisely a position is known after a direction change. It is measured here so
  the forward/reverse difference in A can be *compared against* a number instead
  of being asserted.

Do them in that order, and record them separately. Averaging them would hide
exactly the effect #10 exists to measure.

## Prerequisites

- H2 passed (tick sign and the forward convention) and H3 passed (the sign under
  power agrees).
- Motor driver de-energized for part A; powered through B2 for part B.
- A flat, straight, clean running surface. Record it, and record tire pressure
  and any build change, because both affect the rollout.
- A tape measure or a marked straight course with a measured distance; record the
  instrument and its resolution.
- Kill switch and clear runout for part B.
- (Pre-register) the repeatability criterion - the spread within which repeated
  trials count as agreeing.

## Setup

Mark a start line and a finish line a measured distance apart on the surface.
Mark the wheel/chassis so the same starting orientation is reused. The wheel must
roll, not skid: a hand-rolled wheel that is dragged sideways invalidates the
trial and the trial is repeated, not adjusted.

## Held constant

Surface, tire pressure, vehicle build and mass, supply voltage (part B), the
course, the start orientation.

## Varied

- direction: run the course in both directions (a systematic difference between
  forward and reverse is backlash or a mechanical asymmetry - record it, and
  compare it against part C's measured hysteresis before calling it asymmetry);
- command level (part B): the low level and the cruise-near level, one at a time;
- trials: repeat, do not reuse a run.

## Measurements and logs

Part A (unpowered):

1. Raw tick delta N for the full measured distance D, taken **between line
   crossings**: the count from the instant the wheel mark crosses the start line to
   the instant it crosses the finish line. A count read from a stationary snapshot
   includes ticks that did not roll the course.
2. D and N per trial, at least five trials, in both directions.
3. Compute D/N per trial.

Part B (powered):

1. Same course, driven at **two** command levels, one at a time:
   - the lowest command that moves the vehicle reliably (H3's minimum duty is a
     starting point, not a target); and
   - a level near the planned cruise (`kPlannedCruiseMps`), because a single
     low-effort point cannot bound the slip where the competition run actually
     travels. Check the runout covers the cruise stopping distance before running
     this level; if it does not, record the cruise level as **deferred to H7** and
     do not take the #10/#12 verdict until it exists.
2. Record D (ground travel, tape), N (raw ticks), the command level, the supply
   voltage and `batt_counts`, per trial, at least five trials per direction per
   level.
3. Take the count over the same physical span as part A: from the mark crossing the
   start line to the mark crossing the finish line. Record the interval covered, so
   the definition of D matches between A and B.
4. Record the interval from the command to the **first tick change** (enable plus
   deadband), separately from the rolling count. Part A has no such interval; it is
   a part-B bias, and it is measured rather than assumed to be zero.

For both parts record `countedEdges`/`ignoredUpdates` so a noisy run is
identifiable rather than silently averaged in.

## Separating the A-versus-B difference

A systematic A-versus-B difference is not one effect. Measure each part so the
difference is attributed rather than guessed:

1. **Line crossings, not snapshots.** Both counts cover the mark-to-mark span
   (above).
2. **Command-to-first-motion.** The distance covered during the enable+deadband
   interval is a bias with no part-A counterpart; record it.
3. **Coast after de-energize.** Command the stop before the finish mark and record
   how far the vehicle coasts past the command instant; that travel is also a
   part-B effect. (If the run must end at the mark to take the count, do the coast
   measurement as its own trial.)

Only after those are subtracted is the residual a **slip** estimate. Record each
part separately: a single A-versus-B number that lumps deadband, coast and slip is
not a slip measurement, and it is the number most easily mistaken for one.

## Part C - tick hysteresis (backlash)

With the driver de-energized and the drivetrain free, rotate the wheel forward past
an encoder edge, then reverse by hand in small increments until the raw count first
changes in the other direction. The forward travel between the two counts is the
hysteresis, in ticks. Repeat at least five times at different wheel positions and
record the spread.

That number is a fact about the mechanism, and it bounds how well position is known
after any reversal. A forward/reverse difference in part A smaller than the
hysteresis is not evidence of asymmetry - it is inside the backlash.

## Repetitions

At least five trials per direction for each part. If the pre-registered spread is
not met, investigate the outlier's context (surface, pressure, a skid, a battery
state) and record the reason rather than discarding it.

## Safe-stop (abort) conditions

Part A is unpowered. Part B: kill switch primed; abort on any veer off the lane,
unexpected speed, a skid, a current-limit trip, or the vehicle not stopping when
de-energized. The lowest command that moves the vehicle is the one to use, not a
larger one for convenience.

## Acceptance / decision rule

A measurement is accepted when repeated trials agree within the **pre-registered**
criterion. **No expected value is prescribed**: the measured rollout is whatever
the hardware gives, and the acceptance is repeatability, not agreement with a
predicted number. The pre-registered spread must be tighter than the deadband and
coast biases the protocol separates, or the criterion cannot tell those from slip;
record the reasoning when it is set. Once accepted:

- set `kPlannedDistancePerEncoderTickM` in `src/VehicleConfig.h` to the measured
  mean, with `ValueSource::Measured`, and record the raw D/N pairs with it. Do
  not round to a suspiciously neat value.
- if the two directions differ systematically, compare it against part C's
  measured hysteresis before calling it backlash, then record it. Do not average
  across a systematic difference.
- compute the A-versus-B difference, subtract the measured deadband and coast
  parts, and label the residual as the first #10 evidence - systematic or random
  per `AGENTS.md` section 8. The cruise-level point (part B) must exist, or be
  recorded as deferred to H7, before any slip verdict drawn from it is used.

## If it passes

- #6 (rollout / effective distance per encoder tick) promoted.
- #10 (wheel slip) gains its first recorded evidence; it is not yet "verified" -
  that needs the distribution across runs that H10 supplies.
- Log in `DEVELOPMENT_LOG.md` with the raw pairs and the pre-registered
  criterion.

## If it fails or is inconclusive

In order: a skidded hand-roll (repeat it, rolling), a mis-measured course
(re-measure independently), tire pressure or surface variation, encoder noise
(check `ignoredUpdates`), then a mechanical problem. Do not tune other constants
to compensate for a rollout measurement that did not repeat.
