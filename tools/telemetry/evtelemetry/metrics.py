"""Compute the descriptive metrics a captured run makes available.

Everything here is arithmetic over the bytes of a log. Nothing here interprets a
result, and that separation is deliberate: a metric is reproducible from the raw
capture, while a statement about how the vehicle behaves needs physical evidence
that neither a synthetic sample nor a log-derived number can supply.

Two naming rules follow the project's own convention (`SOFTWARE_ARCHITECTURE.md`
section 6) so a reader cannot mix them up:

* a *clock reading* carries ``_ms`` and stays in milliseconds, because that is
  what ``millis()`` provides;
* a *duration* carries ``_s`` and is in seconds.

The raw sensor fields are reported as what they are - ``batt_counts`` and
``curr_counts`` are ADC counts. No conversion to volts or amps is invented here,
because no raw-to-engineering-unit relationship is documented anywhere in the
project and doing it here would turn an unverified guess into something that
looks like a measurement.
"""

from __future__ import annotations

import statistics
from typing import Any, Iterable, Optional, Sequence

from . import schema
from .parse import Event, Log, Record, Sample

__all__ = ["compute_metrics", "PHASE_TIMING_PROBE_PHASES"]

# The phases whose *state* timings describe braking and reversal. These are
# firmware state boundaries, not measurements: how long the policy stayed in a
# phase says nothing about how far the vehicle travelled while it was there,
# because rollout and braking distance are unmeasured facts
# (`VERIFIED_STATE.md` #6 and #9).
PHASE_TIMING_PROBE_PHASES = (
    "ForwardDecel",
    "BrakeZeroSpeed",
    "Reverse",
    "FinalApproach",
    "FinalBrake",
)

_ROUND_DIGITS = 6


def _round(value: Optional[float], digits: int = _ROUND_DIGITS) -> Optional[float]:
    """Rounds for output so a report is byte-reproducible.

    Adds positive zero so a tiny negative value rounds to ``0.0`` rather than
    ``-0.0``. The firmware's formatter refuses to print ``-0.0000`` for the same
    reason (``BoundedText::appendFixed``): a log full of negative zeros reads
    like a sign convention that does not exist.
    """
    if value is None:
        return None
    return float(round(float(value), digits)) + 0.0


def _finite(values: Iterable[Optional[float]]) -> list[float]:
    """The finite numbers among ``values``, dropping every sentinel."""
    out: list[float] = []
    for value in values:
        if value is None:
            continue
        number = float(value)
        if number == number and number not in (float("inf"), float("-inf")):
            out.append(number)
    return out


def _stats(values: Sequence[float]) -> dict[str, Any]:
    if not values:
        return {"count": 0, "min": None, "max": None, "mean": None, "median": None}
    return {
        "count": len(values),
        "min": _round(min(values)),
        "max": _round(max(values)),
        "mean": _round(statistics.fmean(values)),
        "median": _round(statistics.median(values)),
    }


# ---------------------------------------------------------------------------
# Sections
# ---------------------------------------------------------------------------


def _phase_segments(samples: Sequence[Sample]) -> list[dict[str, Any]]:
    """Consecutive same-phase samples collapsed into segments.

    The first segment starts at the log's first sample, which is a boundary of
    the *log*, not necessarily a phase change: the capture may begin mid-phase.
    """
    segments: list[dict[str, Any]] = []
    for sample in samples:
        if segments and segments[-1]["phase"] == sample.phase:
            segments[-1]["end_ms"] = sample.t_abs_ms
            segments[-1]["sample_count"] += 1
        else:
            segments.append(
                {
                    "phase": sample.phase,
                    "start_ms": sample.t_abs_ms,
                    "end_ms": sample.t_abs_ms,
                    "sample_count": 1,
                }
            )
    for segment in segments:
        segment["duration_s"] = _round((segment["end_ms"] - segment["start_ms"]) / 1000.0)
    return segments


def _phase_section(samples: Sequence[Sample]) -> dict[str, Any]:
    segments = _phase_segments(samples)
    order: list[str] = []
    durations: dict[str, float] = {}
    entries: dict[str, int] = {}
    exits: dict[str, int] = {}
    for segment in segments:
        phase = segment["phase"]
        duration = segment["duration_s"] or 0.0
        if phase not in durations:
            order.append(phase)
            durations[phase] = 0.0
            entries[phase] = segment["start_ms"]
        durations[phase] = _round(durations[phase] + duration)
        exits[phase] = segment["end_ms"]
    return {
        "order": order,
        "segments": segments,
        "durations_s": durations,
        "first_entry_t_abs_ms": entries,
        "last_exit_t_abs_ms": exits,
        "segment_count": len(segments),
    }


def _phase_changes_from_samples(samples: Sequence[Sample]) -> list[dict[str, Any]]:
    """Phase boundaries the sample train shows, which is the ground truth.

    The first sample establishes the baseline and is not a change: the log may
    begin mid-phase, and inventing a transition there would be a transition that
    did not happen.
    """
    changes: list[dict[str, Any]] = []
    previous: Optional[str] = None
    for sample in samples:
        if previous is not None and sample.phase != previous:
            changes.append({"t_abs_ms": sample.t_abs_ms, "phase": sample.phase})
        previous = sample.phase
    return changes


def _phase_changes_from_events(events: Sequence[Event]) -> list[dict[str, Any]]:
    """The same boundaries as the event lane reports them.

    A `PhaseChange` event's ``detail`` is the *new* phase's wire name, so this is
    directly comparable with the sample-derived list above.
    """
    return [
        {"t_abs_ms": event.t_abs_ms, "phase": event.detail}
        for event in events
        if event.event == "PhaseChange"
    ]


def _phase_timing(phase_section: dict[str, Any]) -> dict[str, Any]:
    """Braking and reversal *state* timings, and nothing more.

    Every number here is a difference between firmware phase boundaries. None of
    them is a stopping distance, and none of them may be read as one: the vehicle
    layer's own state machine decides when a phase ends, and that decision is not
    a measurement of where the vehicle came to rest.
    """
    entries: dict[str, int] = phase_section["first_entry_t_abs_ms"]
    durations: dict[str, float] = phase_section["durations_s"]

    def delta_ms(later: Optional[int], earlier: Optional[int]) -> Optional[float]:
        if later is None or earlier is None:
            return None
        return _round((later - earlier) / 1000.0)

    decel = entries.get("ForwardDecel")
    brake_zero = entries.get("BrakeZeroSpeed")
    reverse = entries.get("Reverse")
    final_brake = entries.get("FinalBrake")

    return {
        "note": (
            "firmware phase-boundary timings only; not stopping distances and not "
            "physical braking evidence"
        ),
        "entry_t_abs_ms": {
            phase: entries.get(phase) for phase in PHASE_TIMING_PROBE_PHASES
        },
        "duration_s": {
            phase: durations.get(phase) for phase in PHASE_TIMING_PROBE_PHASES
        },
        "forward_decel_to_brake_zero_s": delta_ms(brake_zero, decel),
        "brake_zero_dwell_s": durations.get("BrakeZeroSpeed"),
        "brake_zero_to_reverse_s": delta_ms(reverse, brake_zero),
        "forward_decel_to_reverse_s": delta_ms(reverse, decel),
        "reverse_dwell_s": durations.get("Reverse"),
        "final_approach_to_final_brake_s": delta_ms(final_brake, entries.get("FinalApproach")),
        "reversal_observed": reverse is not None,
    }


def _drops_section(records: Sequence[Record]) -> dict[str, Any]:
    samples = [record for record in records if isinstance(record, Sample)]
    events = [record for record in records if isinstance(record, Event)]

    def track(getter) -> dict[str, Any]:
        series = [(record.t_abs_ms, getter(record)) for record in records]
        monotonic = all(
            later[1] >= earlier[1] for earlier, later in zip(series, series[1:])
        )
        first_non_zero = next((t_ms for t_ms, value in series if value > 0), None)
        return {
            "final": series[-1][1] if series else 0,
            "max": max((value for _, value in series), default=0),
            "monotonic_non_decreasing": monotonic,
            "first_non_zero_t_abs_ms": first_non_zero,
        }

    return {
        "samples": track(lambda record: record.drop_samples),
        "events": track(lambda record: record.drop_events),
        "sample_record_count": len(samples),
        "event_record_count": len(events),
    }


def _adjacent_steps(samples: Sequence[Sample]) -> dict[str, Any]:
    """The spacing between consecutive samples, on the unwrapped clock."""
    steps = [
        later.t_abs_ms - earlier.t_abs_ms
        for earlier, later in zip(samples, samples[1:])
    ]
    zero = sum(1 for step in steps if step == 0)
    return {
        "count": len(steps),
        "min_ms": min(steps) if steps else None,
        "max_ms": max(steps) if steps else None,
        "zero_ms_count": zero,
        "over_1s_count": sum(1 for step in steps if step > 1000),
        "raw_t_ms_backward_count": sum(
            1 for earlier, later in zip(samples, samples[1:]) if later.t_ms < earlier.t_ms
        ),
    }


def _position_section(samples: Sequence[Sample]) -> dict[str, Any]:
    usable = [sample for sample in samples if sample.is_usable_position]
    unusable = [sample for sample in samples if not sample.is_usable_position]
    positions = [float(sample.position_m) for sample in usable]  # type: ignore[arg-type]

    final = usable[-1] if usable else None
    final_position = float(final.position_m) if final is not None else None  # type: ignore[arg-type]
    final_remaining = float(final.remaining_m) if final is not None and final.remaining_m is not None else None
    believed_target = (
        final_position + final_remaining
        if final_position is not None and final_remaining is not None
        else None
    )

    return {
        "usable_sample_count": len(usable),
        "unusable_sample_count": len(unusable),
        "first_m": _round(positions[0]) if positions else None,
        "last_m": _round(positions[-1]) if positions else None,
        "min_m": _round(min(positions)) if positions else None,
        "max_m": _round(max(positions)) if positions else None,
        # Displacement from the Start Point at the last usable tick. This is the
        # quantity a tape measure from the start line compares against, which is
        # why `endpoints` uses it and not `net_travel_m`.
        "end_position_m": _round(final_position),
        # Net displacement across the log: a forward excursion plus a reversal
        # cancels here, so it is a different number from `end_position_m` in any
        # run that reverses - which the synthetic nominal runs do.
        "net_travel_m": _round(positions[-1] - positions[0]) if positions else None,
        "final_remaining_m": _round(final_remaining),
        "believed_target_m": _round(believed_target),
        "note": "position is the firmware's run-axis estimate, metres from the Start Point",
    }


def _velocity_section(samples: Sequence[Sample]) -> dict[str, Any]:
    usable = [sample for sample in samples if sample.is_usable_velocity]
    measured = [float(sample.velocity_mps) for sample in usable]  # type: ignore[arg-type]
    commanded = _finite(sample.velocity_cmd_mps for sample in samples)
    pairs = [
        (float(sample.velocity_mps), float(sample.velocity_cmd_mps))  # type: ignore[arg-type]
        for sample in usable
        if sample.velocity_cmd_mps is not None
    ]
    errors = [measured_value - commanded_value for measured_value, commanded_value in pairs]

    return {
        "usable_sample_count": len(usable),
        "unusable_sample_count": len(samples) - len(usable),
        "measured": _stats(measured),
        "commanded": _stats(commanded),
        "tracking_error_mps": {
            **_stats(errors),
            "mean_abs": _round(statistics.fmean([abs(value) for value in errors]))
            if errors
            else None,
            "max_abs": _round(max([abs(value) for value in errors])) if errors else None,
        },
        "note": "commanded is the shaped target for the tick; measured is the estimator's own verdict",
    }


def _effort_section(samples: Sequence[Sample]) -> dict[str, Any]:
    efforts = [float(sample.effort_permille) for sample in samples]
    by_mode: dict[str, dict[str, Any]] = {}
    for sample in samples:
        bucket = by_mode.setdefault(sample.motor, {"count": 0, "values": []})
        bucket["count"] += 1
        bucket["values"].append(float(sample.effort_permille))
    shaped = {
        mode: {
            "count": bucket["count"],
            "max_permille": int(max(bucket["values"])) if bucket["values"] else None,
            "mean_permille": _round(statistics.fmean(bucket["values"]))
            if bucket["values"]
            else None,
        }
        for mode, bucket in sorted(by_mode.items())
    }
    nonzero_powered = sum(
        1
        for sample in samples
        if sample.effort_permille > 0 and sample.motor in ("DriveForward", "DriveReverse")
    )
    return {
        "overall_permille": _stats(efforts),
        "by_motor_mode": shaped,
        "nonzero_powered_sample_count": nonzero_powered,
        "note": "effort_permille is the requested effort out of 1000, not a current measurement",
    }


def _raw_inputs_section(samples: Sequence[Sample]) -> dict[str, Any]:
    batteries = [float(sample.battery_counts) for sample in samples]
    currents = [float(sample.current_counts) for sample in samples]
    low = [sample for sample in samples if sample.driver_fault_bar_low]
    return {
        "battery_counts": {
            **_stats(batteries),
            "first": int(batteries[0]) if batteries else None,
            "last": int(batteries[-1]) if batteries else None,
        },
        "current_counts": {
            **_stats(currents),
            "first": int(currents[0]) if currents else None,
            "last": int(currents[-1]) if currents else None,
        },
        "nfault_low": {
            "sample_count": len(low),
            "first_t_abs_ms": low[0].t_abs_ms if low else None,
        },
        "units": "raw ADC counts and a raw logic level; no conversion is documented",
    }


def _faults_section(samples: Sequence[Sample], events: Sequence[Event]) -> dict[str, Any]:
    latched = [event for event in events if event.event == "FaultLatched"]
    sample_faults: dict[str, int] = {}
    for sample in samples:
        if sample.fault != "None":
            sample_faults[sample.fault] = sample_faults.get(sample.fault, 0) + 1
    first_latched = latched[0].t_abs_ms if latched else None
    if first_latched is None:
        first_latched = next(
            (sample.t_abs_ms for sample in samples if sample.fault != "None"), None
        )
    return {
        "latched_event_count": len(latched),
        "latched_faults": [
            {"t_abs_ms": event.t_abs_ms, "fault": event.detail, "phase": event.phase}
            for event in latched
        ],
        "sample_fault_counts": dict(sorted(sample_faults.items())),
        "first_latched_t_abs_ms": first_latched,
    }


def _authorization_section(samples: Sequence[Sample]) -> dict[str, Any]:
    counts: dict[str, int] = {}
    for sample in samples:
        counts[sample.authz] = counts.get(sample.authz, 0) + 1
    denied = sum(value for key, value in counts.items() if key.startswith("Denied"))
    drive = sum(
        counts.get(key, 0) for key in ("AuthorizedDrive", "AuthorizedClamped")
    )
    first_drive = next(
        (
            sample.t_abs_ms
            for sample in samples
            if sample.authz in ("AuthorizedDrive", "AuthorizedClamped")
        ),
        None,
    )
    return {
        "outcome_counts": dict(sorted(counts.items())),
        "denied_sample_count": denied,
        "drive_authorized_sample_count": drive,
        "first_drive_authorized_t_abs_ms": first_drive,
    }


def _configuration_check_section(samples: Sequence[Sample]) -> dict[str, Any]:
    seen: dict[str, int] = {}
    first: dict[str, int] = {}
    for sample in samples:
        if sample.check == "none":
            continue
        seen[sample.check] = seen.get(sample.check, 0) + 1
        first.setdefault(sample.check, sample.t_abs_ms)
    return {
        "failing_check_counts": dict(sorted(seen.items())),
        "first_seen_t_abs_ms": dict(sorted(first.items())),
        "note": "the most fundamental failing configuration check recorded in each tick",
    }


def _run_window(samples: Sequence[Sample], events: Sequence[Event]) -> dict[str, Any]:
    armed_samples = [sample for sample in samples if sample.armed]
    armed_first = armed_samples[0].t_abs_ms if armed_samples else None
    armed_last = armed_samples[-1].t_abs_ms if armed_samples else None
    done = next((sample.t_abs_ms for sample in samples if sample.phase == "Done"), None)
    fault = next((sample.t_abs_ms for sample in samples if sample.phase == "Fault"), None)
    return {
        "armed_first_t_abs_ms": armed_first,
        "armed_last_t_abs_ms": armed_last,
        "armed_duration_s": _round((armed_last - armed_first) / 1000.0)
        if armed_first is not None and armed_last is not None
        else None,
        "done_t_abs_ms": done,
        "fault_t_abs_ms": fault,
        "note": "the armed window is the firmware's own armed flag, not a physical start or finish line",
    }


def _endpoint_section(
    samples: Sequence[Sample], phase_section: dict[str, Any], timing: dict[str, Any]
) -> dict[str, Any]:
    usable = [sample for sample in samples if sample.is_usable_position]
    final = usable[-1] if usable else None
    return {
        "final_phase": samples[-1].phase if samples else None,
        "final_fault": samples[-1].fault if samples else None,
        "ended_in_fault": any(sample.phase == "Fault" for sample in samples),
        "reversal_observed": bool(timing["reversal_observed"]),
        "phases_observed": list(phase_section["order"]),
        "believed_end_position_m": _round(float(final.position_m))  # type: ignore[arg-type]
        if final is not None
        else None,
        "final_remaining_m": _round(float(final.remaining_m))
        if final is not None and final.remaining_m is not None
        else None,
        "note": "believed endpoints are the firmware's own estimates and are not measured positions",
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def compute_metrics(log: Log, run_id: str) -> dict[str, Any]:
    """Every descriptive metric the log supports, as a JSON-able mapping.

    The result depends only on the log's contents and the schema, never on the
    clock or the environment, so two runs over the same bytes produce the same
    report.
    """
    samples = log.samples
    events = log.events
    phase_section = _phase_section(samples)
    timing = _phase_timing(phase_section)

    from_samples = _phase_changes_from_samples(samples)
    from_events = _phase_changes_from_events(events)
    sample_keys = {(item["t_abs_ms"], item["phase"]) for item in from_samples}
    event_keys = {(item["t_abs_ms"], item["phase"]) for item in from_events}
    only_samples = sample_keys - event_keys
    only_events = event_keys - sample_keys
    mismatches = [
        {"t_abs_ms": t_ms, "phase": phase, "only_in": only_in}
        for t_ms, phase, only_in in sorted(
            (item for item in ((t, p, "samples") for t, p in only_samples))
        )
        + sorted((item for item in ((t, p, "events") for t, p in only_events)))
    ]

    first = log.first_t_abs_ms
    last = log.last_t_abs_ms

    return {
        "run_id": run_id,
        "source": {
            "path": log.source,
            "sha256": log.sha256,
            "total_lines": log.total_lines,
        },
        "schema": {"version": schema.SCHEMA_VERSION, "prefixes": [schema.SAMPLE_PREFIX, schema.EVENT_PREFIX]},
        "counts": {
            "samples": len(samples),
            "events": len(events),
            "ignored_lines": len(log.ignored_lines),
            "parse_errors": len(log.errors),
        },
        "window": {
            "first_t_abs_ms": first,
            "last_t_abs_ms": last,
            "log_duration_s": _round((last - first) / 1000.0)
            if first is not None and last is not None
            else None,
        },
        "run_window": _run_window(samples, events),
        # Nested under a unit-carrying key rather than spread flat, so the unit is
        # still in the key path for every leaf (`section 6`).
        "control_dt": {
            "dt_ms": _stats([float(sample.dt_ms) for sample in samples]),
            "zero_ms_count": sum(1 for sample in samples if sample.dt_ms == 0),
            "over_50ms_count": sum(1 for sample in samples if sample.dt_ms > 50),
            "over_100ms_count": sum(1 for sample in samples if sample.dt_ms > 100),
            "note": "the measured interval each tick was given, as the tick itself reported it",
        },
        "adjacent_sample_spacing": _adjacent_steps(samples),
        "drops": _drops_section(log.records),
        "phases": phase_section,
        "transitions": {
            "events": [
                {
                    "t_abs_ms": event.t_abs_ms,
                    "event": event.event,
                    "phase": event.phase,
                    "detail": event.detail,
                    "armed": event.armed,
                    "fault": event.fault,
                }
                for event in events
            ],
            "phase_changes_from_samples": from_samples,
            "phase_changes_from_events": from_events,
            # A boundary is normally seen twice - once by the sample train and once
            # by the event lane. These two counts say how many boundaries each side
            # saw that the other did not, which is the readable form; the sum below
            # is a single number for a table, not a count of distinct boundaries.
            "phase_changes_only_in_samples_count": len(only_samples),
            "phase_changes_only_in_events_count": len(only_events),
            "phase_changes_only_in_samples": [
                item
                for item in from_samples
                if (item["t_abs_ms"], item["phase"]) in only_samples
            ],
            "phase_changes_only_in_events": [
                item
                for item in from_events
                if (item["t_abs_ms"], item["phase"]) in only_events
            ],
            "phase_change_mismatch_count": len(mismatches),
            "phase_change_mismatches": mismatches[:50],
            "note": (
                "disagreement is expected when the drop counters are non-zero: the event "
                "lane drains first, so a dropped sample can hide a boundary from the "
                "sample train while the event survives"
            ),
        },
        "position": _position_section(samples),
        "velocity": _velocity_section(samples),
        "effort": _effort_section(samples),
        "raw_inputs": _raw_inputs_section(samples),
        "faults": _faults_section(samples, events),
        "authorization": _authorization_section(samples),
        "configuration_checks": _configuration_check_section(samples),
        "phase_timing": timing,
        "endpoints": _endpoint_section(samples, phase_section, timing),
    }
