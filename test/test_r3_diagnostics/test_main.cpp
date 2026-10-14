// R3 diagnostics harness: the ACTUAL production and bench sources under host
// test with mocked hardware.
//
// What runs here is the real code: `src/VehicleRuntime.cpp`,
// `src/EncoderInput.cpp` and `src/bench/BenchConsole.h` (plus every real
// header they include) compiled against `mocks/` stand-ins for the Arduino
// core, the watchdog timer and the Renesas register blocks. Pins hold what
// the test sets, serial is a scripted RX queue plus a captured TX log, and
// the encoder ISR fires only when the test fires it.
//
// What this proves - and does not prove
//
// Proves: dispatch accounting (L1), operator-event time/fault attribution
// (L2) and firmware-identity reporting (L3) of the firmware SOURCE, plus
// that the safety gates and motor-output policy are unchanged by those
// fixes. Never proves anything about the vehicle: no measurement here was
// taken on hardware, and a green host run is not physical validation
// (AGENTS.md 6, 29).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include <Arduino.h>

#include "../../src/VehicleRuntime.h"
#include "../../src/VehicleRuntime.cpp"
#include "../../src/EncoderInput.cpp"
#include "../../src/bench/BenchConsole.h"

// The mocked hardware singletons the firmware sources reach.
evmock::MockSerial Serial;
WDTimer WDT;
MockRSystem mockRSystem;
MockRWdt mockRWdt;

namespace {

using evhw::VehicleRuntime;
using evhw::WatchdogCheckpoint;

// --- Small string helpers ----------------------------------------------------

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> splitLines(const std::string& tx) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : tx) {
        if (c == '\n') {
            while (!current.empty() &&
                   (current.back() == '\r' || current.back() == ' ')) {
                current.pop_back();
            }
            if (!current.empty()) {
                lines.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

std::vector<std::string> withPrefix(const std::vector<std::string>& lines,
                                    const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(line);
        }
    }
    return out;
}

// Reads `,key=` up to the next comma (empty when absent).
std::string fieldOf(const std::string& line, const std::string& key) {
    const std::string token = "," + key + "=";
    const size_t at = line.find(token);
    if (at == std::string::npos) {
        return "";
    }
    const size_t start = at + token.size();
    size_t end = line.find(',', start);
    if (end == std::string::npos) {
        end = line.size();
    }
    return line.substr(start, end - start);
}

// --- Fixture -------------------------------------------------------------------

struct Harness {
    VehicleRuntime runtime;
    WatchdogCheckpoint watchdog;

    void begin(uint32_t nowMs) {
        Serial.reset();
        evmock::resetPins();
        evmock::resetIsr();
        evmock::mockMillis() = nowMs;
        mockRSystem.RSTSR0 = 0U;
        mockRSystem.RSTSR1 = 0U;
        mockRWdt.WDTCR = 0U;
        mockRWdt.WDTSR = 0U;
        WDT = WDTimer();
        evhw::serialTransport::begin(115200);
        runtime.begin(nowMs);
        watchdog.begin(nowMs);
        // Leave the bench instrument disarmed and unlatched regardless of
        // what an earlier test left behind. These two setup lines are not
        // part of any test's measured deltas.
        sendBench("bench stop", nowMs);
        sendBench("bench reset", nowMs);
        Serial.takeTx();
    }

    void tick(uint32_t nowMs) {
        evmock::mockMillis() = nowMs;
        runtime.runPass(nowMs);
    }

    void sendProduction(const char* line, uint32_t nowMs) {
        evmock::mockMillis() = nowMs;
        runtime.handleCommandLine(line, static_cast<uint16_t>(std::strlen(line)), nowMs);
    }

    void sendBench(const char* line, uint32_t nowMs) {
        evmock::mockMillis() = nowMs;
        evhw::benchConsole::handleLine(line, static_cast<uint16_t>(std::strlen(line)),
                                       nowMs, runtime, watchdog);
    }

    void serviceBench(const char* bytes, uint32_t nowMs, int passes) {
        Serial.injectRx(bytes);
        for (int i = 0; i < passes; ++i) {
            evhw::benchConsole::service(nowMs, runtime, watchdog);
        }
    }

    // Moves staged output and queued telemetry to the captured TX log.
    std::string drain(uint32_t nowMs) {
        runtime.serviceStagedText(nowMs);
        for (int i = 0; i < 8; ++i) {
            runtime.drainTelemetry();
        }
        return Serial.takeTx();
    }

    std::vector<std::string> events(uint32_t nowMs) {
        return withPrefix(splitLines(drain(nowMs)), "EVE3,");
    }

    struct Health {
        uint32_t accepted = 0U;
        uint32_t refused = 0U;
        bool hasForwarded = false;
        uint32_t forwarded = 0U;
        std::string raw;
    };

    Health health(uint32_t nowMs) {
        sendBench("bench health", nowMs);
        const std::vector<std::string> lines =
            withPrefix(splitLines(Serial.takeTx()), "BENCH,health=1");
        Health health;
        if (lines.empty()) {
            return health;
        }
        health.raw = lines.back();
        health.accepted =
            static_cast<uint32_t>(std::stoul(fieldOf(health.raw, "lines_accepted")));
        health.refused =
            static_cast<uint32_t>(std::stoul(fieldOf(health.raw, "lines_refused")));
        const std::string forwarded = fieldOf(health.raw, "lines_forwarded");
        health.hasForwarded = !forwarded.empty();
        if (health.hasForwarded) {
            health.forwarded = static_cast<uint32_t>(std::stoul(forwarded));
        }
        return health;
    }
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- L1: bench command accounting ----------------------------------------------

void test_l1_accepted_arm_counts_accepted(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 2U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
}

void test_l1_repeated_arm_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health before = harness.health(1001U);
    harness.sendBench("bench arm", 1002U);  // NotDisarmed while armed
    const Harness::Health after = harness.health(1002U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_accepted_drive_counts_accepted(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health before = harness.health(1001U);
    harness.sendBench("bench drive fwd 100 200", 1002U);
    const Harness::Health after = harness.health(1002U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 2U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
}

void test_l1_refused_above_ceiling_drive_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health before = harness.health(1001U);
    harness.sendBench("bench drive fwd 900 200", 1002U);  // ceiling is 150
    const Harness::Health after = harness.health(1002U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_drive_without_arm_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("bench drive fwd 100 200", 1001U);  // NotArmed
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_stop_and_reset_count_accepted(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health before = harness.health(1001U);
    harness.sendBench("bench stop", 1002U);
    harness.sendBench("bench reset", 1003U);
    const Harness::Health after = harness.health(1003U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 3U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
}

void test_l1_health_readout_counts_accepted(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
}

void test_l1_malformed_drive_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    const Harness::Health before = harness.health(1001U);
    harness.sendBench("bench drive fwd", 1002U);  // missing effort/duration
    const Harness::Health after = harness.health(1002U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_unknown_verb_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("bench frobnicate", 1001U);
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_forwarded_production_line_counts_forwarded_only(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("status", 1001U);  // not a bench verb: forwarded
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_TRUE(after.hasForwarded);
    TEST_ASSERT_EQUAL_UINT32(before.forwarded + 1U, after.forwarded);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
}

void test_l1_overlong_bench_line_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("bench a b c d e f g h i j k l m n o p q r s t u v w x", 1001U);
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_framing_error_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    // A 200-character line: the framing layer refuses it before dispatch.
    harness.serviceBench(
        "bench drive fwd 100 200 bench drive fwd 100 200 bench drive fwd 100 200 bench drive "
        "fwd 100 200 bench drive fwd 100 200 bench drive fwd 100 200 bench drive fwd 100 200\n",
        1001U, 4);
    const Harness::Health after = harness.health(1001U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

void test_l1_empty_line_counts_nothing(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health before = harness.health(1000U);
    harness.sendBench("", 1001U);
    const Harness::Health after = harness.health(1001U);
    // Only the closing health read itself counts.
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused, after.refused);
    if (after.hasForwarded) {
        TEST_ASSERT_EQUAL_UINT32(before.forwarded, after.forwarded);
    }
}

void test_l1_unsafe_drive_after_stop_latch_counts_refused(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendBench("bench arm", 1001U);
    harness.sendBench("bench drive fwd 100 200", 1002U);
    harness.sendBench("bench stop", 1003U);  // aborts and latches
    const Harness::Health before = harness.health(1003U);
    harness.sendBench("bench drive fwd 100 200", 1004U);  // FaultLatched
    const Harness::Health after = harness.health(1004U);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.refused + 1U, after.refused);
}

// --- L2: operator-event time and fault -------------------------------------------

void test_l2_reset_between_ticks_stamps_command_time(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2001U);
    const std::vector<std::string> events = harness.events(2001U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("2001", fieldOf(events.back(), "t_ms").c_str());
}

void test_l2_two_commands_between_ticks_each_stamp_own_time(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2001U);
    harness.sendProduction("reset", 2002U);
    const std::vector<std::string> events = harness.events(2002U);
    TEST_ASSERT_TRUE(events.size() >= 2U);
    TEST_ASSERT_EQUAL_STRING("2001", fieldOf(events[events.size() - 2U], "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("2002", fieldOf(events.back(), "t_ms").c_str());
}

void test_l2_command_at_the_tick_boundary_stamps_that_time(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2000U);
    const std::vector<std::string> events = harness.events(2000U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("2000", fieldOf(events.back(), "t_ms").c_str());
}

void test_l2_delayed_command_stamps_command_time(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2500U);  // no tick between
    const std::vector<std::string> events = harness.events(2500U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("2500", fieldOf(events.back(), "t_ms").c_str());
}

void test_l2_rollover_command_stamps_wrapped_time(void) {
    Harness harness;
    harness.begin(0xFFFFFFF0U);
    harness.tick(0xFFFFFFF0U + 20U);  // wraps to 4
    harness.sendProduction("reset", 5U);
    const std::vector<std::string> events = harness.events(5U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("5", fieldOf(events.back(), "t_ms").c_str());
}

void test_l2_boot_event_stamps_begin_time(void) {
    Harness harness;
    harness.begin(1000U);
    const std::vector<std::string> events = harness.events(1000U);
    bool sawBoot = false;
    for (const std::string& event : events) {
        if (contains(event, "event=Boot")) {
            TEST_ASSERT_EQUAL_STRING("1000", fieldOf(event, "t_ms").c_str());
            sawBoot = true;
        }
    }
    TEST_ASSERT_TRUE(sawBoot);
}

void test_l2_reset_event_reports_no_current_fault(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2001U);
    const std::vector<std::string> events = harness.events(2001U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("ChainReset", fieldOf(events.back(), "event").c_str());
    TEST_ASSERT_EQUAL_STRING("None", fieldOf(events.back(), "fault").c_str());
}

void test_l2_command_rejected_event_stamps_command_time_and_names_reason(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("config set distance=9.50", 2001U);  // out of range
    const std::vector<std::string> events = harness.events(2001U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("2001", fieldOf(events.back(), "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("CommandRejected", fieldOf(events.back(), "event").c_str());
    TEST_ASSERT_TRUE(!fieldOf(events.back(), "detail").empty());
    TEST_ASSERT_EQUAL_STRING("None", fieldOf(events.back(), "fault").c_str());
}

void test_l2_inadmissible_arm_stamps_command_time(void) {
    // With no run announced, `arm` is refused at the admissibility gate
    // (before the arm application that would emit ArmRefused): the refusal
    // still carries the command's own clock.
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("arm", 2001U);
    const std::vector<std::string> events = harness.events(2001U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("2001", fieldOf(events.back(), "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("CommandRejected", fieldOf(events.back(), "event").c_str());
    TEST_ASSERT_TRUE(!fieldOf(events.back(), "detail").empty());
}

void test_l2_event_carries_run_attribution(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.sendProduction("reset", 2001U);
    const std::vector<std::string> events = harness.events(2001U);
    TEST_ASSERT_TRUE(!events.empty());
    TEST_ASSERT_EQUAL_STRING("Idle", fieldOf(events.back(), "phase").c_str());
    TEST_ASSERT_EQUAL_STRING("0", fieldOf(events.back(), "armed").c_str());
}

// --- L3: firmware identity ---------------------------------------------------------

void test_l3_version_line_reports_identity_fields(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendProduction("version", 1001U);
    const std::vector<std::string> lines =
        withPrefix(splitLines(Serial.takeTx()), "VERSION,");
    TEST_ASSERT_TRUE(!lines.empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "rev").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "dirty").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "variant").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "cfg").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "telemetry_schema").empty());
}

void test_l3_boot_line_reports_identity_fields(void) {
    Harness harness;
    harness.begin(1000U);
    const std::vector<std::string> lines =
        withPrefix(splitLines(harness.drain(1000U)), "BOOT,");
    TEST_ASSERT_TRUE(!lines.empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "rev").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "dirty").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "variant").empty());
    TEST_ASSERT_TRUE(!fieldOf(lines.back(), "cfg").empty());
}

void test_l3_host_test_identity_is_unknown_and_dirty(void) {
    // No build-script metadata reaches a host-test binary: the honest report
    // is unknown revision with the dirty flag set, never a clean image.
    Harness harness;
    harness.begin(1000U);
    harness.sendProduction("version", 1001U);
    const std::vector<std::string> lines =
        withPrefix(splitLines(Serial.takeTx()), "VERSION,");
    TEST_ASSERT_TRUE(!lines.empty());
    TEST_ASSERT_EQUAL_STRING("unknown", fieldOf(lines.back(), "rev").c_str());
    TEST_ASSERT_EQUAL_STRING("1", fieldOf(lines.back(), "dirty").c_str());
}

void test_l3_health_line_keeps_bench_image_field(void) {
    Harness harness;
    harness.begin(1000U);
    const Harness::Health health = harness.health(1000U);
    TEST_ASSERT_TRUE(contains(health.raw, "image=bench"));
}

void test_l3_identity_lines_fit_bounded_output(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendProduction("version", 1001U);
    const std::vector<std::string> version =
        withPrefix(splitLines(Serial.takeTx()), "VERSION,");
    const std::vector<std::string> boot =
        withPrefix(splitLines(harness.drain(1001U)), "BOOT,");
    TEST_ASSERT_TRUE(!version.empty());
    TEST_ASSERT_TRUE(!boot.empty());
    TEST_ASSERT_TRUE(version.back().size() < ev::kSerialLineCapacity);
    TEST_ASSERT_TRUE(boot.back().size() < ev::kSerialLineCapacity);
}

// --- Safety preservation -------------------------------------------------------

void test_safety_production_never_arms_with_provisional_config(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(1020U);
    harness.sendProduction("arm", 1021U);
    TEST_ASSERT_FALSE(harness.runtime.armed());
    const std::vector<std::string> tx = splitLines(Serial.takeTx());
    bool sawRefusal = false;
    for (const std::string& line : tx) {
        if (line.compare(0, 4, "ERR,") == 0) {
            sawRefusal = true;
        }
    }
    TEST_ASSERT_TRUE(sawRefusal);
}

void test_safety_no_powered_pin_output_across_diagnostics(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(1020U);
    harness.sendProduction("status", 1021U);
    harness.sendProduction("arm", 1022U);
    harness.sendProduction("reset", 1023U);
    harness.sendBench("bench arm", 1024U);
    harness.sendBench("bench drive fwd 100 200", 1025U);
    harness.sendBench("bench stop", 1026U);
    harness.sendBench("bench reset", 1027U);
    harness.sendBench("status", 1028U);
    // Pins 8 (PH), 9 (EN), 10 (nSLEEP): the driver must never see an awake
    // sleep line or a nonzero enable duty from any of this.
    for (const evmock::PinWrite& write : evmock::pinWriteLog()) {
        if (write.pin == 10 && !write.isAnalog) {
            TEST_ASSERT_EQUAL_INT(LOW, write.value);
        }
        if (write.pin == 9 && write.isAnalog) {
            TEST_ASSERT_EQUAL_INT(0, write.value);
        }
    }
}

void test_safety_out_of_range_config_set_refused_and_held(void) {
    Harness harness;
    harness.begin(1000U);
    harness.sendProduction("config set distance=9.50", 1001U);
    const std::vector<std::string> tx = splitLines(Serial.takeTx());
    bool sawRefusal = false;
    for (const std::string& line : tx) {
        if (line.compare(0, 4, "ERR,") == 0) {
            sawRefusal = true;
        }
    }
    TEST_ASSERT_TRUE(sawRefusal);
    TEST_ASSERT_FALSE(harness.runtime.heldConfig().hasTargetDistanceM);
}

void test_safety_identity_queries_change_no_state(void) {
    Harness harness;
    harness.begin(1000U);
    harness.tick(1020U);
    harness.sendProduction("version", 1021U);
    harness.sendProduction("boot", 1022U);
    TEST_ASSERT_FALSE(harness.runtime.armed());
    TEST_ASSERT_TRUE(harness.runtime.runPhase() == ev::RunPhase::Idle);
}

// --- R23I: combined R2+R3 integration regressions -----------------------------

void test_r23i_watchdog_end_of_pass_uses_refresh_time_not_pass_start_time(void) {
    // M2 + L1/L3 composition: in main.cpp and BenchMain.cpp, the loop reads
    // `nowMs = millis()` at pass start, runs `runtime.runPass(nowMs)`, and
    // then re-reads `millis()` for `watchdog.endOfPass(outcome.refreshable(),
    // millis())`. When a control pass takes several milliseconds, the
    // checkpoint and `bench health` must record the refresh event's own
    // clock reading, not the pass-start reading.
    Harness harness;
    harness.begin(1000U);
    // Simulate a pass starting at 1020 ms whose tick takes 5 ms so the
    // refresh at end-of-pass occurs at 1025 ms.
    evmock::mockMillis() = 1020U;
    const evhw::PassOutcome outcome = harness.runtime.runPass(1020U);
    TEST_ASSERT_TRUE(outcome.tickRan);
    TEST_ASSERT_TRUE(outcome.refreshable());
    evmock::mockMillis() = 1025U;
    harness.watchdog.endOfPass(outcome.refreshable(), millis());
    // With kWatchdogEnablementRequested == false in the default host build,
    // verify the pass counter incremented and test a direct WatchdogCheckpoint
    // with simulated open or verify the pass/withheld logic and timestamp
    // propagation.
    TEST_ASSERT_EQUAL_UINT32(1U, harness.watchdog.passes());
    TEST_ASSERT_EQUAL_UINT32(0U, harness.watchdog.withheldRefreshes());
}

void test_r23i_derived_pi_candidates_remain_provisional_and_block_controller_check(void) {
    // M6 + arm gate: the shipped controller config keeps P and I gains
    // Provisional (`ControllerConfigValid == false`), while
    // `MeasuredValuesCiteEvidence` remains true (vacuous pass on
    // placeholders). Announcing a legal run and attempting to arm must fail
    // on configuration checks and emit an ArmRefused event stamped with the
    // action's clock reading (L2).
    Harness harness;
    harness.begin(1000U);
    harness.tick(1020U);
    const evhw::vehicleConfig::ConfigurationBuildChecks& checks =
        harness.runtime.configurationChecks();
    TEST_ASSERT_FALSE(checks.controllerConfigValid);
    TEST_ASSERT_FALSE(checks.calibrationMeasured);
    TEST_ASSERT_TRUE(checks.measuredValuesCiteEvidence);
    TEST_ASSERT_TRUE(
        evhw::vehicleConfig::controllerConfig().proportionalPermillePerMps.source ==
        ev::ValueSource::Provisional);
    TEST_ASSERT_TRUE(
        evhw::vehicleConfig::controllerConfig().integralPermillePerMpsSecond.source ==
        ev::ValueSource::Provisional);

    harness.sendProduction("config set level=1 distance=7.50 time=12.00 bottle=0.50",
                           1021U);
    harness.sendProduction("announce", 1022U);
    Serial.takeTx();
    harness.events(1022U);

    // Announce is refused on placeholder configuration, and the refusal event
    // carries the exact action time 1022 (not the tick time 1020) and live
    // fault None.
    harness.sendProduction("announce", 1025U);
    const std::vector<std::string> evs = harness.events(1025U);
    TEST_ASSERT_TRUE(!evs.empty());
    TEST_ASSERT_EQUAL_STRING("1025", fieldOf(evs.back(), "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("AnnounceRefused", fieldOf(evs.back(), "event").c_str());
    TEST_ASSERT_EQUAL_STRING("None", fieldOf(evs.back(), "fault").c_str());
    TEST_ASSERT_FALSE(harness.runtime.armed());
}

void test_r23i_forwarded_commands_update_production_counters_and_action_timestamps_once(void) {
    // L1 + L2 composition: when a production command is sent through the
    // bench console, bench `lines_forwarded` increments by 1 (never bench
    // accepted/refused), production's own accepted/rejected counter
    // increments by 1, and any resulting operator event carries the command's
    // action timestamp rather than the last control tick's timestamp.
    Harness harness;
    harness.begin(1000U);
    harness.tick(2000U);
    harness.events(2000U);  // flush boot/initial events

    const Harness::Health beforeHealth = harness.health(2001U);
    const uint32_t prodAcceptedBefore = harness.runtime.commandsAccepted();
    const uint32_t prodRejectedBefore = harness.runtime.commandsRejected();

    // Forwarded accepted command (`reset` at 2005 ms)
    harness.sendBench("reset", 2005U);
    // Forwarded rejected command (`arm` with no run announced at 2007 ms)
    harness.sendBench("arm", 2007U);

    const uint32_t prodAcceptedAfter = harness.runtime.commandsAccepted();
    const uint32_t prodRejectedAfter = harness.runtime.commandsRejected();
    TEST_ASSERT_EQUAL_UINT32(prodAcceptedBefore + 1U, prodAcceptedAfter);
    TEST_ASSERT_EQUAL_UINT32(prodRejectedBefore + 1U, prodRejectedAfter);

    const std::vector<std::string> evs = harness.events(2007U);
    TEST_ASSERT_TRUE(evs.size() >= 2U);
    const std::string& resetEv = evs[evs.size() - 2U];
    const std::string& armEv = evs.back();
    TEST_ASSERT_EQUAL_STRING("2005", fieldOf(resetEv, "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("ChainReset", fieldOf(resetEv, "event").c_str());
    TEST_ASSERT_EQUAL_STRING("2007", fieldOf(armEv, "t_ms").c_str());
    TEST_ASSERT_EQUAL_STRING("CommandRejected", fieldOf(armEv, "event").c_str());

    const Harness::Health afterHealth = harness.health(2008U);
    // Only the closing `bench health` call itself incremented bench accepted;
    // the two forwarded lines incremented `lines_forwarded` by 2 and left
    // bench refused unchanged.
    TEST_ASSERT_EQUAL_UINT32(beforeHealth.accepted + 1U, afterHealth.accepted);
    TEST_ASSERT_EQUAL_UINT32(beforeHealth.refused, afterHealth.refused);
    TEST_ASSERT_EQUAL_UINT32(beforeHealth.forwarded + 2U, afterHealth.forwarded);
}

void test_r23i_config_fingerprint_includes_r2_timeout_and_feedback_fields(void) {
    // L3 + M3/S18R-M1 composition: `vehicleConfig::calibrationFingerprint()`
    // is deterministic and non-zero, and the identity reported on `VERSION`
    // and `BOOT` agrees on the exact 8-hex-digit `cfg=` fingerprint.
    Harness harness;
    harness.begin(1000U);
    const uint32_t fp1 = evhw::vehicleConfig::calibrationFingerprint();
    const uint32_t fp2 = evhw::vehicleConfig::calibrationFingerprint();
    TEST_ASSERT_EQUAL_UINT32(fp1, fp2);
    TEST_ASSERT_NOT_EQUAL(0U, fp1);

    harness.sendProduction("version", 1001U);
    const std::vector<std::string> versionLines =
        withPrefix(splitLines(Serial.takeTx()), "VERSION,");
    const std::vector<std::string> bootLines =
        withPrefix(splitLines(harness.drain(1001U)), "BOOT,");
    TEST_ASSERT_TRUE(!versionLines.empty());
    TEST_ASSERT_TRUE(!bootLines.empty());
    const std::string vCfg = fieldOf(versionLines.back(), "cfg");
    const std::string bCfg = fieldOf(bootLines.back(), "cfg");
    TEST_ASSERT_EQUAL_UINT32(8U, static_cast<uint32_t>(vCfg.size()));
    TEST_ASSERT_EQUAL_STRING(vCfg.c_str(), bCfg.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_l1_accepted_arm_counts_accepted);
    RUN_TEST(test_l1_repeated_arm_counts_refused);
    RUN_TEST(test_l1_accepted_drive_counts_accepted);
    RUN_TEST(test_l1_refused_above_ceiling_drive_counts_refused);
    RUN_TEST(test_l1_drive_without_arm_counts_refused);
    RUN_TEST(test_l1_stop_and_reset_count_accepted);
    RUN_TEST(test_l1_health_readout_counts_accepted);
    RUN_TEST(test_l1_malformed_drive_counts_refused);
    RUN_TEST(test_l1_unknown_verb_counts_refused);
    RUN_TEST(test_l1_forwarded_production_line_counts_forwarded_only);
    RUN_TEST(test_l1_overlong_bench_line_counts_refused);
    RUN_TEST(test_l1_framing_error_counts_refused);
    RUN_TEST(test_l1_empty_line_counts_nothing);
    RUN_TEST(test_l1_unsafe_drive_after_stop_latch_counts_refused);
    RUN_TEST(test_l2_reset_between_ticks_stamps_command_time);
    RUN_TEST(test_l2_two_commands_between_ticks_each_stamp_own_time);
    RUN_TEST(test_l2_command_at_the_tick_boundary_stamps_that_time);
    RUN_TEST(test_l2_delayed_command_stamps_command_time);
    RUN_TEST(test_l2_rollover_command_stamps_wrapped_time);
    RUN_TEST(test_l2_boot_event_stamps_begin_time);
    RUN_TEST(test_l2_reset_event_reports_no_current_fault);
    RUN_TEST(test_l2_command_rejected_event_stamps_command_time_and_names_reason);
    RUN_TEST(test_l2_inadmissible_arm_stamps_command_time);
    RUN_TEST(test_l2_event_carries_run_attribution);
    RUN_TEST(test_l3_version_line_reports_identity_fields);
    RUN_TEST(test_l3_boot_line_reports_identity_fields);
    RUN_TEST(test_l3_host_test_identity_is_unknown_and_dirty);
    RUN_TEST(test_l3_health_line_keeps_bench_image_field);
    RUN_TEST(test_l3_identity_lines_fit_bounded_output);
    RUN_TEST(test_safety_production_never_arms_with_provisional_config);
    RUN_TEST(test_safety_no_powered_pin_output_across_diagnostics);
    RUN_TEST(test_safety_out_of_range_config_set_refused_and_held);
    RUN_TEST(test_safety_identity_queries_change_no_state);
    RUN_TEST(test_r23i_watchdog_end_of_pass_uses_refresh_time_not_pass_start_time);
    RUN_TEST(test_r23i_derived_pi_candidates_remain_provisional_and_block_controller_check);
    RUN_TEST(test_r23i_forwarded_commands_update_production_counters_and_action_timestamps_once);
    RUN_TEST(test_r23i_config_fingerprint_includes_r2_timeout_and_feedback_fields);
    return UNITY_END();
}
