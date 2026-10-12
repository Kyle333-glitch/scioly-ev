"""M13 provenance tests: ordinary comments never decide provenance.

The October 8 audit (M13) found two defects on the supported analyzer CLI:

* the parser treated the first token of ANY ``#`` comment as a provenance
  declaration and kept only the first one, so ``# capture exported by
  student`` before ``# SYNTHETIC fixture`` produced ``declared=CAPTURE`` and
  ``manifest synthetic=false``;
* ``report._note_for`` labelled any declared non-synthetic provenance as
  synthetic, so JSON/text/SVG disagreed about the same source.

The accepted grammar (see ``parse.SYNTHETIC_HEADER_MARKER``): a ``#`` line
whose first token after the ``#`` - whitespace/comma-delimited,
case-insensitive - is exactly ``SYNTHETIC`` or ``CAPTURE``. Everything else
is an ordinary comment. ``SYNTHETIC`` anywhere wins over ``CAPTURE``
regardless of order; a missing declaration stays ``None``.
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

from tests.support import REPO_ROOT, sample_path, spec

from evtelemetry import cli, synthetic
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import parse_lines, parse_text, read_log
from evtelemetry.report import is_synthetic, run_report

ANALYZE_PY = os.path.join(REPO_ROOT, "tools", "telemetry", "analyze.py")

# A short but schema-real telemetry body: the generator's own lines with every
# ``#`` header stripped, so each test prepends exactly the headers it means.
BODY = [line for line in synthetic.generate_lines(spec("SYNTHETIC-A")) if not line.startswith("#")][:80]

SYNTHETIC_SVG_NOTE = "SYNTHETIC FIXTURE - generated data, not a measurement"


def parse_with(headers: list[str], source: str = "<test>"):
    return parse_lines([*headers, *BODY], source=source)


def metrics_with(headers: list[str], run_id: str = "NEUTRAL-1", source: str = "<test>"):
    return compute_metrics(parse_with(headers, source), run_id)


def run_cli(argv: list[str]) -> tuple[int, str, str]:
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


class DeclarationGrammarTest(unittest.TestCase):
    """TESTS A-M: the parser honors only the explicit declaration grammar."""

    def test_A_ordinary_comment_then_synthetic_is_synthetic(self) -> None:
        """The audit's original fixture: an export note before the declaration."""
        log = parse_with(["# capture exported by student", "# SYNTHETIC fixture"])
        # The first line IS an explicit CAPTURE declaration under the grammar,
        # so this is a conflict - resolved conservatively, never suppressed.
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertTrue(log.provenance_conflict)
        self.assertTrue(is_synthetic("NEUTRAL-1", "<test>", log.declared_provenance))

    def test_B_synthetic_then_ordinary_stays_synthetic(self) -> None:
        log = parse_with(["# SYNTHETIC fixture", "# exported for review"])
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertFalse(log.provenance_conflict)

    def test_C_ordinary_comment_only_declares_nothing(self) -> None:
        """No first token here is a declaration word: nothing is declared."""
        log = parse_with(
            ["# exported by student", "# bench session 3", "# run notes: wobble at start"]
        )
        self.assertIsNone(log.declared_provenance)
        self.assertFalse(log.provenance_conflict)
        self.assertFalse(is_synthetic("NEUTRAL-1", "<test>", log.declared_provenance))

    def test_D_explicit_capture_only_is_capture_without_a_synthetic_note(self) -> None:
        metrics = metrics_with(["# CAPTURE bench session"])
        self.assertEqual(metrics["capture"]["provenance_declared"], "CAPTURE")
        self.assertFalse(metrics["capture"]["provenance_conflict"])
        self.assertFalse(
            is_synthetic("NEUTRAL-1", "<test>", metrics["capture"]["provenance_declared"])
        )
        # The documented grammar is honored and no synthetic note is fabricated.
        self.assertEqual(
            report_provenance_line(metrics), "provenance declared by the capture: CAPTURE"
        )
        self.assertNotIn("SYNTHETIC", report_provenance_line(metrics))
        self.assertEqual(
            report_note(metrics), "run NEUTRAL-1 - believed values from the firmware"
        )

    def test_E_capture_then_synthetic_stays_synthetic(self) -> None:
        log = parse_with(["# CAPTURE bench session", "# SYNTHETIC fixture"])
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertTrue(log.provenance_conflict)
        self.assertTrue(is_synthetic("NEUTRAL-1", "<test>", log.declared_provenance))

    def test_F_synthetic_then_capture_stays_synthetic(self) -> None:
        """Order must not matter: the later CAPTURE cannot undo SYNTHETIC."""
        log = parse_with(["# SYNTHETIC fixture", "# CAPTURE bench session"])
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertTrue(log.provenance_conflict)
        metrics = compute_metrics(log, "NEUTRAL-1")
        self.assertIn("SYNTHETIC FIXTURE", report_provenance_line(metrics))
        self.assertIn("conflicting CAPTURE", report_provenance_line(metrics))

    def test_G_many_ordinary_comments_before_synthetic(self) -> None:
        log = parse_with(
            [
                "# lab notes",
                "# exported by student",
                "# bench session 3",
                "# run notes: wobble at start",
                "# SYNTHETIC fixture",
            ]
        )
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        self.assertFalse(log.provenance_conflict)

    def test_H_repeated_synthetic_declarations_are_stable(self) -> None:
        first = parse_with(["# SYNTHETIC a", "# SYNTHETIC b"])
        second = parse_with(["# SYNTHETIC a", "# SYNTHETIC b"])
        for log in (first, second):
            self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
            self.assertFalse(log.provenance_conflict)
        self.assertEqual(len(first.samples), len(second.samples))
        self.assertEqual(first.declared_provenance, second.declared_provenance)

    def test_I_no_declaration_is_a_truthful_none(self) -> None:
        log = parse_lines(list(BODY), source="<test>")
        self.assertIsNone(log.declared_provenance)
        self.assertFalse(log.provenance_conflict)
        metrics = compute_metrics(log, "NEUTRAL-1")
        self.assertIsNone(metrics["capture"]["provenance_declared"])
        self.assertIsNone(report_provenance_line(metrics))

    def test_J_filenames_never_override_a_header(self) -> None:
        # A neutral name with a SYNTHETIC header: synthetic by declaration.
        log = parse_with(["# SYNTHETIC fixture"])
        self.assertTrue(is_synthetic("BENCH-1", "bench-1.csv", log.declared_provenance))
        # A misleading name with a CAPTURE header: the name ADDS the warning
        # (conservative direction); the header declaration itself is intact.
        capture = parse_with(["# CAPTURE bench session"])
        self.assertEqual(capture.declared_provenance, "CAPTURE")
        self.assertTrue(
            is_synthetic("BENCH-1", "my_synthetic_notes.csv", capture.declared_provenance)
        )
        # Neutral names with CAPTURE or nothing declared: not synthetic.
        self.assertFalse(
            is_synthetic("BENCH-1", "bench-1.csv", capture.declared_provenance)
        )
        self.assertFalse(is_synthetic("BENCH-1", "bench-1.csv", None))

    def test_K_casing_and_whitespace_variants(self) -> None:
        cases = [
            (["#   synthetic   fixture"], "SYNTHETIC"),
            (["#Synthetic fixture"], "SYNTHETIC"),
            (["#\tCAPTURE\tbench"], "CAPTURE"),
            (["# capture, bench session"], "CAPTURE"),
            (["# SYNTHETIC"], "SYNTHETIC"),
            (["#SYNTHETIC fixture"], "SYNTHETIC"),
        ]
        for headers, expected in cases:
            with self.subTest(headers=headers):
                self.assertEqual(parse_with(headers).declared_provenance, expected)

    def test_L_prose_is_not_a_declaration(self) -> None:
        """The word 'synthetic' outside first-token position declares nothing."""
        for headers in (
            ["# I am not using synthetic test data today"],
            ["# today I reviewed synthetic data"],
            ["# the synthetic fixture was reviewed"],
            ["# notes mention synthetic data"],
        ):
            with self.subTest(headers=headers):
                log = parse_with([*headers, "# run notes: steady"])
                self.assertIsNone(log.declared_provenance)
                self.assertFalse(
                    is_synthetic("NEUTRAL-1", "<test>", log.declared_provenance)
                )

    def test_M_malformed_declarations_declare_nothing(self) -> None:
        """Near-misses are ordinary comments: no silent certainty either way."""
        for headers in (
            ["# SYNTHETICALLY generated"],
            ["# SYNTHETIC_FIXTURE"],
            ["# SYNTHETIC-TELEMETRY"],
            ["#"],
            ["#   "],
        ):
            with self.subTest(headers=headers):
                log = parse_with(headers)
                self.assertIsNone(log.declared_provenance)
                self.assertFalse(log.provenance_conflict)

    def test_N_file_and_text_parser_paths_agree(self) -> None:
        """TEST N: read_log (.csv and .log) and parse_text share the grammar."""
        headers = ["# capture exported by student", "# SYNTHETIC fixture"]
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for name in ("capture.csv", "capture.log"):
                path = os.path.join(tmp, name)
                with open(path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write("\n".join([*headers, *BODY]) + "\n")
                paths.append(path)
            from_file = [read_log(path) for path in paths]
        text = "\n".join([*headers, *BODY]) + "\n"
        from_text = parse_text(text, source="<text>")
        for log in (*from_file, from_text):
            self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
            self.assertTrue(log.provenance_conflict)


def report_provenance_line(metrics) -> str | None:
    from evtelemetry.report import _provenance_line

    return _provenance_line(metrics)


def report_note(metrics) -> str:
    from evtelemetry.report import _note_for

    return _note_for(metrics)


class OutputConsistencyTest(unittest.TestCase):
    """TESTS O/P: JSON, text, SVG, manifest and per-run report agree."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _write(self, headers: list[str], name: str) -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join([*headers, *BODY]) + "\n")
        return path

    def _analyze(self, path: str, run_id: str, out: str) -> tuple[dict, str, str]:
        code, _, err = run_cli(["analyze", path, "--out", out, "--run-id", run_id])
        self.assertEqual(code, 0, err)
        manifest = json.loads(read(os.path.join(out, "manifest.json")))
        summary = read(os.path.join(out, "runs", run_id, "summary.txt"))
        svg = read(os.path.join(out, "runs", run_id, "plots", "position.svg"))
        return manifest, summary, svg

    def test_O_synthetic_outputs_agree(self) -> None:
        """TEST O (synthetic side): manifest, JSON, text and SVG all say synthetic."""
        path = self._write(["# capture exported by student", "# SYNTHETIC fixture"], "bench-1.csv")
        out = os.path.join(self.tmp, "report")
        manifest, summary, svg = self._analyze(path, "NEUTRAL-1", out)
        entry = manifest["inputs"][0]
        self.assertEqual(entry["provenance_declared"], synthetic.SYNTHETIC_MARKER)
        self.assertTrue(entry["synthetic"])
        metrics = json.loads(read(os.path.join(out, "runs", "NEUTRAL-1", "metrics.json")))
        self.assertEqual(
            metrics["capture"]["provenance_declared"], entry["provenance_declared"]
        )
        self.assertTrue(metrics["capture"]["provenance_conflict"])
        self.assertIn("SYNTHETIC FIXTURE - declared in the capture header", summary)
        self.assertIn(SYNTHETIC_SVG_NOTE, svg)

    def test_O_capture_outputs_agree(self) -> None:
        """TEST O (capture side): nothing claims synthetic anywhere."""
        path = self._write(["# CAPTURE bench session"], "bench-2.csv")
        out = os.path.join(self.tmp, "report")
        manifest, summary, svg = self._analyze(path, "NEUTRAL-2", out)
        entry = manifest["inputs"][0]
        self.assertEqual(entry["provenance_declared"], "CAPTURE")
        self.assertFalse(entry["synthetic"])
        self.assertIn("provenance declared by the capture: CAPTURE", summary)
        self.assertNotIn("SYNTHETIC", summary)
        self.assertNotIn("SYNTHETIC", svg)
        self.assertIn("believed values from the firmware", svg)

    def test_O_undeclared_outputs_agree(self) -> None:
        """TEST O (missing provenance): honestly undeclared in every output."""
        path = self._write([], "bench-3.csv")
        out = os.path.join(self.tmp, "report")
        manifest, summary, svg = self._analyze(path, "NEUTRAL-3", out)
        entry = manifest["inputs"][0]
        self.assertIsNone(entry["provenance_declared"])
        self.assertFalse(entry["synthetic"])
        self.assertNotIn("provenance:", summary)
        self.assertNotIn("SYNTHETIC", summary)
        self.assertNotIn("SYNTHETIC", svg)

    def test_P_manifest_matches_the_per_run_report(self) -> None:
        """TEST P: the root manifest never contradicts the run it describes."""
        path = self._write(["# SYNTHETIC fixture", "# exported for review"], "bench-4.csv")
        out = os.path.join(self.tmp, "report")
        manifest, summary, _ = self._analyze(path, "NEUTRAL-4", out)
        entry = manifest["inputs"][0]
        metrics = json.loads(read(os.path.join(out, "runs", "NEUTRAL-4", "metrics.json")))
        self.assertEqual(
            entry["provenance_declared"], metrics["capture"]["provenance_declared"]
        )
        # The manifest flag and the human-readable summary say the same thing.
        self.assertEqual(entry["synthetic"], ("SYNTHETIC FIXTURE" in summary))


class CliProvenanceTest(unittest.TestCase):
    """CLI-level provenance: subprocess entry point, mixed inputs, reruns."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def _write(self, headers: list[str], name: str) -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join([*headers, *BODY]) + "\n")
        return path

    def test_A_supported_cli_subprocess_marks_the_audit_fixture_synthetic(self) -> None:
        """The audit's exact case through the real `analyze.py` entry point."""
        path = self._write(
            ["# capture exported by student", "# SYNTHETIC fixture"], "bench-1.csv"
        )
        completed = subprocess.run(
            [sys.executable, ANALYZE_PY, "analyze", path, "--out", self.out,
             "--run-id", "NEUTRAL-A"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        manifest = json.loads(read(os.path.join(self.out, "manifest.json")))
        entry = manifest["inputs"][0]
        self.assertEqual(entry["provenance_declared"], synthetic.SYNTHETIC_MARKER)
        self.assertTrue(entry["synthetic"])
        summary = read(os.path.join(self.out, "runs", "NEUTRAL-A", "summary.txt"))
        self.assertIn("SYNTHETIC FIXTURE - declared in the capture header", summary)
        svg = read(os.path.join(self.out, "runs", "NEUTRAL-A", "plots", "position.svg"))
        self.assertIn(SYNTHETIC_SVG_NOTE, svg)

    def test_Q_mixed_inputs_make_no_blanket_genuine_claim(self) -> None:
        """TEST Q: a synthetic and a capture input compared side by side."""
        synth = self._write(["# SYNTHETIC fixture"], "bench-s.csv")
        capture = self._write(["# CAPTURE bench session"], "bench-c.csv")
        code, _, err = run_cli(
            ["analyze", synth, capture, "--out", self.out,
             "--run-id", "MIX-S", "--run-id", "MIX-C"]
        )
        self.assertEqual(code, 0, err)
        manifest = json.loads(read(os.path.join(self.out, "manifest.json")))
        flags = {entry["run_id"]: entry["synthetic"] for entry in manifest["inputs"]}
        self.assertEqual(flags, {"MIX-S": True, "MIX-C": False})
        comparison_txt = read(os.path.join(self.out, "comparison.txt"))
        comparison_json = json.loads(read(os.path.join(self.out, "comparison.json")))
        _ = comparison_json  # per-run provenance lives in the manifest, not here
        for needle in ("genuine", "real measurement", "trusted capture", "verified capture"):
            self.assertNotIn(needle, comparison_txt.lower())
        # Each run's own summary still tells the truth individually.
        self.assertIn(
            "SYNTHETIC FIXTURE",
            read(os.path.join(self.out, "runs", "MIX-S", "summary.txt")),
        )
        mix_c = read(os.path.join(self.out, "runs", "MIX-C", "summary.txt"))
        self.assertNotIn("SYNTHETIC", mix_c)

    def test_R_rerun_provenance_does_not_leak_between_generations(self) -> None:
        """TEST R: a capture rerun into the synthetic run's directory forgets it."""
        synth = self._write(["# SYNTHETIC fixture"], "bench-s.csv")
        capture = self._write(["# CAPTURE bench session"], "bench-c.csv")
        code, _, err = run_cli(["analyze", synth, "--out", self.out, "--run-id", "SLOT"])
        self.assertEqual(code, 0, err)
        code, _, err = run_cli(["analyze", capture, "--out", self.out, "--run-id", "SLOT"])
        self.assertEqual(code, 0, err)
        manifest = json.loads(read(os.path.join(self.out, "manifest.json")))
        self.assertEqual(len(manifest["inputs"]), 1)
        entry = manifest["inputs"][0]
        self.assertEqual(entry["provenance_declared"], "CAPTURE")
        self.assertFalse(entry["synthetic"])
        summary = read(os.path.join(self.out, "runs", "SLOT", "summary.txt"))
        self.assertNotIn("SYNTHETIC", summary)
        svg = read(os.path.join(self.out, "runs", "SLOT", "plots", "position.svg"))
        self.assertNotIn("SYNTHETIC", svg)

    def test_U_invalid_input_publishes_no_provenance(self) -> None:
        """TEST U: a log the tool cannot read fails without a fake classification."""
        bad = os.path.join(self.tmp, "bad.csv")
        with open(bad, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("# SYNTHETIC fixture\nEVS3,ver=3,t_ms=garbage\n")
        code, _, _ = run_cli(["analyze", bad, "--out", self.out, "--run-id", "BAD"])
        self.assertEqual(code, 2)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "BAD", "metrics.json")))

    def test_V_shipped_fixtures_keep_their_declarations(self) -> None:
        """TEST V: every committed synthetic sample still declares SYNTHETIC."""
        for run_id, name in (
            ("SYNTHETIC-A", "synthetic_a.csv"),
            ("SYNTHETIC-B", "synthetic_b.csv"),
            ("SYNTHETIC-C", "synthetic_c.csv"),
        ):
            with self.subTest(run_id=run_id):
                log = read_log(sample_path(name))
                self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
                self.assertFalse(log.provenance_conflict)
        code, _, err = run_cli(
            ["analyze", sample_path("synthetic_a.csv"), "--out", self.out,
             "--run-id", "NEUTRAL-V"]
        )
        self.assertEqual(code, 0, err)
        entry = json.loads(read(os.path.join(self.out, "manifest.json")))["inputs"][0]
        self.assertEqual(entry["provenance_declared"], synthetic.SYNTHETIC_MARKER)
        self.assertTrue(entry["synthetic"])


class PriorRegressionGuardTest(unittest.TestCase):
    """TESTS S/T: the R1A/R1B repairs still hold beside provenance reruns."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")

    def _write(self, headers: list[str], name: str) -> str:
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join([*headers, *BODY]) + "\n")
        return path

    def test_S_unknown_svg_survives_a_provenance_rerun_byte_identical(self) -> None:
        """TEST S (R1A): a user SVG beside the plots is never tool-owned."""
        path = self._write(["# CAPTURE bench session"], "bench-c.csv")
        code, _, err = run_cli(["analyze", path, "--out", self.out, "--run-id", "SLOT"])
        self.assertEqual(code, 0, err)
        plots = os.path.join(self.out, "runs", "SLOT", "plots")
        student = os.path.join(plots, "student-note.svg")
        with open(student, "wb") as handle:
            handle.write(b"<svg>student</svg>")
        with open(student, "rb") as handle:
            before = handle.read()
        code, _, err = run_cli(
            ["analyze", path, "--out", self.out, "--run-id", "SLOT", "--no-plots"]
        )
        self.assertEqual(code, 0, err)
        with open(student, "rb") as handle:
            self.assertEqual(handle.read(), before)
        self.assertFalse(os.path.exists(os.path.join(plots, "position.svg")))

    def test_T_dropped_root_reports_do_not_survive(self) -> None:
        """TEST T (R1B): a two-run comparison is gone after a one-run rerun."""
        first = self._write(["# SYNTHETIC fixture"], "bench-s.csv")
        second = self._write(["# CAPTURE bench session"], "bench-c.csv")
        code, _, err = run_cli(
            ["analyze", first, second, "--out", self.out,
             "--run-id", "AAA", "--run-id", "BBB"]
        )
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "comparison.json")))
        code, _, err = run_cli(["analyze", first, "--out", self.out, "--run-id", "AAA"])
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))
        manifest = json.loads(read(os.path.join(self.out, "manifest.json")))
        self.assertEqual([entry["run_id"] for entry in manifest["inputs"]], ["AAA"])


if __name__ == "__main__":
    unittest.main()
