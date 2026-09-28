"""PC-side analysis tooling for the vehicle's telemetry stream.

The package parses the firmware's frozen telemetry schema, computes descriptive
metrics from a captured log, draws reproducible dependency-free SVG charts, and
compares a run's believed endpoints against measurements taken by hand.

The tool is a *consumer* of the telemetry contract, exactly as the firmware's own
serial transport is: it reads the schema defined in
``lib/evcore/src/TelemetryRecord.h`` and ``TelemetryFormat.h`` and never invents
a field, a unit or a conversion. It reaches no conclusion about the vehicle -
``VERIFIED_STATE.md`` and physical testing own that - and it never writes there.

The package is standard-library only. Nothing needs installing, so the tool runs
on the same clean Windows machine that builds the firmware, and its output has no
dependency that could change under it.
"""

from __future__ import annotations

# Bumped when the report's shape changes, so a report directory says which tool
# produced it. It is not a date and not a schema version.
#
# 2: the measurement CSV gained acceptance/exclusion/pack/target context columns,
#    endpoints.json carries them, and `distribution` writes a series report.
TOOL_VERSION = "2"

__all__ = ["TOOL_VERSION"]
