# P-H2 - Encoder sign and counting, hand-driven

**Primary class:** P (physical behaviour check).
**Also contains:** E gate (the motor driver is de-energized throughout).
**Order:** after H0. Independent of the driver protocols, and worth doing early
because it needs no motor power at all.
**Answers:** which tick sign means forward travel; how many ticks one wheel
revolution produces; whether the count is clean and repeatable by hand; how much
mechanical hysteresis (backlash) sits in the count.
**Bench instrumentation required:** B3 (`bench ticks`: raw signed tick readout
plus `counted_edges`/`ignored_updates`). Built (`src/bench/`, `env:bench`), never
run.

Do not use `pos_m` for this test: it is derived through the rollout scale, which
is itself unmeasured until H4. Raw ticks only.

## Prerequisites

- H0 passed.
- Motor driver de-energized (`nSLEEP` low), motor supply off or current-limited.
- Wheels off the floor; drivetrain free to turn by hand.
- **The vehicle's forward direction is defined before this test** and recorded:
  mark which end of the chassis is the front and which way the wheel turns when
  the vehicle would travel forward. This is a mechanical convention chosen by the
  team, not a measurement, and it is what gives the tick sign a meaning.
- A visible mark on the wheel or axle to count revolutions.

## Setup

Raw tick readout captured to a file. Hand-rotate the wheel; never power the
motor. Note the encoder's mounting side, since "forward" is defined at the
vehicle, not at the encoder.

## Held constant

Encoder mounting and wiring, pull-up configuration, firmware image, the
forward-direction convention, the surface the wheel is on (hand-rolling on a
bench gives the same count as on the floor; the count is mechanical).

## Varied

- direction: forward, then reverse, then forward again;
- speed: slow (well under one edge per tick period) and brisk hand rotation;
- distance: single revolutions, then runs of at least ten revolutions.

## Measurements and logs

1. Signed raw tick delta for each rotation direction, repeated. Record the sign
   with its direction in the same line; the sign is meaningless alone.
2. Ticks per full wheel revolution, measured over at least ten revolutions per
   run, repeated. Record `countedEdges` and `ignoredUpdates` alongside.
3. Monotonicity: does the count only increase while rotating steadily one way,
   and only decrease the other way? Record any reversal of sign during steady
   rotation - that is noise or a decoding problem, not a rounding issue.
4. Return to mark: rotate one way and back to the start mark; record the net
   tick delta. A non-zero net over a closed loop is evidence about the decoder or
   the mechanics, and it must be recorded even if small.
5. Slow rotation: does every edge count? Brisk rotation: is the count still
   consistent with the slow case?
6. With B3, `ignoredUpdates` while rotating steadily. In this decoder (x2, A
   channel change only), ignored non-A transitions are expected by design, so a
   non-zero count is interpreted against the decoder's semantics rather than
   treated as a failure automatically. Record it and say what it means.
7. **Hysteresis (backlash), in ticks.** Rotate forward past an encoder edge, then
   reverse by hand in small increments until the raw count first changes in the
   other direction. The travel between the two counts is the hysteresis. Repeat at
   several wheel positions and record the spread. This is the tolerance on any
   position taken after a direction change; without it, a small forward/reverse
   difference cannot be told from backlash (H4 part C repeats it on the surface).

## Repetitions

Forward: ten revolutions x five runs. Reverse: ten revolutions x five runs.
Slow/brisk: three each. Return-to-mark: three. Hysteresis: five, at different
wheel positions.

## Safe-stop (abort) conditions

No powered motion is possible. Abort if the count changes with the wheel
stationary (noise investigation), if any wiring or the encoder heats, or if
hand-rolling is not free (the driver may not actually be de-energized - check
that before continuing).

## Acceptance / decision rule

Pass requires: the tick sign is consistent for a given physical direction across
every repetition (this is #4); ticks per revolution is repeatable within the
pre-registered spread (#5, hand-driven); a closed loop returns to within the
pre-registered bound of zero; slow and brisk rotations agree within the
pre-registered spread; and the hysteresis is repeatable within its own
pre-registered spread (a hysteresis wider than the sign test's tolerance makes
the sign test inconclusive, not passing). **No expected value is prescribed** - the measured count
is whatever the hardware produces, and the acceptance is repeatability, not
agreement with a predicted number. "No counts at all" is a finding about the pin
or the interrupt channel, not a zero measurement.

## If it passes

- #4 (encoder direction sign): recorded relative to the forward convention.
- #5 (signal quality): the hand-driven half; the powered half is H5.
- Record ticks per revolution and the direction convention, and use them only for
  the hand-driven scale - the vehicle rollout (#6) is H4 and is not derivable
  from this alone.

## If it fails or is inconclusive

Escalation order: a pin with no interrupt channel attaches nothing (check the pin
against the framework's pin table and against H1A's continuity table); a wiring
or pull-up problem; a mechanical encoder problem; then a decoding bug. Do not
compensate for a noisy count with a scale factor - that hides the noise (it is
row #5's whole point).
