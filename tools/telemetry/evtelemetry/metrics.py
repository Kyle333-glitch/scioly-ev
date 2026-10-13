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

import dataclasses
import statistics
from typing import Any, Iterable, Optional, Sequence

from . import schema
from .parse import Event, Log, Record, Sample

__all__ = ["compute_metrics", "select_run", "SelectedRun", "PHASE_TIMING_PROBE_PHASES"]

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

# The span of the firmware's `millis()` clock. Elapsed time is an unsigned
# difference of two readings, so it is taken modulo this - the same arithmetic the
# firmware uses - and a wrap between two readings cannot produce a negative
# duration here either.
_UINT32_SPAN = 1 << 32


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


def _spans_break(
    first_line_number: int, last_line_number: int, break_lines: frozenset[int]
) -> bool:
    """Whether the sample interval holds a suspected clock restart.

    ``break_lines`` are the line numbers whose step *into* them was classified
    ``restart_suspected``: a backwards reading that is neither a uint32 wrap
    nor a few-millisecond re-ordering. Any elapsed difference whose sample
    range contains such a step would be reconstructed as ~49.7 days that did
    not happen, so it is unreliable rather than large.
    """
    return any(
        first_line_number < line <= last_line_number for line in break_lines
    )


def _phase_segments(
    samples: Sequence[Sample], *, break_lines: frozenset[int] = frozenset()
) -> list[dict[str, Any]]:
    """Consecutive same-phase samples collapsed into segments.

    Two different numbers come out of a segment, and they are *not* the same
    fact:

    * ``observed_span_s`` is the span the samples themselves cover - the first
      same-phase sample to the *last* one. The phase was still in effect when the
      last one was written, so this span is a lower bound: it is short by however
      much of the phase the sampling did not cover. A one-sample phase reports
      ``0.0``, which is a statement about the samples, not about the phase.
    * ``duration_s`` is the **transition-bounded** duration - the first sample of
      the phase to the first sample of the *next* phase. A phase change is
      detected by the next sample that carries the new phase, so that reading is
      where the change is first observable, and the difference is the phase's
      duration to the resolution the capture has. The last segment of the
      capture has no next phase to bound it, so its ``duration_s`` is ``None`` and
      it is marked ``censored``: reporting its observed span as a dwell would
      present a truncated number as a complete one.

    When the samples a span or duration covers hold a suspected clock restart
    (``break_lines``), that number is ``None`` and the segment is marked
    ``timing_unreliable``: the unwrapped timeline there is monotonic but
    untrusted, and a ~49.7-day fabrication is worse than an explicit unknown.

    The first segment starts at the log's first sample, which is a boundary of
    the *log*, not necessarily a phase change: the capture may begin mid-phase.
    """
    segments: list[dict[str, Any]] = []
    for sample in samples:
        if segments and segments[-1]["phase"] == sample.phase:
            segments[-1]["end_ms"] = sample.t_abs_ms
            segments[-1]["end_line"] = sample.line_number
            segments[-1]["sample_count"] += 1
        else:
            segments.append(
                {
                    "phase": sample.phase,
                    "start_ms": sample.t_abs_ms,
                    "end_ms": sample.t_abs_ms,
                    "start_line": sample.line_number,
                    "end_line": sample.line_number,
                    "sample_count": 1,
                }
            )
    for index, segment in enumerate(segments):
        following = segments[index + 1] if index + 1 < len(segments) else None
        span_broken = _spans_break(segment["start_line"], segment["end_line"], break_lines)
        duration_broken = following is not None and _spans_break(
            segment["start_line"], following["start_line"], break_lines
        )
        segment["timing_unreliable"] = span_broken or bool(duration_broken)
        segment["observed_span_s"] = (
            None
            if span_broken
            else _round((segment["end_ms"] - segment["start_ms"]) / 1000.0)
        )
        segment["censored"] = following is None
        segment["duration_s"] = (
            None
            if following is None or duration_broken
            else _round((following["start_ms"] - segment["start_ms"]) / 1000.0)
        )
        del segment["start_line"]
        del segment["end_line"]
    return segments


def _phase_section(
    samples: Sequence[Sample], *, break_lines: frozenset[int] = frozenset()
) -> dict[str, Any]:
    segments = _phase_segments(samples, break_lines=break_lines)
    order: list[str] = []
    durations: dict[str, Optional[float]] = {}
    spans: dict[str, Optional[float]] = {}
    censored: dict[str, bool] = {}
    unreliable: dict[str, bool] = {}
    entries: dict[str, int] = {}
    exits: dict[str, int] = {}
    for segment in segments:
        phase = segment["phase"]
        if phase not in spans:
            order.append(phase)
            durations[phase] = 0.0
            spans[phase] = 0.0
            censored[phase] = False
            unreliable[phase] = False
            entries[phase] = segment["start_ms"]
        if segment["timing_unreliable"]:
            # Part of this phase's timeline crosses a suspected clock restart:
            # the phase total is unknown, not the sum of its trustworthy parts.
            unreliable[phase] = True
        if segment["duration_s"] is None:
            # The phase runs to the end of the capture: nothing bounded it, so it
            # contributes no bounded duration and the phase is marked censored.
            censored[phase] = True
        elif not segment["timing_unreliable"]:
            durations[phase] = _round(durations[phase] + segment["duration_s"])
        if not segment["timing_unreliable"]:
            spans[phase] = _round(spans[phase] + (segment["observed_span_s"] or 0.0))
        exits[phase] = segment["end_ms"]
    for phase in order:
        if unreliable[phase]:
            durations[phase] = None
            spans[phase] = None
    return {
        "order": order,
        "segments": segments,
        # Transition-bounded durations, in seconds: the first sample of the phase
        # to the first sample of the next one. `censored_phases` names the phases
        # whose last segment reached the end of the capture, whose bounded
        # duration is therefore a lower bound (`observed_span_s` only).
        # `timing_unreliable_phases` names the phases whose timeline crosses a
        # suspected clock restart: their duration and span are `None` with the
        # reason in the segment flags, never a ~49.7-day fabrication.
        "durations_s": durations,
        "observed_span_s": spans,
        "censored_phases": [phase for phase in order if censored[phase]],
        "timing_unreliable_phases": [phase for phase in order if unreliable[phase]],
        "first_entry_t_abs_ms": entries,
        "last_exit_t_abs_ms": exits,
        "segment_count": len(segments),
        "duration_note": (
            "durations_s is transition-bounded (first sample of the phase to the "
            "first sample of the next phase); observed_span_s is the span the "
            "samples covered, which is a lower bound while a phase is still in "
            "effect at the last sample"
        ),
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
            changes.append(
                {
                    "t_abs_ms": sample.t_abs_ms,
                    "t_ms": sample.t_ms,
                    "session_index": sample.session_index,
                    "phase": sample.phase,
                }
            )
        previous = sample.phase
    return changes


def _phase_changes_from_events(events: Sequence[Event]) -> list[dict[str, Any]]:
    """The same boundaries as the event lane reports them.

    A `PhaseChange` event's ``detail`` is the *new* phase's wire name, so this is
    directly comparable with the sample-derived list above.
    """
    return [
        {
            "t_abs_ms": event.t_abs_ms,
            "t_ms": event.t_ms,
            "session_index": event.session_index,
            "phase": event.detail,
        }
        for event in events
        if event.event == "PhaseChange"
    ]


def _phase_change_key(change: dict[str, Any]) -> tuple[int, int, str]:
    """The identity of a boundary as both lanes can agree on it.

    The raw ``t_ms`` reading is used, not the unwrapped ``t_abs_ms``: each lane is
    unwrapped on its own timeline, so two lanes' ``t_abs_ms`` values are only
    comparable when both lanes happen to be anchored at the same reading. Within
    one session the raw reading is the same clock, so it is the one both lanes
    share.
    """
    return (change["session_index"], change["t_ms"], change["phase"])


def _phase_timing(
    phase_section: dict[str, Any],
    samples: Sequence[Sample],
    break_lines: frozenset[int],
) -> dict[str, Any]:
    """Braking and reversal *state* timings, and nothing more.

    Every number here is a difference between firmware phase boundaries. None of
    them is a stopping distance, and none of them may be read as one: the vehicle
    layer's own state machine decides when a phase ends, and that decision is not
    a measurement of where the vehicle came to rest.

    A dwell whose phase timeline is unreliable, or a delta whose boundary
    interval crosses a suspected clock restart, is ``None`` with the reason in
    ``timing_unavailable_reason`` - never a ~49.7-day fabrication.
    """
    entries: dict[str, int] = phase_section["first_entry_t_abs_ms"]
    durations: dict[str, Optional[float]] = phase_section["durations_s"]

    entry_line: dict[str, int] = {}
    for sample in samples:
        entry_line.setdefault(sample.phase, sample.line_number)

    def delta_ms(
        later_phase: str, earlier_phase: str
    ) -> Optional[float]:
        later = entries.get(later_phase)
        earlier = entries.get(earlier_phase)
        if later is None or earlier is None:
            return None
        if _spans_break(
            entry_line[earlier_phase], entry_line[later_phase], break_lines
        ):
            return None
        return _round((later - earlier) / 1000.0)

    unreliable = bool(phase_section["timing_unreliable_phases"])
    deltas = {
        "forward_decel_to_brake_zero_s": delta_ms("BrakeZeroSpeed", "ForwardDecel"),
        "brake_zero_to_reverse_s": delta_ms("Reverse", "BrakeZeroSpeed"),
        "forward_decel_to_reverse_s": delta_ms("Reverse", "ForwardDecel"),
        "final_approach_to_final_brake_s": delta_ms("FinalBrake", "FinalApproach"),
    }
    if any(
        phase in phase_section["timing_unreliable_phases"]
        for phase in ("BrakeZeroSpeed", "Reverse")
    ) or any(
        _spans_break(
            entry_line[earlier], entry_line[later], break_lines
        )
        for earlier, later in (
            ("ForwardDecel", "BrakeZeroSpeed"),
            ("BrakeZeroSpeed", "Reverse"),
            ("ForwardDecel", "Reverse"),
            ("FinalApproach", "FinalBrake"),
        )
        if earlier in entry_line and later in entry_line
    ):
        unreliable = True

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
        "forward_decel_to_brake_zero_s": deltas["forward_decel_to_brake_zero_s"],
        "brake_zero_dwell_s": durations.get("BrakeZeroSpeed"),
        "brake_zero_to_reverse_s": deltas["brake_zero_to_reverse_s"],
        "forward_decel_to_reverse_s": deltas["forward_decel_to_reverse_s"],
        "reverse_dwell_s": durations.get("Reverse"),
        "final_approach_to_final_brake_s": deltas["final_approach_to_final_brake_s"],
        "reversal_observed": reverse is not None,
        "timing_unavailable_reason": (
            "a suspected clock restart crosses the analyzed run's phase timeline; "
            "phase-boundary intervals across it are unavailable"
            if unreliable
            else None
        ),
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


def _adjacent_steps(
    samples: Sequence[Sample], *, break_lines: frozenset[int] = frozenset()
) -> dict[str, Any]:
    """The spacing between consecutive samples, on the unwrapped clock.

    Steps that cross a suspected clock restart are counted in
    ``unreliable_step_count`` and excluded from the min/max: the unwrapped
    difference there is monotonic but untrusted, and reporting it as the
    maximum gap would present ~49.7 days as a sampling fact. A reordered step
    stays: its small (possibly negative) signed difference is the honest
    same-epoch spacing.
    """
    pairs = list(zip(samples, samples[1:]))
    unreliable = sum(1 for _, later in pairs if later.line_number in break_lines)
    steps = [
        later.t_abs_ms - earlier.t_abs_ms
        for earlier, later in pairs
        if later.line_number not in break_lines
    ]
    zero = sum(1 for step in steps if step == 0)
    return {
        "count": len(pairs),
        "min_ms": min(steps) if steps else None,
        "max_ms": max(steps) if steps else None,
        "zero_ms_count": zero,
        "over_1s_count": sum(1 for step in steps if step > 1000),
        "raw_t_ms_backward_count": sum(
            1 for earlier, later in pairs if later.t_ms < earlier.t_ms
        ),
        "unreliable_step_count": unreliable,
    }


# The phases that only occur after the vehicle has reversed. If the run observed
# any of them, its final stop is a *reverse-leg* stop, where the firmware's
# remaining distance is `position - target` and not `target - position`
# (`MotionPlan::remainingDistanceTo`).
_REVERSE_PHASES = ("Reverse", "FinalApproach", "FinalBrake")


def _observed_phases(samples: Sequence[Sample], events: Sequence[Event]) -> set[str]:
    """Every phase the run is known to have entered, from both lanes.

    Both lanes are used because either can be the one that survived a drop: the
    sample train shows the phase in `phase`, and a `PhaseChange` event names the
    new phase in `detail`.
    """
    phases = {sample.phase for sample in samples}
    phases.update(event.detail for event in events if event.event == "PhaseChange")
    return phases


def _stopped_leg(phases: set[str]) -> str:
    """Which leg the final stop belongs to: ``"forward"`` or ``"reverse"``."""
    return "reverse" if any(phase in phases for phase in _REVERSE_PHASES) else "forward"


def _verified_stop_sample(samples: Sequence[Sample]) -> Optional[Sample]:
    """The sample observed at the run's verified final-stop instant.

    Producer contract (`CompetitionStateMachine.h`, `VehicleRuntime.cpp`): the
    verification cycle stamps `finalStopVerifiedAtMs = nowMs` and the same
    tick's sample carries `t_ms = nowMs`, so the tick that verified the stop
    is the one record whose raw `t_ms` equals its own `stop_verified_ms`.
    The flag and timestamp latch into every later Done sample while the
    position keeps reporting the live run-axis estimate, so a later sample
    carrying the flag is evidence that verification *occurred*, never evidence
    that its position was observed *at verification*. Only a usable sample in
    the analyzed (selected) run with the run's timeline flags
    (`run_start_ok`, `dwell_start_ok`, `stop_verified_ok`) whose raw `t_ms`
    equals its own `stop_verified_ms` represents *where the run stopped*.

    A capture that ends while the dwell is still open, that dropped the
    verification tick, or that started after it has no such sample, so its
    endpoint is not a verified stop and is reported as such. No tolerance,
    interpolation or cross-sample pairing is applied: the association is
    within one record, on the producer's own clock, so uint32 wrap needs no
    window and cross-run/cross-session pairing is impossible (callers pass
    only the selected run's samples).
    """
    candidates = [
        sample
        for sample in samples
        if sample.run_start_ok
        and sample.dwell_start_ok
        and sample.stop_verified_ok
        and sample.t_ms == sample.stop_verified_ms
    ]
    if not candidates:
        return None
    return candidates[0]


def _position_section(
    samples: Sequence[Sample],
    events: Sequence[Event],
    break_lines: frozenset[int] = frozenset(),
) -> dict[str, Any]:
    usable = [sample for sample in samples if sample.is_usable_position]
    unusable = [sample for sample in samples if not sample.is_usable_position]
    positions = [float(sample.position_m) for sample in usable]  # type: ignore[arg-type]

    latest = usable[-1] if usable else None
    latest_position = float(latest.position_m) if latest is not None else None  # type: ignore[arg-type]
    latest_remaining = (
        float(latest.remaining_m)
        if latest is not None and latest.remaining_m is not None
        else None
    )

    phases = _observed_phases(samples, events)
    leg = _stopped_leg(phases)
    verified = _verified_stop_sample(usable)
    stop_position = (
        float(verified.position_m) if verified is not None else None  # type: ignore[arg-type]
    )
    stop_remaining = (
        float(verified.remaining_m)
        if verified is not None and verified.remaining_m is not None
        else None
    )

    # Every selected sample carrying the latched timeline (flag set), whether or
    # not it is the verification tick: evidence that verification *occurred*.
    latched = [
        sample
        for sample in samples
        if sample.run_start_ok and sample.dwell_start_ok and sample.stop_verified_ok
    ]
    latched_stamps = sorted({sample.stop_verified_ms for sample in latched})

    # The endpoint is the verified final stop, and nothing else. The last sample in
    # the file is a different fact: after the stop the vehicle may be handled or
    # retrieved, which moves it, and a capture that ends mid-run has no stop at all.
    # Substituting one for the other is how an analysis reported a different
    # position than the one the run stopped at, so both are reported, separately
    # named, with the source that produced each. A latched flag without the
    # verification-tick sample is UNAVAILABLE, never the later position: matching
    # values do not prove a late sample was taken at verification.
    if stop_position is not None:
        end_position = stop_position
        end_source: Optional[str] = "verified_final_stop"
        end_reason: Optional[str] = None
    elif not latched:
        if latest_position is not None:
            end_position = latest_position
            end_source = "latest_usable_sample"
            end_reason = (
                "no verified final stop was recorded (the run's final-stop dwell never "
                "verified), so this is the last usable position, not a stop position"
            )
        else:
            end_position = None
            end_source = None
            end_reason = "no usable position sample was recorded"
    elif len(latched_stamps) > 1:
        end_position = None
        end_source = None
        end_reason = (
            "the selected run carries contradictory stop-verification timestamps "
            f"({', '.join(str(stamp) for stamp in latched_stamps)} ms); no single "
            "verification instant can be attributed, so the verified stop position "
            "is unavailable (the latest usable position remains a later observation, "
            "not a verified stop)"
        )
    else:
        stamp = latched_stamps[0]
        first_latched = min(sample.t_abs_ms for sample in latched)
        tick_match_unusable = next(
            (
                sample
                for sample in samples
                if sample.run_start_ok
                and sample.dwell_start_ok
                and sample.stop_verified_ok
                and sample.t_ms == sample.stop_verified_ms
            ),
            None,
        )
        if tick_match_unusable is not None:
            # A record exists at the verification instant but carries no usable
            # position: the instant is known, the position there is not.
            end_position = None
            end_source = None
            end_reason = (
                f"the run recorded stop verification at {stamp} ms but the "
                f"verification-instant sample at {stamp} ms carries no usable "
                "position, so the verified stop position is unavailable (the latest "
                "usable position remains a later observation, not a verified stop)"
            )
        else:
            end_position = None
            end_source = None
            end_reason = (
                f"the run recorded stop verification at {stamp} ms but no usable "
                "sample was observed at that instant "
                f"(first verified sample at {first_latched} ms); the verified stop "
                "position is unavailable (the latest usable position remains a later "
                "observation, not a verified stop)"
            )

    # Sampling uncertainty: the stop is confirmed by a sample, and the vehicle kept
    # moving between that sample and the one before it. The endpoint is therefore
    # only known to within that window, and the window is reported rather than
    # implied. `None` when the confirming sample is the first usable one.
    stop_index = usable.index(verified) if verified is not None else None
    previous_usable = (
        usable[stop_index - 1] if stop_index is not None and stop_index > 0 else None
    )
    sampling_window_ms = None
    if verified is not None and previous_usable is not None:
        if _spans_break(
            previous_usable.line_number, verified.line_number, break_lines
        ):
            # The confirming sample and its predecessor straddle a suspected
            # restart: the window between them has no elapsed meaning.
            sampling_window_ms = None
        else:
            sampling_window_ms = verified.t_abs_ms - previous_usable.t_abs_ms
    sampling_window_m = (
        abs(float(verified.position_m) - float(previous_usable.position_m))  # type: ignore[arg-type]
        if verified is not None
        and previous_usable is not None
        and verified.position_m is not None
        and previous_usable.position_m is not None
        else None
    )

    # Leg-correct reconstruction of the target the run believed it was driving to.
    # On the forward leg the firmware's remaining is `target - position`; on the
    # reverse leg it is `position - target`, so the same `+` on both legs turns a
    # reverse-leg stop into a target on the far side of the run. When the leg is
    # reverse and the log's remaining is forward-shaped, the reconstruction is
    # arithmetically wrong and says so through `believed_target_source` being
    # absent while the numbers remain visible in the payload.
    believed_target: Optional[float] = None
    believed_target_source: Optional[str] = None
    if stop_position is not None and stop_remaining is not None:
        believed_target = (
            stop_position - stop_remaining if leg == "reverse" else stop_position + stop_remaining
        )
        believed_target_source = f"reconstructed_from_verified_stop_{leg}_leg"

    return {
        "usable_sample_count": len(usable),
        "unusable_sample_count": len(unusable),
        "first_m": _round(positions[0]) if positions else None,
        # The last usable tick, whatever happened afterwards. Equal to
        # `end_position_m` only when no sample followed the verified stop.
        "last_m": _round(latest_position),
        "min_m": _round(min(positions)) if positions else None,
        "max_m": _round(max(positions)) if positions else None,
        "verified_stop_m": _round(stop_position),
        "verified_stop_t_abs_ms": verified.t_abs_ms if verified is not None else None,
        "verified_stop_leg": leg if verified is not None else None,
        "verified_stop_remaining_m": _round(stop_remaining),
        # Displacement from the Start Point at the verified final stop. This is the
        # quantity a tape measure from the start line compares against, which is
        # why `endpoints` uses it and not `net_travel_m`.
        "end_position_m": _round(end_position),
        "end_position_source": end_source,
        "end_position_unavailable_reason": end_reason,
        "end_position_sampling_window_ms": sampling_window_ms,
        "end_position_sampling_window_m": _round(sampling_window_m),
        # How far the position estimate moved after the verified stop (handling,
        # retrieval, or a fault): the difference between the two, kept so a report
        # can state it instead of silently reporting one as the other.
        "after_stop_travel_m": (
            _round(latest_position - stop_position)
            if latest_position is not None and stop_position is not None
            else None
        ),
        # Net displacement across the analysed session: a forward excursion plus a
        # reversal cancels here, so it is a different number from `end_position_m`
        # in any run that reverses - which the synthetic nominal runs do.
        "net_travel_m": _round(positions[-1] - positions[0]) if positions else None,
        "final_remaining_m": _round(latest_remaining),
        "believed_target_m": _round(believed_target),
        "believed_target_source": believed_target_source,
        "note": (
            "position is the firmware's run-axis estimate, metres from the Start "
            "Point; `end_position_m` is the verified final stop (the usable sample "
            "whose t_ms equals its own stop_verified_ms), `last_m` is the "
            "last usable tick"
        ),
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
    # Only a sample that actually carried a level can be low. When the vehicle has
    # no external nFAULT line the level field is absent (``None``), so `low` is
    # empty for a reason that is not a healthy driver - `nfault_capabilities`
    # below says which reason, and `level_read_sample_count` says whether any
    # level was read at all.
    low = [sample for sample in samples if sample.driver_fault_bar_low is True]
    capabilities = sorted({sample.driver_fault_capability for sample in samples})
    level_read_sample_count = sum(
        1 for sample in samples if sample.driver_fault_bar_low is not None
    )
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
        "nfault_capabilities": capabilities,
        "nfault_low": {
            "sample_count": len(low),
            "level_read_sample_count": level_read_sample_count,
            "first_t_abs_ms": low[0].t_abs_ms if low else None,
            "note": (
                "sample_count is how many samples read low; level_read_sample_count is "
                "how many carried a level at all, and is 0 when the vehicle has no "
                "external nFAULT line (see nfault_capabilities). An unavailable signal "
                "is not a healthy driver."
            ),
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


def _run_window(
    samples: Sequence[Sample],
    events: Sequence[Event],
    break_lines: frozenset[int] = frozenset(),
) -> dict[str, Any]:
    """The armed window, the run's own timeline, and the internal Run Time.

    The internal Run Time is the firmware's own metric: the unsigned difference
    ``dwell_start_ms - run_start_ms``, actuation to the start of the *verified*
    final-stop dwell. The rules end Run Time at the complete stop and exclude the
    Complete Stop interval itself, so a value taken to the ``Done`` tick would
    overcount the confirmation interval by construction. It is still the
    firmware's believed time on its own clock - never the event's official Run
    Time, which a timekeeper or video measures - and it is ``None`` when the run
    never verified a final stop, rather than approximated from some other window.

    A snapshot of the timeline is reported separately: the raw readings, the
    candidate dwell start while one is open (`final_stop_dwell_verified` says
    whether it is the verified one), and the completion reading.
    """
    armed_samples = [sample for sample in samples if sample.armed]
    armed_first = armed_samples[0].t_abs_ms if armed_samples else None
    armed_last = armed_samples[-1].t_abs_ms if armed_samples else None
    done = next((sample.t_abs_ms for sample in samples if sample.phase == "Done"), None)
    fault = next((sample.t_abs_ms for sample in samples if sample.phase == "Fault"), None)

    # The run's timeline. Every sample after the actuation repeats the readings, so
    # the first sample carrying each fact reports the run; the latest dwell start is
    # used while no verification exists, because a candidate that was discarded and
    # restarted has a new reading and the old one is not a fact about the run.
    actuated = next((sample for sample in samples if sample.run_start_ok), None)
    latest_dwell = None
    for sample in samples:
        if sample.run_start_ok and sample.dwell_start_ok:
            latest_dwell = sample
    verified = next(
        (
            sample
            for sample in samples
            if sample.run_start_ok and sample.dwell_start_ok and sample.stop_verified_ok
        ),
        None,
    )

    run_start_ms = actuated.run_start_ms if actuated is not None else None
    dwell_start_ms = None
    if verified is not None:
        dwell_start_ms = verified.dwell_start_ms
    elif latest_dwell is not None:
        dwell_start_ms = latest_dwell.dwell_start_ms
    stop_verified_ms = verified.stop_verified_ms if verified is not None else None

    internal_run_time_s = None
    final_stop_verification_completed_s = None
    internal_run_time_unavailable_reason = None
    if verified is not None:
        if actuated is not None and _spans_break(
            actuated.line_number, verified.line_number, break_lines
        ):
            # The run's own timeline readings straddle a suspected restart:
            # the unsigned difference would be ~49.7 days that did not happen.
            internal_run_time_unavailable_reason = (
                "a suspected clock restart falls between the run's actuation and "
                "its verified final stop; the firmware's own timeline readings "
                "across it have no elapsed meaning"
            )
        else:
            internal_run_time_s = _round(
                ((verified.dwell_start_ms - verified.run_start_ms) % _UINT32_SPAN)
                / 1000.0
            )
            final_stop_verification_completed_s = _round(
                ((verified.stop_verified_ms - verified.run_start_ms) % _UINT32_SPAN)
                / 1000.0
            )

    armed_duration_s: Optional[float] = None
    armed_duration_unavailable_reason: Optional[str] = None
    if armed_first is not None and armed_last is not None:
        first_armed = armed_samples[0]
        last_armed = armed_samples[-1]
        if _spans_break(first_armed.line_number, last_armed.line_number, break_lines):
            armed_duration_unavailable_reason = (
                "a suspected clock restart falls inside the armed window; an "
                "elapsed duration across it would be reconstructed as about "
                "49.7 days that did not happen"
            )
        else:
            armed_duration_s = _round((armed_last - armed_first) / 1000.0)

    return {
        "armed_first_t_abs_ms": armed_first,
        "armed_last_t_abs_ms": armed_last,
        "armed_duration_s": armed_duration_s,
        "armed_duration_unavailable_reason": armed_duration_unavailable_reason,
        "done_t_abs_ms": done,
        "fault_t_abs_ms": fault,
        "run_start_ms": run_start_ms,
        "final_stop_dwell_start_ms": dwell_start_ms,
        "stop_verified_ms": stop_verified_ms,
        "final_stop_dwell_verified": verified is not None,
        # How the run ended, from its own last phase: a run that reached Done (the
        # only phase that exists after a verified final stop), a run a fault ended,
        # or a capture that stopped before either - an incomplete run is reported as
        # incomplete rather than being measured as if it had finished.
        "completion": (
            "done"
            if done is not None and not any(sample.phase == "Fault" for sample in samples)
            else "fault"
            if fault is not None
            else "incomplete"
        ),
        "stop_confirmed_t_abs_ms": verified.t_abs_ms if verified is not None else None,
        "internal_run_time_s": internal_run_time_s,
        "internal_run_time_unavailable_reason": internal_run_time_unavailable_reason,
        "final_stop_verification_completed_s": final_stop_verification_completed_s,
        "note": (
            "the armed window is the firmware's own armed flag, not a physical start or "
            "finish line; the internal Run Time is the firmware's own clock from "
            "actuation to the start of the verified final-stop dwell, so it excludes "
            "the Complete Stop interval - it is not the event's official Run Time, "
            "which a timekeeper or video measures"
        ),
    }


def _endpoint_section(
    samples: Sequence[Sample],
    phase_section: dict[str, Any],
    timing: dict[str, Any],
    position: dict[str, Any],
    run_window: dict[str, Any],
) -> dict[str, Any]:
    """The believed endpoint and what kind of stop it came from.

    The believed endpoint is the *verified final stop* whenever the run recorded
    one (`position.end_position_m`). When it did not, the section reports the last
    usable sample as exactly that and gives the reason - a capture that ends
    mid-run has no endpoint, and saying it does would promote the last written
    bytes into a measurement of where the vehicle came to rest.
    """
    source = position["end_position_source"]
    return {
        "final_phase": samples[-1].phase if samples else None,
        "final_fault": samples[-1].fault if samples else None,
        "ended_in_fault": any(sample.phase == "Fault" for sample in samples),
        "reversal_observed": bool(timing["reversal_observed"]),
        "phases_observed": list(phase_section["order"]),
        "believed_end_position_m": position["end_position_m"],
        "believed_end_position_source": source,
        "believed_end_position_unavailable_reason": position[
            "end_position_unavailable_reason"
        ],
        "believed_end_position_sampling_window_ms": position[
            "end_position_sampling_window_ms"
        ],
        "believed_end_position_sampling_window_m": position[
            "end_position_sampling_window_m"
        ],
        "verified_stop_leg": position["verified_stop_leg"],
        "stop_verified": run_window["final_stop_dwell_verified"],
        "completion": run_window["completion"],
        "believed_target_m": position["believed_target_m"],
        "believed_target_source": position["believed_target_source"],
        "final_remaining_m": position["final_remaining_m"],
        "note": (
            "believed endpoints are the firmware's own estimates and are not measured "
            "positions; `believed_end_position_source` says whether the endpoint is a "
            "verified final stop or the last sample, and the sampling window bounds how "
            "far the estimate could have moved between samples"
        ),
    }


def _capture_section(log: Log, selection: dict[str, Any]) -> dict[str, Any]:
    """What the capture as a whole is, beyond the one session being analyzed."""
    sessions = log.sessions
    wrapped = [item for item in log.wrap_observations if item["kind"] == "wrap"]
    reordered = [item for item in log.wrap_observations if item["kind"] == "reordered"]
    restart_suspected = [
        item for item in log.wrap_observations if item["kind"] == "restart_suspected"
    ]
    return {
        "provenance_declared": log.declared_provenance,
        # Whether the capture declared both SYNTHETIC and CAPTURE (in either
        # order). The classification stays conservative (synthetic); this flag
        # is how the report can state the conflict instead of hiding one claim.
        "provenance_conflict": log.provenance_conflict,
        "session_count": len(sessions),
        "sessions": [
            {
                "index": session.index,
                "opened_by_boot": session.opened_by_boot,
                "boot_t_ms": session.boot_t_ms,
                "sample_count": session.sample_count,
                "event_count": session.event_count,
                "first_t_abs_ms": session.first_t_abs_ms,
                "last_t_abs_ms": session.last_t_abs_ms,
            }
            for session in sessions
        ],
        "selection": selection,
        # Three named kinds of backwards raw reading, kept apart so neither is read
        # as the other: a rollover of the uint32 clock (`wrap`), a few milliseconds
        # of re-ordering between two stamps (`reordered`), and a step that is
        # neither - which is what a reset without a captured `Boot` line looks like
        # (`restart_suspected`).
        "clock_wrap_count": len(wrapped),
        "clock_wraps": wrapped,
        "reordered_reading_count": len(reordered),
        "reordered_readings": reordered[:20],
        "restart_suspected_count": len(restart_suspected),
        "restart_suspected_steps": restart_suspected[:20],
        "note": (
            "each session is one firmware boot; records from different sessions were "
            "written by different boots and their `t_ms` readings are not comparable"
        ),
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def _run_starts(samples: Sequence[Sample]) -> list[int]:
    """The distinct actuation readings in a session, in the order they first appear.

    The run's own timeline reports one actuation reading per run
    (`run_start_ms`), repeated on every sample after it. A session that re-armed
    and ran again therefore carries two of them, which is how a run is identified
    inside one session - the timestamps alone cannot tell two runs apart.
    """
    starts: list[int] = []
    for sample in samples:
        if sample.run_start_ok and (not starts or starts[-1] != sample.run_start_ms):
            starts.append(sample.run_start_ms)
    return starts


def _event_at_or_after_actuation(actuation_ms: int, t_ms: int) -> bool:
    """Whether a raw reading falls at or after an actuation reading, wrap-aware.

    Both readings come from the same session's ``millis()`` clock, so the
    firmware's own unsigned-difference arithmetic applies: the event belongs to
    the actuated run when ``(t_ms - actuation_ms) mod 2**32`` is in the first
    half of the range. A raw ``>=`` comparison says the same thing only when no
    uint32 wrap falls between the two readings; across a genuine wrap it places
    a pre-wrap event after every post-wrap sample, or drops a post-wrap event
    that precedes the first post-wrap sample tick. The boundary is inclusive:
    an event stamped exactly at the actuation reading is the run's own.
    """
    return ((t_ms - actuation_ms) % _UINT32_SPAN) < (_UINT32_SPAN // 2)


@dataclasses.dataclass(frozen=True)
class SelectedRun:
    """The one selected run every run-specific consumer must describe.

    This is the shared selection contract (R1F: M9/M10). Metrics, phase bands,
    event markers and SVG series all derive from the same ``samples`` and
    ``events`` tuples, so an excluded earlier-run or earlier-boot record cannot
    appear in a selected-run chart merely because it exists in the log, and an
    excluded prior-run event cannot count toward selected-run faults,
    transitions, windows or markers.

    Field scope:

    * ``samples`` / ``events`` are RUN-SPECIFIC: the selected run's records.
    * ``session_samples`` / ``session_events`` are SESSION-SPECIFIC: every
      record of the analyzed session, including earlier runs. The session
      window and drop diagnostics use these.
    * Whole-capture integrity (other sessions, wraps, restarts, parse errors)
      stays on ``log`` itself and is never shrunk to this view.

    The start boundary is the run's actuation reading (``run_start_ms``), not
    the first sample tick: a legitimately attributable actuation or
    announcement event that precedes the first sample is the run's own and is
    kept. The end boundary is open: the selected run is the last run of the
    session, so every at-or-after-actuation session event is a member,
    including one stamped after the last sample. When no sample carries an
    actuation reading the run boundaries are unknown: the whole session is the
    run, every session event is a member, and ``run_start_source`` says so
    rather than inventing a start.
    """

    session_index: int
    run_start_ms: Optional[int]
    run_start_source: str
    samples: tuple[Sample, ...]
    events: tuple[Event, ...]
    session_samples: tuple[Sample, ...]
    session_events: tuple[Event, ...]
    event_membership_rule: str
    event_window_uncertain: Optional[str]


def select_run(log: Log) -> SelectedRun:
    """The selected run as an immutable view both metrics and plots consume.

    The selection rule is unchanged: the last session that contains samples is
    the session, and within it the last actuated run is the run. What changes
    (R1F) is that the selected *event* membership is defined here, once, with
    an epoch-aware actuation boundary - instead of each consumer re-deriving
    (or ignoring) it with raw timestamp comparisons.
    """
    sample_sessions = sorted({sample.session_index for sample in log.samples})
    event_sessions = sorted({event.session_index for event in log.events})
    if sample_sessions:
        selected = sample_sessions[-1]
    elif event_sessions:
        selected = event_sessions[-1]
    else:
        selected = 0

    session_samples = tuple(
        sample for sample in log.samples if sample.session_index == selected
    )
    session_events = tuple(
        event for event in log.events if event.session_index == selected
    )

    starts = _run_starts(session_samples)
    selected_run_start_ms = starts[-1] if starts else None
    run_first_index = 0
    if len(starts) > 1 and selected_run_start_ms is not None:
        run_first_index = next(
            index
            for index, sample in enumerate(session_samples)
            if sample.run_start_ok and sample.run_start_ms == selected_run_start_ms
        )
    run_samples = session_samples[run_first_index:]

    unsegmented_restart = any(
        observation["kind"] == "restart_suspected"
        and observation["session_index"] == selected
        for observation in log.wrap_observations
    )
    event_window_uncertain: Optional[str] = None
    if unsegmented_restart:
        # A backwards step that is neither a wrap nor a re-ordering sits inside
        # the analyzed session: raw readings across it have no elapsed meaning
        # (R1D), so an unsigned actuation comparison across it is flagged
        # rather than trusted silently. Membership below is still computed;
        # the flag says the boundary evidence is incomplete.
        event_window_uncertain = (
            "a suspected clock restart falls inside the analyzed session; raw "
            "timestamp comparisons across it have no elapsed meaning, so the "
            "actuation boundary is uncertain there"
        )

    if len(starts) > 1 and selected_run_start_ms is not None:
        run_events = tuple(
            event
            for event in session_events
            if _event_at_or_after_actuation(selected_run_start_ms, event.t_ms)
        )
        event_membership_rule = "at_or_after_actuation_unsigned"
        run_start_source = "actuation_reading"
    else:
        # One run (or no actuation evidence at all): there is no cross-run
        # boundary to get wrong, so every session event is a member. When no
        # sample carries an actuation reading the run interval itself is
        # unknown, and that is recorded instead of invented.
        run_events = session_events
        event_membership_rule = (
            "all_session_events_single_run"
            if starts else "no_actuation_evidence_whole_session"
        )
        run_start_source = (
            "actuation_reading" if starts else "no_actuation_evidence"
        )

    return SelectedRun(
        session_index=selected,
        run_start_ms=selected_run_start_ms,
        run_start_source=run_start_source,
        samples=run_samples,
        events=run_events,
        session_samples=session_samples,
        session_events=session_events,
        event_membership_rule=event_membership_rule,
        event_window_uncertain=event_window_uncertain,
    )


def _select_run(
    log: Log,
) -> tuple[list[Sample], list[Event], list[Event], dict[str, Any]]:
    """Which session and which run the analyzer describes, and what it left out.

    A capture is analyzed as *one run*. The last session that contains samples is
    the session; within it, if the actuation reading changed - the run was
    completed, disarmed and a second run was actuated - the last run is the one
    analyzed and the earlier run's samples are excluded. A session with one run
    keeps every sample, including the ticks before actuation, so the pre-run
    window stays visible.

    ``mixed_sessions`` and ``mixed_runs`` say that the capture holds more than one
    boot and more than one run. The selection is deterministic either way, but a
    consumer comparing a capture against an external measurement has to know that
    the bytes it is reading do not describe one attributable run.

    The membership itself comes from :func:`select_run` so metrics and plots
    share one record set; this wrapper keeps the historical tuple shape.
    """
    view = select_run(log)
    run_samples = list(view.samples)
    run_events = list(view.events)
    events = list(view.session_events)

    sample_sessions = sorted({sample.session_index for sample in log.samples})
    selection = {
        "rule": "last_run_in_last_session_with_samples",
        "selected_session_index": view.session_index,
        "session_count": log.session_count,
        "sample_session_count": len(sample_sessions),
        "mixed_sessions": len(sample_sessions) > 1,
        "runs_in_session": len(_run_starts(view.session_samples)),
        "selected_run_start_ms": view.run_start_ms,
        "mixed_runs": len(_run_starts(view.session_samples)) > 1,
        "excluded_sample_count": len(log.samples) - len(run_samples),
        "excluded_event_count": len(log.events) - len(events),
        "selected_run_sample_count": len(run_samples),
        "selected_run_event_count": len(run_events),
        "excluded_run_sample_count": len(view.session_samples) - len(run_samples),
        "excluded_run_event_count": len(view.session_events) - len(run_events),
        "run_start_source": view.run_start_source,
        "event_membership_rule": view.event_membership_rule,
        "event_window_uncertain": view.event_window_uncertain,
        "note": (
            "one capture is analyzed as one run: the last run of the last session "
            "with samples. Records from other sessions or runs are excluded, and "
            "`mixed_sessions`/`mixed_runs` say whether any exist"
        ),
    }
    return run_samples, run_events, events, selection


def compute_metrics(log: Log, run_id: str) -> dict[str, Any]:
    """Every descriptive metric the log supports, as a JSON-able mapping.

    The result depends only on the log's contents and the schema, never on the
    clock or the environment, so two runs over the same bytes produce the same
    report.
    """
    samples, run_events, events, selection = _select_run(log)
    # Scope contract (R1F): `samples`/`run_events` are the RUN-SPECIFIC
    # selected record set every run metric, band and marker must describe.
    # `events` is the SESSION-SPECIFIC set (selected session, all runs) used
    # only for the session window and drop diagnostics. Whole-capture
    # integrity (other sessions, wraps, restarts, parse errors) stays on `log`.
    session_records = sorted([*samples, *events], key=lambda record: record.order)
    # Sample steps into these lines were classified `restart_suspected`: a
    # backwards reading that is neither a uint32 wrap nor a few-millisecond
    # re-ordering. Elapsed differences across them are unavailable everywhere
    # below - never a ~49.7-day fabrication.
    sample_break_lines = frozenset(
        observation["line_number"]
        for observation in log.wrap_observations
        if observation["kind"] == "restart_suspected"
        and observation["lane"] == "sample"
        and observation["session_index"] == selection["selected_session_index"]
    )
    phase_section = _phase_section(samples, break_lines=sample_break_lines)
    timing = _phase_timing(phase_section, samples, sample_break_lines)

    from_samples = _phase_changes_from_samples(samples)
    from_events = _phase_changes_from_events(run_events)
    sample_keys = {_phase_change_key(change) for change in from_samples}
    event_keys = {_phase_change_key(change) for change in from_events}
    only_samples = sample_keys - event_keys
    only_events = event_keys - sample_keys
    mismatches = [
        {
            "session_index": session_index,
            "t_ms": t_ms,
            "phase": phase,
            "only_in": only_in,
        }
        for session_index, t_ms, phase, only_in in sorted(
            (
                (change["session_index"], change["t_ms"], change["phase"], "samples")
                for change in from_samples
                if _phase_change_key(change) in only_samples
            )
        )
        + sorted(
            (
                (change["session_index"], change["t_ms"], change["phase"], "events")
                for change in from_events
                if _phase_change_key(change) in only_events
            )
        )
    ]

    first = min(
        (record.t_abs_ms for record in session_records),
        default=None,
    )
    last = max(
        (record.t_abs_ms for record in session_records),
        default=None,
    )
    # A backwards step inside the analyzed session that is neither a rollover nor a
    # re-ordering is a clock restart the capture did not label. The per-lane
    # unwrap still reconstructs it as the unsigned difference, so the window's
    # duration would be ~49.7 days of elapsed time that never happened: it is
    # reported as unavailable, with the reason, rather than as a number.
    unsegmented_restart = any(
        observation["kind"] == "restart_suspected"
        and observation["session_index"] == selection["selected_session_index"]
        for observation in log.wrap_observations
    )
    window_unavailable_reason = (
        "a backwards timestamp that is neither a uint32 wrap nor a few-millisecond "
        "re-ordering appears inside the analyzed session; the capture likely spans a "
        "clock restart whose Boot line was not recorded, and an elapsed duration "
        "across it would be reconstructed as about 49.7 days that did not happen"
        if unsegmented_restart
        else None
    )
    position = _position_section(samples, run_events, sample_break_lines)
    run_window = _run_window(samples, run_events, sample_break_lines)

    return {
        "run_id": run_id,
        "source": {
            "path": log.source,
            "sha256": log.sha256,
            "total_lines": log.total_lines,
        },
        "schema": {"version": schema.SCHEMA_VERSION, "prefixes": [schema.SAMPLE_PREFIX, schema.EVENT_PREFIX]},
        "capture": _capture_section(log, selection),
        "counts": {
            "samples": len(samples),
            "events": len(run_events),
            "ignored_lines": len(log.ignored_lines),
            "parse_errors": len(log.errors),
        },
        "window": {
            "first_t_abs_ms": first,
            "last_t_abs_ms": last,
            "log_duration_s": (
                _round((last - first) / 1000.0)
                if first is not None and last is not None and not unsegmented_restart
                else None
            ),
            "duration_unavailable_reason": window_unavailable_reason,
            "session_index": selection["selected_session_index"],
            "note": (
                "the analyzed session's window; each lane's `t_abs_ms` is anchored "
                "to that lane's first record, so cross-lane comparisons use the raw "
                "`t_ms` reading"
            ),
        },
        "run_window": run_window,
        # Nested under a unit-carrying key rather than spread flat, so the unit is
        # still in the key path for every leaf (`section 6`).
        "control_dt": {
            "dt_ms": _stats([float(sample.dt_ms) for sample in samples]),
            "zero_ms_count": sum(1 for sample in samples if sample.dt_ms == 0),
            "over_50ms_count": sum(1 for sample in samples if sample.dt_ms > 50),
            "over_100ms_count": sum(1 for sample in samples if sample.dt_ms > 100),
            "note": "the measured interval each tick was given, as the tick itself reported it",
        },
        "adjacent_sample_spacing": _adjacent_steps(
            samples, break_lines=sample_break_lines
        ),
        "drops": _drops_section(session_records),
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
                for event in run_events
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
                item for item in from_samples if _phase_change_key(item) in only_samples
            ],
            "phase_changes_only_in_events": [
                item for item in from_events if _phase_change_key(item) in only_events
            ],
            "phase_change_mismatch_count": len(mismatches),
            "phase_change_mismatches": mismatches[:50],
            "note": (
                "disagreement is expected when the drop counters are non-zero: the event "
                "lane drains first, so a dropped sample can hide a boundary from the "
                "sample train while the event survives"
            ),
        },
        "position": position,
        "velocity": _velocity_section(samples),
        "effort": _effort_section(samples),
        "raw_inputs": _raw_inputs_section(samples),
        "faults": _faults_section(samples, run_events),
        "authorization": _authorization_section(samples),
        "configuration_checks": _configuration_check_section(samples),
        "phase_timing": timing,
        "endpoints": _endpoint_section(
            samples, phase_section, timing, position, run_window
        ),
    }
