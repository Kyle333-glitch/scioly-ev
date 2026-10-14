#include <cstdint>
#include <cstring>

#include <unity.h>

#include <TelemetryFormat.h>
#include <TelemetryRecord.h>
#include <VehicleState.h>

// R3/L2: operator-event time/fault attribution contract.
//
// Operator events used to carry the last control tick's clock and fault
// snapshot instead of the action's own clock and the live fault of record.
// The runtime fix stamps the command's `nowMs` and selects the live fault
// through `currentFaultOfRecord()`; these tests pin the selection precedence
// and the serialization compatibility the fix preserves (same `EVE3` fields,
// corrected values - no schema change).

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::TelemetryEvent makeOperatorEvent(uint32_t tMs, ev::FaultCode fault,
                                     ev::TelemetryEventCode code, const char* detail) {
    ev::TelemetryEvent event;
    event.tMs = tMs;
    event.phase = ev::RunPhase::Idle;
    event.armed = false;
    event.event = code;
    event.detail = detail;
    event.fault = fault;
    return event;
}

}  // namespace

// --- Fault selection ---------------------------------------------------------

void test_no_fault_anywhere_reports_none(void) {
    TEST_ASSERT_TRUE(ev::currentFaultOfRecord(ev::FaultCode::None, ev::FaultCode::None) ==
                     ev::FaultCode::None);
}

void test_a_run_fault_is_reported_when_only_the_run_holds_one(void) {
    TEST_ASSERT_TRUE(ev::currentFaultOfRecord(ev::FaultCode::PhaseTimeout,
                                              ev::FaultCode::None) ==
                     ev::FaultCode::PhaseTimeout);
}

void test_an_authority_fault_is_reported_when_only_the_authority_holds_one(void) {
    TEST_ASSERT_TRUE(ev::currentFaultOfRecord(ev::FaultCode::None,
                                              ev::FaultCode::DriverFaultBar) ==
                     ev::FaultCode::DriverFaultBar);
}

void test_the_run_fault_wins_when_both_owners_hold_one(void) {
    // Mirrors the tick's own precedence (`safetyState.detectedFault` prefers
    // the run's fault), so the event and the surrounding snapshot agree.
    TEST_ASSERT_TRUE(ev::currentFaultOfRecord(ev::FaultCode::PhaseTimeout,
                                              ev::FaultCode::DriverFaultBar) ==
                     ev::FaultCode::PhaseTimeout);
}

void test_a_cleared_run_fault_falls_back_to_the_authority_latch(void) {
    TEST_ASSERT_TRUE(ev::currentFaultOfRecord(ev::FaultCode::None,
                                              ev::FaultCode::RunDurationExceeded) ==
                     ev::FaultCode::RunDurationExceeded);
}

// --- Serialization compatibility -----------------------------------------------

void test_event_serialization_keeps_schema_fields_with_corrected_values(void) {
    // A command at 2001 ms with no live fault must serialize exactly these
    // corrected values - the fix changes values, never the schema.
    char buffer[256];
    const ev::TelemetryEvent event = makeOperatorEvent(2001U, ev::FaultCode::None,
                                                       ev::TelemetryEventCode::ChainReset, "-");
    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_EQUAL_STRING("EVE3,ver=3,t_ms=2001,phase=Idle,armed=0,event=ChainReset,detail=-,"
                             "fault=None,drop_s=0,drop_e=0",
                             buffer);
}

void test_a_cleared_fault_travels_in_detail_not_in_the_fault_field(void) {
    // After a reset the live fault is None; what was cleared stays readable
    // in `detail` while `fault` reports the current (clear) state.
    char buffer[256];
    const ev::TelemetryEvent event =
        makeOperatorEvent(2001U, ev::FaultCode::None, ev::TelemetryEventCode::ChainReset,
                          ev::telemetryFaultName(ev::FaultCode::PhaseTimeout));
    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_TRUE(std::strstr(buffer, "event=ChainReset") != nullptr);
    TEST_ASSERT_TRUE(std::strstr(buffer, "detail=PhaseTimeout") != nullptr);
    TEST_ASSERT_TRUE(std::strstr(buffer, "fault=None") != nullptr);
}

void test_event_timestamps_survive_uint32_rollover_verbatim(void) {
    // Wrap-safe by representation: the stamp is the action's clock reading,
    // copied, never differenced.
    char buffer[256];
    const ev::TelemetryEvent event = makeOperatorEvent(5U, ev::FaultCode::None,
                                                       ev::TelemetryEventCode::ChainReset, "-");
    TEST_ASSERT_TRUE(ev::formatTelemetryEvent(event, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_TRUE(std::strstr(buffer, "t_ms=5,") != nullptr);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_fault_anywhere_reports_none);
    RUN_TEST(test_a_run_fault_is_reported_when_only_the_run_holds_one);
    RUN_TEST(test_an_authority_fault_is_reported_when_only_the_authority_holds_one);
    RUN_TEST(test_the_run_fault_wins_when_both_owners_hold_one);
    RUN_TEST(test_a_cleared_run_fault_falls_back_to_the_authority_latch);
    RUN_TEST(test_event_serialization_keeps_schema_fields_with_corrected_values);
    RUN_TEST(test_a_cleared_fault_travels_in_detail_not_in_the_fault_field);
    RUN_TEST(test_event_timestamps_survive_uint32_rollover_verbatim);
    return UNITY_END();
}
