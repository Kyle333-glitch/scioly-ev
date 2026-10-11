#include <unity.h>
#include <cstring>
#include <Arduino.h>
#include <WDT.h>

#include "../../src/VehicleConfig.h"
// Established audit seam: expose host object state, never change deployed gates.
#define private public
#include "../../src/VehicleRuntime.h"
#undef private
#include "../test_scenario/ScenarioHarness.h"

// Native normally excludes src/. Link the real implementations in this suite
// only; no copied reset or runtime implementation and no setup()/loop().
#include "../../src/EncoderInput.cpp"
#include "../../src/VehicleRuntime.cpp"

namespace fakeArduino {
int pins[256] = {};
int duty[256] = {};
uint32_t primask = 0U;
void (*encoderCallback)() = nullptr;
void (*boundaryHook)(int) = nullptr;
bool pending = false;
bool inIsr = false;
unsigned admitted = 0U, blocked = 0U, saves = 0U, disables = 0U, restores = 0U;
void admitPending() {
    if (pending && primask == 0U && !inIsr && encoderCallback != nullptr) {
        pending = false;
        inIsr = true;
        ++admitted;
        encoderCallback();
        inIsr = false;
    }
}
void requestEncoderInterrupt() {
    pending = true;
    if (primask != 0U) { ++blocked; }
    admitPending();
}
void boundary(int point) {
    if (!inIsr && boundaryHook != nullptr) { boundaryHook(point); }
}
}
FakeSerial Serial;
FakeWdt WDT;

namespace {
using namespace fakeArduino;
using namespace evhw;
unsigned hookCalls = 0U;
int injectionPoint = -1;
unsigned injectionOccurrence = 0U;
unsigned seenOccurrences = 0U;

void levels(bool a, bool b) {
    const bool changed = pins[kEncoderA] != (a ? HIGH : LOW);
    pins[kEncoderA] = a ? HIGH : LOW;
    pins[kEncoderB] = b ? HIGH : LOW;
    if (changed) { requestEncoderInterrupt(); }
}
void positiveCycle() {
    levels(true, false); levels(true, true);
    levels(false, true); levels(false, false);
}
void expectZeroCounters() {
    const encoderInput::TickSnapshot snap = encoderInput::snapshotTicks();
    TEST_ASSERT_EQUAL_INT32(0, snap.ticks);
    TEST_ASSERT_EQUAL_UINT32(0U, snap.countedEdges);
    TEST_ASSERT_EQUAL_UINT32(0U, snap.ignoredUpdates);
}
void command(VehicleRuntime& runtime, const char* line, uint32_t nowMs) {
    runtime.handleCommandLine(line, static_cast<uint16_t>(std::strlen(line)), nowMs);
}
void ownershipHook(int point) {
    if (point == 0 && hookCalls++ == 0U) {
        // Immediately BEFORE the adapter masks: the original six-field reset
        // must not already have run in the pure chain with IRQs enabled.
        TEST_ASSERT_EQUAL_INT32(2, encoderInput::g_decoder.ticks());
        TEST_ASSERT_EQUAL_UINT32(2U, encoderInput::g_decoder.countedEdges());
        TEST_ASSERT_TRUE(encoderInput::g_decoder.hasReference());
    }
}
void injectionHook(int point) {
    if (point == injectionPoint && seenOccurrences++ == injectionOccurrence) {
        boundaryHook = nullptr;
        requestEncoderInterrupt();
        if (primask != 0U) {
            TEST_ASSERT_EQUAL_UINT32(0U, admitted);
            TEST_ASSERT_TRUE(pending);
        }
    }
}
void seedCaches(VehicleRuntime& runtime) {
    const auto fixture = evscenario::nominalFixtures();
    runtime.estimator_.update(20U, 0, fixture.calibration.distancePerEncoderTickM);
    runtime.estimate_ = runtime.estimator_.update(40U, 10, fixture.calibration.distancePerEncoderTickM);
    TEST_ASSERT_TRUE(runtime.estimate_.valid);
    runtime.lastValidEstimate_ = runtime.estimate_;
    runtime.hasEstimate_ = runtime.hasLastValidEstimate_ = runtime.hasOdometrySample_ = true;
    runtime.odometryTravel_ = ev::OdometryEstimate{1.0, true, ev::OdometryEstimateStatus::Valid};
    runtime.positionUsable_ = true;
}
void expectResetState(VehicleRuntime& runtime) {
    TEST_ASSERT_FALSE(runtime.armed());
    TEST_ASSERT_FALSE(runtime.faultLatched());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle), static_cast<int>(runtime.runPhase()));
    TEST_ASSERT_FALSE(runtime.run_.stopVerified());
    TEST_ASSERT_FALSE(runtime.hasEstimate_);
    TEST_ASSERT_FALSE(runtime.hasLastValidEstimate_);
    TEST_ASSERT_FALSE(runtime.hasOdometrySample_);
    TEST_ASSERT_FALSE(runtime.positionUsable_);
    TEST_ASSERT_FALSE(runtime.estimate_.zeroSpeedClaimed);
    TEST_ASSERT_FALSE(runtime.lastValidEstimate_.valid);
    TEST_ASSERT_EQUAL_UINT32(0U, runtime.estimator_.acceptedSamples());
    TEST_ASSERT_FALSE(runtime.announcedConfigValid_);
    TEST_ASSERT_FALSE(runtime.executionPlan_.plan().valid);
    TEST_ASSERT_EQUAL_INT32(0, runtime.runOriginTicks_);
    TEST_ASSERT_EQUAL_INT(0, duty[kMotorEnable]);
    TEST_ASSERT_EQUAL_INT(LOW, pins[kMotorSleepBar]);
    expectZeroCounters();
}
}

void setUp(void) {
    boundaryHook = nullptr; primask = 0U; pending = inIsr = false;
    std::memset(pins, 0, sizeof(pins)); std::memset(duty, 0, sizeof(duty));
    pins[evhw::kStartTrigger] = HIGH;
    evhw::encoderInput::begin();
    admitted = blocked = saves = disables = restores = 0U;
    hookCalls = seenOccurrences = 0U;
    Serial.bytes.clear();
    evhw::serialTransport::begin(115200);
}
void tearDown(void) { boundaryHook = nullptr; primask = 0U; pending = false; }

void test_runtime_reset_has_no_earlier_unprotected_decoder_reset(void) {
    evhw::VehicleRuntime runtime;
    runtime.begin(0U);
    positiveCycle();
    boundaryHook = ownershipHook;
    const unsigned before = disables;
    command(runtime, "reset", 41U);
    boundaryHook = nullptr;
    TEST_ASSERT_TRUE(hookCalls > 0U);
    // One guarded reset/reference, then one guarded origin snapshot.
    TEST_ASSERT_EQUAL_UINT32(2U, disables - before);
    expectResetState(runtime);
}

void test_reset_reference_restores_enabled_interrupts(void) {
    positiveCycle();
    evhw::encoderInput::resetAndReestablishReference();
    TEST_ASSERT_EQUAL_UINT32(0U, primask);
    TEST_ASSERT_EQUAL_UINT32(1U, saves);
    TEST_ASSERT_EQUAL_UINT32(1U, disables);
    TEST_ASSERT_EQUAL_UINT32(1U, restores);
    expectZeroCounters();
}

void test_reset_reference_preserves_caller_disabled_interrupts(void) {
    positiveCycle();
    __disable_irq();
    evhw::encoderInput::resetAndReestablishReference();
    TEST_ASSERT_EQUAL_UINT32(1U, primask);
    evhw::encoderInput::reestablishReference();
    TEST_ASSERT_EQUAL_UINT32(1U, primask);
    evhw::encoderInput::snapshotTicks();
    TEST_ASSERT_EQUAL_UINT32(1U, primask);
    levels(true, false);
    TEST_ASSERT_TRUE(pending);
    const unsigned before = admitted;
    __set_PRIMASK(0U);  // Only the outer caller unmasks.
    TEST_ASSERT_EQUAL_UINT32(before + 1U, admitted);
    TEST_ASSERT_EQUAL_INT32(1, evhw::encoderInput::snapshotTicks().ticks);
}

void test_pending_isr_is_deferred_across_reset_and_reference(void) {
    // Inject before masking, just after masking (before all six field writes),
    // at EACH sequential pin read, and immediately before restoration.
    const int points[] = {0, 1, 2, 2, 3};
    const unsigned occurrences[] = {0U, 0U, 0U, 1U, 0U};
    for (uint32_t initialMask = 0U; initialMask <= 1U; ++initialMask) {
        for (unsigned i = 0U; i < 5U; ++i) {
            setUp(); positiveCycle();
            admitted = blocked = 0U;
            primask = initialMask;
            injectionPoint = points[i]; injectionOccurrence = occurrences[i];
            boundaryHook = injectionHook;
            evhw::encoderInput::resetAndReestablishReference();
            boundaryHook = nullptr;
            TEST_ASSERT_EQUAL_UINT32(initialMask, primask);
            TEST_ASSERT_TRUE(evhw::encoderInput::g_decoder.hasReference());
            TEST_ASSERT_FALSE(evhw::encoderInput::g_decoder.aLevel());
            TEST_ASSERT_FALSE(evhw::encoderInput::g_decoder.bLevel());
            TEST_ASSERT_EQUAL_INT32(0, evhw::encoderInput::g_decoder.ticks());
            TEST_ASSERT_EQUAL_UINT32(0U, evhw::encoderInput::g_decoder.countedEdges());
            if (initialMask != 0U) {
                TEST_ASSERT_EQUAL_UINT32(0U, admitted);
                TEST_ASSERT_TRUE(pending);
                __set_PRIMASK(0U);
            }
            TEST_ASSERT_EQUAL_UINT32(1U, admitted);
            TEST_ASSERT_FALSE(pending);
            // Duplicate pending callback sees a complete reference, never a
            // partial reset. Before-mask callbacks are discarded by the reset.
            TEST_ASSERT_EQUAL_UINT32(i == 0U && initialMask == 0U ? 0U : 1U,
                                    evhw::encoderInput::g_decoder.ignoredUpdates());
        }
    }
}

void test_all_initial_phases_have_no_phantom_tick_and_keep_both_signs(void) {
    // Software Gray cycle; physical forward/reverse remains unqualified.
    const bool a[] = {false, true, true, false};
    const bool b[] = {false, false, true, true};
    for (unsigned start = 0U; start < 4U; ++start) {
        for (int sign = -1; sign <= 1; sign += 2) {
            pins[evhw::kEncoderA] = a[start]; pins[evhw::kEncoderB] = b[start];
            evhw::encoderInput::resetAndReestablishReference();
            expectZeroCounters();
            unsigned index = start;
            for (unsigned step = 0U; step < 4U; ++step) {
                index = static_cast<unsigned>((static_cast<int>(index) + sign + 4) % 4);
                levels(a[index], b[index]);
            }
            const auto snap = evhw::encoderInput::snapshotTicks();
            TEST_ASSERT_EQUAL_INT32(sign * 2, snap.ticks);
            TEST_ASSERT_EQUAL_UINT32(2U, snap.countedEdges);
            TEST_ASSERT_EQUAL_UINT32(0U, snap.ignoredUpdates);
        }
    }
}

void test_runtime_fault_reset_clears_caches_but_preserves_direction_barrier(void) {
    evhw::VehicleRuntime runtime; runtime.begin(0U);
    positiveCycle(); seedCaches(runtime);
    const auto fixture = evscenario::nominalFixtures();
    ev::SafetyReadiness ready;
    ready.configurationValid = ready.calibrationUsable = ready.limitsUsable = true;
    ev::SafetySystemState moving; moving.readiness = ready; moving.requiredDataFresh = true;
    TEST_ASSERT_TRUE(runtime.authority_.arm(ready));
    runtime.authority_.authorize(ev::MotorEffort(ev::MotorMode::DriveReverse, 100U), moving, fixture.limits);
    moving.detectedFault = ev::FaultCode::TravelStalled;
    runtime.authority_.authorize(ev::MotorEffort(ev::MotorMode::DriveReverse, 100U), moving, fixture.limits);
    ev::RunInputs input; input.detectedFault = ev::FaultCode::TravelStalled;
    runtime.run_.update(40U, input);
    TEST_ASSERT_TRUE(runtime.faultLatched());
    command(runtime, "arm", 40U); TEST_ASSERT_FALSE(runtime.armed());
    command(runtime, "reset", 41U);
    expectResetState(runtime);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(runtime.authority_.lastAuthorizedDirection()));
    command(runtime, "arm", 42U); TEST_ASSERT_FALSE(runtime.armed());
    moving.detectedFault = ev::FaultCode::None;
    // Internal host-only re-arm proves the preserved S18A barrier; public
    // production arm remains refused above. No calibration/gate edit.
    TEST_ASSERT_TRUE(runtime.authority_.arm(ready));
    const auto barred = runtime.authority_.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 100U), moving, fixture.limits);
    TEST_ASSERT_TRUE(barred.command.isDisabled());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(barred.outcome));
}

void test_runtime_reset_near_wrap_does_not_restart_schedule_or_claim_stop(void) {
    evhw::VehicleRuntime runtime; runtime.begin(UINT32_MAX - 30U);
    positiveCycle(); seedCaches(runtime);
    command(runtime, "reset", UINT32_MAX - 10U);
    expectResetState(runtime);
    TEST_ASSERT_FALSE(runtime.runPass(UINT32_MAX - 11U).tickRan);
    TEST_ASSERT_TRUE(runtime.runPass(UINT32_MAX - 10U).tickRan);
    TEST_ASSERT_TRUE(runtime.runPass(9U).tickRan);
    TEST_ASSERT_EQUAL_UINT32(20U, runtime.telemetry().dtMs);
    TEST_ASSERT_FALSE(runtime.telemetry().atZeroSpeed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(runtime.telemetry().commandedMode));
}

void test_reset_then_new_announcement_still_respects_production_plan_gate(void) {
    evhw::VehicleRuntime runtime; runtime.begin(0U);
    positiveCycle(); command(runtime, "reset", 41U);
    const unsigned accepted = runtime.commandsAccepted();
    command(runtime, "config set level=regional distance=5 time=15 bottle_line=2", 42U);
    TEST_ASSERT_EQUAL_UINT32(accepted + 1U, runtime.commandsAccepted());
    command(runtime, "announce", 43U);
    evhw::serialTransport::port().pump();
    TEST_ASSERT_NOT_NULL(std::strstr(Serial.bytes.c_str(), "PolicyNotUsable"));
    command(runtime, "arm", 44U);
    TEST_ASSERT_FALSE(runtime.armed());
    TEST_ASSERT_FALSE(runtime.executionPlan_.plan().valid);
    TEST_ASSERT_FALSE(evhw::vehicleConfig::kRunTimingModelValidated);
}

void test_moving_edge_before_or_after_reference_has_explicit_epoch_semantics(void) {
    for (int point = 2; point <= 3; ++point) {
        setUp(); positiveCycle(); admitted = blocked = 0U;
        injectionPoint = point;
        boundaryHook = [](int boundaryPoint) {
            if (boundaryPoint == injectionPoint) {
                boundaryHook = nullptr;
                // Physical pins can move despite PRIMASK. The callback cannot.
                levels(true, false);
                TEST_ASSERT_EQUAL_UINT32(1U, primask);
                TEST_ASSERT_EQUAL_UINT32(0U, admitted);
                TEST_ASSERT_TRUE(pending);
            }
        };
        evhw::encoderInput::resetAndReestablishReference();
        boundaryHook = nullptr;
        TEST_ASSERT_EQUAL_UINT32(0U, primask);
        TEST_ASSERT_EQUAL_UINT32(1U, admitted);
        const auto snap = evhw::encoderInput::snapshotTicks();
        // Before reads: the new A is the reference, so the pending callback
        // duplicates it (edge attribution discarded). After reads: it is one
        // positive transition against the complete new reference, counted once.
        TEST_ASSERT_EQUAL_INT32(point == 2 ? 0 : 1, snap.ticks);
        TEST_ASSERT_EQUAL_UINT32(point == 2 ? 0U : 1U, snap.countedEdges);
        TEST_ASSERT_EQUAL_UINT32(point == 2 ? 1U : 0U, snap.ignoredUpdates);
    }
}

void test_runtime_reset_inside_outer_guard_never_enables_interrupts(void) {
    evhw::VehicleRuntime runtime; runtime.begin(0U);
    positiveCycle();
    __disable_irq();
    command(runtime, "reset", 41U);
    TEST_ASSERT_EQUAL_UINT32(1U, primask);
    expectResetState(runtime);
    levels(true, false);
    TEST_ASSERT_TRUE(pending);
    TEST_ASSERT_EQUAL_UINT32(1U, primask);
    __set_PRIMASK(0U);
    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_INT32(1, evhw::encoderInput::snapshotTicks().ticks);
}

void test_begin_and_reference_only_operations_preserve_mask_and_count(void) {
    for (uint32_t mask = 0U; mask <= 1U; ++mask) {
        primask = mask;
        pins[evhw::kEncoderA] = HIGH; pins[evhw::kEncoderB] = HIGH;
        evhw::encoderInput::begin();
        TEST_ASSERT_EQUAL_UINT32(mask, primask);
        TEST_ASSERT_TRUE(evhw::encoderInput::g_decoder.hasReference());
        TEST_ASSERT_TRUE(evhw::encoderInput::g_decoder.aLevel());
        TEST_ASSERT_TRUE(evhw::encoderInput::g_decoder.bLevel());
        expectZeroCounters();
        // Existing reference is retained, not silently reset by this API.
        pins[evhw::kEncoderA] = LOW;
        evhw::encoderInput::reestablishReference();
        TEST_ASSERT_EQUAL_UINT32(mask, primask);
        TEST_ASSERT_EQUAL_INT32(1, evhw::encoderInput::snapshotTicks().ticks);
        primask = 0U;
    }
}

void test_synthetic_new_announcement_after_reset_still_requires_usable_plan(void) {
    evhw::VehicleRuntime runtime; runtime.begin(0U);
    command(runtime, "reset", 41U);
    // Host object fixture only: make announcement policy usable, NOT production
    // constants. The real builder still refuses its Provisional dependencies.
    const auto fixture = evscenario::nominalFixtures();
    runtime.run_ = ev::CompetitionStateMachine(fixture.phase, fixture.reversal);
    for (uint8_t i = 0U; i < ev::kConfigurationCheckCount; ++i) {
        runtime.configurationChecks_.setPassed(static_cast<ev::ConfigurationCheckId>(i), true);
    }
    command(runtime, "config set level=regional distance=5 time=15 bottle_line=2", 42U);
    command(runtime, "announce", 43U);
    TEST_ASSERT_TRUE(runtime.announcedConfigValid_);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::ParameterValid),
                          static_cast<int>(runtime.runPhase()));
    TEST_ASSERT_FALSE(runtime.executionPlan_.plan().valid);
    command(runtime, "arm", 44U);
    evhw::serialTransport::port().pump();
    TEST_ASSERT_NOT_NULL(std::strstr(Serial.bytes.c_str(), "ExecutionPlanNotEstablished"));
    TEST_ASSERT_FALSE(runtime.armed());
    TEST_ASSERT_EQUAL_INT(0, duty[evhw::kMotorEnable]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_runtime_reset_has_no_earlier_unprotected_decoder_reset);
    RUN_TEST(test_reset_reference_restores_enabled_interrupts);
    RUN_TEST(test_reset_reference_preserves_caller_disabled_interrupts);
    RUN_TEST(test_pending_isr_is_deferred_across_reset_and_reference);
    RUN_TEST(test_all_initial_phases_have_no_phantom_tick_and_keep_both_signs);
    RUN_TEST(test_runtime_fault_reset_clears_caches_but_preserves_direction_barrier);
    RUN_TEST(test_runtime_reset_near_wrap_does_not_restart_schedule_or_claim_stop);
    RUN_TEST(test_reset_then_new_announcement_still_respects_production_plan_gate);
    RUN_TEST(test_moving_edge_before_or_after_reference_has_explicit_epoch_semantics);
    RUN_TEST(test_runtime_reset_inside_outer_guard_never_enables_interrupts);
    RUN_TEST(test_begin_and_reference_only_operations_preserve_mask_and_count);
    RUN_TEST(test_synthetic_new_announcement_after_reset_still_requires_usable_plan);
    return UNITY_END();
}
