"""Offline unit tests for tools/calibration/rollout_scale.py (S22).

All rows are synthetic fixtures. Nothing here measures a vehicle.
"""

import math
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import rollout_scale


def row(trial_id, start, end, travel_m, direction="forward"):
    return {
        "trial_id": trial_id,
        "tick_delta": end - start,
        "travel_m": travel_m,
        "direction": direction,
        "surface": "",
        "notes": "synthetic",
    }


class ComputeScalesTest(unittest.TestCase):
    def test_known_scale_example(self):
        report = rollout_scale.compute_scales([row("T1", 0, 20000, 2.0)])
        self.assertEqual(1, len(report.trials))
        self.assertEqual(0, len(report.rejected))
        self.assertAlmostEqual(0.0001, report.mean_scale, places=12)

    def test_sign_is_reported_not_folded(self):
        report = rollout_scale.compute_scales([row("T1", 0, -20000, 2.0, "forward")])
        self.assertEqual(1, len(report.trials))
        self.assertFalse(report.trials[0].sign_agrees)
        # Magnitude still uses abs(delta): polarity stays a separate fact.
        self.assertAlmostEqual(0.0001, report.trials[0].scale_m_per_tick, places=12)
        self.assertFalse(report.consistent)

    def test_zero_tick_delta_is_rejected_not_divided(self):
        report = rollout_scale.compute_scales([row("T1", 5000, 5000, 2.0)])
        self.assertEqual(0, len(report.trials))
        self.assertEqual(1, len(report.rejected))

    def test_residuals_match_definition_against_mean(self):
        rows = [
            row("T1", 0, 20000, 2.0),
            row("T2", 0, 20010, 2.0),
            row("T3", 0, 19990, 2.0),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(3, len(report.trials))
        for trial in report.trials:
            expected = trial.travel_m - abs(trial.tick_delta) * report.mean_scale
            self.assertAlmostEqual(expected, trial.residual_m, places=12)

    def test_inconsistent_trial_flags_report(self):
        rows = [row("T1", 0, 20000, 2.0), row("T2", 0, 20000, 2.2)]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(2, len(report.trials))
        self.assertFalse(report.consistent)

    def test_consistent_series_passes(self):
        rows = [row(f"T{i}", i * 20000, i * 20000 + 20000, 2.0) for i in range(5)]
        report = rollout_scale.compute_scales(rows)
        self.assertTrue(report.consistent)

    def test_reverse_direction_agrees_with_negative_delta(self):
        report = rollout_scale.compute_scales([row("R1", 40000, 20000, 2.0, "reverse")])
        self.assertTrue(report.trials[0].sign_agrees)

    def test_empty_input_has_no_summary(self):
        report = rollout_scale.compute_scales([])
        self.assertEqual([], report.trials)
        self.assertEqual(0.0, report.mean_scale)


class ParseValidationTest(unittest.TestCase):
    def _write(self, text):
        tmp = tempfile.NamedTemporaryFile(
            "w", suffix=".csv", delete=False, encoding="utf-8", newline="\n"
        )
        tmp.write(text)
        tmp.close()
        self.addCleanup(os.unlink, tmp.name)
        return tmp.name

    def test_bad_direction_rejected(self):
        path = self._write("trial_id,start_ticks,end_ticks,travel_m,direction\nT1,0,10,2.0,sideways\n")
        with self.assertRaises(ValueError):
            rollout_scale._read_csv_skipping_comments(path)

    def test_non_positive_travel_rejected(self):
        path = self._write("trial_id,start_ticks,end_ticks,travel_m,direction\nT1,0,10,0.0,forward\n")
        with self.assertRaises(ValueError):
            rollout_scale._read_csv_skipping_comments(path)

    def test_missing_column_rejected(self):
        path = self._write("trial_id,start_ticks,end_ticks,direction\nT1,0,10,forward\n")
        with self.assertRaises(ValueError):
            rollout_scale._read_csv_skipping_comments(path)

    def test_template_parses(self):
        path = self._write(rollout_scale.TEMPLATE)
        rows = rollout_scale._read_csv_skipping_comments(path)
        self.assertEqual(3, len(rows))
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(3, len(report.trials))


class CliTest(unittest.TestCase):
    def test_print_template(self):
        proc = subprocess.run(
            [sys.executable, "tools/calibration/rollout_scale.py", "--print-template"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, proc.returncode)
        self.assertIn("trial_id,start_ticks,end_ticks,travel_m,direction", proc.stdout)


if __name__ == "__main__":
    unittest.main()
