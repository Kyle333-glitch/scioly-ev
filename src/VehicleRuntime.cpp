#include "VehicleRuntime.h"

#include <MotorDriverPlan.h>
#include <NearZeroVerifier.h>
#include <RunDurationGuard.h>
#include <SafetyLimits.h>
#include <Snapshots.h>

#include "EncoderInput.h"
#include "MotorDriver.h"

namespace evhw {

using ev::ControlTickStage;

// Names the incident where a run state machine accepted an announcement and then
// refused the delay it was offered. It is unreachable with this configuration -
// the run's execution plan's `initialDelayS` is bounded by the policy's own legal
// maximum (`RunPhaseConfig::maximumOptionalDelayS`) - so it exists to make a
// caller bug visible instead of silent.
constexpr const char* kReasonDelayNotPermitted = "InitialDelayNotUsable";

namespace {

// How many telemetry records one pass may move. Bounded so a full lane cannot
// monopolise the loop; at the default cadence one record per tick is produced and
// the loop passes many times per tick, so this is never the rate limit - it is the
// guarantee that the pass' cost is bounded.
constexpr uint8_t kTelemetryRecordsPerPass = 4U;

// How many loop passes between samples of the host control-line state
// (`serialTransport::hostLineState()`). Every pass would run the USB task tens of
// thousands of times a second for a fact that is not a deadline; this bounds the
// cost while still noticing an attaching host long before the next control tick.
constexpr uint8_t kHostPollPasses = 32U;

}  // namespace

VehicleRuntime::VehicleRuntime()
    : calibration_(vehicleConfig::calibration()),
      safetyLimits_(vehicleConfig::safetyLimits()),
      estimatorConfig_(vehicleConfig::estimatorConfig()),
      profileLimits_(vehicleConfig::profileLimits()),
      estimator_(estimatorConfig_),
      velocityController_(vehicleConfig::controllerConfig()),
      run_(vehicleConfig::runPhaseConfig(), vehicleConfig::reversalConfig()),
      recoveryStopVerifier_(vehicleConfig::reversalStopVerification()),
      scheduler_(vehicleConfig::kControlPeriodMs),
      lineReader_(commandLine_, ev::kCommandLineCapacity) {}

void VehicleRuntime::begin(uint32_t nowMs) {
    // 1. De-energize the driver first. Nothing else in this function can ask for
    //    motion, and nothing that runs later can find the driver enabled.
    motorDriver::beginDisabled();

    // 2. Encoder: attach the one interrupt and take the reference sample. This
    //    must happen while the drivetrain is known to be stationary.
    encoderInput::begin();

    // 3. Raw inputs.
    sensorInputs::begin();

    // 4. The run axis origin is the tick reading here, because no run has been
    //    announced and the axis has no other meaning yet. A run boundary replaces
    //    both this reading and the geometry with the announced values.
    runOriginTicks_ = encoderInput::snapshotTicks().ticks;
    legForward_ = true;
    axisGeometry_ = ev::RunAxisGeometry(0.0, 0.0, 0.0);

    // 5. The regulator's control state. This refuses while the gains are
    //    provisional, which is a fact telemetry reports: an unconfigured
    //    regulator produces no effort at all, not an unchecked one.
    regulatorEnabled_ = velocityController_.enable();

    // 6. The configuration-build verdicts, evaluated once here so the reason the
    //    vehicle cannot be armed is available even before the first tick.
    configurationChecks_ = vehicleConfig::configurationBuildChecks(announcedConfigValid_, run_);

    // 7. The operator interface's first line, owed but not staged: the boot line
    //    is pumped by the first passes of the loop (`serviceSerial`), so a host
    //    that is absent, slow or closed cannot delay safe initialization - and a
    //    host that attaches later still learns which firmware it is talking to.
    //    The greeting is *owed* here rather than queued, and it is re-owed for a
    //    reader that arrives later (`greeter_`).
    greeter_.requestGreeting(nowMs);

    // 8. The session's own boundary record. It is the first record of a session
    //    (`TelemetryRecord.h`) and it is queued, not written: nothing here waits
    //    for a port.
    pushOperatorEvent(ev::TelemetryEventCode::Boot, ev::kTelemetryMissingField);

    // 9. Start the schedule last: the first control tick is one period away, and
    //    until then the loop does no control work.
    scheduler_.start(nowMs);
}

PassOutcome VehicleRuntime::runPass(uint32_t nowMs) {
    PassOutcome outcome;

    const ev::ControlTickDecision decision = scheduler_.decide(nowMs);

    if (!decision.due) {
        // No tick this pass. The scheduler's own bookkeeping is the only work:
        // there is deliberately nothing here that samples, computes or writes.
        return outcome;
    }

    runControlTick(nowMs, decision.dtMs);
    scheduler_.advance(nowMs);

    // The tick has closed its own record; report it.
    outcome.tickRan = true;
    outcome.tickCompleted = orderGuard_.tickInOrder();
    outcome.orderViolation = !outcome.tickCompleted;

    telemetry_.lateByMs = decision.lateByMs;
    telemetry_.droppedDeadlines = decision.droppedTicks;
    // The session's own extremes, from the scheduler rather than from this
    // decision: a soak test asks what the worst has been, not what the last tick
    // was (P-H0 Step 7).
    telemetry_.worstLateByMs = scheduler_.worstLateByMs();
    telemetry_.droppedDeadlinesTotal = scheduler_.droppedDeadlines();
    telemetry_.worstStallMs = scheduler_.worstStallMs();
    telemetry_.clockRewind = decision.clockRewind;
    telemetry_.controlTicks = scheduler_.ticks();
    telemetry_.orderInOrder = orderGuard_.tickInOrder();
    telemetry_.orderError = orderGuard_.tickVerdict().error;
    telemetry_.orderStage = orderGuard_.tickVerdict().stage;
    telemetry_.orderViolations = orderGuard_.violatedTicks();

    // The optional hook, for a later transport. It runs here - inside the pass,
    // after the tick closed its record and before the loop's watchdog checkpoint -
    // so it must not block. No production code attaches one: the hook is null in
    // this build, and the tick has already staged its record in the queue that
    // `serviceSerial()` drains after the checkpoint.
    if (telemetryHook_ != nullptr) {
        telemetryHook_(telemetryHookContext_, telemetry_);
    }
    return outcome;
}

void VehicleRuntime::runControlTick(uint32_t nowMs, uint32_t dtMs) {
    orderGuard_.begin();

    // ---------------------------------------------------------------- stage 1
    // One coherent read of the count the interrupt maintains. The snapshot is
    // what every later stage of this cycle uses; nothing re-reads the encoder.
    orderGuard_.enter(ControlTickStage::EncoderSnapshot);
    const encoderInput::TickSnapshot ticks = encoderInput::snapshotTicks();

    // ---------------------------------------------------------------- stage 2
    // The whole measurement update: distance travelled, the windowed velocity
    // estimate, the two readiness verdicts that travel with them, and the
    // position on the run axis with its measured tick-sign polarity applied once.
    //
    // An incremental encoder is sampled continuously by its interrupt, so there
    // is no "no sample this cycle" state to report: the count always exists, and
    // what can be stale is the derived estimate, which the readiness verdicts age
    // against this cycle's clock reading.
    orderGuard_.enter(ControlTickStage::OdometryAndVelocity);
    odometryTravel_ = ev::distanceTravelledM(runOriginTicks_, ticks.ticks,
                                             calibration_.distancePerEncoderTickM);
    hasOdometrySample_ = true;
    odometrySampleMs_ = nowMs;

    estimate_ = estimator_.update(nowMs, ticks.ticks, calibration_.distancePerEncoderTickM);
    hasEstimate_ = true;
    if (estimate_.valid) {
        // Retained for the stop-verification packet: that packet pairs the
        // current cycle's zero-speed claim with the last window-derived estimate,
        // because the estimator never reports both in one answer.
        lastValidEstimate_ = estimate_;
        hasLastValidEstimate_ = true;
    }

    const ev::VelocityReadiness velocityReady = ev::velocityReadiness(
        hasEstimate_, estimate_,
        ev::sampleAge(nowMs, estimate_.elapsedMs, vehicleConfig::kMaxSampleAgeMs));
    const ev::OdometryReadiness odometryReady = ev::odometryReadiness(
        hasOdometrySample_, odometryTravel_,
        ev::sampleAge(nowMs, odometrySampleMs_, vehicleConfig::kMaxSampleAgeMs));

    const ev::RunAxisPosition axisPosition =
        ev::runAxisPositionM(runOriginTicks_, ticks.ticks, calibration_.distancePerEncoderTickM,
                             vehicleConfig::positiveTicksDirection());
    positionUsable_ = odometryReady.usable && axisPosition.usable;
    const double remainingToTargetM =
        ev::remainingDistanceTo(axisGeometry_.targetPointPositionM, axisPosition.positionM,
                                legForward_);
    // The leg the run is travelling ends at ONE execution target, and the plan is
    // its owner: the forward leg at the plan's explicit forward stop, the reverse
    // leg at the announced Target Point the plan carries. This is the distance the
    // travel taper is sized from, so the shaping and the run's own progression
    // (which completes the forward phases at the same stored value) cannot disagree
    // about where the forward travel ends.
    //
    // With no usable plan there is no leg target at all, and the answer is zero
    // remaining distance rather than a shape toward a rule landmark: the Bottle
    // Line is not a substitute execution target, and a run that has none does not
    // travel forward.
    const ev::LegExecutionTarget legTarget = ev::legExecutionTargetM(executionPlan_.plan(), legForward_);
    const double legRemainingM =
        legTarget.usable
            ? ev::remainingDistanceToClamped(legTarget.positionM, axisPosition.positionM,
                                             legForward_)
            : 0.0;

    // ---------------------------------------------------------------- stage 3
    // Raw inputs. Nothing here is converted or interpreted: the counts are counts
    // and the levels are levels (`SOFTWARE_ARCHITECTURE.md` section 12).
    //
    // The external nFAULT line is not installed on this vehicle, so
    // `sensorInputs::sample()` does not read the pin at all and the sample
    // carries `NotInstalled` (`SensorInputs.h`). The counter below is guarded by
    // the capability on purpose: an unguarded increment would count a floating
    // pin's toggles as driver-fault evidence, which is a fabricated measurement
    // rather than a missing one.
    orderGuard_.enter(ControlTickStage::SensorSnapshot);
    rawInputs_ = sensorInputs::sample();
    if (rawInputs_.driverFaultCapability == ev::DriverFaultCapability::Installed &&
        rawInputs_.driverFaultBarLow) {
        ++cyclesWithDriverFaultLow_;
    }

    // ---------------------------------------------------------------- stage 4
    // This cycle's health: the configuration-build verdicts the boundary's
    // readiness comes from, and any fault this layer can name.
    //
    // The driver fault line is interpreted by `ev::observedDriverFault()`, which
    // is capability-aware (`lib/evcore/src/DriverFault.h`).
    //
    // **This vehicle has no external nFAULT line**
    // (`vehicleConfig::kPlannedDriverFaultCapability` is `NotInstalled`), so the
    // call below returns `FaultCode::None` unconditionally and for a different
    // reason than an unverified polarity would: there is no signal to interpret.
    // No level is fed to it as though monitoring existed - the level it receives
    // is explicitly not a reading (`SensorInputs.h`). The DRV8874's own on-chip
    // protection is unaffected; what is absent is the MCU's ability to observe a
    // fault it reports, which is recorded in `docs/limitations.md` and is why
    // external fault telemetry is unavailable on this hardware.
    //
    // On a vehicle that *does* wire the line, the same call refuses to claim a
    // fault while the polarity is unverified (`VERIFIED_STATE.md` #1, #2): an
    // assumed polarity would either turn a healthy driver into a latched fault or
    // turn a real fault into a log entry nobody acts on. Once H1B has measured the
    // polarity, promoting `kPlannedDriverFaultPolarity` gives the line an
    // identity, and the authority latches it exactly as it latches any other
    // fault. The `DriverFaultConfigurationValid` check is what keeps such a
    // vehicle unarmable until then, so "the fault input is invisible" can never be
    // the state of an armed vehicle that has one.
    orderGuard_.enter(ControlTickStage::FaultEvaluation);
    configurationChecks_ = vehicleConfig::configurationBuildChecks(announcedConfigValid_, run_);
    const ev::FaultCode observedFault =
        ev::observedDriverFault(rawInputs_.driverFaultCapability, rawInputs_.driverFaultBarLow,
                                vehicleConfig::plannedDriverFaultPolarity());

    // The run-scoped maximum-duration bound (S10): one elapsed timer, begun at
    // the run's own actuation reading and evaluated once per cycle, in every
    // phase. It is deliberately global rather than per-phase - the per-phase
    // bounds catch a phase that never got its evidence, this catches the run as
    // a whole - and it is computed here, inside the control tick, so no host,
    // USB state or serial command can postpone it. `runDurationClockActive()`
    // owns which interval is bounded (the run's own timeline and phase, the one
    // owner of the actuation reading), `ev::runDurationVerdict()` owns the
    // wrap-safe elapsed arithmetic, and the bound only bites while
    // `maxRunDurationS` is a `Measured` duration - a placeholder cannot stop a
    // run (it already refuses arming through `SafetyLimitsValid`).
    const ev::RunTimeline runClock = run_.timeline();
    const ev::RunDurationVerdict runDuration =
        ev::runDurationVerdict(ev::runDurationClockActive(runClock, run_.phase(), run_.faultCode()),
                              runClock.actuatedAtMs, nowMs, safetyLimits_.maxRunDurationS);
    // This cycle's one observed fault. The run-policy fault the duration bound
    // raises takes exactly the same path as any other observed fault: the run
    // latches it (its fault of record), and the authority latches it too and
    // answers with its single deterministic fault stop. The driver fault is kept
    // first so a real electrical fault is never masked by a run that overran.
    //
    // The feedback-progress supervision (S18C/P4) is evaluated here, before the
    // run policy, from the previous cycle's accepted-powered fact: while
    // control cycles keep authorizing a driving mode, the signed tick count
    // must keep advancing in the expected direction, or the run latches
    // `EncoderFeedbackLost` this same cycle and the authority answers with its
    // single fault stop - so a violation stops this cycle's authorization, not
    // the next one's. Priority is driver first (electrical), then feedback
    // (active-travel safety), then the run-scoped duration backstop; every one
    // answers with the same Disable stop. The sample is always fresh here by
    // construction: the interrupt-sampled count exists every cycle, so there
    // is no "no tick sample" state to report (R2/S18R-M1). Encoder invalidity
    // in production appears as derived-estimate staleness, which denies power
    // the same cycle through `requiredDataFresh`; the monitor's own invalid
    // path bounds scripted dropouts in the host mirror.
    const bool feedbackViolated = feedbackMonitor_.update(
        ticks.ticks, true, lastCycleAuthorizedPowered_, legForward_,
        vehicleConfig::positiveTicksDirection(), run_.config().encoderProgressTimeoutS,
        dtMs);
    const ev::FaultCode feedbackFault = ev::encoderProgressFault(feedbackViolated);
    const ev::FaultCode cycleFault =
        observedFault != ev::FaultCode::None
            ? observedFault
            : (feedbackFault != ev::FaultCode::None ? feedbackFault
                                                    : ev::runDurationFault(runDuration));

    // ---------------------------------------------------------------- stage 5
    // The run policy, fed only this cycle's evidence. The distance it consumes is
    // the previous cycle's leg (the leg this cycle will name does not exist yet),
    // which is the wiring order the scenarios pinned.
    orderGuard_.enter(ControlTickStage::RunStateMachine);
    ev::RunInputs runInputs;
    runInputs.authorityArmed = authority_.isArmed();
    runInputs.startTriggered = rawInputs_.startTriggerActive;
    // The forward progression facts for this cycle, thresholded at the run's own
    // execution plan landmarks (`deriveBottleEvidence()`): this vehicle has no
    // bottle sensor, so these are statements about how far the vehicle has
    // travelled - approach is "the credible capture window is open", never "the
    // bottle was touched" - and every distance behind them is a `Sourced` value that
    // must be `Measured` before the plan exists, so this path cannot run on a
    // placeholder. The derivation reports nothing at all while the position verdict
    // is unusable, which is what makes a phase whose completion never arrives end in
    // its configured timeout rather than in a guessed exit.
    //
    // The leg it uses is the leg the run is on now (`legForward_`, updated in
    // stage 6 of the previous tick from the run's own command), because the leg
    // this cycle will name does not exist until the run has been stepped.
    const ev::BottleEvidence bottleEvidence = ev::deriveBottleEvidence(
        executionPlan_.plan(), legForward_, axisPosition.positionM, positionUsable_);
    runInputs.approachComplete = bottleEvidence.approachComplete;
    runInputs.captureComplete = bottleEvidence.captureComplete;
    runInputs.pushComplete = bottleEvidence.pushComplete;
    runInputs.remainingDistanceUsable = positionUsable_;
    runInputs.remainingDistanceM = remainingToTargetM;
    // The brake entry of the leg being travelled (S18B): the leg's remaining
    // distance to its execution target is inside that direction's measured brake
    // offset, which is exactly where the shaping taper below reaches zero and the
    // run's travel-progress bound begins to apply. It is computed here, from the
    // same plan-owned leg target and direction limits the shaper uses, so the run
    // policy never needs its own copy of the measured offset; with no usable leg
    // target there is no brake entry.
    const ev::MotionDirection legDirectionNow =
        legForward_ ? ev::MotionDirection::Forward : ev::MotionDirection::Reverse;
    const double legBrakeOffsetM =
        runProfileLimits_.forDirection(legDirectionNow).brakeOffsetM.value;
    runInputs.legInBrakeEntry = legTarget.usable && std::isfinite(legBrakeOffsetM) &&
                                legRemainingM <= legBrakeOffsetM;
    runInputs.stopEvidence = ev::composeStopEvidence(
        estimate_, hasEstimate_, lastValidEstimate_, hasLastValidEstimate_, nowMs,
        vehicleConfig::kMaxSampleAgeMs, vehicleConfig::kStopEvidenceMaxAgeMs);
    runInputs.detectedFault = cycleFault;

    const ev::RunStatus status = run_.update(nowMs, runInputs);

    // The disarmed stop-recovery verdict (S18A/P2): the same stop evidence the
    // run policy just consumed, through the reversal gate's own stop
    // definition. It contributes to the zero-speed fact below only while the
    // authority is disarmed, so a direction barrier that survived a software
    // reset discharges on a freshly observed standstill - never on the reset,
    // and never during an armed run, where the run policy owns that fact.
    // With the shipped Provisional stop verification this never verifies, so
    // production behavior is unchanged; it is live as soon as the stop
    // definition is usable.
    const ev::NearZeroVerdict recoveryStop =
        recoveryStopVerifier_.update(nowMs, runInputs.stopEvidence);
    const bool disarmedRecoveryVerified =
        !authority_.isArmed() && recoveryStop.verified;

    // ---------------------------------------------------------------- stage 6
    // The run's request: its own intention (never a held one - this layer
    // re-presents nothing), the leg tag it names, and the shaped target the
    // regulator will follow.
    orderGuard_.enter(ControlTickStage::RunMotionRequest);
    if (status.intention.command != ev::RunMotionCommand::None) {
        legForward_ = ev::runCommandNamesForwardLeg(status.intention.command);
    }
    const bool intentionCurrent = ev::runIntentionIsCurrentFor(status.intention, nowMs);
    const bool travel = ev::isRunTravelCommand(status.intention.command);
    const bool stop = ev::isRunStopCommand(status.intention.command);
    const ev::MotionDirection direction =
        legForward_ ? ev::MotionDirection::Forward : ev::MotionDirection::Reverse;

    // The limits this run is shaped with (`runProfileLimits_`, set at the
    // announcement): the configured strategy cruise, or - only when a validated
    // physical timing model chose one - the slower cruise that fits the announced
    // Target Time. While timing is unvalidated the idealized estimate therefore
    // changes nothing here. The brake offset and velocity ceiling still come from
    // the leg being travelled.
    //
    // The optional strategy ceiling that belongs to this cycle's run phase: only
    // the bottle phases (Approach/Capture/Push) carry one, every other phase is
    // explicitly without a ceiling, and the ceiling enters the profile's desired
    // magnitude before the rate limits - so a phase change tightens through the
    // normal deceleration ramp instead of stepping. Every production ceiling is a
    // Provisional placeholder today, so the run policy refuses and no run may be
    // armed: the values stay unusable until the bottle-phase measurements exist
    // (P-H8).
    const ev::StrategyVelocityCeiling strategyCeiling =
        ev::strategyVelocityCeilingForPhase(run_.config(), status.phase);
    const ev::MotionProfileCommand shaped = ev::nextTargetVelocity(
        profileTarget_.inputsFor(travel ? ev::MotionProfileIntent::Travel
                                        : ev::MotionProfileIntent::Stop,
                                 direction, legRemainingM, dtMs, strategyCeiling),
        runProfileLimits_.forDirection(direction));
    profileTarget_.accept(shaped);

    // A travel request may only be presented as powered once the shaped target
    // points along the leg the run asked for. The profile deliberately ramps a
    // previous direction to zero before it flips the sign, and presenting the
    // regulator's effort while the shape is still on the old side would ask for
    // powered effort in the direction the run has just stopped travelling in.
    const bool shapePointsAlongLeg = ev::targetVelocityPointsAlongLeg(
        shaped, direction, runProfileLimits_.forDirection(direction));

    // ---------------------------------------------------------------- stage 7
    // The effort: the regulator's, or the run's own deliberate stop mapping.
    //
    // A ramp-down is regulated, and that is deliberate: `RampDown*` means the
    // profile is bringing the commanded target to zero at the deceleration limit,
    // and the effort has to follow that target down. Mapping it straight to a
    // non-powered request (which is what the run's stop mapping does, correctly,
    // for `Brake*`) would drop the commanded effort in one cycle and turn the
    // controlled ramp into a coast. The regulator's one-sided effort limit is what
    // keeps this safe: while the target decays it can only ask for effort in the
    // travel direction, and once the target reaches zero the request quantizes to
    // `Disabled`, so a ramp can never ask for powered effort the other way.
    //
    // The regulator is deliberately not stepped for a deliberate stop, so an
    // unactuated cycle accumulates no regulator state against a target nothing is
    // following.
    orderGuard_.enter(ControlTickStage::MotionControl);
    ev::MotorEffort effort(ev::MotorMode::Disabled, 0U);
    bool regulatorStepped = false;
    ev::VelocityControlStatus controlStatus = ev::VelocityControlStatus::Disabled;
    const bool rampDown = ev::isRunRampDownCommand(status.intention.command);
    if ((travel || rampDown) && intentionCurrent && shapePointsAlongLeg) {
        const ev::VelocityControlResult control = velocityController_.update(
            nowMs, shaped.targetVelocityMps, velocityReady.usable, estimate_.velocityMps);
        effort = control.effort;
        regulatorStepped = true;
        controlStatus = control.status;
    } else if (stop) {
        effort = ev::runStopEffort(status.intention.command);
    }

    // ---------------------------------------------------------------- stage 8
    // Exactly one request, one verdict, one command. This is the only call to the
    // authority in the firmware, so the direction memory and the fault response
    // cannot depend on a call order.
    //
    // `requiredDataFresh` is the position verdict, not the speed verdict: a
    // stationary vehicle's speed evidence is the estimator's explicit zero-speed
    // claim, whose `valid` is false by design, so gating on the speed verdict
    // would make a restart from a verified stop impossible. The speed verdict's
    // home is the regulator's `measurementUsable` argument, above.
    orderGuard_.enter(ControlTickStage::SafetyAuthorization);
    ev::SafetySystemState safetyState;
    safetyState.readiness.configurationValid = configurationChecks_.configurationValid;
    safetyState.readiness.calibrationUsable = configurationChecks_.calibrationMeasured;
    safetyState.readiness.limitsUsable = configurationChecks_.safetyLimitsValid;
    safetyState.requiredDataFresh = positionUsable_;
    safetyState.atZeroSpeed = status.stopVerified || disarmedRecoveryVerified;
    safetyState.nowMs = nowMs;
    safetyState.detectedFault =
        status.fault != ev::FaultCode::None ? status.fault : cycleFault;

    const ev::AuthorizationResult authorized = authority_.authorize(effort, safetyState, safetyLimits_);

    // The next cycle's "accepted powered travel" fact for the feedback monitor:
    // whether this cycle's authorized command was a driving mode. A request the
    // boundary refused (or a non-powered stop) never starts or sustains the
    // progress interval.
    lastCycleAuthorizedPowered_ = authorized.command.isDriving();

    // ---------------------------------------------------------------- stage 9
    // The only command that can reach the driver is the one the authority
    // returned. If this cycle's stage order was already violated, the driver gets
    // the de-energized state instead, the authority is disarmed, and the pass is
    // reported unrefreshable to the watchdog checkpoint - firmware whose control
    // cycle ran wrongly does not get to keep driving, and does not get to keep
    // feeding the watchdog either.
    orderGuard_.enter(ControlTickStage::MotorCommand);
    if (orderGuard_.tickInOrder()) {
        motorDriver::apply(authorized.command);
    } else {
        motorDriver::failSafe();
        authority_.disarm();
    }

    // --------------------------------------------------------------- stage 10
    // This cycle's record. The clock reading is the one this pass was given, so
    // the log and the control path cannot disagree about when something happened.
    orderGuard_.enter(ControlTickStage::TelemetryCapture);
    TelemetrySnapshot& snapshot = telemetry_;
    snapshot.nowMs = nowMs;
    snapshot.dtMs = dtMs;
    snapshot.encoderTicks = ticks.ticks;
    snapshot.countedEdges = ticks.countedEdges;
    snapshot.ignoredUpdates = ticks.ignoredUpdates;
    snapshot.runAxisPositionM = axisPosition.positionM;
    snapshot.remainingToTargetM = remainingToTargetM;
    snapshot.legRemainingM = legRemainingM;
    snapshot.legTargetUsable = legTarget.usable;
    snapshot.legTargetM = legTarget.positionM;
    snapshot.executionPlanUsable = executionPlan_.plan().valid;
    snapshot.executionPlanStatus = executionPlan_.plan().status;
    snapshot.forwardStopMpPositionM = executionPlan_.plan().forwardStopMpPositionM;
    snapshot.measuredVelocityMps = estimate_.velocityMps;
    snapshot.velocityUsable = velocityReady.usable;
    snapshot.velocityStatus = estimate_.status;
    snapshot.positionUsable = positionUsable_;
    snapshot.batteryCounts = rawInputs_.batteryCounts;
    snapshot.currentCounts = rawInputs_.currentCounts;
    snapshot.driverFaultCapability = rawInputs_.driverFaultCapability;
    snapshot.driverFaultBarLow = rawInputs_.driverFaultBarLow;
    snapshot.startTriggerActive = rawInputs_.startTriggerActive;
    snapshot.driverFaultLowCycles = cyclesWithDriverFaultLow_;
    snapshot.driverFaultConfigurationValid = configurationChecks_.driverFaultConfigurationValid;
    snapshot.phase = status.phase;
    // The run's own timeline, read from the run policy's record: the tick that
    // actuated the run, the final stop's dwell start and the verified final-stop
    // completion. Copied rather than recomputed, so the sample cannot disagree
    // with the run that produced it about when its final stop began.
    const ev::RunTimeline runTimeline = run_.timeline();
    snapshot.runActuated = runTimeline.actuated;
    snapshot.runActuatedAtMs = runTimeline.actuatedAtMs;
    snapshot.finalStopDwellStartKnown = runTimeline.dwellStartKnown;
    snapshot.finalStopDwellStartMs = runTimeline.finalStopDwellStartMs;
    snapshot.finalStopVerified = runTimeline.finalStopVerified;
    snapshot.finalStopVerifiedAtMs = runTimeline.finalStopVerifiedAtMs;
    snapshot.approachComplete = bottleEvidence.approachComplete;
    snapshot.captureComplete = bottleEvidence.captureComplete;
    snapshot.pushComplete = bottleEvidence.pushComplete;
    snapshot.runCommand = status.intention.command;
    snapshot.intentionCurrent = intentionCurrent;
    snapshot.targetVelocityMps = shaped.targetVelocityMps;
    snapshot.profileStatus = shaped.status;
    snapshot.presentedMode = effort.mode;
    snapshot.presentedPermille = effort.magnitudePermille;
    snapshot.regulatorStepped = regulatorStepped;
    snapshot.controlStatus = controlStatus;
    snapshot.armed = authority_.isArmed();
    snapshot.readinessComplete = safetyState.readiness.allReady();
    snapshot.requiredDataFresh = safetyState.requiredDataFresh;
    snapshot.atZeroSpeed = safetyState.atZeroSpeed;
    snapshot.fault = safetyState.detectedFault;
    snapshot.outcome = authorized.outcome;
    snapshot.commandedMode = authorized.command.mode();
    snapshot.commandedPermille = authorized.command.magnitudePermille();
    snapshot.commandedClass = authorized.command.actuationClass();
    snapshot.commandedDirection = authorized.command.poweredDirection();
    snapshot.checks = configurationChecks_;
    snapshot.orderViolations = orderGuard_.violatedTicks();

    // The record is closed only now: an incomplete tick is not detectable until
    // the last stage has arrived.
    orderGuard_.finish();

    // The telemetry of this tick. It comes last because it reads the closed
    // record: a sample built before the last stage would be a partial one.
    captureTelemetry();
}

// ---------------------------------------------------------------------------
// Telemetry capture and drain
// ---------------------------------------------------------------------------

// One tick's sample, from the tick's own snapshot. Every field is a value the
// chain produced or a raw reading this layer took; nothing is recomputed for
// display, which is what lets a log be compared against the control path.
ev::TelemetrySample VehicleRuntime::buildSample() const {
    const TelemetrySnapshot& snapshot = telemetry_;
    ev::TelemetrySample sample;
    sample.tMs = snapshot.nowMs;
    sample.dtMs = static_cast<uint16_t>(snapshot.dtMs > 0xFFFFU ? 0xFFFFU : snapshot.dtMs);
    sample.phase = snapshot.phase;
    sample.command = snapshot.runCommand;
    sample.armed = snapshot.armed;
    sample.runActuated = snapshot.runActuated;
    sample.runActuatedAtMs = snapshot.runActuatedAtMs;
    sample.finalStopDwellStartKnown = snapshot.finalStopDwellStartKnown;
    sample.finalStopDwellStartMs = snapshot.finalStopDwellStartMs;
    sample.finalStopVerified = snapshot.finalStopVerified;
    sample.finalStopVerifiedAtMs = snapshot.finalStopVerifiedAtMs;
    sample.positionM = snapshot.runAxisPositionM;
    sample.positionUsable = snapshot.positionUsable;
    sample.measuredVelocityMps = snapshot.measuredVelocityMps;
    sample.velocityUsable = snapshot.velocityUsable;
    sample.requestedVelocityMps = snapshot.targetVelocityMps;
    sample.remainingDistanceM = snapshot.remainingToTargetM;
    sample.motorMode = snapshot.presentedMode;
    sample.effortPermille = snapshot.presentedPermille;
    sample.batteryCounts = snapshot.batteryCounts;
    sample.currentCounts = snapshot.currentCounts;
    sample.driverFaultCapability = snapshot.driverFaultCapability;
    sample.driverFaultBarLow = snapshot.driverFaultBarLow;
    sample.fault = snapshot.fault;
    sample.outcome = snapshot.outcome;
    sample.firstFailedCheck = snapshot.checks.firstFailure();
    return sample;
}

// The session's decision, and the transitions the tick revealed. Called once per
// control tick, and deliberately independent of the operator interface: a session
// that is disabled counts nothing and stores nothing (`TelemetryQueue.h`), while
// the transition detector still observes every tick so that re-enabling the
// stream cannot emit a transition that happened while it was off.
void VehicleRuntime::captureTelemetry() {
    const ev::TelemetrySample sample = buildSample();

    if (telemetrySession_.sampleDue()) {
        telemetrySession_.pushSample(sample);
    }

    const ev::TelemetryEventBatch batch = eventDetector_.observe(sample);
    for (uint8_t index = 0U; index < batch.count; ++index) {
        telemetrySession_.pushEvent(batch.events[index]);
    }
}

// Queued records, out. The order inside the queue is events first, oldest first
// (`TelemetryQueue.h`); this loop preserves it, and never pops a record while the
// port is still sending the previous line: a popped record that the transport
// refused would be a loss no drop counter could report.
void VehicleRuntime::drainTelemetry() {
    serialTransport::LineTransport& transport = serialTransport::port();
    // Nothing is popped while no host is attached. The port's free-space report is
    // not enough to decide this: it describes the USB FIFO, which has room whether
    // or not anyone is holding the port, so a record staged into an unattended port
    // would sit here and be *absent from the queue* - and the queue's own drop
    // accounting is the record of what the host missed. Waiting is the same
    // discipline the boot line follows (`hostAttached_`), and it changes nothing
    // the host sees: a reconnecting host still gets the queue's oldest records
    // first. `hostAttached_` is sampled once per pass in `serviceStagedText()`,
    // which both this image and the bench image call first.
    for (uint8_t index = 0U; index < kTelemetryRecordsPerPass; ++index) {
        if (!hostAttached_ || transport.busy() || !telemetrySession_.hasPending()) {
            return;
        }
        ev::TelemetryRecord record;
        if (!telemetrySession_.popNext(record)) {
            return;
        }
        const uint16_t length =
            ev::formatTelemetryRecord(record, transport.workingBuffer(), ev::kSerialLineCapacity);
        if (length == ev::kTelemetryFormatDidNotFit) {
            ++telemetryFormatFailures_;
            continue;  // a record that does not fit is counted, never sent short
        }
        if (!transport.stage(length, false)) {
            ++telemetryStagingFailures_;
            return;
        }
        transport.pump();
    }
}

// ---------------------------------------------------------------------------
// The operator interface
// ---------------------------------------------------------------------------

// What leaves the firmware this pass: the line already in flight, then the boot
// line, then as many help lines as the port accepts before the transport reports
// it is busy again. The help cursor can advance more than one line in a pass, but
// the work is bounded by the static table (`ev::kOperatorHelpLineCount` lines), so
// a `help` answer can never make a single pass unbounded.
void VehicleRuntime::serviceStagedText(uint32_t nowMs) {
    serialTransport::LineTransport& transport = serialTransport::port();
    transport.pump();

    // Sample the host control-line state, bounded per pass (see
    // `kHostPollPasses`). A newly asserted line - DTR *or* RTS - is an attach: a
    // host that opens the port is greeted even if this port has been quiet for the
    // whole boot. The boot line says which firmware is running and with which
    // configuration-build verdicts, and the `EVE3` boot record carries none of
    // that. A line staged while nobody was reading is not a delivered line
    // (finding F-4, D21), so the state is tracked rather than assumed. Why two
    // bits are tracked, and why neither can make this a guarantee, is in
    // `SerialTransport.h` and `VehicleRuntime.h`.
    if (hostPollCountdown_ == 0U) {
        hostPollCountdown_ = kHostPollPasses;
        const serialTransport::HostLineState state = serialTransport::hostLineState();
        greeter_.observeLines(state.dtr, state.rts, nowMs);
        hostLineState_ = state;
        // `dtr` alone is the write gate, because it is what `Serial.write()`
        // tests: a record is never popped for a port that cannot move it.
        hostAttached_ = state.dtr;
    } else {
        --hostPollCountdown_;
    }

    // The clock the greeting's silence rule runs against (see `greeter_`). It is
    // sampled every pass, not at the control-line rate: it is arithmetic on
    // `nowMs`, and the deadline it enforces is `ev::HostGreetPolicy::kSilenceMs`
    // wide, so the exact pass it fires on does not matter.
    greeter_.observeClock(nowMs);

    if (transport.busy()) {
        return;
    }

    if (greeter_.owed()) {
        if (!hostAttached_) {
            // Nobody is reading. Keep the greeting owed and keep the transport
            // idle, rather than committing a line the port cannot move.
            return;
        }
        greeter_.delivered(nowMs);
        stageBootLine();
        return;
    }

    while (helpLineIndex_ < ev::kOperatorHelpLineCount) {
        ev::BoundedText text(transport.workingBuffer(), ev::kSerialLineCapacity);
        text.appendText(ev::kOperatorHelpLines[helpLineIndex_]);
        if (text.truncated()) {
            ++responsesLost_;
        } else if (!transport.stage(text.length(), true)) {
            ++responsesLost_;
            return;
        }
        ++helpLineIndex_;
        transport.pump();
        if (transport.busy()) {
            return;
        }
    }
}

// The greeting, formatted and staged. Shared by the attach/silence re-arm in
// `serviceStagedText()` and by the `boot` command, so the line an operator asks
// for and the line a host is greeted with cannot drift apart. It is staged as an
// urgent response: a greeting is what a reader is waiting for, and it may replace
// a staged telemetry line rather than queue behind it.
void VehicleRuntime::stageBootLine() {
    serialTransport::LineTransport& transport = serialTransport::port();
    ev::BoundedText text(transport.workingBuffer(), ev::kSerialLineCapacity);
    text.appendText("BOOT");
    text.field("fw").appendText(vehicleConfig::kFirmwareId);
    text.field("schema").appendUnsigned(ev::kTelemetrySchemaVersion);
    text.field("cadence_ms").appendUnsigned(vehicleConfig::kControlPeriodMs);
    text.field("watchdog").appendBool(kWatchdogEnablementRequested);
    text.field("checks_all").appendBool(configurationChecks_.allPass());
    text.field("regulator").appendBool(regulatorEnabled_);
    text.field("armed").appendBool(authority_.isArmed());
    text.field("telemetry").appendBool(telemetrySession_.enabled());
    text.field("stride").appendUnsigned(telemetrySession_.strideTicks());
    // The operator interface itself: the loop is running and commands will be
    // answered from this pass on. It is the one field of the boot line that is
    // about the interface rather than about the chain.
    text.field("ready").appendBool(true);
    respondLine(text.truncated() ? ev::kBoundedLineDidNotFit : text.length());
}

void VehicleRuntime::serviceSerial(uint32_t nowMs) {
    serviceStagedText(nowMs);

    // Received bytes, bounded per pass. A line that arrives slowly is accumulated
    // across passes; nothing here waits for the rest of it.
    //
    // At most one command is applied per pass, which is what keeps the answer
    // contract honest: an operator's reply is one staged line, and a host that
    // pipelines several terminators into one read window waits a pass per command
    // rather than letting an earlier answer be displaced by the next response.
    // Bytes that arrived behind the handled line stay in the port and are read
    // next pass.
    uint8_t budget = serialTransport::kReadBytesPerPass;
    while (budget > 0U) {
        --budget;
        const int16_t received = serialTransport::port().readByte();
        if (received < 0) {
            break;
        }
        // A byte from the host is the clearest evidence that a reader is there,
        // and it restarts the silence interval: an operator typing commands is one
        // session and is greeted once, not once per command (finding F-4).
        greeter_.observeHostByte(nowMs);
        const ev::LineEvent event = lineReader_.push(static_cast<char>(received));
        if (event == ev::LineEvent::LineReady) {
            handleCommandLine(lineReader_.line(), lineReader_.lineLength(), nowMs);
            break;  // one command per pass: the rest of the window waits
        }
        if (event == ev::LineEvent::LineTooLong) {
            rejectWithoutCommand(ev::CommandError::LineTooLong);
            break;  // the refusal is this pass' one answer
        }
        if (event == ev::LineEvent::InvalidCharacter) {
            rejectWithoutCommand(ev::CommandError::InvalidCharacter);
            break;  // the refusal is this pass' one answer
        }
        // EmptyLine: a bare terminator (including the `\n` of a `\r\n` pair) is
        // not an error and carries no command; it is not an answer, so reading
        // continues past it.
    }

    // Telemetry last, so the operator's own feedback outranks the log.
    drainTelemetry();
}

// A rejection that has no command to name: the line never became one.
void VehicleRuntime::rejectWithoutCommand(ev::CommandError error) {
    ++commandsRejected_;
    const char* reason = ev::commandErrorName(error);
    respondRefusal(ev::OperatorCommandId::None, reason);
    pushOperatorEvent(ev::TelemetryEventCode::CommandRejected, reason);
}

void VehicleRuntime::handleCommandLine(const char* line, uint16_t length, uint32_t nowMs) {
    const ev::CommandParseResult parsed = ev::parseOperatorCommand(line, length);
    if (!parsed.ok) {
        rejectWithoutCommand(parsed.error);
        return;
    }

    const ev::OperatorCommandId id = parsed.request.id;
    const ev::CommandAdmissibility admissible = ev::admissibilityOf(id, systemState());
    if (!admissible.allowed) {
        // The state owns the decision, not the command: a run that has started,
        // an announced run, a latched fault. The refusal names which one.
        ++commandsRejected_;
        const char* reason = ev::commandRefusalName(admissible.refusal);
        respondRefusal(id, reason);
        pushOperatorEvent(ev::TelemetryEventCode::CommandRejected, reason);
        return;
    }

    // A `config set` is judged on its values here, before anything is stored: a
    // held value is echoed back by `config show` and cannot be told apart from a
    // value the operator meant, so an out-of-range or non-finite number must not
    // become state (H0 finding F-5). The refusal is classified as a rejection, not
    // an acceptance, so the counters keep meaning "applied" and "not applied".
    if (id == ev::OperatorCommandId::ConfigSet) {
        const ev::RunConfigValidation supplied =
            ev::validateSuppliedConfigFields(parsed.request.fields);
        if (!supplied.ok) {
            ++commandsRejected_;
            respondLine(ev::formatConfigSetRefusal(
                supplied.error, serialTransport::port().workingBuffer(), ev::kSerialLineCapacity));
            pushOperatorEvent(ev::TelemetryEventCode::CommandRejected,
                              ev::telemetryConfigErrorName(supplied.error));
            return;
        }
    }

    ++commandsAccepted_;
    applyCommand(parsed.request, nowMs);
}

void VehicleRuntime::applyCommand(const ev::OperatorRequest& request, uint32_t nowMs) {
    const uint16_t capacity = ev::kSerialLineCapacity;

    // The shared line buffer is fetched *inside* the cases that format a line,
    // never here. Fetching it is now a stateful act - it retires a staged reply
    // that had not started moving, because the caller is about to overwrite it
    // (`ev::LineFramer::workingBuffer()`, and the M2 repair in S15) - so a
    // command that formats nothing (`help`, which only arms the static-line
    // cursor) must not touch it. Reaching for the buffer before the switch would
    // have discarded a waiting answer for a command that produced none.
    switch (request.id) {
        case ev::OperatorCommandId::None:
            // Unreachable: `admissibilityOf()` refuses a request that carries no
            // command. Listed so a new command cannot be forgotten silently.
            return;

        case ev::OperatorCommandId::Help:
            // The cursor in `serviceStagedText()` emits the lines, as many per
            // pass as the port accepts, bounded by the table's line count.
            helpLineIndex_ = 0U;
            return;

        case ev::OperatorCommandId::Status:
            respondLine(ev::formatStatusLine(statusView(), serialTransport::port().workingBuffer(),
                                            capacity));
            return;

        case ev::OperatorCommandId::Version:
            respondLine(ev::formatVersionLine(vehicleConfig::kFirmwareId,
                                              serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::Checks:
            respondLine(ev::formatChecksLine(configurationChecks_,
                                             serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::Boot:
            // The greeting on demand. The answer *is* the boot line, so it is the
            // same formatter the automatic greeting uses and there is no separate
            // acknowledgment to keep in agreement with it. Delivering it here is
            // what keeps it to one line: `owed()` is cleared as the line is
            // staged, so the next pass cannot greet a second time, and asking for
            // it is itself host activity, so the silence interval restarts.
            greeter_.delivered(nowMs);
            stageBootLine();
            return;

        case ev::OperatorCommandId::ConfigShow:
        case ev::OperatorCommandId::ConfigCheck:
            // Both read the held configuration through the single rule-validation
            // entry point, so the answer cannot depend on which was typed.
            respondLine(ev::formatConfigEcho(heldConfig_, heldVerdict(),
                                             serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::Plan:
            // The read-only sanity check of the exact plan this vehicle is holding.
            // It reads the plan and renders it; it adopts nothing, clears nothing
            // and changes no phase, so it is admissible in every state - including
            // during a run - and cannot gate the control path.
            respondLine(ev::formatPlanLine(planView(), serialTransport::port().workingBuffer(),
                                           capacity));
            return;

        case ev::OperatorCommandId::ConfigSet:
            applyConfigSet(request.fields);
            return;

        case ev::OperatorCommandId::ConfigClear:
            heldConfig_ = ev::RunConfigFields{};
            pushOperatorEvent(ev::TelemetryEventCode::ConfigCleared, ev::kTelemetryMissingField);
            respondLine(ev::formatOperatorAck(ev::OperatorCommandId::ConfigClear,
                                              serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::Announce:
            applyAnnounce(nowMs);
            return;

        case ev::OperatorCommandId::Arm:
            applyArm();
            return;

        case ev::OperatorCommandId::Disarm:
            // Always available, always safe. The latch drops here, but the
            // operator path itself never writes a pin: the de-energized command
            // reaches the driver through the tick's MotorCommand stage on the
            // next control tick, bounded by one control period
            // (`vehicleConfig::kControlPeriodMs`). The `Disarmed` transition
            // itself is reported by the detector on that same tick: the operator
            // path does not duplicate what a sample can show.
            authority_.disarm();
            respondLine(ev::formatOperatorAck(ev::OperatorCommandId::Disarm,
                                              serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::Reset:
            applyReset(nowMs);
            return;

        case ev::OperatorCommandId::TelemetryOn:
            telemetrySession_.enable();
            pushOperatorEvent(ev::TelemetryEventCode::TelemetryEnabled, ev::kTelemetryMissingField);
            respondLine(ev::formatTelemetryAck(ev::OperatorCommandId::TelemetryOn, telemetryView(),
                                               serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::TelemetryOff:
            telemetrySession_.disable();
            respondLine(ev::formatTelemetryAck(ev::OperatorCommandId::TelemetryOff, telemetryView(),
                                               serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::TelemetryStatus:
            respondLine(ev::formatTelemetryState(telemetryView(),
                                                 serialTransport::port().workingBuffer(), capacity));
            return;

        case ev::OperatorCommandId::TelemetryRate:
            applyTelemetryRate(request.telemetryRateHz);
            return;
    }
}

// Stores the fields the operator named. Only the fields that were supplied are
// touched, so `config set` can be typed in any order and repeated. What can reach
// here has already passed the per-field rule checks
// (`ev::validateSuppliedConfigFields()`, called from `handleCommandLine()`), so no
// out-of-range or non-finite value is ever stored; the announced grid and the
// cross-field geometry checks still belong to `validateRunConfig()` through the
// policy, which `config show` and `announce` run.
void VehicleRuntime::applyConfigSet(const ev::RunConfigFields& supplied) {
    if (supplied.hasLevel) {
        heldConfig_.hasLevel = true;
        heldConfig_.levelIndex = supplied.levelIndex;
    }
    if (supplied.hasTargetDistanceM) {
        heldConfig_.hasTargetDistanceM = true;
        heldConfig_.targetDistanceM = supplied.targetDistanceM;
    }
    if (supplied.hasTargetTimeS) {
        heldConfig_.hasTargetTimeS = true;
        heldConfig_.targetTimeS = supplied.targetTimeS;
    }
    if (supplied.hasBottleLineDistanceM) {
        heldConfig_.hasBottleLineDistanceM = true;
        heldConfig_.bottleLineDistanceM = supplied.bottleLineDistanceM;
    }

    pushOperatorEvent(ev::TelemetryEventCode::ConfigSet, ev::kTelemetryMissingField);
    respondLine(ev::formatConfigSetAck(heldConfig_, heldVerdict(),
                                       serialTransport::port().workingBuffer(),
                                       ev::kSerialLineCapacity));
}

void VehicleRuntime::applyAnnounce(uint32_t nowMs) {
    const ev::RunConfig config = ev::completedRunConfig(heldConfig_);
    const ev::RunAxisGeometry geometry = ev::RunAxisGeometry::fromRunConfig(config);

    // The run's **diagnostic** timing estimate: what the announced Target Time,
    // the announced geometry and the configured limits imply kinematically. It is
    // built and reported, but it decides nothing on its own. While the timing
    // model is unvalidated (`vehicleConfig::kRunTimingModelValidated`) the run's
    // cruise is the configured strategy cruise and its deliberate delay is 0, and
    // the estimate may not refuse an otherwise legal announcement (S7).
    const ev::RunTimingPlan timingEstimate =
        ev::planRunTiming(vehicleConfig::runTimingPlanInputs(geometry, config.targetTimeS));

    // The one timing decision this boundary makes: whether that estimate may
    // control the run. Both the production readiness flag and the estimate's own
    // validity must agree, so a validated model whose estimate refused on this
    // announcement falls back to the configured cruise and no delay rather than to
    // a half-planned run.
    const bool timingReady = vehicleConfig::kRunTimingModelValidated && timingEstimate.valid;
    const double predictedMotionTimeS = timingReady ? timingEstimate.travelDurationS : 0.0;
    const double initialDelayS = timingReady ? timingEstimate.plannedDelayS : 0.0;

    // This run's immutable execution plan, built before the run is announced so
    // that the plan's own `initialDelayS` is the delay the run is announced with.
    // The plan owns the run's deliberate start delay (and its execution targets);
    // with `timingReady` false it records `initialDelayS = 0.0` regardless of the
    // numbers above - the honest production value while the model is unvalidated.
    const ev::RunExecutionPlan executionPlan =
        ev::buildRunExecutionPlan(config, vehicleConfig::bottleEvidenceConfig(),
                                  vehicleConfig::runExecutionDependencies(), timingReady,
                                  predictedMotionTimeS, initialDelayS)
            .plan;

    const ev::AnnounceVerdict verdict =
        ev::announceVerdictFor(heldConfig_, configurationChecks_, timingEstimate);
    if (!verdict.ok) {
        // The gate refuses without latching anything: a half-typed configuration
        // is a refusal to report, not a fault to clear.
        respondLine(ev::formatAnnounceRefusal(verdict, serialTransport::port().workingBuffer(),
                                              ev::kSerialLineCapacity));
        pushOperatorEvent(ev::TelemetryEventCode::AnnounceRefused,
                          ev::announceRefusalName(verdict.refusal));
        return;
    }

    // The run is announced with the delay its own execution plan states; the phase
    // policy contributes only the legal/safety maximum `announceRun()` bounds it
    // with. This is the one place a per-run delay reaches the run.
    const ev::RunStartStatus status = run_.announceRun(
        config, ev::validateRunConfig(config), executionPlan.initialDelayS, nowMs);
    if (status != ev::RunStartStatus::Started) {
        // The gate accepted what the policy then refused: a caller bug, reported
        // rather than left as a half-announced run. `InitialDelayNotUsable` is the
        // one status the gate cannot have covered (the plan's delay is bounded by
        // the policy's own maximum), so it is named rather than collapsed into a
        // generic refusal.
        const char* reason = status == ev::RunStartStatus::InitialDelayNotUsable
                                 ? kReasonDelayNotPermitted
                                 : ev::kOperatorReasonChecksFailed;
        respondRefusal(ev::OperatorCommandId::Announce, reason);
        pushOperatorEvent(ev::TelemetryEventCode::AnnounceRefused, reason);
        return;
    }

    announcedConfigValid_ = true;
    // The geometry the announced configuration defines. The origin stays where it
    // is: the Start Point is established at the arming decision, which is the
    // moment the vehicle is staged on the line.
    axisGeometry_ = geometry;
    // The diagnostic timing estimate is held as the record of what the announced
    // configuration implied. It decides nothing: the run's delay is the execution
    // plan's (stored by `announceRun()` just above), and the shaped cruise below is
    // the configured one unless a validated timing model chose otherwise.
    timingPlan_ = timingEstimate;
    runProfileLimits_ = timingReady
                            ? vehicleConfig::profileLimitsWithCruise(timingEstimate.plannedCruiseMps)
                            : vehicleConfig::profileLimits();
    // This run's execution plan, from the same announcement the rule geometry above
    // came from and the same `Sourced` dependencies the configuration carries. It
    // owns the run's ONE forward execution target and the run's ONE deliberate
    // start delay, and it is adopted even when it refuses: a refusal is a fact
    // about this run (`MissingForwardExecutionTarget` while the margin is a
    // placeholder) and the named reason is what a diagnostic reports. With a
    // refused plan the run has no forward target to shape and no forward
    // completion to claim, so it cannot travel forward - the fail-closed
    // behaviour, and the state of this vehicle today.
    executionPlan_.adopt(executionPlan);
    legForward_ = true;
    // The plan's fate is no longer silent on the evidence lanes (audit P12 /
    // OBS-FWD): the `AnnounceAccepted` event carries the plan's named status as
    // its detail, so the event lane says whether this run has a forward execution
    // target and, if not, which refusal explains it; the acknowledgement carries
    // the same status plus the admitted forward stop in metres when there is one.
    // Both are facts about the plan just adopted - nothing here fabricates a stop
    // when the plan refused, and the `plan` query remains the live view.
    pushOperatorEvent(ev::TelemetryEventCode::AnnounceAccepted,
                      ev::runExecutionPlanStatusName(executionPlan.status));
    ev::ExecutionPlanEcho planEcho;
    planEcho.status = executionPlan.status;
    if (executionPlan.valid) {
        planEcho.forwardStopMpPositionM = executionPlan.forwardStopMpPositionM;
    }
    respondLine(ev::formatAnnounceAck(verdict, planEcho, serialTransport::port().workingBuffer(),
                                     ev::kSerialLineCapacity));
}

void VehicleRuntime::applyArm() {
    const ev::ArmVerdict verdict = ev::armVerdictFor(configurationChecks_);
    if (!verdict.ok) {
        respondLine(ev::formatArmRefusal(verdict, serialTransport::port().workingBuffer(),
                                         ev::kSerialLineCapacity));
        pushOperatorEvent(ev::TelemetryEventCode::ArmRefused,
                          ev::configurationCheckName(verdict.firstFailedCheck));
        return;
    }

    // The arm-time requirement `admissibilityOf()` already enforces, re-asserted
    // where the authority is actually asked (S16). Same condition, same named
    // refusal: the run that was announced must still hold a usable execution
    // plan. It is repeated here because this is the boundary the invariant is
    // about - an invalid plan must not reach `authority_.arm()` no matter which
    // caller got this far, and the plan is read live rather than from a cached
    // copy of the state the command was admitted against.
    if (!executionPlan_.plan().valid) {
        const char* reason =
            ev::commandRefusalName(ev::CommandRefusal::ExecutionPlanNotEstablished);
        respondRefusal(ev::OperatorCommandId::Arm, reason);
        pushOperatorEvent(ev::TelemetryEventCode::ArmRefused, reason);
        return;
    }

    // The authority decides, and it can still refuse (a latched fault of its own);
    // the operator sees that as a refusal rather than as an armed vehicle.
    if (!authority_.arm(verdict.readiness)) {
        respondRefusal(ev::OperatorCommandId::Arm, ev::kOperatorReasonChecksFailed);
        pushOperatorEvent(ev::TelemetryEventCode::ArmRefused, ev::kOperatorReasonChecksFailed);
        return;
    }

    // The Start Point, established at the deliberate arming decision: this is the
    // moment the vehicle is staged on the line, and the encoder reading here is
    // what every later position is measured from. The run axis has no meaning
    // before an announcement (its geometry is zero), so this is the first reading
    // that can serve as an origin.
    runOriginTicks_ = encoderInput::snapshotTicks().ticks;
    // The driver-fault-low cycle counter is evidence about this run, not
    // since-boot: arming is where a run starts, so it starts counting again here
    // (H1B Part 3 and H9 read it, and a since-boot total would mix sessions).
    cyclesWithDriverFaultLow_ = 0U;
    respondLine(ev::formatOperatorAck(ev::OperatorCommandId::Arm,
                                     serialTransport::port().workingBuffer(),
                                     ev::kSerialLineCapacity));
    // The `Armed` transition is reported by the detector on the next tick.
}

void VehicleRuntime::applyReset(uint32_t nowMs) {
    // The whole new-run sequence, from the module that owns it - not a subset
    // assembled here: run policy, regulator, estimator, target and authority.
    // Direction history is deliberately retained; software reset proves no stop.
    const ev::MotionChainResetReport report = ev::resetMotionChainForNewRun(
        estimator_, velocityController_, profileTarget_, authority_, run_, nowMs);
    // The sole ISR-shared decoder reset owner (S18C-M1): reset and reference
    // sampling together inside the adapter's PRIMASK-preserving guard. The pure
    // chain never mutates that decoder. This establishes software coherence,
    // not mechanical standstill or lossless capture of moving electrical edges.
    encoderInput::resetAndReestablishReference();

    // Nothing that measured the previous run may survive the boundary (audit M12
    // / S13I I-3): the estimator's and decoder's own history is gone above, and
    // these are the runtime-held copies that described it. Without this, a stop
    // verification within the stop-evidence age of a reset could pair a fresh
    // zero-speed claim with the previous run's speed fact - unreachable in a
    // physical run, but a reset must establish "nothing measured", not a mixture.
    estimate_ = ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    hasEstimate_ = false;
    lastValidEstimate_ = ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    hasLastValidEstimate_ = false;
    // The recovery verifier restarts with the run: a fresh run must not inherit
    // a previous run's observed standstill, and a fresh barrier must not be
    // discharged by it either (S18A/P2). Live evidence re-establishes it.
    recoveryStopVerifier_.reset();
    // The feedback-progress supervision restarts with the run: no previous
    // attempt's ticks or powered time may bound the next run, and the next
    // powered travel takes a fresh reference with the full allowance (S18C/P4).
    // This clears evidence; it never discharges the S18A direction barrier.
    feedbackMonitor_.reset();
    lastCycleAuthorizedPowered_ = false;
    odometryTravel_ = ev::OdometryEstimate{0.0, false, ev::OdometryEstimateStatus::ScaleIsNotMeasured};
    odometrySampleMs_ = 0U;
    hasOdometrySample_ = false;
    positionUsable_ = false;

    regulatorEnabled_ = report.controllerEnabled;
    announcedConfigValid_ = false;
    // The held configuration is deliberately *not* discarded: it is what the
    // operator typed, and a new run must be announced with it again - one command
    // - rather than retyped. `config clear` is the command that discards it.
    axisGeometry_ = ev::RunAxisGeometry(0.0, 0.0, 0.0);
    // The announced run's plan and its shaped limits go with the run: the next one
    // is planned from its own announcement (which may name a different Target
    // Time), and no planned cruise survives a new-run boundary.
    timingPlan_ = ev::refusedTimingPlan(ev::RunTimingPlanStatus::NotPlanned);
    // The execution plan goes with it too: there is no run to have execution
    // targets until the next announcement produces its own plan.
    executionPlan_.clear();
    runProfileLimits_ = vehicleConfig::profileLimits();
    legForward_ = true;
    runOriginTicks_ = encoderInput::snapshotTicks().ticks;
    cyclesWithDriverFaultLow_ = 0U;

    const char* clearedFault = report.clearedFault == ev::FaultCode::None
                                   ? ev::kTelemetryMissingField
                                   : ev::telemetryFaultName(report.clearedFault);
    pushOperatorEvent(ev::TelemetryEventCode::ChainReset, clearedFault);
    respondLine(ev::formatOperatorAck(ev::OperatorCommandId::Reset,
                                     serialTransport::port().workingBuffer(),
                                     ev::kSerialLineCapacity));
}

void VehicleRuntime::applyTelemetryRate(double requestedRateHz) {
    const uint16_t cadenceHz = static_cast<uint16_t>(1000U / vehicleConfig::kControlPeriodMs);
    const ev::TelemetryRateVerdict verdict = ev::telemetryStrideForRate(requestedRateHz, cadenceHz);
    if (!verdict.ok) {
        const char* reason = ev::telemetryRateRefusalName(verdict.refusal);
        respondRefusal(ev::OperatorCommandId::TelemetryRate, reason);
        pushOperatorEvent(ev::TelemetryEventCode::CommandRejected, reason);
        return;
    }

    telemetrySession_.setStrideTicks(verdict.strideTicks);
    respondLine(ev::formatTelemetryRateAck(verdict, serialTransport::port().workingBuffer(),
                                          ev::kSerialLineCapacity));
}

// ---------------------------------------------------------------------------
// The operator interface's readings
// ---------------------------------------------------------------------------

void VehicleRuntime::respondLine(uint16_t length) {
    serialTransport::LineTransport& transport = serialTransport::port();
    if (length == ev::kBoundedLineDidNotFit) {
        ++responsesLost_;
        return;
    }
    if (!transport.stage(length, true)) {
        ++responsesLost_;
        return;
    }
    transport.pump();
}

void VehicleRuntime::respondRefusal(ev::OperatorCommandId id, const char* reason) {
    respondLine(ev::formatOperatorRefusal(id, reason, serialTransport::port().workingBuffer(),
                                         ev::kSerialLineCapacity));
}

// An event the sample train cannot show: an operator's action or its outcome. The
// transitions that a sample *can* show (arming, a fault, a phase change) are left
// to the detector, so a reader never sees the same transition twice.
void VehicleRuntime::pushOperatorEvent(ev::TelemetryEventCode code, const char* detail) {
    ev::TelemetryEvent event;
    event.tMs = telemetry_.nowMs;
    event.phase = run_.phase();
    event.armed = authority_.isArmed();
    event.event = code;
    event.detail = detail;
    event.fault = telemetry_.fault;
    telemetrySession_.pushEvent(event);
}

ev::OperatorSystemState VehicleRuntime::systemState() const {
    ev::OperatorSystemState state;
    state.phase = run_.phase();
    state.armed = authority_.isArmed();
    // Either fault of record blocks everything but `reset`: the run policy's and
    // the authority's are separate latches, and `reset` clears both.
    state.faultLatched = faultLatched();
    state.checks = configurationChecks_;
    // The plan the run layer is holding right now, read here instead of being
    // cached with the checks: every command is judged against the announcement
    // that is current when it is typed, so a plan cleared by `reset` or replaced
    // by a new announcement cannot be read as the previous one's (S16). It is the
    // adopted plan's own invariant that makes this one fact sufficient - plan
    // positions exist only when `valid` is true, so "usable" is exactly what the
    // run has to travel with.
    state.executionPlanEstablished = executionPlan_.plan().valid;
    return state;
}

ev::OperatorStatusView VehicleRuntime::statusView() const {
    ev::OperatorStatusView view;
    view.state = buildSample();
    view.config = heldConfig_;
    view.configVerdict = heldVerdict();
    return view;
}

ev::OperatorPlanView VehicleRuntime::planView() const {
    ev::OperatorPlanView view;
    // The plan the run layer is holding: the immutable value adopted at the
    // announcement boundary, or `NotPlanned` before the first announcement and
    // after a new-run boundary. Copied field by field, so nothing here can write
    // to it - the plan is `const` and this is the only read of it.
    const ev::RunExecutionPlan& plan = executionPlan_.plan();
    view.status = plan.status;
    if (plan.valid) {
        view.earliestContactMpPositionM = plan.earliestContactMpPositionM;
        view.latestContactMpPositionM = plan.latestContactMpPositionM;
        view.captureGuaranteedMpPositionM = plan.captureGuaranteedMpPositionM;
        view.forwardStopMpPositionM = plan.forwardStopMpPositionM;
        view.reverseStopMpPositionM = plan.reverseStopMpPositionM;
    }
    view.timingReady = plan.timingReady;
    view.predictedMotionTimeS = plan.predictedMotionTimeS;
    view.initialDelayS = plan.initialDelayS;

    // The announced rule geometry and the announced Target Time, which are facts
    // about the announcement rather than about the plan: a refused plan still has
    // one, and reporting it is how "the geometry is what you typed, and the plan
    // refuses for this reason" can be read off one line. `announcedConfigValid_`
    // is set at the announcement boundary and cleared at a new-run boundary, which
    // is exactly the lifetime of the run's announced geometry.
    if (announcedConfigValid_) {
        view.ruleGeometryKnown = true;
        view.geometry = axisGeometry_;
        view.targetTimeKnown = heldConfig_.hasTargetTimeS;
        view.targetTimeS = heldConfig_.targetTimeS;
    }
    return view;
}

ev::OperatorTelemetryView VehicleRuntime::telemetryView() const {
    ev::OperatorTelemetryView view;
    view.enabled = telemetrySession_.enabled();
    view.strideTicks = telemetrySession_.strideTicks();
    view.controlCadenceHz = static_cast<uint16_t>(1000U / vehicleConfig::kControlPeriodMs);
    view.drops = telemetrySession_.drops();
    return view;
}

ev::HeldRunConfigVerdict VehicleRuntime::heldVerdict() const {
    return ev::heldRunConfigVerdict(heldConfig_);
}

}  // namespace evhw
