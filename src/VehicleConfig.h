#pragma once

#include <CompetitionStateMachine.h>
#include <ConfigurationChecks.h>
#include <Motion.h>
#include <MotionProfile.h>
#include <NearZeroVerifier.h>
#include <ReversalSequencer.h>
#include <RunConfig.h>
#include <SafetyLimits.h>
#include <Units.h>
#include <VehicleCalibration.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

// ---------------------------------------------------------------------------
// The vehicle layer's configuration - and the honest provenance of every value
// ---------------------------------------------------------------------------
//
// This file exists so the vehicle layer has data to validate, refuse to arm on,
// and report. It is NOT a tuning file and nothing in it is a measurement.
//
// Three kinds of value live here, and they are kept apart on purpose
// (`SOFTWARE_ARCHITECTURE.md` section 7):
//
//   1. **Cadence and staleness policy.** Loop period, sample-age limits and the
//      stop-evidence limit are design choices about how the firmware behaves, not
//      facts about the vehicle. They are still choices nobody has validated, and
//      the sample-age one is a guess until the loop's real cost is measured.
//   2. **`ValueSource::Provisional` placeholders.** Every calibration value,
//      safety ceiling, gain, limit, tolerance and threshold is provisional: a
//      number that exists so the data types are complete and the refusal paths
//      can run. A provisional value can never authorize powered motion, and
//      `allConfigurationChecksPass()` below is false for as long as any of them
//      is provisional. That is the intended state of this firmware, not an
//      obstacle to work around.
//   3. **Rule-derived values.** Where a value comes from the event rules it says
//      so and cites the rule; the run's final stop dwell is the Complete Stop
//      interval (rule 8.p) and arrives as `RuleDerived`.
//
// No number here may be moved into `lib/`, and none may be relabelled
// `Measured` without a recorded physical measurement (`VERIFIED_STATE.md`).
// Replacing a placeholder is one edit here plus one line in the development log.

namespace evhw {
namespace vehicleConfig {

// ---------------------------------------------------------------------------
// 1. Cadence and staleness policy
// ---------------------------------------------------------------------------

// The requested control cadence: 50 Hz. This is a design target, not a measured
// loop cost. The scheduler measures the interval that actually happens, and the
// telemetry snapshot reports it, so overrun is visible rather than assumed away.
constexpr uint32_t kControlPeriodMs = 20U;

// The firmware identity, as the operator interface reports it (`VERSION,fw=...`)
// and as a serial log identifies the build it came from. A compile-time literal
// with no comma in it: the response formatter writes it verbatim into a
// comma-separated line.
constexpr const char* kFirmwareId = "ev-nano-r4-p7b";

// How old a control-path sample may be and still be used. Three control cycles,
// chosen as a starting policy.
constexpr uint32_t kMaxSampleAgeMs = 60U;

// How old the window-derived speed in the stop-verification packet may be. The
// packet pairs it with the current cycle's zero-speed claim, so this limit must
// span the longer of the two stop dwells; `stopEvidenceLimitSpansDwell()` is what
// refuses a limit that does not.
constexpr uint32_t kStopEvidenceMaxAgeMs = 3200U;

// The velocity estimator's window span and its explicit zero-speed claim timeout.
// Both are policy, not measurement: no window has been shown to be adequate for
// this encoder yet (`VERIFIED_STATE.md` #5, #10), and the claim is a software
// statement about observed non-change, never a measurement.
constexpr uint32_t kEstimatorWindowMs = 20U;
constexpr uint32_t kEstimatorNoEdgeTimeoutMs = 100U;

// ---------------------------------------------------------------------------
// 2. Placeholders - every one of these is Provisional
// ---------------------------------------------------------------------------

// The tick-to-distance scale. A rollout measurement replaces this
// (`VERIFIED_STATE.md` #6).
constexpr double kPlannedDistancePerEncoderTickM = 0.0001;

// The extra travel after commanding a stop, per direction. Braking-distance
// characterization replaces these (`VERIFIED_STATE.md` #9).
constexpr double kPlannedForwardBrakeOffsetM = 0.05;
constexpr double kPlannedReverseBrakeOffsetM = 0.05;

// The highest effort the firmware may ever command, permille of full scale. Used
// for BOTH the safety ceiling and the regulator's own ceiling, from this one
// constant, because the two must be numerically equal (`effortCeilingsAgree()`).
constexpr uint16_t kPlannedMaxEffortPermille = 300U;

// Safety ceilings. None of these has an enforcement point yet except the effort
// ceiling: the run timer and the battery check that will own the other three are
// not built (`SOFTWARE_ARCHITECTURE.md` section 7).
constexpr double kPlannedMaxForwardVelocityMps = 1.2;
constexpr double kPlannedMaxReverseVelocityMps = 0.8;
constexpr double kPlannedMaxRunDurationS = 30.0;
constexpr double kPlannedMinBatteryV = 9.0;

// Target-shaping placeholders: a slow cruise, a taper, and the two brake offsets
// above.
constexpr double kPlannedCruiseMps = 0.5;
constexpr double kPlannedAccelMps2 = 0.5;
constexpr double kPlannedDecelMps2 = 1.0;

// Regulator placeholders. None is a tuning result (`VERIFIED_STATE.md` #8).
constexpr double kPlannedFeedForwardPermillePerMps = 250.0;
constexpr double kPlannedProportionalPermillePerMps = 150.0;
constexpr double kPlannedIntegralPermillePerMpsSecond = 80.0;
constexpr uint16_t kPlannedMaxIntegralPermille = 200U;
constexpr double kPlannedMaxIntegrationStepS = 0.05;

// Near-zero stop detection. The reversal gate's dwell is a short stop; the run's
// final stop is the event's Complete Stop and is rule-derived (rule 8.p).
constexpr double kPlannedReversalStopThresholdMps = 0.05;
constexpr double kPlannedReversalStopDwellS = 0.2;

// Run-policy bounds. Every one of these is a bound the run is allowed to wait
// for evidence under, so a placeholder that is too short turns into a phase
// timeout rather than into motion - which is the safe direction.
constexpr double kPlannedOptionalDelayS = 0.0;
constexpr double kPlannedApproachTimeoutS = 20.0;
constexpr double kPlannedCaptureTimeoutS = 5.0;
constexpr double kPlannedPushTimeoutS = 20.0;
constexpr double kPlannedReverseTimeoutS = 15.0;
constexpr double kPlannedFinalApproachTimeoutS = 15.0;
constexpr double kPlannedFinalStopTimeoutS = 10.0;
constexpr double kPlannedFinalApproachDistanceM = 0.3;
constexpr double kPlannedReversalTimeoutS = 3.0;

// The planned polarity fact, in its three encodings, for the one physical
// direction the project cannot name yet (`VERIFIED_STATE.md` #4): which way the
// motor turns for a positive command, which way the decoder counts for forward
// travel, and which driver input means forward. One measurement sets all three,
// and until then every one of them is Provisional - so no driver input
// combination for a drive is ever produced by an authorized command.
constexpr ev::MotionDirection kPlannedPositiveCommandDirection = ev::MotionDirection::Forward;
constexpr ev::MotorMode kPlannedPositiveEffortMode = ev::MotorMode::DriveForward;
constexpr ev::MotionDirection kPlannedPositiveTicksDirection = ev::MotionDirection::Forward;

// ---------------------------------------------------------------------------
// Provenance helpers
// ---------------------------------------------------------------------------

inline ev::Sourced<double> provisionalValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

inline ev::Sourced<uint16_t> provisionalPermille(uint16_t value) {
    return ev::Sourced<uint16_t>(value, ev::ValueSource::Provisional);
}

inline ev::Sourced<ev::MotionDirection> provisionalDirection(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Provisional);
}

inline ev::Sourced<ev::MotorMode> provisionalEffortMode(ev::MotorMode mode) {
    return ev::Sourced<ev::MotorMode>(mode, ev::ValueSource::Provisional);
}

// The final stop dwell is the event's Complete Stop (rule 8.p). It is a rule
// value, never a measurement: the run must hold its final stop at least this
// long before it may be called complete.
inline ev::Sourced<double> completeStopDwellS() {
    return ev::Sourced<double>(ev::rules2027::kCompleteStopS, ev::ValueSource::RuleDerived);
}

// ---------------------------------------------------------------------------
// 3. Configuration builders
// ---------------------------------------------------------------------------

inline ev::VehicleCalibration calibration() {
    return ev::VehicleCalibration{provisionalValue(kPlannedDistancePerEncoderTickM),
                                  provisionalValue(kPlannedForwardBrakeOffsetM),
                                  provisionalValue(kPlannedReverseBrakeOffsetM)};
}

inline ev::SafetyLimits safetyLimits() {
    return ev::SafetyLimits{provisionalPermille(kPlannedMaxEffortPermille),
                            provisionalValue(kPlannedMaxForwardVelocityMps),
                            provisionalValue(kPlannedMaxReverseVelocityMps),
                            provisionalValue(kPlannedMaxRunDurationS),
                            provisionalValue(kPlannedMinBatteryV)};
}

inline ev::VelocityEstimatorConfig estimatorConfig() {
    return ev::VelocityEstimatorConfig(kEstimatorWindowMs, kEstimatorNoEdgeTimeoutMs);
}

inline ev::VelocityControllerConfig controllerConfig() {
    // The ceiling is derived from the safety limit's own constant, so the two
    // cannot disagree numerically by accident; the authority still clamps
    // independently.
    return ev::VelocityControllerConfig(provisionalValue(kPlannedFeedForwardPermillePerMps),
                                        provisionalValue(kPlannedProportionalPermillePerMps),
                                        provisionalValue(kPlannedIntegralPermillePerMpsSecond),
                                        provisionalEffortMode(kPlannedPositiveEffortMode),
                                        provisionalPermille(kPlannedMaxEffortPermille),
                                        kPlannedMaxIntegralPermille,
                                        kPlannedMaxIntegrationStepS);
}

inline ev::MotionProfileLimits profileLimitsForOneDirection(ev::Sourced<double> velocityCeilingMps) {
    return ev::MotionProfileLimits(provisionalValue(kPlannedCruiseMps), velocityCeilingMps,
                                   provisionalValue(kPlannedAccelMps2),
                                   provisionalValue(kPlannedDecelMps2),
                                   provisionalValue(kPlannedForwardBrakeOffsetM),
                                   provisionalDirection(kPlannedPositiveCommandDirection));
}

inline ev::DirectionalMotionProfileLimits profileLimits() {
    return ev::DirectionalMotionProfileLimits(
        profileLimitsForOneDirection(provisionalValue(kPlannedMaxForwardVelocityMps)),
        profileLimitsForOneDirection(provisionalValue(kPlannedMaxReverseVelocityMps)));
}

inline ev::NearZeroConfig reversalStopVerification() {
    return ev::NearZeroConfig(provisionalValue(kPlannedReversalStopThresholdMps),
                              provisionalValue(kPlannedReversalStopDwellS));
}

inline ev::NearZeroConfig finalStopVerification() {
    return ev::NearZeroConfig(provisionalValue(kPlannedReversalStopThresholdMps),
                              completeStopDwellS());
}

inline ev::RunPhaseConfig runPhaseConfig() {
    return ev::RunPhaseConfig(provisionalValue(kPlannedOptionalDelayS),
                              provisionalValue(kPlannedApproachTimeoutS),
                              provisionalValue(kPlannedCaptureTimeoutS),
                              provisionalValue(kPlannedPushTimeoutS),
                              provisionalValue(kPlannedReverseTimeoutS),
                              provisionalValue(kPlannedFinalApproachTimeoutS),
                              provisionalValue(kPlannedFinalStopTimeoutS),
                              provisionalValue(kPlannedFinalApproachDistanceM),
                              finalStopVerification());
}

inline ev::ReversalConfig reversalConfig() {
    return ev::ReversalConfig(reversalStopVerification(),
                              provisionalValue(kPlannedReversalTimeoutS));
}

inline ev::Sourced<ev::MotionDirection> positiveTicksDirection() {
    return provisionalDirection(kPlannedPositiveTicksDirection);
}

// ---------------------------------------------------------------------------
// The configuration-build check report
// ---------------------------------------------------------------------------
//
// One type, not two. The checks are production functions that live with the
// modules they check; the *report* - which checks exist, what they are called
// and how a refusal is attributed - lives in `lib/evcore/src/ConfigurationChecks.h`,
// because the operator interface and the telemetry schema both read it. A second
// mirror type here would be able to drift from it, and a check that exists in one
// report but not the other is a check no refusal could ever name.
//
// With this stage's configuration, `allPass()` is false and is expected to stay
// false: the calibration, the limits, the gains and every polarity encoding are
// provisional, so `calibrationMeasured`, `safetyLimitsValid`, `controllerConfigValid`,
// `profileLimitsValid`, `phaseConfigValid` and `reversalConfigValid` all fail, and
// `configurationValid` is false because no run has been announced.
using ConfigurationBuildChecks = ev::ConfigurationBuildChecks;

// Evaluates the checks against the running chain. `announcedConfigValid` is the
// verdict of the single run-configuration validation entry point for the run that
// is currently announced (false when none is).
inline ConfigurationBuildChecks configurationBuildChecks(bool announcedConfigValid,
                                                         const ev::CompetitionStateMachine& run) {
    ConfigurationBuildChecks checks{};
    const ev::VehicleCalibration calibrationValues = calibration();
    const ev::SafetyLimits limits = safetyLimits();
    const ev::VelocityControllerConfig controller = controllerConfig();
    const ev::DirectionalMotionProfileLimits profile = profileLimits();
    const ev::RunPhaseConfig phase = runPhaseConfig();
    const ev::ReversalConfig reversal = reversalConfig();

    checks.configurationValid = announcedConfigValid;
    checks.calibrationMeasured = calibrationValues.isFullyMeasured();
    checks.effortCeilingsAgree =
        ev::effortCeilingsAgree(controller.maxEffortPermille, limits.maxEffortPermille);
    checks.commandPolarityAgrees =
        ev::polarityAgrees(profile.forward.positiveCommandDirection, controller.positiveEffortMode);
    checks.tickPolarityAgrees = ev::tickPolarityAgrees(positiveTicksDirection(),
                                                       profile.forward.positiveCommandDirection);
    checks.stopEvidenceSpansReversalStop =
        ev::stopEvidenceLimitSpansDwell(kStopEvidenceMaxAgeMs, reversal.verification);
    checks.stopEvidenceSpansFinalStop =
        ev::stopEvidenceLimitSpansDwell(kStopEvidenceMaxAgeMs, phase.stopVerification);
    checks.safetyLimitsValid = ev::validateSafetyLimits(limits).ok;
    checks.controllerConfigValid = ev::validateVelocityControllerConfig(controller).ok;
    checks.estimatorConfigValid = ev::validateVelocityEstimatorConfig(estimatorConfig()).ok;
    checks.profileLimitsValid = ev::validateMotionProfileLimits(profile.forward).ok &&
                                ev::validateMotionProfileLimits(profile.reverse).ok;
    checks.phaseConfigValid = ev::validateRunPhaseConfig(phase).ok;
    checks.reversalConfigValid = ev::validateReversalConfig(reversal).ok;
    checks.runPolicyUsable = run.configUsable();
    return checks;
}

}  // namespace vehicleConfig
}  // namespace evhw
