"""Selected-run membership: metrics and charts describe the same run (R1F).

M9: the metrics pipeline selects the last run/session, but ``run_plots()``
used to iterate the whole log, so one chart combined all-runs samples with
selected-run phase bands and markers.

M10: the selected-run event subset used to be the whole session's events (and
the run filter that did exist compared raw ``t_ms`` from the first sample
tick), so earlier-run events contaminated selected counts, transitions,
faults and windows, while legitimate actuation events preceding the first
sample were dropped.

Every test here goes through the real parser, the real selection helper and
the real metrics/report code. At least one exercises the supported CLI in a
subprocess end to end.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

_TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

from evtelemetry import schema
from evtelemetry.endpoints import compare_endpoints, load_measurements  # noqa: F401
from evtelemetry.metrics import compute_metrics, select_run
from evtelemetry.parse import parse_text
from evtelemetry.report import ReportError, run_plots

UINT32_SPAN = 1 << 32

_SAMPLE_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "dt_ms": "20",
    "phase": "Idle",
    "cmd": "none",
    "armed": "0",
    "run_start_ms": "0",
    "run_start_ok": "0",
    "dwell_start_ms": "0",
    "dwell_start_ok": "0",
    "stop_verified_ms": "0",
    "stop_verified_ok": "0",
    "pos_m": "0.0000",
    "pos_ok": "1",
    "vel_mps": "0.0000",
    "vel_ok": "1",
    "vel_cmd_mps": "0.0000",
    "rem_m": "1.0000",
    "motor": "Disabled",
    "effort_permille": "0",
    "batt_counts": "800",
    "curr_counts": "200",
    "nfault": "NotInstalled",
    "nfault_low": "-",
    "fault": "None",
    "authz": "DeniedNotArmed",
    "check": "none",
    "drop_s": "0",
    "drop_e": "0",
}

_EVENT_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "phase": "Idle",
    "armed": "0",
    "event": "TelemetryEnabled",
    "detail": "-",
    "fault": "None",
    "drop_s": "0",
    "drop_e": "0",
}


def _sample(t_ms: int, **overrides: str) -> str:
    values = dict(_SAMPLE_DEFAULTS)
    values.update(overrides)
    values["t_ms"] = str(t_ms)
    return ",".join(
        [schema.SAMPLE_PREFIX] + [f"{k}={values[k]}" for k in schema.SAMPLE_FIELDS]
    )


def _event(t_ms: int, **overrides: str) -> str:
    values = dict(_EVENT_DEFAULTS)
    values.update(overrides)
    values["t_ms"] = str(t_ms)
    return ",".join(
        [schema.EVENT_PREFIX] + [f"{k}={values[k]}" for k in schema.EVENT_FIELDS]
    )


def _boot(t_ms: int = 0) -> str:
    return _event(t_ms, event="Boot")


def _parse(lines: list[str], source: str = "<test>"):
    return parse_text("\n".join(lines) + "\n", source=source)


def _metrics(lines: list[str], run_id: str = "TEST-1") -> dict:
    return compute_metrics(_parse(lines), run_id)


def _polyline_point_counts(svg: str) -> list[int]:
    return [
        len(match.group(1).strip().split())
        for match in re.finditer(r'<polyline[^>]*points="([^"]*)"', svg)
    ]


def _marker_line_count(svg: str) -> int:
    return len(re.findall(r'stroke-dasharray="2 3"', svg))


class M9TwoBootCase(unittest.TestCase):
    """A. M9 ORIGINAL TWO-BOOT CASE."""

    def test_chart_series_contains_only_the_selected_sample(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="99.0000"),
            _boot(0),
            _sample(0, pos_m="1.0000"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "M9")
        self.assertEqual(metrics["counts"]["samples"], 1)
        self.assertEqual(
            metrics["capture"]["selection"]["selected_session_index"], 1
        )
        view = select_run(log)
        self.assertEqual(len(view.samples), 1)
        self.assertAlmostEqual(float(view.samples[0].position_m or 0.0), 1.0)
        plots = run_plots(metrics, log)
        position = plots["plots/position.svg"]
        # One sample cannot form a polyline segment; the excluded 99 m tick
        # must not stretch the axis either (before the repair the axis ran
        # 0..100 with two 2-point series).
        self.assertEqual(_polyline_point_counts(position), [])
        self.assertNotIn(">80<", position)
        self.assertNotIn(">100<", position)

    def test_two_point_selected_run_plots_two_points_not_four(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="99.0000"),
            _sample(20, pos_m="98.0000"),
            _boot(0),
            _sample(0, pos_m="1.0000"),
            _sample(20, pos_m="1.5000"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "M9B")
        self.assertEqual(metrics["counts"]["samples"], 2)
        plots = run_plots(metrics, log)
        # position/velocity carry two series each, effort one.
        self.assertEqual(_polyline_point_counts(plots["plots/position.svg"]), [2, 2])
        self.assertEqual(_polyline_point_counts(plots["plots/velocity.svg"]), [2, 2])
        self.assertEqual(_polyline_point_counts(plots["plots/effort.svg"]), [2])


class EarlierSamplesExcluded(unittest.TestCase):
    """B/C/D. Earlier-boot and earlier-run samples stay out of the charts."""

    def test_multiple_earlier_outliers_excluded(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="50.0000"),
            _sample(20, pos_m="99.0000"),
            _sample(40, pos_m="75.0000"),
            _boot(0),
            _sample(0, pos_m="1.0000"),
            _sample(20, pos_m="2.0000"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "OUTLIER")
        self.assertEqual(metrics["counts"]["samples"], 2)
        self.assertEqual(
            metrics["capture"]["selection"]["excluded_sample_count"], 3
        )
        position = run_plots(metrics, log)["plots/position.svg"]
        self.assertEqual(_polyline_point_counts(position), [2, 2])

    def test_same_session_earlier_run_samples_excluded(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="50.0000", run_start_ms="500", run_start_ok="1"),
            _sample(1020, pos_m="51.0000", run_start_ms="500", run_start_ok="1"),
            _sample(2000, pos_m="1.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="2.0000", run_start_ms="1800", run_start_ok="1"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "TWORUN")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["runs_in_session"], 2)
        self.assertEqual(selection["selected_run_start_ms"], 1800)
        self.assertEqual(metrics["counts"]["samples"], 2)
        self.assertEqual(selection["excluded_run_sample_count"], 2)
        position = run_plots(metrics, log)["plots/position.svg"]
        self.assertEqual(_polyline_point_counts(position), [2, 2])

    def test_reset_clock_boots_do_not_overlap_on_selected_axes(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(1000, pos_m="42.0000"),
            _sample(1020, pos_m="43.0000"),
            _boot(0),
            _sample(1000, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "RESET")
        self.assertEqual(metrics["counts"]["samples"], 2)
        position = run_plots(metrics, log)["plots/position.svg"]
        self.assertEqual(_polyline_point_counts(position), [2, 2])
        self.assertNotIn(">40<", position)


class PhaseAndMarkerAlignment(unittest.TestCase):
    """E/F. Bands and markers belong to the selected run."""

    def test_phase_bands_match_the_selected_series(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="5.0000", phase="Approach",
                    run_start_ms="500", run_start_ok="1"),
            _sample(1020, pos_m="5.5000", phase="Approach",
                    run_start_ms="500", run_start_ok="1"),
            _sample(2000, pos_m="1.0000", phase="Idle",
                    run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="2.0000", phase="Idle",
                    run_start_ms="1800", run_start_ok="1"),
        ]
        metrics = _metrics(lines)
        segments = metrics["phases"]["segments"]
        self.assertTrue(segments)
        for segment in segments:
            self.assertGreaterEqual(segment["start_ms"], 2000)
        self.assertEqual(
            [segment["phase"] for segment in segments], ["Idle"]
        )

    def test_markers_exclude_previous_run_fault(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="FaultLatched", detail="run1"),
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _event(1900, event="Armed"),
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "MARK")
        marker_events = [event["event"] for event in metrics["transitions"]["events"]]
        self.assertNotIn("FaultLatched", marker_events)
        self.assertIn("Armed", marker_events)
        position = run_plots(metrics, log)["plots/position.svg"]
        # One marker line: the selected run's Armed event only.
        self.assertEqual(_marker_line_count(position), 1)


class M10EventMembership(unittest.TestCase):
    """G. M10 ORIGINAL EVENT CASE, plus H/I/J/K/L/M/N/O."""

    def test_prior_run_event_excluded_from_selected_counts(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="CommandRejected", detail="run1"),
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "M10")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["runs_in_session"], 2)
        self.assertEqual(selection["selected_run_start_ms"], 1800)
        self.assertEqual(metrics["counts"]["events"], 0)
        self.assertEqual(metrics["transitions"]["events"], [])
        self.assertEqual(selection["selected_run_event_count"], 0)
        self.assertEqual(selection["excluded_run_event_count"], 1)
        self.assertEqual(selection["event_membership_rule"],
                         "at_or_after_actuation_unsigned")

    def test_pre_first_sample_actuation_event_is_kept(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="CommandRejected", detail="run1"),
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _event(1550, event="Armed"),
            _sample(1600, pos_m="6.0000", run_start_ms="1500", run_start_ok="1"),
            _sample(1620, pos_m="6.5000", run_start_ms="1500", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "PREFIRST")
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(
            [event["event"] for event in metrics["transitions"]["events"]],
            ["Armed"],
        )

    def test_single_run_keeps_events_without_actuation_context(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(900, event="Armed"),
            _sample(1000, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
        ]
        metrics = _metrics(lines, "NOACT")
        selection = metrics["capture"]["selection"]
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(selection["run_start_source"], "no_actuation_evidence")
        self.assertIsNone(selection["selected_run_start_ms"])

    def test_selected_run_without_events_reports_zero(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _event(100, event="Armed"),
            _sample(200, pos_m="9.0000"),
            _boot(0),
            _sample(100, pos_m="1.0000"),
            _sample(120, pos_m="2.0000"),
        ]
        metrics = _metrics(lines, "NOEV")
        # The selected session's own Boot is its only member event; the
        # earlier boot's Armed event is excluded from the selected view.
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(
            [event["event"] for event in metrics["transitions"]["events"]],
            ["Boot"],
        )
        self.assertEqual(
            metrics["capture"]["selection"]["excluded_event_count"], 2
        )
        # The earlier boot's events are still visible as whole-capture evidence.
        sessions = metrics["capture"]["sessions"]
        self.assertEqual(len(sessions), 2)
        self.assertEqual(sessions[0]["event_count"], 2)

    def test_earlier_run_fault_is_not_a_selected_fault(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="FaultLatched", detail="run1"),
            _sample(1200, pos_m="5.0000",
                    run_start_ms="500", run_start_ok="1"),
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "FAULTK")
        self.assertEqual(metrics["faults"]["latched_event_count"], 0)
        self.assertEqual(metrics["faults"]["latched_faults"], [])
        self.assertIsNone(metrics["faults"]["first_latched_t_abs_ms"])
        # The earlier run's fault survives nowhere in the selected view.
        self.assertEqual(
            metrics["capture"]["selection"]["excluded_run_event_count"], 1
        )

    def test_selected_run_fault_remains_visible(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _event(2100, event="FaultLatched", detail="run2"),
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "FAULTL")
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(metrics["faults"]["latched_event_count"], 1)
        self.assertEqual(
            metrics["faults"]["latched_faults"][0]["fault"], "run2"
        )
        position = run_plots(metrics, log)["plots/position.svg"]
        self.assertEqual(_marker_line_count(position), 1)

    def test_start_boundary_is_inclusive(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="CommandRejected", detail="run1"),
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _event(1500, event="Armed"),
            _sample(1600, pos_m="6.0000", run_start_ms="1500", run_start_ok="1"),
            _sample(1620, pos_m="6.5000", run_start_ms="1500", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "STARTIN")
        self.assertEqual(
            [event["event"] for event in metrics["transitions"]["events"]],
            ["Armed"],
        )

    def test_event_after_last_sample_is_a_member(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
            _event(2500, event="Disarmed"),
        ]
        metrics = _metrics(lines, "POSTLAST")
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(
            metrics["transitions"]["events"][0]["event"], "Disarmed"
        )

    def test_duplicate_same_time_events_are_stable(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _event(2100, event="Armed"),
            _event(2100, event="Disarmed"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        first = [event["event"] for event in _metrics(lines)["transitions"]["events"]]
        second = [event["event"] for event in _metrics(lines)["transitions"]["events"]]
        self.assertEqual(first, ["Armed", "Disarmed"])
        self.assertEqual(second, first)


class ClockSafety(unittest.TestCase):
    """P/Q/R. Wrap, reorder and restart around the actuation boundary."""

    def test_event_before_wrap_belongs_to_post_wrap_run(self) -> None:
        actuation = UINT32_SPAN - 6
        lines = [
            "# CAPTURE bench session",
            _sample(UINT32_SPAN - 6, pos_m="5.0000",
                    run_start_ms="100", run_start_ok="1"),
            _event(3, event="Armed"),
            _sample(5, pos_m="6.0000",
                    run_start_ms=str(actuation), run_start_ok="1"),
            _sample(30, pos_m="6.5000",
                    run_start_ms=str(actuation), run_start_ok="1"),
        ]
        metrics = _metrics(lines, "WRAP")
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        # The post-wrap Armed event precedes the first post-wrap sample tick
        # in raw numbers; only the unsigned actuation window keeps it.
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(
            metrics["transitions"]["events"][0]["event"], "Armed"
        )

    def test_pre_wrap_prior_run_event_stays_excluded(self) -> None:
        actuation = UINT32_SPAN - 6
        lines = [
            "# CAPTURE bench session",
            _event(UINT32_SPAN - 20, event="CommandRejected", detail="run1"),
            _sample(UINT32_SPAN - 6, pos_m="5.0000",
                    run_start_ms="100", run_start_ok="1"),
            _sample(5, pos_m="6.0000",
                    run_start_ms=str(actuation), run_start_ok="1"),
            _sample(30, pos_m="6.5000",
                    run_start_ms=str(actuation), run_start_ok="1"),
        ]
        metrics = _metrics(lines, "WRAPX")
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertEqual(metrics["counts"]["events"], 0)
        self.assertEqual(
            metrics["capture"]["selection"]["excluded_run_event_count"], 1
        )

    def test_benign_reorder_keeps_single_run_membership(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="1.0000"),
            _sample(999, pos_m="1.1000"),
            _sample(1020, pos_m="1.2000"),
            _event(1010, event="Armed"),
        ]
        metrics = _metrics(lines, "REORDER")
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        selection = metrics["capture"]["selection"]
        self.assertFalse(selection["mixed_runs"])
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertIsNone(selection["event_window_uncertain"])

    def test_suspected_restart_flags_the_window(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(5000, pos_m="1.0000", run_start_ms="4800", run_start_ok="1"),
            _sample(100, pos_m="2.0000", run_start_ms="4800", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "RESTART")
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 1)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIsNotNone(
            metrics["capture"]["selection"]["event_window_uncertain"]
        )


class AttributionEdges(unittest.TestCase):
    """S/T/U/V/W. Boots, announcements and partial captures."""

    def test_event_only_boot_does_not_contaminate_selected_counts(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="1.0000"),
            _sample(20, pos_m="2.0000"),
            _boot(0),
            _event(10, event="Armed"),
        ]
        metrics = _metrics(lines, "EVONLY")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["selected_session_index"], 0)
        self.assertEqual(metrics["counts"]["samples"], 2)
        # Only the selected session's own Boot is a member; the trailing
        # event-only boot contributes no selected samples or events.
        self.assertEqual(metrics["counts"]["events"], 1)
        self.assertEqual(
            [event["event"] for event in metrics["transitions"]["events"]],
            ["Boot"],
        )
        sessions = metrics["capture"]["sessions"]
        self.assertEqual(len(sessions), 2)
        self.assertEqual(sessions[1]["sample_count"], 0)
        self.assertEqual(sessions[1]["event_count"], 2)

    def test_repeated_announcement_of_one_run_is_one_run(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="1.0000", run_start_ms="500", run_start_ok="1"),
            _sample(1020, pos_m="2.0000", run_start_ms="500", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "REPEAT")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["runs_in_session"], 1)
        self.assertFalse(selection["mixed_runs"])
        self.assertEqual(selection["selected_run_start_ms"], 500)

    def test_last_run_wins_deterministically(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="1.0000", run_start_ms="500", run_start_ok="1"),
            _sample(2000, pos_m="2.0000", run_start_ms="1500", run_start_ok="1"),
            _sample(3000, pos_m="3.0000", run_start_ms="2500", run_start_ok="1"),
            _sample(3020, pos_m="3.5000", run_start_ms="2500", run_start_ok="1"),
        ]
        metrics = _metrics(lines, "THREE")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["runs_in_session"], 3)
        self.assertEqual(selection["selected_run_start_ms"], 2500)
        self.assertEqual(metrics["counts"]["samples"], 2)
        self.assertEqual(selection["excluded_run_sample_count"], 2)

    def test_partial_capture_invents_no_start(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _sample(1000, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
            _event(1010, event="Armed"),
        ]
        metrics = _metrics(lines, "PARTIAL")
        selection = metrics["capture"]["selection"]
        self.assertIsNone(selection["selected_run_start_ms"])
        self.assertEqual(selection["run_start_source"], "no_actuation_evidence")
        self.assertIsNone(metrics["run_window"]["run_start_ms"])
        self.assertEqual(metrics["counts"]["events"], 1)


class OutputAgreement(unittest.TestCase):
    """X/Y/AE. JSON, text, SVG, manifest and endpoint semantics agree."""

    def test_json_text_svg_agree_on_membership(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _event(1100, event="CommandRejected", detail="run1"),
            _sample(1200, pos_m="5.0000", run_start_ms="500", run_start_ok="1"),
            _event(1900, event="Armed"),
            _sample(2000, pos_m="6.0000", run_start_ms="1800", run_start_ok="1"),
            _sample(2020, pos_m="6.5000", run_start_ms="1800", run_start_ok="1"),
        ]
        log = _parse(lines)
        metrics = compute_metrics(log, "AGREE")
        view = select_run(log)
        self.assertEqual(metrics["counts"]["samples"], len(view.samples))
        self.assertEqual(metrics["counts"]["events"], len(view.events))
        self.assertEqual(len(metrics["transitions"]["events"]), len(view.events))
        position = run_plots(metrics, log)["plots/position.svg"]
        self.assertEqual(_polyline_point_counts(position), [2, 2])
        self.assertEqual(_marker_line_count(position), 1)

    def test_mismatched_metrics_and_log_refuse_to_plot(self) -> None:
        lines_a = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="1.0000"),
            _sample(20, pos_m="2.0000"),
        ]
        lines_b = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="9.0000"),
        ]
        metrics_a = compute_metrics(_parse(lines_a), "A")
        with self.assertRaises(ReportError):
            run_plots(metrics_a, _parse(lines_b))

    def test_endpoint_semantics_unchanged_single_run(self) -> None:
        # R1G/M11: a tick-matched verification sample (t_ms equals its own
        # stop_verified_ms, per the firmware producer contract) stays a
        # verified endpoint; the selection repair did not move this.
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="0.0000"),
            _sample(20, pos_m="1.0000"),
            _sample(
                60,
                pos_m="2.5000",
                phase="Done",
                run_start_ms="480",
                run_start_ok="1",
                dwell_start_ms="3100",
                dwell_start_ok="1",
                stop_verified_ms="60",
                stop_verified_ok="1",
                rem_m="0.5000",
                authz="AuthorizedStop",
            ),
        ]
        metrics = _metrics(lines, "EP")
        self.assertEqual(
            metrics["endpoints"]["believed_end_position_source"],
            "verified_final_stop",
        )
        self.assertAlmostEqual(
            metrics["endpoints"]["believed_end_position_m"] or 0.0, 2.5
        )


class CliIntegration(unittest.TestCase):
    """Supported-CLI subprocess regression plus regeneration/no-loss/error."""

    def _write(self, directory: str, name: str, lines: list[str]) -> str:
        path = os.path.join(directory, name)
        with open(path, "w", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
        return path

    def _analyze(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, os.path.join(_TOOLS_DIR, "analyze.py"), *args],
            capture_output=True,
            text=True,
            cwd=os.path.dirname(_TOOLS_DIR),
        )

    def test_cli_two_boot_report_matches_selected_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = self._write(
                tmp,
                "m9.csv",
                [
                    "# CAPTURE bench session",
                    _boot(0),
                    _sample(0, pos_m="99.0000"),
                    _sample(20, pos_m="98.0000"),
                    _boot(0),
                    _sample(0, pos_m="1.0000"),
                    _sample(20, pos_m="2.0000"),
                ],
            )
            out_dir = os.path.join(tmp, "out")
            proc = self._analyze(
                "analyze", log_path, "--out", out_dir, "--run-id", "M9CLI"
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            with open(os.path.join(out_dir, "runs", "M9CLI", "metrics.json")) as handle:
                metrics = json.load(handle)
            self.assertEqual(metrics["counts"]["samples"], 2)
            with open(os.path.join(out_dir, "runs", "M9CLI", "plots", "position.svg")) as handle:
                position = handle.read()
            self.assertEqual(_polyline_point_counts(position), [2, 2])
            with open(os.path.join(out_dir, "manifest.json")) as handle:
                manifest = json.load(handle)
            entry = manifest["inputs"][0]
            self.assertEqual(entry["samples"], 2)
            self.assertEqual(entry["analyzed_session_index"], 1)

    def test_cli_regeneration_replaces_stale_plots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            first = self._write(
                tmp,
                "first.csv",
                [
                    "# CAPTURE bench session",
                    _boot(0),
                    _sample(0, pos_m="1.0000"),
                    _sample(20, pos_m="2.0000"),
                    _sample(40, pos_m="3.0000"),
                ],
            )
            out_dir = os.path.join(tmp, "out")
            proc = self._analyze(
                "analyze", first, "--out", out_dir, "--run-id", "REGEN"
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            run_dir = os.path.join(out_dir, "runs", "REGEN")
            with open(os.path.join(run_dir, "plots", "position.svg")) as handle:
                self.assertEqual(_polyline_point_counts(handle.read()), [3, 3])
            second = self._write(
                tmp,
                "second.csv",
                [
                    "# CAPTURE bench session",
                    _boot(0),
                    _sample(0, pos_m="1.0000"),
                    _sample(20, pos_m="2.0000"),
                ],
            )
            proc = self._analyze(
                "analyze", second, "--out", out_dir, "--run-id", "REGEN"
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            with open(os.path.join(run_dir, "plots", "position.svg")) as handle:
                self.assertEqual(_polyline_point_counts(handle.read()), [2, 2])

    def test_cli_preserves_unknown_user_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = self._write(
                tmp,
                "loss.csv",
                [
                    "# CAPTURE bench session",
                    _boot(0),
                    _sample(0, pos_m="1.0000"),
                    _sample(20, pos_m="2.0000"),
                ],
            )
            out_dir = os.path.join(tmp, "out")
            run_dir = os.path.join(out_dir, "runs", "LOSS")
            os.makedirs(os.path.join(run_dir, "plots"), exist_ok=True)
            note_path = os.path.join(run_dir, "plots", "student-note.svg")
            with open(note_path, "w", newline="\n") as handle:
                handle.write("<svg>student</svg>")
            extra_dir = os.path.join(out_dir, "field-notes")
            os.makedirs(extra_dir, exist_ok=True)
            extra_path = os.path.join(extra_dir, "day1.txt")
            with open(extra_path, "w", newline="\n") as handle:
                handle.write("traction good, wind calm")
            proc = self._analyze(
                "analyze", log_path, "--out", out_dir, "--run-id", "LOSS"
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            with open(note_path) as handle:
                self.assertEqual(handle.read(), "<svg>student</svg>")
            with open(extra_path) as handle:
                self.assertEqual(handle.read(), "traction good, wind calm")

    def test_cli_invalid_input_publishes_nothing_complete(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad_path = self._write(
                tmp, "bad.csv", ["EVS3,ver=3,t_ms=oops\n"]
            )
            out_dir = os.path.join(tmp, "out")
            proc = self._analyze("analyze", bad_path, "--out", out_dir)
            self.assertEqual(proc.returncode, 2)
            self.assertFalse(os.path.exists(os.path.join(out_dir, "manifest.json")))


if __name__ == "__main__":
    unittest.main()
