#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <CompetitionStateMachine.h>
#include <Motion.h>
#include <MotionChainReset.h>
#include <MotionProfile.h>
#include <Odometry.h>
#include <QuadratureDecoder.h>
#include <ReversalSequencer.h>
#include <RunAxis.h>
#include <RunConfig.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <Snapshots.h>
#include <Units.h>
#include <VehicleCalibration.h>
#include <VehicleState.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

// ---------------------------------------------------------------------------
// Scripted end-to-end scenario harness - test support, not production code
// ---------------------------------------------------------------------------
//
// What this file is
//
// * The vehicle layer's obligations, written down once, so the scenarios can
//   drive the real production chain with stated evidence instead of hand-built
//   module calls:
//
//     stated cycle facts
//         -> QuadratureDecoder           (production decoding)
//         -> distanceTravelledM          (production tick-to-distance arithmetic)
//         -> VelocityEstimator           (production window estimation)
//         -> odometryReadiness/velocityReadiness (production freshness verdicts)
//         -> the stop-verification packet (the current claim paired with the last
//            window-derived estimate)
//         -> CompetitionStateMachine     (production phase sequence)
//         -> MotionProfile               (production target shaping)
//         -> VelocityController          (production effort)
//         -> SafetyAuthority             (the only authorizer)
//         -> AuthorizedMotorCommand      (recorded at the actuator boundary)
//
// * Deliberately not a simulator. Nothing here integrates an effort into a
//   velocity, and nothing models torque, friction, braking distance, bottle
//   dynamics, wheel slip or sensor noise. Every encoder reading is prescribed by
//   the scenario, never derived from a commanded effort, so a scenario asserts
//   what the logic does with stated evidence - never that a vehicle would move,
//   stop, push a bottle or reach a phase in the real world. Every row in
//   VERIFIED_STATE.md stays UNVERIFIED; a passing scenario is software
//   verification only.
//
// What this file is not
//
// * Not production code. It lives in test/ and is never compiled into the
//   firmware. It exists because the vehicle layer that will own these wiring
//   obligations does not exist yet (SOFTWARE_ARCHITECTURE.md sections 13-14;
//   DEVELOPMENT_LOG.md D10 fix 5), so the obligations are exercised here first.
// * Not a second implementation of any decision. Every phase, intention, fault,
//   target, effort, authorization and stop verdict below is produced by
//   lib/evcore. The harness only states evidence and presents requests.
//
// The vehicle-layer obligations this harness implements
//
//   1. exactly one request to SafetyAuthority::authorize() per cycle, and the
//      single authorized command is the only thing that reaches the actuator
//      boundary;
//   2. no run-originated request is presented unless
//      ev::runIntentionIsCurrentFor() says the intention is current for that
//      cycle's clock reading;
//   3. every run stop command maps onto a non-powered request through
//      ev::runStopEffort(), and its leg tag is never read as a powered
//      direction;
//   4. the run's verified-stop verdict is what is mapped onto
//      SafetySystemState::atZeroSpeed;
//   5. an observed fault is reported to both the run policy and the authority in
//      the same cycle, and a fault the run has already latched keeps being
//      reported;
//   6. the configuration-build checks run before arming and an incomplete
//      configuration cannot arm: effort ceilings, the two polarity agreements
//      (`ev::polarityAgrees()`, `ev::tickPolarityAgrees()`), the stop-evidence
//      limit spanning both stop dwells (`ev::stopEvidenceLimitSpansDwell()`) and
//      every per-module validation;
//   7. a travel request is only presented as a *powered* request once the shaped
//      target actually points along the leg the run asked for
//      (`ev::targetVelocityPointsAlongLeg()`). The profile deliberately ramps a
//      previous direction down before it flips the sign
//      (`MotionProfileStatus::DirectionChangeHold`), so a travel command that
//      arrives while the shape is still on the old side is presented as no
//      powered effort at all. Presenting the regulator's effort there would ask
//      for powered effort in the direction the run has just stopped travelling
//      in - and the authority would have nothing to refuse it with, because its
//      direction latch was legitimately cleared by the verified stop that earned
//      the reversal. Pinned by `expectEveryDriveFollowsTheRunLeg`;
//   8. the stop-verification packet is composed from the two production
//      estimator answers by `ev::composeStopEvidence()`, and the run axis -
//      announced geometry, polarity-applied position, and the two remaining
//      distances - is computed by `RunAxis.h`.
//
// Where those rules live
//
//   Every rule above is a production decision with a production owner, and this
//   harness calls the owner rather than re-deriving it:
//
//     ev::composeStopEvidence()              NearZeroVerifier.h
//     ev::stopEvidenceLimitSpansDwell()      NearZeroVerifier.h
//     ev::targetVelocityPointsAlongLeg()     MotionProfile.h
//     ev::runStopEffort()                    CompetitionStateMachine.h
//     ev::runCommandNamesForwardLeg()        CompetitionStateMachine.h
//     ev::RunAxisGeometry / runAxisPositionM()
//     ev::remainingToTargetM() / legRemainingM()   RunAxis.h
//     ev::tickPolarityAgrees()               SafetyLimits.h
//
//   A change to any of those decisions changes the scenarios with it, which is
//   why they are not written down a second time here.
//
// How the stop-verification packet is composed
//
//   `NearZeroVerifier` needs three facts at once: a fresh, valid speed estimate,
//   a speed within its threshold, and the estimator's explicit zero-speed claim.
//   The estimator never reports those in one answer - a window-derived number
//   carries `zeroSpeedClaimed == false`, and the claim carries `valid == false` -
//   so the packet is composed from two production answers: the claim of the
//   current cycle, and the last window-derived estimate, aged against a limit
//   that is deliberately at least as long as the stop dwell. The zero the packet
//   rests on is therefore always produced before the claim interval it is paired
//   with, and on a dropout the claim expires through the ordinary freshness
//   verdict like every other sample. Both facts and both limits are production
//   outputs and caller policy - nothing here invents a speed - and the
//   composition itself is `ev::composeStopEvidence()`, unit-tested in
//   `test_near_zero_verifier` and pinned end to end by
//   `test_the_stop_evidence_packet_is_composed_from_two_production_answers`.
//
// What a scenario may inject, and why
//
//   A scenario states evidence; a few named hooks let it state a fault the real
//   vehicle layer could see, or a caller bug it must survive:
//
//     * a sensor dropout (no sample this cycle) and prescribed raw channel
//       levels, so duplicate, B-only and double-channel samples reach the real
//       decoder instead of a clean Gray cycle only;
//     * a scripted clock: a stalled tick (0 ms), uneven steps, or a rewind;
//     * a held intention, i.e. an intention kept from an earlier cycle and
//       presented again - no run-originated *powered* request may come of it;
//     * a hostile request, which deliberately bypasses the vehicle layer's own
//       gates and presents a chosen request to the authority, so the authority's
//       independent refusal is exercised rather than the harness's gate.
//
//   The last two are never used in a normal scenario. They exist so the gates
//   that cannot be observed otherwise are not dead code: a scenario using them
//   asserts what refusal the chain produces, not that the harness could produce
//   one.

namespace evscenario {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
//
// Every number below is a test fixture. None is a measurement, a tuning value, a
// rule value or a placeholder any firmware path may use. They are marked
// `ValueSource::Measured` only so the production configuration validators accept
// them: the production types have no unmeasured path to a working chain, which
// is exactly the contract (SOFTWARE_ARCHITECTURE.md section 7).

constexpr uint32_t kFixtureControlPeriodMs = 20U;      // 50 Hz control loop
constexpr uint32_t kFixtureMaxSampleAgeMs = 60U;       // three control cycles
constexpr int32_t kFixtureTicksPerCycle = 200;         // 10 000 ticks/s
constexpr double kFixtureDistancePerTickM = 0.0001;    // hence 1.0 m/s
constexpr uint32_t kFixtureEstimatorWindowMs = 20U;
constexpr uint32_t kFixtureEstimatorNoEdgeTimeoutMs = 100U;

constexpr double kFixtureCruiseMps = 1.0;
constexpr double kFixtureVelocityCeilingMps = 1.2;
constexpr double kFixtureAccelMps2 = 1.0;
constexpr double kFixtureDecelMps2 = 2.0;
constexpr double kFixtureBrakeOffsetM = 0.05;

constexpr double kFixtureFeedForwardPermillePerMps = 300.0;
constexpr double kFixtureProportionalPermillePerMps = 200.0;
constexpr double kFixtureIntegralPermillePerMpsSecond = 100.0;
constexpr uint16_t kFixtureMaxEffortPermille = 600U;
constexpr uint16_t kFixtureMaxIntegralPermille = 300U;
constexpr double kFixtureMaxIntegrationStepS = 0.05;

// Long enough that only a scenario that deliberately starves a phase reaches
// them; the nominal run spends about 16 s of scripted time in the whole
// sequence at the fixture rate, so these bounds are runway, not policy.
constexpr double kFixtureOptionalDelayS = 0.1;
constexpr double kFixtureApproachTimeoutS = 5.0;
constexpr double kFixtureCaptureTimeoutS = 2.0;
constexpr double kFixturePushTimeoutS = 12.0;
constexpr double kFixtureReverseTimeoutS = 10.0;
constexpr double kFixtureFinalApproachTimeoutS = 10.0;
constexpr double kFixtureFinalStopTimeoutS = 1.0;
constexpr double kFixtureFinalApproachDistanceM = 0.5;

constexpr double kFixtureStopThresholdMps = 0.05;
constexpr double kFixtureStopDwellS = 0.1;
constexpr double kFixtureReversalTimeoutS = 1.0;

// The fixture limit on the gap between two updates that may still be credited as
// continued near-zero evidence. Wide relative to the scenario cadence, because
// these scenarios are about the chain rather than about the gap rule; the rule
// itself is tested in `test_near_zero_verifier`.
constexpr uint32_t kFixtureMaxUpdateGapMs = 250U;

// The staleness limit for the estimate the stop-verification packet pairs with
// the current zero-speed claim. Deliberately longer than the stop dwell: the
// window-derived zero is always produced before the claim interval it is paired
// with, so a limit shorter than the dwell could never let the two coincide.
constexpr uint32_t kFixtureStopEvidenceMaxAgeMs = 200U;

// How far past the bottle the vehicle must travel before the scenario reports
// the capture complete. The mechanism does not exist, so this stands in for
// evidence the real vehicle layer will have to produce (VERIFIED_STATE.md #11).
constexpr double kFixtureCaptureTravelM = 0.1;

inline ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

inline ev::Sourced<uint16_t> measuredPermille(uint16_t value) {
    return ev::Sourced<uint16_t>(value, ev::ValueSource::Measured);
}

inline ev::Sourced<ev::MotionDirection> measuredPositiveDirection(ev::MotionDirection direction) {
    return ev::Sourced<ev::MotionDirection>(direction, ev::ValueSource::Measured);
}

inline ev::Sourced<ev::MotorMode> measuredPositiveMode(ev::MotorMode mode) {
    return ev::Sourced<ev::MotorMode>(mode, ev::ValueSource::Measured);
}

// The fixture stop-verification policy, in one place: threshold, dwell and the
// fixture gap limit. A new configuration field has to be added here rather than
// at each of the two call sites below.
inline ev::NearZeroConfig fixtureStopConfig() {
    return ev::NearZeroConfig(measuredValue(kFixtureStopThresholdMps),
                              measuredValue(kFixtureStopDwellS),
                              ev::Sourced<uint32_t>(kFixtureMaxUpdateGapMs,
                                                    ev::ValueSource::Measured));
}

// The single fixture set every scenario uses.
struct ScenarioFixtures {
    ScenarioFixtures(uint32_t controlPeriodMs_, uint32_t maxSampleAgeMs_,
                     uint32_t stopEvidenceMaxAgeMs_, ev::VehicleCalibration calibration_,
                     ev::SafetyLimits limits_,
                     ev::VelocityEstimatorConfig estimator_,
                     ev::VelocityControllerConfig controller_,
                     ev::DirectionalMotionProfileLimits profile_, ev::RunPhaseConfig phase_,
                     ev::ReversalConfig reversal_,
                     ev::Sourced<ev::MotionDirection> positiveTicksDirection_)
        : controlPeriodMs(controlPeriodMs_),
          maxSampleAgeMs(maxSampleAgeMs_),
          stopEvidenceMaxAgeMs(stopEvidenceMaxAgeMs_),
          calibration(calibration_),
          limits(limits_),
          estimator(estimator_),
          controller(controller_),
          profile(profile_),
          phase(phase_),
          reversal(reversal_),
          positiveTicksDirection(positiveTicksDirection_) {}

    // Milliseconds between control cycles.
    uint32_t controlPeriodMs;

    // The caller's staleness limit for the control path's readiness verdicts.
    // `sampleAge()` deliberately has no default, so the limit is caller policy,
    // and this is fixture policy.
    uint32_t maxSampleAgeMs;

    // The caller's staleness limit for the stop-verification packet's speed
    // fact. A separate limit because it answers a different question: not "is
    // this speed current enough to regulate on" but "was a zero observed within
    // the interval the claim covers".
    uint32_t stopEvidenceMaxAgeMs;

    ev::VehicleCalibration calibration;
    ev::SafetyLimits limits;
    ev::VelocityEstimatorConfig estimator;
    ev::VelocityControllerConfig controller;

    // Both legs are built from the same fixture polarity, so the run cannot be
    // handed one leg's values for the other and `polarityAgrees()` holds.
    ev::DirectionalMotionProfileLimits profile;
    ev::RunPhaseConfig phase;
    ev::ReversalConfig reversal;

    // The fixture polarity fact (VERIFIED_STATE.md #4 is UNVERIFIED): here the
    // decoder's Positive tick convention is prescribed to correspond to run-axis
    // forward travel. It is the third encoding of the one polarity fact, and
    // `ev::runAxisPositionM()` is where it is applied - once.
    ev::Sourced<ev::MotionDirection> positiveTicksDirection;
};

inline ScenarioFixtures nominalFixtures() {
    const ev::MotionProfileLimits forward(
        measuredValue(kFixtureCruiseMps), measuredValue(kFixtureVelocityCeilingMps),
        measuredValue(kFixtureAccelMps2), measuredValue(kFixtureDecelMps2),
        measuredValue(kFixtureBrakeOffsetM),
        measuredPositiveDirection(ev::MotionDirection::Forward));
    const ev::MotionProfileLimits reverse(
        measuredValue(kFixtureCruiseMps), measuredValue(kFixtureVelocityCeilingMps),
        measuredValue(kFixtureAccelMps2), measuredValue(kFixtureDecelMps2),
        measuredValue(kFixtureBrakeOffsetM),
        measuredPositiveDirection(ev::MotionDirection::Forward));

    return ScenarioFixtures(
        kFixtureControlPeriodMs, kFixtureMaxSampleAgeMs, kFixtureStopEvidenceMaxAgeMs,
        ev::VehicleCalibration{measuredValue(kFixtureDistancePerTickM),
                               measuredValue(kFixtureBrakeOffsetM),
                               measuredValue(kFixtureBrakeOffsetM)},
        ev::SafetyLimits{measuredPermille(kFixtureMaxEffortPermille),
                         measuredValue(kFixtureVelocityCeilingMps),
                         measuredValue(kFixtureVelocityCeilingMps),
                         measuredValue(60.0), measuredValue(9.0)},
        ev::VelocityEstimatorConfig{kFixtureEstimatorWindowMs,
                                    kFixtureEstimatorNoEdgeTimeoutMs},
        ev::VelocityControllerConfig(
            measuredValue(kFixtureFeedForwardPermillePerMps),
            measuredValue(kFixtureProportionalPermillePerMps),
            measuredValue(kFixtureIntegralPermillePerMpsSecond),
            measuredPositiveMode(ev::MotorMode::DriveForward),
            measuredPermille(kFixtureMaxEffortPermille), kFixtureMaxIntegralPermille,
            kFixtureMaxIntegrationStepS),
        ev::DirectionalMotionProfileLimits(forward, reverse),
        ev::RunPhaseConfig(measuredValue(kFixtureOptionalDelayS),
                           measuredValue(kFixtureApproachTimeoutS),
                           measuredValue(kFixtureCaptureTimeoutS),
                           measuredValue(kFixturePushTimeoutS),
                           measuredValue(kFixtureReverseTimeoutS),
                           measuredValue(kFixtureFinalApproachTimeoutS),
                           measuredValue(kFixtureFinalStopTimeoutS),
                           measuredValue(kFixtureFinalApproachDistanceM),
                           fixtureStopConfig()),
        ev::ReversalConfig(fixtureStopConfig(), measuredValue(kFixtureReversalTimeoutS)),
        measuredPositiveDirection(ev::MotionDirection::Forward));
}

// The valid announced run every scenario starts from: Regional level, 5.00 m
// Target Distance, 10.0 s Target Time, 1.00 m Bottle Line Distance and the
// Regional bottle placement of 2.0 m (rules 8.h, 8.i, 8.j and 7.c.ii.(5)).
inline ev::RunConfig validRunConfig() {
    return ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
}

// ---------------------------------------------------------------------------
// The scripted cycle and its record
// ---------------------------------------------------------------------------

// Everything the harness is *told* this cycle. Nothing here is a decision and
// nothing is derived from a commanded effort: the encoder advance, the trigger,
// the bottle evidence and the observed fault are stated facts about what the
// (non-existent) vehicle layer saw. Everything else - ticks, distance, speed,
// claim, readiness - is produced by the chain.
struct CycleScript {
    // Prescribed signed encoder advance for this cycle, in the decoder's tick
    // convention. The real decoder counts these edges; the harness never writes
    // a tick count directly.
    int32_t encoderAdvanceTicks = 0;

    // False means no encoder sample arrived this cycle at all - a dropout, which
    // is a different fact from an observed non-change (advance 0). A dropout
    // refreshes nothing, so the readiness verdicts age the last sample.
    bool sensorSampleFresh = true;

    // Raw channel levels the scenario prescribes directly, bypassing the Gray
    // cycle. One update() reaches the production decoder, which decides what the
    // sample counts: a duplicate or B-only sample is ignored, an A transition is
    // counted from its landing state. Unset means the cycle drives the decoder
    // through the Gray cycle to `encoderAdvanceTicks` instead.
    bool hasRawLevels = false;
    bool rawLevelA = false;
    bool rawLevelB = false;

    // The clock reading the vehicle layer sees this cycle. Unset means the fixture
    // control period passed since the previous cycle. Set it and state the advance
    // in milliseconds: 0 is a stalled clock, an uneven value is a jittered loop,
    // and a negative value rewinds the stamp the way a reset clock would. The
    // chain's response to that evidence is what a scenario using this asserts.
    bool scriptedClock = false;
    int32_t clockAdvanceMs = 0;

    // A run intention the caller kept from an earlier cycle and presents again,
    // instead of the run's own current intention. This exists so obligation 2 can
    // be *exercised*: presenting a stale Travel intention must produce no powered
    // request. Never set in a scenario that is not about that.
    const ev::RunMotionIntention* heldIntention = nullptr;

    // A request presented to the safety boundary in place of the vehicle layer's
    // own, deliberately bypassing obligations 2 and 7 so the authority's
    // independent refusal is what answers it. Never set in a normal scenario.
    bool hasHostileRequest = false;
    ev::MotorEffort hostileRequest{ev::MotorMode::Disabled, 0U};

    bool startTriggered = false;
    bool bottleContactDetected = false;
    bool captureComplete = false;
    bool pushComplete = false;

    // The stop-verification packet is deliberately not stated here: it is
    // composed from the estimator's own answers (see the header), so a scenario
    // says how the encoder moved and nothing more.

    // A fault the vehicle layer observed this cycle (None when healthy). It is
    // reported to the run policy and to the safety authority in the same cycle.
    ev::FaultCode fault = ev::FaultCode::None;
};

// One cycle's full record: what was stated, what the chain decided, what was
// presented, and what reached the boundary. Zero-valued on construction, so a
// fresh record claims nothing.
struct CycleRecord {
    uint32_t nowMs = 0U;
    uint32_t dtMs = 0U;

    // Stated.
    bool sensorSampleFresh = true;
    int32_t encoderAdvanceTicks = 0;

    // The run policy's decision. `intention` is always the run's own intention
    // for this cycle - a scenario that presents a held one records it separately,
    // so the run's own record stays true to what the run said.
    ev::RunPhase phase = ev::RunPhase::Idle;
    ev::RunMotionIntention intention{ev::RunMotionCommand::None, 0U};
    ev::RunMotionCommand command = ev::RunMotionCommand::None;

    // The request source this cycle: the run's own intention, or a caller-held
    // one a scenario presented instead (CycleScript::heldIntention).
    ev::RunMotionIntention presentedIntention{ev::RunMotionCommand::None, 0U};

    // True when the presented intention was current for this cycle's clock
    // reading. A false here is a caller that held an intention too long, and it
    // is what the vehicle layer's freshness gate must refuse.
    bool intentionCurrent = false;

    ev::ReversalPhase reversalPhase = ev::ReversalPhase::Idle;
    bool stopVerified = false;
    ev::FaultCode faultOfRecord = ev::FaultCode::None;
    uint32_t phaseElapsedMs = 0U;

    // Measurements and readiness (production verdicts; the age limits are
    // fixtures).
    int32_t encoderTicks = 0;
    double runAxisPositionM = 0.0;
    double remainingToTargetM = 0.0;
    double legRemainingM = 0.0;

    // The three readiness facts behind the position: the odometry verdict
    // (validity and freshness), the run-axis conversion's own verdict (the
    // tick-sign polarity), and the combined position verdict the boundary and
    // the run policy are given.
    bool odometryUsable = false;
    bool runAxisUsable = false;
    bool positionUsable = false;
    double measuredVelocityMps = 0.0;
    bool velocityUsable = false;
    ev::VelocityEstimateStatus velocityStatus = ev::VelocityEstimateStatus::WarmingUp;

    // The authority's inputs exactly as they were presented.
    bool authorityArmed = false;
    bool readinessComplete = false;
    bool requiredDataFresh = false;
    bool atZeroSpeed = false;
    ev::FaultCode reportedFault = ev::FaultCode::None;

    // The single request presented this cycle and the boundary's verdict.
    ev::MotorEffort presentedEffort{ev::MotorMode::Disabled, 0U};
    ev::AuthzOutcome outcome = ev::AuthzOutcome::DeniedNotArmed;

    // Whether the regulator produced the request, and why. `controlStatus` is
    // meaningful only when `regulatorStepped` is true: a stop cycle deliberately
    // does not step the regulator, and neither does a gate refusal.
    bool regulatorStepped = false;
    ev::VelocityControlStatus controlStatus = ev::VelocityControlStatus::Disabled;

    // What reached the actuator boundary.
    ev::MotorMode commandedMode = ev::MotorMode::Disabled;
    uint16_t commandedPermille = 0U;
    ev::ActuationClass commandedClass = ev::ActuationClass::Disabled;
    ev::PoweredDirection commandedDirection = ev::PoweredDirection::None;

    // Audit counters as they stood *before* this cycle's command was applied:
    // how many non-powered stop cycles at a reported zero, and whether a
    // verified stop was presented, since the last powered drive.
    uint32_t stopCyclesAtZeroSinceLastDrive = 0U;
    bool verifiedStopSinceLastDrive = false;
};

// One command that reached the actuator boundary. Exactly one is recorded per
// cycle, in cycle order, so the cycle's own stamp is the record's stamp.
struct ActuatorRecord {
    ev::MotorMode mode = ev::MotorMode::Disabled;
    uint16_t magnitudePermille = 0U;
    ev::ActuationClass actuationClass = ev::ActuationClass::Disabled;
    ev::PoweredDirection poweredDirection = ev::PoweredDirection::None;
};

// The actuator boundary.
//
// This models the boundary's *type*, not an actuator: the only thing it can
// accept is an `AuthorizedMotorCommand`, which only `SafetyAuthority` can
// construct. It records what the firmware had permission to do and performs no
// physical action of any kind.
class ActuatorBoundary {
public:
    void apply(const ev::AuthorizedMotorCommand& command) {
        ActuatorRecord record;
        record.mode = command.mode();
        record.magnitudePermille = command.magnitudePermille();
        record.actuationClass = command.actuationClass();
        record.poweredDirection = command.poweredDirection();
        records_.push_back(record);
    }

    const std::vector<ActuatorRecord>& records() const { return records_; }

    void clear() { records_.clear(); }

private:
    std::vector<ActuatorRecord> records_;
};

// Pinned at compile time: nothing outside the authority can build a command, and
// neither a raw request nor the run's own vocabulary is one.
static_assert(!std::is_constructible<ev::AuthorizedMotorCommand, ev::MotorEffort>::value,
              "a MotorEffort must not be constructible into an authorized command");
static_assert(!std::is_convertible<ev::MotorEffort, ev::AuthorizedMotorCommand>::value,
              "a MotorEffort must not convert into an authorized command");
static_assert(!std::is_constructible<ev::AuthorizedMotorCommand, ev::RunMotionCommand>::value,
              "a run command must never become an authorized command");
static_assert(!std::is_default_constructible<ev::AuthorizedMotorCommand>::value,
              "an authorized command must never default-construct");

// ---------------------------------------------------------------------------
// ScenarioHarness
// ---------------------------------------------------------------------------
//
// `nowMs` is the harness's own scripted clock reading, advanced by the control
// period once per step. It is a timestamp, not a duration, and it is passed to
// every production module as an argument - none of them reads a clock.
class ScenarioHarness {
public:
    explicit ScenarioHarness(const ScenarioFixtures& fixtures)
        : fixtures_(fixtures),
          estimator_(fixtures.estimator),
          controller_(fixtures.controller),
          profile_(fixtures.profile),
          run_(fixtures.phase, fixtures.reversal) {
        // A fresh decoder needs one stationary sample before any motion. Both
        // channels low is one legal rest state; the reference sample never
        // counts (QuadratureDecoder.h).
        establishDecoderReference();
        // The regulator starts from a fresh control state, the same way
        // `resetMotionChainForNewRun()` leaves it. Nothing here arms anything.
        controllerEnabled_ = controller_.enable();
    }

    const ScenarioFixtures& fixtures() const { return fixtures_; }

    uint32_t nowMs() const { return nowMs_; }
    uint32_t cycles() const { return static_cast<uint32_t>(trace_.size()); }
    uint32_t droppedSamples() const { return droppedSamples_; }

    // True if the scripted tick advance ever needed more decoder steps than the
    // guard allows. A scenario that trips this is not a scenario.
    bool decoderStepGuardTripped() const { return decoderStepGuardTripped_; }

    const std::vector<CycleRecord>& trace() const { return trace_; }
    const std::vector<ActuatorRecord>& actuatorRecords() const { return actuator_.records(); }

    // The record of the cycle before the first step is a zero record: it claims
    // nothing and reports the safe zero values.
    const CycleRecord& last() const {
        static const CycleRecord empty;
        return trace_.empty() ? empty : trace_.back();
    }

    // --- Run boundary ------------------------------------------------------

    // Announces the run's parameters through the production state machine, with
    // the verdict from the single validation entry point. The announced geometry
    // is held by the caller (this harness), because the state machine consumes it
    // and deliberately does not store it; on a refused announcement nothing is
    // adopted.
    ev::RunStartStatus announce(const ev::RunConfig& config) {
        const ev::RunConfigValidation validation = ev::validateRunConfig(config);
        const ev::RunStartStatus status = run_.announceRun(config, validation, nowMs_);
        if (status != ev::RunStartStatus::RunAlreadyActive &&
            status != ev::RunStartStatus::FaultLatched) {
            configurationValid_ = validation.ok;
        }
        if (status == ev::RunStartStatus::Started) {
            // The announced geometry, through the production mapping. The axis's
            // zero is the Start Point, which is where the origin tick reading is
            // taken; only the difference of the two readings is converted.
            axisGeometry_ = ev::RunAxisGeometry::fromRunConfig(config);
            runOriginTicks_ = decoder_.ticks();
            legForward_ = true;
        }
        return status;
    }

    // The configuration-build checks the vehicle layer runs before arming. Every
    // check is a production function; none of them is re-implemented here.
    bool configurationBuildChecksPass() const {
        return configurationValid_ && fixtures_.calibration.isFullyMeasured() &&
               ev::validateSafetyLimits(fixtures_.limits).ok &&
               ev::effortCeilingsAgree(fixtures_.controller.maxEffortPermille,
                                       fixtures_.limits.maxEffortPermille) &&
               ev::polarityAgrees(fixtures_.profile.forward.positiveCommandDirection,
                                  fixtures_.controller.positiveEffortMode) &&
               ev::tickPolarityAgrees(fixtures_.positiveTicksDirection,
                                      fixtures_.profile.forward.positiveCommandDirection) &&
               // The composed packet serves both stops - the reversal gate's and
               // the final stop's - so its limit must span both dwells.
               ev::stopEvidenceLimitSpansDwell(fixtures_.stopEvidenceMaxAgeMs,
                                               fixtures_.reversal.verification) &&
               ev::stopEvidenceLimitSpansDwell(fixtures_.stopEvidenceMaxAgeMs,
                                               fixtures_.phase.stopVerification) &&
               ev::validateMotionProfileLimits(fixtures_.profile.forward).ok &&
               ev::validateMotionProfileLimits(fixtures_.profile.reverse).ok &&
               ev::validateVelocityEstimatorConfig(fixtures_.estimator).ok &&
               ev::validateRunPhaseConfig(fixtures_.phase).ok &&
               ev::validateReversalConfig(fixtures_.reversal).ok && run_.configUsable() &&
               controller_.configUsable();
    }

    // The deliberate transition into the armed state. It refuses whenever the
    // readiness data is incomplete, so an unmeasured configuration cannot arm.
    bool arm() {
        if (!configurationBuildChecksPass()) {
            return false;
        }
        ev::SafetyReadiness readiness;
        readiness.configurationValid = configurationValid_;
        readiness.calibrationUsable = fixtures_.calibration.isFullyMeasured();
        readiness.limitsUsable = ev::validateSafetyLimits(fixtures_.limits).ok;
        return authority_.arm(readiness);
    }

    // The whole-chain new-run boundary, exactly as the production sequence
    // defines it. Afterwards the harness re-establishes the decoder reference the
    // reset discarded, and forgets everything that described the previous run.
    // It never arms.
    ev::MotionChainResetReport resetForNewRun() {
        const ev::MotionChainResetReport report = ev::resetMotionChainForNewRun(
            decoder_, estimator_, controller_, profileTarget_, authority_, run_, nowMs_);

        establishDecoderReference();
        configurationValid_ = false;
        hasEstimate_ = false;
        hasLastValidEstimate_ = false;
        hasOdometrySample_ = false;
        encoderTicks_ = 0;
        runAxisPositionM_ = 0.0;
        odometrySampleMs_ = 0U;
        runOriginTicks_ = 0;
        axisGeometry_ = ev::RunAxisGeometry(0.0, 0.0, 0.0);
        legForward_ = true;
        stopCyclesAtZeroSinceLastDrive_ = 0U;
        verifiedStopSinceLastDrive_ = false;
        droppedSamples_ = 0U;
        return report;
    }

    // --- One cycle ---------------------------------------------------------

    const CycleRecord& step(const CycleScript& script) {
        // The clock is the vehicle layer's loop: the fixture period by default,
        // or whatever a scenario scripted - a stall (0), a jittered step, or a
        // rewind. The 64-bit sum keeps the wraparound behaviour of a millis()
        // reading deterministic.
        nowMs_ = script.scriptedClock
                     ? static_cast<uint32_t>(static_cast<int64_t>(nowMs_) +
                                             script.clockAdvanceMs)
                     : nowMs_ + fixtures_.controlPeriodMs;
        const uint32_t dtMs = nowMs_ - lastCycleMs_;
        lastCycleMs_ = nowMs_;

        // 1. Sensors: the prescribed advances go through the real decoder, the
        //    real tick-to-distance arithmetic and the real estimator.
        if (script.sensorSampleFresh) {
            if (script.hasRawLevels) {
                // The scenario prescribes the sample; the production decoder
                // decides what it counts - a duplicate or B-only sample is
                // ignored, an A transition is counted from its landing state.
                applyLevels(script.rawLevelA, script.rawLevelB);
            } else {
                advanceEncoderBy(script.encoderAdvanceTicks);
            }
            encoderTicks_ = decoder_.ticks();
            odometryTravel_ = ev::distanceTravelledM(
                runOriginTicks_, encoderTicks_, fixtures_.calibration.distancePerEncoderTickM);
            odometrySampleMs_ = nowMs_;
            hasOdometrySample_ = true;
            estimate_ = estimator_.update(nowMs_, encoderTicks_,
                                          fixtures_.calibration.distancePerEncoderTickM);
            hasEstimate_ = true;
            if (estimate_.valid) {
                lastValidEstimate_ = estimate_;
                hasLastValidEstimate_ = true;
            }
        } else {
            ++droppedSamples_;
        }

        // 2. Readiness: the production verdicts, aged against the fixture limit.
        const ev::SampleAge odometryAge =
            ev::sampleAge(nowMs_, odometrySampleMs_, fixtures_.maxSampleAgeMs);
        const ev::OdometryReadiness odometryReady =
            ev::odometryReadiness(hasOdometrySample_, odometryTravel_, odometryAge);
        const ev::SampleAge velocityAge =
            ev::sampleAge(nowMs_, estimate_.elapsedMs, fixtures_.maxSampleAgeMs);
        const ev::VelocityReadiness velocityReady =
            ev::velocityReadiness(hasEstimate_, estimate_, velocityAge);

        // 3. Position on the run axis: the production conversion, with the
        //    measured tick-sign polarity applied exactly once, and the production
        //    geometry arithmetic. The position verdict is the odometry verdict
        //    combined with the conversion's own: a sample that is valid and fresh
        //    is still not a position when the tick-sign polarity is a placeholder.
        const ev::RunAxisPosition runAxisPosition = ev::runAxisPositionM(
            runOriginTicks_, encoderTicks_, fixtures_.calibration.distancePerEncoderTickM,
            fixtures_.positiveTicksDirection);
        runAxisPositionM_ = runAxisPosition.positionM;
        const bool positionUsable = odometryReady.usable && runAxisPosition.usable;
        const double remainingToTargetM =
            ev::remainingToTargetM(axisGeometry_, legForward_, runAxisPositionM_);
        const double legRemainingM =
            ev::legRemainingM(axisGeometry_, legForward_, runAxisPositionM_);

        // 4. The evidence packet the run policy consumes this cycle.
        ev::RunInputs inputs;
        inputs.authorityArmed = authority_.isArmed();
        inputs.startTriggered = script.startTriggered;
        inputs.bottleContactDetected = script.bottleContactDetected;
        inputs.captureComplete = script.captureComplete;
        inputs.pushComplete = script.pushComplete;
        inputs.remainingDistanceUsable = positionUsable;
        inputs.remainingDistanceM = remainingToTargetM;
        // The stop-verification packet, composed by its production owner from the
        // current cycle's claim and the retained window-derived estimate: the
        // control path's limit for the claim, the packet's own longer limit for
        // the speed.
        inputs.stopEvidence = ev::composeStopEvidence(
            estimate_, hasEstimate_, lastValidEstimate_, hasLastValidEstimate_, nowMs_,
            fixtures_.maxSampleAgeMs, fixtures_.stopEvidenceMaxAgeMs);
        inputs.detectedFault = script.fault;

        const ev::RunStatus status = run_.update(nowMs_, inputs);

        // The leg comes from the run's own command's leg tag, never from a
        // direction, and never from a caller-held intention: a stale request must
        // not move the geometry the run is working in.
        if (status.intention.command != ev::RunMotionCommand::None) {
            legForward_ = ev::runCommandNamesForwardLeg(status.intention.command);
        }

        // 5. Exactly one request, built the way the vehicle layer must build it.
        //    The request source is the run's own intention, or the held one a
        //    scenario presented to exercise the freshness gate.
        const ev::RunMotionIntention& presented =
            script.heldIntention != nullptr ? *script.heldIntention : status.intention;
        const bool intentionCurrent = ev::runIntentionIsCurrentFor(presented, nowMs_);
        const bool travel = ev::isRunTravelCommand(presented.command);
        const bool stop = ev::isRunStopCommand(presented.command);
        const ev::MotionDirection direction =
            legForward_ ? ev::MotionDirection::Forward : ev::MotionDirection::Reverse;
        const ev::MotionProfileCommand shaped = ev::nextTargetVelocity(
            profileTarget_.inputsFor(travel ? ev::MotionProfileIntent::Travel
                                            : ev::MotionProfileIntent::Stop,
                                     direction, legRemainingM, dtMs),
            profile_.forDirection(direction));
        profileTarget_.accept(shaped);

        const bool shapePointsAlongTheLeg = ev::targetVelocityPointsAlongLeg(
            shaped, direction, profile_.forDirection(direction));

        ev::MotorEffort effort(ev::MotorMode::Disabled, 0U);
        bool regulatorStepped = false;
        ev::VelocityControlStatus controlStatus = ev::VelocityControlStatus::Disabled;
        if (script.hasHostileRequest) {
            // A deliberate gate bypass: the scenario dictates the request so the
            // authority's own refusal is what answers it. Only boundary tests set
            // this, and none of them is a normal scenario.
            effort = script.hostileRequest;
        } else if (travel && intentionCurrent && shapePointsAlongTheLeg) {
            // A travel request is the regulator's, and only the regulator's. A
            // travel command whose intention is stale, or whose shaped target is
            // still ramping the previous direction down, falls through to the
            // vehicle layer's own unpowered output (obligations 2 and 7).
            const ev::VelocityControlResult control = controller_.update(
                nowMs_, shaped.targetVelocityMps, velocityReady.usable, estimate_.velocityMps);
            effort = control.effort;
            regulatorStepped = true;
            controlStatus = control.status;
        } else if (stop) {
            // The deliberate non-powered stop, through its production mapping.
            // The regulator is deliberately not stepped: an unactuated cycle must
            // not accumulate regulator state against a target nothing is
            // following.
            effort = ev::runStopEffort(presented.command);
        }

        // 6. The safety boundary: one request, one verdict, one command.
        ev::SafetySystemState state;
        state.readiness.configurationValid = configurationValid_;
        state.readiness.calibrationUsable = fixtures_.calibration.isFullyMeasured();
        state.readiness.limitsUsable = ev::validateSafetyLimits(fixtures_.limits).ok;
        // `requiredDataFresh` is the position verdict (SOFTWARE_ARCHITECTURE.md
        // section 12). The velocity verdict belongs to the regulator's
        // `measurementUsable` argument (section 13), deliberately: a stationary
        // vehicle's explicit zero-speed claim is not a valid estimate, and if
        // that fact gated this field, no run could ever start moving again from a
        // verified stop.
        state.requiredDataFresh = positionUsable;
        state.atZeroSpeed = status.stopVerified;
        state.nowMs = nowMs_;
        state.detectedFault =
            status.fault != ev::FaultCode::None ? status.fault : script.fault;
        const ev::AuthorizationResult authorized =
            authority_.authorize(effort, state, fixtures_.limits);

        // 7. The actuator boundary: the single authorized command.
        actuator_.apply(authorized.command);

        CycleRecord record;
        record.nowMs = nowMs_;
        record.dtMs = dtMs;
        record.sensorSampleFresh = script.sensorSampleFresh;
        record.encoderAdvanceTicks = script.encoderAdvanceTicks;
        record.phase = status.phase;
        record.intention = status.intention;
        record.command = status.intention.command;
        record.presentedIntention = presented;
        record.intentionCurrent = intentionCurrent;
        record.reversalPhase = status.reversalPhase;
        record.stopVerified = status.stopVerified;
        record.faultOfRecord = status.fault;
        record.phaseElapsedMs = status.phaseElapsedMs;
        record.encoderTicks = encoderTicks_;
        record.runAxisPositionM = runAxisPositionM_;
        record.remainingToTargetM = remainingToTargetM;
        record.legRemainingM = legRemainingM;
        record.odometryUsable = odometryReady.usable;
        record.runAxisUsable = runAxisPosition.usable;
        record.positionUsable = positionUsable;
        record.measuredVelocityMps = estimate_.velocityMps;
        record.velocityUsable = velocityReady.usable;
        record.velocityStatus = estimate_.status;
        record.authorityArmed = authority_.isArmed();
        record.readinessComplete = state.readiness.allReady();
        record.requiredDataFresh = state.requiredDataFresh;
        record.atZeroSpeed = state.atZeroSpeed;
        record.reportedFault = state.detectedFault;
        record.presentedEffort = effort;
        record.regulatorStepped = regulatorStepped;
        record.controlStatus = controlStatus;
        record.outcome = authorized.outcome;
        record.commandedMode = authorized.command.mode();
        record.commandedPermille = authorized.command.magnitudePermille();
        record.commandedClass = authorized.command.actuationClass();
        record.commandedDirection = authorized.command.poweredDirection();
        record.stopCyclesAtZeroSinceLastDrive = stopCyclesAtZeroSinceLastDrive_;
        record.verifiedStopSinceLastDrive = verifiedStopSinceLastDrive_;

        // The audit the reversal property is asserted against: a powered
        // direction change must have been preceded by an authorized non-powered
        // stop taken while the run reported a verified zero.
        if (authorized.command.isDriving()) {
            stopCyclesAtZeroSinceLastDrive_ = 0U;
            verifiedStopSinceLastDrive_ = false;
        } else {
            if (state.atZeroSpeed) {
                ++stopCyclesAtZeroSinceLastDrive_;
                verifiedStopSinceLastDrive_ = status.stopVerified;
            }
        }

        trace_.push_back(record);
        return trace_.back();
    }

    // --- Readouts ---------------------------------------------------------

    ev::RunPhase phase() const { return run_.phase(); }
    ev::FaultCode faultCode() const { return run_.faultCode(); }
    bool stopVerified() const { return run_.stopVerified(); }
    ev::ReversalPhase reversalPhase() const { return run_.reversalPhase(); }
    bool controllerEnabled() const { return controllerEnabled_ && controller_.isEnabled(); }

    ev::SafetyAuthority& authority() { return authority_; }
    const ev::SafetyAuthority& authority() const { return authority_; }
    ev::VelocityEstimator& estimator() { return estimator_; }
    ev::QuadratureDecoder& decoder() { return decoder_; }
    const ev::MotionProfileTarget& profileTarget() const { return profileTarget_; }
    ev::CompetitionStateMachine& run() { return run_; }

    // Where the vehicle is on the run axis, metres from the Start Point, from the
    // production conversion with the fixture polarity applied once.
    double runAxisPositionM() const { return runAxisPositionM_; }

    // The run layer's geometry, held here because the state machine deliberately
    // does not store the announced parameters. It is the production geometry
    // type, adopted from a valid announcement only, so a scenario reads the same
    // positions the firmware will.
    const ev::RunAxisGeometry& axisGeometry() const { return axisGeometry_; }
    double targetPointPositionM() const { return axisGeometry_.targetPointPositionM; }
    double bottleStartPositionM() const { return axisGeometry_.bottleStartPositionM; }
    double turnAroundPositionM() const { return axisGeometry_.turnAroundPositionM(); }

    // The tick reading the run's origin was taken at, i.e. run-axis position
    // zero. Exposed so a test can compute its own independent position oracle
    // from the raw ticks instead of trusting the geometry that produced them.
    int32_t runOriginTicks() const { return runOriginTicks_; }

    bool configurationValid() const { return configurationValid_; }

private:
    // The Gray cycle `00 -> 10 -> 11 -> 01 -> 00` in the decoder's own Positive
    // convention. The harness prescribes which way around it the count moves; the
    // decoder decides what each transition counts.
    struct ChannelLevels {
        bool a;
        bool b;
    };
    static const ChannelLevels* grayCycle() {
        static const ChannelLevels levels[4] = {{false, false},
                                                {true, false},
                                                {true, true},
                                                {false, true}};
        return levels;
    }

    static constexpr uint32_t kMaxDecoderStepsPerCycle = 4096U;

    void establishDecoderReference() { applyLevels(false, false); }

    // Index of a (A, B) pair in the Gray cycle table. All four combinations of
    // two levels appear in the cycle, so this always finds one.
    static uint32_t levelIndexFor(bool aLevel, bool bLevel) {
        for (uint32_t index = 0U; index < 4U; ++index) {
            if (grayCycle()[index].a == aLevel && grayCycle()[index].b == bLevel) {
                return index;
            }
        }
        return 0U;
    }

    // One sample to the real decoder, keeping the harness's own place in the
    // Gray cycle in step with the levels it just prescribed.
    void applyLevels(bool aLevel, bool bLevel) {
        levelIndex_ = levelIndexFor(aLevel, bLevel);
        decoder_.update(aLevel, bLevel);
    }

    void stepLevels(int direction) {
        const uint32_t step = direction > 0 ? 1U : 3U;
        const uint32_t next = static_cast<uint32_t>((levelIndex_ + step) % 4U);
        applyLevels(grayCycle()[next].a, grayCycle()[next].b);
    }

    // Drives the real decoder to a prescribed signed tick count.
    void advanceEncoderBy(int32_t ticks) {
        const int64_t target = static_cast<int64_t>(decoder_.ticks()) + ticks;
        if (target < static_cast<int64_t>(INT32_MIN) || target > static_cast<int64_t>(INT32_MAX)) {
            decoderStepGuardTripped_ = true;
            return;
        }
        uint32_t steps = 0U;
        while (static_cast<int64_t>(decoder_.ticks()) != target) {
            if (steps++ >= kMaxDecoderStepsPerCycle) {
                decoderStepGuardTripped_ = true;
                return;
            }
            stepLevels(ticks >= 0 ? 1 : -1);
        }
    }

    ScenarioFixtures fixtures_;

    ev::QuadratureDecoder decoder_;
    uint32_t levelIndex_ = 0U;
    bool decoderStepGuardTripped_ = false;
    int32_t encoderTicks_ = 0;

    ev::VelocityEstimator estimator_;
    // Held defaults before the first estimate exists. The shape of an absent
    // number is owned by `refusedVelocityEstimate()`; this harness mirrors the
    // vehicle layer's members, so it uses the same definition.
    ev::VelocityEstimate estimate_ =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    bool hasEstimate_ = false;

    // The last window-derived estimate, held for the stop-verification packet's
    // speed fact (see the header). Nothing else reads it: the control path uses
    // the current answer. Cleared with everything else at a chain reset.
    ev::VelocityEstimate lastValidEstimate_ =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    bool hasLastValidEstimate_ = false;

    ev::OdometryEstimate odometryTravel_{0.0, false, ev::OdometryEstimateStatus::ScaleIsNotMeasured};
    uint32_t odometrySampleMs_ = 0U;
    bool hasOdometrySample_ = false;

    ev::VelocityController controller_;
    bool controllerEnabled_ = false;
    ev::DirectionalMotionProfileLimits profile_;
    ev::MotionProfileTarget profileTarget_;
    ev::SafetyAuthority authority_;
    ev::CompetitionStateMachine run_;
    ActuatorBoundary actuator_;

    std::vector<CycleRecord> trace_;

    uint32_t nowMs_ = 0U;
    uint32_t lastCycleMs_ = 0U;
    uint32_t droppedSamples_ = 0U;

    // The run layer's own geometry, adopted from a valid announcement only. The
    // zero geometry is the no-run state: the axis origin, with no run to give it
    // meaning.
    bool configurationValid_ = false;
    ev::RunAxisGeometry axisGeometry_{0.0, 0.0, 0.0};
    int32_t runOriginTicks_ = 0;
    double runAxisPositionM_ = 0.0;
    bool legForward_ = true;

    // The reversal audit, mirroring what the authority's direction latch is
    // allowed to clear on: non-powered cycles presented while the run reported a
    // verified stop, reset by every powered command.
    uint32_t stopCyclesAtZeroSinceLastDrive_ = 0U;
    bool verifiedStopSinceLastDrive_ = false;
};

}  // namespace evscenario
