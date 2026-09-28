"""End-to-end tests for the acceptance gate.

What has to be true for this tooling to be trusted:

* it runs on the synthetic samples with nothing installed but Python;
* incompatible or corrupt input fails *clearly* - a non-zero exit and a message
  that names the file, the line and what was expected;
* the same inputs produce byte-identical outputs;
* synthetic data cannot reach ``VERIFIED_STATE.md``, and a run does not modify
  it.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import tempfile
import unittest

from tests.support import REPO_ROOT, metrics_for, sample_path, spec

from evtelemetry import cli, report, synthetic


def run_cli(argv: list[str]) -> tuple[int, str, str]:
    """Runs the CLI in-process and returns ``(exit_code, stdout, stderr)``."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cli.main(argv)
        except SystemExit as exit_:  # argparse's usage errors
            code = int(exit_.code or 0)
    return code, out.getvalue(), err.getvalue()


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def tree(root: str) -> dict[str, str]:
    """Every file under ``root``, keyed by relative path, with its text."""
    files: dict[str, str] = {}
    for directory, _, names in os.walk(root):
        for name in names:
            path = os.path.join(directory, name)
            files[os.path.relpath(path, root).replace("\\", "/")] = read(path)
    return files


def sha256(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


class SyntheticFixtureTest(unittest.TestCase):
    def test_synth_writes_labelled_fixtures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, out, _ = run_cli(["synth", "--out", tmp])
            self.assertEqual(code, 0)
            self.assertIn("SYNTHETIC", out)
            self.assertIn("not measured data", out)
            names = sorted(tree(tmp))
            self.assertEqual(
                names,
                ["synthetic_a.csv", "synthetic_b.csv", "synthetic_c.csv"],
            )
            for name in names:
                self.assertIn("NOT MEASURED DATA", read(os.path.join(tmp, name)))

    def test_synth_is_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            run_cli(["synth", "--out", first])
            run_cli(["synth", "--out", second])
            self.assertEqual(tree(first), tree(second))


class AnalyzeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def test_analyze_runs_on_the_synthetic_samples(self) -> None:
        code, out, err = run_cli(
            [
                "analyze",
                sample_path("synthetic_a.csv"),
                sample_path("synthetic_b.csv"),
                sample_path("synthetic_c.csv"),
                "--out",
                self.out,
            ]
        )
        self.assertEqual(code, 0, err)
        self.assertIn("SYNTHETIC FIXTURE", out)

        files = tree(self.out)
        # A report per run, plus the comparison, plus the manifest.
        for run in ("synthetic_a", "synthetic_b", "synthetic_c"):
            self.assertIn(f"runs/{run}/metrics.json", files)
            self.assertIn(f"runs/{run}/summary.txt", files)
            for plot in (
                "position",
                "velocity",
                "effort",
                "control_dt",
                "raw_inputs",
                "phase_timeline",
            ):
                self.assertIn(f"runs/{run}/plots/{plot}.svg", files)
        self.assertIn("comparison.json", files)
        self.assertIn("comparison.txt", files)
        self.assertIn("manifest.json", files)

        # Every SVG is well-formed XML, and every metrics file is JSON.
        import xml.etree.ElementTree as element_tree

        for name, text in files.items():
            if name.endswith(".svg"):
                with self.subTest(svg=name):
                    element_tree.fromstring(text)
            if name.endswith(".json"):
                with self.subTest(json=name):
                    json.loads(text)

    def test_the_manifest_ties_the_report_to_the_raw_bytes(self) -> None:
        run_cli(["analyze", sample_path("synthetic_a.csv"), "--out", self.out])
        manifest = json.loads(read(os.path.join(self.out, "manifest.json")))
        entry = manifest["inputs"][0]
        self.assertEqual(entry["sha256"], sha256(sample_path("synthetic_a.csv")))
        self.assertEqual(entry["samples"], metrics_for(spec("SYNTHETIC-A"))["counts"]["samples"])
        self.assertEqual(manifest["schema_version"], 1)
        # No timestamp anywhere: a report must be reproducible byte for byte.
        self.assertNotIn("generated_at", manifest)

    def test_the_report_is_reproducible(self) -> None:
        second = os.path.join(self.tmp, "report2")
        for out in (self.out, second):
            code, _, err = run_cli(["analyze", sample_path("synthetic_a.csv"), "--out", out])
            self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.out), tree(second))

    def test_plots_can_be_skipped(self) -> None:
        code, _, _ = run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out, "--no-plots"]
        )
        self.assertEqual(code, 0)
        self.assertNotIn("runs/synthetic_a/plots/position.svg", tree(self.out))

    def test_run_ids_can_be_given_positionally(self) -> None:
        code, _, _ = run_cli(
            [
                "analyze",
                sample_path("synthetic_a.csv"),
                "--out",
                self.out,
                "--run-id",
                "BENCH-1",
            ]
        )
        self.assertEqual(code, 0)
        self.assertIn("runs/BENCH-1/metrics.json", tree(self.out))

    def test_the_summary_says_what_a_log_is_not(self) -> None:
        run_cli(["analyze", sample_path("synthetic_a.csv"), "--out", self.out])
        summary = read(os.path.join(self.out, "runs", "synthetic_a", "summary.txt"))
        self.assertIn("not evidence about what the vehicle did", summary)
        self.assertIn("not stopping distances", summary)
        self.assertIn("no conversion documented", summary)


class CorruptInputTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def _write(self, lines: list[str], name: str = "capture.csv") -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
        return path

    def _good_lines(self) -> list[str]:
        return [line for line in synthetic.generate_lines(spec("SYNTHETIC-A"))]

    def test_a_future_schema_version_is_refused_with_a_clear_message(self) -> None:
        lines = [
            line.replace("EVS1", "EVS2", 1) if line.startswith("EVS1") else line
            for line in self._good_lines()
        ]
        path = self._write(lines)
        code, _, err = run_cli(["analyze", path, "--out", self.out])
        self.assertEqual(code, 2)
        self.assertIn("schema version 2", err)
        self.assertIn("capture.csv", err)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))

    def test_a_truncated_record_is_refused(self) -> None:
        lines = self._good_lines()
        index = next(i for i, line in enumerate(lines) if line.startswith("EVS1"))
        lines[index] = ",".join(lines[index].split(",")[:10])
        path = self._write(lines)
        code, _, err = run_cli(["analyze", path, "--out", self.out])
        self.assertEqual(code, 2)
        self.assertIn("field set is not the schema's", err)
        self.assertIn("missing", err)

    def test_a_renamed_column_is_refused_rather_than_misread(self) -> None:
        lines = [
            line.replace("vel_cmd_mps", "vel_cmd") if line.startswith("EVS1") else line
            for line in self._good_lines()
        ]
        path = self._write(lines)
        code, _, err = run_cli(["analyze", path, "--out", self.out])
        self.assertEqual(code, 2)
        self.assertIn("vel_cmd_mps", err)  # the expected field set is printed

    def test_a_missing_file_is_reported_without_a_stack_trace(self) -> None:
        code, _, err = run_cli(
            ["analyze", os.path.join(self.tmp, "nope.csv"), "--out", self.out]
        )
        self.assertEqual(code, 2)
        self.assertIn("no such log file", err)

    def test_continue_on_error_surveys_a_damaged_capture_and_says_so(self) -> None:
        lines = self._good_lines()
        index = next(i for i, line in enumerate(lines) if line.startswith("EVS1"))
        lines.insert(index, "EVS1,ver=1,t_ms=garbage")
        path = self._write(lines)
        code, _, _ = run_cli(["analyze", path, "--out", self.out, "--continue-on-error"])
        self.assertEqual(code, 0)

        metrics = json.loads(read(os.path.join(self.out, "runs", "capture", "metrics.json")))
        self.assertEqual(metrics["counts"]["parse_errors"], 1)
        errors = read(os.path.join(self.out, "runs", "capture", "parse_errors.txt"))
        self.assertIn("t_ms", errors)
        self.assertIn("partial log", errors)

    def test_an_empty_file_is_reported_as_empty_not_as_success(self) -> None:
        path = self._write(["# nothing here"])
        code, out, err = run_cli(["analyze", path, "--out", self.out])
        self.assertEqual(code, 0, err)
        self.assertIn("samples=0", out)


class EndpointComparisonTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def _measurements(self, text: str) -> str:
        path = os.path.join(self.tmp, "measured.csv")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def test_measurements_are_compared_and_never_concluded_from(self) -> None:
        path = self._measurements(
            "# taken by hand\n"
            "run_id,measured_end_position_m,measured_run_time_s,notes\n"
            "synthetic_a,2.500,23.0,tape measure\n"
        )
        code, _, err = run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out, "--measurements", path]
        )
        self.assertEqual(code, 0, err)
        comparison = json.loads(read(os.path.join(self.out, "endpoints.json")))
        row = comparison["runs"][0]
        self.assertAlmostEqual(row["measured_end_position_m"], 2.5)
        self.assertIsNotNone(row["believed_end_position_m"])
        self.assertAlmostEqual(
            row["end_position_delta_m"], 2.5 - row["believed_end_position_m"]
        )
        self.assertIn("not by itself a conclusion", comparison["note"])

        text = read(os.path.join(self.out, "endpoints.txt"))
        self.assertIn("descriptive only", text)
        self.assertIn("VERIFIED_STATE.md", text)

    def test_a_run_without_a_measurement_is_reported_as_such(self) -> None:
        path = self._measurements("run_id,measured_end_position_m\nOTHER,1.0\n")
        run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out, "--measurements", path]
        )
        comparison = json.loads(read(os.path.join(self.out, "endpoints.json")))
        self.assertEqual(comparison["runs_without_a_measurement"], ["synthetic_a"])
        self.assertEqual(comparison["measured_rows_without_a_run"], ["OTHER"])
        # A missing measurement is `None`, never a zero that reads as "exact".
        self.assertIsNone(comparison["runs"][0]["end_position_delta_m"])

    def test_a_mistyped_measurement_column_is_refused(self) -> None:
        path = self._measurements("run_id,measured_travel_m\nsynthetic_a,2.5\n")
        code, _, err = run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out, "--measurements", path]
        )
        self.assertEqual(code, 2)
        self.assertIn("unknown column", err)
        self.assertIn("measured_end_position_m", err)

    def test_a_non_numeric_measurement_is_refused(self) -> None:
        path = self._measurements("run_id,measured_end_position_m\nsynthetic_a,about two metres\n")
        code, _, err = run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out, "--measurements", path]
        )
        self.assertEqual(code, 2)
        self.assertIn("not a number", err)

    def test_the_template_is_available_without_a_log(self) -> None:
        code, out, _ = run_cli(["analyze", "unused.csv", "--print-measurement-template"])
        self.assertEqual(code, 0)
        self.assertIn("measured_end_position_m", out)


class EvidenceGuardTest(unittest.TestCase):
    """Synthetic data and generated reports must not reach the authority files."""

    def test_writing_into_an_authority_file_is_refused(self) -> None:
        for name in report.PROTECTED_OUTPUT_NAMES:
            with self.subTest(name=name):
                with self.assertRaises(report.ReportError):
                    report.write_json(name, {"anything": 1})
                with self.assertRaises(report.ReportError):
                    report.write_json(os.path.join("some", "dir", name), {"anything": 1})

    def test_analysing_synthetic_data_does_not_touch_verified_state(self) -> None:
        verified = os.path.join(REPO_ROOT, "VERIFIED_STATE.md")
        before = sha256(verified)
        with tempfile.TemporaryDirectory() as tmp:
            for argv in (
                ["synth", "--out", os.path.join(tmp, "samples")],
                [
                    "analyze",
                    sample_path("synthetic_a.csv"),
                    sample_path("synthetic_b.csv"),
                    "--out",
                    os.path.join(tmp, "report"),
                ],
            ):
                code, _, err = run_cli(argv)
                self.assertEqual(code, 0, err)
        self.assertEqual(sha256(verified), before)

    def test_the_out_directory_cannot_be_an_authority_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, _, err = run_cli(
                [
                    "analyze",
                    sample_path("synthetic_a.csv"),
                    "--out",
                    os.path.join(tmp, "VERIFIED_STATE.md"),
                ]
            )
            self.assertEqual(code, 2)
            self.assertIn("refusing to write", err)


class UsageTest(unittest.TestCase):
    def test_no_subcommand_is_a_usage_error(self) -> None:
        code, _, err = run_cli([])
        self.assertEqual(code, 1)
        self.assertIn("usage", err.lower())

    def test_help_lists_both_subcommands(self) -> None:
        code, out, _ = run_cli(["--help"])
        self.assertEqual(code, 0)
        self.assertIn("synth", out)
        self.assertIn("analyze", out)

    def test_version_is_reported(self) -> None:
        code, out, _ = run_cli(["--version"])
        self.assertEqual(code, 0)
        self.assertIn("telemetry tools", out)


if __name__ == "__main__":
    unittest.main()
