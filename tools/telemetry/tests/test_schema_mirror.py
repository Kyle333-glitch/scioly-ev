"""The mirror must not drift from the firmware.

``evtelemetry/schema.py`` is a hand-maintained copy of the firmware's telemetry
schema, and a hand-maintained copy is exactly the kind of thing that goes stale
silently. This test re-reads the firmware headers and fails if the mirror
disagrees with them, so a field renamed or a vocabulary entry added in the
firmware cannot leave the analysis tool quietly reading a different record.

The test is skipped - not failed - when the firmware headers are absent, so the
tooling still works when copied somewhere else. It never silently passes when
the headers *are* present but unreadable: an extraction that finds nothing is a
failure, because "could not verify" must not read like "verified".
"""

from __future__ import annotations

import os
import re
import unittest

from tests.support import FIRMWARE_LIB

from evtelemetry import schema

_TELEMETRY_RECORD = os.path.join(FIRMWARE_LIB, "TelemetryRecord.h")
_TELEMETRY_FORMAT = os.path.join(FIRMWARE_LIB, "TelemetryFormat.h")
_CONFIGURATION_CHECKS = os.path.join(FIRMWARE_LIB, "ConfigurationChecks.h")
_BOUNDED_TEXT = os.path.join(FIRMWARE_LIB, "BoundedText.h")


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _brace_block(source: str, search_from: int, description: str) -> str:
    """The brace-balanced block that follows ``search_from``."""
    brace = source.find("{", search_from)
    if brace < 0:
        raise AssertionError(f"could not find the body of {description!r}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unbalanced braces in {description!r}")


def _function_body(source: str, signature_fragment: str) -> str:
    """The body of a function, found by a fragment of its signature."""
    start = source.find(signature_fragment)
    if start < 0:
        raise AssertionError(f"could not find the function containing {signature_fragment!r}")
    return _brace_block(source, start, signature_fragment)


def _field_order(source: str, signature_fragment: str) -> list[str]:
    body = _function_body(source, signature_fragment)
    return re.findall(r'\.field\("([A-Za-z0-9_]+)"\)', body)


def _vocabulary(source: str, signature_fragment: str) -> tuple[str, ...]:
    """The `switch` cases of a vocabulary function, in order.

    Only the switch block is read. Each of these functions ends with a defensive
    `return "?";` (or `return "none";`) after the switch - an unreachable
    fallback, not a vocabulary entry - and counting it would report a difference
    that does not exist.
    """
    body = _function_body(source, signature_fragment)
    switch = body.find("switch")
    if switch < 0:
        raise AssertionError(f"no switch found in {signature_fragment!r}")
    return tuple(re.findall(r'return "([^"]*)";', _brace_block(body, switch, signature_fragment)))


@unittest.skipUnless(
    os.path.exists(_TELEMETRY_FORMAT) and os.path.exists(_TELEMETRY_RECORD),
    "firmware headers are not present, so the mirror cannot be checked",
)
class SchemaMirrorTest(unittest.TestCase):
    def setUp(self) -> None:
        self.format_source = _read(_TELEMETRY_FORMAT)
        self.record_source = _read(_TELEMETRY_RECORD)

    def test_schema_version_matches(self) -> None:
        match = re.search(
            r"constexpr uint8_t kTelemetrySchemaVersion = (\d+)U;", self.record_source
        )
        self.assertIsNotNone(match, "kTelemetrySchemaVersion not found in TelemetryRecord.h")
        self.assertEqual(int(match.group(1)), schema.SCHEMA_VERSION)

    def test_prefixes_match(self) -> None:
        for constant, expected in (
            ("kTelemetrySamplePrefix", schema.SAMPLE_PREFIX),
            ("kTelemetryEventPrefix", schema.EVENT_PREFIX),
        ):
            match = re.search(rf'{constant} = "([^"]+)";', self.record_source)
            self.assertIsNotNone(match, f"{constant} not found")
            self.assertEqual(match.group(1), expected)

    def test_missing_field_marker_matches(self) -> None:
        match = re.search(r'kTelemetryMissingField = "([^"]*)";', self.record_source)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), schema.MISSING_FIELD)

    def test_field_decimals_match(self) -> None:
        match = re.search(r"constexpr uint8_t kTelemetryFieldDecimals = (\d+)U;", self.record_source)
        self.assertIsNotNone(match, "kTelemetryFieldDecimals not found")
        self.assertEqual(int(match.group(1)), schema.FIELD_DECIMALS)

    def test_sample_field_order_matches_the_formatter(self) -> None:
        order = _field_order(self.format_source, "formatTelemetrySample")
        self.assertTrue(order, "no fields extracted from formatTelemetrySample")
        self.assertIn(
            "appendTelemetryDropCounters(text, sample.drops)", 
            _function_body(self.format_source, "formatTelemetrySample"),
            "the sample formatter no longer ends with the drop counters",
        )
        self.assertEqual(tuple(order) + schema.DROP_FIELDS, schema.SAMPLE_FIELDS)

    def test_event_field_order_matches_the_formatter(self) -> None:
        order = _field_order(self.format_source, "formatTelemetryEvent")
        self.assertTrue(order, "no fields extracted from formatTelemetryEvent")
        self.assertEqual(tuple(order) + schema.DROP_FIELDS, schema.EVENT_FIELDS)

    def test_vocabularies_match(self) -> None:
        checks = _read(_CONFIGURATION_CHECKS)
        cases = (
            ("telemetryPhaseName", schema.PHASES, self.record_source),
            ("telemetryRunCommandName", schema.COMMANDS, self.record_source),
            ("telemetryMotorModeName", schema.MOTOR_MODES, self.record_source),
            ("telemetryFaultName", schema.FAULTS, self.record_source),
            ("telemetryAuthzOutcomeName", schema.AUTHZ_OUTCOMES, self.record_source),
            ("telemetryConfigErrorName", schema.CONFIG_ERRORS, self.record_source),
            ("telemetryEventName", schema.EVENT_CODES, self.record_source),
            ("configurationCheckName", schema.CONFIG_CHECKS, checks),
        )
        for name, expected, source in cases:
            with self.subTest(function=name):
                found = _vocabulary(source, name)
                self.assertTrue(found, f"no names extracted from {name}")
                self.assertEqual(found, expected)

    def test_fixed_point_sentinels_match(self) -> None:
        bounded = _read(_BOUNDED_TEXT)
        fixed_body = _function_body(bounded, "appendFixed")
        for sentinel in (
            schema.SENTINEL_NAN,
            schema.SENTINEL_INF,
            schema.SENTINEL_NEG_INF,
            schema.SENTINEL_OVF,
        ):
            with self.subTest(sentinel=sentinel):
                # `inf` and `-inf` are chosen by a conditional inside one call, so
                # the check is that the literal is present in the sentinel logic
                # rather than that it is the argument of its own appendText().
                self.assertIn(
                    f'"{sentinel}"',
                    fixed_body,
                    f"BoundedText no longer writes the '{sentinel}' sentinel",
                )

    def test_vocabulary_names_are_wire_safe(self) -> None:
        for name in schema.all_vocabulary_names():
            with self.subTest(name=name):
                self.assertNotIn(",", name)
                self.assertNotIn("=", name)
                self.assertNotIn(" ", name)
                self.assertLessEqual(len(name), schema.MAX_VOCABULARY_NAME_CHARS)
                self.assertNotEqual(name, "")


if __name__ == "__main__":
    unittest.main()
