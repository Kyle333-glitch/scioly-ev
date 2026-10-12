"""Endpoint comparison eligibility (October 8 audit M12).

A comparison is eligible only when the whole capture's integrity supports
attributing the believed endpoint to the one analyzed run. These tests pin
the fail-closed contract through the real parser, real metrics and the real
``compare_endpoints`` gate - plus the supported CLI, including one test via a
``python tools/telemetry/analyze.py`` subprocess:

* a clean single-session capture with a verified stop stays comparable;
* a suspected unsegmented restart refuses, with no attributable delta;
* an event-only second Boot refuses even though the sample-derived
  ``mixed_sessions`` flag is false;
* permissively skipped malformed records refuse;
* mixed sessions/runs, missing endpoints and invalid sources keep refusing;
* a genuine uint32 wrap and a benign reorder stay comparable;
* refused deltas are null, never a fabricated zero;
* descriptive believed values survive every refusal.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from tests.support import REPO_ROOT, sample_path

from evtelemetry import cli, schema
from evtelemetry.endpoints import (
    Measurement,
    MeasurementTable,
    compare_endpoints,
)
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import TelemetryFormatError, parse_text

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


def _verified(t_ms: int, pos_m: float) -> str:
    """A sample carrying the run's verified final stop."""
    return _sample(
        t_ms,
        phase="Done",
        run_start_ms="480",
        run_start_ok="1",
        dwell_start_ms="3100",
        dwell_start_ok="1",
        stop_verified_ms="6100",
        stop_verified_ok="1",
        pos_m=f"{pos_m:.4f}",
        rem_m="0.5000",
        authz="AuthorizedStop",
    )


def _clean_lines(end_pos_m: float = 2.5) -> list[str]:
    """One boot, one run, one verified stop: the eligible control."""
    return [
        "# CAPTURE bench session",
        _boot(0),
        _sample(0, pos_m="0.0000"),
        _sample(20, pos_m="1.0000"),
        _sample(40, pos_m="2.0000"),
        _verified(60, end_pos_m),
    ]


def _parse(lines: list[str], continue_on_error: bool = False):
    return parse_text(
        "\n".join(lines) + "\n", source="<test>", continue_on_error=continue_on_error
    )


def _table(*run_ids: str, measured: float = 2.0) -> MeasurementTable:
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


def _row(lines: list[str], run_id: str = "BENCH-1", **kwargs) -> dict:
    metrics = compute_metrics(_parse(lines, **kwargs), run_id)
    return compare_endpoints([metrics], _table(run_id))["runs"][0]


def run_cli(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cli.main(argv)
        except SystemExit as exit_:
            code = int(exit_.code or 0)
    return code, out.getvalue(), err.getvalue()


def _write(path: str, lines: list[str]) -> str:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return path


def _measurements_csv(path: str, run_ids: list[str]) -> str:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("run_id,measured_end_position_m,notes\n")
        for run_id in run_ids:
            handle.write(f"{run_id},2.0,tape measure\n")
    return path


class CleanCaptureTest(unittest.TestCase):
    """TEST A/B/AA: eligible captures stay comparable with honest deltas."""

    def test_clean_single_session_capture_is_comparable(self) -> None:
        row = _row(_clean_lines())
        self.assertTrue(row["comparable"])
        self.assertIsNone(row["comparison_refused_reason"])
        # Believed 2.5 m against measured 2.0 m: the delta is arithmetic.
        self.assertAlmostEqual(row["end_position_delta_m"], -0.5)
        self.assertEqual(row["believed_end_position_source"], "verified_final_stop")

    def test_clean_pair_gives_an_eligible_delta_per_run(self) -> None:
        metrics = [
            compute_metrics(_parse(_clean_lines(2.5)), "RUN-1"),
            compute_metrics(_parse(_clean_lines(3.0)), "RUN-2"),
        ]
        result = compare_endpoints(metrics, _table("RUN-1", "RUN-2"))
        self.assertEqual(len(result["refused_comparisons"]), 0)
        deltas = {row["run_id"]: row["end_position_delta_m"] for row in result["runs"]}
        self.assertAlmostEqual(deltas["RUN-1"], -0.5)
        self.assertAlmostEqual(deltas["RUN-2"], -1.0)

    def test_each_constituent_capture_is_checked_independently(self) -> None:
        """TEST S: one clean and one restarted input give split verdicts."""
        metrics = [
            compute_metrics(_parse(_clean_lines()), "GOOD"),
            compute_metrics(_parse(_restart_lines()), "BAD"),
        ]
        result = compare_endpoints(metrics, _table("GOOD", "BAD"))
        by_id = {row["run_id"]: row for row in result["runs"]}
        self.assertTrue(by_id["GOOD"]["comparable"])
        self.assertIsNotNone(by_id["GOOD"]["end_position_delta_m"])
        self.assertFalse(by_id["BAD"]["comparable"])
        self.assertIsNone(by_id["BAD"]["end_position_delta_m"])
        self.assertEqual(
            [entry["run_id"] for entry in result["refused_comparisons"]], ["BAD"]
        )


def _restart_lines() -> list[str]:
    lines = _clean_lines()
    # A large backwards step nowhere near the rollover point: an unsegmented
    # clock restart the capture did not label with a Boot.
    return [
        "# CAPTURE bench session",
        _boot(0),
        _sample(0, pos_m="0.0000"),
        _sample(500000, pos_m="1.0000"),
        _sample(1200, pos_m="2.0000"),
        _verified(1220, 2.5),
    ]


class RestartTest(unittest.TestCase):
    """TEST C/N: a suspected unsegmented restart refuses the comparison."""

    def test_suspected_restart_refuses_with_no_attributable_delta(self) -> None:
        row = _row(_restart_lines(), run_id="RESTART")
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIsNone(row["run_time_delta_s"])
        self.assertIn("suspected clock restart", row["comparison_refused_reason"])
        # Descriptive observations survive: the believed endpoint is still there.
        self.assertEqual(row["believed_end_position_m"], 2.5)
        self.assertEqual(row["believed_end_position_source"], "verified_final_stop")

    def test_large_backward_jump_without_boot_refuses(self) -> None:
        """TEST N: the R1D unsegmented-jump shape is not comparable."""
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(200000, pos_m="0.0000"),
            _sample(200020, pos_m="1.0000"),
            _sample(5000, pos_m="2.0000"),
            _verified(5020, 2.5),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIn("suspected clock restart", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])


class EventOnlyBootTest(unittest.TestCase):
    """TEST D/O/Q: an event-only extra Boot cannot bypass the sample check."""

    def test_event_only_second_boot_refuses(self) -> None:
        lines = [*_clean_lines(), _boot(99999)]
        metrics = compute_metrics(_parse(lines), "EVENTONLY")
        selection = metrics["capture"]["selection"]
        # The historical false-positive shape: two sessions, no mixed samples.
        self.assertEqual(metrics["capture"]["session_count"], 2)
        self.assertFalse(selection["mixed_sessions"])
        row = compare_endpoints([metrics], _table("EVENTONLY"))["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIn("event-only session", row["comparison_refused_reason"])

    def test_boot_followed_by_events_only_refuses(self) -> None:
        """TEST O: a second boot with event evidence but no samples refuses."""
        lines = [
            *_clean_lines(),
            _boot(99999),
            _event(100019, event="PhaseChange", detail="Done"),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIn("event-only session", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])

    def test_duplicate_boot_with_no_records_between_refuses(self) -> None:
        """TEST AB: back-to-back Boots open an event-only session."""
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _boot(5),
            _sample(20, pos_m="0.0000"),
            _sample(40, pos_m="1.0000"),
            _verified(60, 2.5),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIn("event-only session", row["comparison_refused_reason"])


class ParseErrorTest(unittest.TestCase):
    """TEST G/H: skipped malformed records refuse; strict failure is loud."""

    def test_permissively_skipped_records_refuse_attribution(self) -> None:
        lines = _clean_lines()
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        metrics = compute_metrics(
            _parse(lines, continue_on_error=True), "PARSEERR"
        )
        self.assertEqual(metrics["counts"]["parse_errors"], 1)
        row = compare_endpoints([metrics], _table("PARSEERR"))["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIn("skipped", row["comparison_refused_reason"])

    def test_strict_parse_failure_cannot_publish_success(self) -> None:
        lines = _clean_lines()
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        with self.assertRaises(TelemetryFormatError):
            _parse(lines)

    def test_restart_plus_parse_error_gives_deterministic_reasons(self) -> None:
        """TEST P: every applicable reason is preserved, in a fixed order."""
        lines = _restart_lines()
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        metrics = compute_metrics(
            _parse(lines, continue_on_error=True), "BOTH"
        )
        row = compare_endpoints([metrics], _table("BOTH"))["runs"][0]
        self.assertFalse(row["comparable"])
        reason = row["comparison_refused_reason"]
        self.assertIn("suspected clock restart", reason)
        self.assertIn("skipped", reason)
        self.assertLess(
            reason.index("suspected clock restart"), reason.index("skipped")
        )
        # Deterministic across recomputation.
        again = compare_endpoints([metrics], _table("BOTH"))["runs"][0]
        self.assertEqual(again["comparison_refused_reason"], reason)


class SessionRunAttributionTest(unittest.TestCase):
    """TEST E/F: mixed sample sessions and ambiguous runs refuse."""

    def test_mixed_sample_sessions_refuse(self) -> None:
        second = [
            _boot(0),
            _sample(0, pos_m="9.0000"),
            _verified(20, 9.5),
        ]
        row = _row([*_clean_lines(), *second])
        self.assertFalse(row["comparable"])
        self.assertIn("more than one run", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])

    def test_multiple_run_identities_refuse(self) -> None:
        """TEST F: two actuations in one session cannot be paired externally."""
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="0.0000", run_start_ms="100", run_start_ok="1"),
            _sample(20, pos_m="1.0000", run_start_ms="100", run_start_ok="1"),
            _sample(40, pos_m="1.5000", run_start_ms="200", run_start_ok="1"),
            _verified(60, 2.5),
        ]
        # The verified stop carries run 480; restamp it into the second run.
        lines[-1] = _sample(
            60,
            phase="Done",
            run_start_ms="200",
            run_start_ok="1",
            dwell_start_ms="3100",
            dwell_start_ok="1",
            stop_verified_ms="6100",
            stop_verified_ok="1",
            pos_m="2.5000",
            rem_m="0.5000",
            authz="AuthorizedStop",
        )
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIn("more than one run", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])


class EndpointPresenceTest(unittest.TestCase):
    """TEST I/J/K: no endpoint, no comparison - but no fabricated zero."""

    def test_zero_samples_refuse(self) -> None:
        lines = ["# CAPTURE bench session", _boot(0)]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIsNone(row["believed_end_position_m"])

    def test_missing_verified_stop_refuses(self) -> None:
        lines = [
            "# CAPTURE bench session",
            _boot(0),
            _sample(0, pos_m="0.0000"),
            _sample(20, pos_m="1.0000"),
        ]
        row = _row(lines)
        self.assertFalse(row["comparable"])
        self.assertIn("no verified final stop", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])
        # TEST Z: unavailable is null, never a fake zero.
        self.assertIsNotNone(row["believed_end_position_m"])

    def test_unverified_source_keeps_descriptive_values(self) -> None:
        """TEST K: latest_usable_sample keeps its numbers, not a delta."""
        row = _row(
            [
                "# CAPTURE bench session",
                _boot(0),
                _sample(0, pos_m="0.0000"),
                _sample(20, pos_m="1.0000"),
            ]
        )
        self.assertEqual(row["believed_end_position_source"], "latest_usable_sample")
        self.assertEqual(row["believed_end_position_m"], 1.0)
        self.assertIsNone(row["end_position_delta_m"])


class ClockSemanticsTest(unittest.TestCase):
    """TEST L/M/Y: R1D benign clock shapes stay comparable."""

    def test_genuine_uint32_wrap_stays_comparable(self) -> None:
        top = UINT32_SPAN - 6
        lines = [
            "# CAPTURE bench session",
            _boot(top - 40),
            _sample(top - 40, pos_m="0.0000"),
            _sample(top - 20, pos_m="1.0000"),
            _sample(5, pos_m="2.0000"),
            _verified(25, 2.5),
        ]
        metrics = compute_metrics(_parse(lines), "WRAP")
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 1)
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        row = compare_endpoints([metrics], _table("WRAP"))["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["end_position_delta_m"], -0.5)

    def test_benign_reorder_stays_comparable(self) -> None:
        """TEST M/Y: 1000 -> 999 -> 1020 is a reorder, not a restart."""
        lines = [
            "# CAPTURE bench session",
            _boot(900),
            _sample(1000, pos_m="0.0000"),
            _sample(999, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
            _verified(1040, 2.5),
        ]
        metrics = compute_metrics(_parse(lines), "REORDER")
        self.assertEqual(metrics["capture"]["restart_suspected_count"], 0)
        row = compare_endpoints([metrics], _table("REORDER"))["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["end_position_delta_m"], -0.5)


class CliIntegrationTest(unittest.TestCase):
    """Supported-CLI probes through temporary directories only."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def _analyze(
        self,
        lines: list[str],
        run_id: str,
        extra: list[str] | None = None,
    ) -> dict:
        capture = _write(os.path.join(self.tmp, f"{run_id}.csv"), lines)
        measurements = _measurements_csv(
            os.path.join(self.tmp, "meas.csv"), [run_id]
        )
        argv = [
            "analyze",
            capture,
            "--out",
            self.out,
            "--run-id",
            run_id,
            "--measurements",
            measurements,
        ]
        if extra:
            argv.extend(extra)
        code, _, err = run_cli(argv)
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def test_cli_clean_capture_is_comparable(self) -> None:
        endpoints = self._analyze(_clean_lines(), "CLEAN")
        row = endpoints["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["end_position_delta_m"], -0.5)
        self.assertEqual(endpoints["refused_comparisons"], [])

    def test_cli_restart_refuses(self) -> None:
        endpoints = self._analyze(_restart_lines(), "RESTART")
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIn("suspected clock restart", row["comparison_refused_reason"])
        text_path = os.path.join(self.out, "endpoints.txt")
        with open(text_path, encoding="utf-8") as handle:
            text = handle.read()
        # TEST U: JSON and text agree on eligibility.
        self.assertIn("comparisons refused", text)
        self.assertIn("suspected clock restart", text)

    def test_cli_event_only_boot_refuses(self) -> None:
        endpoints = self._analyze([*_clean_lines(), _boot(99999)], "EVENTONLY")
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIn("event-only session", row["comparison_refused_reason"])
        self.assertIsNone(row["end_position_delta_m"])

    def test_cli_permissive_parse_error_refuses(self) -> None:
        lines = _clean_lines()
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        endpoints = self._analyze(lines, "PARSEERR", extra=["--continue-on-error"])
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIn("skipped", row["comparison_refused_reason"])

    def test_cli_strict_parse_failure_publishes_no_success(self) -> None:
        """TEST H: exit 2 and no endpoints file posing as a result."""
        lines = _clean_lines()
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        capture = _write(os.path.join(self.tmp, "BAD.csv"), lines)
        measurements = _measurements_csv(os.path.join(self.tmp, "meas.csv"), ["BAD"])
        code, _, _ = run_cli(
            [
                "analyze",
                capture,
                "--out",
                self.out,
                "--run-id",
                "BAD",
                "--measurements",
                measurements,
            ]
        )
        self.assertEqual(code, 2)
        self.assertFalse(os.path.exists(os.path.join(self.out, "endpoints.json")))

    def test_cli_subprocess_end_to_end(self) -> None:
        """At least one regression drives the real analyze.py entry point."""
        capture = _write(os.path.join(self.tmp, "SUB.csv"), _clean_lines())
        measurements = _measurements_csv(os.path.join(self.tmp, "meas.csv"), ["SUB"])
        proc = subprocess.run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools", "telemetry", "analyze.py"),
                "analyze",
                capture,
                "--out",
                self.out,
                "--run-id",
                "SUB",
                "--measurements",
                measurements,
            ],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            row = json.load(handle)["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["end_position_delta_m"], -0.5)

    def test_cli_damaged_rerun_does_not_replay_the_old_delta(self) -> None:
        """TEST R: a damaged rerun replaces the prior valid comparison."""
        self._analyze(_clean_lines(), "RERUN")
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            self.assertTrue(json.load(handle)["runs"][0]["comparable"])
        endpoints = self._analyze(_restart_lines(), "RERUN")
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])

    def test_cli_mixed_provenance_stays_truthful(self) -> None:
        """TEST T: one synthetic and one capture input keep their labels."""
        synth = ["# SYNTHETIC fixture", *_clean_lines()[1:]]
        capture = _write(os.path.join(self.tmp, "SYN.csv"), synth)
        real = _write(os.path.join(self.tmp, "REAL.csv"), _clean_lines())
        measurements = _measurements_csv(
            os.path.join(self.tmp, "meas.csv"), ["SYN", "REAL"]
        )
        code, _, err = run_cli(
            [
                "analyze",
                capture,
                real,
                "--out",
                self.out,
                "--run-id",
                "SYN",
                "--run-id",
                "REAL",
                "--measurements",
                measurements,
            ]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "manifest.json"), encoding="utf-8") as handle:
            inputs = {
                entry["run_id"]: entry
                for entry in json.load(handle)["inputs"]
            }
        self.assertTrue(inputs["SYN"]["synthetic"])
        self.assertFalse(inputs["REAL"]["synthetic"])
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            rows = {
                row["run_id"]: row for row in json.load(handle)["runs"]
            }
        self.assertTrue(rows["SYN"]["comparable"])
        self.assertTrue(rows["REAL"]["comparable"])

    def test_cli_sample_fixture_remains_eligible(self) -> None:
        """TEST AA: the committed synthetic-A capture still compares."""
        measurements = _measurements_csv(
            os.path.join(self.tmp, "meas.csv"), ["synthetic_a"]
        )
        code, _, err = run_cli(
            [
                "analyze",
                sample_path("synthetic_a.csv"),
                "--out",
                self.out,
                "--measurements",
                measurements,
            ]
        )
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            row = json.load(handle)["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertIsNotNone(row["end_position_delta_m"])


if __name__ == "__main__":
    unittest.main()
