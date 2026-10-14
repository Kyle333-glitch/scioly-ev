"""Parse a captured telemetry stream into records.

The firmware writes one line per record: ``EVS<v>,...`` for a control tick and
``EVE<v>,...`` for a transition or an operator action. This module turns those
lines into :class:`Sample` and :class:`Event` values, and refuses anything that
does not match the frozen schema.

Four rules shape the design, and all four exist so a reader cannot be misled
by a log it does not understand:

* **Telemetry must parse, or the parse fails.** A line whose prefix is the
  telemetry root (``EV``) is a claim to be telemetry. If it declares a different
  schema version, if its field set is not the one the schema froze, if a field
  is misspelled or reordered, or if a value is not the kind of value that field
  holds, that is an error and it is reported with the line number, the offending
  line and what was expected. Silently skipping it (or silently accepting a
  renamed column) is how an analysis ends up describing a different record than
  the one that was logged.
* **Non-telemetry is not an error.** A captured serial stream also contains
  operator responses (``OK``, ``STATUS``, ...), the staged ``BOOT,...`` line and
  whatever else the port saw. Those lines are counted as ignored, because a log
  is not required to be pure telemetry. Header lines that declare the capture's
  own provenance (``# SYNTHETIC ...``) are read for that declaration and are
  otherwise ignored like any other non-telemetry line.
* **A capture can contain several *sessions*, and every record says which one it
  belongs to.** The firmware writes a ``Boot`` event once per boot
  (``VehicleRuntime::begin()``); a second ``Boot`` in one capture is a reboot,
  and its ``millis()`` reading restarts. Records before the first boot are
  session 0 (a capture that began mid-session), and each ``Boot`` opens the next
  session, which the ``Boot`` record itself belongs to. A backwards timestamp
  *within* one session is a uint32 wrap; a backwards timestamp across a session
  boundary is a restart, and it is never reconstructed as 49.7 days of elapsed
  time. Each record carries its ``session_index`` so a consumer can refuse to
  compare two sessions against each other.
* **Time is unwrapped, per lane and per session.** ``t_ms`` is a raw
  ``millis()`` reading, so it wraps every ~49.7 days. Three different things
  produce a backwards raw reading, and they are *not* all rollovers (see
  :func:`classify_backwards_step`): a few-millisecond step is a *reordered*
  reading and stays in the current epoch with its signed (negative) delta, so
  ``1000 -> 999 -> 1020`` keeps the timeline ``1000, 999, 1020``; only a
  credible step across the top of the uint32 range - a genuine *wrap* -
  advances the epoch by 2**32, using the firmware's own unsigned-difference
  arithmetic so a duration computed here cannot disagree with the firmware
  because of a rollover. A backwards step that is neither is
  *restart_suspected*: the timeline is still reconstructed monotonically with
  the unsigned difference (so ordering invariants hold), but the step is named
  in ``wrap_observations`` and every elapsed metric across it is reported as
  unavailable rather than as ~49.7 days that did not happen. The two record
  kinds are unwrapped independently: the event lane drains before the sample
  lane, so an event can legitimately appear *before* an older sample in the
  byte stream, and a single shared counter would turn that legal interleaving
  into a bogus timestamp jump. ``t_abs_ms`` is therefore a *per-lane,
  per-session* timeline: durations within one lane and one session are exact,
  and a boundary seen by both lanes is compared by its raw ``t_ms`` within the
  same session (which is the same clock reading) rather than by two
  independently anchored timelines.
"""

from __future__ import annotations

import dataclasses
import hashlib
import math
import re
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Optional, Sequence, Union

from . import schema

__all__ = [
    "TelemetryFormatError",
    "SchemaVersionError",
    "FieldShapeError",
    "FieldValueError",
    "Sample",
    "Event",
    "Record",
    "LineError",
    "SessionInfo",
    "Log",
    "parse_line",
    "parse_lines",
    "parse_text",
    "read_log",
]

# The explicit provenance grammar a capture header may declare (M13).
#
# A declaration is a ``#`` comment line whose first token after the ``#`` -
# delimited by whitespace or a comma, matched case-insensitively - is exactly
# one of these two words. ``SYNTHETIC`` (``synthetic.SYNTHETIC_MARKER`` mirrors
# it) says the capture is a generated fixture rather than a recorded session;
# ``CAPTURE`` says the capture claims to be a recorded session (so ``# capture
# exported by student`` IS an explicit CAPTURE declaration - and a later
# ``SYNTHETIC`` still overrules it). Every other ``#`` line is an ordinary
# comment and declares nothing: prose (``# I am not using synthetic test data
# today``) is not provenance, and a declaration word buried inside prose is
# not a declaration either. It is detected from the capture's
# own bytes, so renaming or copying a fixture cannot erase its provenance: a
# name may add a warning, never remove one.
SYNTHETIC_HEADER_MARKER = "SYNTHETIC"
CAPTURE_HEADER_MARKER = "CAPTURE"
_EXPLICIT_DECLARATIONS = frozenset({SYNTHETIC_HEADER_MARKER, CAPTURE_HEADER_MARKER})

_UINT_RE = re.compile(r"^[0-9]+$")
_TOKEN_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
_PREFIX_RE = re.compile(r"^EV([SE])([0-9]+)$")
# The firmware writes a fixed-point value as an optional sign, at least one
# integer digit, and exactly `kTelemetryFieldDecimals` zero-padded fraction
# digits (`BoundedText::appendFixed`). Accepting a looser spelling - `1e5`, `1.5`,
# `.5` - would let a line this project never produced parse as if it had.
_FIXED_RE = re.compile(r"^-?[0-9]+\.[0-9]{%d}$" % schema.FIELD_DECIMALS)

_UINT32_SPAN = 1 << 32


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class TelemetryFormatError(ValueError):
    """A line that claims to be telemetry does not match the frozen schema.

    The message is written to be enough on its own: it names the source, the
    line number, what was wrong, and what the schema expected. That is the
    "fails clearly" contract - a corrupt line must never be quietly dropped,
    because a reader who loses one record cannot tell whether the vehicle did
    something surprising or the tool did.
    """

    def __init__(
        self,
        reason: str,
        *,
        source: Optional[str] = None,
        line_number: Optional[int] = None,
        raw: Optional[str] = None,
        expected: Optional[str] = None,
    ) -> None:
        self.reason = reason
        self.source = source
        self.line_number = line_number
        self.raw = raw
        self.expected = expected
        super().__init__(self._compose())

    @property
    def location(self) -> str:
        where = self.source or "<stream>"
        if self.line_number is not None:
            return f"{where}:{self.line_number}"
        return where

    def _compose(self) -> str:
        parts = [f"{self.location}: {self.reason}"]
        if self.raw is not None:
            shown = self.raw.rstrip("\r\n")
            if len(shown) > 200:
                shown = shown[:197] + "..."
            parts.append(f"  line: {shown}")
        if self.expected is not None:
            parts.append(f"  expected: {self.expected}")
        return "\n".join(parts)


class SchemaVersionError(TelemetryFormatError):
    """The record declares a schema version this tool does not understand."""


class FieldShapeError(TelemetryFormatError):
    """The field set, field order or spelling is not the one the schema froze."""


class FieldValueError(TelemetryFormatError):
    """A field carries a value that is not the kind of value that field holds."""


# ---------------------------------------------------------------------------
# Records
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Sample:
    """One control tick, as ``EVS<v>`` describes it.

    Fixed-point fields are ``float`` values, except that the ``ovf`` sentinel
    parses to ``None``: ``ovf`` means "finite but beyond the formatter's range",
    which is a bug or uninitialised data rather than a number, and representing
    it as ``inf`` would let it enter a mean as if it were a large measurement.
    """

    line_number: int
    order: int
    t_ms: int
    t_abs_ms: int
    dt_ms: int
    phase: str
    command: str
    armed: bool
    # The run's own timeline, as the firmware records it: raw `millis()` readings
    # with an availability flag each. `run_start_ms` is the actuation reading;
    # `dwell_start_ms` is the start of the final stop's verification interval (the
    # candidate, or the interval that satisfied the rule); `stop_verified_ms` is
    # the reading the verification completed at. The internal Run Time is the
    # unsigned difference of the first two - see `metrics._run_window()` - and is
    # not the event's official time.
    run_start_ms: int
    run_start_ok: bool
    dwell_start_ms: int
    dwell_start_ok: bool
    stop_verified_ms: int
    stop_verified_ok: bool
    position_m: Optional[float]
    position_ok: bool
    velocity_mps: Optional[float]
    velocity_ok: bool
    velocity_cmd_mps: Optional[float]
    remaining_m: Optional[float]
    motor: str
    effort_permille: int
    battery_counts: int
    current_counts: int
    # Whether the vehicle has an external nFAULT line at all (`nfault`), and the
    # level when it does. When the line is not installed the level field carries
    # the missing marker and parses to ``None``: a log may not spell "no signal"
    # as a healthy level, so this is deliberately not a bool.
    driver_fault_capability: str
    driver_fault_bar_low: Optional[bool]
    fault: str
    authz: str
    check: str
    drop_samples: int
    drop_events: int
    # Which firmware session this record belongs to (0 for a capture that began
    # mid-session; each `Boot` event opens the next). Two records with different
    # `session_index` values were written by different boots, and their `t_ms`
    # readings are not comparable - the clock restarted between them.
    session_index: int = 0

    @property
    def t_abs_s(self) -> float:
        return self.t_abs_ms / 1000.0

    @property
    def is_usable_position(self) -> bool:
        return self.position_ok and self.position_m is not None and math.isfinite(self.position_m)

    @property
    def is_usable_velocity(self) -> bool:
        return self.velocity_ok and self.velocity_mps is not None and math.isfinite(self.velocity_mps)


@dataclass(frozen=True)
class Event:
    """A transition or an operator action, as ``EVE<v>`` describes it."""

    line_number: int
    order: int
    t_ms: int
    t_abs_ms: int
    phase: str
    armed: bool
    event: str
    detail: str
    fault: str
    drop_samples: int
    drop_events: int
    # See `Sample.session_index`.
    session_index: int = 0

    @property
    def t_abs_s(self) -> float:
        return self.t_abs_ms / 1000.0


Record = Union[Sample, Event]


@dataclass(frozen=True)
class SessionInfo:
    """One firmware session in a capture: the records between boot boundaries.

    ``opened_by_boot`` is true when a ``Boot`` event opened this session, which is
    what makes a backwards timestamp inside it a *wrap* rather than a restart.
    ``boot_t_ms`` is that event's raw reading (the firmware writes it at
    ``millis()`` 0). Session 0 is a capture that began mid-session; it has no
    boot record and is reported as such rather than being treated as a session
    with an unknown start.
    """

    index: int
    opened_by_boot: bool
    boot_t_ms: Optional[int]
    sample_count: int
    event_count: int
    first_t_abs_ms: Optional[int]
    last_t_abs_ms: Optional[int]


@dataclass(frozen=True)
class LineError:
    """A line that failed to parse, kept when the caller asked to continue."""

    line_number: int
    message: str


# ---------------------------------------------------------------------------
# Single-line parsing
# ---------------------------------------------------------------------------


def _parse_unsigned(field_name: str, value: str, **where: object) -> int:
    if not _UINT_RE.match(value):
        raise FieldValueError(
            f"field '{field_name}' is not a plain unsigned decimal: {value!r}",
            expected=f"0..{schema.UNSIGNED_FIELDS[field_name]}",
            **where,  # type: ignore[arg-type]
        )
    number = int(value)
    widest = schema.UNSIGNED_FIELDS[field_name]
    if number > widest:
        raise FieldValueError(
            f"field '{field_name}' is {number}, wider than its firmware type holds",
            expected=f"0..{widest}",
            **where,  # type: ignore[arg-type]
        )
    return number


def _parse_bool(field_name: str, value: str, **where: object) -> bool:
    if value == "0":
        return False
    if value == "1":
        return True
    raise FieldValueError(
        f"field '{field_name}' is not the schema's single-character boolean: {value!r}",
        expected="0 or 1",
        **where,  # type: ignore[arg-type]
    )


def _parse_fixed(field_name: str, value: str, **where: object) -> Optional[float]:
    """A fixed-point field, or the sentinel it carries instead of a number."""
    if value == schema.SENTINEL_OVF:
        return None
    if value == schema.SENTINEL_NAN:
        return float("nan")
    if value == schema.SENTINEL_INF:
        return float("inf")
    if value == schema.SENTINEL_NEG_INF:
        return float("-inf")
    if value == schema.MISSING_FIELD:
        # The schema says a sample writes every field, so the marker should not
        # appear here. Accepting it as "unknown" is strictly less dangerous than
        # accepting it as a number, and it cannot hide a corruption that any
        # other check would catch.
        return None
    if not _FIXED_RE.match(value):
        raise FieldValueError(
            f"field '{field_name}' is not a number in the schema's spelling: {value!r}",
            expected=(
                f"a decimal with exactly {schema.FIELD_DECIMALS} fraction digits, or one of "
                f"{schema.SENTINEL_NAN}/{schema.SENTINEL_INF}/{schema.SENTINEL_NEG_INF}/"
                f"{schema.SENTINEL_OVF}/{schema.MISSING_FIELD}"
            ),
            **where,  # type: ignore[arg-type]
        )
    return float(value)


def _parse_enum(field_name: str, value: str, **where: object) -> str:
    vocabulary = schema.ENUM_FIELDS[field_name]
    if value not in vocabulary:
        raise FieldValueError(
            f"field '{field_name}' carries {value!r}, which is not in the schema's vocabulary",
            expected="one of " + ", ".join(vocabulary),
            **where,  # type: ignore[arg-type]
        )
    return value


def _parse_driver_fault_level(value: str, **where: object) -> Optional[bool]:
    """The nFAULT level field: a boolean, or absent when the line is not installed.

    The missing marker is accepted *only* as "no line exists" - it is not a third
    truth value about a wired line. The caller pairs this with `nfault`, and
    :func:`parse_line` refuses a sample whose two fields disagree, so a log can
    never claim an installed line with no level or an absent line with one.
    """
    if value == schema.MISSING_FIELD:
        return None
    return _parse_bool(schema.DRIVER_FAULT_LEVEL_FIELD, value, **where)  # type: ignore[arg-type]


def _parse_detail(value: str, **where: object) -> str:
    if value == schema.MISSING_FIELD:
        return schema.MISSING_FIELD
    if not _TOKEN_RE.match(value) or len(value) > schema.MAX_VOCABULARY_NAME_CHARS:
        raise FieldValueError(
            f"field 'detail' is not a vocabulary name or the missing marker: {value!r}",
            expected=(
                f"'{schema.MISSING_FIELD}' or an identifier of at most "
                f"{schema.MAX_VOCABULARY_NAME_CHARS} characters"
            ),
            **where,  # type: ignore[arg-type]
        )
    return value


def _split_fields(
    tokens: Sequence[str], expected_fields: Sequence[str], **where: object
) -> dict[str, str]:
    """Checks the field set and order, then returns the values by name."""
    keys: list[str] = []
    values: dict[str, str] = {}
    for token in tokens:
        key, separator, value = token.partition("=")
        if separator != "=" or key == "":
            raise FieldShapeError(
                f"field token {token!r} is not 'name=value'",
                expected="every field is written as name=value",
                **where,  # type: ignore[arg-type]
            )
        keys.append(key)
        values[key] = value

    actual = tuple(keys)
    if actual == tuple(expected_fields):
        return values

    missing = [name for name in expected_fields if name not in values]
    unexpected = [name for name in keys if name not in expected_fields]
    if missing or unexpected:
        detail = []
        if missing:
            detail.append("missing " + ", ".join(missing))
        if unexpected:
            detail.append("unexpected " + ", ".join(unexpected))
        raise FieldShapeError(
            "the record's field set is not the schema's (" + "; ".join(detail) + ")",
            expected=",".join(expected_fields),
            **where,  # type: ignore[arg-type]
        )

    raise FieldShapeError(
        "the record's fields are not in the schema's order",
        expected=",".join(expected_fields),
        **where,  # type: ignore[arg-type]
    )


def parse_line(
    raw: str,
    *,
    line_number: int = 0,
    order: int = 0,
    source: Optional[str] = None,
) -> Optional[Record]:
    """Parses one line.

    Returns the record, or ``None`` when the line is not telemetry (blank, an
    operator response, a ``BOOT,...`` line, monitor noise). Raises
    :class:`TelemetryFormatError` - specifically a :class:`SchemaVersionError`,
    :class:`FieldShapeError` or :class:`FieldValueError` - when the line claims
    to be telemetry and is not the telemetry this tool understands.

    ``t_abs_ms`` equals ``t_ms`` here; :func:`parse_lines` unwraps it against the
    rest of the stream.
    """
    where = {"source": source, "line_number": line_number, "raw": raw}
    text = raw.rstrip("\r\n")

    if text == "" or not text.startswith(schema.TELEMETRY_ROOT):
        return None

    tokens = text.split(",")
    prefix = tokens[0]
    match = _PREFIX_RE.match(prefix)
    if match is None:
        raise SchemaVersionError(
            f"line begins with the telemetry root '{schema.TELEMETRY_ROOT}' but its first "
            f"token {prefix!r} is not a record prefix",
            expected=f"{schema.SAMPLE_PREFIX},... or {schema.EVENT_PREFIX},...",
            **where,  # type: ignore[arg-type]
        )

    kind, declared_version = match.group(1), int(match.group(2))
    if declared_version != schema.SCHEMA_VERSION:
        raise SchemaVersionError(
            f"the record declares schema version {declared_version}, and this tool "
            f"understands version {schema.SCHEMA_VERSION}",
            expected=f"a log written by schema version {schema.SCHEMA_VERSION}",
            **where,  # type: ignore[arg-type]
        )

    expected_prefix = schema.SAMPLE_PREFIX if kind == "S" else schema.EVENT_PREFIX
    if prefix != expected_prefix:
        raise SchemaVersionError(
            f"the record's prefix {prefix!r} does not match its kind",
            expected=expected_prefix,
            **where,  # type: ignore[arg-type]
        )

    expected_fields = schema.SAMPLE_FIELDS if kind == "S" else schema.EVENT_FIELDS
    values = _split_fields(tokens[1:], expected_fields, **where)

    # The version appears twice - in the prefix's digit and in the `ver` field -
    # and the schema's own test pins that they agree. A record where they do not
    # is a record whose self-description is inconsistent, so it is refused rather
    # than resolved by preferring one.
    version_field = _parse_unsigned("ver", values["ver"], **where)
    if version_field != declared_version:
        raise SchemaVersionError(
            f"the prefix says schema version {declared_version} and 'ver' says {version_field}",
            expected="the prefix digit and the 'ver' field to agree",
            **where,  # type: ignore[arg-type]
        )

    t_ms = _parse_unsigned(schema.TIME_FIELD, values[schema.TIME_FIELD], **where)
    drop_samples = _parse_unsigned("drop_s", values["drop_s"], **where)
    drop_events = _parse_unsigned("drop_e", values["drop_e"], **where)
    phase = _parse_enum("phase", values["phase"], **where)
    armed = _parse_bool("armed", values["armed"], **where)
    fault = _parse_enum("fault", values["fault"], **where)

    if kind == "S":
        driver_fault_capability = _parse_enum("nfault", values["nfault"], **where)
        driver_fault_bar_low = _parse_driver_fault_level(values["nfault_low"], **where)
        # The two fields describe one fact and must agree. A record that says the
        # line is not installed *and* carries a level is claiming a reading from a
        # signal it says does not exist; a record that says it is installed with
        # no level is a gap where the schema says a sample has every field. Both
        # are refused rather than resolved by preferring one field.
        if driver_fault_capability == "NotInstalled" and driver_fault_bar_low is not None:
            raise FieldValueError(
                "the record says the external nFAULT line is not installed but carries a level",
                expected=f"'{schema.MISSING_FIELD}' in '{schema.DRIVER_FAULT_LEVEL_FIELD}' "
                "when 'nfault' is NotInstalled",
                **where,  # type: ignore[arg-type]
            )
        if driver_fault_capability == "Installed" and driver_fault_bar_low is None:
            raise FieldValueError(
                "the record says the external nFAULT line is installed but carries no level",
                expected=f"'0' or '1' in '{schema.DRIVER_FAULT_LEVEL_FIELD}' "
                "when 'nfault' is Installed",
                **where,  # type: ignore[arg-type]
            )
        return Sample(
            line_number=line_number,
            order=order,
            t_ms=t_ms,
            t_abs_ms=t_ms,
            dt_ms=_parse_unsigned(schema.DT_FIELD, values[schema.DT_FIELD], **where),
            phase=phase,
            command=_parse_enum("cmd", values["cmd"], **where),
            armed=armed,
            run_start_ms=_parse_unsigned("run_start_ms", values["run_start_ms"], **where),
            run_start_ok=_parse_bool("run_start_ok", values["run_start_ok"], **where),
            dwell_start_ms=_parse_unsigned("dwell_start_ms", values["dwell_start_ms"], **where),
            dwell_start_ok=_parse_bool("dwell_start_ok", values["dwell_start_ok"], **where),
            stop_verified_ms=_parse_unsigned(
                "stop_verified_ms", values["stop_verified_ms"], **where
            ),
            stop_verified_ok=_parse_bool(
                "stop_verified_ok", values["stop_verified_ok"], **where
            ),
            position_m=_parse_fixed("pos_m", values["pos_m"], **where),
            position_ok=_parse_bool("pos_ok", values["pos_ok"], **where),
            velocity_mps=_parse_fixed("vel_mps", values["vel_mps"], **where),
            velocity_ok=_parse_bool("vel_ok", values["vel_ok"], **where),
            velocity_cmd_mps=_parse_fixed("vel_cmd_mps", values["vel_cmd_mps"], **where),
            remaining_m=_parse_fixed("rem_m", values["rem_m"], **where),
            motor=_parse_enum("motor", values["motor"], **where),
            effort_permille=_parse_unsigned(
                "effort_permille", values["effort_permille"], **where
            ),
            battery_counts=_parse_unsigned("batt_counts", values["batt_counts"], **where),
            current_counts=_parse_unsigned("curr_counts", values["curr_counts"], **where),
            driver_fault_capability=driver_fault_capability,
            driver_fault_bar_low=driver_fault_bar_low,
            fault=fault,
            authz=_parse_enum("authz", values["authz"], **where),
            check=_parse_enum("check", values["check"], **where),
            drop_samples=drop_samples,
            drop_events=drop_events,
        )

    return Event(
        line_number=line_number,
        order=order,
        t_ms=t_ms,
        t_abs_ms=t_ms,
        phase=phase,
        armed=armed,
        event=_parse_enum("event", values["event"], **where),
        detail=_parse_detail(values[schema.DETAIL_FIELD], **where),
        fault=fault,
        drop_samples=drop_samples,
        drop_events=drop_events,
    )


# ---------------------------------------------------------------------------
# Stream parsing
# ---------------------------------------------------------------------------


@dataclass
class Log:
    """One captured stream, parsed.

    ``errors`` is empty unless the caller asked to continue past a bad line. A
    strict parse of a corrupt log therefore raises instead of returning a
    partial :class:`Log`, and a permissive parse keeps every failure so a report
    can state exactly which lines were unreadable.
    """

    source: str
    samples: list[Sample] = field(default_factory=list)
    events: list[Event] = field(default_factory=list)
    ignored_lines: list[int] = field(default_factory=list)
    errors: list[LineError] = field(default_factory=list)
    total_lines: int = 0
    sha256: Optional[str] = None
    # The capture's own provenance declaration, from its explicit ``#`` header
    # declarations (``SYNTHETIC`` or ``CAPTURE`` under the grammar above), or
    # ``None`` when it declared nothing. A ``SYNTHETIC`` declaration anywhere in
    # the capture wins over any ``CAPTURE`` declaration regardless of order, so
    # an ordinary comment can neither suppress nor invent it. It travels with
    # the parsed records so no report can drop it by looking only at a filename.
    declared_provenance: Optional[str] = None
    # True when the capture explicitly declared both ``SYNTHETIC`` and
    # ``CAPTURE`` (in either order). The classification stays conservative
    # (synthetic) and the report says the conflict existed rather than hiding
    # one of the declarations.
    provenance_conflict: bool = False
    # Every backwards step observed inside one lane of one session, as
    # ``{"line_number", "lane", "session_index", "from_ms", "to_ms", "delta_ms"}``.
    # A backwards step within a session is the only wrap this tool reconstructs;
    # the observation is kept so a report can state that it happened rather than
    # leaving a 49.7-day jump implicit.
    wrap_observations: list[dict] = field(default_factory=list)

    @property
    def session_count(self) -> int:
        return len(self.sessions)

    @property
    def sessions(self) -> list[SessionInfo]:
        return _summarize_sessions(self.samples, self.events)

    @property
    def records(self) -> list[Record]:
        """Both lanes, in the order they arrived in the byte stream."""
        merged: list[Record] = [*self.samples, *self.events]
        merged.sort(key=lambda record: record.order)
        return merged

    @property
    def first_t_abs_ms(self) -> Optional[int]:
        stamps = [record.t_abs_ms for record in self.records]
        return min(stamps) if stamps else None

    @property
    def last_t_abs_ms(self) -> Optional[int]:
        stamps = [record.t_abs_ms for record in self.records]
        return max(stamps) if stamps else None

    @property
    def is_empty(self) -> bool:
        return not self.samples and not self.events


def _unwrap(
    previous_ms: Optional[int],
    previous_abs: int,
    t_ms: int,
    *,
    kind: Optional[str] = None,
) -> int:
    """Time reconstruction for one lane, given the step's classification.

    A ``reordered`` step (a few milliseconds backwards) stays in the current
    epoch with its signed delta: treating it as a rollover would invent a new
    2**32 ms epoch for what was a 1 ms re-reading of the clock. A genuine
    ``wrap`` - and, monotonically, a ``restart_suspected`` step - uses the
    unsigned difference, which is the same rule `SOFTWARE_ARCHITECTURE.md`
    section 4 fixes for the firmware, so a duration computed here cannot
    disagree with the firmware's own across a real rollover. A restart step
    keeps the timeline ordered but is *not* trusted: consumers must refuse
    elapsed durations across it (see ``metrics.compute_metrics``).
    """
    if previous_ms is None:
        return t_ms
    if kind == "reordered":
        return previous_abs + (t_ms - previous_ms)
    delta = (t_ms - previous_ms) % _UINT32_SPAN
    return previous_abs + delta


# The event name that opens a session (`TelemetryEventCode::Boot`).
_BOOT_EVENT = "Boot"


def _declared_provenance(raw: str) -> Optional[str]:
    """The explicit provenance a capture line declares, if any.

    Only the grammar named at ``SYNTHETIC_HEADER_MARKER`` counts: a ``#`` line
    (after optional leading whitespace) whose first token after the ``#`` -
    delimited by whitespace or a comma, matched case-insensitively - is exactly
    ``SYNTHETIC`` or ``CAPTURE``. Anything else (``# exported by student``,
    ``# I am not using synthetic test data today``, ``# run_id: ...``) is an
    ordinary comment and returns ``None``. The returned value is
    the normalized word (``SYNTHETIC`` or ``CAPTURE``), never the raw prose.
    """
    text = raw.lstrip()
    if not text.startswith("#"):
        return None
    body = text[1:].strip()
    token = body.split(",", 1)[0].split(None, 1)[0].strip().upper() if body else ""
    if token in _EXPLICIT_DECLARATIONS:
        return token
    return None


# How close to the top of the uint32 range a reading has to be for a rollover to
# the bottom to be read as a *wrap* rather than something else. `millis()` counts
# milliseconds, so 120 s is a wide margin around the ~49.7-day boundary: the
# firmware's own control cadence is tens of milliseconds and a capture is seconds
# to minutes long, so a reading this near the top followed by one this near the
# bottom is a rollover.
_WRAP_WINDOW_MS = 120_000

# A backwards step smaller than this is the firmware reading `millis()` again
# between the moment it stamped one record and the next: the two stamps are not
# ordered, but no clock restarted.
_REORDER_WINDOW_MS = 1_000


def _late_pre_wrap_abs(
    previous_ms: Optional[int], previous_abs: int, t_ms: int
) -> Optional[int]:
    """The reconstructed time of a late pre-wrap record, or ``None``.

    A lane that rolled over (``U-2 -> 1``) can then receive a slightly
    out-of-order record from just before the rollover (``-> U-1``). In raw
    value that step runs *forward* by almost the whole uint32 range, but it
    is really the clock being re-read a couple of milliseconds late: the
    record belongs ``(previous_ms - t_ms) mod 2**32`` milliseconds before the
    post-wrap time, in the previous epoch. Anything smaller than a near-full-
    range forward jump is an ordinary step and returns ``None``.
    """
    if previous_ms is None or t_ms <= previous_ms:
        return None
    if t_ms - previous_ms <= _UINT32_SPAN - _REORDER_WINDOW_MS:
        return None
    return previous_abs - ((previous_ms - t_ms) % _UINT32_SPAN)


def classify_backwards_step(previous_ms: Optional[int], t_ms: int) -> Optional[dict]:
    """Classifies a step whose raw reading is behind the previous one, or ``None``.

    Three different things produce a backwards raw reading, and only two of them
    can be reconstructed from the readings alone:

    * ``reordered`` - the step is a few milliseconds back. The firmware reads the
      clock again before it writes a record, so consecutive records can carry
      readings taken a moment apart, in either order. The interval between them is
      noise, not a clock event.
    * ``wrap`` - the readings sit either side of the top of the uint32 range (the
      previous one near the top, the next one near the bottom). This is the
      rollover a long-running board eventually produces, and its elapsed time is
      the unsigned difference.
    * ``restart_suspected`` - anything else: a large backwards step nowhere near
      the rollover point. A clock that restarted (a reset whose ``Boot`` line was
      not captured) looks exactly like this, and so does a corrupt reading. It can
      **not** be told apart here, so it is named and reported rather than being
      reconstructed as ~49.7 days of elapsed time - which is what treating every
      backwards reading as a wrap used to do.
    """
    if previous_ms is None or t_ms >= previous_ms:
        return None
    unsigned = (t_ms - previous_ms) % _UINT32_SPAN
    raw_back = previous_ms - t_ms
    if raw_back < _REORDER_WINDOW_MS:
        kind = "reordered"
    elif previous_ms > _UINT32_SPAN - _WRAP_WINDOW_MS and t_ms < _WRAP_WINDOW_MS:
        kind = "wrap"
    else:
        kind = "restart_suspected"
    return {
        "kind": kind,
        "from_ms": previous_ms,
        "to_ms": t_ms,
        "unsigned_delta_ms": unsigned,
        "raw_backwards_ms": raw_back,
    }


def _summarize_sessions(samples: list[Sample], events: list[Event]) -> list[SessionInfo]:
    """Groups already-unwrapped records into their firmware sessions."""
    boot_ms: dict[int, Optional[int]] = {}
    for event in events:
        if event.event == _BOOT_EVENT and event.session_index not in boot_ms:
            boot_ms[event.session_index] = event.t_ms

    buckets: dict[int, list[Union[Sample, Event]]] = {}
    for record in (*samples, *events):
        buckets.setdefault(record.session_index, []).append(record)

    sessions: list[SessionInfo] = []
    for index in sorted(buckets):
        records = buckets[index]
        stamps = [record.t_abs_ms for record in records]
        sessions.append(
            SessionInfo(
                index=index,
                opened_by_boot=index in boot_ms,
                boot_t_ms=boot_ms.get(index),
                sample_count=sum(1 for record in records if isinstance(record, Sample)),
                event_count=sum(1 for record in records if isinstance(record, Event)),
                first_t_abs_ms=min(stamps) if stamps else None,
                last_t_abs_ms=max(stamps) if stamps else None,
            )
        )
    return sessions


def parse_lines(
    lines: Iterable[str],
    *,
    source: Optional[str] = None,
    continue_on_error: bool = False,
) -> Log:
    """Parses a whole stream.

    With ``continue_on_error=False`` (the default) the first line that claims to
    be telemetry and does not parse raises. With ``continue_on_error=True`` the
    failure is recorded in ``Log.errors`` and parsing continues, which is for
    surveying a damaged capture - never for producing a metrics report that
    would read as if the log were complete.

    ``Boot`` events open sessions. A capture written by one firmware run has one
    session; a capture that spans a reset has more, and each record's
    ``session_index`` says which one it belongs to so a consumer can refuse to
    compare across the restart instead of reading it as 49.7 days.
    """
    log = Log(source=source or "<stream>")
    sample_previous_ms: Optional[int] = None
    sample_previous_abs = 0
    sample_backfilled = False
    event_previous_ms: Optional[int] = None
    event_previous_abs = 0
    event_backfilled = False
    session_index = 0
    session_records = 0
    provenance: Optional[str] = None
    saw_synthetic = False
    saw_capture = False

    for index, raw in enumerate(lines, start=1):
        log.total_lines = index
        try:
            record = parse_line(raw, line_number=index, order=index, source=source)
        except TelemetryFormatError as error:
            if not continue_on_error:
                raise
            log.errors.append(LineError(line_number=index, message=str(error)))
            continue

        if record is None:
            declared = _declared_provenance(raw)
            if declared == SYNTHETIC_HEADER_MARKER:
                # Irreversible: a later ordinary comment or CAPTURE claim can
                # never undo it, and an earlier one can never have blocked it.
                saw_synthetic = True
                provenance = declared
            elif declared == CAPTURE_HEADER_MARKER:
                saw_capture = True
                if provenance is None:
                    provenance = declared
            log.ignored_lines.append(index)
            continue

        if isinstance(record, Sample):
            previous_ms = sample_previous_ms
            backfill = _late_pre_wrap_abs(previous_ms, sample_previous_abs, record.t_ms)
            step = classify_backwards_step(previous_ms, record.t_ms)
            if (
                backfill is None
                and sample_backfilled
                and previous_ms is not None
                and step is not None
                and step["kind"] == "wrap"
            ):
                # The rollover this step resembles is the one already counted
                # before the backfilled record (R1R-M3b, e.g. U-2 -> 1 -> U-1
                # -> 20): the lane is already in the new epoch, so the
                # unsigned delta is the honest one and no second wrap is
                # observed. The flag is consumed by this one step.
                record = dataclasses.replace(
                    record,
                    t_abs_ms=sample_previous_abs + step["unsigned_delta_ms"],
                    session_index=session_index,
                )
                sample_previous_ms = record.t_ms
                sample_previous_abs = record.t_abs_ms
                sample_backfilled = False
                log.samples.append(record)
                session_records += 1
                continue
            sample_backfilled = False
            if backfill is not None:
                # A slightly out-of-order pre-wrap record arriving after the
                # lane already rolled over (R1R-M3b, e.g. U-2 -> 1 -> U-1):
                # reading it as a +49.7-day forward step fabricates a second
                # wrap epoch. It belongs a few milliseconds before the
                # post-wrap clock, like any reordered reading.
                log.wrap_observations.append(
                    {
                        "line_number": index,
                        "lane": "sample",
                        "session_index": session_index,
                        "kind": "reordered",
                        "from_ms": previous_ms,
                        "to_ms": record.t_ms,
                        "unsigned_delta_ms": (record.t_ms - previous_ms) % _UINT32_SPAN,  # type: ignore[operator]
                        "raw_backwards_ms": (previous_ms - record.t_ms) % _UINT32_SPAN,  # type: ignore[operator]
                    }
                )
                record = dataclasses.replace(
                    record,
                    t_abs_ms=backfill,
                    session_index=session_index,
                )
                sample_backfilled = True
            else:
                if step is not None:
                    log.wrap_observations.append(
                        {
                            "line_number": index,
                            "lane": "sample",
                            "session_index": session_index,
                            **step,
                        }
                    )
                record = dataclasses.replace(
                    record,
                    t_abs_ms=_unwrap(
                        previous_ms,
                        sample_previous_abs,
                        record.t_ms,
                        kind=step["kind"] if step is not None else None,
                    ),
                    session_index=session_index,
                )
            sample_previous_ms = record.t_ms
            sample_previous_abs = record.t_abs_ms
            log.samples.append(record)
            session_records += 1
        else:
            # The Boot event opens a session and belongs to it: the records after
            # it are the new boot's, so both lanes' unwrap chains restart here. A
            # capture whose first record is the Boot event is session 0; a Boot
            # seen after any record of the current session opens the next one.
            if record.event == _BOOT_EVENT:
                if session_records:
                    session_index += 1
                sample_previous_ms = None
                sample_previous_abs = 0
                sample_backfilled = False
                event_previous_ms = None
                event_previous_abs = 0
                event_backfilled = False
            previous_ms = event_previous_ms
            backfill = _late_pre_wrap_abs(previous_ms, event_previous_abs, record.t_ms)
            step = classify_backwards_step(previous_ms, record.t_ms)
            if (
                backfill is None
                and event_backfilled
                and previous_ms is not None
                and step is not None
                and step["kind"] == "wrap"
            ):
                # Same already-counted rollover as in the sample lane (R1R-M3b).
                record = dataclasses.replace(
                    record,
                    t_abs_ms=event_previous_abs + step["unsigned_delta_ms"],
                    session_index=session_index,
                )
                event_previous_ms = record.t_ms
                event_previous_abs = record.t_abs_ms
                event_backfilled = False
                log.events.append(record)
                session_records += 1
                continue
            event_backfilled = False
            if backfill is not None:
                log.wrap_observations.append(
                    {
                        "line_number": index,
                        "lane": "event",
                        "session_index": session_index,
                        "kind": "reordered",
                        "from_ms": previous_ms,
                        "to_ms": record.t_ms,
                        "unsigned_delta_ms": (record.t_ms - previous_ms) % _UINT32_SPAN,  # type: ignore[operator]
                        "raw_backwards_ms": (previous_ms - record.t_ms) % _UINT32_SPAN,  # type: ignore[operator]
                    }
                )
                record = dataclasses.replace(
                    record,
                    t_abs_ms=backfill,
                    session_index=session_index,
                )
                event_backfilled = True
            else:
                if step is not None:
                    log.wrap_observations.append(
                        {
                            "line_number": index,
                            "lane": "event",
                            "session_index": session_index,
                            **step,
                        }
                    )
                record = dataclasses.replace(
                    record,
                    t_abs_ms=_unwrap(
                        previous_ms,
                        event_previous_abs,
                        record.t_ms,
                        kind=step["kind"] if step is not None else None,
                    ),
                    session_index=session_index,
                )
            event_previous_ms = record.t_ms
            event_previous_abs = record.t_abs_ms
            log.events.append(record)
            session_records += 1

    log.declared_provenance = provenance
    log.provenance_conflict = saw_synthetic and saw_capture
    return log


def parse_text(
    text: str,
    *,
    source: Optional[str] = None,
    continue_on_error: bool = False,
) -> Log:
    """Parses an already-decoded capture."""
    log = parse_lines(
        text.splitlines(), source=source, continue_on_error=continue_on_error
    )
    log.sha256 = hashlib.sha256(text.encode("utf-8", errors="surrogatepass")).hexdigest()
    return log


def read_log(
    path: str,
    *,
    continue_on_error: bool = False,
) -> Log:
    """Reads a capture from disk.

    Decoded as UTF-8 with ``errors="replace"``: a damaged byte becomes U+FFFD,
    which is not a valid value in any field, so a corrupted telemetry line still
    fails the schema checks instead of being silently transliterated into
    something plausible. The file's raw bytes are hashed so a report can be tied
    back to exactly the log it was computed from.
    """
    with open(path, "rb") as handle:
        raw_bytes = handle.read()
    text = raw_bytes.decode("utf-8", errors="replace")
    log = parse_lines(text.splitlines(), source=path, continue_on_error=continue_on_error)
    log.sha256 = hashlib.sha256(raw_bytes).hexdigest()
    return log
