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
from dataclasses import dataclass, field
from typing import Any, Optional, Sequence

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
    try:
        return float(text)
    except ValueError:
        raise MeasurementError(
            f"{source}:{row_number}: column '{column}' is not a number: {text!r}"
        ) from None


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

    Fails clearly on a header that is missing ``run_id``, missing every numeric
    measurement column, or carrying a column this tool does not define - a
    mistyped column would otherwise be silently ignored and read as "not
    measured", which is the same failure mode as losing a record.
    """
    with open(path, "rb") as handle:
        raw_bytes = handle.read()
    raw = raw_bytes.decode("utf-8", errors="replace")
    sha256 = hashlib.sha256(raw_bytes).hexdigest()

    # Comment lines are allowed so the file can explain itself.
    body = "".join(
        line for line in raw.splitlines(keepends=True) if not line.lstrip().startswith("#")
    )
    reader = csv.DictReader(io.StringIO(body))
    if reader.fieldnames is None:
        raise MeasurementError(f"{path}: the file has no header row")

    fieldnames = [name.strip() for name in reader.fieldnames]
    unknown = [name for name in fieldnames if name not in MEASUREMENT_COLUMNS]
    if unknown:
        raise MeasurementError(
            f"{path}: unknown column(s) {', '.join(unknown)}; "
            f"allowed columns are {', '.join(MEASUREMENT_COLUMNS)}"
        )
    if _REQUIRED_COLUMN not in fieldnames:
        raise MeasurementError(
            f"{path}: the header must contain a '{_REQUIRED_COLUMN}' column"
        )
    if not any(name in fieldnames for name in MEASUREMENT_NUMERIC_COLUMNS):
        raise MeasurementError(
            f"{path}: the header must contain at least one of "
            f"{', '.join(MEASUREMENT_NUMERIC_COLUMNS)}"
        )

    rows: dict[str, Measurement] = {}
    for row_number, row in enumerate(reader, start=2):
        run_id = (row.get(_REQUIRED_COLUMN) or "").strip()
        if run_id == "":
            raise MeasurementError(f"{path}:{row_number}: '{_REQUIRED_COLUMN}' is empty")
        if run_id in rows:
            raise MeasurementError(f"{path}:{row_number}: duplicate run_id {run_id!r}")
        rows[run_id] = Measurement(
            run_id=run_id,
            measured_end_position_m=_parse_number(
                row.get("measured_end_position_m") or "",
                column="measured_end_position_m",
                row_number=row_number,
                source=path,
            ),
            measured_run_time_s=_parse_number(
                row.get("measured_run_time_s") or "",
                column="measured_run_time_s",
                row_number=row_number,
                source=path,
            ),
            pack_voltage_v=_parse_number(
                row.get("pack_voltage_v") or "",
                column="pack_voltage_v",
                row_number=row_number,
                source=path,
            ),
            recharged=_parse_bool(
                row.get("recharged") or "",
                column="recharged",
                row_number=row_number,
                source=path,
            ),
            accepted=_parse_bool(
                row.get("accepted") or "",
                column="accepted",
                row_number=row_number,
                source=path,
            ),
            exclusion_reason=_text(row.get("exclusion_reason")),
            target_distance_m=_parse_number(
                row.get("target_distance_m") or "",
                column="target_distance_m",
                row_number=row_number,
                source=path,
            ),
            tournament_level=_text(row.get("tournament_level")) or None,
            notes=_text(row.get("notes")),
        )

    return MeasurementTable(source=path, sha256=sha256, rows=rows)


def _delta(measured: Optional[float], believed: Optional[float]) -> Optional[float]:
    if measured is None or believed is None:
        return None
    return round(measured - believed, 6)


def compare_endpoints(
    metrics_by_run: Sequence[dict[str, Any]],
    measurements: MeasurementTable,
) -> dict[str, Any]:
    """Puts the person's numbers next to the firmware's, run by run.

    A blank cell yields a ``None`` delta rather than a zero one, so "not
    measured" can never read as "measured exactly right".
    """
    rows: list[dict[str, Any]] = []
    matched: set[str] = set()

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

        believed_end_position = metrics["position"]["end_position_m"]
        believed_time = metrics["run_window"]["armed_duration_s"]
        if believed_time is None:
            believed_time = metrics["window"]["log_duration_s"]

        rows.append(
            {
                "run_id": run_id,
                "has_measurement": measurement is not None,
                "measured_end_position_m": measured_end_position,
                "believed_end_position_m": believed_end_position,
                "end_position_delta_m": _delta(measured_end_position, believed_end_position),
                "net_travel_m": metrics["position"]["net_travel_m"],
                "measured_run_time_s": measured_time,
                "believed_run_time_s": believed_time,
                "run_time_delta_s": _delta(measured_time, believed_time),
                "believed_target_m": metrics["position"]["believed_target_m"],
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
    }
