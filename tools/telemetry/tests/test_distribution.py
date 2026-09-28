"""Tests for the measurement schema and the series distribution.

Two things are pinned here that the endpoint tests did not cover:

* the measurement CSV carries the *context* a repeatability record needs -
  whether a run was accepted, why it was excluded, the pack state and the target -
  and a mistyped value in any of those is refused rather than read as a number;
* the distribution arithmetic is done over the accepted runs only, an excluded
  run is kept and named rather than dropped, and the systematic offset and the
  random spread are separated the way ``AGENTS.md`` section 8 asks for.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import tempfile
import unittest

from tests.support import sample_path

from evtelemetry import cli
from evtelemetry.distribution import describe_values, measurement_distribution
from evtelemetry.endpoints import (
    MeasurementError,
    compare_endpoints,
    load_measurements,
)
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import read_log


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


class DescribeValuesTest(unittest.TestCase):
    def test_an_empty_series_reports_no_numbers_not_zero(self) -> None:
        stats = describe_values([])
        self.assertEqual(stats["count"], 0)
        self.assertIsNone(stats["mean"])
        self.assertIsNone(stats["stdev"])

    def test_one_observation_has_no_sample_stdev(self) -> None:
        stats = describe_values([4.0])
        self.assertEqual(stats["count"], 1)
        self.assertIsNone(stats["stdev"])
        self.assertEqual(stats["range"], 0.0)

    def test_the_statistics_are_the_expected_arithmetic(self) -> None:
        stats = describe_values([1.0, 2.0, 3.0])
        self.assertEqual(stats["mean"], 2.0)
        self.assertEqual(stats["median"], 2.0)
        self.assertEqual(stats["stdev"], 1.0)  # sample stdev of 1,2,3
        self.assertEqual(stats["range"], 2.0)
        self.assertEqual(stats["max_abs_deviation_from_mean"], 1.0)


class MeasurementFileTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _write(self, text: str) -> str:
        path = os.path.join(self.tmp, "measured.csv")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def test_the_context_columns_are_read(self) -> None:
        path = self._write(
            "run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,"
            "recharged,accepted,exclusion_reason,target_distance_m,tournament_level,notes\n"
            "R1,6.431,13.31,11.9,yes,yes,,6.500,State,tape\n"
        )
        table = load_measurements(path)
        row = table.get("R1")
        assert row is not None
        self.assertEqual(row.pack_voltage_v, 11.9)
        self.assertIs(row.recharged, True)
        self.assertIs(row.accepted, True)
        self.assertEqual(row.exclusion_reason, "")
        self.assertEqual(row.target_distance_m, 6.5)
        self.assertEqual(row.tournament_level, "State")

    def test_a_blank_context_cell_is_not_recorded_rather_than_zero(self) -> None:
        path = self._write("run_id,measured_end_position_m,accepted\nR1,6.0,\n")
        row = load_measurements(path).get("R1")
        assert row is not None
        self.assertIsNone(row.accepted)
        self.assertIsNone(row.pack_voltage_v)

    def test_a_bad_yes_no_value_is_refused(self) -> None:
        path = self._write("run_id,measured_end_position_m,accepted\nR1,6.0,maybe\n")
        with self.assertRaises(MeasurementError) as caught:
            load_measurements(path)
        self.assertIn("yes/no value", str(caught.exception))

    def test_a_mistyped_context_column_is_refused_by_name(self) -> None:
        path = self._write("run_id,measured_end_position_m,pack_volts\nR1,6.0,11.9\n")
        with self.assertRaises(MeasurementError) as caught:
            load_measurements(path)
        self.assertIn("pack_volts", str(caught.exception))

    def test_the_template_carries_the_context_columns(self) -> None:
        for column in ("accepted", "exclusion_reason", "pack_voltage_v", "target_distance_m"):
            self.assertIn(column, cli.MEASUREMENT_TEMPLATE)


class DistributionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.path = os.path.join(self.tmp, "measured.csv")
        with open(self.path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,"
                "recharged,accepted,exclusion_reason,target_distance_m,tournament_level\n"
                "R1,6.431,13.31,11.9,yes,yes,,6.500,State\n"
                "R2,6.118,13.42,11.7,no,no,pack sagged mid-run,6.500,State\n"
                "R3,6.520,13.28,,yes,yes,,6.500,State\n"
                "R4,6.000,13.50,,,,,6.500,\n"
                "R5,5.900,13.60,,,no,,6.500,State\n"
            )
        self.payload = measurement_distribution(load_measurements(self.path))

    def test_classification_keeps_every_run(self) -> None:
        classification = self.payload["classification"]
        self.assertEqual(classification["total"], 5)
        self.assertEqual(classification["accepted"], 2)
        self.assertEqual(classification["excluded"], 2)
        self.assertEqual(classification["unjudged"], 1)
        self.assertEqual(classification["unjudged_runs"], ["R4"])
        self.assertIn("R5", classification["excluded_without_a_recorded_reason"])

    def test_an_excluded_run_is_named_with_its_reason(self) -> None:
        excluded = self.payload["classification"]["excluded_runs"]
        self.assertIn({"run_id": "R2", "exclusion_reason": "pack sagged mid-run"}, excluded)

    def test_the_distribution_excludes_the_excluded_run(self) -> None:
        # R2's 6.118 is deliberately outside the accepted pair and must not move
        # the mean: only R1 (6.431) and R3 (6.520) contribute.
        stats = self.payload["accepted_columns"]["measured_end_position_m"]
        self.assertEqual(stats["count"], 2)
        self.assertAlmostEqual(stats["mean"], (6.431 + 6.520) / 2, places=6)
        self.assertNotAlmostEqual(stats["mean"], (6.118 + 6.431 + 6.520) / 3, places=4)

    def test_the_residual_separates_offset_from_spread(self) -> None:
        residual = self.payload["residual_end_position_m"]
        self.assertEqual(residual["rows_used"], 2)
        # measured_end - target: (6.431 - 6.500) and (6.520 - 6.500).
        self.assertAlmostEqual(residual["systematic_offset_m"], (-0.069 + 0.020) / 2, places=6)
        self.assertIsNotNone(residual["random_spread_m"])
        self.assertGreater(residual["random_spread_m"], 0.0)

    def test_groups_are_keyed_by_level_and_target(self) -> None:
        groups = self.payload["groups"]
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0]["tournament_level"], "State")
        self.assertEqual(groups[0]["target_distance_m"], 6.5)
        self.assertEqual(groups[0]["run_count"], 2)

    def test_the_same_file_gives_the_same_distribution(self) -> None:
        again = measurement_distribution(load_measurements(self.path))
        self.assertEqual(json.dumps(self.payload, sort_keys=True), json.dumps(again, sort_keys=True))

    def test_the_payload_claims_nothing_about_the_vehicle(self) -> None:
        self.assertIn("by itself", self.payload["note"])
        self.assertIn("VERIFIED_STATE.md", self.payload["note"])


class DistributionCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "report")
        self.path = os.path.join(self.tmp, "measured.csv")
        with open(self.path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,measured_run_time_s,pack_voltage_v,"
                "recharged,accepted,exclusion_reason,target_distance_m,tournament_level\n"
                "R1,6.431,13.31,11.9,yes,yes,,6.500,State\n"
                "R2,6.118,13.42,11.7,no,no,pack sagged mid-run,6.500,State\n"
                "R3,6.520,13.28,,yes,yes,,6.500,State\n"
            )

    def test_the_command_writes_a_deterministic_report(self) -> None:
        code, out, err = run_cli(["distribution", self.path, "--out", self.out])
        self.assertEqual(code, 0, err)
        self.assertIn("excluded runs", out)

        payload = json.loads(read(os.path.join(self.out, "distribution.json")))
        self.assertEqual(payload["classification"]["accepted"], 2)
        text = read(os.path.join(self.out, "distribution.txt"))
        self.assertIn("systematic_offset", text)
        self.assertIn("pack sagged mid-run", text)

        second = os.path.join(self.tmp, "report2")
        run_cli(["distribution", self.path, "--out", second])
        self.assertEqual(
            read(os.path.join(self.out, "distribution.txt")),
            read(os.path.join(second, "distribution.txt")),
        )
        self.assertEqual(
            read(os.path.join(self.out, "distribution.json")),
            read(os.path.join(second, "distribution.json")),
        )

    def test_a_missing_measurement_file_is_reported_cleanly(self) -> None:
        code, _, err = run_cli(
            ["distribution", os.path.join(self.tmp, "nope.csv"), "--out", self.out]
        )
        self.assertEqual(code, 2)
        self.assertIn("no such measurement file", err)

    def test_the_out_directory_cannot_name_an_authority_file(self) -> None:
        code, _, err = run_cli(
            ["distribution", self.path, "--out", os.path.join(self.tmp, "VERIFIED_STATE.md")]
        )
        self.assertEqual(code, 2)
        self.assertIn("refusing to write", err)

    def test_help_lists_the_distribution_command(self) -> None:
        code, out, _ = run_cli(["--help"])
        self.assertEqual(code, 0)
        self.assertIn("distribution", out)


class EndpointContextTest(unittest.TestCase):
    """The endpoint comparison must carry the acceptance context with each row."""

    def test_context_columns_reach_the_comparison(self) -> None:
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        path = os.path.join(tmp, "measured.csv")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "run_id,measured_end_position_m,pack_voltage_v,accepted,"
                "exclusion_reason,target_distance_m,tournament_level\n"
                "synthetic_a,2.500,11.9,no,tape slipped,2.400,State\n"
            )
        log = read_log(sample_path("synthetic_a.csv"))
        metrics = compute_metrics(log, "synthetic_a")
        comparison = compare_endpoints([metrics], load_measurements(path))
        row = comparison["runs"][0]
        self.assertIs(row["accepted"], False)
        self.assertEqual(row["exclusion_reason"], "tape slipped")
        self.assertEqual(row["target_distance_m"], 2.4)
        self.assertEqual(row["tournament_level"], "State")
        self.assertEqual(row["pack_voltage_v"], 11.9)


if __name__ == "__main__":
    unittest.main()
