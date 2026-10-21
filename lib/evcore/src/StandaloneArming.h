#pragma once

#include <cstdint>

#include "StartTrigger.h"

namespace ev {

// ---------------------------------------------------------------------------
// StandaloneArmingGate - the deliberate arm gesture, using the pencil trigger
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The transition between "the firmware booted and is waiting" and "a run has
//   been prepared (announced and armed) and may be started", for the
//   laptop-free standalone image. It exists because standalone operation removes
//   the operator's typed `announce` and `arm`: something onboard has to make
//   that deliberate decision, and it must not be something a bump, a reset or a
//   pencil left on the contact can do by accident.
// * Pure and host-testable. It reads no clock (every call takes `nowMs`), touches
//   no pin, holds no Arduino type and allocates nothing.
//
// The gesture
//
//   **Hold the start trigger for `kStandaloneArmHoldMs`.** The gate only accepts
//   the gesture after `ev::StartTriggerDetector` has observed the trigger stably
//   released (`StartTriggerState::WaitingForInactive` is over), so a trigger that
//   was already held while the board booted cannot arm anything. Once the gesture
//   completes the gate asks its caller to prepare the run once, and never asks
//   again in this power cycle - a held or repeatedly pressed trigger cannot
//   announce or arm a second time.
//
// Why a hold and not just "the first press"
//
//   Without the gesture, a reset during a run would re-arm the vehicle by itself
//   and the next incidental contact with the pencil would start a fresh run from
//   a drifted origin. The hold is short enough to do with one hand on the pencil
//   (1.5 s), needs no extra hardware, and - because the gate is reset by every
//   boot - it is exactly the "intentional transition into an armed state" that
//   `AGENTS.md` section 5 requires.
//
// What it is not
//
// * It is not the arming authority. It asks; `VehicleRuntime` still announces and
//   arms through the same rule validation and `ev::SafetyAuthority` a typed
//   command uses, and a refused arm leaves the gate un-prepared.
// * It does not start a run. Preparing is announce + arm; the run itself still
//   waits for a *separate*, debounced press of the same trigger
//   (`RunInputs::startTriggered`, supplied by `StartTriggerDetector`).

// How long the trigger must be held to count as a deliberate arm gesture.
// Long enough that a bump or a dropped pencil cannot produce it, short enough to
// do comfortably without a second hand.
constexpr uint32_t kStandaloneArmHoldMs = 1500U;

enum class StandaloneArmPhase : uint8_t {
    // The trigger has not been observed stably released since boot, or is being
    // released again after a refused preparation. Nothing can be armed here.
    WaitingForRelease = 0,
    // The trigger is stably released and a hold would arm the vehicle. This is
    // also where a refused preparation waits for a fresh gesture.
    WaitingForHold = 1,
    // The trigger is held and the hold interval has not completed yet.
    Holding = 2,
    // The hold completed and the preparation request was issued this pass.
    Preparing = 3,
    // The run is announced and armed; the next fresh press starts it.
    Prepared = 4,
};

inline const char* standaloneArmPhaseName(StandaloneArmPhase phase) {
    switch (phase) {
        case StandaloneArmPhase::WaitingForRelease:
            return "WaitingForRelease";
        case StandaloneArmPhase::WaitingForHold:
            return "WaitingForHold";
        case StandaloneArmPhase::Holding:
            return "Holding";
        case StandaloneArmPhase::Preparing:
            return "Preparing";
        case StandaloneArmPhase::Prepared:
            return "Prepared";
    }
    return "?";
}

struct StandaloneArmVerdict {
    // True on the single pass the hold completed: the caller must now prepare the
    // run (announce + arm) and report the outcome back with `prepared()`.
    bool requestPrepare = false;

    // True once preparation succeeded: the run is armed and waiting for a fresh
    // trigger press.
    bool prepared = false;

    StandaloneArmPhase phase = StandaloneArmPhase::WaitingForRelease;
};

class StandaloneArmingGate {
public:
    explicit StandaloneArmingGate(uint32_t holdMs = kStandaloneArmHoldMs) : holdMs_(holdMs) {}

    // Back to the post-boot state: nothing is prepared, and the next arm gesture
    // must start from a stably released trigger.
    void reset() {
        phase_ = StandaloneArmPhase::WaitingForRelease;
        prepared_ = false;
        holding_ = false;
        holdStartedMs_ = 0U;
        awaitingRelease_ = false;
    }

    // Reports the outcome of the preparation the gate asked for. `armed` is the
    // live authority state after the attempt: a refused announcement or a refused
    // arm leaves the gate un-prepared, so the vehicle waits instead of pretending
    // readiness, and the next gesture is a fresh one.
    void reportPreparation(bool armed) {
        prepared_ = armed;
        phase_ = armed ? StandaloneArmPhase::Prepared : StandaloneArmPhase::WaitingForRelease;
        holding_ = false;
        awaitingRelease_ = true;
    }

    // Steps the gate with the caller's clock and the trigger detector's verdict
    // for this pass. `trigger.state` is what blocks a boot-held trigger; the raw
    // level is what times the hold, because the detector's own
    // `inactiveQualified` is false for the whole time a press is held.
    StandaloneArmVerdict update(uint32_t nowMs, const StartTriggerVerdict& trigger) {
        StandaloneArmVerdict verdict;

        if (prepared_) {
            phase_ = StandaloneArmPhase::Prepared;
            verdict.prepared = true;
            verdict.phase = phase_;
            return verdict;
        }

        // The request has been issued and its outcome not yet reported: stay put
        // rather than starting a second hold. A caller that reported the outcome
        // in the same pass (the vehicle layer does) never observes this state.
        if (phase_ == StandaloneArmPhase::Preparing) {
            verdict.phase = phase_;
            return verdict;
        }

        // A refused preparation is retried only after a clean release, so holding
        // the pencil through a refusal cannot produce a retry storm.
        if (awaitingRelease_) {
            if (!trigger.rawActive) {
                awaitingRelease_ = false;
            }
            phase_ = StandaloneArmPhase::WaitingForRelease;
            verdict.phase = phase_;
            return verdict;
        }

        // A trigger that was already held when the board booted never leaves this
        // state, and nothing may be armed from it.
        if (trigger.state == StartTriggerState::WaitingForInactive) {
            holding_ = false;
            phase_ = StandaloneArmPhase::WaitingForRelease;
            verdict.phase = phase_;
            return verdict;
        }

        if (!trigger.rawActive) {
            holding_ = false;
            phase_ = StandaloneArmPhase::WaitingForHold;
            verdict.phase = phase_;
            return verdict;
        }

        if (!holding_) {
            holding_ = true;
            holdStartedMs_ = nowMs;
            phase_ = StandaloneArmPhase::Holding;
        } else if (static_cast<uint32_t>(nowMs - holdStartedMs_) >= holdMs_) {
            // One request per gesture: the gate reports the gesture and waits for
            // the caller's outcome, so a continued hold cannot re-request.
            holding_ = false;
            phase_ = StandaloneArmPhase::Preparing;
            verdict.requestPrepare = true;
        }

        verdict.phase = phase_;
        return verdict;
    }

    StandaloneArmPhase phase() const { return phase_; }
    bool prepared() const { return prepared_; }
    uint32_t holdMs() const { return holdMs_; }

private:
    uint32_t holdMs_;
    StandaloneArmPhase phase_ = StandaloneArmPhase::WaitingForRelease;
    bool prepared_ = false;
    bool holding_ = false;
    bool awaitingRelease_ = false;
    uint32_t holdStartedMs_ = 0U;
};

}  // namespace ev
