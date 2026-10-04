#include <cstdint>
#include <cstring>
#include <limits>

#include <unity.h>

#include <TelemetryFormat.h>

// Contract tests for the telemetry formatter (lib/evcore/src/TelemetryFormat.h):
// the frozen field order and spelling, the absence markers, the "all or nothing"
// rule, and the derived worst-case line bound.
//
// These tests pin bytes, not behavior. A pass means the formatter produces the
// same characters for the same record on the host and on the board (the
// formatting is integer arithmetic behind one `floor()`), and that the bound the
// transport sizes its buffer from is the bound the tables actually produce. It
// says nothing about how the vehicle moves, and nothing about whether a serial
// port drained the bytes.

void setUp(void) {}
void tearDown(void) {}

namespace {

// The longest entry of each vocabulary table, measured by the test itself rather
// than assumed, so the worst-case derivation in `TelemetryFormat.h` is checked
// against the tables instead of against a copied comment.
const uint32_t kLongestPhaseNameChars = 14U;        // "ParameterValid"
const uint32_t kLongestCommandNameChars = 15U;      // "RampDownForward"
const uint32_t kLongestMotorModeNameChars = 12U;    // "DriveForward"
const uint32_t kLongestFaultNameChars = 22U;        // "ProvisionalCalibration"
const uint32_t kLongestAuthzNameChars = 27U;        // "DeniedUnsafeDirectionChange"
const uint32_t kLongestConfigErrorNameChars = 29U;  // "BottleLineDistanceOffInterval"
const uint32_t kLongestEventNameChars = 16U;        // "TelemetryEnabled"
const uint32_t kLongestCheckNameChars = 29U;        // "StopEvidenceSpansReversalStop"
const uint32_t kLongestDriverFaultCapabilityChars = 12U;  // "NotInstalled"
const uint32_t kLongestDriverFaultLevelChars = 1U;        // "-" when the line is absent

// Every vocabulary name is written into a comma-separated line and then split on
// `,` and the first `=` by the reader, so a name may contain neither a comma, a
// space nor an equals sign, and must be short enough that the bound holds.
void assertWireSafeName(const char* name) {
    TEST_ASSERT_NOT_NULL(name);
    const size_t length = strlen(name);
    TEST_ASSERT_TRUE_MESSAGE(length > 0U, "an empty vocabulary name would read as a missing field");
    TEST_ASSERT_TRUE_MESSAGE(length <= 30U, "a vocabulary name is longer than the bound assumes");
    for (size_t index = 0U; index < length; ++index) {
        TEST_ASSERT_TRUE(name[index] != ',');
        TEST_ASSERT_TRUE(name[index] != ' ');
        TEST_ASSERT_TRUE(name[index] != '=');
    }
}

size_t longerOf(size_t current, const char* name) {
    const size_t length = strlen(name);
    return length > current ? length : current;
}

// A sample with every field set to a distinct, easily recognizable value, so an
// accidental field swap shows up as a wrong byte rather than as a wrong number
// somewhere.
ev::TelemetrySample recognizableSample() {
    ev::TelemetrySample sample;
    sample.tMs = 5832U;
    sample.dtMs = 20U;
    sample.phase = ev::RunPhase::Approach;
    sample.command = ev::RunMotionCommand::DriveForward;
    sample.armed = true;
    sample.positionM = 6.214;
    sample.positionUsable = true;
    sample.measuredVelocityMps = 0.714;
    sample.velocityUsable = true;
    sample.requestedVelocityMps = 0.7;
    sample.remainingDistanceM = 0.286;
    sample.motorMode = ev::MotorMode::DriveForward;
    sample.effortPermille = 740U;
    sample.batteryCounts = 812U;
    sample.currentCounts = 513U;
    sample.driverFaultCapability = ev::DriverFaultCapability::Installed;
    sample.driverFaultBarLow = false;
    sample.fault = ev::FaultCode::None;
    sample.outcome = ev::AuthzOutcome::AuthorizedDrive;
    sample.firstFailedCheck = ev::ConfigurationCheckId::Count;
    return sample;
}

// The sample built from the longest entry of every table, which is what the
// declared worst-case length must be.
ev::TelemetrySample widestSample() {
    ev::TelemetrySample sample;
    sample.tMs = 0xFFFFFFFFu;
    sample.dtMs = 65535U;
    sample.phase = ev::RunPhase::ParameterValid;
    sample.command = ev::RunMotionCommand::RampDownForward;
    sample.armed = true;
    sample.positionM = -9999999.9999;
    sample.positionUsable = false;
    sample.measuredVelocityMps = -9999999.9999;
    sample.velocityUsable = false;
    sample.requestedVelocityMps = -9999999.9999;
    sample.remainingDistanceM = -9999999.9999;
    sample.motorMode = ev::MotorMode::DriveForward;
    sample.effortPermille = 65535U;
    sample.batteryCounts = 65535U;
    sample.currentCounts = 65535U;
    // The widest spelling of the capability field, and the level field at its
    // widest (a boolean, one character - the absent marker is the same width).
    sample.driverFaultCapability = ev::DriverFaultCapability::NotInstalled;
    sample.driverFaultBarLow = true;
    sample.fault = ev::FaultCode::ProvisionalCalibration;
    sample.outcome = ev::AuthzOutcome::DeniedUnsafeDirectionChange;
    sample.firstFailedCheck = ev::ConfigurationCheckId::StopEvidenceSpansReversalStop;
    sample.drops.samples = 0xFFFFFFFFu;
    sample.drops.events = 0xFFFFFFFFu;
    return sample;
}

ev::TelemetryEvent widestEvent() {
    ev::TelemetryEvent event;
    event.tMs = 0xFFFFFFFFu;
    event.phase = ev::RunPhase::ParameterValid;
    event.armed = true;
    event.event = ev::TelemetryEventCode::TelemetryEnabled;
    event.detail = "BottleLineDistanceOffInterval";
    event.fault = ev::FaultCode::ProvisionalCalibration;
    event.drops.samples = 0xFFFFFFFFu;
    event.drops.events = 0xFFFFFFFFu;
    return event;
}

}  // namespace

// The whole sample schema in one assertion: every field, in order, spelled as the
// schema froze it, with the units in the name. If this test fails, an analysis
// script that reads an older log would misread a column, so the failure is the
// point - a rename must bump the schema version deliberately.
void test_sample_line_pins_field_order_and_spelling(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    const ev::TelemetrySample sample = recognizableSample();

    const uint16_t length = ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity);

    TEST_ASSERT_EQUAL_STRING(
        "EVS2,ver=2,t_ms=5832,dt_ms=20,phase=Approach,cmd=DriveForward,armed=1,"
        "pos_m=6.2140,pos_ok=1,vel_mps=0.7140,vel_ok=1,vel_cmd_mps=0.7000,rem_m=0.2860,"
        "motor=DriveForward,effort_permille=740,batt_counts=812,curr_counts=513,"
        "nfault=Installed,nfault_low=0,"
        "fault=None,authz=AuthorizedDrive,check=none,drop_s=0,drop_e=0",
        buffer);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), length);
}

// The event schema, likewise. `detail` carries the fault name for a
// `FaultLatched` event; the field order is the one the schema froze.
void test_event_line_pins_field_order_and_spelling(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetryEvent event;
    event.tMs = 1200U;
    event.phase = ev::RunPhase::Approach;
    event.armed = true;
    event.event = ev::TelemetryEventCode::FaultLatched;
    event.detail = ev::telemetryFaultName(ev::FaultCode::ProvisionalCalibration);
    event.fault = ev::FaultCode::ProvisionalCalibration;
    event.drops.samples = 3U;
    event.drops.events = 1U;

    const uint16_t length = ev::formatTelemetryEvent(event, buffer, ev::kTelemetryLineCapacity);

    TEST_ASSERT_EQUAL_STRING(
        "EVE2,ver=2,t_ms=1200,phase=Approach,armed=1,event=FaultLatched,"
        "detail=ProvisionalCalibration,fault=ProvisionalCalibration,drop_s=3,drop_e=1",
        buffer);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), length);
}

// An event without a detail is still a complete record: the field is present and
// carries the missing marker, so a trailing comma can never be read as a value
// and a reader can count columns without special cases.
void test_absent_detail_is_the_missing_marker(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetryEvent event;
    event.event = ev::TelemetryEventCode::Disarmed;

    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",detail=-,"));

    event.detail = "";
    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",detail=-,"));

    // The marker itself is what the default carries.
    ev::TelemetryEvent defaulted;
    TEST_ASSERT_EQUAL_STRING("-", defaulted.detail);
}

// A sample is never missing a field, because every field is a value the control
// path produced. An unusable position is still written, next to the flag that
// says so.
void test_sample_always_carries_every_field(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetrySample sample;
    sample.positionUsable = false;
    sample.velocityUsable = false;

    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",pos_ok=0,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",vel_ok=0,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",check=none,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",fault=None,"));
    // The missing-field marker is a one-character string, written into a field
    // exactly like a name would be.
    TEST_ASSERT_EQUAL_STRING("-", ev::kTelemetryMissingField);
}

// A bad measurement must not widen a record, and it must not silently become a
// plausible number: the sentinels are fixed spellings a reader can search for.
void test_non_finite_and_out_of_range_values_use_fixed_sentinels(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetrySample sample;
    sample.positionM = std::numeric_limits<double>::quiet_NaN();
    sample.measuredVelocityMps = std::numeric_limits<double>::infinity();
    sample.requestedVelocityMps = -std::numeric_limits<double>::infinity();
    sample.remainingDistanceM = 1.0e7;  // exactly the sentinel threshold

    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",pos_m=nan,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",vel_mps=inf,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",vel_cmd_mps=-inf,"));
    // The threshold is inclusive: 1e7 is already `ovf`, and just below it is a
    // number.
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",rem_m=ovf,"));

    sample.remainingDistanceM = 9999999.9999;
    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",rem_m=9999999.9999,"));
}

// A value too small to round to a tenth of a millimetre prints as zero, never as
// `-0`: a log full of `-0.0000` reads like a sign convention that does not exist.
void test_negative_value_rounding_to_zero_has_no_sign(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetrySample sample;
    sample.positionM = -0.00001;

    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",pos_m=0.0000,"));
    TEST_ASSERT_NULL(strstr(buffer, "-0.0000"));
}

// Every name that can reach a line is checked against the property the line
// format depends on, and the longest entry of each table is asserted: that
// number is what the worst-case derivation in the header is built from.
void test_vocabulary_names_are_wire_safe_and_measured(void) {
    size_t longestPhase = 0U;
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::RunPhase::Fault); ++index) {
        const char* name = ev::telemetryPhaseName(static_cast<ev::RunPhase>(index));
        assertWireSafeName(name);
        longestPhase = longerOf(longestPhase, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestPhaseNameChars, static_cast<uint32_t>(longestPhase));

    size_t longestCommand = 0U;
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::RunMotionCommand::BrakeReverse);
         ++index) {
        const char* name = ev::telemetryRunCommandName(static_cast<ev::RunMotionCommand>(index));
        assertWireSafeName(name);
        longestCommand = longerOf(longestCommand, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestCommandNameChars, static_cast<uint32_t>(longestCommand));

    size_t longestMotorMode = 0U;
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::MotorMode::DriveReverse); ++index) {
        const char* name = ev::telemetryMotorModeName(static_cast<ev::MotorMode>(index));
        assertWireSafeName(name);
        longestMotorMode = longerOf(longestMotorMode, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestMotorModeNameChars, static_cast<uint32_t>(longestMotorMode));

    size_t longestFault = 0U;
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::FaultCode::RunSequencingError);
         ++index) {
        const char* name = ev::telemetryFaultName(static_cast<ev::FaultCode>(index));
        assertWireSafeName(name);
        longestFault = longerOf(longestFault, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestFaultNameChars, static_cast<uint32_t>(longestFault));

    size_t longestAuthz = 0U;
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::AuthzOutcome::DeniedUnsafeDirectionChange); ++index) {
        const char* name = ev::telemetryAuthzOutcomeName(static_cast<ev::AuthzOutcome>(index));
        assertWireSafeName(name);
        longestAuthz = longerOf(longestAuthz, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestAuthzNameChars, static_cast<uint32_t>(longestAuthz));

    size_t longestConfigError = 0U;
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::ConfigError::BottleStartNotLevelDistance); ++index) {
        const char* name = ev::telemetryConfigErrorName(static_cast<ev::ConfigError>(index));
        assertWireSafeName(name);
        longestConfigError = longerOf(longestConfigError, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestConfigErrorNameChars,
                             static_cast<uint32_t>(longestConfigError));

    size_t longestEvent = 0U;
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::TelemetryEventCode::CommandRejected);
         ++index) {
        const char* name = ev::telemetryEventName(static_cast<ev::TelemetryEventCode>(index));
        assertWireSafeName(name);
        longestEvent = longerOf(longestEvent, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestEventNameChars, static_cast<uint32_t>(longestEvent));

    size_t longestCheck = 0U;
    for (uint8_t index = 0U; index < ev::kConfigurationCheckCount; ++index) {
        const char* name = ev::configurationCheckName(static_cast<ev::ConfigurationCheckId>(index));
        assertWireSafeName(name);
        longestCheck = longerOf(longestCheck, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestCheckNameChars, static_cast<uint32_t>(longestCheck));
    // The "no failing check" spelling is not a check name but reaches the same
    // field, so it is checked too.
    assertWireSafeName(ev::configurationCheckName(ev::ConfigurationCheckId::Count));
}

// The declared worst case is the real one: a record built from the longest entry
// of every table formats to exactly the number the header declares, so any future
// entry that grows a field breaks this test rather than the capacity assumption.
void test_worst_case_lines_match_the_declared_bound(void) {
    char buffer[ev::kTelemetryLineBufferBytes];

    const uint16_t sampleLength =
        ev::formatTelemetrySample(widestSample(), buffer, ev::kTelemetryLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kTelemetrySampleWorstCaseChars, sampleLength);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), sampleLength);

    const uint16_t eventLength =
        ev::formatTelemetryEvent(widestEvent(), buffer, ev::kTelemetryLineCapacity);
    TEST_ASSERT_EQUAL_UINT32(ev::kTelemetryEventWorstCaseChars, eventLength);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(strlen(buffer)), eventLength);
}

// A record that does not fit is dropped whole. A shortened line would silently
// lose the tail - the fault, the authorization outcome, the failing check, the
// drop counters - and a reader could not tell a missing column from an
// unremarkable one.
void test_a_record_that_does_not_fit_emits_nothing(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    buffer[0] = 'x';
    buffer[1] = '\0';
    const ev::TelemetrySample sample = recognizableSample();

    // Room for the prefix and a few fields: enough to look plausible, not enough
    // for a line.
    const uint16_t length = ev::formatTelemetrySample(sample, buffer, 60U);

    TEST_ASSERT_EQUAL_UINT16(ev::kTelemetryFormatDidNotFit, length);
    TEST_ASSERT_EQUAL_STRING("", buffer);

    // Exactly the declared capacity is enough; one character less is not.
    TEST_ASSERT_TRUE(ev::formatTelemetrySample(widestSample(), buffer,
                                               ev::kTelemetrySampleWorstCaseChars) > 0U);
    TEST_ASSERT_EQUAL_UINT16(ev::kTelemetryFormatDidNotFit,
                             ev::formatTelemetrySample(widestSample(), buffer,
                                                       ev::kTelemetrySampleWorstCaseChars - 1U));
    TEST_ASSERT_EQUAL_STRING("", buffer);
}

// The external nFAULT line is not installed on this vehicle, and a log may never
// spell that as a healthy level. The two fields are written together: `nfault`
// says the signal does not exist, and `nfault_low` carries the absent marker
// rather than `0`.
void test_absent_driver_fault_line_is_not_spelled_as_a_healthy_level(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetrySample sample;
    sample.driverFaultCapability = ev::DriverFaultCapability::NotInstalled;
    // The value the vehicle layer holds for the level is whatever the unread
    // field defaults to; the point is that the capability decides the spelling,
    // not the level. Both boolean states must produce the same marker.
    sample.driverFaultBarLow = false;

    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",nfault=NotInstalled,"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",nfault_low=-,"));
    TEST_ASSERT_NULL(strstr(buffer, "nfault_low=0"));
    TEST_ASSERT_NULL(strstr(buffer, "nfault_low=1"));

    sample.driverFaultBarLow = true;
    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",nfault_low=-,"));
    TEST_ASSERT_NULL(strstr(buffer, "nfault_low=0"));
    TEST_ASSERT_NULL(strstr(buffer, "nfault_low=1"));

    // A vehicle that has the line reports its capability and a real level, so
    // the marker cannot be read as "no line exists" when one does.
    sample.driverFaultCapability = ev::DriverFaultCapability::Installed;
    sample.driverFaultBarLow = false;
    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",nfault=Installed,nfault_low=0,"));

    sample.driverFaultBarLow = true;
    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",nfault=Installed,nfault_low=1,"));
}

// The capability vocabulary is wire-safe and its longest entry is the one the
// worst-case derivation above uses.
void test_driver_fault_capability_names_are_wire_safe(void) {
    size_t longest = 0U;
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::DriverFaultCapability::NotInstalled); ++index) {
        const char* name = ev::driverFaultCapabilityName(static_cast<ev::DriverFaultCapability>(index));
        assertWireSafeName(name);
        longest = longerOf(longest, name);
    }
    TEST_ASSERT_EQUAL_UINT32(kLongestDriverFaultCapabilityChars, static_cast<uint32_t>(longest));

    // The level field's two spellings: a one-character boolean, or the absent
    // marker. Neither widens the record beyond what the bound assumes.
    TEST_ASSERT_EQUAL_STRING("-", ev::telemetryDriverFaultLevelName(
                                      ev::DriverFaultCapability::NotInstalled, true));
    TEST_ASSERT_EQUAL_STRING("0", ev::telemetryDriverFaultLevelName(
                                      ev::DriverFaultCapability::Installed, false));
    TEST_ASSERT_EQUAL_STRING("1", ev::telemetryDriverFaultLevelName(
                                      ev::DriverFaultCapability::Installed, true));
    TEST_ASSERT_EQUAL_UINT32(kLongestDriverFaultLevelChars,
                             static_cast<uint32_t>(strlen(ev::kTelemetryMissingField)));
}

// The prefix's digit and the `ver` field are the same version, and both come from
// the one constant. A reader that trusts either one learns the same thing.
void test_prefix_and_version_field_agree(void) {
    TEST_ASSERT_EQUAL_UINT8(ev::kTelemetrySchemaVersion,
                            static_cast<uint8_t>(ev::kTelemetrySamplePrefix[3] - '0'));
    TEST_ASSERT_EQUAL_UINT8(ev::kTelemetrySchemaVersion,
                            static_cast<uint8_t>(ev::kTelemetryEventPrefix[3] - '0'));
    TEST_ASSERT_EQUAL_UINT16(4U, static_cast<uint16_t>(strlen(ev::kTelemetrySamplePrefix)));
    TEST_ASSERT_EQUAL_UINT16(4U, static_cast<uint16_t>(strlen(ev::kTelemetryEventPrefix)));

    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetrySample sample;

    TEST_ASSERT_TRUE(ev::formatTelemetrySample(sample, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buffer, "EVS2,ver=2,", 11U));

    ev::TelemetryEvent event;
    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buffer, "EVE2,ver=2,", 11U));
}

// The drain loop hands the formatter whatever the queue popped, so the dispatch
// has to follow the record's kind rather than a caller's assumption.
void test_record_formatting_dispatches_on_kind(void) {
    char buffer[ev::kTelemetryLineBufferBytes];
    ev::TelemetryRecord record;
    record.kind = ev::TelemetryRecordKind::Sample;
    record.sample = recognizableSample();
    record.event.tMs = 99U;

    TEST_ASSERT_TRUE(ev::formatTelemetryRecord(record, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buffer, "EVS2,", 5U));

    record.kind = ev::TelemetryRecordKind::Event;
    TEST_ASSERT_TRUE(ev::formatTelemetryRecord(record, buffer, ev::kTelemetryLineCapacity) > 0U);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buffer, "EVE2,", 5U));
    TEST_ASSERT_NOT_NULL(strstr(buffer, ",t_ms=99,"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sample_line_pins_field_order_and_spelling);
    RUN_TEST(test_event_line_pins_field_order_and_spelling);
    RUN_TEST(test_absent_detail_is_the_missing_marker);
    RUN_TEST(test_sample_always_carries_every_field);
    RUN_TEST(test_non_finite_and_out_of_range_values_use_fixed_sentinels);
    RUN_TEST(test_negative_value_rounding_to_zero_has_no_sign);
    RUN_TEST(test_vocabulary_names_are_wire_safe_and_measured);
    RUN_TEST(test_worst_case_lines_match_the_declared_bound);
    RUN_TEST(test_a_record_that_does_not_fit_emits_nothing);
    RUN_TEST(test_prefix_and_version_field_agree);
    RUN_TEST(test_record_formatting_dispatches_on_kind);
    RUN_TEST(test_absent_driver_fault_line_is_not_spelled_as_a_healthy_level);
    RUN_TEST(test_driver_fault_capability_names_are_wire_safe);
    return UNITY_END();
}
