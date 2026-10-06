#include <cstdint>
#include <cstring>

#include <unity.h>

#include <OperatorResponse.h>

// Contract tests for the operator response formatter
// (lib/evcore/src/OperatorResponse.h): the spelling of every response, the
// missing-value markers, the refusal reasons that come from other modules'
// vocabularies, and the worst-case line bound the transport sizes its buffer
// from.
//
// These tests pin bytes. A pass means every response is whole, bounded, and
// spelled the way the schema froze it - not that a command had any effect on the
// vehicle, which is the policy's and the vehicle layer's business.

void setUp(void) {}
void tearDown(void) {}

namespace {

// The configuration every test uses when it needs a valid one: State level
// (0.10 m grid), 7.50 m target, 15.0 s, 1.00 m Bottle Line Distance. Both
// boundaries and the grid are the rules module's, not this test's.
ev::RunConfigFields validStateConfig() {
    ev::RunConfigFields fields;
    fields.hasLevel = true;
    fields.levelIndex = 1U;  // state
    fields.hasTargetDistanceM = true;
    fields.targetDistanceM = 7.50;
    fields.hasTargetTimeS = true;
    fields.targetTimeS = 15.0;
    fields.hasBottleLineDistanceM = true;
    fields.bottleLineDistanceM = 1.00;
    return fields;
}

ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

// The plan the vehicle layer builds for the fixture configuration, through the
// production arithmetic: State level, 7.50 m target, 1.00 m Bottle Line Distance
// (9.50 m of announced travel) and an announced time those limits can meet.
ev::RunTimingPlan planForTargetTime(double targetTimeS) {
    return ev::planRunTiming(ev::RunTimingPlanInputs(
        9.50, ev::Sourced<double>(targetTimeS, ev::ValueSource::RuleDerived, "rules2027 8.i"),
        measuredValue(1.20), measuredValue(1.20), measuredValue(0.50), measuredValue(1.00),
        measuredValue(0.10), measuredValue(0.20), measuredValue(30.0)));
}

ev::ConfigurationBuildChecks allChecksPassing() {
    // Set through the id mapping rather than field by field, so a check added to
    // the vocabulary cannot be left false here by accident - which would show up
    // as a mystery extra bit in some other test's mask.
    ev::ConfigurationBuildChecks checks;
    for (uint8_t index = 0U; index < ev::kConfigurationCheckCount; ++index) {
        checks.setPassed(static_cast<ev::ConfigurationCheckId>(index), true);
    }
    return checks;
}

ev::TelemetrySample recognizableState() {
    ev::TelemetrySample state;
    state.tMs = 5832U;
    state.dtMs = 20U;
    state.phase = ev::RunPhase::Approach;
    state.command = ev::RunMotionCommand::DriveForward;
    state.armed = true;
    state.positionM = 6.214;
    state.positionUsable = true;
    state.measuredVelocityMps = 0.714;
    state.velocityUsable = true;
    state.requestedVelocityMps = 0.7;
    state.remainingDistanceM = 0.286;
    state.motorMode = ev::MotorMode::DriveForward;
    state.effortPermille = 740U;
    state.outcome = ev::AuthzOutcome::AuthorizedDrive;
    return state;
}

// The widest values every field of a status line can hold: the longest word of
// each vocabulary table and the widest number `BoundedText` can write.
ev::OperatorStatusView widestStatusView() {
    ev::OperatorStatusView view;
    view.state = recognizableState();
    view.state.phase = ev::RunPhase::ParameterValid;
    view.state.positionM = -9999999.9999;
    view.state.measuredVelocityMps = -9999999.9999;
    view.state.requestedVelocityMps = -9999999.9999;
    view.state.remainingDistanceM = -9999999.9999;
    view.state.effortPermille = 65535U;
    view.state.fault = ev::FaultCode::ProvisionalCalibration;
    view.state.outcome = ev::AuthzOutcome::DeniedUnsafeDirectionChange;
    view.state.firstFailedCheck = ev::ConfigurationCheckId::StopEvidenceSpansReversalStop;
    view.config = validStateConfig();
    view.configVerdict.complete = true;
    view.configVerdict.rulesOk = false;
    view.configVerdict.error = ev::ConfigError::BottleLineDistanceOffInterval;
    return view;
}

}  // namespace

// An acknowledgment always names the command it acknowledges, so a typed command
// is never silently accepted.
void test_acknowledgements_name_the_command(void) {
    char buffer[ev::kSerialLineBufferBytes];

    TEST_ASSERT_TRUE(ev::formatOperatorAck(ev::OperatorCommandId::Help, buffer,
                                           ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("OK,cmd=help", buffer);

    ev::formatOperatorAck(ev::OperatorCommandId::ConfigClear, buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_STRING("OK,cmd=config clear", buffer);

    // Every command in the vocabulary produces a complete, bounded line; a
    // command added later cannot silently fall out of the acknowledgment table.
    for (uint8_t index = 1U;
         index <= static_cast<uint8_t>(ev::OperatorCommandId::TelemetryRate); ++index) {
        const ev::OperatorCommandId id = static_cast<ev::OperatorCommandId>(index);
        const uint16_t length = ev::formatOperatorAck(id, buffer, ev::kSerialLineCapacity);
        TEST_ASSERT_TRUE_MESSAGE(length > 0U, ev::operatorCommandName(id));
        TEST_ASSERT_EQUAL_INT(0, strncmp(buffer, "OK,cmd=", 7U));
        TEST_ASSERT_EQUAL_STRING(ev::operatorCommandName(id), buffer + 7U);
    }
}

// A refusal names both the command and the reason, and the reason is the
// vocabulary that owns that case rather than a new string invented here.
void test_refusals_name_the_command_and_the_reason(void) {
    char buffer[ev::kSerialLineBufferBytes];

    TEST_ASSERT_TRUE(ev::formatOperatorRefusal(ev::OperatorCommandId::ConfigSet,
                                               ev::commandRefusalName(ev::CommandRefusal::RunUnderway),
                                               buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("ERR,cmd=config set,reason=RunUnderway", buffer);

    // The parse vocabulary reaches the operator through the same formatter.
    ev::formatOperatorRefusal(ev::OperatorCommandId::None,
                              ev::commandErrorName(ev::CommandError::UnknownCommand), buffer,
                              ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_STRING("ERR,cmd=none,reason=UnknownCommand", buffer);
}

// A configuration that has not been filled in says so field by field. A missing
// value is marked, never omitted: the columns of the response never move, so an
// operator can see what is still missing.
void test_incomplete_configuration_marks_every_missing_field(void) {
    char buffer[ev::kSerialLineBufferBytes];
    const ev::RunConfigFields fields;
    const ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(fields);

    TEST_ASSERT_FALSE(verdict.complete);
    TEST_ASSERT_TRUE(ev::formatConfigEcho(fields, verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "CONFIG,level=-,target_m=-,time_s=-,bottle_line_m=-,bottle_start_m=-,complete=0,rules=0,"
        "error=-",
        buffer);
}

// A complete configuration the rules accept echoes its values, the Bottle Start
// Point the level fixes, and the passing verdict.
void test_valid_configuration_echoes_values_and_derived_bottle_start(void) {
    char buffer[ev::kSerialLineBufferBytes];
    const ev::RunConfigFields fields = validStateConfig();
    const ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(fields);

    TEST_ASSERT_TRUE(verdict.complete);
    TEST_ASSERT_TRUE(verdict.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None), static_cast<int>(verdict.error));

    TEST_ASSERT_TRUE(ev::formatConfigEcho(fields, verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "CONFIG,level=state,target_m=7.5000,time_s=15.0000,bottle_line_m=1.0000,"
        "bottle_start_m=3.0000,complete=1,rules=1,error=None",
        buffer);
}

// A configuration the rules reject is echoed *with its values*, not replaced by
// an error: the operator sees the number that broke the rule next to the rule it
// broke.
void test_rejected_configuration_echoes_values_and_the_rule_error(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::RunConfigFields fields = validStateConfig();
    fields.targetDistanceM = 3.00;  // below the 5.00 m minimum (rule 8.h)
    const ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(fields);

    TEST_ASSERT_TRUE(verdict.complete);
    TEST_ASSERT_FALSE(verdict.rulesOk);

    TEST_ASSERT_TRUE(ev::formatConfigEcho(fields, verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "CONFIG,level=state,target_m=3.0000,time_s=15.0000,bottle_line_m=1.0000,"
        "bottle_start_m=3.0000,complete=1,rules=0,error=TargetDistanceOutOfRange",
        buffer);
}

// `config set` confirms by echoing the accumulated configuration, so a repeated
// command shows the state it produced rather than only an acknowledgement.
void test_config_set_acknowledgement_repeats_the_held_configuration(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::RunConfigFields fields;
    fields.hasLevel = true;
    fields.levelIndex = 1U;
    const ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(fields);

    TEST_ASSERT_TRUE(ev::formatConfigSetAck(fields, verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "OK,cmd=config set,level=state,target_m=-,time_s=-,bottle_line_m=-,bottle_start_m=3.0000,"
        "complete=0,rules=0,error=-",
        buffer);

    // The second field lands in the same line: the count of typed fields is not
    // what matters, the configuration is.
    fields.hasTargetDistanceM = true;
    fields.targetDistanceM = 7.50;
    TEST_ASSERT_TRUE(ev::formatConfigSetAck(fields, ev::heldRunConfigVerdict(fields), buffer,
                                            ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "OK,cmd=config set,level=state,target_m=7.5000,time_s=-,bottle_line_m=-,"
        "bottle_start_m=3.0000,complete=0,rules=0,error=-",
        buffer);
}

// A refused `config set` names the rule the value broke and carries no
// configuration echo: nothing was stored, and an echo of the unchanged held state
// beside an error invites reading the refused value as held (finding F-5).
void test_config_set_refusal_names_the_rule_and_echoes_nothing(void) {
    char buffer[ev::kSerialLineBufferBytes];

    TEST_ASSERT_TRUE(ev::formatConfigSetRefusal(ev::ConfigError::TargetDistanceOutOfRange,
                                                buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "ERR,cmd=config set,reason=ValueOutOfRange,error=TargetDistanceOutOfRange", buffer);

    // Every rule error the door can report is a comma-free vocabulary name, so the
    // line stays parseable whatever was refused.
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::ConfigError::BottleStartNotLevelDistance);
         ++index) {
        const ev::ConfigError error = static_cast<ev::ConfigError>(index);
        TEST_ASSERT_TRUE(ev::formatConfigSetRefusal(error, buffer, ev::kSerialLineCapacity) > 0U);
        TEST_ASSERT_NOT_NULL(strstr(buffer, "ERR,cmd=config set,reason=ValueOutOfRange,error="));
        TEST_ASSERT_NOT_NULL(strstr(buffer, ev::telemetryConfigErrorName(error)));
        // The rule error is the last field and carries no delimiter of its own, so
        // a script can read it to end of line whatever was refused.
        const char* name = ev::telemetryConfigErrorName(error);
        TEST_ASSERT_EQUAL_INT(0, strcmp(buffer + strlen(buffer) - strlen(name), name));
    }
}

// The check report names every check, so "which fact is missing" is answered by
// named columns rather than by a count.
void test_check_report_names_every_check(void) {
    char buffer[ev::kSerialLineBufferBytes];
    const ev::ConfigurationBuildChecks allFailing;

    TEST_ASSERT_TRUE(ev::formatChecksLine(allFailing, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "CHECK,all=0,count=16,first=ConfigurationValid,mask=65535,"
        "ConfigurationValid=0,CalibrationMeasured=0,MeasuredValuesCiteEvidence=0,"
        "EffortCeilingsAgree=0,CommandPolarityAgrees=0,TickPolarityAgrees=0,"
        "DriverFaultConfigurationValid=0,StopEvidenceSpansReversalStop=0,"
        "StopEvidenceSpansFinalStop=0,SafetyLimitsValid=0,ControllerConfigValid=0,"
        "EstimatorConfigValid=0,ProfileLimitsValid=0,PhaseConfigValid=0,"
        "ReversalConfigValid=0,RunPolicyUsable=0",
        buffer);

    const ev::ConfigurationBuildChecks allPassing = allChecksPassing();
    TEST_ASSERT_TRUE(ev::formatChecksLine(allPassing, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "CHECK,all=1,count=0,first=none,mask=0,"
        "ConfigurationValid=1,CalibrationMeasured=1,MeasuredValuesCiteEvidence=1,"
        "EffortCeilingsAgree=1,CommandPolarityAgrees=1,TickPolarityAgrees=1,"
        "DriverFaultConfigurationValid=1,StopEvidenceSpansReversalStop=1,"
        "StopEvidenceSpansFinalStop=1,SafetyLimitsValid=1,ControllerConfigValid=1,"
        "EstimatorConfigValid=1,ProfileLimitsValid=1,PhaseConfigValid=1,"
        "ReversalConfigValid=1,RunPolicyUsable=1",
        buffer);
}

// A refusal that is about a missing fact names the fact, and the mask carries the
// rest without needing one field per check.
void test_checks_failure_attribution_is_named_and_masked(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::ConfigurationBuildChecks checks = allChecksPassing();
    checks.calibrationMeasured = false;
    checks.profileLimitsValid = false;
    const ev::ArmVerdict verdict = ev::armVerdictFor(checks);

    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::CalibrationMeasured),
                          static_cast<int>(verdict.firstFailedCheck));

    TEST_ASSERT_TRUE(ev::formatArmRefusal(verdict, buffer, ev::kSerialLineCapacity) > 0U);
    // The two failing checks' bits: CalibrationMeasured and ProfileLimitsValid.
    const uint16_t expectedMask =
        checks.failureMask();
    TEST_ASSERT_EQUAL_UINT32(4098U, static_cast<uint32_t>(expectedMask));
    TEST_ASSERT_EQUAL_STRING(
        "ERR,cmd=arm,reason=ChecksFailed,first_fail=CalibrationMeasured,mask=4098", buffer);
}

// An accepted arm needs no explanation; a refused announcement does, and which
// detail field is filled says which gate refused it.
void test_announcement_refusals_carry_the_gate_that_refused(void) {
    char buffer[ev::kSerialLineBufferBytes];
    const ev::ConfigurationBuildChecks checks = allChecksPassing();

    // A completed configuration the rules reject: the rule error is the detail.
    const ev::RunTimingPlan plan = planForTargetTime(15.0);

    ev::RunConfigFields fields = validStateConfig();
    fields.targetDistanceM = 3.00;
    ev::AnnounceVerdict verdict = ev::announceVerdictFor(fields, checks, plan);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::ConfigInvalid),
                          static_cast<int>(verdict.refusal));
    TEST_ASSERT_TRUE(ev::formatAnnounceRefusal(verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "ERR,cmd=announce,reason=ConfigInvalid,error=TargetDistanceOutOfRange,first_fail=-", buffer);

    // A configuration the rules accept but the run policy cannot sequence: the
    // failing check is the detail.
    ev::AnnounceVerdict policyVerdict = ev::announceVerdictFor(validStateConfig(), checks, plan);
    TEST_ASSERT_TRUE(policyVerdict.ok);
    ev::ConfigurationBuildChecks unusable = checks;
    unusable.runPolicyUsable = false;
    policyVerdict = ev::announceVerdictFor(validStateConfig(), unusable, plan);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::PolicyNotUsable),
                          static_cast<int>(policyVerdict.refusal));
    TEST_ASSERT_TRUE(ev::formatAnnounceRefusal(policyVerdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "ERR,cmd=announce,reason=PolicyNotUsable,error=-,first_fail=RunPolicyUsable", buffer);

    // An announced time the diagnostic timing estimate cannot meet is no longer a
    // refusal (S7): the estimate is unvalidated kinematics, so it travels with the
    // verdict but cannot invalidate an otherwise legal announcement.
    const ev::RunTimingPlan impossible = planForTargetTime(1.0);
    TEST_ASSERT_FALSE(impossible.valid);
    const ev::AnnounceVerdict unreachable =
        ev::announceVerdictFor(validStateConfig(), checks, impossible);
    TEST_ASSERT_TRUE(unreachable.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::None),
                          static_cast<int>(unreachable.refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunTimingPlanStatus::TargetTimeTooShort),
                          static_cast<int>(unreachable.plan.status));
}

// The accepted announcement carries the diagnostic timing estimate, because it is
// the kinematic picture behind the announcement - not something the operator can
// reconstruct from the typed configuration and the limits by eye. It is reported,
// not obeyed: the run's delay is its execution plan's (S7).
void test_announce_ack_carries_the_run_plan(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::AnnounceVerdict verdict;
    verdict.ok = true;
    verdict.plan = ev::RunTimingPlan(0.5000, 2.0000, 10.0000, 0.2000, 12.2000, 2.8000, true,
                                     ev::RunTimingPlanStatus::Planned);

    TEST_ASSERT_TRUE(ev::formatAnnounceAck(verdict, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "OK,cmd=announce,plan=Planned,cruise_mps=0.5000,delay_s=2.0000,total_s=12.2000,"
        "margin_s=2.8000",
        buffer);

    // The estimate the gate accepts for the fixture configuration is built by the
    // same arithmetic the vehicle layer runs, and the gate accepts it without
    // consulting whether it can meet the announced time.
    const ev::RunTimingPlan fixturePlan = planForTargetTime(15.0);
    const ev::AnnounceVerdict gated =
        ev::announceVerdictFor(validStateConfig(), allChecksPassing(), fixturePlan);
    TEST_ASSERT_TRUE(gated.ok);
    TEST_ASSERT_TRUE(gated.plan.valid);
}

// The status line uses the telemetry sample's field names for the same facts, so
// one reader can grep both streams for `phase=`, `check=` or `pos_m=`.
void test_status_line_pins_the_control_state(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::OperatorStatusView view;
    view.state = recognizableState();
    view.config = validStateConfig();
    view.configVerdict = ev::heldRunConfigVerdict(view.config);

    TEST_ASSERT_TRUE(ev::formatStatusLine(view, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "STATUS,phase=Approach,armed=1,pos_m=6.2140,pos_ok=1,vel_mps=0.7140,vel_ok=1,"
        "vel_cmd_mps=0.7000,rem_m=0.2860,motor=DriveForward,effort_permille=740,fault=None,"
        "authz=AuthorizedDrive,check=none,config_complete=1,config_rules=1,config_error=None",
        buffer);
}

// A status line with nothing held and nothing measured is still a complete line:
// every field is present, and the markers say what is unknown.
void test_status_line_reports_an_empty_vehicle(void) {
    char buffer[ev::kSerialLineBufferBytes];
    const ev::OperatorStatusView view;

    TEST_ASSERT_TRUE(ev::formatStatusLine(view, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",phase=Idle,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",pos_ok=0,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",authz=DeniedNotArmed,"));
    // The configuration verdict is the last field of the line, so the marker for
    // "nothing held" is what the line ends with rather than what it contains.
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",config_error=-"));
    TEST_ASSERT_EQUAL_STRING("config_error=-", buffer + strlen(buffer) - 14U);
}

// Telemetry control reports the state it produced, including the rate the stride
// actually realizes, which is not the requested rate when the request did not
// divide the cadence exactly.
void test_telemetry_responses_report_the_realized_state(void) {
    char buffer[ev::kSerialLineBufferBytes];
    ev::OperatorTelemetryView view;
    view.enabled = false;
    view.strideTicks = 1U;
    view.controlCadenceHz = 50U;
    view.drops.samples = 3U;
    view.drops.events = 1U;

    TEST_ASSERT_TRUE(ev::formatTelemetryState(view, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("TELEMETRY,enabled=0,stride=1,rate_hz=50.0000,drop_s=3,drop_e=1",
                             buffer);

    view.enabled = true;
    TEST_ASSERT_TRUE(ev::formatTelemetryAck(ev::OperatorCommandId::TelemetryOn, view, buffer,
                                            ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "OK,cmd=telemetry on,enabled=1,stride=1,rate_hz=50.0000,drop_s=3,drop_e=1", buffer);

    // 10 Hz on a 50 Hz loop is one sample every fifth tick, and exactly 10 Hz.
    const ev::TelemetryRateVerdict rate = ev::telemetryStrideForRate(10.0, 50U);
    TEST_ASSERT_TRUE(rate.ok);
    TEST_ASSERT_TRUE(ev::formatTelemetryRateAck(rate, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("OK,cmd=telemetry rate,stride=5,rate_hz=10.0000", buffer);

    // 3 Hz on a 50 Hz loop cannot be exact: the stride is a whole number of
    // ticks, so the realized rate is reported rather than the requested one.
    const ev::TelemetryRateVerdict inexact = ev::telemetryStrideForRate(3.0, 50U);
    TEST_ASSERT_TRUE(inexact.ok);
    TEST_ASSERT_EQUAL_UINT32(17U, inexact.strideTicks);
    TEST_ASSERT_TRUE(ev::formatTelemetryRateAck(inexact, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("OK,cmd=telemetry rate,stride=17,rate_hz=2.9412", buffer);
}

// The version line carries the schema number next to the firmware identity,
// because a log and a firmware are only comparable when both are known.
void test_version_line_reports_identity_and_schema(void) {
    char buffer[ev::kSerialLineBufferBytes];

    TEST_ASSERT_TRUE(ev::formatVersionLine("ev-nano-r4-p7b", buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("VERSION,fw=ev-nano-r4-p7b,telemetry_schema=2", buffer);

    TEST_ASSERT_TRUE(ev::formatVersionLine(nullptr, buffer, ev::kSerialLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_STRING("VERSION,fw=-,telemetry_schema=2", buffer);

    // The schema number is the telemetry schema's, not a second copy of it.
    TEST_ASSERT_TRUE(strstr(buffer, "telemetry_schema=2") != nullptr);
    TEST_ASSERT_EQUAL_UINT8(2U, ev::kTelemetrySchemaVersion);
}

// The declared worst case is the real one, per response family. A table entry
// that grows breaks this test rather than the transport's buffer assumption.
void test_worst_case_responses_match_the_declared_bounds(void) {
    char buffer[ev::kSerialLineBufferBytes];

    const uint16_t statusLength =
        ev::formatStatusLine(widestStatusView(), buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorStatusLineWorstCaseChars, statusLength);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), statusLength);

    const uint16_t checksLength =
        ev::formatChecksLine(allChecksPassing(), buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(checksLength > 0U);

    // The announce confirmation: the widest plan status name, so the refusal path
    // and the acceptance path are bounded by the same estimate. The widest form
    // carries negative values, which is the widest a fixed-point field can be.
    ev::AnnounceVerdict wideAnnounce;
    wideAnnounce.plan = ev::RunTimingPlan(-9999999.9999, -9999999.9999, -9999999.9999,
                                          -9999999.9999, -9999999.9999, -9999999.9999, false,
                                          ev::RunTimingPlanStatus::PlanExceedsMaxRunDuration);
    const uint16_t announceAckLength =
        ev::formatAnnounceAck(wideAnnounce, buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorAnnounceAckWorstCaseChars, announceAckLength);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), announceAckLength);

    // The widest check report is the one whose first failure is the longest check
    // name, so every check before it passes and every check from it on fails.
    ev::ConfigurationBuildChecks widest;
    for (uint8_t index = 0U; index < ev::kConfigurationCheckCount; ++index) {
        widest.setPassed(static_cast<ev::ConfigurationCheckId>(index),
                         index < static_cast<uint8_t>(ev::ConfigurationCheckId::StopEvidenceSpansReversalStop));
    }
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::ConfigurationCheckId::StopEvidenceSpansReversalStop),
        static_cast<int>(widest.firstFailure()));
    const uint16_t widestChecksLength =
        ev::formatChecksLine(widest, buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorChecksLineWorstCaseChars, widestChecksLength);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), widestChecksLength);

    // The configuration echo: the widest level name, the widest values the
    // grammar can store, and the longest rule error name that can be reported.
    ev::RunConfigFields wideFields;
    wideFields.hasLevel = true;
    wideFields.levelIndex = 2U;  // national
    wideFields.hasTargetDistanceM = true;
    wideFields.targetDistanceM = -9999999.9999;
    wideFields.hasTargetTimeS = true;
    wideFields.targetTimeS = -9999999.9999;
    wideFields.hasBottleLineDistanceM = true;
    wideFields.bottleLineDistanceM = -9999999.9999;
    ev::HeldRunConfigVerdict wideVerdict;
    wideVerdict.complete = true;
    wideVerdict.rulesOk = false;
    wideVerdict.error = ev::ConfigError::BottleLineDistanceOffInterval;
    const uint16_t configLength =
        ev::formatConfigEcho(wideFields, wideVerdict, buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorConfigEchoWorstCaseChars, configLength);

    // The `config set` confirmation is the echo plus its `cmd` field.
    const uint16_t setAckLength =
        ev::formatConfigSetAck(wideFields, wideVerdict, buffer, ev::kSerialLineCapacity);
    // The confirmation is the echo with `OK,cmd=config set` in place of the
    // `CONFIG` prefix: seventeen characters of prefix against six.
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorConfigEchoWorstCaseChars + 11U, setAckLength);

    // Telemetry state: two ten-digit drop counters and the widest rate.
    // The widest stride is the type's maximum: a huge stride costs ten digits
    // where the rate it produces costs six, and a small stride is the other way
    // round, so the maximum of the sum is what has to fit.
    ev::OperatorTelemetryView wideTelemetry;
    wideTelemetry.enabled = true;
    wideTelemetry.strideTicks = 0xFFFFFFFFu;
    wideTelemetry.controlCadenceHz = 65535U;
    wideTelemetry.drops.samples = 0xFFFFFFFFu;
    wideTelemetry.drops.events = 0xFFFFFFFFu;
    const uint16_t telemetryLength =
        ev::formatTelemetryState(wideTelemetry, buffer, ev::kSerialLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kOperatorTelemetryLineWorstCaseChars, telemetryLength);

    // And every one of them fits the buffer the transport actually allocates.
    TEST_ASSERT_TRUE(ev::kOperatorResponseWorstCaseChars <= ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(ev::kOperatorStatusLineWorstCaseChars <= ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(ev::kOperatorConfigEchoWorstCaseChars <= ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(ev::kOperatorTelemetryLineWorstCaseChars <= ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(ev::kOperatorAnnounceAckWorstCaseChars <= ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(setAckLength <= ev::kSerialLineCapacity);
}

// A response that does not fit is emitted as nothing, never as a shortened line:
// a refusal whose reason was truncated would read as a different refusal.
void test_a_response_that_does_not_fit_emits_nothing(void) {
    char buffer[ev::kSerialLineBufferBytes];
    buffer[0] = 'x';
    buffer[1] = '\0';

    const uint16_t length =
        ev::formatChecksLine(allChecksPassing(), buffer, 40U);

    TEST_ASSERT_EQUAL_UINT16(ev::kBoundedLineDidNotFit, length);
    TEST_ASSERT_EQUAL_STRING("", buffer);
}

// The help table is data the transport writes verbatim, so this test pins its two
// properties: every line is short enough to be a line, and every command the
// parser accepts is mentioned.
void test_help_table_lists_the_commands(void) {
    TEST_ASSERT_TRUE(ev::kOperatorHelpLineCount > 0U);

    // Each command is listed under its own name, at the start of its line, so a
    // help line can be checked against the parser without matching a substring of
    // another command's name.
    bool mentionsArm = false;
    bool mentionsDisarm = false;
    bool mentionsConfigSet = false;
    bool mentionsTelemetryRate = false;
    bool mentionsStatus = false;
    bool mentionsBoot = false;
    for (uint16_t index = 0U; index < ev::kOperatorHelpLineCount; ++index) {
        const char* line = ev::kOperatorHelpLines[index];
        TEST_ASSERT_NOT_NULL(line);
        TEST_ASSERT_TRUE(strlen(line) > 0U);
        TEST_ASSERT_TRUE(strlen(line) < ev::kSerialLineCapacity);
        mentionsArm = mentionsArm || strncmp(line, "arm ", 4U) == 0;
        mentionsDisarm = mentionsDisarm || strncmp(line, "disarm ", 7U) == 0;
        mentionsStatus = mentionsStatus || strncmp(line, "status ", 7U) == 0;
        mentionsBoot = mentionsBoot || strncmp(line, "boot ", 5U) == 0;
        mentionsConfigSet = mentionsConfigSet || strncmp(line, "config set ", 11U) == 0;
        mentionsTelemetryRate = mentionsTelemetryRate || strncmp(line, "telemetry rate ", 15U) == 0;
    }
    TEST_ASSERT_TRUE(mentionsStatus);
    TEST_ASSERT_TRUE(mentionsBoot);
    TEST_ASSERT_TRUE(mentionsArm);
    TEST_ASSERT_TRUE(mentionsDisarm);
    TEST_ASSERT_TRUE(mentionsConfigSet);
    TEST_ASSERT_TRUE(mentionsTelemetryRate);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_acknowledgements_name_the_command);
    RUN_TEST(test_refusals_name_the_command_and_the_reason);
    RUN_TEST(test_incomplete_configuration_marks_every_missing_field);
    RUN_TEST(test_valid_configuration_echoes_values_and_derived_bottle_start);
    RUN_TEST(test_rejected_configuration_echoes_values_and_the_rule_error);
    RUN_TEST(test_config_set_acknowledgement_repeats_the_held_configuration);
    RUN_TEST(test_config_set_refusal_names_the_rule_and_echoes_nothing);
    RUN_TEST(test_check_report_names_every_check);
    RUN_TEST(test_checks_failure_attribution_is_named_and_masked);
    RUN_TEST(test_announcement_refusals_carry_the_gate_that_refused);
    RUN_TEST(test_status_line_pins_the_control_state);
    RUN_TEST(test_status_line_reports_an_empty_vehicle);
    RUN_TEST(test_telemetry_responses_report_the_realized_state);
    RUN_TEST(test_version_line_reports_identity_and_schema);
    RUN_TEST(test_worst_case_responses_match_the_declared_bounds);
    RUN_TEST(test_a_response_that_does_not_fit_emits_nothing);
    RUN_TEST(test_help_table_lists_the_commands);
    return UNITY_END();
}
