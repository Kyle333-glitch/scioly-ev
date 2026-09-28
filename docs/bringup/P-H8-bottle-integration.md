# P-H8 - Bottle integration

**Primary class:** P (physical behaviour check).
**Order:** after H7.
**Answers:** does the vehicle contact the bottle, push it, and finish at or past
the bottle line without losing it (#11), and with what success fraction - measured
independently of what the firmware believed.
**Bench instrumentation required:** none beyond the run path; the bottle
mechanism itself is hardware, and it must be present and mechanically safe before
this protocol starts.

**Rules vs measurements.** The bottle line distance and the bottle placement
distance are *rule-derived* (rules 8.j and 7.c.ii.(5), mirrored in
`docs/rules/README.md`), not measured facts. Choose the tournament level before
running, record which level, and set the configuration from the rule for that
level. Do not "tune" a rule value to make a run look better.

## Prerequisites

- H7 passed, with the vehicle stopping at a target and reversing correctly.
- The bottle mechanism fitted, mechanically safe (no sharp edges or stored
  energy that can release unexpectedly), and confirmed by hand before powering.
- The official bottle, identified by its making/type **and condition** (fill level
  if the rule specifies one, cap on, any deformation) and recorded; a spare if
  available and also identified.
- The course marked: start point, target point, bottle line, and bottle placement
  for the chosen tournament level. Record the **placement tolerance** used (how
  far from the ideal placement a run is still valid) and how the bottle was
  actually placed (measured from the start line, with the tape's resolution).
- A means of **independent** contact evidence: a witness mark on the bottle (a
  chalk line or a small tape flag), and either a second observer or a camera able
  to see the contact. The firmware's `pos_m` is a belief; #11 needs an observation.
- Kill switch and clear runout; a second person able to stop the vehicle if the
  operator is watching the bottle.
- (Pre-register) in `data/prereg/`: the **success fraction** that counts as
  passing (for example "at least 8 of 10 runs finish with the bottle past the line"),
  and the reasoning behind it - the allowed number of failures must come from what
  the event can tolerate, not from the best run observed.

## Setup

Place the bottle per the rule for the chosen level. Mark where the bottle must
end up (at least past the bottle line) and where contact is expected. Capture
telemetry every run.

## Held constant

Course, bottle identity **and condition**, bottle placement (within the recorded
placement tolerance), tire pressure, build, mechanism adjustment, tournament
level. A bottle that was dropped, dented or refilled between runs is a different
bottle: record the change and treat it as a new series.

## Varied

**One at a time**, with the others held constant:

- approach distance / capture threshold (where the mechanism engages);
- approach speed;
- push completion criterion (what counts as "pushed").

Changing two at once makes a good run unattributable and a bad run undiagnosable.

## Measurements and logs

Per run, record both what the firmware believed and what a person saw, kept
separately:

- **believed:** contact position (`pos_m` at the tick the Capture phase begins);
  push start and end positions; push distance; the phase timeline (`Capture`,
  `Push`) from the `EVE1` events; any fault or phase timeout; run time.
- **observed:** the bottle's start position before the run (tape, per the
  placement tolerance) and final position after it (tape from the start point);
  whether the witness mark was disturbed and when; whether the bottle tipped, was
  thrown (beyond the line, airborne), or lost contact mid-push; and the observer's
  description of the contact.

Also record the command level, `batt_counts` and the meter pack voltage so a weak
pack is not mistaken for a mechanism problem; the candidate configuration (what was
varied); and `accepted=no` with an `exclusion_reason` for any run excluded from
that configuration's success fraction.

## Repetitions

Enough runs per condition for the pre-registered success fraction to be decided,
and at least three conditions. The count is derived in the pre-registration from
that fraction and the smallest failure rate worth distinguishing (`AGENTS.md`
section 28), not chosen for convenience. A single good push is not a result: the
failure modes (no contact, double contact, tipping) are what this protocol exists
to expose, and each one counts against the fraction.

## Safe-stop (abort) conditions

Kill switch primed. Abort immediately if the bottle is launched or thrown, the
mechanism jams or binds, the vehicle does not stop after the push, or the
mechanism touches a person. After an abort, stop the run with `disarm` and
inspect the mechanism before the next attempt.

## Acceptance / decision rule

A configuration is accepted when the **measured** (not believed) outcome meets the
event's requirement at at least the pre-registered **success fraction** of runs,
with the failure modes recorded and - for the excluded runs - the exclusions
justified before the fraction was read. Reporting the fraction is mandatory: "it
worked three times" is not a result, and the fraction is what a competition run
is actually betting on.

The judgement uses the observed evidence (bottle start/end positions, witness
mark, observer), not the firmware's `pos_m`. The two are recorded side by side so
the believed contact can be checked against the observed one. **No target push
distance is prescribed here**: the requirement is the event's, and the mechanism
parameters are chosen from the data.

## If it passes

- #11 (bottle interaction behaviour) promoted, with the placement and its
  tolerance, the bottle condition, the success fraction, the failure modes and the
  independent contact evidence recorded.
- Set the bottle-interaction parameters (capture threshold, approach distance,
  push criterion) from the measured data, with `ValueSource::Measured`, and cite
  the rule for the placement/line values. If the believed contact position and the
  observed one disagree systematically, fix the estimator or the mechanism - do not
  move the threshold that hides it.
- Log in `DEVELOPMENT_LOG.md`.

## If it fails or is inconclusive

In order: a rules/placement mistake (re-check the marked line against the rule);
a mechanism adjustment or binding; an electronics/current problem if the
mechanism is powered; a measurement mistake; then the firmware's capture/push
sequencing. Do not widen a capture threshold to paper over a mechanism that only
sometimes contacts the bottle - record the variance instead.
