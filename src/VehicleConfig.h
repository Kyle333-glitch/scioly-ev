#pragma once

#include <CompetitionStateMachine.h>
#include <ConfigurationChecks.h>
#include <DriverFault.h>
#include <FirmwareIdentity.h>
#include <Motion.h>
#include <MotionProfile.h>
#include <NearZeroVerifier.h>
#include <ReversalSequencer.h>
#include <RunConfig.h>
#include <RunExecutionPlan.h>
#include <SafetyLimits.h>
#include <Units.h>
#include <VehicleCalibration.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

#if defined(EV_BUILD_IDENTITY_AVAILABLE)
// Generated per build by scripts/ev_build_identity.py into the build
// directory (never committed): EV_BUILD_REVISION, EV_BUILD_DIRTY.
#include "EvBuildIdentity.h"
#endif

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

// The firmware identity, as the operator interface reports it (`VERSION,...`
// with the shared identity fragment, and `BOOT,...` with the same fields) and
// as a serial log identifies the build it came from.
//
// This is deliberately NOT a constant string: a constant cannot tell two
// materially different images apart (R3/L3). The identity a device reports is
// composed by `firmwareIdentity()` below from four independent facts: the
// stable family name, the source revision plus dirty state supplied per build
// by the build script, the image variant supplied per environment by the
// build flags, and a fingerprint over the calibration/configuration values
// this build carries. `VERSION`/`BOOT` render it through
// `ev::appendFirmwareIdentity()`; see `lib/evcore/src/FirmwareIdentity.h`
// for the field contract and what the identity does not claim (in
// particular: a revision names the source, never the flashed binary).
constexpr const char* kFirmwareFamily = ev::kFirmwareFamily;

// The source revision this image was built from: the short commit hash the
// build script recorded, or `unknown` when no revision metadata reached the
// build (a host-test binary, a source archive without version control).
// Unknown is honest, never fatal: the build succeeds and says so.
inline const char* buildRevision() {
#if defined(EV_BUILD_IDENTITY_AVAILABLE)
    return EV_BUILD_REVISION;
#else
    return ev::kFirmwareRevisionUnknown;
#endif
}

// True when the tree the image was built from had uncommitted changes - or
// when no revision metadata was available at all, in which case cleanness
// cannot be shown and must not be claimed. A dirty build never masquerades
// as a clean committed image.
inline bool buildIsDirty() {
#if defined(EV_BUILD_IDENTITY_AVAILABLE)
    return EV_BUILD_DIRTY != 0;
#else
    return true;
#endif
}

// The image variant this environment builds. It comes from the
// per-environment `EV_FIRMWARE_VARIANT` build define (`platformio.ini`), not
// from probing the running image, and an undefined define reads as Unknown
// rather than as any real image.
inline ev::FirmwareVariant firmwareVariant() {
#if defined(EV_FIRMWARE_VARIANT) && (EV_FIRMWARE_VARIANT == 0)
    return ev::FirmwareVariant::Production;
#elif defined(EV_FIRMWARE_VARIANT) && (EV_FIRMWARE_VARIANT == 1)
    return ev::FirmwareVariant::Bench;
#elif defined(EV_FIRMWARE_VARIANT) && (EV_FIRMWARE_VARIANT == 2)
    return ev::FirmwareVariant::BenchWatchdog;
#else
    return ev::FirmwareVariant::Unknown;
#endif
}

// How old a control-path sample may be and still be used. Three control cycles,
// chosen as a starting policy.
constexpr uint32_t kMaxSampleAgeMs = 60U;

// How old the window-derived speed in the stop-verification packet may be. The
// packet pairs it with the current cycle's zero-speed claim - which appears
// only after the estimator's no-edge interval - so this limit must span the
// claim lead plus the longer of the two stop dwells;
// `stopEvidenceSpansClaimAndDwell()` is what refuses a limit that does not
// (S18A/P5).
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

// Safety ceilings. The effort ceiling is enforced through the regulator's own
// ceiling; the maximum run duration is enforced at run time by the vehicle
// layer's run-scoped bound (`RunDurationGuard.h`), which only bounds a run once
// this value is `Measured`; and the battery voltage is a capability-aware arming
// precondition rather than a runtime comparison (see
// `kPlannedBatteryVoltageCapability`). Every value here is a placeholder, so no
// run may be armed on it.
constexpr double kPlannedMaxForwardVelocityMps = 1.2;
constexpr double kPlannedMaxReverseVelocityMps = 0.8;
constexpr double kPlannedMaxRunDurationS = 30.0;
constexpr double kPlannedMinBatteryV = 9.0;

// Target-shaping placeholders: a slow cruise, a taper, and the two brake offsets
// above.
constexpr double kPlannedCruiseMps = 0.5;
constexpr double kPlannedAccelMps2 = 0.5;
constexpr double kPlannedDecelMps2 = 1.0;

// The slowest cruise at which travel is known to be reliable, m/s. The diagnostic
// timing model never estimates below it - a speed the drivetrain cannot follow is
// not a way to spend time, so the slack it cannot spend travelling would become a
// deliberate stationary delay instead. A rollout/velocity trace (H4/H5) plus a
// "lowest commanded cruise that produces reliable movement" measurement replaces
// this; until then it is a placeholder and no run may be announced on it.
constexpr double kPlannedMinimumCruiseMps = 0.10;

// Bottle-phase strategy ceilings: the speeds competition strategy wants the
// three bottle phases travelled at, metres per second (Approach, Capture, Push).
// They only ever tighten - `MotionProfile` takes the smallest of the cruise
// ceiling, the distance taper and the active phase ceiling - so they cannot
// raise a shaped target, cannot bypass an acceleration/deceleration limit, and a
// phase change is ramped, never a step.
//
// These are placeholders, and the placeholder value is deliberately the
// configured cruise: no per-phase speed has been established on this vehicle, so
// an unmeasured ceiling must not slow (or speed) anything. While they are
// `Provisional`, `validateRunPhaseConfig()` refuses the whole run policy - exactly
// as it already does for the planned-cruise floor - so no run can be announced or
// armed on them, and the ceilings are unusable until the bottle-integration
// protocol (P-H8) establishes real per-phase speeds. Do not tune these to make a
// run move.
constexpr double kPlannedApproachCeilingMps = kPlannedCruiseMps;
constexpr double kPlannedCaptureCeilingMps = kPlannedCruiseMps;
constexpr double kPlannedPushCeilingMps = kPlannedCruiseMps;

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

// The **longest** deliberate pre-travel delay a run may spend, seconds: a
// legal/safety maximum only. The delay a run actually spends is its own immutable
// execution plan's `initialDelayS` (`ev::RunExecutionPlan`), handed to the run
// policy at the announcement boundary, and that value may not exceed this bound.
// It is the event's own maximum Target Time (rule 8.i): no run may wait longer
// than the longest run the event can ask for, which is a bound the rules state
// rather than a number anyone has to measure. Deliberately NOT `Measured` - no
// experiment on this vehicle can establish what the event's rules say - and it is
// RuleDerived rather than Provisional because it *is* a rule value, cited below
// through `completeStopDwellS()`'s convention.
constexpr double kPlannedMaximumOptionalDelayS = ev::rules2027::kTargetTimeMaxS;

// Whether a **physical timing model** has been validated for this vehicle and
// build: measured accel/decel/cruise behaviour correlated against realised Run
// Times closely enough that the kinematic estimate may control a run's cruise and
// deliberate delay. It is the one production flag behind that decision, and false
// is its only value until the evidence exists.
//
// While it is false, none of the following may happen on a real run:
//
//   * `planRunTiming()` may not choose the run's cruise - the configured strategy
//     cruise is used (the estimated numbers stay a labelled diagnostic);
//   * the estimate's delay may not become the run's deliberate delay - the
//     execution plan carries `timingReady = false` and `initialDelayS = 0`; and
//   * an estimate that cannot meet the announced Target Time may not refuse an
//     otherwise legal announcement (`announceVerdictFor()` does not consult the
//     estimate for a refusal at all).
//
// Setting this true is not the validation; the evidence is. Validating the model
// means replacing the `Provisional` accel/decel/cruise limits with measured ones
// and recording several announced Target Times against realised Run Times on a
// marked course (P-H14 step 12) with the raw data preserved, so the estimate can
// be compared with what the vehicle actually did.
//
// The estimate's structure is itself unvalidated, independently of its limits
// (S18C/P7, October audits M16/M7): it sizes one acceleration/deceleration pair
// over the whole rule-geometry out-and-back (`plannedTravelDistanceM()`), while
// execution travels two legs - forward to the plan's forward stop (Bottle Line
// plus the measured forward-stop margin, which the rule distance omits on both
// legs), a stop and reversal between them, then reverse to the Target Point -
// with two sets of acceleration/deceleration behavior, per-phase strategy
// ceilings and ramps, stop-verification dwells, and bottle-interaction timing
// that no kinematic estimate contains. Enabling the flag therefore requires a
// model-structure review against two-leg execution arithmetic first, and the
// physical Run Time comparison second; a single empirical correction must not
// be assumed transferable across the legal parameter space. Until both exist
// the numbers stay a labelled diagnostic: reported in the announce
// acknowledgement and the `plan` line (whose `timing_ready` and absent
// `predicted_s` marker say unavailable rather than zero), never a choice of
// cruise, delay, or refusal.
constexpr bool kRunTimingModelValidated = false;

// Bottle-interaction placeholders: how far before the Bottle Start Point the
// pusher is taken to touch the bottle, how far past that the bottle counts as
// captured, and how far past the Bottle Line position the push must carry it.
// None of these is a mechanism measurement - the pusher's reach, the funnel's
// capture length and where the bottle ends up relative to the line are exactly
// what the bottle-integration protocol (P-H8) measures against independently
// observed bottle positions. Until then they are placeholders, and a run may not
// be announced on them.
constexpr double kPlannedBottleContactLeadM = 0.0;
constexpr double kPlannedBottleCaptureAdvanceM = 0.0;
constexpr double kPlannedBottlePushMarginM = 0.0;

// The forward execution target's own physical dependencies
// (`ev::RunExecutionDependencies`): how far either side of the nominal contact
// position the bottle's touch may credibly be reported, and how far beyond the
// Bottle Line rule landmark the vehicle's commanded forward stop lies. The second
// is the one fact that establishes the forward execution target, and it is not a
// number anyone has measured: pusher reach, the contact window, capture/transfer
// behaviour, the push margin and braking behaviour all fold into that single
// distance, and the bottle-integration protocol (P-H8) plus a braking
// characterization are what establish it.
//
// Both are placeholders, deliberately, and the consequence is fail-closed:
// `ev::buildRunExecutionPlan()` refuses with `MissingForwardExecutionTarget` while
// the margin is not `Measured`, so a run has no forward execution target to shape
// toward and no forward travel to command. Nothing here may be promoted to
// `Measured` without the recorded measurement, and no value may be guessed in the
// meantime to make a run drive.
constexpr double kPlannedContactWindowToleranceM = 0.0;
constexpr double kPlannedForwardStopMarginBeyondBottleLineM = 0.0;

// Run-policy bounds. Every one of these is a bound the run is allowed to wait
// for evidence under, so a placeholder that is too short turns into a phase
// timeout rather than into motion - which is the safe direction.
constexpr double kPlannedApproachTimeoutS = 20.0;
constexpr double kPlannedCaptureTimeoutS = 5.0;
constexpr double kPlannedPushTimeoutS = 20.0;
constexpr double kPlannedReverseTimeoutS = 15.0;
constexpr double kPlannedFinalApproachTimeoutS = 15.0;
constexpr double kPlannedFinalStopTimeoutS = 10.0;
constexpr double kPlannedFinalApproachDistanceM = 0.3;
constexpr double kPlannedReversalTimeoutS = 3.0;

// The travel-progress bound (S18B): how long a travel phase may go without the
// position it acts on advancing toward the leg's execution target before the run
// faults `TravelStalled` rather than presenting a zero target until its phase
// bound expires. It is a **placeholder** - no run may be armed on it - and the
// measurement that replaces it is a low-speed, loaded trace of the time between
// the shaped target reaching zero (inside the measured brake offset) and the
// vehicle actually coming to rest, plus a conservative margin; it is not a
// number anyone has measured, and it is deliberately not derived from the phase
// bounds, which measure a different thing (waiting for evidence, not advancing).
// Until then it is Provisional, `validateRunPhaseConfig()` refuses the run
// policy, and no run can be announced or armed on it.
constexpr double kPlannedTravelStallTimeoutS = 2.0;

// The feedback-progress bound (S18C/P4): how long accepted powered travel may
// go without the signed tick count advancing in the expected direction before
// the run faults `EncoderFeedbackLost` rather than sustaining a drive request
// on frozen feedback. It is a **placeholder** - no run may be armed on it -
// and the measurement that replaces it is the slowest reliable edge spacing
// under conservative loaded operation (both signs), the startup latency from
// first authorized drive to first edge, and the loaded scheduling gaps, plus a
// conservative margin (P-H5/H7/H9); it is not a number anyone has measured,
// and it is deliberately not derived from the bench stall window, the phase
// bounds, or the brake-entry timeout, which measure different things
// (command servicing, waiting for evidence, coasting to a landmark).
// Until then it is Provisional, `validateRunPhaseConfig()` refuses the run
// policy, and no run can be announced or armed on it.
constexpr double kPlannedEncoderProgressTimeoutS = 0.5;

// The planned polarity fact, in its three encodings, for the one physical
// direction the project cannot name yet (`VERIFIED_STATE.md` #4): which way the
// motor turns for a positive command, which way the decoder counts for forward
// travel, and which driver input means forward. One measurement sets all three,
// and until then every one of them is Provisional - so no driver input
// combination for a drive is ever produced by an authorized command.
constexpr ev::MotionDirection kPlannedPositiveCommandDirection = ev::MotionDirection::Forward;
constexpr ev::MotorMode kPlannedPositiveEffortMode = ev::MotorMode::DriveForward;
constexpr ev::MotionDirection kPlannedPositiveTicksDirection = ev::MotionDirection::Forward;

// **The external DRV8874 nFAULT signal is NOT INSTALLED on this vehicle.** The
// assembled vehicle does not wire the driver's fault output to the Nano, so
// there is no line to measure, no polarity to establish and no external driver
// fault for the MCU to observe. This constant states that, and it is the reason
// the driver-fault configuration check no longer demands a polarity measurement
// on this build.
//
// What this does NOT say: that the driver is unprotected. The DRV8874's on-chip
// overcurrent, thermal and undervoltage protections are independent of this
// signal and are untouched by this change; what is absent is only the MCU's
// ability to *observe* a fault the driver reports. `docs/limitations.md` records
// the consequence - external fault telemetry is unavailable, so no firmware path
// can stop the vehicle for a driver fault on this hardware.
//
// This is a wiring fact stated by the team, not a measurement. It is deliberately
// not a `Sourced` value: there is no `ValueSource` for "stated", and marking it
// `Measured` would fabricate a measurement that does not exist. It is the
// zero-risk direction of the two - `Installed` is the default, so a forgotten
// assignment keeps the line required and unverified, which refuses arming rather
// than silently dropping the gate.
constexpr ev::DriverFaultCapability kPlannedDriverFaultCapability =
    ev::DriverFaultCapability::NotInstalled;

// The reading of the driver's nFAULT line. `Unverified` - the zero value - is
// deliberate and it is **not** changed by the capability above: nobody measured
// the line, and relabelling it `ActiveLow` would be inventing the measurement
// this project's rules exist to prevent. With the line not installed, the
// capability makes this value's verification *not applicable* rather than
// satisfied (`ev::validateDriverFaultConfig()`), so nothing reads it and
// `nfault=NotInstalled` is what telemetry reports.
//
// If the line is ever wired (a later build with the driver's nFAULT brought out),
// the change is two edits: set `kPlannedDriverFaultCapability` back to
// `Installed`, and measure the polarity (H1B) before arming - the configuration
// check refuses the vehicle until that is done.
constexpr ev::DriverFaultPolarity kPlannedDriverFaultPolarity =
    ev::DriverFaultPolarity::Unverified;

// **No battery-voltage sensing is installed on this vehicle.** The assembled V5
// vehicle has no battery divider wired to the Nano, so there is no circuit that
// could ever read the pack voltage: the A1 counts this firmware reports are raw
// ADC counts of an unconnected input, not a voltage
// (`data/evidence/D31-2026-09-29/README.md`, `docs/limitations.md`). This constant
// states that, and it is the reason the arming gate no longer demands a measured
// `minBatteryV` on this build: the requirement is *not applicable* rather than
// outstanding (`ev::validateSafetyLimits()`, `lib/evcore/src/SafetyLimits.h`).
//
// What this does NOT say: that the pack voltage is safe. There is no runtime
// voltage comparison on this project to disable - no divider ratio, shunt value or
// ADC reference relationship is documented, so no raw-to-volts conversion exists -
// and `docs/limitations.md` records the consequence: no firmware path can stop the
// vehicle for a low battery on this hardware, and battery-condition correlations
// cannot be produced.
//
// This is a sensing fact stated by the team, not a measurement, and it is
// deliberately not a `Sourced` value: there is no `ValueSource` for "stated", and
// marking it `Measured` would fabricate a measurement that does not exist. It
// mirrors `kPlannedDriverFaultCapability` (D32). `Installed` is the default, so a
// forgotten assignment keeps the voltage gate *required*, which refuses arming
// while `minBatteryV` is a placeholder rather than silently dropping the gate.
//
// If a battery-sense circuit is ever installed, the change is two edits: set this
// back to `Installed`, and measure the pack so `kPlannedMinBatteryV` can be
// promoted to `Measured` from recorded evidence. The arming gate refuses the
// vehicle until that is done, and the runtime comparison a real gate would need is
// a separate, later change - it is not implied by flipping the capability.
constexpr ev::BatteryVoltageCapability kPlannedBatteryVoltageCapability =
    ev::BatteryVoltageCapability::NotInstalled;

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

// The external nFAULT capability, as the runtime and the configuration check read
// it. Not a `Sourced` value, and that is stated rather than implied: see
// `kPlannedDriverFaultCapability`.
inline ev::DriverFaultCapability plannedDriverFaultCapability() {
    return kPlannedDriverFaultCapability;
}

// The battery-voltage sensing capability, as the configuration check reads it.
// Not a `Sourced` value, and that is stated rather than implied: see
// `kPlannedBatteryVoltageCapability`.
inline ev::BatteryVoltageCapability plannedBatteryVoltageCapability() {
    return kPlannedBatteryVoltageCapability;
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

// One leg's shaping limits, for the direction being travelled.
//
// The two legs carry *different* measured values - the forward and reverse brake
// offsets differ, and so do the two safety velocity ceilings - so the direction is
// an argument here rather than a constant: a caller that packed one leg's values
// by hand could silently attach the forward offset to a reverse leg, and the
// profile is handed one set of limits and cannot tell. Defect fixed in D35: the
// reverse leg used to be built from `kPlannedForwardBrakeOffsetM` as well. The
// selection itself is `ev::brakeOffsetForDirection()` (`MotionProfile.h`), a pure
// function the host test `test_the_builder_selects_the_brake_offset_by_direction`
// exercises with a deliberately unequal fixture pair - the two planned constants
// below are numerically equal, so only an unequal pair can tell the two
// selections apart.
inline ev::MotionProfileLimits profileLimitsForOneDirection(
    ev::MotionDirection direction, ev::Sourced<double> cruiseMps,
    ev::Sourced<double> velocityCeilingMps) {
    return ev::MotionProfileLimits(
        cruiseMps, velocityCeilingMps, provisionalValue(kPlannedAccelMps2),
        provisionalValue(kPlannedDecelMps2),
        ev::brakeOffsetForDirection(direction,
                                    provisionalValue(kPlannedForwardBrakeOffsetM),
                                    provisionalValue(kPlannedReverseBrakeOffsetM)),
        provisionalDirection(kPlannedPositiveCommandDirection));
}

inline ev::DirectionalMotionProfileLimits profileLimits() {
    return ev::DirectionalMotionProfileLimits(
        profileLimitsForOneDirection(ev::MotionDirection::Forward,
                                     provisionalValue(kPlannedCruiseMps),
                                     provisionalValue(kPlannedMaxForwardVelocityMps)),
        profileLimitsForOneDirection(ev::MotionDirection::Reverse,
                                     provisionalValue(kPlannedCruiseMps),
                                     provisionalValue(kPlannedMaxReverseVelocityMps)));
}

// The same limits with the run's *planned* cruise in place of the configured
// strategy cruise: what the vehicle layer presents for an announced run once a
// **validated** physical timing model has chosen a speed (`ev::planRunTiming()`).
// While the model is unvalidated the vehicle layer uses `profileLimits()` and this
// path is not taken at all.
//
// The planned value is derived at the announcement boundary from the announced
// Target Time and limits that are all `Measured` (the estimate refuses otherwise,
// so this path is unreachable on a placeholder). Its provenance is therefore
// `ValueSource::Derived` (D35): a computed planning value, never a measurement
// of the vehicle, with the derivation cited. Nothing may promote it to
// `VERIFIED_STATE.md`, and no calibration gate may treat it as `Measured`.
inline ev::Sourced<double> plannedCruiseValue(double plannedCruiseMps) {
    return ev::Sourced<double>(plannedCruiseMps, ev::ValueSource::Derived,
                               "derived: planRunTiming() from measured limits and the announced "
                               "Target Time (rules2027 8.i)");
}

inline ev::DirectionalMotionProfileLimits profileLimitsWithCruise(double plannedCruiseMps) {
    const ev::Sourced<double> cruise = plannedCruiseValue(plannedCruiseMps);
    return ev::DirectionalMotionProfileLimits(
        profileLimitsForOneDirection(ev::MotionDirection::Forward, cruise,
                                     provisionalValue(kPlannedMaxForwardVelocityMps)),
        profileLimitsForOneDirection(ev::MotionDirection::Reverse, cruise,
                                     provisionalValue(kPlannedMaxReverseVelocityMps)));
}

inline ev::NearZeroConfig reversalStopVerification() {
    return ev::NearZeroConfig(provisionalValue(kPlannedReversalStopThresholdMps),
                              provisionalValue(kPlannedReversalStopDwellS),
                              measuredGapMs(kMaxDwellGapMs, kMaxDwellGapEvidence));
}

// How the forward leg's bottle evidence is derived from the run axis. Every
// distance is a placeholder that must be measured by the bottle-integration
// protocol before a run may be announced; see `kPlannedBottleContactLeadM`.
inline ev::BottleEvidenceConfig bottleEvidenceConfig() {
    return ev::BottleEvidenceConfig(provisionalValue(kPlannedBottleContactLeadM),
                                    provisionalValue(kPlannedBottleCaptureAdvanceM),
                                    provisionalValue(kPlannedBottlePushMarginM));
}

// What the execution plan is built from, besides the announcement and the
// bottle-interaction calibration above: the two physical facts behind the forward
// stop. Both are placeholders, so `buildRunExecutionPlan()` refuses and the run has
// no execution target at all: a run announced on them cannot shape a forward leg
// and cannot claim a forward completion. Since S16 that refusal is also an arming
// precondition in its own right - `arm` is refused with the named
// `ExecutionPlanNotEstablished` refusal while the run's adopted plan is not usable
// - so this configuration cannot produce an armed run even after every other
// configuration check passes. See `kPlannedForwardStopMarginBeyondBottleLineM`.
inline ev::RunExecutionDependencies runExecutionDependencies() {
    return ev::RunExecutionDependencies(provisionalValue(kPlannedContactWindowToleranceM),
                                        provisionalValue(kPlannedForwardStopMarginBeyondBottleLineM));
}

// The delay bound: the event's maximum Target Time, as a rule value (rule 8.i),
// so a run's execution plan can never wait longer than the longest run the event
// can ask for. It bounds the run's own plan; it is never the run's delay. A rule
// citation is not a measurement, and this is not one.
inline ev::Sourced<double> maximumOptionalDelayS() {
    return ev::Sourced<double>(kPlannedMaximumOptionalDelayS, ev::ValueSource::RuleDerived,
                               "rules2027 8.i (Target Time maximum)");
}

inline ev::NearZeroConfig finalStopVerification() {
    return ev::NearZeroConfig(provisionalValue(kPlannedReversalStopThresholdMps),
                              completeStopDwellS(),
                              measuredGapMs(kMaxDwellGapMs, kMaxDwellGapEvidence));
}

inline ev::RunPhaseConfig runPhaseConfig() {
    return ev::RunPhaseConfig(maximumOptionalDelayS(), provisionalValue(kPlannedApproachTimeoutS),
                              provisionalValue(kPlannedCaptureTimeoutS),
                              provisionalValue(kPlannedPushTimeoutS),
                              provisionalValue(kPlannedReverseTimeoutS),
                              provisionalValue(kPlannedFinalApproachTimeoutS),
                              provisionalValue(kPlannedFinalStopTimeoutS),
                               provisionalValue(kPlannedTravelStallTimeoutS),
                               provisionalValue(kPlannedEncoderProgressTimeoutS),
                              provisionalValue(kPlannedFinalApproachDistanceM),
                              finalStopVerification(),
                              provisionalValue(kPlannedMinimumCruiseMps),
                              provisionalValue(kPlannedApproachCeilingMps),
                              provisionalValue(kPlannedCaptureCeilingMps),
                              provisionalValue(kPlannedPushCeilingMps), bottleEvidenceConfig());
}

inline ev::ReversalConfig reversalConfig() {
    return ev::ReversalConfig(reversalStopVerification(),
                              provisionalValue(kPlannedReversalTimeoutS));
}

inline ev::Sourced<ev::MotionDirection> positiveTicksDirection() {
    return provisionalDirection(kPlannedPositiveTicksDirection);
}

// The announced Target Time, as the timing plan reads it: an announced *rule*
// parameter (rule 8.i), bounded and grid-checked by `validateRunConfig()` at the
// announcement boundary, and cited here so the plan's own feasibility arithmetic
// is traceable to the rule that produced it.
inline ev::Sourced<double> announcedTargetTimeS(double targetTimeS) {
    return ev::Sourced<double>(targetTimeS, ev::ValueSource::RuleDerived,
                               "announced Target Time, rules2027 8.i");
}

// Everything the timing plan needs for one announced run, built from the announced
// geometry and the announced Target Time.
//
// The planning ceiling is the *more restrictive* of the two legs' velocity
// ceilings, so a plan that fits is a plan that fits on both legs; the cruise is
// capped by it as well as by the strategy cruise. That conservatism is deliberate:
// a plan built on the forward ceiling alone would be optimistic about the reverse
// leg, and the profile would refuse (or clamp) a cruise above the leg's ceiling.
inline ev::RunTimingPlanInputs runTimingPlanInputs(const ev::RunAxisGeometry& geometry,
                                                  double announcedTargetTimeSValue) {
    const ev::Sourced<double> cruiseCeiling = provisionalValue(kPlannedCruiseMps);
    const ev::Sourced<double> tighterVelocityCeiling =
        kPlannedMaxForwardVelocityMps < kPlannedMaxReverseVelocityMps
            ? provisionalValue(kPlannedMaxForwardVelocityMps)
            : provisionalValue(kPlannedMaxReverseVelocityMps);
    return ev::RunTimingPlanInputs(
        ev::plannedTravelDistanceM(geometry), announcedTargetTimeS(announcedTargetTimeSValue),
        cruiseCeiling, tighterVelocityCeiling, provisionalValue(kPlannedAccelMps2),
        provisionalValue(kPlannedDecelMps2), provisionalValue(kPlannedMinimumCruiseMps),
        // The reversal stop's own dwell is inside Run Time; the final Complete Stop
        // is not (rule 8.p), so only the reversal time is reserved here. It is the
        // same `Sourced` value the sequencer verifies a stop against, from the same
        // builder, so the plan cannot reserve a dwell the run does not hold.
        reversalStopVerification().dwellS, provisionalValue(kPlannedMaxRunDurationS));
}

// ---------------------------------------------------------------------------
// Build identity: the configuration fingerprint (R3/L3)
// ---------------------------------------------------------------------------
//
// `firmwareIdentity()` (below) reports which calibration/configuration this
// image carries, so two images built from the same source with different
// tuning report different identities. The fingerprint is a 32-bit FNV-1a
// fold (`ev::FingerprintFold`) over the VALUE bytes plus the PROVENANCE byte
// of every `Sourced` number the configuration builders produce - the same
// coverage `allPromotedValuesCiteEvidence()` audits - plus the plain policy
// scalars, capabilities and model flag that equally change what the firmware
// does. Evidence pointers are deliberately NOT folded: they are addresses,
// not values, and would make identical builds hash differently.
//
// What this covers, exactly: the 3 calibration values; the 5 safety limits;
// the 3 controller gains plus effort mode, effort ceiling, integral bound
// and step; both profile legs (cruise, ceiling, accel, decel, brake offset,
// direction); both near-zero verifications (threshold, dwell, gap); the
// reversal timeout; all 9 phase timeouts plus final-approach distance,
// minimum cruise, the 3 strategy ceilings and the 3 bottle distances; the 2
// execution-plan dependencies; the 2 rule-derived bounds (complete-stop
// dwell, maximum optional delay); the tick polarity and driver-fault
// polarity; the cadence/staleness policy scalars; the timing-model flag; and
// both sensing capabilities. `kFirmwareConfigSchema` is bumped if this set
// ever changes.
//
// Residual limitations (stated, not fixed): the fingerprint covers values,
// not the toolchain that compiled them, not the platform release, and not
// the evidence the values cite - so it distinguishes configurations, never
// binaries. A revision still names the source, never the flashed image.
namespace identity {

inline void mixSourced(ev::FingerprintFold& fold, const ev::Sourced<double>& value) {
    fold.mixDouble(value.value);
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixSourced(ev::FingerprintFold& fold, const ev::Sourced<uint16_t>& value) {
    fold.mixUint16(value.value);
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixSourced(ev::FingerprintFold& fold, const ev::Sourced<uint32_t>& value) {
    fold.mixUint32(value.value);
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixDirection(ev::FingerprintFold& fold,
                         const ev::Sourced<ev::MotionDirection>& value) {
    fold.mixUint8(static_cast<uint8_t>(value.value));
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixEffortMode(ev::FingerprintFold& fold,
                          const ev::Sourced<ev::MotorMode>& value) {
    fold.mixUint8(static_cast<uint8_t>(value.value));
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixFaultPolarity(ev::FingerprintFold& fold,
                             const ev::Sourced<ev::DriverFaultPolarity>& value) {
    fold.mixUint8(static_cast<uint8_t>(value.value));
    fold.mixUint8(static_cast<uint8_t>(value.source));
}

inline void mixLimits(ev::FingerprintFold& fold, const ev::MotionProfileLimits& limits) {
    mixSourced(fold, limits.cruiseVelocityMps);
    mixSourced(fold, limits.velocityCeilingMps);
    mixSourced(fold, limits.accelLimitMps2);
    mixSourced(fold, limits.decelLimitMps2);
    mixSourced(fold, limits.brakeOffsetM);
    mixDirection(fold, limits.positiveCommandDirection);
}

inline void mixNearZero(ev::FingerprintFold& fold, const ev::NearZeroConfig& config) {
    mixSourced(fold, config.velocityThresholdMps);
    mixSourced(fold, config.dwellS);
    mixSourced(fold, config.maxUpdateGapMs);
}

}  // namespace identity

// The fingerprint this build's calibration/configuration folds to.
// Computed on demand (microseconds at boot-line rates), so it is always the
// values in force rather than a build-time snapshot that could go stale.
inline uint32_t calibrationFingerprint() {
    ev::FingerprintFold fold;
    const ev::VehicleCalibration calibrationValues = calibration();
    identity::mixSourced(fold, calibrationValues.distancePerEncoderTickM);
    identity::mixSourced(fold, calibrationValues.forwardBrakeOffsetM);
    identity::mixSourced(fold, calibrationValues.reverseBrakeOffsetM);

    const ev::SafetyLimits limits = safetyLimits();
    identity::mixSourced(fold, limits.maxEffortPermille);
    identity::mixSourced(fold, limits.maxForwardVelocityMps);
    identity::mixSourced(fold, limits.maxReverseVelocityMps);
    identity::mixSourced(fold, limits.maxRunDurationS);
    identity::mixSourced(fold, limits.minBatteryV);

    const ev::VelocityControllerConfig controller = controllerConfig();
    identity::mixSourced(fold, controller.feedForwardPermillePerMps);
    identity::mixSourced(fold, controller.proportionalPermillePerMps);
    identity::mixSourced(fold, controller.integralPermillePerMpsSecond);
    identity::mixEffortMode(fold, controller.positiveEffortMode);
    identity::mixSourced(fold, controller.maxEffortPermille);
    fold.mixUint16(controller.maxIntegralPermille);
    fold.mixDouble(controller.maxIntegrationStepS);

    const ev::DirectionalMotionProfileLimits profile = profileLimits();
    identity::mixLimits(fold, profile.forward);
    identity::mixLimits(fold, profile.reverse);

    const ev::NearZeroConfig reversalStop = reversalStopVerification();
    const ev::NearZeroConfig finalStop = finalStopVerification();
    identity::mixNearZero(fold, reversalStop);
    identity::mixNearZero(fold, finalStop);

    const ev::RunPhaseConfig phase = runPhaseConfig();
    identity::mixSourced(fold, phase.maximumOptionalDelayS);
    identity::mixSourced(fold, phase.approachTimeoutS);
    identity::mixSourced(fold, phase.captureTimeoutS);
    identity::mixSourced(fold, phase.pushTimeoutS);
    identity::mixSourced(fold, phase.reverseTimeoutS);
    identity::mixSourced(fold, phase.finalApproachTimeoutS);
    identity::mixSourced(fold, phase.finalStopTimeoutS);
    identity::mixSourced(fold, phase.travelStallTimeoutS);
    identity::mixSourced(fold, phase.encoderProgressTimeoutS);
    identity::mixSourced(fold, phase.finalApproachDistanceM);
    identity::mixSourced(fold, phase.minimumPlannedCruiseMps);
    identity::mixSourced(fold, phase.approachCeilingMps);
    identity::mixSourced(fold, phase.captureCeilingMps);
    identity::mixSourced(fold, phase.pushCeilingMps);
    identity::mixSourced(fold, phase.bottleEvidence.contactLeadDistanceM);
    identity::mixSourced(fold, phase.bottleEvidence.captureAdvanceDistanceM);
    identity::mixSourced(fold, phase.bottleEvidence.pushMarginDistanceM);

    const ev::ReversalConfig reversal = reversalConfig();
    identity::mixSourced(fold, reversal.reversalTimeoutS);

    const ev::RunExecutionDependencies planDependencies = runExecutionDependencies();
    identity::mixSourced(fold, planDependencies.contactWindowToleranceM);
    identity::mixSourced(fold, planDependencies.forwardStopMarginBeyondBottleLineM);

    identity::mixDirection(fold, positiveTicksDirection());
    identity::mixFaultPolarity(fold, plannedDriverFaultPolarity());
    fold.mixUint8(static_cast<uint8_t>(plannedDriverFaultCapability()));
    fold.mixUint8(static_cast<uint8_t>(plannedBatteryVoltageCapability()));

    const ev::VelocityEstimatorConfig estimator = estimatorConfig();
    fold.mixUint32(estimator.windowMs);
    fold.mixUint32(estimator.noEdgeTimeoutMs);
    fold.mixUint32(kControlPeriodMs);
    fold.mixUint32(kMaxSampleAgeMs);
    fold.mixUint32(kStopEvidenceMaxAgeMs);
    fold.mixUint8(kRunTimingModelValidated ? 1U : 0U);
    return fold.done();
}

// The complete device-reported build identity: family, source revision,
// dirty state, image variant and configuration fingerprint. Rendered by
// `VERSION`/`BOOT` through `ev::appendFirmwareIdentity()`.
inline ev::FirmwareIdentity firmwareIdentity() {
    ev::FirmwareIdentity identity;
    identity.family = ev::kFirmwareFamily;
    identity.revision = buildRevision();
    identity.dirty = buildIsDirty();
    identity.variant = firmwareVariant();
    identity.configFingerprint = calibrationFingerprint();
    identity.configSchema = ev::kFirmwareConfigSchema;
    return identity;
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
    // The execution plan's own two dependencies (audit S13R M-2): they establish
    // the forward execution target the plan is built from, so a promotion that
    // turns either of them `Measured` has to name its evidence exactly like every
    // other promoted value. They travel with the rest of the aggregate.
    const ev::RunExecutionDependencies planDependencies = runExecutionDependencies();

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
    const bool phaseCited = provenance::cited(phase.maximumOptionalDelayS) &&
                            provenance::cited(phase.approachTimeoutS) &&
                            provenance::cited(phase.captureTimeoutS) &&
                            provenance::cited(phase.pushTimeoutS) &&
                            provenance::cited(phase.reverseTimeoutS) &&
                            provenance::cited(phase.finalApproachTimeoutS) &&
                            provenance::cited(phase.finalStopTimeoutS) &&
                            provenance::cited(phase.travelStallTimeoutS) &&
                            provenance::cited(phase.encoderProgressTimeoutS) &&
                            provenance::cited(phase.finalApproachDistanceM) &&
                            provenance::cited(phase.minimumPlannedCruiseMps) &&
                            provenance::cited(phase.approachCeilingMps) &&
                            provenance::cited(phase.captureCeilingMps) &&
                            provenance::cited(phase.pushCeilingMps) &&
                            provenance::cited(phase.bottleEvidence.contactLeadDistanceM) &&
                            provenance::cited(phase.bottleEvidence.captureAdvanceDistanceM) &&
                            provenance::cited(phase.bottleEvidence.pushMarginDistanceM);
    const bool planDependenciesCited =
        provenance::cited(planDependencies.contactWindowToleranceM) &&
        provenance::cited(planDependencies.forwardStopMarginBeyondBottleLineM);

    return calibrationCited && limitsCited && controllerCited && polarityCited && profileCited &&
           stopCited && phaseCited && planDependenciesCited;
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
// `configurationValid` is false because no run has been announced. One of the
// checks is about a fact rather than a value and is false for its own reason:
// `measuredValuesCiteEvidence` will bite on the first promotion that names no
// evidence. The driver-fault check is *not* one of the failing ones on this
// vehicle: its external nFAULT line is not installed, so its polarity
// verification is not applicable rather than outstanding
// (`driverFaultConfigurationValid`).
//
// `safetyLimitsValid` is capability-aware about the battery voltage on this
// vehicle for the same reason: `kPlannedBatteryVoltageCapability` is
// `NotInstalled`, so a measured `minBatteryV` is not a condition of this build
// and that one requirement is not applicable (`validateSafetyLimits()`,
// `lib/evcore/src/SafetyLimits.h`). The check still fails, and for a reason this
// vehicle *can* address: `maxEffortPermille`, both velocity ceilings and the
// maximum run duration are still placeholders, so no run may be armed until they
// are measured. Making the battery requirement conditional therefore removes a
// gate this vehicle could never satisfy without fabricating a measurement; it
// removes no gate the vehicle could meet.
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
    checks.driverFaultConfigurationValid =
        ev::validateDriverFaultConfig(plannedDriverFaultCapability(),
                                      plannedDriverFaultPolarity())
            .ok;
    checks.effortCeilingsAgree =
        ev::effortCeilingsAgree(controller.maxEffortPermille, limits.maxEffortPermille);
    checks.commandPolarityAgrees =
        ev::polarityAgrees(profile.forward.positiveCommandDirection, controller.positiveEffortMode);
    checks.tickPolarityAgrees = ev::tickPolarityAgrees(positiveTicksDirection(),
                                                       profile.forward.positiveCommandDirection);
    // The packet pairs the current claim - which appears only after the
    // estimator's own no-edge interval - with the retained window-derived
    // speed, so the limit must span the claim lead plus the dwell, not the
    // dwell alone (S18A/P5).
    checks.stopEvidenceSpansReversalStop = ev::stopEvidenceSpansClaimAndDwell(
        kStopEvidenceMaxAgeMs, kEstimatorNoEdgeTimeoutMs, reversal.verification);
    checks.stopEvidenceSpansFinalStop = ev::stopEvidenceSpansClaimAndDwell(
        kStopEvidenceMaxAgeMs, kEstimatorNoEdgeTimeoutMs, phase.stopVerification);
    checks.safetyLimitsValid =
        ev::validateSafetyLimits(limits, plannedBatteryVoltageCapability()).ok;
    checks.controllerConfigValid = ev::validateVelocityControllerConfig(controller).ok;
    // The estimator window must both precede the no-edge claim (checked inside
    // `validateVelocityEstimatorConfig()`) and be coverable by the retained
    // history at the control cadence (S18A/P5).
    checks.estimatorConfigValid =
        ev::validateVelocityEstimatorConfig(estimatorConfig()).ok &&
        ev::estimatorWindowCoverableAtCadence(kEstimatorWindowMs, kControlPeriodMs);
    checks.profileLimitsValid = ev::validateMotionProfileLimits(profile.forward).ok &&
                                ev::validateMotionProfileLimits(profile.reverse).ok &&
                                ev::directionalCommandPolarityAgrees(
                                    profile.forward.positiveCommandDirection,
                                    profile.reverse.positiveCommandDirection);
    checks.phaseConfigValid = ev::validateRunPhaseConfig(phase).ok;
    checks.reversalConfigValid = ev::validateReversalConfig(reversal).ok;
    checks.runPolicyUsable = run.configUsable();
    return checks;
}

}  // namespace vehicleConfig
}  // namespace evhw
