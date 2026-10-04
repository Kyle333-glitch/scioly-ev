#pragma once

#include <Arduino.h>

#include <CompetitionStateMachine.h>
#include <ControlScheduler.h>
#include <ControlTickOrder.h>
#include <HostGreetPolicy.h>
#include <Motion.h>
#include <MotionChainReset.h>
#include <MotionProfile.h>
#include <Odometry.h>
#include <OperatorPolicy.h>
#include <OperatorResponse.h>
#include <RunAxis.h>
#include <SafetyAuthority.h>
#include <SerialCommandParser.h>
#include <TelemetryFormat.h>
#include <TelemetryQueue.h>
#include <VehicleCalibration.h>
#include <VehicleState.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

#include "EncoderInput.h"
#include "SensorInputs.h"
#include "SerialTransport.h"
#include "VehicleConfig.h"
#include "WatchdogCheckpoint.h"

// ---------------------------------------------------------------------------
// The cooperative runtime shell - the vehicle layer's one control tick
// ---------------------------------------------------------------------------
//
// What this layer is
//
//   The production version of the wiring the scripted scenarios pinned
//   (`test/test_scenario/`): it takes the pure chain from `lib/evcore` - decoder,
//   odometry, estimator, run policy, target shaper, regulator, safety boundary -
//   and runs it once per control tick, in the frozen stage order, with the
//   hardware boundary at the ends. It is the only caller of
//   `SafetyAuthority::authorize()`, it presents exactly one request per cycle, and
//   the only command that reaches the motor driver is the one the authority
//   returned.
//
// What this layer is not
//
// * It is not a scheduler of its own: the cadence decision is
//   `ev::ControlScheduler`, which is pure and host-tested, and the order is
//   `ev::ControlTickOrderGuard`, likewise. Both are fixed modules, not
//   conventions this file re-invents.
// * It is not blocking. There is no `delay()` in the pass, no wait for a
//   measurement, and no busy loop: a stage that has no data yet reports that and
//   the chain reacts.
// * It is not an arming decision, even now that the operator interface exists.
//   Nothing here arms by itself: `announce` and `arm` are commands an operator
//   types, each one gated by `ev::OperatorPolicy` and applied through the same
//   transitions the chain already owns (`announceRun()`, `SafetyAuthority::arm()`).
//   With the provisional configuration in `VehicleConfig.h` those gates refuse,
//   so the run policy stays in `Idle` and every drive request is refused by the
//   boundary. That is the intended state of this firmware.
// * It is not a telemetry transport in the blocking sense: the tick captures one
//   snapshot, formats one record when the session says one is due, and stages it.
//   Bytes leave through `serviceSerial()`, which the loop calls *after* the
//   watchdog checkpoint, and no write in this file can wait for a host
//   (`SerialTransport.h`).
//
// The control tick, in the frozen order (`ev::ControlTickStage`)
//
//   1. EncoderSnapshot       one coherent read of the ISR-owned tick count
//   2. OdometryAndVelocity   odometry, the velocity estimate, the two readiness
//                            verdicts and the run-axis position they imply
//   3. SensorSnapshot        the raw battery/current/trigger readings, and the
//                            driver-fault capability (this vehicle has no
//                            external nFAULT line to read)
//   4. FaultEvaluation       the configuration-build verdicts the boundary's
//                            readiness comes from, and this cycle's fault
//   5. RunStateMachine       the run policy, fed only this cycle's evidence
//   6. RunMotionRequest      the run's own intention, the leg tag and the shaped
//                            target - `RunMotionIntention`, never the
//                            vocabulary-only `MotionRequest` type
//   7. MotionControl         the regulator, or the run's deliberate stop mapping
//   8. SafetyAuthorization   exactly one request, one verdict, one command
//   9. MotorCommand          the authorized command, or the driver's de-energized
//                            state when the tick was not trustworthy
//   10. TelemetryCapture     the cycle's snapshot, the optional event hook, and the
//                            one telemetry sample the session is due
//
// The runtime never reads a clock: every method takes the caller's `millis()`
// reading, which is why this file has no timing arithmetic of its own.
//
// The operator interface (P7B)
//
//   `serviceSerial()` is the whole of it, and it runs outside the control tick,
//   after the checkpoint, in the loop's low-priority section. It moves staged
//   bytes out, reads what the host sent, applies at most one command per pass,
//   answers it, and then drains the telemetry queue - in that order, so an
//   operator's own feedback outranks the log. Everything it decides comes from the
//   pure modules: `SerialCommandParser.h` turns bytes into a command or a named
//   rejection, `OperatorPolicy.h` decides whether the current state admits it,
//   `OperatorResponse.h` renders the answer, and the effects go through the same
//   chain calls the control path uses. Nothing in the operator path can drive:
//   there is no command that commands an effort, a mode or a direction.

namespace evhw {

// One control tick's visible state. This is a diagnostic record, not a frozen
// schema: the telemetry contract is undecided (`SOFTWARE_ARCHITECTURE.md` section
// 10) and no transport consumes this yet.
//
// Every field is a value the chain produced or a raw reading this layer took -
// nothing is recomputed for display, so the log cannot disagree with the control
// path about what happened.
struct TelemetrySnapshot {
    // Timing, from the scheduler's decision and its counters.
    uint32_t nowMs = 0U;
    uint32_t dtMs = 0U;
    // This tick's own lateness and the deadlines it dropped.
    uint32_t lateByMs = 0U;
    uint32_t droppedDeadlines = 0U;
    // The session's extremes over every tick since `begin()`, which is what a
    // soak test reports: the scheduler's worst lateness so far, and every
    // deadline dropped so far. The two fields above describe the last tick only
    // (P-H0 Step 7 asks for the worst case, not a spot reading).
    uint32_t worstLateByMs = 0U;
    uint32_t droppedDeadlinesTotal = 0U;
    // The longest gap between two consecutive committed ticks (first tick
    // excluded). Same "session extreme" kind of number, different question: this
    // is the bound on how much near-zero dwell may be credited across an
    // unobserved gap (P-H0W Step 2).
    uint32_t worstStallMs = 0U;
    uint32_t controlTicks = 0U;
    bool clockRewind = false;

    // Measurements and the verdicts that travelled with them.
    int32_t encoderTicks = 0;
    uint32_t countedEdges = 0U;
    uint32_t ignoredUpdates = 0U;
    double runAxisPositionM = 0.0;
    double remainingToTargetM = 0.0;
    double legRemainingM = 0.0;
    double measuredVelocityMps = 0.0;
    bool velocityUsable = false;
    ev::VelocityEstimateStatus velocityStatus = ev::VelocityEstimateStatus::WarmingUp;
    bool positionUsable = false;

    // Raw inputs. Counts and levels, never engineering units.
    uint16_t batteryCounts = 0U;
    uint16_t currentCounts = 0U;
    // Whether the external nFAULT line exists on this vehicle. `NotInstalled` on
    // this build, which is why the level below is not a reading of anything.
    ev::DriverFaultCapability driverFaultCapability = ev::DriverFaultCapability::Installed;
    bool driverFaultBarLow = false;
    bool startTriggerActive = false;
    // How many control ticks have seen a *wired* driver fault line low since
    // boot. A count of a raw level, not a fault: see the tick's own note on the
    // interpretation. Stays zero while the line is not installed - the counter
    // never counts a pin that is not a signal, so a non-zero value here is
    // evidence only on a vehicle that has the line.
    uint32_t driverFaultLowCycles = 0U;
    // True when the external driver-fault configuration is satisfied: the line's
    // polarity has been measured (Installed) or the line does not exist
    // (NotInstalled, in which case there is no polarity to measure). Reported
    // next to `driverFaultCapability` so the two cannot be read apart.
    bool driverFaultConfigurationValid = false;

    // The run policy and the request this cycle presented.
    ev::RunPhase phase = ev::RunPhase::Idle;
    ev::RunMotionCommand runCommand = ev::RunMotionCommand::None;
    bool intentionCurrent = false;
    double targetVelocityMps = 0.0;
    ev::MotionProfileStatus profileStatus = ev::MotionProfileStatus::Ok;
    ev::MotorMode presentedMode = ev::MotorMode::Disabled;
    uint16_t presentedPermille = 0U;
    bool regulatorStepped = false;
    ev::VelocityControlStatus controlStatus = ev::VelocityControlStatus::Disabled;

    // The safety boundary's inputs and its one verdict.
    bool armed = false;
    bool readinessComplete = false;
    bool requiredDataFresh = false;
    bool atZeroSpeed = false;
    ev::FaultCode fault = ev::FaultCode::None;
    ev::AuthzOutcome outcome = ev::AuthzOutcome::DeniedNotArmed;

    // What reached the driver.
    ev::MotorMode commandedMode = ev::MotorMode::Disabled;
    uint16_t commandedPermille = 0U;
    ev::ActuationClass commandedClass = ev::ActuationClass::Disabled;
    ev::PoweredDirection commandedDirection = ev::PoweredDirection::None;

    // The configuration-build verdicts. Every one of these is expected to be
    // false while the values in `VehicleConfig.h` are provisional: this is the
    // evidence that the vehicle cannot be armed, and which check refuses it.
    vehicleConfig::ConfigurationBuildChecks checks{};

    // The stage order of the tick this snapshot describes.
    bool orderInOrder = false;
    ev::ControlTickOrderError orderError = ev::ControlTickOrderError::None;
    ev::ControlTickStage orderStage = ev::ControlTickStage::EncoderSnapshot;
    uint32_t orderViolations = 0U;
};

// An optional event hook, called once per control tick that ran, with the record
// that tick closed. It exists so a later telemetry transport can attach without
// this layer growing any knowledge of it.
//
// Where it runs matters: inside the pass, after the tick's stages and before the
// loop's single watchdog checkpoint. It must therefore not block - a hook that
// waited would hold up that checkpoint and the pass' cadence. No production code
// attaches one: the hook is null in this build, and the tick has already staged
// its record in the queue `serviceSerial()` drains after the checkpoint.
using TelemetryHook = void (*)(void* context, const TelemetrySnapshot& snapshot);

// What one pass of the main loop did.
struct PassOutcome {
    // A control tick was due and ran.
    bool tickRan = false;

    // That tick presented every stage, in order, exactly once.
    bool tickCompleted = false;

    // The tick ran but its stage order was violated. A structural problem in this
    // layer, not a vehicle condition.
    bool orderViolation = false;

    // May the watchdog be refreshed for this pass? True when the pass finished
    // and, if a tick ran, that tick completed in order. Read by the single
    // watchdog checkpoint and by nothing else.
    bool refreshable() const { return !orderViolation; }
};

class VehicleRuntime {
public:
    // Builds the chain from the provisional configuration in `VehicleConfig.h`.
    // Touches no hardware and reads no clock: `begin()` is what initializes the
    // boundary, and it is the only method that may run before the loop does.
    VehicleRuntime();

    // Safe initialization, in the only order that is safe:
    //
    //   1. the motor driver is driven to its de-energized state, before any other
    //      subsystem exists to ask for motion;
    //   2. the encoder interrupt is attached and its reference sample taken;
    //   3. the raw inputs are configured;
    //   4. the chain's per-run state is initialized and the run origin recorded;
    //   5. the configuration-build verdicts are evaluated and reported;
    //   6. the scheduler is started, so the first control tick is one period away.
    //
    // `nowMs` is the caller's `millis()` reading.
    void begin(uint32_t nowMs);

    // One pass of the main loop: the scheduling decision, and the control tick
    // when one is due. Nothing blocks, and a pass that runs no tick is a few
    // comparisons.
    PassOutcome runPass(uint32_t nowMs);

    // The snapshot of the most recent control tick.
    const TelemetrySnapshot& telemetry() const { return telemetry_; }

    // The configuration-build verdicts as of the last tick.
    const vehicleConfig::ConfigurationBuildChecks& configurationChecks() const {
        return configurationChecks_;
    }

    // True when the regulator's control state could be created. It is false
    // while the gains are provisional, which means no powered effort is computed
    // at all - one of the independent reasons this firmware cannot drive.
    bool regulatorEnabled() const { return regulatorEnabled_; }

    // True when the authority is armed. The only path to it is the deliberate
    // `arm` command, applied through `SafetyAuthority::arm()` with every
    // configuration-build check passed; with the provisional configuration this
    // stage ships, that gate refuses and this stays false.
    bool armed() const { return authority_.isArmed(); }

    ev::RunPhase runPhase() const { return run_.phase(); }

    // True while either fault latch of record is set: the run policy's or the
    // safety authority's. Exactly the fact `status` reports as `faultLatched`,
    // exposed because a latched fault *clears* arming - so `!armed()` cannot tell
    // a free motor from a faulted one, and a caller that needs to know whether the
    // motor is free (the bench layer's arbitration, `MotorArbitration.h`) must ask
    // this instead of inferring it. The two latches are separate and only `reset`
    // clears both.
    bool faultLatched() const {
        return run_.faultCode() != ev::FaultCode::None ||
               authority_.latchedFault().code != ev::FaultCode::None;
    }

    // True while the run is in a phase that must stay armed
    // (`ev::isRunPhaseUnderway`): travelling, braking to a verified stop, or
    // reversing. False in `Idle`, `ParameterValid`, `Armed`, `Done` and `Fault`.
    bool runUnderway() const { return ev::isRunPhaseUnderway(run_.phase()); }

    // True when the control-tick order guard has recorded any violation since
    // boot: a tick whose stages arrived out of order, or a tick begun while a
    // previous one was unfinished. It latches for the life of the build on
    // purpose - a structural fault in the caller is not a vehicle condition, and a
    // firmware whose control cycle ran wrongly must not be allowed to keep
    // driving (see `ev::ControlTickOrderGuard`).
    bool orderViolationDetected() const {
        return orderGuard_.violatedTicks() != 0U || orderGuard_.abandonedTicks() != 0U;
    }

    // Attaches an event hook. A null hook detaches; nothing in this firmware
    // attaches one, so the hook stays null and the call in `runPass()` is never
    // reached in production.
    void setTelemetryHook(TelemetryHook hook, void* context) {
        telemetryHook_ = hook;
        telemetryHookContext_ = context;
    }

    // One pass of the operator interface, outside the control tick and after the
    // watchdog checkpoint: staged bytes out, received bytes in, at most one
    // command applied, its answer staged, then the telemetry queue drained.
    // Bounded work, no waiting, and no effect on the control path beyond the
    // deliberate transitions an operator asked for. A host that pipelines several
    // commands into one read window waits a pass per command.
    void serviceSerial(uint32_t nowMs);

    // The two halves of `serviceSerial()`, available separately because the bench
    // image (B1-B4, `src/bench/`) owns its own read loop: it pumps the operator
    // output and drains telemetry in the same order the production loop does,
    // around its own command dispatch. Production calls neither of these
    // directly - it calls `serviceSerial()`, which is exactly these two around the
    // read loop - so the bench cannot change the production order by using them.
    //
    // `serviceStagedText()` takes the clock because the greeting is owed to a
    // reader that has been silent long enough to be treated as new (finding F-4);
    // see `greeter_`.
    void serviceStagedText(uint32_t nowMs);
    void drainTelemetry();

    // One operator line, applied: parsed, gated by the operator policy, answered,
    // and - when it is one of the deliberate transitions - applied through the
    // chain's own entry points. `serviceSerial()` calls it for every line it
    // reads. It is public so the bench console can hand back a line it did not
    // recognize as a bench verb rather than reimplement any part of the operator
    // interface; it grants no capability a host typing the same line does not
    // already have, and it can no more drive the motor than it could before.
    void handleCommandLine(const char* line, uint16_t length, uint32_t nowMs);

    // The held configuration an operator has entered, as `config show` reports it.
    const ev::RunConfigFields& heldConfig() const { return heldConfig_; }

    // The telemetry session's own state. Reported by `telemetry status` and by the
    // status line; read by the tests and the docs, never by the control path.
    bool telemetryEnabled() const { return telemetrySession_.enabled(); }
    uint32_t telemetryStrideTicks() const { return telemetrySession_.strideTicks(); }

    // Operator-interface counters: lines understood, lines refused, and the three
    // ways an answer or a record can fail to reach the host. Evidence for a bench
    // session, not control inputs.
    uint32_t commandsAccepted() const { return commandsAccepted_; }
    uint32_t commandsRejected() const { return commandsRejected_; }
    uint32_t responsesLost() const { return responsesLost_; }
    uint32_t telemetryLinesSent() const { return serialTransport::port().linesSent(); }
    uint32_t telemetryFormatFailures() const { return telemetryFormatFailures_; }
    uint32_t telemetryStagingFailures() const { return telemetryStagingFailures_; }

private:
    // The control tick. Its body is the stage order documented above, entered
    // through the order guard so a reordering is reported instead of trusted.
    void runControlTick(uint32_t nowMs, uint32_t dtMs);

    // --------------------------------------------------------------- operator
    // interface (P7B). The pure decisions live in `lib/evcore` and are
    // host-tested; these methods are the wiring: they read the chain, call the
    // policy, and apply what the policy allowed through the chain's own entry
    // points.
    void rejectWithoutCommand(ev::CommandError error);
    void applyCommand(const ev::OperatorRequest& request, uint32_t nowMs);
    void applyConfigSet(const ev::RunConfigFields& supplied);
    void applyAnnounce(uint32_t nowMs);
    void applyArm();
    void applyReset(uint32_t nowMs);
    void applyTelemetryRate(double requestedRateHz);

    // The greeting, formatted and staged. Shared by the attach/silence re-arm in
    // `serviceStagedText()` and by the `boot` command, so the line an operator asks
    // for and the line a host is greeted with cannot drift apart.
    void stageBootLine();

    void respondLine(uint16_t length);
    void respondRefusal(ev::OperatorCommandId id, const char* reason);
    void pushOperatorEvent(ev::TelemetryEventCode code, const char* detail);

    ev::OperatorSystemState systemState() const;
    ev::OperatorStatusView statusView() const;
    ev::OperatorTelemetryView telemetryView() const;
    ev::HeldRunConfigVerdict heldVerdict() const;

    // The one sample this tick produced, whether or not a record goes out. Built
    // from the tick's own snapshot, so a record cannot disagree with the control
    // path about what happened.
    ev::TelemetrySample buildSample() const;

    // Records this tick in the telemetry session when one is due, and reports the
    // transitions it revealed. Called from stage 10 of the control tick.
    void captureTelemetry();

    // Configuration (all provisional; see VehicleConfig.h).
    ev::VehicleCalibration calibration_;
    ev::SafetyLimits safetyLimits_;
    ev::VelocityEstimatorConfig estimatorConfig_;
    ev::DirectionalMotionProfileLimits profileLimits_;

    // The chain.
    ev::VelocityEstimator estimator_;
    ev::VelocityController velocityController_;
    ev::MotionProfileTarget profileTarget_;
    ev::CompetitionStateMachine run_;
    ev::SafetyAuthority authority_;
    ev::ControlScheduler scheduler_;
    ev::ControlTickOrderGuard orderGuard_;

    // Per-run measurement state. The encoder reference and the run origin are
    // established at `begin()` and at a later run boundary; the harness's rule
    // that the distance a cycle consumes belongs to the previous cycle's leg is
    // implemented here in the tick.
    int32_t runOriginTicks_ = 0;
    bool legForward_ = true;
    ev::RunAxisGeometry axisGeometry_{0.0, 0.0, 0.0};
    // Held defaults before the first estimate exists. The shape of an absent
    // number is owned by `refusedVelocityEstimate()`; spelling it here by hand
    // would silently survive a new field or a reordered enumerator.
    ev::VelocityEstimate estimate_ =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    bool hasEstimate_ = false;
    ev::VelocityEstimate lastValidEstimate_ =
        ev::refusedVelocityEstimate(ev::VelocityEstimateStatus::WarmingUp);
    bool hasLastValidEstimate_ = false;
    ev::OdometryEstimate odometryTravel_{0.0, false, ev::OdometryEstimateStatus::ScaleIsNotMeasured};
    uint32_t odometrySampleMs_ = 0U;
    bool hasOdometrySample_ = false;

    // This cycle's raw inputs and the derived verdicts the boundary is given.
    sensorInputs::RawSample rawInputs_{};
    vehicleConfig::ConfigurationBuildChecks configurationChecks_{};
    bool positionUsable_ = false;
    // Control ticks observed with a *wired* driver's nFAULT line low, since the
    // current run started. Reset at a successful arm and at `reset`, deliberately:
    // H1B Part 3 (ten deliberate ground-and-release cycles) and H9's trip both read
    // this as evidence about *that* test, and a since-boot total cannot tell this
    // test's activity from an earlier one's.
    //
    // On this vehicle the line is not installed, so nothing increments it: the
    // tick's stage 3 increments this only when
    // `vehicleConfig::kPlannedDriverFaultCapability` is `Installed`. The counter
    // staying at zero here is *not* evidence of a healthy driver - there is no
    // signal - which is why the capability travels beside it in the snapshot and
    // why telemetry writes the level field as absent rather than as `0`.
    uint32_t cyclesWithDriverFaultLow_ = 0U;

    // The state of the vehicle layer itself.
    bool announcedConfigValid_ = false;
    bool regulatorEnabled_ = false;

    // The operator interface's own state. None of it is run-control state: the
    // held configuration is what an operator has typed (not what any run uses),
    // the session's flag is the log's, and the cursors are just which static text
    // is still waiting to be sent.
    char commandLine_[ev::kCommandLineCapacity + 1U];
    ev::CommandLineReader lineReader_;
    ev::RunConfigFields heldConfig_{};
    ev::TelemetrySession telemetrySession_{};
    ev::TelemetryEventDetector eventDetector_{};
    uint16_t helpLineIndex_ = ev::kOperatorHelpLineCount;
    // Whether the boot line is owed to a reader, and why (H0 finding F-4): a
    // newly asserted control line (DTR or RTS), a host silent long enough to be a
    // new reader, or an explicit `boot`. The decision is the pure
    // `ev::HostGreetPolicy`, host-tested at its edges and at its silence boundary;
    // this layer supplies the two inputs it cannot see - the sampled control lines
    // and the clock - and commits the line when the policy says it is owed.
    //
    // What that is, and is not: the greeting is sent once per boot to the first
    // host that attaches, again at each observed attach edge, and again at each
    // silence deadline (once per `ev::HostGreetPolicy::kSilenceMs` of host
    // silence). A host that presents no control-line edge *and* never writes
    // cannot be told apart from the previous reader, so the greeting is not
    // claimed to reach such a host promptly - the silence rule bounds that case
    // rather than removing it. On target, one of three attaches showed no attach
    // transition at all, which is the observation that rule answers; see
    // `data/evidence/P-H0-2026-09-28/d22-f4-attach-observations.txt`.
    ev::HostGreetPolicy greeter_{};
    // The last sampled host control-line state, the derived "can a write leave"
    // flag, and the countdown that samples both. The USB task is deliberately not
    // run on every pass: this loop measured ~21 microseconds a pass on the first
    // hardware session, and an attach is not a deadline - a host is greeted within
    // `kHostPollPasses` passes, far inside one control period.
    //
    // `hostAttached_` is `dtr` alone, because that is the predicate `write()` gates
    // on - used as "can a write leave", which is exactly what the transport needs,
    // and not as a reliable "a reader is present" oracle for anything else. `rts`
    // reaches the greeter as a second attach edge and is never a reason to believe
    // a write can move.
    serialTransport::HostLineState hostLineState_{};
    bool hostAttached_ = false;
    uint8_t hostPollCountdown_ = 0U;

    uint32_t commandsAccepted_ = 0U;
    uint32_t commandsRejected_ = 0U;
    uint32_t responsesLost_ = 0U;
    uint32_t telemetryFormatFailures_ = 0U;
    uint32_t telemetryStagingFailures_ = 0U;

    TelemetrySnapshot telemetry_{};
    TelemetryHook telemetryHook_ = nullptr;
    void* telemetryHookContext_ = nullptr;
};

}  // namespace evhw
