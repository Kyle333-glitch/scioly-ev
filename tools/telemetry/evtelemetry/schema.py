"""The frozen telemetry schema, mirrored from the firmware.

This module is a hand-maintained copy of two firmware headers:

* ``lib/evcore/src/TelemetryRecord.h`` - the record kinds, the prefixes, the
  schema version, the missing-field marker and the wire spelling of every
  enumerated value;
* ``lib/evcore/src/TelemetryFormat.h`` - the field set of each record and the
  order those fields are written in.

Nothing here may be "improved" independently of the firmware. A field name, a
field's meaning or the spelling of a vocabulary entry is part of the schema: the
firmware's own comment says a rename "changes the schema and requires a version
bump". If this file and the firmware disagree, the analysis of an old log would
silently misread a column, which is the failure mode this whole schema exists to
prevent. ``tools/telemetry/tests/test_schema_mirror.py`` re-reads the firmware
headers and fails if this mirror has drifted.

Note what is deliberately *not* here: any conversion from raw counts to
engineering units. ``batt_counts`` and ``curr_counts`` are ADC counts in the
firmware, and ``SOFTWARE_ARCHITECTURE.md`` section 12 records that no
raw-to-engineering-unit relationship is documented. Inventing one in an analysis
tool would turn an unverified guess into something that looks like a
measurement, so the tool reports counts and nothing else.
"""

from __future__ import annotations

from typing import Final

# ---------------------------------------------------------------------------
# Prefixes, version and sentinels
# ---------------------------------------------------------------------------

# The schema version, from `ev::kTelemetrySchemaVersion`. A log whose version is
# not this one is refused rather than partially understood.
#
# Version 2 (D32): the driver-fault check was renamed, a sample gained the
# `nfault` capability field, and `nfault_low` stopped being a boolean - it now
# carries the missing marker when the vehicle has no external nFAULT line. A
# version-1 `nfault_low=0` meant "sampled and read high", which is a claim the
# assembled vehicle cannot make.
SCHEMA_VERSION: Final[int] = 2

# `EVS<version>` is one control tick; `EVE<version>` is a transition or an
# operator action. The digit is the version, so a record is self-describing.
SAMPLE_PREFIX: Final[str] = f"EVS{SCHEMA_VERSION}"
EVENT_PREFIX: Final[str] = f"EVE{SCHEMA_VERSION}"

# The `EV` root, from the convention `AGENTS.md` section 16 illustrates. Any line
# that begins with it is telemetry and must parse; anything else in a captured
# stream (an operator response, a `BOOT,...` line, the serial monitor's own
# noise) is not telemetry and is counted as ignored rather than rejected.
TELEMETRY_ROOT: Final[str] = "EV"

# A record missing a field carries this marker instead of a value
# (`ev::kTelemetryMissingField`).
MISSING_FIELD: Final[str] = "-"

# Fixed-point fields carry `kTelemetryFieldDecimals` decimals: 0.1 mm and
# 0.1 mm/s.
FIELD_DECIMALS: Final[int] = 4

# `BoundedText::appendFixed()` writes one of these instead of a number, so a bad
# value can never widen a record. `ovf` means "finite but at or beyond 1e7",
# i.e. a bug or uninitialised data rather than a measurement.
SENTINEL_NAN: Final[str] = "nan"
SENTINEL_INF: Final[str] = "inf"
SENTINEL_NEG_INF: Final[str] = "-inf"
SENTINEL_OVF: Final[str] = "ovf"

# The longest vocabulary name the firmware's own tests allow (`test_telemetry_format`).
MAX_VOCABULARY_NAME_CHARS: Final[int] = 30

# ---------------------------------------------------------------------------
# Field sets, in the order the formatter writes them
# ---------------------------------------------------------------------------

# `EVS<v>,ver=..,t_ms=..,dt_ms=..,phase=..,cmd=..,armed=..,pos_m=..,pos_ok=..,
#    vel_mps=..,vel_ok=..,vel_cmd_mps=..,rem_m=..,motor=..,effort_permille=..,
#    batt_counts=..,curr_counts=..,nfault=..,nfault_low=..,fault=..,authz=..,
#    check=..,drop_s=..,drop_e=..`
SAMPLE_FIELDS: Final[tuple[str, ...]] = (
    "ver",
    "t_ms",
    "dt_ms",
    "phase",
    "cmd",
    "armed",
    "pos_m",
    "pos_ok",
    "vel_mps",
    "vel_ok",
    "vel_cmd_mps",
    "rem_m",
    "motor",
    "effort_permille",
    "batt_counts",
    "curr_counts",
    "nfault",
    "nfault_low",
    "fault",
    "authz",
    "check",
    "drop_s",
    "drop_e",
)

# `EVE<v>,ver=..,t_ms=..,phase=..,armed=..,event=..,detail=..,fault=..,
#    drop_s=..,drop_e=..`
EVENT_FIELDS: Final[tuple[str, ...]] = (
    "ver",
    "t_ms",
    "phase",
    "armed",
    "event",
    "detail",
    "fault",
    "drop_s",
    "drop_e",
)

# ---------------------------------------------------------------------------
# Vocabularies (the wire spelling of every enumerated value)
# ---------------------------------------------------------------------------

# `telemetryPhaseName()`.
PHASES: Final[tuple[str, ...]] = (
    "Idle",
    "ParameterValid",
    "Armed",
    "OptionalDelay",
    "Approach",
    "Capture",
    "Push",
    "ForwardDecel",
    "BrakeZeroSpeed",
    "Reverse",
    "FinalApproach",
    "FinalBrake",
    "Done",
    "Fault",
)

# `telemetryRunCommandName()`.
COMMANDS: Final[tuple[str, ...]] = (
    "none",
    "DriveForward",
    "DriveReverse",
    "RampDownForward",
    "RampDownReverse",
    "BrakeForward",
    "BrakeReverse",
)

# `telemetryMotorModeName()`.
MOTOR_MODES: Final[tuple[str, ...]] = (
    "Disabled",
    "Coast",
    "Brake",
    "DriveForward",
    "DriveReverse",
)

# `telemetryFaultName()`.
FAULTS: Final[tuple[str, ...]] = (
    "None",
    "InvalidRunConfig",
    "ProvisionalCalibration",
    "MissingSafetyLimits",
    "InvalidMotorEffort",
    "PhaseTimeout",
    "ReversalNotVerified",
    "DisarmedDuringRun",
    "RunSequencingError",
    "DriverFaultBar",
)

# `telemetryAuthzOutcomeName()`.
AUTHZ_OUTCOMES: Final[tuple[str, ...]] = (
    "AuthorizedDrive",
    "AuthorizedClamped",
    "AuthorizedStop",
    "ForcedFaultStop",
    "DeniedNotArmed",
    "DeniedNotReady",
    "DeniedStaleData",
    "DeniedIllegalEffort",
    "DeniedUnsafeDirectionChange",
)

# `telemetryConfigErrorName()` (the rule-validation error of a rejected
# configuration).
CONFIG_ERRORS: Final[tuple[str, ...]] = (
    "None",
    "NonFiniteValue",
    "InvalidTournamentLevel",
    "TargetDistanceOutOfRange",
    "TargetDistanceOffInterval",
    "TargetTimeOutOfRange",
    "TargetTimeOffInterval",
    "BottleLineDistanceOutOfRange",
    "BottleLineDistanceOffInterval",
    "BottleStartNotBeforeTarget",
    "BottleStartNotLevelDistance",
)

# `TelemetryEventCode` names.
EVENT_CODES: Final[tuple[str, ...]] = (
    "Boot",
    "TelemetryEnabled",
    "PhaseChange",
    "Armed",
    "Disarmed",
    "FaultLatched",
    "AnnounceAccepted",
    "AnnounceRefused",
    "ArmRefused",
    "ChainReset",
    "ConfigSet",
    "ConfigCleared",
    "CommandRejected",
)

# `driverFaultCapabilityName()`: whether the vehicle has an external nFAULT line
# at all. `NotInstalled` is this vehicle's value.
DRIVER_FAULT_CAPABILITIES: Final[tuple[str, ...]] = (
    "Installed",
    "NotInstalled",
)

# `telemetryDriverFaultLevelName()`: a boolean when the line exists, and the
# missing marker when it does not. Deliberately not a boolean vocabulary - a log
# may not spell "no signal" as a healthy level.
DRIVER_FAULT_LEVELS: Final[tuple[str, ...]] = ("0", "1", MISSING_FIELD)

# `configurationCheckName()`, including the spelling of `Count` (which is not a
# check: it is how "no failing check" is written into the `check` field).
#
# `DriverFaultConfigurationValid` is the D32 rename of
# `DriverFaultPolarityVerified`: the check is now capability-aware, because this
# vehicle has no external nFAULT line to measure a polarity for.
CONFIG_CHECKS: Final[tuple[str, ...]] = (
    "ConfigurationValid",
    "CalibrationMeasured",
    "MeasuredValuesCiteEvidence",
    "EffortCeilingsAgree",
    "CommandPolarityAgrees",
    "TickPolarityAgrees",
    "DriverFaultConfigurationValid",
    "StopEvidenceSpansReversalStop",
    "StopEvidenceSpansFinalStop",
    "SafetyLimitsValid",
    "ControllerConfigValid",
    "EstimatorConfigValid",
    "ProfileLimitsValid",
    "PhaseConfigValid",
    "ReversalConfigValid",
    "RunPolicyUsable",
    "none",
)

# Every name that reaches a record, gathered for the drift check. The `check`
# field can carry any of `CONFIG_CHECKS`; the `detail` field of an event can
# carry a phase, fault or event name, or a refusal/error token.
def all_vocabulary_names() -> tuple[str, ...]:
    """Every vocabulary entry this module knows, sorted and de-duplicated."""
    return tuple(
        sorted(
            set(PHASES)
            | set(COMMANDS)
            | set(MOTOR_MODES)
            | set(FAULTS)
            | set(AUTHZ_OUTCOMES)
            | set(CONFIG_ERRORS)
            | set(EVENT_CODES)
            | set(CONFIG_CHECKS)
            | set(DRIVER_FAULT_CAPABILITIES)
            | set(DRIVER_FAULT_LEVELS)
        )
    )


# ---------------------------------------------------------------------------
# Value kinds
# ---------------------------------------------------------------------------

# Unsigned fields and the widest value their firmware type can hold. A value
# outside the range means the line is corrupt, not that the vehicle recorded
# something unusual.
UNSIGNED_FIELDS: Final[dict[str, int]] = {
    "ver": 0xFF,
    "t_ms": 0xFFFFFFFF,
    "dt_ms": 0xFFFF,
    "effort_permille": 0xFFFF,
    "batt_counts": 0xFFFF,
    "curr_counts": 0xFFFF,
    "drop_s": 0xFFFFFFFF,
    "drop_e": 0xFFFFFFFF,
}

# Written as a single character, `0` or `1` (`BoundedText::appendBool`).
BOOL_FIELDS: Final[frozenset[str]] = frozenset({"armed", "pos_ok", "vel_ok"})

# `appendFixed()` fields. `ovf` is a sentinel rather than a number, so these
# parse to either a float (possibly NaN or infinite) or `None` for `ovf`.
FIXED_FIELDS: Final[frozenset[str]] = frozenset({"pos_m", "vel_mps", "vel_cmd_mps", "rem_m"})

# Enumerated string fields and the vocabulary each one may draw from.
ENUM_FIELDS: Final[dict[str, tuple[str, ...]]] = {
    "phase": PHASES,
    "cmd": COMMANDS,
    "motor": MOTOR_MODES,
    "fault": FAULTS,
    "authz": AUTHZ_OUTCOMES,
    "check": CONFIG_CHECKS,
    "event": EVENT_CODES,
    "nfault": DRIVER_FAULT_CAPABILITIES,
}

# The driver-fault level field. Not a boolean vocabulary: `not_installed`-style
# absence is written as the missing marker, so a reader cannot read "no signal"
# as "healthy".
DRIVER_FAULT_LEVEL_FIELD: Final[str] = "nfault_low"

# `detail` is not validated against one single vocabulary because it legitimately
# carries a name from several of them (a fault name for `FaultLatched`, a phase
# name for `PhaseChange`, a check name for `ArmRefused`, a refusal token for
# `CommandRejected`, ...). What it must always be is one comma-free identifier
# token, or the missing marker: every spelling in every one of those tables is
# identifier-like (`OperatorPolicy.h`, `SerialCommandParser.h`), and `detail` is
# a `const char*` to a static string so it can never be built at run time. A
# `,`, a space or an `=` in it would mean the line is corrupt, and a value that
# is not identifier-like cannot be a name the firmware ever writes.
DETAIL_FIELD: Final[str] = "detail"

# The fields that carry the drop counters, in both record kinds.
DROP_FIELDS: Final[tuple[str, str]] = ("drop_s", "drop_e")

# The time field, in both record kinds: a `millis()` reading, in milliseconds.
TIME_FIELD: Final[str] = "t_ms"

# The interval the control tick was given, in milliseconds. Only a sample has it.
DT_FIELD: Final[str] = "dt_ms"
