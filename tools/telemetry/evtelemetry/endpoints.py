"""Compare a run's believed endpoints against measurements taken by a person.

The firmware's telemetry says what the *vehicle believed*: a run-axis position,
an armed window, a remaining distance. Whether the vehicle actually finished
where it believed it did is a physical question, and the only answer to it is a
tape measure and a stopwatch. This module takes those external numbers from a
small CSV and puts them next to the believed ones.

It computes differences and stops there. Deliberately:

* a difference is arithmetic, and it is reproducible;
* a *conclusion* - the vehicle overruns, it slips, it under-brakes - needs
  repeated measurements and is recorded in ``VERIFIED_STATE.md``, never here;
* a synthetic run has no external measurement at all, so an endpoint summary for
  synthetic data is not even meaningful, and the tool says so by leaving the
  measured columns empty rather than filling in the believed numbers twice.

``believed_run_time_s`` is the firmware's internal Run Time (the run's own clock,
actuation to the start of the verified final-stop dwell); it is blank when the log
shows no verified final stop, and it is never approximated from the armed window
or the log duration, because neither is the rules' Run Time. The external
measurement remains the official one.

The measurement file is a plain CSV with this header (``run_id`` plus at least
one numeric measurement column):

    run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,recharged,accepted,exclusion_reason,target_distance_m,tournament_level,notes
    BENCH-1,6.431,13.31,11.9,yes,yes,,6.500,State,"tape measure, start line to where it stopped"
    BENCH-2,6.118,13.42,11.7,no,no,pack sagged mid-run,6.500,State,tape measure

``measured_end_position_m`` is where the vehicle actually came to rest, measured
from the start line - the same quantity as the firmware's believed end position,
so the two are directly comparable. It is deliberately *not* called "travel":
in a run that reverses, total path length, net displacement and final position
are three different numbers, and a column that could mean any of them would be
worse than no column at all.

The context columns exist because a bare endpoint is not a repeatability record.
Whether a run was *accepted* into the series, and why it was excluded, is part of
the measurement: `AGENTS.md` section 19 says not to cherry-pick and to keep
enough context to explain a rejected run, and a rejection reason that lives only
in a notebook is not kept. The pack columns (`pack_voltage_v`, `recharged`) and
the target/tournament columns let the distribution be grouped meaningfully later
without re-reading the operator's notes.

Blank cells are allowed and mean "not measured" (or "not judged", for
``accepted``); the tool reports which runs have no measurement rather than
treating a blank as zero.
"""

from __future__ import annotations

import csv
import hashlib
import io
import math
from dataclasses import dataclass, field
from typing import Any, Optional, Sequence

from .synthetic import SYNTHETIC_MARKER

__all__ = [
    "MeasurementError",
    "Measurement",
    "MeasurementTable",
    "MEASUREMENT_TEMPLATE",
    "MEASUREMENT_COLUMNS",
    "MEASUREMENT_NUMERIC_COLUMNS",
    "MEASUREMENT_BOOL_COLUMNS",
    "load_measurements",
    "compare_endpoints",
]

# The exact columns the loader accepts. A typo in a header is a corrupt input,
# not a field to guess at, so an unknown column is refused by name.
MEASUREMENT_COLUMNS: tuple[str, ...] = (
    "run_id",
    "measured_end_position_m",
    "measured_run_time_s",
    "pack_voltage_v",
    "recharged",
    "accepted",
    "exclusion_reason",
    "target_distance_m",
    "tournament_level",
    "notes",
)

# Columns parsed as numbers. At least one must be present, or the file carries no
# measurement at all.
MEASUREMENT_NUMERIC_COLUMNS: tuple[str, ...] = (
    "measured_end_position_m",
    "measured_run_time_s",
    "pack_voltage_v",
    "target_distance_m",
)

# Columns parsed as a yes/no judgement. Blank means "not recorded"; an
# unrecognised spelling is refused rather than silently read as one of them.
MEASUREMENT_BOOL_COLUMNS: tuple[str, ...] = ("recharged", "accepted")

MEASUREMENT_TEMPLATE = (
    "# external measurements taken by hand, one row per run.\n"
    "# NOT firmware data, and NOT evidence on its own: repeatability needs repeats.\n"
    "# accepted/exclusion_reason are part of the record - an excluded run is kept, not deleted.\n"
    "run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,recharged,"
    "accepted,exclusion_reason,target_distance_m,tournament_level,notes\n"
    "BENCH-1,6.431,13.31,11.9,yes,yes,,6.500,State,"
    '"tape measure, start line to where it stopped"\n'
    "BENCH-2,6.118,13.42,11.7,no,no,pack sagged mid-run,6.500,State,tape measure\n"
)

_REQUIRED_COLUMN = "run_id"

# The spellings a boolean cell accepts, so "y"/"N"/"TRUE" cannot be silently
# misread as the opposite of what the operator meant.
_TRUE_SPELLINGS = frozenset({"yes", "y", "true", "t", "1"})
_FALSE_SPELLINGS = frozenset({"no", "n", "false", "f", "0"})

# The domain each numeric column is a measurement *of*. A value outside it is a
# corrupt input, not a data point: a negative elapsed time or a non-positive pack
# voltage is not a reading of anything, and letting one through turns an input
# error into a statistic that looks like a measurement. `measured_end_position_m`
# is deliberately unbounded above and below - in a run that reverses, resting
# behind the start line is a real physical outcome, and inventing a floor would
# discard a real measurement.
_MEASUREMENT_DOMAINS: dict[str, tuple[float, bool]] = {
    "measured_run_time_s": (0.0, False),
    "pack_voltage_v": (0.0, True),
    "target_distance_m": (0.0, True),
}

def _finite_number(
    text: str,
    *,
    column: str,
    row_number: int,
    source: str,
) -> float:
    """A finite number, or a named error.

    ``float()`` admits ``nan``, ``inf`` and ``1e309``; each of those would travel
    into a statistic and then into a report as a non-finite value, so they are
    refused here where the input line can still be named.
    """
    try:
        value = float(text)
    except ValueError:
        raise MeasurementError(
            f"{source}:{row_number}: column '{column}' is not a number: {text!r}"
        ) from None
    if not math.isfinite(value):
        raise MeasurementError(
            f"{source}:{row_number}: column '{column}' is not a finite number: {text!r}"
        )
    domain = _MEASUREMENT_DOMAINS.get(column)
    if domain is not None:
        minimum, exclusive = domain
        if value < minimum or (exclusive and value == minimum):
            raise MeasurementError(
                f"{source}:{row_number}: column '{column}' is outside its domain "
                f"(must be {'>' if exclusive else '>='} {minimum:g}): {text!r}"
            )
    return value


class MeasurementError(ValueError):
    """The measurement file is not the file this tool can read."""


@dataclass(frozen=True)
class Measurement:
    """One row of the measurement file. A ``None`` means "not recorded"."""

    run_id: str
    measured_end_position_m: Optional[float]
    measured_run_time_s: Optional[float]
    pack_voltage_v: Optional[float]
    recharged: Optional[bool]
    accepted: Optional[bool]
    exclusion_reason: str
    target_distance_m: Optional[float]
    tournament_level: Optional[str]
    notes: str


@dataclass(frozen=True)
class MeasurementTable:
    """Every row, keyed by run id, with the file's hash for traceability."""

    source: Optional[str]
    sha256: str
    rows: dict[str, Measurement] = field(default_factory=dict)

    def get(self, run_id: str) -> Optional[Measurement]:
        return self.rows.get(run_id)


def _parse_number(
    text: str, *, column: str, row_number: int, source: str
) -> Optional[float]:
    text = text.strip()
    if text == "":
        return None
    return _finite_number(text, column=column, row_number=row_number, source=source)


def _parse_bool(
    text: str, *, column: str, row_number: int, source: str
) -> Optional[bool]:
    text = text.strip()
    if text == "":
        return None
    lowered = text.lower()
    if lowered in _TRUE_SPELLINGS:
        return True
    if lowered in _FALSE_SPELLINGS:
        return False
    raise MeasurementError(
        f"{source}:{row_number}: column '{column}' must be a yes/no value "
        f"(one of yes/no/true/false/1/0, or blank): {text!r}"
    )


def _text(value: Optional[str]) -> str:
    return (value or "").strip()


def load_measurements(path: str) -> MeasurementTable:
    """Reads the measurement CSV.

    Fails clearly on a file this tool cannot read faithfully. Every one of these
    is a case where the alternative is a number that looks like a measurement:

    * a header that is missing ``run_id``, missing every numeric measurement
      column, or carrying a column this tool does not define (a mistyped column
      would otherwise be silently ignored and read as "not measured");
    * a header with the same column name twice (the second one would silently
      replace the first, so which column is read depends on the file's order);
    * a header whose names carry whitespace (` measured_run_time_s`) - the
      surrounding whitespace is normalized away and the *normalized* names are
      the mapping the rows are read through, so a padded header loads like a clean
      one instead of loading as "not measured";
    * a row whose cell count is not the header's (a short row would otherwise
      leave the missing cells ``None``, which reads as "not recorded");
    * a numeric cell that is not finite, or is outside the domain of the column
      it is written under.
    """
    with open(path, "rb") as handle:
        raw_bytes = handle.read()
    raw = raw_bytes.decode("utf-8", errors="replace")
    sha256 = hashlib.sha256(raw_bytes).hexdigest()

    # Comment lines are allowed so the file can explain itself. They are dropped
    # after the CSV reader has split the file, not before, so a quoted value that
    # contains a newline still reads as one cell.
    reader = csv.reader(io.StringIO(raw))
    fieldnames: Optional[list[str]] = None
    rows: dict[str, Measurement] = {}

    for cells in reader:
        if not cells or all(cell.strip() == "" for cell in cells):
            continue
        if cells[0].lstrip().startswith("#"):
            continue
        row_number = reader.line_num

        if fieldnames is None:
            fieldnames = [name.strip() for name in cells]
            seen: list[str] = []
            duplicates: list[str] = []
            for name in fieldnames:
                if name in seen and name not in duplicates:
                    duplicates.append(name)
                seen.append(name)
            if duplicates:
                raise MeasurementError(
                    f"{path}:{row_number}: duplicate column(s) "
                    f"{', '.join(duplicates)} in the header"
                )
            unknown = [name for name in fieldnames if name not in MEASUREMENT_COLUMNS]
            if unknown:
                raise MeasurementError(
                    f"{path}:{row_number}: unknown column(s) {', '.join(unknown)}; "
                    f"allowed columns are {', '.join(MEASUREMENT_COLUMNS)}"
                )
            if _REQUIRED_COLUMN not in fieldnames:
                raise MeasurementError(
                    f"{path}:{row_number}: the header must contain a "
                    f"'{_REQUIRED_COLUMN}' column"
                )
            if not any(name in fieldnames for name in MEASUREMENT_NUMERIC_COLUMNS):
                raise MeasurementError(
                    f"{path}:{row_number}: the header must contain at least one of "
                    f"{', '.join(MEASUREMENT_NUMERIC_COLUMNS)}"
                )
            continue

        if len(cells) != len(fieldnames):
            raise MeasurementError(
                f"{path}:{row_number}: the row has {len(cells)} cell(s) and the "
                f"header has {len(fieldnames)}; every row must carry every column"
            )

        values = dict(zip(fieldnames, cells))
        run_id = (values.get(_REQUIRED_COLUMN) or "").strip()
        if run_id == "":
            raise MeasurementError(f"{path}:{row_number}: '{_REQUIRED_COLUMN}' is empty")
        if run_id in rows:
            raise MeasurementError(f"{path}:{row_number}: duplicate run_id {run_id!r}")
        rows[run_id] = Measurement(
            run_id=run_id,
            measured_end_position_m=_parse_number(
                values.get("measured_end_position_m") or "",
                column="measured_end_position_m",
                row_number=row_number,
                source=path,
            ),
            measured_run_time_s=_parse_number(
                values.get("measured_run_time_s") or "",
                column="measured_run_time_s",
                row_number=row_number,
                source=path,
            ),
            pack_voltage_v=_parse_number(
                values.get("pack_voltage_v") or "",
                column="pack_voltage_v",
                row_number=row_number,
                source=path,
            ),
            recharged=_parse_bool(
                values.get("recharged") or "",
                column="recharged",
                row_number=row_number,
                source=path,
            ),
            accepted=_parse_bool(
                values.get("accepted") or "",
                column="accepted",
                row_number=row_number,
                source=path,
            ),
            exclusion_reason=_text(values.get("exclusion_reason")),
            target_distance_m=_parse_number(
                values.get("target_distance_m") or "",
                column="target_distance_m",
                row_number=row_number,
                source=path,
            ),
            tournament_level=_text(values.get("tournament_level")) or None,
            notes=_text(values.get("notes")),
        )

    if fieldnames is None:
        raise MeasurementError(f"{path}: the file has no header row")

    return MeasurementTable(source=path, sha256=sha256, rows=rows)


def _row_is_synthetic(metrics: dict[str, Any]) -> bool:
    """Whether the run is a generated fixture: header declaration or name."""
    declared = (metrics["capture"].get("provenance_declared") or "")
    if declared.strip().upper().startswith(SYNTHETIC_MARKER):
        return True
    haystack = f"{metrics['run_id']} {metrics['source']['path']}".lower()
    return SYNTHETIC_MARKER.lower() in haystack


def _row_provenance_warning(metrics: dict[str, Any]) -> Optional[str]:
    """The fixture/capture caveat for one endpoint row, if any applies."""
    declared = metrics["capture"].get("provenance_declared")
    conflict = metrics["capture"].get("provenance_conflict")
    if _row_is_synthetic(metrics):
        if conflict:
            return (
                "SYNTHETIC FIXTURE - declared in the capture header, not measured "
                "(a conflicting CAPTURE declaration was also present and was not "
                "honored); not calibration evidence"
            )
        if declared and declared.strip().upper().startswith(SYNTHETIC_MARKER):
            return "SYNTHETIC FIXTURE - declared in the capture header, not measured"
        return "SYNTHETIC FIXTURE - synthetic run id or file name, not measured"
    if declared:
        return (
            f"provenance {declared!r} is declared by the capture itself, not "
            "verified: a CAPTURE label is an untrusted claim, not proof that "
            "hardware measurements occurred"
        )
    return None


def _delta(measured: Optional[float], believed: Optional[float]) -> Optional[float]:
    if measured is None or believed is None:
        return None
    try:
        result = float(measured) - float(believed)
    except (OverflowError, ValueError):
        raise MeasurementError(
            f"the endpoint delta {measured!r} minus {believed!r} is not "
            "representable as a finite number; refusing the comparison rather "
            "than emitting a non-finite delta"
        ) from None
    if result != result or result in (float("inf"), float("-inf")):
        raise MeasurementError(
            f"the endpoint delta {measured!r} minus {believed!r} is not "
            "representable as a finite number; refusing the comparison rather "
            "than emitting a non-finite delta"
        )
    return round(result, 6)


def _capture_comparability(metrics: dict[str, Any]) -> Optional[str]:
    """Why this capture's believed endpoint must not be compared, or ``None``.

    A comparison is eligible only when the whole capture's integrity supports
    attributing the believed endpoint to the one analyzed run. Each condition
    below refuses the comparison outright rather than coloring it:

    * the capture holds more than one session or more than one run, so a caller
      cannot say which run an external measurement was taken on - the analyzer
      picks the last run, but the *pairing* is not the tool's to assert;
    * the selected run recorded no verified final stop, so there is no endpoint to
      compare - the last sample is not a stop position, and comparing a tape
      measure against it would invent an endpoint the firmware never verified;
    * the capture contains a suspected unsegmented clock restart (a backwards
      timestamp that is neither a uint32 wrap nor a few-millisecond
      re-ordering), so records on either side of the discontinuity cannot be
      attributed to one continuous capture;
    * the capture holds an event-only session - a Boot with no samples - which
      the sample-derived session check cannot see, so the available evidence
      does not prove one unambiguous capture/session;
    * the capture was parsed permissively with skipped malformed records, which
      could conceal Boot, run, event, sample or stop-verification evidence, so
      session/run attribution cannot be established from the surviving records.

    Unknown integrity is not verified integrity: every applicable reason is
    reported (joined in a fixed order), and any refusal leaves the numeric
    deltas unavailable rather than presenting an attributable calibration delta.
    Descriptive believed values stay visible; only the comparison is refused.
    """
    selection = metrics["capture"]["selection"]
    reasons: list[str] = []
    # Contradictory verification evidence (R1R-M2) already makes the believed
    # endpoint unavailable, which the endpoint-source check below refuses;
    # the contradiction text travels in `end_position_unavailable_reason`.
    duration_reason = (metrics.get("window") or {}).get("duration_unavailable_reason")
    if duration_reason and metrics["position"]["end_position_source"] == "verified_final_stop":
        reasons.append(
            "the capture's chronology is uncertain "
            f"({duration_reason}); an elapsed run time across it cannot be attributed"
        )
    if selection["mixed_sessions"] or selection["mixed_runs"]:
        reasons.append(
            "the capture holds more than one run ("
            f"sessions={selection['session_count']}, "
            f"runs={selection['runs_in_session']}); an external measurement cannot "
            "be attributed to the analyzed run"
        )
    if metrics["position"]["end_position_source"] != "verified_final_stop":
        # R1G/M11: verification may have latched with no verification-tick
        # position. That run verified a stop but has no attributable stop
        # position - which is not the same as never verifying - so the reason
        # says which one, with the position section's precise attribution.
        if metrics["run_window"]["final_stop_dwell_verified"]:
            reasons.append(
                "the analyzed run recorded stop verification but no verified "
                "stop position is available ("
                f"{metrics['position']['end_position_unavailable_reason']})"
            )
        else:
            reasons.append(
                "the analyzed run recorded no verified final stop ("
                f"{metrics['position']['end_position_unavailable_reason']})"
            )
    restart_count = metrics["capture"].get("restart_suspected_count", 0)
    if restart_count:
        reasons.append(
            f"the capture contains {restart_count} suspected clock restart(s) "
            "(a backwards timestamp that is neither a uint32 wrap nor a "
            "few-millisecond re-ordering); records on either side of the "
            "discontinuity cannot be attributed to one continuous capture"
        )
    event_only_sessions = [
        session["index"]
        for session in metrics["capture"].get("sessions", [])
        if not session["sample_count"]
    ]
    if event_only_sessions:
        reasons.append(
            "the capture holds an event-only session "
            f"(session(s) {event_only_sessions} opened by Boot with no samples); "
            "the available evidence does not prove one unambiguous capture/session"
        )
    parse_errors = metrics["counts"]["parse_errors"]
    if parse_errors:
        reasons.append(
            f"the capture was parsed permissively with {parse_errors} skipped "
            "malformed record(s) that could conceal Boot, run, event, sample or "
            "stop-verification evidence; session/run attribution cannot be "
            "established from the surviving records"
        )
    if reasons:
        return "; ".join(reasons)
    return None


def compare_endpoints(
    metrics_by_run: Sequence[dict[str, Any]],
    measurements: MeasurementTable,
) -> dict[str, Any]:
    """Puts the person's numbers next to the firmware's, run by run.

    A blank cell yields a ``None`` delta rather than a zero one, so "not
    measured" can never read as "measured exactly right".

    A delta is computed only when the whole capture's integrity supports
    attributing the believed endpoint to the one analyzed run
    (``_capture_comparability``): one session, one run, a verified final stop,
    no suspected unsegmented restart, no event-only extra Boot, and no
    permissively skipped malformed records. A capture that cannot be attributed
    keeps the believed numbers visible and reports *why* the comparison was
    refused, because a delta against an unattributable endpoint is exactly the
    confusion this module exists to avoid.
    """
    rows: list[dict[str, Any]] = []
    matched: set[str] = set()
    refusals: list[dict[str, str]] = []

    for metrics in metrics_by_run:
        run_id = metrics["run_id"]
        measurement = measurements.get(run_id)
        if measurement is not None:
            matched.add(run_id)
            measured_end_position = measurement.measured_end_position_m
            measured_time = measurement.measured_run_time_s
            notes = measurement.notes
        else:
            measured_end_position = None
            measured_time = None
            notes = ""

        refusal = _capture_comparability(metrics)
        if refusal is not None:
            refusals.append({"run_id": run_id, "reason": refusal})

        believed_end_position = metrics["position"]["end_position_m"]
        # The firmware's internal Run Time: actuation to the start of the verified
        # final-stop dwell, so the rule's Complete Stop interval is not counted. It
        # is deliberately not substituted from another window: when the run did not
        # verify a final stop, the believed time is unavailable and stays `None`,
        # because an armed-flag span or a whole-log duration is not a Run Time and a
        # number that looks comparable would be worse than a blank.
        believed_time = metrics["run_window"]["internal_run_time_s"]
        try:
            end_delta = _delta(measured_end_position, believed_end_position)
            time_delta = _delta(measured_time, believed_time)
        except MeasurementError as error:
            end_delta = None
            time_delta = None
            refusal = (refusal + "; " if refusal else "") + str(error)
            if not any(entry["run_id"] == run_id for entry in refusals):
                refusals.append({"run_id": run_id, "reason": refusal})
            else:
                for entry in refusals:
                    if entry["run_id"] == run_id:
                        entry["reason"] = refusal
        rows.append(
            {
                "run_id": run_id,
                "has_measurement": measurement is not None,
                "comparable": refusal is None,
                "comparison_refused_reason": refusal,
                "measured_end_position_m": measured_end_position,
                "believed_end_position_m": believed_end_position,
                # `end_position_source` is what makes `believed_end_position_m`
                # readable: the verified final stop, or the last usable sample when
                # there was no verified stop.
                "believed_end_position_source": metrics["position"][
                    "end_position_source"
                ],
                "believed_end_position_sampling_window_m": metrics["position"][
                    "end_position_sampling_window_m"
                ],
                "believed_stop_verified": metrics["run_window"][
                    "final_stop_dwell_verified"
                ],
                "completion": metrics["run_window"]["completion"],
                "capture_session_index": metrics["capture"]["selection"][
                    "selected_session_index"
                ],
                "capture_mixed_sessions": metrics["capture"]["selection"][
                    "mixed_sessions"
                ],
                "capture_mixed_runs": metrics["capture"]["selection"][
                    "mixed_runs"
                ],
                "capture_provenance_declared": metrics["capture"][
                    "provenance_declared"
                ],
                # Normalized fixture provenance travels with the row (R1R-L1):
                # the explicit declaration alone is not the whole answer -
                # a SYNTHETIC run id or file name adds the warning, and a
                # conflicting CAPTURE claim never removes it. A standalone
                # root export must still read as a fixture, never as
                # physically verified calibration evidence.
                "synthetic": _row_is_synthetic(metrics),
                "capture_provenance_conflict": metrics["capture"].get(
                    "provenance_conflict"
                ),
                "provenance_warning": _row_provenance_warning(metrics),
                "end_position_delta_m": end_delta if refusal is None else None,
                "net_travel_m": metrics["position"]["net_travel_m"],
                "measured_run_time_s": measured_time,
                "believed_run_time_s": believed_time,
                "run_time_delta_s": time_delta if refusal is None else None,
                "believed_target_m": metrics["position"]["believed_target_m"],
                "believed_target_source": metrics["position"]["believed_target_source"],
                # The context columns travel with the row so an accepted or
                # excluded run is never separated from the reason.
                "target_distance_m": measurement.target_distance_m
                if measurement is not None
                else None,
                "tournament_level": measurement.tournament_level
                if measurement is not None
                else None,
                "pack_voltage_v": measurement.pack_voltage_v
                if measurement is not None
                else None,
                "recharged": measurement.recharged if measurement is not None else None,
                "accepted": measurement.accepted if measurement is not None else None,
                "exclusion_reason": measurement.exclusion_reason
                if measurement is not None
                else "",
                "notes": notes,
            }
        )

    return {
        "source": measurements.source,
        "source_sha256": measurements.sha256,
        "note": (
            "descriptive comparison only: measured values come from a person, believed "
            "values come from the firmware, and a difference between them is not by "
            "itself a conclusion about the vehicle"
        ),
        "columns": list(MEASUREMENT_COLUMNS),
        "runs": rows,
        "measured_rows_without_a_run": sorted(set(measurements.rows) - matched),
        "runs_without_a_measurement": [
            row["run_id"] for row in rows if not row["has_measurement"]
        ],
        "refused_comparisons": refusals,
    }
