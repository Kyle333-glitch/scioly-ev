"""Parser tests: the schema is read exactly, and corruption is refused.

The point of these tests is the *failure* half. A parser that accepts anything is
indistinguishable from one that works, until an analysis quietly describes a
column that no longer means what it used to.
"""

from __future__ import annotations

import unittest

from tests.support import parse_spec, spec

from evtelemetry import parse, schema
from evtelemetry.parse import (
    Event,
    FieldShapeError,
    FieldValueError,
    Sample,
    SchemaVersionError,
    TelemetryFormatError,
    parse_line,
    parse_lines,
    parse_text,
)

# The literal line the firmware's own `test_telemetry_format` pins. Using it here
# means the tool is checked against the record a board actually emits, not
# against a paraphrase of it.
REAL_SAMPLE_LINE = (
    "EVS2,ver=2,t_ms=5832,dt_ms=20,phase=Approach,cmd=DriveForward,armed=1,"
    "pos_m=6.2140,pos_ok=1,vel_mps=0.7140,vel_ok=1,vel_cmd_mps=0.7000,rem_m=0.2860,"
    "motor=DriveForward,effort_permille=740,batt_counts=812,curr_counts=513,"
    "nfault=Installed,nfault_low=0,"
    "fault=None,authz=AuthorizedDrive,check=none,drop_s=0,drop_e=0"
)
REAL_EVENT_LINE = (
    "EVE2,ver=2,t_ms=1200,phase=Approach,armed=1,event=FaultLatched,"
    "detail=ProvisionalCalibration,fault=ProvisionalCalibration,drop_s=3,drop_e=1"
)

_SAMPLE_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "t_ms": "0",
    "dt_ms": "20",
    "phase": "Idle",
    "cmd": "none",
    "armed": "0",
    "pos_m": "0.0000",
    "pos_ok": "0",
    "vel_mps": "0.0000",
    "vel_ok": "0",
    "vel_cmd_mps": "0.0000",
    "rem_m": "0.0000",
    "motor": "Disabled",
    "effort_permille": "0",
    "batt_counts": "800",
    "curr_counts": "200",
    "nfault": "Installed",
    "nfault_low": "0",
    "fault": "None",
    "authz": "DeniedNotArmed",
    "check": "none",
    "drop_s": "0",
    "drop_e": "0",
}

_EVENT_DEFAULTS = {
    "ver": str(schema.SCHEMA_VERSION),
    "t_ms": "0",
    "phase": "Idle",
    "armed": "0",
    "event": "Boot",
    "detail": "-",
    "fault": "None",
    "drop_s": "0",
    "drop_e": "0",
}


def sample_line(**overrides: str) -> str:
    values = dict(_SAMPLE_DEFAULTS)
    values.update(overrides)
    return ",".join(
        [schema.SAMPLE_PREFIX] + [f"{key}={values[key]}" for key in schema.SAMPLE_FIELDS]
    )


def event_line(**overrides: str) -> str:
    values = dict(_EVENT_DEFAULTS)
    values.update(overrides)
    return ",".join(
        [schema.EVENT_PREFIX] + [f"{key}={values[key]}" for key in schema.EVENT_FIELDS]
    )


class RealRecordTest(unittest.TestCase):
    def test_a_real_sample_line_parses_field_for_field(self) -> None:
        record = parse_line(REAL_SAMPLE_LINE)
        self.assertIsInstance(record, Sample)
        assert isinstance(record, Sample)
        self.assertEqual(record.t_ms, 5832)
        self.assertEqual(record.dt_ms, 20)
        self.assertEqual(record.phase, "Approach")
        self.assertEqual(record.command, "DriveForward")
        self.assertTrue(record.armed)
        self.assertAlmostEqual(record.position_m, 6.214)
        self.assertTrue(record.position_ok)
        self.assertAlmostEqual(record.velocity_mps, 0.714)
        self.assertAlmostEqual(record.velocity_cmd_mps, 0.7)
        self.assertAlmostEqual(record.remaining_m, 0.286)
        self.assertEqual(record.motor, "DriveForward")
        self.assertEqual(record.effort_permille, 740)
        self.assertEqual(record.battery_counts, 812)
        self.assertEqual(record.current_counts, 513)
        self.assertFalse(record.driver_fault_bar_low)
        self.assertEqual(record.fault, "None")
        self.assertEqual(record.authz, "AuthorizedDrive")
        self.assertEqual(record.check, "none")
        self.assertEqual((record.drop_samples, record.drop_events), (0, 0))

    def test_a_real_event_line_parses_field_for_field(self) -> None:
        record = parse_line(REAL_EVENT_LINE)
        self.assertIsInstance(record, Event)
        assert isinstance(record, Event)
        self.assertEqual(record.t_ms, 1200)
        self.assertEqual(record.phase, "Approach")
        self.assertTrue(record.armed)
        self.assertEqual(record.event, "FaultLatched")
        self.assertEqual(record.detail, "ProvisionalCalibration")
        self.assertEqual(record.fault, "ProvisionalCalibration")
        self.assertEqual((record.drop_samples, record.drop_events), (3, 1))

    def test_trailing_carriage_return_is_tolerated(self) -> None:
        # A serial capture on Windows may keep the CR before the LF.
        record = parse_line(REAL_SAMPLE_LINE + "\r")
        self.assertIsInstance(record, Sample)


class NonTelemetryTest(unittest.TestCase):
    def test_lines_that_are_not_telemetry_are_ignored(self) -> None:
        for line in (
            "",
            "   ",
            "# SYNTHETIC TELEMETRY - generated fixture",
            "BOOT,fw=0.1,note=ready",
            "OK,announce",
            "ERR,UnknownCommand",
            "STATUS,phase=Idle,armed=0",
            "CONFIG,level=state",
            "CHECK,ConfigurationValid=0",
            "TELEMETRY,enabled=1",
            "VERSION,ev=1",
        ):
            with self.subTest(line=line):
                self.assertIsNone(parse_line(line))

    def test_a_log_of_only_non_telemetry_is_empty_not_broken(self) -> None:
        log = parse_lines(["BOOT,fw=1", "", "OK,status"])
        self.assertTrue(log.is_empty)
        self.assertEqual(log.ignored_lines, [1, 2, 3])
        self.assertEqual(log.errors, [])


class CorruptionTest(unittest.TestCase):
    """Every case here must fail, and fail with a message a person can act on."""

    def assertRefused(self, line: str, error: type[TelemetryFormatError], needle: str) -> None:
        with self.assertRaises(error) as caught:
            parse_line(line, line_number=7, source="session.log")
        message = str(caught.exception)
        self.assertIn("session.log:7", message)
        self.assertIn(needle, message)
        # The offending line is shown, so a reader does not have to go looking.
        self.assertIn("line:", message)

    def test_a_different_schema_version_is_refused(self) -> None:
        self.assertRefused(
            sample_line().replace("EVS2", "EVS1", 1), SchemaVersionError, "schema version 1"
        )
        self.assertRefused(
            event_line().replace("EVE2", "EVE3", 1), SchemaVersionError, "schema version 3"
        )

    def test_an_unknown_prefix_is_refused(self) -> None:
        self.assertRefused(
            sample_line().replace("EVS2", "EVX2", 1), SchemaVersionError, "not a record prefix"
        )

    def test_the_prefix_digit_and_the_version_field_must_agree(self) -> None:
        # Both say 2, so this parses; changing only `ver` must not.
        self.assertRefused(
            sample_line(ver="1"), SchemaVersionError, "prefix says schema version 2"
        )

    def test_a_renamed_field_is_refused_not_absorbed(self) -> None:
        self.assertRefused(
            sample_line().replace("vel_cmd_mps=", "velocity_cmd="),
            FieldShapeError,
            "field set is not the schema's",
        )

    def test_a_missing_field_is_named(self) -> None:
        self.assertRefused(
            sample_line().replace(",dt_ms=20", "", 1), FieldShapeError, "missing dt_ms"
        )

    def test_an_extra_field_is_named(self) -> None:
        self.assertRefused(
            sample_line() + ",extra=1", FieldShapeError, "unexpected extra"
        )

    def test_reordered_fields_are_refused(self) -> None:
        fields = sample_line().split(",")
        fields[1], fields[2] = fields[2], fields[1]  # swap ver and t_ms
        self.assertRefused(",".join(fields), FieldShapeError, "not in the schema's order")

    def test_a_field_that_is_not_name_equals_value_is_refused(self) -> None:
        self.assertRefused(
            sample_line().replace("dt_ms=20", "dt_ms:20", 1), FieldShapeError, "name=value"
        )

    def test_a_non_numeric_unsigned_field_is_refused(self) -> None:
        self.assertRefused(sample_line(t_ms="soon"), FieldValueError, "not a plain unsigned")

    def test_a_negative_unsigned_field_is_refused(self) -> None:
        self.assertRefused(sample_line(effort_permille="-5"), FieldValueError, "not a plain unsigned")

    def test_a_value_wider_than_its_firmware_type_is_refused(self) -> None:
        self.assertRefused(sample_line(dt_ms="70000"), FieldValueError, "wider than its firmware type")
        self.assertRefused(sample_line(batt_counts="65536"), FieldValueError, "wider than its firmware type")

    def test_a_boolean_that_is_not_the_schema_character_is_refused(self) -> None:
        self.assertRefused(sample_line(armed="true"), FieldValueError, "single-character boolean")

    def test_the_driver_fault_level_accepts_only_a_boolean_or_absence(self) -> None:
        # A wired line carries a boolean; an absent line carries the marker. A
        # third spelling - `not_installed` in the level field, say - is refused
        # rather than quietly absorbed.
        self.assertRefused(
            sample_line(nfault_low="not_installed"), FieldValueError, "single-character boolean"
        )

    def test_an_absent_line_with_a_level_is_refused(self) -> None:
        # The two fields describe one fact: a record may not claim the signal does
        # not exist and carry a reading of it.
        self.assertRefused(
            sample_line(nfault="NotInstalled", nfault_low="0"),
            FieldValueError,
            "not installed but carries a level",
        )

    def test_an_installed_line_with_no_level_is_refused(self) -> None:
        self.assertRefused(
            sample_line(nfault="Installed", nfault_low="-"),
            FieldValueError,
            "installed but carries no level",
        )

    def test_the_absent_line_is_the_only_way_to_spell_an_unavailable_signal(self) -> None:
        record = parse_line(sample_line(nfault="NotInstalled", nfault_low="-"))
        assert isinstance(record, Sample)
        self.assertEqual(record.driver_fault_capability, "NotInstalled")
        self.assertIsNone(record.driver_fault_bar_low)

    def test_an_unknown_vocabulary_value_is_refused(self) -> None:
        self.assertRefused(sample_line(phase="Cruising"), FieldValueError, "not in the schema's vocabulary")
        self.assertRefused(event_line(event="SomethingNew"), FieldValueError, "not in the schema's vocabulary")

    def test_a_fixed_point_value_in_a_foreign_spelling_is_refused(self) -> None:
        for value in ("1e5", "1.5", ".5", "1.00000", "0.0000 "):
            with self.subTest(value=value):
                self.assertRefused(sample_line(pos_m=value), FieldValueError, "schema's spelling")

    def test_a_comma_inside_a_value_cannot_survive_the_line_format(self) -> None:
        # A comma is the field separator, so a grouped number splits the record.
        # It is caught as a shape error, which is the honest diagnosis: the line
        # has more fields than the schema defines.
        self.assertRefused(sample_line(pos_m="1,000.0000"), FieldShapeError, "name=value")

    def test_a_detail_that_is_not_a_token_is_refused(self) -> None:
        for detail in ("two words", "a=b", "9starts_with_a_digit", "x" * 31):
            with self.subTest(detail=detail):
                self.assertRefused(event_line(detail=detail), FieldValueError, "vocabulary name")

    def test_the_missing_marker_is_accepted_where_the_schema_uses_it(self) -> None:
        record = parse_line(event_line(detail="-"))
        assert isinstance(record, Event)
        self.assertEqual(record.detail, "-")

    def test_an_unknown_schema_error_is_still_a_format_error(self) -> None:
        with self.assertRaises(TelemetryFormatError):
            parse_line(sample_line().replace("EVS2", "EVS9", 1))


class SentinelTest(unittest.TestCase):
    def test_every_sentinel_parses_to_the_right_value(self) -> None:
        record = parse_line(sample_line(pos_m="nan", vel_mps="inf", vel_cmd_mps="-inf", rem_m="ovf"))
        assert isinstance(record, Sample)
        self.assertNotEqual(record.position_m, record.position_m)  # NaN
        self.assertEqual(record.velocity_mps, float("inf"))
        self.assertEqual(record.velocity_cmd_mps, float("-inf"))
        self.assertIsNone(record.remaining_m)  # `ovf` is not a number

    def test_ovf_is_none_rather_than_infinity(self) -> None:
        # Representing `ovf` as infinity would let a bug enter a mean as a huge
        # measurement; `None` keeps it out of every statistic.
        record = parse_line(sample_line(rem_m="ovf"))
        assert isinstance(record, Sample)
        self.assertIsNone(record.remaining_m)

    def test_usability_flags_are_independent_of_the_sentinels(self) -> None:
        record = parse_line(sample_line(pos_m="nan", pos_ok="1"))
        assert isinstance(record, Sample)
        self.assertTrue(record.position_ok)
        # ... but the derived predicate still refuses to call NaN usable.
        self.assertFalse(record.is_usable_position)


class StreamTest(unittest.TestCase):
    def test_records_keep_the_order_they_arrived_in(self) -> None:
        log = parse_lines([event_line(t_ms="10"), sample_line(t_ms="20"), event_line(t_ms="30")])
        self.assertEqual([type(record).__name__ for record in log.records], ["Event", "Sample", "Event"])

    def test_millis_wraparound_is_unwrapped(self) -> None:
        log = parse_lines([sample_line(t_ms=str(0xFFFFFFF0)), sample_line(t_ms="16")])
        self.assertEqual(log.samples[0].t_abs_ms, 0xFFFFFFF0)
        self.assertEqual(log.samples[1].t_abs_ms - log.samples[0].t_abs_ms, 32)

    def test_the_two_lanes_are_unwrapped_independently(self) -> None:
        # Events drain first, so an event for a later tick can precede a sample
        # for an earlier one. One shared counter would read that legal
        # interleaving as a timestamp jump of nearly 2^32 ms.
        log = parse_lines(
            [
                event_line(t_ms="100"),
                event_line(t_ms="200"),
                sample_line(t_ms="100"),
                sample_line(t_ms="200"),
            ]
        )
        self.assertEqual([event.t_abs_ms for event in log.events], [100, 200])
        self.assertEqual([sample.t_abs_ms for sample in log.samples], [100, 200])

    def test_a_strict_parse_refuses_the_first_bad_line(self) -> None:
        with self.assertRaises(FieldValueError):
            parse_lines([sample_line(), sample_line(phase="Nope"), sample_line()])

    def test_a_permissive_parse_records_errors_and_keeps_going(self) -> None:
        log = parse_lines(
            [sample_line(t_ms="0"), sample_line(phase="Nope", t_ms="20"), sample_line(t_ms="40")],
            source="damaged.log",
            continue_on_error=True,
        )
        self.assertEqual(len(log.samples), 2)
        self.assertEqual(len(log.errors), 1)
        self.assertIn("damaged.log:2", log.errors[0].message)

    def test_a_permissive_parse_still_reports_the_line_in_its_report(self) -> None:
        log = parse_lines(["EVS1,ver=1"], continue_on_error=True)
        self.assertEqual(log.errors[0].line_number, 1)
        self.assertIn("schema version 1", log.errors[0].message)

    def test_parse_text_hashes_the_capture(self) -> None:
        log = parse_text(sample_line())
        self.assertIsNotNone(log.sha256)
        self.assertEqual(len(log.sha256 or ""), 64)

    def test_synthetic_captures_parse_strictly(self) -> None:
        # The tool's own fixtures must be real schema lines, not a convenient
        # paraphrase: this is what proves the generator writes the wire format.
        for run_id in ("SYNTHETIC-A", "SYNTHETIC-B", "SYNTHETIC-C"):
            with self.subTest(run_id=run_id):
                log = parse_spec(spec(run_id))
                self.assertGreater(len(log.samples), 0)
                self.assertGreater(len(log.events), 0)
                self.assertEqual(log.errors, [])


if __name__ == "__main__":
    unittest.main()
