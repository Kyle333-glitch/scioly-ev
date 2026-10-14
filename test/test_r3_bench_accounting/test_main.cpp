#include <cstdint>

#include <unity.h>

#include <BenchLineAccounting.h>

// R3/L1: truthful bench-console command accounting.
//
// The health counters used to count only a subset of outcomes. This module is
// the centralized contract the dispatch records through; these tests pin that
// every answered line lands in exactly one category, that forwarded
// production lines are never double-counted as bench outcomes, and that the
// received total is derived rather than separately stored.

void setUp(void) {}
void tearDown(void) {}

void test_a_fresh_counter_counts_nothing(void) {
    const ev::BenchLineCounters counters;
    TEST_ASSERT_EQUAL_UINT32(0U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.forwarded);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.received());
}

void test_an_accepted_command_counts_once_as_accepted(void) {
    ev::BenchLineCounters counters;
    counters.record(ev::BenchLineOutcome::Accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.forwarded);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.received());
}

void test_a_refused_command_counts_once_as_refused(void) {
    ev::BenchLineCounters counters;
    counters.record(ev::BenchLineOutcome::Refused);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.forwarded);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.received());
}

void test_a_forwarded_command_is_not_a_bench_outcome(void) {
    // The production interface owns a forwarded line's outcome; counting it
    // here as accepted or refused would count one command twice.
    ev::BenchLineCounters counters;
    counters.record(ev::BenchLineOutcome::Forwarded);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(0U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.forwarded);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.received());
}

void test_a_mixed_session_adds_up_without_double_counting(void) {
    ev::BenchLineCounters counters;
    counters.record(ev::BenchLineOutcome::Accepted);   // bench arm
    counters.record(ev::BenchLineOutcome::Accepted);   // bench drive
    counters.record(ev::BenchLineOutcome::Refused);    // over-ceiling drive
    counters.record(ev::BenchLineOutcome::Accepted);   // bench health
    counters.record(ev::BenchLineOutcome::Forwarded);  // status
    counters.record(ev::BenchLineOutcome::Refused);    // unknown verb
    TEST_ASSERT_EQUAL_UINT32(3U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(2U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.forwarded);
    TEST_ASSERT_EQUAL_UINT32(6U, counters.received());
}

void test_repeated_commands_each_count(void) {
    ev::BenchLineCounters counters;
    counters.record(ev::BenchLineOutcome::Accepted);
    counters.record(ev::BenchLineOutcome::Refused);  // repeated arm while armed
    counters.record(ev::BenchLineOutcome::Refused);
    TEST_ASSERT_EQUAL_UINT32(1U, counters.accepted);
    TEST_ASSERT_EQUAL_UINT32(2U, counters.refused);
    TEST_ASSERT_EQUAL_UINT32(3U, counters.received());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_fresh_counter_counts_nothing);
    RUN_TEST(test_an_accepted_command_counts_once_as_accepted);
    RUN_TEST(test_a_refused_command_counts_once_as_refused);
    RUN_TEST(test_a_forwarded_command_is_not_a_bench_outcome);
    RUN_TEST(test_a_mixed_session_adds_up_without_double_counting);
    RUN_TEST(test_repeated_commands_each_count);
    return UNITY_END();
}
