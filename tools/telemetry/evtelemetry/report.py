"""Write a metrics report and its plots, reproducibly.

Two properties matter more than anything else here.

**Reproducible bytes.** Every output is deterministic: JSON is dumped with sorted
keys, numbers are rounded before they are printed, element order is fixed, and no
output contains a timestamp, a host name or a random identifier. Two runs over
the same inputs produce identical files, which is what makes a chart citable and
a diff meaningful. That is why there is no "generated at" banner anywhere.

**A guard against writing into the wrong file.** ``VERIFIED_STATE.md`` is the
project's record of what has been *measured*, and its own rules say that a host
test, a simulation and a synthetic sample are not evidence. This tool never
writes there, and it refuses any output path that names it - synthetic data must
not be able to reach the project's evidence record by accident.
"""

from __future__ import annotations

import json
import os
from typing import Any, Optional, Sequence

from . import schema, svgplot
from .parse import Event, Log, Sample

__all__ = [
    "PROTECTED_OUTPUT_NAMES",
    "ReportError",
    "run_report",
    "write_manifest",
    "write_comparison_report",
    "write_endpoints_report",
    "write_distribution_report",
    "write_parse_errors",
    "summarize_run",
    "summarize_comparison",
    "summarize_distribution",
    "run_plots",
]

# Output names this tool must never create or overwrite. `VERIFIED_STATE.md` is
# the one that matters: it holds measured facts with their evidence, and neither
# a synthetic fixture nor a log-derived metric may end up in it.
PROTECTED_OUTPUT_NAMES: tuple[str, ...] = (
    "VERIFIED_STATE.md",
    "DEVELOPMENT_LOG.md",
    "SOFTWARE_ARCHITECTURE.md",
    "AGENTS.md",
)


class ReportError(ValueError):
    """The report could not be written as asked."""


def _guard(path: str) -> None:
    """Refuses any output path that names a project authority file.

    Every path component is checked, not just the last one. A *directory* named
    after an authority file inside a report tree is a trap for whoever reads the
    tree next, so it is refused too - the rule is easier to state and easier to
    rely on that way.
    """
    components = os.path.normpath(os.path.abspath(path)).split(os.sep)
    for component in components:
        if component in PROTECTED_OUTPUT_NAMES:
            raise ReportError(
                f"refusing to write '{component}': the path names a project authority "
                f"file, and this tool writes only to its own output directory"
            )


def _write_text(path: str, text: str) -> None:
    _guard(path)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def write_json(path: str, payload: Any) -> None:
    """JSON with sorted keys and a trailing newline, so a diff is meaningful."""
    _write_text(path, json.dumps(payload, sort_keys=True, indent=2) + "\n")


def is_synthetic(run_id: str, source: Optional[str]) -> bool:
    """True when a run is a generated fixture rather than a captured session."""
    haystack = f"{run_id} {source or ''}".lower()
    return "synthetic" in haystack


# ---------------------------------------------------------------------------
# Human-readable summaries
# ---------------------------------------------------------------------------


def _fmt(value: Optional[float], digits: int = 3) -> str:
    if value is None:
        return "-"
    # A value that rounds away to nothing prints as zero, never as "-0.000". The
    # firmware's own formatter makes the same choice, and for the same reason: a
    # column full of negative zeros reads like a sign convention that does not
    # exist.
    rounded = round(value, digits)
    if rounded == 0.0:
        rounded = 0.0
    return f"{rounded:.{digits}f}"


def _fmt_int(value: Optional[int]) -> str:
    return "-" if value is None else str(value)


def summarize_run(metrics: dict[str, Any]) -> str:
    """A plain-text summary: every line a number and its label.

    It states what the firmware reported and nothing about the vehicle, and it
    ends by saying so, because a reader who skims a summary is exactly the reader
    who would otherwise mistake a believed position for a measured one.
    """
    counts = metrics["counts"]
    window = metrics["window"]
    run_window = metrics["run_window"]
    dt_stats = metrics["control_dt"]["dt_ms"]
    dt = metrics["control_dt"]
    drops = metrics["drops"]
    phases = metrics["phases"]
    timing = metrics["phase_timing"]
    velocity = metrics["velocity"]
    position = metrics["position"]
    effort = metrics["effort"]
    raw = metrics["raw_inputs"]
    faults = metrics["faults"]
    authz = metrics["authorization"]
    spacing = metrics["adjacent_sample_spacing"]
    transitions = metrics["transitions"]
    synthetic = is_synthetic(metrics["run_id"], metrics["source"]["path"])

    lines: list[str] = []
    lines.append(f"run_id:            {metrics['run_id']}")
    if synthetic:
        lines.append("provenance:        SYNTHETIC FIXTURE - generated, not measured")
    lines.append(f"source:            {metrics['source']['path']}")
    lines.append(f"sha256:            {metrics['source']['sha256']}")
    lines.append(
        f"schema:            {schema.SAMPLE_PREFIX}/{schema.EVENT_PREFIX} version {schema.SCHEMA_VERSION}"
    )
    lines.append(
        f"records:           samples={counts['samples']} events={counts['events']} "
        f"ignored_lines={counts['ignored_lines']} parse_errors={counts['parse_errors']}"
    )
    lines.append(
        f"log window:        {_fmt_int(window['first_t_abs_ms'])} ms .. "
        f"{_fmt_int(window['last_t_abs_ms'])} ms  ({_fmt(window['log_duration_s'])} s)"
    )
    lines.append(
        f"armed window:      {_fmt_int(run_window['armed_first_t_abs_ms'])} ms .. "
        f"{_fmt_int(run_window['armed_last_t_abs_ms'])} ms  "
        f"({_fmt(run_window['armed_duration_s'])} s)"
    )
    lines.append(
        f"control dt:        n={dt_stats['count']} "
        f"min={_fmt_int(int(dt_stats['min']) if dt_stats['min'] is not None else None)} ms "
        f"median={_fmt_int(int(dt_stats['median']) if dt_stats['median'] is not None else None)} ms "
        f"max={_fmt_int(int(dt_stats['max']) if dt_stats['max'] is not None else None)} ms "
        f"mean={_fmt(dt_stats['mean'])} ms  zero={dt['zero_ms_count']} "
        f">50ms={dt['over_50ms_count']} >100ms={dt['over_100ms_count']}"
    )
    lines.append(
        f"sample spacing:    max gap={_fmt_int(spacing['max_ms'])} ms  "
        f"zero gaps={spacing['zero_ms_count']}  >1s gaps={spacing['over_1s_count']}  "
        f"t_ms went backwards {spacing['raw_t_ms_backward_count']}x (expected 0 or a wrap)"
    )
    lines.append(
        f"drops:             samples {drops['samples']['final']} "
        f"(monotonic={'yes' if drops['samples']['monotonic_non_decreasing'] else 'NO'})  "
        f"events {drops['events']['final']} "
        f"(monotonic={'yes' if drops['events']['monotonic_non_decreasing'] else 'NO'})"
    )

    phase_order = phases["order"]
    if phase_order:
        rendered = " | ".join(
            f"{phase} {_fmt(phases['durations_s'].get(phase), 3)}s" for phase in phase_order
        )
        lines.append(f"phases:            {rendered}")
    else:
        lines.append("phases:            (none)")

    lines.append(
        "phase timing:      "
        f"ForwardDecel->BrakeZeroSpeed {_fmt(timing['forward_decel_to_brake_zero_s'])} s; "
        f"BrakeZeroSpeed dwell {_fmt(timing['brake_zero_dwell_s'])} s; "
        f"BrakeZeroSpeed->Reverse {_fmt(timing['brake_zero_to_reverse_s'])} s; "
        f"ForwardDecel->Reverse {_fmt(timing['forward_decel_to_reverse_s'])} s; "
        f"reversal={'yes' if timing['reversal_observed'] else 'no'}"
    )
    lines.append(
        "                  (firmware state boundaries only - not stopping distances)"
    )
    lines.append(
        f"phase changes:     from samples {len(transitions['phase_changes_from_samples'])}, "
        f"from events {len(transitions['phase_changes_from_events'])}, "
        f"only-in-samples {transitions['phase_changes_only_in_samples_count']}, "
        f"only-in-events {transitions['phase_changes_only_in_events_count']}"
    )
    lines.append(
        f"velocity (m/s):    measured {_fmt(velocity['measured']['min'])} .. "
        f"{_fmt(velocity['measured']['max'])} mean {_fmt(velocity['measured']['mean'])}; "
        f"commanded {_fmt(velocity['commanded']['min'])} .. "
        f"{_fmt(velocity['commanded']['max'])} mean {_fmt(velocity['commanded']['mean'])}; "
        f"max |error| {_fmt(velocity['tracking_error_mps']['max_abs'])}"
    )
    lines.append(
        f"position (m):      first {_fmt(position['first_m'])} last {_fmt(position['last_m'])} "
        f"min {_fmt(position['min_m'])} max {_fmt(position['max_m'])}; "
        f"believed end {_fmt(position['end_position_m'])}; "
        f"net travel {_fmt(position['net_travel_m'])}; "
        f"believed target {_fmt(position['believed_target_m'])}; "
        f"usable {position['usable_sample_count']}/{counts['samples']}"
    )
    lines.append(
        f"effort (permille): max {_fmt(effort['overall_permille']['max'], 1)} "
        f"mean {_fmt(effort['overall_permille']['mean'], 1)}; "
        f"powered samples {effort['nonzero_powered_sample_count']}; "
        + "modes "
        + ", ".join(
            f"{mode} n={bucket['count']} max={bucket['max_permille']}"
            for mode, bucket in effort["by_motor_mode"].items()
        )
    )
    lines.append(
        f"raw inputs:        counts only, no conversion documented - "
        f"battery {raw['battery_counts']['min']} .. {raw['battery_counts']['max']}; "
        f"current {raw['current_counts']['min']} .. {raw['current_counts']['max']}; "
        f"nfault {','.join(raw['nfault_capabilities']) or 'none'}, "
        f"low {raw['nfault_low']['sample_count']}/{raw['nfault_low']['level_read_sample_count']} "
        f"levels read"
    )
    lines.append(
        f"faults:            latched events {faults['latched_event_count']}; "
        f"sample faults {faults['sample_fault_counts'] or 'none'}; "
        f"first at {_fmt_int(faults['first_latched_t_abs_ms'])} ms"
    )
    lines.append(
        f"authorization:     {authz['outcome_counts']}  "
        f"(denied {authz['denied_sample_count']}, drive granted {authz['drive_authorized_sample_count']})"
    )
    checks = metrics["configuration_checks"]["failing_check_counts"]
    lines.append(f"configuration:     failing checks seen {checks or 'none'}")
    lines.append("")
    lines.append(
        "This summary describes what the firmware reported. A log records what the chain "
        "believed and commanded; it is not evidence about what the vehicle did."
    )
    return "\n".join(lines) + "\n"


def summarize_comparison(metrics_by_run: Sequence[dict[str, Any]]) -> str:
    """A side-by-side table over several runs, so repeats are comparable."""
    header = (
        f"{'run_id':<16} {'samples':>8} {'dur_s':>8} {'net_m':>8} {'end_m':>8} {'target_m':>9} "
        f"{'final_phase':>14} {'fault':>22} {'drop_s':>7} {'ph_mm':>6}"
    )
    lines = [
        "Comparison across runs (descriptive only - no run is evidence for another).",
        "",
        header,
        "-" * len(header),
    ]
    for metrics in metrics_by_run:
        lines.append(
            f"{metrics['run_id']:<16} "
            f"{metrics['counts']['samples']:>8} "
            f"{_fmt(metrics['run_window']['armed_duration_s']):>8} "
            f"{_fmt(metrics['position']['net_travel_m']):>8} "
            f"{_fmt(metrics['position']['end_position_m']):>8} "
            f"{_fmt(metrics['position']['believed_target_m']):>9} "
            f"{str(metrics['endpoints']['final_phase']):>14} "
            f"{str(metrics['endpoints']['final_fault']):>22} "
            f"{metrics['drops']['samples']['final']:>7} "
            f"{metrics['transitions']['phase_change_mismatch_count']:>6}"
        )
    lines.append("")
    lines.append(
        "dur_s is the armed window. net_m is the net run-axis displacement across the "
        "log, end_m is the believed displacement from the Start Point at the last usable "
        "tick, and target_m is the believed target; all in metres."
    )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------


def _samples_x(samples: Sequence[Sample]) -> list[float]:
    return [sample.t_abs_s for sample in samples]


def _note_for(metrics: dict[str, Any]) -> str:
    if is_synthetic(metrics["run_id"], metrics["source"]["path"]):
        return "SYNTHETIC FIXTURE - generated data, not a measurement"
    return f"run {metrics['run_id']} - believed values from the firmware"


def _phase_bands(metrics: dict[str, Any]) -> list[tuple[float, float, str]]:
    bands: list[tuple[float, float, str]] = []
    for segment in metrics["phases"]["segments"]:
        bands.append((segment["start_ms"] / 1000.0, segment["end_ms"] / 1000.0, segment["phase"]))
    return bands


def _event_markers(metrics: dict[str, Any]) -> list[Event]:
    """Events worth a vertical line: faults and arming changes, not every phase."""
    interesting = {"FaultLatched", "Armed", "Disarmed", "CommandRejected", "ArmRefused"}
    return [
        event
        for event in _events_as_records(metrics)
        if event["event"] in interesting
    ]


def _events_as_records(metrics: dict[str, Any]) -> list[dict[str, Any]]:
    return metrics["transitions"]["events"]


def run_plots(metrics: dict[str, Any], log: Log) -> dict[str, str]:
    """The SVG charts for one run, keyed by relative file name.

    Every chart plots only what the log contains. The phase bands are drawn from
    the sample train's own phase column, so a reader can see when the policy
    believed it was braking, and the raw-input chart deliberately carries the
    words "raw ADC counts" in its axis label rather than an invented unit.
    """
    samples = log.samples
    if not samples:
        empty = svgplot.Chart(
            title=f"{metrics['run_id']} - empty log",
            x_label="time (s)",
            y_label="",
            note=_note_for(metrics),
        ).render()
        return {"plots/empty.svg": empty}

    xs = _samples_x(samples)
    bands = _phase_bands(metrics)
    note = _note_for(metrics)
    markers = _event_markers(metrics)
    marker_label = {"FaultLatched": "fault", "Armed": "armed", "Disarmed": "disarmed"}

    charts: dict[str, str] = {}

    position = svgplot.Chart(
        title=f"{metrics['run_id']} - run-axis position",
        x_label="time since the log's first record (s)",
        y_label="position / remaining (m)",
        note=note,
    )
    for band in bands:
        position.add_band(band[0], band[1])
    position.add_series("believed position (m)", xs, [sample.position_m for sample in samples])
    position.add_series("believed remaining (m)", xs, [sample.remaining_m for sample in samples])
    position.add_reference(0.0, "0 m")
    for event in markers:
        position.add_marker(event["t_abs_ms"] / 1000.0, marker_label.get(event["event"], ""))
    charts["plots/position.svg"] = position.render()

    velocity = svgplot.Chart(
        title=f"{metrics['run_id']} - measured vs commanded velocity",
        x_label="time since the log's first record (s)",
        y_label="velocity (m/s)",
        note=note,
    )
    for band in bands:
        velocity.add_band(band[0], band[1])
    velocity.add_series("measured (m/s)", xs, [sample.velocity_mps for sample in samples])
    velocity.add_series(
        "commanded (m/s)", xs, [sample.velocity_cmd_mps for sample in samples], dashed=True
    )
    velocity.add_reference(0.0, "0 m/s")
    for event in markers:
        velocity.add_marker(event["t_abs_ms"] / 1000.0, marker_label.get(event["event"], ""))
    charts["plots/velocity.svg"] = velocity.render()

    effort = svgplot.Chart(
        title=f"{metrics['run_id']} - requested motor effort",
        x_label="time since the log's first record (s)",
        y_label="effort (permille of full scale)",
        note=note,
        y_min=-20.0,
    )
    for band in bands:
        effort.add_band(band[0], band[1])
    effort.add_series("effort (permille)", xs, [float(sample.effort_permille) for sample in samples])
    for event in markers:
        effort.add_marker(event["t_abs_ms"] / 1000.0, marker_label.get(event["event"], ""))
    charts["plots/effort.svg"] = effort.render()

    dt = svgplot.Chart(
        title=f"{metrics['run_id']} - control tick interval",
        x_label="time since the log's first record (s)",
        y_label="reported dt (ms)",
        note=note,
    )
    dt.add_series("dt_ms", xs, [float(sample.dt_ms) for sample in samples])
    charts["plots/control_dt.svg"] = dt.render()

    raw = svgplot.Chart(
        title=f"{metrics['run_id']} - raw sensor input",
        x_label="time since the log's first record (s)",
        y_label="raw ADC counts (no conversion defined)",
        note=note,
    )
    raw.add_series(
        "battery counts",
        xs,
        [float(sample.battery_counts) for sample in samples],
        color=svgplot.PALETTE[0],
    )
    raw.add_series(
        "current counts",
        xs,
        [float(sample.current_counts) for sample in samples],
        color=svgplot.PALETTE[1],
    )
    charts["plots/raw_inputs.svg"] = raw.render()

    # The phase timeline: one step line at each phase's index, over the bands.
    order = metrics["phases"]["order"]
    index_of = {phase: float(index) for index, phase in enumerate(order)}
    phase_key = "  ".join(f"{index}={phase}" for index, phase in enumerate(order))
    timeline = svgplot.Chart(
        title=f"{metrics['run_id']} - firmware phase timeline",
        x_label="time since the log's first record (s)",
        y_label="phase index (see the note below)",
        note=f"{note}  |  phase index: {phase_key}",
    )
    for band in bands:
        timeline.add_band(band[0], band[1])
    timeline.add_series(
        "belief: phase vs time",
        xs,
        [index_of.get(sample.phase) for sample in samples],
    )
    for event in markers:
        timeline.add_marker(event["t_abs_ms"] / 1000.0, marker_label.get(event["event"], ""))
    charts["plots/phase_timeline.svg"] = timeline.render()

    return charts


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------


def run_report(
    out_dir: str,
    metrics: dict[str, Any],
    log: Log,
    *,
    include_plots: bool = True,
) -> list[str]:
    """Writes one run's report directory. Returns the relative paths written."""
    _guard(os.path.join(out_dir, "metrics.json"))
    os.makedirs(out_dir, exist_ok=True)

    written: list[str] = []
    write_json(os.path.join(out_dir, "metrics.json"), metrics)
    written.append("metrics.json")
    _write_text(os.path.join(out_dir, "summary.txt"), summarize_run(metrics))
    written.append("summary.txt")

    if include_plots:
        for relative, svg in run_plots(metrics, log).items():
            _write_text(os.path.join(out_dir, relative), svg)
            written.append(relative)

    return written


def write_manifest(out_dir: str, payload: dict[str, Any]) -> None:
    write_json(os.path.join(out_dir, "manifest.json"), payload)


def write_comparison_report(out_dir: str, metrics_by_run: Sequence[dict[str, Any]]) -> None:
    write_json(
        os.path.join(out_dir, "comparison.json"),
        {
            "note": (
                "side-by-side believed metrics; comparing runs is descriptive and does not "
                "establish repeatability without physical measurements"
            ),
            "runs": [
                {
                    "run_id": metrics["run_id"],
                    "samples": metrics["counts"]["samples"],
                    "log_duration_s": metrics["window"]["log_duration_s"],
                    "armed_duration_s": metrics["run_window"]["armed_duration_s"],
                    "net_travel_m": metrics["position"]["net_travel_m"],
                    "end_position_m": metrics["position"]["end_position_m"],
                    "believed_target_m": metrics["position"]["believed_target_m"],
                    "final_phase": metrics["endpoints"]["final_phase"],
                    "final_fault": metrics["endpoints"]["final_fault"],
                    "final_sample_drops": metrics["drops"]["samples"]["final"],
                    "phase_change_mismatch_count": metrics["transitions"][
                        "phase_change_mismatch_count"
                    ],
                }
                for metrics in metrics_by_run
            ],
        },
    )
    _write_text(os.path.join(out_dir, "comparison.txt"), summarize_comparison(metrics_by_run))


def write_endpoints_report(out_dir: str, comparison: dict[str, Any]) -> None:
    write_json(os.path.join(out_dir, "endpoints.json"), comparison)

    header = (
        f"{'run_id':<16} {'measured_end_m':>14} {'believed_end_m':>14} "
        f"{'delta_m':>9} {'measured_time_s':>16} {'believed_time_s':>16} {'delta_s':>9} "
        f"{'target_m':>9} {'pack_v':>7} {'accepted':>8} {'reason':<20}"
    )
    lines = [
        "External measurement comparison (descriptive only).",
        "",
        "The measured columns are what a person recorded by hand; the believed columns are",
        "the firmware's own numbers. `measured_end_m` is where the vehicle came to rest,",
        "measured from the start line. A difference is arithmetic, not a conclusion: judging",
        "it needs repeated runs and physical evidence, recorded in VERIFIED_STATE.md.",
        "",
        header,
        "-" * len(header),
    ]
    for row in comparison["runs"]:
        accepted = row["accepted"]
        accepted_text = "-" if accepted is None else ("yes" if accepted else "no")
        lines.append(
            f"{row['run_id']:<16} "
            f"{_fmt(row['measured_end_position_m'], 4):>14} "
            f"{_fmt(row['believed_end_position_m'], 4):>14} "
            f"{_fmt(row['end_position_delta_m'], 4):>9} "
            f"{_fmt(row['measured_run_time_s'], 3):>16} "
            f"{_fmt(row['believed_run_time_s'], 3):>16} "
            f"{_fmt(row['run_time_delta_s'], 3):>9} "
            f"{_fmt(row['target_distance_m'], 4):>9} "
            f"{_fmt(row['pack_voltage_v'], 2):>7} "
            f"{accepted_text:>8} {row['exclusion_reason'][:20]:<20}"
        )
    if comparison["measured_rows_without_a_run"]:
        lines.append("")
        lines.append(
            "measured run ids with no analyzed log: "
            + ", ".join(comparison["measured_rows_without_a_run"])
        )
    if comparison["runs_without_a_measurement"]:
        lines.append("")
        lines.append(
            "analyzed runs with no measurement row: "
            + ", ".join(comparison["runs_without_a_measurement"])
        )
    lines.append("")
    lines.append(
        "`accepted` is the operator's own judgement, recorded before the distribution was "
        "read; an excluded run keeps its reason and is never dropped from the file."
    )
    _write_text(os.path.join(out_dir, "endpoints.txt"), "\n".join(lines) + "\n")


def summarize_distribution(payload: dict[str, Any]) -> str:
    """A plain-text view of a series distribution.

    Every line is a number and its unit. The classification block prints first on
    purpose: a reader must see how many runs were excluded, and why, before any
    average can be mistaken for a summary of all of them.
    """
    classification = payload["classification"]
    lines: list[str] = [
        "Series distribution over hand measurements (descriptive only).",
        "",
        "These numbers are derived from measurements a person recorded. The raw",
        "measurements are the record; this file is arithmetic over them, and neither",
        "is by itself a conclusion about the vehicle.",
        "",
        f"source:            {payload['source']}",
        f"sha256:            {payload['source_sha256']}",
        f"classification:    total={classification['total']} "
        f"accepted={classification['accepted']} "
        f"excluded={classification['excluded']} "
        f"unjudged={classification['unjudged']}",
    ]

    if classification["excluded_runs"]:
        lines.append("")
        lines.append("excluded runs (kept in the record, not averaged in):")
        for entry in classification["excluded_runs"]:
            reason = entry["exclusion_reason"] or "(no reason recorded)"
            lines.append(f"  {entry['run_id']}: {reason}")
    if classification["unjudged_runs"]:
        lines.append("")
        lines.append(
            "unjudged runs (no accepted column recorded): "
            + ", ".join(classification["unjudged_runs"])
        )

    lines.append("")
    lines.append("accepted-run distributions:")
    for column in payload["columns"]:
        stats = payload["accepted_columns"][column]
        lines.append(
            f"  {column:<24} n={stats['count']:<4} "
            f"min={_fmt(stats['min'], 4):>10} max={_fmt(stats['max'], 4):>10} "
            f"mean={_fmt(stats['mean'], 4):>10} median={_fmt(stats['median'], 4):>10} "
            f"stdev={_fmt(stats['stdev'], 4):>10} range={_fmt(stats['range'], 4):>10}"
        )

    residual = payload["residual_end_position_m"]
    lines.append("")
    lines.append(
        f"residual (measured_end - target), m: n={residual['count']} "
        f"systematic_offset={_fmt(residual['systematic_offset_m'], 4)} "
        f"random_spread(stdev)={_fmt(residual['random_spread_m'], 4)} "
        f"min={_fmt(residual['min'], 4)} max={_fmt(residual['max'], 4)}"
    )
    lines.append(
        "                  (a small mean is calibratable; a wide spread is physical)"
    )

    groups = payload["groups"]
    if groups:
        lines.append("")
        lines.append(f"groups by {' + '.join(payload['grouped_by'])}:")
        for group in groups:
            level = group["tournament_level"] or "(no level)"
            target = _fmt(group["target_distance_m"], 3)
            end_position = group["columns"]["measured_end_position_m"]
            run_time = group["columns"]["measured_run_time_s"]
            lines.append(
                f"  level={level:<12} target_m={target:>8} "
                f"runs={group['run_count']:<3} "
                f"end_m mean={_fmt(end_position['mean'], 4):>9} "
                f"stdev={_fmt(end_position['stdev'], 4):>9} "
                f"time_s mean={_fmt(run_time['mean'], 3):>8} "
                f"stdev={_fmt(run_time['stdev'], 3):>8}"
            )

    lines.append("")
    lines.append(
        "A log-derived report says what the firmware believed; this file says what a person "
        "measured. Neither is pasted into VERIFIED_STATE.md - that needs the raw record."
    )
    return "\n".join(lines) + "\n"


def write_distribution_report(out_dir: str, payload: dict[str, Any]) -> None:
    """Writes the series distribution as JSON and as a readable text table."""
    write_json(os.path.join(out_dir, "distribution.json"), payload)
    _write_text(os.path.join(out_dir, "distribution.txt"), summarize_distribution(payload))


def write_parse_errors(out_dir: str, log: Log) -> None:
    """Records the lines a permissive parse could not read.

    Only written when there are any: a truncated or corrupted capture must leave
    a trace in the report, so nobody reads a partial metrics table as if the log
    had been complete.
    """
    if not log.errors:
        return
    lines = [
        f"Unreadable lines in {log.source} (parsed with --continue-on-error).",
        "",
        "Metrics computed from a partial log describe a partial log. Do not read them as if",
        "the capture were complete.",
        "",
    ]
    for error in log.errors:
        lines.append(error.message)
    _write_text(os.path.join(out_dir, "parse_errors.txt"), "\n".join(lines) + "\n")
