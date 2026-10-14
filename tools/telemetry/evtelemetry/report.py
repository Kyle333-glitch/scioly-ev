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

import contextlib
import hashlib
import json
import os
import stat
from typing import Any, Optional, Sequence

from . import schema, svgplot, synthetic
from .metrics import select_run
from .parse import Event, Log, Sample

__all__ = [
    "PROTECTED_OUTPUT_NAMES",
    "STAGING_DIR_NAME",
    "LOCK_FILE_NAME",
    "PUBLICATION_STATE_NAME",
    "ReportError",
    "is_reparse_or_link",
    "validate_output_confinement",
    "acquire_output_lock",
    "release_output_lock",
    "recover_interrupted_publication",
    "mark_publication_incomplete",
    "clear_publication_state",
    "render_run_artifacts",
    "write_text_report",
    "reconcile_run_dir",
    "build_comparison_payload",
    "reconcile_root_reports",
    "reconcile_stale_run_dirs",
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

_FILE_ATTRIBUTE_REPARSE_POINT = getattr(
    stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
)


class ReportError(ValueError):
    """The report could not be written as asked."""


def is_reparse_or_link(path: str) -> bool:
    """True when ``path`` itself is a symbolic link, junction, or reparse point.

    On Windows, Python's ``os.path.islink`` returns ``False`` for NTFS directory
    junctions (mount-point reparse points), yet ``os.isdir``, ``os.listdir``,
    ``os.remove``, ``os.makedirs`` and ``open`` follow them. Checking
    ``os.path.isjunction`` (where available) and ``os.lstat``'s
    ``FILE_ATTRIBUTE_REPARSE_POINT`` / ``st_reparse_tag`` catches junctions as
    well as ordinary symbolic links without following the link target.
    """
    try:
        if os.path.islink(path):
            return True
    except OSError:
        return False
    isjunction = getattr(os.path, "isjunction", None)
    if isjunction is not None:
        try:
            if isjunction(path):
                return True
        except OSError:
            pass
    try:
        st = os.lstat(path)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    if getattr(st, "st_reparse_tag", 0) != 0:
        return True
    if getattr(st, "st_file_attributes", 0) & _FILE_ATTRIBUTE_REPARSE_POINT:
        return True
    return False


def _guard(path: str) -> None:
    """Refuses any output path that names a project authority file or reparse point.

    Every path component is checked, not just the last one. A *directory* named
    after an authority file inside a report tree is a trap for whoever reads the
    tree next, so it is refused too - the rule is easier to state and easier to
    rely on that way. In addition, no existing ancestor or target component may
    be a symbolic link, Windows junction, or reparse point (R1R-M1).
    """
    abs_path = os.path.abspath(path)
    components = os.path.normpath(abs_path).split(os.sep)
    for component in components:
        if component in PROTECTED_OUTPUT_NAMES:
            raise ReportError(
                f"refusing to write '{component}': the path names a project authority "
                f"file, and this tool writes only to its own output directory"
            )
    drive, tail = os.path.splitdrive(abs_path)
    parts = [part for part in tail.split(os.sep) if part and part != "."]
    current = drive + os.sep if drive else os.sep
    for part in parts:
        current = os.path.join(current, part)
        if os.path.lexists(current) and is_reparse_or_link(current):
            raise ReportError(
                f"refusing to write through reparse point or symbolic link: {current}"
            )


def validate_output_confinement(
    out_dir: str,
    *,
    expect_files: Sequence[str] = (),
    expect_dirs: Sequence[str] = (),
) -> None:
    """Verifies that ``out_dir`` and all expected managed slots are confined.

    Raises :class:`ReportError` before any mutation if:

    * ``out_dir`` or any ancestor is a protected name or reparse point/junction;
    * ``out_dir`` exists as a regular file rather than a directory;
    * any expected directory slot (e.g. ``runs``, ``runs/<id>``, ``plots``)
      exists as a non-directory or reparse point/junction;
    * any expected file slot (or its ``.partial`` staging sibling) exists as a
      directory or reparse point/junction.
    """
    _guard(out_dir)
    if os.path.lexists(out_dir):
        if is_reparse_or_link(out_dir) or not os.path.isdir(out_dir):
            raise ReportError(
                f"refusing to use output destination {out_dir!r}: path exists and is "
                "not a regular directory"
            )
    out_abs = os.path.abspath(out_dir)
    for rel_dir in expect_dirs:
        norm = os.path.normpath(rel_dir)
        if os.path.isabs(rel_dir) or norm.startswith(".."):
            raise ReportError(
                f"refusing to use directory path outside output root: {rel_dir!r}"
            )
        target_dir = os.path.join(out_abs, norm)
        _guard(target_dir)
        if os.path.lexists(target_dir):
            if is_reparse_or_link(target_dir) or not os.path.isdir(target_dir):
                raise ReportError(
                    f"refusing to write into {target_dir!r}: path is not a regular directory"
                )
    for rel_file in expect_files:
        norm = os.path.normpath(rel_file)
        if os.path.isabs(rel_file) or norm.startswith(".."):
            raise ReportError(
                f"refusing to use file path outside output root: {rel_file!r}"
            )
        target_file = os.path.join(out_abs, norm)
        _guard(target_file)
        _guard(target_file + ".partial")
        parent = os.path.dirname(target_file)
        if os.path.lexists(parent):
            if is_reparse_or_link(parent) or not os.path.isdir(parent):
                raise ReportError(
                    f"refusing to write {target_file!r}: parent {parent!r} is not a regular directory"
                )
        for candidate in (target_file, target_file + ".partial"):
            if os.path.lexists(candidate):
                if is_reparse_or_link(candidate) or os.path.isdir(candidate):
                    raise ReportError(
                        f"refusing to write {candidate!r}: path exists and is a directory or reparse point"
                    )


def _write_text(path: str, text: str) -> None:
    _guard(path)
    if os.path.lexists(path) and (is_reparse_or_link(path) or os.path.isdir(path)):
        raise ReportError(
            f"refusing to write {path!r}: destination is a directory or reparse point"
        )
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except OSError as error:
        raise ReportError(f"could not write {path}: {error}") from None


def _write_text_atomic(path: str, text: str) -> None:
    """Writes a file by rename, so a reader never sees a half-written one.

    A JSON report is read by other tools; a partially flushed file is a file that
    parses as something else. The temporary name is fixed (not random) because a
    leftover from an interrupted run should be recognisable rather than a new
    mystery file each time, and it is removed if the write fails.
    """
    _guard(path)
    temporary = path + ".partial"
    _guard(temporary)
    for target in (path, temporary):
        if os.path.lexists(target) and (is_reparse_or_link(target) or os.path.isdir(target)):
            raise ReportError(
                f"refusing to write {target!r}: destination is a directory or reparse point"
            )
    directory = os.path.dirname(path) or "."
    try:
        os.makedirs(directory, exist_ok=True)
        with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except ReportError:
        with contextlib.suppress(OSError):
            if not is_reparse_or_link(temporary):
                os.remove(temporary)
        raise
    except OSError as error:
        with contextlib.suppress(OSError):
            if not is_reparse_or_link(temporary):
                os.remove(temporary)
        raise ReportError(f"could not write {path}: {error}") from None
    except BaseException:
        with contextlib.suppress(OSError):
            if not is_reparse_or_link(temporary):
                os.remove(temporary)
        raise


def write_json(path: str, payload: Any) -> None:
    """JSON with sorted keys and a trailing newline, so a diff is meaningful.

    ``allow_nan=False``: `json.dumps` writes the bare tokens ``NaN`` and
    ``Infinity`` by default, which are not JSON and which read as numbers in a
    JSON5 parser and as errors in a strict one. A non-finite value reaching this
    point is a defect upstream, so it fails here rather than producing a file that
    only some readers accept.
    """
    try:
        text = json.dumps(payload, sort_keys=True, indent=2, allow_nan=False) + "\n"
    except ValueError as error:
        raise ReportError(
            f"refusing to write non-finite JSON to {os.path.basename(path)}: {error}"
        ) from None
    _write_text_atomic(path, text)


def is_synthetic(
    run_id: str,
    source: Optional[str],
    declared_provenance: Optional[str] = None,
) -> bool:
    """True when a run is a generated fixture rather than a captured session.

    Two things can say so, and either is enough: the capture's own header
    (``# SYNTHETIC ...``, carried through the parse as ``declared_provenance``),
    or the run id / file name. A name may **add** a warning and never remove one,
    so a fixture copied to ``BENCH-1.csv`` still reports as synthetic.
    """
    if (declared_provenance or "").strip().upper().startswith(synthetic.SYNTHETIC_MARKER):
        return True
    haystack = f"{run_id} {source or ''}".lower()
    return synthetic.SYNTHETIC_MARKER.lower() in haystack


def _declared_provenance(metrics: dict[str, Any]) -> Optional[str]:
    capture = metrics.get("capture") or {}
    return capture.get("provenance_declared")


def _provenance_line(metrics: dict[str, Any]) -> Optional[str]:
    """The provenance warning for a run, with where the claim came from.

    The synthetic question has exactly one answer (see ``is_synthetic``): a
    header ``SYNTHETIC`` declaration or a synthetic run id/name. A declared
    ``CAPTURE`` is reported as what it is - a claim by the capture, not a
    verified measurement - and never as a synthetic warning, so the text
    summary cannot disagree with the JSON manifest or the plot captions.
    """
    declared = _declared_provenance(metrics)
    by_name = is_synthetic(metrics["run_id"], metrics["source"]["path"])
    if declared and declared.strip().upper().startswith(synthetic.SYNTHETIC_MARKER):
        if (metrics.get("capture") or {}).get("provenance_conflict"):
            return (
                "SYNTHETIC FIXTURE - declared in the capture header, not measured "
                "(a conflicting CAPTURE declaration was also present and was not honored)"
            )
        if by_name:
            return "SYNTHETIC FIXTURE - declared in the capture header, not measured"
        return (
            "SYNTHETIC FIXTURE - declared in the capture header, not measured "
            "(the run id/name does not say so)"
        )
    if by_name:
        return "SYNTHETIC FIXTURE - generated, not measured (no header declaration)"
    if declared:
        return f"provenance declared by the capture: {declared}"
    return None


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
    capture = metrics["capture"]
    selection = capture["selection"]

    lines: list[str] = []
    lines.append(f"run_id:            {metrics['run_id']}")
    provenance = _provenance_line(metrics)
    if provenance:
        lines.append(f"provenance:        {provenance}")
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
        f"capture:           sessions={capture['session_count']} "
        f"runs_in_session={selection['runs_in_session']} "
        f"analyzed_session={selection['selected_session_index']} "
        f"analyzed_run_start_ms={_fmt_int(selection['selected_run_start_ms'])} "
        f"excluded_samples={selection['excluded_sample_count']}"
    )
    if selection["mixed_sessions"] or selection["mixed_runs"]:
        lines.append(
            "                   MIXED CAPTURE: more than one run is present, so this "
            "report describes the last one and an external measurement cannot be "
            "attributed to it"
        )
    if (
        capture["clock_wrap_count"]
        or capture["reordered_reading_count"]
        or capture["restart_suspected_count"]
    ):
        lines.append(
            f"clock:             wraps={capture['clock_wrap_count']} "
            f"reordered readings={capture['reordered_reading_count']} "
            f"suspected restarts={capture['restart_suspected_count']} "
            "(a wrap is a uint32 rollover; a reordered reading is a few ms "
            "backwards; a suspected restart is neither and its elapsed time "
            "is unavailable)"
        )
    lines.append(
        f"log window:        {_fmt_int(window['first_t_abs_ms'])} ms .. "
        f"{_fmt_int(window['last_t_abs_ms'])} ms  ({_fmt(window['log_duration_s'])} s)"
    )
    if window["duration_unavailable_reason"]:
        lines.append(f"timing:            unavailable ({window['duration_unavailable_reason']})")
    lines.append(
        f"armed window:      {_fmt_int(run_window['armed_first_t_abs_ms'])} ms .. "
        f"{_fmt_int(run_window['armed_last_t_abs_ms'])} ms  "
        f"({_fmt(run_window['armed_duration_s'])} s)"
    )
    if run_window.get("armed_duration_unavailable_reason"):
        lines.append(
            f"                   armed duration unavailable "
            f"({run_window['armed_duration_unavailable_reason']})"
        )
    lines.append(
        f"run timeline:      actuated {_fmt_int(run_window['run_start_ms'])} ms  "
        f"final-stop dwell start {_fmt_int(run_window['final_stop_dwell_start_ms'])} ms  "
        f"verified {_fmt_int(run_window['stop_verified_ms'])} ms"
    )
    lines.append(
        f"internal run time: {_fmt(run_window['internal_run_time_s'])} s  "
        f"(actuation to the verified final-stop dwell start; the Complete Stop interval is "
        f"excluded, and the official Run Time comes from a timekeeper or video)"
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
        # Transition-bounded durations: the phase's first sample to the next
        # phase's first sample. A phase still in effect at the end of the capture
        # has no such bound, so it prints its observed span and is marked.
        rendered = " | ".join(
            f"{phase} {_fmt(phases['durations_s'].get(phase), 3)}s"
            + ("(censored)" if phase in phases["censored_phases"] else "")
            for phase in phase_order
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
        f"net travel {_fmt(position['net_travel_m'])}; "
        f"usable {position['usable_sample_count']}/{counts['samples']}"
    )
    lines.append(
        f"believed endpoint: {_fmt(position['end_position_m'])} m "
        f"({position['end_position_source'] or 'unavailable'}"
        + (
            f", {position['verified_stop_leg']} leg"
            if position["verified_stop_leg"]
            else ""
        )
        + f"; sampling window {_fmt(position['end_position_sampling_window_m'], 4)} m "
        f"over {_fmt_int(position['end_position_sampling_window_ms'])} ms); "
        f"after-stop travel {_fmt(position['after_stop_travel_m'])} m; "
        f"believed target {_fmt(position['believed_target_m'])} m "
        f"({position['believed_target_source'] or 'unavailable'})"
    )
    if position["end_position_unavailable_reason"]:
        lines.append(
            f"                   ({position['end_position_unavailable_reason']})"
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
        f"{'run_id':<16} {'samples':>8} {'run_s':>8} {'net_m':>8} {'end_m':>8} {'target_m':>9} "
        f"{'final_phase':>14} {'fault':>22} {'drop_s':>7} {'ph_mm':>6}"
    )
    lines = [
        "Comparison across runs (descriptive only - no run is evidence for another).",
        "",
    ]
    fixture_runs = [
        metrics["run_id"]
        for metrics in metrics_by_run
        if _comparison_provenance(metrics)["synthetic"]
    ]
    if fixture_runs:
        lines.append(
            "SYNTHETIC FIXTURE(S): "
            + ", ".join(fixture_runs)
            + " - generated data, not measurements and never calibration evidence."
        )
        lines.append("")
    lines += [
        header,
        "-" * len(header),
    ]
    for metrics in metrics_by_run:
        lines.append(
            f"{metrics['run_id']:<16} "
            f"{metrics['counts']['samples']:>8} "
            f"{_fmt(metrics['run_window']['internal_run_time_s']):>8} "
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
        "run_s is the firmware's internal Run Time: actuation to the start of the verified "
        "final-stop dwell, so the Complete Stop interval is not counted - and a run that "
        "never verified a final stop has none. It is the firmware's own clock, not the "
        "official Run Time. net_m is the net run-axis displacement across the analyzed "
        "run, end_m is the believed displacement from the Start Point **at the verified "
        "final stop** (blank when the run never verified one - the last sample is not a "
        "stop position), and target_m is the believed target, reconstructed with the sign "
        "of the leg the run stopped on; all in metres."
    )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------


def _samples_x(samples: Sequence[Sample]) -> list[float]:
    return [sample.t_abs_s for sample in samples]


def _note_for(metrics: dict[str, Any]) -> str:
    # One interpretation, shared with the manifest: ``is_synthetic`` with the
    # declared provenance is the same call ``cli`` uses for ``manifest.json``.
    # A declared CAPTURE (or any non-synthetic declaration) must therefore
    # never produce a synthetic plot note while the JSON says synthetic=false.
    if is_synthetic(
        metrics["run_id"], metrics["source"]["path"], _declared_provenance(metrics)
    ):
        note = "SYNTHETIC FIXTURE - generated data, not a measurement"
    else:
        note = f"run {metrics['run_id']} - believed values from the firmware"
    # A chart whose x-axis spans a suspected restart would otherwise read as
    # a confident ~49.7-day timeline. The series are still plotted (no
    # observation is dropped), but the axis is labelled untrusted.
    if (metrics.get("window") or {}).get("duration_unavailable_reason"):
        note += " | TIMING UNRELIABLE: the elapsed axis spans a suspected clock restart"
    return note


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

    Every chart plots only the selected run's records (R1F: M9). The series
    come from the same shared selection view the metrics describe, the phase
    bands are drawn from the selected sample train's own phase column, and the
    event markers come from the metrics' selected-run transitions - so a chart
    can no longer combine all-runs samples with selected-run bands and
    markers. The raw-input chart deliberately carries the words "raw ADC
    counts" in its axis label rather than an invented unit.
    """
    view = select_run(log)
    recorded = metrics["capture"]["selection"]
    if (
        view.session_index != recorded["selected_session_index"]
        or view.run_start_ms != recorded.get("selected_run_start_ms")
        or len(view.samples) != metrics["counts"]["samples"]
        or len(view.events) != len(metrics["transitions"]["events"])
    ):
        # The metrics and the log disagree about which run is selected: plotting
        # one run's series under another run's bands and markers is exactly the
        # M9 defect, so refuse instead of drawing it.
        raise ReportError(
            "the metrics and the log describe different selected runs; refusing "
            "to plot"
        )
    samples = list(view.samples)
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


# The per-analysis artifacts this tool owns inside a run directory. Reconciling
# exactly these means a rerun replaces the set it produces and removes what it no
# longer produces, without ever touching a file this tool did not write.
_REPORT_ARTIFACTS: tuple[str, ...] = ("metrics.json", "summary.txt", "parse_errors.txt")

# Ownership contract for the ``plots/`` directory (M14): these seven names are
# the only SVGs ``run_plots()`` can write - ``empty.svg`` for an empty log, or
# the six charts for a non-empty one. A rerun may delete one of these names
# when the current analysis did not produce it. Any other file in ``plots/`` -
# including any other ``.svg`` name, at any letter case - is NOT tool-owned and
# is never deleted. A ``.svg`` extension alone never proves ownership. In
# particular ``--no-plots`` removes the previous generation's managed plots but
# leaves every unmanaged file (for example ``plots/student-note.svg``) alone.
# Name matching is case-insensitive because a report tree written on one
# machine is read on another and Windows folds letter case: ``POSITION.SVG``
# occupies the same managed slot as ``position.svg``. A symbolic link is never
# tool-owned either: a link at a managed name, or a ``plots`` entry that is
# itself a link, disables cleanup for that entry rather than removing it.
_MANAGED_PLOTS: tuple[str, ...] = (
    "plots/empty.svg",
    "plots/position.svg",
    "plots/velocity.svg",
    "plots/effort.svg",
    "plots/control_dt.svg",
    "plots/raw_inputs.svg",
    "plots/phase_timeline.svg",
)


def _reconcile(out_dir: str, kept: Sequence[str]) -> list[str]:
    """Removes this tool's own artifacts that the current run did not write.

    A clean rerun must not leave the previous run's ``parse_errors.txt`` beside a
    metrics table with no errors, and ``--no-plots`` must not leave the previous
    plots behind: both would attribute an old result to new bytes. Only the
    known artifact names (``_REPORT_ARTIFACTS`` plus ``_MANAGED_PLOTS``) are
    considered, and only their removal is reported - anything else in the
    directory is left alone. Deletion paths are built from those fixed names
    only; entries of ``kept`` are used for membership checks and never turned
    into filesystem paths, so a manifest or caller value cannot authorize a
    deletion outside the managed set.
    """
    removed: list[str] = []
    keep_folded = {str(entry).casefold() for entry in kept}
    plots_dir = os.path.join(out_dir, "plots")
    plots_is_link = is_reparse_or_link(plots_dir)
    plots_is_dir = not plots_is_link and os.path.isdir(plots_dir)
    # Actual on-disk names keyed by folded name, so a managed slot is found
    # whatever letter case the filesystem stored. Unmanaged names never enter
    # the candidate list however they are cased.
    on_disk: dict[str, str] = {}
    if plots_is_dir:
        with contextlib.suppress(OSError):
            for name in sorted(os.listdir(plots_dir)):
                on_disk.setdefault(name.casefold(), name)
    for relative in list(_REPORT_ARTIFACTS) + list(_MANAGED_PLOTS):
        for candidate in (relative, relative + _PARTIAL_SUFFIX):
            if candidate.casefold() in keep_folded:
                continue
            head, _, tail = candidate.partition("/")
            if head == "plots":
                if not plots_is_dir:
                    continue
                actual = on_disk.get(tail.casefold())
                if actual is None:
                    continue
                path = os.path.join(plots_dir, actual)
                label = f"{head}/{actual}"
            else:
                path = os.path.join(out_dir, candidate)
                label = candidate
            if is_reparse_or_link(path):
                continue
            if os.path.isfile(path):
                try:
                    os.remove(path)
                except OSError as error:
                    raise ReportError(f"could not reconcile {label}: {error}") from None
                removed.append(label)
    if plots_is_dir:
        with contextlib.suppress(OSError):
            if not os.listdir(plots_dir):
                os.rmdir(plots_dir)
    return removed


# Ownership contract for the report root (M15): these four names are the only
# root files the ``analyze`` command conditionally produces - ``comparison.*``
# only for two or more runs, ``endpoints.*`` only with ``--measurements``. A
# rerun that no longer produces one of these pairs removes it, so a stale
# report cannot masquerade as the current generation's. Any other root file -
# including ``distribution.json/.txt`` (owned by the ``distribution``
# subcommand, never by ``analyze``), ``manifest.json`` (rewritten by every
# generation, so never stale), unknown ``.json``/``.txt``/``.svg`` names, and
# anything a user planted - is NOT tool-owned and is never deleted. Name
# matching is case-insensitive for the same cross-machine Windows reason as
# ``_MANAGED_PLOTS``. A symbolic link is never tool-owned either. Deletion
# paths are built from these fixed names only; no prior-manifest or caller
# value is ever turned into a filesystem path, so a corrupt or hostile prior
# manifest cannot authorize a deletion outside the managed set.
_MANAGED_ROOT_REPORTS: tuple[str, ...] = (
    "comparison.json",
    "comparison.txt",
    "endpoints.json",
    "endpoints.txt",
)

# The fixed temporary suffix ``_write_text_atomic`` writes through. When a
# managed root slot is reconciled away, its recognisable leftover is removed
# with it; the suffix alone never authorises deleting any other name, and a
# ``.partial`` beside a slot the current generation kept is left alone.
_PARTIAL_SUFFIX = ".partial"


def reconcile_root_reports(
    out_dir: str,
    *,
    keep_comparison: bool,
    keep_endpoints: bool,
) -> list[str]:
    """Removes managed root reports the current generation did not produce.

    Call after every current artifact is written and before the manifest
    claims the tree, so a stale ``comparison`` (from a rerun with fewer than
    two runs) or stale ``endpoints`` (from a rerun without ``--measurements``)
    cannot sit beside a manifest that contradicts it. Only the fixed
    ``_MANAGED_ROOT_REPORTS`` names (plus their ``.partial`` leftovers) are
    considered; anything else at the root is left alone. Returns the removed
    file names as found on disk.
    """
    removed: list[str] = []
    candidates: list[str] = []
    for slot in _MANAGED_ROOT_REPORTS:
        if slot.startswith("comparison.") and keep_comparison:
            continue
        if slot.startswith("endpoints.") and keep_endpoints:
            continue
        candidates.append(slot)
        candidates.append(slot + _PARTIAL_SUFFIX)
    if not candidates:
        return removed
    on_disk: dict[str, str] = {}
    with contextlib.suppress(OSError):
        for name in sorted(os.listdir(out_dir)):
            on_disk.setdefault(name.casefold(), name)
    for candidate in candidates:
        actual = on_disk.get(candidate.casefold())
        if actual is None:
            continue
        path = os.path.join(out_dir, actual)
        if is_reparse_or_link(path):
            continue
        if os.path.isfile(path):
            try:
                os.remove(path)
            except OSError as error:
                raise ReportError(f"could not reconcile {actual}: {error}") from None
            removed.append(actual)
    return removed


def _fold_run_dir_name(name: str) -> str:
    """Folds a ``runs/`` entry the way the analyze preflight folds run ids."""
    return name.casefold().rstrip(" .")


def reconcile_stale_run_dirs(out_dir: str, current_run_dirs: Sequence[str]) -> list[str]:
    """Reconciles previous generations' run directories the current inputs drop.

    Only immediate children of ``<out>/runs`` whose folded name matches no
    current run directory are considered, and only their tool-owned managed
    files (``_REPORT_ARTIFACTS`` plus ``_MANAGED_PLOTS``, via ``_reconcile``)
    are removed. Unknown files, symbolic links, plain files directly under
    ``runs/``, and a ``runs`` entry that is itself a symlink are all
    preserved; a stale directory that still holds unknown content is left in
    place, and without its metrics report it cannot pose as a current run.
    The directory itself is removed only when it is left empty.

    A stale entry whose folded name matches a current run is treated as that
    run's slot and preserved untouched. Deletion targets are confined to the
    managed report destination: entries are single path components listed
    from ``runs/`` itself (never manifest or caller paths), every candidate
    is verified to stay inside it, and links are never followed.

    Returns the removed entries as paths relative to ``runs/``.
    """
    removed: list[str] = []
    runs_dir = os.path.join(out_dir, "runs")
    if is_reparse_or_link(runs_dir) or not os.path.isdir(runs_dir):
        return removed
    try:
        runs_abs = os.path.abspath(runs_dir)
        current = {_fold_run_dir_name(os.path.basename(path)) for path in current_run_dirs}
        entries = sorted(os.listdir(runs_dir))
    except OSError:
        return removed
    for entry in entries:
        if entry in (".", ".."):
            continue
        if _fold_run_dir_name(entry) in current:
            continue
        path = os.path.join(runs_dir, entry)
        if is_reparse_or_link(path) or not os.path.isdir(path):
            continue
        if os.path.abspath(path) != os.path.join(runs_abs, entry):
            continue
        for label in _reconcile(path, []):
            removed.append(f"{entry}/{label}")
        with contextlib.suppress(OSError):
            if not os.listdir(path):
                os.rmdir(path)
                removed.append(entry)
    return removed


# Publication contract (R1R-K1): a report directory carries at most one
# authoritative generation, identified by `manifest.json`. The manifest is
# written last and only after every artifact it names is in place; a
# directory without a manifest (or with a `.publication_state.json` saying
# `incomplete`) is not a complete generation and must not be read as one.
# Rendering happens in memory (or a confined staging directory) before any
# live managed slot is touched, so a failure during rendering leaves the
# previous generation intact, and a failure during commit leaves no manifest
# claiming completeness. Full filesystem transactionality is explicitly NOT
# claimed: concurrent writers are refused via an exclusive lock file, and a
# path that changes between validation and use (TOCTOU) is refused where
# detected but cannot be ruled out on this platform - hostile concurrent
# mutation of the output tree during publication is unsupported and refused
# rather than guaranteed safe.
STAGING_DIR_NAME = ".staging"
LOCK_FILE_NAME = ".analyze.lock"
PUBLICATION_STATE_NAME = ".publication_state.json"


def acquire_output_lock(out_dir: str) -> None:
    """Takes the exclusive writer lock, or refuses a concurrent writer.

    The lock is a plain file created exclusively: whoever holds it is the
    only authorized writer. A second concurrent invocation fails with a
    controlled error instead of interleaving two generations. A lock left by
    a killed process is not silently stolen; the message says how to clear
    it after confirming no writer is running.
    """
    _guard(os.path.join(out_dir, LOCK_FILE_NAME))
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, LOCK_FILE_NAME)
    if os.path.lexists(path) and (
        is_reparse_or_link(path) or os.path.isdir(path)
    ):
        raise ReportError(
            f"refusing to publish into {out_dir!r}: the writer lock is a "
            "directory or reparse point"
        )
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        raise ReportError(
            f"refusing to publish into {out_dir!r}: another analysis appears "
            f"to be writing there ({LOCK_FILE_NAME} exists); concurrent "
            "writers are not supported - wait for it to finish, or, after "
            "confirming no writer is running, remove the lock file and retry"
        ) from None
    except OSError as error:
        raise ReportError(f"could not take the writer lock in {out_dir!r}: {error}") from None
    try:
        os.write(fd, str(os.getpid()).encode("ascii"))
    finally:
        os.close(fd)


def release_output_lock(out_dir: str) -> None:
    """Releases the writer lock, never following a reparse point."""
    path = os.path.join(out_dir, LOCK_FILE_NAME)
    try:
        if os.path.lexists(path) and not is_reparse_or_link(path):
            os.remove(path)
    except OSError:
        pass


def recover_interrupted_publication(out_dir: str) -> list[str]:
    """Cleans staging and partial-file residue from an interrupted run.

    Runs under the writer lock before rendering, so its deletions cannot race
    a live commit. Staging content is never promoted here - only discarded -
    and managed `.partial` leftovers are removed alongside it. Returns what
    was removed, relative to the output root.
    """
    removed: list[str] = []
    staging = os.path.join(out_dir, STAGING_DIR_NAME)
    if os.path.lexists(staging):
        if is_reparse_or_link(staging) or not os.path.isdir(staging):
            raise ReportError(
                f"refusing to recover {out_dir!r}: staging path is a reparse "
                "point or file, not a regular directory"
            )
        for root, dirs, files in os.walk(staging, topdown=True):
            for name in files:
                full = os.path.join(root, name)
                if is_reparse_or_link(full):
                    continue
                with contextlib.suppress(OSError):
                    os.remove(full)
                removed.append(os.path.relpath(full, out_dir))
            for name in list(dirs):
                full = os.path.join(root, name)
                if is_reparse_or_link(full):
                    dirs.remove(name)
        with contextlib.suppress(OSError):
            os.rmdir(staging)
    return removed


def mark_publication_incomplete(out_dir: str, reason: str) -> None:
    """Records that no complete generation is published, without a manifest.

    Called after the previous manifest is invalidated and before/while the
    new artifacts commit, so a reader can tell an interrupted tree from a
    complete one. Never followed by a manifest write on the same path
    without a successful full commit.
    """
    payload = {
        "publication_state": "incomplete",
        "reason": reason,
        "note": (
            "no complete generation is published: the manifest was invalidated "
            "before this commit and the commit did not finish"
        ),
    }
    write_json(os.path.join(out_dir, PUBLICATION_STATE_NAME), payload)


def clear_publication_state(out_dir: str) -> None:
    """Removes the incomplete-publication marker after a successful commit."""
    path = os.path.join(out_dir, PUBLICATION_STATE_NAME)
    if os.path.lexists(path) and not is_reparse_or_link(path):
        with contextlib.suppress(OSError):
            os.remove(path)


def render_run_artifacts(
    metrics: dict[str, Any],
    log: Log,
    *,
    include_plots: bool = True,
) -> dict[str, tuple[str, str]]:
    """Renders one run's report files in memory, without touching the disk.

    Returns ``{relative_path: (kind, text)}`` where kind is ``"json"`` (text
    is the canonical JSON encoding of the metrics) or ``"text"``. The
    caller commits the mapping through the atomic writers, then reconciles
    the directory against its keys - so rendering failures never leave a
    half-written live generation behind.
    """
    artifacts: dict[str, tuple[str, str]] = {}
    artifacts["metrics.json"] = (
        "json",
        json.dumps(metrics, sort_keys=True, indent=2, allow_nan=False) + "\n",
    )
    artifacts["summary.txt"] = ("text", summarize_run(metrics))
    if include_plots:
        for relative, svg in run_plots(metrics, log).items():
            artifacts[relative] = ("text", svg)
    if log.errors:
        artifacts["parse_errors.txt"] = ("text", _parse_errors_text(log))
    return artifacts


def write_text_report(path: str, text: str) -> None:
    """Writes a text report file through the confinement guard."""
    _write_text(path, text)


def reconcile_run_dir(out_dir: str, kept: Sequence[str]) -> list[str]:
    """Public alias for per-run reconciliation against a committed set."""
    return _reconcile(out_dir, kept)


def _comparison_provenance(metrics: dict[str, Any]) -> dict[str, Any]:
    """The normalized fixture provenance of one compared run (R1R-L1)."""
    declared = (metrics["capture"] or {}).get("provenance_declared")
    conflict = bool((metrics["capture"] or {}).get("provenance_conflict"))
    header_synthetic = bool(
        declared and declared.strip().upper().startswith(synthetic.SYNTHETIC_MARKER)
    )
    name_synthetic = is_synthetic(metrics["run_id"], metrics["source"]["path"])
    is_fixture = header_synthetic or name_synthetic
    if is_fixture:
        warning = "SYNTHETIC FIXTURE - generated data, not a measurement"
        if conflict:
            warning += (
                " (a conflicting CAPTURE declaration was also present and was "
                "not honored)"
            )
        elif not header_synthetic:
            warning += " (synthetic run id or file name; no header declaration)"
    elif declared:
        warning = (
            f"provenance {declared!r} is declared by the capture itself, not "
            "verified: a CAPTURE label is an untrusted claim, not proof that "
            "hardware measurements occurred"
        )
    else:
        warning = None
    return {
        "synthetic": is_fixture,
        "provenance_declared": declared,
        "provenance_conflict": conflict,
        "provenance_warning": warning,
    }


def build_comparison_payload(
    metrics_by_run: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    """The side-by-side believed-metrics payload, without writing it."""
    provenance_by_run = [_comparison_provenance(metrics) for metrics in metrics_by_run]
    return {
        "note": (
            "side-by-side believed metrics; comparing runs is descriptive and does not "
            "establish repeatability without physical measurements"
        ),
        "provenance_note": (
            "CAPTURE provenance is declared by the capture itself and is unverified; "
            "SYNTHETIC runs are generated fixtures, never calibration evidence. "
            "Per-run provenance travels in each run entry and must accompany any "
            "quotation of these numbers."
        ),
        "synthetic_runs": [
            metrics["run_id"]
            for metrics, provenance in zip(metrics_by_run, provenance_by_run)
            if provenance["synthetic"]
        ],
        "runs": [
            {
                "run_id": metrics["run_id"],
                "synthetic": provenance["synthetic"],
                "provenance_declared": provenance["provenance_declared"],
                "provenance_conflict": provenance["provenance_conflict"],
                "provenance_warning": provenance["provenance_warning"],
                "samples": metrics["counts"]["samples"],
                "log_duration_s": metrics["window"]["log_duration_s"],
                "armed_duration_s": metrics["run_window"]["armed_duration_s"],
                "internal_run_time_s": metrics["run_window"]["internal_run_time_s"],
                "final_stop_verification_completed_s": metrics["run_window"][
                    "final_stop_verification_completed_s"
                ],
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
            for metrics, provenance in zip(metrics_by_run, provenance_by_run)
        ],
    }


def run_report(
    out_dir: str,
    metrics: dict[str, Any],
    log: Log,
    *,
    include_plots: bool = True,
) -> list[str]:
    """Writes one run's report directory. Returns the relative paths written.

    The directory is reconciled against what this analysis produced, so it never
    holds a mix of two analyses of different bytes.
    """
    validate_output_confinement(
        out_dir,
        expect_dirs=["."],
        expect_files=["metrics.json", "summary.txt"],
    )
    os.makedirs(out_dir, exist_ok=True)

    rendered = render_run_artifacts(metrics, log, include_plots=include_plots)

    written: list[str] = []
    for relative, (kind, text) in rendered.items():
        if kind == "json":
            write_json(os.path.join(out_dir, relative), metrics)
        else:
            _write_text(os.path.join(out_dir, relative), text)
        written.append(relative)

    _reconcile(out_dir, written)
    return written


def write_manifest(out_dir: str, payload: dict[str, Any]) -> None:
    write_json(os.path.join(out_dir, "manifest.json"), payload)


def write_comparison_report(out_dir: str, metrics_by_run: Sequence[dict[str, Any]]) -> None:
    write_json(
        os.path.join(out_dir, "comparison.json"),
        build_comparison_payload(metrics_by_run),
    )
    _write_text(os.path.join(out_dir, "comparison.txt"), summarize_comparison(metrics_by_run))


def write_endpoints_report(out_dir: str, comparison: dict[str, Any]) -> None:
    write_json(os.path.join(out_dir, "endpoints.json"), comparison)

    header = (
        f"{'run_id':<16} {'measured_end_m':>14} {'believed_end_m':>14} "
        f"{'delta_m':>9} {'measured_time_s':>16} {'believed_time_s':>16} {'delta_s':>9} "
        f"{'target_m':>9} {'pack_v':>7} {'accepted':>8} {'reason':<20}"
    )
    synthetic_rows = [
        str(row["run_id"])
        for row in comparison["runs"]
        if row.get("synthetic")
    ]
    declared_rows = [
        f"{row['run_id']}={row.get('capture_provenance_declared')}"
        for row in comparison["runs"]
        if not row.get("synthetic") and row.get("capture_provenance_declared")
    ]
    lines = [
        "External measurement comparison (descriptive only).",
        "",
        "The measured columns are what a person recorded by hand; the believed columns are",
        "the firmware's own numbers. `measured_end_m` is where the vehicle came to rest,",
        "measured from the start line. A difference is arithmetic, not a conclusion: judging",
        "it needs repeated runs and physical evidence, recorded in VERIFIED_STATE.md.",
        "",
    ]
    if synthetic_rows:
        lines.append(
            "SYNTHETIC FIXTURE(S): "
            + ", ".join(synthetic_rows)
            + " - generated data, not measurements and never calibration evidence."
        )
        lines.append("")
    if declared_rows:
        lines.append(
            "Capture-declared provenance (untrusted claims, not verified measurements): "
            + ", ".join(declared_rows)
            + ". A CAPTURE label does not prove hardware measurements occurred."
        )
        lines.append("")
    lines += [
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
    refusals = comparison.get("refused_comparisons") or []
    if refusals:
        lines.append("")
        lines.append("comparisons refused (the believed endpoint is not attributable):")
        for entry in refusals:
            lines.append(f"  {entry['run_id']}: {entry['reason']}")
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


def _parse_errors_text(log: Log) -> str:
    lines = [
        f"Unreadable lines in {log.source} (parsed with --continue-on-error).",
        "",
        "Metrics computed from a partial log describe a partial log. Do not read them as if",
        "the capture were complete.",
        "",
    ]
    for error in log.errors:
        lines.append(error.message)
    return "\n".join(lines) + "\n"


def write_parse_errors(out_dir: str, log: Log) -> None:
    """Records the lines a permissive parse could not read.

    A capture with errors leaves a trace in the report, so nobody reads a partial
    metrics table as if the log had been complete. A capture with no errors
    *removes* the trace a previous analysis of the same run left, because a stale
    ``parse_errors.txt`` beside a clean metrics table is the same misreading in
    the other direction.
    """
    path = os.path.join(out_dir, "parse_errors.txt")
    if not log.errors:
        _guard(path)
        if os.path.isfile(path):
            os.remove(path)
        return
    _write_text(path, _parse_errors_text(log))
