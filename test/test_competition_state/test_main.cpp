#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <CompetitionStateMachine.h>
#include <Motion.h>
#include <NearZeroVerifier.h>
#include <ReversalSequencer.h>
#include <RunConfig.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <Units.h>
#include <VehicleState.h>

// Host tests for the competition run state machine: the frozen phases, their
// transitions, the evidence each one waits for, the timeouts, the fault path and
// the direction handoff with the reversal sequencer and (independently) with
// `SafetyAuthority`.
//
// Every number below is a test fixture. None is a measured value, none is a
// tuning value, and none may be used by a firmware path: the vehicle's stop,
// braking, bottle interaction and geometry facts are all UNVERIFIED
// (VERIFIED_STATE.md #4, #6, #9, #11). The state machine is exercised as a
// sequence of stated evidence, not as a simulation of a vehicle - a passing test
// here says nothing about whether the vehicle would reach any phase in the real
// world.

static_assert(!std::is_default_constructible<ev::RunStatus>::value,
              "a run status must never default-construct");
static_assert(!std::is_default_constructible<ev::RunMotionIntention>::value,
              "a run intention must never default-construct");
static_assert(!std::is_default_constructible<ev::RunPhaseConfig>::value,
              "no silent defaults; every bound must be supplied with its source");
static_assert(static_cast<int>(ev::RunPhase::Idle) == 0,
              "the idle phase must stay the zero value");
static_assert(static_cast<int>(ev::RunMotionCommand::None) == 0,
              "no motion request must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

// Fixture update-gap limit for the stop verifiers. Wide enough that these tests
// stay about phases and bounds; the gap rule has its own tests in
// `test_near_zero_verifier`.
inline ev::Sourced<uint32_t> fixtureStopGap() {
    return ev::Sourced<uint32_t>(250U, ev::ValueSource::Measured);
}

// The stop-verification policy the fixtures use: threshold, dwell and the fixture
// gap limit, so a new configuration field cannot be forgotten at one call site.
ev::NearZeroConfig stopConfig(ev::Sourced<double> threshold, ev::Sourced<double> dwell) {
    return ev::NearZeroConfig(threshold, dwell, fixtureStopGap());
}

// The bottle-evidence policy the fixtures use: a measured lead, advance and push
// margin. The derivation itself has its own tests (`test_bottle_evidence`); the
// fixtures here only need a usable configuration, because the run policy refuses
// to be sequenced on a placeholder.
ev::BottleEvidenceConfig fixtureBottleConfig() {
    return ev::BottleEvidenceConfig(measured(0.10), measured(0.20), measured(0.05));
}

// Fixture policy: a 0.2 s stop verification, a 3 s reversal bound, phase bounds
// long enough that only the test that starves a phase sees them, and a
// planned-cruise floor the plan tests can build on.
// The three bottle-phase strategy ceilings are fixture values like every other
// number here: measured so the production validator accepts them, and no claim
// about any vehicle. `0.30` is deliberately unrelated to the fixture cruise, so
// a test that cares can change one ceiling and see only its own phase move.
ev::RunPhaseConfig phaseConfig(double maximumOptionalDelayS) {
    return ev::RunPhaseConfig(
        measured(maximumOptionalDelayS), measured(5.0), measured(1.0), measured(5.0), measured(5.0),
        measured(5.0), measured(5.0), measured(2.0), measured(1.0),
        stopConfig(measured(0.05), measured(0.2)), measured(0.10), measured(0.30), measured(0.30),
        measured(0.30), fixtureBottleConfig());
}

ev::ReversalConfig reversalConfig() {
    return ev::ReversalConfig(stopConfig(measured(0.05), measured(0.2)), measured(3.0));
}

// A rule-valid announced run: Regional level, 5.00 m, 10.0 s, 1.00 m bottle line,
// bottle at 2.0 m (rules 8.h, 8.i, 8.j, 7.c.ii.(5)).
ev::RunConfig validRunConfig() {
    return ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
}

// A run whose announced Target Distance is not on the Regional grid, so
// validateRunConfig() rejects it.
ev::RunConfig offGridRunConfig() {
    return ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.1, 10.0, 1.0, 2.0);
}

const char* phaseName(ev::RunPhase phase) {
    switch (phase) {
        case ev::RunPhase::Idle: return "Idle";
        case ev::RunPhase::ParameterValid: return "ParameterValid";
        case ev::RunPhase::Armed: return "Armed";
        case ev::RunPhase::OptionalDelay: return "OptionalDelay";
        case ev::RunPhase::Approach: return "Approach";
        case ev::RunPhase::Capture: return "Capture";
        case ev::RunPhase::Push: return "Push";
        case ev::RunPhase::ForwardDecel: return "ForwardDecel";
        case ev::RunPhase::BrakeZeroSpeed: return "BrakeZeroSpeed";
        case ev::RunPhase::Reverse: return "Reverse";
        case ev::RunPhase::FinalApproach: return "FinalApproach";
        case ev::RunPhase::FinalBrake: return "FinalBrake";
        case ev::RunPhase::Done: return "Done";
        case ev::RunPhase::Fault: return "Fault";
    }
    return "?";
}

struct Harness {
    ev::CompetitionStateMachine machine;
    uint32_t nowMs;

    // The delay the fixture run's own execution plan states. These fixtures
    // deliberately spend the whole policy maximum (a legal/safety bound, never
    // the run's value), so a run that has to reach OptionalDelay announces its
    // whole bound and a fixture with a zero bound skips the phase entirely - the
    // delay is the plan's per-run value, not a policy value.
    double fixturePlanDelayS;

    Harness(const ev::RunPhaseConfig& config, const ev::ReversalConfig& reversal, uint32_t startMs,
            double fixturePlanDelayS_ = 0.0)
        : machine(config, reversal), nowMs(startMs), fixturePlanDelayS(fixturePlanDelayS_) {}
};

Harness defaultHarness(double maximumOptionalDelayS) {
    return Harness(phaseConfig(maximumOptionalDelayS), reversalConfig(), 1000U,
                   maximumOptionalDelayS);
}

// Announces the valid fixture run with an explicit per-run delay: the value a
// run's execution plan would have supplied (`RunExecutionPlan::initialDelayS`).
// The run policy bounds it by the announced policy (`maximumOptionalDelayS`).
ev::RunStartStatus announceValidRunWithDelay(Harness& harness, double planDelayS) {
    const ev::RunConfig config = validRunConfig();
    return harness.machine.announceRun(config, ev::validateRunConfig(config), planDelayS,
                                       harness.nowMs);
}

ev::RunStartStatus announceValidRun(Harness& harness) {
    return announceValidRunWithDelay(harness, harness.fixturePlanDelayS);
}

// One cycle's stated evidence. Every field defaults to a stated, safe fact; the
// tests that starve a phase turn the relevant one off.
struct Evidence {
    bool armed = true;
    bool start = false;
    bool approach = false;
    bool captured = false;
    bool pushed = false;
    bool remainingUsable = false;
    double remainingM = 0.0;
    // Whether the leg being travelled is inside its measured brake offset (S18B),
    // the fact that arms the run's brake-entry travel-progress bound. Stated here
    // like every other input; in production the vehicle layer derives it from the
    // plan's leg target and the direction's brake offset.
    bool brakeEntry = false;
    bool stopUsable = true;
    double stopSpeedMps = 0.0;
    bool stopClaimed = true;
    ev::FaultCode fault = ev::FaultCode::None;
};

ev::RunInputs inputsFrom(const Evidence& evidence) {
    ev::RunInputs inputs;
    inputs.authorityArmed = evidence.armed;
    inputs.startTriggered = evidence.start;
    inputs.approachComplete = evidence.approach;
    inputs.captureComplete = evidence.captured;
    inputs.pushComplete = evidence.pushed;
    inputs.remainingDistanceUsable = evidence.remainingUsable;
    inputs.remainingDistanceM = evidence.remainingM;
    inputs.legInBrakeEntry = evidence.brakeEntry;
    inputs.stopEvidence.velocityUsable = evidence.stopUsable;
    inputs.stopEvidence.velocityMps = evidence.stopSpeedMps;
    inputs.stopEvidence.zeroSpeedClaimed = evidence.stopClaimed;
    inputs.detectedFault = evidence.fault;
    return inputs;
}

// The evidence that moves the run one phase further from where it is. Used by
// the tests that reach a phase before starving it; the nominal-transition test
// states its evidence explicitly instead.
Evidence evidenceFor(ev::RunPhase phase) {
    Evidence evidence;
    switch (phase) {
        case ev::RunPhase::Idle:
        case ev::RunPhase::ParameterValid:
        case ev::RunPhase::OptionalDelay:
        case ev::RunPhase::Fault:
        case ev::RunPhase::Done:
            break;
        case ev::RunPhase::Armed:
            evidence.start = true;
            break;
        case ev::RunPhase::Approach:
            evidence.approach = true;
            break;
        case ev::RunPhase::Capture:
            evidence.captured = true;
            break;
        case ev::RunPhase::Push:
            evidence.pushed = true;
            break;
        case ev::RunPhase::ForwardDecel:
            // Near zero observed, but the count is still changing: this is what
            // moves the run from the ramp-down into the deliberate stop.
            evidence.stopSpeedMps = 0.0;
            evidence.stopClaimed = false;
            break;
        case ev::RunPhase::BrakeZeroSpeed:
            // The claim arrives and the dwell accumulates until the permission.
            evidence.stopClaimed = true;
            break;
        case ev::RunPhase::Reverse:
            evidence.remainingUsable = true;
            evidence.remainingM = 0.5;
            break;
        case ev::RunPhase::FinalApproach:
            evidence.remainingUsable = true;
            evidence.remainingM = 0.0;
            break;
        case ev::RunPhase::FinalBrake:
            evidence.stopClaimed = true;
            break;
    }
    return evidence;
}

ev::RunStatus tick(Harness& harness, const Evidence& evidence, uint32_t stepMs = 50U) {
    harness.nowMs += stepMs;
    return harness.machine.update(harness.nowMs, inputsFrom(evidence));
}

// Advances the run, one stated cycle at a time, until it reaches `target`.
// Returns false when the run faults or the step budget runs out, so a test that
// expects a phase is never satisfied by an unrelated state.
bool driveTo(Harness& harness, ev::RunPhase target) {
    if (harness.machine.phase() == target) {
        return true;
    }
    for (uint32_t step = 0U; step < 400U; ++step) {
        const ev::RunStatus status =
            harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(harness.machine.phase())));
        if (status.phase == target) {
            return true;
        }
        if (status.phase == ev::RunPhase::Fault) {
            return false;
        }
        harness.nowMs += 50U;
    }
    return false;
}

void expectPhase(ev::RunPhase expected, const ev::RunStatus& status) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(status.phase),
                                  phaseName(expected));
}

void expectCommand(ev::RunMotionCommand expected, const ev::RunStatus& status) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(status.intention.command));
}

void expectFault(ev::FaultCode expected, const ev::RunStatus& status) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault), static_cast<int>(status.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(status.fault));
}

// Starves the current phase of its exit evidence until the configured bound
// expires, and returns the resulting status.
ev::RunStatus starveUntilFault(Harness& harness, const Evidence& evidence,
                               uint32_t maxSteps = 400U) {
    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidence));
    for (uint32_t step = 0U; step < maxSteps && status.phase != ev::RunPhase::Fault; ++step) {
        status = tick(harness, evidence);
    }
    return status;
}

ev::SafetyLimits fixtureLimits() {
    return ev::SafetyLimits{
        ev::Sourced<uint16_t>(600U, ev::ValueSource::Measured),
        measured(2.5),
        measured(2.5),
        measured(30.0),
        measured(9.0),
    };
}

ev::SafetyReadiness ready() {
    ev::SafetyReadiness readiness;
    readiness.configurationValid = true;
    readiness.calibrationUsable = true;
    readiness.limitsUsable = true;
    return readiness;
}

ev::SafetySystemState readySafetyState() {
    ev::SafetySystemState state;
    state.readiness = ready();
    state.requiredDataFresh = true;
    state.atZeroSpeed = false;
    state.nowMs = 1000U;
    state.detectedFault = ev::FaultCode::None;
    return state;
}

// The one request the vehicle layer presents for one run command. Travel is a
// powered request in the direction the command names; every stop command is the
// production non-powered mapping (`runStopEffort()`), so this reference request
// cannot drift from what the firmware presents.
ev::MotorEffort vehicleEffortFor(ev::RunMotionCommand command) {
    switch (command) {
        case ev::RunMotionCommand::DriveForward:
            return ev::MotorEffort(ev::MotorMode::DriveForward, 300U);
        case ev::RunMotionCommand::DriveReverse:
            return ev::MotorEffort(ev::MotorMode::DriveReverse, 300U);
        case ev::RunMotionCommand::BrakeForward:
        case ev::RunMotionCommand::BrakeReverse:
        case ev::RunMotionCommand::RampDownForward:
        case ev::RunMotionCommand::RampDownReverse:
            return ev::runStopEffort(command);
        case ev::RunMotionCommand::None:
            return ev::MotorEffort(ev::MotorMode::Disabled, 0U);
    }
    return ev::MotorEffort(ev::MotorMode::Disabled, 0U);
}

}  // namespace

// --- Idle -------------------------------------------------------------------

void test_a_fresh_state_machine_is_idle_and_asks_for_nothing(void) {
    Harness harness = defaultHarness(0.0);
    const ev::RunStatus status =
        harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::Idle)));

    expectPhase(ev::RunPhase::Idle, status);
    expectCommand(ev::RunMotionCommand::None, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None), static_cast<int>(status.fault));
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));

    // A trigger in Idle starts nothing: there is no announced run to run.
    Evidence trigger;
    trigger.start = true;
    trigger.approach = true;
    trigger.pushed = true;
    trigger.remainingUsable = true;
    trigger.remainingM = 0.0;
    const ev::RunStatus after = tick(harness, trigger);
    expectPhase(ev::RunPhase::Idle, after);
    expectCommand(ev::RunMotionCommand::None, after);
    TEST_ASSERT_FALSE(ev::runIntentionIsCurrentFor(after.intention, harness.nowMs + 50U));
}

void test_a_fault_is_reachable_from_idle(void) {
    Harness harness = defaultHarness(0.0);
    Evidence evidence;
    evidence.fault = ev::FaultCode::ProvisionalCalibration;
    const ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidence));
    expectFault(ev::FaultCode::ProvisionalCalibration, status);
    expectCommand(ev::RunMotionCommand::None, status);
}

// --- Announcement -----------------------------------------------------------

void test_an_invalid_announcement_faults_and_requires_an_explicit_reset(void) {
    Harness harness = defaultHarness(0.0);
    const ev::RunConfig config = offGridRunConfig();
    const ev::RunConfigValidation validation = ev::validateRunConfig(config);
    TEST_ASSERT_FALSE(validation.ok);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::ConfigInvalid),
                          static_cast<int>(harness.machine.announceRun(config, validation, 0.0,
                                                                      harness.nowMs)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::InvalidRunConfig),
                          static_cast<int>(harness.machine.faultCode()));

    // A second announcement cannot start over the fault.
    const ev::RunConfig valid = validRunConfig();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunStartStatus::FaultLatched),
        static_cast<int>(harness.machine.announceRun(valid, ev::validateRunConfig(valid), 0.0,
                                                    harness.nowMs)));

    // Nothing acts during the fault, however much evidence arrives.
    Evidence everything;
    everything.start = true;
    everything.approach = true;
    everything.captured = true;
    everything.pushed = true;
    const ev::RunStatus status = tick(harness, everything);
    expectFault(ev::FaultCode::InvalidRunConfig, status);
    expectCommand(ev::RunMotionCommand::None, status);

    // Only an explicit reset clears it, and the announcement then has to be made
    // again.
    harness.machine.reset(harness.nowMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.machine.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.machine.faultCode()));
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed) == false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRun(harness)));
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed));
}

void test_a_policy_configuration_that_is_not_usable_faults(void) {
    // This machine's own bounds are placeholders, so it may not sequence a run.
    const ev::RunPhaseConfig placeholderDelay = ev::RunPhaseConfig(
        provisional(0.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)),
        measured(0.10),
        measured(0.30), measured(0.30), measured(0.30), fixtureBottleConfig());
    Harness harness(placeholderDelay, reversalConfig(), 1000U);
    TEST_ASSERT_FALSE(harness.machine.configUsable());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::PolicyConfigNotUsable),
                          static_cast<int>(announceValidRun(harness)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(harness.machine.faultCode()));

    // The reversal policy is part of the same machine, and a placeholder there is
    // just as unacceptable.
    const ev::ReversalConfig placeholderReversal(
        stopConfig(provisional(0.05), measured(0.2)), measured(3.0));
    Harness reversalHarness(phaseConfig(0.0), placeholderReversal, 1000U);
    TEST_ASSERT_FALSE(reversalHarness.machine.configUsable());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::PolicyConfigNotUsable),
                          static_cast<int>(announceValidRun(reversalHarness)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(reversalHarness.machine.faultCode()));
}

void test_a_run_cannot_be_announced_twice(void) {
    Harness harness = defaultHarness(0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRun(harness)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::ParameterValid),
                          static_cast<int>(harness.machine.phase()));

    // Even while the run only waits, a second announcement is refused: the run
    // is already announced and a new one needs a reset.
    Evidence waiting;
    waiting.armed = false;
    const ev::RunStatus status = tick(harness, waiting);
    expectPhase(ev::RunPhase::ParameterValid, status);
    expectCommand(ev::RunMotionCommand::None, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::RunAlreadyActive),
                          static_cast<int>(announceValidRun(harness)));
}

// --- Arming and the trigger -------------------------------------------------

void test_a_trigger_before_arming_starts_nothing(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);

    Evidence trigger;
    trigger.armed = false;
    trigger.start = true;
    ev::RunStatus status = tick(harness, trigger);
    expectPhase(ev::RunPhase::ParameterValid, status);
    expectCommand(ev::RunMotionCommand::None, status);

    status = tick(harness, evidenceFor(ev::RunPhase::ParameterValid));
    expectPhase(ev::RunPhase::Armed, status);
    expectCommand(ev::RunMotionCommand::None, status);

    // With no intentional delay, the trigger starts the travel.
    status = tick(harness, evidenceFor(ev::RunPhase::Armed));
    expectPhase(ev::RunPhase::Approach, status);
    expectCommand(ev::RunMotionCommand::DriveForward, status);
    TEST_ASSERT_TRUE(ev::isRunTravelCommand(status.intention.command));
}

void test_a_disarm_before_the_trigger_returns_to_parameter_valid(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed));

    // A disarmed run cannot be started by the trigger either: the disarm wins,
    // because what is being asked for is motion and there is no armed state to
    // ground it in.
    Evidence disarmed;
    disarmed.armed = false;
    disarmed.start = true;
    ev::RunStatus status = tick(harness, disarmed);
    expectPhase(ev::RunPhase::ParameterValid, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None), static_cast<int>(status.fault));
    expectCommand(ev::RunMotionCommand::None, status);

    status = tick(harness, evidenceFor(ev::RunPhase::ParameterValid));
    expectPhase(ev::RunPhase::Armed, status);
}

void test_the_optional_delay_is_spent_before_any_travel(void) {
    Harness harness = defaultHarness(1.0);
    announceValidRun(harness);

    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::OptionalDelay));
    const uint32_t enteredMs = harness.nowMs;

    Evidence waiting;
    for (uint32_t step = 0U; step < 18U; ++step) {
        const ev::RunStatus status = tick(harness, waiting);
        expectPhase(ev::RunPhase::OptionalDelay, status);
        expectCommand(ev::RunMotionCommand::None, status);
        TEST_ASSERT_EQUAL_UINT32((step + 1U) * 50U, status.phaseElapsedMs);
    }
    TEST_ASSERT_EQUAL_UINT32(900U, harness.nowMs - enteredMs);

    const ev::RunStatus started = tick(harness, waiting);
    expectPhase(ev::RunPhase::OptionalDelay, started);
    TEST_ASSERT_EQUAL_UINT32(950U, started.phaseElapsedMs);

    // The delay is the run's own bound, not a phase timeout: it moves to the
    // travel phase rather than faulting.
    const ev::RunStatus next = tick(harness, waiting);
    expectPhase(ev::RunPhase::Approach, next);
    expectCommand(ev::RunMotionCommand::DriveForward, next);
}

// --- Bottle phases ----------------------------------------------------------

void test_approach_waits_for_completion_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    Evidence starved;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
    expectCommand(ev::RunMotionCommand::None, status);
}

void test_capture_waits_for_engagement_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Capture));

    Evidence starved;
    starved.approach = true;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

void test_push_waits_for_completion_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    Evidence starved;
    starved.approach = true;
    starved.captured = true;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

void test_the_phase_bound_is_inclusive_and_late_evidence_is_refused(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));
    const uint32_t enteredMs = harness.nowMs;

    // One millisecond inside the bound the phase is still the phase.
    harness.nowMs = enteredMs + 4999U;
    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(Evidence()));
    expectPhase(ev::RunPhase::Approach, status);

    // Exactly on the bound the deadline has arrived, so even the exit evidence
    // that would have ended the phase faults the run instead of being accepted
    // late.
    Evidence arrivedLate;
    arrivedLate.approach = true;
    harness.nowMs = enteredMs + 5000U;
    status = harness.machine.update(harness.nowMs, inputsFrom(arrivedLate));
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

void test_a_stalled_loop_faults_at_the_next_update(void) {
    // A control loop that stalls is not a pause the run can sleep through. The
    // phase bound is a deadline, so on the next update the run faults - even when
    // the exit evidence that would have ended the phase is finally there, because
    // a phase that overran its bound is not the run that was announced.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    Evidence approachArrivedLate;
    approachArrivedLate.approach = true;
    harness.nowMs += 6000U;  // past the 5 s approach bound
    const ev::RunStatus status =
        harness.machine.update(harness.nowMs, inputsFrom(approachArrivedLate));
    expectFault(ev::FaultCode::PhaseTimeout, status);

    // The reversal's own timeout covers the same case between ForwardDecel and
    // BrakeZeroSpeed, where the run policy keeps no separate bound.
    Harness reversal = defaultHarness(0.0);
    announceValidRun(reversal);
    TEST_ASSERT_TRUE(driveTo(reversal, ev::RunPhase::ForwardDecel));

    reversal.nowMs += 4000U;  // past the 3 s reversal bound
    const ev::RunStatus reversalStatus =
        reversal.machine.update(reversal.nowMs, inputsFrom(evidenceFor(ev::RunPhase::ForwardDecel)));
    expectFault(ev::FaultCode::ReversalNotVerified, reversalStatus);
    TEST_ASSERT_FALSE(reversalStatus.stopVerified);
}

void test_a_non_finite_remaining_distance_does_not_end_the_final_leg(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Reverse));

    // "Usable" is the vehicle layer's verdict, and an infinity is not a distance:
    // a negative infinity would otherwise satisfy "at or inside the final
    // approach" and end the travel early on arithmetic that produced nothing.
    Evidence bogus;
    bogus.remainingUsable = true;
    bogus.remainingM = -std::numeric_limits<double>::infinity();
    ev::RunStatus status = tick(harness, bogus);
    expectPhase(ev::RunPhase::Reverse, status);

    bogus.remainingM = std::numeric_limits<double>::quiet_NaN();
    status = tick(harness, bogus);
    expectPhase(ev::RunPhase::Reverse, status);

    const ev::RunStatus faulted = starveUntilFault(harness, bogus);
    expectFault(ev::FaultCode::PhaseTimeout, faulted);
}

void test_a_bound_below_a_millisecond_expires_rather_than_never(void) {
    // A positive bound that converts to zero whole milliseconds must still bound
    // the phase. Treating it as "no bound" would silently turn a misconfigured
    // timeout into an unbounded wait.
    const ev::RunPhaseConfig shortBounds =
        ev::RunPhaseConfig(measured(0.0), measured(0.0005), measured(1.0), measured(5.0),
                           measured(5.0), measured(5.0), measured(5.0), measured(2.0),
                           measured(1.0), stopConfig(measured(0.05), measured(0.2)), measured(0.10),
                           measured(0.30),
                           measured(0.30), measured(0.30), fixtureBottleConfig());
    Harness harness(shortBounds, reversalConfig(), 1000U);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    const ev::RunStatus status = tick(harness, Evidence());
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

// --- The frozen transition sequence -----------------------------------------

void test_every_legal_transition_in_order(void) {
    Harness harness = defaultHarness(0.0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRun(harness)));

    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::ParameterValid));
    expectPhase(ev::RunPhase::Armed, status);
    expectCommand(ev::RunMotionCommand::None, status);

    status = tick(harness, evidenceFor(ev::RunPhase::Armed));
    expectPhase(ev::RunPhase::Approach, status);
    expectCommand(ev::RunMotionCommand::DriveForward, status);

    status = tick(harness, evidenceFor(ev::RunPhase::Approach));
    expectPhase(ev::RunPhase::Capture, status);
    expectCommand(ev::RunMotionCommand::DriveForward, status);

    status = tick(harness, evidenceFor(ev::RunPhase::Capture));
    expectPhase(ev::RunPhase::Push, status);
    expectCommand(ev::RunMotionCommand::DriveForward, status);

    status = tick(harness, evidenceFor(ev::RunPhase::Push));
    expectPhase(ev::RunPhase::ForwardDecel, status);
    expectCommand(ev::RunMotionCommand::RampDownForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::RampingDown),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // Still moving: the reversal ramps down and promises nothing.
    Evidence moving;
    moving.stopSpeedMps = 1.0;
    moving.stopClaimed = false;
    status = tick(harness, moving);
    expectPhase(ev::RunPhase::ForwardDecel, status);
    expectCommand(ev::RunMotionCommand::RampDownForward, status);

    // Near zero observed, count still changing: brake and wait.
    Evidence nearZeroNoClaim;
    nearZeroNoClaim.stopClaimed = false;
    status = tick(harness, nearZeroNoClaim);
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Braking),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // The claim arrives and the dwell starts.
    status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::VerifyingNearZero),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // The dwell keeps accumulating until the permission is earned; the run keeps
    // presenting the stop while that happens.
    for (uint32_t step = 0U;
         step < 10U && status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
        expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
        expectCommand(ev::RunMotionCommand::BrakeForward, status);
    }
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::DirectionChangeReady),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_TRUE(status.stopVerified);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);

    // The reverse travel is asked for on the cycle after the stop was presented.
    status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    expectPhase(ev::RunPhase::Reverse, status);
    expectCommand(ev::RunMotionCommand::DriveReverse, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    status = tick(harness, evidenceFor(ev::RunPhase::Reverse));
    expectPhase(ev::RunPhase::FinalApproach, status);
    expectCommand(ev::RunMotionCommand::DriveReverse, status);

    status = tick(harness, evidenceFor(ev::RunPhase::FinalApproach));
    expectPhase(ev::RunPhase::FinalBrake, status);
    expectCommand(ev::RunMotionCommand::BrakeReverse, status);
    TEST_ASSERT_FALSE(status.stopVerified);

    for (uint32_t step = 0U; step < 4U; ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
        expectPhase(ev::RunPhase::FinalBrake, status);
    }
    status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    expectPhase(ev::RunPhase::Done, status);
    expectCommand(ev::RunMotionCommand::BrakeReverse, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));
}

void test_the_reversal_presentation_and_the_sequencer_evidence_stay_distinct(void) {
    // The frozen split: ForwardDecel/BrakeZeroSpeed are what the run presents;
    // RampingDown/Braking/VerifyingNearZero/DirectionChangeReady are what the
    // owned sequencer has established. The mapping between them is asserted here
    // so a future refactor cannot silently collapse one into the other, and the
    // run's stopVerified is shown to be the sequencer's latch repeated for the
    // stop being presented, not a run-owned copy that could drift.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    // Push complete: the run enters ForwardDecel and presents the ramp-down
    // while the sequencer has only just started ramping down.
    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::Push));
    expectPhase(ev::RunPhase::ForwardDecel, status);
    expectCommand(ev::RunMotionCommand::RampDownForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::RampingDown),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // Still moving: presentation and evidence stay on the ramp-down.
    Evidence moving;
    moving.stopSpeedMps = 1.0;
    moving.stopClaimed = false;
    status = tick(harness, moving);
    expectPhase(ev::RunPhase::ForwardDecel, status);
    expectCommand(ev::RunMotionCommand::RampDownForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::RampingDown),
                          static_cast<int>(status.reversalPhase));

    // Near zero with the count still changing: the presentation flips to the
    // deliberate stop and the evidence says Braking.
    Evidence braked;
    braked.stopClaimed = false;
    status = tick(harness, braked);
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Braking),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // The claim arrives and the dwell accumulates under the same presentation.
    status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::VerifyingNearZero),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);

    // The permission is earned: the sequencer's latch, repeated by the run while
    // it presents that same stop.
    for (uint32_t step = 0U;
         step < 10U && status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::DirectionChangeReady),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_TRUE(status.stopVerified);

    // The handoff consumes the evidence: the run asks for reverse and reports no
    // verified stop, because the verdict was about the stop it presented and not
    // about the vehicle that is now asked to travel.
    status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    expectPhase(ev::RunPhase::Reverse, status);
    expectCommand(ev::RunMotionCommand::DriveReverse, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);
}

void test_the_handoff_presents_a_stop_cycle_before_asking_for_reverse(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    // Reach the earned permission and hold it for one cycle: the run still asks
    // for the deliberate stop, and reports the verified stop the authority needs
    // to clear its direction memory.
    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::BrakeZeroSpeed)));
    for (uint32_t step = 0U; step < 8U &&
                             status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);
    TEST_ASSERT_TRUE(ev::isRunStopCommand(status.intention.command));
    TEST_ASSERT_TRUE(status.stopVerified);

    const ev::RunStatus reverse = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    expectPhase(ev::RunPhase::Reverse, reverse);
    expectCommand(ev::RunMotionCommand::DriveReverse, reverse);
    TEST_ASSERT_TRUE(ev::isRunTravelCommand(reverse.intention.command));
}

void test_fresh_motion_before_the_handoff_aborts_the_reversal(void) {
    // S18A/P1 at the consumption boundary: the stop was verified and the
    // one-cycle handoff is pending, but the next tick's live evidence shows
    // fresh motion after a long gap. The handoff must not consume the stale
    // permission: the run holds the deliberate stop, and the full dwell must
    // re-run before any reverse travel is asked for.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    // Reach the earned permission; the handoff is now pending.
    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::BrakeZeroSpeed)));
    for (uint32_t step = 0U; step < 8U &&
                             status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::DirectionChangeReady),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_TRUE(status.stopVerified);

    // Fresh motion (+1 m/s, no claim) after a 2000 ms gap: still inside the
    // fixture's 3 s sequence budget, so only the evidence rule can refuse.
    Evidence moving;
    moving.stopUsable = true;
    moving.stopSpeedMps = 1.0;
    moving.stopClaimed = false;
    status = tick(harness, moving, 2000U);
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::RampingDown),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);
    expectCommand(ev::RunMotionCommand::BrakeForward, status);

    // The stop re-earned on live evidence still hands off to reverse.
    for (uint32_t step = 0U; step < 8U && status.phase != ev::RunPhase::Reverse; ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    expectPhase(ev::RunPhase::Reverse, status);
    expectCommand(ev::RunMotionCommand::DriveReverse, status);
}

void test_a_fault_with_a_pending_handoff_faults_instead_of_reversing(void) {
    // Fault precedence at the handoff: a fault raised while the one-cycle
    // stop is being presented discards the pending handoff with everything
    // else. The run faults; it never flips to reverse travel.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::BrakeZeroSpeed)));
    for (uint32_t step = 0U; step < 8U &&
                             status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::DirectionChangeReady),
                          static_cast<int>(status.reversalPhase));

    Evidence faulted = evidenceFor(ev::RunPhase::BrakeZeroSpeed);
    faulted.fault = ev::FaultCode::InvalidRunConfig;
    status = tick(harness, faulted);
    expectPhase(ev::RunPhase::Fault, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);
}

void test_reset_with_a_pending_handoff_returns_to_nothing_verified(void) {
    // S18A/P2 lifecycle at the handoff: a reset while the one-cycle stop is
    // being presented clears the pending handoff with the run. No verified
    // stop survives into the fresh run, and a new announcement starts clean.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::BrakeZeroSpeed)));
    for (uint32_t step = 0U; step < 8U &&
                             status.reversalPhase != ev::ReversalPhase::DirectionChangeReady;
         ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::BrakeZeroSpeed));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::DirectionChangeReady),
                          static_cast<int>(status.reversalPhase));

    harness.machine.reset(harness.nowMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.machine.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(harness.machine.reversalPhase()));
    TEST_ASSERT_FALSE(harness.machine.stopVerified());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRun(harness)));
}

void test_no_evidence_can_skip_the_reversal_phases(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    // Everything the run could ever be told, all at once: the run still advances
    // one phase per cycle and never jumps from the approach to the reverse.
    Evidence everything;
    everything.approach = true;
    everything.captured = true;
    everything.pushed = true;
    everything.remainingUsable = true;
    everything.remainingM = 0.0;
    everything.stopClaimed = true;

    ev::RunStatus status = tick(harness, everything);
    expectPhase(ev::RunPhase::Capture, status);
    status = tick(harness, everything);
    expectPhase(ev::RunPhase::Push, status);
    status = tick(harness, everything);
    expectPhase(ev::RunPhase::ForwardDecel, status);
    status = tick(harness, everything);
    expectPhase(ev::RunPhase::BrakeZeroSpeed, status);

    // From here the run holds the stop until the reversal is earned and the
    // handoff cycle has passed; no input can talk it into a reverse travel
    // before that.
    for (uint32_t step = 0U; step < 20U; ++step) {
        status = tick(harness, everything);
        if (status.phase == ev::RunPhase::Reverse) {
            break;
        }
        TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(status.intention.command),
                                  "a travel command before the verified stop");
    }
    expectPhase(ev::RunPhase::Reverse, status);
    expectCommand(ev::RunMotionCommand::DriveReverse, status);
}

// --- The reversal refusal paths ---------------------------------------------

void test_the_reversal_timeout_faults_instead_of_reversing(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::ForwardDecel));

    Evidence moving;
    moving.stopSpeedMps = 1.5;
    moving.stopClaimed = false;

    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(moving));
    for (uint32_t step = 0U; step < 80U && status.phase != ev::RunPhase::Fault; ++step) {
        TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));
        status = tick(harness, moving);
    }

    expectFault(ev::FaultCode::ReversalNotVerified, status);
    expectCommand(ev::RunMotionCommand::None, status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));
    TEST_ASSERT_FALSE(status.stopVerified);
}

void test_stale_stop_evidence_never_permits_the_reversal(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::ForwardDecel));

    Evidence stale;
    stale.stopUsable = false;
    stale.stopClaimed = true;

    const ev::RunStatus status = starveUntilFault(harness, stale);
    expectFault(ev::FaultCode::ReversalNotVerified, status);
}

void test_oscillation_around_the_threshold_faults_instead_of_reversing(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::ForwardDecel));

    ev::RunStatus status = harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::ForwardDecel)));
    for (uint32_t step = 0U; step < 80U && status.phase != ev::RunPhase::Fault; ++step) {
        Evidence oscillating;
        oscillating.stopSpeedMps = (step % 2U) == 0U ? 0.06 : 0.0;
        oscillating.stopClaimed = true;
        TEST_ASSERT_FALSE(status.stopVerified);
        status = tick(harness, oscillating);
    }
    expectFault(ev::FaultCode::ReversalNotVerified, status);
    TEST_ASSERT_FALSE(status.stopVerified);
}

void test_a_fault_during_the_reversal_stops_the_run_and_keeps_the_first_fault(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    Evidence faulted = evidenceFor(ev::RunPhase::BrakeZeroSpeed);
    faulted.fault = ev::FaultCode::MissingSafetyLimits;
    const ev::RunStatus status = tick(harness, faulted);
    expectFault(ev::FaultCode::MissingSafetyLimits, status);
    expectCommand(ev::RunMotionCommand::None, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(status.reversalPhase));

    // A later, different fault does not replace the fault of record.
    Evidence other = evidenceFor(ev::RunPhase::BrakeZeroSpeed);
    other.fault = ev::FaultCode::InvalidRunConfig;
    const ev::RunStatus later = tick(harness, other);
    expectFault(ev::FaultCode::MissingSafetyLimits, later);
}

void test_a_disarm_during_the_run_faults(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    Evidence disarmed;
    disarmed.armed = false;
    const ev::RunStatus status = tick(harness, disarmed);
    expectFault(ev::FaultCode::DisarmedDuringRun, status);
    expectCommand(ev::RunMotionCommand::None, status);
}

void test_a_disarm_during_the_reversal_faults(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::BrakeZeroSpeed));

    Evidence disarmed = evidenceFor(ev::RunPhase::BrakeZeroSpeed);
    disarmed.armed = false;
    const ev::RunStatus status = tick(harness, disarmed);
    expectFault(ev::FaultCode::DisarmedDuringRun, status);
}

// --- The reverse leg --------------------------------------------------------

void test_reverse_waits_for_the_closing_distance_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Reverse));

    // The run knows where it is only when the vehicle layer says the run-axis
    // distance is usable; an unusable number is not a distance of zero.
    Evidence starved;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

void test_final_approach_waits_for_the_target_point_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalApproach));

    Evidence starved;
    starved.remainingUsable = true;
    starved.remainingM = 0.4;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

void test_final_brake_waits_for_the_verified_stop_and_times_out(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalBrake));

    Evidence starved;
    starved.remainingUsable = true;
    starved.remainingM = 0.0;
    starved.stopUsable = false;
    starved.stopClaimed = true;
    const ev::RunStatus status = starveUntilFault(harness, starved);
    expectFault(ev::FaultCode::PhaseTimeout, status);
    TEST_ASSERT_FALSE(status.stopVerified);
}

// --- The brake-entry travel-progress bound (S18B) ---------------------------

// A travel phase that has reached brake entry - the shaper has reached zero and
// the run expects the vehicle to coast the rest of the way to the landmark - and
// then makes no progress ends with the run's own identity, not the phase bound.
// Both legs, and the phases on each.
void test_a_brake_entry_travel_phase_without_progress_faults_travel_stalled(void) {
    struct Case {
        ev::RunPhase phase;
        double remainingM;
    };
    // The remaining distance is held constant so the phase neither completes nor
    // leaves; the value only has to keep the phase (Reverse stays above the final
    // approach distance, FinalApproach stays above the Target Point).
    const Case cases[] = {
        {ev::RunPhase::Approach, -5.0},
        {ev::RunPhase::Push, -1.0},
        {ev::RunPhase::Reverse, 2.0},
        {ev::RunPhase::FinalApproach, 0.5},
    };

    for (const Case& testCase : cases) {
        Harness harness = defaultHarness(0.0);
        announceValidRun(harness);
        TEST_ASSERT_TRUE_MESSAGE(driveTo(harness, testCase.phase), phaseName(testCase.phase));

        Evidence stuck;
        stuck.remainingUsable = true;
        stuck.remainingM = testCase.remainingM;
        stuck.brakeEntry = true;
        const ev::RunStatus status = starveUntilFault(harness, stuck, 400U);
        expectFault(ev::FaultCode::TravelStalled, status);
        expectCommand(ev::RunMotionCommand::None, status);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::FaultCode::PhaseTimeout),
                                      static_cast<int>(status.fault), phaseName(testCase.phase));
    }
}

// Outside brake entry the bound is inactive: the run is still commanding travel,
// so the phase's own bound is the bounded outcome.
void test_the_travel_progress_bound_is_inactive_outside_brake_entry(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Approach));

    Evidence stuck;
    stuck.remainingUsable = true;
    stuck.remainingM = 2.0;
    stuck.brakeEntry = false;
    const ev::RunStatus status = starveUntilFault(harness, stuck, 400U);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

// Unusable position evidence pauses the bound rather than faulting on it: with no
// progress fact there is nothing to fault on, and the phase bound is the outcome.
void test_the_travel_progress_bound_pauses_on_unusable_evidence(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    Evidence stuck;
    stuck.remainingUsable = false;
    stuck.brakeEntry = true;
    const ev::RunStatus status = starveUntilFault(harness, stuck, 400U);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

// Progress resets the bound: travel that keeps advancing is never faulted, however
// long it takes, and only the stall after the last advance ends the run.
void test_progress_resets_the_travel_progress_bound(void) {
    // A push bound long enough that the phase deadline cannot be what ends this
    // run, so only the stall window is under test.
    const ev::RunPhaseConfig longPushBound(
        measured(0.0), measured(5.0), measured(1.0), measured(10.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0),
        stopConfig(measured(0.05), measured(0.2)), measured(0.10), measured(0.30), measured(0.30),
        measured(0.30), fixtureBottleConfig());
    Harness harness(longPushBound, reversalConfig(), 1000U);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    Evidence moving;
    moving.remainingUsable = true;
    moving.brakeEntry = true;
    double remainingM = -0.5;
    // Advance for 5 s - more than twice the fixture's 2 s window - and stay in
    // Push: continuous progress must never be faulted, however long it takes.
    for (uint32_t step = 0U; step < 100U; ++step) {
        moving.remainingM = remainingM;
        remainingM -= 0.001;
        const ev::RunStatus status = tick(harness, moving);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Push),
                                      static_cast<int>(status.phase),
                                      "the bound faulted travel that was still advancing");
    }

    // Now it stops advancing: the bound ends the run after its own window from the
    // last improvement, not from the phase entry.
    moving.remainingM = remainingM;
    const ev::RunStatus status = starveUntilFault(harness, moving, 200U);
    expectFault(ev::FaultCode::TravelStalled, status);
}

// The phase bound keeps precedence. A policy whose stall window is longer than the
// phase budget must not change which identity a starved phase produces: the phase
// deadline is still the final fail-safe.
void test_the_phase_bound_keeps_precedence_over_the_travel_progress_bound(void) {
    const ev::RunPhaseConfig shortPushBound(
        measured(0.0), measured(5.0), measured(1.0), measured(1.0), measured(5.0), measured(5.0),
        measured(5.0), measured(5.0), measured(1.0),
        stopConfig(measured(0.05), measured(0.2)), measured(0.10), measured(0.30), measured(0.30),
        measured(0.30), fixtureBottleConfig());
    Harness harness(shortPushBound, reversalConfig(), 1000U);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    Evidence stuck;
    stuck.remainingUsable = true;
    stuck.remainingM = -1.0;
    stuck.brakeEntry = true;
    // 1 s push bound vs a 5 s stall window: the phase deadline fires first.
    const ev::RunStatus status = starveUntilFault(harness, stuck, 400U);
    expectFault(ev::FaultCode::PhaseTimeout, status);
}

// The bound's arithmetic is unsigned subtraction like every other bound here, so
// it survives the millis() wrap.
void test_the_travel_progress_bound_is_wrap_safe(void) {
    Harness harness = defaultHarness(0.0);
    harness.nowMs = 0xFFFFFF00U;  // 256 ms before the wrap, inside the stall window
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Push));

    Evidence stuck;
    stuck.remainingUsable = true;
    stuck.remainingM = -1.0;
    stuck.brakeEntry = true;
    const ev::RunStatus status = starveUntilFault(harness, stuck, 400U);
    expectFault(ev::FaultCode::TravelStalled, status);
}

// --- Done and reset ---------------------------------------------------------

void test_done_never_permits_further_drive(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Done));

    // Every piece of evidence that could once have moved the run, asserted
    // together: nothing moves it, and no travel command is ever emitted again.
    Evidence everything;
    everything.start = true;
    everything.approach = true;
    everything.captured = true;
    everything.pushed = true;
    everything.remainingUsable = true;
    everything.remainingM = 0.0;
    everything.stopClaimed = true;

    for (uint32_t step = 0U; step < 40U; ++step) {
        const ev::RunStatus status = tick(harness, everything);
        expectPhase(ev::RunPhase::Done, status);
        expectCommand(ev::RunMotionCommand::BrakeReverse, status);
        TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));
        TEST_ASSERT_TRUE(status.stopVerified);
    }
}

void test_a_fault_after_done_is_still_recorded(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Done));

    Evidence faulted;
    faulted.fault = ev::FaultCode::PhaseTimeout;
    const ev::RunStatus status = tick(harness, faulted);
    expectFault(ev::FaultCode::PhaseTimeout, status);
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));
}

void test_reset_requires_a_new_announcement_and_a_new_arming_decision(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Done));

    harness.machine.reset(harness.nowMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.machine.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.machine.faultCode()));
    TEST_ASSERT_FALSE(harness.machine.stopVerified());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ReversalPhase::Idle),
                          static_cast<int>(harness.machine.reversalPhase()));

    // No run is announced, so the trigger and the armed fact do nothing. Idle
    // was stamped with the reset's reading, so the phase clock measures from the
    // boundary that entered it, not from machine construction.
    Evidence everything;
    everything.start = true;
    everything.approach = true;
    const ev::RunStatus idle = tick(harness, everything);
    expectPhase(ev::RunPhase::Idle, idle);
    expectCommand(ev::RunMotionCommand::None, idle);
    TEST_ASSERT_EQUAL_UINT32(50U, idle.phaseElapsedMs);

    // A re-arm is a fresh decision: announce, then arm, then trigger.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRun(harness)));
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed));
}

// --- Fault reachability from every phase ------------------------------------

void test_fault_is_reachable_from_every_non_terminal_phase(void) {
    const ev::RunPhase phases[] = {
        ev::RunPhase::Idle,           ev::RunPhase::ParameterValid,
        ev::RunPhase::Armed,          ev::RunPhase::OptionalDelay,
        ev::RunPhase::Approach,       ev::RunPhase::Capture,
        ev::RunPhase::Push,           ev::RunPhase::ForwardDecel,
        ev::RunPhase::BrakeZeroSpeed, ev::RunPhase::Reverse,
        ev::RunPhase::FinalApproach,  ev::RunPhase::FinalBrake,
        ev::RunPhase::Done,
    };

    for (const ev::RunPhase phase : phases) {
        Harness harness = defaultHarness(0.5);
        if (phase != ev::RunPhase::Idle) {
            announceValidRun(harness);
        }
        if (phase != ev::RunPhase::Idle && phase != ev::RunPhase::ParameterValid) {
            TEST_ASSERT_TRUE(driveTo(harness, phase));
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(phase),
                                      static_cast<int>(harness.machine.phase()),
                                      phaseName(phase));

        Evidence faulted = evidenceFor(phase);
        faulted.fault = ev::FaultCode::ProvisionalCalibration;
        const ev::RunStatus status = tick(harness, faulted);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                      static_cast<int>(status.phase),
                                      phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                                      static_cast<int>(status.fault), phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunMotionCommand::None),
                                      static_cast<int>(status.intention.command),
                                      phaseName(phase));
    }
}

void test_only_an_underway_run_requires_the_armed_state(void) {
    const ev::RunPhase underway[] = {
        ev::RunPhase::OptionalDelay,   ev::RunPhase::Approach,
        ev::RunPhase::Capture,         ev::RunPhase::Push,
        ev::RunPhase::ForwardDecel,    ev::RunPhase::BrakeZeroSpeed,
        ev::RunPhase::Reverse,         ev::RunPhase::FinalApproach,
        ev::RunPhase::FinalBrake,
    };
    const ev::RunPhase notUnderway[] = {
        ev::RunPhase::Idle,           ev::RunPhase::ParameterValid,
        ev::RunPhase::Armed,          ev::RunPhase::Done,
        ev::RunPhase::Fault,
    };

    for (const ev::RunPhase phase : underway) {
        TEST_ASSERT_TRUE(ev::isRunPhaseUnderway(phase));
    }
    for (const ev::RunPhase phase : notUnderway) {
        TEST_ASSERT_FALSE(ev::isRunPhaseUnderway(phase));
    }
}

// --- The intention and the authority ----------------------------------------

void test_an_intention_is_current_only_for_its_own_cycle(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);

    const ev::RunStatus status =
        harness.machine.update(harness.nowMs, inputsFrom(evidenceFor(ev::RunPhase::ParameterValid)));
    TEST_ASSERT_TRUE(ev::runIntentionIsCurrentFor(status.intention, harness.nowMs));
    TEST_ASSERT_FALSE(ev::runIntentionIsCurrentFor(status.intention, harness.nowMs + 50U));
    TEST_ASSERT_FALSE(ev::runIntentionIsCurrentFor(status.intention, harness.nowMs - 50U));

    // A stale Travel intention must never be driven on. Here it is command-free
    // anyway, which is the other half of the protection: no phase before the
    // travel emits a travel command.
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));
}

void test_a_stop_command_is_a_leg_tag_the_authority_never_reads_as_direction(void) {
    // A deliberate stop has no powered direction. The Forward/Reverse suffix on
    // RampDown*/Brake* says which leg the stop belongs to so it cannot be
    // attached to the wrong one - it is not a direction any consumer may act on,
    // and a stop is never "completed" with a drive in the tagged direction. The
    // mapping pinned here is the production one (`runStopEffort()`), which is
    // what the vehicle layer presents.
    const ev::RunMotionCommand stops[] = {
        ev::RunMotionCommand::RampDownForward, ev::RunMotionCommand::RampDownReverse,
        ev::RunMotionCommand::BrakeForward,    ev::RunMotionCommand::BrakeReverse,
    };
    for (const ev::RunMotionCommand command : stops) {
        TEST_ASSERT_FALSE(ev::isRunTravelCommand(command));
        TEST_ASSERT_TRUE(ev::isRunStopCommand(command));
        const ev::MotorEffort effort = ev::runStopEffort(command);
        TEST_ASSERT_TRUE(ev::validateMotorEffort(effort).ok);
        TEST_ASSERT_FALSE(ev::isDrivingMode(effort.mode));
        TEST_ASSERT_EQUAL_UINT16(0U, effort.magnitudePermille);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                              static_cast<int>(ev::poweredDirectionOf(effort.mode)));
    }

    // RampDown* asks for no powered effort at all; Brake* is the active
    // non-powered hold. Both drop the leg tag.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::RampDownForward).mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::RampDownReverse).mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Brake),
                          static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::BrakeForward).mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Brake),
                          static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::BrakeReverse).mode));

    // A command that is not a stop command fails closed to no powered effort, so
    // a caller bug cannot turn a travel command into a second, competing stop.
    const ev::RunMotionCommand notStops[] = {ev::RunMotionCommand::None,
                                             ev::RunMotionCommand::DriveForward,
                                             ev::RunMotionCommand::DriveReverse};
    for (const ev::RunMotionCommand command : notStops) {
        const ev::MotorEffort effort = ev::runStopEffort(command);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                              static_cast<int>(effort.mode));
        TEST_ASSERT_EQUAL_UINT16(0U, effort.magnitudePermille);
    }

    // The two brake tags are indistinguishable to the authority: both present
    // the same non-powered hold, so no leg tag can become a direction.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::BrakeForward).mode),
        static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::BrakeReverse).mode));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::RampDownForward).mode),
        static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::RampDownReverse).mode));

    // Travel commands do carry the powered direction they name, so the tag
    // dropping above is specific to stops rather than a lost direction.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::PoweredDirection::Forward),
        static_cast<int>(ev::poweredDirectionOf(
            vehicleEffortFor(ev::RunMotionCommand::DriveForward).mode)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::PoweredDirection::Reverse),
        static_cast<int>(ev::poweredDirectionOf(
            vehicleEffortFor(ev::RunMotionCommand::DriveReverse).mode)));
}

void test_the_run_leg_is_the_leg_the_command_names(void) {
    // Travel names the leg it travels; a stop names the leg it belongs to. This
    // is the leg-tag interpretation the run axis and the along-leg gate consume,
    // and it is a leg - never a powered direction. `None` names no leg at all: a
    // caller keeps the leg it had, which is why the answer for `None` is not to
    // be applied.
    TEST_ASSERT_TRUE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::DriveForward));
    TEST_ASSERT_TRUE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::RampDownForward));
    TEST_ASSERT_TRUE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::BrakeForward));
    TEST_ASSERT_FALSE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::DriveReverse));
    TEST_ASSERT_FALSE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::RampDownReverse));
    TEST_ASSERT_FALSE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::BrakeReverse));
    TEST_ASSERT_FALSE(ev::runCommandNamesForwardLeg(ev::RunMotionCommand::None));

    // The two mappings agree with each other and with the command vocabulary: a
    // stop is a stop and a travel is a travel, so the leg tag is never the only
    // thing a consumer has to go on.
    const ev::RunMotionCommand allCommands[] = {
        ev::RunMotionCommand::DriveForward, ev::RunMotionCommand::DriveReverse,
        ev::RunMotionCommand::RampDownForward, ev::RunMotionCommand::RampDownReverse,
        ev::RunMotionCommand::BrakeForward, ev::RunMotionCommand::BrakeReverse};
    for (const ev::RunMotionCommand command : allCommands) {
        TEST_ASSERT_EQUAL_INT(ev::isRunTravelCommand(command) ? 1 : 0,
                              ev::isRunStopCommand(command) ? 0 : 1);
    }
}

void test_the_authority_refuses_a_direct_reverse_independently(void) {
    ev::SafetyAuthority authority;
    const ev::SafetyLimits limits = fixtureLimits();
    TEST_ASSERT_TRUE(authority.arm(ready()));

    // The run's own system state, with the zero-speed fact taken from the state
    // machine's verification verdict - which is what the vehicle layer does.
    ev::SafetySystemState state = readySafetyState();

    // Forward drive is granted and remembered.
    const ev::AuthorizationResult forward =
        authority.authorize(ev::MotorEffort(ev::MotorMode::DriveForward, 300U), state, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(forward.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    // A caller that skips the run - a "malicious" direct reverse - is refused
    // even while zero speed is claimed.
    state.atZeroSpeed = true;
    const ev::AuthorizationResult direct =
        authority.authorize(ev::MotorEffort(ev::MotorMode::DriveReverse, 300U), state, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(direct.outcome));
    TEST_ASSERT_FALSE(direct.command.isDriving());

    // The deliberate path: the run presents its own stop at the verified zero,
    // which is the only thing that clears the remembered direction.
    const ev::AuthorizationResult stop = authority.authorize(
        ev::MotorEffort(ev::MotorMode::Brake, 0U), state, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedStop),
                          static_cast<int>(stop.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(authority.lastAuthorizedDirection()));

    const ev::AuthorizationResult reverse =
        authority.authorize(ev::MotorEffort(ev::MotorMode::DriveReverse, 300U), state, limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::AuthorizedDrive),
                          static_cast<int>(reverse.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(authority.lastAuthorizedDirection()));
}

void test_the_run_and_the_authority_never_flip_direction_without_a_verified_stop(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);

    ev::SafetyAuthority authority;
    const ev::SafetyLimits limits = fixtureLimits();
    TEST_ASSERT_TRUE(authority.arm(ready()));

    uint32_t stopCyclesSinceTravel = 0U;
    bool verifiedStopSinceTravel = false;
    bool flipPrecededByStop = false;
    bool flipPrecededByVerifiedStop = false;
    ev::RunMotionCommand lastTravel = ev::RunMotionCommand::None;
    ev::PoweredDirection grantedDirection = ev::PoweredDirection::None;

    for (uint32_t step = 0U; step < 200U; ++step) {
        const ev::RunStatus status = harness.machine.update(
            harness.nowMs, inputsFrom(evidenceFor(harness.machine.phase())));
        harness.nowMs += 50U;

        const ev::RunMotionCommand command = status.intention.command;
        if (ev::isRunTravelCommand(command)) {
            if (lastTravel != ev::RunMotionCommand::None && command != lastTravel) {
                // A commanded reversal must be preceded by at least one cycle of
                // the deliberate stop, taken at a verified stop.
                flipPrecededByStop = stopCyclesSinceTravel >= 1U;
                flipPrecededByVerifiedStop = verifiedStopSinceTravel;
            }
            lastTravel = command;
            stopCyclesSinceTravel = 0U;
            verifiedStopSinceTravel = false;
        } else {
            if (ev::isRunStopCommand(command)) {
                ++stopCyclesSinceTravel;
            }
            if (status.stopVerified) {
                verifiedStopSinceTravel = true;
            }
        }

        // Present exactly one request per cycle, as the vehicle layer must.
        ev::SafetySystemState state = readySafetyState();
        state.atZeroSpeed = status.stopVerified;
        state.nowMs = harness.nowMs;
        const ev::MotorEffort effort = vehicleEffortFor(command);
        const ev::AuthorizationResult result = authority.authorize(effort, state, limits);

        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::Fault),
                              static_cast<int>(status.phase));

        if (ev::isRunTravelCommand(command)) {
            TEST_ASSERT_TRUE(result.command.isDriving());
            const ev::PoweredDirection direction = result.command.poweredDirection();
            if (grantedDirection != ev::PoweredDirection::None && direction != grantedDirection) {
                // The authority only ever moved to the other direction after a
                // stop it authorized at a reported zero.
                TEST_ASSERT_TRUE(flipPrecededByStop);
                TEST_ASSERT_TRUE(flipPrecededByVerifiedStop);
            }
            grantedDirection = direction;
        } else {
            TEST_ASSERT_FALSE(result.command.isDriving());
        }

        if (status.phase == ev::RunPhase::Done) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Done),
                          static_cast<int>(harness.machine.phase()));
}

void test_reversal_refusals_map_to_faults(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(ev::faultForReversalRefusal(ev::ReversalRefusal::None)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::ProvisionalCalibration),
        static_cast<int>(ev::faultForReversalRefusal(ev::ReversalRefusal::ConfigIsNotUsable)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultCode::ReversalNotVerified),
        static_cast<int>(ev::faultForReversalRefusal(ev::ReversalRefusal::TimedOut)));

    // Every run-policy fault is a software condition, so the fault policy stops
    // by removing outputs (the only response safe to assume before the driver has
    // been measured).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultStopPolicy::Disable),
                          static_cast<int>(ev::faultStopPolicyFor(ev::FaultCode::PhaseTimeout)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultStopPolicy::Disable),
        static_cast<int>(ev::faultStopPolicyFor(ev::FaultCode::ReversalNotVerified)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultStopPolicy::Disable),
        static_cast<int>(ev::faultStopPolicyFor(ev::FaultCode::DisarmedDuringRun)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultStopPolicy::Disable),
        static_cast<int>(ev::faultStopPolicyFor(ev::FaultCode::RunSequencingError)));
    // The run-scoped duration bound is a software condition too (S10): it raises a
    // whole-run identity, and it gets the same "remove output" response.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::FaultStopPolicy::Disable),
        static_cast<int>(ev::faultStopPolicyFor(ev::FaultCode::RunDurationExceeded)));
}

// D35: the maximum optional delay is a rule value (rule 8.i), not a
// measurement. A cited RuleDerived bound passes; a placeholder or an uncited
// rule value fails, mirroring the final-stop dwell policy.
void test_cited_rule_derived_optional_delay_passes(void) {
    const ev::Sourced<double> citedDelay(20.0, ev::ValueSource::RuleDerived,
                                         "rules2027 8.i (Target Time maximum)");
    TEST_ASSERT_TRUE(citedDelay.citesEvidence());
    TEST_ASSERT_TRUE(ev::optionalDelayProvenanceIsEstablished(citedDelay));
    const ev::RunPhaseConfig config(
        citedDelay, measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)),
        measured(0.10), measured(0.30), measured(0.30), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_TRUE(ev::validateRunPhaseConfig(config).ok);
    ev::CompetitionStateMachine machine(config, reversalConfig());
    TEST_ASSERT_TRUE(machine.configUsable());
}

void test_uncited_and_provisional_optional_delay_fail(void) {
    // Provisional placeholder fails.
    const ev::RunPhaseConfig provisionalConfig(
        provisional(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0),
        measured(5.0), measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)),
        measured(0.10), measured(0.30), measured(0.30), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_FALSE(ev::validateRunPhaseConfig(provisionalConfig).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhaseConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateRunPhaseConfig(provisionalConfig).error));

    // Uncited RuleDerived fails exactly like a placeholder: no smuggled bound.
    const ev::Sourced<double> uncitedDelay(20.0, ev::ValueSource::RuleDerived);
    TEST_ASSERT_FALSE(uncitedDelay.citesEvidence());
    TEST_ASSERT_FALSE(ev::optionalDelayProvenanceIsEstablished(uncitedDelay));
    const ev::RunPhaseConfig uncitedConfig(
        uncitedDelay, measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)),
        measured(0.10), measured(0.30), measured(0.30), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_FALSE(ev::validateRunPhaseConfig(uncitedConfig).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhaseConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateRunPhaseConfig(uncitedConfig).error));

    // A measured bound still passes, so the fix did not narrow the old path.
    TEST_ASSERT_TRUE(ev::validateRunPhaseConfig(phaseConfig(1.0)).ok);
}

// The bottle-phase strategy ceilings are the run policy's per-phase speeds, and
// the run layer selects one by the phase the run itself named. This pins the
// mapping and its limits: only Approach/Capture/Push carry a ceiling, and every
// other phase is explicitly without one, so a ramp, a stop or the reverse leg is
// never shaped by a bottle-phase ceiling.
void test_the_bottle_phase_ceilings_are_selected_by_phase(void) {
    const ev::RunPhaseConfig config(
        measured(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)), measured(0.10),
        measured(0.11), measured(0.22), measured(0.33), fixtureBottleConfig());
    TEST_ASSERT_TRUE(ev::validateRunPhaseConfig(config).ok);

    const ev::StrategyVelocityCeiling approach =
        ev::strategyVelocityCeilingForPhase(config, ev::RunPhase::Approach);
    TEST_ASSERT_TRUE(approach.active);
    TEST_ASSERT_TRUE(approach.velocityMps == 0.11);

    const ev::StrategyVelocityCeiling capture =
        ev::strategyVelocityCeilingForPhase(config, ev::RunPhase::Capture);
    TEST_ASSERT_TRUE(capture.active);
    TEST_ASSERT_TRUE(capture.velocityMps == 0.22);

    const ev::StrategyVelocityCeiling push =
        ev::strategyVelocityCeilingForPhase(config, ev::RunPhase::Push);
    TEST_ASSERT_TRUE(push.active);
    TEST_ASSERT_TRUE(push.velocityMps == 0.33);

    const ev::RunPhase ceilinglessPhases[] = {
        ev::RunPhase::Idle,          ev::RunPhase::ParameterValid, ev::RunPhase::Armed,
        ev::RunPhase::OptionalDelay, ev::RunPhase::ForwardDecel,  ev::RunPhase::BrakeZeroSpeed,
        ev::RunPhase::Reverse,       ev::RunPhase::FinalApproach,  ev::RunPhase::FinalBrake,
        ev::RunPhase::Done,          ev::RunPhase::Fault};
    for (ev::RunPhase phase : ceilinglessPhases) {
        const ev::StrategyVelocityCeiling ceiling =
            ev::strategyVelocityCeilingForPhase(config, phase);
        TEST_ASSERT_FALSE(ceiling.active);
        TEST_ASSERT_TRUE(ceiling.velocityMps == 0.0);
    }
}

// Every production bottle-phase ceiling is a Provisional placeholder today, so
// the whole run policy refuses and the ceilings are unusable: no run can be
// announced or armed on them. These are the refusals the bottle-phase
// measurements must earn their way past (P-H8), and no value may be guessed in
// the meantime to make a run move.
void test_a_placeholder_bottle_phase_ceiling_is_unusable(void) {
    // Provisional provenance: refused before anything is sequenced.
    const ev::RunPhaseConfig provisionalCeilings(
        measured(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)), measured(0.10),
        provisional(0.30), measured(0.30), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_FALSE(ev::validateRunPhaseConfig(provisionalCeilings).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhaseConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateRunPhaseConfig(provisionalCeilings).error));
    ev::CompetitionStateMachine placeholderMachine(provisionalCeilings, reversalConfig());
    TEST_ASSERT_FALSE(placeholderMachine.configUsable());

    // A measured ceiling that is not a positive speed is refused: the phases it
    // belongs to carry travel, and a zero ceiling would be a standstill the run
    // cannot complete.
    const ev::RunPhaseConfig zeroCeiling(
        measured(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)), measured(0.10),
        measured(0.0), measured(0.30), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhaseConfigError::NonPositiveStrategyCeiling),
                          static_cast<int>(ev::validateRunPhaseConfig(zeroCeiling).error));

    // A NaN ceiling is told apart from a non-positive one.
    const ev::RunPhaseConfig nanCeiling(
        measured(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(2.0), measured(1.0), stopConfig(measured(0.05), measured(0.2)), measured(0.10),
        measured(0.30), measured(std::nan("")), measured(0.30), fixtureBottleConfig());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhaseConfigError::NonFiniteValue),
                          static_cast<int>(ev::validateRunPhaseConfig(nanCeiling).error));
}

// ---------------------------------------------------------------------------
// The run's recorded timeline (S8)
// ---------------------------------------------------------------------------
//
// The run policy records when the run was actuated, the start of the final
// stop's dwell interval and when that interval's verification completed. These
// tests pin the ownership, the discard/restart rule, the clock-wrap behaviour and
// the two boundaries that can erase the record (a fault and a reset). Every time
// here is a fixture; none is a measured vehicle fact.

void test_the_run_timeline_records_actuation_the_dwell_and_the_verification(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);

    const ev::RunTimeline fresh = harness.machine.timeline();
    TEST_ASSERT_FALSE(fresh.actuated);
    TEST_ASSERT_FALSE(fresh.dwellStartKnown);
    TEST_ASSERT_FALSE(fresh.finalStopVerified);
    TEST_ASSERT_EQUAL_UINT32(0U, fresh.actuatedAtMs);
    TEST_ASSERT_EQUAL_UINT32(0U, fresh.finalStopDwellStartMs);
    TEST_ASSERT_EQUAL_UINT32(0U, fresh.finalStopVerifiedAtMs);

    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed));
    // Nothing is actuated until the trigger: an announced, armed run is not a run
    // that has begun.
    TEST_ASSERT_FALSE(harness.machine.timeline().actuated);

    const ev::RunStatus started = tick(harness, evidenceFor(ev::RunPhase::Armed));
    expectPhase(ev::RunPhase::Approach, started);
    const uint32_t actuatedAtMs = harness.nowMs;
    const ev::RunTimeline actuated = harness.machine.timeline();
    TEST_ASSERT_TRUE(actuated.actuated);
    TEST_ASSERT_EQUAL_UINT32(actuatedAtMs, actuated.actuatedAtMs);
    // A run that has not reached its final stop has no dwell start and no
    // verification to report, and a zero reading is not a claim of either.
    TEST_ASSERT_FALSE(actuated.dwellStartKnown);
    TEST_ASSERT_FALSE(actuated.finalStopVerified);

    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalBrake));

    // The first final-brake cycle opens the dwell candidate, and the candidate's
    // start is that cycle's own clock reading.
    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    const uint32_t dwellOpenedAtMs = harness.nowMs;
    expectPhase(ev::RunPhase::FinalBrake, status);
    const ev::RunTimeline dwelling = harness.machine.timeline();
    TEST_ASSERT_TRUE(dwelling.dwellStartKnown);
    TEST_ASSERT_EQUAL_UINT32(dwellOpenedAtMs, dwelling.finalStopDwellStartMs);
    // An open interval is a candidate, not a verified stop: the rule's interval
    // has not completed yet.
    TEST_ASSERT_FALSE(dwelling.finalStopVerified);

    uint32_t verifiedAtMs = 0U;
    for (uint32_t step = 0U; step < 20U; ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
        if (status.phase == ev::RunPhase::Done) {
            verifiedAtMs = harness.nowMs;
            break;
        }
        expectPhase(ev::RunPhase::FinalBrake, status);
    }
    expectPhase(ev::RunPhase::Done, status);
    TEST_ASSERT_TRUE(status.stopVerified);
    // `Done` is still a deliberate non-powered hold, unchanged.
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(status.intention.command));

    const ev::RunTimeline done = harness.machine.timeline();
    TEST_ASSERT_TRUE(done.actuated);
    TEST_ASSERT_EQUAL_UINT32(actuatedAtMs, done.actuatedAtMs);
    TEST_ASSERT_TRUE(done.dwellStartKnown);
    TEST_ASSERT_TRUE(done.finalStopVerified);
    TEST_ASSERT_EQUAL_UINT32(verifiedAtMs, done.finalStopVerifiedAtMs);
    // The verified start is the candidate's start (mirrored from the verifier,
    // not recomputed), and the fixture's 0.2 s dwell is what ran between it and
    // the completion.
    TEST_ASSERT_EQUAL_UINT32(dwellOpenedAtMs, done.finalStopDwellStartMs);
    TEST_ASSERT_EQUAL_UINT32(200U, done.finalStopVerifiedAtMs - done.finalStopDwellStartMs);
    // The internal Run Time is the unsigned difference of the actuation and the
    // verified dwell start: the Complete Stop's own interval is outside it, so a
    // reader that took the `Done` tick instead would overcount by exactly it.
    TEST_ASSERT_EQUAL_UINT32(dwellOpenedAtMs - actuatedAtMs,
                             done.finalStopDwellStartMs - done.actuatedAtMs);
    TEST_ASSERT_EQUAL_UINT32(done.finalStopDwellStartMs + 200U, done.finalStopVerifiedAtMs);
}

void test_a_broken_final_stop_dwell_is_discarded_and_restarted(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalBrake));

    // The first candidate opens and accumulates toward the rule's interval.
    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    const uint32_t firstCandidateAtMs = harness.nowMs;
    expectPhase(ev::RunPhase::FinalBrake, status);
    TEST_ASSERT_EQUAL_UINT32(firstCandidateAtMs,
                             harness.machine.timeline().finalStopDwellStartMs);
    status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    expectPhase(ev::RunPhase::FinalBrake, status);
    TEST_ASSERT_TRUE(harness.machine.timeline().dwellStartKnown);

    // The near-zero evidence breaks before the interval completes: the candidate
    // is discarded. There is nothing to keep - it was never verified - and the
    // run must not report a start it can no longer vouch for.
    Evidence moving;
    moving.stopSpeedMps = 0.5;
    moving.stopClaimed = false;
    status = tick(harness, moving);
    expectPhase(ev::RunPhase::FinalBrake, status);
    const ev::RunTimeline discarded = harness.machine.timeline();
    TEST_ASSERT_FALSE(discarded.dwellStartKnown);
    TEST_ASSERT_EQUAL_UINT32(0U, discarded.finalStopDwellStartMs);
    TEST_ASSERT_FALSE(discarded.finalStopVerified);

    // The interval restarts where the evidence returns, not where it first began.
    status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    const uint32_t secondCandidateAtMs = harness.nowMs;
    expectPhase(ev::RunPhase::FinalBrake, status);
    const ev::RunTimeline restarted = harness.machine.timeline();
    TEST_ASSERT_TRUE(restarted.dwellStartKnown);
    TEST_ASSERT_EQUAL_UINT32(secondCandidateAtMs, restarted.finalStopDwellStartMs);
    TEST_ASSERT_TRUE(secondCandidateAtMs > firstCandidateAtMs);

    uint32_t verifiedAtMs = 0U;
    for (uint32_t step = 0U; step < 20U; ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
        if (status.phase == ev::RunPhase::Done) {
            verifiedAtMs = harness.nowMs;
            break;
        }
        expectPhase(ev::RunPhase::FinalBrake, status);
    }
    expectPhase(ev::RunPhase::Done, status);

    const ev::RunTimeline done = harness.machine.timeline();
    TEST_ASSERT_TRUE(done.finalStopVerified);
    // The verified start is the interval that actually satisfied the rule - the
    // second one - and not the discarded first candidate.
    TEST_ASSERT_EQUAL_UINT32(secondCandidateAtMs, done.finalStopDwellStartMs);
    TEST_ASSERT_EQUAL_UINT32(200U, done.finalStopVerifiedAtMs - done.finalStopDwellStartMs);
    TEST_ASSERT_EQUAL_UINT32(verifiedAtMs, done.finalStopVerifiedAtMs);
}

void test_the_run_timeline_stays_correct_across_the_uint32_wrap(void) {
    // Start a few cycles before the `millis()` wrap so the whole run crosses it:
    // the actuation reading is then numerically *above* the verified dwell start.
    Harness harness(phaseConfig(0.0), reversalConfig(), 0xFFFFFF00U, 0.0);
    announceValidRun(harness);

    bool actuated = false;
    bool reachedDone = false;
    // Counted here, in the test, from the ticks themselves - not recovered from
    // the same readings under test.
    uint32_t elapsedFromActuationMs = 0U;
    for (uint32_t step = 0U; step < 200U && !reachedDone; ++step) {
        const ev::RunStatus status = tick(harness, evidenceFor(harness.machine.phase()));
        if (!actuated && harness.machine.timeline().actuated) {
            actuated = true;
            elapsedFromActuationMs = 0U;
        } else if (actuated) {
            elapsedFromActuationMs += 50U;
        }
        if (status.phase == ev::RunPhase::Done) {
            reachedDone = true;
        } else if (status.phase == ev::RunPhase::Fault) {
            break;
        }
    }
    TEST_ASSERT_TRUE(actuated);
    TEST_ASSERT_TRUE(reachedDone);

    const ev::RunTimeline done = harness.machine.timeline();
    TEST_ASSERT_TRUE(done.finalStopVerified);
    // The readings really wrapped: the raw actuation reading is above the raw
    // dwell start, which no wall-clock reading could satisfy.
    TEST_ASSERT_TRUE(done.actuatedAtMs > done.finalStopDwellStartMs);
    // An unsigned difference is all the elapsed time needs - no reconciliation,
    // no epoch - and it is exactly the span this test counted tick by tick.
    TEST_ASSERT_EQUAL_UINT32(elapsedFromActuationMs,
                             done.finalStopVerifiedAtMs - done.actuatedAtMs);
    TEST_ASSERT_EQUAL_UINT32(elapsedFromActuationMs - 200U,
                             done.finalStopDwellStartMs - done.actuatedAtMs);
}

void test_reset_clears_the_run_timeline_and_a_new_run_starts_empty(void) {
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalBrake));
    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    for (uint32_t step = 0U; step < 20U && status.phase != ev::RunPhase::Done; ++step) {
        status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    }
    expectPhase(ev::RunPhase::Done, status);
    TEST_ASSERT_TRUE(harness.machine.timeline().finalStopVerified);

    harness.machine.reset(harness.nowMs + 50U);
    const ev::RunTimeline cleared = harness.machine.timeline();
    TEST_ASSERT_FALSE(cleared.actuated);
    TEST_ASSERT_FALSE(cleared.dwellStartKnown);
    TEST_ASSERT_FALSE(cleared.finalStopVerified);
    TEST_ASSERT_EQUAL_UINT32(0U, cleared.actuatedAtMs);
    TEST_ASSERT_EQUAL_UINT32(0U, cleared.finalStopDwellStartMs);
    TEST_ASSERT_EQUAL_UINT32(0U, cleared.finalStopVerifiedAtMs);

    // A run announced after the reset starts with an empty timeline: the new-run
    // boundary cannot inherit a reading from the run before it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(announceValidRunWithDelay(harness, 0.0)));
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::Armed));
    TEST_ASSERT_FALSE(harness.machine.timeline().actuated);
    status = tick(harness, evidenceFor(ev::RunPhase::Armed));
    expectPhase(ev::RunPhase::Approach, status);
    const ev::RunTimeline restarted = harness.machine.timeline();
    TEST_ASSERT_TRUE(restarted.actuated);
    TEST_ASSERT_EQUAL_UINT32(harness.nowMs, restarted.actuatedAtMs);
    TEST_ASSERT_FALSE(restarted.dwellStartKnown);
    TEST_ASSERT_FALSE(restarted.finalStopVerified);
}

void test_a_fault_discards_a_candidate_but_keeps_a_verified_record(void) {
    // (a) An in-progress dwell is not a fact: a fault discards it, and the run
    // that faulted has no verified final stop to report.
    Harness harness = defaultHarness(0.0);
    announceValidRun(harness);
    TEST_ASSERT_TRUE(driveTo(harness, ev::RunPhase::FinalBrake));
    ev::RunStatus status = tick(harness, evidenceFor(ev::RunPhase::FinalBrake));
    expectPhase(ev::RunPhase::FinalBrake, status);
    TEST_ASSERT_TRUE(harness.machine.timeline().dwellStartKnown);

    Evidence faulted;
    faulted.fault = ev::FaultCode::PhaseTimeout;
    status = tick(harness, faulted);
    expectFault(ev::FaultCode::PhaseTimeout, status);
    const ev::RunTimeline afterFault = harness.machine.timeline();
    // The run *was* actuated; a fault cannot unmake that.
    TEST_ASSERT_TRUE(afterFault.actuated);
    TEST_ASSERT_FALSE(afterFault.dwellStartKnown);
    TEST_ASSERT_FALSE(afterFault.finalStopVerified);

    // (b) A completed verification is a fact: a later fault does not erase what
    // the run completed, while the presentation verdict stays false in `Fault`.
    Harness second = defaultHarness(0.0);
    announceValidRun(second);
    TEST_ASSERT_TRUE(driveTo(second, ev::RunPhase::FinalBrake));
    status = tick(second, evidenceFor(ev::RunPhase::FinalBrake));
    for (uint32_t step = 0U; step < 20U && status.phase != ev::RunPhase::Done; ++step) {
        status = tick(second, evidenceFor(ev::RunPhase::FinalBrake));
    }
    expectPhase(ev::RunPhase::Done, status);
    const uint32_t verifiedAtMs = second.machine.timeline().finalStopVerifiedAtMs;

    Evidence lateFault;
    lateFault.fault = ev::FaultCode::PhaseTimeout;
    status = tick(second, lateFault);
    expectFault(ev::FaultCode::PhaseTimeout, status);
    TEST_ASSERT_FALSE(status.stopVerified);
    const ev::RunTimeline kept = second.machine.timeline();
    TEST_ASSERT_TRUE(kept.finalStopVerified);
    TEST_ASSERT_EQUAL_UINT32(verifiedAtMs, kept.finalStopVerifiedAtMs);
    TEST_ASSERT_TRUE(kept.dwellStartKnown);
    TEST_ASSERT_TRUE(kept.actuated);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_fresh_state_machine_is_idle_and_asks_for_nothing);
    RUN_TEST(test_a_fault_is_reachable_from_idle);
    RUN_TEST(test_an_invalid_announcement_faults_and_requires_an_explicit_reset);
    RUN_TEST(test_a_policy_configuration_that_is_not_usable_faults);
    RUN_TEST(test_a_run_cannot_be_announced_twice);
    RUN_TEST(test_a_trigger_before_arming_starts_nothing);
    RUN_TEST(test_a_disarm_before_the_trigger_returns_to_parameter_valid);
    RUN_TEST(test_the_optional_delay_is_spent_before_any_travel);
    RUN_TEST(test_approach_waits_for_completion_and_times_out);
    RUN_TEST(test_capture_waits_for_engagement_and_times_out);
    RUN_TEST(test_push_waits_for_completion_and_times_out);
    RUN_TEST(test_a_bound_below_a_millisecond_expires_rather_than_never);
    RUN_TEST(test_the_phase_bound_is_inclusive_and_late_evidence_is_refused);
    RUN_TEST(test_a_stalled_loop_faults_at_the_next_update);
    RUN_TEST(test_a_non_finite_remaining_distance_does_not_end_the_final_leg);
    RUN_TEST(test_every_legal_transition_in_order);
    RUN_TEST(test_the_reversal_presentation_and_the_sequencer_evidence_stay_distinct);
    RUN_TEST(test_the_handoff_presents_a_stop_cycle_before_asking_for_reverse);
    RUN_TEST(test_fresh_motion_before_the_handoff_aborts_the_reversal);
    RUN_TEST(test_a_fault_with_a_pending_handoff_faults_instead_of_reversing);
    RUN_TEST(test_reset_with_a_pending_handoff_returns_to_nothing_verified);
    RUN_TEST(test_no_evidence_can_skip_the_reversal_phases);
    RUN_TEST(test_the_reversal_timeout_faults_instead_of_reversing);
    RUN_TEST(test_stale_stop_evidence_never_permits_the_reversal);
    RUN_TEST(test_oscillation_around_the_threshold_faults_instead_of_reversing);
    RUN_TEST(test_a_fault_during_the_reversal_stops_the_run_and_keeps_the_first_fault);
    RUN_TEST(test_a_disarm_during_the_run_faults);
    RUN_TEST(test_a_disarm_during_the_reversal_faults);
    RUN_TEST(test_reverse_waits_for_the_closing_distance_and_times_out);
    RUN_TEST(test_final_approach_waits_for_the_target_point_and_times_out);
    RUN_TEST(test_final_brake_waits_for_the_verified_stop_and_times_out);
    RUN_TEST(test_a_brake_entry_travel_phase_without_progress_faults_travel_stalled);
    RUN_TEST(test_the_travel_progress_bound_is_inactive_outside_brake_entry);
    RUN_TEST(test_the_travel_progress_bound_pauses_on_unusable_evidence);
    RUN_TEST(test_progress_resets_the_travel_progress_bound);
    RUN_TEST(test_the_phase_bound_keeps_precedence_over_the_travel_progress_bound);
    RUN_TEST(test_the_travel_progress_bound_is_wrap_safe);
    RUN_TEST(test_done_never_permits_further_drive);
    RUN_TEST(test_a_fault_after_done_is_still_recorded);
    RUN_TEST(test_reset_requires_a_new_announcement_and_a_new_arming_decision);
    RUN_TEST(test_fault_is_reachable_from_every_non_terminal_phase);
    RUN_TEST(test_only_an_underway_run_requires_the_armed_state);
    RUN_TEST(test_an_intention_is_current_only_for_its_own_cycle);
    RUN_TEST(test_a_stop_command_is_a_leg_tag_the_authority_never_reads_as_direction);
    RUN_TEST(test_the_run_leg_is_the_leg_the_command_names);
    RUN_TEST(test_the_authority_refuses_a_direct_reverse_independently);
    RUN_TEST(test_the_run_and_the_authority_never_flip_direction_without_a_verified_stop);
    RUN_TEST(test_reversal_refusals_map_to_faults);
    RUN_TEST(test_cited_rule_derived_optional_delay_passes);
    RUN_TEST(test_uncited_and_provisional_optional_delay_fail);
    RUN_TEST(test_the_bottle_phase_ceilings_are_selected_by_phase);
    RUN_TEST(test_a_placeholder_bottle_phase_ceiling_is_unusable);
    RUN_TEST(test_the_run_timeline_records_actuation_the_dwell_and_the_verification);
    RUN_TEST(test_a_broken_final_stop_dwell_is_discarded_and_restarted);
    RUN_TEST(test_the_run_timeline_stays_correct_across_the_uint32_wrap);
    RUN_TEST(test_reset_clears_the_run_timeline_and_a_new_run_starts_empty);
    RUN_TEST(test_a_fault_discards_a_candidate_but_keeps_a_verified_record);
    return UNITY_END();
}
