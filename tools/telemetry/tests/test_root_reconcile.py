"""Root-report reconciliation regressions (R1B / October 8 M15).

Contract under test (see ``evtelemetry.report._MANAGED_ROOT_REPORTS`` and
``reconcile_stale_run_dirs``): after a successful ``analyze`` generation,

* ``comparison.json/.txt`` exist only when the current generation produced
  them (two or more runs); otherwise the previous pair is removed;
* ``endpoints.json/.txt`` exist only when the current generation produced
  them (``--measurements`` was given); otherwise the previous pair is removed;
* ``runs/<id>/`` directories no current input maps to are reconciled to
  their unknown content (managed files removed) or removed when empty;
* every other file - unknown root files, ``distribution.*`` (owned by the
  ``distribution`` subcommand, never by ``analyze``), unknown subdirectories,
  symlinks, and user files inside run directories - survives byte-identical;
* no prior-manifest or caller value is ever turned into a deletion path, so
  a corrupt or hostile manifest cannot authorize outside deletion;
* a failure before generation (bad input, preflight collision) removes
  nothing, and a publication failure never yields a manifest that claims the
  failed generation.

Every test uses an isolated ``tempfile`` directory. No user, student,
project or competition directory is ever touched.
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

from tests.support import REPO_ROOT, sample_path

from evtelemetry import cli, report
from evtelemetry.report import ReportError


ANALYZE_SCRIPT = os.path.join(REPO_ROOT, "tools", "telemetry", "analyze.py")
SAMPLE_A = sample_path("synthetic_a.csv")
SAMPLE_B = sample_path("synthetic_b.csv")
SAMPLE_C = sample_path("synthetic_c.csv")

MEASUREMENTS_HEADER = (
    "run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,"
    "recharged,accepted,exclusion_reason,target_distance_m,tournament_level,notes\n"
)


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


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_bytes(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def manifest_inputs(out: str) -> list[str]:
    with open(os.path.join(out, "manifest.json"), encoding="utf-8") as handle:
        return [entry["run_id"] for entry in json.load(handle)["inputs"]]


def inventory(out: str) -> dict[str, str]:
    """Every file under ``out`` keyed by relative path, valued by SHA-256."""
    found: dict[str, str] = {}
    for root, _dirs, files in os.walk(out, followlinks=False):
        for name in files:
            full = os.path.join(root, name)
            if os.path.islink(full):
                continue
            found[os.path.relpath(full, out).replace(os.sep, "/")] = sha256_of(full)
    return found


class IsolatedCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="r1b-m15-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "out")

    def analyze(self, *extra: str) -> tuple[int, str, str]:
        return run_cli(["analyze", *extra, "--out", self.out])

    def write_measurements(self, name: str, rows: list[str]) -> str:
        path = os.path.join(self.tmp, name)
        write_text(path, MEASUREMENTS_HEADER + "".join(rows))
        return path


class TwoToOneCase(IsolatedCase):
    """TEST A - two inputs, then one: the stale pair must not pose as current."""

    def test_rerun_with_one_input_removes_stale_comparison_and_run_dir(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "comparison.json")))
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "BBB")))

        planted = {
            os.path.join(self.out, "notes.json"): b'{"mine": true}',
            os.path.join(self.out, "my-folder", "nested.txt"): b"nested user bytes",
            os.path.join(self.out, "runs", "field-notes.txt"): b"notes beside runs",
        }
        for path, data in planted.items():
            write_bytes(path, data)
        before = {path: sha256_of(path) for path in planted}

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)

        self.assertEqual(manifest_inputs(self.out), ["AAA"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "BBB")))
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "AAA")))
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)

    def test_two_to_one_via_real_entry_point_script(self) -> None:
        """Same case through ``python tools/telemetry/analyze.py`` as a process."""
        first = subprocess.run(
            [sys.executable, ANALYZE_SCRIPT, "analyze", SAMPLE_A, SAMPLE_B,
             "--out", self.out, "--run-id", "AAA", "--run-id", "BBB"],
            capture_output=True, text=True,
        )
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "comparison.json")))

        second = subprocess.run(
            [sys.executable, ANALYZE_SCRIPT, "analyze", SAMPLE_A,
             "--out", self.out, "--run-id", "AAA"],
            capture_output=True, text=True,
        )
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(manifest_inputs(self.out), ["AAA"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "BBB")))


class OneToTwoCase(IsolatedCase):
    """TEST B - one input, then two: the comparison is newly generated."""

    def test_regrowing_the_pair_writes_a_current_comparison(self) -> None:
        self.assertEqual(self.analyze(SAMPLE_A, "--run-id", "AAA")[0], 0)
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))

        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "comparison.json"), encoding="utf-8") as handle:
            pair = [row["run_id"] for row in json.load(handle)["runs"]]
        self.assertEqual(pair, ["AAA", "BBB"])
        self.assertEqual(manifest_inputs(self.out), ["AAA", "BBB"])
        text = read_bytes(os.path.join(self.out, "comparison.txt")).decode("utf-8")
        self.assertIn("AAA", text)
        self.assertIn("BBB", text)


class DifferentPairsCase(IsolatedCase):
    """TEST C - A+B, then C+D: no old result poses as the current pair."""

    def test_old_pair_results_do_not_survive_a_new_pair(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)

        code, _, err = self.analyze(SAMPLE_C, SAMPLE_A, "--run-id", "CCC", "--run-id", "DDD")
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "comparison.json"), encoding="utf-8") as handle:
            pair = [row["run_id"] for row in json.load(handle)["runs"]]
        self.assertEqual(pair, ["CCC", "DDD"])
        text = read_bytes(os.path.join(self.out, "comparison.txt")).decode("utf-8")
        self.assertNotIn("AAA", text)
        self.assertNotIn("BBB", text)
        self.assertEqual(manifest_inputs(self.out), ["CCC", "DDD"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "AAA")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "BBB")))
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "CCC")))
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "DDD")))


class MeasurementsRemovedCase(IsolatedCase):
    """TEST D - with measurements, then without: stale endpoints must go."""

    def test_rerun_without_measurements_removes_stale_endpoints(self) -> None:
        measurements = self.write_measurements(
            "meas.csv", ["AAA,2.470,21.9,11.9,yes,yes,,2.500,State,tape\n"]
        )
        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA", "--measurements", measurements)
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "endpoints.json")))

        user_file = os.path.join(self.out, "operator-note.txt")
        write_bytes(user_file, b"operator bytes")
        before = sha256_of(user_file)

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(os.path.join(self.out, "endpoints.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "endpoints.txt")))
        with open(os.path.join(self.out, "manifest.json"), encoding="utf-8") as handle:
            self.assertIsNone(json.load(handle)["measurements_source"])
        self.assertEqual(sha256_of(user_file), before)


class MeasurementsReplacedCase(IsolatedCase):
    """TEST E - measurements X, then Y: the endpoints must reflect Y."""

    def test_rerun_with_new_measurements_reflects_the_new_file(self) -> None:
        meas_x = self.write_measurements(
            "measX.csv", ["AAA,2.470,21.9,11.9,yes,yes,,2.500,State,tape\n"]
        )
        meas_y = self.write_measurements(
            "measY.csv", ["AAA,2.999,22.5,12.1,no,yes,,2.500,State,retapec\n"]
        )
        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA", "--measurements", meas_x)
        self.assertEqual(code, 0, err)

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA", "--measurements", meas_y)
        self.assertEqual(code, 0, err)
        with open(os.path.join(self.out, "endpoints.json"), encoding="utf-8") as handle:
            payload = json.load(handle)
        self.assertEqual(payload["source_sha256"], sha256_of(meas_y))
        self.assertEqual(payload["runs"][0]["measured_end_position_m"], 2.999)
        with open(os.path.join(self.out, "manifest.json"), encoding="utf-8") as handle:
            self.assertEqual(json.load(handle)["measurements_source"], meas_y)


class RepeatedGenerationCase(IsolatedCase):
    """TEST F - the same report twice: deterministic inventory, users kept."""

    def test_identical_rerun_is_stable(self) -> None:
        measurements = self.write_measurements(
            "meas.csv", ["AAA,2.470,21.9,11.9,yes,yes,,2.500,State,tape\n"]
        )
        argv = [SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB",
                "--measurements", measurements]
        code, _, err = self.analyze(*argv)
        self.assertEqual(code, 0, err)
        user_file = os.path.join(self.out, "runs", "AAA", "hand-sketch.svg")
        write_bytes(user_file, b"<svg>hand sketch</svg>")
        code, _, err = self.analyze(*argv)
        self.assertEqual(code, 0, err)
        first = inventory(self.out)

        code, _, err = self.analyze(*argv)
        self.assertEqual(code, 0, err)
        self.assertEqual(inventory(self.out), first)
        self.assertIn("runs/AAA/hand-sketch.svg", first)


class UnknownRootFilesCase(IsolatedCase):
    """TEST G - unrelated root files (incl. distribution output) survive."""

    def test_unknown_root_files_survive_a_shrinking_rerun(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        planted = {
            os.path.join(self.out, "custom.json"): b'{"operator": 1}',
            os.path.join(self.out, "notes.txt"): b"operator notes",
            os.path.join(self.out, "sketch.svg"): b"<svg>operator</svg>",
            os.path.join(self.out, "table.csv"): b"a,b\n1,2\n",
            os.path.join(self.out, "distribution.json"): b'{"series": true}',
            os.path.join(self.out, "distribution.txt"): b"series text",
        }
        for path, data in planted.items():
            write_bytes(path, data)
        before = {path: sha256_of(path) for path in planted}

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)


class UnknownSubdirsCase(IsolatedCase):
    """TEST H - user-created directories are never recursively deleted."""

    def test_unknown_directories_survive(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        planted = {
            os.path.join(self.out, "my-study", "notes.txt"): b"study notes",
            os.path.join(self.out, "my-study", "inner", "deep.txt"): b"deep bytes",
            os.path.join(self.out, "runs", "user-data", "capture.txt"): b"user capture",
            os.path.join(self.out, "runs", "user-data", "inner", "x.bin"): b"\x00\x01\x02",
        }
        for path, data in planted.items():
            write_bytes(path, data)
        before = {path: sha256_of(path) for path in planted}

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "user-data")))


class LegacyWithoutManifestCase(IsolatedCase):
    """TEST I - legacy-looking outputs without an ownership record."""

    def test_legacy_tree_gets_the_conservative_treatment(self) -> None:
        write_bytes(os.path.join(self.out, "comparison.json"), b'{"runs": ["OLD"]}')
        write_bytes(os.path.join(self.out, "comparison.txt"), b"OLD comparison")
        write_bytes(os.path.join(self.out, "endpoints.json"), b'{"runs": ["OLD"]}')
        write_bytes(os.path.join(self.out, "legacy-notes.txt"), b"legacy unknown")
        write_bytes(os.path.join(self.out, "runs", "OLD", "metrics.json"), b'{"old": true}')
        write_bytes(os.path.join(self.out, "runs", "OLD", "kept.txt"), b"user bytes in old dir")

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        # Managed slots reconcile even without a prior manifest (documented
        # managed-slot contract); unknown legacy files stay.
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "endpoints.json")))
        self.assertEqual(
            read_bytes(os.path.join(self.out, "legacy-notes.txt")), b"legacy unknown"
        )
        self.assertFalse(
            os.path.exists(os.path.join(self.out, "runs", "OLD", "metrics.json"))
        )
        self.assertEqual(
            read_bytes(os.path.join(self.out, "runs", "OLD", "kept.txt")),
            b"user bytes in old dir",
        )
        self.assertTrue(os.path.isdir(os.path.join(self.out, "runs", "OLD")))
        self.assertEqual(manifest_inputs(self.out), ["AAA"])


class CorruptedManifestCase(IsolatedCase):
    """TEST J - a malformed prior manifest grants no deletion and blocks nothing."""

    def test_garbage_manifest_is_replaced_and_user_files_kept(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        write_text(os.path.join(self.out, "manifest.json"), "not json{{{")
        user_file = os.path.join(self.out, "notes.txt")
        write_bytes(user_file, b"user bytes")
        before = sha256_of(user_file)

        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        self.assertEqual(manifest_inputs(self.out), ["AAA", "BBB"])
        self.assertEqual(sha256_of(user_file), before)


class PathTraversalCase(IsolatedCase):
    """TEST K - hostile prior-manifest paths authorize no outside deletion."""

    def test_evil_manifest_paths_delete_nothing_outside(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        victims = {
            os.path.join(self.tmp, "victim.json"): b"victim",
            os.path.join(self.tmp, "comparison.json"): b"outside same-name file",
            os.path.join(self.tmp, "other", "metrics.json"): b"outside metrics",
        }
        for path, data in victims.items():
            write_bytes(path, data)
        before = {path: sha256_of(path) for path in victims}
        write_text(
            os.path.join(self.out, "manifest.json"),
            json.dumps(
                {
                    "inputs": [
                        {"run_id": "../victim"},
                        {"run_id": "../../victim"},
                        {"run_id": "/abs"},
                        {"run_id": "C:\\evil"},
                        {"run_id": "..\\victim"},
                    ]
                }
            ),
        )

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)
        # The genuine stale managed outputs still reconciled.
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.json")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "runs", "BBB")))

    def test_reconcile_labels_never_leave_the_managed_tree(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        removed = report.reconcile_root_reports(
            self.out, keep_comparison=False, keep_endpoints=False
        )
        removed += report.reconcile_stale_run_dirs(
            self.out, [os.path.join(self.out, "runs", "AAA")]
        )
        for entry in removed:
            self.assertFalse(os.path.isabs(entry), entry)
            self.assertNotIn("..", entry, entry)


class SymlinkSafetyCase(unittest.TestCase):
    """TEST L - links are never followed for deletion. Skipped if unprivileged."""

    def test_symlinked_root_slot_and_run_dir_are_preserved(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1b-m15-link-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out = os.path.join(tmp, "out")
        code, _, err = run_cli(
            ["analyze", SAMPLE_A, SAMPLE_B, "--out", out, "--run-id", "AAA", "--run-id", "BBB"]
        )
        self.assertEqual(code, 0, err)
        try:
            target = os.path.join(tmp, "real-comparison.json")
            write_bytes(target, b'{"real": true}')
            os.remove(os.path.join(out, "comparison.json"))
            os.symlink(target, os.path.join(out, "comparison.json"))

            outside = os.path.join(tmp, "elsewhere")
            os.makedirs(outside)
            write_bytes(os.path.join(outside, "metrics.json"), b'{"outside": true}')
            shutil.rmtree(os.path.join(out, "runs", "BBB"))
            os.symlink(outside, os.path.join(out, "runs", "BBB"))
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        before_target = sha256_of(target)
        before_outside = sha256_of(os.path.join(outside, "metrics.json"))

        code, _, err = run_cli(["analyze", SAMPLE_A, "--out", out, "--run-id", "AAA"])
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.islink(os.path.join(out, "comparison.json")))
        self.assertEqual(sha256_of(target), before_target)
        self.assertTrue(os.path.islink(os.path.join(out, "runs", "BBB")))
        self.assertEqual(sha256_of(os.path.join(outside, "metrics.json")), before_outside)

    def test_symlinked_runs_dir_disables_stale_reconciliation(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1b-m15-link-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out = os.path.join(tmp, "out")
        os.makedirs(out)
        try:
            elsewhere = os.path.join(tmp, "elsewhere")
            os.makedirs(os.path.join(elsewhere, "STALE"))
            write_bytes(os.path.join(elsewhere, "STALE", "metrics.json"), b'{"stale": true}')
            os.symlink(elsewhere, os.path.join(out, "runs"))
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        removed = report.reconcile_stale_run_dirs(
            out, [os.path.join(out, "runs", "AAA")]
        )
        self.assertEqual(removed, [])
        self.assertTrue(
            os.path.isfile(os.path.join(elsewhere, "STALE", "metrics.json"))
        )


class FailureBeforeGenerationCase(IsolatedCase):
    """TEST M - invalid input / preflight collision removes nothing."""

    def _plant_stale_tree(self) -> dict[str, str]:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        measurements = self.write_measurements(
            "meas.csv", ["AAA,2.470,21.9,11.9,yes,yes,,2.500,State,tape\n"]
        )
        code, _, err = self.analyze(
            SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB",
            "--measurements", measurements,
        )
        self.assertEqual(code, 0, err)
        user_file = os.path.join(self.out, "notes.txt")
        write_bytes(user_file, b"user bytes")
        paths = [
            os.path.join(self.out, "comparison.json"),
            os.path.join(self.out, "comparison.txt"),
            os.path.join(self.out, "endpoints.json"),
            os.path.join(self.out, "endpoints.txt"),
            os.path.join(self.out, "manifest.json"),
            os.path.join(self.out, "runs", "BBB", "metrics.json"),
            user_file,
        ]
        return {path: sha256_of(path) for path in paths}

    def test_missing_log_touches_nothing(self) -> None:
        before = self._plant_stale_tree()
        code, _, _ = self.analyze(os.path.join(self.tmp, "no-such-log.csv"), "--run-id", "AAA")
        self.assertNotEqual(code, 0)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)

    def test_run_id_collision_touches_nothing(self) -> None:
        before = self._plant_stale_tree()
        code, _, _ = run_cli(
            ["analyze", SAMPLE_A, SAMPLE_A, "--out", self.out,
             "--run-id", "audit/A", "--run-id", "audit?A"]
        )
        self.assertNotEqual(code, 0)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)

    def test_invalid_measurements_touch_nothing(self) -> None:
        before = self._plant_stale_tree()
        bad = self.write_measurements("bad.csv", ["AAA,not-a-number,\n"])
        code, _, _ = self.analyze(SAMPLE_A, "--run-id", "AAA", "--measurements", bad)
        self.assertNotEqual(code, 0)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)


class FailureDuringPublicationCase(IsolatedCase):
    """TEST N - a mid-publication error never yields a falsely complete manifest.

    Publication is staged: everything renders in memory first, the previous
    manifest is invalidated, the new artifacts commit, and the new manifest
    commits last. A commit failure therefore leaves no manifest claiming
    completeness - only an explicit incomplete-publication marker - and the
    next successful invocation recovers deterministically.
    """

    def test_failed_second_run_writes_no_manifest_claiming_it(self) -> None:
        calls: list[str] = []
        real_write_json = cli.write_json

        def flaky(path: str, payload: object) -> None:
            calls.append(path)
            if len(calls) == 2:
                raise ReportError("injected publication failure")
            real_write_json(path, payload)  # type: ignore[arg-type]

        with mock.patch.object(cli, "write_json", side_effect=flaky):
            code, _, _ = self.analyze(
                SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB"
            )
        self.assertNotEqual(code, 0)
        # A fresh tree has no manifest at all: visibly incomplete, never
        # falsely complete.
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))
        with open(os.path.join(self.out, ".publication_state.json"), encoding="utf-8") as handle:
            state = json.load(handle)
        self.assertEqual(state["publication_state"], "incomplete")

    def test_failed_rerun_invalidates_the_manifest_and_recovers(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        self.assertEqual(manifest_inputs(self.out), ["AAA"])

        calls: list[str] = []
        real_write_json = cli.write_json

        def flaky(path: str, payload: object) -> None:
            calls.append(path)
            if len(calls) == 2:
                raise ReportError("injected publication failure")
            real_write_json(path, payload)  # type: ignore[arg-type]

        with mock.patch.object(cli, "write_json", side_effect=flaky):
            code, _, _ = self.analyze(
                SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB"
            )
        self.assertNotEqual(code, 0)
        # The previous manifest was invalidated before the commit, so no
        # manifest - old or new - can present the mixed tree as a complete
        # generation. The incomplete marker says so explicitly.
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))
        with open(os.path.join(self.out, ".publication_state.json"), encoding="utf-8") as handle:
            state = json.load(handle)
        self.assertEqual(state["publication_state"], "incomplete")
        # Recovery is deterministic: the next successful invocation
        # publishes the full generation and clears the marker.
        code, _, err = self.analyze(
            SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB"
        )
        self.assertEqual(code, 0, err)
        self.assertEqual(manifest_inputs(self.out), ["AAA", "BBB"])
        self.assertFalse(
            os.path.exists(os.path.join(self.out, ".publication_state.json"))
        )


class CaseHandlingCase(unittest.TestCase):
    """TEST O - Windows case folding, without fixtures that collide on Windows."""

    def test_folded_equal_stale_dir_is_preserved(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1b-m15-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out = os.path.join(tmp, "out")
        stale = os.path.join(out, "runs", "AAA")
        write_bytes(os.path.join(stale, "metrics.json"), b'{"old": true}')
        # One directory only: no fixture collides with itself on any platform.
        removed = report.reconcile_stale_run_dirs(
            out, [os.path.join(out, "runs", "aaa")]
        )
        self.assertEqual(removed, [])
        self.assertTrue(os.path.isfile(os.path.join(stale, "metrics.json")))

    def test_root_slot_case_variant_reconciles(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1b-m15-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out = os.path.join(tmp, "out")
        os.makedirs(out)
        write_bytes(os.path.join(out, "COMPARISON.JSON"), b"stale")
        removed = report.reconcile_root_reports(
            out, keep_comparison=False, keep_endpoints=True
        )
        self.assertIn("COMPARISON.JSON", removed)
        self.assertFalse(os.path.exists(os.path.join(out, "COMPARISON.JSON")))

    def test_kept_root_slot_case_variant_survives(self) -> None:
        tmp = tempfile.mkdtemp(prefix="r1b-m15-case-")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        out = os.path.join(tmp, "out")
        os.makedirs(out)
        write_bytes(os.path.join(out, "comparison.json"), b"current")
        removed = report.reconcile_root_reports(
            out, keep_comparison=True, keep_endpoints=False
        )
        self.assertNotIn("comparison.json", removed)
        self.assertEqual(read_bytes(os.path.join(out, "comparison.json")), b"current")


class R1APreservationCase(IsolatedCase):
    """TEST P - R1A's --no-plots ownership guarantee still holds."""

    def test_no_plots_keeps_unmanaged_svgs(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        student = os.path.join(self.out, "runs", "AAA", "plots", "student-note.svg")
        write_bytes(student, b"<svg>student</svg>")
        before = sha256_of(student)

        code, _, err = self.analyze(
            SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB", "--no-plots"
        )
        self.assertEqual(code, 0, err)
        self.assertEqual(sha256_of(student), before)
        # The current-generation root pair still reconciles alongside.
        self.assertTrue(os.path.isfile(os.path.join(self.out, "comparison.json")))


class DeletionTargetsConstrainedCase(IsolatedCase):
    """TEST Q - every deletion target stays inside the managed destination."""

    def test_outside_same_named_files_survive_a_shrinking_rerun(self) -> None:
        code, _, err = self.analyze(SAMPLE_A, SAMPLE_B, "--run-id", "AAA", "--run-id", "BBB")
        self.assertEqual(code, 0, err)
        victims = {
            os.path.join(self.tmp, "comparison.json"): b"outside",
            os.path.join(self.tmp, "comparison.txt"): b"outside",
            os.path.join(self.tmp, "endpoints.json"): b"outside",
            os.path.join(self.tmp, "runs", "BBB", "metrics.json"): b"outside",
        }
        for path, data in victims.items():
            write_bytes(path, data)
        before = {path: sha256_bytes(data) for path, data in victims.items()}

        code, _, err = self.analyze(SAMPLE_A, "--run-id", "AAA")
        self.assertEqual(code, 0, err)
        for path, digest in before.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(sha256_of(path), digest, path)


if __name__ == "__main__":
    unittest.main()
