#pragma once

#include <cstdint>

#include "BoundedText.h"
#include "TelemetryRecord.h"

namespace ev {

// ---------------------------------------------------------------------------
// The telemetry formatter - one record, one line, or nothing
// ---------------------------------------------------------------------------
//
// With `TelemetryRecord.h` this completes the wire format: the schema owns *what*
// a record means (the kinds, the prefixes, the field set and the vocabulary of
// every enumerated value) and this module owns *how it is spelled* (the field
// order, the field names, the numeric rendering). Neither knows where the bytes
// go: the drop policy lives in `TelemetryQueue.h` and the transport in the
// vehicle layer, and changing either does not change a character this file
// produces. That separation is what makes a log comparable across a firmware
// change that touched only the transport.
//
// The lines, in the order the fields are written:
//
//   EVS3,ver=..,t_ms=..,dt_ms=..,phase=..,cmd=..,armed=..,run_start_ms=..,
//     run_start_ok=..,dwell_start_ms=..,dwell_start_ok=..,stop_verified_ms=..,
//     stop_verified_ok=..,pos_m=..,pos_ok=..,vel_mps=..,vel_ok=..,
//     vel_cmd_mps=..,rem_m=..,motor=..,effort_permille=..,batt_counts=..,
//     curr_counts=..,nfault=..,nfault_low=..,fault=..,authz=..,check=..,
//     drop_s=..,drop_e=..
//
//   EVE3,ver=..,t_ms=..,phase=..,armed=..,event=..,detail=..,fault=..,
//     drop_s=..,drop_e=..
//
// The field names are part of the schema and are frozen with it: a name may be
// added, but renaming one changes the meaning of a column in an existing log and
// needs the version bump. Every name is a single lowercase token, so a line is
// split on `,` and then on the first `=` without any escaping, quoting or
// whitespace handling.
//
// Absent values. A sample carries every field unconditionally: each one is a
// value the control path produced this tick, and an unusable position is still a
// position - it is marked unusable by `pos_ok=0` next to it, not by leaving the
// field out, so a reader never has to tell "zero" apart from "unknown". An
// event's `detail` is the one field a caller may legitimately not have, and it
// then carries `-` (`kTelemetryMissingField`), which is a value, not a gap.
//
// All or nothing. A record that does not fit the caller's buffer is not emitted
// as a shortened line. Truncation eats the *tail* fields - the fault, the
// authorization outcome, the failing check, the drop counters - which are
// exactly the fields a post-mortem needs, and a reader could not tell a missing
// column from an unremarkable one. The formatter therefore reports "did not fit"
// and the transport drops and counts the record; the counters inside the records
// that survive then state how many were lost.

// Every quantity below is written at `kTelemetryFieldDecimals` (`TelemetryRecord.h`),
// which is also what the operator responses use - the same fact at the same
// resolution in both streams.

// The worst-case line lengths. Each is a sum of widest fields, measured against
// the tables and then summed here, so the number is a derivation rather than an
// estimate.
//
// A sample is bounded by
//
//   EVS3(4) + ver(6) + t_ms(6+10) + dt_ms(7+5) + phase(7+14 "ParameterValid") +
//   cmd(5+15 "RampDownForward") + armed(7+1) + run_start_ms(12+2+10) +
//   run_start_ok(12+2+1) + dwell_start_ms(14+2+10) + dwell_start_ok(14+2+1) +
//   stop_verified_ms(16+2+10) + stop_verified_ok(16+2+1) + pos_m(7+13) +
//   pos_ok(8+1) + vel_mps(9+13) + vel_ok(8+1) + vel_cmd_mps(13+13) +
//   rem_m(7+13) + motor(7+12 "DriveForward") + effort_permille(17+5) +
//   batt_counts(13+5) + curr_counts(13+5) + nfault(8+12 "NotInstalled") +
//   nfault_low(12+1 "-") + fault(7+22 "ProvisionalCalibration") +
//   authz(7+27 "DeniedUnsafeDirectionChange") +
//   check(7+29 "StopEvidenceSpansReversalStop") + drop_s(8+10) + drop_e(8+10)
//   = 567
//
// and an event by
//
//   EVE3(4) + ver(6) + t_ms(16) + phase(21) + armed(8) +
//   event(7+16 "TelemetryEnabled") + detail(8+29 "BottleLineDistanceOffInterval") +
//   fault(7+22) + drop_s(18) + drop_e(18) = 180
//
// where each number's digit count is `BoundedText`'s own bound (10 for an
// unsigned 32-bit, 5 for the unsigned 16-bit fields, 13 for a fixed-point
// double) and each name is the longest entry of its vocabulary table at the time
// of writing. The digits and the `appendFixed()` sentinels are enforced by
// `BoundedText`; the table lengths are pinned by the host tests, which measure
// every vocabulary table and format a record built from its longest entry, so a
// longer name fails a test instead of quietly producing a line this module would
// have to drop.
//
// `nfault_low` is no longer always a boolean: it carries the schema's
// absent-value marker when the vehicle has no external nFAULT line (D32), which
// is why the sample gained the `nfault` availability field next to it. The
// level's width is unchanged - `-` is one character, like `0` and `1`.
//
// The three run-timeline readings (S8) are `uint32` clock readings, each at the
// same ten-digit worst case as `t_ms`, and each availability flag is one
// character - so the derivation above is where the sample's growth lives, not a
// formatting detail that could be trimmed.
constexpr uint16_t kTelemetrySampleWorstCaseChars = 567U;
constexpr uint16_t kTelemetryEventWorstCaseChars = 180U;
constexpr uint16_t kTelemetryWorstCaseChars =
    kTelemetrySampleWorstCaseChars > kTelemetryEventWorstCaseChars
        ? kTelemetrySampleWorstCaseChars
        : kTelemetryEventWorstCaseChars;

// The buffer the transport must give the formatter, excluding the terminator the
// transport writes itself. The number itself lives in `BoundedText.h`, because
// the transport runs operator responses through the same buffer; these aliases
// are what the telemetry code and its tests read.
constexpr uint16_t kTelemetryLineCapacity = kSerialLineCapacity;
constexpr uint16_t kTelemetryLineBufferBytes = kSerialLineBufferBytes;

static_assert(kTelemetrySampleWorstCaseChars <= kTelemetryLineCapacity,
              "the line capacity must hold a worst-case sample");
static_assert(kTelemetryEventWorstCaseChars <= kTelemetryLineCapacity,
              "the line capacity must hold a worst-case event");

// Returned instead of a length when the record did not fit, and the helper that
// enforces "whole line or nothing". Both are the shared serial-line contract; the
// names here are what the telemetry code reads.
constexpr uint16_t kTelemetryFormatDidNotFit = kBoundedLineDidNotFit;

// The drop counters, written as the last two fields of either kind of record.
// They travel in every record so a log with a gap in it says so: a host that
// reads only the samples still learns that something was lost.
inline void appendTelemetryDropCounters(BoundedText& text, const TelemetryDrops& drops) {
    text.field("drop_s").appendUnsigned(drops.samples);
    text.field("drop_e").appendUnsigned(drops.events);
}

// Turns a completed writer into a line, or into nothing. A truncated writer is
// reset to an empty string and reported as "did not fit", so a caller can never
// emit a partial record by ignoring the return value - it either has a complete
// line or an empty buffer.
inline uint16_t finishTelemetryLine(const BoundedText& text, char* buffer) {
    return finishBoundedLine(text, buffer);
}

// One sample, as one line, NUL-terminated, without a line terminator (the
// transport appends `\r\n` and owns that decision). Returns the line length in
// characters, or `kTelemetryFormatDidNotFit` when it did not fit `capacity`.
//
// `capacity` excludes the terminator, so `buffer` must hold `capacity + 1` bytes:
// see `kTelemetryLineBufferBytes`.
inline uint16_t formatTelemetrySample(const TelemetrySample& sample, char* buffer,
                                      uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kTelemetrySamplePrefix);
    text.field("ver").appendUnsigned(kTelemetrySchemaVersion);
    text.field("t_ms").appendUnsigned(sample.tMs);
    text.field("dt_ms").appendUnsigned(sample.dtMs);
    text.field("phase").appendText(telemetryPhaseName(sample.phase));
    text.field("cmd").appendText(telemetryRunCommandName(sample.command));
    text.field("armed").appendBool(sample.armed);
    // The run's own timeline: the reading first, then the flag that says whether
    // it exists, in the same shape as `pos_m`/`pos_ok`. A sample writes every
    // field, so an absent reading is still written - as a zero next to a `0` flag.
    text.field("run_start_ms").appendUnsigned(sample.runActuatedAtMs);
    text.field("run_start_ok").appendBool(sample.runActuated);
    text.field("dwell_start_ms").appendUnsigned(sample.finalStopDwellStartMs);
    text.field("dwell_start_ok").appendBool(sample.finalStopDwellStartKnown);
    text.field("stop_verified_ms").appendUnsigned(sample.finalStopVerifiedAtMs);
    text.field("stop_verified_ok").appendBool(sample.finalStopVerified);
    text.field("pos_m").appendFixed(sample.positionM, kTelemetryFieldDecimals);
    text.field("pos_ok").appendBool(sample.positionUsable);
    text.field("vel_mps").appendFixed(sample.measuredVelocityMps, kTelemetryFieldDecimals);
    text.field("vel_ok").appendBool(sample.velocityUsable);
    text.field("vel_cmd_mps").appendFixed(sample.requestedVelocityMps, kTelemetryFieldDecimals);
    text.field("rem_m").appendFixed(sample.remainingDistanceM, kTelemetryFieldDecimals);
    text.field("motor").appendText(telemetryMotorModeName(sample.motorMode));
    text.field("effort_permille").appendUnsigned(sample.effortPermille);
    text.field("batt_counts").appendUnsigned(sample.batteryCounts);
    text.field("curr_counts").appendUnsigned(sample.currentCounts);
    // Availability first, then the level: `nfault` says whether the external line
    // exists at all, and `nfault_low` carries `not_installed` when it does not.
    // Both are written unconditionally, because a sample writes every field.
    text.field("nfault").appendText(driverFaultCapabilityName(sample.driverFaultCapability));
    text.field("nfault_low").appendText(
        telemetryDriverFaultLevelName(sample.driverFaultCapability, sample.driverFaultBarLow));
    text.field("fault").appendText(telemetryFaultName(sample.fault));
    text.field("authz").appendText(telemetryAuthzOutcomeName(sample.outcome));
    text.field("check").appendText(configurationCheckName(sample.firstFailedCheck));
    appendTelemetryDropCounters(text, sample.drops);
    return finishTelemetryLine(text, buffer);
}

// One event, as one line, under the same contract as the sample formatter.
//
// `detail` is written verbatim and must be a comma-free vocabulary name; the
// schema stores it as a pointer so it can only ever be a static string, and every
// table in the schema is comma-free by construction. An empty string is written
// as the missing-field marker rather than as an empty field, so a trailing comma
// can never be mistaken for a value.
inline uint16_t formatTelemetryEvent(const TelemetryEvent& event, char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kTelemetryEventPrefix);
    text.field("ver").appendUnsigned(kTelemetrySchemaVersion);
    text.field("t_ms").appendUnsigned(event.tMs);
    text.field("phase").appendText(telemetryPhaseName(event.phase));
    text.field("armed").appendBool(event.armed);
    text.field("event").appendText(telemetryEventName(event.event));
    const char* detail = event.detail;
    if (detail == nullptr || detail[0] == '\0') {
        detail = kTelemetryMissingField;
    }
    text.field("detail").appendText(detail);
    text.field("fault").appendText(telemetryFaultName(event.fault));
    appendTelemetryDropCounters(text, event.drops);
    return finishTelemetryLine(text, buffer);
}

// Either kind, as the drain loop sees it. The record's kind selects the line, so
// a consumer of the queue never has to know which lane a record came from.
inline uint16_t formatTelemetryRecord(const TelemetryRecord& record, char* buffer,
                                      uint16_t capacity) {
    if (record.kind == TelemetryRecordKind::Event) {
        return formatTelemetryEvent(record.event, buffer, capacity);
    }
    return formatTelemetrySample(record.sample, buffer, capacity);
}

}  // namespace ev
