#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// StartTriggerDetector - debounced, intentional physical start-trigger filter
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The pure, host-testable filter between the raw active-LOW trigger reading
//   (`SensorInputs.h`) and the run or diagnostic state machine that acts on a
//   start request.
// * It enforces four safety properties required for standalone battery-powered
//   operation without a laptop (`AGENTS.md` section 5):
//
//   1. **Boot-held rejection.** A trigger that is already held active during
//      startup or after a reset can never cause motion. The detector must first
//      observe the trigger stably inactive (`inactiveQualificationMs`) before it
//      arms its edge detector.
//   2. **Contact debounce.** Brief electrical noise or contact bounce shorter
//      than `activeDebounceMs` does not fire an activation.
//   3. **Single-edge pulse per press.** Holding the pencil on the trigger emits
//      `activated == true` on the single update that crosses the debounce
//      threshold and `false` on every subsequent update until the trigger is
//      stably released again (`inactiveQualificationMs`) and pressed anew.
//   4. **No clock reads or GPIO.** Every call takes `nowMs` and the caller's raw
//      `rawActive` boolean (`digitalRead(kStartTrigger) == LOW` on the active-LOW
//      pull-up input). All time comparisons use unsigned 32-bit subtraction, so
//      behavior is wrap-safe across the `millis()` rollover.

struct StartTriggerConfig {
    constexpr StartTriggerConfig(uint32_t inactiveQualificationMs_,
                                 uint32_t activeDebounceMs_)
        : inactiveQualificationMs(inactiveQualificationMs_),
          activeDebounceMs(activeDebounceMs_) {}

    // How long the trigger must remain continuously inactive (HIGH on the
    // active-LOW pin) after boot, reset, or a completed press before a new press
    // can be recognized, milliseconds.
    uint32_t inactiveQualificationMs;

    // How long the trigger must remain continuously active (LOW on the
    // active-LOW pin) after inactive qualification before an intentional
    // activation edge is emitted, milliseconds.
    uint32_t activeDebounceMs;
};

// Default physical-trigger timing policy: 100 ms of stable inactive level to
// clear boot/release, and 40 ms (two 20 ms control periods) of continuous
// active contact to confirm an intentional press.
constexpr StartTriggerConfig kDefaultStartTriggerConfig(100U, 40U);

enum class StartTriggerState : uint8_t {
    // Waiting for the trigger to be observed continuously inactive after boot,
    // reset, or a previous press. A trigger held active during boot stays here.
    WaitingForInactive = 0,
    // The trigger has been stably inactive and is ready to detect a fresh press.
    ReadyForPress = 1,
    // The trigger is currently active and accumulating debounce time.
    DebouncingPress = 2,
    // The press has fired its single activation pulse and is still being held
    // (or has not yet completed its post-press inactive qualification).
    HeldAfterTrigger = 3,
};

inline const char* startTriggerStateName(StartTriggerState state) {
    switch (state) {
        case StartTriggerState::WaitingForInactive:
            return "WaitingForInactive";
        case StartTriggerState::ReadyForPress:
            return "ReadyForPress";
        case StartTriggerState::DebouncingPress:
            return "DebouncingPress";
        case StartTriggerState::HeldAfterTrigger:
            return "HeldAfterTrigger";
    }
    return "?";
}

struct StartTriggerVerdict {
    // True on the single update where a fresh, debounced press is recognized.
    // This is the signal fed to `RunInputs::startTriggered` or the standalone
    // diagnostic trigger.
    bool activated = false;

    // True once the trigger has completed its post-boot / post-press inactive
    // qualification and is not currently latched after a trigger.
    bool inactiveQualified = false;

    // The raw active reading passed to `update()`.
    bool rawActive = false;

    StartTriggerState state = StartTriggerState::WaitingForInactive;
};

class StartTriggerDetector {
public:
    explicit StartTriggerDetector(
        const StartTriggerConfig& config = kDefaultStartTriggerConfig)
        : config_(config) {}

    const StartTriggerConfig& config() const { return config_; }

    // Resets the detector to its post-boot state: any subsequent press requires
    // a fresh inactive qualification window first.
    void reset() {
        state_ = StartTriggerState::WaitingForInactive;
        timerStarted_ = false;
        stateEnteredMs_ = 0U;
        lastRawActive_ = false;
    }

    // Steps the detector with the caller's timestamp and the raw active-LOW
    // boolean (`true` when the trigger contact is closed / pin is LOW).
    StartTriggerVerdict update(uint32_t nowMs, bool rawActive) {
        lastRawActive_ = rawActive;
        StartTriggerVerdict verdict;
        verdict.rawActive = rawActive;

        switch (state_) {
            case StartTriggerState::WaitingForInactive:
                if (rawActive) {
                    // Still held active (e.g. during boot): restart the
                    // inactive qualification timer.
                    timerStarted_ = false;
                } else {
                    if (!timerStarted_) {
                        timerStarted_ = true;
                        stateEnteredMs_ = nowMs;
                    }
                    if (static_cast<uint32_t>(nowMs - stateEnteredMs_) >=
                        config_.inactiveQualificationMs) {
                        state_ = StartTriggerState::ReadyForPress;
                        timerStarted_ = false;
                    }
                }
                break;

            case StartTriggerState::ReadyForPress:
                if (rawActive) {
                    if (config_.activeDebounceMs == 0U) {
                        state_ = StartTriggerState::HeldAfterTrigger;
                        timerStarted_ = false;
                        verdict.activated = true;
                    } else {
                        state_ = StartTriggerState::DebouncingPress;
                        timerStarted_ = true;
                        stateEnteredMs_ = nowMs;
                    }
                }
                break;

            case StartTriggerState::DebouncingPress:
                if (!rawActive) {
                    // Released or bounced open before the debounce window
                    // elapsed: return to ready without firing.
                    state_ = StartTriggerState::ReadyForPress;
                    timerStarted_ = false;
                } else if (static_cast<uint32_t>(nowMs - stateEnteredMs_) >=
                           config_.activeDebounceMs) {
                    state_ = StartTriggerState::HeldAfterTrigger;
                    timerStarted_ = false;
                    verdict.activated = true;
                }
                break;

            case StartTriggerState::HeldAfterTrigger:
                if (rawActive) {
                    // Still held (or bounced back active before release
                    // qualification finished): keep waiting for a clean release.
                    timerStarted_ = false;
                } else {
                    if (!timerStarted_) {
                        timerStarted_ = true;
                        stateEnteredMs_ = nowMs;
                    }
                    if (static_cast<uint32_t>(nowMs - stateEnteredMs_) >=
                        config_.inactiveQualificationMs) {
                        state_ = StartTriggerState::ReadyForPress;
                        timerStarted_ = false;
                    }
                }
                break;
        }

        verdict.state = state_;
        verdict.inactiveQualified =
            (state_ == StartTriggerState::ReadyForPress ||
             state_ == StartTriggerState::DebouncingPress);
        return verdict;
    }

    StartTriggerState state() const { return state_; }
    bool inactiveQualified() const {
        return state_ == StartTriggerState::ReadyForPress ||
               state_ == StartTriggerState::DebouncingPress;
    }
    bool lastRawActive() const { return lastRawActive_; }

private:
    StartTriggerConfig config_;
    StartTriggerState state_ = StartTriggerState::WaitingForInactive;
    bool timerStarted_ = false;
    uint32_t stateEnteredMs_ = 0U;
    bool lastRawActive_ = false;
};

}  // namespace ev
