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
from evtelemetry.metrics import compute_metrics
from evtelemetry.parse import Log, parse_text


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
        """Transition-bounded durations, so they equal the legs exactly.

        A phase's duration is measured from the first sample of the phase to the
        first sample of the *next* one - the reading at which the change is first
        observable - not from the last same-phase sample, which is one cadence
        early and would report a complete phase one interval short.
        """
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
                self.assertAlmostEqual(
                    durations[leg.phase], leg.duration_ms / 1000.0, places=6
                )

    def test_the_phase_still_in_effect_at_the_end_is_marked_censored(self) -> None:
        """A span is not a dwell: the final phase is bounded by nothing.

        The capture ends while `Done` is still in effect, so its duration is
        unknown and is reported as `None` with the phase listed as censored, while
        its observed span (a lower bound) is still available.
        """
        phases = self.metrics["phases"]
        self.assertEqual(phases["censored_phases"], ["Done"])
        final_segment = phases["segments"][-1]
        self.assertEqual(final_segment["phase"], "Done")
        self.assertTrue(final_segment["censored"])
        self.assertIsNone(final_segment["duration_s"])
        self.assertIsNotNone(final_segment["observed_span_s"])
        # Every bounded phase keeps a bounded duration, and the observed span of a
        # bounded phase is never longer than its duration.
        for phase in phases["order"]:
            if phase == "Done":
                continue
            with self.subTest(phase=phase):
                self.assertIsNotNone(phases["durations_s"][phase])
                self.assertLessEqual(
                    phases["observed_span_s"][phase], phases["durations_s"][phase]
                )

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
        # ForwardDecel runs 1200 ms, BrakeZeroSpeed 400 ms. The dwell is the
        # transition-bounded duration (BrakeZeroSpeed's first sample to Reverse's
        # first sample), so it is the whole leg, not one cadence less.
        self.assertAlmostEqual(timing["forward_decel_to_brake_zero_s"], 1.2, places=6)
        self.assertAlmostEqual(timing["brake_zero_dwell_s"], 0.4, places=6)
        self.assertAlmostEqual(timing["brake_zero_to_reverse_s"], 0.4, places=6)
        self.assertAlmostEqual(timing["forward_decel_to_reverse_s"], 1.6, places=6)
        # The note must survive: these are not stopping distances.
        self.assertIn("not stopping distances", timing["note"])

    def test_the_endpoint_is_the_verified_stop_and_says_so(self) -> None:
        """The endpoint is the verified final stop, never just the last sample.

        Before S15 the endpoint was the last usable position anywhere in the
        capture, while the run's time was taken from the verified stop - so a
        vehicle handled after the run reported a position it never stopped at.
        """
        position = self.metrics["position"]
        self.assertEqual(position["first_m"], 0.0)
        self.assertIsNotNone(position["end_position_m"])
        self.assertEqual(position["end_position_source"], "verified_final_stop")
        self.assertIsNone(position["end_position_unavailable_reason"])
        self.assertEqual(position["verified_stop_m"], position["end_position_m"])
        self.assertEqual(position["verified_stop_leg"], "reverse")
        # The run reverses, so the believed end position is below the maximum.
        self.assertLess(position["end_position_m"], position["max_m"])
        # The stop is confirmed by a sample, so the endpoint is known only to within
        # the distance travelled between that sample and the previous one. The
        # window is reported, and here the vehicle had already settled, so it is 0.
        self.assertEqual(position["end_position_sampling_window_ms"], self.spec.cadence_ms)
        self.assertEqual(position["end_position_sampling_window_m"], 0.0)
        # Nothing moved the vehicle after the stop in this fixture.
        self.assertEqual(position["after_stop_travel_m"], 0.0)
        self.assertEqual(position["last_m"], position["end_position_m"])

    def test_the_reverse_leg_target_is_reconstructed_with_the_reverse_sign(self) -> None:
        """A reverse-leg stop's target is `position - remaining`, not `+`.

        The firmware's remaining distance on a reverse leg is `position - target`
        (`MotionPlan::remainingDistanceTo`). Adding it - the forward-leg
        definition - puts the believed target on the far side of the run, which is
        the defect M8 reported; the fixture carries the firmware's own per-leg
        signs, so the reconstruction has to use the leg the run actually stopped
        on.
        """
        position = self.metrics["position"]
        self.assertEqual(position["verified_stop_leg"], "reverse")
        self.assertEqual(position["believed_target_m"], self.spec.reverse_target_m)
        self.assertEqual(
            position["believed_target_source"],
            "reconstructed_from_verified_stop_reverse_leg",
        )
        # The forward definition would have produced the reverse leg's number with
        # the wrong sign; the two must not agree here, or this test proves nothing.
        forward_shape = position["verified_stop_m"] + position["verified_stop_remaining_m"]
        self.assertNotAlmostEqual(forward_shape, position["believed_target_m"], places=3)
        self.assertIn("firmware's run-axis estimate", position["note"])

    def test_a_reverse_leg_stop_keeps_the_forward_target_visible(self) -> None:
        """The forward execution target and the reverse target are both reported.

        The fixture's forward legs drive to `target_m` and its reverse legs come
        back to `reverse_target_m`; the sample lane's own `rem_m` shows both, so
        the analyzer can pin each leg against the fixture's declared value.
        """
        self.assertNotEqual(self.spec.reverse_target_m, self.spec.target_m)
        self.assertEqual(self.metrics["position"]["believed_target_m"], self.spec.reverse_target_m)

    def test_endpoints_section_reports_the_same_endpoint_and_its_completion(self) -> None:
        endpoints = self.metrics["endpoints"]
        self.assertTrue(endpoints["stop_verified"])
        self.assertEqual(endpoints["completion"], "done")
        self.assertEqual(endpoints["believed_end_position_source"], "verified_final_stop")
        self.assertEqual(
            endpoints["believed_end_position_m"],
            self.metrics["position"]["end_position_m"],
        )
        self.assertEqual(endpoints["verified_stop_leg"], "reverse")

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
            if "duration" in key and not key.endswith("_reason"):
                self.assertTrue(key.endswith("_s"), key)
        for key, value in metrics["run_window"].items():
            if (
                key == "note"
                or key.endswith("_t_abs_ms")
                or key.endswith("_reason")
                or isinstance(value, bool)
            ):
                continue
            # `completion` is a named outcome (done/fault/incomplete), not a
            # measurement, so it carries no unit of its own.
            if key == "completion":
                continue
            self.assertTrue(key.endswith("_ms") or key.endswith("_s"), key)
        for key in metrics["control_dt"]:
            if key in ("note", "over_50ms_count", "over_100ms_count", "zero_ms_count"):
                continue
            self.assertTrue(key.endswith("_ms"), key)
        for key in metrics["phase_timing"]:
            if key in ("note", "entry_t_abs_ms", "duration_s", "reversal_observed"):
                continue
            if key.endswith("_reason"):
                continue
            self.assertTrue(key.endswith("_s"), key)
        for key in metrics["position"]:
            if key in (
                "usable_sample_count",
                "unusable_sample_count",
                "note",
                # Clock readings, a flag, the leg, and the named source/reason of
                # the endpoint - each carries its own unit or none, by design.
                "verified_stop_t_abs_ms",
                "verified_stop_leg",
                "end_position_source",
                "end_position_unavailable_reason",
                "believed_target_source",
            ):
                continue
            # A distance is metres; the sampling window's clock interval is ms.
            self.assertTrue(key.endswith("_m") or key.endswith("_ms"), key)


def _restamp(lines: list[str], updates: dict[str, str]) -> list[str]:
    """Rewrites ``key=value`` fields in telemetry lines, keeping field order."""
    out: list[str] = []
    for line in lines:
        tokens = line.split(",")
        parts = [tokens[0]]
        for token in tokens[1:]:
            key, _, _value = token.partition("=")
            parts.append(f"{key}={updates[key]}" if key in updates else token)
        out.append(",".join(parts))
    return out


def _samples(lines: list[str]) -> list[str]:
    return [line for line in lines if line.startswith(schema.SAMPLE_PREFIX)]


_BOOT_LINE = (
    f"{schema.EVENT_PREFIX},ver={schema.SCHEMA_VERSION},t_ms=0,phase=Idle,armed=0,"
    "event=Boot,detail=-,fault=None,drop_s=0,drop_e=0"
)


class CaptureBoundaryTest(unittest.TestCase):
    """T1/M7: a capture can hold several boots and several runs.

    The old analyzer reconstructed every backwards timestamp as a uint32 wrap and
    had no notion of a session, so a capture holding a reset became a 49.7-day
    log with one run's timeline glued to another run's position. These cases pin
    the three different things a backwards reading can be, and the refusal that
    keeps an unlabelled restart from being reported as elapsed time.
    """

    def setUp(self) -> None:
        self.spec = spec("SYNTHETIC-A")
        self.lines = synthetic.generate_lines(self.spec)

    def _parse(self, lines: list[str]) -> Log:
        return parse_text("\n".join(lines) + "\n", source="<test>")

    def test_a_boot_opens_a_new_session_instead_of_a_49_day_jump(self) -> None:
        head = self.lines[:40]
        # The tail is the same generator's first ticks, written again from a
        # restarted clock: the Boot line says so, and it opens the new session.
        restarted = [
            _restamp([line], {"t_ms": str(index * 20)})[0]
            for index, line in enumerate(_samples(self.lines)[:6], start=1)
        ]
        log = self._parse([*head, _BOOT_LINE, *restarted])
        self.assertEqual(log.session_count, 2)
        self.assertTrue(all(session.opened_by_boot for session in log.sessions))
        self.assertEqual(log.sessions[1].sample_count, 6)
        self.assertEqual(log.sessions[1].boot_t_ms, 0)

        metrics = compute_metrics(log, "SYNTHETIC-A")
        selection = metrics["capture"]["selection"]
        self.assertTrue(selection["mixed_sessions"])
        self.assertEqual(selection["selected_session_index"], 1)
        self.assertEqual(metrics["counts"]["samples"], 6)
        self.assertEqual(
            selection["excluded_sample_count"], log.sessions[0].sample_count
        )
        self.assertGreater(selection["excluded_sample_count"], 0)
        # The restarted clock is analyzed on its own: no ~49.7-day duration.
        self.assertLess(metrics["window"]["log_duration_s"], 1.0)
        self.assertEqual(metrics["capture"]["clock_wrap_count"], 0)

    def test_a_wrap_inside_one_session_is_reconstructed(self) -> None:
        top = 1 << 32
        # The first three readings sit just under the top of the uint32 range and
        # the rest just above the bottom: one rollover, 21 ms of elapsed time.
        stamps = [top - 40, top - 20, top - 1, 20, 40, 60]
        lines = [
            _restamp([line], {"t_ms": str(stamps[index])})[0]
            for index, line in enumerate(_samples(self.lines)[3:9])
        ]
        log = self._parse(lines)
        metrics = compute_metrics(log, "SYNTHETIC-A")
        capture = metrics["capture"]
        self.assertEqual(capture["clock_wrap_count"], 1)
        self.assertEqual(capture["clock_wraps"][0]["kind"], "wrap")
        self.assertEqual(capture["clock_wraps"][0]["unsigned_delta_ms"], 21)
        self.assertEqual(capture["reordered_reading_count"], 0)
        self.assertEqual(capture["restart_suspected_count"], 0)
        self.assertIsNone(metrics["window"]["duration_unavailable_reason"])
        self.assertEqual(metrics["adjacent_sample_spacing"]["max_ms"], 21)
        self.assertEqual(metrics["window"]["log_duration_s"], 0.1)

    def test_a_few_milliseconds_backwards_is_a_reordered_reading_not_a_wrap(self) -> None:
        stamps = [1000, 1020, 1040, 1036, 1060, 1080]
        lines = [
            _restamp([line], {"t_ms": str(stamps[index])})[0]
            for index, line in enumerate(_samples(self.lines)[3:9])
        ]
        metrics = compute_metrics(self._parse(lines), "SYNTHETIC-A")
        capture = metrics["capture"]
        self.assertEqual(capture["reordered_reading_count"], 1)
        self.assertEqual(capture["clock_wrap_count"], 0)
        self.assertEqual(capture["restart_suspected_count"], 0)
        # The reordered reading stays in the current epoch with its signed
        # delta, so the window is the readings' own 80 ms, not a new epoch.
        self.assertEqual(metrics["window"]["log_duration_s"], 0.08)
        self.assertIsNone(metrics["window"]["duration_unavailable_reason"])

    def test_an_unlabelled_restart_refuses_an_elapsed_duration(self) -> None:
        """A big backwards step away from the rollover is not a wrap.

        This is the T1 reproducer's shape - samples, then a clock that restarted -
        with the ``Boot`` line missing from the capture. Nothing local can tell it
        from a corrupt reading, so the tool must not turn it into 49.7 days of
        elapsed time: the step is named, and the duration is refused with a reason.
        """
        stamps = [20000, 20020, 20040, 20, 40, 60]
        lines = [
            _restamp([line], {"t_ms": str(stamps[index])})[0]
            for index, line in enumerate(_samples(self.lines)[3:9])
        ]
        metrics = compute_metrics(self._parse(lines), "SYNTHETIC-A")
        capture = metrics["capture"]
        self.assertEqual(capture["restart_suspected_count"], 1)
        self.assertEqual(capture["restart_suspected_steps"][0]["kind"], "restart_suspected")
        self.assertEqual(capture["clock_wrap_count"], 0)
        self.assertIsNone(metrics["window"]["log_duration_s"])
        self.assertIn(
            "restart", metrics["window"]["duration_unavailable_reason"] or ""
        )

    def test_a_second_run_in_one_session_is_selected_not_mixed(self) -> None:
        """A session that re-armed holds two runs; the last one is analyzed."""
        first_run = self.lines
        # A completed second run, appended to the same session: a fresh actuation
        # reading and the run timeline that goes with it.
        # R1G/M11: the restamped run must stay producer-faithful - it has to
        # include the verification tick itself (the sample whose t_ms equals
        # its own stop_verified_ms), not only later Done ticks repeating the
        # stamp. The last six samples are all post-verification Dones, so the
        # slice starts at the verification tick: six samples either way.
        all_samples = _samples(self.lines)

        def _fields(line: str) -> dict[str, str]:
            return dict(
                token.split("=", 1) for token in line.split(",")[1:] if "=" in token
            )

        verify_at = next(
            index
            for index, line in enumerate(all_samples)
            if _fields(line).get("stop_verified_ok") == "1"
            and _fields(line).get("t_ms") == _fields(line).get("stop_verified_ms")
        )
        tail = all_samples[verify_at : verify_at + 6]
        restamped_stamp = str(30000)
        second_run = [
            _restamp(
                [line],
                {
                    "t_ms": str(30000 + index * 20),
                    "run_start_ms": "30000",
                    "dwell_start_ms": "33000",
                    "stop_verified_ms": restamped_stamp,
                },
            )[0]
            for index, line in enumerate(tail)
        ]
        metrics = compute_metrics(
            self._parse([*first_run, *second_run]), "SYNTHETIC-A"
        )
        selection = metrics["capture"]["selection"]
        self.assertEqual(selection["runs_in_session"], 2)
        self.assertTrue(selection["mixed_runs"])
        self.assertFalse(selection["mixed_sessions"])
        self.assertEqual(selection["selected_run_start_ms"], 30000)
        # Only the second run's samples are analyzed, and its own stop is the
        # endpoint - not the first run's position with the second run's time.
        self.assertEqual(metrics["counts"]["samples"], len(second_run))
        self.assertEqual(metrics["run_window"]["run_start_ms"], 30000)
        self.assertEqual(metrics["position"]["end_position_source"], "verified_final_stop")

    def test_a_fixture_keeps_its_declared_provenance(self) -> None:
        """The header says SYNTHETIC, whatever the file is called."""
        log = self._parse(self.lines)
        self.assertEqual(log.declared_provenance, synthetic.SYNTHETIC_MARKER)
        metrics = compute_metrics(log, "BENCH-1")
        self.assertEqual(
            metrics["capture"]["provenance_declared"], synthetic.SYNTHETIC_MARKER
        )

    def test_a_capture_with_no_declaration_declares_nothing(self) -> None:
        log = self._parse(_samples(self.lines)[:4])
        self.assertIsNone(log.declared_provenance)


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
