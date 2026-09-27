#include <cstdint>

#include <unity.h>

#include <Motion.h>
#include <MotorDriverPlan.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>

// Contract tests for the planned DRV8874 PH/EN driver mapping
// (lib/evcore/src/MotorDriverPlan.h).
//
// These tests pin the PLAN and its safety direction: which input levels each
// authorized command asks for, that a non-powered command never enables the
// driver with a drive level, that a refused request cannot produce driver levels
// that are enabled, and that the duty is mapped from the authorized magnitude.
//
// They are not evidence about the driver. What any of these level combinations
// does on the vehicle - brake, coast, direction, timing, current - is UNVERIFIED
// (`VERIFIED_STATE.md` #1, #2, #3) and can only be settled on the bench with the
// drivetrain unloaded. Nothing here is a measurement.

void setUp(void) {}
void tearDown(void) {}

namespace {

// Fixture limits, not vehicle data: five measured ceilings so the authority can
// authorize a drive.
ev::SafetyLimits fixtureLimits(uint16_t maxEffortPermille) {
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
ev::SafetySystemState readyState(uint32_t nowMs = 1000U) {
    ev::SafetySystemState state;
    state.readiness = fullyReady();
    state.requiredDataFresh = true;
    state.atZeroSpeed = true;
    state.nowMs = nowMs;
    state.detectedFault = ev::FaultCode::None;
    return state;
}

// The authority is the only source of an AuthorizedMotorCommand, so the plan is
// exercised the way the firmware exercises it: through an authorization.
struct AuthorizedFixture {
    explicit AuthorizedFixture(uint16_t maxEffortPermille)
        : limits(fixtureLimits(maxEffortPermille)) {
        authority.arm(fullyReady());
    }

    ev::SafetyLimits limits;
    ev::SafetyAuthority authority;
};

void expectLevels(const ev::Drv8874Inputs& inputs, bool expectedNSleepHigh,
                  bool expectedPhaseHigh, uint16_t expectedDutyPermille) {
    TEST_ASSERT_EQUAL_INT(expectedNSleepHigh, inputs.nSleepHigh);
    TEST_ASSERT_EQUAL_INT(expectedPhaseHigh, inputs.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(expectedDutyPermille, inputs.dutyPermille);
}

}  // namespace

// Every non-powered command keeps the drive level at zero, and the two commands
// that mean "no powered output" ask for the de-energized state.
void test_non_powered_commands_never_apply_a_drive_level(void) {
    AuthorizedFixture fixture(600U);

    const ev::AuthorizationResult disabled = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::Disabled, 0U), readyState(), fixture.limits);
    const ev::Drv8874Inputs disabledInputs = ev::drv8874InputsFor(disabled.command);
    expectLevels(disabledInputs, false, false, 0U);
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(disabledInputs));

    const ev::AuthorizationResult coast = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::Coast, 0U), readyState(), fixture.limits);
    const ev::Drv8874Inputs coastInputs = ev::drv8874InputsFor(coast.command);
    expectLevels(coastInputs, false, false, 0U);
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(coastInputs));

    // Brake keeps the driver enabled with no drive level applied. Whether that
    // electrically brakes or coasts is not claimed here and is measured on the
    // bench.
    const ev::AuthorizationResult brake = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::Brake, 0U), readyState(), fixture.limits);
    const ev::Drv8874Inputs brakeInputs = ev::drv8874InputsFor(brake.command);
    expectLevels(brakeInputs, true, false, 0U);
}

// A drive asks for the driver enabled, with the duty taken from the authorized
// magnitude, and PH carrying the direction.
void test_drive_maps_direction_and_duty(void) {
    AuthorizedFixture fixture(600U);

    const ev::AuthorizationResult forward = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 450U), readyState(), fixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(forward.outcome));
    expectLevels(ev::drv8874InputsFor(forward.command), true, true, 450U);

    // A direction change is refused while another drive is latched, so the
    // reverse command is exercised from a fresh authority.
    AuthorizedFixture reverseFixture(600U);
    const ev::AuthorizationResult reverse = reverseFixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveReverse, 450U), readyState(), reverseFixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(reverse.outcome));
    expectLevels(ev::drv8874InputsFor(reverse.command), true, false, 450U);
}

// The plan maps the AUTHORIZED magnitude, not the requested one: a clamped drive
// must not ask the driver for more than the authority granted.
void test_duty_follows_the_authorized_magnitude_not_the_request(void) {
    AuthorizedFixture fixture(250U);

    const ev::AuthorizationResult clamped = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 1000U), readyState(), fixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedClamped),
                          static_cast<int>(clamped.outcome));
    expectLevels(ev::drv8874InputsFor(clamped.command), true, true, 250U);
}

// A zero-effort drive is a legal authorization, and the plan applies no drive
// level for it. It is deliberately not a stop: only Disabled/Coast/Brake are.
void test_zero_effort_drive_applies_no_drive_level(void) {
    AuthorizedFixture fixture(600U);

    const ev::AuthorizationResult drive = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 0U), readyState(), fixture.limits);
    TEST_ASSERT_TRUE(drive.command.isDriving());
    expectLevels(ev::drv8874InputsFor(drive.command), true, true, 0U);
}

// Every refusal in the authority's vocabulary still leaves the driver
// de-energized: a request that could not be authorized can never produce driver
// levels that are enabled.
void test_refusals_leave_the_driver_de_energized(void) {
    // Not armed: the drive is denied.
    ev::SafetyAuthority unarmed;
    const ev::SafetyLimits limits = fixtureLimits(600U);
    const ev::AuthorizationResult denied = unarmed.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 600U), readyState(), limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedNotArmed),
                          static_cast<int>(denied.outcome));
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(ev::drv8874InputsFor(denied.command)));

    // A stale or incomplete state: the drive is denied for the same reason the
    // authority refuses it, and the driver levels follow the refusal.
    AuthorizedFixture fixture(600U);
    ev::SafetySystemState stale = readyState();
    stale.requiredDataFresh = false;
    const ev::AuthorizationResult staleDenied = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 600U), stale, fixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedStaleData),
                          static_cast<int>(staleDenied.outcome));
    TEST_ASSERT_TRUE(
        ev::drv8874InputsAreDeEnergized(ev::drv8874InputsFor(staleDenied.command)));

    // An illegal request: no powered output, and no drive level.
    const ev::AuthorizationResult illegal = fixture.authority.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 1500U), readyState(), fixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedIllegalEffort),
                          static_cast<int>(illegal.outcome));
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(ev::drv8874InputsFor(illegal.command)));

    // A latched fault forces the stop policy's own command.
    ev::SafetyAuthority faulted;
    faulted.arm(fullyReady());
    ev::SafetySystemState withFault = readyState();
    withFault.detectedFault = ev::FaultCode::PhaseTimeout;
    const ev::AuthorizationResult faultStop = faulted.authorize(
        ev::MotorEffort(ev::MotorMode::DriveForward, 600U), withFault, fixture.limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faultStop.outcome));
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(ev::drv8874InputsFor(faultStop.command)));
}

// The permille-to-PWM mapping is the full-scale mapping with rounding, clamped so
// a corrupt value cannot wrap.
void test_pwm_count_mapping_is_bounded_and_monotonic(void) {
    const uint8_t bits = ev::kPlannedPwmResolutionBits;

    TEST_ASSERT_EQUAL_UINT16(0U, ev::pwmCountFromPermille(0U, bits));
    TEST_ASSERT_EQUAL_UINT16(1U, ev::pwmCountFromPermille(2U, bits));
    TEST_ASSERT_EQUAL_UINT16(51U, ev::pwmCountFromPermille(200U, bits));
    TEST_ASSERT_EQUAL_UINT16(128U, ev::pwmCountFromPermille(500U, bits));
    TEST_ASSERT_EQUAL_UINT16(255U, ev::pwmCountFromPermille(1000U, bits));

    // Above full scale is clamped, never wrapped into a low duty.
    TEST_ASSERT_EQUAL_UINT16(255U, ev::pwmCountFromPermille(2000U, bits));

    // A different resolution is arithmetic on the same scale.
    TEST_ASSERT_EQUAL_UINT16(1023U, ev::pwmCountFromPermille(1000U, 10U));

    // Monotonic and bounded over the whole permille range.
    uint16_t previous = 0U;
    for (uint16_t permille = 1U; permille <= 1000U; ++permille) {
        const uint16_t count = ev::pwmCountFromPermille(permille, bits);
        TEST_ASSERT_TRUE(count >= previous);
        TEST_ASSERT_TRUE(count <= 255U);
        previous = count;
    }
}

// The de-energized predicate is the property the fail-safe path relies on, so it
// is pinned against the levels it must call de-energized: anything that enables
// the driver, or that applies a drive level, is not.
void test_de_energized_predicate_is_conservative(void) {
    TEST_ASSERT_TRUE(ev::drv8874InputsAreDeEnergized(ev::Drv8874Inputs{false, false, 0U}));
    TEST_ASSERT_FALSE(ev::drv8874InputsAreDeEnergized(ev::Drv8874Inputs{true, false, 0U}));
    TEST_ASSERT_FALSE(ev::drv8874InputsAreDeEnergized(ev::Drv8874Inputs{false, false, 1U}));
    TEST_ASSERT_FALSE(ev::drv8874InputsAreDeEnergized(ev::Drv8874Inputs{true, true, 600U}));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_non_powered_commands_never_apply_a_drive_level);
    RUN_TEST(test_drive_maps_direction_and_duty);
    RUN_TEST(test_duty_follows_the_authorized_magnitude_not_the_request);
    RUN_TEST(test_zero_effort_drive_applies_no_drive_level);
    RUN_TEST(test_refusals_leave_the_driver_de_energized);
    RUN_TEST(test_pwm_count_mapping_is_bounded_and_monotonic);
    RUN_TEST(test_de_energized_predicate_is_conservative);
    return UNITY_END();
}
