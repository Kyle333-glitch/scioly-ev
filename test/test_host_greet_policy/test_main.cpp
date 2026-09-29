#include <unity.h>

#include <HostGreetPolicy.h>

// Contract tests for the host greeting decision (lib/evcore/src/HostGreetPolicy.h):
// which observations owe the boot greeting, that a greeting once delivered is not
// owed again for the same reader, and that host silence re-owes it at the silence
// cadence rather than once.
//
// A pass means the firmware greets a reader it can actually see. It says nothing
// about whether a write can leave the port - that is the transport's own question
// - and nothing about a host that presents no control-line edge and never writes,
// which no host-side signal can distinguish (H0 finding F-4).

void setUp(void) {}
void tearDown(void) {}

// The silence interval as the vehicle layer uses it, so a test that steps past it
// is testing the boundary rather than a number copied out of the header.
constexpr uint32_t kSilence = ev::HostGreetPolicy::kSilenceMs;

// A fresh policy has observed nothing, so it owes nothing.
void test_nothing_is_owed_until_something_is_observed(void) {
    ev::HostGreetPolicy greeter;

    TEST_ASSERT_FALSE(greeter.owed());
    greeter.observeClock(0U);
    TEST_ASSERT_FALSE(greeter.owed());
}

// An explicit request is owed immediately: this is the `boot` command, and the
// vehicle's own first line at initialization.
void test_a_requested_greeting_is_owed_immediately(void) {
    ev::HostGreetPolicy greeter;

    greeter.requestGreeting(1234U);
    TEST_ASSERT_TRUE(greeter.owed());
}

// A greeting that went out is not owed again for the reader that got it. A level
// that is merely still asserted is not a new attach, which is what keeps a
// long-running capture to one greeting.
void test_a_delivered_greeting_is_not_owed_again_for_the_same_reader(void) {
    ev::HostGreetPolicy greeter;

    greeter.observeLines(true, false, 100U);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(100U);
    TEST_ASSERT_FALSE(greeter.owed());

    greeter.observeLines(true, false, 200U);
    greeter.observeLines(true, false, 300U);
    TEST_ASSERT_FALSE(greeter.owed());
}

// A control line that newly goes high is a host opening the port. Either line is
// an attach: DTR alone can be unchanged across a genuinely new reader (F-4).
void test_a_newly_asserted_control_line_owes_the_greeting(void) {
    ev::HostGreetPolicy greeter;

    greeter.observeLines(true, false, 100U);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(100U);

    // The host goes away, then comes back on RTS alone.
    greeter.observeLines(false, false, 200U);
    TEST_ASSERT_FALSE(greeter.owed());
    greeter.observeLines(false, true, 300U);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(300U);

    // Both lines drop and DTR returns: an attach even though RTS stayed low.
    greeter.observeLines(false, false, 400U);
    greeter.observeLines(true, false, 500U);
    TEST_ASSERT_TRUE(greeter.owed());
}

// A byte from the host restarts the silence interval, so an operator typing
// commands is one session and is greeted once rather than once per command.
void test_a_host_byte_restarts_the_silence_interval(void) {
    ev::HostGreetPolicy greeter;

    greeter.requestGreeting(0U);
    greeter.delivered(0U);

    greeter.observeHostByte(4U * kSilence / 5U);
    greeter.observeClock(kSilence - 1U);
    TEST_ASSERT_FALSE(greeter.owed());

    greeter.observeHostByte(kSilence);
    greeter.observeClock(2U * kSilence - 1U);
    TEST_ASSERT_FALSE(greeter.owed());

    greeter.observeClock(2U * kSilence);
    TEST_ASSERT_TRUE(greeter.owed());
}

// Silence is the one attach no control-line edge can report, so it is checked at
// its boundary and it repeats while the silence continues: a reader that opens
// the port without moving either line is still greeted, and greeted again after
// each interval rather than only once.
void test_silence_owes_the_greeting_at_the_boundary_and_repeats(void) {
    ev::HostGreetPolicy greeter;

    greeter.requestGreeting(0U);
    greeter.delivered(0U);

    greeter.observeClock(kSilence - 1U);
    TEST_ASSERT_FALSE(greeter.owed());

    greeter.observeClock(kSilence);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(kSilence);

    // A second, a third interval: each one owes the greeting again.
    greeter.observeClock(2U * kSilence - 1U);
    TEST_ASSERT_FALSE(greeter.owed());
    greeter.observeClock(2U * kSilence);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(2U * kSilence);

    greeter.observeClock(3U * kSilence);
    TEST_ASSERT_TRUE(greeter.owed());
}

// The two inputs are independent: an attach edge is noticed while the silence
// clock keeps its own state, and a byte neither suppresses nor fabricates an
// edge.
void test_attach_edges_and_the_silence_clock_are_independent(void) {
    ev::HostGreetPolicy greeter;

    greeter.observeLines(true, true, 10U);
    TEST_ASSERT_TRUE(greeter.owed());
    greeter.delivered(10U);

    // Bytes keep the silence clock quiet, so no silent deadline is reached...
    greeter.observeHostByte(11U);
    greeter.observeLines(false, false, 12U);
    greeter.observeClock(10U + kSilence - 1U);
    TEST_ASSERT_FALSE(greeter.owed());

    // ...and a line that goes high still owes the greeting regardless.
    greeter.observeLines(false, true, 10U + kSilence - 1U);
    TEST_ASSERT_TRUE(greeter.owed());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_owed_until_something_is_observed);
    RUN_TEST(test_a_requested_greeting_is_owed_immediately);
    RUN_TEST(test_a_delivered_greeting_is_not_owed_again_for_the_same_reader);
    RUN_TEST(test_a_newly_asserted_control_line_owes_the_greeting);
    RUN_TEST(test_a_host_byte_restarts_the_silence_interval);
    RUN_TEST(test_silence_owes_the_greeting_at_the_boundary_and_repeats);
    RUN_TEST(test_attach_edges_and_the_silence_clock_are_independent);
    return UNITY_END();
}
