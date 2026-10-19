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
    polarity (which tick sign means forward travel) is established from the
    data itself -- forward-positive is NEVER assumed. A direction group with
    a strict majority sign sets the expectation for that direction; the two
    directions must oppose (H2: forward and reverse have opposite tick
    signs). A minority-sign trial in an established direction is EXCLUDED
    from the mean, spread, residuals and per-direction estimates -- it is
    listed for review, never silently averaged in;
  * per-trial scale, residual vs the chosen scale, and a verdict per trial
    (included / excluded-sign / rejected) are reported;
  * per-direction trial counts and scale estimates are reported, and the
    forward/reverse difference is called out separately from ordinary
    within-direction scatter. H4 prescribes no numeric acceptance band, so
    the 2% figure below is an explicitly ADVISORY warning threshold, never
    an acceptance criterion: acceptance is a pre-registered external band
    plus a recorded evidence pointer (P-H4 / P-H14 Step 4);
  * the summary reports mean, min/max spread and standard deviation across
    INCLUDED trials only. Standard deviation is undefined for fewer than 2
    trials and is reported as n/a -- a single trial never looks certain;
  * questionable results get status REVIEW_REQUIRED, which is explicitly
    non-promotable. This tool never writes firmware and never promotes a
    value; promotion happens only through the H4 procedure with recorded
    evidence.

Exit codes: 0 with included (or descriptive) trials, 2 when no trial was
usable, 1 on unusable input (missing file, bad header).
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


# Advisory-only warning thresholds. These flag data for human review; they
# are NOT H4/P-H14 acceptance criteria (acceptance is a pre-registered
# external band recorded before the session, plus an evidence pointer).
ADVISORY_SPREAD_BAND = 0.02
ADVISORY_DIRECTION_BAND = 0.02


@dataclass
class TrialResult:
    trial_id: str
    tick_delta: int
    travel_m: float
    direction: str
    scale_m_per_tick: float
    sign_agrees: bool
    residual_m: float = 0.0
    verdict: str = "included"  # "included" | "excluded_sign"
    exclude_reason: str = ""


@dataclass
class ScaleReport:
    trials: list = field(default_factory=list)  # included in statistics
    excluded: list = field(default_factory=list)  # sign-inconsistent, NOT averaged
    rejected: list = field(default_factory=list)  # invalid, NOT averaged
    mean_scale: float = 0.0  # over included trials only
    std_scale: object = None  # None when fewer than 2 included trials (n/a)
    min_scale: float = 0.0
    max_scale: float = 0.0
    consistent: bool = False  # legacy alias of promotable; see candidate_status
    polarity: str = "undetermined"
    # "forward-positive" | "forward-negative" | "undetermined" | "contradictory"
    polarity_established: bool = False
    candidate_status: str = "REVIEW_REQUIRED"  # "CANDIDATE" | "REVIEW_REQUIRED"
    promotable: bool = False
    status_reasons: list = field(default_factory=list)
    n_forward: int = 0
    n_reverse: int = 0
    mean_forward: object = None
    mean_reverse: object = None
    direction_diff_m_per_tick: object = None
    direction_diff_rel: object = None


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


def _majority_sign(deltas: list) -> int | None:
    """Strict-majority tick sign of a direction group: +1, -1, or None on tie."""
    pos = sum(1 for d in deltas if d > 0)
    neg = sum(1 for d in deltas if d < 0)
    if pos > neg:
        return +1
    if neg > pos:
        return -1
    return None


def compute_scales(rows: list) -> ScaleReport:
    """Core arithmetic over already-parsed rows. Pure and fully tested.

    Polarity is established from the data (strict-majority sign per
    direction; the two directions must oppose). Forward-positive is never
    assumed, so a rig where forward travel yields negative ticks is handled.
    Trials contradicting their direction's established majority are excluded
    from every statistic and listed for review.
    """
    report = ScaleReport()
    valid: list = []
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
        valid.append(
            TrialResult(
                trial_id=trial_id,
                tick_delta=delta,
                travel_m=travel_m,
                direction=direction,
                scale_m_per_tick=scale,
                sign_agrees=False,  # decided below against the majority sign
            )
        )
    if not valid:
        report.status_reasons.append("no usable trials")
        return report

    fwd = [t for t in valid if t.direction == "forward"]
    rev = [t for t in valid if t.direction == "reverse"]
    fwd_sign = _majority_sign([t.tick_delta for t in fwd]) if fwd else None
    rev_sign = _majority_sign([t.tick_delta for t in rev]) if rev else None

    # A lone trial trivially agrees with itself, but one trial cannot
    # establish polarity: that needs corroboration (n >= 2 with a strict
    # majority, and opposition where both directions are present). A missing
    # direction is "untested", not a tie -- only a direction that HAS trials
    # but no strict majority blocks establishment.
    fwd_tied = bool(fwd) and fwd_sign is None
    rev_tied = bool(rev) and rev_sign is None
    if len(valid) < 2:
        report.polarity = "undetermined"
        report.status_reasons.append(
            "single trial cannot establish polarity or spread -- "
            "insufficient evidence, not a candidate"
        )
        valid[0].sign_agrees = True  # unanimous group of one; still uncorroborated
        report.trials.append(valid[0])
    elif fwd_tied or rev_tied:
        # Cannot tell which trial is wrong, so exclude nothing and promote
        # nothing.
        report.polarity = "undetermined"
        report.status_reasons.append(
            "tied tick signs within a direction -- polarity undetermined, "
            "no trial excluded, nothing is a candidate"
        )
        for trial in valid:
            report.trials.append(trial)
    elif fwd_sign is not None and rev_sign is not None and fwd_sign == rev_sign:
        report.polarity = "contradictory"
        report.polarity_established = False
        report.status_reasons.append(
            "forward and reverse majority tick signs agree "
            f"({'positive' if fwd_sign > 0 else 'negative'}) -- polarity contradictory, "
            "no trial excluded, nothing is a candidate"
        )
        for trial in valid:
            trial.sign_agrees = False
            report.trials.append(trial)  # descriptive only; not a candidate
    else:
        # One bit of information: either direction's majority sign fixes
        # polarity under the H2 opposition rule (reverse-negative implies
        # forward-positive and vice versa). No assumed forward sign anywhere.
        if fwd_sign is not None:
            forward_is_positive = fwd_sign > 0
        else:
            forward_is_positive = rev_sign < 0
        report.polarity = (
            "forward-positive" if forward_is_positive else "forward-negative"
        )
        report.polarity_established = True
        if fwd and not rev:
            report.status_reasons.append("note: reverse direction untested in this file")
        if rev and not fwd:
            report.status_reasons.append("note: forward direction untested in this file")
        for trial in valid:
            expected = fwd_sign if trial.direction == "forward" else rev_sign
            agrees = (trial.tick_delta > 0) if expected > 0 else (trial.tick_delta < 0)
            trial.sign_agrees = agrees
            if agrees:
                report.trials.append(trial)
            else:
                trial.verdict = "excluded_sign"
                trial.exclude_reason = (
                    f"tick sign disagrees with the {trial.direction} majority "
                    f"({'+' if expected > 0 else '-'}); excluded from the candidate"
                )
                report.excluded.append(trial)
        if report.excluded:
            report.status_reasons.append(
                f"{len(report.excluded)} sign-inconsistent trial(s) excluded "
                "from the candidate (see EXCLUDED rows)"
            )

    if not report.trials:
        report.status_reasons.append("no included trials")
        return report

    # Statistics cover INCLUDED trials only. Excluded trials never move the mean.
    scales = [t.scale_m_per_tick for t in report.trials]
    mean = sum(scales) / len(scales)
    report.mean_scale = mean
    report.min_scale = min(scales)
    report.max_scale = max(scales)
    if len(scales) > 1:
        variance = sum((s - mean) ** 2 for s in scales) / len(scales)
        report.std_scale = math.sqrt(variance)
    else:
        report.std_scale = None  # undefined for n < 2; never reported as 0
    for trial in report.trials:
        predicted_m = abs(trial.tick_delta) * mean
        trial.residual_m = trial.travel_m - predicted_m

    fwd_in = [t for t in report.trials if t.direction == "forward"]
    rev_in = [t for t in report.trials if t.direction == "reverse"]
    report.n_forward = len(fwd_in)
    report.n_reverse = len(rev_in)
    if fwd_in:
        report.mean_forward = sum(t.scale_m_per_tick for t in fwd_in) / len(fwd_in)
    if rev_in:
        report.mean_reverse = sum(t.scale_m_per_tick for t in rev_in) / len(rev_in)
    if fwd_in and rev_in:
        diff = abs(report.mean_forward - report.mean_reverse)
        report.direction_diff_m_per_tick = diff
        report.direction_diff_rel = diff / mean if mean > 0 else None

    # Advisory (NOT acceptance) checks. Acceptance needs the pre-registered
    # H4 band plus an evidence record; this tool only warns and withholds.
    if report.polarity_established and not report.excluded:
        spread_bad = any(
            abs(t.scale_m_per_tick - mean) > ADVISORY_SPREAD_BAND * mean
            for t in report.trials
        )
        if spread_bad:
            report.status_reasons.append(
                f"trial spread exceeds the advisory {ADVISORY_SPREAD_BAND:.0%} band "
                "(advisory only, not the H4 criterion) -- review required"
            )
        elif report.direction_diff_rel is not None and \
                report.direction_diff_rel > ADVISORY_DIRECTION_BAND:
            report.status_reasons.append(
                f"forward/reverse means differ by {report.direction_diff_rel:.3%} "
                f"(advisory band {ADVISORY_DIRECTION_BAND:.0%}; H4 sets no numeric "
                "band -- compare against Part C hysteresis, do not average "
                "across a systematic difference)"
            )
        elif len(report.trials) < 2:
            report.status_reasons.append("fewer than 2 included trials")
    if report.rejected:
        report.status_reasons.append(
            f"note: {len(report.rejected)} invalid trial(s) rejected "
            "(see REJECTED rows; they never entered the statistics)"
        )

    promotable = (
        report.polarity_established
        and not report.excluded
        and report.polarity not in ("contradictory", "undetermined")
        and len(report.trials) >= 2
        and all(
            abs(t.scale_m_per_tick - mean) <= ADVISORY_SPREAD_BAND * mean
            for t in report.trials
        )
        and not (
            report.direction_diff_rel is not None
            and report.direction_diff_rel > ADVISORY_DIRECTION_BAND
        )
    )
    # Strip the informational single-direction notes from blocking reasons:
    # they describe coverage, not a defect, and must not read as failures.
    report.status_reasons = [
        r for r in report.status_reasons if not r.startswith("note:")
    ] + [r for r in report.status_reasons if r.startswith("note:")]
    if promotable:
        report.candidate_status = "CANDIDATE"
        report.promotable = True
        report.consistent = True
    else:
        report.candidate_status = "REVIEW_REQUIRED"
        report.promotable = False
        report.consistent = False
        if not any(not r.startswith("note:") for r in report.status_reasons):
            report.status_reasons.insert(0, "withheld for review (see reasons)")
    return report


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


def _fmt_opt(value: object) -> str:
    return f"{value:.9f}" if isinstance(value, float) else "n/a"


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
    for trial in report.excluded:
        out.append(
            f"{trial.trial_id},EXCLUDED,{trial.tick_delta},"
            f"{trial.direction},{trial.exclude_reason},,"
        )
    for trial_id, reason in report.rejected:
        out.append(f"{trial_id},REJECTED,,,{reason},,")
    if report.trials:
        std_text = (
            f"{report.std_scale:.9f}" if isinstance(report.std_scale, float)
            else "n/a (fewer than 2 included trials; spread is unknown, not zero)"
        )
        out.append(
            f"# included={len(report.trials)} excluded={len(report.excluded)} "
            f"rejected={len(report.rejected)} "
            f"mean_scale_m_per_tick={report.mean_scale:.9f} "
            f"min={report.min_scale:.9f} max={report.max_scale:.9f} "
            f"std={std_text} consistent={'yes' if report.consistent else 'NO'}"
        )
        out.append(
            f"# polarity={report.polarity} "
            f"(established={'yes' if report.polarity_established else 'NO'}; "
            "derived from the data, never assumed; H2 owns the rig convention)"
        )
        out.append(
            "# forward: "
            f"n={report.n_forward} mean={_fmt_opt(report.mean_forward)}  "
            "reverse: "
            f"n={report.n_reverse} mean={_fmt_opt(report.mean_reverse)}  "
            "direction_diff="
            f"{_fmt_opt(report.direction_diff_m_per_tick)} m/tick"
            + (
                f" (rel {report.direction_diff_rel:.3%}; advisory band "
                f"{ADVISORY_DIRECTION_BAND:.0%} -- H4 sets no numeric band)"
                if isinstance(report.direction_diff_rel, float) else ""
            )
        )
        if report.candidate_status == "CANDIDATE":
            out.append(
                "# STATUS=CANDIDATE (advisory checks passed): "
                f"{report.mean_scale:.9f} m/tick  "
                f"(spread {(report.max_scale - report.min_scale):.9f}).  "
                "NOT a calibration and NOT promoted to firmware: promotion "
                "requires the pre-registered H4 acceptance band plus a "
                "recorded evidence pointer (P-H4 / P-H14 Step 4)."
            )
        else:
            reasons = "; ".join(report.status_reasons) or "withheld for review"
            out.append(
                "# STATUS=REVIEW_REQUIRED (NOT a candidate -- do not promote "
                f"to firmware): {reasons}.  Descriptive mean over included "
                f"trials {report.mean_scale:.9f} m/tick is shown for diagnosis "
                "only."
            )
    else:
        reasons = "; ".join(report.status_reasons) or "no usable trials"
        out.append(f"# no included trials: nothing to summarise ({reasons})")
        for reason in report.status_reasons:
            out.append(f"# reason: {reason}")
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
