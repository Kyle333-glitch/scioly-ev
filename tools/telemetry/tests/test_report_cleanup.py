"""Ownership-safe report cleanup regressions (R1A / October 8 M14).

Contract under test (see ``evtelemetry.report._MANAGED_PLOTS``): a rerun may
delete only the tool's own managed names - ``metrics.json``,
``summary.txt``, ``parse_errors.txt`` and the seven plot names
``run_plots()`` can write. Any other file - including any other ``.svg``
name - is never deleted. A ``.svg`` extension alone never proves ownership.

Every test uses an isolated ``tempfile`` directory. No user, student,
project or competition directory is ever touched.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from tests.support import REPO_ROOT, sample_path

from evtelemetry import cli, report
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import parse_lines


ANALYZE_SCRIPT = os.path.join(REPO_ROOT, "tools", "telemetry", "analyze.py")
SAMPLE_A = sample_path("synthetic_a.csv")

STUDENT_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg">'
    "<text>student hand annotation - DO NOT DELETE</text></svg>"
)

MANAGED_PLOTS = [
    "plots/empty.svg",
    "plots/position.svg",
    "plots/velocity.svg",
    "plots/effort.svg",
    "plots/control_dt.svg",
    "plots/raw_inputs.svg",
    "plots/phase_timeline.svg",
]
FULL_PLOT_SET = [name for name in MANAGED_PLOTS if name != "plots/empty.svg"]


def run_cli(argv: list[str]) -> tuple[int, str, str]:
    """Runs the analyzer CLI in-process; returns (exit, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cli.main(argv)
        except SystemExit as exit_:  # argparse usage errors
            code = int(exit_.code or 0)
    return code, out.getvalue(), err.getvalue()


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def write_bytes(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def plots_in(run_dir: str) -> list[str]:
    plots = os.path.join(run_dir, "plots")
    if not os.path.isdir(plots):
        return []
    return sorted(os.listdir(plots))


class IsolatedCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="r1a-m14-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "out")
        self.run_dir = os.path.join(self.out, "runs", "repro")

    def analyze(self, *extra: str) -> tuple[int, str, str]:
        return run_cli(
            ["analyze", SAMPLE_A, "--out", self.out, "--run-id", "repro", *extra]
        )


class OriginalDataLossCase(IsolatedCase):
    """TEST A - the audited data-loss case: --no-plots must keep the student's SVG."""

    def test_no_plots_preserves_student_svg_byte_identical(self) -> None:
        code, _, err = self.analyze()
        self.assertEqual(code, 0, err)
        for name in FULL_PLOT_SET:
            self.assertTrue(
                os.path.isfile(os.path.join(self.run_dir, name)), name
            )

        student = os.path.join(self.run_dir, "plots", "student-note.svg")
        notes = os.path.join(self.run_dir, "plots", "notes.txt")
        write_bytes(student, STUDENT_SVG.encode("utf-8"))
        write_bytes(notes, b"unrelated user text")
        before_hash = sha256_of(student)
        before_notes = read_bytes(notes)

        code, _, err = self.analyze("--no-plots")
        self.assertEqual(code, 0, err)

        # The student's file survives byte-identical; the unrelated text too.
        self.assertTrue(os.path.isfile(student))
        self.assertEqual(sha256_of(student), before_hash)
        self.assertEqual(read_bytes(student), STUDENT_SVG.encode("utf-8"))
        self.assertEqual(read_bytes(notes), before_notes)
        # The previous generation's managed plots are still reconciled away.
        for name in FULL_PLOT_SET:
            self.assertFalse(
                os.path.exists(os.path.join(self.run_dir, name)), name
            )

    def test_no_plots_via_real_entry_point_script(self) -> None:
        """Same case through ``python tools/telemetry/analyze.py`` as a process."""
        first = subprocess.run(
            [sys.executable, ANALYZE_SCRIPT, "analyze", SAMPLE_A,
             "--out", self.out, "--run-id", "repro"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(first.returncode, 0, first.stderr)
        student = os.path.join(self.run_dir, "plots", "student-note.svg")
        write_bytes(student, STUDENT_SVG.encode("utf-8"))
        before_hash = sha256_of(student)

        second = subprocess.run(
            [sys.executable, ANALYZE_SCRIPT, "analyze", SAMPLE_A,
             "--out", self.out, "--run-id", "repro", "--no-plots"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertTrue(os.path.isfile(student))
        self.assertEqual(sha256_of(student), before_hash)


class RegenerationCase(IsolatedCase):
    """TEST B - ordinary regeneration replaces managed outputs, keeps user files."""

    def test_regenerate_with_plots_restores_charts_beside_user_files(self) -> None:
        self.assertEqual(self.analyze()[0], 0)
        student = os.path.join(self.run_dir, "plots", "student-note.svg")
        write_bytes(student, STUDENT_SVG.encode("utf-8"))
        before_hash = sha256_of(student)

        code, _, err = self.analyze()
        self.assertEqual(code, 0, err)
        for name in FULL_PLOT_SET:
            path = os.path.join(self.run_dir, name)
            self.assertTrue(os.path.isfile(path), name)
            self.assertGreater(os.path.getsize(path), 0, name)
        self.assertEqual(sha256_of(student), before_hash)


class RepeatedNoPlotsCase(IsolatedCase):
    """TEST C - repeated --no-plots runs delete nothing unknown."""

    def test_two_no_plots_runs_are_stable(self) -> None:
        self.assertEqual(self.analyze()[0], 0)
        student = os.path.join(self.run_dir, "plots", "student-note.svg")
        write_bytes(student, STUDENT_SVG.encode("utf-8"))
        before_hash = sha256_of(student)

        self.assertEqual(self.analyze("--no-plots")[0], 0)
        after_first = sorted(plots_in(self.run_dir))
        code, _, err = self.analyze("--no-plots")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(plots_in(self.run_dir)), after_first)
        self.assertIn("student-note.svg", after_first)
        self.assertEqual(sha256_of(student), before_hash)


class MixedFileTypesCase(IsolatedCase):
    """TEST D - unrelated .svg/.txt/.json in the report tree are all preserved."""

    def test_unrelated_svg_txt_json_all_survive(self) -> None:
        self.assertEqual(self.analyze()[0], 0)
        planted = {
            os.path.join(self.run_dir, "plots", "student-note.svg"): b"<svg>mine</svg>",
            os.path.join(self.run_dir, "plots", "notes.txt"): b"user notes",
            os.path.join(self.run_dir, "plots", "data.json"): b'{"mine": true}',
            os.path.join(self.run_dir, "custom.json"): b'{"run": "notes"}',
        }
        for path, data in planted.items():
            write_bytes(path, data)

        code, _, err = self.analyze("--no-plots")
        self.assertEqual(code, 0, err)
        for path, data in planted.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(read_bytes(path), data, path)


class UnknownSvgNamesCase(IsolatedCase):
    """TEST E - no unknown SVG name is removed for having a .svg suffix."""

    def test_many_unknown_svgs_survive(self) -> None:
        self.assertEqual(self.analyze()[0], 0)
        # NOTE: names must be pairwise distinct under case folding, because
        # the Windows filesystem folds letter case (two names differing only
        # by case are one file there).
        names = [
            "student-note.svg",
            "my-chart.svg",
            "annotation.svg",
            "extra.svg",
            "hand-sketch.svg",
        ]
        for name in names:
            write_bytes(
                os.path.join(self.run_dir, "plots", name),
                f"<svg>{name}</svg>".encode("utf-8"),
            )

        code, _, err = self.analyze("--no-plots")
        self.assertEqual(code, 0, err)
        surviving = plots_in(self.run_dir)
        for name in names:
            self.assertIn(name, surviving)


class KnownStalePlotsCase(unittest.TestCase):
    """TEST F - genuinely analyzer-generated obsolete plots reconcile correctly."""

    def test_empty_log_replaces_six_charts_with_empty_svg(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-stale-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")

        with open(SAMPLE_A, encoding="utf-8") as handle:
            full_log = parse_lines(handle.read().splitlines(), source="<full>")
        full_metrics = compute_metrics(full_log, "stale")
        report.run_report(run_dir, full_metrics, full_log, include_plots=True)
        for name in FULL_PLOT_SET:
            self.assertTrue(os.path.isfile(os.path.join(run_dir, name)), name)

        empty_log = parse_lines([], source="<empty>")
        empty_metrics = compute_metrics(empty_log, "stale")
        report.run_report(run_dir, empty_metrics, empty_log, include_plots=True)

        self.assertTrue(os.path.isfile(os.path.join(run_dir, "plots/empty.svg")))
        for name in FULL_PLOT_SET:
            self.assertFalse(os.path.exists(os.path.join(run_dir, name)), name)

    def test_charts_replace_empty_svg(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-stale-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")

        empty_log = parse_lines([], source="<empty>")
        report.run_report(
            run_dir, compute_metrics(empty_log, "stale"), empty_log, include_plots=True
        )
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "plots/empty.svg")))

        with open(SAMPLE_A, encoding="utf-8") as handle:
            full_log = parse_lines(handle.read().splitlines(), source="<full>")
        report.run_report(
            run_dir, compute_metrics(full_log, "stale"), full_log, include_plots=True
        )
        self.assertFalse(os.path.exists(os.path.join(run_dir, "plots/empty.svg")))
        for name in FULL_PLOT_SET:
            self.assertTrue(os.path.isfile(os.path.join(run_dir, name)), name)


class EmptyDirectoryCase(unittest.TestCase):
    """TEST G - empty or missing directories never fail and never vanish."""

    def test_reconcile_missing_plots_dir(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-empty-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        os.makedirs(run_dir)
        self.assertEqual(report._reconcile(run_dir, ["metrics.json"]), [])
        self.assertTrue(os.path.isdir(run_dir))

    def test_reconcile_empty_plots_dir_removed_without_error(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-empty-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        os.makedirs(os.path.join(run_dir, "plots"))
        self.assertEqual(report._reconcile(run_dir, []), [])
        # An empty plots/ directory carries no information, so it is tidied;
        # the run directory itself always remains.
        self.assertTrue(os.path.isdir(run_dir))

    def test_reconcile_missing_run_dir(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-empty-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        missing = os.path.join(tmp, "no-such-run")
        self.assertEqual(report._reconcile(missing, []), [])


class LegacyAmbiguityCase(IsolatedCase):
    """TEST H - outputs without an ownership record are treated conservatively."""

    def test_legacy_unknown_svg_retained_with_note(self) -> None:
        # A stale file the tool cannot prove it owns is left behind; the
        # report does not claim a fully reconciled tree for it.
        self.assertEqual(self.analyze()[0], 0)
        legacy = os.path.join(self.run_dir, "plots", "legacy-hand-plot.svg")
        write_bytes(legacy, b"<svg>legacy unknown</svg>")
        removed = report._reconcile(self.run_dir, ["metrics.json", "summary.txt"])
        self.assertNotIn("plots/legacy-hand-plot.svg", removed)
        self.assertTrue(os.path.isfile(legacy))

    def test_managed_slot_from_prior_generation_reconciles(self) -> None:
        # The documented contract: the seven managed slots are tool-owned, so
        # a stale managed name from a previous generation is removed even
        # though no manifest records it.
        self.assertEqual(self.analyze("--no-plots")[0], 0)
        stale = os.path.join(self.run_dir, "plots", "position.svg")
        write_bytes(stale, b"<svg>stale managed slot</svg>")
        removed = report._reconcile(self.run_dir, ["metrics.json", "summary.txt"])
        self.assertIn("plots/position.svg", removed)
        self.assertFalse(os.path.exists(stale))


class ManifestSafetyCase(unittest.TestCase):
    """TEST I - hostile manifest/kept values cannot authorize outside deletion."""

    def test_hostile_kept_entries_delete_nothing_outside(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-manifest-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        plots = os.path.join(run_dir, "plots")
        os.makedirs(plots)
        victim_outside = os.path.join(tmp, "victim.svg")
        write_bytes(victim_outside, b"<svg>outside</svg>")
        user_svg = os.path.join(plots, "student-note.svg")
        write_bytes(user_svg, b"<svg>mine</svg>")

        removed = report._reconcile(
            run_dir,
            [
                "../victim.svg",
                "/abs.svg",
                "plots/../../victim.svg",
                "..\\victim.svg",
                "plots/student-note.svg",
                "metrics.json",
            ],
        )
        self.assertTrue(os.path.isfile(victim_outside))
        self.assertEqual(read_bytes(victim_outside), b"<svg>outside</svg>")
        self.assertTrue(os.path.isfile(user_svg))
        for entry in removed:
            self.assertNotIn("..", entry)
            self.assertFalse(os.path.isabs(entry))


class SymlinkSafetyCase(unittest.TestCase):
    """Each case attempts link creation and skips when the OS refuses."""
    """Symlinks are never tool-owned: links are left, never followed for delete."""

    def test_symlink_at_managed_name_is_preserved(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-link-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        plots = os.path.join(run_dir, "plots")
        os.makedirs(plots)
        try:
            target = os.path.join(tmp, "real.svg")
            write_bytes(target, b"<svg>real target</svg>")
            os.symlink(target, os.path.join(plots, "position.svg"))
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        removed = report._reconcile(run_dir, ["metrics.json"])
        self.assertEqual(removed, [])
        self.assertTrue(os.path.islink(os.path.join(plots, "position.svg")))
        self.assertEqual(read_bytes(target), b"<svg>real target</svg>")

    def test_symlinked_plots_dir_is_not_reconciled(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-link-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        os.makedirs(run_dir)
        try:
            elsewhere = os.path.join(tmp, "elsewhere")
            os.makedirs(elsewhere)
            write_bytes(os.path.join(elsewhere, "position.svg"), b"<svg>x</svg>")
            os.symlink(elsewhere, os.path.join(run_dir, "plots"))
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        removed = report._reconcile(run_dir, [])
        self.assertEqual(removed, [])
        self.assertTrue(os.path.isfile(os.path.join(elsewhere, "position.svg")))


class WindowsPathCase(unittest.TestCase):
    """TEST J - case folding and separators behave per the documented contract."""

    def test_case_variant_of_unknown_name_survives(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        plots = os.path.join(run_dir, "plots")
        os.makedirs(plots)
        upper = os.path.join(plots, "STUDENT-NOTE.SVG")
        write_bytes(upper, b"<svg>upper unknown</svg>")
        removed = report._reconcile(run_dir, ["metrics.json"])
        self.assertTrue(os.path.isfile(upper))
        self.assertNotIn("plots/STUDENT-NOTE.SVG", removed)

    def test_case_variant_of_managed_slot_reconciles(self) -> None:
        # Managed slots match case-insensitively: on a case-insensitive
        # filesystem POSITION.SVG *is* position.svg, so the folded name owns
        # the slot on every platform rather than only on Windows.
        tmp = tempfile.mkdtemp(prefix="r1a-m14-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        plots = os.path.join(run_dir, "plots")
        os.makedirs(plots)
        write_bytes(os.path.join(plots, "POSITION.SVG"), b"<svg>stale</svg>")
        removed = report._reconcile(run_dir, ["metrics.json"])
        self.assertIn("plots/POSITION.SVG", removed)
        self.assertFalse(os.path.exists(os.path.join(plots, "POSITION.SVG")))

    def test_backslash_kept_entry_is_not_a_path(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1a-m14-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        run_dir = os.path.join(tmp, "run")
        os.makedirs(os.path.join(run_dir, "plots"))
        # Must not raise and must not delete anything: kept entries are
        # membership tokens, never filesystem paths.
        self.assertEqual(
            report._reconcile(run_dir, ["plots\\position.svg", "metrics.json"]), []
        )


class UserContentBytesCase(IsolatedCase):
    """TEST K - every unrelated user byte survives a successful rerun."""

    def test_all_user_bytes_preserved(self) -> None:
        self.assertEqual(self.analyze()[0], 0)
        planted = {
            os.path.join(self.run_dir, "plots", "student-note.svg"): (
                STUDENT_SVG.encode("utf-8")
            ),
            os.path.join(self.run_dir, "plots", "sketch.SVG"): b"<svg>upper</svg>",
            os.path.join(self.run_dir, "plots", "notes.txt"): b"notes \x00 binary?",
            os.path.join(self.run_dir, "plots", "data.json"): b'{"a": [1, 2]}',
        }
        before = {}
        for path, data in planted.items():
            write_bytes(path, data)
            before[path] = sha256_of(path)
        self.assertEqual(self.analyze("--no-plots")[0], 0)
        for path, data in planted.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), before[path], path)
            self.assertEqual(read_bytes(path), data, path)


class CliErrorHandlingCase(IsolatedCase):
    """TEST L - an input/report error performs no cleanup of unrelated files."""

    def test_failed_run_touches_nothing(self) -> None:
        user_file = os.path.join(self.out, "runs", "repro", "plots", "mine.svg")
        write_bytes(user_file, b"<svg>mine</svg>")
        before = sha256_of(user_file)
        code, _, _ = run_cli(
            ["analyze", os.path.join(self.tmp, "no-such-log.csv"),
             "--out", self.out, "--run-id", "repro"]
        )
        self.assertNotEqual(code, 0)
        self.assertTrue(os.path.isfile(user_file))
        self.assertEqual(sha256_of(user_file), before)


if __name__ == "__main__":
    unittest.main()
