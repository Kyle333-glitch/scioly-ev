"""Timestamp-continuity tests: reorders are not rollovers (October 8 audit M8).

The firmware clock is a uint32 ``millis()`` reading. Three different things
produce a backwards raw reading and only one of them advances the epoch:

* a few-millisecond backwards step is a *reordered* reading (the firmware
  reads the clock again between stamps) and stays in the current epoch;
* a step across the top of the uint32 range is a genuine *wrap* and advances
  the epoch by exactly 2**32;
* anything else is *restart_suspected* (an unlabelled reset or a corrupt
  reading): its timeline is reconstructed monotonically but no elapsed
  duration across it is reported, because that number would be ~49.7 days
  that did not happen.

Before this repair ``_unwrap`` applied modulo-uint32 arithmetic to every
backwards step, so ``1000 -> 999 -> 1020`` reported a 4294967.316 s log
duration with matching phase spans, sample spacing and plot axes.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from tests.support import REPO_ROOT

from evtelemetry import schema
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import (
    FieldValueError,
    Log,
    classify_backwards_step,
    parse_text,
)

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


def _parse(lines: list[str]) -> Log:
    return parse_text("\n".join(lines) + "\n", source="<test>")


def _metrics(lines: list[str], run_id: str = "NEUTRAL-X") -> dict:
    return compute_metrics(_parse(lines), run_id)


def _sample_lines(stamps: list[int], **overrides: str) -> list[str]:
    return [_sample(t, **overrides) for t in stamps]


class MonotonicTest(unittest.TestCase):
    """TEST A: an ordinary forward sequence is untouched."""

    def test_forward_stamps_keep_their_readings(self) -> None:
        log = _parse(_sample_lines([1000, 1020, 1040]))
        self.assertEqual([s.t_abs_ms for s in log.samples], [1000, 1020, 1040])
        self.assertEqual([s.t_ms for s in log.samples], [1000, 1020, 1040])
        self.assertEqual(log.wrap_observations, [])
        metrics = compute_metrics(log, "NEUTRAL-A")
        self.assertEqual(metrics["window"]["log_duration_s"], 0.04)
        self.assertIsNone(metrics["window"]["duration_unavailable_reason"])


class ReorderTest(unittest.TestCase):
    """TEST B: 1000 -> 999 -> 1020 introduces no epoch and no 49.7 days."""

    def test_small_reorder_stays_in_the_current_epoch(self) -> None:
        log = _parse(_sample_lines([1000, 999, 1020]))
        # Raw readings are preserved exactly; the unwrapped timeline keeps
        # the same values because no rollover happened.
        self.assertEqual([s.t_ms for s in log.samples], [1000, 999, 1020])
        self.assertEqual([s.t_abs_ms for s in log.samples], [1000, 999, 1020])
        self.assertEqual(len(log.wrap_observations), 1)
        self.assertEqual(log.wrap_observations[0]["kind"], "reordered")

    def test_small_reorder_reports_a_small_duration(self) -> None:
        metrics = _metrics(_sample_lines([1000, 999, 1020]))
        self.assertEqual(metrics["capture"]["reordered_reading_count"], 1)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 0)
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        self.assertIsNone(metrics["window"]["duration_unavailable_reason"])
        self.assertEqual(metrics["window"]["log_duration_s"], 0.021)
        spacing = metrics["adjacent_sample_spacing"]
        self.assertEqual(spacing["max_ms"], 21)
        self.assertEqual(spacing["min_ms"], -1)
        self.assertEqual(spacing["raw_t_ms_backward_count"], 1)

    def test_reorder_classification_names_a_few_milliseconds(self) -> None:
        step = classify_backwards_step(1000, 999)
        assert step is not None
        self.assertEqual(step["kind"], "reordered")
        self.assertIsNone(classify_backwards_step(1000, 1020))
        self.assertIsNone(classify_backwards_step(None, 999))


class GenuineWrapTest(unittest.TestCase):
    """TEST C/D: a credible rollover still reconstructs a short interval."""

    def test_wrap_near_the_boundary_gives_a_short_interval(self) -> None:
        top = UINT32_SPAN
        log = _parse(_sample_lines([top - 2, 1, 20]))
        self.assertEqual(log.samples[0].t_abs_ms, top - 2)
        self.assertEqual(log.samples[1].t_abs_ms - log.samples[0].t_abs_ms, 3)
        self.assertEqual(log.samples[2].t_abs_ms - log.samples[1].t_abs_ms, 19)
        metrics = compute_metrics(log, "NEUTRAL-C")
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertEqual(metrics["capture"]["reordered_reading_count"], 0)
        self.assertEqual(metrics["window"]["log_duration_s"], 0.022)

    def test_phase_elapsed_time_survives_a_wrap(self) -> None:
        top = UINT32_SPAN
        lines = [
            _sample(top - 20, phase="Idle"),
            _sample(top - 10, phase="Idle"),
            _sample(5, phase="Approach"),
            _sample(15, phase="Approach"),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        # Idle runs from TOP-20 to the first Approach sample (unsigned +15).
        self.assertAlmostEqual(metrics["phases"]["durations_s"]["Idle"], 0.025)
        self.assertEqual(metrics["window"]["log_duration_s"], 0.035)


class FarBackwardTest(unittest.TestCase):
    """TEST E: a large step away from the boundary invents no epoch."""

    def test_far_backward_step_is_named_not_wrapped(self) -> None:
        log = _parse(_sample_lines([500000, 1200, 1220]))
        self.assertEqual(len(log.wrap_observations), 1)
        self.assertEqual(log.wrap_observations[0]["kind"], "restart_suspected")
        # The timeline stays monotonic under the firmware's unsigned
        # arithmetic; what is refused is any confident duration across it.
        self.assertEqual(
            log.samples[1].t_abs_ms - log.samples[0].t_abs_ms,
            (1200 - 500000) % UINT32_SPAN,
        )

    def test_far_backward_step_refuses_elapsed_durations(self) -> None:
        metrics = _metrics(_sample_lines([500000, 1200, 1220]))
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIn("restart", metrics["window"]["duration_unavailable_reason"] or "")


class AmbiguousDiscontinuityTest(unittest.TestCase):
    """TEST G: an ambiguous break gives unavailable timing, never a huge number."""

    def test_phase_and_spacing_do_not_inherit_the_false_epoch(self) -> None:
        metrics = _metrics(_sample_lines([500000, 1200, 1220]))
        phases = metrics["phases"]
        # The one segment spans the suspected restart, so its span and
        # duration are unknown rather than ~49.7 days.
        self.assertTrue(phases["segments"][0]["timing_unreliable"])
        self.assertIsNone(phases["segments"][0]["duration_s"])
        self.assertIsNone(phases["segments"][0]["observed_span_s"])
        self.assertIsNone(phases["durations_s"]["Idle"])
        self.assertIsNone(phases["observed_span_s"]["Idle"])
        self.assertIn("Idle", phases["timing_unreliable_phases"])
        spacing = metrics["adjacent_sample_spacing"]
        # Only the step that does not cross the break feeds min/max.
        self.assertEqual(spacing["min_ms"], 20)
        self.assertEqual(spacing["max_ms"], 20)
        self.assertEqual(spacing["unreliable_step_count"], 1)

    def test_armed_window_across_a_break_is_unavailable(self) -> None:
        metrics = _metrics(_sample_lines([500000, 1200, 1220], armed="1"))
        run_window = metrics["run_window"]
        self.assertIsNotNone(run_window["armed_first_t_abs_ms"])
        self.assertIsNotNone(run_window["armed_last_t_abs_ms"])
        self.assertIsNone(run_window["armed_duration_s"])
        self.assertIn("restart", run_window["armed_duration_unavailable_reason"] or "")

    def test_run_time_across_a_break_is_unavailable(self) -> None:
        lines = [
            _sample(500000, run_start_ms="500000", run_start_ok="1"),
            _sample(500100, run_start_ms="500000", run_start_ok="1"),
            _sample(
                1200,
                run_start_ms="500000",
                run_start_ok="1",
                dwell_start_ms="1200",
                dwell_start_ok="1",
                stop_verified_ms="1210",
                stop_verified_ok="1",
            ),
            _sample(
                1220,
                run_start_ms="500000",
                run_start_ok="1",
                dwell_start_ms="1200",
                dwell_start_ok="1",
                stop_verified_ms="1210",
                stop_verified_ok="1",
            ),
        ]
        run_window = _metrics(lines)["run_window"]
        self.assertTrue(run_window["final_stop_dwell_verified"])
        self.assertIsNone(run_window["internal_run_time_s"])
        self.assertIsNone(run_window["final_stop_verification_completed_s"])
        self.assertIn(
            "restart", run_window["internal_run_time_unavailable_reason"] or ""
        )


class BootSessionTest(unittest.TestCase):
    """TEST F/R: an explicit boot separates clock origins."""

    def _boot_capture(self) -> list[str]:
        return [
            _sample(1000),
            _sample(1020),
            _boot(0),
            _sample(20),
            _sample(40),
        ]

    def test_boot_opens_a_new_session_with_its_own_anchor(self) -> None:
        log = _parse(self._boot_capture())
        self.assertEqual(log.session_count, 2)
        self.assertEqual([s.session_index for s in log.samples], [0, 0, 1, 1])
        # Each lane re-anchors at the boot: no cross-session elapsed time.
        self.assertEqual(log.samples[2].t_abs_ms, 20)
        self.assertEqual(log.samples[3].t_abs_ms, 40)
        metrics = compute_metrics(log, "NEUTRAL-F")
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["selected_session_index"], 1)
        self.assertEqual(metrics["counts"]["samples"], 2)
        # The session window runs from its Boot event (t=0) to its last
        # sample: one anchored session, not a 49.7-day cross-session jump.
        self.assertEqual(metrics["window"]["log_duration_s"], 0.04)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 0)
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)

    def test_sessions_report_independent_windows(self) -> None:
        log = _parse(self._boot_capture())
        sessions = log.sessions
        self.assertEqual(sessions[0].first_t_abs_ms, 1000)
        self.assertEqual(sessions[0].last_t_abs_ms, 1020)
        # Session 1 opens with its Boot event at t=0; the sample lane
        # re-anchors independently at its own first reading.
        self.assertEqual(sessions[1].first_t_abs_ms, 0)
        self.assertEqual(sessions[1].last_t_abs_ms, 40)


class RepeatedStampTest(unittest.TestCase):
    """TEST H: repeated readings are stable and deterministic."""

    def test_repeated_timestamps_do_not_move_time(self) -> None:
        log = _parse(_sample_lines([1000, 1000, 1000]))
        self.assertEqual([s.t_abs_ms for s in log.samples], [1000, 1000, 1000])
        self.assertEqual(log.wrap_observations, [])
        metrics = compute_metrics(log, "NEUTRAL-H")
        self.assertEqual(metrics["window"]["log_duration_s"], 0.0)
        spacing = metrics["adjacent_sample_spacing"]
        self.assertEqual(spacing["min_ms"], 0)
        self.assertEqual(spacing["max_ms"], 0)
        self.assertEqual(spacing["zero_ms_count"], 2)


class RepeatedReorderTest(unittest.TestCase):
    """TEST I: repeated small reorders accumulate no false epochs."""

    def test_many_small_reorders_stay_small(self) -> None:
        stamps = [1000, 999, 1001, 1000, 1002, 1001, 1003]
        log = _parse(_sample_lines(stamps))
        self.assertEqual([s.t_abs_ms for s in log.samples], stamps)
        self.assertEqual(
            sum(1 for o in log.wrap_observations if o["kind"] == "reordered"), 3
        )
        self.assertFalse(
            any(o["kind"] == "wrap" for o in log.wrap_observations)
        )
        metrics = compute_metrics(log, "NEUTRAL-I")
        self.assertEqual(metrics["window"]["log_duration_s"], 0.004)
        self.assertLess(metrics["adjacent_sample_spacing"]["max_ms"], 1000)


class WrapEdgeReorderTest(unittest.TestCase):
    """TEST J/K: a reorder next to a genuine wrap confuses neither."""

    def test_reorder_immediately_before_a_wrap(self) -> None:
        top = UINT32_SPAN
        log = _parse(_sample_lines([top - 6, top - 7, 5, 15]))
        kinds = [o["kind"] for o in log.wrap_observations]
        self.assertEqual(kinds, ["reordered", "wrap"])
        self.assertEqual(
            [s.t_abs_ms for s in log.samples],
            [top - 6, top - 7, top - 6 + 12 - 1, top - 6 + 22 - 1],
        )
        metrics = compute_metrics(log, "NEUTRAL-J")
        self.assertEqual(metrics["window"]["log_duration_s"], 0.022)

    def test_reorder_immediately_after_a_wrap(self) -> None:
        top = UINT32_SPAN
        log = _parse(_sample_lines([top - 2, 3, 2, 12]))
        kinds = [o["kind"] for o in log.wrap_observations]
        self.assertEqual(kinds, ["wrap", "reordered"])
        self.assertEqual(
            [s.t_abs_ms for s in log.samples],
            [top - 2, top - 2 + 5, top - 2 + 4, top - 2 + 14],
        )
        metrics = compute_metrics(log, "NEUTRAL-K")
        self.assertEqual(metrics["window"]["log_duration_s"], 0.014)


class RestartDiagnosticsTest(unittest.TestCase):
    """TEST L: restart-suspicion diagnostics are preserved, without certainty."""

    def test_restart_observation_carries_both_deltas(self) -> None:
        log = _parse(_sample_lines([500000, 1200]))
        (observation,) = log.wrap_observations
        self.assertEqual(observation["kind"], "restart_suspected")
        self.assertEqual(observation["lane"], "sample")
        self.assertEqual(observation["from_ms"], 500000)
        self.assertEqual(observation["to_ms"], 1200)
        self.assertEqual(
            observation["unsigned_delta_ms"], (1200 - 500000) % UINT32_SPAN
        )
        self.assertEqual(observation["raw_backwards_ms"], 498800)
        metrics = compute_metrics(log, "NEUTRAL-L")
        capture = metrics["capture"]
        self.assertEqual(capture["restart_suspected_count"], 1)
        self.assertEqual(capture["restart_suspected_steps"][0]["kind"], "restart_suspected")
        self.assertEqual(capture["clock_wrap_count"], 0)


class TwoLaneTimeTest(unittest.TestCase):
    """TEST M/N: both lanes share one session-local clock meaning."""

    def test_event_and_sample_lanes_unwrap_a_rollover_together(self) -> None:
        top = UINT32_SPAN
        log = _parse(
            [
                _event(top - 10),
                _sample(top - 10),
                _event(5),
                _sample(5),
            ]
        )
        self.assertEqual(
            [item["kind"] for item in log.wrap_observations], ["wrap", "wrap"]
        )
        self.assertEqual(log.events[0].t_abs_ms, top - 10)
        self.assertEqual(log.events[1].t_abs_ms, top - 10 + 15)
        self.assertEqual(log.samples[0].t_abs_ms, top - 10)
        self.assertEqual(log.samples[1].t_abs_ms, top - 10 + 15)
        # Within one session the raw reading is the shared clock.
        self.assertEqual(log.events[0].t_ms, log.samples[0].t_ms)
        self.assertEqual(log.events[0].session_index, log.samples[0].session_index)

    def test_reorder_in_the_event_lane_stays_in_epoch(self) -> None:
        log = _parse([_event(500), _event(499), _event(520), _sample(500)])
        self.assertEqual([e.t_abs_ms for e in log.events], [500, 499, 520])
        self.assertEqual([e.t_ms for e in log.events], [500, 499, 520])
        kinds = [o["kind"] for o in log.wrap_observations if o["lane"] == "event"]
        self.assertEqual(kinds, ["reordered"])

    def test_legal_lane_interleaving_is_not_a_jump(self) -> None:
        # The event lane drains first, so a later event may precede an older
        # sample in the byte stream; per-lane anchoring reads no jump there.
        log = _parse([_event(200), _sample(100)])
        self.assertEqual(log.wrap_observations, [])
        self.assertEqual(log.events[0].t_abs_ms, 200)
        self.assertEqual(log.samples[0].t_abs_ms, 100)


class BoundaryTest(unittest.TestCase):
    """TEST X: thresholds and overflow edges behave deterministically."""

    def test_999_ms_back_is_reordered_1000_ms_back_is_not(self) -> None:
        self.assertEqual(classify_backwards_step(2000, 1001)["kind"], "reordered")
        self.assertEqual(
            classify_backwards_step(2000, 1000)["kind"], "restart_suspected"
        )

    def test_wrap_window_edge_is_exclusive(self) -> None:
        top = UINT32_SPAN
        # Exactly at the window edge the evidence is insufficient: named, not
        # reconstructed as a confident rollover.
        self.assertEqual(
            classify_backwards_step(top - 120000, 5)["kind"], "restart_suspected"
        )
        self.assertEqual(
            classify_backwards_step(top - 119999, 5)["kind"], "wrap"
        )

    def test_max_uint32_stamp_is_accepted_and_unwraps(self) -> None:
        log = _parse(_sample_lines([UINT32_SPAN - 1, 10]))
        self.assertEqual(log.samples[1].t_abs_ms - log.samples[0].t_abs_ms, 11)

    def test_epochs_cannot_accumulate_from_reorders(self) -> None:
        stamps = [10000]
        for _ in range(50):
            stamps.append(stamps[-1] - 1)
            stamps.append(stamps[-1] + 1)
        log = _parse(_sample_lines(stamps))
        self.assertFalse(any(o["kind"] == "wrap" for o in log.wrap_observations))
        span = max(s.t_abs_ms for s in log.samples) - min(
            s.t_abs_ms for s in log.samples
        )
        self.assertEqual(span, 1)


class MalformedTimeTest(unittest.TestCase):
    """TEST S: strict timestamp parsing is preserved."""

    def test_out_of_range_stamps_are_refused(self) -> None:
        with self.assertRaises(FieldValueError):
            _parse(_sample_lines([UINT32_SPAN]))
        with self.assertRaises(FieldValueError):
            _parse([_event(UINT32_SPAN)])

    def test_negative_stamps_are_refused(self) -> None:
        with self.assertRaises(FieldValueError):
            _parse([_sample(1000).replace("t_ms=1000", "t_ms=-1")])


class ReportAgreementTest(unittest.TestCase):
    """TEST O/P/Q: metrics, text and plots agree; axes stay honest."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _write(self, lines: list[str], name: str) -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("# neutral capture export\n" + "\n".join(lines) + "\n")
        return path

    def _analyze_in_process(self, path: str, run_id: str) -> str:
        from evtelemetry import cli

        out = os.path.join(self.tmp, f"report-{run_id}")
        code = cli.main(["analyze", path, "--out", out, "--run-id", run_id])
        self.assertEqual(code, 0)
        return out

    def test_json_and_text_agree_on_unknown_timing(self) -> None:
        out = self._analyze_in_process(
            self._write(_sample_lines([500000, 1200, 1220]), "restart.csv"),
            "NEUTRAL-P",
        )
        with open(os.path.join(out, "runs", "NEUTRAL-P", "metrics.json")) as handle:
            metrics = json.load(handle)
        with open(os.path.join(out, "runs", "NEUTRAL-P", "summary.txt")) as handle:
            summary = handle.read()
        self.assertIsNone(metrics["window"]["log_duration_s"])
        # The text shows an unknown duration with its reason, not a number.
        self.assertIn("log window:", summary)
        self.assertIn("unavailable", summary)
        self.assertIn("restart", summary)
        self.assertIn("suspected restarts=1", summary)

    def test_reorder_plot_axis_has_no_fabricated_epoch(self) -> None:
        out = self._analyze_in_process(
            self._write(_sample_lines([1000, 999, 1020]), "reorder.csv"),
            "NEUTRAL-Q",
        )
        with open(
            os.path.join(out, "runs", "NEUTRAL-Q", "metrics.json")
        ) as handle:
            metrics = json.load(handle)
        self.assertEqual(metrics["window"]["log_duration_s"], 0.021)
        with open(
            os.path.join(out, "runs", "NEUTRAL-Q", "plots", "position.svg")
        ) as handle:
            svg = handle.read()
        big_ticks = re.findall(r">(\d{5,})<", svg)
        self.assertEqual(big_ticks, [])

    def test_restart_plot_carries_the_timing_warning(self) -> None:
        out = self._analyze_in_process(
            self._write(_sample_lines([500000, 1200, 1220]), "restart2.csv"),
            "NEUTRAL-Q2",
        )
        with open(
            os.path.join(out, "runs", "NEUTRAL-Q2", "plots", "position.svg")
        ) as handle:
            svg = handle.read()
        self.assertIn("TIMING UNRELIABLE", svg)

    def test_regeneration_in_the_same_directory_is_stable(self) -> None:
        """TEST U: reruns reconcile safely (R1A/R1B behavior preserved)."""
        path = self._write(_sample_lines([1000, 999, 1020]), "stable.csv")
        from evtelemetry import cli

        out = os.path.join(self.tmp, "report-stable")
        self.assertEqual(cli.main(["analyze", path, "--out", out, "--run-id", "STABLE"]), 0)
        first = _tree(out)
        self.assertEqual(cli.main(["analyze", path, "--out", out, "--run-id", "STABLE"]), 0)
        self.assertEqual(_tree(out), first)
        self.assertIn("runs/STABLE/plots/position.svg", first)


def _tree(root: str) -> dict[str, str]:
    files: dict[str, str] = {}
    for directory, _, names in os.walk(root):
        for name in names:
            path = os.path.join(directory, name)
            with open(path, "r", encoding="utf-8") as handle:
                files[os.path.relpath(path, root).replace("\\", "/")] = handle.read()
    return files


class SyntheticProvenanceGuardTest(unittest.TestCase):
    """TEST T: neutral and synthetic fixtures keep their R1C meaning."""

    def test_explicit_synthetic_fixture_is_unchanged(self) -> None:
        from evtelemetry import synthetic

        spec = next(s for s in synthetic.SPECS if s.run_id == "SYNTHETIC-A")
        log = parse_text(
            "\n".join(synthetic.generate_lines(spec)) + "\n", source="<test>"
        )
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertFalse(log.provenance_conflict)

    def test_neutral_capture_declares_nothing(self) -> None:
        log = _parse(_sample_lines([1000, 999, 1020]))
        self.assertIsNone(log.declared_provenance)
        self.assertFalse(log.provenance_conflict)


class SupportedCliSubprocessTest(unittest.TestCase):
    """TEST V: the documented entry point reproduces the repair end to end."""

    def test_subprocess_reports_a_small_duration_for_the_reorder(self) -> None:
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        capture = os.path.join(tmp, "capture.csv")
        with open(capture, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("# neutral capture export\n")
            handle.write("\n".join(_sample_lines([1000, 999, 1020])) + "\n")
        out = os.path.join(tmp, "report")
        completed = subprocess.run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools", "telemetry", "analyze.py"),
                "analyze",
                capture,
                "--out",
                out,
                "--run-id",
                "NEUTRAL-V",
            ],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        with open(os.path.join(out, "runs", "NEUTRAL-V", "metrics.json")) as handle:
            metrics = json.load(handle)
        self.assertEqual(metrics["window"]["log_duration_s"], 0.021)
        self.assertEqual(metrics["capture"]["reordered_reading_count"], 1)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 0)


if __name__ == "__main__":
    unittest.main()
