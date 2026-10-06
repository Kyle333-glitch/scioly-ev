#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <Motion.h>
#include <SafetyLimits.h>
#include <Snapshots.h>
#include <Units.h>
#include <VehicleCalibration.h>
#include <VehicleState.h>

// Contract tests for the pure-core data types. The numbers used here are test
// fixtures only; they are deliberately not calibration or limit values for any
// real vehicle.

// Pinned at compile time: provenance is not optional. If any of these types ever
// gains a default constructor, the contract in SOFTWARE_ARCHITECTURE.md section 7
// is broken and this test target stops compiling.
static_assert(!std::is_default_constructible<ev::Sourced<double>>::value,
              "no silent defaults");
static_assert(!std::is_default_constructible<ev::VehicleCalibration>::value,
              "no silent defaults");
static_assert(!std::is_default_constructible<ev::SafetyLimits>::value, "no silent defaults");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Fixture values, not vehicle data.
ev::VehicleCalibration calibrationWith(ev::ValueSource source) {
    return ev::VehicleCalibration{
        ev::Sourced<double>(0.0004, source),
        ev::Sourced<double>(0.07, source),
        ev::Sourced<double>(0.09, source),
    };
}

ev::SafetyLimits limitsWith(ev::ValueSource source) {
    return ev::SafetyLimits{
        ev::Sourced<uint16_t>(900, source),
        ev::Sourced<double>(2.0, source),
        ev::Sourced<double>(1.0, source),
        ev::Sourced<double>(30.0, source),
        ev::Sourced<double>(9.0, source),
    };
}

void expectEffortInvalid(ev::MotorEffortError expected, const ev::MotorEffort& effort) {
    const ev::MotorEffortValidation result = ev::validateMotorEffort(effort);
    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_TRUE(result.error != ev::MotorEffortError::None);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.error));
}

void expectMotionRequestInvalid(ev::MotionRequestError expected,
                                const ev::MotionRequest& request) {
    const ev::MotionRequestValidation result = ev::validateMotionRequest(request);
    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_TRUE(result.error != ev::MotionRequestError::None);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.error));
}

}  // namespace

// --- Provenance -------------------------------------------------------------

void test_only_measured_values_count_as_measured(void) {
    TEST_ASSERT_FALSE(ev::Sourced<double>(0.5, ev::ValueSource::Provisional).isMeasured());
    TEST_ASSERT_FALSE(ev::Sourced<double>(0.5, ev::ValueSource::RuleDerived).isMeasured());
    TEST_ASSERT_TRUE(ev::Sourced<double>(0.5, ev::ValueSource::Measured).isMeasured());
}

void test_calibration_is_fully_measured_only_when_every_value_is_measured(void) {
    TEST_ASSERT_TRUE(calibrationWith(ev::ValueSource::Measured).isFullyMeasured());
    TEST_ASSERT_FALSE(calibrationWith(ev::ValueSource::Provisional).isFullyMeasured());

    // One remaining provisional value is enough to hold the whole thing back.
    ev::VehicleCalibration mixed = calibrationWith(ev::ValueSource::Measured);
    mixed.reverseBrakeOffsetM = ev::Sourced<double>(0.09, ev::ValueSource::Provisional);
    TEST_ASSERT_FALSE(mixed.isFullyMeasured());
}

void test_safety_limits_are_fully_established_only_when_every_limit_is_measured(void) {
    TEST_ASSERT_TRUE(limitsWith(ev::ValueSource::Measured).isFullyEstablished());
    TEST_ASSERT_FALSE(limitsWith(ev::ValueSource::Provisional).isFullyEstablished());
    TEST_ASSERT_FALSE(limitsWith(ev::ValueSource::RuleDerived).isFullyEstablished());
}

// The battery-voltage requirement is a capability-aware condition, exactly as the
// driver-fault polarity requirement is (`DriverFault.h`, D32). On a vehicle with
// no battery-sense circuit the voltage requirement is *not applicable* rather than
// outstanding, so production arming is not permanently impossible for a quantity
// the vehicle cannot sense; on a vehicle that has the circuit, nothing changed.
void test_battery_voltage_is_required_only_when_the_circuit_exists(void) {
    const ev::BatteryVoltageCapability installed = ev::BatteryVoltageCapability::Installed;
    const ev::BatteryVoltageCapability notInstalled = ev::BatteryVoltageCapability::NotInstalled;

    // Every limit measured, with the circuit present: validates as before.
    const ev::SafetyLimits allMeasured = limitsWith(ev::ValueSource::Measured);
    TEST_ASSERT_TRUE(allMeasured.isFullyEstablished(installed));
    TEST_ASSERT_TRUE(ev::validateSafetyLimits(allMeasured, installed).ok);

    // The placeholder battery voltage the production config ships. With the
    // circuit installed it is refused; with no circuit it is not applicable.
    ev::SafetyLimits placeholderBattery = limitsWith(ev::ValueSource::Measured);
    placeholderBattery.minBatteryV = ev::Sourced<double>(9.0, ev::ValueSource::Provisional);

    TEST_ASSERT_FALSE(placeholderBattery.isFullyEstablished(installed));
    const ev::SafetyLimitsValidation refused =
        ev::validateSafetyLimits(placeholderBattery, installed);
    TEST_ASSERT_FALSE(refused.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::SafetyLimitsError::NotFullyMeasured),
                          static_cast<int>(refused.error));

    TEST_ASSERT_TRUE(placeholderBattery.isFullyEstablished(notInstalled));
    const ev::SafetyLimitsValidation accepted =
        ev::validateSafetyLimits(placeholderBattery, notInstalled);
    TEST_ASSERT_TRUE(accepted.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::SafetyLimitsError::None),
                          static_cast<int>(accepted.error));

    // The default is the conservative one: a caller that states no capability
    // gets the strict, Installed requirement, so the gate is never dropped by
    // omission.
    TEST_ASSERT_FALSE(placeholderBattery.isFullyEstablished());
    TEST_ASSERT_FALSE(ev::validateSafetyLimits(placeholderBattery).ok);

    // Every other ceiling is still required on a NotInstalled build: relaxing the
    // battery requirement must not relax the maximum run duration (or anything
    // else).
    ev::SafetyLimits placeholderDuration = limitsWith(ev::ValueSource::Measured);
    placeholderDuration.maxRunDurationS =
        ev::Sourced<double>(30.0, ev::ValueSource::Provisional);
    const ev::SafetyLimitsValidation durationStillRequired =
        ev::validateSafetyLimits(placeholderDuration, notInstalled);
    TEST_ASSERT_FALSE(durationStillRequired.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::SafetyLimitsError::NotFullyMeasured),
                          static_cast<int>(durationStillRequired.error));

    // A value the vehicle cannot sense is still a configured value: a
    // non-positive or non-finite battery ceiling is malformed whatever the
    // capability, so the numeric checks stay unconditional.
    ev::SafetyLimits malformed = limitsWith(ev::ValueSource::Measured);
    malformed.minBatteryV = ev::Sourced<double>(0.0, ev::ValueSource::Measured);
    const ev::SafetyLimitsValidation nonPositive =
        ev::validateSafetyLimits(malformed, notInstalled);
    TEST_ASSERT_FALSE(nonPositive.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::SafetyLimitsError::NonPositiveLimit),
                          static_cast<int>(nonPositive.error));
}

void test_the_battery_voltage_capability_has_a_stable_spelling(void) {
    // `Installed` is the conservative zero value, so a forgotten assignment
    // keeps the voltage gate required.
    const ev::BatteryVoltageCapability defaulted{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BatteryVoltageCapability::Installed),
                          static_cast<int>(defaulted));
    TEST_ASSERT_EQUAL_STRING(
        "Installed", ev::batteryVoltageCapabilityName(ev::BatteryVoltageCapability::Installed));
    TEST_ASSERT_EQUAL_STRING("NotInstalled",
                             ev::batteryVoltageCapabilityName(
                                 ev::BatteryVoltageCapability::NotInstalled));
}

// --- Motor command boundary -------------------------------------------------

void test_authorized_motor_command_cannot_be_default_constructed(void) {
    // Typedefs keep the template commas out of the Unity macro arguments.
    typedef std::is_default_constructible<ev::AuthorizedMotorCommand> DefaultConstructible;
    TEST_ASSERT_FALSE(DefaultConstructible::value);
}

void test_authorized_motor_command_cannot_be_built_from_a_motor_effort(void) {
    // An effort is a request, not permission. Only the arming authority (a
    // later stage) can turn one into an authorized command.
    typedef std::is_constructible<ev::AuthorizedMotorCommand, ev::MotorEffort> EffortConstructible;
    TEST_ASSERT_FALSE(EffortConstructible::value);
}

void test_disabled_motor_mode_is_the_safe_zero_value(void) {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(ev::MotorMode::Disabled));
    TEST_ASSERT_FALSE(ev::isDrivingMode(ev::MotorMode::Disabled));
    TEST_ASSERT_FALSE(ev::isDrivingMode(ev::MotorMode::Coast));
    TEST_ASSERT_FALSE(ev::isDrivingMode(ev::MotorMode::Brake));
    TEST_ASSERT_TRUE(ev::isDrivingMode(ev::MotorMode::DriveForward));
    TEST_ASSERT_TRUE(ev::isDrivingMode(ev::MotorMode::DriveReverse));
}

void test_motor_effort_magnitude_is_checked_against_full_scale(void) {
    const ev::MotorEffort atFullScale(ev::MotorMode::DriveForward, 1000);
    const ev::MotorEffortValidation fullScaleResult = ev::validateMotorEffort(atFullScale);
    TEST_ASSERT_TRUE(fullScaleResult.ok);
    TEST_ASSERT_TRUE(fullScaleResult.error == ev::MotorEffortError::None);

    const ev::MotorEffort atZero(ev::MotorMode::DriveReverse, 0);
    const ev::MotorEffortValidation zeroResult = ev::validateMotorEffort(atZero);
    TEST_ASSERT_TRUE(zeroResult.ok);
    TEST_ASSERT_TRUE(zeroResult.error == ev::MotorEffortError::None);

    expectEffortInvalid(ev::MotorEffortError::MagnitudeOutOfRange,
                        ev::MotorEffort(ev::MotorMode::DriveForward, 1001));
    expectEffortInvalid(ev::MotorEffortError::MagnitudeOutOfRange,
                        ev::MotorEffort(ev::MotorMode::DriveReverse, 65535));
}

void test_motor_effort_rejects_magnitude_on_non_driving_modes(void) {
    TEST_ASSERT_TRUE(ev::validateMotorEffort(ev::MotorEffort(ev::MotorMode::Coast, 0)).ok);
    TEST_ASSERT_TRUE(ev::validateMotorEffort(ev::MotorEffort(ev::MotorMode::Brake, 0)).ok);
    TEST_ASSERT_TRUE(ev::validateMotorEffort(ev::MotorEffort(ev::MotorMode::Disabled, 0)).ok);

    expectEffortInvalid(ev::MotorEffortError::MagnitudeOnNonDrivingMode,
                        ev::MotorEffort(ev::MotorMode::Coast, 1));
    expectEffortInvalid(ev::MotorEffortError::MagnitudeOnNonDrivingMode,
                        ev::MotorEffort(ev::MotorMode::Brake, 500));
    expectEffortInvalid(ev::MotorEffortError::MagnitudeOnNonDrivingMode,
                        ev::MotorEffort(ev::MotorMode::Disabled, 500));
}

void test_motor_effort_rejects_an_undefined_mode(void) {
    expectEffortInvalid(ev::MotorEffortError::UnknownMode,
                        ev::MotorEffort(static_cast<ev::MotorMode>(99), 0));
}

// --- Motion requests --------------------------------------------------------

void test_motion_request_accepts_positive_distance_and_time(void) {
    const ev::MotionRequestValidation forward =
        ev::validateMotionRequest(ev::MotionRequest(ev::MotionDirection::Forward, 7.0, 14.0));
    TEST_ASSERT_TRUE(forward.ok);
    TEST_ASSERT_TRUE(forward.error == ev::MotionRequestError::None);

    const ev::MotionRequestValidation reverse =
        ev::validateMotionRequest(ev::MotionRequest(ev::MotionDirection::Reverse, 0.5, 2.0));
    TEST_ASSERT_TRUE(reverse.ok);
    TEST_ASSERT_TRUE(reverse.error == ev::MotionRequestError::None);
}

void test_motion_request_rejects_non_positive_distance_or_time(void) {
    expectMotionRequestInvalid(ev::MotionRequestError::TargetDistanceNotPositive,
                               ev::MotionRequest(ev::MotionDirection::Forward, 0.0, 5.0));
    expectMotionRequestInvalid(ev::MotionRequestError::TargetDistanceNotPositive,
                               ev::MotionRequest(ev::MotionDirection::Forward, -1.0, 5.0));
    expectMotionRequestInvalid(ev::MotionRequestError::TimeBudgetNotPositive,
                               ev::MotionRequest(ev::MotionDirection::Forward, 1.0, 0.0));
    expectMotionRequestInvalid(ev::MotionRequestError::TimeBudgetNotPositive,
                               ev::MotionRequest(ev::MotionDirection::Forward, 1.0, -2.0));
}

void test_motion_request_rejects_non_finite_values(void) {
    expectMotionRequestInvalid(ev::MotionRequestError::NonFiniteValue,
                               ev::MotionRequest(ev::MotionDirection::Forward, kNaN, 5.0));
    expectMotionRequestInvalid(ev::MotionRequestError::NonFiniteValue,
                               ev::MotionRequest(ev::MotionDirection::Forward, kInf, 5.0));
    expectMotionRequestInvalid(ev::MotionRequestError::NonFiniteValue,
                               ev::MotionRequest(ev::MotionDirection::Forward, 1.0, kNaN));
    expectMotionRequestInvalid(ev::MotionRequestError::NonFiniteValue,
                               ev::MotionRequest(ev::MotionDirection::Forward, 1.0, -kInf));
}

// --- Snapshots and state ----------------------------------------------------

void test_sensor_snapshot_reports_whether_it_has_a_battery_reading(void) {
    const ev::SensorSnapshot noReading(1000, -25, 0, false);
    TEST_ASSERT_FALSE(noReading.batteryReadingValid);

    const ev::SensorSnapshot withReading(1000, -25, 11200, true);
    TEST_ASSERT_TRUE(withReading.batteryReadingValid);
    TEST_ASSERT_EQUAL_INT(11200, withReading.batteryMv);
}

void test_odometry_snapshot_carries_its_own_validity(void) {
    const ev::OdometrySnapshot invalid(1000, 0.0, 0.0, false);
    TEST_ASSERT_FALSE(invalid.valid);

    const ev::OdometrySnapshot valid(1000, 6.25, 0.75, true);
    TEST_ASSERT_TRUE(valid.valid);
}

// The run's phase vocabulary is a real state machine now, and its safe zero
// value is pinned in test_competition_state next to that machine; this target
// no longer keeps a second, competing phase list.

void test_fault_state_defaults_to_no_fault(void) {
    const ev::FaultState fault;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(ev::FaultCode::None));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None), static_cast<int>(fault.code));
    TEST_ASSERT_FALSE(fault.latched);
    TEST_ASSERT_EQUAL_UINT32(0, fault.raisedAtMs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_only_measured_values_count_as_measured);
    RUN_TEST(test_calibration_is_fully_measured_only_when_every_value_is_measured);
    RUN_TEST(test_safety_limits_are_fully_established_only_when_every_limit_is_measured);
    RUN_TEST(test_battery_voltage_is_required_only_when_the_circuit_exists);
    RUN_TEST(test_the_battery_voltage_capability_has_a_stable_spelling);
    RUN_TEST(test_authorized_motor_command_cannot_be_default_constructed);
    RUN_TEST(test_authorized_motor_command_cannot_be_built_from_a_motor_effort);
    RUN_TEST(test_disabled_motor_mode_is_the_safe_zero_value);
    RUN_TEST(test_motor_effort_magnitude_is_checked_against_full_scale);
    RUN_TEST(test_motor_effort_rejects_magnitude_on_non_driving_modes);
    RUN_TEST(test_motor_effort_rejects_an_undefined_mode);
    RUN_TEST(test_motion_request_accepts_positive_distance_and_time);
    RUN_TEST(test_motion_request_rejects_non_positive_distance_or_time);
    RUN_TEST(test_motion_request_rejects_non_finite_values);
    RUN_TEST(test_sensor_snapshot_reports_whether_it_has_a_battery_reading);
    RUN_TEST(test_odometry_snapshot_carries_its_own_validity);
    RUN_TEST(test_fault_state_defaults_to_no_fault);
    return UNITY_END();
}
