#pragma once

#include "Snapshots.h"
#include "Units.h"
#include "VelocityEstimator.h"

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
// * Two stamps do not say whether the control loop was running in between, so
//   the module does not pretend to know: a gap longer than the configured
//   `maxUpdateGapMs` is *not* credited as continued evidence. The interval
//   restarts at the observation that ended the gap, and the verdict reports
//   `UpdateGapExceeded`, which is why the dwell that earns a stop can never have
//   been accumulated across a stall the caller never saw.
//
//   The limit is not a number this module invents. What it bounds is the amount
//   of the stop dwell that could be credited without observation, and the only
//   thing that bounds that on the real board is how long the control loop can
//   stall - an explicit measurement in the H0W bench record
//   (`docs/bringup/P-H0W-watchdog-qualification.md`, the maximum control-loop
//   stall). The limit therefore arrives as a `Sourced` value like the threshold
//   and the dwell: until the stall has been measured, no interval may be
//   credited across a gap, because no gap is known to be short enough.
//
//   A sequence that consumes a verdict still carries its own timeout as well,
//   and a gap longer than that timeout still refuses the sequence. The two are
//   independent: the timeout answers "has this taken too long?", and this limit
//   answers "was the interval I am crediting actually observed?".
//
// What this module is not
//
// * It does not set `SafetySystemState::atZeroSpeed`. It produces a verdict; the
//   vehicle layer maps it onto that field, per the producer contract stated at
//   `SafetySystemState::atZeroSpeed`.
// * No threshold and no dwell is written down here. Both arrive as `Sourced`
//   values, and a placeholder produces a refusal instead of a verdict. Until the
//   stop is measured (`VERIFIED_STATE.md` #5, #9), the honest configuration is
//   `Provisional` and the honest result is a refusal that authorizes nothing.
// * One provenance rule is deliberately wider than "Measured", and it is stated
//   where it is applied (`dwellProvenanceIsEstablished()`): a dwell the official
//   rules *fix* may gate a stop. The run's final stop is the event's Complete
//   Stop (rule 8.p, `ev::rules2027::kCompleteStopS`), which is a rule value and
//   never a measurement - demanding a measurement for it would make a run with
//   an otherwise complete calibration unsequenceable, and relabelling the rule
//   value `Measured` would be fabricating a measurement. A `Provisional` dwell is
//   still refused, and the threshold and the gap limit are still required to be
//   `Measured`: they describe this vehicle, not the event.
// * It has no clock (`nowMs` is an argument), no hardware, no allocation, no
//   filtering and no prediction. It is not a stop command and it brakes nothing.
// * It has no timeout of its own. How long a verification may take is decided by
//   the sequence that needs it - a reversal, a final stop - and that sequence
//   supplies its own bound.

// The policy this verifier applies: how close to zero, for how long. Both values
// are supplied by the caller; neither has a default, because neither has been
// measured.
struct NearZeroConfig {
    constexpr NearZeroConfig(Sourced<double> velocityThresholdMps_, Sourced<double> dwellS_,
                             Sourced<uint32_t> maxUpdateGapMs_)
        : velocityThresholdMps(velocityThresholdMps_),
          dwellS(dwellS_),
          maxUpdateGapMs(maxUpdateGapMs_) {}

    // A measured speed magnitude at or below this counts as near zero, metres per
    // second. A bench candidate value, not a measured property of the vehicle.
    Sourced<double> velocityThresholdMps;

    // How long the near-zero evidence must hold continuously before the stop is
    // verified, seconds. This is the interval over which the stop was observed
    // (SafetyAuthority.h). Either a value measured on this vehicle or - for the
    // run's final stop - the interval the rules fix (rule 8.p); see
    // `dwellProvenanceIsEstablished()`.
    Sourced<double> dwellS;

    // The longest gap between two updates that may still be credited as continued
    // evidence, milliseconds. Supplied by the caller from the measured maximum
    // control-loop stall (H0W); not a default and not a guess.
    Sourced<uint32_t> maxUpdateGapMs;
};

enum class NearZeroConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,     // threshold or gap limit is not ValueSource::Measured
    DwellIsNotEstablished,    // the dwell is neither measured nor a cited rule value
    NonFiniteValue,           // NaN or infinity
    NonPositiveThreshold,     // a zero threshold could never be observed
    NonPositiveDwell,         // a zero dwell would verify on the first observation
    NonPositiveUpdateGap,     // a zero gap limit would refuse every continued interval
};

// True when a dwell may gate a stop: it was measured on this vehicle, or the
// official rules fix it and the citing rule is recorded (`Sourced::citesEvidence()`).
//
// The rule case exists for exactly one value - the run's Complete Stop interval
// (rule 8.p) - and it is a *rule* value: the event says how long the stop is, so
// no experiment on this vehicle can turn it into a measurement, and a vehicle
// with nothing left but that rule value must still be able to complete a run.
// The citation is required rather than assumed, so a `RuleDerived` dwell that
// names no rule is refused exactly like a placeholder - which is what keeps this
// from becoming a way to smuggle an unmeasured number past the gate.
inline bool dwellProvenanceIsEstablished(const Sourced<double>& dwell) {
    if (dwell.isMeasured()) {
        return true;
    }
    return dwell.source == ValueSource::RuleDerived && dwell.citesEvidence();
}

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
    if (!config.velocityThresholdMps.isMeasured() || !config.maxUpdateGapMs.isMeasured()) {
        return {false, NearZeroConfigError::ValuesAreNotMeasured};
    }
    if (!dwellProvenanceIsEstablished(config.dwellS)) {
        return {false, NearZeroConfigError::DwellIsNotEstablished};
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
    // A zero gap limit is refused rather than reinterpreted: it would mean "no
    // interval may ever be credited across any gap", which is not a policy
    // anyone can state from a stall measurement and would silently make every
    // stop unverifiable. A caller that means "the loop is a single instant"
    // states a positive millisecond value.
    if (config.maxUpdateGapMs.value == 0U) {
        return {false, NearZeroConfigError::NonPositiveUpdateGap};
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

// ---------------------------------------------------------------------------
// Composing the packet - the pairing decision, in one production owner
// ---------------------------------------------------------------------------
//
// The three facts above never arrive in one estimator answer. `VelocityEstimator`
// reports either a window-derived number with `zeroSpeedClaimed == false` or the
// explicit zero-speed claim with `valid == false` - never both, and both
// behaviours are pinned by `test_velocity_estimator`. A caller that needs the
// verifier's three facts at once, which is every sequence that has to earn a
// verified stop (the reversal gate and the final stop), must therefore pair the
// current cycle's claim with the last window-derived estimate, under a staleness
// limit of its own.
//
// That pairing is a production decision, not each caller's private arrangement:
// two callers who pair different answers would be verifying two different stops.
// It lives here, next to the verdict it serves, and is pinned by its own tests.

// True when a packet's speed-fact staleness limit can span the stop dwell.
//
// The zero the packet rests on is always produced *before* the claim interval it
// is paired with: the window-derived estimate is the last one that observed
// movement, and the claim only appears after the estimator's own no-edge
// interval. The verdict is earned by the pair holding for the whole dwell, so a
// limit shorter than the dwell cannot span the interval the verdict is earned
// over, and such a packet can never verify a stop. Refusing that configuration is
// the difference between a sequence that fails loudly and one that can only time
// out.
//
// This is a configuration-build check, not a runtime clamp: it does not make a
// stop verifiable, it only refuses a configuration that cannot be. An unusable
// near-zero configuration is refused too, because there is no dwell to span.
//
// The comparison absorbs decimal floating-point representation error only, never
// a real shortfall: 0.2 s is 200.00000000000003 ms in binary, and a 200 ms limit
// is the whole dwell. The tolerance is far below one millisecond, which is the
// resolution of the clock both the dwell and the age are measured in.
inline bool stopEvidenceLimitSpansDwell(uint32_t speedMaxAgeMs, const NearZeroConfig& config) {
    if (!validateNearZeroConfig(config).ok) {
        return false;
    }
    constexpr double kToleranceMs = 1e-6;
    return static_cast<double>(speedMaxAgeMs) + kToleranceMs >= config.dwellS.value * 1000.0;
}

// Composes the verifier's evidence packet from the two production answers a
// caller has, so no sequence rebuilds the composition by hand.
//
// `currentEstimate` is this cycle's estimator answer: its explicit zero-speed
// claim is the claim fact, accepted only within the control path's own freshness
// limit (`claimMaxAgeMs`). A claim older than the control path's staleness limit
// is not describing this cycle, and pairing it anyway would verify a stop from a
// fact the control path has already stopped acting on. An absent current answer
// claims nothing.
//
// `lastValidEstimate` is the most recent window-derived estimate (the caller
// keeps it whenever `VelocityEstimate::valid` is true), and its measured speed is
// the speed fact. It is aged against `speedMaxAgeMs`, the packet's own, longer
// limit: the speed fact was produced before the claim interval it is paired with,
// so it needs a limit that spans the dwell (`stopEvidenceLimitSpansDwell()`).
// Without a retained estimate the packet carries no usable speed at all.
//
// Nothing is invented here: both facts are production outputs, both limits are
// caller policy, and a fact that does not hold is reported false rather than
// replaced by a plausible value.
inline NearZeroEvidence composeStopEvidence(const VelocityEstimate& currentEstimate,
                                            bool hasCurrentEstimate,
                                            const VelocityEstimate& lastValidEstimate,
                                            bool hasLastValidEstimate, uint32_t nowMs,
                                            uint32_t claimMaxAgeMs, uint32_t speedMaxAgeMs) {
    NearZeroEvidence evidence;
    evidence.velocityUsable =
        velocityReadiness(hasLastValidEstimate, lastValidEstimate,
                          sampleAge(nowMs, lastValidEstimate.elapsedMs, speedMaxAgeMs))
            .usable;
    evidence.velocityMps = lastValidEstimate.velocityMps;
    const SampleAge claimAge = sampleAge(nowMs, currentEstimate.elapsedMs, claimMaxAgeMs);
    evidence.zeroSpeedClaimed =
        hasCurrentEstimate && claimAge.fresh && currentEstimate.zeroSpeedClaimed;
    return evidence;
}

// Why the verdict is what it is.
enum class NearZeroStatus : uint8_t {
    // Near-zero evidence held continuously for the configured dwell.
    Verified = 0,
    // Near-zero evidence holds now; the dwell interval is still accumulating.
    DwellInProgress = 1,
    // The evidence does not show a near zero (or shows nothing at all).
    AwaitingEvidence = 2,
    // Threshold/dwell/gap limit are not Measured or are malformed: no verdict is
    // possible.
    ConfigIsNotUsable = 3,
    // The stamp did not advance (or was rewound): the dwell is held, not grown.
    TimeDidNotAdvance = 4,
    // The gap since the previous update was longer than the configured limit, so
    // the interval was not credited: the evidence holds now, but the observed
    // dwell restarts here. The control loop was not seen to run across the gap.
    UpdateGapExceeded = 5,
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

    // True while one continuous near-zero interval is open: the evidence held on
    // the last update, whether the interval is still accumulating, being held by
    // a frozen stamp, or has already earned the verdict. False after a reset, and
    // false on any update whose evidence did not hold - so a consumer that wants
    // the interval's start never has to reconstruct "is there one" from a
    // status it might classify wrongly.
    bool intervalOpen() const { return hasNearZeroSince_; }

    // The clock reading the open interval started at; 0 when none is open.
    // Diagnostic, like `dwellElapsedMs()`: a restart is a new reading here, and
    // the interval that satisfies the rule is the one whose start this is. It is
    // reported rather than left to the caller to recover from the elapsed span,
    // because the start is the fact a run's own timeline needs and the elapsed
    // span exists only for the verdict.
    uint32_t intervalStartedAtMs() const { return nearZeroSinceMs_; }

    // One verification step.
    //
    // The interval is credited only across the updates the caller actually
    // makes: each update that still shows the evidence extends it, and any
    // update whose evidence does not hold clears it. A stamp that does not
    // advance holds the dwell instead of growing or clearing it, which is what a
    // frozen clock should do; the sequence that owns the timeout still sees time
    // standing still and starts nothing new.
    //
    // A gap longer than `maxUpdateGapMs` is NOT credited as continued evidence
    // (see the module header): the interval restarts at this observation and the
    // verdict reports `UpdateGapExceeded`. Whether the loop ran across the gap is
    // not answerable from two stamps, so a gap the caller cannot vouch for does
    // not grow a dwell that earns a stop.
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

        // The evidence holds now, but not across an interval longer than the
        // configured gap limit. The interval restarts here rather than being
        // credited: the observation is real, the continuity is not observed.
        if (static_cast<uint32_t>(sinceLastMs) > config_.maxUpdateGapMs.value) {
            nearZeroSinceMs_ = nowMs;
            dwellElapsedMs_ = 0U;
            lastStampMs_ = nowMs;
            return NearZeroVerdict(false, NearZeroStatus::UpdateGapExceeded, true, true, true, 0U);
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
