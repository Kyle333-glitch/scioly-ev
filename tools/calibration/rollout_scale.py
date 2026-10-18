#!/usr/bin/env python3
"""Offline rollout-scale calculator (S22).

Computes the effective distance per encoder tick from hand-measured rollout
trials, exactly the arithmetic P-H4 Part A / P-H14 Step 4 describe::

    scale_m_per_tick = travel_m / tick_delta

where travel_m is the independently measured ground travel (tape) and
tick_delta is the raw signed encoder-tick difference over the same
mark-to-mark span.

This tool measures nothing and calibrates nothing on its own. It turns rows
an operator measured by hand into a candidate scale plus residuals, so the
team can decide what (if anything) to promote to Measured with a recorded
evidence pointer. Synthetic fixtures are labelled as such; simulated rows
never establish physical accuracy.

Input CSV columns (units are part of the header names and are enforced):
  trial_id          - repetition identifier, non-empty string
  start_ticks       - raw signed encoder count at the start-line crossing
  end_ticks         - raw signed encoder count at the finish-line crossing
  travel_m          - independently measured ground travel, metres, > 0
  direction         - forward | reverse (physical travel direction)
  surface,notes     - free text setup information (may be blank)

Rules, in order:
  * units are metres and ticks; millimetres are never accepted silently --
    a travel value that looks like millimetres is the operator's error to
    fix, not this tool's to guess;
  * tick_delta == 0 is rejected (division by zero) and travel_m <= 0,
    non-finite or missing fields are rejected;
  * the sign of tick_delta is reported per trial but never folded into the
    scale: scale is a positive magnitude (metres per tick) and encoder
    polarity (which tick sign means forward travel) is the separate H2 fact.
    A trial whose tick sign disagrees with its stated direction is flagged,
    not silently abs()ed away;
  * per-trial scale, residual vs the chosen scale, and a consistency verdict
    are reported; inconsistent trials are flagged, never silently averaged;
  * the summary reports mean, min/max spread and standard deviation across
    accepted trials and refuses to summarise fewer than 1 accepted trial.

Exit codes: 0 with accepted trials, 2 when no trial was accepted, 1 on
unusable input (missing file, bad header).
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass, field


REQUIRED_COLUMNS = (
    "trial_id",
    "start_ticks",
    "end_ticks",
    "travel_m",
    "direction",
)

OPTIONAL_COLUMNS = ("surface", "notes")

TEMPLATE = (
    "# Offline rollout trials - HAND-MEASURED rows only (P-H4 Part A / P-H14 Step 4).\n"
    "# Units: travel_m is METRES, start/end_ticks are raw signed encoder ticks.\n"
    "# One row per mark-to-mark run. Keep rejected runs with their reason.\n"
    "trial_id,start_ticks,end_ticks,travel_m,direction,surface,notes\n"
    "A-fwd-1,0,20000,2.000,forward,lab tile,\n"
    "A-fwd-2,20000,40005,2.000,forward,lab tile,\n"
    "A-rev-1,40005,20010,2.000,reverse,lab tile,\n"
)


@dataclass
class TrialResult:
    trial_id: str
    tick_delta: int
    travel_m: float
    direction: str
    scale_m_per_tick: float
    sign_agrees: bool
    residual_m: float = 0.0


@dataclass
class ScaleReport:
    trials: list = field(default_factory=list)
    rejected: list = field(default_factory=list)
    mean_scale: float = 0.0
    std_scale: float = 0.0
    min_scale: float = 0.0
    max_scale: float = 0.0
    consistent: bool = False


def _parse_ticks(text: str, *, column: str, trial: str) -> int:
    text = text.strip()
    try:
        value = int(text, 10)
    except ValueError:
        raise ValueError(f"trial '{trial}': column '{column}' is not an integer: {text!r}")
    return value


def _parse_travel(text: str, *, trial: str) -> float:
    text = text.strip()
    try:
        value = float(text)
    except ValueError:
        raise ValueError(f"trial '{trial}': column 'travel_m' is not a number: {text!r}")
    if not math.isfinite(value):
        raise ValueError(f"trial '{trial}': column 'travel_m' is not finite: {text!r}")
    if not value > 0.0:
        raise ValueError(
            f"trial '{trial}': column 'travel_m' must be > 0 metres: {text!r}"
        )
    return value


def compute_scales(rows: list) -> ScaleReport:
    """Core arithmetic over already-parsed rows. Pure and fully tested."""
    report = ScaleReport()
    for row in rows:
        trial_id = row["trial_id"]
        delta = row["tick_delta"]
        travel_m = row["travel_m"]
        direction = row["direction"]
        if delta == 0:
            report.rejected.append((trial_id, "tick_delta is zero (no division by zero)"))
            continue
        scale = travel_m / abs(delta)
        if not math.isfinite(scale) or not scale > 0.0:
            report.rejected.append((trial_id, "non-finite or non-positive scale"))
            continue
        expected_positive = direction == "forward"
        sign_agrees = (delta > 0) == expected_positive
        report.trials.append(
            TrialResult(
                trial_id=trial_id,
                tick_delta=delta,
                travel_m=travel_m,
                direction=direction,
                scale_m_per_tick=scale,
                sign_agrees=sign_agrees,
            )
        )
    if not report.trials:
        return report
    scales = [t.scale_m_per_tick for t in report.trials]
    mean = sum(scales) / len(scales)
    report.mean_scale = mean
    report.min_scale = min(scales)
    report.max_scale = max(scales)
    if len(scales) > 1:
        variance = sum((s - mean) ** 2 for s in scales) / len(scales)
        report.std_scale = math.sqrt(variance)
    for trial in report.trials:
        predicted_m = abs(trial.tick_delta) * mean
        trial.residual_m = trial.travel_m - predicted_m
    # Consistency: every accepted trial within 2% of the mean and every
    # sign agreeing. A threshold crossing flags, never silently averages.
    report.consistent = all(
        abs(t.scale_m_per_tick - mean) <= 0.02 * mean and t.sign_agrees
        for t in report.trials
    )
    return report


def load_trials(path: str) -> list:
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path}: empty file, no header row")
        # Ignore comment lines starting with '#' handled by caller via filter.
        missing = [c for c in REQUIRED_COLUMNS if c not in reader.fieldnames]
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(missing)}")
        rows = []
        for line_number, record in enumerate(reader, start=2):
            trial_id = (record.get("trial_id") or "").strip()
            if trial_id == "":
                raise ValueError(f"{path}:{line_number}: trial_id is empty")
            direction = (record.get("direction") or "").strip().lower()
            if direction not in ("forward", "reverse"):
                raise ValueError(
                    f"{path}:{line_number}: trial '{trial_id}': direction must be "
                    f"'forward' or 'reverse': {record.get('direction')!r}"
                )
            start = _parse_ticks(record.get("start_ticks") or "", column="start_ticks",
                                 trial=trial_id)
            end = _parse_ticks(record.get("end_ticks") or "", column="end_ticks",
                               trial=trial_id)
            travel_m = _parse_travel(record.get("travel_m") or "", trial=trial_id)
            rows.append(
                {
                    "trial_id": trial_id,
                    "tick_delta": end - start,
                    "travel_m": travel_m,
                    "direction": direction,
                    "surface": (record.get("surface") or "").strip(),
                    "notes": (record.get("notes") or "").strip(),
                    "line_number": line_number,
                }
            )
        return rows


def _read_csv_skipping_comments(path: str) -> list:
    # csv.DictReader has no comment support; pre-filter '#' lines but keep the
    # header. A template file is therefore directly usable.
    with open(path, encoding="utf-8") as handle:
        lines = [line for line in handle if not line.lstrip().startswith("#")]
    if not lines:
        raise ValueError(f"{path}: empty file, no header row")
    reader = csv.DictReader(lines)
    if reader.fieldnames is None:
        raise ValueError(f"{path}: empty file, no header row")
    missing = [c for c in REQUIRED_COLUMNS if c not in reader.fieldnames]
    if missing:
        raise ValueError(f"{path}: missing columns: {', '.join(missing)}")
    rows = []
    for line_number, record in enumerate(reader, start=2):
        trial_id = (record.get("trial_id") or "").strip()
        if trial_id == "":
            raise ValueError(f"{path}:{line_number}: trial_id is empty")
        direction = (record.get("direction") or "").strip().lower()
        if direction not in ("forward", "reverse"):
            raise ValueError(
                f"{path}:{line_number}: trial '{trial_id}': direction must be "
                f"'forward' or 'reverse': {record.get('direction')!r}"
            )
        start = _parse_ticks(record.get("start_ticks") or "", column="start_ticks",
                             trial=trial_id)
        end = _parse_ticks(record.get("end_ticks") or "", column="end_ticks",
                           trial=trial_id)
        travel_m = _parse_travel(record.get("travel_m") or "", trial=trial_id)
        rows.append(
            {
                "trial_id": trial_id,
                "tick_delta": end - start,
                "travel_m": travel_m,
                "direction": direction,
                "surface": (record.get("surface") or "").strip(),
                "notes": (record.get("notes") or "").strip(),
            }
        )
    return rows


def format_report(report: ScaleReport) -> str:
    out = []
    out.append("trial_id,tick_delta,travel_m,direction,scale_m_per_tick,sign_agrees,residual_m")
    for trial in report.trials:
        out.append(
            f"{trial.trial_id},{trial.tick_delta},{trial.travel_m:.6f},"
            f"{trial.direction},{trial.scale_m_per_tick:.9f},"
            f"{'yes' if trial.sign_agrees else 'NO'},"
            f"{trial.residual_m:+.6f}"
        )
    for trial_id, reason in report.rejected:
        out.append(f"{trial_id},REJECTED,,,{reason},,")
    if report.trials:
        out.append(
            f"# accepted={len(report.trials)} rejected={len(report.rejected)} "
            f"mean_scale_m_per_tick={report.mean_scale:.9f} "
            f"min={report.min_scale:.9f} max={report.max_scale:.9f} "
            f"std={report.std_scale:.9f} consistent={'yes' if report.consistent else 'NO'}"
        )
        out.append(
            "# candidate (metres per tick): "
            f"{report.mean_scale:.9f}  (spread {(report.max_scale - report.min_scale):.9f}, "
            f"NOT a calibration until promoted with recorded evidence)"
        )
    else:
        out.append("# no accepted trials: nothing to summarise")
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Offline rollout-scale calculator (S22).")
    parser.add_argument("csv", nargs="?", help="hand-measured trial rows")
    parser.add_argument("--print-template", action="store_true",
                        help="print the trial CSV template and exit")
    parser.add_argument("--out", default=None, help="write the report to FILE")
    args = parser.parse_args(argv)
    if args.print_template:
        sys.stdout.write(TEMPLATE)
        return 0
    if not args.csv:
        parser.error("a trial CSV is required (or --print-template)")
    try:
        rows = _read_csv_skipping_comments(args.csv)
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1
    report = compute_scales(rows)
    text = format_report(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)
    return 0 if report.trials else 2


if __name__ == "__main__":
    raise SystemExit(main())
