#pragma once

#include <cstdint>

#include "Motion.h"
#include "StandaloneIndicator.h"
#include "StartTrigger.h"

namespace ev {

// ---------------------------------------------------------------------------
// ImmediateSequenceController - open-loop, pencil-triggered movement sequence
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The open-loop autonomous movement controller for the immediate standalone
//   operating mode (`env:nano_r4`).
// * Completely independent of the closed-loop competition readiness checks
//   (`CalibrationMeasured`, `MeasuredValuesCiteEvidence`, `ControllerConfigValid`,
//   `RunPolicyUsable`, `ExecutionPlanNotEstablished`, braking offsets, encoder
//   distance scaling, bottle geometry, timing models, and the 1.5 s hold-to-arm
//   gesture).
// * Executes the explicit state machine:
//
//   POWER_OFF -> INITIALIZING -> WAITING_FOR_TRIGGER -> FORWARD -> STOPPING
//             -> REVERSE -> FINISHED
//
//   (with FAULT as the latched fail-safe state if the overall run timeout or
//   a configuration error is encountered).
//
// Essential safeguards preserved
//
//   1. Disabled motor at boot (`POWER_OFF` and `INITIALIZING`).
//   2. Boot-held trigger rejection via `ev::StartTriggerDetector`: the trigger
//      must be stably released (`inactiveQualificationMs`) during `INITIALIZING`
//      before entering `WAITING_FOR_TRIGGER`.
//   3. Debounced trigger press (`activeDebounceMs`): a single fresh press in
//      `WAITING_FOR_TRIGGER` starts the sequence directly without any 1.5 s hold
//      or serial arming command.
//   4. Bounded effort (`kImmediateMaxEffortPermille`) and a hard maximum overall
//      runtime (`runTimeoutMs`).
//   5. Explicit motor-off interval (`stoppingDurationMs`) between `FORWARD` and
//      `REVERSE` so the motor output is de-energized before changing direction.
//   6. Motor shutdown on completion (`FINISHED`) or timeout (`FAULT`), with no
//      retrigger allowed until a deliberate reset or power cycle.

// Hard ceiling on open-loop effort in this controller (permille of full scale).
constexpr uint16_t kImmediateMaxEffortPermille = 500U;

struct ImmediateSequenceConfig {
    constexpr ImmediateSequenceConfig(uint16_t forwardEffortPermille_,
                                      uint32_t forwardDurationMs_,
                                      uint32_t stoppingDurationMs_,
                                      uint16_t reverseEffortPermille_,
                                      uint32_t reverseDurationMs_,
                                      uint32_t runTimeoutMs_,
                                      StartTriggerConfig triggerConfig_)
        : forwardEffortPermille(forwardEffortPermille_),
          forwardDurationMs(forwardDurationMs_),
          stoppingDurationMs(stoppingDurationMs_),
          reverseEffortPermille(reverseEffortPermille_),
          reverseDurationMs(reverseDurationMs_),
          runTimeoutMs(runTimeoutMs_),
          triggerConfig(triggerConfig_) {}

    uint16_t forwardEffortPermille;
    uint32_t forwardDurationMs;
    uint32_t stoppingDurationMs;
    uint16_t reverseEffortPermille;
    uint32_t reverseDurationMs;
    uint32_t runTimeoutMs;
    StartTriggerConfig triggerConfig;
};

// Initial conservative supervised, wheels-raised test configuration:
//   Forward motor effort:              150/1000
//   Forward duration:                  1,000 ms
//   Motor-off interval before reverse: 500 ms
//   Reverse motor effort:              150/1000
//   Reverse duration:                  800 ms
//   Overall run timeout:               4,000 ms
//   Retrigger after completion:        Disabled until deliberate reset/power cycle
constexpr uint16_t kDefaultImmediateForwardEffortPermille = 150U;
constexpr uint32_t kDefaultImmediateForwardDurationMs = 1000U;
constexpr uint32_t kDefaultImmediateStoppingDurationMs = 500U;
constexpr uint16_t kDefaultImmediateReverseEffortPermille = 150U;
constexpr uint32_t kDefaultImmediateReverseDurationMs = 800U;
constexpr uint32_t kDefaultImmediateRunTimeoutMs = 4000U;

constexpr ImmediateSequenceConfig kDefaultImmediateSequenceConfig(
    kDefaultImmediateForwardEffortPermille,
    kDefaultImmediateForwardDurationMs,
    kDefaultImmediateStoppingDurationMs,
    kDefaultImmediateReverseEffortPermille,
    kDefaultImmediateReverseDurationMs,
    kDefaultImmediateRunTimeoutMs,
    kDefaultStartTriggerConfig);

constexpr bool immediateSequenceConfigValid(const ImmediateSequenceConfig& config) {
    // The duration sum is widened to 64 bits before comparing: the three
    // phase durations are each `uint32_t`, so a 32-bit sum could wrap (e.g.
    // three large durations summing past 2^32 would compare as a small
    // number) and admit phases that outlast the hard run timeout. The
    // comparison must see the true sum. Single-return form: this header is
    // also compiled as C++11 on the host toolchain.
    return config.forwardEffortPermille > 0U &&
           config.forwardEffortPermille <= kImmediateMaxEffortPermille &&
           config.reverseEffortPermille > 0U &&
           config.reverseEffortPermille <= kImmediateMaxEffortPermille &&
           config.forwardDurationMs > 0U &&
           config.stoppingDurationMs > 0U &&
           config.reverseDurationMs > 0U &&
           config.runTimeoutMs > 0U &&
           (static_cast<uint64_t>(config.forwardDurationMs) +
            static_cast<uint64_t>(config.stoppingDurationMs) +
            static_cast<uint64_t>(config.reverseDurationMs)) <=
               static_cast<uint64_t>(config.runTimeoutMs);
}

static_assert(immediateSequenceConfigValid(kDefaultImmediateSequenceConfig),
              "default immediate sequence configuration must satisfy safety bounds");

enum class ImmediateSequenceState : uint8_t {
    POWER_OFF = 0,
    INITIALIZING = 1,
    WAITING_FOR_TRIGGER = 2,
    FORWARD = 3,
    STOPPING = 4,
    REVERSE = 5,
    FINISHED = 6,
    FAULT = 7,
};

inline const char* immediateSequenceStateName(ImmediateSequenceState state) {
    switch (state) {
        case ImmediateSequenceState::POWER_OFF:
            return "POWER_OFF";
        case ImmediateSequenceState::INITIALIZING:
            return "INITIALIZING";
        case ImmediateSequenceState::WAITING_FOR_TRIGGER:
            return "WAITING_FOR_TRIGGER";
        case ImmediateSequenceState::FORWARD:
            return "FORWARD";
        case ImmediateSequenceState::STOPPING:
            return "STOPPING";
        case ImmediateSequenceState::REVERSE:
            return "REVERSE";
        case ImmediateSequenceState::FINISHED:
            return "FINISHED";
        case ImmediateSequenceState::FAULT:
            return "FAULT";
    }
    return "?";
}

// Output command for the immediate open-loop sequence. Constructible only by
// `ImmediateSequenceController` so that `evhw::motorDriver::applyImmediate()`
// only accepts commands produced by this state machine.
class ImmediateDriveOutput {
public:
    constexpr ImmediateDriveOutput() : mode_(MotorMode::Disabled), magnitudePermille_(0U) {}

    MotorMode mode() const { return mode_; }
    uint16_t magnitudePermille() const { return magnitudePermille_; }
    ActuationClass actuationClass() const { return actuationClassOf(mode_); }
    bool isDriving() const { return isDrivingMode(mode_); }

private:
    friend class ImmediateSequenceController;

    constexpr ImmediateDriveOutput(MotorMode mode, uint16_t magnitudePermille)
        : mode_(mode), magnitudePermille_(magnitudePermille) {}

    MotorMode mode_;
    uint16_t magnitudePermille_;
};

struct ImmediateSequenceStep {
    ImmediateSequenceState state = ImmediateSequenceState::POWER_OFF;
    ImmediateDriveOutput output{};
    StandaloneIndicatorState indicator = StandaloneIndicatorState::Waiting;
    bool sequenceStartedThisPass = false;
    bool sequenceFinishedThisPass = false;
    bool faultedThisPass = false;
};

class ImmediateSequenceController {
public:
    explicit ImmediateSequenceController(
        const ImmediateSequenceConfig& config = kDefaultImmediateSequenceConfig)
        : config_(config), trigger_(config.triggerConfig) {}

    const ImmediateSequenceConfig& config() const { return config_; }
    ImmediateSequenceState state() const { return state_; }
    const StartTriggerDetector& trigger() const { return trigger_; }

    // Transitions from POWER_OFF to INITIALIZING on boot or deliberate reset.
    // The motor remains disabled and the trigger detector requires a full
    // post-boot inactive qualification window before entering WAITING_FOR_TRIGGER.
    void begin(uint32_t nowMs) {
        trigger_.reset();
        runStartedAtMs_ = nowMs;
        phaseEnteredAtMs_ = nowMs;
        if (!immediateSequenceConfigValid(config_)) {
            state_ = ImmediateSequenceState::FAULT;
            return;
        }
        state_ = ImmediateSequenceState::INITIALIZING;
    }

    // Resets back to POWER_OFF (motor disabled, trigger reset).
    void reset() {
        trigger_.reset();
        state_ = ImmediateSequenceState::POWER_OFF;
        runStartedAtMs_ = 0U;
        phaseEnteredAtMs_ = 0U;
    }

    // Steps the state machine with the current `millis()` timestamp and the raw
    // active-LOW trigger reading (`true` when D12 reads LOW).
    ImmediateSequenceStep update(uint32_t nowMs, bool rawTriggerActive) {
        ImmediateSequenceStep step;

        if (!immediateSequenceConfigValid(config_)) {
            if (state_ != ImmediateSequenceState::FAULT) {
                state_ = ImmediateSequenceState::FAULT;
                step.faultedThisPass = true;
            }
            step.state = state_;
            step.output = ImmediateDriveOutput(MotorMode::Disabled, 0U);
            step.indicator = indicatorState();
            return step;
        }

        // If update() is called before begin(), stay safely in POWER_OFF with
        // motor disabled.
        if (state_ == ImmediateSequenceState::POWER_OFF) {
            step.state = state_;
            step.output = ImmediateDriveOutput(MotorMode::Disabled, 0U);
            step.indicator = indicatorState();
            return step;
        }

        const StartTriggerVerdict triggerVerdict = trigger_.update(nowMs, rawTriggerActive);

        // Enforce the hard overall runtime ceiling across all active run states.
        if (state_ == ImmediateSequenceState::FORWARD ||
            state_ == ImmediateSequenceState::STOPPING ||
            state_ == ImmediateSequenceState::REVERSE) {
            if (static_cast<uint32_t>(nowMs - runStartedAtMs_) >= config_.runTimeoutMs) {
                state_ = ImmediateSequenceState::FAULT;
                step.faultedThisPass = true;
            }
        }

        switch (state_) {
            case ImmediateSequenceState::POWER_OFF:
                break;

            case ImmediateSequenceState::INITIALIZING:
                if (triggerVerdict.inactiveQualified) {
                    state_ = ImmediateSequenceState::WAITING_FOR_TRIGGER;
                    phaseEnteredAtMs_ = nowMs;
                }
                break;

            case ImmediateSequenceState::WAITING_FOR_TRIGGER:
                if (triggerVerdict.activated) {
                    state_ = ImmediateSequenceState::FORWARD;
                    runStartedAtMs_ = nowMs;
                    phaseEnteredAtMs_ = nowMs;
                    step.sequenceStartedThisPass = true;
                }
                break;

            case ImmediateSequenceState::FORWARD:
                if (static_cast<uint32_t>(nowMs - phaseEnteredAtMs_) >=
                    config_.forwardDurationMs) {
                    state_ = ImmediateSequenceState::STOPPING;
                    phaseEnteredAtMs_ = nowMs;
                }
                break;

            case ImmediateSequenceState::STOPPING:
                if (static_cast<uint32_t>(nowMs - phaseEnteredAtMs_) >=
                    config_.stoppingDurationMs) {
                    state_ = ImmediateSequenceState::REVERSE;
                    phaseEnteredAtMs_ = nowMs;
                }
                break;

            case ImmediateSequenceState::REVERSE:
                if (static_cast<uint32_t>(nowMs - phaseEnteredAtMs_) >=
                    config_.reverseDurationMs) {
                    state_ = ImmediateSequenceState::FINISHED;
                    phaseEnteredAtMs_ = nowMs;
                    step.sequenceFinishedThisPass = true;
                }
                break;

            case ImmediateSequenceState::FINISHED:
            case ImmediateSequenceState::FAULT:
                // Terminal until deliberate reset() / begin() or power cycle.
                break;
        }

        step.state = state_;
        step.output = currentOutput();
        step.indicator = indicatorState();
        return step;
    }

    ImmediateDriveOutput currentOutput() const {
        switch (state_) {
            case ImmediateSequenceState::FORWARD:
                return ImmediateDriveOutput(MotorMode::DriveForward,
                                            config_.forwardEffortPermille);
            case ImmediateSequenceState::REVERSE:
                return ImmediateDriveOutput(MotorMode::DriveReverse,
                                            config_.reverseEffortPermille);
            case ImmediateSequenceState::POWER_OFF:
            case ImmediateSequenceState::INITIALIZING:
            case ImmediateSequenceState::WAITING_FOR_TRIGGER:
            case ImmediateSequenceState::STOPPING:
            case ImmediateSequenceState::FINISHED:
            case ImmediateSequenceState::FAULT:
                return ImmediateDriveOutput(MotorMode::Disabled, 0U);
        }
        return ImmediateDriveOutput(MotorMode::Disabled, 0U);
    }

    StandaloneIndicatorState indicatorState() const {
        switch (state_) {
            case ImmediateSequenceState::POWER_OFF:
            case ImmediateSequenceState::INITIALIZING:
                return StandaloneIndicatorState::Waiting;
            case ImmediateSequenceState::WAITING_FOR_TRIGGER:
                return StandaloneIndicatorState::Ready;
            case ImmediateSequenceState::FORWARD:
            case ImmediateSequenceState::STOPPING:
            case ImmediateSequenceState::REVERSE:
                return StandaloneIndicatorState::Running;
            case ImmediateSequenceState::FINISHED:
                return StandaloneIndicatorState::Finished;
            case ImmediateSequenceState::FAULT:
                return StandaloneIndicatorState::Faulted;
        }
        return StandaloneIndicatorState::Faulted;
    }

private:
    ImmediateSequenceConfig config_;
    StartTriggerDetector trigger_;
    ImmediateSequenceState state_ = ImmediateSequenceState::POWER_OFF;
    uint32_t runStartedAtMs_ = 0U;
    uint32_t phaseEnteredAtMs_ = 0U;
};

}  // namespace ev
