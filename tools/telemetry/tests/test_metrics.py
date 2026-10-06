"""Metrics tests: the arithmetic, the fixtures, and reproducibility.

Two things are being pinned here. The first is that a metric means what its name
says - a "phase timing" is a firmware state boundary and never a distance. The
second is that the same bytes always give the same numbers, because a report that
cannot be reproduced cannot be cited.
"""

from __future__ import annotations

import json
import unittest

from tests.support import metrics_for, sample_path, spec

from evtelemetry import metrics as metrics_module
from evtelemetry import schema, synthetic


class NominalRunTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.spec = spec("SYNTHETIC-A")
        cls.metrics = metrics_for(cls.spec)

    def test_counts_and_window(self) -> None:
        expected_ticks = sum(leg.duration_ms for leg in self.spec.legs) // self.spec.cadence_ms
        self.assertEqual(self.metrics["counts"]["samples"], expected_ticks)
        self.assertEqual(self.metrics["counts"]["parse_errors"], 0)
        # The six fixture header lines are not telemetry and are counted, not rejected.
        self.assertEqual(self.metrics["counts"]["ignored_lines"], 6)
        self.assertGreater(self.metrics["window"]["log_duration_s"], 20.0)

    def test_control_dt_is_the_reported_interval(self) -> None:
        dt = self.metrics["control_dt"]["dt_ms"]
        self.assertEqual(dt["min"], float(self.spec.cadence_ms))
        self.assertEqual(dt["max"], float(self.spec.cadence_ms))
        self.assertEqual(self.metrics["control_dt"]["zero_ms_count"], 0)

    def test_the_armed_window_starts_where_the_run_arms(self) -> None:
        run_window = self.metrics["run_window"]
        self.assertEqual(run_window["armed_first_t_abs_ms"], 320)
        self.assertGreater(run_window["armed_duration_s"] or 0.0, 0.0)

    def test_phase_order_matches_the_generator(self) -> None:
        self.assertEqual(
            self.metrics["phases"]["order"], [leg.phase for leg in self.spec.legs]
        )

    def test_phase_durations_track_the_generator_legs(self) -> None:
        durations = self.metrics["phases"]["durations_s"]
        for leg in self.spec.legs:
            # `FinalBrake` and `Done` are the two legs the fixture's final-stop
            # model rewrites: the phase becomes `Done` at the verification
            # instant, so those durations are the modelled timeline rather than
            # the demand table and are pinned by
            # `test_the_internal_run_time_is_the_fixtures_own_timeline` below.
            if leg.phase in ("FinalBrake", "Done"):
                continue
            with self.subTest(phase=leg.phase):
                expected = leg.duration_ms / 1000.0
                # The last tick of a leg is the first of the next, so a segment is
                # one cadence shorter than the leg.
                self.assertAlmostEqual(durations[leg.phase], expected - 0.02, places=6)

    def test_the_internal_run_time_is_the_fixtures_own_timeline(self) -> None:
        """The run's own clock, and the rule's confirmation interval outside it."""
        timeline = synthetic.final_stop_timeline(self.spec)
        self.assertIsNotNone(timeline)
        run_window = self.metrics["run_window"]

        self.assertTrue(run_window["final_stop_dwell_verified"])
        self.assertEqual(run_window["run_start_ms"], timeline.run_start_ms)
        self.assertEqual(run_window["final_stop_dwell_start_ms"], timeline.dwell_start_ms)
        self.assertEqual(run_window["stop_verified_ms"], timeline.stop_verified_ms)

        internal_run_time_s = (timeline.dwell_start_ms - timeline.run_start_ms) / 1000.0
        completed_s = (timeline.stop_verified_ms - timeline.run_start_ms) / 1000.0
        self.assertAlmostEqual(run_window["internal_run_time_s"], internal_run_time_s, places=6)
        self.assertAlmostEqual(
            run_window["final_stop_verification_completed_s"], completed_s, places=6
        )

        # The metric must not overcount the rule's own Complete Stop interval:
        # the distance between the two readings is exactly that interval, and it
        # is outside the Run Time. A value taken at `Done` would be the longer one.
        self.assertAlmostEqual(completed_s - internal_run_time_s, 3.0, places=6)
        # `Done` begins at the verification instant, so the two readings agree.
        self.assertEqual(run_window["done_t_abs_ms"], run_window["stop_verified_ms"])

    def test_braking_and_reversal_timing_are_state_boundaries(self) -> None:
        timing = self.metrics["phase_timing"]
        self.assertTrue(timing["reversal_observed"])
        # ForwardDecel runs 1200 ms, BrakeZeroSpeed 400 ms.
        self.assertAlmostEqual(timing["forward_decel_to_brake_zero_s"], 1.2, places=6)
        self.assertAlmostEqual(timing["brake_zero_dwell_s"], 0.38, places=6)
        self.assertAlmostEqual(timing["brake_zero_to_reverse_s"], 0.4, places=6)
        self.assertAlmostEqual(timing["forward_decel_to_reverse_s"], 1.6, places=6)
        # The note must survive: these are not stopping distances.
        self.assertIn("not stopping distances", timing["note"])

    def test_position_metrics_are_labelled_as_believed(self) -> None:
        position = self.metrics["position"]
        self.assertEqual(position["first_m"], 0.0)
        self.assertIsNotNone(position["end_position_m"])
        self.assertIsNotNone(position["believed_target_m"])
        self.assertEqual(position["believed_target_m"], self.spec.target_m)
        self.assertIn("firmware's run-axis estimate", position["note"])
        # The run reverses, so the believed end position is below the maximum.
        self.assertLess(position["end_position_m"], position["max_m"])

    def test_velocity_and_effort_are_present(self) -> None:
        velocity = self.metrics["velocity"]
        self.assertGreater(velocity["usable_sample_count"], 0)
        self.assertIsNotNone(velocity["tracking_error_mps"]["max_abs"])
        self.assertGreater(self.metrics["effort"]["overall_permille"]["max"], 0.0)
        self.assertIn("DriveForward", self.metrics["effort"]["by_motor_mode"])

    def test_raw_inputs_are_counts_and_say_so(self) -> None:
        raw = self.metrics["raw_inputs"]
        self.assertEqual(raw["units"], "raw ADC counts and a raw logic level; no conversion is documented")
        self.assertGreater(raw["battery_counts"]["max"], 0)
        self.assertEqual(raw["nfault_low"]["sample_count"], 0)

    def test_an_absent_driver_fault_line_is_not_reported_as_a_healthy_level(self) -> None:
        """The fixture vehicle has no external nFAULT line, like the assembled one.

        The metric must distinguish "no level was read" from "a level was read and
        it was high": `level_read_sample_count` is zero and the capability says
        why, so a reader cannot conclude the driver is healthy from `sample_count`
        being zero.
        """
        raw = self.metrics["raw_inputs"]
        self.assertEqual(raw["nfault_capabilities"], ["NotInstalled"])
        self.assertEqual(raw["nfault_low"]["level_read_sample_count"], 0)
        self.assertIn("not a healthy driver", raw["nfault_low"]["note"])

    def test_no_fault_and_no_denied_drive_in_the_nominal_run(self) -> None:
        self.assertEqual(self.metrics["faults"]["latched_event_count"], 0)
        self.assertGreater(self.metrics["authorization"]["drive_authorized_sample_count"], 0)
        # Only the pre-arming ticks are denied.
        self.assertEqual(self.metrics["authorization"]["outcome_counts"]["DeniedNotArmed"], 16)

    def test_phase_changes_agree_between_the_lanes(self) -> None:
        transitions = self.metrics["transitions"]
        self.assertEqual(
            len(transitions["phase_changes_from_samples"]),
            len(transitions["phase_changes_from_events"]),
        )
        self.assertEqual(transitions["phase_changes_only_in_samples_count"], 0)
        self.assertEqual(transitions["phase_changes_only_in_events_count"], 0)
        self.assertEqual(transitions["phase_change_mismatch_count"], 0)


class DroppedBurstTest(unittest.TestCase):
    """Variant B simulates a queue that could not store six records."""

    def setUp(self) -> None:
        self.spec = spec("SYNTHETIC-B")
        self.metrics = metrics_for(self.spec)

    def test_the_drop_counter_is_carried_and_reported(self) -> None:
        # The fixture's own tick count, less the burst the queue could not store.
        ticks = sum(leg.duration_ms for leg in self.spec.legs) // self.spec.cadence_ms
        self.assertEqual(self.metrics["drops"]["samples"]["final"], 6)
        self.assertTrue(self.metrics["drops"]["samples"]["monotonic_non_decreasing"])
        self.assertEqual(self.metrics["counts"]["samples"], ticks - 6)

    def test_the_dropped_boundary_is_visible_as_a_lane_disagreement(self) -> None:
        transitions = self.metrics["transitions"]
        self.assertEqual(transitions["phase_changes_only_in_samples_count"], 1)
        self.assertEqual(transitions["phase_changes_only_in_events_count"], 1)
        self.assertIn("drop counters", transitions["note"])
        # Reading the disagreement must never be silent about which side saw what.
        self.assertEqual(
            transitions["phase_changes_only_in_events"][0]["phase"], "ForwardDecel"
        )


class FaultRunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.metrics = metrics_for(spec("SYNTHETIC-C"))

    def test_the_fault_is_latched_and_the_run_ends_in_fault(self) -> None:
        faults = self.metrics["faults"]
        self.assertEqual(faults["latched_event_count"], 1)
        self.assertEqual(faults["latched_faults"][0]["fault"], "PhaseTimeout")
        self.assertEqual(faults["first_latched_t_abs_ms"], 10400)
        self.assertTrue(self.metrics["endpoints"]["ended_in_fault"])
        self.assertEqual(self.metrics["endpoints"]["final_phase"], "Fault")

    def test_no_reversal_happened(self) -> None:
        self.assertFalse(self.metrics["phase_timing"]["reversal_observed"])
        self.assertIsNone(self.metrics["phase_timing"]["forward_decel_to_reverse_s"])

    def test_the_fault_stop_is_reported_per_sample(self) -> None:
        self.assertGreater(self.metrics["authorization"]["outcome_counts"]["ForcedFaultStop"], 0)

    def test_a_run_that_never_verified_a_final_stop_has_no_run_time(self) -> None:
        run_window = self.metrics["run_window"]
        self.assertFalse(run_window["final_stop_dwell_verified"])
        self.assertIsNone(run_window["internal_run_time_s"])
        self.assertIsNone(run_window["final_stop_verification_completed_s"])
        # The actuation reading survives: the run did begin, and the fault is the
        # reason there is no verified stop to time to.
        self.assertIsNotNone(run_window["run_start_ms"])


class ReproducibilityTest(unittest.TestCase):
    def test_the_same_bytes_give_the_same_metrics(self) -> None:
        first = metrics_for(spec("SYNTHETIC-A"))
        second = metrics_for(spec("SYNTHETIC-A"))
        self.assertEqual(
            json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True)
        )

    def test_the_rounding_never_produces_a_negative_zero(self) -> None:
        metrics = metrics_for(spec("SYNTHETIC-A"))
        text = json.dumps(metrics, sort_keys=True)
        self.assertNotIn("-0.0,", text)
        self.assertNotIn("-0.0\n", text)

    def test_metric_names_carry_their_units(self) -> None:
        metrics = metrics_for(spec("SYNTHETIC-A"))
        # A duration is seconds, a clock reading is milliseconds, a distance is
        # metres. The unit is always somewhere in the key path, either on the leaf
        # or on the section that holds it. A boolean is a flag, not a measurement,
        # and carries no unit of its own.
        for key in metrics["window"]:
            if "duration" in key:
                self.assertTrue(key.endswith("_s"), key)
        for key, value in metrics["run_window"].items():
            if key == "note" or key.endswith("_t_abs_ms") or isinstance(value, bool):
                continue
            self.assertTrue(key.endswith("_ms") or key.endswith("_s"), key)
        for key in metrics["control_dt"]:
            if key in ("note", "over_50ms_count", "over_100ms_count", "zero_ms_count"):
                continue
            self.assertTrue(key.endswith("_ms"), key)
        for key in metrics["phase_timing"]:
            if key in ("note", "entry_t_abs_ms", "duration_s", "reversal_observed"):
                continue
            self.assertTrue(key.endswith("_s"), key)
        for key in metrics["position"]:
            if key in ("usable_sample_count", "unusable_sample_count", "note"):
                continue
            self.assertTrue(key.endswith("_m"), key)


class FixtureDriftTest(unittest.TestCase):
    """The committed fixtures must be exactly what the generator produces."""

    def test_committed_samples_match_the_generator(self) -> None:
        for specification in synthetic.SPECS:
            name = f"{specification.run_id.lower().replace('-', '_')}.csv"
            with self.subTest(run_id=specification.run_id):
                with open(sample_path(name), "r", encoding="utf-8") as handle:
                    self.assertEqual(handle.read(), synthetic.generate_text(specification))

    def test_every_fixture_is_labelled_synthetic(self) -> None:
        for specification in synthetic.SPECS:
            name = f"{specification.run_id.lower().replace('-', '_')}.csv"
            with self.subTest(run_id=specification.run_id):
                with open(sample_path(name), "r", encoding="utf-8") as handle:
                    header = [next(handle) for _ in range(4)]
                self.assertTrue(header[0].startswith("# SYNTHETIC"))
                self.assertIn("NOT MEASURED DATA", "".join(header))
                self.assertIn("never cite it in VERIFIED_STATE.md", "".join(header))

    def test_the_schema_mirror_has_no_field_the_firmware_does_not_write(self) -> None:
        # A cheap cross-check that does not need the firmware headers: every field
        # name must appear in the synthetic lines the mirror itself formats.
        line = synthetic.generate_lines(synthetic.SPECS[0])
        sample = next(item for item in line if item.startswith(schema.SAMPLE_PREFIX))
        tokens = sample.split(",")
        self.assertEqual(tokens[0], schema.SAMPLE_PREFIX)
        written = [token.split("=", 1)[0] for token in tokens[1:]]
        self.assertEqual(tuple(written), schema.SAMPLE_FIELDS)


class PhaseTimingProbeTest(unittest.TestCase):
    def test_the_probe_list_covers_the_braking_and_reversal_phases(self) -> None:
        for phase in ("ForwardDecel", "BrakeZeroSpeed", "Reverse", "FinalBrake"):
            with self.subTest(phase=phase):
                self.assertIn(phase, metrics_module.PHASE_TIMING_PROBE_PHASES)


if __name__ == "__main__":
    unittest.main()
