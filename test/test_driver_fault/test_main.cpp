#include <cstdint>
#include <cstring>

#include <unity.h>

#include <ConfigurationChecks.h>
#include <DriverFault.h>
#include <Units.h>

// Contract tests for the external DRV8874 nFAULT signal's capability model
// (`lib/evcore/src/DriverFault.h`).
//
// Why this target exists: the assembled vehicle does **not** wire the driver's
// nFAULT output to the MCU, and the firmware has to say that without pretending
// the signal is healthy and without demanding a polarity measurement for a line
// that does not exist. These tests pin the three cases that matter - installed
// and verified, installed and unverified, and not installed - plus the runtime
// rule that an absent signal can never produce an observed fault.
//
// What a pass here does NOT mean: that any vehicle wiring was measured. These are
// logic tests over the capability type. Whether the DRV8874's fault output is
// wired on a given vehicle is a hardware fact, and on this one it is recorded in
// `src/PinMap.h` and `docs/limitations.md`, not established here.

void setUp(void) {}
void tearDown(void) {}

namespace {

// A polarity that a measurement established, as H1B would record it. The
// citation is what `Sourced::citesEvidence()` asks for on anything that is not a
// placeholder, and it is deliberately present so these tests exercise the same
// shape the real promotion will have.
ev::Sourced<ev::DriverFaultPolarity> measuredPolarity(ev::DriverFaultPolarity polarity) {
    return ev::Sourced<ev::DriverFaultPolarity>(
        polarity, ev::ValueSource::Measured, "test fixture: measured polarity");
}

// The polarity this build actually ships: provisional and unverified, because
// nobody has measured the line and the capability says there is no line to
// measure. Deliberately not relabelled in any test below.
ev::Sourced<ev::DriverFaultPolarity> unverifiedPolarity() {
    return ev::Sourced<ev::DriverFaultPolarity>(ev::DriverFaultPolarity::Unverified,
                                                ev::ValueSource::Provisional);
}

}  // namespace

// 1. Installed + verified polarity passes the driver-fault configuration
// requirement. This is the case the check has always been for.
void test_installed_with_verified_polarity_passes(void) {
    const ev::DriverFaultConfigValidation validation =
        ev::validateDriverFaultConfig(ev::DriverFaultCapability::Installed,
                                      measuredPolarity(ev::DriverFaultPolarity::ActiveLow));
    TEST_ASSERT_TRUE(validation.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultConfigError::None),
                          static_cast<int>(validation.error));

    TEST_ASSERT_TRUE(ev::driverFaultPolarityVerified(
        measuredPolarity(ev::DriverFaultPolarity::ActiveHigh)));
}

// 2. Installed + unverified polarity fails. A vehicle that has the line and has
// not measured it may not be armed: a driver fault nobody interprets is a driver
// fault nobody stops for.
void test_installed_with_unverified_polarity_fails(void) {
    const ev::DriverFaultConfigValidation validation =
        ev::validateDriverFaultConfig(ev::DriverFaultCapability::Installed,
                                      unverifiedPolarity());
    TEST_ASSERT_FALSE(validation.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultConfigError::PolarityNotVerified),
                          static_cast<int>(validation.error));

    // "Measured that it is unmeasured" is still refused: a promotion to Measured
    // may not stand in for a measurement that never happened.
    const ev::DriverFaultConfigValidation contradictory = ev::validateDriverFaultConfig(
        ev::DriverFaultCapability::Installed,
        ev::Sourced<ev::DriverFaultPolarity>(ev::DriverFaultPolarity::Unverified,
                                             ev::ValueSource::Measured));
    TEST_ASSERT_FALSE(contradictory.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultConfigError::PolarityNotVerified),
                          static_cast<int>(contradictory.error));
}

// 3. NotInstalled + unverified polarity passes the driver-fault-specific
// requirement, and the polarity value is not touched: its verification is
// *inapplicable*, not satisfied. The shipped constant stays `Unverified`.
void test_not_installed_makes_polarity_verification_inapplicable(void) {
    const ev::Sourced<ev::DriverFaultPolarity> shipped = unverifiedPolarity();
    const ev::DriverFaultConfigValidation validation =
        ev::validateDriverFaultConfig(ev::DriverFaultCapability::NotInstalled, shipped);
    TEST_ASSERT_TRUE(validation.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultConfigError::None),
                          static_cast<int>(validation.error));

    // No fake verification happened: the value is still Unverified and still
    // provisional, and the low-level predicate still reports it as unverified.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultPolarity::Unverified),
                          static_cast<int>(shipped.value));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ValueSource::Provisional),
                          static_cast<int>(shipped.source));
    TEST_ASSERT_FALSE(ev::driverFaultPolarityVerified(shipped));
}

// The capability default is the conservative one: a forgotten assignment leaves
// the line required, so arming is refused rather than the external fault gate
// silently disappearing.
void test_installed_is_the_default_capability(void) {
    const ev::DriverFaultCapability defaulted{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::DriverFaultCapability::Installed),
                          static_cast<int>(defaulted));
    TEST_ASSERT_FALSE(
        ev::validateDriverFaultConfig(defaulted, unverifiedPolarity()).ok);
}

// The capability vocabulary is wire-safe: these names reach the telemetry `nfault`
// field and the bench readout.
void test_capability_names_are_wire_safe(void) {
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::DriverFaultCapability::NotInstalled); ++index) {
        const char* name =
            ev::driverFaultCapabilityName(static_cast<ev::DriverFaultCapability>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_TRUE(strlen(name) <= 30U);
        for (const char* cursor = name; *cursor != '\0'; ++cursor) {
            TEST_ASSERT_TRUE(*cursor != ',' && *cursor != '=' && *cursor != ' ');
        }
    }
    TEST_ASSERT_EQUAL_STRING("Installed",
                             ev::driverFaultCapabilityName(ev::DriverFaultCapability::Installed));
    TEST_ASSERT_EQUAL_STRING(
        "NotInstalled",
        ev::driverFaultCapabilityName(ev::DriverFaultCapability::NotInstalled));
}

// 4 and 5. NotInstalled cannot produce an observed driver fault, from either
// level, whatever the polarity value says. The absent signal is not interpreted
// at all - including when the polarity happens to be verified, which is the case
// a capability check that forgot to consult the capability would let through.
void test_not_installed_never_observes_a_driver_fault(void) {
    const ev::Sourced<ev::DriverFaultPolarity> verifiedLow =
        measuredPolarity(ev::DriverFaultPolarity::ActiveLow);
    const ev::Sourced<ev::DriverFaultPolarity> verifiedHigh =
        measuredPolarity(ev::DriverFaultPolarity::ActiveHigh);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::NotInstalled, true,
                                                 verifiedLow)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::NotInstalled, false,
                                                 verifiedLow)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::NotInstalled, true,
                                                 verifiedHigh)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::NotInstalled, false,
                                                 unverifiedPolarity())));

    // The level predicate itself is unchanged, so a wired vehicle's behavior is
    // untouched: it is the capability argument that refuses, not a rewritten
    // polarity table.
    TEST_ASSERT_TRUE(ev::driverFaultLevelIndicatesFault(true, ev::DriverFaultPolarity::ActiveLow));
    TEST_ASSERT_FALSE(ev::driverFaultLevelIndicatesFault(false, ev::DriverFaultPolarity::ActiveLow));
    TEST_ASSERT_TRUE(
        ev::driverFaultLevelIndicatesFault(false, ev::DriverFaultPolarity::ActiveHigh));
    TEST_ASSERT_FALSE(ev::driverFaultLevelIndicatesFault(true, ev::DriverFaultPolarity::Unverified));
}

// The installed cases still behave exactly as they did, so the capability model
// is not a weakening of the wired-vehicle path.
void test_installed_mode_runtime_behavior_is_unchanged(void) {
    const ev::Sourced<ev::DriverFaultPolarity> activeLow =
        measuredPolarity(ev::DriverFaultPolarity::ActiveLow);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::DriverFaultBar),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::Installed, true,
                                                 activeLow)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::Installed, false,
                                                 activeLow)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::None),
        static_cast<int>(ev::observedDriverFault(ev::DriverFaultCapability::Installed, true,
                                                 unverifiedPolarity())));
}

// 8. The capability-aware check is the one the configuration report carries, and
// it does not weaken any other check: the id is named, it has its own field, and
// a report with everything else failing still fails on the most fundamental
// reason first.
void test_configuration_check_id_is_capability_aware_and_does_not_weaken_others(void) {
    TEST_ASSERT_EQUAL_STRING("DriverFaultConfigurationValid",
                             ev::configurationCheckName(
                                 ev::ConfigurationCheckId::DriverFaultConfigurationValid));

    // The rename kept its position in the ordering, so an arming refusal's
    // `firstFailure()` ordering is unchanged for every other check.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::TickPolarityAgrees) + 1,
                          static_cast<int>(ev::ConfigurationCheckId::DriverFaultConfigurationValid));

    ev::ConfigurationBuildChecks checks;
    checks.driverFaultConfigurationValid = true;
    TEST_ASSERT_TRUE(
        checks.passed(ev::ConfigurationCheckId::DriverFaultConfigurationValid));

    // A report where the driver-fault check passes is still not an armable
    // vehicle: the other gates are untouched and still refuse.
    TEST_ASSERT_FALSE(checks.allPass());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::ConfigurationValid),
                          static_cast<int>(checks.firstFailure()));

    // And the field is not aliased to another check's verdict.
    ev::ConfigurationBuildChecks other;
    other.calibrationMeasured = true;
    TEST_ASSERT_FALSE(
        other.passed(ev::ConfigurationCheckId::DriverFaultConfigurationValid));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_installed_with_verified_polarity_passes);
    RUN_TEST(test_installed_with_unverified_polarity_fails);
    RUN_TEST(test_not_installed_makes_polarity_verification_inapplicable);
    RUN_TEST(test_installed_is_the_default_capability);
    RUN_TEST(test_capability_names_are_wire_safe);
    RUN_TEST(test_not_installed_never_observes_a_driver_fault);
    RUN_TEST(test_installed_mode_runtime_behavior_is_unchanged);
    RUN_TEST(test_configuration_check_id_is_capability_aware_and_does_not_weaken_others);
    return UNITY_END();
}
