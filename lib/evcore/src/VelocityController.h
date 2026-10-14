#pragma once

#include "Motion.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Candidate feed-forward + PI velocity control - effort request only
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One velocity regulator producing one number: a `MotorEffort` request, in
//   permille, for the commanded direction. It adds a feed-forward term from the
//   target velocity, a proportional term from the velocity error and an
//   integral term accumulated over measured elapsed time.
// * Elapsed time is always the time actually reported by the caller. Nothing
//   assumes a 20 ms or any other loop period: the callers across a run may be
//   unevenly spaced, and a stall is visible as a large dt rather than silently
//   treated as one nominal period.
// * Deterministic for every input it refuses. An invalid target, a corrupt
//   measurement, a non-advancing clock, a disabled controller and an unusable
//   configuration each produce a stated status and a non-powered output; none
//   of them produces a driving request.
// * A deliberate, one-sided effort limit: while travelling, the regulator only
//   asks for powered effort in the commanded direction. An overspeed is
//   corrected by reducing effort toward zero, never by demanding powered effort
//   the other way. A commanded reversal is a deliberate decelerate -> brake ->
//   settle sequence owned by the run state machine (AGENTS.md section 15), not
//   something a velocity loop does on its own.
//
// What this module is not
//
// * It authorizes nothing. Its output is a request: only `SafetyAuthority` can
//   turn a request into an `AuthorizedMotorCommand`, and nothing here can
//   construct one.
// * It touches no pin, no driver and no Arduino type, and it does not stop,
//   brake, coast or disable the vehicle. Those are run-state and fault
//   decisions. Its own disabled output is a refusal to drive, not a stop.
// * It owns no gains, no effort ceiling and no direction polarity. Every one of
//   those arrives in the configuration as a `Sourced<T>` value, and a value that
//   is not `ValueSource::Measured` produces a non-powered output instead of a
//   drive. None has been measured yet, so no tuning value is written down here.
// * It has no derivative term, no gain scheduling, no model of the motor,
//   drivetrain or friction, and no anti-lock, stall or slip logic.
// * It has no wrong-way guard. If the measured velocity opposes the target, the
//   error is simply large and the regulator asks for full effort in the
//   commanded direction, bounded by its ceiling - it does not detect, report or
//   refuse that condition, because deciding what a stalled, slipping or
//   backwards-moving drivetrain means belongs to the run state machine and the
//   fault path, not to a velocity loop. The tests pin this behavior so it stays
//   deliberate rather than accidental.
// * It does not decide which sign is physical forward travel. Positive command
//   signs are consistently the sign convention of the measurement fed to it;
//   the mapping from that sign to a powered direction is the configurable,
//   UNVERIFIED polarity fact (`VERIFIED_STATE.md` #4), and no plausible default
//   is invented for it.

// Regulator configuration. Every field is supplied by the caller; there are no
// defaults, because none of these has been measured.
struct VelocityControllerConfig {
    constexpr VelocityControllerConfig(Sourced<double> feedForwardPermillePerMps_,
                                       Sourced<double> proportionalPermillePerMps_,
                                       Sourced<double> integralPermillePerMpsSecond_,
                                       Sourced<MotorMode> positiveEffortMode_,
                                       Sourced<uint16_t> maxEffortPermille_,
                                       uint16_t maxIntegralPermille_,
                                       double maxIntegrationStepS_)
        : feedForwardPermillePerMps(feedForwardPermillePerMps_),
          proportionalPermillePerMps(proportionalPermillePerMps_),
          integralPermillePerMpsSecond(integralPermillePerMpsSecond_),
          positiveEffortMode(positiveEffortMode_),
          maxEffortPermille(maxEffortPermille_),
          maxIntegralPermille(maxIntegralPermille_),
          maxIntegrationStepS(maxIntegrationStepS_) {}

    // Effort permille per metre per second of target velocity. The simplest
    // linear feed-forward: no friction, voltage or motor model.
    Sourced<double> feedForwardPermillePerMps;

    // Effort permille per metre per second of velocity error.
    Sourced<double> proportionalPermillePerMps;

    // Effort permille per (metre per second x second) of accumulated error. The
    // integral term added per step is gain * error * dtSeconds.
    Sourced<double> integralPermillePerMpsSecond;

    // The powered direction a positive effort request means. This is the sign
    // polarity between the command convention and the motor driver, and it is
    // UNVERIFIED (`VERIFIED_STATE.md` #4): a provisional value here stops the
    // regulator from driving rather than guessing a direction.
    Sourced<MotorMode> positiveEffortMode;

    // Ceiling this regulator may request, permille of full scale (0-1000). The
    // caller derives it from the measured `SafetyLimits::maxEffortPermille`, and
    // the two must be numerically equal before the vehicle may be armed;
    // `ev::effortCeilingsAgree()` (SafetyLimits.h) is that check.
    //
    // Anti-windup and the `outputSaturated` report below are computed against
    // THIS ceiling only. A ceiling above the safety ceiling would let this loop
    // integrate headroom it does not have while SafetyAuthority clamped
    // silently downstream, splitting one saturation across two logs; a ceiling
    // below it silently loses requestable effort. SafetyAuthority still clamps
    // independently as defence in depth.
    Sourced<uint16_t> maxEffortPermille;

    // Ceiling on the integral term alone, permille. 0 disables integral action
    // entirely, which is a deliberate way to run feed-forward + proportional
    // only. This is an anti-windup bound, not a measured vehicle property.
    uint16_t maxIntegralPermille;

    // Longest integration step honored, seconds. A control loop that stalled for
    // longer than this integrates only this much, so one late cycle cannot wind
    // the integral up by a whole stall. This is a loop policy, not a measured
    // vehicle property.
    double maxIntegrationStepS;
};

enum class VelocityControllerConfigError : uint8_t {
    None = 0,
    ValuesAreNotMeasured,        // a gain, ceiling or polarity is not Measured
    NonFiniteValue,              // NaN or infinity in a gain or step limit
    NegativeGain,                // a gain below zero would invert the loop
    EffortCeilingOutOfRange,     // maxEffortPermille above full scale
    IntegralCeilingOutOfRange,   // maxIntegralPermille above full scale
    PositiveEffortModeIsNotDriving,  // polarity is not DriveForward/DriveReverse
    IntegrationStepNotPositive,  // maxIntegrationStepS not > 0
};

struct VelocityControllerConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    VelocityControllerConfigError error;
};

// Single entry point for configuration validation. Checks provenance first,
// then finiteness, then the sign, then the ranges, so the most fundamental
// problem is reported first.
inline VelocityControllerConfigValidation validateVelocityControllerConfig(
    const VelocityControllerConfig& config) {
    if (!config.feedForwardPermillePerMps.isMeasured() ||
        !config.proportionalPermillePerMps.isMeasured() ||
        !config.integralPermillePerMpsSecond.isMeasured() ||
        !config.positiveEffortMode.isMeasured() || !config.maxEffortPermille.isMeasured()) {
        return {false, VelocityControllerConfigError::ValuesAreNotMeasured};
    }
    if (!std::isfinite(config.feedForwardPermillePerMps.value) ||
        !std::isfinite(config.proportionalPermillePerMps.value) ||
        !std::isfinite(config.integralPermillePerMpsSecond.value) ||
        !std::isfinite(config.maxIntegrationStepS)) {
        return {false, VelocityControllerConfigError::NonFiniteValue};
    }
    if (config.feedForwardPermillePerMps.value < 0.0 ||
        config.proportionalPermillePerMps.value < 0.0 ||
        config.integralPermillePerMpsSecond.value < 0.0) {
        return {false, VelocityControllerConfigError::NegativeGain};
    }
    if (config.maxEffortPermille.value > 1000U) {
        return {false, VelocityControllerConfigError::EffortCeilingOutOfRange};
    }
    if (config.maxIntegralPermille > 1000U) {
        return {false, VelocityControllerConfigError::IntegralCeilingOutOfRange};
    }
    if (!isDrivingMode(config.positiveEffortMode.value)) {
        return {false, VelocityControllerConfigError::PositiveEffortModeIsNotDriving};
    }
    if (!(config.maxIntegrationStepS > 0.0)) {
        return {false, VelocityControllerConfigError::IntegrationStepNotPositive};
    }
    return {true, VelocityControllerConfigError::None};
}

// Why the regulator returned what it did. Diagnostic only: the effort itself is
// the output, and every non-Ok status carries a non-powered effort.
enum class VelocityControlStatus : uint8_t {
    Ok = 0,                // a feedback term was used; effort may be driving
    FeedForwardOnly,       // no usable velocity estimate: open-loop effort only
    Disabled,              // not enabled: non-powered output
    ConfigIsNotUsable,     // gains/ceiling/polarity not Measured or malformed
    InvalidTarget,         // non-finite target velocity: no effort is computed
    InvalidMeasurement,    // measurement claims to be usable but is not a number
    TimeDidNotAdvance,     // stamp not strictly newer: no integration this cycle
    ArithmeticNotFinite,   // a computed term or the demand is not finite: refused
                           // before conversion. Checked after InvalidTarget and
                           // InvalidMeasurement (caller faults win) and overriding
                           // the time/regulation statuses below, because infinite
                           // diagnostics must never be reported as regulation.
};

// One regulator answer. There is no default constructor: an effort with no
// stated reason behind it must be impossible to write.
struct VelocityControlResult {
    constexpr VelocityControlResult(MotorEffort effort_, VelocityControlStatus status_,
                                    double errorMps_, double feedForwardPermille_,
                                    double proportionalPermille_, double integralPermille_,
                                    uint32_t dtMs_, bool outputSaturated_,
                                    bool integrationStepClamped_, bool antiWindupActive_,
                                    bool integralResetForSignChange_)
        : effort(effort_),
          status(status_),
          errorMps(errorMps_),
          feedForwardPermille(feedForwardPermille_),
          proportionalPermille(proportionalPermille_),
          integralPermille(integralPermille_),
          dtMs(dtMs_),
          outputSaturated(outputSaturated_),
          integrationStepClamped(integrationStepClamped_),
          antiWindupActive(antiWindupActive_),
          integralResetForSignChange(integralResetForSignChange_) {}

    // The request. Always valid per validateMotorEffort(); never authorized.
    MotorEffort effort;

    VelocityControlStatus status;

    // Target minus measured, metres per second. 0.0 when no usable measurement
    // was available, so no error is reported that was never formed. Finite for
    // every physically meaningful pair of inputs: only two finite velocities
    // whose difference overflows would make it an infinity, and the effort stays
    // clamped to the direction's ceiling even then.
    double errorMps;

    // The three terms of the demand before the effort limit was applied, in
    // permille. Kept for telemetry: a log that shows only the clamped output
    // cannot explain why the loop behaved as it did.
    double feedForwardPermille;
    double proportionalPermille;
    double integralPermille;

    // Elapsed time honored this cycle, milliseconds. 0 when the clock did not
    // advance or was never established.
    uint32_t dtMs;

    // True when the demand exceeded the direction's effort limit and was
    // reduced to it (or to zero).
    bool outputSaturated;

    // True when dt was longer than maxIntegrationStepS and integration used the
    // clamp instead.
    bool integrationStepClamped;

    // True when integration was skipped because the demand was already at a
    // limit and the error pushed further into it. This is the anti-windup path,
    // kept visible so a log can distinguish it from a loop that does not want to
    // integrate.
    bool antiWindupActive;

    // True when the integral term was dropped because the commanded direction
    // changed since the previous cycle.
    bool integralResetForSignChange;
};

// ---------------------------------------------------------------------------
// VelocityController
// ---------------------------------------------------------------------------
//
// `nowMs` is supplied by the caller on every update (the same value `millis()`
// returns); the class never reads a clock. It is not enabled on construction
// and not enabled by `reset()`, so no previous command state can survive a
// reset or a power-up.
class VelocityController {
public:
    explicit VelocityController(const VelocityControllerConfig& config) : config_(config) {}

    const VelocityControllerConfig& config() const { return config_; }

    bool configUsable() const { return validateVelocityControllerConfig(config_).ok; }

    bool isEnabled() const { return enabled_; }

    // Starts a fresh control state: no integral, no time base, no remembered
    // direction. Refused when the configuration is not usable, so an unmeasured
    // gain cannot be enabled by accident; the refusal is visible as the return
    // value and as isEnabled() staying false.
    bool enable() {
        if (!configUsable()) {
            return false;
        }
        if (!enabled_) {
            enabled_ = true;
            clearControlState();
        }
        return true;
    }

    // Always available, always safe. Ends the control state entirely: the
    // integral is dropped and no time base or direction survives, so a later
    // enable() starts from nothing rather than resuming.
    void disable() {
        enabled_ = false;
        clearControlState();
    }

    // Returns the regulator to its post-construction state: disabled, no
    // integral, no time base, no remembered direction. Nothing resumes
    // automatically.
    void reset() { disable(); }

    // The integral term as it stands, permille. Diagnostic only.
    double integralPermille() const { return integralPermille_; }

    // One control step.
    //
    // `targetVelocityMps` is the profile's target, signed in the same convention
    // as the measurement. `measurementUsable` is the estimator's verdict *after*
    // freshness has been applied: the vehicle layer passes
    // `velocityReadiness(...).usable` (VelocityEstimator.h), not the bare
    // `VelocityEstimate::valid`. This class has no clock for the measurement and
    // cannot tell a fresh estimate from a last-valid one reused across cycles -
    // that question is owned by `velocityReadiness()`, exactly as the position
    // path uses `odometryReadiness()`. `measuredVelocityMps` is the signed speed
    // that verdict describes; a caller may pass any other estimate that carries
    // the same two facts.
    //
    // A missing measurement is not a reason to stop driving: with no feedback
    // the regulator runs feed-forward only, which is what start-up looks like
    // while the estimator window is still filling. A measurement that claims to
    // be usable and is not a number is an anomaly and is refused instead.
    VelocityControlResult update(uint32_t nowMs, double targetVelocityMps, bool measurementUsable,
                                 double measuredVelocityMps) {
        if (!enabled_) {
            return stopped(VelocityControlStatus::Disabled);
        }
        // Defensive: enable() already refuses an unusable configuration, so this
        // only fires if the configuration could change under a running
        // regulator. It must never emit a drive in that case.
        if (!configUsable()) {
            return stopped(VelocityControlStatus::ConfigIsNotUsable);
        }

        if (!std::isfinite(targetVelocityMps)) {
            // No effort is computed from a value that is not a number, and the
            // integral is deliberately preserved: the target may be a transient
            // reporting fault, and the loop itself is still sound.
            return stopped(VelocityControlStatus::InvalidTarget);
        }

        const bool measurementValid = measurementUsable && std::isfinite(measuredVelocityMps);
        if (measurementUsable && !std::isfinite(measuredVelocityMps)) {
            // The feedback path contradicted itself. Refuse this cycle and drop
            // the integral: continuing from an integral accumulated against
            // known-corrupt feedback would act on data that was never valid.
            integralPermille_ = 0.0;
            return stopped(VelocityControlStatus::InvalidMeasurement);
        }

        // --- Time base -----------------------------------------------------
        // dt is measured, never assumed. The first cycle after enabling has no
        // previous stamp, so there is nothing to measure against and no
        // integration happens. A stamp that does not advance (a repeated sample,
        // or a rewound clock) is refused in the same way and is not stored, so
        // the next advancing stamp still yields the true elapsed time.
        // A non-advancing stamp is reported in preference to the measurement
        // state, because it is the reason nothing integrates this cycle; the
        // feed-forward and proportional terms do not need time and are still
        // computed below.
        uint32_t dtMs = 0U;
        bool timeAdvancing = false;
        VelocityControlStatus status = measurementValid ? VelocityControlStatus::Ok
                                                        : VelocityControlStatus::FeedForwardOnly;
        if (!hasLastStamp_) {
            hasLastStamp_ = true;
            lastStampMs_ = nowMs;
        } else {
            const int32_t sinceLastMs = static_cast<int32_t>(nowMs - lastStampMs_);
            if (sinceLastMs <= 0) {
                status = VelocityControlStatus::TimeDidNotAdvance;
            } else {
                dtMs = static_cast<uint32_t>(sinceLastMs);
                lastStampMs_ = nowMs;
                timeAdvancing = true;
            }
        }

        // --- Commanded direction and its effort limit ----------------------
        // The direction of travel decides which powered effort may be asked for.
        // With a zero target the interval collapses to {0}: no powered effort is
        // requested at all, and the saturation logic below refuses to integrate.
        const int8_t targetSign = signOf(targetVelocityMps);
        const double effortLimitPermille = static_cast<double>(config_.maxEffortPermille.value);
        const double upperLimitPermille = (targetSign > 0) ? effortLimitPermille : 0.0;
        const double lowerLimitPermille = (targetSign < 0) ? -effortLimitPermille : 0.0;

        // --- Direction change ----------------------------------------------
        // A change of commanded sign - including starting or ending at zero -
        // drops the integral. The integral was accumulated for the previous
        // direction and would otherwise fight the new one; unwinding it slowly
        // is exactly the delay a deliberate reversal must not inherit.
        bool integralResetForSignChange = false;
        if (hasPreviousTargetSign_ && targetSign != previousTargetSign_) {
            integralPermille_ = 0.0;
            integralResetForSignChange = true;
        }
        previousTargetSign_ = targetSign;
        hasPreviousTargetSign_ = true;

        // --- Feed-forward, proportional, integral ---------------------------
        // R2/M18: finite configuration can still overflow its terms. A very
        // large finite gain times a finite target or error is an infinity
        // (validation accepts finite extremes - there is no measured gain cap
        // to appeal to), and opposite infinities cancel to NaN. The checks
        // below refuse the cycle with a named status before any clamping,
        // rounding or demand application.
        const double feedForwardPermille =
            config_.feedForwardPermillePerMps.value * targetVelocityMps;
        const double errorMps = measurementValid ? (targetVelocityMps - measuredVelocityMps) : 0.0;
        const double proportionalPermille = config_.proportionalPermillePerMps.value * errorMps;
        if (!std::isfinite(feedForwardPermille) || !std::isfinite(errorMps) ||
            !std::isfinite(proportionalPermille)) {
            return stopped(VelocityControlStatus::ArithmeticNotFinite);
        }

        double demandedPermille = feedForwardPermille + proportionalPermille + integralPermille_;

        bool integrationStepClamped = false;
        bool antiWindupActive = false;
        if (timeAdvancing && measurementValid &&
            config_.integralPermillePerMpsSecond.value > 0.0 && config_.maxIntegralPermille > 0U) {
            // Anti-windup by conditional integration: never integrate while the
            // demand is already at a limit and the error pushes further into it.
            // This is the simplest method that is clearly correct - no back
            // calculation, no tracking time constant, nothing to tune.
            const bool pushingFurtherIntoLimit =
                (demandedPermille > upperLimitPermille && errorMps > 0.0) ||
                (demandedPermille < lowerLimitPermille && errorMps < 0.0);
            if (pushingFurtherIntoLimit) {
                antiWindupActive = true;
            } else {
                double integrationStepS = static_cast<double>(dtMs) / 1000.0;
                if (integrationStepS > config_.maxIntegrationStepS) {
                    integrationStepS = config_.maxIntegrationStepS;
                    integrationStepClamped = true;
                }
                const double integralCeilingPermille = static_cast<double>(config_.maxIntegralPermille);
                // R2/M18: the increment itself can overflow while every input
                // is finite. An unrepresentable increment is held, not wound:
                // the established integral stays, exactly as anti-windup holds
                // rather than integrates. (A NaN can never arise here from
                // finite inputs, but a held integral is still the safe answer
                // for one, since clamp() passes NaN through untouched.)
                const double integralIncrement =
                    config_.integralPermillePerMpsSecond.value * errorMps * integrationStepS;
                if (std::isfinite(integralIncrement)) {
                    integralPermille_ = clamp(integralIncrement + integralPermille_,
                                              -integralCeilingPermille, integralCeilingPermille);
                }
                demandedPermille = feedForwardPermille + proportionalPermille + integralPermille_;
            }
        }

        // --- Arithmetic refusal ---------------------------------------------
        // The demand is the sum of three independently finite terms, but
        // opposite infinities cancel: +inf feed-forward against -inf
        // proportional is NaN, and either sign of infinity alone would clamp
        // into a powered effort the loop never regulated to. Refuse before
        // the conversion below; the refusal carries zeroed diagnostic terms
        // (stopped()) so the log cannot mistake it for computed control.
        if (!std::isfinite(integralPermille_) || !std::isfinite(demandedPermille)) {
            return stopped(VelocityControlStatus::ArithmeticNotFinite);
        }

        // --- Effort request -------------------------------------------------
        const double requestedPermille =
            clamp(demandedPermille, lowerLimitPermille, upperLimitPermille);
        // Whole permille is the resolution of the request. An effort that
        // quantizes to zero is emitted as Disabled, never as a Drive with
        // magnitude 0: a zero-effort Drive still latches a powered direction in
        // the safety boundary (Motion.h), and no direction may be latched that
        // is not actually being powered.
        const double roundedPermille = std::floor(std::fabs(requestedPermille) + 0.5);
        MotorEffort effort = MotorEffort(MotorMode::Disabled, 0U);
        if (roundedPermille >= 1.0) {
            effort = MotorEffort(requestedPermille > 0.0 ? config_.positiveEffortMode.value
                                                         : otherDriveMode(config_.positiveEffortMode.value),
                                 static_cast<uint16_t>(roundedPermille));
        }

        // Reported against the final demand, which the integral update above may
        // have moved to or away from the limit.
        const bool outputSaturated = demandedPermille > upperLimitPermille ||
                                     demandedPermille < lowerLimitPermille;
        return VelocityControlResult{effort, status, errorMps, feedForwardPermille,
                                     proportionalPermille, integralPermille_, dtMs,
                                     outputSaturated, integrationStepClamped, antiWindupActive,
                                     integralResetForSignChange};
    }

private:
    static int8_t signOf(double value) {
        if (value > 0.0) {
            return 1;
        }
        if (value < 0.0) {
            return -1;
        }
        return 0;
    }

    static MotorMode otherDriveMode(MotorMode mode) {
        return mode == MotorMode::DriveForward ? MotorMode::DriveReverse : MotorMode::DriveForward;
    }

    static double clamp(double value, double low, double high) {
        if (value < low) {
            return low;
        }
        if (value > high) {
            return high;
        }
        return value;
    }

    // A refusal always carries the same non-powered effort and no terms, so a
    // refused cycle can never be mistaken for a computed demand. The integral is
    // reported as it stands: where a refusal preserves it, the report says so.
    VelocityControlResult stopped(VelocityControlStatus status) const {
        return VelocityControlResult{MotorEffort(MotorMode::Disabled, 0U), status, 0.0, 0.0, 0.0,
                                     integralPermille_, 0U, false, false, false, false};
    }

    void clearControlState() {
        integralPermille_ = 0.0;
        hasLastStamp_ = false;
        lastStampMs_ = 0U;
        hasPreviousTargetSign_ = false;
        previousTargetSign_ = 0;
    }

    VelocityControllerConfig config_;
    bool enabled_ = false;
    double integralPermille_ = 0.0;
    bool hasLastStamp_ = false;
    uint32_t lastStampMs_ = 0U;
    bool hasPreviousTargetSign_ = false;
    int8_t previousTargetSign_ = 0;
};

// Pin the "no silent answer" contract and the safe zero values.
static_assert(!std::is_default_constructible<VelocityControlResult>::value,
              "a control result must never default-construct into a valid-looking answer");
static_assert(static_cast<int>(VelocityControlStatus::Ok) == 0,
              "the ok status must stay the zero value");

}  // namespace ev
