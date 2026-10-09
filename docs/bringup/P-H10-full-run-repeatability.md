# P-H10 - Full-run repeatability

**Primary class:** P (physical behaviour check).
**Order:** after H7, H8 and H9.
**Answers:** what the complete sequence does across many identical runs; whether
the remaining error is systematic or random (#10 sustained evidence); whether a
ground / dead-wheel encoder is justified (#12).
**Bench instrumentation required:** none beyond the shipped run path and the
analysis tooling, which is used here for the first time on a real capture.

## Prerequisites

- H7, H8 and H9 passed. The configuration is complete and measured; a run can be
  announced, armed, and started from the trigger.
- The exact competition geometry marked: start point, target point, bottle line,
  bottle placement for the chosen tournament level. The same course for every
  run.
- The official bottle and the vehicle in its final build. Record any change to
  the build between runs - a change resets the series.
- Tape and stopwatch (or video with a visible clock), telemetry capture per run,
  kill switch, second person.
- **USB tether discipline (moving runs).** Strain-relieve the capture cable and
  record the arrangement; before a run enters the distribution, check the
  capture's validity (`drop_s`/`drop_e`, `dt_ms` distribution) (C15).
- (Pre-register) in `data/prereg/`: the acceptance criteria, the number of runs,
  the **held-out** target and/or level (chosen before the primary series is
  analysed), the run-time measurement method, and the exclusion rule.

## Setup

Mark the start point so the vehicle is placed the same way every run (record the
placement tolerance used - the rules allow about 1 cm). Record the target
distance and target time for the chosen tournament level from the rules.

Prepare the hand-measurement CSV once, using the tool's own template. The flag
prints the template and exits, but the `analyze` subcommand still requires a log
argument, so pass any path:

```bash
python tools/telemetry/analyze.py analyze --print-measurement-template <any-log>
```

The template columns are `run_id`, `measured_end_position_m`,
`measured_run_time_s`, `pack_voltage_v`, `recharged`, `accepted`,
`exclusion_reason`, `target_distance_m`, `tournament_level` and `notes`.
`measured_end_position_m` is where the vehicle came to rest, measured from the
start line - the same quantity as the firmware's believed end position, which is
what makes the comparison meaningful.

The context columns are not optional in practice. `accepted` and
`exclusion_reason` are the **structured home for the exclusion rule**: a run judged
unacceptable is marked `no` with its reason at the time it is judged, so the
rejected run stays in the file instead of disappearing into a notebook, and the
reason was recorded before the distribution was read. `pack_voltage_v` and
`recharged` are what make a battery effect visible rather than assumed, and
`target_distance_m` is what makes the residual computable against the real target.

### How the run time is measured

The stopwatch is an instrument, so its method and its error are part of the
measurement - a run time without them is not comparable across operators or runs:

- define the **start instant** once (the arming/motion transition or the trigger)
  and use it for every run;
- define the **stop instant** as when the vehicle comes to rest at the end of the
  sequence - the same event the tape measurement is taken at;
- record the stopwatch resolution and the operator's reaction time (measure it
  once by timing a known interval), and report the combined uncertainty alongside
  the distribution rather than leaving it implicit;
- if video with a visible clock is available, prefer it and say so; read the time
  to the frame's resolution.

The **scored** number is the official Run Time of rule 8.o - actuation to complete
stop - and only the stopwatch/video measurement produces it; it is what goes in
`measured_run_time_s` and what may be judged against the rules. The firmware's
internal Run Time is a different, believed quantity: a sample carries the run's
own timeline (`run_start_ms`, `dwell_start_ms`, `stop_verified_ms`), and the
analyzer derives `internal_run_time_s = dwell_start_ms - run_start_ms`, which ends
at the **start** of the verified final-stop dwell and therefore excludes the rule's
Complete Stop interval (rule 8.p) instead of counting it. Record both per run and
never substitute one for the other: the internal metric has never been compared
with an official measurement, and only a series like this one that records them
side by side can validate it.

## Held constant

Course, bottle, build, mechanism adjustment, tire pressure, placement procedure,
configuration, tournament level.

## Varied

Nothing. The whole point of this protocol is that the only variation is the
run-to-run behaviour of the vehicle itself. Do not adjust anything mid-series.

## Measurements and logs

Record, per run: the supply voltage (`batt_counts` and a meter reading) and
whether the pack was recharged between runs - the event allows very few runs on
one pack, so pack state is part of the experiment, not a nuisance. Record the
measured resting position and run time in the measurement CSV, and keep the raw
capture and the hand measurements together.

## Procedure

1. Place the vehicle at the start point. Arm, then start with the trigger.
2. Capture the whole serial stream to a file under `data/raw/`.
3. Measure the resting position with the tape, and the run time with the
   stopwatch; enter both in the measurement CSV.
4. Repeat for at least the pre-registered number of runs. Keep every run, including
   the bad ones, and record `accepted=no` with the `exclusion_reason` at the time
   you judge it - before you look at the distribution, so the exclusion rule is a
   rule and not a reaction to the spread.

## Held-out check (calibration must generalise, not memorise)

The series above runs at the primary target and level, which is also what the
configuration was built around. After it, run a **held-out** condition that was
not used to set anything: a target distance outside the primary set and/or a
different tournament level, with its own rule-derived bottle line and placement.
The held-out condition is chosen and written down in the pre-registration before
the primary series is analysed.

A configuration that meets the criterion only at the calibrated target has been
fit to the test, not calibrated. Record the held-out results **separately** and do
not fold them into the primary distribution.

## Analysis

Analyze all the captures together:

```bash
python tools/telemetry/analyze.py analyze data/raw/run1.log data/raw/run2.log \
    --measurements data/evidence/<session>/measurements.csv --out data/reports/session-run
```

Use that report for what it is: the phase timeline, `dt_ms`, the drop counters,
and the believed endpoints. **It is not evidence.** The evidence is the
tape-and-stopwatch measurement, recorded by hand.

Then compute the series statistics from the hand measurements, with the tool
doing the arithmetic so it is reproducible and the exclusions stay visible:

```bash
python tools/telemetry/analyze.py distribution data/evidence/<session>/measurements.csv \
    --out data/reports/<session>-distribution
```

That gives mean, sample standard deviation, min/max and range over the *accepted*
runs, and the residual of resting position against each run's recorded target -
reported as a systematic offset (the mean) and a random spread (the standard
deviation), which is the split `AGENTS.md` section 8 asks for. The raw CSV stays
the record; the distribution file is derived from it. Compare the measured versus
believed endpoints separately to see whether an error is in the vehicle or in what
the firmware believed (`tools/telemetry/README.md` explains both).

## Repetitions

At least the pre-registered number of runs, derived from the criterion and the
smallest effect worth detecting (`AGENTS.md` section 28) - ten is a common
starting point, not a default. If the spread is dominated by a few outliers, read
the outliers' context in the captures and in the notes before adding runs; do not
remove them, and record any exclusion in the CSV with its reason.

## Safe-stop (abort) conditions

Kill switch primed. Abort a run on veer outside the lane, a phase timeout that
leaves the vehicle moving, or the bottle thrown. After an abort, record what
happened and fix the cause before continuing.

## Acceptance / decision rule

A configuration is competition-viable when the measured distribution of resting
position and run time satisfies the **pre-registered** criteria across the
repetitions, the excluded runs are explained by their recorded reasons rather than
discarded, and the **held-out** condition also passes. **No expected position or
time is prescribed** - the rule gives a range; the measurement gives the
distribution. Classify the residual error as systematic or random per `AGENTS.md`
section 8, using the `distribution` report's systematic offset and random spread:
a tight offset around the target is calibratable; a wide scatter is a physical
effect and is not fixed by adding a constant.

## If it passes

- #10 (wheel slip) gains the sustained evidence it needs: the distribution of
  motor-encoder travel versus measured ground travel across many loaded runs.
- #12 (benefit of a ground / dead-wheel encoder): record an explicit verdict from
  the measured slip. If the slip magnitude is small and the residual is
  systematic, the verdict is "added hardware not justified by current evidence" -
  and that verdict is the row's evidence, whether or not hardware is added. If
  the slip is large, write down the measured problem, its magnitude, and why the
  motor encoder cannot resolve it before proposing hardware.
- Record the measured full-run behaviour in `DEVELOPMENT_LOG.md`. Selecting and
  freezing a competition build is a separate, deliberate decision - this protocol
  feeds it; it does not make it.

## If it fails or is inconclusive

Escalation order: a measurement mistake; a configuration mistake (the wrong
scale, offset, or rule value now live); a wiring/electrical issue (compare
`batt_counts` across runs); a mechanical issue (traction, binding, mechanism);
a basic firmware bug; then calibration; then the control strategy; and only then
consider additional hardware. Do not add sensing to explain a spread that a
measurement mistake could explain, and do not cherry-pick the good runs.
