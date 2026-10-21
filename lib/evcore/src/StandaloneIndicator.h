#pragma once

#include <cstdint>

#include "CompetitionStateMachine.h"

namespace ev {

// ---------------------------------------------------------------------------
// StandaloneLedPattern - the onboard status indicator
// ---------------------------------------------------------------------------
//
// With no computer attached, `LED_BUILTIN` is the operator's only status readout
// for a battery-powered vehicle. This header owns the pattern vocabulary and the
// pure timing, so the production image and the standalone diagnostic image report
// their state the same way and neither invents a blink in its entry point.
//
// Every pattern is a pure function of `nowMs` and is evaluated once per loop
// pass; nothing here blocks, allocates or reads hardware.

enum class StandaloneIndicatorState : uint8_t {
    // Waiting: the trigger has not been stably released since boot, a
    // preparation or arm was refused (for example, calibration is still
    // Provisional), or the vehicle is simply idle and disarmed.
    // Slow heartbeat, 100 ms on every 1000 ms.
    Waiting = 0,
    // Armed and waiting for a trigger press. Steady on.
    Ready = 1,
    // Motion underway. Medium blink, 100 ms on / 100 ms off.
    Running = 2,
    // Completed normally; the motor is disabled and held. Double blink per
    // 1200 ms.
    Finished = 3,
    // Latched fault. Fast blink, 50 ms on / 50 ms off.
    Faulted = 4,
};

inline const char* standaloneIndicatorStateName(StandaloneIndicatorState state) {
    switch (state) {
        case StandaloneIndicatorState::Waiting:
            return "Waiting";
        case StandaloneIndicatorState::Ready:
            return "Ready";
        case StandaloneIndicatorState::Running:
            return "Running";
        case StandaloneIndicatorState::Finished:
            return "Finished";
        case StandaloneIndicatorState::Faulted:
            return "Faulted";
    }
    return "?";
}

// The production run's indicator state, from the facts the runtime already
// reports. `Ready` requires BOTH an armed authority and a stably released
// trigger: an armed run whose trigger is still held cannot be started, so
// "steady on" there would claim a readiness the vehicle does not have.
inline StandaloneIndicatorState productionIndicatorState(RunPhase phase, bool armed,
                                                         bool faultLatched,
                                                         bool triggerReleased) {
    if (faultLatched || phase == RunPhase::Fault) {
        return StandaloneIndicatorState::Faulted;
    }
    if (phase == RunPhase::Done) {
        return StandaloneIndicatorState::Finished;
    }
    if (isRunPhaseUnderway(phase)) {
        return StandaloneIndicatorState::Running;
    }
    if (armed && phase == RunPhase::Armed && triggerReleased) {
        return StandaloneIndicatorState::Ready;
    }
    return StandaloneIndicatorState::Waiting;
}

// True when `LED_BUILTIN` should be driven HIGH at `nowMs`.
inline bool standaloneIndicatorLedOn(StandaloneIndicatorState state, uint32_t nowMs) {
    switch (state) {
        case StandaloneIndicatorState::Waiting:
            return (nowMs % 1000U) < 100U;
        case StandaloneIndicatorState::Ready:
            return true;
        case StandaloneIndicatorState::Running:
            return (nowMs % 200U) < 100U;
        case StandaloneIndicatorState::Finished: {
            const uint32_t phaseMs = nowMs % 1200U;
            return (phaseMs < 100U) || (phaseMs >= 200U && phaseMs < 300U);
        }
        case StandaloneIndicatorState::Faulted:
            return (nowMs % 100U) < 50U;
    }
    return false;
}

}  // namespace ev
