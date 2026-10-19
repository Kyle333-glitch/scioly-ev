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
        # S22C: polarity is established from the data (strict majority per
        # direction), never by assuming forward means positive ticks. Two
        # forward-positive trials establish forward-positive; the lone
        # forward-negative trial is excluded from every statistic and
        # listed for review -- flagged, never silently averaged.
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 0, 20010, 2.0, "forward"),
            row("BAD", 0, -20000, 2.0, "forward"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("forward-positive", report.polarity)
        self.assertTrue(report.polarity_established)
        self.assertEqual(2, len(report.trials))
        self.assertEqual(1, len(report.excluded))
        self.assertEqual("BAD", report.excluded[0].trial_id)
        self.assertFalse(report.excluded[0].sign_agrees)
        # Magnitude still uses abs(delta): polarity stays a separate fact.
        self.assertAlmostEqual(0.0001, report.excluded[0].scale_m_per_tick, places=12)
        # The excluded trial moved neither the mean nor the verdict.
        included_scales = [t.scale_m_per_tick for t in report.trials]
        self.assertAlmostEqual(
            sum(included_scales) / len(included_scales), report.mean_scale, places=12
        )
        self.assertFalse(report.consistent)
        self.assertFalse(report.promotable)
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)

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

    def test_mixed_direction_mean_and_residuals_use_magnitude(self):
        # S22V regression: the all-forward fixtures elsewhere cannot tell a
        # signed-delta bug from the specified magnitude arithmetic. Forward
        # +20000 and reverse -20000 over 2.0 m each must give the same
        # positive scale, a 0.0001 mean, and zero residuals. An implementation
        # averaging signed deltas (mean 0.0) or predicting without abs()
        # (reverse residual 4.0 m) fails here.
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("R1", 20000, 0, 2.0, "reverse"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(2, len(report.trials))
        self.assertAlmostEqual(0.0001, report.mean_scale, places=12)
        for trial in report.trials:
            self.assertAlmostEqual(0.0001, trial.scale_m_per_tick, places=12)
            self.assertAlmostEqual(0.0, trial.residual_m, places=12)
            self.assertTrue(trial.sign_agrees)
        self.assertTrue(report.consistent)

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


class S22cPolarityTest(unittest.TestCase):
    """S22C: polarity comes from the data; no assumed forward sign."""

    def test_consistent_forward_only_is_candidate(self):
        rows = [row(f"F{i}", i * 20000, i * 20000 + 20000, 2.0, "forward")
                for i in range(3)]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(3, len(report.trials))
        self.assertEqual(0, len(report.excluded))
        self.assertEqual("forward-positive", report.polarity)
        self.assertTrue(report.polarity_established)
        self.assertEqual("CANDIDATE", report.candidate_status)
        self.assertTrue(report.promotable)
        self.assertTrue(report.consistent)
        self.assertEqual(3, report.n_forward)
        self.assertEqual(0, report.n_reverse)
        self.assertIsNone(report.mean_reverse)

    def test_consistent_reverse_only_is_candidate(self):
        rows = [row(f"R{i}", 60000 - i * 20000, 40000 - i * 20000, 2.0, "reverse")
                for i in range(3)]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual(3, len(report.trials))
        self.assertEqual("forward-positive", report.polarity)  # via opposition
        self.assertTrue(report.polarity_established)
        self.assertEqual("CANDIDATE", report.candidate_status)
        self.assertEqual(0, report.n_forward)
        self.assertEqual(3, report.n_reverse)

    def test_forward_negative_rig_is_supported(self):
        # A rig where forward travel yields NEGATIVE ticks is legitimate
        # (H2 owns the convention). Majority signs oppose, so the candidate
        # stands with polarity forward-negative -- the old forward-means-+
        # assumption would have flagged every trial here.
        rows = [
            row("F1", 0, -20000, 2.0, "forward"),
            row("F2", -20000, -40010, 2.0, "forward"),
            row("R1", -40010, -20010, 2.0, "reverse"),
            row("R2", -20010, -5, 2.0, "reverse"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("forward-negative", report.polarity)
        self.assertTrue(report.polarity_established)
        self.assertEqual(4, len(report.trials))
        self.assertEqual(0, len(report.excluded))
        for trial in report.trials:
            self.assertTrue(trial.sign_agrees)
        self.assertEqual("CANDIDATE", report.candidate_status)

    def test_contradictory_polarity_promotes_nothing(self):
        # Both directions majority-positive: something is mislabelled or
        # miswired. Exclude nothing (cannot tell which side is wrong) and
        # promote nothing.
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 20000, 40000, 2.0, "forward"),
            row("R1", 40000, 60000, 2.0, "reverse"),
            row("R2", 60000, 80000, 2.0, "reverse"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("contradictory", report.polarity)
        self.assertFalse(report.polarity_established)
        self.assertEqual(0, len(report.excluded))
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)
        self.assertFalse(report.promotable)

    def test_tied_direction_promotes_nothing(self):
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 20000, 0, 2.0, "forward"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("undetermined", report.polarity)
        self.assertFalse(report.polarity_established)
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)


class S22cDirectionDifferenceTest(unittest.TestCase):
    """S22C: forward/reverse disagreement is reported, not averaged away."""

    def test_direction_disagreement_requires_review(self):
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 20000, 40000, 2.0, "forward"),
            row("R1", 40000, 20000, 2.2, "reverse"),  # ~10% coarser scale
            row("R2", 20000, 0, 2.2, "reverse"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertTrue(report.polarity_established)
        self.assertEqual(2, report.n_forward)
        self.assertEqual(2, report.n_reverse)
        self.assertAlmostEqual(0.0001, report.mean_forward, places=12)
        self.assertAlmostEqual(0.00011, report.mean_reverse, places=12)
        self.assertIsNotNone(report.direction_diff_rel)
        self.assertGreater(report.direction_diff_rel, 0.02)
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)
        self.assertFalse(report.promotable)
        text = rollout_scale.format_report(report)
        self.assertIn("REVIEW_REQUIRED", text)
        self.assertIn("direction_diff", text)

    def test_small_direction_difference_stays_supported(self):
        # Template-like data: sub-advisory-band directional scatter keeps a
        # candidate, with per-direction numbers visible.
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 20000, 40005, 2.0, "forward"),
            row("R1", 40005, 20010, 2.0, "reverse"),
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("CANDIDATE", report.candidate_status)
        self.assertIsNotNone(report.mean_forward)
        self.assertIsNotNone(report.mean_reverse)
        self.assertIsNotNone(report.direction_diff_rel)


class S22cUncertaintyTest(unittest.TestCase):
    """S22C: honest uncertainty -- no zero-std certainty, no invented criteria."""

    def test_single_trial_std_is_na_and_review_required(self):
        report = rollout_scale.compute_scales([row("T1", 0, 20000, 2.0)])
        self.assertEqual(1, len(report.trials))
        self.assertIsNone(report.std_scale)  # unknown, never 0.0
        self.assertEqual("undetermined", report.polarity)
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)
        self.assertFalse(report.promotable)
        text = rollout_scale.format_report(report)
        self.assertIn("n/a", text)
        self.assertIn("REVIEW_REQUIRED", text)

    def test_std_defined_for_two_or_more_trials(self):
        rows = [row("T1", 0, 20000, 2.0), row("T2", 0, 20010, 2.0)]
        report = rollout_scale.compute_scales(rows)
        self.assertIsNotNone(report.std_scale)
        self.assertGreater(report.std_scale, 0.0)

    def test_report_states_advisory_not_acceptance(self):
        rows = [row(f"T{i}", i * 20000, i * 20000 + 20000, 2.0) for i in range(3)]
        text = rollout_scale.format_report(rollout_scale.compute_scales(rows))
        self.assertIn("advisory", text.lower())
        self.assertIn("pre-registered", text)

    def test_invalid_rows_rejected_and_cli_exit_2(self):
        report = rollout_scale.compute_scales([row("T1", 5000, 5000, 2.0)])
        self.assertEqual([], report.trials)
        self.assertEqual(1, len(report.rejected))
        self.assertEqual("REVIEW_REQUIRED", report.candidate_status)
        tmp = tempfile.NamedTemporaryFile(
            "w", suffix=".csv", delete=False, encoding="utf-8", newline="\n"
        )
        try:
            tmp.write("trial_id,start_ticks,end_ticks,travel_m,direction\n"
                      "T1,5000,5000,2.0,forward\n")
            tmp.close()
            proc = subprocess.run(
                [sys.executable, "tools/calibration/rollout_scale.py", tmp.name],
                capture_output=True, text=True,
            )
            self.assertEqual(2, proc.returncode)
            self.assertIn("no included trials", proc.stdout)
        finally:
            os.unlink(tmp.name)

    def test_non_finite_travel_rejected_at_parse(self):
        for bad in ("nan", "inf", "-inf"):
            tmp = tempfile.NamedTemporaryFile(
                "w", suffix=".csv", delete=False, encoding="utf-8", newline="\n"
            )
            try:
                tmp.write("trial_id,start_ticks,end_ticks,travel_m,direction\n"
                          f"T1,0,20000,{bad},forward\n")
                tmp.close()
                with self.assertRaises(ValueError, msg=bad):
                    rollout_scale._read_csv_skipping_comments(tmp.name)
            finally:
                os.unlink(tmp.name)

    def test_residuals_use_magnitude_with_mixed_sign_rig(self):
        # Forward-negative rig with unequal spans: residuals must follow
        # travel - |delta| * mean (signed-delta arithmetic fails here).
        rows = [
            row("F1", 0, -20000, 2.0, "forward"),
            row("R1", -20000, 100, 2.01, "reverse"),  # delta +20100
        ]
        report = rollout_scale.compute_scales(rows)
        self.assertEqual("forward-negative", report.polarity)
        self.assertEqual(2, len(report.trials))
        for trial in report.trials:
            expected = trial.travel_m - abs(trial.tick_delta) * report.mean_scale
            self.assertAlmostEqual(expected, trial.residual_m, places=12)
            self.assertGreater(trial.scale_m_per_tick, 0.0)

    def test_report_determinism(self):
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 20000, 40005, 2.0, "forward"),
            row("R1", 40005, 20010, 2.0, "reverse"),
            row("BAD", 40005, 20005, 2.0, "forward"),  # Exercises EXCLUDED
        ]
        first = rollout_scale.format_report(rollout_scale.compute_scales(rows))
        second = rollout_scale.format_report(rollout_scale.compute_scales(rows))
        self.assertEqual(first, second)
        self.assertIn("EXCLUDED", first)

    def test_questionable_results_carry_do_not_promote(self):
        rows = [
            row("F1", 0, 20000, 2.0, "forward"),
            row("F2", 0, 20010, 2.0, "forward"),
            row("BAD", 0, -20000, 2.0, "forward"),
        ]
        text = rollout_scale.format_report(rollout_scale.compute_scales(rows))
        self.assertIn("REVIEW_REQUIRED", text)
        self.assertIn("do not promote", text.lower())

    def test_candidate_still_disclaims_promotion(self):
        rows = [row(f"T{i}", i * 20000, i * 20000 + 20000, 2.0) for i in range(3)]
        text = rollout_scale.format_report(rollout_scale.compute_scales(rows))
        self.assertIn("CANDIDATE", text)
        self.assertIn("NOT promoted", text)


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
