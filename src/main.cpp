#include <Arduino.h>

#include <cstdio>

#include <BoundedText.h>
#include <FirmwareIdentity.h>
#include <ImmediateSequence.h>
#include <MotorDriverPlan.h>
#include <StandaloneIndicator.h>
#include <StartTrigger.h>

#include "MotorDriver.h"
#include "PinMap.h"
#include "SerialTransport.h"
#include "WatchdogCheckpoint.h"

#if defined(EV_BUILD_IDENTITY_AVAILABLE)
// Generated per build by scripts/ev_build_identity.py into the build
// directory (never committed): EV_BUILD_REVISION, EV_BUILD_DIRTY.
#include "EvBuildIdentity.h"
#endif

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
//   7. Reports an output-only USB diagnostic (BOOT identity plus periodic
//      status) when a host is attached. The diagnostic is never required:
//      with no USB host the sequence runs identically and silently.
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
//
// Starting-torque note: 15% PWM is a conservative wheels-raised starting
// point, and a nonzero PWM command does NOT guarantee the shaft turns -
// static friction, drivetrain load, battery sag and the driver's
// current limiting can all hold the motor stalled at a low duty. If the LED
// sequence runs but the shaft does not move, raise the effort deliberately
// in small steps (e.g. 150 -> 200 -> 250, never above the 500 ceiling
// enforced by `ev::kImmediateMaxEffortPermille`) within the verified
// driver/motor/supply limits, with wheels raised and the motor-power
// disconnect within reach. Do not add automatic effort increases: an
// unattended escalation is uncontrolled motion.
//
// Stop-interval note: the 500 ms STOPPING phase is a bounded motor-off
// interval, NOT a measured standstill. It keeps this wheels-raised test
// mode bounded, but it does not prove the wheels stopped before reversal;
// a ground-operated mode needs a stop condition verified on hardware
// (encoder-observed standstill) before reversing.
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

// ===========================================================================
// OPTIONAL USB DIAGNOSTICS (output-only, never required for motion)
// ===========================================================================
//
// When a USB host is attached, this image reports:
//
//   * one `EV BOOT ...` line identifying the firmware (family, source
//     revision, dirty state, image variant, and the programmed
//     effort/duration/timeout values), so a serial monitor shows whether
//     the board runs this immediate image or some other image (e.g. the
//     competition or bench image, which report a different variant); and
//   * one `EV ...` status line every 500 ms with the state-machine phase,
//     the raw D12 trigger reading, the debounced trigger state, the
//     commanded motor mode/effort, the requested D8/D9/D10 levels, and the
//     time since trigger acceptance and phase entry.
//
// What this diagnostic is NOT:
//
//   * It never blocks: lines go through the bounded, free-space-gated
//     transport (`evhw::serialTransport`), which stages at most one line
//     and pumps only what the port accepts right now. With no host
//     attached nothing is staged at all, so standalone battery operation
//     is unaffected.
//   * It never reads serial input and contains no motor-drive command.
//     There is no way to command motion from the host in this image.
//   * It is not evidence about the vehicle: it reports what the firmware
//     commanded and sampled, never a measurement of what moved.
constexpr unsigned long kDiagSerialBaud = 115200;
constexpr uint32_t kDiagStatusPeriodMs = 500U;

const char* diagRevision() {
#if defined(EV_BUILD_IDENTITY_AVAILABLE)
    return EV_BUILD_REVISION;
#else
    return ev::kFirmwareRevisionUnknown;
#endif
}

int diagDirtyFlag() {
#if defined(EV_BUILD_IDENTITY_AVAILABLE)
    return EV_BUILD_DIRTY;
#else
    return 1;
#endif
}

const char* diagMotorModeName(ev::MotorMode mode) {
    switch (mode) {
        case ev::MotorMode::Disabled:
            return "Disabled";
        case ev::MotorMode::Coast:
            return "Coast";
        case ev::MotorMode::Brake:
            return "Brake";
        case ev::MotorMode::DriveForward:
            return "DriveForward";
        case ev::MotorMode::DriveReverse:
            return "DriveReverse";
    }
    return "?";
}

bool diagBootReported = false;
uint32_t diagLastReportMs = 0U;
bool diagHaveLastState = false;
ev::ImmediateSequenceState diagLastState = ev::ImmediateSequenceState::POWER_OFF;
uint32_t diagStateEnteredMs = 0U;
uint32_t diagRunStartMs = 0U;
bool diagRunStarted = false;

// Stages the line already composed into the transport's working buffer (a
// NUL-terminated `snprintf` count of `written` characters, excluding the
// terminator); drops the line when it does not fit or the transport is busy
// with a previous one. Dropping is correct here: the next 500 ms report
// replaces this one, and a diagnostic must never displace or delay motion.
void diagStageLine(int written) {
    if (written <= 0 || static_cast<uint16_t>(written) > ev::kSerialLineCapacity) {
        return;
    }
    (void)evhw::serialTransport::port().stage(static_cast<uint16_t>(written), false);
}

void diagStageBootLine() {
    char* working = evhw::serialTransport::port().workingBuffer();
    const int written = std::snprintf(
        working, ev::kSerialLineBufferBytes,
        "EV BOOT fw=%s rev=%s dirty=%d variant=immediate-standalone "
        "fwd_permille=%u fwd_ms=%lu stop_ms=%lu rev_permille=%u rev_ms=%lu timeout_ms=%lu",
        ev::kFirmwareFamily, diagRevision(), diagDirtyFlag(),
        static_cast<unsigned>(kForwardEffortPermille),
        static_cast<unsigned long>(kForwardDurationMs),
        static_cast<unsigned long>(kStoppingDurationMs),
        static_cast<unsigned>(kReverseEffortPermille),
        static_cast<unsigned long>(kReverseDurationMs),
        static_cast<unsigned long>(kRunTimeoutMs));
    diagStageLine(written);
}

void diagStageStatusLine(uint32_t nowMs, bool rawTriggerActive,
                         const ev::ImmediateSequenceStep& step) {
    // Requested driver levels (intent sampled from the same plan table the
    // pin writes use), plus the PWM count the EN duty maps to.
    const ev::Drv8874Inputs requested =
        ev::drv8874InputsForRequest(step.output.mode(), step.output.magnitudePermille());
    const uint16_t enCount =
        ev::pwmCountFromPermille(requested.dutyPermille, ev::kPlannedPwmResolutionBits);
    const uint32_t sinceTrigMs = diagRunStarted ? (nowMs - diagRunStartMs) : 0U;
    const uint32_t phaseMs = nowMs - diagStateEnteredMs;
    // The configuration is compile-time validated (static_assert above), so
    // the only runtime path into FAULT is the hard run timeout.
    const char* fault = (step.state == ev::ImmediateSequenceState::FAULT) ? "run_timeout" : "none";
    char* working = evhw::serialTransport::port().workingBuffer();
    const int written = std::snprintf(
        working, ev::kSerialLineBufferBytes,
        "EV state=%s trig_raw=%d trig=%s qual=%d mode=%s permille=%u "
        "ph=%d en=%u nsleep=%d since_trig_ms=%lu phase_ms=%lu fault=%s",
        ev::immediateSequenceStateName(step.state), rawTriggerActive ? 1 : 0,
        ev::startTriggerStateName(controllerRef().trigger().state()),
        controllerRef().trigger().inactiveQualified() ? 1 : 0,
        diagMotorModeName(step.output.mode()),
        static_cast<unsigned>(step.output.magnitudePermille()),
        requested.phaseHigh ? 1 : 0, static_cast<unsigned>(enCount),
        requested.nSleepHigh ? 1 : 0, static_cast<unsigned long>(sinceTrigMs),
        static_cast<unsigned long>(phaseMs), fault);
    diagStageLine(written);
}

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

    // 4. Open the optional USB diagnostic port. Output-only: nothing here
    //    waits for a host, reads input, or gates motion on a connection.
    evhw::serialTransport::begin(kDiagSerialBaud);
    diagBootReported = false;
    diagLastReportMs = 0U;
    diagHaveLastState = false;
    diagRunStarted = false;

    // 5. Initialize the open-loop sequence controller (`POWER_OFF -> INITIALIZING`).
    const uint32_t nowMs = millis();
    controllerRef().begin(nowMs);

    // 6. Record watchdog checkpoint boot instant (disabled in this image).
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

    if (step.sequenceStartedThisPass) {
        diagRunStartMs = nowMs;
        diagRunStarted = true;
    }
    if (!diagHaveLastState || step.state != diagLastState) {
        diagHaveLastState = true;
        diagLastState = step.state;
        diagStateEnteredMs = nowMs;
    }

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

    // Optional USB diagnostics: pump staged bytes every pass (nonblocking),
    // and stage at most one line per 500 ms, only while a host is attached.
    // With no USB host nothing is staged and motion runs identically.
    evhw::serialTransport::port().pump();
    if (static_cast<uint32_t>(nowMs - diagLastReportMs) >= kDiagStatusPeriodMs) {
        diagLastReportMs = nowMs;
        if (static_cast<bool>(Serial)) {
            if (!diagBootReported) {
                diagStageBootLine();
                diagBootReported = true;
            } else {
                diagStageStatusLine(nowMs, rawTriggerActive, step);
            }
        }
    }
}
