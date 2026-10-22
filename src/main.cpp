#include <Arduino.h>

#include <ImmediateSequence.h>
#include <StandaloneIndicator.h>

#include "MotorDriver.h"
#include "PinMap.h"
#include "WatchdogCheckpoint.h"

static_assert(!evhw::kWatchdogEnablementRequested,
              "the production image must not request watchdog enablement; build "
              "env:bench-watchdog for the H0W qualification (P-H0W)");

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
#error "EV_BENCH_H1A_DIAGNOSTIC is bench-only (H1A) and must never be defined for the vehicle image"
#endif

#if defined(EV_EMERGENCY_IMAGE) || defined(DEV_EMERGENCY_IMAGE)
#error "emergency image defines are removed (D35) and must never be defined for the vehicle image"
#endif

// ---------------------------------------------------------------------------
// Immediate standalone operating mode (`env:nano_r4`)
// ---------------------------------------------------------------------------
//
// What this image does:
//
//   1. Boots with the motor driver immediately disabled (nSLEEP LOW, PH LOW,
//      EN = 0 duty).
//   2. Configures the pencil start trigger on D12 (`evhw::kStartTrigger`) with
//      `INPUT_PULLUP` (released = HIGH, activated = LOW).
//   3. Enters `INITIALIZING` and waits for the trigger to be stably released
//      for 100 ms (rejecting a boot-held trigger).
//   4. Enters `WAITING_FOR_TRIGGER` (steady orange LED).
//   5. On a single fresh, debounced pencil-trigger press (40 ms, no 1.5 s hold
//      required, no serial commands, no calibration checks), executes:
//        FORWARD -> STOPPING -> REVERSE -> FINISHED
//   6. De-energizes the motor on completion (`FINISHED`) or timeout (`FAULT`),
//      and ignores any further trigger presses until reset/power cycle.
//
// Note: `VehicleRuntime` is NOT instantiated or ticked in this image, so there
// is only one authorized motor-pin writer (`evhw::motorDriver::applyImmediate`)
// and no closed-loop runtime tick can overwrite the motor pins. The full
// competition controller is preserved in `env:nano_r4-competition`
// (`src/competition/CompetitionMain.cpp`).

namespace {

// ===========================================================================
// IMMEDIATE MOVEMENT SEQUENCE CONFIGURATION
// ===========================================================================
// Modify these constants to adjust the open-loop motor effort and phase
// durations for supervised testing:
//
//   * Efforts are in permille (0..1000, where 150 = 15.0% PWM duty, ceiling 500).
//   * Durations are in milliseconds (`ms`).
//   * `kRunTimeoutMs` must be >= `kForwardDurationMs + kStoppingDurationMs + kReverseDurationMs`.
constexpr uint16_t kForwardEffortPermille = ev::kDefaultImmediateForwardEffortPermille;    // 150 / 1000 (15% PWM)
constexpr uint32_t kForwardDurationMs     = ev::kDefaultImmediateForwardDurationMs;        // 1,000 ms
constexpr uint32_t kStoppingDurationMs    = ev::kDefaultImmediateStoppingDurationMs;       // 500 ms motor-off before reverse
constexpr uint16_t kReverseEffortPermille = ev::kDefaultImmediateReverseEffortPermille;    // 150 / 1000 (15% PWM)
constexpr uint32_t kReverseDurationMs     = ev::kDefaultImmediateReverseDurationMs;        // 800 ms
constexpr uint32_t kRunTimeoutMs          = ev::kDefaultImmediateRunTimeoutMs;             // 4,000 ms hard runtime ceiling

constexpr ev::ImmediateSequenceConfig kSequenceConfig(
    kForwardEffortPermille,
    kForwardDurationMs,
    kStoppingDurationMs,
    kReverseEffortPermille,
    kReverseDurationMs,
    kRunTimeoutMs,
    ev::kDefaultStartTriggerConfig);

static_assert(ev::immediateSequenceConfigValid(kSequenceConfig),
              "Immediate movement sequence configuration violates safety bounds");

ev::ImmediateSequenceController& controllerRef() {
    static ev::ImmediateSequenceController instance(kSequenceConfig);
    return instance;
}

evhw::WatchdogCheckpoint watchdog;

}  // namespace

void setup() {
    // 1. De-energize and configure the DRV8874 motor driver pins first, before
    //    anything else runs.
    evhw::motorDriver::beginDisabled();

    // 2. Configure the onboard status LED (orange LED on D13).
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 3. Configure the physical pencil start trigger on D12 (active LOW with
    //    internal pull-up).
    pinMode(evhw::kStartTrigger, INPUT_PULLUP);

    // 4. Initialize the open-loop sequence controller (`POWER_OFF -> INITIALIZING`).
    const uint32_t nowMs = millis();
    controllerRef().begin(nowMs);

    // 5. Record watchdog checkpoint boot instant (disabled in this image).
    watchdog.begin(nowMs);
    watchdog.openIfRequested(millis());
}

void loop() {
    const uint32_t nowMs = millis();

    // Read the physical pencil trigger on D12 (active LOW with INPUT_PULLUP).
    const bool rawTriggerActive = (digitalRead(evhw::kStartTrigger) == LOW);

    // Step the open-loop state machine.
    const ev::ImmediateSequenceStep step =
        controllerRef().update(nowMs, rawTriggerActive);

    // Command the DRV8874 motor driver through the single motor-driver adapter.
    // Whenever the state machine is not in FORWARD or REVERSE, `step.output` is
    // `MotorMode::Disabled` (0 effort), which drives nSLEEP LOW, PH LOW, EN 0.
    evhw::motorDriver::applyImmediate(step.output);

    // Service the watchdog checkpoint.
    watchdog.endOfPass(true, millis());

    // Update the orange status LED (`LED_BUILTIN`):
    //   - Waiting (INITIALIZING): slow heartbeat (100 ms on per 1000 ms)
    //   - Ready (WAITING_FOR_TRIGGER): steady ON
    //   - Running (FORWARD / STOPPING / REVERSE): medium blink (100 ms on / 100 ms off)
    //   - Finished (FINISHED): double blink per 1200 ms
    //   - Faulted (FAULT): fast blink (50 ms on / 50 ms off)
    digitalWrite(LED_BUILTIN,
                 ev::standaloneIndicatorLedOn(step.indicator, nowMs) ? HIGH : LOW);
}
