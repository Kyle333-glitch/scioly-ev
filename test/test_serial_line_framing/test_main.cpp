#include <cstdint>
#include <cstring>
#include <string>

#include <unity.h>

#include <BoundedText.h>
#include <SerialLineFraming.h>

// The framing contract the serial transport depends on (audit M2/B5, S15).
//
// The defect these cases exist for: the old transport shared ONE buffer between
// the line being formatted and the line being sent, so an urgent operator
// response could overwrite a telemetry line whose first bytes had already left
// the port. The host then read a telemetry prefix and a reply as one record with
// no terminator between them - a malformed line whose two halves belonged to
// different records - and an earlier reply could be silently transformed into a
// different one.
//
// `ev::LineFramer` is the pure module that decides what may replace what. These
// cases drive it with a mock port that can accept partial writes, refuse writes
// while reporting room, and accept nothing at all - the three behaviors
// `Serial.write` on this board can present - and assert the byte stream, not a
// summary of it: every case compares the exact bytes a host would have received.
//
// Nothing here is a hardware test. It says the framing decision is coherent; it
// says nothing about a USB port, a cable or a host's reader.

void setUp(void) {}
void tearDown(void) {}

namespace {

// A controllable write port in the shape `ev::LineFramer::pump()` expects.
struct MockPort {
    int room = 4096;
    // The most one `write()` call accepts, regardless of the room reported.
    size_t maxPerWrite = 4096;
    // When true, `write()` reports room but accepts nothing: the "cannot assume
    // write() sends every byte" case.
    bool refuseWrites = false;
    // The most this port ever accepts, so a caller can stop pumping mid-line.
    size_t totalAccepted = 0;
    size_t accepted = 0;
    std::string out;

    int availableForWrite() { return room; }

    size_t write(const uint8_t* bytes, size_t count) {
        if (refuseWrites || count == 0U) {
            return 0U;
        }
        size_t writable = count;
        if (writable > maxPerWrite) {
            writable = maxPerWrite;
        }
        if (totalAccepted > 0U) {
            if (accepted >= totalAccepted) {
                return 0U;
            }
            const size_t remaining = totalAccepted - accepted;
            if (writable > remaining) {
                writable = remaining;
            }
        }
        if (writable == 0U) {
            return 0U;
        }
        out.append(reinterpret_cast<const char*>(bytes), writable);
        accepted += writable;
        return writable;
    }
};

// Formats a line into the framer's working buffer the way every production
// caller does: compose into the buffer, then stage the composed length.
bool stageLine(ev::LineFramer& framer, const char* text, bool urgent) {
    const size_t length = std::strlen(text);
    char* buffer = framer.workingBuffer();
    std::memcpy(buffer, text, length);
    return framer.stage(static_cast<uint16_t>(length), urgent);
}

const std::string kTerminator = "\r\n";

}  // namespace

// A clean line leaves as itself, terminator included, and moves as one pump call
// when the port accepts it.
void test_a_clean_line_moves_whole_with_its_terminator(void) {
    ev::LineFramer framer;
    MockPort port;

    TEST_ASSERT_TRUE(stageLine(framer, "EVS3,t_ms=20", false));
    TEST_ASSERT_TRUE(framer.busy());
    TEST_ASSERT_EQUAL_UINT32(0U, framer.linesSent());
    TEST_ASSERT_TRUE(framer.pump(port));

    TEST_ASSERT_EQUAL_STRING("EVS3,t_ms=20\r\n", port.out.c_str());
    TEST_ASSERT_EQUAL_UINT32(1U, framer.linesSent());
    TEST_ASSERT_EQUAL_UINT32(1U, framer.linesStaged());
    TEST_ASSERT_FALSE(framer.busy());
    TEST_ASSERT_EQUAL_UINT16(0U, framer.pendingBytes());
}

// A short write resumes from where it stopped; the host still receives exactly
// one record, in order.
void test_a_partial_write_resumes_and_never_reorders(void) {
    ev::LineFramer framer;
    MockPort port;
    port.maxPerWrite = 3U;

    TEST_ASSERT_TRUE(stageLine(framer, "ABCDEFGHIJ", false));
    // 13 bytes total (10 + terminator), 3 at a time.
    while (!framer.pump(port)) {
        TEST_ASSERT_TRUE(port.out.size() < 13U);
    }

    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ\r\n", port.out.c_str());
    TEST_ASSERT_EQUAL_UINT32(1U, framer.linesSent());
}

// A port that reports no room leaves the line staged - nothing is lost, nothing
// is dropped - and the line moves when room appears.
void test_a_port_that_accepts_nothing_stages_until_it_can(void) {
    ev::LineFramer framer;
    MockPort port;
    port.room = 0;

    TEST_ASSERT_TRUE(stageLine(framer, "OK,cmd=disarm", true));
    TEST_ASSERT_FALSE(framer.pump(port));
    TEST_ASSERT_EQUAL_UINT32(0U, port.out.size());
    // 13 characters plus the two-byte terminator.
    TEST_ASSERT_EQUAL_UINT32(15U, framer.pendingBytes());
    TEST_ASSERT_TRUE(framer.busy());

    port.room = 4096;
    TEST_ASSERT_TRUE(framer.pump(port));
    TEST_ASSERT_EQUAL_STRING("OK,cmd=disarm\r\n", port.out.c_str());
}

// A port can report room and then accept nothing. The framer may not assume the
// write moved bytes: the line stays staged, intact, for a later pump.
void test_a_zero_write_with_reported_room_keeps_the_line_staged(void) {
    ev::LineFramer framer;
    MockPort port;
    port.refuseWrites = true;

    TEST_ASSERT_TRUE(stageLine(framer, "STATUS,phase=Idle", false));
    TEST_ASSERT_FALSE(framer.pump(port));
    TEST_ASSERT_EQUAL_UINT32(0U, port.out.size());
    // 17 characters plus the two-byte terminator.
    TEST_ASSERT_EQUAL_UINT32(19U, framer.pendingBytes());

    port.refuseWrites = false;
    TEST_ASSERT_TRUE(framer.pump(port));
    TEST_ASSERT_EQUAL_STRING("STATUS,phase=Idle\r\n", port.out.c_str());
}

// The defect itself, as a byte-stream assertion: an urgent response staged while
// a telemetry line is partially emitted goes out AFTER that line's terminator.
// The host receives two well-formed records, never one merged one.
void test_an_urgent_line_never_joins_a_partially_emitted_record(void) {
    ev::LineFramer framer;
    MockPort port;
    port.totalAccepted = 20U;  // stop the first line mid-body

    const char* telemetry = "EVS3,t_ms=100,phase=Approach,pos_m=1.2345";
    TEST_ASSERT_TRUE(stageLine(framer, telemetry, false));
    TEST_ASSERT_FALSE(framer.pump(port));
    const std::string firstBytes = port.out;
    TEST_ASSERT_EQUAL_UINT32(20U, static_cast<uint32_t>(firstBytes.size()));
    TEST_ASSERT_EQUAL_STRING(std::string(telemetry).substr(0U, 20U).c_str(), firstBytes.c_str());
    TEST_ASSERT_TRUE(framer.busy());

    // The formatter composes the reply and stages it urgently while the telemetry
    // line is still moving.
    TEST_ASSERT_TRUE(stageLine(framer, "OK,cmd=disarm", true));

    // The reply cannot have displaced the in-flight bytes: they are still exactly
    // the telemetry line's prefix, and the pending bytes are the rest of it plus
    // the whole reply.
    TEST_ASSERT_EQUAL_UINT32(20U, static_cast<uint32_t>(port.out.size()));
    TEST_ASSERT_EQUAL_STRING(std::string(telemetry).substr(0U, 20U).c_str(), port.out.c_str());
    const uint16_t expectedPending = static_cast<uint16_t>(
        std::strlen(telemetry) - 20U + 2U + std::strlen("OK,cmd=disarm") + 2U);
    TEST_ASSERT_EQUAL_UINT16(expectedPending, framer.pendingBytes());

    // Let the port take everything. The stream must be exactly two records.
    port.totalAccepted = 0U;
    while (!framer.pump(port)) {
    }
    TEST_ASSERT_EQUAL_STRING("EVS3,t_ms=100,phase=Approach,pos_m=1.2345\r\nOK,cmd=disarm\r\n",
                             port.out.c_str());
    TEST_ASSERT_EQUAL_UINT32(2U, framer.linesSent());
}

// Formatting a new line while one is in flight must not touch the in-flight
// bytes: the working buffer is the other slot.
void test_the_working_buffer_never_touches_the_transmitting_line(void) {
    ev::LineFramer framer;
    MockPort port;
    port.totalAccepted = 10U;

    const char* active = "EVS3,t_ms=20,phase=Approach";
    TEST_ASSERT_TRUE(stageLine(framer, active, false));
    TEST_ASSERT_FALSE(framer.pump(port));
    TEST_ASSERT_EQUAL_UINT32(10U, port.out.size());

    // A formatter composes a long reply and then the stage is refused (telemetry
    // is not urgent): the composed bytes went to the other slot, so the active
    // line's unsent remainder is untouched.
    char* scratch = framer.workingBuffer();
    std::memset(scratch, 'Z', 40U);
    TEST_ASSERT_FALSE(framer.stage(40U, false));
    TEST_ASSERT_EQUAL_UINT32(1U, framer.telemetryLinesRefused());

    port.totalAccepted = 0U;
    while (!framer.pump(port)) {
    }
    TEST_ASSERT_EQUAL_STRING("EVS3,t_ms=20,phase=Approach\r\n", port.out.c_str());
}

// The bounded urgent policy: with one line in flight, an urgent reply waits; a
// second urgent reply replaces the *waiting* one (which has emitted nothing) and
// the displacement is counted. The active line is still whole.
void test_a_waiting_urgent_is_replaced_by_a_newer_one_and_counted(void) {
    ev::LineFramer framer;
    MockPort port;
    port.totalAccepted = 5U;

    TEST_ASSERT_TRUE(stageLine(framer, "EVS3,aaaa", false));
    TEST_ASSERT_FALSE(framer.pump(port));

    TEST_ASSERT_TRUE(stageLine(framer, "OK,cmd=arm", true));
    TEST_ASSERT_EQUAL_UINT32(0U, framer.linesAbandoned());
    TEST_ASSERT_TRUE(stageLine(framer, "OK,cmd=status", true));
    TEST_ASSERT_EQUAL_UINT32(1U, framer.linesAbandoned());

    port.totalAccepted = 0U;
    while (!framer.pump(port)) {
    }
    TEST_ASSERT_EQUAL_STRING("EVS3,aaaa\r\nOK,cmd=status\r\n", port.out.c_str());
    TEST_ASSERT_EQUAL_UINT32(2U, framer.linesSent());
    TEST_ASSERT_EQUAL_UINT32(3U, framer.linesStaged());
}

// Telemetry never displaces anything: a non-urgent line is refused while a line
// is active or waiting, and the refusal is counted.
void test_telemetry_is_refused_while_a_line_is_active_or_waiting(void) {
    ev::LineFramer framer;
    MockPort port;
    port.totalAccepted = 4U;

    TEST_ASSERT_TRUE(stageLine(framer, "EVS3,one", false));
    TEST_ASSERT_FALSE(framer.pump(port));

    TEST_ASSERT_TRUE(stageLine(framer, "OK,cmd=disarm", true));
    // A telemetry caller checks `busy()` before it composes anything (the drain
    // does exactly that), so this is the defensive path: the stage itself refuses
    // and nothing is displaced. It is asserted at `stage()` directly because
    // composing a line is what retires a waiting one - and no telemetry caller
    // composes while busy.
    TEST_ASSERT_FALSE(framer.stage(3U, false));
    TEST_ASSERT_FALSE(framer.stage(3U, false));
    TEST_ASSERT_EQUAL_UINT32(2U, framer.telemetryLinesRefused());
    TEST_ASSERT_EQUAL_UINT32(0U, framer.linesAbandoned());

    port.totalAccepted = 0U;
    while (!framer.pump(port)) {
    }
    TEST_ASSERT_EQUAL_STRING("EVS3,one\r\nOK,cmd=disarm\r\n", port.out.c_str());
}

// A length beyond the buffer is refused and counted; a formatter that overflowed
// cannot move bytes the framer did not store.
void test_oversize_lines_are_refused_and_counted(void) {
    ev::LineFramer framer;
    MockPort port;

    TEST_ASSERT_FALSE(framer.stage(static_cast<uint16_t>(ev::kSerialLineCapacity + 1U), false));
    TEST_ASSERT_FALSE(framer.stage(static_cast<uint16_t>(ev::kSerialLineCapacity + 1U), true));
    TEST_ASSERT_EQUAL_UINT32(2U, framer.oversizeLinesRefused());
    TEST_ASSERT_EQUAL_UINT32(0U, framer.linesStaged());
    TEST_ASSERT_FALSE(framer.busy());
    TEST_ASSERT_TRUE(framer.pump(port));
    TEST_ASSERT_EQUAL_UINT32(0U, port.out.size());
}

// The longest line the schema allows fits exactly, terminator included, and a
// full pump moves it in one call.
void test_the_longest_schema_line_fits_and_moves(void) {
    ev::LineFramer framer;
    MockPort port;

    std::string longest(ev::kSerialLineCapacity, 'x');
    TEST_ASSERT_TRUE(stageLine(framer, longest.c_str(), false));
    TEST_ASSERT_TRUE(framer.pump(port));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ev::kSerialLineCapacity + 2U),
                             static_cast<uint32_t>(port.out.size()));
    TEST_ASSERT_EQUAL_STRING((longest + kTerminator).c_str(), port.out.c_str());
}

// `begin()` returns to the post-construction framing state; counters are session
// evidence and survive it.
void test_begin_clears_the_staging_state_but_not_the_counters(void) {
    ev::LineFramer framer;
    MockPort port;
    port.totalAccepted = 3U;

    TEST_ASSERT_TRUE(stageLine(framer, "EVS3,one", false));
    TEST_ASSERT_FALSE(framer.pump(port));
    TEST_ASSERT_TRUE(framer.busy());

    framer.begin();
    TEST_ASSERT_FALSE(framer.busy());
    TEST_ASSERT_EQUAL_UINT16(0U, framer.pendingBytes());
    TEST_ASSERT_EQUAL_UINT32(0U, framer.linesSent());
    TEST_ASSERT_EQUAL_UINT32(1U, framer.linesStaged());

    MockPort freshPort;
    TEST_ASSERT_TRUE(stageLine(framer, "EVS3,two", false));
    TEST_ASSERT_TRUE(framer.pump(freshPort));
    TEST_ASSERT_EQUAL_STRING("EVS3,two\r\n", freshPort.out.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_clean_line_moves_whole_with_its_terminator);
    RUN_TEST(test_a_partial_write_resumes_and_never_reorders);
    RUN_TEST(test_a_port_that_accepts_nothing_stages_until_it_can);
    RUN_TEST(test_a_zero_write_with_reported_room_keeps_the_line_staged);
    RUN_TEST(test_an_urgent_line_never_joins_a_partially_emitted_record);
    RUN_TEST(test_the_working_buffer_never_touches_the_transmitting_line);
    RUN_TEST(test_a_waiting_urgent_is_replaced_by_a_newer_one_and_counted);
    RUN_TEST(test_telemetry_is_refused_while_a_line_is_active_or_waiting);
    RUN_TEST(test_oversize_lines_are_refused_and_counted);
    RUN_TEST(test_the_longest_schema_line_fits_and_moves);
    RUN_TEST(test_begin_clears_the_staging_state_but_not_the_counters);
    return UNITY_END();
}
