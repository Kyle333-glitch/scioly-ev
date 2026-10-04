"""Parse a captured telemetry stream into records.

The firmware writes one line per record: ``EVS<v>,...`` for a control tick and
``EVE<v>,...`` for a transition or an operator action. This module turns those
lines into :class:`Sample` and :class:`Event` values, and refuses anything that
does not match the frozen schema.

Three rules shape the design, and all three exist so a reader cannot be misled
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
  is not required to be pure telemetry.
* **Time is unwrapped, per lane.** ``t_ms`` is a raw ``millis()`` reading, so it
  wraps every ~49.7 days, and the firmware's own arithmetic treats elapsed time
  as an unsigned difference. Records are unwrapped using exactly that arithmetic
  so a duration computed here cannot disagree with the firmware because of a
  wrap. The two record kinds are unwrapped independently: the event lane drains
  before the sample lane, so an event can legitimately appear *before* an older
  sample in the byte stream, and a single shared counter would turn that legal
  interleaving into a bogus timestamp jump.
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
    "Log",
    "parse_line",
    "parse_lines",
    "parse_text",
    "read_log",
]

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

    @property
    def t_abs_s(self) -> float:
        return self.t_abs_ms / 1000.0


Record = Union[Sample, Event]


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


def _unwrap(previous_ms: Optional[int], previous_abs: int, t_ms: int) -> int:
    """Unsigned-difference time reconstruction, matching the firmware's rule.

    Elapsed time is ``t_ms - previous_ms`` in ``uint32`` arithmetic, so a wrap is
    just a large unsigned delta rather than a negative one. This is the same
    rule `SOFTWARE_ARCHITECTURE.md` section 4 fixes for the firmware, which is
    why a duration computed here cannot disagree with the firmware's own.
    """
    if previous_ms is None:
        return t_ms
    delta = (t_ms - previous_ms) % _UINT32_SPAN
    return previous_abs + delta


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
    """
    log = Log(source=source or "<stream>")
    sample_previous_ms: Optional[int] = None
    sample_previous_abs = 0
    event_previous_ms: Optional[int] = None
    event_previous_abs = 0

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
            log.ignored_lines.append(index)
            continue

        if isinstance(record, Sample):
            record = dataclasses.replace(
                record,
                t_abs_ms=_unwrap(sample_previous_ms, sample_previous_abs, record.t_ms),
            )
            sample_previous_ms = record.t_ms
            sample_previous_abs = record.t_abs_ms
            log.samples.append(record)
        else:
            record = dataclasses.replace(
                record,
                t_abs_ms=_unwrap(event_previous_ms, event_previous_abs, record.t_ms),
            )
            event_previous_ms = record.t_ms
            event_previous_abs = record.t_abs_ms
            log.events.append(record)

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
