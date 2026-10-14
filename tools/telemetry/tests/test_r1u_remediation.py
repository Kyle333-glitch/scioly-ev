"""R1U unified-remediation regressions: every actionable R1R software finding.

Each class maps to one R1R finding ID. Fixtures are built from the schema
field lists with independently written values; no builder helper is reused.
Assertions check generated numbers and unavailability fields, never just
the subprocess exit code.

Findings covered:
  R1R-M1  Windows reparse-point (junction) confinement
  R1R-M2  verification-evidence contradiction before candidate acceptance
  R1R-M3  cross-lane chronology, backfill records, negative phase durations
  R1R-M4  run_start_ms=0 with plots enabled
  R1R-K1  staged publication, invalidation, lock, recovery
  R1R-L1  provenance in every exported row and caption
  R1R-K2  extreme finite arithmetic without overflow or invented bounds
  R1R-L2  half-range event membership and long-capture ambiguity policy
  R1R-O1  sampling-window wording is descriptive, not a physical bound
Plus metamorphic properties and cross-workstream integration spot checks.

Junction tests create NTFS junctions in disposable temp directories only,
never near real user data, and skip honestly when junction creation is
unavailable. Symbolic-link privilege skips remain skips, not passes.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tests.support import REPO_ROOT

from evtelemetry import cli, schema
from evtelemetry import distribution as distribution_module
from evtelemetry.distribution import describe_values
from evtelemetry.endpoints import Measurement, MeasurementTable, compare_endpoints
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import parse_text
from evtelemetry.report import ReportError

UINT32_SPAN = 1 << 32
HALF_SPAN = UINT32_SPAN // 2

_SAMPLE_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "dt_ms": "20",
    "phase": "Idle",
    "cmd": "none",
    "armed": "1",
    "run_start_ms": "100",
    "run_start_ok": "1",
    "dwell_start_ms": "200",
    "dwell_start_ok": "1",
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
    "authz": "AuthorizedStop",
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


def _done(t_ms: int, pos_m: float, stamp_ms: int, **overrides: str) -> str:
    kw = dict(
        phase="Done",
        pos_m=f"{pos_m:.4f}",
        stop_verified_ms=str(stamp_ms),
        stop_verified_ok="1",
    )
    kw.update(overrides)
    return _sample(t_ms, **kw)


def _parse(lines: list[str], **kwargs):
    return parse_text("\n".join(lines) + "\n", source="<r1u>", **kwargs)


def _metrics(lines: list[str], run_id: str = "R1U", **kwargs) -> dict:
    return compute_metrics(_parse(lines, **kwargs), run_id)


def _table(*run_ids: str, measured: float = 5.0) -> MeasurementTable:
    rows = {
        run_id: Measurement(
            run_id=run_id,
            measured_end_position_m=measured,
            measured_run_time_s=None,
            pack_voltage_v=None,
            recharged=None,
            accepted=None,
            exclusion_reason="",
            target_distance_m=None,
            tournament_level=None,
            notes="tape measure",
        )
        for run_id in run_ids
    }
    return MeasurementTable(source="<test>", sha256="test", rows=rows)


def _row(lines: list[str], run_id: str = "R1U", **kwargs) -> dict:
    return compare_endpoints([_metrics(lines, run_id, **kwargs)], _table(run_id))["runs"][0]


def run_cli(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cli.main(argv)
        except SystemExit as exit_:
            code = int(exit_.code or 0)
    return code, out.getvalue(), err.getvalue()


def _write(path: str, lines: list[str]) -> str:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return path


def _sha256(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


class IsolatedCliCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="r1u-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "out")

    def analyze(self, *args: str) -> tuple[int, str, str]:
        return run_cli(["analyze", *args, "--out", self.out])


def _make_junction(target: str, link: str) -> None:
    """Creates an NTFS directory junction, or skips honestly."""
    if sys.platform != "win32":
        raise unittest.SkipTest("Windows junctions require Windows")
    if os.path.lexists(link):
        raise unittest.SkipTest("junction link path already exists")
    completed = subprocess.run(
        ["cmd", "/c", "mklink", "/J", link, target],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0 or not os.path.isdir(link):
        raise unittest.SkipTest(
            f"junction creation unavailable: {completed.stderr.strip()}"
        )


def _clean_log_lines() -> list[str]:
    return [
        "# CAPTURE clean",
        _boot(0),
        _sample(100, phase="FinalBrake", pos_m="5.0000"),
        _sample(120, phase="FinalBrake", pos_m="5.0000"),
        _done(3200, 5.0, 3200),
        _done(6000, 8.0, 3200),
    ]


class FilesystemConfinementTest(IsolatedCliCase):
    """R1R-M1: no analyzer mutation follows a reparse point outside the root."""

    def test_plots_junction_neither_cleaned_nor_written(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R")
        self.assertEqual(code, 0, err)
        plots = os.path.join(self.out, "runs", "R", "plots")
        self.assertTrue(os.path.isdir(plots))
        external = os.path.join(self.tmp, "external_plots")
        os.makedirs(external)
        sentinel = os.path.join(external, "position.svg")
        with open(sentinel, "wb") as handle:
            handle.write(b"<svg>external bytes</svg>")
        before = _sha256(sentinel)
        shutil.rmtree(plots)
        _make_junction(external, plots)
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertNotEqual(code, 0)
        self.assertIn("reparse", err.lower())
        self.assertTrue(os.path.isfile(sentinel))
        self.assertEqual(_sha256(sentinel), before)
        # Plot generation through the same junction is refused too.
        code, _, err = self.analyze(log, "--run-id", "R")
        self.assertNotEqual(code, 0)
        self.assertEqual(_sha256(sentinel), before)

    def test_stale_run_junction_is_never_traversed(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        stale = os.path.join(self.out, "runs", "OLD")
        os.makedirs(stale)
        with open(os.path.join(stale, "metrics.json"), "w") as handle:
            handle.write("{}")
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        external = os.path.join(self.tmp, "external_run")
        os.makedirs(external)
        sentinel = os.path.join(external, "metrics.json")
        with open(sentinel, "wb") as handle:
            handle.write(b'{"external": true}')
        before = _sha256(sentinel)
        shutil.rmtree(os.path.join(self.out, "runs", "OLD"), ignore_errors=True)
        _make_junction(external, os.path.join(self.out, "runs", "OLD"))
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(sentinel))
        self.assertEqual(_sha256(sentinel), before)

    def test_output_root_junction_is_refused(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        external = os.path.join(self.tmp, "external_root")
        os.makedirs(external)
        _make_junction(external, self.out)
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertNotEqual(code, 0)
        self.assertIn("reparse", err.lower())
        self.assertEqual(os.listdir(external), [])

    def test_runs_junction_is_refused(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        external = os.path.join(self.tmp, "external_runs")
        os.makedirs(external)
        shutil.rmtree(os.path.join(self.out, "runs"))
        _make_junction(external, os.path.join(self.out, "runs"))
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertNotEqual(code, 0)
        self.assertEqual(os.listdir(external), [])

    def test_hostile_prior_manifest_authorizes_nothing(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        external = os.path.join(self.tmp, "precious")
        os.makedirs(external)
        sentinel = os.path.join(external, "keep.json")
        with open(sentinel, "wb") as handle:
            handle.write(b'{"keep": true}')
        before = _sha256(sentinel)
        os.makedirs(self.out, exist_ok=True)
        hostile = {
            "inputs": [],
            "delete": [sentinel, "../../precious"],
            "runs": ["../../../../precious"],
        }
        with open(os.path.join(self.out, "manifest.json"), "w") as handle:
            json.dump(hostile, handle)
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        self.assertEqual(_sha256(sentinel), before)

    def test_safe_output_still_regenerates(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R")
        self.assertEqual(code, 0, err)
        first = _sha256(os.path.join(self.out, "runs", "R", "metrics.json"))
        self.assertTrue(
            os.path.isfile(os.path.join(self.out, "runs", "R", "plots", "position.svg"))
        )
        code, _, err = self.analyze(log, "--run-id", "R")
        self.assertEqual(code, 0, err)
        self.assertEqual(
            first, _sha256(os.path.join(self.out, "runs", "R", "metrics.json"))
        )

    def test_unrelated_user_svg_survives_everything(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R")
        self.assertEqual(code, 0, err)
        user_svg = os.path.join(self.out, "runs", "R", "plots", "student-note.svg")
        with open(user_svg, "wb") as handle:
            handle.write(b"<svg>student</svg>")
        before = _sha256(user_svg)
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        self.assertEqual(_sha256(user_svg), before)


class VerificationContradictionTest(unittest.TestCase):
    """R1R-M2: all verification evidence is validated before any candidate."""

    def test_contradictory_stamp_with_match_is_unavailable(self) -> None:
        lines = [
            "# CAPTURE m2a",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _done(6000, 8.0, 3300),
        ]
        row = _row(lines)
        self.assertIsNone(row["believed_end_position_m"])
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIn("3200", row["comparison_refused_reason"])
        self.assertIn("3300", row["comparison_refused_reason"])

    def test_contradictory_stamp_close_variant(self) -> None:
        lines = [
            "# CAPTURE m2a-close",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _done(3220, 5.0, 3220),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])

    def test_duplicate_positions_are_order_independent(self) -> None:
        forward = [
            "# CAPTURE dup-fwd",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _done(3200, 8.0, 3200),
        ]
        reversed_lines = [
            "# CAPTURE dup-rev",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 8.0, 3200),
            _done(3200, 5.0, 3200),
        ]
        first = _row(forward)
        second = _row(reversed_lines)
        for row in (first, second):
            self.assertIsNone(row["believed_end_position_m"])
            self.assertFalse(row["comparable"])
            self.assertIsNone(row["end_position_delta_m"])
        # Metamorphic invariant: order cannot change an eligible position -
        # both orders refuse, so no eligible delta differs.
        self.assertEqual(
            first["believed_end_position_m"], second["believed_end_position_m"]
        )

    def test_identical_duplicates_stay_usable(self) -> None:
        lines = [
            "# CAPTURE dup-same",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _done(3200, 5.0, 3200),
        ]
        row = _row(lines)
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["believed_end_position_m"], 5.0)

    def test_unusable_duplicate_refuses(self) -> None:
        lines = [
            "# CAPTURE dup-unusable",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200, pos_ok="0"),
            _done(3200, 5.0, 3200),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["believed_end_position_m"])

    def test_wrong_phase_verification_refuses(self) -> None:
        lines = [
            "# CAPTURE wrong-phase",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _sample(
                3200,
                phase="Approach",
                pos_m="5.0000",
                stop_verified_ms="3200",
                stop_verified_ok="1",
            ),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["believed_end_position_m"])
        self.assertIn("Approach", row["comparison_refused_reason"])

    def test_phase_regression_refuses(self) -> None:
        lines = [
            "# CAPTURE regression",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _sample(
                3220,
                phase="Approach",
                pos_m="5.1000",
                stop_verified_ms="3200",
                stop_verified_ok="1",
            ),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["believed_end_position_m"])

    def test_impossible_dwell_timeline_refuses(self) -> None:
        lines = [
            "# CAPTURE dwell-impossible",
            _boot(4000),
            _sample(
                4000,
                phase="FinalBrake",
                pos_m="5.0000",
                run_start_ms="4000",
                dwell_start_ms="200",
                dwell_start_ok="1",
            ),
            _done(4200, 5.0, 4200, run_start_ms="4000", dwell_start_ms="100"),
        ]
        metrics = _metrics(lines)
        self.assertIsNone(metrics["position"]["end_position_m"])
        self.assertIsNone(metrics["run_window"]["internal_run_time_s"])
        row = compare_endpoints([metrics], _table("R1U"))["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["run_time_delta_s"])

    def test_changed_dwell_between_samples_refuses(self) -> None:
        lines = [
            "# CAPTURE dwell-change",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200, dwell_start_ms="200"),
            _done(6000, 8.0, 3200, dwell_start_ms="250"),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])

    def test_valid_tick_match_stays_eligible(self) -> None:
        lines = [
            "# CAPTURE valid",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
            _done(6000, 5.5, 3200),
        ]
        row = _row(lines)
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["believed_end_position_m"], 5.0)
        self.assertAlmostEqual(row["end_position_delta_m"], 0.0)


class CrossLaneChronologyTest(unittest.TestCase):
    """R1R-M3/H: lanes share an epoch only with bridging evidence."""

    def test_sparse_pre_event_post_sample_has_no_confident_duration(self) -> None:
        top = UINT32_SPAN - 1
        lines = [
            "# CAPTURE sparse-realistic",
            _event(top, event="Armed", phase="Armed", armed="1"),
            _sample(
                20, pos_m="0.0000", run_start_ms=str(top - 20), run_start_ok="1",
                dwell_start_ms="0", dwell_start_ok="0",
            ),
            _sample(
                40, pos_m="0.1000", run_start_ms=str(top - 20), run_start_ok="1",
                dwell_start_ms="0", dwell_start_ok="0",
            ),
            _event(3000, event="Disarmed", phase="Done", armed="0"),
        ]
        metrics = _metrics(lines)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIsNotNone(metrics["window"]["duration_unavailable_reason"])
        self.assertNotAlmostEqual(
            metrics["window"]["first_t_abs_ms"] or 0,
            metrics["window"]["last_t_abs_ms"] or 0,
            delta=UINT32_SPAN // 2,
        )
        row = compare_endpoints([metrics], _table("R1U"))["runs"][0]
        self.assertFalse(row["comparable"])

    def test_sparse_post_event_pre_sample_wrap(self) -> None:
        top = UINT32_SPAN - 1
        lines = [
            "# CAPTURE event-wrap-sample-postwrap",
            _event(top, event="Armed", phase="Armed", armed="1"),
            _event(10, event="CommandRejected", phase="Armed", armed="1"),
            _sample(20, pos_m="0.0000"),
            _sample(40, pos_m="0.1000"),
            _event(3000, event="Disarmed", phase="Done", armed="0"),
        ]
        metrics = _metrics(lines)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIsNotNone(metrics["window"]["duration_unavailable_reason"])

    def test_late_pre_wrap_record_is_not_a_second_wrap(self) -> None:
        # Samples only (no event lane): the lane-local reconstruction must
        # show one rollover and ~22 ms, not a fabricated second epoch.
        top = UINT32_SPAN - 1
        lines = [
            "# CAPTURE backfill",
            _sample(top - 2, pos_m="1.0000"),
            _sample(1, pos_m="1.1000"),
            _sample(top - 1, pos_m="1.0500"),
            _sample(20, pos_m="1.2000"),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        duration = metrics["window"]["log_duration_s"]
        self.assertIsNotNone(duration)
        # (U-3 -> 1) +4 ms across the rollover, (1 -> U-2) -3 ms backfill,
        # (U-2 -> 20) +22 ms in the new epoch: 23 ms, one rollover.
        self.assertAlmostEqual(duration, 0.023, places=3)

    def test_backfill_with_ambiguous_boot_is_uncertain(self) -> None:
        # The same lane with a Boot event at raw 0 first: the Boot cannot be
        # placed in either epoch from the values alone, so cross-lane timing
        # is honestly unavailable rather than confidently wrong.
        top = UINT32_SPAN - 1
        lines = [
            "# CAPTURE backfill-boot",
            _boot(0),
            _sample(top - 2, pos_m="1.0000"),
            _sample(1, pos_m="1.1000"),
            _sample(top - 1, pos_m="1.0500"),
            _sample(20, pos_m="1.2000"),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIsNotNone(metrics["window"]["duration_unavailable_reason"])

    def test_negative_phase_duration_is_unavailable(self) -> None:
        lines = [
            "# CAPTURE phase-reorder",
            _boot(900),
            _sample(1000, phase="Approach", pos_m="0.0000"),
            _sample(999, phase="Capture", pos_m="1.0000"),
            _sample(1020, phase="Push", pos_m="2.0000"),
        ]
        metrics = _metrics(lines)
        first = metrics["phases"]["segments"][0]
        self.assertIsNone(first["duration_s"])
        self.assertTrue(first["timing_unreliable"])
        for segment in metrics["phases"]["segments"]:
            if segment["duration_s"] is not None:
                self.assertGreaterEqual(segment["duration_s"], 0.0)

    def test_genuine_wrap_keeps_short_duration(self) -> None:
        top = UINT32_SPAN - 1
        lines = [
            "# CAPTURE genuine-wrap",
            _boot(top - 60),
            _sample(top - 60, pos_m="0.0000"),
            _sample(top - 1, pos_m="1.0000"),
            _sample(1, pos_m="1.1000"),
            _sample(20, pos_m="1.2000"),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertAlmostEqual(metrics["window"]["log_duration_s"] or 0.0, 0.081, places=3)

    def test_benign_reorder_keeps_small_span(self) -> None:
        lines = [
            "# CAPTURE reorder",
            _sample(1000, pos_m="0.0000"),
            _sample(999, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        self.assertAlmostEqual(metrics["window"]["log_duration_s"] or 0.0, 0.021, places=3)

    def test_long_monotonic_capture_stays_supported(self) -> None:
        lines = ["# CAPTURE long"]
        lines.append(_boot(0))
        lines.append(_sample(0, pos_m="0.0000"))
        lines.append(_sample(3600000, pos_m="6.0000"))
        metrics = _metrics(lines)
        self.assertAlmostEqual(metrics["window"]["log_duration_s"] or 0.0, 3600.0)

    def test_unbridged_half_range_gap_is_uncertain(self) -> None:
        lines = [
            "# CAPTURE half-gap",
            _boot(0),
            _sample(100, pos_m="0.0000"),
            _sample(HALF_SPAN + 5000, pos_m="1.0000"),
        ]
        metrics = _metrics(lines)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIsNotNone(metrics["window"]["duration_unavailable_reason"])


class ZeroTimestampTest(IsolatedCliCase):
    """R1R-M4: integer zero is a valid timestamp, not a missing value."""

    def test_zero_actuation_plots_successfully(self) -> None:
        lines = [
            "# CAPTURE zero-start",
            _boot(0),
            _sample(
                0, phase="FinalBrake", pos_m="0.0000",
                run_start_ms="0", run_start_ok="1",
                dwell_start_ms="2800", dwell_start_ok="1",
            ),
            _done(3100, 5.0, 3100, run_start_ms="0", dwell_start_ms="2800"),
            _done(6000, 5.5, 3100, run_start_ms="0", dwell_start_ms="2800"),
        ]
        log = _write(os.path.join(self.tmp, "zero.log"), lines)
        plotted = os.path.join(self.tmp, "out_plots")
        code, _, err = run_cli(["analyze", log, "--out", plotted, "--run-id", "ZERO"])
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(plotted, "runs", "ZERO", "plots", "position.svg")))
        plain = os.path.join(self.tmp, "out_plain")
        code, _, err = run_cli(
            ["analyze", log, "--out", plain, "--run-id", "ZERO", "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(plotted, "runs", "ZERO", "metrics.json")) as handle:
            plotted_metrics = json.load(handle)
        with open(os.path.join(plain, "runs", "ZERO", "metrics.json")) as handle:
            plain_metrics = json.load(handle)
        self.assertEqual(
            plotted_metrics["capture"]["selection"]["selected_run_start_ms"], 0
        )
        self.assertEqual(
            plotted_metrics["position"]["end_position_m"],
            plain_metrics["position"]["end_position_m"],
        )

    def test_zero_timestamps_at_every_record(self) -> None:
        lines = [
            "# CAPTURE all-zero",
            _boot(0),
            _event(0, event="Armed", phase="Armed", armed="1"),
            _sample(
                0, phase="FinalBrake", pos_m="0.0000",
                run_start_ms="0", run_start_ok="1",
                dwell_start_ms="0", dwell_start_ok="1",
            ),
            _done(0, 1.0, 0, run_start_ms="0", dwell_start_ms="0"),
        ]
        log = _write(os.path.join(self.tmp, "allzero.log"), lines)
        code, _, err = run_cli(["analyze", log, "--out", self.out, "--run-id", "Z0"])
        self.assertEqual(code, 0, err)


class PublicationIntegrityTest(IsolatedCliCase):
    """R1R-K1: only complete generations are authoritative."""

    def _manifest_state(self) -> tuple[bool, dict | None]:
        manifest = os.path.join(self.out, "manifest.json")
        state_path = os.path.join(self.out, ".publication_state.json")
        state = None
        if os.path.isfile(state_path):
            with open(state_path, encoding="utf-8") as handle:
                state = json.load(handle)
        return os.path.isfile(manifest), state

    def test_manifest_claims_complete_only(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        present, state = self._manifest_state()
        self.assertTrue(present)
        self.assertIsNone(state)
        with open(os.path.join(self.out, "manifest.json"), encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["publication_state"], "complete")

    def test_render_failure_touches_nothing(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        before = _sha256(os.path.join(self.out, "manifest.json"))
        with mock.patch.object(
            cli,
            "render_run_artifacts",
            side_effect=ReportError("injected render failure"),
            create=True,
        ):
            code, _, _ = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertNotEqual(code, 0)
        # Rendering precedes invalidation: the previous generation is intact.
        self.assertEqual(_sha256(os.path.join(self.out, "manifest.json")), before)

    def test_commit_failure_leaves_no_manifest(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        real_write_text = cli.write_text_report  # type: ignore[attr-defined]
        calls: list[str] = []

        def flaky(path: str, text: str) -> None:
            calls.append(path)
            if len(calls) == 1:
                raise ReportError("injected commit failure")
            real_write_text(path, text)

        with mock.patch.object(cli, "write_text_report", side_effect=flaky, create=True):
            code, _, _ = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertNotEqual(code, 0)
        present, state = self._manifest_state()
        self.assertFalse(present)
        self.assertIsNotNone(state)
        self.assertEqual((state or {})["publication_state"], "incomplete")

    def test_concurrent_writer_is_refused(self) -> None:
        from evtelemetry.report import acquire_output_lock
        from evtelemetry.report import release_output_lock

        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        os.makedirs(self.out, exist_ok=True)
        acquire_output_lock(self.out)
        try:
            code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
            self.assertNotEqual(code, 0)
            self.assertIn("lock", err.lower())
        finally:
            release_output_lock(self.out)
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)

    def test_staging_residue_recovers(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        staging = os.path.join(self.out, ".staging")
        os.makedirs(staging)
        with open(os.path.join(staging, "junk.txt"), "w") as handle:
            handle.write("leftover")
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(os.path.join(staging, "junk.txt")))
        present, _ = self._manifest_state()
        self.assertTrue(present)

    def test_source_output_collision_is_refused(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        before = _sha256(os.path.join(self.out, "manifest.json"))
        nested_log = os.path.join(self.out, "runs", "R", "metrics.json")
        code, _, err = self.analyze(nested_log, "--run-id", "R2")
        self.assertNotEqual(code, 0)
        self.assertEqual(_sha256(os.path.join(self.out, "manifest.json")), before)


class ProvenanceExportTest(IsolatedCliCase):
    """R1R-L1: every export carries fixture provenance; CAPTURE stays untrusted."""

    def _run_pair(self, header: str, run_id: str) -> dict:
        lines = [
            header,
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
        ]
        log = _write(os.path.join(self.tmp, f"{run_id}.log"), lines)
        measurements = os.path.join(self.tmp, f"{run_id}.csv")
        with open(measurements, "w", newline="\n") as handle:
            handle.write("run_id,measured_end_position_m,notes\n")
            handle.write(f"{run_id},5.0,tape measure\n")
        out = os.path.join(self.tmp, f"out_{run_id}")
        code, _, err = run_cli(
            ["analyze", log, "--out", out, "--run-id", run_id,
             "--measurements", measurements, "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "endpoints.json"), encoding="utf-8") as handle:
            endpoints = json.load(handle)
        with open(os.path.join(out, "endpoints.txt"), encoding="utf-8") as handle:
            endpoints_text = handle.read()
        return {"endpoints": endpoints, "text": endpoints_text, "out": out}

    def test_synthetic_header_reaches_every_export(self) -> None:
        result = self._run_pair("# SYNTHETIC r1u provenance", "SYNTH-1")
        row = result["endpoints"]["runs"][0]
        self.assertTrue(row["synthetic"])
        self.assertIsNotNone(row["provenance_warning"])
        self.assertIn("SYNTHETIC", result["text"])

    def test_capture_declaration_stays_untrusted_in_exports(self) -> None:
        result = self._run_pair("# CAPTURE bench", "NEUTRAL-Q2")
        row = result["endpoints"]["runs"][0]
        self.assertFalse(row["synthetic"])
        self.assertIn("untrusted", (row["provenance_warning"] or "").lower())
        self.assertIn("CAPTURE", result["text"])
        # Eligibility policy is unchanged: descriptive comparison still works.
        self.assertTrue(row["comparable"])

    def test_conflicting_declarations_stay_visible(self) -> None:
        lines = [
            "# CAPTURE bench",
            "# SYNTHETIC r1u conflict",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
        ]
        log = _write(os.path.join(self.tmp, "conflict.log"), lines)
        out = os.path.join(self.tmp, "out_conflict")
        code, _, err = run_cli(
            ["analyze", log, "--out", out, "--run-id", "NEUTRAL-Q3", "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "runs", "NEUTRAL-Q3", "metrics.json")) as handle:
            metrics = json.load(handle)
        self.assertTrue(metrics["capture"]["provenance_conflict"])

    def test_comparison_carries_per_run_provenance(self) -> None:
        synth_log = _write(
            os.path.join(self.tmp, "s.log"),
            ["# SYNTHETIC mixed", _boot(0),
             _sample(100, phase="FinalBrake", pos_m="5.0000"),
             _done(3200, 5.0, 3200)],
        )
        cap_log = _write(
            os.path.join(self.tmp, "c.log"),
            ["# CAPTURE mixed", _boot(0),
             _sample(100, phase="FinalBrake", pos_m="6.0000"),
             _done(3200, 6.0, 3200)],
        )
        out = os.path.join(self.tmp, "out_mixed")
        code, _, err = run_cli(
            ["analyze", synth_log, cap_log, "--out", out,
             "--run-id", "SYNTH-M", "--run-id", "NEUTRAL-M", "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "comparison.json"), encoding="utf-8") as handle:
            payload = json.load(handle)
        by_id = {row["run_id"]: row for row in payload["runs"]}
        self.assertTrue(by_id["SYNTH-M"]["synthetic"])
        self.assertFalse(by_id["NEUTRAL-M"]["synthetic"])
        self.assertIn("SYNTH-M", payload["synthetic_runs"])
        with open(os.path.join(out, "comparison.txt"), encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("SYNTHETIC", text)


class ExtremeArithmeticTest(IsolatedCliCase):
    """R1R-K2: extreme finite values never overflow, clamp, or corrupt JSON."""

    def test_repeated_extremes_give_exact_statistics(self) -> None:
        stats = describe_values([1e308, 1e308])
        self.assertEqual(stats["count"], 2)
        self.assertEqual(stats["mean"], 1e308)
        self.assertEqual(stats["median"], 1e308)
        self.assertEqual(stats["stdev"], 0.0)
        self.assertEqual(stats["range"], 0.0)

    def test_unrepresentable_spread_refuses_by_name(self) -> None:
        with self.assertRaises(distribution_module.DistributionError) as caught:
            describe_values([-1e308, 1e308])
        self.assertIn("range", str(caught.exception))

    def test_distribution_cli_handles_representable_extremes(self) -> None:
        # Two rows of 1e308 have an exactly representable mean/median and a
        # zero spread: sound arithmetic handles them without refusal.
        measurements = os.path.join(self.tmp, "extreme.csv")
        with open(measurements, "w", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,accepted,exclusion_reason\n"
                "E1,1e308,yes,\n"
                "E2,1e308,yes,\n"
            )
        out = os.path.join(self.tmp, "dist")
        code, _, err = run_cli(["distribution", measurements, "--out", out])
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "distribution.json"), encoding="utf-8") as handle:
            payload = json.load(handle)
        column = payload["accepted_columns"]["measured_end_position_m"]
        self.assertEqual(column["mean"], 1e308)
        self.assertEqual(column["stdev"], 0.0)

    def test_distribution_cli_refuses_unrepresentable_spread_cleanly(self) -> None:
        measurements = os.path.join(self.tmp, "spread.csv")
        with open(measurements, "w", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,accepted,exclusion_reason\n"
                "E1,-1e308,yes,\n"
                "E2,1e308,yes,\n"
            )
        code, _, err = run_cli(
            ["distribution", measurements, "--out", os.path.join(self.tmp, "dist2")]
        )
        self.assertEqual(code, 2)
        self.assertNotIn("Traceback", err)
        self.assertIn("representable", err)
        self.assertFalse(
            os.path.exists(os.path.join(self.tmp, "dist2", "distribution.json"))
        )

    def test_distribution_cli_succeeds_on_representable_extremes(self) -> None:
        measurements = os.path.join(self.tmp, "ok.csv")
        with open(measurements, "w", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,accepted,exclusion_reason\n"
                "E1,1e308,yes,\n"
            )
        out = os.path.join(self.tmp, "dist_ok")
        code, _, err = run_cli(["distribution", measurements, "--out", out])
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "distribution.json"), encoding="utf-8") as handle:
            payload = json.load(handle)
        column = payload["accepted_columns"]["measured_end_position_m"]
        self.assertEqual(column["mean"], 1e308)

    def test_extreme_endpoint_delta_refuses_without_crash(self) -> None:
        lines = [
            "# CAPTURE extreme-endpoint",
            _boot(0),
            _sample(100, phase="FinalBrake", pos_m="5.0000"),
            _done(3200, 5.0, 3200),
        ]
        metrics = _metrics(lines, "EXTREME")
        # An opposite-sign extreme pair whose difference is not a finite
        # float refuses the comparison instead of emitting Infinity.
        metrics["position"]["end_position_m"] = -1e308
        table = MeasurementTable(
            source="<test>",
            sha256="test",
            rows={
                "EXTREME": Measurement(
                    run_id="EXTREME",
                    measured_end_position_m=1e308,
                    measured_run_time_s=None,
                    pack_voltage_v=None,
                    recharged=None,
                    accepted=None,
                    exclusion_reason="",
                    target_distance_m=None,
                    tournament_level=None,
                    notes="",
                )
            },
        )
        result = compare_endpoints([metrics], table)
        row = result["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        json.dumps(result, allow_nan=False)

    def test_normal_values_are_unchanged(self) -> None:
        stats = describe_values([1.0, 2.0, 3.0])
        self.assertEqual(
            stats,
            {
                "count": 3,
                "min": 1.0,
                "max": 3.0,
                "mean": 2.0,
                "median": 2.0,
                "stdev": 1.0,
                "range": 2.0,
                "max_abs_deviation_from_mean": 1.0,
            },
        )


class LongCapturePolicyTest(unittest.TestCase):
    """R1R-L2/H: supported horizons work; ambiguous epochs refuse honestly."""

    def test_ancient_event_does_not_contaminate_selected_run(self) -> None:
        start2 = HALF_SPAN + 5000
        lines = [
            "# CAPTURE half-range",
            _event(0, event="FaultLatched", detail="old", phase="Fault"),
            _sample(100, pos_m="0.0000", run_start_ms="100", run_start_ok="1"),
            _sample(
                start2, pos_m="1.0000",
                run_start_ms=str(start2), run_start_ok="1",
                dwell_start_ms="0", dwell_start_ok="0",
            ),
            _sample(
                start2 + 3200, pos_m="1.5000", phase="Done",
                run_start_ms=str(start2), run_start_ok="1",
                dwell_start_ms="0", dwell_start_ok="0",
            ),
        ]
        metrics = _metrics(lines)
        self.assertEqual(metrics["faults"]["latched_event_count"], 0)
        self.assertIsNotNone(
            metrics["capture"]["selection"]["event_window_uncertain"]
        )

    def test_ordinary_two_run_membership_is_unchanged(self) -> None:
        lines = [
            "# CAPTURE two-runs",
            _boot(0),
            _event(90, event="Armed", phase="Armed", armed="1"),
            _sample(100, pos_m="0.0000", run_start_ms="100", run_start_ok="1"),
            _event(1990, event="Disarmed", phase="Done", armed="0"),
            _event(2090, event="Armed", phase="Armed", armed="1"),
            _sample(2100, pos_m="1.0000", run_start_ms="2000", run_start_ok="1"),
        ]
        metrics = _metrics(lines)
        kinds = [event["event"] for event in metrics["transitions"]["events"]]
        self.assertIn("Armed", kinds)
        self.assertNotIn("Disarmed", kinds)

    def test_sampling_window_is_descriptive_not_a_bound(self) -> None:
        metrics = _metrics(_clean_log_lines())
        note = metrics["endpoints"]["note"]
        self.assertNotIn("bounds how far", note)
        self.assertIn("sampling delta", note)


class MetamorphicPropertyTest(IsolatedCliCase):
    """Cross-cutting invariants that hold regardless of input order or path."""

    def test_identical_inputs_give_identical_bytes(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        first = os.path.join(self.tmp, "first")
        second = os.path.join(self.tmp, "second")
        for out in (first, second):
            code, _, err = run_cli(
                ["analyze", log, "--out", out, "--run-id", "R", "--no-plots"]
            )
            self.assertEqual(code, 0, err)
        for name in ("manifest.json", "runs/R/metrics.json", "runs/R/summary.txt"):
            self.assertEqual(_sha256(os.path.join(first, name)), _sha256(os.path.join(second, name)))

    def test_zero_is_never_normalized_away(self) -> None:
        metrics = _metrics(_clean_log_lines())
        window = metrics["window"]
        self.assertIsNotNone(window["first_t_abs_ms"])
        self.assertGreaterEqual(window["first_t_abs_ms"], 0)

    def test_synthetic_stays_synthetic_under_comment_noise(self) -> None:
        lines = (
            ["# ordinary operator note", "# exported by student", "# SYNTHETIC r1u"]
            + _clean_log_lines()[1:]
        )
        log = _write(os.path.join(self.tmp, "noisy.log"), lines)
        code, _, err = self.analyze(log, "--run-id", "SYNTH-NOISE", "--no-plots")
        self.assertEqual(code, 0, err)
        with open(
            os.path.join(self.out, "runs", "SYNTH-NOISE", "metrics.json"),
            encoding="utf-8",
        ) as handle:
            metrics = json.load(handle)
        self.assertEqual(metrics["capture"]["provenance_declared"], "SYNTHETIC")

    def test_damaged_input_cannot_make_a_valid_pair_comparable(self) -> None:
        good = _write(os.path.join(self.tmp, "good.log"), _clean_log_lines())
        bad_lines = ["# CAPTURE bad", _boot(0), "EVS3,ver=3,t_ms=BOGUS"]
        bad = _write(os.path.join(self.tmp, "bad.log"), bad_lines)
        code, _, err = run_cli(
            ["analyze", good, bad, "--out", self.out,
             "--run-id", "GOOD", "--run-id", "BAD",
             "--continue-on-error", "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "comparison.json"), encoding="utf-8") as handle:
            payload = json.load(handle)
        self.assertEqual(len(payload["runs"]), 2)
        by_id = {row["run_id"]: row for row in payload["runs"]}
        self.assertNotEqual(
            by_id["GOOD"]["end_position_m"], by_id["BAD"]["end_position_m"]
        )

    def test_interrupted_tree_is_detectable(self) -> None:
        log = _write(os.path.join(self.tmp, "clean.log"), _clean_log_lines())
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        os.remove(os.path.join(self.out, "manifest.json"))
        with open(os.path.join(self.out, ".publication_state.json"), "w") as handle:
            json.dump({"publication_state": "incomplete", "reason": "test"}, handle)
        self.assertFalse(os.path.isfile(os.path.join(self.out, "manifest.json")))
        code, _, err = self.analyze(log, "--run-id", "R", "--no-plots")
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "manifest.json")))


if __name__ == "__main__":
    unittest.main()
