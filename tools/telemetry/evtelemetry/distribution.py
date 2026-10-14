"""Descriptive statistics across a *series* of hand measurements.

One endpoint comparison answers "did this run stop where it believed it did?".
A series answers the question a competition build actually needs: "is the error
systematic, and is it repeatable?" ``AGENTS.md`` section 8 asks for exactly that
split, and section 19 lists the arithmetic behind it - mean, median, standard
deviation, min/max, repeatability - computed from the raw measurements with the
raw measurements kept separate.

This module is that arithmetic and nothing more. It reads the same measurement
CSV ``endpoints`` reads and reports:

* the distribution of every numeric column, over the runs that were **accepted**
  into the series (an excluded run is listed with its reason, not averaged in);
* the residual of where the vehicle stopped against the target it was sent to,
  which is the systematic offset (the mean) plus the random spread (the standard
  deviation) that ``AGENTS.md`` section 8 describes;
* the same numbers split by tournament level and target, because mixing targets
  in one mean hides a scale error behind a zero error.

Three properties are deliberate:

* **excluded runs stay in the record.** A run judged unacceptable is still a run
  that happened; the payload lists it with its reason rather than dropping it,
  because a file that only contains the good runs is a file that cannot explain
  itself;
* **no conclusion.** "The vehicle overruns by 31 mm" is a statement about the
  vehicle and belongs in ``VERIFIED_STATE.md`` after physical testing. This
  module produces numbers from a person's measurements; it does not name a cause;
* **no target is invented.** If a run's target was not recorded, its residual is
  simply not computed, rather than assumed to be the event target.
"""

from __future__ import annotations

import decimal
import math
import statistics
from typing import Any, Optional, Sequence

from .endpoints import MEASUREMENT_NUMERIC_COLUMNS, Measurement, MeasurementTable

__all__ = [
    "DISTRIBUTION_NUMERIC_COLUMNS",
    "DistributionError",
    "describe_values",
    "measurement_distribution",
]


class DistributionError(ValueError):
    """A series whose derived statistics are not representable."""

# The numeric columns a distribution is reported for. The first two are the
# measured facts; the last two are the context that makes them interpretable.
DISTRIBUTION_NUMERIC_COLUMNS: tuple[str, ...] = MEASUREMENT_NUMERIC_COLUMNS

_ROUND_DIGITS = 6


def _round(value: Optional[float], digits: int = _ROUND_DIGITS) -> Optional[float]:
    """Rounds for output so the report is byte-reproducible (and never -0.0)."""
    if value is None:
        return None
    return float(round(float(value), digits)) + 0.0


def _as_finite_float(value: decimal.Decimal, *, statistic: str) -> float:
    """A Decimal statistic as a finite float, or a named refusal.

    A derived quantity can be genuinely unrepresentable in binary64 (e.g. the
    range of ``[-1e308, 1e308]`` is ~2e616): emitting ``inf`` would corrupt
    strict JSON downstream, and clamping would invent a number, so the
    series is refused with the statistic named.
    """
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        raise DistributionError(
            f"the {statistic} of this measurement series is not representable "
            f"as a finite number ({value}); refusing the distribution rather "
            "than emitting a non-finite statistic"
        ) from None
    if not math.isfinite(result):
        raise DistributionError(
            f"the {statistic} of this measurement series is not representable "
            f"as a finite number ({value}); refusing the distribution rather "
            "than emitting a non-finite statistic"
        )
    return result


def _describe_exact(ordered: list[float]) -> dict[str, Any]:
    """Statistics via exact decimal arithmetic (overflow fallback).

    Reached only when the float path overflowed or went non-finite, so
    ordinary series keep their exact historical float behavior. ``repr`` of
    each float round-trips the binary value into Decimal, making the sum,
    mean, median, variance, range and deviation exact; each derived quantity
    that still does not fit in a float refuses by name instead of becoming
    ``inf``. No clamping, no silent record loss, no invented bound.
    """
    count = len(ordered)
    stepped = [decimal.Decimal(repr(value)) for value in ordered]
    total = sum(stepped, decimal.Decimal(0))
    mean_d = total / count
    middle = count // 2
    if count % 2:
        median_d = stepped[middle]
    else:
        median_d = (stepped[middle - 1] + stepped[middle]) / 2
    if count >= 2:
        variance_d = (
            sum((value - mean_d) ** 2 for value in stepped) / (count - 1)
        )
        stdev_d = variance_d.sqrt()
        stdev = _round(_as_finite_float(stdev_d, statistic="standard deviation"))
    else:
        stdev = None
    range_d = stepped[-1] - stepped[0]
    deviation_d = max(abs(value - mean_d) for value in stepped)
    return {
        "count": count,
        "min": _round(ordered[0]),
        "max": _round(ordered[-1]),
        "mean": _round(_as_finite_float(mean_d, statistic="mean")),
        "median": _round(_as_finite_float(median_d, statistic="median")),
        "stdev": stdev,
        "range": _round(_as_finite_float(range_d, statistic="range")),
        "max_abs_deviation_from_mean": _round(
            _as_finite_float(deviation_d, statistic="maximum deviation")
        ),
    }


def describe_values(values: Sequence[float]) -> dict[str, Any]:
    """Descriptive statistics for one series.

    ``stdev`` is the **sample** standard deviation (``n - 1``): these
    measurements are a sample of the vehicle's behaviour, not the whole
    population of possible runs. With one observation it is undefined and is
    reported as ``None`` rather than as a confident zero.

    Finite but extreme inputs (around 1e308) use exact decimal arithmetic
    when the float path overflows: a representable result (mean 1e308,
    zero spread of identical extremes) is computed accurately, while a
    genuinely unrepresentable one refuses with a controlled
    :class:`DistributionError` instead of an uncaught ``OverflowError`` or
    a non-finite JSON value.
    """
    ordered = sorted(float(value) for value in values)
    count = len(ordered)
    if count == 0:
        return {
            "count": 0,
            "min": None,
            "max": None,
            "mean": None,
            "median": None,
            "stdev": None,
            "range": None,
            "max_abs_deviation_from_mean": None,
        }
    try:
        mean = statistics.fmean(ordered)
        median = statistics.median(ordered)
        stdev_raw = statistics.stdev(ordered) if count >= 2 else None
        span = ordered[-1] - ordered[0]
        deviation = max(abs(value - mean) for value in ordered)
        candidates = [mean, median, span, deviation]
        if stdev_raw is not None:
            candidates.append(stdev_raw)
        if all(math.isfinite(candidate) for candidate in candidates):
            return {
                "count": count,
                "min": _round(ordered[0]),
                "max": _round(ordered[-1]),
                "mean": _round(mean),
                "median": _round(median),
                "stdev": _round(stdev_raw) if stdev_raw is not None else None,
                "range": _round(span),
                "max_abs_deviation_from_mean": _round(deviation),
            }
    except OverflowError:
        pass
    return _describe_exact(ordered)


def _values(rows: Sequence[Measurement], attribute: str) -> list[float]:
    out: list[float] = []
    for row in rows:
        value = getattr(row, attribute)
        if value is not None:
            out.append(float(value))
    return out


def _residuals(rows: Sequence[Measurement]) -> list[float]:
    """Where the vehicle stopped minus the target it was sent to, in metres.

    Only rows that recorded **both** numbers contribute. A missing target does
    not silently become the event target: inventing one would turn an unrecorded
    run into an apparently systematic error. An opposite-sign extreme pair
    whose difference is not a finite float refuses the distribution rather
    than contributing ``inf``.
    """
    out: list[float] = []
    for row in rows:
        if row.measured_end_position_m is None or row.target_distance_m is None:
            continue
        try:
            residual = float(row.measured_end_position_m) - float(row.target_distance_m)
        except OverflowError:
            raise DistributionError(
                f"the residual for run {row.run_id!r} is not representable as a "
                "finite number; refusing the distribution rather than emitting "
                "a non-finite residual"
            ) from None
        if not math.isfinite(residual):
            difference = decimal.Decimal(repr(float(row.measured_end_position_m))) - (
                decimal.Decimal(repr(float(row.target_distance_m)))
            )
            residual = _as_finite_float(
                difference, statistic=f"residual for run {row.run_id!r}"
            )
        out.append(residual)
    return out


def _group_key(row: Measurement) -> tuple[str, Optional[float]]:
    return (row.tournament_level or "", row.target_distance_m)


def _describe_group(key: tuple[str, Optional[float]], rows: Sequence[Measurement]) -> dict[str, Any]:
    level, target = key
    return {
        "tournament_level": level or None,
        "target_distance_m": _round(target),
        "run_count": len(rows),
        "columns": {
            column: describe_values(_values(rows, column))
            for column in DISTRIBUTION_NUMERIC_COLUMNS
        },
        "residual_end_position_m": describe_values(_residuals(rows)),
    }


def measurement_distribution(table: MeasurementTable) -> dict[str, Any]:
    """The series statistics for a measurement file.

    Distribution statistics are computed over the runs whose ``accepted`` cell is
    yes. A run marked no, or left unjudged, is reported in ``classification`` but
    never averaged into a distribution - ``AGENTS.md`` section 19 forbids the
    cherry-pick, and this is the other half of it: an excluded run is kept and
    named, not quietly included.
    """
    measurements = list(table.rows.values())
    accepted = [row for row in measurements if row.accepted is True]
    excluded = [row for row in measurements if row.accepted is False]
    unjudged = [row for row in measurements if row.accepted is None]

    groups: dict[tuple[str, Optional[float]], list[Measurement]] = {}
    for row in accepted:
        groups.setdefault(_group_key(row), []).append(row)

    ordered_keys = sorted(groups, key=lambda key: (key[0], key[1] is None, key[1] or 0.0))
    group_payload = [_describe_group(key, groups[key]) for key in ordered_keys]

    residuals = _residuals(accepted)
    residual_stats = describe_values(residuals)

    return {
        "source": table.source,
        "source_sha256": table.sha256,
        "note": (
            "descriptive statistics over measurements taken by hand. The raw measurements "
            "are the record; these numbers are derived from them, and neither is by itself "
            "a conclusion about the vehicle (VERIFIED_STATE.md owns that)."
        ),
        "columns": list(DISTRIBUTION_NUMERIC_COLUMNS),
        "classification": {
            "total": len(measurements),
            "accepted": len(accepted),
            "excluded": len(excluded),
            "unjudged": len(unjudged),
            "excluded_runs": [
                {"run_id": row.run_id, "exclusion_reason": row.exclusion_reason}
                for row in excluded
            ],
            "excluded_without_a_recorded_reason": [
                row.run_id for row in excluded if not row.exclusion_reason
            ],
            "unjudged_runs": [row.run_id for row in unjudged],
        },
        "accepted_columns": {
            column: describe_values(_values(accepted, column))
            for column in DISTRIBUTION_NUMERIC_COLUMNS
        },
        "residual_end_position_m": {
            **residual_stats,
            # The two numbers AGENTS.md section 8 asks a reader to separate: a
            # mean offset is calibratable, a wide standard deviation is physical.
            "systematic_offset_m": residual_stats["mean"],
            "random_spread_m": residual_stats["stdev"],
            "rows_used": residual_stats["count"],
            "note": (
                "measured resting position minus the recorded target. A small mean with a "
                "small spread is calibratable; a wide spread is not a software constant."
            ),
        },
        "groups": group_payload,
        "grouped_by": ["tournament_level", "target_distance_m"],
    }
