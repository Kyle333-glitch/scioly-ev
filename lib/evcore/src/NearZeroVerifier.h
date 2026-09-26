#pragma once

#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Near-zero verification - is the vehicle actually stopped?
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One question, answered from evidence the vehicle layer already has: has the
//   vehicle been within a configured near-zero speed threshold, continuously,
//   for a configured dwell interval? That is all it decides.
// * The evidence it combines is deliberately three independent facts, because
//   none of them is sufficient alone and the safety contract says so
//   (SafetyAuthority.h, `atZeroSpeed`):
//     - a fresh, valid velocity estimate (`velocityReadiness().usable`), so a
//       last-valid number reused across cycles cannot earn a stop;
//     - that measured speed within the configured threshold;
//     - the estimator's explicit zero-speed claim, i.e. observed encoder
//       non-change for the estimator's own no-edge interval. A window can read
//       below the threshold while the encoder is still ticking slowly, so the
//       claim is what rules out a creep.
//   The verdict reports which of those three facts held, so a log can attribute
//   a wait to the fact that is missing instead of guessing at it.
// * The dwell must be continuous *across the updates the caller makes*. Any
//   update whose evidence does not hold clears the interval, so a vehicle
//   oscillating around the threshold never verifies a stop and can never earn a
//   direction change through this module.
// * What it cannot observe is the time between two updates: a gap between two
//   advancing stamps is credited as if the evidence had continued across it.
//   Two stamps do not say whether the control loop was running, and this module
//   does not pretend to know - a stalled loop is a command-timeout question with
//   its own owner (AGENTS.md section 5), and `NearZeroEvidence::velocityUsable`
//   is the caller's assertion that the evidence handed in is fresh
//   (`velocityReadiness()`), which this module deliberately does not re-derive.
//   Every sequence that consumes a verdict also carries a timeout that is
//   evaluated before the verification step, so a stalled loop during a reversal
//   refuses the reversal rather than quietly earning it.
//
// What this module is not
//
// * It does not set `SafetySystemState::atZeroSpeed`. It produces a verdict; the
//   vehicle layer maps it onto that field, per the producer contract stated at
//   `SafetySystemState::atZeroSpeed`.
// * No threshold and no dwell is written down here. Both arrive as `Sourced`
//   values, and a value that is not `ValueSource::Measured` produces a refusal
//   instead of a verdict. Until the stop is measured (`VERIFIED_STATE.md` #5,
//   #9), the honest configuration is `Provisional` and the honest result is a
//   refusal that authorizes nothing.
// * It has no clock (`nowMs` is an argument), no hardware, no allocation, no
//   filtering and no prediction. It is not a stop command and it brakes nothing.
// * It has no timeout of its own. How long a verification may take is decided by
//   the sequence that needs it - a reversal, a final stop - and that sequence
//   supplies its own bound.

// The policy this verifier applies: how close to zero, for how long. Both values
// are supplied by the caller; neither has a default, because neither has been
// measured.
struct NearZeroConfig {
    constexpr NearZeroConfig(Sourced<double> velocityThresholdMps_, Sourced<double> dwellS_)
        : velocityThresholdMps(velocityThresholdMps_), dwellS(dwellS_) {}

    // A measured speed magnitude at or below this counts as near zero, metres per
    // second. A bench candidate value, not a measured property of the vehicle.
    Sourced<double> velocityThresholdMps;

    // How long the near-zero evidence must hold continuously before the stop is
    // verified, seconds. This is the interval over which the stop was observed
    // (SafetyAuthority.h); a bench candidate value, not a measured stopping
    // behavior.
    Sourced<double> dwellS;
};

enum class NearZeroConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,     // threshold or dwell is not ValueSource::Measured
    NonFiniteValue,           // NaN or infinity
    NonPositiveThreshold,     // a zero threshold could never be observed
    NonPositiveDwell,         // a zero dwell would verify on the first observation
};

struct NearZeroConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    NearZeroConfigError error;
};

// Single entry point for this module's configuration validation.
//
// A non-positive threshold or dwell is refused rather than reinterpreted: a zero
// threshold would mean "exactly zero, always" and would never verify on a real
// measurement, and a zero dwell would verify a stop on the first near-zero
// observation, which is not an observed interval at all.
inline NearZeroConfigValidation validateNearZeroConfig(const NearZeroConfig& config) {
    if (!config.velocityThresholdMps.isMeasured() || !config.dwellS.isMeasured()) {
        return {false, NearZeroConfigError::ValuesAreNotMeasured};
    }
    if (!std::isfinite(config.velocityThresholdMps.value) || !std::isfinite(config.dwellS.value)) {
        return {false, NearZeroConfigError::NonFiniteValue};
    }
    if (!(config.velocityThresholdMps.value > 0.0)) {
        return {false, NearZeroConfigError::NonPositiveThreshold};
    }
    if (!(config.dwellS.value > 0.0)) {
        return {false, NearZeroConfigError::NonPositiveDwell};
    }
    return {true, NearZeroConfigError::None};
}

// The three facts one verification step rests on. Defaults are the unusable
// ones, so a default-built evidence packet observes nothing.
struct NearZeroEvidence {
    // The freshness verdict for the speed below (`velocityReadiness().usable`).
    // False means there is no current measurement: no verdict may rest on it.
    bool velocityUsable = false;

    // The measured speed the verdict describes, metres per second, in the
    // estimator's own sign convention. Ignored when velocityUsable is false.
    double velocityMps = 0.0;

    // The estimator's explicit zero-speed claim (VelocityEstimate::
    // zeroSpeedClaimed): no encoder change observed for its no-edge interval.
    bool zeroSpeedClaimed = false;
};

// Why the verdict is what it is.
enum class NearZeroStatus : uint8_t {
    // Near-zero evidence held continuously for the configured dwell.
    Verified = 0,
    // Near-zero evidence holds now; the dwell interval is still accumulating.
    DwellInProgress = 1,
    // The evidence does not show a near zero (or shows nothing at all).
    AwaitingEvidence = 2,
    // Threshold/dwell are not Measured or are malformed: no verdict is possible.
    ConfigIsNotUsable = 3,
    // The stamp did not advance (or was rewound): the dwell is held, not grown.
    TimeDidNotAdvance = 4,
};

// One verification answer. Deliberately no default constructor: a verdict with
// no evidence behind it must be impossible to write.
struct NearZeroVerdict {
    constexpr NearZeroVerdict(bool verified_, NearZeroStatus status_, bool velocityUsable_,
                              bool withinThreshold_, bool zeroSpeedClaimed_, uint32_t dwellElapsedMs_)
        : verified(verified_),
          status(status_),
          velocityUsable(velocityUsable_),
          withinThreshold(withinThreshold_),
          zeroSpeedClaimed(zeroSpeedClaimed_),
          dwellElapsedMs(dwellElapsedMs_) {}

    // Invariant, pinned by every call: verified == (status == Verified).
    bool verified;

    NearZeroStatus status;

    // The facts this verdict used, reported so a wait can be attributed.
    bool velocityUsable;
    bool withinThreshold;
    bool zeroSpeedClaimed;

    // The continuous near-zero interval observed so far, milliseconds (an
    // elapsed span measured from clock readings). 0 when the evidence does not
    // currently hold or a fresh interval just started.
    uint32_t dwellElapsedMs;
};

// ---------------------------------------------------------------------------
// NearZeroVerifier
// ---------------------------------------------------------------------------
//
// The dwell is the only state it holds. `nowMs` is the caller's `millis()`
// reading; the class never reads a clock, so it stays deterministic and
// host-testable.
class NearZeroVerifier {
public:
    explicit NearZeroVerifier(const NearZeroConfig& config) : config_(config) {}

    const NearZeroConfig& config() const { return config_; }

    bool configUsable() const { return validateNearZeroConfig(config_).ok; }

    // Returns to its post-construction state: no observation, no dwell. Used by
    // a run boundary; nothing resumes automatically.
    void reset() {
        hasNearZeroSince_ = false;
        nearZeroSinceMs_ = 0U;
        lastStampMs_ = 0U;
        dwellElapsedMs_ = 0U;
    }

    // The continuous near-zero interval observed so far, milliseconds.
    // Diagnostic only; the verdict is the output.
    uint32_t dwellElapsedMs() const { return dwellElapsedMs_; }

    // One verification step.
    //
    // The interval is credited only across the updates the caller actually
    // makes: each update that still shows the evidence extends it, and any
    // update whose evidence does not hold clears it. A stamp that does not
    // advance holds the dwell instead of growing or clearing it, which is what a
    // frozen clock should do; the sequence that owns the timeout still sees time
    // standing still and starts nothing new.
    //
    // A gap between two advancing updates is credited as continued evidence (see
    // the module header): whether the loop ran in between is not answerable from
    // two stamps, so the caller that needs "and it stayed stopped while I
    // watched" owns the watching - the reversal sequence does exactly that, by
    // evaluating its own timeout before this step.
    NearZeroVerdict update(uint32_t nowMs, const NearZeroEvidence& evidence) {
        if (!configUsable()) {
            return NearZeroVerdict(false, NearZeroStatus::ConfigIsNotUsable, evidence.velocityUsable,
                                   false, evidence.zeroSpeedClaimed, dwellElapsedMs_);
        }

        const bool withinThreshold =
            evidence.velocityUsable && std::isfinite(evidence.velocityMps) &&
            std::fabs(evidence.velocityMps) <= config_.velocityThresholdMps.value;
        const bool nearZero = withinThreshold && evidence.zeroSpeedClaimed;

        if (!nearZero) {
            hasNearZeroSince_ = false;
            nearZeroSinceMs_ = 0U;
            dwellElapsedMs_ = 0U;
            lastStampMs_ = nowMs;
            return NearZeroVerdict(false, NearZeroStatus::AwaitingEvidence, evidence.velocityUsable,
                                   withinThreshold, evidence.zeroSpeedClaimed, 0U);
        }

        if (!hasNearZeroSince_) {
            // First observation of this interval. The dwell starts here, so it
            // cannot be earned by an observation that was never made.
            hasNearZeroSince_ = true;
            nearZeroSinceMs_ = nowMs;
            dwellElapsedMs_ = 0U;
            lastStampMs_ = nowMs;
            return NearZeroVerdict(false, NearZeroStatus::DwellInProgress, true, true, true, 0U);
        }

        // Signed difference, so a repeated stamp and a rewound clock are both
        // detected rather than turned into an enormous dwell.
        const int32_t sinceLastMs = static_cast<int32_t>(nowMs - lastStampMs_);
        if (sinceLastMs <= 0) {
            return NearZeroVerdict(false, NearZeroStatus::TimeDidNotAdvance, true, true, true,
                                   dwellElapsedMs_);
        }

        lastStampMs_ = nowMs;
        // Unsigned subtraction stays correct across uint32 wraparound, and the
        // signed guard above keeps this a real elapsed span.
        dwellElapsedMs_ = static_cast<uint32_t>(nowMs - nearZeroSinceMs_);

        // A configured dwell longer than the clock can represent simply never
        // verifies; it is not shortened to something reachable.
        const double dwellMs = config_.dwellS.value * 1000.0;
        const bool verified = static_cast<double>(dwellElapsedMs_) >= dwellMs;
        if (verified) {
            return NearZeroVerdict(true, NearZeroStatus::Verified, true, true, true, dwellElapsedMs_);
        }
        return NearZeroVerdict(false, NearZeroStatus::DwellInProgress, true, true, true,
                               dwellElapsedMs_);
    }

private:
    NearZeroConfig config_;
    bool hasNearZeroSince_ = false;
    uint32_t nearZeroSinceMs_ = 0U;
    uint32_t lastStampMs_ = 0U;
    uint32_t dwellElapsedMs_ = 0U;
};

// Pin the "no silent answer" contract and the permissive zero values.
static_assert(!std::is_default_constructible<NearZeroVerdict>::value,
              "a verification verdict must never default-construct");
static_assert(static_cast<int>(NearZeroStatus::Verified) == 0,
              "the verified status must stay the zero value");

}  // namespace ev
