#include "VehicleRuntime.h"

#include <MotorDriverPlan.h>
#include <NearZeroVerifier.h>
#include <SafetyLimits.h>
#include <Snapshots.h>

#include "EncoderInput.h"
#include "MotorDriver.h"

namespace evhw {

using ev::ControlTickStage;

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
        ev::remainingToTargetM(axisGeometry_, legForward_, axisPosition.positionM);
    const double legRemainingM = ev::legRemainingM(axisGeometry_, legForward_, axisPosition.positionM);

    // ---------------------------------------------------------------- stage 3
    // Raw inputs. Nothing here is converted or interpreted: the counts are counts
    // and the levels are levels (`SOFTWARE_ARCHITECTURE.md` section 12).
    orderGuard_.enter(ControlTickStage::SensorSnapshot);
    rawInputs_ = sensorInputs::sample();
    if (rawInputs_.driverFaultBarLow) {
        ++cyclesWithDriverFaultLow_;
    }

    // ---------------------------------------------------------------- stage 4
    // This cycle's health: the configuration-build verdicts the boundary's
    // readiness comes from, and any fault this layer can name.
    //
    // The driver fault line is interpreted by `ev::observedDriverFault()`, which
    // refuses to claim a fault while the line's polarity is unverified
    // (`VERIFIED_STATE.md` #1, #2). With the shipped configuration that is what
    // happens: the level is sampled, counted and reported as a raw level, and no
    // fault is invented from it. The refusal is not a gap in the tick - it is the
    // one honest answer available, because an assumed polarity would either turn a
    // healthy driver into a latched fault or turn a real fault into a log entry
    // nobody acts on. Once H1B has measured the polarity, promoting
    // `kPlannedDriverFaultPolarity` gives this line an identity, and the authority
    // latches it exactly as it latches any other fault. The
    // `DriverFaultPolarityVerified` check is what keeps the vehicle unarmable until
    // then, so "the fault input is invisible" can never be the state of an armed
    // vehicle.
    orderGuard_.enter(ControlTickStage::FaultEvaluation);
    configurationChecks_ = vehicleConfig::configurationBuildChecks(announcedConfigValid_, run_);
    const ev::FaultCode observedFault =
        ev::observedDriverFault(rawInputs_.driverFaultBarLow, vehicleConfig::plannedDriverFaultPolarity());

    // ---------------------------------------------------------------- stage 5
    // The run policy, fed only this cycle's evidence. The distance it consumes is
    // the previous cycle's leg (the leg this cycle will name does not exist yet),
    // which is the wiring order the scenarios pinned.
    orderGuard_.enter(ControlTickStage::RunStateMachine);
    ev::RunInputs runInputs;
    runInputs.authorityArmed = authority_.isArmed();
    runInputs.startTriggered = rawInputs_.startTriggerActive;
    // Bottle contact, capture and push are evidence the vehicle must produce. The
    // mechanism does not exist, so none is reported and no phase that waits for
    // one can advance (`VERIFIED_STATE.md` #11).
    runInputs.bottleContactDetected = false;
    runInputs.captureComplete = false;
    runInputs.pushComplete = false;
    runInputs.remainingDistanceUsable = positionUsable_;
    runInputs.remainingDistanceM = remainingToTargetM;
    runInputs.stopEvidence = ev::composeStopEvidence(
        estimate_, hasEstimate_, lastValidEstimate_, hasLastValidEstimate_, nowMs,
        vehicleConfig::kMaxSampleAgeMs, vehicleConfig::kStopEvidenceMaxAgeMs);
    runInputs.detectedFault = observedFault;

    const ev::RunStatus status = run_.update(nowMs, runInputs);

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

    const ev::MotionProfileCommand shaped = ev::nextTargetVelocity(
        profileTarget_.inputsFor(travel ? ev::MotionProfileIntent::Travel
                                        : ev::MotionProfileIntent::Stop,
                                 direction, legRemainingM, dtMs),
        profileLimits_.forDirection(direction));
    profileTarget_.accept(shaped);

    // A travel request may only be presented as powered once the shaped target
    // points along the leg the run asked for. The profile deliberately ramps a
    // previous direction to zero before it flips the sign, and presenting the
    // regulator's effort while the shape is still on the old side would ask for
    // powered effort in the direction the run has just stopped travelling in.
    const bool shapePointsAlongLeg =
        ev::targetVelocityPointsAlongLeg(shaped, direction, profileLimits_.forDirection(direction));

    // ---------------------------------------------------------------- stage 7
    // The effort: the regulator's, or the run's own deliberate stop mapping. The
    // regulator is deliberately not stepped for a stop, so an unactuated cycle
    // accumulates no regulator state against a target nothing is following.
    orderGuard_.enter(ControlTickStage::MotionControl);
    ev::MotorEffort effort(ev::MotorMode::Disabled, 0U);
    bool regulatorStepped = false;
    ev::VelocityControlStatus controlStatus = ev::VelocityControlStatus::Disabled;
    if (travel && intentionCurrent && shapePointsAlongLeg) {
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
    safetyState.atZeroSpeed = status.stopVerified;
    safetyState.nowMs = nowMs;
    safetyState.detectedFault =
        status.fault != ev::FaultCode::None ? status.fault : observedFault;

    const ev::AuthorizationResult authorized = authority_.authorize(effort, safetyState, safetyLimits_);

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
    snapshot.measuredVelocityMps = estimate_.velocityMps;
    snapshot.velocityUsable = velocityReady.usable;
    snapshot.velocityStatus = estimate_.status;
    snapshot.positionUsable = positionUsable_;
    snapshot.batteryCounts = rawInputs_.batteryCounts;
    snapshot.currentCounts = rawInputs_.currentCounts;
    snapshot.driverFaultBarLow = rawInputs_.driverFaultBarLow;
    snapshot.startTriggerActive = rawInputs_.startTriggerActive;
    snapshot.driverFaultLowCycles = cyclesWithDriverFaultLow_;
    snapshot.driverFaultPolarityVerified = configurationChecks_.driverFaultPolarityVerified;
    snapshot.phase = status.phase;
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
    // configuration-build verdicts, and the `EVE1` boot record carries none of
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
    char* buffer = serialTransport::port().workingBuffer();

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
            respondLine(ev::formatStatusLine(statusView(), buffer, capacity));
            return;

        case ev::OperatorCommandId::Version:
            respondLine(ev::formatVersionLine(vehicleConfig::kFirmwareId, buffer, capacity));
            return;

        case ev::OperatorCommandId::Checks:
            respondLine(ev::formatChecksLine(configurationChecks_, buffer, capacity));
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
            respondLine(ev::formatConfigEcho(heldConfig_, heldVerdict(), buffer, capacity));
            return;

        case ev::OperatorCommandId::ConfigSet:
            applyConfigSet(request.fields);
            return;

        case ev::OperatorCommandId::ConfigClear:
            heldConfig_ = ev::RunConfigFields{};
            pushOperatorEvent(ev::TelemetryEventCode::ConfigCleared, ev::kTelemetryMissingField);
            respondLine(ev::formatOperatorAck(ev::OperatorCommandId::ConfigClear, buffer, capacity));
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
            respondLine(ev::formatOperatorAck(ev::OperatorCommandId::Disarm, buffer, capacity));
            return;

        case ev::OperatorCommandId::Reset:
            applyReset(nowMs);
            return;

        case ev::OperatorCommandId::TelemetryOn:
            telemetrySession_.enable();
            pushOperatorEvent(ev::TelemetryEventCode::TelemetryEnabled, ev::kTelemetryMissingField);
            respondLine(ev::formatTelemetryAck(ev::OperatorCommandId::TelemetryOn, telemetryView(),
                                               buffer, capacity));
            return;

        case ev::OperatorCommandId::TelemetryOff:
            telemetrySession_.disable();
            respondLine(ev::formatTelemetryAck(ev::OperatorCommandId::TelemetryOff, telemetryView(),
                                               buffer, capacity));
            return;

        case ev::OperatorCommandId::TelemetryStatus:
            respondLine(ev::formatTelemetryState(telemetryView(), buffer, capacity));
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
    const ev::AnnounceVerdict verdict = ev::announceVerdictFor(heldConfig_, configurationChecks_);
    if (!verdict.ok) {
        // The gate refuses without latching anything: a half-typed configuration
        // is a refusal to report, not a fault to clear.
        respondLine(ev::formatAnnounceRefusal(verdict, serialTransport::port().workingBuffer(),
                                              ev::kSerialLineCapacity));
        pushOperatorEvent(ev::TelemetryEventCode::AnnounceRefused,
                          ev::announceRefusalName(verdict.refusal));
        return;
    }

    const ev::RunConfig config = ev::completedRunConfig(heldConfig_);
    const ev::RunStartStatus status =
        run_.announceRun(config, ev::validateRunConfig(config), nowMs);
    if (status != ev::RunStartStatus::Started) {
        // The gate accepted what the policy then refused: a caller bug, reported
        // rather than left as a half-announced run.
        respondRefusal(ev::OperatorCommandId::Announce, ev::kOperatorReasonChecksFailed);
        pushOperatorEvent(ev::TelemetryEventCode::AnnounceRefused,
                          ev::kOperatorReasonChecksFailed);
        return;
    }

    announcedConfigValid_ = true;
    // The geometry the announced configuration defines. The origin stays where it
    // is: the Start Point is established at the arming decision, which is the
    // moment the vehicle is staged on the line.
    axisGeometry_ = ev::runAxisGeometryFor(heldConfig_);
    legForward_ = true;
    pushOperatorEvent(ev::TelemetryEventCode::AnnounceAccepted, ev::kTelemetryMissingField);
    respondLine(ev::formatOperatorAck(ev::OperatorCommandId::Announce,
                                     serialTransport::port().workingBuffer(),
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
    // assembled here: the run policy, the regulator's control state, the decoder,
    // the estimator, the profile target and the authority all return to their
    // post-construction state, and the report says what was discarded.
    const ev::MotionChainResetReport report = ev::resetMotionChainForNewRun(
        encoderInput::decoder(), estimator_, velocityController_, profileTarget_, authority_, run_,
        nowMs);
    encoderInput::reestablishReference();

    regulatorEnabled_ = report.controllerEnabled;
    announcedConfigValid_ = false;
    // The held configuration is deliberately *not* discarded: it is what the
    // operator typed, and a new run must be announced with it again - one command
    // - rather than retyped. `config clear` is the command that discards it.
    axisGeometry_ = ev::RunAxisGeometry(0.0, 0.0, 0.0);
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
    return state;
}

ev::OperatorStatusView VehicleRuntime::statusView() const {
    ev::OperatorStatusView view;
    view.state = buildSample();
    view.config = heldConfig_;
    view.configVerdict = heldVerdict();
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
