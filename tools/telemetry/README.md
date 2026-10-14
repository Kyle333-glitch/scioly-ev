# Telemetry analysis tooling

PC-side tools for reading the firmware's telemetry stream: parse a captured log,
compute descriptive metrics, draw reproducible charts, compare a run's believed
endpoints against measurements taken by hand, and describe the distribution
across a series of those measurements.

**Read this first.** These tools describe what the *firmware reported*. They do
not measure the vehicle and they do not conclude anything about it. Whether the
vehicle went where it believed it did is answered by a tape measure and a
stopwatch, and recorded in `VERIFIED_STATE.md` - never here. In particular:

- a metric is derived from a log and is reproducible;
- a *conclusion* needs physical evidence, which no log and no synthetic fixture
  can supply;
- nothing this tool writes may be pasted into `VERIFIED_STATE.md`, and the tool
  refuses to write there.

The tools are **standard library only**. Nothing needs installing, no
`pip`, no virtual environment, no notebook and no dashboard.

## Quick start

From the repository root:

```bash
# 1. Write the synthetic fixtures (generated data, not measurements)
python tools/telemetry/analyze.py synth

# 2. Analyze them - this is the smoke test that nothing is missing
python tools/telemetry/analyze.py analyze tools/telemetry/samples/synthetic_a.csv

# 3. Analyze a real capture, comparing several runs
python tools/telemetry/analyze.py analyze data/raw/session1.log data/raw/session2.log \
    --out data/reports/session1-2

# 4. Describe the spread across a series of hand measurements
python tools/telemetry/analyze.py distribution data/evidence/session1/measurements.csv \
    --out data/reports/session1-distribution
```

A capture is just the text the serial port produced, saved to a file:
`pio device monitor` output, or a terminal capture. Nothing has to be converted
first.

## Commands

### `synth`

Writes the synthetic fixtures that validate the parser and the plots before any
real log exists. Deterministic: regenerating them reproduces identical bytes.

```
python tools/telemetry/analyze.py synth [--out DIR] [--force]
```

| Fixture | What it exercises |
|---------|-------------------|
| `synthetic_a.csv` | a nominal run: every phase, a reversal, no drops |
| `synthetic_b.csv` | a dropped burst that straddles a phase boundary, plus a rejected command |
| `synthetic_c.csv` | a fault latched mid-run, ending the run |

Every fixture begins with `# SYNTHETIC` comment lines saying it is not measured
data. Those comment lines are also the tool's test that non-telemetry lines in a
capture are tolerated rather than rejected.

### `analyze`

```
python tools/telemetry/analyze.py analyze LOG [LOG ...] [options]
```

| Option | Meaning |
|--------|---------|
| `--out DIR` | report directory (default `data/reports/analysis` at the repo root) |
| `--run-id ID` | run id for the corresponding log, repeatable and positional (default: the file name) |
| `--measurements FILE` | CSV of hand measurements, for the endpoint summary |
| `--no-plots` | write metrics and summaries without SVG plots |
| `--continue-on-error` | record unreadable lines and keep going instead of refusing the log |
| `--print-measurement-template` | print the measurement CSV template and exit |

Exit codes: `0` success, `1` bad command line, `2` the input could not be read or
could not be parsed.

### `distribution`

```
python tools/telemetry/analyze.py distribution MEASUREMENTS.csv [--out DIR]
```

Reads the hand-measurement CSV (the same file `--measurements` takes) and reports
the distribution across the series: mean, median, sample standard deviation,
min/max and range for each numeric column, split by tournament level and target,
plus the residual of where the vehicle stopped against the target it was sent to.

The residual is reported as two numbers on purpose, because `AGENTS.md` section 8
asks a reader to separate them: the **mean** is the systematic offset (which is
calibratable) and the **standard deviation** is the random spread (which is a
physical effect, not a constant to tune).

Only the runs whose `accepted` cell is `yes` are averaged in. An excluded run is
listed with its reason - a rejection that lives only in a notebook is not kept,
so the reason belongs in the file. A run left unjudged is listed too, and is kept
deterministic by being reported as its own class rather than silently included.

## What the report contains

```
<out>/manifest.json            inputs, their sha256, and what was written
<out>/comparison.json/.txt     side-by-side believed metrics (two or more runs)
<out>/endpoints.json/.txt      measured vs believed endpoints (with --measurements)
<out>/runs/<run_id>/metrics.json
<out>/runs/<run_id>/summary.txt
<out>/runs/<run_id>/parse_errors.txt   only if lines were unreadable
<out>/runs/<run_id>/plots/*.svg
<out>/distribution.json/.txt   series statistics (from the distribution command)
```

Metrics cover, per run:

- **state transitions**: every event in the stream, plus the phase boundaries
  derived from the sample train, with the disagreements between the two;
- **position**: believed run-axis position, min/max, net travel, remaining
  distance, and the **believed endpoint**, which is the position at the run's
  *verified final stop* (`end_position_source: verified_final_stop`) - not the last
  sample in the file, which may be a vehicle being handled after the run. The
  verified stop is the usable sample whose raw `t_ms` equals its own
  `stop_verified_ms` (the verification tick on the firmware's own clock): the
  flag and timestamp latch into every later `Done` sample, so a later sample
  carrying them is proof verification *occurred*, never proof its position was
  observed *at verification*. When the verification tick itself is missing
  (dropped, or the capture started after it), the endpoint is unavailable
  (`end_position_source` null with the reason) even if a later sample has the
  same value - value equality never promotes a late sample. When the
  run never verified a stop, the last usable sample is reported as exactly that
  (`end_position_source: latest_usable_sample`) with the reason, and the two are
  never substituted for one another. `last_m` is the last usable tick,
  `after_stop_travel_m` is how far the estimate moved after the stop, and
  `end_position_sampling_window_m`/`_ms` report the observed adjacent-sample step
  around the confirming one (the position and interval difference to the previous
  usable sample). That window is a sampling delta, not a physical error bound:
  motion can occur and return between samples, and quantization and reference
  errors remain. `believed_target_m` is reconstructed
  with the sign of the leg the run stopped on - `position + remaining` forward,
  `position - remaining` in reverse (`MotionPlan::remainingDistanceTo`), recorded in
  `believed_target_source`;
- **capture**: how the input was segmented - the sessions (one per firmware boot,
  opened by a `Boot` event), the runs inside the analyzed one (a run is identified
  by its actuation reading), which session/run was analyzed, what was excluded, and
  `mixed_sessions`/`mixed_runs`. Backwards raw timestamps are classified, not
  assumed to be wraps: `clock_wrap_count` (a rollover), `reordered_reading_count`
  (a few milliseconds of re-ordering between two stamps) and
  `restart_suspected_count` (neither - a reset whose `Boot` line was not captured, in
  which case the elapsed duration is refused rather than reported as ~49.7 days);
- **phase durations** are *transition-bounded* (the first sample of the phase to the
  first sample of the next one), so a complete phase is not one interval short. A
  phase still in effect at the end of the capture has no bound: its duration is
  `None`, it is listed in `censored_phases`, and only its observed sample span is
  reported;
- **velocity**: measured and commanded ranges and means, and the tracking error;
- **motor effort**: overall and per motor mode, in permille of full scale;
- **raw inputs**: `batt_counts` and `curr_counts` **as raw ADC counts** - no
  conversion is documented anywhere in the project, so none is invented here;
- **control `dt`**: the interval each tick was given, plus gaps and duplicate
  timestamps;
- **braking and reversal timing**: how long the policy spent in `ForwardDecel`,
  `BrakeZeroSpeed` and `Reverse`. These are firmware *state* timings, not
  stopping distances;
- **run duration**: the armed window (the firmware's own armed flag, not a physical
  start or finish line) and the firmware's run timeline - `run_start_ms` (the
  actuation reading), `final_stop_dwell_start_ms`, `stop_verified_ms`, the derived
  `internal_run_time_s` (actuation to the **start** of the verified final-stop
  dwell, so the rule's Complete Stop interval is excluded) and
  `final_stop_verification_completed_s`. Both derived numbers are `None` unless a
  final stop was verified, so a missing value is never read as "the run never
  finished". The internal Run Time is the firmware's own believed clock, never the
  event's official Run Time, which a timekeeper or video measures;
- **drops**: the drop counters, whether they ever went backwards, and when the
  first record was lost.

Plots are plain SVG written by a small deterministic writer in
`evtelemetry/svgplot.py`: no timestamps, no randomness, fixed element order, so
two runs over the same log produce byte-identical charts.

## The external measurement file

`--measurements` and `distribution` take the same small CSV. `run_id` is
required, and at least one numeric measurement column (`measured_end_position_m`,
`measured_run_time_s`, `pack_voltage_v`, `target_distance_m`):

```csv
run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,recharged,accepted,exclusion_reason,target_distance_m,tournament_level,notes
BENCH-1,6.431,13.31,11.9,yes,yes,,6.500,State,"tape measure, start line to where it stopped"
BENCH-2,6.118,13.42,11.7,no,no,pack sagged mid-run,6.500,State,tape measure
```

`measured_end_position_m` is where the vehicle actually came to rest, measured
from the start line - the same quantity as the firmware's believed end position,
so the two are directly comparable. It is deliberately not called "travel": in a
run that reverses, path length, net displacement and final position are three
different numbers.

`measured_run_time_s` is the **official** Run Time of rule 8.o - actuation to
complete stop, as the timekeeper or a video measured it. It is compared against
the firmware's **internal** Run Time (`run_window.internal_run_time_s`), which
ends at the start of the verified final-stop dwell for exactly that reason; the
comparison's `run_s` column and `believed_run_time_s` are that internal number,
not the official one. A missing cell means "not measured", and never zero.

The remaining columns exist because a bare endpoint is not a repeatability
record:

| Column | Meaning |
|--------|---------|
| `pack_voltage_v` | the pack voltage a meter read for that run (alongside the firmware's `batt_counts`) |
| `recharged` | whether the pack was recharged before the run (`yes`/`no`) |
| `accepted` | whether the operator accepted the run into the series (`yes`/`no`); blank means unjudged |
| `exclusion_reason` | why a run was excluded - recorded when it is excluded, not after the spread is seen |
| `target_distance_m` | the target the run was sent to, so the residual is against the real target |
| `tournament_level` | the rules level in force, so runs at different levels are not pooled |

A blank cell means "not measured" (or "not judged"), and yields no delta,
rather than a zero that would read as "measured exactly right". A mistyped
`accepted`/`recharged` value is refused rather than guessed at, because reading
`maybe` as either one would be worse than refusing the file. Rows whose run id
was not analyzed, and runs with no measurement row, are both listed rather than
silently skipped.

A comparison is eligible only when the whole capture's integrity supports
attributing the believed endpoint to the one analyzed run: one session, one
run, a verified final stop, no suspected unsegmented clock restart, no
event-only extra `Boot` (a session with events but no samples, which the
sample-derived `mixed_sessions` flag cannot see), and no permissively skipped
malformed records. Unknown integrity is not verified integrity, so any of
these refuses the comparison with an explicit reason in
`comparison_refused_reason` (every applicable reason is kept, in a fixed
order), and the refused deltas are null - never zero. The descriptive
believed values stay visible; only the attributable delta is withheld.

## How a log is read

Telemetry lines are `EVS<version>,...` (one control tick) and `EVE<version>,...`
(one transition or operator action), as frozen by the firmware in
`lib/evcore/src/TelemetryRecord.h` and `TelemetryFormat.h`. This tooling mirrors
that schema in `evtelemetry/schema.py`, and a host test re-reads the firmware
headers and fails if the mirror drifts.

The rules:

- **A telemetry line must parse, or the parse fails.** A line beginning with the
  telemetry root `EV` is a claim to be telemetry. If it declares another schema
  version, if its field set, order or spelling differs, or if a value is not the
  kind of value that field holds, the tool reports the file, the line number, the
  offending line and what the schema expected. Nothing is skipped silently,
  because a reader who loses one record cannot tell whether the vehicle did
  something surprising or the tool did.
- **Anything else is ignored, not an error.** Operator responses (`OK`, `STATUS`,
  `ERR`, ...), the staged `BOOT,...` line, comment lines and monitor noise are
  counted as ignored lines. Only an explicit provenance declaration counts as
  one: a `#` line whose first token after the `#` - whitespace/comma-delimited,
  case-insensitive - is exactly `SYNTHETIC` or `CAPTURE`. Every other `#` line
  is an ordinary comment (`# exported by student`, `# I am not using synthetic
  test data today`) and declares nothing. A `SYNTHETIC` declaration anywhere in
  the capture wins over any `CAPTURE` claim regardless of order (a conflict is
  recorded in `metrics.json` as `capture.provenance_conflict` and named in
  `summary.txt`); a missing declaration stays undeclared, never a fabricated
  `CAPTURE`. The declaration travels with the parsed records: a fixture copied
  or renamed to a neutral file name still reports as synthetic, and a name may
  add that warning but never remove one. A declared `CAPTURE` is the capture's
  own claim, not a verified measurement.
- **Time is unwrapped per lane, and a reorder is not a rollover.** A backwards
  `t_ms` step is classified before it is reconstructed: a few-millisecond step is
  a *reordered* reading and stays in the current epoch with its signed delta, so
  `1000 -> 999 -> 1020` keeps the timeline `1000, 999, 1020` and a ~20 ms
  duration. Only a step across the top of the uint32 range - a genuine *wrap* -
  advances the epoch by 2**32, using the same unsigned arithmetic the firmware
  uses, so a real rollover cannot produce a wrong duration and the legal
  event-before-sample interleaving cannot produce a bogus timestamp jump. A
  backwards step that is neither is *restart_suspected* (an unlabelled reset or a
  corrupt reading): the timeline stays monotonic but every elapsed metric across
  it - log, armed and phase durations, sample-spacing extremes, run times and
  phase-timing deltas - is reported as unavailable with its reason, never as
  ~49.7 days, and the charts carry a TIMING UNRELIABLE note. Each lane is
  unwrapped on its own timeline, so cross-lane comparisons use the raw `t_ms`
  reading within one session. A late pre-wrap record arriving after its lane
  already rolled over is read as a reordered reading in the previous epoch, not
  as a second wrap. When the two lanes start on opposite sides of a rollover
  with no bridging evidence, or when one lane holds an unbridged gap of half
  the uint32 range or more, their relative alignment cannot be established:
  cross-lane elapsed durations are then unavailable with an explicit reason
  (still no fabricated ~49.7 days), lane-local observations stay available, and
  single-lane monotonic captures of any ordinary length keep working. A phase
  change across a benign reorder reports no negative duration: the affected
  segment is marked timing-unreliable instead.
- **A capture can hold several boots and several runs, and they are never mixed.**
  A `Boot` event opens a session; within a session, a changed actuation reading
  marks a new run. The analyzer describes the last run of the last session with
  samples, reports everything it excluded, and sets `mixed_sessions`/`mixed_runs`.
  Metrics, phase bands, event markers and charts all describe that same selected
  record set: selected-run plots never include excluded earlier-run samples, and
  selected-run counts never include excluded earlier-run events. The event
  boundary is the run's actuation reading (compared wrap-aware), not the first
  sample tick, so an attributable actuation event preceding the first sample is
  kept. Session-wide facts (the session window, drop diagnostics) and
  whole-capture integrity (other sessions, wraps, restarts, parse errors) stay
  separate and are never shrunk to the selected run.
  An endpoint comparison is refused (with its reason, in `endpoints.json`/`.txt`)
  when the capture cannot be attributed to one run, when that run recorded no
  verified final stop, when the run's verification evidence contradicts itself
  (conflicting stamps, conflicting same-tick positions, verification claimed
  mid-drive, or an impossible dwell timeline - the endpoint is then unavailable
  rather than first-candidate-wins, and input order cannot change an eligible
  delta), or when the capture's chronology is uncertain (suspected restart or
  unalignable cross-lane epochs). Descriptive believed values stay visible;
  only the attributable deltas are withheld. Per-row fixture provenance
  (`synthetic`, the declared provenance, conflicts, and a plain-text warning)
  travels in `endpoints.json` and `comparison.json`, and the `.txt` exports
  name synthetic runs and untrusted CAPTURE claims, so a copied root export
  still reads as a fixture and never as calibration evidence.
- **`ovf` parses to "no value"**, not to infinity, so a bug in a record cannot
  enter a mean as a huge measurement.
- **A hand-measurement file is read literally or refused.** Header names are
  whitespace-normalized before the rows are read through them, duplicate columns
  are refused, a row whose cell count is not the header's is refused, and a numeric
  cell must be finite and inside its column's domain (`measured_run_time_s`
  non-negative, `pack_voltage_v` and `target_distance_m` positive). Every such
  failure is a documented `error:` line with the file and row, an exit code of 2, and
  no partial report - not a traceback and not a silent "not measured".

## Reproducibility

Every output is deterministic: JSON is dumped with sorted keys, numbers are
rounded before printing, element order is fixed, and no output contains a
timestamp, a host name or a random id. Two runs over the same inputs produce
identical files, which is what makes a chart citable and a diff meaningful. The
manifest records the `sha256` of each input, so a report can be tied back to
exactly the bytes it was computed from.

## Tests

```bash
python -m unittest discover -s tools/telemetry/tests -t tools/telemetry
```

The suite covers the parser's failure modes, the metrics against the synthetic
generator, the schema mirror against the firmware headers, the session/run
segmentation and verified-stop semantics, the measurement-file validation, the
output-integrity cases (run-id collisions, stale artifacts, provenance, `synth`
without `--force`), and the end-to-end workflow including reproducibility and the
guard that keeps this tool away from `VERIFIED_STATE.md`.

## Output ownership and publication

A run directory holds only the artifacts the current analysis wrote: a rerun
reconciles the directory (a clean rerun removes the previous `parse_errors.txt`, and
`--no-plots` removes the previous plots) so a directory never mixes two analyses.
The report root is reconciled the same way, over a fixed set of managed names
only: `comparison.json/.txt` exist only when the current generation compared two or
more runs, `endpoints.json/.txt` exist only when it was given `--measurements`,
and each run owns `metrics.json`, `summary.txt`, `parse_errors.txt` (when the
parse left errors) and the seven plot slots `plots/empty.svg`, `plots/position.svg`,
`plots/velocity.svg`, `plots/effort.svg`, `plots/control_dt.svg`,
`plots/raw_inputs.svg`, `plots/phase_timeline.svg`. A rerun that drops to one run,
or drops the measurements file, removes the pair it no longer produces, and run
directories no current input maps to are reconciled to their unknown content (or
removed when empty). Anything else - unknown files, unknown directories,
`distribution.*` (owned by the `distribution` subcommand, never by `analyze`),
and links - is left untouched, and no prior manifest is ever trusted to name a
deletion target: deletion paths are built from the fixed managed names only.

The destination is a dedicated analyzer output directory, not a shared folder:
the managed names above are reserved slots, and unrelated bytes occupying one
will be replaced or removed by a successful generation. Before any mutation the
tool validates that the output root, its ancestors, and every managed slot are
regular files and directories - never a symbolic link, Windows junction, or
other reparse point - and refuses path traversal, absolute-path escapes, and
source files colliding with the destination. Junctions are detected via
`os.path.isjunction`/`islink` plus reparse-point metadata, because `islink`
alone does not classify Windows junctions; cleanup never traverses a reparse
point and writers never write through one. A second concurrent writer is
refused via an exclusive lock file. These checks cannot rule out a path that
is replaced between validation and use: hostile concurrent mutation of the
output tree during publication is unsupported, and no race-proof confinement
beyond the executed checks is claimed.

Publication is staged. Every artifact renders in memory first; the previous
`manifest.json` is then invalidated and an incomplete-publication marker
(`.publication_state.json`) is written; the new artifacts commit; the new
manifest - carrying `publication_state: complete` - commits last and clears the
marker. A failure during rendering leaves the previous generation intact; a
failure during commit leaves no manifest claiming completeness, only the
explicit incomplete marker, and the next successful invocation recovers
deterministically. A directory without a manifest is not a complete generation
and must not be read as one.

Every run id is checked for output-path collisions before anything is written -
including case-only differences and the characters that normalize to `_` - because a
report tree generated on one machine is read on another. JSON is written by rename
and refuses non-finite numbers, so a reader never sees a half-written file and no
report contains `NaN`/`Infinity`. `synth` never replaces an existing fixture whose
content differs unless `--force` is given.

## Layout

```
tools/telemetry/
    analyze.py                 entry point (the only file to run)
    evtelemetry/
        schema.py              the mirror of the firmware's frozen schema
        parse.py               strict line and stream parsing
        metrics.py             descriptive metrics over a parsed log
        endpoints.py           measured-vs-believed endpoint comparison
        distribution.py        series statistics over hand measurements
        svgplot.py             dependency-free deterministic SVG charts
        report.py              report and plot writing, plus the guard
        synthetic.py           the deterministic fixture generator
        cli.py                 the command line
    samples/                   the committed synthetic fixtures
    tests/                     the host tests
```
