"""Verified stop-position attribution (October 8 audit M11).

A latched ``stop_verified_ok`` flag does not identify the endpoint sample.

Producer contract (``CompetitionStateMachine.h``, ``VehicleRuntime.cpp``,
read for interpretation only - no firmware change here): the verification
cycle stamps ``finalStopVerifiedAtMs = nowMs`` and the same tick's sample
carries ``t_ms = nowMs``. The flag and timestamp then latch into every later
``Done`` sample while the position keeps reporting the live run-axis
estimate. Only a usable selected-run sample whose raw ``t_ms`` equals its
own ``stop_verified_ms`` was observed *at verification*; a later ``Done``
sample - even one with a numerically identical position - is a later
observation, possibly after handling or retrieval.

These tests pin the fail-closed attribution rule through the real parser,
real metrics, real endpoints gate and the real report renderers, plus one
supported-CLI subprocess run of the original counterexample:

* the original M11 shape (verification 3100 ms, first Done 6000 ms at 8 m)
  yields no verified endpoint and no eligible delta;
* a tick-matched verification sample stays eligible;
* value equality, first/last picking, tolerances and interpolation never
  promote a late sample;
* wrap/reorder/restart and multi-run/session rules (R1D/R1F/R1E) are
  preserved; refused comparisons carry null deltas and precise reasons;
* JSON, text, CLI, manifest and comparison outputs agree.

Synthetic fixtures below validate software behavior only: no passing test
here is evidence about what any physical vehicle did (TEST AN).
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

from tests.support import REPO_ROOT

from evtelemetry import cli, schema
from evtelemetry.endpoints import Measurement, MeasurementTable, compare_endpoints
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import parse_text

UINT32_SPAN = 1 << 32

_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "dt_ms": "20",
    "phase": "Idle",
    "cmd": "none",
    "armed": "1",
    "run_start_ms": "500",
    "run_start_ok": "1",
    "dwell_start_ms": "2800",
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
    values = dict(_DEFAULTS)
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
    """A Done sample with the latched verification timeline."""
    kw = dict(
        phase="Done",
        pos_m=f"{pos_m:.4f}",
        stop_verified_ms=str(stamp_ms),
        stop_verified_ok="1",
    )
    kw.update(overrides)
    return _sample(t_ms, **kw)


def _parse(lines: list[str], **kwargs) -> object:
    return parse_text("\n".join(lines) + "\n", source="<test>", **kwargs)


def _metrics(lines: list[str], run_id: str = "M11", **kwargs) -> dict:
    return compute_metrics(_parse(lines, **kwargs), run_id)


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


def _m11_lines() -> list[str]:
    """TEST A: the audit counterexample. Verification at 3100 ms; the tick is
    missing; the first received Done sample is at 6000 ms at 8 m."""
    return [
        "# CAPTURE m11 original",
        _boot(0),
        _sample(500, phase="FinalBrake", pos_m="4.9000"),
        _sample(3000, phase="FinalBrake", pos_m="5.0000"),
        _done(6000, 8.0, 3100),
        _done(6100, 8.0, 3100),
    ]


def _valid_lines() -> list[str]:
    """TEST B/K: a tick-matched verification sample at 3100 ms, 5 m."""
    return [
        "# CAPTURE m11 valid",
        _boot(0),
        _sample(500, phase="FinalBrake", pos_m="4.9000"),
        _sample(3000, phase="FinalBrake", pos_m="5.0000"),
        _done(3100, 5.0, 3100),
        _done(6000, 5.5, 3100),
    ]


class OriginalFailureTest(unittest.TestCase):
    """TEST A/AB/AA/Z/AD: the historical misattribution is gone."""

    def test_late_done_is_not_the_verified_stop(self) -> None:
        m = _metrics(_m11_lines())
        pos = m["position"]
        self.assertIsNone(pos["end_position_m"])
        self.assertIsNone(pos["end_position_source"])
        self.assertIsNone(pos["verified_stop_m"])
        self.assertIsNone(pos["verified_stop_t_abs_ms"])
        # No fake zero, NaN or infinity (TEST AA).
        self.assertNotEqual(pos["end_position_m"], 0.0)
        # The later observation stays descriptively available (TEST Z).
        self.assertAlmostEqual(pos["last_m"], 8.0)
        self.assertIsNone(pos["after_stop_travel_m"])
        # Verification itself is still recorded as latched state.
        self.assertTrue(m["run_window"]["final_stop_dwell_verified"])
        self.assertEqual(m["run_window"]["stop_verified_ms"], 3100)

    def test_unavailable_reason_names_the_gap(self) -> None:
        reason = _metrics(_m11_lines())["position"][
            "end_position_unavailable_reason"
        ]
        self.assertIn("3100", reason)
        self.assertIn("6000", reason)
        # TEST AD: a temporal-attribution limit, not an encoder failure.
        self.assertNotIn("encoder", reason.lower())
        self.assertNotIn("nFAULT", reason)

    def test_missing_verified_position_refuses_comparison(self) -> None:
        row = compare_endpoints(
            [_metrics(_m11_lines(), "M11")], _table("M11", measured=8.0)
        )["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIsNone(row["believed_end_position_m"])
        self.assertIn("3100", row["comparison_refused_reason"])


class ValidVerificationTest(unittest.TestCase):
    """TEST B/K/AC: genuine verification-tick evidence stays eligible."""

    def test_tick_matched_sample_is_the_verified_stop(self) -> None:
        m = _metrics(_valid_lines())
        pos = m["position"]
        self.assertEqual(pos["end_position_source"], "verified_final_stop")
        self.assertAlmostEqual(pos["end_position_m"], 5.0)
        self.assertAlmostEqual(pos["verified_stop_m"], 5.0)
        self.assertEqual(pos["verified_stop_t_abs_ms"], 3100)
        self.assertAlmostEqual(pos["last_m"], 5.5)
        self.assertAlmostEqual(pos["after_stop_travel_m"], 0.5)

    def test_valid_evidence_permits_comparison(self) -> None:
        row = compare_endpoints(
            [_metrics(_valid_lines(), "M11")], _table("M11", measured=5.25)
        )["runs"][0]
        self.assertTrue(row["comparable"])
        self.assertAlmostEqual(row["end_position_delta_m"], 0.25)


class LateSampleShapeTest(unittest.TestCase):
    """TEST C/D/E/X: late-sample value, count and order change nothing."""

    def test_same_position_late_sample_is_still_unavailable(self) -> None:
        lines = [
            "# CAPTURE same value late",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _sample(3000, phase="FinalBrake", pos_m="5.0000"),
            _done(6000, 5.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIsNone(m["position"]["end_position_source"])
        self.assertAlmostEqual(m["position"]["last_m"], 5.0)

    def test_several_late_samples_never_become_the_stop(self) -> None:
        lines = [
            "# CAPTURE several lates",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _done(6000, 7.0, 3100),
            _done(6100, 7.5, 3100),
            _done(6200, 6.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIsNone(m["position"]["verified_stop_t_abs_ms"])

    def test_late_duplicates_do_not_multiply_verification(self) -> None:
        m = _metrics(_valid_lines())
        # One verification tick plus one later Done: exactly one verified
        # endpoint at the tick, with the later travel stated, not merged.
        self.assertAlmostEqual(m["position"]["end_position_m"], 5.0)
        self.assertAlmostEqual(m["position"]["after_stop_travel_m"], 0.5)

    def test_no_tolerance_around_the_verification_tick(self) -> None:
        """TEST AM: association is exact equality - a 1 ms neighbor with the
        stamp is still a later observation, not the verification sample."""
        lines = [
            "# CAPTURE off by one",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _done(3101, 5.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIsNone(m["position"]["end_position_source"])


class IncoherentMetadataTest(unittest.TestCase):
    """TEST F/G/H/W: incoherent or contradictory verification metadata."""

    def test_unusable_verification_tick_position_is_unavailable(self) -> None:
        lines = [
            "# CAPTURE unusable tick",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _done(3100, 5.0, 3100, pos_ok="0"),
            _done(6000, 5.5, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIn("no usable position", m["position"][
            "end_position_unavailable_reason"
        ])

    def test_flag_without_timeline_is_not_verified(self) -> None:
        """TEST G: stop flag set but the run timeline flags missing."""
        lines = [
            "# CAPTURE incoherent flags",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _sample(
                6000,
                phase="Done",
                pos_m="8.0000",
                run_start_ok="0",
                dwell_start_ok="0",
                stop_verified_ms="6000",
                stop_verified_ok="1",
            ),
        ]
        m = _metrics(lines)
        self.assertNotEqual(m["position"]["end_position_source"], "verified_final_stop")
        self.assertFalse(m["run_window"]["final_stop_dwell_verified"])

    def test_stamp_without_flag_is_not_verified(self) -> None:
        """TEST H: a nonzero stamp with the flag clear claims nothing."""
        lines = [
            "# CAPTURE stamp no flag",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _sample(
                6000, phase="Done", pos_m="8.0000",
                stop_verified_ms="3100", stop_verified_ok="0",
            ),
        ]
        m = _metrics(lines)
        self.assertNotEqual(m["position"]["end_position_source"], "verified_final_stop")

    def test_contradictory_stamps_are_unavailable(self) -> None:
        """TEST W: two latched timestamps - no single instant to attribute."""
        lines = [
            "# CAPTURE contradictory",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _done(6000, 8.0, 3100),
            _done(6100, 8.0, 3200),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIn("contradictory", m["position"][
            "end_position_unavailable_reason"
        ])


class RunOutcomeTest(unittest.TestCase):
    """TEST I/J: runs that never verified keep existing semantics."""

    def test_no_verification_keeps_latest_sample_semantics(self) -> None:
        lines = [
            "# CAPTURE no verification",
            _boot(0),
            _sample(0, pos_m="0.0000"),
            _sample(20, pos_m="1.0000"),
        ]
        m = _metrics(lines)
        self.assertEqual(m["position"]["end_position_source"], "latest_usable_sample")
        self.assertAlmostEqual(m["position"]["end_position_m"], 1.0)

    def test_aborted_run_fabricates_no_verified_endpoint(self) -> None:
        lines = [
            "# CAPTURE aborted",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _sample(
                6000, phase="Fault", pos_m="4.9500", fault="PhaseTimeout",
            ),
        ]
        m = _metrics(lines)
        self.assertNotEqual(m["position"]["end_position_source"], "verified_final_stop")
        row = compare_endpoints([m], _table("M11"))["runs"][0]
        self.assertFalse(row["comparable"])


class ClockAndSelectionTest(unittest.TestCase):
    """TEST L/M/N/O/P/Q/R/S/T/U/V/AK: R1D clock and R1F selection intact."""

    def test_verification_near_uint32_wrap(self) -> None:
        top = UINT32_SPAN - 10
        lines = [
            "# CAPTURE wrap verification",
            _boot(top - 60),
            _sample(top - 60, phase="FinalBrake", pos_m="4.9000"),
            _done(top, 5.0, top),
            _done(5, 5.5, top),
        ]
        m = _metrics(lines, "WRAP")
        self.assertEqual(m["capture"]["clock_wrap_count"], 1)
        pos = m["position"]
        self.assertEqual(pos["end_position_source"], "verified_final_stop")
        self.assertAlmostEqual(pos["end_position_m"], 5.0)
        # The endpoint is the pre-wrap tick, not the post-wrap late sample.
        self.assertNotEqual(pos["verified_stop_t_abs_ms"], 5)

    def test_benign_reorder_keeps_tick_attribution(self) -> None:
        lines = [
            "# CAPTURE reorder",
            _boot(900),
            _sample(1000, pos_m="0.0000"),
            _sample(999, pos_m="1.0000"),
            _sample(1020, pos_m="2.0000"),
            _done(1040, 2.5, 1040),
        ]
        m = _metrics(lines, "REORDER")
        self.assertEqual(m["capture"]["restart_suspected_count"], 0)
        self.assertEqual(m["position"]["end_position_source"], "verified_final_stop")

    def test_restart_keeps_position_but_refuses_comparison(self) -> None:
        """TEST N: tick-matched position survives; R1E still refuses."""
        lines = [
            "# CAPTURE restart with verification",
            _boot(0),
            _sample(0, pos_m="0.0000"),
            _sample(500000, pos_m="1.0000"),
            _sample(1200, pos_m="2.0000"),
            _done(1220, 2.5, 1220),
        ]
        m = _metrics(lines, "RESTART")
        self.assertEqual(m["position"]["end_position_source"], "verified_final_stop")
        row = compare_endpoints([m], _table("RESTART"))["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["end_position_delta_m"])
        self.assertIn("restart", row["comparison_refused_reason"])

    def test_verification_in_excluded_run_is_not_selected(self) -> None:
        """TEST P/V: run 1 verified at its own tick; the selected run 2 has a
        flag but no tick - the endpoint is unavailable, not run 1's."""
        lines = [
            "# CAPTURE two runs",
            _boot(0),
            _sample(0, pos_m="0.0000", run_start_ms="100", run_start_ok="1",
                    dwell_start_ms="0", dwell_start_ok="0"),
            _done(60, 9.0, 60, run_start_ms="100"),
            _sample(2000, pos_m="1.0000", run_start_ms="2000", run_start_ok="1",
                    dwell_start_ms="0", dwell_start_ok="0",
                    stop_verified_ms="0", stop_verified_ok="0",
                    phase="FinalBrake"),
            _done(6000, 8.0, 3100, run_start_ms="2000"),
        ]
        m = _metrics(lines)
        self.assertTrue(m["capture"]["selection"]["mixed_runs"])
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertIsNone(m["position"]["end_position_source"])

    def test_two_boots_do_not_share_verification(self) -> None:
        """TEST O: the selected session's tick-match cannot come from the
        earlier boot's raw tick with the same number."""
        lines = [
            "# CAPTURE two boots",
            _boot(0),
            _sample(3100, phase="FinalBrake", pos_m="1.0000"),
            _boot(5000),
            _sample(5100, phase="FinalBrake", pos_m="4.9000"),
            _done(6000, 8.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])

    def test_event_only_extra_boot_still_refuses(self) -> None:
        """TEST Q: R1E capture-integrity refusal preserved."""
        row = compare_endpoints(
            [_metrics([*_valid_lines(), _boot(99999)], "EVENTONLY")],
            _table("EVENTONLY"),
        )["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIn("event-only", row["comparison_refused_reason"])

    def test_permissive_parse_omission_still_refuses(self) -> None:
        """TEST R: R1E refusal preserved."""
        lines = list(_valid_lines())
        lines.insert(3, "EVS3,ver=3,t_ms=BOGUS,dt_ms=20,phase=Done")
        m = _metrics(lines, continue_on_error=True)
        row = compare_endpoints([m], _table("M11"))["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIn("skipped", row["comparison_refused_reason"])

    def test_sparse_log_invents_no_samples(self) -> None:
        """TEST S: a gap with no verification tick stays unavailable."""
        lines = [
            "# CAPTURE sparse",
            _boot(0),
            _sample(500, phase="FinalBrake", pos_m="4.9000"),
            _done(9000, 8.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])
        self.assertAlmostEqual(m["position"]["last_m"], 8.0)

    def test_partial_capture_starting_late_is_unavailable(self) -> None:
        """TEST T: capture begins at a latched Done - no endpoint evidence."""
        lines = [
            "# CAPTURE partial",
            _done(6000, 8.0, 3100),
            _done(6100, 8.0, 3100),
        ]
        m = _metrics(lines)
        self.assertIsNone(m["position"]["end_position_m"])


class ReportAgreementTest(unittest.TestCase):
    """TEST AE/AF/AG/AH/AI/AJ/AN: every report form tells one story."""

    def _analyze(self, out: str, lines: list[str], run_id: str,
                 extra: list[str] | None = None) -> dict:
        cap = os.path.join(out, f"{run_id}.csv")
        with open(cap, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
        meas = os.path.join(out, "meas.csv")
        with open(meas, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("run_id,measured_end_position_m,notes\n")
            handle.write(f"{run_id},8.0,tape measure\n")
        argv = ["analyze", cap, "--out", os.path.join(out, "report"),
                "--run-id", run_id, "--measurements", meas]
        if extra:
            argv.extend(extra)
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            code = cli.main(argv)
        self.assertEqual(code, 0, err.getvalue())
        with open(os.path.join(out, "report", "endpoints.json"),
                   encoding="utf-8") as handle:
            return json.load(handle)

    def test_json_text_cli_agree_on_unavailable(self) -> None:
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        endpoints = self._analyze(tmp, _m11_lines(), "M11")
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["believed_end_position_m"])
        with open(os.path.join(tmp, "report", "runs", "M11", "summary.txt"),
                    encoding="utf-8") as handle:
            summary = handle.read()
        self.assertIn("unavailable", summary)
        self.assertNotIn("verified_final_stop", summary)
        # TEST AN: reports describe believed firmware values, never physics.
        self.assertIn("believed", summary)

    def test_rerun_without_measurements_leaves_no_stale_comparison(self) -> None:
        """TEST AF/AJ: R1B reconciliation - a prior eligible comparison must
        not masquerade as current after a rerun that produces none."""
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        self._analyze(tmp, _valid_lines(), "M11")
        with open(os.path.join(tmp, "report", "endpoints.json"),
                    encoding="utf-8") as handle:
            first = json.load(handle)
        self.assertTrue(first["runs"][0]["comparable"])
        # Rerun the same tree with the M11 shape and no measurements: the old
        # endpoints output must be reconciled away, not left behind.
        cap = os.path.join(tmp, "M11.csv")
        with open(cap, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(_m11_lines()) + "\n")
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            code = cli.main(["analyze", cap, "--out", os.path.join(tmp, "report"),
                             "--run-id", "M11"])
        self.assertEqual(code, 0, err.getvalue())
        self.assertFalse(os.path.exists(os.path.join(tmp, "report", "endpoints.json")))
        with open(os.path.join(tmp, "report", "manifest.json"),
                    encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(len(manifest["inputs"]), 1)
        self.assertIsNone(manifest["inputs"][0]["end_position_source"])

    def test_prior_run_events_stay_excluded(self) -> None:
        """TEST AG (M10): R1F selected-run membership preserved."""
        lines = [
            "# CAPTURE prior event",
            _boot(0),
            _event(1100, event="CommandRejected", detail="NotArmed", phase="Idle"),
            _sample(1200, pos_m="0.0000", run_start_ms="500", run_start_ok="1",
                    phase="FinalBrake"),
            _sample(2000, pos_m="1.0000", run_start_ms="1800", run_start_ok="1",
                    phase="FinalBrake"),
            _done(2020, 1.5, 2020, run_start_ms="1800"),
        ]
        m = _metrics(lines)
        self.assertEqual(m["counts"]["events"], 0)
        self.assertEqual(m["position"]["end_position_source"], "verified_final_stop")

    def test_synthetic_provenance_stays_authoritative(self) -> None:
        """TEST AH (M13): an explicit SYNTHETIC declaration survives."""
        m = _metrics(["# SYNTHETIC generated fixture", *_m11_lines()[1:]])
        self.assertEqual(m["capture"]["provenance_declared"], "SYNTHETIC")

    def test_unknown_svg_survives_no_plots(self) -> None:
        """TEST AI (M14): tool-owned reconciliation preserves strangers."""
        from evtelemetry import report as report_module

        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        cap = os.path.join(tmp, "M11.csv")
        with open(cap, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(_valid_lines()) + "\n")
        out = os.path.join(tmp, "report")
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            code = cli.main(["analyze", cap, "--out", out, "--run-id", "M11"])
        self.assertEqual(code, 0, err.getvalue())
        stranger = os.path.join(out, "runs", "M11", "plots", "student-note.svg")
        with open(stranger, "w", encoding="utf-8") as handle:
            handle.write("<svg></svg>")
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            code = cli.main(["analyze", cap, "--out", out, "--run-id", "M11",
                             "--no-plots"])
        self.assertEqual(code, 0, err.getvalue())
        self.assertTrue(os.path.exists(stranger))
        self.assertIsNotNone(report_module)  # silence unused-import lint


class SupportedCliTest(unittest.TestCase):
    """TEST AL: the original M11 counterexample through the supported CLI."""

    def test_subprocess_reports_unavailable_not_8m(self) -> None:
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        cap = os.path.join(tmp, "m11.csv")
        with open(cap, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(_m11_lines()) + "\n")
        meas = os.path.join(tmp, "meas.csv")
        with open(meas, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("run_id,measured_end_position_m,notes\nM11,8.0,tape\n")
        out = os.path.join(tmp, "report")
        proc = subprocess.run(
            [sys.executable, os.path.join(REPO_ROOT, "tools", "telemetry",
                                          "analyze.py"),
             "analyze", cap, "--out", out, "--run-id", "M11",
             "--measurements", meas],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(os.path.join(out, "endpoints.json"),
                   encoding="utf-8") as handle:
            endpoints = json.load(handle)
        row = endpoints["runs"][0]
        self.assertFalse(row["comparable"])
        self.assertIsNone(row["believed_end_position_m"])
        self.assertIsNone(row["end_position_delta_m"])
        with open(os.path.join(out, "runs", "M11", "metrics.json"),
                   encoding="utf-8") as handle:
            metrics = json.load(handle)
        self.assertIsNone(metrics["position"]["end_position_m"])
        self.assertAlmostEqual(metrics["position"]["last_m"], 8.0)


if __name__ == "__main__":
    unittest.main()
