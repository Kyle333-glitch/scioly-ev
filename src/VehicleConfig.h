#pragma once

#include <CompetitionStateMachine.h>
#include <ConfigurationChecks.h>
#include <DriverFault.h>
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
//
// This is a cadence-and-staleness policy value, so it is a plain constant rather
// than a `Sourced` measurement - but it is also a guess, and it is the *longest*
// staleness this firmware acts on. Its honest value comes out of the H5 velocity
// trace and the H7 stop characterization (how fast the window estimate is worth
// trusting at each speed), and until those exist the number is only as good as
// the dwell it spans. It is deliberately not derived from the loop period: a
// limit tied to the control cadence would shrink silently when the cadence
// changed, and the dwell it has to span did not change with it.
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

// The longest gap between two control updates that may still be credited as
// continued near-zero evidence, milliseconds - and a **measured** value as of
// H0W: `worst_stall_ms`, the longest interval between two consecutive committed
// control ticks, was read from B1 under every healthy load condition (telemetry
// off, the highest admissible rate with the host reading, the same rate with the
// host deliberately not reading, and a 62 s soak) and never exceeded **28 ms**.
//
// It is deliberately not the 20 ms control period: the period is the cadence the
// loop is *asked* for, and this is the worst gap it actually delivered, so a
// dwell that spans a longer gap than this is not credited without fresh evidence
// (`NearZeroVerifier.h`). Lower is the safe direction - a future run that stalls
// longer only makes the verifier restart its interval - and the value is one
// measurement, not a ceiling: re-measure it (H0W Step 8) when a loaded run adds
// work to the same loop.
//
// Evidence: `data/evidence/P-H0W-2026-09-28/h0w-step8-pins-baseline.log`
// (`worst_stall_ms=28`, two readings 57 s apart), with
// `h0w-step8-stall-loads.log` / `h0w-step8-stall-unread.log` at 21-22 ms; log
// entry D28.
constexpr uint32_t kMaxDwellGapMs = 28U;
constexpr const char* kMaxDwellGapEvidence =
    "data/evidence/P-H0W-2026-09-28/h0w-step8-pins-baseline.log (worst_stall_ms), D28";

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

// The planned reading of the driver's nFAULT line. `Unverified` - the zero value -
// is deliberate and is what this build ships: nobody has measured the line, so no
// level is interpreted as a fault and the raw level is reported instead
// (`VERIFIED_STATE.md` #1, #2). H1B measures which level the driver drives for
// each line state; promoting this value to `ActiveLow` or `ActiveHigh` with a
// citation is what turns the line into a latched fault, and
// `DriverFaultPolarityVerified` refuses to arm the vehicle until that has
// happened - a driver fault nobody interprets is a driver fault nobody stops for.
constexpr ev::DriverFaultPolarity kPlannedDriverFaultPolarity =
    ev::DriverFaultPolarity::Unverified;

// ---------------------------------------------------------------------------
// Provenance helpers
// ---------------------------------------------------------------------------

inline ev::Sourced<double> provisionalValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

inline ev::Sourced<uint16_t> provisionalPermille(uint16_t value) {
    return ev::Sourced<uint16_t>(value, ev::ValueSource::Provisional);
}

inline ev::Sourced<uint32_t> provisionalGapMs(uint32_t value) {
    return ev::Sourced<uint32_t>(value, ev::ValueSource::Provisional);
}

// A gap that was measured on the board rather than guessed. The citation is not
// optional: `Sourced::citesEvidence()` is what the `MeasuredValuesCiteEvidence`
// configuration check reads, and a measured value without one fails it.
inline ev::Sourced<uint32_t> measuredGapMs(uint32_t value, const char* citation) {
    return ev::Sourced<uint32_t>(value, ev::ValueSource::Measured, citation);
}

inline ev::Sourced<ev::MotionDirection> provisionalDirection(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Provisional);
}

inline ev::Sourced<ev::MotorMode> provisionalEffortMode(ev::MotorMode mode) {
    return ev::Sourced<ev::MotorMode>(mode, ev::ValueSource::Provisional);
}

inline ev::Sourced<ev::DriverFaultPolarity> plannedDriverFaultPolarity() {
    return ev::Sourced<ev::DriverFaultPolarity>(kPlannedDriverFaultPolarity,
                                                ev::ValueSource::Provisional);
}

// The final stop dwell is the event's Complete Stop (rule 8.p). It is a rule
// value, never a measurement: the run must hold its final stop at least this
// long before it may be called complete. The citation is the rule itself, which
// is what `Sourced::citesEvidence()` asks for on anything that is not a
// placeholder.
inline ev::Sourced<double> completeStopDwellS() {
    return ev::Sourced<double>(ev::rules2027::kCompleteStopS, ev::ValueSource::RuleDerived,
                               "rules2027 8.p (Complete Stop)");
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
                              provisionalValue(kPlannedReversalStopDwellS),
                              measuredGapMs(kMaxDwellGapMs, kMaxDwellGapEvidence));
}

inline ev::NearZeroConfig finalStopVerification() {
    return ev::NearZeroConfig(provisionalValue(kPlannedReversalStopThresholdMps),
                              completeStopDwellS(),
                              measuredGapMs(kMaxDwellGapMs, kMaxDwellGapEvidence));
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
// Provenance: does every promoted value cite where it came from?
// ---------------------------------------------------------------------------
//
// Promoting a placeholder to `ValueSource::Measured` is the one edit that lets a
// value authorize powered motion, and the promotion is supposed to carry a
// pointer to the evidence with it (`VERIFIED_STATE.md`, "How to promote a
// fact": the row, the capture under `data/evidence/`, the log entry). Nothing
// about that is enforceable by the compiler, so it is made into a
// configuration-build check instead: the report carries
// `MeasuredValuesCiteEvidence`, and it is false for as long as any non-placeholder
// value here names no evidence.
//
// The check passes vacuously while every value is provisional, and that is
// correct rather than a loophole: a placeholder is not a claim about the vehicle
// and needs no citation. It bites exactly when the promotion happens - which is
// the moment a missing citation would otherwise be invisible, because the value
// would start authorizing motion while looking like every other "measured" one.
//
// This is deliberately not a `firstFailure()` ordering device: it reports a
// missing record, not a missing measurement, so it is listed after
// `CalibrationMeasured` (the measurement itself comes first) and is checked for
// every value the builders produce.
namespace provenance {

template <typename T>
inline bool cited(const ev::Sourced<T>& value) {
    return value.citesEvidence();
}

inline bool cited(const ev::NearZeroConfig& config) {
    return cited(config.velocityThresholdMps) && cited(config.dwellS) &&
           cited(config.maxUpdateGapMs);
}

inline bool cited(const ev::MotionProfileLimits& limits) {
    return cited(limits.cruiseVelocityMps) && cited(limits.velocityCeilingMps) &&
           cited(limits.accelLimitMps2) && cited(limits.decelLimitMps2) &&
           cited(limits.brakeOffsetM) && cited(limits.positiveCommandDirection);
}

}  // namespace provenance

// True when every value the builders above produce that is not a placeholder
// names its evidence. Read by the configuration-build report; nothing reads it at
// run time.
inline bool allPromotedValuesCiteEvidence() {
    const ev::VehicleCalibration calibrationValues = calibration();
    const ev::SafetyLimits limits = safetyLimits();
    const ev::VelocityControllerConfig controller = controllerConfig();
    const ev::DirectionalMotionProfileLimits profile = profileLimits();
    const ev::NearZeroConfig reversalStop = reversalStopVerification();
    const ev::NearZeroConfig finalStop = finalStopVerification();
    const ev::RunPhaseConfig phase = runPhaseConfig();
    const ev::ReversalConfig reversal = reversalConfig();

    const bool calibrationCited =
        provenance::cited(calibrationValues.distancePerEncoderTickM) &&
        provenance::cited(calibrationValues.forwardBrakeOffsetM) &&
        provenance::cited(calibrationValues.reverseBrakeOffsetM);
    const bool limitsCited = provenance::cited(limits.maxEffortPermille) &&
                             provenance::cited(limits.maxForwardVelocityMps) &&
                             provenance::cited(limits.maxReverseVelocityMps) &&
                             provenance::cited(limits.maxRunDurationS) &&
                             provenance::cited(limits.minBatteryV);
    const bool controllerCited = provenance::cited(controller.feedForwardPermillePerMps) &&
                                 provenance::cited(controller.proportionalPermillePerMps) &&
                                 provenance::cited(controller.integralPermillePerMpsSecond) &&
                                 provenance::cited(controller.positiveEffortMode) &&
                                 provenance::cited(controller.maxEffortPermille);
    const bool polarityCited = provenance::cited(positiveTicksDirection()) &&
                               provenance::cited(plannedDriverFaultPolarity());
    const bool profileCited = provenance::cited(profile.forward) &&
                              provenance::cited(profile.reverse);
    const bool stopCited =
        provenance::cited(reversalStop) && provenance::cited(finalStop) &&
        provenance::cited(reversal.verification) && provenance::cited(reversal.reversalTimeoutS);
    const bool phaseCited = provenance::cited(phase.optionalDelayS) &&
                            provenance::cited(phase.approachTimeoutS) &&
                            provenance::cited(phase.captureTimeoutS) &&
                            provenance::cited(phase.pushTimeoutS) &&
                            provenance::cited(phase.reverseTimeoutS) &&
                            provenance::cited(phase.finalApproachTimeoutS) &&
                            provenance::cited(phase.finalStopTimeoutS) &&
                            provenance::cited(phase.finalApproachDistanceM);

    return calibrationCited && limitsCited && controllerCited && polarityCited && profileCited &&
           stopCited && phaseCited;
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
// `configurationValid` is false because no run has been announced. Two of the
// checks are about facts rather than values and are false for their own reasons:
// `driverFaultPolarityVerified` waits on H1B's line measurement, and
// `measuredValuesCiteEvidence` will bite on the first promotion that names no
// evidence.
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
    checks.measuredValuesCiteEvidence = allPromotedValuesCiteEvidence();
    checks.driverFaultPolarityVerified =
        ev::validateDriverFaultConfig(plannedDriverFaultPolarity()).ok;
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
