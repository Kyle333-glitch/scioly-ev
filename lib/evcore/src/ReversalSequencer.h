#pragma once

#include "Motion.h"
#include "NearZeroVerifier.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Controlled reversal sequencing - motion sequencing, not run policy
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The sequence that earns a powered direction change, and nothing else:
//
//       request a reversal in the other direction
//           -> RampingDown          the current direction is brought to zero
//           -> Braking              near zero observed; the deliberate stop holds
//           -> VerifyingNearZero    the stop is held while the dwell accumulates
//           -> DirectionChangeReady the change has been earned and is latched
//
//   and, whenever the near zero is not reached in time or the evidence is
//   unusable, a deterministic refusal instead:
//
//           -> StopRequired         hold the stop; do not reverse
//
// * It answers one yes/no question for the run layer: may the travel direction
//   change yet? `DirectionChangeReady` is that yes, and once earned it is
//   latched, because a verified stop does not stop being verified.
// * The near-zero test is not invented here. It is `NearZeroVerifier`
//   (threshold + dwell over fresh, valid speed evidence and the estimator's
//   explicit zero-speed claim), configured from the same `Sourced` values.
// * The whole sequence is bounded by `reversalTimeoutS`. That single bound
//   covers the ramp-down and the verification together: a vehicle that never
//   comes near zero, or whose speed evidence stays stale, ends at
//   `StopRequired` instead of silently reversing.
//
// What this module is not
//
// * It commands nothing. It has no effort, no PWM, no driver mode and no
//   hardware of any kind; it reports a phase and the run layer decides what to
//   ask the motion chain for. It never brakes and never drives.
// * It is not the run state machine. It knows no competition phase, no bottle,
//   no distance and no scoring, and it has no opinion about *why* the run wants
//   the other direction. An approach leg, a recovery move and a diagnostic both
//   sequence the same way.
// * It is not the safety boundary and does not replace one. Its permission says
//   "the stop was verified"; whether a powered command may then be issued is
//   `SafetyAuthority`'s decision, and the authority refuses a direct flip
//   independently - including when this module has earned the change, if the
//   caller never presented the stop that clears the authority's direction latch.
// * It does not ramp anything itself. The commanded velocity ramp (target to
//   zero, no sign flip inside one step) is `MotionProfile`'s; this module states
//   *when* the ramp is complete enough to brake, and *when* the direction change
//   is allowed.
// * It owns no threshold, dwell or timeout. All three arrive as `Sourced` values
//   and none has been measured (`VERIFIED_STATE.md` #9); a configuration that is
//   not `ValueSource::Measured` refuses the reversal instead of guessing.

// The policy of one reversal. Both values are supplied by the caller.
struct ReversalConfig {
    constexpr ReversalConfig(NearZeroConfig verification_, Sourced<double> reversalTimeoutS_)
        : verification(verification_), reversalTimeoutS(reversalTimeoutS_) {}

    // How a stop is recognized (threshold and dwell).
    NearZeroConfig verification;

    // The longest the whole sequence may take, seconds: ramp-down, brake command
    // and verification together. An unverified near zero that outlives this
    // bound is a refusal, not a longer wait.
    Sourced<double> reversalTimeoutS;
};

enum class ReversalConfigError : uint8_t {
    None = 0,
    VerificationNotUsable,   // the NearZeroConfig failed its own validation
    TimeoutNotMeasured,      // reversalTimeoutS is not ValueSource::Measured
    NonFiniteTimeout,        // NaN or infinity
    NonPositiveTimeout,      // a zero timeout would refuse every reversal
};

struct ReversalConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    ReversalConfigError error;
};

// Single entry point for this module's configuration validation. The near-zero
// configuration is validated by `validateNearZeroConfig()` rather than
// re-checked here, so the threshold/dwell policy has exactly one definition.
inline ReversalConfigValidation validateReversalConfig(const ReversalConfig& config) {
    if (!validateNearZeroConfig(config.verification).ok) {
        return {false, ReversalConfigError::VerificationNotUsable};
    }
    if (!config.reversalTimeoutS.isMeasured()) {
        return {false, ReversalConfigError::TimeoutNotMeasured};
    }
    if (!std::isfinite(config.reversalTimeoutS.value)) {
        return {false, ReversalConfigError::NonFiniteTimeout};
    }
    if (!(config.reversalTimeoutS.value > 0.0)) {
        return {false, ReversalConfigError::NonPositiveTimeout};
    }
    return {true, ReversalConfigError::None};
}

// The sequence phase. Idle is the zero value, so a default-built sequencer is
// inert and permits nothing.
enum class ReversalPhase : uint8_t {
    // No reversal is being sequenced. Nothing is promised and nothing is held.
    Idle = 0,
    // The current direction is being brought to zero; no direction change is
    // allowed yet.
    RampingDown = 1,
    // The measured speed is within the near-zero threshold but the estimator has
    // not yet declared that the count stopped changing: the deliberate stop is
    // what the run layer presents now.
    Braking = 2,
    // The stop is observed and being held; the dwell interval is accumulating.
    VerifyingNearZero = 3,
    // The direction change has been earned. Latched until reset().
    DirectionChangeReady = 4,
    // The reversal will not happen. The run layer must hold a stop and raise the
    // fault named by `refusal`.
    StopRequired = 5,
};

// Why the reversal stopped. Non-None exactly when the phase is StopRequired.
enum class ReversalRefusal : uint8_t {
    None = 0,
    // The verification/dwell/timeout configuration is not usable. A reversal may
    // never rest on a placeholder, so it is refused rather than attempted.
    ConfigIsNotUsable,
    // The near zero was not verified within the configured reversal timeout.
    TimedOut,
};

// One step's answer. Deliberately no default constructor: a phase with no facts
// behind it must be impossible to write.
struct ReversalStatus {
    constexpr ReversalStatus(ReversalPhase phase_, ReversalRefusal refusal_, bool stopVerified_,
                             NearZeroVerdict verification_, uint32_t sequenceElapsedMs_)
        : phase(phase_),
          refusal(refusal_),
          stopVerified(stopVerified_),
          verification(verification_),
          sequenceElapsedMs(sequenceElapsedMs_) {}

    ReversalPhase phase;

    // Why a stop is required. Non-None exactly when phase == StopRequired.
    ReversalRefusal refusal;

    // True once the near-zero stop has been verified. Latched: a verified stop
    // does not stop being verified, and the permission must not flicker under a
    // caller that reads it one cycle later.
    bool stopVerified;

    // The facts behind the latest verification step (which of the three held).
    // Reported so a refused reversal can be attributed to the missing fact.
    NearZeroVerdict verification;

    // Time since this sequence began, milliseconds. 0 while `Idle`, and 0 when no
    // sequence ever began. A refusal latches the time it was raised, so a status
    // read after the fact still reports how long the sequence was given before it
    // was refused.
    uint32_t sequenceElapsedMs;
};

// Whether a reversal actually started, and why not when it did not. A caller
// that ignores the return value still sees the refusal in the phase.
enum class ReversalBeginStatus : uint8_t {
    Started = 0,
    // The requested direction is the current one: there is nothing to sequence.
    NotNeeded,
    // A sequence is already running. It is never restarted, because restarting
    // would restart the timeout and a vehicle that keeps stalling could wait
    // forever.
    AlreadyActive,
    // A refusal is already of record. reset() is required before a new sequence.
    StopLatched,
    // The configuration is not usable; the refusal is now of record.
    ConfigIsNotUsable,
};

// ---------------------------------------------------------------------------
// ReversalSequencer
// ---------------------------------------------------------------------------
//
// `nowMs` is the caller's `millis()` reading on every call; the class never
// reads a clock. It holds one sequence's state and nothing else: no run
// knowledge, no hardware, no allocation.
class ReversalSequencer {
public:
    explicit ReversalSequencer(const ReversalConfig& config)
        : config_(config), verifier_(config.verification), verification_(noObservation()) {}

    const ReversalConfig& config() const { return config_; }

    bool configUsable() const { return validateReversalConfig(config_).ok; }

    ReversalPhase phase() const { return phase_; }

    // True only in DirectionChangeReady: the single permission this module
    // grants. It states that a stop was verified, not that the vehicle is
    // stopped now, so a consumer that has commanded powered travel since earning
    // it must have reset the sequencer before trusting it again. The run layer
    // does exactly that at the handoff, which is the only place the permission is
    // consumed.
    bool directionChangePermitted() const { return phase_ == ReversalPhase::DirectionChangeReady; }

    // True once the near-zero stop has been verified (latched).
    bool stopVerified() const { return stopVerified_; }

    // Returns the sequencer to its post-construction state: no sequence, no
    // permission, no refusal, no dwell. This is the only way out of
    // StopRequired, and it is what a run boundary calls; nothing resumes
    // automatically and no previously earned permission survives.
    void reset() {
        phase_ = ReversalPhase::Idle;
        refusal_ = ReversalRefusal::None;
        stopVerified_ = false;
        verifier_.reset();
        verification_ = noObservation();
        startedAtMs_ = 0U;
        refusedAtElapsedMs_ = 0U;
    }

    // Attempts to begin a reversal sequence that changes `currentDirection` into
    // `requestedDirection`.
    //
    // A reversal is refused outright, before any time passes, when the
    // configuration is not usable: a threshold or dwell that is still a
    // placeholder can never authorize a direction change, so the refusal is
    // immediate, deterministic and latched. That check comes first, so with an
    // unusable configuration every later attempt keeps reporting the
    // configuration rather than being masked by the latched stop.
    ReversalBeginStatus beginReversal(MotionDirection currentDirection,
                                      MotionDirection requestedDirection, uint32_t nowMs) {
        if (!configUsable()) {
            phase_ = ReversalPhase::StopRequired;
            refusal_ = ReversalRefusal::ConfigIsNotUsable;
            stopVerified_ = false;
            // No sequence began, so there is no elapsed time to report.
            refusedAtElapsedMs_ = 0U;
            return ReversalBeginStatus::ConfigIsNotUsable;
        }
        if (phase_ == ReversalPhase::StopRequired) {
            return ReversalBeginStatus::StopLatched;
        }
        if (currentDirection == requestedDirection) {
            return ReversalBeginStatus::NotNeeded;
        }
        if (phase_ != ReversalPhase::Idle) {
            return ReversalBeginStatus::AlreadyActive;
        }

        phase_ = ReversalPhase::RampingDown;
        refusal_ = ReversalRefusal::None;
        stopVerified_ = false;
        verifier_.reset();
        verification_ = noObservation();
        startedAtMs_ = nowMs;
        return ReversalBeginStatus::Started;
    }

    // One sequencing step. The evidence packet is the same one the near-zero
    // verifier consumes: a fresh, valid speed estimate, that speed, and the
    // estimator's explicit zero-speed claim.
    //
    // The timeout is evaluated before the verification step, deliberately: a
    // control loop that stalled for longer than the configured bound refuses the
    // reversal on the next update instead of crediting the missing time to a
    // dwell it did not observe. The bound is the sequence's whole budget, so it
    // is also what decides how long an unobserved pause may be.
    ReversalStatus update(uint32_t nowMs, const NearZeroEvidence& evidence) {
        if (phase_ == ReversalPhase::Idle) {
            return status(0U);
        }
        if (phase_ == ReversalPhase::StopRequired) {
            return status(refusedAtElapsedMs_);
        }

        const uint32_t elapsedMs = static_cast<uint32_t>(nowMs - startedAtMs_);
        if (phase_ != ReversalPhase::DirectionChangeReady &&
            elapsedMs >= msFromSeconds(config_.reversalTimeoutS.value)) {
            phase_ = ReversalPhase::StopRequired;
            refusal_ = ReversalRefusal::TimedOut;
            refusedAtElapsedMs_ = elapsedMs;
            return status(elapsedMs);
        }
        if (phase_ == ReversalPhase::DirectionChangeReady) {
            // The permission is latched; the verifier is no longer stepped, so a
            // later measurement cannot un-earn a verified stop.
            return status(elapsedMs);
        }

        verification_ = verifier_.update(nowMs, evidence);
        if (verification_.verified) {
            stopVerified_ = true;
            phase_ = ReversalPhase::DirectionChangeReady;
        } else if (verification_.status == NearZeroStatus::DwellInProgress) {
            phase_ = ReversalPhase::VerifyingNearZero;
        } else if (verification_.withinThreshold) {
            // Near zero, but the estimator has not declared that the count
            // stopped changing. That is the brake-and-wait part of the sequence.
            phase_ = ReversalPhase::Braking;
        } else {
            // Still moving, or no usable evidence at all. Either way no
            // direction change may be promised; the timeout above bounds how
            // long this can last.
            phase_ = ReversalPhase::RampingDown;
        }
        return status(elapsedMs);
    }

private:
    // What a sequencer that has never stepped reports: nothing observed.
    static NearZeroVerdict noObservation() {
        return NearZeroVerdict(false, NearZeroStatus::AwaitingEvidence, false, false, false, 0U);
    }

    ReversalStatus status(uint32_t sequenceElapsedMs) const {
        return ReversalStatus(phase_, refusal_, stopVerified_, verification_, sequenceElapsedMs);
    }

    ReversalConfig config_;
    NearZeroVerifier verifier_;
    ReversalPhase phase_ = ReversalPhase::Idle;
    ReversalRefusal refusal_ = ReversalRefusal::None;
    bool stopVerified_ = false;
    NearZeroVerdict verification_;
    uint32_t startedAtMs_ = 0U;

    // The elapsed time latched when the refusal was raised, so a refused
    // sequence keeps reporting how long it was given instead of collapsing to 0.
    // Diagnostic only; the refusal itself is `refusal_`.
    uint32_t refusedAtElapsedMs_ = 0U;
};

// Pin the safe zero values: an inert sequencer must be the default state, and a
// permission must never be the zero value of its own type.
static_assert(static_cast<int>(ReversalPhase::Idle) == 0,
              "the idle phase must stay the zero value");
static_assert(static_cast<int>(ReversalRefusal::None) == 0,
              "no refusal must stay the zero value");
static_assert(static_cast<int>(ReversalBeginStatus::Started) == 0,
              "a started reversal must stay the zero value");
static_assert(!std::is_default_constructible<ReversalStatus>::value,
              "a reversal status must never default-construct");
static_assert(!std::is_default_constructible<NearZeroVerdict>::value,
              "a verification verdict must never default-construct");

}  // namespace ev
