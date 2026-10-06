#pragma once

#include <cstdint>

#include "BoundedText.h"
#include "OperatorPolicy.h"
#include "TelemetryRecord.h"

namespace ev {

// ---------------------------------------------------------------------------
// The operator responses - the same line format, the other direction
// ---------------------------------------------------------------------------
//
// The text half of the operator interface. Everything an operator types becomes
// one line of `key=value` fields, in exactly the style the telemetry stream uses
// (`AGENTS.md` section 16), so one reader can grep both and one parser can read
// both. The differences from telemetry are deliberate and few:
//
// * **A different prefix** per response kind (`OK`, `ERR`, `STATUS`, `CONFIG`,
//   `CHECK`, `TELEMETRY`, `VERSION`), so a human scanning a serial monitor sees
//   what they asked for without decoding field names.
// * **A refusal names its reason.** An `ERR` line carries the reason a command
//   was not applied, spelled by the vocabulary that owns it: a parse rejection by
//   `commandErrorName()`, a state restriction by `commandRefusalName()`, an
//   announcement by `announceRefusalName()`, a telemetry rate by
//   `telemetryRateRefusalName()`. Nothing invents a new reason string for a case
//   that already has one.
// * **Absent values are marked, not omitted.** A configuration field that has
//   not been set is written as `-` (`kTelemetryMissingField`) rather than left
//   out, so the columns of a response never move and an operator can see that a
//   value is missing rather than guess it from a short line.
//
// What this module is not: it decides nothing. It renders a verdict somebody else
// reached - the policy (`OperatorPolicy.h`), the parser (`SerialCommandParser.h`)
// or the vehicle layer - and it holds no state, reads no clock, commands nothing
// and cannot change what a run does. A response is a report, and the only thing
// it can make the firmware do is emit bytes.
//
// Every formatter returns the line length in characters, or
// `kBoundedLineDidNotFit` with the buffer emptied: a response is emitted whole or
// not at all, because a refusal whose reason got truncated is worse than a
// refusal that never arrived.

// Response prefixes. Like the telemetry prefixes these are part of the wire
// vocabulary: a script distinguishes a response from a record by its first token.
constexpr const char* kOperatorOkPrefix = "OK";
constexpr const char* kOperatorErrorPrefix = "ERR";
constexpr const char* kOperatorStatusPrefix = "STATUS";
constexpr const char* kOperatorConfigPrefix = "CONFIG";
constexpr const char* kOperatorChecksPrefix = "CHECK";
constexpr const char* kOperatorTelemetryPrefix = "TELEMETRY";
constexpr const char* kOperatorVersionPrefix = "VERSION";

// The one refusal reason that is not a name from another table: an arming intent
// refused because the configuration-build checks did not all pass. The failing
// fact travels with it, so the reason string stays short and the detail is a
// field.
constexpr const char* kOperatorReasonChecksFailed = "ChecksFailed";

// Worst-case line lengths, one per response family. Each is a sum of widest
// fields the way `TelemetryFormat.h` derives its own, and each is pinned by a
// host test that formats a response built from the longest name of every table it
// uses.
//
//   STATUS  = 357   every control-state field, the widest names, the longest
//                   configuration rule error name
//   ANNOUNCE_ACK = 138  the widest plan status name and four 13-character values
//   CONFIG  = 170   the widest level name, three 13-character typed values, the
//                   derived Bottle Start Point (one of three rule values), and
//                   the longest rule error name
//   CHECK   = 445   sixteen check names with a value each, three characters of
//                   punctuation per name, and a five-digit mask; the widest form
//                   is a report whose first failure is one of the two longest
//                   check names (and whose mask is therefore the widest that form
//                   can have, since every check from that one on fails)
//   TELEMETRY = 88  the widest stride (ten digits) next to the rate it produces
//                   (six), and two ten-digit drop counters
constexpr uint16_t kOperatorStatusLineWorstCaseChars = 357U;
constexpr uint16_t kOperatorConfigEchoWorstCaseChars = 170U;
constexpr uint16_t kOperatorChecksLineWorstCaseChars = 445U;
constexpr uint16_t kOperatorTelemetryLineWorstCaseChars = 88U;
// The announce confirmation is the longest plan echo: the widest plan status name
// (`PlanExceedsMaxRunDuration`), four 13-character fixed-point values, and the
// prefix and field names around them.
constexpr uint16_t kOperatorAnnounceAckWorstCaseChars = 138U;

// The largest response this module can produce. `config set`'s confirmation is
// the echo plus a `cmd` field, comfortably under the checks report.
constexpr uint16_t kOperatorResponseWorstCaseChars = kOperatorChecksLineWorstCaseChars;

static_assert(kOperatorResponseWorstCaseChars <= kSerialLineCapacity,
              "the shared serial line must hold the widest operator response");

// ---------------------------------------------------------------------------
// The views
// ---------------------------------------------------------------------------
//
// What a response needs from the vehicle layer, gathered into one struct per
// response family. The vehicle layer fills them from the chain's own readings, so
// a response can never disagree with the run about what is happening - it renders
// the same numbers the control path used.

// A status line reports the control state and the configuration verdict, because
// those are the two things an operator asks about when something will not arm.
// Raw hardware readings (battery, current, nFAULT) are deliberately *not* here:
// they belong to the telemetry sample train, which records them every tick, and a
// one-shot status line is a poor place to sample them.
struct OperatorStatusView {
    TelemetrySample state{};
    RunConfigFields config{};
    HeldRunConfigVerdict configVerdict{};
};

// The telemetry stream's own state. Reported by `telemetry status` and confirmed
// by `telemetry on|off|rate`, so an operator can see the stride that was actually
// chosen rather than the rate that was requested.
struct OperatorTelemetryView {
    bool enabled = false;
    uint32_t strideTicks = 1U;
    uint16_t controlCadenceHz = 0U;
    TelemetryDrops drops{};
};

// ---------------------------------------------------------------------------
// Acknowledgements and refusals
// ---------------------------------------------------------------------------

// `OK,cmd=<command>`. The confirmation an operator gets for a command that needs
// no other detail, so a typed command is never silently accepted.
inline uint16_t formatOperatorAck(OperatorCommandId id, char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorOkPrefix);
    text.field("cmd").appendText(operatorCommandName(id));
    return finishBoundedLine(text, buffer);
}

// `ERR,cmd=<command>,reason=<reason>`. The reason must be a comma-free vocabulary
// name; every table this module reports from is comma-free by construction.
inline uint16_t formatOperatorRefusal(OperatorCommandId id, const char* reason, char* buffer,
                                      uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorErrorPrefix);
    text.field("cmd").appendText(operatorCommandName(id));
    text.field("reason").appendText(reason);
    return finishBoundedLine(text, buffer);
}

// `ERR,cmd=arm,reason=ChecksFailed,first_fail=<check>,mask=<n>`. The failing
// check is named and the whole mask is reported, because the operator's next
// action depends on *which* fact is missing - a calibration value and a wiring
// polarity need different responses.
inline uint16_t formatArmRefusal(const ArmVerdict& verdict, char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorErrorPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::Arm));
    text.field("reason").appendText(kOperatorReasonChecksFailed);
    text.field("first_fail").appendText(configurationCheckName(verdict.firstFailedCheck));
    text.field("mask").appendUnsigned(verdict.failedCheckMask);
    return finishBoundedLine(text, buffer);
}

// `ERR,cmd=announce,reason=<refusal>,error=<detail>,first_fail=<check>`. Both
// detail fields are always present: `error` names the rule error when the rules
// rejected the configuration, `first_fail` names the configuration check when the
// run policy cannot sequence the run, and the missing marker when a field does
// not apply to this refusal. An announced Target Time the diagnostic timing
// estimate cannot meet is deliberately no longer a refusal (S7): the estimate is
// unvalidated kinematics and may not invalidate an otherwise legal run. The
// estimate travels on the acknowledgement instead.
inline uint16_t formatAnnounceRefusal(const AnnounceVerdict& verdict, char* buffer,
                                      uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorErrorPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::Announce));
    text.field("reason").appendText(announceRefusalName(verdict.refusal));
    const char* detail = kTelemetryMissingField;
    if (verdict.refusal == AnnounceRefusal::ConfigInvalid) {
        detail = telemetryConfigErrorName(verdict.config.error);
    }
    text.field("error").appendText(detail);
    text.field("first_fail").appendText(verdict.refusal == AnnounceRefusal::PolicyNotUsable
                                            ? configurationCheckName(verdict.firstFailedCheck)
                                            : kTelemetryMissingField);
    return finishBoundedLine(text, buffer);
}

// `OK,cmd=announce,plan=<status>,cruise_mps=<v>,delay_s=<v>,total_s=<v>,margin_s=<v>`
// - the confirmation that the run was announced, with the **diagnostic** timing
// estimate the vehicle layer built for it (`planRunTiming()`). The estimate is
// echoed rather than only stored because it is the kinematic picture behind the
// announcement - what the announced Target Time, the announced geometry and the
// configured limits imply - and an operator who cannot see it cannot judge an
// announcement. It is deliberately **not** a claim about what the run will do:
// while the timing model is unvalidated (S7) the run's cruise is the configured
// one, its deliberate delay is its execution plan's `initialDelayS` (0), and
// `plan=<status>` may therefore refuse while the run is accepted. `total_s` is
// the estimate's whole time (travel + the reversal stop's reserve + the delay)
// and `margin_s` is what the announced time has left over.
inline uint16_t formatAnnounceAck(const AnnounceVerdict& verdict, char* buffer,
                                  uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorOkPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::Announce));
    text.field("plan").appendText(runTimingPlanStatusName(verdict.plan.status));
    text.field("cruise_mps").appendFixed(verdict.plan.plannedCruiseMps, kTelemetryFieldDecimals);
    text.field("delay_s").appendFixed(verdict.plan.plannedDelayS, kTelemetryFieldDecimals);
    text.field("total_s").appendFixed(verdict.plan.intendedTotalS, kTelemetryFieldDecimals);
    text.field("margin_s").appendFixed(verdict.plan.marginS, kTelemetryFieldDecimals);
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// The configuration echo
// ---------------------------------------------------------------------------

// The fields of a held configuration, in a fixed order, with a marker for every
// value that has not been supplied. Shared by the `CONFIG` report and the
// `config set` confirmation so the two can never disagree about what is held.
//
// The Bottle Start Point is the one *derived* field: rule 7.c.ii.(5) fixes it
// from the tournament level, so it is echoed from the same function the run uses
// rather than from a stored value that could drift.
inline void appendConfigEchoFields(BoundedText& text, const RunConfigFields& fields,
                                   const HeldRunConfigVerdict& verdict) {
    text.field("level").appendText(fields.hasLevel ? tournamentLevelName(fields.levelIndex)
                                                   : kTelemetryMissingField);
    if (fields.hasTargetDistanceM) {
        text.field("target_m").appendFixed(fields.targetDistanceM, kTelemetryFieldDecimals);
    } else {
        text.field("target_m").appendText(kTelemetryMissingField);
    }
    if (fields.hasTargetTimeS) {
        text.field("time_s").appendFixed(fields.targetTimeS, kTelemetryFieldDecimals);
    } else {
        text.field("time_s").appendText(kTelemetryMissingField);
    }
    if (fields.hasBottleLineDistanceM) {
        text.field("bottle_line_m").appendFixed(fields.bottleLineDistanceM, kTelemetryFieldDecimals);
    } else {
        text.field("bottle_line_m").appendText(kTelemetryMissingField);
    }
    if (fields.hasLevel) {
        text.field("bottle_start_m")
            .appendFixed(bottleStartDistanceM(tournamentLevelForIndex(fields.levelIndex)),
                         kTelemetryFieldDecimals);
    } else {
        text.field("bottle_start_m").appendText(kTelemetryMissingField);
    }

    text.field("complete").appendBool(verdict.complete);
    text.field("rules").appendBool(verdict.rulesOk);
    // `-` when there is nothing to validate yet, the rule error when there is,
    // and `None` when the rules accept it. An incomplete configuration is not
    // "valid so far", and the field says so the same way it says anything else.
    if (!verdict.complete) {
        text.field("error").appendText(kTelemetryMissingField);
    } else if (verdict.rulesOk) {
        text.field("error").appendText(telemetryConfigErrorName(ConfigError::None));
    } else {
        text.field("error").appendText(telemetryConfigErrorName(verdict.error));
    }
}

// `CONFIG,...` - the answer to `config show` and `config check`. Both commands
// render the same fields, so a configuration cannot look different depending on
// which one the operator typed.
inline uint16_t formatConfigEcho(const RunConfigFields& fields, const HeldRunConfigVerdict& verdict,
                                 char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorConfigPrefix);
    appendConfigEchoFields(text, fields, verdict);
    return finishBoundedLine(text, buffer);
}

// `OK,cmd=config set,...` - the confirmation that the fields the operator just
// supplied were stored, followed by the full configuration they are now part of.
// An operator who has typed three of four fields sees the same line as `config
// show` would print, plus the acknowledgment, which is what makes a repeated
// `config set` safe to type.
inline uint16_t formatConfigSetAck(const RunConfigFields& fields, const HeldRunConfigVerdict& verdict,
                                   char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorOkPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::ConfigSet));
    appendConfigEchoFields(text, fields, verdict);
    return finishBoundedLine(text, buffer);
}

// `ERR,cmd=config set,reason=ValueOutOfRange,error=<rule error>` - a supplied
// field the rules do not accept was refused and *nothing was stored*, so this line
// is the whole effect of the command (`OperatorPolicy.h`,
// `validateSuppliedConfigFields()`). There is no configuration echo on purpose: an
// echo of the unchanged held state next to an error invites reading the refused
// value as held. `reason` is the parse vocabulary's own name for the class of
// mistake, the same one a malformed number gets; `error` is the rule the value
// broke, in the vocabulary `CONFIG` and the announce refusal already use.
inline uint16_t formatConfigSetRefusal(ConfigError error, char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorErrorPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::ConfigSet));
    text.field("reason").appendText(
        commandErrorName(CommandError::ValueOutOfRange));
    text.field("error").appendText(telemetryConfigErrorName(error));
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// The check report
// ---------------------------------------------------------------------------

// `CHECK,all=<0|1>,count=<n>,first=<check>,mask=<n>,<CheckName>=<0|1>,...`
//
// One field per check, keyed by the check's own wire name, so the answer to
// "which checks fail" is a set of named columns rather than a count. The summary
// comes first because that is what determines whether the next command can work:
// `all` gates arming, `first` names the fact to fix, and `mask` is the same
// information for a script (bit `n` for enumerator value `n`).
inline uint16_t formatChecksLine(const ConfigurationBuildChecks& checks, char* buffer,
                                 uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorChecksPrefix);
    text.field("all").appendBool(checks.allPass());
    text.field("count").appendUnsigned(checks.failureCount());
    text.field("first").appendText(configurationCheckName(checks.firstFailure()));
    text.field("mask").appendUnsigned(checks.failureMask());
    for (uint8_t index = 0U; index < kConfigurationCheckCount; ++index) {
        const ConfigurationCheckId id = static_cast<ConfigurationCheckId>(index);
        text.field(configurationCheckName(id)).appendBool(checks.passed(id));
    }
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// `STATUS,...` - the run state, in the same field names the telemetry sample
// uses, followed by the configuration verdict and the rule error that is
// blocking it. The names matching the sample's is not cosmetic: it means a reader
// that greps `phase=` or `check=` finds the same fact in both streams.
inline uint16_t formatStatusLine(const OperatorStatusView& view, char* buffer, uint16_t capacity) {
    const TelemetrySample& state = view.state;
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorStatusPrefix);
    text.field("phase").appendText(telemetryPhaseName(state.phase));
    text.field("armed").appendBool(state.armed);
    text.field("pos_m").appendFixed(state.positionM, kTelemetryFieldDecimals);
    text.field("pos_ok").appendBool(state.positionUsable);
    text.field("vel_mps").appendFixed(state.measuredVelocityMps, kTelemetryFieldDecimals);
    text.field("vel_ok").appendBool(state.velocityUsable);
    text.field("vel_cmd_mps").appendFixed(state.requestedVelocityMps, kTelemetryFieldDecimals);
    text.field("rem_m").appendFixed(state.remainingDistanceM, kTelemetryFieldDecimals);
    text.field("motor").appendText(telemetryMotorModeName(state.motorMode));
    text.field("effort_permille").appendUnsigned(state.effortPermille);
    text.field("fault").appendText(telemetryFaultName(state.fault));
    text.field("authz").appendText(telemetryAuthzOutcomeName(state.outcome));
    text.field("check").appendText(configurationCheckName(state.firstFailedCheck));

    text.field("config_complete").appendBool(view.configVerdict.complete);
    text.field("config_rules").appendBool(view.configVerdict.rulesOk);
    if (!view.configVerdict.complete) {
        text.field("config_error").appendText(kTelemetryMissingField);
    } else if (view.configVerdict.rulesOk) {
        text.field("config_error").appendText(telemetryConfigErrorName(ConfigError::None));
    } else {
        text.field("config_error").appendText(telemetryConfigErrorName(view.configVerdict.error));
    }
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// Telemetry control
// ---------------------------------------------------------------------------

// The realized rate the stride produces at the reported cadence. Zero would be
// "never", which the session does not offer, so a zero stride is reported as the
// finest rate there is rather than as a division by zero.
inline double operatorTelemetryRateHz(const OperatorTelemetryView& view) {
    const uint32_t stride = view.strideTicks == 0U ? 1U : view.strideTicks;
    return static_cast<double>(view.controlCadenceHz) / static_cast<double>(stride);
}

inline void appendTelemetryStateFields(BoundedText& text, const OperatorTelemetryView& view) {
    text.field("enabled").appendBool(view.enabled);
    text.field("stride").appendUnsigned(view.strideTicks);
    text.field("rate_hz").appendFixed(operatorTelemetryRateHz(view), kTelemetryFieldDecimals);
    text.field("drop_s").appendUnsigned(view.drops.samples);
    text.field("drop_e").appendUnsigned(view.drops.events);
}

// `TELEMETRY,...` - the answer to `telemetry status`.
inline uint16_t formatTelemetryState(const OperatorTelemetryView& view, char* buffer,
                                     uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorTelemetryPrefix);
    appendTelemetryStateFields(text, view);
    return finishBoundedLine(text, buffer);
}

// `OK,cmd=telemetry on|off,...` - the confirmation of a telemetry command, with
// the state it produced. The realized state is reported rather than the requested
// one, so `telemetry off` cannot leave an operator believing a stream is still
// running.
inline uint16_t formatTelemetryAck(OperatorCommandId id, const OperatorTelemetryView& view,
                                   char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorOkPrefix);
    text.field("cmd").appendText(operatorCommandName(id));
    appendTelemetryStateFields(text, view);
    return finishBoundedLine(text, buffer);
}

// `OK,cmd=telemetry rate,stride=<n>,rate_hz=<r>` - the stride that was chosen and
// the rate it actually produces at this cadence, which is not the requested value
// whenever the request did not divide the cadence exactly.
inline uint16_t formatTelemetryRateAck(const TelemetryRateVerdict& verdict, char* buffer,
                                       uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorOkPrefix);
    text.field("cmd").appendText(operatorCommandName(OperatorCommandId::TelemetryRate));
    text.field("stride").appendUnsigned(verdict.strideTicks);
    text.field("rate_hz").appendFixed(verdict.effectiveRateHz, kTelemetryFieldDecimals);
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// `VERSION,fw=<id>,telemetry_schema=<n>`. The schema number is reported next to
// the firmware identity because a log and a firmware are only comparable when
// both are known; `firmwareId` is a compile-time string owned by the vehicle
// layer and must not contain a comma.
inline uint16_t formatVersionLine(const char* firmwareId, char* buffer, uint16_t capacity) {
    BoundedText text(buffer, capacity);
    text.appendText(kOperatorVersionPrefix);
    text.field("fw").appendText(firmwareId == nullptr ? kTelemetryMissingField : firmwareId);
    text.field("telemetry_schema").appendUnsigned(kTelemetrySchemaVersion);
    return finishBoundedLine(text, buffer);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

// The command list, one line per entry, written verbatim by the transport. Static
// text rather than a formatter: there is nothing to compute, and a table of
// literals is what a person can check against the parser by reading it.
constexpr const char* const kOperatorHelpLines[] = {
    "help          - this list",
    "version       - firmware identity and telemetry schema",
    "status        - run state and the held configuration's verdict",
    "checks        - every configuration-build check, named",
    "boot          - re-send the boot line for this reader",
    "config show   - echo the held configuration",
    "config check  - re-run the rule validation now",
    "config set level=<regional|state|national> distance=<m> time=<s> bottle_line=<m>",
    "config clear  - discard the held configuration",
    "announce      - offer the held configuration to a run",
    "arm           - arm for the announced run (every check must pass)",
    "disarm        - disarm; always available, also during a run",
    "reset         - return the chain to the new-run boundary",
    "telemetry on | off | status",
    "telemetry rate <hz>",
    "records: EVS/EVE lines; responses: OK, ERR, STATUS, CONFIG, CHECK, TELEMETRY, VERSION",
};

constexpr uint16_t kOperatorHelpLineCount =
    static_cast<uint16_t>(sizeof(kOperatorHelpLines) / sizeof(kOperatorHelpLines[0]));

}  // namespace ev
