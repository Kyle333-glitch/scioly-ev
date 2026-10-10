#include <cstdint>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <VehicleState.h>

// Contract tests for the final permission boundary between a motion request and
// powered motion. The numbers here are test fixtures only; they are deliberately
// not calibration values or limit values for any real vehicle.

// Pinned at compile time: a command that can be actuated cannot be fabricated.
// If either of these ever gains a public constructor, the single-permission-path
// contract in SOFTWARE_ARCHITECTURE.md is broken and this target stops compiling.
static_assert(!std::is_default_constructible<ev::AuthorizedMotorCommand>::value,
              "an authorized command must come from the authority");
static_assert(!std::is_default_constructible<ev::AuthorizationResult>::value,
              "denials must still carry a safe command");

void setUp(void) {}
void tearDown(void) {}

namespace {

// Fixture values, not vehicle data.
ev::SafetyLimits limitsWithMaxEffort(uint16_t maxEffortPermille) {
    const ev::ValueSource source = ev::ValueSource::Measured;
    return ev::SafetyLimits{
        ev::Sourced<uint16_t>(maxEffortPermille, source),
        ev::Sourced<double>(2.0, source),
        ev::Sourced<double>(1.0, source),
        ev::Sourced<double>(30.0, source),
        ev::Sourced<double>(9.0, source),
    };
}

ev::SafetyReadiness fullyReady() {
    ev::SafetyReadiness readiness;
    readiness.configurationValid = true;
    readiness.calibrationUsable = true;
    readiness.limitsUsable = true;
    return readiness;
}

// A state in which every permission a drive needs is satisfied.
ev::SafetySystemState readyState(bool atZeroSpeed = true, uint32_t nowMs = 1000U) {
    ev::SafetySystemState state;
    state.readiness = fullyReady();
    state.requiredDataFresh = true;
    state.atZeroSpeed = atZeroSpeed;
    state.nowMs = nowMs;
    state.detectedFault = ev::FaultCode::None;
    return state;
}

ev::MotorEffort driveForward(uint16_t magnitudePermille) {
    return ev::MotorEffort(ev::MotorMode::DriveForward, magnitudePermille);
}

ev::MotorEffort driveReverse(uint16_t magnitudePermille) {
    return ev::MotorEffort(ev::MotorMode::DriveReverse, magnitudePermille);
}

void expectOutcome(ev::AuthzOutcome expected, const ev::AuthorizationResult& result) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.outcome));
}

// A refusal is never "no command": it is always a concrete, non-powered command.
void expectSafeStop(const ev::AuthorizationResult& result) {
    TEST_ASSERT_FALSE(result.command.isDriving());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                          static_cast<int>(result.command.actuationClass()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(result.command.mode()));
    TEST_ASSERT_EQUAL_INT(0, result.command.magnitudePermille());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(result.command.poweredDirection()));
}

// Arms an authority that is already fully ready and healthy.
void armHealthy(ev::SafetyAuthority& authority) {
    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
    TEST_ASSERT_TRUE(authority.isArmed());
}

uint32_t nextRandom(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state >> 8;
}

}  // namespace

// ---------------------------------------------------------------------------
// Arming and disarming
// ---------------------------------------------------------------------------

void test_arm_requires_complete_readiness(void) {
    ev::SafetyAuthority authority;
    TEST_ASSERT_FALSE(authority.isArmed());

    ev::SafetyReadiness readiness = fullyReady();
    readiness.limitsUsable = false;
    TEST_ASSERT_FALSE(authority.arm(readiness));
    TEST_ASSERT_FALSE(authority.isArmed());

    readiness = fullyReady();
    readiness.calibrationUsable = false;
    TEST_ASSERT_FALSE(authority.arm(readiness));
    TEST_ASSERT_FALSE(authority.isArmed());

    readiness = fullyReady();
    readiness.configurationValid = false;
    TEST_ASSERT_FALSE(authority.arm(readiness));
    TEST_ASSERT_FALSE(authority.isArmed());

    // Default-built readiness authorizes nothing.
    TEST_ASSERT_FALSE(authority.arm(ev::SafetyReadiness{}));
    TEST_ASSERT_FALSE(authority.isArmed());

    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
    TEST_ASSERT_TRUE(authority.isArmed());
}

void test_arm_is_refused_while_a_fault_is_latched(void) {
    ev::SafetyAuthority authority;
    ev::SafetySystemState state = readyState();
    state.detectedFault = ev::FaultCode::InvalidRunConfig;
    const ev::AuthorizationResult latching =
        authority.authorize(driveForward(100), state, limitsWithMaxEffort(1000));
    // Pin that the precondition really happened, so a future refactor fails
    // this test for the right reason rather than on an unrelated denial.
    expectOutcome(ev::AuthzOutcome::ForcedFaultStop, latching);
    TEST_ASSERT_FALSE(authority.isArmed());

    TEST_ASSERT_FALSE(authority.arm(fullyReady()));
    TEST_ASSERT_FALSE(authority.isArmed());
}

void test_disarm_immediately_denies_drive(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(400), readyState(), limits));

    authority.disarm();
    TEST_ASSERT_FALSE(authority.isArmed());
    const ev::AuthorizationResult denied =
        authority.authorize(driveForward(400), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::DeniedNotArmed, denied);
    expectSafeStop(denied);
}

// ---------------------------------------------------------------------------
// Fault latch, fault response, and clearing
// ---------------------------------------------------------------------------

void test_detected_fault_latches_disarms_and_forces_a_stop(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    ev::SafetySystemState state = readyState();
    state.nowMs = 4321U;
    state.detectedFault = ev::FaultCode::MissingSafetyLimits;

    // The fault and a drive request arrive in the same cycle: the fault wins.
    const ev::AuthorizationResult result = authority.authorize(driveForward(500), state, limits);
    expectOutcome(ev::AuthzOutcome::ForcedFaultStop, result);
    expectSafeStop(result);
    TEST_ASSERT_FALSE(authority.isArmed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::MissingSafetyLimits),
                          static_cast<int>(authority.latchedFault().code));
    TEST_ASSERT_TRUE(authority.latchedFault().latched);
    TEST_ASSERT_EQUAL_INT(4321, static_cast<int>(authority.latchedFault().raisedAtMs));
}

void test_fault_stop_cannot_be_overridden_by_later_drive_requests(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    ev::SafetySystemState faulty = readyState();
    faulty.detectedFault = ev::FaultCode::InvalidMotorEffort;
    authority.authorize(driveForward(200), faulty, limits);

    // The condition clears on the input, but the fault stays latched until it is
    // explicitly cleared, and the authority stays disarmed.
    for (int i = 0; i < 5; ++i) {
        const ev::AuthorizationResult result =
            authority.authorize(driveForward(900), readyState(true, 2000U + static_cast<uint32_t>(i)), limits);
        expectOutcome(ev::AuthzOutcome::ForcedFaultStop, result);
        expectSafeStop(result);
        TEST_ASSERT_FALSE(authority.isArmed());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::InvalidMotorEffort),
                              static_cast<int>(authority.latchedFault().code));
    }
    // A later, different fault must not silently replace the fault of record.
    ev::SafetySystemState other = readyState();
    other.detectedFault = ev::FaultCode::ProvisionalCalibration;
    authority.authorize(driveForward(0), other, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::InvalidMotorEffort),
                          static_cast<int>(authority.latchedFault().code));
}

void test_clear_fault_works_only_when_a_fault_is_latched(void) {
    ev::SafetyAuthority authority;
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // Nothing latched: there is nothing to clear.
    armHealthy(authority);
    TEST_ASSERT_FALSE(authority.clearFault());

    ev::SafetySystemState faulty = readyState();
    faulty.detectedFault = ev::FaultCode::InvalidRunConfig;
    authority.authorize(driveForward(100), faulty, limits);
    // Latching a fault always disarms, so clearing is never a way to keep
    // driving on a system that just reported a fault.
    TEST_ASSERT_FALSE(authority.isArmed());

    TEST_ASSERT_TRUE(authority.clearFault());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(authority.latchedFault().code));
    // Clearing is not idempotent in the permissive direction.
    TEST_ASSERT_FALSE(authority.clearFault());

    // Only a fresh arming decision restores powered motion.
    TEST_ASSERT_FALSE(authority.isArmed());
    armHealthy(authority);
}

void test_clear_fault_while_armed_is_refused(void) {
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // One authority latches a fault. Latching always disarms, and while the
    // fault is latched arming is refused.
    ev::SafetyAuthority faulted;
    ev::SafetySystemState faulty = readyState();
    faulty.detectedFault = ev::FaultCode::InvalidRunConfig;
    faulted.authorize(driveForward(100), faulty, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::InvalidRunConfig),
                          static_cast<int>(faulted.latchedFault().code));
    TEST_ASSERT_FALSE(faulted.arm(fullyReady()));
    TEST_ASSERT_FALSE(faulted.isArmed());

    // The armed guard is reachable and load-bearing, not dead code: an
    // authority that has never latched a fault can be armed, and clearing
    // while armed is refused.
    ev::SafetyAuthority healthy;
    armHealthy(healthy);
    TEST_ASSERT_FALSE(healthy.clearFault());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(healthy.latchedFault().code));
}

void test_clear_fault_ends_the_arming_period(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(400), readyState(), limits));

    // The fault arrives while the vehicle is not reported stopped, so the
    // direction memory survives it. Latching always disarms.
    ev::SafetySystemState faulty = readyState(/*atZeroSpeed=*/false);
    faulty.detectedFault = ev::FaultCode::ProvisionalCalibration;
    expectOutcome(ev::AuthzOutcome::ForcedFaultStop,
                  authority.authorize(driveForward(400), faulty, limits));
    TEST_ASSERT_FALSE(authority.isArmed());

    TEST_ASSERT_TRUE(authority.clearFault());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(authority.latchedFault().code));
    // Clearing ends the arming period: it never restores powered motion itself.
    TEST_ASSERT_FALSE(authority.isArmed());

    // Re-arming is a separate, fresh decision.
    TEST_ASSERT_TRUE(authority.arm(fullyReady()));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(400), readyState(), limits));
    // The direction memory survived the fault, so a flip is still refused.
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveReverse(400), readyState(), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));
}

void test_zero_timestamp_does_not_suppress_fault_latching(void) {
    // nowMs must be a real millis() reading supplied by the vehicle layer; a
    // forgotten assignment is not detectable at this boundary, so a zero stamp
    // is accepted and must not weaken the latch.
    ev::SafetyAuthority authority;
    ev::SafetySystemState state = readyState(/*atZeroSpeed=*/true, /*nowMs=*/0U);
    state.detectedFault = ev::FaultCode::InvalidRunConfig;

    const ev::AuthorizationResult result =
        authority.authorize(driveForward(100), state, limitsWithMaxEffort(1000));
    expectOutcome(ev::AuthzOutcome::ForcedFaultStop, result);
    expectSafeStop(result);
    TEST_ASSERT_TRUE(authority.latchedFault().latched);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::InvalidRunConfig),
                          static_cast<int>(authority.latchedFault().code));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(authority.latchedFault().raisedAtMs));
    TEST_ASSERT_FALSE(authority.isArmed());
}

void test_fault_response_is_independent_of_fault_identity(void) {
    // Knowing what went wrong must not silently decide what the motor does.
    // Every software fault present today gets the conservative response, and no
    // fault identity maps to anything but a stop policy.
    const ev::FaultCode codes[] = {
        ev::FaultCode::None,           ev::FaultCode::InvalidRunConfig,
        ev::FaultCode::ProvisionalCalibration, ev::FaultCode::MissingSafetyLimits,
        ev::FaultCode::InvalidMotorEffort,
    };
    for (ev::FaultCode code : codes) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultStopPolicy::Disable),
                              static_cast<int>(ev::faultStopPolicyFor(code)));
    }
}

void test_stop_command_for_policy_can_never_produce_a_drive(void) {
    const ev::FaultStopPolicy policies[] = {ev::FaultStopPolicy::Disable,
                                            ev::FaultStopPolicy::Coast,
                                            ev::FaultStopPolicy::Brake};
    for (ev::FaultStopPolicy policy : policies) {
        const ev::AuthorizedMotorCommand command = ev::SafetyAuthority::stopCommandFor(policy);
        TEST_ASSERT_FALSE(command.isDriving());
        TEST_ASSERT_EQUAL_INT(0, command.magnitudePermille());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                              static_cast<int>(command.poweredDirection()));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(ev::SafetyAuthority::stopCommandFor(
                                               ev::FaultStopPolicy::Disable).mode()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Coast),
                          static_cast<int>(ev::SafetyAuthority::stopCommandFor(
                                               ev::FaultStopPolicy::Coast).mode()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Brake),
                          static_cast<int>(ev::SafetyAuthority::stopCommandFor(
                                               ev::FaultStopPolicy::Brake).mode()));
}

void test_each_fault_code_can_be_latched_cleared_and_rearmed(void) {
    const ev::FaultCode codes[] = {
        ev::FaultCode::InvalidRunConfig, ev::FaultCode::ProvisionalCalibration,
        ev::FaultCode::MissingSafetyLimits, ev::FaultCode::InvalidMotorEffort,
    };
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    for (ev::FaultCode code : codes) {
        ev::SafetyAuthority authority;
        ev::SafetySystemState faulty = readyState();
        faulty.detectedFault = code;
        authority.authorize(driveForward(300), faulty, limits);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(code),
                              static_cast<int>(authority.latchedFault().code));
        TEST_ASSERT_TRUE(authority.clearFault());
        TEST_ASSERT_TRUE(authority.arm(fullyReady()));
        expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                      authority.authorize(driveForward(300), readyState(), limits));
    }
}

// ---------------------------------------------------------------------------
// DISABLED / BRAKE / DRIVE semantics
// ---------------------------------------------------------------------------

void test_mode_actuation_classes_match_their_documented_semantics(void) {
    // Direction and effort are meaningful only for a Drive action.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                          static_cast<int>(ev::actuationClassOf(ev::MotorMode::Disabled)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                          static_cast<int>(ev::actuationClassOf(ev::MotorMode::Coast)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Brake),
                          static_cast<int>(ev::actuationClassOf(ev::MotorMode::Brake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(ev::actuationClassOf(ev::MotorMode::DriveForward)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(ev::actuationClassOf(ev::MotorMode::DriveReverse)));

    // An unknown mode is never classified as powered.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                          static_cast<int>(ev::actuationClassOf(static_cast<ev::MotorMode>(200))));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(ev::poweredDirectionOf(ev::MotorMode::Disabled)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(ev::poweredDirectionOf(ev::MotorMode::Coast)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(ev::poweredDirectionOf(ev::MotorMode::Brake)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(ev::poweredDirectionOf(ev::MotorMode::DriveForward)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(ev::poweredDirectionOf(ev::MotorMode::DriveReverse)));
}

void test_authorized_command_exposes_mode_effort_class_and_direction(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    const ev::AuthorizationResult result =
        authority.authorize(driveReverse(650), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, result);
    TEST_ASSERT_TRUE(result.command.isDriving());
    TEST_ASSERT_FALSE(result.command.isDisabled());
    TEST_ASSERT_FALSE(result.command.isBraking());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveReverse),
                          static_cast<int>(result.command.mode()));
    TEST_ASSERT_EQUAL_INT(650, result.command.magnitudePermille());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(result.command.poweredDirection()));
}

void test_non_powered_commands_are_always_available_and_unguided_by_arming(void) {
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // Braking in particular must work without arming, and while a fault is
    // latched, because it removes energy rather than adding it.
    {
        ev::SafetyAuthority authority;
        const ev::AuthorizationResult result =
            authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::AuthorizedStop, result);
        TEST_ASSERT_TRUE(result.command.isBraking());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Brake),
                              static_cast<int>(result.command.actuationClass()));
        TEST_ASSERT_EQUAL_INT(0, result.command.magnitudePermille());
    }
    {
        // While a fault is latched the fault response dictates the command, so a
        // requested brake is replaced by the policy stop. That is the
        // conservative choice while driver braking behavior is unverified; a
        // measured fault may select a different policy later.
        ev::SafetyAuthority authority;
        armHealthy(authority);
        ev::SafetySystemState faulty = readyState();
        faulty.detectedFault = ev::FaultCode::InvalidRunConfig;
        authority.authorize(driveForward(100), faulty, limits);

        const ev::AuthorizationResult result =
            authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::ForcedFaultStop, result);
        expectSafeStop(result);
    }
    {
        ev::SafetyAuthority authority;
        const ev::AuthorizationResult coast =
            authority.authorize(ev::MotorEffort(ev::MotorMode::Coast, 0), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::AuthorizedStop, coast);
        TEST_ASSERT_TRUE(coast.command.isCoasting());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Disabled),
                              static_cast<int>(coast.command.actuationClass()));

        const ev::AuthorizationResult disabled =
            authority.authorize(ev::MotorEffort(ev::MotorMode::Disabled, 0), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::AuthorizedStop, disabled);
        TEST_ASSERT_TRUE(disabled.command.isDisabled());
    }
}

// ---------------------------------------------------------------------------
// Drive permission preconditions
// ---------------------------------------------------------------------------

void test_drive_requires_arming_readiness_and_fresh_data(void) {
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // Not armed.
    {
        ev::SafetyAuthority authority;
        const ev::AuthorizationResult result =
            authority.authorize(driveForward(500), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::DeniedNotArmed, result);
        expectSafeStop(result);
    }
    // Armed, but the configuration/calibration/limits are no longer usable.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        ev::SafetySystemState state = readyState();
        state.readiness.limitsUsable = false;
        const ev::AuthorizationResult result = authority.authorize(driveForward(500), state, limits);
        expectOutcome(ev::AuthzOutcome::DeniedNotReady, result);
        expectSafeStop(result);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                              static_cast<int>(authority.lastAuthorizedDirection()));
    }
    // Armed, ready, but the required measurement data is not fresh.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        ev::SafetySystemState state = readyState();
        state.requiredDataFresh = false;
        const ev::AuthorizationResult result = authority.authorize(driveForward(500), state, limits);
        expectOutcome(ev::AuthzOutcome::DeniedStaleData, result);
        expectSafeStop(result);
    }
    // Timing state is irrelevant to permission: only the declared flags matter.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        ev::SafetySystemState state = readyState(true, 0U);
        expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                      authority.authorize(driveForward(500), state, limits));
    }
}

void test_illegal_effort_never_produces_a_powered_command(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    const ev::MotorEffort illegal[] = {
        ev::MotorEffort(static_cast<ev::MotorMode>(200), 0),  // unknown mode
        ev::MotorEffort(ev::MotorMode::DriveForward, 1001),    // above full scale
        ev::MotorEffort(ev::MotorMode::Brake, 400),            // magnitude on non-driving
        ev::MotorEffort(ev::MotorMode::Disabled, 1),
        ev::MotorEffort(ev::MotorMode::Coast, 900),
    };
    for (const ev::MotorEffort& request : illegal) {
        TEST_ASSERT_FALSE(ev::validateMotorEffort(request).ok);
        const ev::AuthorizationResult result =
            authority.authorize(request, readyState(), limits);
        expectOutcome(ev::AuthzOutcome::DeniedIllegalEffort, result);
        expectSafeStop(result);
    }
}

void test_full_scale_effort_is_not_an_illegal_request(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::AuthorizationResult result =
        authority.authorize(driveForward(1000), readyState(), limitsWithMaxEffort(1000));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, result);
    TEST_ASSERT_EQUAL_INT(1000, result.command.magnitudePermille());
}

// ---------------------------------------------------------------------------
// Effort ceiling
// ---------------------------------------------------------------------------

void test_requested_effort_above_the_safety_ceiling_is_clamped(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(600);

    const ev::AuthorizationResult result =
        authority.authorize(driveForward(950), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedClamped, result);
    TEST_ASSERT_TRUE(result.command.isDriving());
    TEST_ASSERT_EQUAL_INT(600, result.command.magnitudePermille());
}

void test_requested_effort_at_or_below_the_ceiling_is_granted_as_asked(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(600);

    const ev::AuthorizationResult atCeiling =
        authority.authorize(driveForward(600), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, atCeiling);
    TEST_ASSERT_EQUAL_INT(600, atCeiling.command.magnitudePermille());

    const ev::AuthorizationResult below =
        authority.authorize(driveForward(1), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, below);
    TEST_ASSERT_EQUAL_INT(1, below.command.magnitudePermille());

    const ev::AuthorizationResult zero =
        authority.authorize(driveForward(0), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, zero);
    TEST_ASSERT_EQUAL_INT(0, zero.command.magnitudePermille());
}

// ---------------------------------------------------------------------------
// Repeated commands
// ---------------------------------------------------------------------------

void test_repeated_identical_commands_are_stable(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    for (int i = 0; i < 200; ++i) {
        const ev::AuthorizationResult result =
            authority.authorize(driveForward(700), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::AuthorizedDrive, result);
        TEST_ASSERT_EQUAL_INT(700, result.command.magnitudePermille());
        TEST_ASSERT_TRUE(authority.isArmed());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                              static_cast<int>(authority.lastAuthorizedDirection()));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                              static_cast<int>(authority.latchedFault().code));
    }
}

// ---------------------------------------------------------------------------
// Direction change
// ---------------------------------------------------------------------------

void test_direct_powered_direction_flip_is_refused_even_at_claimed_zero_speed(void) {
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // Forward then reverse.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                      authority.authorize(driveForward(500), readyState(), limits));
        const ev::AuthorizationResult flip =
            authority.authorize(driveReverse(500), readyState(/*atZeroSpeed=*/true), limits);
        expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange, flip);
        expectSafeStop(flip);
        // The refused flip must not rewrite the remembered direction.
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                              static_cast<int>(authority.lastAuthorizedDirection()));
    }
    // Reverse then forward.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                      authority.authorize(driveReverse(500), readyState(), limits));
        const ev::AuthorizationResult flip =
            authority.authorize(driveForward(500), readyState(), limits);
        expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange, flip);
        expectSafeStop(flip);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                              static_cast<int>(authority.lastAuthorizedDirection()));
    }
    // A flip stays refused across many cycles, so it cannot be brute-forced.
    {
        ev::SafetyAuthority authority;
        armHealthy(authority);
        authority.authorize(driveForward(500), readyState(), limits);
        for (int i = 0; i < 50; ++i) {
            expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                          authority.authorize(driveReverse(500), readyState(), limits));
        }
    }
}

void test_same_direction_repeated_drive_is_not_a_direction_change(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);
    authority.authorize(driveForward(300), readyState(), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(900), readyState(), limits));
}

void test_direction_change_is_legal_through_a_zero_speed_stop(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    // Legal sequence: drive forward, decelerate, stop, settle (reported zero
    // speed), then reverse.
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(600), readyState(), limits));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(100), readyState(false), limits));
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(ev::MotorEffort(ev::MotorMode::Coast, 0),
                                      readyState(false), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    // Stopped and settled: the direction memory is cleared here.
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0), readyState(true),
                                      limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    const ev::AuthorizationResult reverse =
        authority.authorize(driveReverse(500), readyState(true), limits);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive, reverse);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(reverse.command.poweredDirection()));
}

void test_stop_without_zero_speed_does_not_clear_the_direction_memory(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    authority.authorize(driveForward(600), readyState(), limits);

    // A stop command issued while the vehicle may still be moving must not open
    // the door to an immediate reversal.
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0),
                                      readyState(false), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveReverse(500), readyState(false), limits));

    // A denial does not clear it either.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));
}

void test_disarming_does_not_forget_the_last_powered_direction(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    authority.authorize(driveReverse(500), readyState(), limits);
    authority.disarm();
    armHealthy(authority);

    // Re-arming must not be a shortcut around the zero-speed path.
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveForward(500), readyState(), limits));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(500), readyState(), limits));
}

// ---------------------------------------------------------------------------
// Reinitialization
// ---------------------------------------------------------------------------

void test_reset_preserves_the_direction_barrier_and_requires_a_fresh_arm(void) {
    // S18A/P2 correction: this test used to require that reset() forget the
    // remembered powered direction. That is the H5 defect: disarm/reset/re-arm
    // then authorized the opposite direction with no stop evidence, because a
    // software reset was treated as a verified mechanical stop. The barrier
    // now survives a reset; only a zero-speed stop discharges it.
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    authority.authorize(driveForward(500), readyState(), limits);

    ev::SafetySystemState faulty = readyState(/*atZeroSpeed=*/false);
    faulty.detectedFault = ev::FaultCode::ProvisionalCalibration;
    authority.authorize(driveForward(500), faulty, limits);
    TEST_ASSERT_FALSE(authority.isArmed());

    authority.reset();
    TEST_ASSERT_FALSE(authority.isArmed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(authority.latchedFault().code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    // After reset a drive needs a new arming decision: nothing resumes.
    expectOutcome(ev::AuthzOutcome::DeniedNotArmed,
                  authority.authorize(driveForward(500), readyState(), limits));

    // Re-arming restores same-direction motion but not the opposite: the
    // reset proved nothing about the drivetrain.
    armHealthy(authority);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(500), readyState(), limits));
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveReverse(500), readyState(), limits));

    // A stop taken while zero speed is reported discharges the barrier, and
    // only then is the opposite direction authorized.
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0U),
                                      readyState(/*atZeroSpeed=*/true), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(500), readyState(), limits));
}

void test_reset_after_reverse_bars_forward_until_a_verified_stop(void) {
    // The H5 lifecycle in the other direction: reverse-powered, then
    // disarm/reset/re-arm while the evidence still reports motion.
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(400), readyState(), limits));
    authority.disarm();
    authority.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    armHealthy(authority);
    ev::SafetySystemState moving = readyState(/*atZeroSpeed=*/false);
    const ev::AuthorizationResult refused =
        authority.authorize(driveForward(400), moving, limits);
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange, refused);
    expectSafeStop(refused);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(authority.lastAuthorizedDirection()));
}

void test_reset_immediately_after_a_powered_command_keeps_the_barrier(void) {
    // No fault, no disarm ceremony: a reset one cycle after a powered command
    // still proves no standstill.
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(500), readyState(), limits));
    authority.reset();

    armHealthy(authority);
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveReverse(500), readyState(), limits));
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(500), readyState(), limits));
}

void test_a_stop_without_reported_zero_speed_never_discharges_after_reset(void) {
    // Missing, stale, or moving evidence cannot discharge the barrier the
    // reset preserved: only a stop taken while zero speed is reported does.
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(500), readyState(), limits));
    authority.reset();
    armHealthy(authority);

    const ev::MotorEffort brake(ev::MotorMode::Brake, 0U);
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(brake, readyState(/*atZeroSpeed=*/false), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveReverse(500), readyState(), limits));
}

void test_a_verified_stop_before_reset_leaves_no_barrier_behind(void) {
    // Reset after a genuinely discharged barrier: the stop already proved
    // standstill through the zero-speed path, so the fresh arm starts clean
    // and either direction may be established.
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveForward(500), readyState(), limits));
    expectOutcome(ev::AuthzOutcome::AuthorizedStop,
                  authority.authorize(ev::MotorEffort(ev::MotorMode::Brake, 0U),
                                      readyState(/*atZeroSpeed=*/true), limits));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    authority.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));
    armHealthy(authority);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(500), readyState(), limits));
}

void test_repeated_disarm_and_reset_never_weakens_the_barrier(void) {
    ev::SafetyAuthority authority;
    armHealthy(authority);
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(500), readyState(), limits));
    for (int round = 0; round < 3; ++round) {
        authority.disarm();
        authority.reset();
        TEST_ASSERT_FALSE(authority.isArmed());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                              static_cast<int>(authority.lastAuthorizedDirection()));
    }
    armHealthy(authority);
    expectOutcome(ev::AuthzOutcome::DeniedUnsafeDirectionChange,
                  authority.authorize(driveForward(500), readyState(), limits));
}

void test_a_booted_authority_holds_no_direction(void) {
    // Boot is the distinct lifecycle boundary: volatile memory starts empty,
    // so there is no prior direction to preserve - and no barrier to
    // discharge. The first powered direction is established by driving it.
    // (A power cycle loses the barrier by construction; the operator
    // standstill requirement across power loss is documented, not sensed.)
    ev::SafetyAuthority authority;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));
    armHealthy(authority);
    expectOutcome(ev::AuthzOutcome::AuthorizedDrive,
                  authority.authorize(driveReverse(500), readyState(), limitsWithMaxEffort(1000)));
}

void test_reset_does_not_clear_a_fault_that_is_still_reported(void) {
    ev::SafetyAuthority authority;
    const ev::SafetyLimits limits = limitsWithMaxEffort(1000);

    ev::SafetySystemState faulty = readyState();
    faulty.detectedFault = ev::FaultCode::MissingSafetyLimits;
    authority.authorize(driveForward(500), faulty, limits);

    authority.reset();
    // The condition is still present, so it immediately latches again.
    const ev::AuthorizationResult result =
        authority.authorize(driveForward(500), faulty, limits);
    expectOutcome(ev::AuthzOutcome::ForcedFaultStop, result);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::MissingSafetyLimits),
                          static_cast<int>(authority.latchedFault().code));
}

// ---------------------------------------------------------------------------
// Invariant: no input sequence can cause an unauthorized powered direction flip
// ---------------------------------------------------------------------------

// One seeded property search over the input cross product. Every per-step
// invariant is asserted here, so a failure points at the step that broke it.
// The caller fixes the seed, so the search is deterministic in CI.
void runDirectionFlipPropertySearch(uint32_t seed) {
    const ev::SafetyLimits limits[] = {limitsWithMaxEffort(1000), limitsWithMaxEffort(350)};
    const ev::MotorMode modes[] = {
        ev::MotorMode::Disabled, ev::MotorMode::Coast,      ev::MotorMode::Brake,
        ev::MotorMode::DriveForward, ev::MotorMode::DriveReverse,
        static_cast<ev::MotorMode>(200),
    };
    const uint16_t magnitudes[] = {0, 1, 350, 1000, 1001};
    const ev::FaultCode faults[] = {
        ev::FaultCode::None, ev::FaultCode::InvalidRunConfig, ev::FaultCode::ProvisionalCalibration,
        ev::FaultCode::MissingSafetyLimits, ev::FaultCode::InvalidMotorEffort,
    };

    ev::SafetyAuthority authority;
    uint32_t rng = seed;

    for (int step = 0; step < 5000; ++step) {
        // Occasional operator actions, so arming/clearing still happens during
        // the search.
        switch (nextRandom(rng) % 12U) {
            case 0:
                authority.arm(fullyReady());
                break;
            case 1:
                authority.disarm();
                break;
            case 2:
                authority.clearFault();
                break;
            case 3:
                authority.reset();
                break;
            default:
                break;
        }

        const ev::MotorEffort request(
            modes[nextRandom(rng) % (sizeof(modes) / sizeof(modes[0]))],
            magnitudes[nextRandom(rng) % (sizeof(magnitudes) / sizeof(magnitudes[0]))]);

        ev::SafetySystemState state;
        state.readiness.configurationValid = (nextRandom(rng) % 2U) != 0U;
        state.readiness.calibrationUsable = (nextRandom(rng) % 2U) != 0U;
        state.readiness.limitsUsable = (nextRandom(rng) % 2U) != 0U;
        state.requiredDataFresh = (nextRandom(rng) % 2U) != 0U;
        state.atZeroSpeed = (nextRandom(rng) % 2U) != 0U;
        state.nowMs = nextRandom(rng);
        state.detectedFault = faults[nextRandom(rng) % (sizeof(faults) / sizeof(faults[0]))];

        const ev::SafetyLimits& activeLimits = limits[nextRandom(rng) % 2U];
        const ev::PoweredDirection memoryBefore = authority.lastAuthorizedDirection();

        const ev::AuthorizationResult result = authority.authorize(request, state, activeLimits);

        // A non-Drive command never carries effort, and a Drive command always
        // carries a bounded effort.
        if (result.command.actuationClass() != ev::ActuationClass::Drive) {
            TEST_ASSERT_EQUAL_INT(0, result.command.magnitudePermille());
        } else {
            TEST_ASSERT_TRUE(result.command.magnitudePermille() <=
                             activeLimits.maxEffortPermille.value);
        }

        if (result.command.isDriving()) {
            // The core invariant: a powered command may only continue or
            // establish a direction, never flip away from the remembered one.
            TEST_ASSERT_TRUE(memoryBefore == ev::PoweredDirection::None ||
                             memoryBefore == result.command.poweredDirection());
            TEST_ASSERT_TRUE(result.outcome == ev::AuthzOutcome::AuthorizedDrive ||
                             result.outcome == ev::AuthzOutcome::AuthorizedClamped);
            // A powered command implies every permission held.
            TEST_ASSERT_TRUE(authority.isArmed());
            TEST_ASSERT_TRUE(authority.latchedFault().code == ev::FaultCode::None);
            TEST_ASSERT_TRUE(state.readiness.allReady());
            TEST_ASSERT_TRUE(state.requiredDataFresh);
            TEST_ASSERT_TRUE(state.detectedFault == ev::FaultCode::None);
            TEST_ASSERT_TRUE(ev::isDrivingMode(request.mode));
            TEST_ASSERT_TRUE(ev::validateMotorEffort(request).ok);
            TEST_ASSERT_TRUE(authority.lastAuthorizedDirection() == result.command.poweredDirection());
        }

        if (result.outcome == ev::AuthzOutcome::DeniedUnsafeDirectionChange) {
            TEST_ASSERT_FALSE(result.command.isDriving());
            TEST_ASSERT_TRUE(memoryBefore != ev::PoweredDirection::None);
            TEST_ASSERT_TRUE(memoryBefore != ev::poweredDirectionOf(request.mode));
        }

        // The direction memory may only be cleared by a stop taken at reported
        // zero speed; every other denial leaves it untouched.
        const bool stopPathCleared =
            (result.outcome == ev::AuthzOutcome::AuthorizedStop ||
             result.outcome == ev::AuthzOutcome::ForcedFaultStop) &&
            state.atZeroSpeed;
        if (!result.command.isDriving() && !stopPathCleared) {
            TEST_ASSERT_TRUE(authority.lastAuthorizedDirection() == memoryBefore);
        }

        if (state.detectedFault != ev::FaultCode::None) {
            TEST_ASSERT_TRUE(authority.latchedFault().code != ev::FaultCode::None);
            TEST_ASSERT_FALSE(authority.isArmed());
        }
    }
}

void test_no_input_sequence_produces_an_unauthorized_direction_flip(void) {
    // Fixed seeds for CI determinism; the invariant must hold for every one.
    const uint32_t seeds[] = {0x5EED1234U, 0xDEADBEEFU, 1U};
    for (uint32_t seed : seeds) {
        runDirectionFlipPropertySearch(seed);
    }
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_arm_requires_complete_readiness);
    RUN_TEST(test_arm_is_refused_while_a_fault_is_latched);
    RUN_TEST(test_disarm_immediately_denies_drive);

    RUN_TEST(test_detected_fault_latches_disarms_and_forces_a_stop);
    RUN_TEST(test_fault_stop_cannot_be_overridden_by_later_drive_requests);
    RUN_TEST(test_clear_fault_works_only_when_a_fault_is_latched);
    RUN_TEST(test_clear_fault_while_armed_is_refused);
    RUN_TEST(test_clear_fault_ends_the_arming_period);
    RUN_TEST(test_zero_timestamp_does_not_suppress_fault_latching);
    RUN_TEST(test_fault_response_is_independent_of_fault_identity);
    RUN_TEST(test_stop_command_for_policy_can_never_produce_a_drive);
    RUN_TEST(test_each_fault_code_can_be_latched_cleared_and_rearmed);

    RUN_TEST(test_mode_actuation_classes_match_their_documented_semantics);
    RUN_TEST(test_authorized_command_exposes_mode_effort_class_and_direction);
    RUN_TEST(test_non_powered_commands_are_always_available_and_unguided_by_arming);

    RUN_TEST(test_drive_requires_arming_readiness_and_fresh_data);
    RUN_TEST(test_illegal_effort_never_produces_a_powered_command);
    RUN_TEST(test_full_scale_effort_is_not_an_illegal_request);

    RUN_TEST(test_requested_effort_above_the_safety_ceiling_is_clamped);
    RUN_TEST(test_requested_effort_at_or_below_the_ceiling_is_granted_as_asked);

    RUN_TEST(test_repeated_identical_commands_are_stable);

    RUN_TEST(test_direct_powered_direction_flip_is_refused_even_at_claimed_zero_speed);
    RUN_TEST(test_same_direction_repeated_drive_is_not_a_direction_change);
    RUN_TEST(test_direction_change_is_legal_through_a_zero_speed_stop);
    RUN_TEST(test_stop_without_zero_speed_does_not_clear_the_direction_memory);
    RUN_TEST(test_disarming_does_not_forget_the_last_powered_direction);

    RUN_TEST(test_reset_preserves_the_direction_barrier_and_requires_a_fresh_arm);
    RUN_TEST(test_reset_after_reverse_bars_forward_until_a_verified_stop);
    RUN_TEST(test_reset_immediately_after_a_powered_command_keeps_the_barrier);
    RUN_TEST(test_a_stop_without_reported_zero_speed_never_discharges_after_reset);
    RUN_TEST(test_a_verified_stop_before_reset_leaves_no_barrier_behind);
    RUN_TEST(test_repeated_disarm_and_reset_never_weakens_the_barrier);
    RUN_TEST(test_a_booted_authority_holds_no_direction);
    RUN_TEST(test_reset_does_not_clear_a_fault_that_is_still_reported);

    RUN_TEST(test_no_input_sequence_produces_an_unauthorized_direction_flip);

    return UNITY_END();
}
