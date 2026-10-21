#include <cmath>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include <StandaloneArming.h>
#include <StandaloneDiagnostic.h>
#include <StandaloneIndicator.h>
#include <StandalonePreset.h>
#include <StartTrigger.h>

// Contract tests for laptop-free ("standalone") operation:
//
//   * the competition preset the standalone image loads at boot is rule-legal
//     geometry and nothing more;
//   * the physical start trigger is debounced, rejects a trigger held at boot and
//     emits one activation edge per intentional press (`StartTrigger.h`);
//   * the arm gesture needs a deliberate hold, cannot arm from a boot-held
//     trigger, and cannot re-request (`StandaloneArming.h`);
//   * the bounded standalone diagnostic applies at most one short low-effort
//     pulse, then latches disabled (`StandaloneDiagnostic.h`);
//   * the status indicator maps run state to the LED patterns the operating guide
//     documents (`StandaloneIndicator.h`).
//
// A pass here means the *logic* is right on a host. It says nothing about the
// vehicle: no motor has moved, no trigger has been pressed and no battery has been
// connected. Those are the physical steps in
// `docs/standalone-ev-operation.md`.

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint32_t kStepMs = 10U;

void expectClose(double expected, double actual) {
    TEST_ASSERT_TRUE(fabs(expected - actual) <= 1.0e-9);
}

// Steps a trigger detector in fixed increments for `durationMs`, returning the
// last verdict. `activatedCount` accumulates every activation edge, which is the
// fact the "one press, one start" tests are about.
ev::StartTriggerVerdict holdFor(ev::StartTriggerDetector& detector, uint32_t& nowMs,
                                bool rawActive, uint32_t durationMs, uint8_t& activatedCount) {
    ev::StartTriggerVerdict verdict;
    const uint32_t untilMs = nowMs + durationMs;
    do {
        verdict = detector.update(nowMs, rawActive);
        if (verdict.activated) {
            ++activatedCount;
        }
        nowMs += kStepMs;
    } while (nowMs < untilMs);
    return verdict;
}

// Brings a fresh detector to steady state with the trigger released, without
// counting the qualification itself as an activation.
void qualify(ev::StartTriggerDetector& detector, uint32_t& nowMs) {
    uint8_t ignored = 0U;
    holdFor(detector, nowMs, false, 150U, ignored);
}

}  // namespace

// ---------------------------------------------------------------------------
// The competition preset
// ---------------------------------------------------------------------------

// The preset is a complete configuration, and the single rule entry point accepts
// it. If the rules change, this test is where the preset must be re-derived.
void test_standalone_preset_is_rule_legal_event_geometry(void) {
    const ev::RunConfigFields fields = ev::standalonePreset::fields();
    TEST_ASSERT_TRUE(fields.complete());

    const ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(fields);
    TEST_ASSERT_TRUE(verdict.complete);
    TEST_ASSERT_TRUE(verdict.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None),
                          static_cast<int>(verdict.error));

    // The advertised preset values, and the Bottle Start Point the level derives
    // (rule 7.c.ii.(5)): 2.00 m at Regional/Invitational.
    const ev::RunConfig config = ev::standalonePreset::runConfig();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TournamentLevel::RegionalInvitational),
                          static_cast<int>(config.level));
    expectClose(5.00, config.targetDistanceM);
    expectClose(20.0, config.targetTimeS);
    expectClose(1.00, config.bottleLineDistanceM);
    expectClose(2.00, config.bottleStartDistanceM);

    // The run axis the announcement will use.
    const ev::RunAxisGeometry geometry = ev::runAxisGeometryFor(fields);
    expectClose(5.00, geometry.targetPointPositionM);
    expectClose(6.00, geometry.bottleLinePositionM());
}

// The preset is geometry only: an off-grid or out-of-range edit is refused by the
// same field check a typed `config set` goes through, so a mistyped preset cannot
// become held state.
void test_an_invalid_preset_edit_is_refused_before_it_is_held(void) {
    ev::RunConfigFields offGrid = ev::standalonePreset::fields();
    offGrid.targetDistanceM = 5.10;  // Regional grid is 0.25 m (rule 8.h)
    TEST_ASSERT_TRUE(ev::validateSuppliedConfigFields(offGrid).ok);
    const ev::HeldRunConfigVerdict held = ev::heldRunConfigVerdict(offGrid);
    TEST_ASSERT_TRUE(held.complete);
    TEST_ASSERT_FALSE(held.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::TargetDistanceOffInterval),
                          static_cast<int>(held.error));

    ev::RunConfigFields outOfRange = ev::standalonePreset::fields();
    outOfRange.targetDistanceM = 4.50;
    const ev::RunConfigValidation refused = ev::validateSuppliedConfigFields(outOfRange);
    TEST_ASSERT_FALSE(refused.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::TargetDistanceOutOfRange),
                          static_cast<int>(refused.error));
}

// ---------------------------------------------------------------------------
// The physical start trigger
// ---------------------------------------------------------------------------

// A trigger already held while the board boots can never produce an activation,
// however long it is held. This is the property that makes a reset during a run
// safe.
void test_a_trigger_held_during_boot_never_activates(void) {
    ev::StartTriggerDetector detector;
    uint32_t nowMs = 0U;
    uint8_t activations = 0U;
    holdFor(detector, nowMs, true, 5000U, activations);
    TEST_ASSERT_EQUAL_UINT8(0U, activations);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StartTriggerState::WaitingForInactive),
                          static_cast<int>(detector.state()));
    TEST_ASSERT_FALSE(detector.inactiveQualified());
}

// Contact bounce shorter than the debounce interval does not fire. After a real
// press, the same bounce is also not a second activation.
void test_contact_bounce_does_not_activate(void) {
    ev::StartTriggerDetector detector;
    uint32_t nowMs = 0U;
    qualify(detector, nowMs);
    TEST_ASSERT_TRUE(detector.inactiveQualified());

    uint8_t activations = 0U;
    // A 20 ms contact, shorter than the 40 ms debounce.
    holdFor(detector, nowMs, true, 20U, activations);
    holdFor(detector, nowMs, false, 50U, activations);
    TEST_ASSERT_EQUAL_UINT8(0U, activations);
    TEST_ASSERT_TRUE(detector.inactiveQualified());
}

// One press, one activation: the edge appears exactly once even while the trigger
// stays held, and a second press after a clean release produces exactly one more.
void test_one_press_produces_one_activation(void) {
    ev::StartTriggerDetector detector;
    uint32_t nowMs = 0U;
    qualify(detector, nowMs);

    uint8_t activations = 0U;
    // First press, held for a long time.
    holdFor(detector, nowMs, true, 800U, activations);
    TEST_ASSERT_EQUAL_UINT8(1U, activations);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StartTriggerState::HeldAfterTrigger),
                          static_cast<int>(detector.state()));

    // Release, requalify, press again.
    holdFor(detector, nowMs, false, 150U, activations);
    TEST_ASSERT_EQUAL_UINT8(1U, activations);
    TEST_ASSERT_TRUE(detector.inactiveQualified());
    holdFor(detector, nowMs, true, 200U, activations);
    TEST_ASSERT_EQUAL_UINT8(2U, activations);
}

// A reset is a boot boundary: a trigger held through it must be released and
// requalified before anything can start again.
void test_reset_requires_a_fresh_release_before_any_activation(void) {
    ev::StartTriggerDetector detector;
    uint32_t nowMs = 0U;
    qualify(detector, nowMs);
    uint8_t activations = 0U;
    holdFor(detector, nowMs, true, 500U, activations);
    TEST_ASSERT_EQUAL_UINT8(1U, activations);

    // The pencil is still down when the firmware resets.
    detector.reset();
    activations = 0U;
    holdFor(detector, nowMs, true, 3000U, activations);
    TEST_ASSERT_EQUAL_UINT8(0U, activations);
    TEST_ASSERT_FALSE(detector.inactiveQualified());

    // Released and requalified: the next press is a deliberate one.
    holdFor(detector, nowMs, false, 150U, activations);
    holdFor(detector, nowMs, true, 200U, activations);
    TEST_ASSERT_EQUAL_UINT8(1U, activations);
}

// ---------------------------------------------------------------------------
// The standalone arm gesture
// ---------------------------------------------------------------------------

// A trigger held from boot cannot arm the vehicle, even beyond the hold interval.
void test_the_arm_gesture_refuses_a_boot_held_trigger(void) {
    ev::StartTriggerDetector detector;
    ev::StandaloneArmingGate gate;
    uint32_t nowMs = 0U;

    bool requested = false;
    for (uint32_t index = 0U; index < 400U; ++index) {  // 4 s of holding
        const ev::StartTriggerVerdict trigger = detector.update(nowMs, true);
        const ev::StandaloneArmVerdict arm = gate.update(nowMs, trigger);
        requested = requested || arm.requestPrepare;
        nowMs += kStepMs;
    }
    TEST_ASSERT_FALSE(requested);
    TEST_ASSERT_FALSE(gate.prepared());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneArmPhase::WaitingForRelease),
                          static_cast<int>(gate.phase()));
}

// A short tap is not a gesture; a completed hold asks for preparation exactly
// once, and holding longer does not ask again.
void test_only_a_full_hold_requests_preparation_once(void) {
    ev::StartTriggerDetector detector;
    ev::StandaloneArmingGate gate;
    uint32_t nowMs = 0U;

    uint8_t ignored = 0U;
    holdFor(detector, nowMs, false, 150U, ignored);

    // A tap: released well before the 1.5 s hold.
    uint32_t requests = 0U;
    for (uint32_t index = 0U; index < 60U; ++index) {  // 600 ms
        const ev::StandaloneArmVerdict arm =
            gate.update(nowMs, detector.update(nowMs, true));
        requests += arm.requestPrepare ? 1U : 0U;
        nowMs += kStepMs;
    }
    holdFor(detector, nowMs, false, 150U, ignored);
    TEST_ASSERT_EQUAL_UINT32(0U, requests);

    // A full hold: exactly one request, whichever pass it lands on.
    for (uint32_t index = 0U; index < 300U; ++index) {  // 3 s, twice the hold
        const ev::StandaloneArmVerdict arm =
            gate.update(nowMs, detector.update(nowMs, true));
        requests += arm.requestPrepare ? 1U : 0U;
        nowMs += kStepMs;
    }
    TEST_ASSERT_EQUAL_UINT32(1U, requests);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneArmPhase::Preparing),
                          static_cast<int>(gate.phase()));
}

// A successful preparation arms the vehicle and stops asking. A refused one
// leaves it un-prepared, and the next gesture must start from a released
// trigger - so a refusal cannot turn into a retry loop.
void test_preparation_outcome_decides_readiness_and_needs_a_fresh_gesture(void) {
    ev::StartTriggerDetector detector;
    ev::StandaloneArmingGate gate;
    uint32_t nowMs = 0U;
    uint8_t ignored = 0U;
    holdFor(detector, nowMs, false, 150U, ignored);

    // Hold to the request.
    ev::StandaloneArmVerdict arm;
    do {
        arm = gate.update(nowMs, detector.update(nowMs, true));
        nowMs += kStepMs;
    } while (!arm.requestPrepare);
    TEST_ASSERT_FALSE(arm.prepared);

    // Refused arm: the vehicle stays un-prepared, and the trigger is still held.
    gate.reportPreparation(false);
    uint32_t requests = 0U;
    for (uint32_t index = 0U; index < 300U; ++index) {
        const ev::StandaloneArmVerdict again =
            gate.update(nowMs, detector.update(nowMs, true));
        requests += again.requestPrepare ? 1U : 0U;
        nowMs += kStepMs;
    }
    TEST_ASSERT_EQUAL_UINT32(0U, requests);
    TEST_ASSERT_FALSE(gate.prepared());

    // Released, stepped pass by pass so the gate can observe the release, then a
    // fresh hold: a new request is allowed.
    for (uint32_t index = 0U; index < 30U; ++index) {
        gate.update(nowMs, detector.update(nowMs, false));
        nowMs += kStepMs;
    }
    requests = 0U;
    for (uint32_t index = 0U; index < 300U; ++index) {
        const ev::StandaloneArmVerdict again =
            gate.update(nowMs, detector.update(nowMs, true));
        requests += again.requestPrepare ? 1U : 0U;
        nowMs += kStepMs;
    }
    TEST_ASSERT_EQUAL_UINT32(1U, requests);

    // This time it succeeds.
    gate.reportPreparation(true);
    TEST_ASSERT_TRUE(gate.prepared());
    const ev::StandaloneArmVerdict settled = gate.update(nowMs, detector.update(nowMs, true));
    TEST_ASSERT_TRUE(settled.prepared);
    TEST_ASSERT_FALSE(settled.requestPrepare);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneArmPhase::Prepared),
                          static_cast<int>(settled.phase));
}

// Boot state is "nothing prepared", and the gate is inert until a gesture.
void test_a_fresh_gate_is_not_prepared_and_requests_nothing(void) {
    ev::StandaloneArmingGate gate;
    ev::StartTriggerVerdict trigger;  // boot state: nothing qualified
    TEST_ASSERT_FALSE(gate.prepared());
    const ev::StandaloneArmVerdict arm = gate.update(0U, trigger);
    TEST_ASSERT_FALSE(arm.requestPrepare);
    TEST_ASSERT_FALSE(arm.prepared);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneArmPhase::WaitingForRelease),
                          static_cast<int>(arm.phase));
    TEST_ASSERT_EQUAL_UINT32(1500U, gate.holdMs());
}

// ---------------------------------------------------------------------------
// The standalone diagnostic
// ---------------------------------------------------------------------------

// The limits are the bench ceilings, and the default pulse is far inside them.
void test_the_diagnostic_limits_are_bounded_and_conservative(void) {
    const ev::BenchDriveLimits& limits = ev::kStandaloneDiagLimits;
    TEST_ASSERT_EQUAL_UINT16(150U, limits.maxEffortPermille);
    TEST_ASSERT_EQUAL_UINT32(500U, limits.maxDurationMs);
    TEST_ASSERT_EQUAL_UINT32(1U, limits.maxCommandsPerArm);
    TEST_ASSERT_TRUE(ev::benchStallProtectionReachable(limits));

    TEST_ASSERT_EQUAL_UINT16(50U, ev::kDefaultStandaloneDiagnosticConfig.effortPermille);
    TEST_ASSERT_EQUAL_UINT32(250U, ev::kDefaultStandaloneDiagnosticConfig.durationMs);
    TEST_ASSERT_TRUE(ev::kDefaultStandaloneDiagnosticConfig.durationMs <
                     ev::kStandaloneDiagMaxDurationMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(ev::kDefaultStandaloneDiagnosticConfig.directionMode));
}

// Boot: nothing is driving, and a trigger held from boot keeps it that way.
void test_the_diagnostic_stays_disabled_while_the_trigger_is_held_at_boot(void) {
    ev::StandaloneDiagnosticController diagnostic;
    uint32_t nowMs = 0U;
    for (uint32_t index = 0U; index < 500U; ++index) {  // 5 s
        const ev::StandaloneDiagnosticStep step =
            diagnostic.update(nowMs, true, 0, false, false, false, false);
        TEST_ASSERT_FALSE(step.ownsMotorPins);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                              static_cast<int>(step.mode));
        nowMs += kStepMs;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::WaitingForTriggerRelease),
                          static_cast<int>(diagnostic.phase()));
}

// One press: one bounded pulse, at the configured conservative effort, owned by
// the diagnostic, followed by a distinct completion with the motor disabled.
void test_one_press_applies_one_bounded_pulse_and_then_completes(void) {
    ev::StandaloneDiagnosticController diagnostic;
    uint32_t nowMs = 0U;

    // Qualify with the trigger released.
    bool ready = false;
    for (uint32_t index = 0U; index < 30U; ++index) {
        const ev::StandaloneDiagnosticStep step =
            diagnostic.update(nowMs, false, 0, false, false, false, false);
        ready = ready || (step.phase == ev::StandaloneDiagnosticPhase::Ready);
        nowMs += kStepMs;
    }
    TEST_ASSERT_TRUE(ready);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::Ready),
                          static_cast<int>(diagnostic.phase()));

    // Press and hold: the pulse starts once, at 50 permille.
    bool started = false;
    ev::StandaloneDiagnosticStep step;
    for (uint32_t index = 0U; index < 30U && !started; ++index) {
        step = diagnostic.update(nowMs, true, 0, false, false, false, false);
        if (step.pulseStartedThisPass) {
            started = true;
            TEST_ASSERT_TRUE(step.ownsMotorPins);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                                  static_cast<int>(step.mode));
            TEST_ASSERT_EQUAL_UINT16(50U, step.permille);
        }
        nowMs += kStepMs;
    }
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::ActivePulse),
                          static_cast<int>(diagnostic.phase()));

    // Run past the 250 ms bounded interval with the trigger still held.
    bool finished = false;
    for (uint32_t index = 0U; index < 60U; ++index) {
        step = diagnostic.update(nowMs, true, 0, false, false, false, false);
        if (step.pulseFinishedThisPass) {
            finished = true;
        }
        nowMs += kStepMs;
    }
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::Completed),
                          static_cast<int>(diagnostic.phase()));
    TEST_ASSERT_FALSE(step.ownsMotorPins);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(step.mode));
    TEST_ASSERT_EQUAL_UINT16(0U, step.permille);

    // Released and pressed again: completed is terminal for this power cycle.
    for (uint32_t index = 0U; index < 30U; ++index) {
        diagnostic.update(nowMs, false, 0, false, false, false, false);
        nowMs += kStepMs;
    }
    for (uint32_t index = 0U; index < 40U; ++index) {
        step = diagnostic.update(nowMs, true, 0, false, false, false, false);
        TEST_ASSERT_FALSE(step.ownsMotorPins);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::Completed),
                              static_cast<int>(diagnostic.phase()));
        nowMs += kStepMs;
    }
}

// An external fault, an armed competition run or a run underway outranks the
// diagnostic: the pulse aborts at once and stays latched off.
void test_an_external_fault_aborts_and_latches_the_diagnostic(void) {
    struct Case {
        bool armed;
        bool faulted;
        bool underway;
        bool orderViolation;
    };
    const Case cases[] = {{false, true, false, false},
                          {true, false, false, false},
                          {false, false, true, false},
                          {false, false, false, true}};
    for (uint8_t caseIndex = 0U; caseIndex < 4U; ++caseIndex) {
        ev::StandaloneDiagnosticController diagnostic;
        uint32_t nowMs = 0U;
        // Qualify and start a pulse.
        for (uint32_t index = 0U; index < 30U; ++index) {
            diagnostic.update(nowMs, false, 0, false, false, false, false);
            nowMs += kStepMs;
        }
        bool started = false;
        for (uint32_t index = 0U; index < 30U && !started; ++index) {
            const ev::StandaloneDiagnosticStep step =
                diagnostic.update(nowMs, true, 0, false, false, false, false);
            started = step.pulseStartedThisPass;
            nowMs += kStepMs;
        }
        TEST_ASSERT_TRUE(started);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::ActivePulse),
                              static_cast<int>(diagnostic.phase()));

        const ev::StandaloneDiagnosticStep step =
            diagnostic.update(nowMs, true, 0, cases[caseIndex].armed,
                              cases[caseIndex].faulted, cases[caseIndex].underway,
                              cases[caseIndex].orderViolation);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::Faulted),
                              static_cast<int>(diagnostic.phase()));
        TEST_ASSERT_FALSE(step.ownsMotorPins);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                              static_cast<int>(step.mode));
    }
}

// The pulse refuses a runtime that is armed even before it starts.
void test_the_diagnostic_never_takes_the_motor_from_an_armed_run(void) {
    ev::StandaloneDiagnosticController diagnostic;
    uint32_t nowMs = 0U;
    for (uint32_t index = 0U; index < 30U; ++index) {
        diagnostic.update(nowMs, false, 0, true, false, false, false);
        nowMs += kStepMs;
    }
    for (uint32_t index = 0U; index < 40U; ++index) {
        const ev::StandaloneDiagnosticStep step =
            diagnostic.update(nowMs, true, 0, true, false, false, false);
        TEST_ASSERT_FALSE(step.ownsMotorPins);
        TEST_ASSERT_FALSE(step.pulseStartedThisPass);
        nowMs += kStepMs;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::StandaloneDiagnosticPhase::Faulted),
                          static_cast<int>(diagnostic.phase()));
}

// ---------------------------------------------------------------------------
// The status indicator
// ---------------------------------------------------------------------------

// The mapping is exhaustive over the run phases, and only an armed run in the
// Armed phase with a released trigger reads as ready.
void test_the_production_indicator_maps_run_state_to_led_patterns(void) {
    using ev::StandaloneIndicatorState;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Faulted),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Fault, false, false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Finished),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Done, false, false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Running),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Approach, true, false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Ready),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Armed, true, false, true)));
    // Armed but the trigger is still held: not startable, so not "ready".
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Waiting),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Armed, true, false, false)));
    // Announced but not armed: waiting.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Waiting),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::ParameterValid, false, false, true)));
    // A latched fault outranks everything.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StandaloneIndicatorState::Faulted),
                          static_cast<int>(ev::productionIndicatorState(
                              ev::RunPhase::Approach, true, true, true)));
}

// The patterns are distinct and each is reachable: steady for ready, a slow
// heartbeat for waiting, a fast one for a fault.
void test_the_led_patterns_are_distinct(void) {
    using ev::StandaloneIndicatorState;

    // Ready is steady on at every sampled instant.
    for (uint32_t nowMs = 0U; nowMs < 2500U; nowMs += 37U) {
        TEST_ASSERT_TRUE(
            ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Ready, nowMs));
    }

    // Waiting: on only in the first 100 ms of each second.
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Waiting, 0U));
    TEST_ASSERT_FALSE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Waiting, 500U));
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Waiting, 1050U));

    // Faulted blinks faster than running: 50 ms on / 50 ms off versus 100/100.
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Faulted, 0U));
    TEST_ASSERT_FALSE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Faulted, 60U));
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Running, 60U));
    TEST_ASSERT_FALSE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Running, 120U));

    // Finished: two pulses at the start of each 1200 ms cycle, then quiet.
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Finished, 0U));
    TEST_ASSERT_FALSE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Finished, 150U));
    TEST_ASSERT_TRUE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Finished, 250U));
    TEST_ASSERT_FALSE(ev::standaloneIndicatorLedOn(StandaloneIndicatorState::Finished, 700U));
}

// Every state and phase has a wire-safe (comma-free) name, because these names
// travel into diagnostic output.
void test_standalone_names_are_wire_safe(void) {
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::StandaloneIndicatorState::Faulted);
         ++index) {
        const char* name = ev::standaloneIndicatorStateName(
            static_cast<ev::StandaloneIndicatorState>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
    }
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::StandaloneDiagnosticPhase::Faulted); ++index) {
        const char* name = ev::standaloneDiagnosticPhaseName(
            static_cast<ev::StandaloneDiagnosticPhase>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
    }
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::StandaloneArmPhase::Prepared); ++index) {
        const char* name = ev::standaloneArmPhaseName(static_cast<ev::StandaloneArmPhase>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
    }
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::StartTriggerState::HeldAfterTrigger);
         ++index) {
        const char* name = ev::startTriggerStateName(static_cast<ev::StartTriggerState>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_standalone_preset_is_rule_legal_event_geometry);
    RUN_TEST(test_an_invalid_preset_edit_is_refused_before_it_is_held);
    RUN_TEST(test_a_trigger_held_during_boot_never_activates);
    RUN_TEST(test_contact_bounce_does_not_activate);
    RUN_TEST(test_one_press_produces_one_activation);
    RUN_TEST(test_reset_requires_a_fresh_release_before_any_activation);
    RUN_TEST(test_the_arm_gesture_refuses_a_boot_held_trigger);
    RUN_TEST(test_only_a_full_hold_requests_preparation_once);
    RUN_TEST(test_preparation_outcome_decides_readiness_and_needs_a_fresh_gesture);
    RUN_TEST(test_a_fresh_gate_is_not_prepared_and_requests_nothing);
    RUN_TEST(test_the_diagnostic_limits_are_bounded_and_conservative);
    RUN_TEST(test_the_diagnostic_stays_disabled_while_the_trigger_is_held_at_boot);
    RUN_TEST(test_one_press_applies_one_bounded_pulse_and_then_completes);
    RUN_TEST(test_an_external_fault_aborts_and_latches_the_diagnostic);
    RUN_TEST(test_the_diagnostic_never_takes_the_motor_from_an_armed_run);
    RUN_TEST(test_the_production_indicator_maps_run_state_to_led_patterns);
    RUN_TEST(test_the_led_patterns_are_distinct);
    RUN_TEST(test_standalone_names_are_wire_safe);
    return UNITY_END();
}
