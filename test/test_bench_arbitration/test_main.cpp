#include <cstdint>
#include <cstring>

#include <unity.h>

#include <MotorArbitration.h>

// Contract tests for the bench/production motor arbitration
// (lib/evcore/src/MotorArbitration.h): the decision that lets the bench write the
// motor driver in a pass.
//
// This exists because the first version of that decision was
// `bench is driving && !armed`, and the two states that most need the motor off
// are exactly the states where `!armed` is true: a latched fault clears arming,
// and a violated control-tick order disarms. The defect lived in the seam between
// the bench instrument and the production chain (`src/bench/BenchConsole.h`,
// `src/bench/BenchMain.cpp`, `src/VehicleRuntime.cpp` stage 9), which has no
// runtime in a host test - so the decision was extracted here to be testable.
//
// The two regression tests below are the ones that would have failed before the
// fix: a faulted motor and an order-violated tick are both `!armed`, and the old
// gate let the bench drive them.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::BenchApplyVerdict verdict(bool benchDriving, bool runArmed, bool faultLatched, bool runUnderway,
                              bool orderViolationDetected) {
    return ev::benchApplyVerdict(benchDriving, runArmed, faultLatched, runUnderway,
                                 orderViolationDetected);
}

}  // namespace

// --- The permit --------------------------------------------------------------

// The only state that permits a write: the bench instrument is driving and
// nothing outranks it. A healthy bench session with the run idle looks like this.
void test_a_driving_instrument_with_an_idle_run_may_apply(void) {
    const ev::BenchApplyVerdict allowed = verdict(true, false, false, false, false);
    TEST_ASSERT_TRUE(allowed.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::None),
                          static_cast<int>(allowed.refusal));
}

void test_an_instrument_that_is_not_driving_writes_nothing(void) {
    const ev::BenchApplyVerdict idle = verdict(false, false, false, false, false);
    TEST_ASSERT_FALSE(idle.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::InstrumentNotDriving),
                          static_cast<int>(idle.refusal));
}

// --- The refusals ------------------------------------------------------------

// A latched fault clears arming, so the old `!armed()` gate read this state as
// "the motor is free". It is not: the firmware has already decided no powered
// motion is allowed.
void test_a_latched_fault_outranks_the_bench_even_though_it_clears_arming(void) {
    const ev::BenchApplyVerdict faulted = verdict(true, false, true, false, false);
    TEST_ASSERT_FALSE(faulted.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::FaultLatched),
                          static_cast<int>(faulted.refusal));
}

// A violated tick order calls the driver's fail-safe write and disarms, so the
// pass that just refused to trust its own cycle is also `!armed`. The bench must
// not write over that fail-safe write.
void test_a_violated_tick_order_outranks_the_bench_even_though_it_disarms(void) {
    const ev::BenchApplyVerdict violated = verdict(true, false, false, false, true);
    TEST_ASSERT_FALSE(violated.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::OrderViolation),
                          static_cast<int>(violated.refusal));
}

// A run that is travelling must keep the driver: its own authorized command
// reached the pins during the tick, and one source per cycle is the rule.
void test_a_run_under_way_outranks_the_bench(void) {
    const ev::BenchApplyVerdict running = verdict(true, true, false, true, false);
    TEST_ASSERT_FALSE(running.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::RunUnderway),
                          static_cast<int>(running.refusal));
}

// An armed run that has not started travelling is still the run's motor.
void test_an_armed_run_outranks_the_bench(void) {
    const ev::BenchApplyVerdict armed = verdict(true, true, false, false, false);
    TEST_ASSERT_FALSE(armed.mayApply);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::RunArmed),
                          static_cast<int>(armed.refusal));
}

// --- Precedence and invariants ----------------------------------------------

// The named reason is the most severe blocking condition, not whichever was
// checked first: a faulted, order-violated, under-way run reports the fault.
void test_the_reported_reason_is_the_most_severe_condition(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::FaultLatched),
                          static_cast<int>(verdict(true, true, true, true, true).refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::OrderViolation),
                          static_cast<int>(verdict(true, true, false, true, true).refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchApplyRefusal::RunUnderway),
                          static_cast<int>(verdict(true, true, false, true, false).refusal));
}

// Exhaustive over the five inputs: a write is permitted exactly when the refusal
// is None, and never when the instrument is not driving. The first property is
// what lets a caller report "refused, because X" without a second test.
void test_may_apply_holds_exactly_when_the_refusal_is_none(void) {
    for (uint8_t bits = 0U; bits < 32U; ++bits) {
        const bool benchDriving = (bits & 0x01U) != 0U;
        const bool runArmed = (bits & 0x02U) != 0U;
        const bool faultLatched = (bits & 0x04U) != 0U;
        const bool runUnderway = (bits & 0x08U) != 0U;
        const bool orderViolation = (bits & 0x10U) != 0U;

        const ev::BenchApplyVerdict result =
            verdict(benchDriving, runArmed, faultLatched, runUnderway, orderViolation);

        TEST_ASSERT_EQUAL_INT(result.mayApply ? 1 : 0,
                              result.refusal == ev::BenchApplyRefusal::None ? 1 : 0);
        if (!benchDriving) {
            TEST_ASSERT_FALSE(result.mayApply);
        }
        if (faultLatched || runArmed || runUnderway || orderViolation) {
            TEST_ASSERT_FALSE(result.mayApply);
        }
    }
}

// Every refusal has its own name, and none of them is the "permitted" spelling.
void test_every_refusal_has_its_own_name(void) {
    const ev::BenchApplyRefusal all[] = {
        ev::BenchApplyRefusal::FaultLatched,  ev::BenchApplyRefusal::OrderViolation,
        ev::BenchApplyRefusal::RunUnderway,   ev::BenchApplyRefusal::RunArmed,
        ev::BenchApplyRefusal::InstrumentNotDriving,
    };
    for (uint8_t index = 0U; index < 5U; ++index) {
        const char* name = ev::benchApplyRefusalName(all[index]);
        TEST_ASSERT_TRUE(name != nullptr);
        TEST_ASSERT_TRUE(std::strlen(name) > 0U);
        TEST_ASSERT_TRUE(std::strcmp(name, "?") != 0);
        TEST_ASSERT_TRUE(std::strcmp(name, "None") != 0);
    }
    TEST_ASSERT_EQUAL_STRING("None", ev::benchApplyRefusalName(ev::BenchApplyRefusal::None));
}

// --- H1: the production commands the bench must revoke its own output for -----

// Exactly the two production commands that would otherwise leave a bench drive
// applied (`disarm`, which the operator reads as a stop while the production
// authority is already disarmed) or rewrite the state a live bench command is
// running against (`reset`).
void test_disarm_and_reset_are_the_commands_that_revoke_the_bench(void) {
    TEST_ASSERT_TRUE(ev::benchRevokesForProductionCommand(ev::OperatorCommandId::Disarm));
    TEST_ASSERT_TRUE(ev::benchRevokesForProductionCommand(ev::OperatorCommandId::Reset));
}

// Everything else is forwarded untouched. The bench is not a filter on the
// operator interface, so a readout or an announcement must reach it unchanged.
void test_every_other_production_command_is_forwarded_untouched(void) {
    const ev::OperatorCommandId others[] = {
        ev::OperatorCommandId::None,         ev::OperatorCommandId::Help,
        ev::OperatorCommandId::Status,       ev::OperatorCommandId::Version,
        ev::OperatorCommandId::Checks,       ev::OperatorCommandId::Boot,
        ev::OperatorCommandId::ConfigShow,   ev::OperatorCommandId::ConfigCheck,
        ev::OperatorCommandId::Plan,         ev::OperatorCommandId::ConfigSet,
        ev::OperatorCommandId::ConfigClear,  ev::OperatorCommandId::Announce,
        ev::OperatorCommandId::Arm,          ev::OperatorCommandId::TelemetryOn,
        ev::OperatorCommandId::TelemetryOff, ev::OperatorCommandId::TelemetryStatus,
        ev::OperatorCommandId::TelemetryRate,
    };
    for (uint8_t index = 0U; index < 17U; ++index) {
        TEST_ASSERT_FALSE(ev::benchRevokesForProductionCommand(others[index]));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_driving_instrument_with_an_idle_run_may_apply);
    RUN_TEST(test_an_instrument_that_is_not_driving_writes_nothing);
    RUN_TEST(test_a_latched_fault_outranks_the_bench_even_though_it_clears_arming);
    RUN_TEST(test_a_violated_tick_order_outranks_the_bench_even_though_it_disarms);
    RUN_TEST(test_a_run_under_way_outranks_the_bench);
    RUN_TEST(test_an_armed_run_outranks_the_bench);
    RUN_TEST(test_the_reported_reason_is_the_most_severe_condition);
    RUN_TEST(test_may_apply_holds_exactly_when_the_refusal_is_none);
    RUN_TEST(test_every_refusal_has_its_own_name);
    RUN_TEST(test_disarm_and_reset_are_the_commands_that_revoke_the_bench);
    RUN_TEST(test_every_other_production_command_is_forwarded_untouched);
    return UNITY_END();
}
