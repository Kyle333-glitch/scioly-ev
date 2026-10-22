#include <cstdint>
#include <string>
#include <unity.h>

#include <Arduino.h>

// Compile and exercise the actual default `nano_r4` entry point (`src/main.cpp`),
// `src/MotorDriver.h`, and `lib/evcore/src/ImmediateSequence.h` against the
// host-side Arduino mock to verify the complete hardware pin path:
//   D12 LOW -> trigger accepted -> movement state -> motor driver command
//           -> PH (D8) / EN (D9) / nSLEEP (D10) outputs.
#include "../../src/main.cpp"

evmock::MockSerial Serial;
WDTimer WDT;

namespace {

int lastAnalogWriteOnPin(int pin) {
    int lastValue = 0;
    for (const evmock::PinWrite& w : evmock::pinWriteLog()) {
        if (w.pin == pin && w.isAnalog) {
            lastValue = w.value;
        }
    }
    return lastValue;
}

void setTriggerPressed(bool pressed) {
    // Active LOW with INPUT_PULLUP on D12 (`evhw::kStartTrigger`).
    evmock::pinLevels()[evhw::kStartTrigger] = pressed ? LOW : HIGH;
}

void resetEntryTest(uint32_t bootMs = 0U) {
    Serial.reset();
    Serial.connected = false;  // Disconnected USB (standalone battery operation)
    evmock::resetPins();
    evmock::resetIsr();
    evmock::mockMillis() = bootMs;
    WDT = WDTimer();
    evhw::motorDriver::setMotorPinsOwned(false);
    evhw::motorDriver::detail::lastPhaseHighWritten() = false;
    setTriggerPressed(false);
    controllerRef().reset();
    setup();
}

void stepLoopAt(uint32_t nowMs) {
    evmock::mockMillis() = nowMs;
    loop();
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_boot_initializes_motor_disabled_and_waits_for_trigger_release(void) {
    resetEntryTest(0U);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                          static_cast<int>(controllerRef().state()));
    const evhw::motorDriver::PinLevels levels = evhw::motorDriver::pinLevels();
    TEST_ASSERT_TRUE(levels.deEnergized);
    TEST_ASSERT_FALSE(levels.nSleepHigh);
    TEST_ASSERT_FALSE(levels.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(0U, levels.dutyPermilleWritten);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));
}

void test_trigger_held_during_boot_never_starts_motor(void) {
    Serial.reset();
    Serial.connected = false;
    evmock::resetPins();
    evmock::mockMillis() = 0U;
    setTriggerPressed(true);  // Held LOW during boot
    controllerRef().reset();
    setup();

    for (uint32_t t = 0U; t <= 5000U; t += 20U) {
        stepLoopAt(t);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                              static_cast<int>(controllerRef().state()));
        TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
        TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));
    }
}

void test_trigger_released_after_boot_and_debounced_press_runs_full_sequence_on_pins(void) {
    resetEntryTest(0U);

    // 1. Trigger released after boot: qualify for 100 ms -> WAITING_FOR_TRIGGER.
    stepLoopAt(0U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                          static_cast<int>(controllerRef().state()));
    stepLoopAt(100U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::WAITING_FOR_TRIGGER),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
    // Steady orange LED in WAITING_FOR_TRIGGER.
    TEST_ASSERT_EQUAL_INT(HIGH, evmock::pinLevels()[LED_BUILTIN]);

    // 2. Short contact bounce (20 ms < 40 ms debounce) does not start motor.
    setTriggerPressed(true);
    stepLoopAt(110U);
    stepLoopAt(125U);
    setTriggerPressed(false);
    stepLoopAt(130U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::WAITING_FOR_TRIGGER),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);

    // 3. Fresh debounced press (40 ms at t = 200..240 ms) directly starts FORWARD
    //    without any 1.5 s hold or serial commands.
    setTriggerPressed(true);
    stepLoopAt(200U);
    stepLoopAt(220U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::WAITING_FOR_TRIGGER),
                          static_cast<int>(controllerRef().state()));

    stepLoopAt(240U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));

    // Verify nonzero forward output on DRV8874 pins (nSLEEP=HIGH, PH=HIGH, EN PWM > 0)
    // and verify nothing overwrote the motor pins with a disabled command.
    evhw::motorDriver::PinLevels fwdLevels = evhw::motorDriver::pinLevels();
    TEST_ASSERT_FALSE(fwdLevels.deEnergized);
    TEST_ASSERT_TRUE(fwdLevels.nSleepHigh);
    TEST_ASSERT_TRUE(fwdLevels.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(150U, fwdLevels.dutyPermilleWritten);
    const uint16_t expectedPwm = ev::pwmCountFromPermille(150U, ev::kPlannedPwmResolutionBits);
    TEST_ASSERT_TRUE(expectedPwm > 0U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedPwm), lastAnalogWriteOnPin(evhw::kMotorEnable));

    // Release trigger during FORWARD and step across multiple loop passes:
    // motor stays energized in FORWARD for the full 1,000 ms duration.
    setTriggerPressed(false);
    stepLoopAt(500U);
    stepLoopAt(1000U);
    stepLoopAt(1239U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().nSleepHigh);
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(150U, evhw::motorDriver::pinLevels().dutyPermilleWritten);

    // 4. At 1,000 ms after start (t = 1240 ms): enters STOPPING (motor-off interval
    //    for 500 ms before reversing).
    stepLoopAt(1240U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::STOPPING),
                          static_cast<int>(controllerRef().state()));
    evhw::motorDriver::PinLevels stopLevels = evhw::motorDriver::pinLevels();
    TEST_ASSERT_TRUE(stopLevels.deEnergized);
    TEST_ASSERT_FALSE(stopLevels.nSleepHigh);
    TEST_ASSERT_FALSE(stopLevels.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(0U, stopLevels.dutyPermilleWritten);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));

    stepLoopAt(1739U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::STOPPING),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);

    // 5. At 500 ms after STOPPING start (t = 1740 ms): enters REVERSE for 800 ms
    //    with nSLEEP=HIGH, PH=LOW, EN PWM = 150 permille.
    stepLoopAt(1740U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::REVERSE),
                          static_cast<int>(controllerRef().state()));
    evhw::motorDriver::PinLevels revLevels = evhw::motorDriver::pinLevels();
    TEST_ASSERT_FALSE(revLevels.deEnergized);
    TEST_ASSERT_TRUE(revLevels.nSleepHigh);
    TEST_ASSERT_FALSE(revLevels.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(150U, revLevels.dutyPermilleWritten);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedPwm), lastAnalogWriteOnPin(evhw::kMotorEnable));

    stepLoopAt(2539U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::REVERSE),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_FALSE(evhw::motorDriver::pinLevels().deEnergized);

    // 6. At 800 ms after REVERSE start (t = 2540 ms): automatic stopping into FINISHED.
    stepLoopAt(2540U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FINISHED),
                          static_cast<int>(controllerRef().state()));
    evhw::motorDriver::PinLevels finLevels = evhw::motorDriver::pinLevels();
    TEST_ASSERT_TRUE(finLevels.deEnergized);
    TEST_ASSERT_FALSE(finLevels.nSleepHigh);
    TEST_ASSERT_FALSE(finLevels.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(0U, finLevels.dutyPermilleWritten);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));

    // 7. No retrigger: pressing D12 again after completion keeps the motor disabled.
    stepLoopAt(2700U);
    setTriggerPressed(true);
    stepLoopAt(2750U);
    stepLoopAt(2800U);
    setTriggerPressed(false);
    stepLoopAt(3000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FINISHED),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));
}

void test_unexpected_reset_returns_to_disabled_state(void) {
    resetEntryTest(0U);
    stepLoopAt(0U);
    stepLoopAt(100U);
    setTriggerPressed(true);
    stepLoopAt(100U);
    stepLoopAt(140U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_FALSE(evhw::motorDriver::pinLevels().deEnergized);

    // Simulate an unexpected MCU reset while the motor was driving and trigger held.
    setup();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));

    // Holding trigger after reset cannot restart.
    stepLoopAt(200U);
    stepLoopAt(500U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
}

void test_driver_ownership_and_no_runtime_interference_across_direction_change(void) {
    resetEntryTest(0U);
    stepLoopAt(0U);
    stepLoopAt(100U);

    // Clear pin write log before pressing trigger so we can inspect the exact
    // DRV8874 write sequence during FORWARD -> STOPPING -> REVERSE -> FINISHED.
    evmock::pinWriteLog().clear();

    setTriggerPressed(true);
    stepLoopAt(100U);
    stepLoopAt(140U);  // Enters FORWARD

    // During FORWARD, every loop pass writes nSLEEP HIGH, PH HIGH, EN > 0 and
    // never writes a conflicting disabled command from any background runtime.
    for (uint32_t t = 160U; t < 1140U; t += 20U) {
        stepLoopAt(t);
        const evhw::motorDriver::PinLevels levels = evhw::motorDriver::pinLevels();
        TEST_ASSERT_FALSE(levels.deEnergized);
        TEST_ASSERT_TRUE(levels.nSleepHigh);
        TEST_ASSERT_TRUE(levels.phaseHigh);
        TEST_ASSERT_EQUAL_UINT16(150U, levels.dutyPermilleWritten);
    }

    // At 1140 ms (STOPPING), motor output is de-energized before direction changes.
    evmock::pinWriteLog().clear();
    stepLoopAt(1140U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::STOPPING),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);

    // Verify write order on transition out of FORWARD: nSLEEP goes LOW and EN goes
    // to 0 before PH changes from HIGH to LOW.
    bool sawSleepLow = false;
    bool sawEnableZero = false;
    bool sawPhaseLowAfterDeenergize = false;
    for (const evmock::PinWrite& w : evmock::pinWriteLog()) {
        if (w.pin == evhw::kMotorSleepBar && !w.isAnalog && w.value == LOW) {
            sawSleepLow = true;
        }
        if (w.pin == evhw::kMotorEnable && w.isAnalog && w.value == 0) {
            sawEnableZero = true;
        }
        if (w.pin == evhw::kMotorPhase && !w.isAnalog && w.value == LOW) {
            if (sawSleepLow && sawEnableZero) {
                sawPhaseLowAfterDeenergize = true;
            }
        }
    }
    TEST_ASSERT_TRUE(sawPhaseLowAfterDeenergize);

    // At 1640 ms (REVERSE), motor drives reverse across every pass without
    // interference.
    for (uint32_t t = 1640U; t < 2440U; t += 20U) {
        stepLoopAt(t);
        const evhw::motorDriver::PinLevels levels = evhw::motorDriver::pinLevels();
        TEST_ASSERT_FALSE(levels.deEnergized);
        TEST_ASSERT_TRUE(levels.nSleepHigh);
        TEST_ASSERT_FALSE(levels.phaseHigh);
        TEST_ASSERT_EQUAL_UINT16(150U, levels.dutyPermilleWritten);
    }

    // At 2440 ms (FINISHED), motor is de-energized.
    stepLoopAt(2440U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FINISHED),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
}

void test_steady_phase_passes_do_not_rewrite_motor_pins(void) {
    resetEntryTest(0U);
    stepLoopAt(0U);
    stepLoopAt(100U);
    setTriggerPressed(true);
    stepLoopAt(100U);
    stepLoopAt(140U);  // Enters FORWARD
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));
    setTriggerPressed(false);

    // The first FORWARD write already reached the pins: every further pass
    // in the same phase must hold those levels without rewriting them, so a
    // steady PWM output is never re-poked once per pass. (The status LED
    // on D13 still updates; only the three motor pins must stay quiet.)
    evmock::pinWriteLog().clear();
    for (uint32_t t = 160U; t < 1140U; t += 20U) {
        stepLoopAt(t);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                              static_cast<int>(controllerRef().state()));
    }
    for (const evmock::PinWrite& w : evmock::pinWriteLog()) {
        TEST_ASSERT_TRUE(w.pin != evhw::kMotorPhase && w.pin != evhw::kMotorEnable &&
                         w.pin != evhw::kMotorSleepBar);
    }
    const evhw::motorDriver::PinLevels held = evhw::motorDriver::pinLevels();
    TEST_ASSERT_FALSE(held.deEnergized);
    TEST_ASSERT_TRUE(held.nSleepHigh);
    TEST_ASSERT_TRUE(held.phaseHigh);
    TEST_ASSERT_EQUAL_UINT16(150U, held.dutyPermilleWritten);

    // Leaving the phase still drives the pins: STOPPING de-energizes.
    stepLoopAt(1140U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::STOPPING),
                          static_cast<int>(controllerRef().state()));
    bool sawMotorWrite = false;
    for (const evmock::PinWrite& w : evmock::pinWriteLog()) {
        if (w.pin == evhw::kMotorPhase || w.pin == evhw::kMotorEnable ||
            w.pin == evhw::kMotorSleepBar) {
            sawMotorWrite = true;
        }
    }
    TEST_ASSERT_TRUE(sawMotorWrite);
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
}

void test_usb_diagnostics_report_identity_and_status(void) {
    resetEntryTest(0U);
    // Disconnected USB: the diagnostic stages nothing, motion unaffected.
    stepLoopAt(0U);
    stepLoopAt(100U);
    TEST_ASSERT_TRUE(Serial.takeTx().empty());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::WAITING_FOR_TRIGGER),
                          static_cast<int>(controllerRef().state()));

    // A host attaches: the BOOT identity appears once, then periodic status.
    Serial.connected = true;
    stepLoopAt(500U);  // stages BOOT; the pump moves it on the next pass
    TEST_ASSERT_TRUE(Serial.takeTx().empty());
    stepLoopAt(520U);  // pump delivers BOOT
    const std::string boot = Serial.takeTx();
    TEST_ASSERT_TRUE(boot.find("EV BOOT") != std::string::npos);
    TEST_ASSERT_TRUE(boot.find("variant=immediate-standalone") != std::string::npos);
    TEST_ASSERT_TRUE(boot.find("fwd_permille=150") != std::string::npos);

    // Status follows and names the live phase, trigger and motor command.
    setTriggerPressed(true);
    stepLoopAt(540U);
    stepLoopAt(580U);  // 40 ms debounced press -> FORWARD
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));
    stepLoopAt(1020U);  // next 500 ms boundary stages the status line
    stepLoopAt(1040U);  // the pump delivers it
    const std::string status = Serial.takeTx();
    TEST_ASSERT_TRUE(status.find("state=FORWARD") != std::string::npos);
    TEST_ASSERT_TRUE(status.find("mode=DriveForward") != std::string::npos);
    TEST_ASSERT_TRUE(status.find("permille=150") != std::string::npos);
    TEST_ASSERT_TRUE(status.find("nsleep=1") != std::string::npos);
    TEST_ASSERT_TRUE(status.find("fault=none") != std::string::npos);
    // The diagnostic changed nothing about the motor output.
    TEST_ASSERT_FALSE(evhw::motorDriver::pinLevels().deEnergized);
}

void test_clock_rollover_keeps_trigger_debounce_and_phase_timing(void) {
    const uint32_t bootMs = 0xFFFFFFC6U;  // 58 ms before the 32-bit wrap
    resetEntryTest(bootMs);
    stepLoopAt(bootMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::INITIALIZING),
                          static_cast<int>(controllerRef().state()));
    stepLoopAt(bootMs + 100U);  // wraps to 0x2A: release qualifies across the wrap
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::WAITING_FOR_TRIGGER),
                          static_cast<int>(controllerRef().state()));

    setTriggerPressed(true);
    stepLoopAt(0x2AU);
    stepLoopAt(0x52U);  // 40 ms of continuous press in wrapped time
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FORWARD),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_FALSE(evhw::motorDriver::pinLevels().deEnergized);

    setTriggerPressed(false);
    stepLoopAt(0x52U + 1000U);  // full forward duration, still in wrapped time
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::STOPPING),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
}

void test_maximum_runtime_timeout_deenergizes_pins_in_entry_loop(void) {
    resetEntryTest(0U);
    stepLoopAt(0U);
    stepLoopAt(100U);
    setTriggerPressed(true);
    stepLoopAt(100U);
    stepLoopAt(140U);  // FORWARD starts at 140 ms
    TEST_ASSERT_FALSE(evhw::motorDriver::pinLevels().deEnergized);

    // Advance clock past the 4,000 ms overall timeout without stepping intermediate
    // phase transitions (simulating a long stall or delayed pass at t = 4200 ms).
    stepLoopAt(4200U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ImmediateSequenceState::FAULT),
                          static_cast<int>(controllerRef().state()));
    TEST_ASSERT_TRUE(evhw::motorDriver::pinLevels().deEnergized);
    TEST_ASSERT_EQUAL_INT(0, lastAnalogWriteOnPin(evhw::kMotorEnable));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_initializes_motor_disabled_and_waits_for_trigger_release);
    RUN_TEST(test_trigger_held_during_boot_never_starts_motor);
    RUN_TEST(test_trigger_released_after_boot_and_debounced_press_runs_full_sequence_on_pins);
    RUN_TEST(test_unexpected_reset_returns_to_disabled_state);
    RUN_TEST(test_driver_ownership_and_no_runtime_interference_across_direction_change);
    RUN_TEST(test_maximum_runtime_timeout_deenergizes_pins_in_entry_loop);
    RUN_TEST(test_steady_phase_passes_do_not_rewrite_motor_pins);
    RUN_TEST(test_usb_diagnostics_report_identity_and_status);
    RUN_TEST(test_clock_rollover_keeps_trigger_debounce_and_phase_timing);
    return UNITY_END();
}
