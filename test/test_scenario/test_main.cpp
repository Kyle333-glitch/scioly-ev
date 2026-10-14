#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include <unity.h>

#include <CompetitionStateMachine.h>
#include <Motion.h>
#include <MotionChainReset.h>
#include <MotionProfile.h>
#include <Odometry.h>
#include <QuadratureDecoder.h>
#include <ReversalSequencer.h>
#include <RunConfig.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <Snapshots.h>
#include <Units.h>
#include <VehicleState.h>
#include <VelocityController.h>
#include <VelocityEstimator.h>

#include "ScenarioHarness.h"

// ---------------------------------------------------------------------------
// P6 - scripted end-to-end scenarios and the fault harness
// ---------------------------------------------------------------------------
//
// These cases drive the real production chain - decoder, odometry, estimator,
// run policy, target shaping, regulator, safety boundary - with predetermined
// timestamps, an announced RunConfig, trigger and bottle evidence, scripted
// encoder readings, sensor dropouts and injected faults, and they assert the
// architecture properties the frozen contracts name:
//
//   * only authorized commands reach the actuator boundary;
//   * no powered direct reversal is ever requested or granted;
//   * FAULT and DONE are safe at the boundary;
//   * an invalid configuration never starts a run;
//   * progression is deterministic for the scripted evidence.
//
// Nothing here is a simulator: no effort is integrated into a velocity, and no
// torque, friction, braking distance, bottle dynamics or sensor noise is
// modelled. Every encoder reading is prescribed by the scenario and the chain's
// response to that prescription is what is asserted. A passing case is software
// verification only - it is never evidence that the vehicle moves, stops, pushes
// a bottle or reaches any phase in the real world, and every hardware row in
// VERIFIED_STATE.md stays UNVERIFIED.

static_assert(!std::is_constructible<ev::AuthorizedMotorCommand, ev::MotorEffort>::value,
              "a request must never become an authorized command");
static_assert(!std::is_default_constructible<ev::RunStatus>::value,
              "a run status must never default-construct");
static_assert(!std::is_default_constructible<ev::MotionProfileCommand>::value,
              "a profile command must never default-construct");
static_assert(!std::is_default_constructible<ev::VelocityEstimate>::value,
              "an estimate must never default-construct");
static_assert(static_cast<int>(ev::RunPhase::Idle) == 0,
              "the idle phase must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

using evscenario::CycleRecord;
using evscenario::CycleScript;
using evscenario::PlantConfig;
using evscenario::PlantModel;
using evscenario::ScenarioHarness;

evscenario::ScenarioFixtures fixtures() { return evscenario::nominalFixtures(); }

// The fixture set the output-coupled plant tests use (S18B): the nominal fixtures
// with room in the reversal and final-stop bounds, so the plant's deliberately
// uncalibrated coast time is not itself what ends a nominally successful planted
// run. These are fixtures like every other number here, not measurements, and they
// change nothing about the brake-entry travel-progress bound under test.
evscenario::ScenarioFixtures plantedFixtures() {
    evscenario::ScenarioFixtures planted = evscenario::nominalFixtures();
    planted.reversal.reversalTimeoutS = evscenario::measuredValue(3.0);
    planted.phase.finalStopTimeoutS = evscenario::measuredValue(3.0);
    return planted;
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

const char* faultName(ev::FaultCode fault) {
    switch (fault) {
        case ev::FaultCode::None: return "None";
        case ev::FaultCode::InvalidRunConfig: return "InvalidRunConfig";
        case ev::FaultCode::ProvisionalCalibration: return "ProvisionalCalibration";
        case ev::FaultCode::MissingSafetyLimits: return "MissingSafetyLimits";
        case ev::FaultCode::InvalidMotorEffort: return "InvalidMotorEffort";
        case ev::FaultCode::PhaseTimeout: return "PhaseTimeout";
        case ev::FaultCode::ReversalNotVerified: return "ReversalNotVerified";
        case ev::FaultCode::DisarmedDuringRun: return "DisarmedDuringRun";
        case ev::FaultCode::RunSequencingError: return "RunSequencingError";
        case ev::FaultCode::DriverFaultBar: return "DriverFaultBar";
        case ev::FaultCode::RunDurationExceeded: return "RunDurationExceeded";
        case ev::FaultCode::TravelStalled: return "TravelStalled";
        case ev::FaultCode::EncoderFeedbackLost: return "EncoderFeedbackLost";
    }
    return "?";
}

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

// The evidence the nominal run needs, phase by phase. The only facts stated are
// how the encoder moved and what the (non-existent) mechanism reported; the
// stop-verification packet is composed by the harness from the estimator's own
// answers, and the geometry comes from the announced configuration.
class NominalDriver {
public:
    CycleScript next(const ScenarioHarness& harness) {
        const ev::RunPhase phase = harness.phase();
        if (phase != lastPhase_) {
            lastPhase_ = phase;
            cyclesInPhase_ = 0U;
        } else {
            ++cyclesInPhase_;
        }

        CycleScript script;
        switch (phase) {
            case ev::RunPhase::Idle:
            case ev::RunPhase::ParameterValid:
            case ev::RunPhase::OptionalDelay:
                break;
            case ev::RunPhase::Armed:
                script.startTriggered = true;
                break;
            case ev::RunPhase::Approach:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                // The progression facts come from the production strategy - the run
                // axis thresholded at the plan's own landmarks - so this script never
                // carries a second copy of a threshold. Approach completes when the
                // credible capture window opens, which is not a contact claim.
                script.approachComplete = harness.forwardBottleEvidence().approachComplete;
                break;
            case ev::RunPhase::Capture:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                script.captureComplete = harness.forwardBottleEvidence().captureComplete;
                break;
            case ev::RunPhase::Push:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                // The push objective the vehicle layer believes this cycle, from the
                // production derivation: the execution plan's forward stop, not the
                // Bottle Line rule landmark. Asking the harness for it keeps this
                // script from carrying a second copy of the threshold.
                script.pushComplete = harness.forwardBottleEvidence().pushComplete;
                break;
            case ev::RunPhase::ForwardDecel:
            case ev::RunPhase::BrakeZeroSpeed:
                // The encoder holds: the stop evidence is earned from observed
                // non-change, exactly as the production estimator reports it.
                break;
            case ev::RunPhase::Reverse:
            case ev::RunPhase::FinalApproach:
                script.encoderAdvanceTicks = -evscenario::kFixtureTicksPerCycle;
                break;
            case ev::RunPhase::FinalBrake:
            case ev::RunPhase::Done:
            case ev::RunPhase::Fault:
                break;
        }
        return script;
    }

    uint32_t cyclesInPhase() const { return cyclesInPhase_; }

private:
    ev::RunPhase lastPhase_ = ev::RunPhase::Idle;
    uint32_t cyclesInPhase_ = 0U;
};

// Steps the harness with the nominal evidence until the run reaches `target`.
// Returns false when the run faults or the budget runs out, so a case that
// expects a phase is never satisfied by an unrelated state.
bool driveToPhase(ScenarioHarness& harness, ev::RunPhase target, NominalDriver& driver,
                  uint32_t maxCycles = 3000U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        if (harness.phase() == target) {
            return true;
        }
        if (harness.step(driver.next(harness)).phase == ev::RunPhase::Fault) {
            return false;
        }
    }
    return harness.phase() == target;
}

// Steps the nominal evidence until the boundary authorized a powered command.
// The first cycles of a leg are not powered while the shaped target is still
// ramping up from zero, so a case that asserts a drive steps to one rather than
// assuming which cycle it appears in.
bool stepUntilDriveAuthorized(ScenarioHarness& harness, NominalDriver& driver,
                              uint32_t maxCycles = 300U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        if (harness.step(driver.next(harness)).commandedClass == ev::ActuationClass::Drive) {
            return true;
        }
    }
    return false;
}

// The same, for a specific direction.
bool stepUntilDirectionAuthorized(ScenarioHarness& harness, NominalDriver& driver,
                                  ev::PoweredDirection direction, uint32_t maxCycles = 300U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        if (harness.step(driver.next(harness)).commandedDirection == direction) {
            return true;
        }
    }
    return false;
}

// Steps the same stated evidence until the run faults, and returns the cycle the
// fault was latched in (or the last cycle when the budget runs out).
CycleRecord starveUntilFault(ScenarioHarness& harness, const CycleScript& starved,
                             uint32_t maxCycles = 2000U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        const CycleRecord& record = harness.step(starved);
        if (record.phase == ev::RunPhase::Fault) {
            return record;
        }
    }
    return harness.last();
}

// The nominal evidence, stepped until the run faults, for a harness whose encoder
// is driven by an attached plant (S18B). The plant ignores the script's prescribed
// advance, so this is the same driver against output-coupled movement.
CycleRecord plantedDriveUntilFault(ScenarioHarness& harness, NominalDriver& driver,
                                   uint32_t maxCycles = 3000U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        const CycleRecord& record = harness.step(driver.next(harness));
        if (record.phase == ev::RunPhase::Fault) {
            return record;
        }
    }
    return harness.last();
}

// The distinct phases of a trace, in the order they were first entered.
std::vector<ev::RunPhase> phaseSequence(const ScenarioHarness& harness) {
    std::vector<ev::RunPhase> sequence;
    for (const CycleRecord& record : harness.trace()) {
        if (sequence.empty() || sequence.back() != record.phase) {
            sequence.push_back(record.phase);
        }
    }
    return sequence;
}

void expectPhaseSequence(const ScenarioHarness& harness, const ev::RunPhase* expected,
                         size_t expectedCount) {
    const std::vector<ev::RunPhase> observed = phaseSequence(harness);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(expectedCount),
                                     static_cast<uint32_t>(observed.size()),
                                     "the trace entered a phase the scenario does not expect");
    for (size_t index = 0U; index < expectedCount && index < observed.size(); ++index) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected[index]),
                                      static_cast<int>(observed[index]),
                                      phaseName(expected[index]));
    }
}

// --- Architecture properties, asserted over whole traces --------------------

// Exactly one request is presented per cycle and exactly one command reaches the
// actuator boundary per cycle. There is no cycle with two verdicts and none with
// none.
void expectOneRequestPerCycle(const ScenarioHarness& harness) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(harness.actuatorRecords().size()),
                                     harness.cycles(),
                                     "every cycle must present exactly one request");
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_TRUE_MESSAGE(record.intention.emittedAtMs == record.nowMs,
                                 "a run intention was presented for a cycle it was not made in");
    }
}

// Nothing powered reaches the boundary without every precondition holding for
// that same cycle, and a non-powered command never carries effort or direction.
void expectEveryCommandWasAuthorizedForThatCycle(const ScenarioHarness& harness) {
    for (const CycleRecord& record : harness.trace()) {
        // The freshness gate is the vehicle layer's, so this is its enforcement:
        // an intention that is not current for the cycle it is presented in may
        // never carry a powered request, whatever the request's source.
        if (!record.intentionCurrent) {
            TEST_ASSERT_FALSE_MESSAGE(ev::isDrivingMode(record.presentedEffort.mode),
                                      "a stale intention carried a powered request");
        }
        if (record.commandedClass == ev::ActuationClass::Drive) {
            TEST_ASSERT_TRUE_MESSAGE(record.authorityArmed, "a drive without an armed authority");
            TEST_ASSERT_TRUE_MESSAGE(record.readinessComplete, "a drive without complete readiness");
            TEST_ASSERT_TRUE_MESSAGE(record.requiredDataFresh, "a drive on stale required data");
            TEST_ASSERT_TRUE_MESSAGE(record.intentionCurrent, "a drive on a stale run intention");
            TEST_ASSERT_TRUE_MESSAGE(ev::isRunTravelCommand(record.command),
                                     "a drive the run did not ask for");
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                          static_cast<int>(record.phase), "a drive during a fault");
            TEST_ASSERT_TRUE_MESSAGE(
                record.outcome == ev::AuthzOutcome::AuthorizedDrive ||
                    record.outcome == ev::AuthzOutcome::AuthorizedClamped,
                "a drive the boundary did not authorize");
            TEST_ASSERT_TRUE(record.commandedDirection != ev::PoweredDirection::None);
            TEST_ASSERT_TRUE(record.commandedPermille > 0U);
        } else {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, record.commandedPermille,
                                             "a non-powered command carried effort");
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::None),
                                          static_cast<int>(record.commandedDirection),
                                          "a non-powered command carried a direction");
        }
    }
}

// Every powered command is delivered in the direction the run's own leg names.
// This is the property that keeps a travel request arriving during a direction
// change from being powered in the direction the run has already left: the
// profile ramps the previous direction down before it flips the sign, and the
// authority, whose latch the verified stop legitimately cleared, would otherwise
// have nothing to refuse that effort with.
void expectEveryDriveFollowsTheRunLeg(const ScenarioHarness& harness) {
    for (const CycleRecord& record : harness.trace()) {
        if (record.commandedClass != ev::ActuationClass::Drive) {
            continue;
        }
        const ev::PoweredDirection leg = ev::runCommandNamesForwardLeg(record.command)
                                             ? ev::PoweredDirection::Forward
                                             : ev::PoweredDirection::Reverse;
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(leg),
                                      static_cast<int>(record.commandedDirection),
                                      "a drive in a direction the run is not travelling");
    }
}

// A powered direction change is only ever reached through a deliberate
// non-powered stop presented while the run reported a verified zero, which is
// what clears the authority's direction memory.
void expectNoPoweredReversalWithoutAVerifiedStop(const ScenarioHarness& harness) {
    ev::PoweredDirection previous = ev::PoweredDirection::None;
    for (const CycleRecord& record : harness.trace()) {
        // The audit starts over at Idle/ParameterValid records, where a reset
        // boundary is visible in the trace. Note this is a trace-model
        // convenience, not a memory fact: since S18A/P2 the authority's
        // direction barrier survives a reset, so a new run's opposite-direction
        // drive is refused until a fresh stop - and a same-direction or
        // discharged first drive leaves no change for this audit to check.
        if (record.phase == ev::RunPhase::Idle || record.phase == ev::RunPhase::ParameterValid) {
            previous = ev::PoweredDirection::None;
        }
        if (record.commandedClass != ev::ActuationClass::Drive) {
            continue;
        }
        if (previous != ev::PoweredDirection::None &&
            record.commandedDirection != previous) {
            TEST_ASSERT_TRUE_MESSAGE(
                record.stopCyclesAtZeroSinceLastDrive >= 1U,
                "a powered reversal without a presented stop at a reported zero");
            TEST_ASSERT_TRUE_MESSAGE(record.verifiedStopSinceLastDrive,
                                     "a powered reversal without a verified stop");
        }
        previous = record.commandedDirection;
    }
}

// Fault and Done are safe: a faulted cycle can only carry the fault policy's
// stop, Done never asks for travel, and neither ever drives.
void expectTerminalPhasesAreSafe(const ScenarioHarness& harness) {
    for (const CycleRecord& record : harness.trace()) {
        if (record.phase == ev::RunPhase::Fault) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::MotorMode::Disabled),
                                          static_cast<int>(record.commandedMode),
                                          "a faulted cycle must carry the fault stop");
            TEST_ASSERT_FALSE(ev::isRunTravelCommand(record.command));
        }
        if (record.phase == ev::RunPhase::Done) {
            TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(record.command),
                                      "Done asked for travel");
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                          static_cast<int>(record.commandedClass),
                                          "Done drove");
        }
    }
}

void expectTraceInvariants(const ScenarioHarness& harness) {
    expectOneRequestPerCycle(harness);
    expectEveryCommandWasAuthorizedForThatCycle(harness);
    expectEveryDriveFollowsTheRunLeg(harness);
    expectNoPoweredReversalWithoutAVerifiedStop(harness);
    expectTerminalPhasesAreSafe(harness);
}

// Announces a valid run and arms the chain, which is the order a run needs: no
// run is announced before the arming decision, so no cycle can be driven on an
// announcement that was never made.
void announceAndArm(ScenarioHarness& harness) {
    const ev::RunConfig config = evscenario::validRunConfig();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(harness.announce(config)));
    TEST_ASSERT_TRUE(harness.arm());
}

}  // namespace

// --- The nominal full scenario ----------------------------------------------

void test_the_nominal_scenario_reaches_done_through_the_real_chain(void) {
    ScenarioHarness harness(fixtures());

    // Nothing is announced yet, so nothing may arm and no run may exist.
    TEST_ASSERT_FALSE(harness.configurationBuildChecksPass());
    TEST_ASSERT_FALSE(harness.arm());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));

    announceAndArm(harness);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::ParameterValid),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_TRUE(harness.controllerEnabled());

    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Done, driver),
                             "the nominal scenario did not reach Done");

    const ev::RunPhase expected[] = {
        ev::RunPhase::Armed,        ev::RunPhase::OptionalDelay, ev::RunPhase::Approach,
        ev::RunPhase::Capture,      ev::RunPhase::Push,          ev::RunPhase::ForwardDecel,
        ev::RunPhase::BrakeZeroSpeed, ev::RunPhase::Reverse,     ev::RunPhase::FinalApproach,
        ev::RunPhase::FinalBrake,   ev::RunPhase::Done};
    expectPhaseSequence(harness, expected, sizeof(expected) / sizeof(expected[0]));

    TEST_ASSERT_FALSE(harness.decoderStepGuardTripped());
    TEST_ASSERT_EQUAL_UINT32(0U, harness.droppedSamples());
    expectTraceInvariants(harness);

    // The scenario is shaped as intended: the whole run - 6.0 m out, a reversal,
    // and 1.0 m back - costs a few hundred twenty-millisecond control cycles, so
    // it stays inside the announced 10.0 s Target Time without any phase being
    // starved. A change to the fixture geometry that broke the shape would show
    // up here rather than as a subtly different trace.
    TEST_ASSERT_TRUE_MESSAGE(harness.cycles() >= 330U && harness.cycles() <= 430U,
                             "the nominal scenario's scripted shape changed");

    // Done is a held, verified stop: no travel is ever asked for again.
    TEST_ASSERT_TRUE(harness.stopVerified());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));

    // The run axis really moved, through the production odometry: the vehicle is
    // back at the Target Point within the resolution of one control cycle.
    TEST_ASSERT_TRUE(std::fabs(harness.runAxisPositionM() - harness.targetPointPositionM()) <=
                     0.02);
    TEST_ASSERT_TRUE(harness.last().remainingToTargetM <= 0.0);

    // The boundary saw exactly the direction sequence the event has: forward to
    // the bottle, a stop, then reverse back to the Target Point.
    bool sawForward = false;
    bool sawReverse = false;
    ev::PoweredDirection order[2] = {ev::PoweredDirection::None, ev::PoweredDirection::None};
    size_t orderCount = 0U;
    for (const evscenario::ActuatorRecord& record : harness.actuatorRecords()) {
        if (record.actuationClass != ev::ActuationClass::Drive) {
            continue;
        }
        sawForward = sawForward || record.poweredDirection == ev::PoweredDirection::Forward;
        sawReverse = sawReverse || record.poweredDirection == ev::PoweredDirection::Reverse;
        if (orderCount < 2U &&
            (orderCount == 0U || order[orderCount - 1U] != record.poweredDirection)) {
            order[orderCount++] = record.poweredDirection;
        }
    }
    TEST_ASSERT_TRUE(sawForward);
    TEST_ASSERT_TRUE(sawReverse);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(order[0]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(order[1]));
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(orderCount));
}

void test_the_nominal_trace_replays_identically(void) {
    // Two independent harnesses, the same stated evidence: every recorded
    // decision must match, because nothing in the chain is random and no module
    // reads a clock of its own.
    const auto run = [](std::vector<CycleRecord>& out) {
        ScenarioHarness harness(fixtures());
        announceAndArm(harness);
        NominalDriver driver;
        TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
        out = harness.trace();
    };

    std::vector<CycleRecord> first;
    std::vector<CycleRecord> second;
    run(first);
    run(second);

    TEST_ASSERT_TRUE(first.size() > 100U);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(first.size()),
                             static_cast<uint32_t>(second.size()));
    for (size_t index = 0U; index < first.size(); ++index) {
        TEST_ASSERT_EQUAL_UINT32(first[index].nowMs, second[index].nowMs);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[index].phase),
                                      static_cast<int>(second[index].phase), "phase diverged");
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[index].command),
                                      static_cast<int>(second[index].command), "command diverged");
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[index].reversalPhase),
                                      static_cast<int>(second[index].reversalPhase),
                                      "reversal evidence diverged");
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[index].commandedMode),
                                      static_cast<int>(second[index].commandedMode),
                                      "authorized mode diverged");
        TEST_ASSERT_EQUAL_UINT16(first[index].commandedPermille, second[index].commandedPermille);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[index].outcome),
                                      static_cast<int>(second[index].outcome),
                                      "authorization outcome diverged");
        TEST_ASSERT_TRUE(first[index].runAxisPositionM == second[index].runAxisPositionM);
    }
}

// --- The step evidence the chain runs on ------------------------------------

void test_the_stop_evidence_packet_is_composed_from_two_production_answers(void) {
    // The estimator never reports the verifier's three facts in one answer: a
    // window-derived number carries no claim, and the claim carries no number.
    const ev::Sourced<double> scale = measured(evscenario::kFixtureDistancePerTickM);
    ev::VelocityEstimator estimator(ev::VelocityEstimatorConfig{
        evscenario::kFixtureEstimatorWindowMs, evscenario::kFixtureEstimatorNoEdgeTimeoutMs});

    // Moving at the fixture rate, then the count stops changing.
    const auto ticksAt = [](uint32_t ms) -> int32_t {
        return ms < 100U ? static_cast<int32_t>(ms * 10U) : 1000;
    };
    ev::VelocityEstimate estimate = estimator.update(0U, 0, scale);
    bool sawValid = false;
    bool sawClaim = false;
    bool sawBoth = false;
    for (uint32_t ms = 20U; ms <= 400U; ms += 20U) {
        estimate = estimator.update(ms, ticksAt(ms), scale);
        sawValid = sawValid || estimate.valid;
        sawClaim = sawClaim || estimate.zeroSpeedClaimed;
        sawBoth = sawBoth || (estimate.valid && estimate.zeroSpeedClaimed);
    }
    TEST_ASSERT_TRUE(sawValid);
    TEST_ASSERT_TRUE(sawClaim);
    TEST_ASSERT_FALSE_MESSAGE(sawBoth,
                              "one estimator answer carried a window zero and the claim");

    // The composition is production (`ev::composeStopEvidence()`), and it needs
    // the packet's own longer limit: the zero is always produced before the claim
    // interval it is paired with, so a limit as short as the control path's can
    // never span both.
    const ev::NearZeroConfig stopPolicy = evscenario::fixtureStopConfig();
    const auto verify = [&](uint32_t packetMaxAgeMs) {
        ev::VelocityEstimator source(ev::VelocityEstimatorConfig{
            evscenario::kFixtureEstimatorWindowMs,
            evscenario::kFixtureEstimatorNoEdgeTimeoutMs});
        ev::NearZeroVerifier verifier(stopPolicy);
        ev::VelocityEstimate current = source.update(0U, 0, scale);
        ev::VelocityEstimate lastValid = current;
        bool hasLastValid = false;
        bool verified = false;
        for (uint32_t ms = 20U; ms <= 800U; ms += 20U) {
            current = source.update(ms, ticksAt(ms), scale);
            if (current.valid) {
                lastValid = current;
                hasLastValid = true;
            }
            const ev::NearZeroEvidence packet = ev::composeStopEvidence(
                current, true, lastValid, hasLastValid, ms, evscenario::kFixtureMaxSampleAgeMs,
                packetMaxAgeMs);
            if (verifier.update(ms, packet).verified) {
                verified = true;
            }
        }
        return verified;
    };
    TEST_ASSERT_TRUE_MESSAGE(verify(evscenario::kFixtureStopEvidenceMaxAgeMs),
                             "the composed packet must be able to earn a stop");
    TEST_ASSERT_FALSE_MESSAGE(verify(evscenario::kFixtureMaxSampleAgeMs),
                              "a control-path staleness limit must not span the stop dwell");

    // The configuration-build check agrees with that empirical result, so the
    // arm decision refuses the short limit instead of leaving a run that can only
    // time out.
    TEST_ASSERT_TRUE(ev::stopEvidenceLimitSpansDwell(evscenario::kFixtureStopEvidenceMaxAgeMs,
                                                     stopPolicy));
    TEST_ASSERT_FALSE(ev::stopEvidenceLimitSpansDwell(evscenario::kFixtureMaxSampleAgeMs,
                                                      stopPolicy));
}

void test_a_stale_run_intention_can_never_drive_the_chain(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    const CycleRecord& rolling = harness.step(driver.next(harness));
    TEST_ASSERT_TRUE(ev::isRunTravelCommand(rolling.command));
    TEST_ASSERT_TRUE(rolling.intentionCurrent);
    TEST_ASSERT_TRUE(ev::runIntentionIsCurrentFor(rolling.intention, rolling.nowMs));
    // The production gate is what the vehicle layer must ask, and it is current
    // for that one cycle only.
    TEST_ASSERT_FALSE(ev::runIntentionIsCurrentFor(
        rolling.intention, rolling.nowMs + evscenario::kFixtureControlPeriodMs));
    TEST_ASSERT_FALSE(ev::runIntentionIsCurrentFor(
        rolling.intention, rolling.nowMs - evscenario::kFixtureControlPeriodMs));
    // Every record in the trace carries an intention stamped with, and presented
    // in, its own cycle; `expectOneRequestPerCycle` asserts that over all of them.
    expectTraceInvariants(harness);
}

// --- Faults -----------------------------------------------------------------

void test_every_fault_identity_is_latched_in_every_phase_and_stops_the_boundary(void) {
    const ev::RunPhase phases[] = {
        ev::RunPhase::Idle,           ev::RunPhase::ParameterValid,
        ev::RunPhase::Armed,          ev::RunPhase::OptionalDelay,
        ev::RunPhase::Approach,       ev::RunPhase::Capture,
        ev::RunPhase::Push,           ev::RunPhase::ForwardDecel,
        ev::RunPhase::BrakeZeroSpeed, ev::RunPhase::Reverse,
        ev::RunPhase::FinalApproach,  ev::RunPhase::FinalBrake,
        ev::RunPhase::Done};

    // Every identity the system can observe, injected as the fault of record. The
    // identity is carried through to the run policy and to the authority
    // unchanged, and the response is the same stop for all of them: fault
    // identity must not silently decide the response.
    const ev::FaultCode identities[] = {
        ev::FaultCode::InvalidRunConfig,     ev::FaultCode::ProvisionalCalibration,
        ev::FaultCode::MissingSafetyLimits,  ev::FaultCode::InvalidMotorEffort,
        ev::FaultCode::PhaseTimeout,         ev::FaultCode::ReversalNotVerified,
        ev::FaultCode::DisarmedDuringRun,    ev::FaultCode::RunSequencingError,
        ev::FaultCode::RunDurationExceeded,  ev::FaultCode::TravelStalled,
        ev::FaultCode::EncoderFeedbackLost,
    };

    for (const ev::RunPhase phase : phases) {
        for (const ev::FaultCode identity : identities) {
            ScenarioHarness harness(fixtures());
            NominalDriver driver;

            if (phase != ev::RunPhase::Idle) {
                announceAndArm(harness);
                if (phase != ev::RunPhase::ParameterValid) {
                    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, phase, driver),
                                             phaseName(phase));
                }
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(phase),
                                          static_cast<int>(harness.phase()), phaseName(phase));

            CycleScript faulted = driver.next(harness);
            faulted.fault = identity;
            const CycleRecord& latched = harness.step(faulted);

            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                          static_cast<int>(latched.phase), phaseName(phase));
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(identity),
                                          static_cast<int>(latched.faultOfRecord),
                                          faultName(identity));
            TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(latched.command), phaseName(phase));
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::MotorMode::Disabled),
                                          static_cast<int>(latched.commandedMode),
                                          faultName(identity));
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, latched.commandedPermille, faultName(identity));
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::None),
                                          static_cast<int>(latched.commandedDirection),
                                          faultName(identity));
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                                          static_cast<int>(latched.outcome), faultName(identity));
            TEST_ASSERT_FALSE(harness.authority().isArmed());
            // The same identity reached the authority in the same cycle, which is
            // what dictated the response.
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(identity),
                                          static_cast<int>(harness.authority().latchedFault().code),
                                          faultName(identity));

            // Everything the run could ever be told, stated together, moves
            // nothing: the fault of record stands and no drive is asked for
            // again. Five cycles covers the timeout, the reversal and the final
            // stop bounds that a phase could otherwise be waiting on.
            CycleScript everything;
            everything.startTriggered = true;
            everything.approachComplete = true;
            everything.captureComplete = true;
            everything.pushComplete = true;
            everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
            for (uint32_t cycle = 0U; cycle < 5U; ++cycle) {
                const CycleRecord& later = harness.step(everything);
                TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                              static_cast<int>(later.phase), phaseName(phase));
                TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(identity),
                                              static_cast<int>(later.faultOfRecord),
                                              faultName(identity));
                TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::MotorMode::Disabled),
                                              static_cast<int>(later.commandedMode),
                                              faultName(identity));
            }

            expectTraceInvariants(harness);
        }
    }
}

void test_a_latched_fault_keeps_stopping_and_clearing_it_does_not_resume_the_run(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    CycleScript faulted = driver.next(harness);
    faulted.fault = ev::FaultCode::ProvisionalCalibration;
    const CycleRecord& latched = harness.step(faulted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(latched.phase));
    // The same observed fault reached the authority in the same cycle, which is
    // why the boundary's response is the fault policy's stop.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(harness.authority().latchedFault().code));
    TEST_ASSERT_FALSE(harness.authority().isArmed());

    // A later, different fault does not replace the fault of record.
    CycleScript other = driver.next(harness);
    other.fault = ev::FaultCode::InvalidRunConfig;
    const CycleRecord& later = harness.step(other);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(later.faultOfRecord));

    // Clearing the authority's latch ends the arming period and restores nothing:
    // the run is still faulted, so it still asks for nothing and the boundary
    // still sees a stop.
    TEST_ASSERT_TRUE(harness.authority().clearFault());
    TEST_ASSERT_FALSE(harness.authority().isArmed());
    TEST_ASSERT_TRUE(harness.arm());
    CycleScript everything;
    everything.startTriggered = true;
    everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    const CycleRecord& stillFaulted = harness.step(everything);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                  static_cast<int>(stillFaulted.phase),
                                  "clearing a fault resuming the run");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunMotionCommand::None),
                          static_cast<int>(stillFaulted.command));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(stillFaulted.commandedClass));

    // Only the whole-chain boundary leaves the fault, and it leaves the chain
    // disarmed and without an announced run.
    const ev::MotionChainResetReport report = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ProvisionalCalibration),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_TRUE(report.controllerEnabled);
    TEST_ASSERT_TRUE(harness.controllerEnabled());
    TEST_ASSERT_FALSE(harness.authority().isArmed());
    TEST_ASSERT_FALSE(harness.configurationBuildChecksPass());
    TEST_ASSERT_FALSE(harness.arm());
    expectTraceInvariants(harness);
}

void test_a_disarm_during_an_underway_run_stops_it(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Push, driver));

    // The authority is disarmed underneath a run that is asking for travel: the
    // run is stopped and latched rather than left asking for motion nothing can
    // be grounded in.
    CycleScript disarmed = driver.next(harness);
    harness.authority().disarm();
    const CycleRecord& record = harness.step(disarmed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(record.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::DisarmedDuringRun),
                          static_cast<int>(record.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(record.commandedClass));
    expectTraceInvariants(harness);
}

// --- Configuration ----------------------------------------------------------

void test_an_invalid_configuration_never_starts_a_run(void) {
    struct Case {
        ev::RunConfig config;
        ev::ConfigError expectedError;
        const char* name;
    };
    const Case cases[] = {
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.1, 10.0, 1.0, 2.0),
         ev::ConfigError::TargetDistanceOffInterval, "target distance off the grid"},
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 9.5, 10.0, 1.0, 2.0),
         ev::ConfigError::TargetDistanceOutOfRange, "target distance out of range"},
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.2, 1.0, 2.0),
         ev::ConfigError::TargetTimeOffInterval, "target time off the grid"},
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 0.5, 2.0),
         ev::ConfigError::BottleLineDistanceOutOfRange, "bottle line out of range"},
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 6.0),
         ev::ConfigError::BottleStartNotBeforeTarget, "bottle beyond the target point"},
        {ev::RunConfig(ev::TournamentLevel::State, 5.0, 10.0, 1.0, 2.0),
         ev::ConfigError::BottleStartNotLevelDistance, "another level's bottle placement"},
        {ev::RunConfig(ev::TournamentLevel::RegionalInvitational,
                       std::numeric_limits<double>::quiet_NaN(), 10.0, 1.0, 2.0),
         ev::ConfigError::NonFiniteValue, "non-finite target distance"},
    };

    for (const Case& testCase : cases) {
        ScenarioHarness harness(fixtures());
        const ev::RunConfigValidation validation = ev::validateRunConfig(testCase.config);
        TEST_ASSERT_FALSE_MESSAGE(validation.ok, testCase.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(testCase.expectedError),
                                      static_cast<int>(validation.error), testCase.name);

        TEST_ASSERT_EQUAL_INT_MESSAGE(
            static_cast<int>(ev::RunStartStatus::ConfigInvalid),
            static_cast<int>(harness.announce(testCase.config)), testCase.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::InvalidRunConfig),
                                      static_cast<int>(harness.faultCode()), testCase.name);
        TEST_ASSERT_FALSE_MESSAGE(harness.configurationValid(), testCase.name);
        TEST_ASSERT_FALSE_MESSAGE(harness.arm(), testCase.name);

        // Every piece of evidence a valid run would need, for long enough to
        // cover every phase bound: nothing starts, and nothing drives.
        CycleScript everything;
        everything.startTriggered = true;
        everything.approachComplete = true;
        everything.captureComplete = true;
        everything.pushComplete = true;
        everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
        for (uint32_t cycle = 0U; cycle < 300U; ++cycle) {
            const CycleRecord& record = harness.step(everything);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                          static_cast<int>(record.phase), testCase.name);
            TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(record.command), testCase.name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::MotorMode::Disabled),
                                          static_cast<int>(record.commandedMode), testCase.name);
        }
        for (const evscenario::ActuatorRecord& record : harness.actuatorRecords()) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                          static_cast<int>(record.actuationClass), testCase.name);
        }
        expectTraceInvariants(harness);
    }
}

void test_a_run_configuration_cannot_be_changed_at_an_illegal_time(void) {
    // A second, genuinely different but valid announcement: 5.25 m and 1.25 m at
    // the Regional grid, so its Bottle Line landmark is 6.50 m instead of 6.00 m.
    const ev::RunConfig first = evscenario::validRunConfig();
    const ev::RunConfig second(ev::TournamentLevel::RegionalInvitational, 5.25, 10.0, 1.25, 2.0);
    TEST_ASSERT_TRUE(ev::validateRunConfig(second).ok);

    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    const double firstBottleLine = harness.bottleLinePositionM();

    // While the run is announced but not started...
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::RunAlreadyActive),
                          static_cast<int>(harness.announce(second)));
    TEST_ASSERT_TRUE(harness.bottleLinePositionM() == firstBottleLine);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::ParameterValid),
                          static_cast<int>(harness.phase()));

    // ...while it is armed...
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Armed, driver));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::RunAlreadyActive),
                          static_cast<int>(harness.announce(second)));
    TEST_ASSERT_TRUE(harness.bottleLinePositionM() == firstBottleLine);

    // ...and while it is underway. The refusal is not a fault and does not
    // disturb the run that already has the announcement.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::RunAlreadyActive),
                          static_cast<int>(harness.announce(second)));
    TEST_ASSERT_TRUE(harness.bottleLinePositionM() == firstBottleLine);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));

    // ...including while the run is complete: Done is not an idle state.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::RunAlreadyActive),
                          static_cast<int>(harness.announce(second)));

    // Only a chain boundary accepts a new configuration, and the accepted one is
    // what the run layer then holds. The loop runs one idle cycle first, exactly
    // as the vehicle layer's loop would, and an idle run asks for nothing that
    // could be powered.
    harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));
    CycleScript idle;
    idle.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.step(idle).phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(harness.last().commandedMode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(harness.announce(second)));
    TEST_ASSERT_TRUE(harness.bottleLinePositionM() ==
                     second.targetDistanceM + second.bottleLineDistanceM);
    TEST_ASSERT_TRUE(second.targetDistanceM + second.bottleLineDistanceM != firstBottleLine);

    // While a fault is of record, no announcement is even considered.
    TEST_ASSERT_TRUE(harness.arm());
    NominalDriver secondDriver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, secondDriver));
    CycleScript faulted = secondDriver.next(harness);
    faulted.fault = ev::FaultCode::MissingSafetyLimits;
    harness.step(faulted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::FaultLatched),
                          static_cast<int>(harness.announce(first)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::MissingSafetyLimits),
                          static_cast<int>(harness.faultCode()));
    expectTraceInvariants(harness);
}

// --- Sensor and evidence staleness ------------------------------------------

void test_a_sensor_dropout_denies_drive_and_the_run_keeps_asking(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    // One healthy cycle first: the same request is authorized while the data is
    // fresh, so the refusal below is about the data and not about the request.
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    TEST_ASSERT_TRUE(harness.last().requiredDataFresh);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(harness.last().commandedClass));

    // Now the encoder stops delivering samples: the last one ages out, and the
    // run's request - which does not change - is refused at the boundary.
    CycleScript dropout = driver.next(harness);
    dropout.sensorSampleFresh = false;
    bool sawStaleCycle = false;
    for (uint32_t cycle = 0U; cycle < 12U; ++cycle) {
        const CycleRecord& record = harness.step(dropout);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                              static_cast<int>(record.phase));
        TEST_ASSERT_TRUE(ev::isRunTravelCommand(record.command));
        // The sample is still fresh for the first cycles of the dropout; from the
        // cycle its age passes the limit, the same unchanged request is refused.
        if (!record.requiredDataFresh) {
            sawStaleCycle = true;
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedStaleData),
                                  static_cast<int>(record.outcome));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                                  static_cast<int>(record.commandedMode));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sawStaleCycle, "the dropped sample never aged out");

    // The run's own bound then ends it: the phase never gets its evidence, and a
    // phase that overran its bound is not the run that was announced.
    const CycleRecord faulted = starveUntilFault(harness, dropout);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_TRUE(harness.droppedSamples() >= 10U);
    expectTraceInvariants(harness);
}

void test_stale_odometry_never_ends_the_reverse_leg_early(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Reverse, driver));

    // The reverse travel is the run's one legal direction change, and it was
    // authorized after the run itself presented the stop cycle.
    TEST_ASSERT_TRUE_MESSAGE(stepUntilDirectionAuthorized(harness, driver,
                                                          ev::PoweredDirection::Reverse),
                             "the reverse leg never presented a powered request");
    TEST_ASSERT_TRUE(harness.last().requiredDataFresh);

    // Then the odometry sample stops being refreshed. The leg's distance can no
    // longer be acted on, so the run must not end the leg - the number it last
    // saw is not evidence about where the vehicle is now.
    CycleScript dropout = driver.next(harness);
    dropout.sensorSampleFresh = false;
    bool sawStaleCycle = false;
    for (uint32_t cycle = 0U; cycle < 12U; ++cycle) {
        const CycleRecord& record = harness.step(dropout);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Reverse),
                              static_cast<int>(record.phase));
        if (!record.odometryUsable) {
            sawStaleCycle = true;
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedStaleData),
                                  static_cast<int>(record.outcome));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                                  static_cast<int>(record.commandedMode));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sawStaleCycle, "the dropped sample never aged out");

    const CycleRecord faulted = starveUntilFault(harness, dropout);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    // It never reached the target point on evidence it did not have.
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::FinalApproach),
                              static_cast<int>(record.phase));
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::FinalBrake),
                              static_cast<int>(record.phase));
    }
    expectTraceInvariants(harness);
}

void test_stale_stop_evidence_never_earns_the_reversal(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::ForwardDecel, driver));

    // The stop evidence stops arriving while the reversal is being sequenced: the
    // near-zero interval cannot accumulate, so the direction change is never
    // earned and the sequence times out instead.
    CycleScript dropout;
    dropout.sensorSampleFresh = false;
    const CycleRecord faulted = starveUntilFault(harness, dropout);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ReversalNotVerified),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_FALSE(faulted.stopVerified);
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_FALSE(record.stopVerified);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::PoweredDirection::Reverse),
                                      static_cast<int>(record.commandedDirection),
                                      "a reversal was permitted on stale evidence");
    }
    expectTraceInvariants(harness);
}

// --- Phase bounds and unverified zero speed ---------------------------------

void test_a_phase_that_never_gets_its_evidence_times_out(void) {
    struct Case {
        ev::RunPhase phase;
        CycleScript starved;
        const char* name;
        ev::FaultCode expectedFault;
    };

    // Never stop reporting earlier evidence, only never provide the evidence the
    // phase is waiting for.
    const auto rolling = []() {
        CycleScript script;
        script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
        return script;
    };
    CycleScript captureStarved = rolling();
    captureStarved.approachComplete = true;
    CycleScript pushStarved = rolling();
    pushStarved.approachComplete = true;
    pushStarved.captureComplete = true;

    // No contact, no capture, no completed push, no closing distance, no target
    // point and no verified final stop. Cases with a moving encoder wait under
    // the phase's own bound; cases with a frozen encoder under authorized
    // powered travel now end at the feedback-progress bound (S18C/P4) with its
    // own identity instead of waiting out the phase. Every one of these cases
    // is outside the leg's brake offset, so the run's brake-entry
    // travel-progress bound (S18B) does not apply to any of them.
    const Case cases[] = {
        {ev::RunPhase::Approach, rolling(), "Approach without contact",
         ev::FaultCode::PhaseTimeout},
        {ev::RunPhase::Capture, captureStarved, "Capture without engagement",
         ev::FaultCode::PhaseTimeout},
        {ev::RunPhase::Push, pushStarved, "Push without completion",
         ev::FaultCode::PhaseTimeout},
        // A vehicle that never moves off the bottle line: the reverse leg never
        // closes its distance, and the frozen counts under authorized reverse
        // drive fault the feedback bound rather than the phase bound.
        {ev::RunPhase::Reverse, CycleScript(), "Reverse without movement",
         ev::FaultCode::EncoderFeedbackLost},
        {ev::RunPhase::FinalApproach, CycleScript(),
         "FinalApproach without reaching the target point",
         ev::FaultCode::EncoderFeedbackLost},
        // A final stop that is never verified: the encoder keeps moving, so the
        // explicit zero-speed claim is never earned.
        {ev::RunPhase::FinalBrake, rolling(), "FinalBrake without a verified stop",
         ev::FaultCode::PhaseTimeout},
    };

    for (const Case& testCase : cases) {
        ScenarioHarness harness(fixtures());
        announceAndArm(harness);
        NominalDriver driver;
        TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, testCase.phase, driver), testCase.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(testCase.phase),
                                      static_cast<int>(harness.phase()), testCase.name);

        const CycleRecord faulted = starveUntilFault(harness, testCase.starved, 3000U);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                      static_cast<int>(faulted.phase), testCase.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(testCase.expectedFault),
                                      static_cast<int>(faulted.faultOfRecord), testCase.name);
        expectTraceInvariants(harness);
    }
}

// The run-scoped maximum-duration bound is global, not per phase. With the run's
// validated maximum set far below every phase bound, a run that stalls must end
// with `RunDurationExceeded`, not with the `PhaseTimeout` that would eventually
// answer the same stall - and the vehicle must be stopped by the authority's own
// fault response, exactly as for any other observed fault.
void test_a_run_that_overruns_its_validated_maximum_faults_on_its_own_identity(void) {
    evscenario::ScenarioFixtures overrun = fixtures();
    // A **measured** 1.0 s maximum: twenty times shorter than the fixture's 5.0 s
    // approach bound, so the two cannot be confused about which one fired.
    overrun.limits.maxRunDurationS = ev::Sourced<double>(1.0, ev::ValueSource::Measured);
    ScenarioHarness harness(overrun);
    announceAndArm(harness);

    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run did not reach Approach inside its maximum");
    const uint32_t cyclesWhenApproaching = harness.cycles();

    // The encoder keeps moving, but Approach's own completion is never stated, so
    // only the global bound can end this run.
    CycleScript rolling;
    rolling.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    const CycleRecord faulted = starveUntilFault(harness, rolling, 3000U);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(faulted.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::RunDurationExceeded),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    // The stop is the authority's own single fault response: nothing powered, no
    // direction, no effort.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(faulted.commandedClass));
    TEST_ASSERT_EQUAL_UINT16(0U, faulted.commandedPermille);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(faulted.commandedDirection));
    // It fired long before the 5.0 s (250-cycle) approach bound could have, which
    // is exactly the independence the bound claims.
    TEST_ASSERT_TRUE_MESSAGE(harness.cycles() - cyclesWhenApproaching < 100U,
                             "the duration bound did not beat the phase bound");
    expectTraceInvariants(harness);
}

void test_a_vehicle_that_never_comes_near_zero_faults_instead_of_reversing(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::ForwardDecel, driver));

    // The encoder keeps moving: there is no near zero and no zero-speed claim, so
    // the reversal has nothing to verify and its single bound has to end it.
    CycleScript stillMoving;
    stillMoving.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;

    CycleRecord last = harness.step(stillMoving);
    for (uint32_t cycle = 0U; cycle < 200U && last.phase != ev::RunPhase::Fault; ++cycle) {
        TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(last.command),
                                  "a powered reversal before the stop was verified");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                      static_cast<int>(last.commandedClass),
                                      "a powered command during the ramp-down");
        TEST_ASSERT_FALSE(last.stopVerified);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::ReversalPhase::RampingDown),
                                      static_cast<int>(last.reversalPhase),
                                      "the reversal must report that it is still ramping down");
        last = harness.step(stillMoving);
    }

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault), static_cast<int>(last.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::ReversalNotVerified),
                          static_cast<int>(last.faultOfRecord));
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Reverse),
                                      static_cast<int>(record.phase),
                                      "the run reversed without a verified stop");
    }
    expectTraceInvariants(harness);
}

void test_a_vehicle_that_never_moves_again_never_finishes_the_reverse_leg(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Reverse, driver));

    // The reverse travel is asked for and authorized, but the encoder never
    // moves: the frozen counts under authorized drive fault the
    // feedback-progress bound (S18C/P4) with its own identity rather than
    // waiting out the leg's own bound. (The reverse leg is still outside its
    // brake offset here, so the brake-entry travel-progress bound does not
    // apply; the feedback bound is the one that observes frozen counts under
    // power anywhere.)
    CycleScript stuck;
    const CycleRecord faulted = starveUntilFault(harness, stuck, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    for (const CycleRecord& record : harness.trace()) {
        if (record.phase == ev::RunPhase::Reverse) {
            TEST_ASSERT_TRUE(record.odometryUsable);
        }
    }
    expectTraceInvariants(harness);
}

// --- The authority boundary against a hostile caller ------------------------

void test_a_direct_reverse_request_is_refused_and_the_run_reverses_legally(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    const CycleRecord forward = harness.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(forward.commandedDirection));

    // An out-of-band probe at the boundary, deliberately outside the harness's
    // own per-cycle presentation: a caller that ignores the run and asks for
    // reverse while forward is latched. It is refused even with a reported zero,
    // and the refusal changes nothing.
    ev::SafetySystemState probe;
    probe.readiness.configurationValid = true;
    probe.readiness.calibrationUsable = true;
    probe.readiness.limitsUsable = true;
    probe.requiredDataFresh = true;
    probe.atZeroSpeed = true;
    probe.nowMs = forward.nowMs;
    probe.detectedFault = ev::FaultCode::None;
    const ev::AuthorizationResult denied = harness.authority().authorize(
        ev::MotorEffort(ev::MotorMode::DriveReverse, 300U), probe, harness.fixtures().limits);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(denied.outcome));
    TEST_ASSERT_FALSE(denied.command.isDriving());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));

    // The run's own reversal still runs the full legal path - ramp down, stop at a
    // verified zero, then reverse - because a refused flip cannot open the door
    // and cannot close it either.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    bool reversedAfterTheProbe = false;
    for (const CycleRecord& record : harness.trace()) {
        if (record.nowMs > forward.nowMs &&
            record.commandedDirection == ev::PoweredDirection::Reverse) {
            reversedAfterTheProbe = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(reversedAfterTheProbe,
                             "the run's own legal reversal did not happen");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Done),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

void test_a_fault_overrides_a_travel_request_in_the_same_cycle(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    // A fault observed in the same cycle the run asks for travel: the run latches
    // it before anything is acted on, and the authority's latched fault dictates
    // the response whatever request arrives.
    CycleScript faulted = driver.next(harness);
    faulted.fault = ev::FaultCode::MissingSafetyLimits;
    const CycleRecord& record = harness.step(faulted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(record.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(record.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(record.commandedMode));
    TEST_ASSERT_FALSE(ev::isDrivingMode(record.commandedMode));
    expectTraceInvariants(harness);
}

// --- Reset, rearm and a second run ------------------------------------------

void test_a_chain_reset_returns_to_nothing_and_a_second_run_works(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    const uint32_t firstRunCycles = harness.cycles();

    const ev::MotionChainResetReport report = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Done),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_TRUE(report.controllerEnabled);

    // Nothing survived: no run, no announcement, no armed authority, no target,
    // and no direction memory.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_FALSE(harness.authority().isArmed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));
    TEST_ASSERT_TRUE(harness.profileTarget().targetVelocityMps() == 0.0);
    TEST_ASSERT_FALSE(harness.configurationBuildChecksPass());
    TEST_ASSERT_FALSE(harness.arm());

    // The first cycle after the boundary is unpowered: an idle run asks for
    // nothing, and nothing is what reaches the actuator.
    CycleScript everything;
    everything.startTriggered = true;
    everything.approachComplete = true;
    everything.captureComplete = true;
    everything.pushComplete = true;
    everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    const CycleRecord& idle = harness.step(everything);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle), static_cast<int>(idle.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunMotionCommand::None),
                          static_cast<int>(idle.command));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(idle.commandedMode));

    // A new run is a fresh announcement and a fresh arming decision, and it
    // reaches Done through the same phases.
    announceAndArm(harness);
    NominalDriver secondDriver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, secondDriver));
    TEST_ASSERT_TRUE(harness.cycles() > firstRunCycles);
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

void test_fresh_motion_before_the_run_handoff_aborts_the_reversal(void) {
    // S18A/P1 through the real chain: the reversal is earned on observed
    // stillness, then the encoder moves again before the handoff consumes the
    // permission. The run must hold the deliberate stop instead of flipping to
    // reverse travel, and re-earn the stop before handing off.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::BrakeZeroSpeed, driver));

    // Let the earned permission arrive on stillness.
    bool earned = false;
    for (uint32_t cycle = 0U; cycle < 60U && !earned; ++cycle) {
        earned = harness.step(driver.next(harness)).reversalPhase ==
                 ev::ReversalPhase::DirectionChangeReady;
    }
    TEST_ASSERT_TRUE_MESSAGE(earned, "the reversal was never earned on stillness");
    TEST_ASSERT_TRUE(harness.stopVerified());

    // Fresh motion before the handoff consumes it: the run holds the stop.
    CycleScript moving;
    moving.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    const CycleRecord& held = harness.step(moving);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::BrakeZeroSpeed),
                          static_cast<int>(held.phase));
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(held.command));
    TEST_ASSERT_FALSE(held.commandedDirection == ev::PoweredDirection::Reverse &&
                      held.commandedClass == ev::ActuationClass::Drive);
    TEST_ASSERT_FALSE(held.stopVerified);

    // Stillness re-earns the stop and the run hands off to reverse travel.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Reverse, driver));
    TEST_ASSERT_TRUE(stepUntilDirectionAuthorized(harness, driver,
                                                  ev::PoweredDirection::Reverse));
    expectTraceInvariants(harness);
}

void test_a_mid_run_reset_bars_the_opposite_direction_until_a_fresh_stop(void) {
    // S18A/P2 through the real chain and the shared new-run reset: reverse
    // travel latches Reverse, a mid-run reset preserves the barrier, the fresh
    // forward run is refused without motion, and a disarmed observed standstill
    // discharges it - never the reset itself.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Reverse, driver));
    TEST_ASSERT_TRUE(stepUntilDirectionAuthorized(harness, driver,
                                                  ev::PoweredDirection::Reverse));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));

    const ev::MotionChainResetReport report = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Reverse),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_FALSE(harness.authority().isArmed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_FALSE(harness.stopVerified());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));

    // A fresh announcement arms, but the opposite direction is refused: the
    // reset proved no standstill.
    announceAndArm(harness);
    CycleScript hostile = driver.next(harness);
    hostile.hasHostileRequest = true;
    hostile.hostileRequest = ev::MotorEffort(ev::MotorMode::DriveForward, 300U);
    const CycleRecord& refused = harness.step(hostile);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(refused.outcome));
    TEST_ASSERT_FALSE(ev::isDrivingMode(refused.commandedMode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Reverse),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));

    // The run's own forward travel is refused the same way: no motion, no
    // fault, the run simply cannot take powered forward motion on a barrier.
    NominalDriver secondDriver;
    bool forwardDrive = false;
    for (uint32_t cycle = 0U; cycle < 40U && !forwardDrive; ++cycle) {
        forwardDrive = harness.step(secondDriver.next(harness)).commandedDirection ==
                       ev::PoweredDirection::Forward;
    }
    TEST_ASSERT_FALSE_MESSAGE(forwardDrive,
                              "forward drive authorized on a reset-preserved reverse barrier");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));

    // Disarming a run ends it by fault - the documented precedence - and the
    // disarmed standstill that follows is what discharges the barrier through
    // the authority's zero-speed path: observed evidence, never the reset.
    harness.authority().disarm();
    CycleScript still;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(harness.step(still).phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::DisarmedDuringRun),
                          static_cast<int>(harness.faultCode()));
    bool discharged = false;
    for (uint32_t cycle = 0U; cycle < 40U && !discharged; ++cycle) {
        harness.step(still);
        discharged = harness.authority().lastAuthorizedDirection() ==
                     ev::PoweredDirection::None;
    }
    TEST_ASSERT_TRUE_MESSAGE(discharged,
                             "a fresh disarmed standstill never discharged the barrier");

    // A fresh run after the discharge travels forward again. One Idle cycle
    // makes the reset boundary visible in the trace, the way the audit's
    // run-boundary model expects.
    const ev::MotionChainResetReport secondReset = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(secondReset.clearedRunPhase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));
    CycleScript idle;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.step(idle).phase));
    announceAndArm(harness);
    TEST_ASSERT_TRUE(
        stepUntilDirectionAuthorized(harness, secondDriver, ev::PoweredDirection::Forward));
    expectTraceInvariants(harness);
}

// --- A stale intention at the boundary, and a hostile request -----------------

void test_a_held_stale_intention_is_presented_as_no_powered_request(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));

    // One cycle that really was authorized, and whose intention is therefore one
    // the run asked for while it was current.
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    const CycleRecord& authorized = harness.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(authorized.commandedDirection));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunMotionCommand::DriveForward),
                          static_cast<int>(authorized.command));
    const ev::RunMotionIntention held = authorized.intention;
    TEST_ASSERT_TRUE(ev::runIntentionIsCurrentFor(held, held.emittedAtMs));

    // The caller keeps that intention and presents it again, cycle after cycle,
    // while the run itself keeps asking for the approach. The run's own request
    // would have been powered; the held one may not be, and it must not become
    // powered just because the chain is otherwise ready to drive.
    for (uint32_t cycle = 0U; cycle < 10U; ++cycle) {
        CycleScript stale = driver.next(harness);
        stale.heldIntention = &held;
        const CycleRecord& record = harness.step(stale);

        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(held.command),
                                      static_cast<int>(record.presentedIntention.command),
                                      "the scenario did not present the held intention");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(held.emittedAtMs,
                                         record.presentedIntention.emittedAtMs,
                                         "the presented intention was restamped");
        TEST_ASSERT_FALSE_MESSAGE(record.intentionCurrent,
                                  "an intention from an earlier cycle read as current");
        TEST_ASSERT_FALSE_MESSAGE(ev::runIntentionIsCurrentFor(held, record.nowMs),
                                  "the production gate accepted a held intention");
        TEST_ASSERT_FALSE(ev::isDrivingMode(record.presentedEffort.mode));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, record.presentedEffort.magnitudePermille,
                                         "a stale intention carried effort");
        TEST_ASSERT_FALSE_MESSAGE(record.regulatorStepped,
                                  "the regulator was stepped for a stale intention");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                      static_cast<int>(record.commandedClass),
                                      "a stale intention reached a powered command");
        TEST_ASSERT_EQUAL_UINT16(0U, record.commandedPermille);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                              static_cast<int>(record.commandedDirection));

        // The run's own request is untouched by the stale one: it is still the
        // travel the run is asking for, still current, and still in the phase
        // that asked for it.
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                              static_cast<int>(record.phase));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunMotionCommand::DriveForward),
                              static_cast<int>(record.command));
        TEST_ASSERT_EQUAL_UINT32(record.nowMs, record.intention.emittedAtMs);
    }

    // And the chain was never wedged: the run's own current intention drives
    // again as soon as the caller stops presenting the held one.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

void test_a_hostile_reverse_request_is_refused_by_the_authority(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(harness.last().commandedDirection));

    // A deliberate bypass of the vehicle layer's own gates: the scenario states
    // the request instead of letting the run present one, so the authority's
    // independent direction memory is what answers a forward-to-reverse flip. The
    // run is otherwise ready to drive, so nothing but the refusal can explain the
    // stop.
    CycleScript hostile = driver.next(harness);
    hostile.hasHostileRequest = true;
    hostile.hostileRequest = ev::MotorEffort(ev::MotorMode::DriveReverse, 300U);
    const CycleRecord& refused = harness.step(hostile);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(refused.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(refused.commandedMode));
    TEST_ASSERT_EQUAL_UINT16(0U, refused.commandedPermille);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(refused.commandedDirection));
    TEST_ASSERT_FALSE(ev::isDrivingMode(refused.commandedMode));
    // The refusal changed nothing: the forward latch stands, the run is still
    // approaching and no fault was raised.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                          static_cast<int>(refused.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(refused.faultOfRecord));
    TEST_ASSERT_FALSE(refused.atZeroSpeed);

    // The run's own next request is still authorized forward, and the run still
    // completes - a refused flip cannot open the reversal door, and cannot close
    // the run either.
    TEST_ASSERT_TRUE(stepUntilDirectionAuthorized(harness, driver,
                                                  ev::PoweredDirection::Forward));
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

// --- Configuration-build facts an arm decision must refuse -------------------

// Each variant keeps the announced RunConfig valid and breaks exactly one
// configuration-build fact. `configurationBuildChecksPass()` is the vehicle
// layer's pre-arm gate, and `arm()` must refuse every one of these: a valid
// announcement is not by itself a usable chain, and the refusal must be visible
// before any run can be driven.
struct BrokenConfiguration {
    const char* name;
    void (*breakIt)(evscenario::ScenarioFixtures&);
};

void breakCalibration(evscenario::ScenarioFixtures& broken) {
    broken.calibration.distancePerEncoderTickM = ev::Sourced<double>(
        evscenario::kFixtureDistancePerTickM, ev::ValueSource::Provisional);
}

void breakSafetyLimits(evscenario::ScenarioFixtures& broken) {
    broken.limits.minBatteryV = ev::Sourced<double>(9.0, ev::ValueSource::Provisional);
}

void breakRegulatorCeiling(evscenario::ScenarioFixtures& broken) {
    broken.controller.maxEffortPermille = ev::Sourced<uint16_t>(
        evscenario::kFixtureMaxEffortPermille, ev::ValueSource::Provisional);
}

void breakProfileLimit(evscenario::ScenarioFixtures& broken) {
    broken.profile.forward.cruiseVelocityMps = ev::Sourced<double>(
        evscenario::kFixtureCruiseMps, ev::ValueSource::Provisional);
}

void breakStopEvidenceLimit(evscenario::ScenarioFixtures& broken) {
    // Shorter than the stop dwell the packet has to span: the claim is earned
    // after the dwell, so a limit this short can never pair the two facts and the
    // run could only ever time out waiting for a stop.
    broken.stopEvidenceMaxAgeMs = evscenario::kFixtureControlPeriodMs;
}

void breakCeilingAgreement(evscenario::ScenarioFixtures& broken) {
    broken.limits.maxEffortPermille = ev::Sourced<uint16_t>(
        static_cast<uint16_t>(evscenario::kFixtureMaxEffortPermille - 100U),
        ev::ValueSource::Measured);
}

void breakCommandEffortPolarity(evscenario::ScenarioFixtures& broken) {
    // The measured tick fact moves with the command fact, so only the
    // command/effort agreement is broken and the tick agreement still holds.
    const ev::Sourced<ev::MotionDirection> reversed =
        evscenario::measuredPositiveDirection(ev::MotionDirection::Reverse);
    broken.positiveTicksDirection = reversed;
    broken.profile.forward.positiveCommandDirection = reversed;
    broken.profile.reverse.positiveCommandDirection = reversed;
}

void breakTickPolarity(evscenario::ScenarioFixtures& broken) {
    broken.positiveTicksDirection =
        evscenario::measuredPositiveDirection(ev::MotionDirection::Reverse);
}

void test_an_incomplete_configuration_cannot_arm_the_chain(void) {
    const BrokenConfiguration cases[] = {
        {"a provisional distance per tick", breakCalibration},
        {"a provisional safety limit", breakSafetyLimits},
        {"a provisional regulator ceiling", breakRegulatorCeiling},
        {"a provisional profile limit", breakProfileLimit},
        {"a stop-evidence limit shorter than the dwell", breakStopEvidenceLimit},
        {"safety and regulator ceilings that disagree", breakCeilingAgreement},
        {"a command polarity opposite the effort polarity", breakCommandEffortPolarity},
        {"a tick polarity opposite the command polarity", breakTickPolarity},
    };

    for (const BrokenConfiguration& broken : cases) {
        evscenario::ScenarioFixtures variants = evscenario::nominalFixtures();
        broken.breakIt(variants);
        ScenarioHarness harness(variants);

        // The single announced RunConfig is valid: the refusal below is about the
        // chain's own configuration, not about the event's parameters.
        TEST_ASSERT_TRUE_MESSAGE(ev::validateRunConfig(evscenario::validRunConfig()).ok,
                                 broken.name);
        TEST_ASSERT_FALSE_MESSAGE(harness.configurationBuildChecksPass(), broken.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            static_cast<int>(ev::RunStartStatus::Started),
            static_cast<int>(harness.announce(evscenario::validRunConfig())), broken.name);
        TEST_ASSERT_FALSE_MESSAGE(harness.arm(), broken.name);
        TEST_ASSERT_FALSE_MESSAGE(harness.authority().isArmed(), broken.name);

        // Every piece of evidence a valid run would need, for well past the point
        // at which a valid chain would have left the announcement phase. Nothing
        // may drive on an incomplete configuration, and the run cannot even leave
        // `ParameterValid`, because leaving it requires the armed fact.
        CycleScript everything;
        everything.startTriggered = true;
        everything.approachComplete = true;
        everything.captureComplete = true;
        everything.pushComplete = true;
        everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
        for (uint32_t cycle = 0U; cycle < 60U; ++cycle) {
            const CycleRecord& record = harness.step(everything);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::ParameterValid),
                                          static_cast<int>(record.phase), broken.name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunMotionCommand::None),
                                          static_cast<int>(record.command), broken.name);
            TEST_ASSERT_FALSE_MESSAGE(record.authorityArmed, broken.name);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                          static_cast<int>(record.commandedClass),
                                          broken.name);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, record.commandedPermille, broken.name);
        }
        expectTraceInvariants(harness);
    }
}

// --- Disarm in the reversal phases -------------------------------------------

void test_a_disarm_in_every_reversal_phase_stops_the_run(void) {
    // The phases around the one legal direction change: the deliberate stop that
    // earns it, the two legs it joins, and the final hold. A disarm in any of
    // them must stop the run and latch, never leave it asking for motion the
    // authority is refusing.
    const ev::RunPhase reversalPhases[] = {
        ev::RunPhase::ForwardDecel, ev::RunPhase::BrakeZeroSpeed, ev::RunPhase::Reverse,
        ev::RunPhase::FinalBrake,
    };

    for (const ev::RunPhase phase : reversalPhases) {
        ScenarioHarness harness(fixtures());
        announceAndArm(harness);
        NominalDriver driver;
        TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, phase, driver), phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(phase),
                                      static_cast<int>(harness.phase()), phaseName(phase));

        harness.authority().disarm();
        const CycleRecord& record = harness.step(driver.next(harness));

        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                      static_cast<int>(record.phase), phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::DisarmedDuringRun),
                                      static_cast<int>(record.faultOfRecord), phaseName(phase));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                      static_cast<int>(record.commandedClass), phaseName(phase));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, record.commandedPermille, phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::None),
                                      static_cast<int>(record.commandedDirection),
                                      phaseName(phase));
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                                      static_cast<int>(record.outcome), phaseName(phase));
        TEST_ASSERT_FALSE(harness.authority().isArmed());

        // The latch holds: five cycles of every evidence the run could want move
        // nothing.
        CycleScript everything;
        everything.startTriggered = true;
        everything.approachComplete = true;
        everything.captureComplete = true;
        everything.pushComplete = true;
        everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
        for (uint32_t cycle = 0U; cycle < 5U; ++cycle) {
            const CycleRecord& later = harness.step(everything);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                          static_cast<int>(later.phase), phaseName(phase));
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, later.commandedPermille, phaseName(phase));
        }
        expectTraceInvariants(harness);
    }
}

// --- Clock evidence ----------------------------------------------------------

void test_a_stalled_clock_is_not_speed_evidence_and_spends_no_phase_time(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));

    const uint32_t frozenMs = harness.nowMs();
    const uint32_t rejectedBefore = harness.estimator().rejectedStamps();
    uint32_t phaseElapsedAtStall = 0U;
    bool havePhaseElapsed = false;
    bool sawDriveDuringStall = false;

    // Twelve cycles at the same clock reading. The samples arrive and the
    // position stays fresh; the speed estimate cannot exist, because nothing can
    // be derived over an interval of zero. A stalled reading must not be turned
    // into a velocity, and it must not consume the phase's time budget either.
    for (uint32_t cycle = 0U; cycle < 12U; ++cycle) {
        CycleScript stalled = driver.next(harness);
        stalled.scriptedClock = true;
        stalled.clockAdvanceMs = 0;
        const CycleRecord& record = harness.step(stalled);

        TEST_ASSERT_EQUAL_UINT32(frozenMs, record.nowMs);
        TEST_ASSERT_EQUAL_UINT32(0U, record.dtMs);
        TEST_ASSERT_FALSE(record.velocityUsable);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityEstimateStatus::TimeDidNotAdvance),
                              static_cast<int>(record.velocityStatus));
        TEST_ASSERT_TRUE(record.measuredVelocityMps == 0.0);
        if (record.regulatorStepped) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::VelocityControlStatus::TimeDidNotAdvance),
                                  static_cast<int>(record.controlStatus));
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Approach),
                                      static_cast<int>(record.phase),
                                      "a stalled clock faulted the run");
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                              static_cast<int>(record.faultOfRecord));
        TEST_ASSERT_TRUE(ev::isRunTravelCommand(record.command));
        if (!havePhaseElapsed) {
            phaseElapsedAtStall = record.phaseElapsedMs;
            havePhaseElapsed = true;
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(
                phaseElapsedAtStall, record.phaseElapsedMs,
                "a stalled clock grew the run's phase elapsed time");
        }
        TEST_ASSERT_TRUE(std::isfinite(record.runAxisPositionM));
        TEST_ASSERT_TRUE(record.commandedPermille <= evscenario::kFixtureMaxEffortPermille);
        sawDriveDuringStall = sawDriveDuringStall ||
                              record.commandedClass == ev::ActuationClass::Drive;
    }

    TEST_ASSERT_TRUE_MESSAGE(sawDriveDuringStall,
                             "the stalled clock wedged the chain instead of refusing the speed");
    TEST_ASSERT_EQUAL_UINT32(rejectedBefore + 12U, harness.estimator().rejectedStamps());

    // An advancing reading is accepted again, and the run finishes normally: the
    // refusal was about the interval, not a latched state.
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

// A control loop that is not a metronome: the same average period as the fixture,
// with the steps uneven. Stamps are strictly increasing, so nothing here is a
// stall or a rewind - only the interval varies.
const int32_t kJitteredClockAdvanceMs[4] = {15, 25, 12, 28};

void test_an_uneven_clock_does_not_disturb_the_run(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;

    uint32_t previousMs = harness.nowMs();
    bool sawDrive = false;
    for (uint32_t cycle = 0U; cycle < 3000U && harness.phase() != ev::RunPhase::Done; ++cycle) {
        CycleScript jittered = driver.next(harness);
        const int32_t advanceMs = kJitteredClockAdvanceMs[cycle % 4U];
        jittered.scriptedClock = true;
        jittered.clockAdvanceMs = advanceMs;
        const CycleRecord& record = harness.step(jittered);

        TEST_ASSERT_TRUE_MESSAGE(record.nowMs > previousMs, "a stamp repeated or rewound");
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(advanceMs), record.dtMs);
        previousMs = record.nowMs;
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::None),
                                      static_cast<int>(record.faultOfRecord),
                                      "an uneven clock faulted the run");
        TEST_ASSERT_TRUE(std::isfinite(record.measuredVelocityMps));
        TEST_ASSERT_TRUE(std::fabs(record.measuredVelocityMps) < 1.5);
        TEST_ASSERT_TRUE(std::isfinite(record.runAxisPositionM));
        TEST_ASSERT_TRUE(record.commandedPermille <= evscenario::kFixtureMaxEffortPermille);
        sawDrive = sawDrive || record.commandedClass == ev::ActuationClass::Drive;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Done),
                                  static_cast<int>(harness.phase()),
                                  "the run did not finish on an uneven clock");
    TEST_ASSERT_TRUE(sawDrive);
    TEST_ASSERT_TRUE(harness.stopVerified());
    expectTraceInvariants(harness);
}

// S13R run-1 M-1 / P13: the clock discipline is unit-pinned per module, but no
// end-to-end scenario drove a run across the real uint32 wrap. This case starts
// the scripted clock 4096 ms below the wrap - the nominal run costs 6.6-8.6 s, so
// the crossing happens mid-travel - and asserts that a wrap is a wrap and not a
// reboot: every `dtMs` is still the control period (unsigned subtraction), no
// fault appears, the run stays armed through the crossing, and the run reaches
// its verified Done exactly as it does from an unwrapped clock.
void test_a_run_that_crosses_the_timestamp_wrap_is_undisturbed(void) {
    ScenarioHarness harness(fixtures());
    harness.primeClock(0xFFFFF000U);  // 4096 ms before the wrap
    announceAndArm(harness);
    NominalDriver driver;

    uint32_t previousMs = harness.nowMs();
    bool crossedWrap = false;
    bool armedThroughWrap = true;
    for (uint32_t cycle = 0U; cycle < 3000U && harness.phase() != ev::RunPhase::Done; ++cycle) {
        const CycleRecord& record = harness.step(driver.next(harness));

        // The wrap itself: the raw reading went backwards, and the elapsed
        // arithmetic must still be the control period.
        if (record.nowMs < previousMs) {
            crossedWrap = true;
        }
        previousMs = record.nowMs;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(fixtures().controlPeriodMs),
                                         record.dtMs, "the wrap produced a bogus interval");
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::None),
                                      static_cast<int>(record.faultOfRecord),
                                      "the wrap faulted the run");
        if (crossedWrap) {
            // A wrap is not a reboot: the authority is not reset and the run is
            // not re-armed by the clock; it is the same armed run across the
            // crossing.
            armedThroughWrap = armedThroughWrap && record.authorityArmed;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(crossedWrap, "the run finished before crossing the wrap");
    TEST_ASSERT_TRUE_MESSAGE(armedThroughWrap, "the wrap disarmed or re-armed the run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunPhase::Done),
                                  static_cast<int>(harness.phase()),
                                  "the run did not finish across the wrap");
    TEST_ASSERT_TRUE(harness.stopVerified());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));
    expectTraceInvariants(harness);
}

void test_a_rewound_clock_stops_the_run_and_the_chain_recovers(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    const uint32_t beforeRewindMs = harness.nowMs();

    // The reading goes backwards, the way a reset clock would - past the point in
    // this phase the run has already measured, so the run's own elapsed
    // measurement cannot be supported by it. The phase then reads as overrun and
    // the run stops and latches instead of trusting time that moved the wrong
    // way. No powered command may come of it.
    CycleScript rewound = driver.next(harness);
    rewound.scriptedClock = true;
    rewound.clockAdvanceMs =
        -static_cast<int32_t>(harness.last().phaseElapsedMs) -
        3 * static_cast<int32_t>(evscenario::kFixtureControlPeriodMs);
    const CycleRecord& refused = harness.step(rewound);

    TEST_ASSERT_TRUE(refused.nowMs < beforeRewindMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(refused.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(refused.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::ActuationClass::Drive),
                          static_cast<int>(refused.commandedClass));
    TEST_ASSERT_EQUAL_UINT16(0U, refused.commandedPermille);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(refused.outcome));
    TEST_ASSERT_FALSE(harness.authority().isArmed());

    CycleScript everything;
    everything.startTriggered = true;
    everything.approachComplete = true;
    everything.captureComplete = true;
    everything.pushComplete = true;
    everything.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    for (uint32_t cycle = 0U; cycle < 5U; ++cycle) {
        const CycleRecord& later = harness.step(everything);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                              static_cast<int>(later.phase));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                              static_cast<int>(later.faultOfRecord));
        TEST_ASSERT_EQUAL_UINT16(0U, later.commandedPermille);
    }

    // Only the whole-chain boundary leaves the fault, and it leaves nothing
    // behind: a second run is a fresh announcement, a fresh arming decision and a
    // fresh origin, and it reaches Done through the same phases.
    const ev::MotionChainResetReport report = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(report.clearedRunPhase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));
    TEST_ASSERT_FALSE(harness.authority().isArmed());
    TEST_ASSERT_FALSE(harness.configurationBuildChecksPass());
    TEST_ASSERT_FALSE(harness.arm());

    announceAndArm(harness);
    NominalDriver secondDriver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, secondDriver));
    TEST_ASSERT_TRUE(harness.stopVerified());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));
    expectTraceInvariants(harness);
}

// --- Raw encoder evidence ----------------------------------------------------

void test_the_production_decoder_decides_what_a_raw_sample_counts(void) {
    ScenarioHarness harness(fixtures());
    const auto feed = [&harness](bool aLevel, bool bLevel) -> const CycleRecord& {
        CycleScript raw;
        raw.hasRawLevels = true;
        raw.rawLevelA = aLevel;
        raw.rawLevelB = bLevel;
        return harness.step(raw);
    };

    // The reference sample was established at construction (both channels low),
    // and it never counts.
    TEST_ASSERT_EQUAL_INT32(0, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, harness.decoder().countedEdges());
    TEST_ASSERT_EQUAL_UINT32(0U, harness.decoder().ignoredUpdates());

    // A repeated sample: A did not change, so there is nothing to count. A
    // spurious or duplicated call must never move the count.
    feed(false, false);
    TEST_ASSERT_EQUAL_INT32(0, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(0U, harness.decoder().countedEdges());
    TEST_ASSERT_EQUAL_UINT32(1U, harness.decoder().ignoredUpdates());

    // The charging A edge lands in (1, 0).
    const CycleRecord& rising = feed(true, false);
    TEST_ASSERT_EQUAL_INT32(1, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(harness.decoder().countedEdges()));

    // A B-only sample - what a caller that also reports B transitions produces -
    // is ignored, not counted twice.
    feed(true, true);
    TEST_ASSERT_EQUAL_INT32(1, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(2U, harness.decoder().ignoredUpdates());

    // The falling A edge completes one Gray cycle: two counts per full cycle (x2
    // decoding), so a whole revolution cannot cancel to zero.
    feed(false, true);
    TEST_ASSERT_EQUAL_INT32(2, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(2U, harness.decoder().countedEdges());

    // A sample in which both channels changed at once is not a Gray step, and the
    // decoder still counts it once, from the state it landed in - the sign is a
    // function of the landing state, never of B alone.
    feed(true, false);
    TEST_ASSERT_EQUAL_INT32(3, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(3U, harness.decoder().countedEdges());
    feed(false, true);
    TEST_ASSERT_EQUAL_INT32(4, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(4U, harness.decoder().countedEdges());

    // Back to the start of the cycle with no A change: again nothing counted.
    feed(false, false);
    TEST_ASSERT_EQUAL_INT32(4, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(3U, harness.decoder().ignoredUpdates());

    // Two A edges that land in the equal-parity states count negative, so a
    // count that oscillates is visible: the signed sum is not the number of
    // edges, and both are reported.
    feed(true, true);
    TEST_ASSERT_EQUAL_INT32(3, harness.decoder().ticks());
    const CycleRecord& oscillating = feed(false, false);
    TEST_ASSERT_EQUAL_INT32(2, harness.decoder().ticks());
    TEST_ASSERT_EQUAL_UINT32(6U, harness.decoder().countedEdges());

    // Every record reports the decoder's own count, and the run axis is the
    // production conversion of it: no run is announced, so the axis origin is
    // zero and the fixture polarity maps positive ticks to forward travel.
    TEST_ASSERT_EQUAL_INT32(1, rising.encoderTicks);
    TEST_ASSERT_EQUAL_INT32(harness.decoder().ticks(), oscillating.encoderTicks);
    TEST_ASSERT_TRUE(oscillating.runAxisUsable);
    TEST_ASSERT_TRUE(std::fabs(oscillating.runAxisPositionM -
                               harness.decoder().ticks() *
                                   evscenario::kFixtureDistancePerTickM) <= 1e-12);
    TEST_ASSERT_FALSE(harness.decoderStepGuardTripped());
}

// --- An independent oracle for the run axis ----------------------------------

void test_the_run_axis_numbers_are_independent_of_the_harnesss_own_fields(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));

    // The announced geometry is the announced configuration - read here from the
    // numbers the event supplies, not from the harness's own geometry fields.
    const ev::RunConfig announced = evscenario::validRunConfig();
    TEST_ASSERT_TRUE(harness.targetPointPositionM() == announced.targetDistanceM);
    TEST_ASSERT_TRUE(harness.bottleStartPositionM() == announced.bottleStartDistanceM);
    TEST_ASSERT_TRUE(harness.bottleLinePositionM() ==
                     announced.targetDistanceM + announced.bottleLineDistanceM);

    // Every recorded position is the raw tick travel with the polarity applied
    // once - computed here from the decoder's own tick count and the origin the
    // chain took, not from the number the chain reported.
    const int32_t originTicks = harness.runOriginTicks();
    const double scaleM = evscenario::kFixtureDistancePerTickM;
    double previousForwardPositionM = -std::numeric_limits<double>::infinity();
    double previousReversePositionM = std::numeric_limits<double>::infinity();
    double previousReverseLegRemainingM = std::numeric_limits<double>::infinity();
    double previousReverseRemainingToTargetM = std::numeric_limits<double>::infinity();
    double firstReverseLegRemainingM = std::numeric_limits<double>::infinity();
    double lastReverseLegRemainingM = std::numeric_limits<double>::infinity();
    bool haveFirstReverseRemaining = false;
    bool sawForwardLeg = false;
    bool sawReverseLeg = false;
    bool sawReverseCycles = false;
    // The leg the run named in the previous record, which is the leg this
    // record's distance was computed on: the vehicle layer has to state a
    // remaining distance before the run has produced this cycle's command.
    bool previousLegForward = true;

    for (const CycleRecord& record : harness.trace()) {
        const double tickTravelM =
            static_cast<double>(static_cast<int64_t>(record.encoderTicks) -
                                static_cast<int64_t>(originTicks)) *
            scaleM;
        TEST_ASSERT_TRUE(std::fabs(record.runAxisPositionM - tickTravelM) <= 1e-9);

        // Numeric sanity over the whole run: nothing is a non-number, nothing
        // runs away, and no command exceeds the fixture ceiling.
        TEST_ASSERT_TRUE(std::isfinite(record.runAxisPositionM));
        TEST_ASSERT_TRUE(std::isfinite(record.remainingToTargetM));
        TEST_ASSERT_TRUE(std::isfinite(record.legRemainingM));
        TEST_ASSERT_TRUE(std::isfinite(record.measuredVelocityMps));
        TEST_ASSERT_TRUE(std::fabs(record.measuredVelocityMps) < 1.5);
        TEST_ASSERT_TRUE(record.runAxisPositionM >= -1.0);
        TEST_ASSERT_TRUE(record.runAxisPositionM <= 7.0);
        TEST_ASSERT_TRUE(record.commandedPermille <= evscenario::kFixtureMaxEffortPermille);

        // Each leg travels its own monotone way: the run's position never moves
        // against the leg its own command names.
        if (ev::isRunTravelCommand(record.command)) {
            if (ev::runCommandNamesForwardLeg(record.command)) {
                TEST_ASSERT_TRUE(record.runAxisPositionM >= previousForwardPositionM - 1e-12);
                previousForwardPositionM = record.runAxisPositionM;
                sawForwardLeg = true;
            } else {
                TEST_ASSERT_TRUE(record.runAxisPositionM <= previousReversePositionM + 1e-12);
                previousReversePositionM = record.runAxisPositionM;
                sawReverseLeg = true;
            }
        }

        // On the reverse leg the closing distance only ever closes. The cycle
        // that changes legs is skipped, because its distance was still computed
        // on the leg the run had named before the change; from the cycle after
        // it, the reverse leg's remaining distance is the reverse-leg one and
        // must only shrink - and the leg distance and the closing distance are
        // the same number there.
        if (record.phase == ev::RunPhase::Reverse && !previousLegForward) {
            sawReverseCycles = true;
            TEST_ASSERT_TRUE(record.legRemainingM <= previousReverseLegRemainingM + 1e-12);
            TEST_ASSERT_TRUE(record.remainingToTargetM <=
                             previousReverseRemainingToTargetM + 1e-12);
            TEST_ASSERT_TRUE(std::fabs(record.legRemainingM - record.remainingToTargetM) <= 1e-12);
            if (!haveFirstReverseRemaining) {
                firstReverseLegRemainingM = record.legRemainingM;
                haveFirstReverseRemaining = true;
            }
            lastReverseLegRemainingM = record.legRemainingM;
            previousReverseLegRemainingM = record.legRemainingM;
            previousReverseRemainingToTargetM = record.remainingToTargetM;
        }
        if (record.command != ev::RunMotionCommand::None) {
            previousLegForward = ev::runCommandNamesForwardLeg(record.command);
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(sawForwardLeg, "the forward leg never travelled");
    TEST_ASSERT_TRUE_MESSAGE(sawReverseLeg, "the reverse leg never travelled");
    TEST_ASSERT_TRUE(sawReverseCycles);
    // The reverse leg really closed its distance, and the run ended at the
    // Target Point with no leg distance left.
    TEST_ASSERT_TRUE(firstReverseLegRemainingM - lastReverseLegRemainingM >= 0.2);
    TEST_ASSERT_TRUE(harness.last().remainingToTargetM <= 0.0);
    TEST_ASSERT_TRUE(harness.last().legRemainingM == 0.0);
    TEST_ASSERT_TRUE(harness.last().runAxisPositionM <= harness.targetPointPositionM());

    // The same production conversion with the measured tick polarity on the other
    // side: the sign of the position follows the polarity fact, applied exactly
    // once, and the agreement check refuses to arm a chain whose command polarity
    // names the other direction. The announcement itself is still a valid
    // RunConfig, so the position can be observed without any run being driven.
    evscenario::ScenarioFixtures reversedFixtures = evscenario::nominalFixtures();
    reversedFixtures.positiveTicksDirection =
        evscenario::measuredPositiveDirection(ev::MotionDirection::Reverse);
    ScenarioHarness reversed(reversedFixtures);
    TEST_ASSERT_FALSE(reversed.configurationBuildChecksPass());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(reversed.announce(evscenario::validRunConfig())));
    TEST_ASSERT_FALSE(reversed.arm());
    TEST_ASSERT_FALSE(reversed.authority().isArmed());

    CycleScript forward;
    forward.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
    const CycleRecord& reversedPosition = reversed.step(forward);
    TEST_ASSERT_EQUAL_INT32(evscenario::kFixtureTicksPerCycle, reversed.decoder().ticks());
    TEST_ASSERT_TRUE(reversedPosition.runAxisUsable);
    TEST_ASSERT_TRUE(std::fabs(reversedPosition.runAxisPositionM +
                               evscenario::kFixtureTicksPerCycle *
                                   evscenario::kFixtureDistancePerTickM) <= 1e-12);
    TEST_ASSERT_TRUE(reversedPosition.positionUsable);
    expectTraceInvariants(reversed);
}

// --- S4: one forward execution target -----------------------------------------

void test_the_forward_stop_the_run_shapes_to_is_the_push_objective_it_completes_at(void) {
    // The run has ONE forward execution target. The travel taper is sized from it
    // (the leg distance) and the forward progression completes at it (the push
    // objective), so the run cannot end its push at one position while its own
    // shaping target is another. Both are read from the same execution plan value,
    // through production accessors - not from two formulas that happen to agree.
    ScenarioHarness harness(fixtures());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(harness.announce(evscenario::validRunConfig())));
    TEST_ASSERT_TRUE_MESSAGE(harness.executionPlanUsable(), "the fixture plan must be usable");
    TEST_ASSERT_TRUE(harness.arm());

    const double forwardStopM = harness.forwardStopMpPositionM();
    // The execution target is not the Bottle Line: the fixture's forward stop lies
    // beyond the rule landmark, by the measured margin the plan was built with.
    TEST_ASSERT_TRUE(forwardStopM > harness.bottleLinePositionM());
    // An explicit tolerance, not a double equality: the host target is 32-bit x86,
    // where the plan's stored sum and this recomputation can differ in the last
    // bits through excess precision.
    TEST_ASSERT_TRUE(std::fabs(forwardStopM - (harness.bottleLinePositionM() +
                                               evscenario::kFixtureForwardStopMarginBeyondBottleLineM)) <=
                     1e-9);
    // The plan's rule-derived push landmark is at or inside the forward stop, which
    // is what lets the forward phases complete there without shortchanging the
    // push objective.
    TEST_ASSERT_TRUE(harness.executionPlan().pushCompleteMpPositionM <= forwardStopM);

    // One stored value, read by both consumers.
    TEST_ASSERT_TRUE(harness.legExecutionTargetM().usable);
    TEST_ASSERT_TRUE(harness.legExecutionTargetM().positionM == forwardStopM);
    TEST_ASSERT_TRUE(harness.forwardBottleEvidence().pushCompletePositionM == forwardStopM);

    // Past the Bottle Line and short of the forward stop the run was still in
    // Push: the rule landmark did not end the push.
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Done, driver));
    bool sawPushBeyondTheBottleLine = false;
    for (const CycleRecord& record : harness.trace()) {
        if (record.phase == ev::RunPhase::Push &&
            record.runAxisPositionM >= harness.bottleLinePositionM() &&
            record.runAxisPositionM < forwardStopM) {
            sawPushBeyondTheBottleLine = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sawPushBeyondTheBottleLine,
                             "the push ended before the forward execution target");

    // The reverse leg's target is the announced Target Point, from the same plan -
    // unchanged by the forward leg's execution target becoming explicit.
    TEST_ASSERT_TRUE(harness.legExecutionTargetM().usable);
    TEST_ASSERT_TRUE(harness.legExecutionTargetM().positionM == harness.targetPointPositionM());
    TEST_ASSERT_TRUE(harness.executionPlan().reverseStopMpPositionM ==
                     harness.targetPointPositionM());
    expectTraceInvariants(harness);
}

// --- S5: truthful forward-phase progression -----------------------------------

void test_the_run_enters_capture_when_the_capture_window_opens_without_claiming_contact(void) {
    // End to end through the production chain: the run's forward progression is
    // produced by the current strategy - the run axis thresholded at the run's own
    // execution-plan landmarks - and what it reports is the truth about that
    // strategy. Approach completes when the credible capture window opens, which is
    // before the nominal contact position and before the capture guarantee, and
    // entering Capture is not a claim that the bottle was touched or captured.
    ScenarioHarness harness(fixtures());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(harness.announce(evscenario::validRunConfig())));
    TEST_ASSERT_TRUE_MESSAGE(harness.executionPlanUsable(), "the fixture plan must be usable");
    TEST_ASSERT_TRUE(harness.arm());

    const double windowOpenM = harness.executionPlan().earliestContactMpPositionM;
    const double captureGuaranteedM = harness.executionPlan().captureGuaranteedMpPositionM;
    TEST_ASSERT_TRUE(windowOpenM < captureGuaranteedM);
    // The threshold is the plan's own field, and the plan's window opening is before
    // the announced Bottle Start Point rule landmark: the progression is execution
    // geometry, not the rule landmark.
    TEST_ASSERT_TRUE(windowOpenM < harness.bottleStartPositionM());

    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Capture, driver),
                             "the run never reached Capture");

    // The cycle that entered Capture: the travel was at or past the window opening
    // and still short of the capture guarantee, and the production derivation at
    // that position agrees - approach complete, capture not.
    const CycleRecord& entered = harness.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Capture),
                          static_cast<int>(entered.phase));
    TEST_ASSERT_TRUE(entered.runAxisPositionM >= windowOpenM);
    TEST_ASSERT_TRUE(entered.runAxisPositionM < captureGuaranteedM);
    TEST_ASSERT_TRUE(entered.runAxisPositionM < harness.bottleStartPositionM());

    const ev::BottleEvidence evidence = harness.forwardBottleEvidence();
    TEST_ASSERT_TRUE(evidence.derived);
    TEST_ASSERT_TRUE(evidence.approachComplete);
    TEST_ASSERT_FALSE(evidence.captureComplete);
    TEST_ASSERT_FALSE(evidence.pushComplete);
    TEST_ASSERT_TRUE(evidence.approachCompletePositionM == windowOpenM);
    expectTraceInvariants(harness);
}

void test_a_capture_phase_ceiling_slows_only_capture_through_the_one_shaper(void) {
    // Two runs of identical stated evidence: the nominal fixtures, and a copy whose
    // Capture strategy ceiling is far below the fixture cruise. This is the
    // capability S6 adds - transit and capture speeds differ through the one target
    // shaper - with no second controller, no bypassed taper and no bypassed rate
    // limit. The clock, the encoder script and the progression evidence are the
    // same in both runs, so any difference in the trace is the ceiling's.
    evscenario::ScenarioFixtures slowCapture = evscenario::nominalFixtures();
    slowCapture.phase.captureCeilingMps = evscenario::measuredValue(0.30);

    ScenarioHarness nominal(fixtures());
    ScenarioHarness slowed(slowCapture);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(nominal.announce(evscenario::validRunConfig())));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunStartStatus::Started),
                          static_cast<int>(slowed.announce(evscenario::validRunConfig())));
    TEST_ASSERT_TRUE(nominal.arm());
    TEST_ASSERT_TRUE(slowed.arm());

    NominalDriver nominalDriver;
    NominalDriver slowedDriver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(nominal, ev::RunPhase::Push, nominalDriver),
                             "the nominal run never passed Capture");
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(slowed, ev::RunPhase::Push, slowedDriver),
                             "the slowed run never passed Capture");

    // The ceiling changes the shaped target, never the sequencing: same trace
    // length, same phase every cycle.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(nominal.trace().size()),
                          static_cast<int>(slowed.trace().size()));

    bool sawFirstCapture = false;
    double captureEntryDifferenceMps = 0.0;
    uint32_t captureCycles = 0U;
    uint32_t tightenedCaptureCycles = 0U;
    for (uint32_t index = 0U; index < static_cast<uint32_t>(nominal.trace().size()); ++index) {
        const CycleRecord& nominalRecord = nominal.trace()[index];
        const CycleRecord& slowedRecord = slowed.trace()[index];
        TEST_ASSERT_EQUAL_INT(static_cast<int>(nominalRecord.phase),
                              static_cast<int>(slowedRecord.phase));
        // Only ever tightens, in every phase and every cycle.
        TEST_ASSERT_TRUE(std::fabs(slowedRecord.shapedTargetVelocityMps) <=
                         std::fabs(nominalRecord.shapedTargetVelocityMps) + 1e-9);

        if (nominalRecord.phase == ev::RunPhase::Capture) {
            ++captureCycles;
            if (!sawFirstCapture) {
                sawFirstCapture = true;
                captureEntryDifferenceMps =
                    std::fabs(nominalRecord.shapedTargetVelocityMps) -
                    std::fabs(slowedRecord.shapedTargetVelocityMps);
            }
            if (std::fabs(slowedRecord.shapedTargetVelocityMps) <
                std::fabs(nominalRecord.shapedTargetVelocityMps) - 1e-9) {
                ++tightenedCaptureCycles;
            }
        }
    }

    // The capture window the fixture geometry gives is several cycles wide, the
    // ceiling tightened the shaped target in it, and the first cycle of Capture
    // was a deceleration-limited ramp away from the approach target - not a step.
    TEST_ASSERT_TRUE(captureCycles >= 5U);
    TEST_ASSERT_TRUE(tightenedCaptureCycles >= captureCycles - 1U);
    TEST_ASSERT_TRUE(captureEntryDifferenceMps > 0.0);
    TEST_ASSERT_TRUE(captureEntryDifferenceMps <= 0.04 + 1e-9);

    // And the slowed run reached Push: the ceiling delays a phase instead of
    // wedging the run.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Push),
                          static_cast<int>(slowed.phase()));
}

// --- Output-coupled liveness (S18B) -----------------------------------------
//
// These cases attach the synthetic plant so the encoder follows the authorized
// output, and assert what the composed production chain does once the commanded
// travel reaches brake entry - where the target shaper has reached zero inside
// the measured brake offset and the run expects the vehicle to coast the rest of
// the way to the landmark. The plant is a deterministic fixture (`ScenarioHarness`
// labels its limits); it is not a vehicle model, and no case here claims real
// stopping behavior.

void test_a_planted_coasting_run_reaches_done_through_the_real_chain(void) {
    // The vehicle has enough inertia to coast past the commanded stop point, so it
    // crosses the forward stop and the Target Point and the run completes - the
    // success path the brake-entry travel-progress bound must not break.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{0.15, 1.0, false});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;

    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Done, driver),
                             "the coasting planted run did not reach Done");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));
    TEST_ASSERT_TRUE(harness.stopVerified());
    // It really travelled, through the plant: forward past the execution target
    // and back to the Target Point (within the plant's own lag).
    double furthestM = 0.0;
    for (const CycleRecord& record : harness.trace()) {
        if (record.runAxisPositionM > furthestM) {
            furthestM = record.runAxisPositionM;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(furthestM > harness.forwardStopMpPositionM(),
                             "the coasting plant never crossed the forward stop");
    TEST_ASSERT_TRUE(std::fabs(harness.last().runAxisPositionM - harness.targetPointPositionM()) < 0.2);
    expectTraceInvariants(harness);
}

void test_a_planted_ideal_follower_stops_short_and_faults_with_its_own_identity(void) {
    // The ideal follower stops the instant the shaped target reaches zero, which
    // is inside the brake offset - short of the forward stop. The run must end
    // with `TravelStalled`, its own bounded identity, rather than present a zero
    // target for the whole push budget and end at `PhaseTimeout` - and it must
    // never claim the push completed or reverse on an unproven landmark.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{1.0, 1.0, false});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;

    const CycleRecord faulted = plantedDriveUntilFault(harness, driver, 4000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(faulted.phase));
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::TravelStalled),
                                  static_cast<int>(faulted.faultOfRecord),
                                  "a short-stop did not end with the travel-progress identity");
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    // The stop is the authority's own single fault response: nothing powered.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    TEST_ASSERT_EQUAL_UINT16(0U, faulted.commandedPermille);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(faulted.commandedDirection));
    // The fault is terminal and no false progression was ever claimed: the run
    // never left Push, never entered the reversal, reverse travel or Done.
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::ForwardDecel),
                                      static_cast<int>(record.phase),
                                      "the run reversed on a push it never completed");
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::Reverse),
                              static_cast<int>(record.phase));
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::Done),
                              static_cast<int>(record.phase));
    }
    // It really stopped short of the forward execution target.
    TEST_ASSERT_TRUE(harness.last().runAxisPositionM < harness.forwardStopMpPositionM());
    // The vehicle was commanded to travel and the plant responded; the fault is
    // about the missing coast, not about a vehicle that never moved.
    TEST_ASSERT_TRUE(plant.positionM() > 0.5);
    expectTraceInvariants(harness);
}

void test_a_planted_frozen_run_makes_no_progress_and_the_feedback_bound_ends_it(void) {
    // A vehicle that never moves under authorized powered travel is a lost
    // feedback fault, not a phase timeout: the feedback-progress bound
    // (S18C/P4) observes frozen counts under power anywhere, including outside
    // brake entry where the S18B travel-progress bound does not apply. This
    // supersedes the S18B-era expectation that the phase's own bound ends a
    // frozen run: S18B deliberately left frozen-encoder detection to S18C, and
    // S18C now detects it with its own identity. No landmark is claimed.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{1.0, 1.0, /*frozen=*/true});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;

    const CycleRecord faulted = plantedDriveUntilFault(harness, driver, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::TravelStalled),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    // The plant never moved, and the run never claimed a forward completion.
    TEST_ASSERT_TRUE(plant.positionM() == 0.0);
    TEST_ASSERT_TRUE(harness.last().runAxisPositionM < harness.forwardStopMpPositionM());
    expectTraceInvariants(harness);
}

void test_a_planted_overshooting_run_still_reaches_done(void) {
    // A controlled overshoot (the vehicle travels further than its command asks)
    // crosses the landmarks early and must still complete safely: the run is not
    // wedged by arriving past a target, and the final stop still verifies.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{0.08, 1.0, false});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;

    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Done, driver),
                             "the overshooting planted run did not reach Done");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));
    TEST_ASSERT_TRUE(harness.stopVerified());
    // It really overshot the forward execution target before coming back.
    double furthestM = 0.0;
    for (const CycleRecord& record : harness.trace()) {
        if (record.runAxisPositionM > furthestM) {
            furthestM = record.runAxisPositionM;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(furthestM > harness.forwardStopMpPositionM() + 0.05,
                             "the overshooting plant did not pass the forward stop");
    expectTraceInvariants(harness);
}

void test_a_planted_reverse_leg_that_stops_short_faults_with_its_own_identity(void) {
    // The reverse leg has the same contract as the forward leg: it must reach the
    // Target Point. Here the forward leg coasts (so the push and reversal
    // complete) and then the reverse leg becomes ideal, stopping short of the
    // Target Point inside the reverse brake offset. The run must end with
    // `TravelStalled` and must never claim the final stop or Done.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{0.15, 1.0, false});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;

    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Reverse, driver),
                             "the planted run never reached the reverse leg");
    plant.setConfig(PlantConfig{1.0, 1.0, false});

    const CycleRecord faulted = plantedDriveUntilFault(harness, driver, 2000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::TravelStalled),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    // It stopped short of the Target Point and never claimed the final stop.
    TEST_ASSERT_TRUE(harness.last().remainingToTargetM > 0.0);
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::FinalBrake),
                                      static_cast<int>(record.phase),
                                      "a short reverse leg claimed the final stop");
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::Done),
                              static_cast<int>(record.phase));
    }
    expectTraceInvariants(harness);
}

// --- The feedback-progress bound (S18C/P4) ----------------------------------
//
// Frozen, wrong-sign, or incoherent encoder counts under authorized powered
// travel fault `EncoderFeedbackLost` with the authority's single fault stop.
// Every count below is a synthetic host fixture, never a vehicle measurement;
// the suite proves the software response, not the physical timeout.

// Frozen forward counts under authorized drive fault with the run's own
// identity, a safe output, and no false progression. This is the committed
// regression for the October H7/H4 defect: 50 frozen cycles once sustained
// 50/50 powered forward requests with no fault (S18C-RESUME pre-repair
// reproduction); now the 26th powered cycle without an edge ends the run.
void test_frozen_forward_counts_fault_with_the_feedback_identity(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    // Moving history first: 20 cycles of real progress.
    for (uint32_t cycle = 0U; cycle < 20U; ++cycle) {
        CycleScript moving;
        moving.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
        harness.step(moving);
    }
    // Then frozen counts with fresh samples until the run faults.
    CycleScript frozen;
    const CycleRecord faulted = starveUntilFault(harness, frozen, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(faulted.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::TravelStalled),
                          static_cast<int>(faulted.faultOfRecord));
    // The stop is the authority's own single fault response.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    TEST_ASSERT_EQUAL_UINT16(0U, faulted.commandedPermille);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::None),
                          static_cast<int>(faulted.commandedDirection));
    // Never a forward completion, a reversal, or Done on a frozen run.
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Reverse),
                                      static_cast<int>(record.phase),
                                      "a frozen run reversed");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Done),
                                      static_cast<int>(record.phase),
                                      "a frozen run completed");
    }
    expectTraceInvariants(harness);
}

// The reverse leg has the same contract: frozen counts under authorized
// reverse drive fault with the same identity and never reach the Target
// Point or Done.
void test_frozen_reverse_counts_fault_with_the_feedback_identity(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Reverse, driver),
                             "the run never reached the reverse leg");
    CycleScript frozen;
    const CycleRecord faulted = starveUntilFault(harness, frozen, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Fault),
                          static_cast<int>(faulted.phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::FinalBrake),
                                      static_cast<int>(record.phase),
                                      "a frozen reverse leg claimed the final stop");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Done),
                                      static_cast<int>(record.phase),
                                      "a frozen reverse leg completed");
    }
    expectTraceInvariants(harness);
}

// Wrong-sign counts are not progress: backwards ticks under forward power
// never reset the interval and the run faults rather than driving on.
void test_wrong_sign_counts_under_forward_power_fault(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    CycleScript backwards;
    backwards.encoderAdvanceTicks = -evscenario::kFixtureTicksPerCycle;
    const CycleRecord faulted = starveUntilFault(harness, backwards, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    expectTraceInvariants(harness);
}

// Intermittent real progress keeps the run alive: one advancing cycle in
// twenty is slow but genuine, and the interval restarts on every edge.
void test_intermittent_progress_under_power_never_faults_the_feedback_bound(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        CycleScript script;
        script.encoderAdvanceTicks =
            (cycle % 20U == 19U) ? evscenario::kFixtureTicksPerCycle : 0;
        const CycleRecord& record = harness.step(script);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                                      static_cast<int>(record.faultOfRecord),
                                      "genuine intermittent progress faulted");
        if (record.phase == ev::RunPhase::Fault) {
            TEST_ASSERT_TRUE_MESSAGE(false, "no fault expected on intermittent progress");
        }
    }
    expectTraceInvariants(harness);
}

// A long sensor dropout pauses the feedback bound instead of faulting on
// evidence the run does not have: no sample is not a frozen count. The run
// keeps asking (its intention is unchanged) while the boundary denies the
// stale data, and the phase bound - not the feedback bound - remains the
// terminal outcome for that case.
void test_a_long_dropout_pauses_the_feedback_bound(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    CycleScript dropout;
    dropout.sensorSampleFresh = false;
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        const CycleRecord& record = harness.step(dropout);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                                      static_cast<int>(record.faultOfRecord),
                                      "a dropout is not a frozen count");
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                          static_cast<int>(harness.phase()));
    expectTraceInvariants(harness);
}

// An armed run that has not been triggered asks for no powered travel, so
// frozen counts there never fault: intentional stationary phases are exempt
// by construction, not by timeout.
void test_an_untriggered_armed_run_with_frozen_counts_never_faults(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    CycleScript frozen;
    // The first cycle carries the arming into the run policy; after that the
    // untriggered run waits in Armed with no powered request.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                          static_cast<int>(harness.step(frozen).phase));
    for (uint32_t cycle = 0U; cycle < 200U; ++cycle) {
        const CycleRecord& record = harness.step(frozen);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::None),
                                      static_cast<int>(record.faultOfRecord),
                                      "an untriggered run must not fault for standing still");
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                          static_cast<int>(harness.phase()));
    expectTraceInvariants(harness);
}

// The bound is wrap-safe: a run frozen across the `millis()` uint32 wrap
// faults with the same identity once its powered allowance expires.
void test_a_frozen_run_across_the_timestamp_wrap_faults(void) {
    ScenarioHarness harness(fixtures());
    harness.primeClock(0xFFFFFF00U);
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach across the wrap");
    CycleScript frozen;
    const CycleRecord faulted = starveUntilFault(harness, frozen, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    expectTraceInvariants(harness);
}

// A large scheduling gap without progress is not forgiven: one 5 s jump
// under power faults at once rather than granting a fresh allowance.
void test_a_large_scheduling_gap_without_progress_faults_at_once(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    CycleScript gap;
    gap.scriptedClock = true;
    gap.clockAdvanceMs = 5000;
    const CycleRecord faulted = starveUntilFault(harness, gap, 10U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    expectTraceInvariants(harness);
}

// A repeated timestamp accumulates no powered time: 100 frozen cycles at a
// stalled clock fault nothing, because a clock that never advances never
// consumes the allowance.
void test_a_stalled_clock_with_frozen_counts_faults_nothing(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    CycleScript stalled;
    stalled.scriptedClock = true;
    stalled.clockAdvanceMs = 0;
    for (uint32_t cycle = 0U; cycle < 100U; ++cycle) {
        const CycleRecord& record = harness.step(stalled);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::None),
                                      static_cast<int>(record.faultOfRecord),
                                      "a stalled clock must not consume the allowance");
    }
    expectTraceInvariants(harness);
}

// Fault, reset, and re-arm after lost feedback: the fault latches and stops
// the boundary, the reset clears the monitor without discharging the S18A
// direction barrier, and a fresh run with movement travels again.
void test_a_feedback_fault_latches_and_a_reset_preserves_the_barrier(void) {
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, driver),
                             "the run never reached Approach");
    TEST_ASSERT_TRUE_MESSAGE(stepUntilDirectionAuthorized(harness, driver,
                                                          ev::PoweredDirection::Forward),
                             "forward drive was never authorized");
    CycleScript frozen;
    const CycleRecord faulted = starveUntilFault(harness, frozen, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    TEST_ASSERT_FALSE(harness.authority().isArmed());

    // The reset clears the fault and the monitor but preserves the powered
    // direction the fault interrupted: software reset proves no stop.
    const ev::MotionChainResetReport report = harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(report.clearedFault));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::PoweredDirection::Forward),
                          static_cast<int>(harness.authority().lastAuthorizedDirection()));

    // A fresh announcement arms, but the opposite direction is still refused
    // until a fresh verified stop discharges it through the zero-speed path.
    announceAndArm(harness);
    CycleScript hostile = driver.next(harness);
    hostile.hasHostileRequest = true;
    hostile.hostileRequest = ev::MotorEffort(ev::MotorMode::DriveReverse, 300U);
    const CycleRecord& refused = harness.step(hostile);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::DeniedUnsafeDirectionChange),
                          static_cast<int>(refused.outcome));

    // And a fresh run with movement travels forward again: the cleared monitor
    // grants the full allowance anew rather than faulting on the old attempt.
    NominalDriver secondDriver;
    TEST_ASSERT_TRUE_MESSAGE(stepUntilDirectionAuthorized(harness, secondDriver,
                                                          ev::PoweredDirection::Forward),
                             "a fresh run with movement never re-authorized forward drive");
    expectTraceInvariants(harness);
}

// --- R2/S18R-M1: sustained-invalid-feedback composition ------------------------
//
// These cases run the full composed chain - decoder, odometry, estimator,
// run policy, shaper, regulator, safety boundary and the mock actuator
// boundary in the production stage order - with scripted invalid encoder
// observations. Every timeout below is a synthetic host-only fixture, never
// a physical claim: the 0.5 s feedback bound is unvalidated (P-H5/H7/H9),
// and a pass here says nothing about electrical disable timing, which no
// mocked I/O can observe.

void test_accepted_power_never_survives_stale_feedback(void) {
    // The S18R-M1 full-runtime case: once samples stop arriving during
    // authorized drive, accepted power must end within the freshness window
    // (60 ms, three control cycles) - revoked by the stale-data denial in
    // the same cycle the data goes stale - and the run must end at the
    // phase bound, never at the feedback bound: a dropout is not a freeze.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    const size_t dropoutStart = harness.trace().size();
    CycleScript dropout = driver.next(harness);
    dropout.sensorSampleFresh = false;
    const CycleRecord faulted = starveUntilFault(harness, dropout);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    uint32_t drivesAfterDropout = 0U;
    for (size_t i = dropoutStart; i < harness.trace().size(); ++i) {
        if (harness.trace()[i].commandedClass == ev::ActuationClass::Drive) {
            ++drivesAfterDropout;
        }
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                              static_cast<int>(harness.trace()[i].faultOfRecord));
    }
    TEST_ASSERT_TRUE_MESSAGE(drivesAfterDropout <= 3U,
                             "accepted power survived stale feedback");
    expectTraceInvariants(harness);
}

void test_a_feedback_fault_disables_at_the_actuator_in_the_same_cycle(void) {
    // Driver write ordering on the fault cycle: the authorized command the
    // fault produces is Disabled, and the mock actuator boundary receives
    // exactly that command - no powered write leaks past the fault, and no
    // drive is ever written again afterwards.
    ScenarioHarness harness(plantedFixtures());
    PlantModel plant(PlantConfig{1.0, 1.0, /*frozen=*/true});
    harness.attachPlant(plant);
    announceAndArm(harness);
    NominalDriver driver;
    const CycleRecord faulted = plantedDriveUntilFault(harness, driver, 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    TEST_ASSERT_EQUAL_UINT32(harness.trace().size(), harness.actuatorRecords().size());
    size_t faultIndex = harness.trace().size();
    for (size_t i = 0U; i < harness.trace().size(); ++i) {
        if (harness.trace()[i].faultOfRecord == ev::FaultCode::EncoderFeedbackLost) {
            faultIndex = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(faultIndex < harness.trace().size());
    for (size_t i = faultIndex; i < harness.actuatorRecords().size(); ++i) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                      static_cast<int>(harness.actuatorRecords()[i].actuationClass),
                                      "a drive reached the actuator at or after the fault");
    }
    expectTraceInvariants(harness);
}

void test_short_dropouts_separated_by_progress_never_fault(void) {
    // A temporarily missing sample is not a sustained invalid condition:
    // two dropped samples separated by healthy advancing cycles must keep
    // the run alive indefinitely.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    for (uint32_t round = 0U; round < 40U; ++round) {
        for (uint32_t cycle = 0U; cycle < 2U; ++cycle) {
            CycleScript dropout = driver.next(harness);
            dropout.sensorSampleFresh = false;
            harness.step(dropout);
        }
        for (uint32_t cycle = 0U; cycle < 4U; ++cycle) {
            harness.step(driver.next(harness));
        }
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                              static_cast<int>(harness.faultCode()));
    }
    expectTraceInvariants(harness);
}

void test_a_feedback_fault_yields_to_a_concurrent_driver_fault(void) {
    // Fault precedence at the composed layer: frozen counts under power with
    // a simultaneously observed driver fault latch the electrical fault, not
    // the feedback fault. Both answer with the same fault stop.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    TEST_ASSERT_TRUE(stepUntilDriveAuthorized(harness, driver));
    CycleScript frozen;
    frozen.fault = ev::FaultCode::DriverFaultBar;
    const CycleRecord faulted = starveUntilFault(harness, frozen, 500U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::DriverFaultBar),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AuthzOutcome::ForcedFaultStop),
                          static_cast<int>(faulted.outcome));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(faulted.commandedMode));
    expectTraceInvariants(harness);
}

void test_a_dropout_in_the_terminal_phase_ends_in_the_phase_timeout(void) {
    // Invalid feedback while terminal: the final stop can never verify on
    // evidence the run does not have, so the FinalBrake bound ends the run.
    // Nothing here claims the stop, and no feedback fault masks the wait.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::FinalBrake, driver),
                             "the nominal run never reached its terminal phase");
    CycleScript dropout;
    dropout.sensorSampleFresh = false;
    const CycleRecord faulted = starveUntilFault(harness, dropout);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(faulted.faultOfRecord));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::RunPhase::Done),
                            static_cast<int>(faulted.phase));
    for (const CycleRecord& record : harness.trace()) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                                      static_cast<int>(record.faultOfRecord),
                                      "a dropout is not a frozen count");
    }
    expectTraceInvariants(harness);
}

void test_a_reset_after_a_feedback_fault_starts_a_clean_run(void) {
    // Reset and re-arm after a feedback fault: the new run owns a fresh
    // allowance and proves progress anew - the fault does not leak into it,
    // and healthy powered travel authorizes drive again.
    ScenarioHarness harness(fixtures());
    announceAndArm(harness);
    NominalDriver driver;
    TEST_ASSERT_TRUE(driveToPhase(harness, ev::RunPhase::Approach, driver));
    const CycleRecord faulted = starveUntilFault(harness, CycleScript(), 1000U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::EncoderFeedbackLost),
                          static_cast<int>(faulted.faultOfRecord));
    harness.resetForNewRun();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(harness.phase()));
    announceAndArm(harness);
    NominalDriver secondDriver;
    TEST_ASSERT_TRUE_MESSAGE(driveToPhase(harness, ev::RunPhase::Approach, secondDriver),
                             "the run after the reset never reached Approach");
    TEST_ASSERT_TRUE_MESSAGE(stepUntilDriveAuthorized(harness, secondDriver),
                             "the run after the reset never authorized drive");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(harness.faultCode()));
    expectTraceInvariants(harness);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_nominal_scenario_reaches_done_through_the_real_chain);
    RUN_TEST(test_the_nominal_trace_replays_identically);
    RUN_TEST(test_the_stop_evidence_packet_is_composed_from_two_production_answers);
    RUN_TEST(test_a_stale_run_intention_can_never_drive_the_chain);
    RUN_TEST(test_a_held_stale_intention_is_presented_as_no_powered_request);
    RUN_TEST(test_a_hostile_reverse_request_is_refused_by_the_authority);
    RUN_TEST(test_every_fault_identity_is_latched_in_every_phase_and_stops_the_boundary);
    RUN_TEST(test_a_latched_fault_keeps_stopping_and_clearing_it_does_not_resume_the_run);
    RUN_TEST(test_a_disarm_during_an_underway_run_stops_it);
    RUN_TEST(test_an_invalid_configuration_never_starts_a_run);
    RUN_TEST(test_a_run_configuration_cannot_be_changed_at_an_illegal_time);
    RUN_TEST(test_a_sensor_dropout_denies_drive_and_the_run_keeps_asking);
    RUN_TEST(test_stale_odometry_never_ends_the_reverse_leg_early);
    RUN_TEST(test_stale_stop_evidence_never_earns_the_reversal);
    RUN_TEST(test_a_phase_that_never_gets_its_evidence_times_out);
    RUN_TEST(test_a_run_that_overruns_its_validated_maximum_faults_on_its_own_identity);
    RUN_TEST(test_a_vehicle_that_never_comes_near_zero_faults_instead_of_reversing);
    RUN_TEST(test_a_vehicle_that_never_moves_again_never_finishes_the_reverse_leg);
    RUN_TEST(test_a_direct_reverse_request_is_refused_and_the_run_reverses_legally);
    RUN_TEST(test_a_fault_overrides_a_travel_request_in_the_same_cycle);
    RUN_TEST(test_a_chain_reset_returns_to_nothing_and_a_second_run_works);
    RUN_TEST(test_fresh_motion_before_the_run_handoff_aborts_the_reversal);
    RUN_TEST(test_a_mid_run_reset_bars_the_opposite_direction_until_a_fresh_stop);
    RUN_TEST(test_an_incomplete_configuration_cannot_arm_the_chain);
    RUN_TEST(test_a_disarm_in_every_reversal_phase_stops_the_run);
    RUN_TEST(test_a_stalled_clock_is_not_speed_evidence_and_spends_no_phase_time);
    RUN_TEST(test_an_uneven_clock_does_not_disturb_the_run);
    RUN_TEST(test_a_run_that_crosses_the_timestamp_wrap_is_undisturbed);
    RUN_TEST(test_a_rewound_clock_stops_the_run_and_the_chain_recovers);
    RUN_TEST(test_the_production_decoder_decides_what_a_raw_sample_counts);
    RUN_TEST(test_the_run_axis_numbers_are_independent_of_the_harnesss_own_fields);
    RUN_TEST(test_the_forward_stop_the_run_shapes_to_is_the_push_objective_it_completes_at);
    RUN_TEST(test_the_run_enters_capture_when_the_capture_window_opens_without_claiming_contact);
    RUN_TEST(test_a_capture_phase_ceiling_slows_only_capture_through_the_one_shaper);
    RUN_TEST(test_a_planted_coasting_run_reaches_done_through_the_real_chain);
    RUN_TEST(test_a_planted_ideal_follower_stops_short_and_faults_with_its_own_identity);
    RUN_TEST(test_a_planted_frozen_run_makes_no_progress_and_the_feedback_bound_ends_it);
    RUN_TEST(test_a_planted_overshooting_run_still_reaches_done);
    RUN_TEST(test_a_planted_reverse_leg_that_stops_short_faults_with_its_own_identity);
    RUN_TEST(test_frozen_forward_counts_fault_with_the_feedback_identity);
    RUN_TEST(test_frozen_reverse_counts_fault_with_the_feedback_identity);
    RUN_TEST(test_wrong_sign_counts_under_forward_power_fault);
    RUN_TEST(test_intermittent_progress_under_power_never_faults_the_feedback_bound);
    RUN_TEST(test_a_long_dropout_pauses_the_feedback_bound);
    RUN_TEST(test_an_untriggered_armed_run_with_frozen_counts_never_faults);
    RUN_TEST(test_a_frozen_run_across_the_timestamp_wrap_faults);
    RUN_TEST(test_a_large_scheduling_gap_without_progress_faults_at_once);
    RUN_TEST(test_a_stalled_clock_with_frozen_counts_faults_nothing);
    RUN_TEST(test_a_feedback_fault_latches_and_a_reset_preserves_the_barrier);
    RUN_TEST(test_accepted_power_never_survives_stale_feedback);
    RUN_TEST(test_a_feedback_fault_disables_at_the_actuator_in_the_same_cycle);
    RUN_TEST(test_short_dropouts_separated_by_progress_never_fault);
    RUN_TEST(test_a_feedback_fault_yields_to_a_concurrent_driver_fault);
    RUN_TEST(test_a_dropout_in_the_terminal_phase_ends_in_the_phase_timeout);
    RUN_TEST(test_a_reset_after_a_feedback_fault_starts_a_clean_run);
    return UNITY_END();
}
