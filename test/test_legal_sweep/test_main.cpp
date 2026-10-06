#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <unity.h>

#include <BottleEvidence.h>
#include <RunConfig.h>
#include <RunExecutionPlan.h>
#include <Units.h>

#include "../test_scenario/ScenarioHarness.h"

// ---------------------------------------------------------------------------
// S11 - full legal-parameter host sweep and deterministic scenario hardening
// ---------------------------------------------------------------------------
//
// What this suite proves (software only)
//
// * Every configuration the announced rules allow - each tournament level's own
//   Target Distance grid, its own Bottle Line grid, the shared Target Time grid
//   and the level's fixed bottle placement - produces one complete, finite,
//   ordered execution plan from the production builder, and that construction is
//   bit-for-bit deterministic.
// * A representative subset of those configurations then drives the real
//   production chain (decoder -> odometry -> estimator -> run policy -> target
//   shaping -> regulator -> safety boundary -> actuator boundary) end to end to
//   Done, in the only phase order the run policy allows, with the forward travel
//   ending at the plan's own forward stop and the reverse leg ending at the
//   announced Target Point.
// * Malformed, off-grid, out-of-range, contradictory and fuzzed inputs are
//   refused by name instead of producing a plausible-looking plan.
//
// What this suite is not
//
// * It is not physical validation. The values this suite marks "measured" are
//   the scenario harness's own test fixtures, not measurements of this vehicle;
//   the numeric grids come from `ev::rules2027` (the code's copy of the official
//   2027 Division C rules). No result here may be promoted into
//   VERIFIED_STATE.md: every physical row there stays UNVERIFIED, and a passing
//   case proves only what the arithmetic and the state machine do with stated
//   evidence.
// * It duplicates no production formula. The plan comes from
//   `ev::buildRunExecutionPlan()`, the geometry from `ev::RunAxisGeometry`, the
//   progression facts from `ev::deriveBottleEvidence()`, the leg target from
//   `ev::legExecutionTargetM()`, and the whole end-to-end run from
//   `evscenario::ScenarioHarness`. The sweep only states inputs and checks the
//   outputs.
// * It invents no physical number. The forward-stop margin, the contact-window
//   tolerance and the bottle-interaction distances are the scenario fixtures;
//   the announced space is the rules source.

void setUp(void) {}
void tearDown(void) {}

namespace {

// ---------------------------------------------------------------------------
// The sweep's own record
// ---------------------------------------------------------------------------

struct SweepReport {
    uint32_t regionalCases = 0U;
    uint32_t stateCases = 0U;
    uint32_t nationalCases = 0U;
    uint32_t randomCases = 0U;
    uint32_t invalidConfigFuzzCases = 0U;
    uint32_t dependencyFuzzCases = 0U;
    uint32_t scenarioRuns = 0U;
    long long gridMs = 0;
    long long randomMs = 0;
    long long fuzzMs = 0;
    long long scenarioMs = 0;
};

SweepReport g_report;

long long elapsedMs(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// A tiny deterministic LCG: a "random" case is reproducible from its seed
// alone, so a failure names a case that can be re-run exactly.
class Lcg {
public:
    explicit Lcg(uint32_t seed) : state_(seed) {}

    uint32_t next() {
        state_ = state_ * 1664525U + 1013904223U;
        return state_;
    }

    uint32_t below(uint32_t bound) { return bound == 0U ? 0U : next() % bound; }

    // A uniform-ish value in [low, high).
    double between(double low, double high) {
        return low + (high - low) * (static_cast<double>(next()) / 4294967296.0);
    }

private:
    uint32_t state_;
};

// ---------------------------------------------------------------------------
// Fixtures and the rules source
// ---------------------------------------------------------------------------

const ev::TournamentLevel kLevels[3] = {ev::TournamentLevel::RegionalInvitational,
                                        ev::TournamentLevel::State,
                                        ev::TournamentLevel::National};

// The scenario harness's own measured fixtures, read once. They are test
// fixtures and not measurements of this vehicle; nothing here is evidence.
const ev::BottleEvidenceConfig& fixtureBottleConfig() {
    static const ev::BottleEvidenceConfig bottle =
        evscenario::nominalFixtures().phase.bottleEvidence;
    return bottle;
}

const ev::RunExecutionDependencies& fixtureDependencies() {
    static const ev::RunExecutionDependencies dependencies =
        evscenario::fixtureExecutionDependencies();
    return dependencies;
}

ev::RunExecutionPlanResult fixturePlanFor(
    const ev::RunConfig& config, bool timingReady = true,
    double predictedMotionTimeS = evscenario::kFixturePredictedMotionTimeS,
    double initialDelayS = evscenario::kFixtureInitialDelayS) {
    return ev::buildRunExecutionPlan(config, fixtureBottleConfig(), fixtureDependencies(),
                                     timingReady, predictedMotionTimeS, initialDelayS);
}

ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisionalValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

ev::BottleEvidenceConfig measuredBottle(double leadM, double advanceM, double pushMarginM) {
    return ev::BottleEvidenceConfig(measuredValue(leadM), measuredValue(advanceM),
                                    measuredValue(pushMarginM));
}

ev::BottleEvidenceConfig provisionalBottle(double leadM, double advanceM, double pushMarginM) {
    return ev::BottleEvidenceConfig(provisionalValue(leadM), provisionalValue(advanceM),
                                    provisionalValue(pushMarginM));
}

ev::RunExecutionDependencies measuredDependencies(double toleranceM, double marginM) {
    return ev::RunExecutionDependencies(measuredValue(toleranceM), measuredValue(marginM));
}

// The number of intervals between the range's two boundaries, from the rules
// source's own interval. Rounded rather than truncated because 4.0 / 0.1 is not
// exactly 40 in binary floating point.
uint32_t gridSteps(double spanM, double intervalM) {
    return static_cast<uint32_t>(std::lround(spanM / intervalM));
}

const char* levelName(ev::TournamentLevel level) {
    switch (level) {
        case ev::TournamentLevel::RegionalInvitational:
            return "RegionalInvitational";
        case ev::TournamentLevel::State:
            return "State";
        case ev::TournamentLevel::National:
            return "National";
    }
    return "Unknown";
}

void describeCase(char* out, size_t size, const ev::RunConfig& config) {
    std::snprintf(out, size,
                  "level=%s target_m=%.6f time_s=%.3f line_m=%.6f bottle_start_m=%.3f",
                  levelName(config.level), config.targetDistanceM, config.targetTimeS,
                  config.bottleLineDistanceM, config.bottleStartDistanceM);
}

// Builds the announced configuration a set of grid indices names.
ev::RunConfig gridConfig(ev::TournamentLevel level, uint32_t distanceIndex,
                         uint32_t lineIndex, uint32_t timeIndex) {
    const double interval = ev::announcedIntervalM(level);
    return ev::RunConfig(level,
                         ev::rules2027::kTargetDistanceMinM + distanceIndex * interval,
                         ev::rules2027::kTargetTimeMinS +
                             timeIndex * ev::rules2027::kTargetTimeIntervalS,
                         ev::rules2027::kBottleLineDistanceMinM + lineIndex * interval,
                         ev::bottleStartDistanceM(level));
}

// ---------------------------------------------------------------------------
// Failure accounting
// ---------------------------------------------------------------------------
//
// A sweep of ~1.7 million plan cases cannot afford one Unity assertion per case:
// the counters accumulate violations and the first one is kept verbatim, so a
// failure is reported once with the exact configuration that produced it.

struct FailureSet {
    uint32_t validate = 0U;
    uint32_t refused = 0U;
    uint32_t nonFinite = 0U;
    uint32_t negative = 0U;
    uint32_t ordering = 0U;
    uint32_t window = 0U;
    uint32_t forwardBeyondCapture = 0U;
    uint32_t reverseTarget = 0U;
    uint32_t forwardStopArithmetic = 0U;
    uint32_t geometry = 0U;
    uint32_t axisIdentity = 0U;
    uint32_t progressionEndpoint = 0U;
    uint32_t overflow = 0U;
    uint32_t determinism = 0U;

    const char* firstProperty = nullptr;
    char firstDetail[240] = {0};
};

void note(FailureSet& failures, const char* property, const char* detail) {
    if (failures.firstProperty == nullptr) {
        failures.firstProperty = property;
        std::snprintf(failures.firstDetail, sizeof(failures.firstDetail), "%s", detail);
    }
}

std::string failureMessage(const char* what, const FailureSet& failures) {
    std::string message(what);
    if (failures.firstProperty != nullptr) {
        message += " [first violation: ";
        message += failures.firstProperty;
        message += " @ ";
        message += failures.firstDetail;
        message += "]";
    }
    return message;
}

void expectSweepClean(const FailureSet& failures, const char* what) {
    const std::string message = failureMessage(what, failures);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.validate, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.refused, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.nonFinite, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.negative, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.ordering, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.window, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.forwardBeyondCapture, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.reverseTarget, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.forwardStopArithmetic, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.geometry, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.axisIdentity, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.progressionEndpoint, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.overflow, message.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, failures.determinism, message.c_str());
}

// ---------------------------------------------------------------------------
// The property check one legal announced configuration must pass
// ---------------------------------------------------------------------------

bool samePlan(const ev::RunExecutionPlan& first, const ev::RunExecutionPlan& second) {
    return first.valid == second.valid && first.status == second.status &&
           first.timingReady == second.timingReady &&
           first.earliestContactMpPositionM == second.earliestContactMpPositionM &&
           first.latestContactMpPositionM == second.latestContactMpPositionM &&
           first.captureGuaranteedMpPositionM == second.captureGuaranteedMpPositionM &&
           first.pushCompleteMpPositionM == second.pushCompleteMpPositionM &&
           first.forwardStopMpPositionM == second.forwardStopMpPositionM &&
           first.reverseStopMpPositionM == second.reverseStopMpPositionM &&
           first.predictedMotionTimeS == second.predictedMotionTimeS &&
           first.initialDelayS == second.initialDelayS &&
           first.geometry.targetPointPositionM == second.geometry.targetPointPositionM &&
           first.geometry.bottleLineDistanceM == second.geometry.bottleLineDistanceM &&
           first.geometry.bottleStartPositionM == second.geometry.bottleStartPositionM;
}

// Every property S11 asserts for one valid synthetic fixture plan:
//
//   1. the announced configuration is legal (`validateRunConfig()`);
//   2. the production builder produces a complete plan;
//   3. every value is finite;
//   4. no distance is negative and the delay is nonnegative;
//   5. the landmark ordering invariant holds;
//   6. the credible contact window is the announced tolerance either side of the
//      nominal contact position and is not inverted;
//   7. the forward execution target lies beyond the required capture region and
//      at or beyond the push objective;
//   8. the reverse target is the announced Target Point;
//   9. the derived positions are the documented arithmetic on the announced
//      geometry and the fixture calibration;
//  10. the geometry carries the announced rule values unchanged;
//  11. one forward endpoint: the motion shaper's leg target and the state
//      progression's completion threshold are the same stored value;
//  12. no overflow or contamination (a sane upper bound on every axis value);
//  13. deterministic repeatability: a second construction is bit-for-bit equal.
//
// One violation per case is recorded; the case is the first one that broke the
// property named in the message.
void checkValidCase(const ev::RunConfig& config, FailureSet& failures, bool checkDeterminism) {
    char detail[240];
    describeCase(detail, sizeof(detail), config);

    if (!ev::validateRunConfig(config).ok) {
        ++failures.validate;
        note(failures, "validateRunConfig refused a legal announced configuration", detail);
        return;
    }

    const ev::RunExecutionPlanResult result = fixturePlanFor(config);
    if (!(result.valid && result.status == ev::RunExecutionPlanStatus::Planned) ||
        !result.plan.valid) {
        ++failures.refused;
        note(failures, "the production plan builder refused a legal announced configuration",
             detail);
        return;
    }
    const ev::RunExecutionPlan& plan = result.plan;

    const ev::BottleEvidenceConfig& bottle = fixtureBottleConfig();
    const ev::RunExecutionDependencies& dependencies = fixtureDependencies();
    const double leadM = bottle.contactLeadDistanceM.value;
    const double captureAdvanceM = bottle.captureAdvanceDistanceM.value;
    const double pushMarginM = bottle.pushMarginDistanceM.value;
    const double toleranceM = dependencies.contactWindowToleranceM.value;
    const double marginM = dependencies.forwardStopMarginBeyondBottleLineM.value;
    const double nominalContactM = plan.geometry.bottleStartPositionM - leadM;

    if (!(std::isfinite(plan.earliestContactMpPositionM) &&
          std::isfinite(plan.latestContactMpPositionM) &&
          std::isfinite(plan.captureGuaranteedMpPositionM) &&
          std::isfinite(plan.pushCompleteMpPositionM) &&
          std::isfinite(plan.forwardStopMpPositionM) &&
          std::isfinite(plan.reverseStopMpPositionM) &&
          std::isfinite(plan.predictedMotionTimeS) && std::isfinite(plan.initialDelayS))) {
        ++failures.nonFinite;
        note(failures, "a plan value is not finite", detail);
        return;
    }

    if (plan.earliestContactMpPositionM < 0.0 || plan.latestContactMpPositionM < 0.0 ||
        plan.captureGuaranteedMpPositionM < 0.0 || plan.pushCompleteMpPositionM < 0.0 ||
        plan.forwardStopMpPositionM < 0.0 || plan.reverseStopMpPositionM < 0.0 ||
        plan.predictedMotionTimeS < 0.0 || plan.initialDelayS < 0.0) {
        ++failures.negative;
        note(failures, "a plan distance or delay is negative", detail);
        return;
    }

    if (!(plan.earliestContactMpPositionM <= plan.latestContactMpPositionM &&
          plan.latestContactMpPositionM <= plan.captureGuaranteedMpPositionM &&
          plan.captureGuaranteedMpPositionM <= plan.forwardStopMpPositionM &&
          plan.pushCompleteMpPositionM <= plan.forwardStopMpPositionM &&
          plan.earliestContactMpPositionM >= 0.0)) {
        ++failures.ordering;
        note(failures, "the plan's landmark ordering invariant is broken", detail);
        return;
    }

    if (!(plan.earliestContactMpPositionM < plan.latestContactMpPositionM &&
          std::fabs((plan.latestContactMpPositionM - plan.earliestContactMpPositionM) -
                    2.0 * toleranceM) <= 1e-9 &&
          std::fabs((plan.latestContactMpPositionM - nominalContactM) - toleranceM) <= 1e-9 &&
          std::fabs((nominalContactM - plan.earliestContactMpPositionM) - toleranceM) <= 1e-9)) {
        ++failures.window;
        note(failures, "the credible contact window is not the announced tolerance either side "
                       "of the nominal contact position",
             detail);
        return;
    }

    if (!(plan.forwardStopMpPositionM > plan.captureGuaranteedMpPositionM &&
          plan.forwardStopMpPositionM >= plan.pushCompleteMpPositionM)) {
        ++failures.forwardBeyondCapture;
        note(failures, "the forward execution target is not beyond the capture region and the "
                       "push objective",
             detail);
        return;
    }

    if (std::fabs(plan.reverseStopMpPositionM - config.targetDistanceM) > 1e-9) {
        ++failures.reverseTarget;
        note(failures, "the reverse stop is not the announced Target Point", detail);
        return;
    }

    if (std::fabs(plan.forwardStopMpPositionM -
                  (plan.geometry.bottleLinePositionM() + marginM)) > 1e-9 ||
        std::fabs(plan.pushCompleteMpPositionM -
                  (plan.geometry.bottleLinePositionM() + pushMarginM)) > 1e-9 ||
        std::fabs(plan.captureGuaranteedMpPositionM - (nominalContactM + captureAdvanceM)) >
            1e-9) {
        ++failures.forwardStopArithmetic;
        note(failures, "a derived position is not the documented arithmetic on its inputs",
             detail);
        return;
    }

    if (plan.geometry.targetPointPositionM != config.targetDistanceM ||
        plan.geometry.bottleLineDistanceM != config.bottleLineDistanceM ||
        plan.geometry.bottleStartPositionM != config.bottleStartDistanceM ||
        std::fabs(plan.geometry.bottleLinePositionM() -
                  (config.targetDistanceM + config.bottleLineDistanceM)) > 1e-9) {
        ++failures.geometry;
        note(failures, "the plan's rule geometry does not carry the announced values", detail);
        return;
    }

    const ev::LegExecutionTarget forwardLegTarget = ev::legExecutionTargetM(plan, true);
    const ev::LegExecutionTarget reverseLegTarget = ev::legExecutionTargetM(plan, false);
    if (!forwardLegTarget.usable || !reverseLegTarget.usable ||
        std::fabs(forwardLegTarget.positionM - plan.forwardStopMpPositionM) > 1e-12 ||
        std::fabs(reverseLegTarget.positionM - plan.reverseStopMpPositionM) > 1e-12) {
        ++failures.axisIdentity;
        note(failures, "a leg target is unusable or is not the plan's own stop", detail);
        return;
    }

    const ev::BottleEvidence atStop =
        ev::deriveBottleEvidence(plan, true, plan.forwardStopMpPositionM, true);
    const ev::BottleEvidence beforeStop =
        ev::deriveBottleEvidence(plan, true, plan.forwardStopMpPositionM - 0.001, true);
    const ev::BottleEvidence atWindowOpen =
        ev::deriveBottleEvidence(plan, true, plan.earliestContactMpPositionM, true);
    const ev::BottleEvidence beforeWindowOpen =
        ev::deriveBottleEvidence(plan, true, plan.earliestContactMpPositionM - 0.001, true);
    const bool thresholdsMatch =
        atStop.approachCompletePositionM == plan.earliestContactMpPositionM &&
        atStop.captureCompletePositionM == plan.captureGuaranteedMpPositionM &&
        atStop.pushCompletePositionM == plan.forwardStopMpPositionM;
    if (!(atStop.derived && atStop.pushComplete && atStop.captureComplete &&
          atStop.approachComplete && !beforeStop.pushComplete && atWindowOpen.derived &&
          atWindowOpen.approachComplete && !beforeWindowOpen.approachComplete &&
          thresholdsMatch)) {
        ++failures.progressionEndpoint;
        note(failures, "the forward progression does not complete at the plan's own forward stop",
             detail);
        return;
    }

    const double maxPositionM =
        std::max(std::max(plan.earliestContactMpPositionM, plan.latestContactMpPositionM),
                 std::max(std::max(plan.captureGuaranteedMpPositionM, plan.pushCompleteMpPositionM),
                          std::max(plan.forwardStopMpPositionM, plan.reverseStopMpPositionM)));
    if (maxPositionM > 100.0 || plan.predictedMotionTimeS > 1.0e6 ||
        plan.initialDelayS > 1.0e6) {
        ++failures.overflow;
        note(failures, "a plan value exceeds a sane upper bound", detail);
        return;
    }

    if (checkDeterminism) {
        const ev::RunExecutionPlanResult again = fixturePlanFor(config);
        if (!(again.valid && again.plan.valid && samePlan(plan, again.plan))) {
            ++failures.determinism;
            note(failures, "two constructions of the same plan are not bit-for-bit equal",
                 detail);
        }
    }
}

// ---------------------------------------------------------------------------
// Explicit refusal and acceptance cases
// ---------------------------------------------------------------------------

void expectPlanRefusal(const char* name, const ev::RunConfig& config,
                       const ev::BottleEvidenceConfig& bottle,
                       const ev::RunExecutionDependencies& dependencies,
                       ev::RunExecutionPlanStatus expected) {
    const ev::RunExecutionPlanResult result =
        ev::buildRunExecutionPlan(config, bottle, dependencies, true,
                                  evscenario::kFixturePredictedMotionTimeS,
                                  evscenario::kFixtureInitialDelayS);
    TEST_ASSERT_FALSE_MESSAGE(result.valid, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(result.status),
                                  name);
    TEST_ASSERT_FALSE_MESSAGE(result.plan.valid, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected),
                                  static_cast<int>(result.plan.status), name);
}

void expectPlanAccepted(const char* name, const ev::RunConfig& config,
                        const ev::BottleEvidenceConfig& bottle,
                        const ev::RunExecutionDependencies& dependencies) {
    const ev::RunExecutionPlanResult result =
        ev::buildRunExecutionPlan(config, bottle, dependencies, true,
                                  evscenario::kFixturePredictedMotionTimeS,
                                  evscenario::kFixtureInitialDelayS);
    TEST_ASSERT_TRUE_MESSAGE(result.valid, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunExecutionPlanStatus::Planned),
                                  static_cast<int>(result.status), name);
    TEST_ASSERT_TRUE_MESSAGE(result.plan.valid, name);
    TEST_ASSERT_TRUE_MESSAGE(std::isfinite(result.plan.forwardStopMpPositionM), name);
}

// ---------------------------------------------------------------------------
// Scenario support: the same scripted evidence the scenario suite uses
// ---------------------------------------------------------------------------
//
// The driver states evidence only. Every completion fact comes from the harness's
// own production derivation (`forwardBottleEvidence()`), never from a threshold
// repeated here, and the encoder advance is the same fixture rate the scenario
// suite scripts.

class LegalRunDriver {
public:
    evscenario::CycleScript next(const evscenario::ScenarioHarness& harness) {
        evscenario::CycleScript script;
        switch (harness.phase()) {
            case ev::RunPhase::Armed:
                script.startTriggered = true;
                break;
            case ev::RunPhase::Approach:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                script.approachComplete = harness.forwardBottleEvidence().approachComplete;
                break;
            case ev::RunPhase::Capture:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                script.captureComplete = harness.forwardBottleEvidence().captureComplete;
                break;
            case ev::RunPhase::Push:
                script.encoderAdvanceTicks = evscenario::kFixtureTicksPerCycle;
                script.pushComplete = harness.forwardBottleEvidence().pushComplete;
                break;
            case ev::RunPhase::Reverse:
            case ev::RunPhase::FinalApproach:
                script.encoderAdvanceTicks = -evscenario::kFixtureTicksPerCycle;
                break;
            case ev::RunPhase::Idle:
            case ev::RunPhase::ParameterValid:
            case ev::RunPhase::OptionalDelay:
            case ev::RunPhase::ForwardDecel:
            case ev::RunPhase::BrakeZeroSpeed:
            case ev::RunPhase::FinalBrake:
            case ev::RunPhase::Done:
            case ev::RunPhase::Fault:
                break;
        }
        return script;
    }
};

bool driveToDone(evscenario::ScenarioHarness& harness, LegalRunDriver& driver,
                 uint32_t maxCycles = 4000U) {
    for (uint32_t cycle = 0U; cycle < maxCycles; ++cycle) {
        if (harness.phase() == ev::RunPhase::Done) {
            return true;
        }
        if (harness.step(driver.next(harness)).phase == ev::RunPhase::Fault) {
            return false;
        }
    }
    return harness.phase() == ev::RunPhase::Done;
}

// The distinct phases of a trace, in the order they were first entered.
std::vector<ev::RunPhase> phaseSequence(const evscenario::ScenarioHarness& harness) {
    std::vector<ev::RunPhase> sequence;
    for (const evscenario::CycleRecord& record : harness.trace()) {
        if (sequence.empty() || sequence.back() != record.phase) {
            sequence.push_back(record.phase);
        }
    }
    return sequence;
}

// The one phase order the run policy allows, for every legal announced
// configuration: forward to the plan's own stop, a deliberate verified stop, a
// reversal, then the reverse leg to the announced Target Point.
void expectLegalPhaseSequence(const evscenario::ScenarioHarness& harness, const char* detail) {
    const ev::RunPhase expected[] = {ev::RunPhase::Armed,
                                     ev::RunPhase::OptionalDelay,
                                     ev::RunPhase::Approach,
                                     ev::RunPhase::Capture,
                                     ev::RunPhase::Push,
                                     ev::RunPhase::ForwardDecel,
                                     ev::RunPhase::BrakeZeroSpeed,
                                     ev::RunPhase::Reverse,
                                     ev::RunPhase::FinalApproach,
                                     ev::RunPhase::FinalBrake,
                                     ev::RunPhase::Done};
    const std::vector<ev::RunPhase> observed = phaseSequence(harness);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(sizeof(expected) / sizeof(expected[0])),
                                     static_cast<uint32_t>(observed.size()), detail);
    for (size_t index = 0U; index < observed.size() &&
                             index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected[index]),
                                      static_cast<int>(observed[index]), detail);
    }
}

// The architecture properties the scenario suite asserts for its own traces,
// asserted again here for every representative configuration the sweep drives.
void expectScenarioTraceInvariants(const evscenario::ScenarioHarness& harness,
                                   const char* detail) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(harness.actuatorRecords().size()),
                                     harness.cycles(), detail);

    ev::PoweredDirection previous = ev::PoweredDirection::None;
    for (const evscenario::CycleRecord& record : harness.trace()) {
        TEST_ASSERT_TRUE_MESSAGE(record.intention.emittedAtMs == record.nowMs, detail);
        if (!record.intentionCurrent) {
            TEST_ASSERT_FALSE_MESSAGE(ev::isDrivingMode(record.presentedEffort.mode), detail);
        }
        if (record.phase == ev::RunPhase::Idle ||
            record.phase == ev::RunPhase::ParameterValid) {
            previous = ev::PoweredDirection::None;
        }
        if (record.commandedClass == ev::ActuationClass::Drive) {
            TEST_ASSERT_TRUE_MESSAGE(record.authorityArmed, detail);
            TEST_ASSERT_TRUE_MESSAGE(record.readinessComplete, detail);
            TEST_ASSERT_TRUE_MESSAGE(record.requiredDataFresh, detail);
            TEST_ASSERT_TRUE_MESSAGE(record.intentionCurrent, detail);
            TEST_ASSERT_TRUE_MESSAGE(ev::isRunTravelCommand(record.command), detail);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::RunPhase::Fault),
                                          static_cast<int>(record.phase), detail);
            TEST_ASSERT_TRUE_MESSAGE(record.commandedDirection != ev::PoweredDirection::None,
                                     detail);
            TEST_ASSERT_TRUE_MESSAGE(record.commandedPermille > 0U, detail);
            const ev::PoweredDirection leg =
                ev::runCommandNamesForwardLeg(record.command) ? ev::PoweredDirection::Forward
                                                              : ev::PoweredDirection::Reverse;
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(leg),
                                          static_cast<int>(record.commandedDirection), detail);
            if (previous != ev::PoweredDirection::None &&
                record.commandedDirection != previous) {
                TEST_ASSERT_TRUE_MESSAGE(record.stopCyclesAtZeroSinceLastDrive >= 1U, detail);
                TEST_ASSERT_TRUE_MESSAGE(record.verifiedStopSinceLastDrive, detail);
            }
            previous = record.commandedDirection;
        } else {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, record.commandedPermille, detail);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::None),
                                          static_cast<int>(record.commandedDirection), detail);
        }
        if (record.phase == ev::RunPhase::Fault) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::MotorMode::Disabled),
                                          static_cast<int>(record.commandedMode), detail);
            TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(record.command), detail);
        }
        if (record.phase == ev::RunPhase::Done) {
            TEST_ASSERT_FALSE_MESSAGE(ev::isRunTravelCommand(record.command), detail);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ev::ActuationClass::Drive),
                                          static_cast<int>(record.commandedClass), detail);
        }
    }
}

// One representative announced configuration for the end-to-end part of the
// sweep. `level` selects the bottle placement; the other three values are on
// that level's announced grids.
struct ScenarioCase {
    ev::TournamentLevel level;
    double targetDistanceM;
    double targetTimeS;
    double bottleLineDistanceM;
};

std::vector<ScenarioCase> scenarioCases() {
    std::vector<ScenarioCase> cases;

    // Both ends of every level's range, and a shared interior point that sits on
    // all three grids (7.00 m / 15.0 s / 2.00 m).
    for (size_t index = 0U; index < 3U; ++index) {
        const ev::TournamentLevel level = kLevels[index];
        cases.push_back(ScenarioCase{level, ev::rules2027::kTargetDistanceMinM,
                                     ev::rules2027::kTargetTimeMinS,
                                     ev::rules2027::kBottleLineDistanceMinM});
        cases.push_back(ScenarioCase{level, ev::rules2027::kTargetDistanceMaxM,
                                     ev::rules2027::kTargetTimeMaxS,
                                     ev::rules2027::kBottleLineDistanceMaxM});
        cases.push_back(ScenarioCase{level, 7.00, 15.0, 2.00});
    }

    // Two deterministic intermediate configurations per level, drawn on that
    // level's own grids so the sweep's "random" cases are announced legal runs.
    Lcg rng(0x5E11U);
    for (size_t index = 0U; index < 3U; ++index) {
        const ev::TournamentLevel level = kLevels[index];
        const double interval = ev::announcedIntervalM(level);
        const uint32_t distanceSteps =
            gridSteps(ev::rules2027::kTargetDistanceMaxM - ev::rules2027::kTargetDistanceMinM,
                      interval);
        const uint32_t lineSteps =
            gridSteps(ev::rules2027::kBottleLineDistanceMaxM - ev::rules2027::kBottleLineDistanceMinM,
                      interval);
        const uint32_t timeSteps = gridSteps(
            ev::rules2027::kTargetTimeMaxS - ev::rules2027::kTargetTimeMinS,
            ev::rules2027::kTargetTimeIntervalS);
        for (uint32_t draw = 0U; draw < 2U; ++draw) {
            const ev::RunConfig config = gridConfig(level, rng.below(distanceSteps + 1U),
                                                    rng.below(lineSteps + 1U),
                                                    rng.below(timeSteps + 1U));
            cases.push_back(ScenarioCase{config.level, config.targetDistanceM, config.targetTimeS,
                                         config.bottleLineDistanceM});
        }
    }
    return cases;
}

// Announces one representative configuration, arms the chain, drives the real
// production chain to Done and asserts the properties S11 assigns to state
// integration. Returns the trace so a caller can check replay identity.
std::vector<evscenario::CycleRecord> runLegalScenarioToDone(const ScenarioCase& scenarioCase) {
    evscenario::ScenarioHarness harness(evscenario::nominalFixtures());
    const ev::RunConfig config(scenarioCase.level, scenarioCase.targetDistanceM,
                               scenarioCase.targetTimeS, scenarioCase.bottleLineDistanceM,
                               ev::bottleStartDistanceM(scenarioCase.level));
    char detail[240];
    describeCase(detail, sizeof(detail), config);

    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::RunStartStatus::Started),
                                  static_cast<int>(harness.announce(config)), detail);
    TEST_ASSERT_TRUE_MESSAGE(harness.arm(), detail);
    TEST_ASSERT_TRUE_MESSAGE(harness.executionPlanUsable(), detail);

    LegalRunDriver driver;
    TEST_ASSERT_TRUE_MESSAGE(driveToDone(harness, driver), detail);

    expectLegalPhaseSequence(harness, detail);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::FaultCode::None),
                                  static_cast<int>(harness.faultCode()), detail);
    TEST_ASSERT_TRUE_MESSAGE(harness.stopVerified(), detail);
    TEST_ASSERT_FALSE_MESSAGE(harness.decoderStepGuardTripped(), detail);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, harness.droppedSamples(), detail);

    const ev::RunExecutionPlan& plan = harness.executionPlan();
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(plan.reverseStopMpPositionM - config.targetDistanceM) <= 1e-12,
                             detail);
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(harness.targetPointPositionM() - config.targetDistanceM) <= 1e-12,
                             detail);

    // The forward travel really reached the plan's own forward stop, and the
    // reverse leg really came back to the announced Target Point, within the
    // resolution of one scripted control cycle (0.02 m).
    double maxPositionM = 0.0;
    for (const evscenario::CycleRecord& record : harness.trace()) {
        maxPositionM = std::max(maxPositionM, record.runAxisPositionM);
    }
    TEST_ASSERT_TRUE_MESSAGE(maxPositionM >= plan.forwardStopMpPositionM, detail);
    TEST_ASSERT_TRUE_MESSAGE(maxPositionM <= plan.forwardStopMpPositionM + 0.05, detail);
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(harness.runAxisPositionM() - config.targetDistanceM) <= 0.02,
                             detail);

    // The boundary saw exactly the event's direction sequence: forward, then
    // reverse, with the deliberate stop between them.
    bool sawForward = false;
    bool sawReverse = false;
    ev::PoweredDirection order[2] = {ev::PoweredDirection::None, ev::PoweredDirection::None};
    uint32_t orderCount = 0U;
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
    TEST_ASSERT_TRUE_MESSAGE(sawForward, detail);
    TEST_ASSERT_TRUE_MESSAGE(sawReverse, detail);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, orderCount, detail);
    if (orderCount == 2U) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::Forward),
                                      static_cast<int>(order[0]), detail);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::PoweredDirection::Reverse),
                                      static_cast<int>(order[1]), detail);
    }

    expectScenarioTraceInvariants(harness, detail);
    return harness.trace();
}

constexpr ev::TournamentLevel kRegional = ev::TournamentLevel::RegionalInvitational;
constexpr ev::TournamentLevel kState = ev::TournamentLevel::State;
constexpr ev::TournamentLevel kNational = ev::TournamentLevel::National;

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

constexpr uint32_t kRandomLegalCases = 20000U;
constexpr uint32_t kInvalidConfigFuzzCases = 3000U;
constexpr uint32_t kDependencyFuzzCases = 2000U;

// The full announced grid of one tournament level: every legal Target Distance
// index x every legal Bottle Line index x every legal Target Time index, with the
// level's own fixed bottle placement. Returns how many configurations it swept.
uint32_t sweepLevelGrid(ev::TournamentLevel level, FailureSet& failures) {
    const double interval = ev::announcedIntervalM(level);
    const uint32_t distanceSteps = gridSteps(
        ev::rules2027::kTargetDistanceMaxM - ev::rules2027::kTargetDistanceMinM, interval);
    const uint32_t lineSteps = gridSteps(
        ev::rules2027::kBottleLineDistanceMaxM - ev::rules2027::kBottleLineDistanceMinM,
        interval);
    const uint32_t timeSteps = gridSteps(
        ev::rules2027::kTargetTimeMaxS - ev::rules2027::kTargetTimeMinS,
        ev::rules2027::kTargetTimeIntervalS);

    uint32_t cases = 0U;
    for (uint32_t distanceIndex = 0U; distanceIndex <= distanceSteps; ++distanceIndex) {
        for (uint32_t lineIndex = 0U; lineIndex <= lineSteps; ++lineIndex) {
            for (uint32_t timeIndex = 0U; timeIndex <= timeSteps; ++timeIndex) {
                const ev::RunConfig config =
                    gridConfig(level, distanceIndex, lineIndex, timeIndex);
                checkValidCase(config, failures, true);
                ++cases;
            }
        }
    }
    return cases;
}

}  // namespace

// --- The full announced grids -------------------------------------------------

void test_the_regional_grid_is_fully_swept_and_every_plan_is_well_formed(void) {
    FailureSet failures;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const uint32_t cases = sweepLevelGrid(kRegional, failures);
    g_report.gridMs += elapsedMs(start);
    g_report.regionalCases = cases;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3213U, cases,
                                     "the Regional announced grid changed size (17 x 9 x 21)");
    expectSweepClean(failures, "the Regional grid (17 distances x 9 bottle lines x 21 times)");
}

void test_the_state_grid_is_fully_swept_and_every_plan_is_well_formed(void) {
    FailureSet failures;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const uint32_t cases = sweepLevelGrid(kState, failures);
    g_report.gridMs += elapsedMs(start);
    g_report.stateCases = cases;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(18081U, cases,
                                     "the State announced grid changed size (41 x 21 x 21)");
    expectSweepClean(failures, "the State grid (41 distances x 21 bottle lines x 21 times)");
}

void test_the_national_grid_is_fully_swept_and_every_plan_is_well_formed(void) {
    FailureSet failures;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const uint32_t cases = sweepLevelGrid(kNational, failures);
    g_report.gridMs += elapsedMs(start);
    g_report.nationalCases = cases;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1692621U, cases,
                                     "the National announced grid changed size (401 x 201 x 21)");
    expectSweepClean(failures,
                     "the National grid (401 distances x 201 bottle lines x 21 times)");
}

// --- Randomized legal intermediate cases --------------------------------------

void test_random_legal_intermediate_configurations_are_well_formed(void) {
    FailureSet failures;
    Lcg rng(0x51EEU);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (uint32_t index = 0U; index < kRandomLegalCases; ++index) {
        const ev::TournamentLevel level = kLevels[rng.below(3U)];
        const double interval = ev::announcedIntervalM(level);
        const uint32_t distanceSteps = gridSteps(4.0, interval);
        const uint32_t lineSteps = gridSteps(2.0, interval);
        const uint32_t timeSteps = gridSteps(10.0, ev::rules2027::kTargetTimeIntervalS);
        const ev::RunConfig config = gridConfig(level, rng.below(distanceSteps + 1U),
                                                rng.below(lineSteps + 1U),
                                                rng.below(timeSteps + 1U));
        checkValidCase(config, failures, true);
    }
    g_report.randomCases = kRandomLegalCases;
    g_report.randomMs += elapsedMs(start);
    expectSweepClean(failures, "randomized legal intermediate configurations");
}

// --- Invalid and fuzz-style cases ---------------------------------------------

// Every corruption below is guaranteed to make the configuration illegal, so the
// property asserted is both directions: the validator refuses it, and the plan
// builder reports `InvalidAnnouncement` rather than producing a plan.
void test_malformed_configuration_fuzz_is_always_refused(void) {
    FailureSet failures;
    Lcg rng(0xC0FFEEU);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (uint32_t index = 0U; index < kInvalidConfigFuzzCases; ++index) {
        const ev::TournamentLevel level = kLevels[rng.below(3U)];
        const double interval = ev::announcedIntervalM(level);
        const uint32_t distanceSteps = gridSteps(4.0, interval);
        const uint32_t lineSteps = gridSteps(2.0, interval);
        const uint32_t timeSteps = gridSteps(10.0, ev::rules2027::kTargetTimeIntervalS);
        double distanceM =
            ev::rules2027::kTargetDistanceMinM + rng.below(distanceSteps + 1U) * interval;
        double lineM =
            ev::rules2027::kBottleLineDistanceMinM + rng.below(lineSteps + 1U) * interval;
        double timeS = ev::rules2027::kTargetTimeMinS +
                       rng.below(timeSteps + 1U) * ev::rules2027::kTargetTimeIntervalS;
        double bottleStartM = ev::bottleStartDistanceM(level);

        switch (rng.below(12U)) {
            case 0U:
                distanceM += interval * 0.5;  // between two grid values
                break;
            case 1U:
                timeS += 0.25;  // half the Target Time grid
                break;
            case 2U:
                lineM += interval * 0.5;
                break;
            case 3U:
                distanceM = ev::rules2027::kTargetDistanceMinM - 0.02;
                break;
            case 4U:
                distanceM = ev::rules2027::kTargetDistanceMaxM + 0.02;
                break;
            case 5U:
                lineM = ev::rules2027::kBottleLineDistanceMinM - 0.02;
                break;
            case 6U:
                lineM = ev::rules2027::kBottleLineDistanceMaxM + 0.02;
                break;
            case 7U:
                timeS = ev::rules2027::kTargetTimeMaxS + 0.5;
                break;
            case 8U:
                distanceM = kNaN;
                break;
            case 9U:
                timeS = kInf;
                break;
            case 10U:
                // Another level's bottle placement, which is internally
                // inconsistent with the level above.
                bottleStartM = level == kRegional ? 3.0 : 2.0;
                break;
            case 11U:
                bottleStartM = distanceM;  // the bottle placed at the Target Point
                break;
        }

        const ev::RunConfig config(level, distanceM, timeS, lineM, bottleStartM);
        char detail[240];
        describeCase(detail, sizeof(detail), config);

        if (ev::validateRunConfig(config).ok) {
            ++failures.validate;
            note(failures, "a malformed announced configuration was accepted", detail);
            continue;
        }
        const ev::RunExecutionPlanResult result = fixturePlanFor(config);
        if (result.valid || result.plan.valid ||
            result.status != ev::RunExecutionPlanStatus::InvalidAnnouncement) {
            ++failures.refused;
            note(failures, "a malformed announced configuration produced a plan", detail);
        }
    }
    g_report.invalidConfigFuzzCases = kInvalidConfigFuzzCases;
    g_report.fuzzMs += elapsedMs(start);
    expectSweepClean(failures, "malformed announced configuration fuzz");
}

void test_dependency_fuzz_never_produces_a_plan_from_invalid_inputs(void) {
    FailureSet failures;
    Lcg rng(0xD0D0U);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (uint32_t index = 0U; index < kDependencyFuzzCases; ++index) {
        const ev::TournamentLevel level = kLevels[rng.below(3U)];
        const double interval = ev::announcedIntervalM(level);
        const uint32_t distanceSteps = gridSteps(4.0, interval);
        const uint32_t lineSteps = gridSteps(2.0, interval);
        const uint32_t timeSteps = gridSteps(10.0, ev::rules2027::kTargetTimeIntervalS);
        const ev::RunConfig config = gridConfig(level, rng.below(distanceSteps + 1U),
                                                rng.below(lineSteps + 1U),
                                                rng.below(timeSteps + 1U));

        // Drawn so that the even-index cases are valid by construction: the
        // window fits inside the capture advance, the early contact is behind the
        // Start Point never, and the forward stop reaches the push objective.
        const double leadM = rng.between(0.0, 0.5);
        const double advanceM = rng.between(0.0, 0.5);
        const double pushMarginM = rng.between(0.0, 0.5);
        const double toleranceM = rng.between(0.0, advanceM);
        const double marginM = rng.between(pushMarginM, 1.0);
        ev::BottleEvidenceConfig bottle = measuredBottle(leadM, advanceM, pushMarginM);
        ev::RunExecutionDependencies dependencies = measuredDependencies(toleranceM, marginM);

        char detail[240];
        describeCase(detail, sizeof(detail), config);

        if ((index & 1U) == 0U) {
            const ev::RunExecutionPlanResult result = ev::buildRunExecutionPlan(
                config, bottle, dependencies, true, evscenario::kFixturePredictedMotionTimeS,
                evscenario::kFixtureInitialDelayS);
            if (!(result.valid && result.plan.valid)) {
                ++failures.refused;
                note(failures, "a by-construction valid dependency set was refused", detail);
                continue;
            }
            if (std::fabs(result.plan.forwardStopMpPositionM -
                          (result.plan.geometry.bottleLinePositionM() + marginM)) > 1e-9) {
                ++failures.forwardStopArithmetic;
                note(failures, "the forward stop is not the line plus the supplied margin",
                     detail);
            }
            continue;
        }

        // Exactly one corrupted input: whichever named refusal it produces is
        // pinned by the explicit cases above this test; here the property is that
        // invalid input never becomes a usable plan.
        switch (rng.below(8U)) {
            case 0U:
                bottle = ev::BottleEvidenceConfig(provisionalValue(leadM),
                                                  measuredValue(advanceM),
                                                  measuredValue(pushMarginM));
                break;
            case 1U:
                bottle = ev::BottleEvidenceConfig(measuredValue(leadM),
                                                  provisionalValue(advanceM),
                                                  measuredValue(pushMarginM));
                break;
            case 2U:
                bottle = ev::BottleEvidenceConfig(measuredValue(leadM),
                                                  measuredValue(advanceM),
                                                  provisionalValue(pushMarginM));
                break;
            case 3U:
                dependencies =
                    ev::RunExecutionDependencies(provisionalValue(toleranceM),
                                                 measuredValue(marginM));
                break;
            case 4U:
                dependencies =
                    ev::RunExecutionDependencies(measuredValue(toleranceM),
                                                 provisionalValue(marginM));
                break;
            case 5U:
                bottle = measuredBottle(kNaN, advanceM, pushMarginM);
                break;
            case 6U:
                dependencies = measuredDependencies(kInf, marginM);
                break;
            case 7U:
                bottle = measuredBottle(-0.05, advanceM, pushMarginM);
                break;
        }

        const ev::RunExecutionPlanResult result = ev::buildRunExecutionPlan(
            config, bottle, dependencies, true, evscenario::kFixturePredictedMotionTimeS,
            evscenario::kFixtureInitialDelayS);
        if (result.valid || result.plan.valid ||
            result.status == ev::RunExecutionPlanStatus::Planned) {
            ++failures.refused;
            note(failures, "a corrupted dependency set produced a usable plan", detail);
        }
    }
    g_report.dependencyFuzzCases = kDependencyFuzzCases;
    g_report.fuzzMs += elapsedMs(start);
    expectSweepClean(failures, "execution dependency fuzz");
}

// --- Named refusals -----------------------------------------------------------

void test_illegal_announcements_are_refused_by_name(void) {
    const ev::BottleEvidenceConfig& bottle = fixtureBottleConfig();
    const ev::RunExecutionDependencies& dependencies = fixtureDependencies();
    using ev::RunExecutionPlanStatus;

    expectPlanRefusal("a Regional target distance off the 0.25 m grid",
                      ev::RunConfig(kRegional, 5.10, 10.0, 1.00, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Regional Bottle Line off the 0.25 m grid",
                      ev::RunConfig(kRegional, 7.00, 10.0, 1.10, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a State target distance off the 0.10 m grid",
                      ev::RunConfig(kState, 5.05, 10.0, 1.00, 3.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a National target distance off the 0.01 m grid",
                      ev::RunConfig(kNational, 5.005, 10.0, 1.00, 4.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Target Time off the 0.5 s grid",
                      ev::RunConfig(kRegional, 7.00, 10.25, 1.00, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a target distance below the official range",
                      ev::RunConfig(kNational, 4.99, 10.0, 1.00, 4.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a target distance above the official range",
                      ev::RunConfig(kNational, 9.01, 10.0, 1.00, 4.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Target Time below the official range",
                      ev::RunConfig(kRegional, 7.00, 9.5, 1.00, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Target Time above the official range",
                      ev::RunConfig(kRegional, 7.00, 20.5, 1.00, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Bottle Line below the official range",
                      ev::RunConfig(kState, 7.00, 10.0, 0.99, 3.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a Bottle Line above the official range",
                      ev::RunConfig(kNational, 7.00, 10.0, 3.01, 4.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a non-finite target distance",
                      ev::RunConfig(kNational, kNaN, 10.0, 1.00, 4.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a non-finite Target Time",
                      ev::RunConfig(kRegional, 7.00, kInf, 1.00, 2.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a non-finite Bottle Line",
                      ev::RunConfig(kState, 7.00, 10.0, kNaN, 3.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("another level's bottle placement",
                      ev::RunConfig(kRegional, 7.00, 10.0, 1.00, 3.0), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
    expectPlanRefusal("a bottle placed at the Target Point",
                      ev::RunConfig(kNational, 5.00, 10.0, 1.00, 5.00), bottle, dependencies,
                      RunExecutionPlanStatus::InvalidAnnouncement);
}

void test_execution_dependencies_are_refused_by_name_and_accepted_at_their_boundaries(void) {
    const ev::RunConfig valid = evscenario::validRunConfig();
    using ev::RunExecutionPlanStatus;

    const ev::RunExecutionDependencies provisionalMargin(measuredValue(0.02),
                                                         provisionalValue(0.10));
    const ev::RunExecutionDependencies provisionalTolerance(provisionalValue(0.02),
                                                            measuredValue(0.10));
    const double fixtureAdvanceM = fixtureBottleConfig().captureAdvanceDistanceM.value;
    const double fixturePushMarginM = fixtureBottleConfig().pushMarginDistanceM.value;

    // Provenance: a placeholder never authorizes a plan that depends on it.
    expectPlanRefusal("a Provisional bottle-interaction distance cannot establish a plan", valid,
                      provisionalBottle(0.10, 0.20, 0.05), fixtureDependencies(),
                      RunExecutionPlanStatus::MissingBottleInteractionCalibration);
    expectPlanRefusal("a Provisional contact-window tolerance cannot establish a plan", valid,
                      fixtureBottleConfig(), provisionalTolerance,
                      RunExecutionPlanStatus::MissingBottleInteractionCalibration);
    expectPlanRefusal("a Provisional forward-stop margin cannot establish a plan", valid,
                      fixtureBottleConfig(), provisionalMargin,
                      RunExecutionPlanStatus::MissingForwardExecutionTarget);

    // Non-finite values, even when they are marked measured.
    expectPlanRefusal("a non-finite contact lead", valid, measuredBottle(kNaN, 0.20, 0.05),
                      fixtureDependencies(), RunExecutionPlanStatus::NonFiniteValue);
    expectPlanRefusal("a non-finite contact-window tolerance", valid, fixtureBottleConfig(),
                      measuredDependencies(kNaN, 0.10), RunExecutionPlanStatus::NonFiniteValue);
    expectPlanRefusal("a non-finite forward-stop margin", valid, fixtureBottleConfig(),
                      measuredDependencies(0.02, kInf), RunExecutionPlanStatus::NonFiniteValue);

    // Contradictory geometry.
    expectPlanRefusal("a negative contact lead", valid, measuredBottle(-0.10, 0.20, 0.05),
                      fixtureDependencies(),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    expectPlanRefusal("a negative push margin", valid, measuredBottle(0.10, 0.20, -0.05),
                      fixtureDependencies(),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    expectPlanRefusal("a contact window wider than the capture advance", valid,
                      fixtureBottleConfig(), measuredDependencies(fixtureAdvanceM + 0.30, 0.10),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    expectPlanRefusal("a negative contact-window tolerance", valid, fixtureBottleConfig(),
                      measuredDependencies(-0.02, 0.10),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    expectPlanRefusal("a forward stop short of the push objective", valid, fixtureBottleConfig(),
                      measuredDependencies(0.02, -0.05),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);
    expectPlanRefusal("a forward stop at the Bottle Line while the push objective is past it",
                      valid, fixtureBottleConfig(), measuredDependencies(0.02, 0.0),
                      RunExecutionPlanStatus::ContradictoryExecutionGeometry);

    // The accepted boundary variants of the same checks.
    expectPlanAccepted("a window exactly as wide as the capture advance", valid,
                       fixtureBottleConfig(), measuredDependencies(fixtureAdvanceM, 0.10));
    expectPlanAccepted("a zero-width credible window", valid,
                       measuredBottle(0.10, 0.0, 0.05), measuredDependencies(0.0, 0.10));
    expectPlanAccepted("a forward stop exactly at the push objective", valid,
                       fixtureBottleConfig(),
                       measuredDependencies(0.02, fixturePushMarginM));
    expectPlanAccepted("a forward stop exactly at the Bottle Line when the push margin is zero",
                       valid, measuredBottle(0.10, 0.20, 0.0), measuredDependencies(0.02, 0.0));
}

void test_a_refused_plan_carries_no_landmark_no_leg_target_and_no_evidence(void) {
    const ev::RunExecutionPlanStatus statuses[] = {
        ev::RunExecutionPlanStatus::InvalidAnnouncement,
        ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
        ev::RunExecutionPlanStatus::MissingForwardExecutionTarget,
        ev::RunExecutionPlanStatus::NonFiniteValue,
        ev::RunExecutionPlanStatus::ContradictoryExecutionGeometry,
        ev::RunExecutionPlanStatus::NotPlanned,
    };
    const size_t statusCount = sizeof(statuses) / sizeof(statuses[0]);
    const char* names[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    for (size_t index = 0U; index < statusCount; ++index) {
        const ev::RunExecutionPlan plan = ev::refusedExecutionPlan(statuses[index]);
        TEST_ASSERT_FALSE(plan.valid);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(statuses[index]), static_cast<int>(plan.status));
        names[index] = ev::runExecutionPlanStatusName(statuses[index]);
        TEST_ASSERT_NOT_NULL(names[index]);
        TEST_ASSERT_TRUE(plan.earliestContactMpPositionM == 0.0);
        TEST_ASSERT_TRUE(plan.latestContactMpPositionM == 0.0);
        TEST_ASSERT_TRUE(plan.captureGuaranteedMpPositionM == 0.0);
        TEST_ASSERT_TRUE(plan.pushCompleteMpPositionM == 0.0);
        TEST_ASSERT_TRUE(plan.forwardStopMpPositionM == 0.0);
        TEST_ASSERT_TRUE(plan.reverseStopMpPositionM == 0.0);
        TEST_ASSERT_FALSE(plan.timingReady);
        TEST_ASSERT_TRUE(plan.predictedMotionTimeS == 0.0);
        TEST_ASSERT_TRUE(plan.initialDelayS == 0.0);
        TEST_ASSERT_FALSE(ev::legExecutionTargetM(plan, true).usable);
        TEST_ASSERT_FALSE(ev::legExecutionTargetM(plan, false).usable);
        const ev::BottleEvidence evidence = ev::deriveBottleEvidence(plan, true, 6.0, true);
        TEST_ASSERT_FALSE(evidence.derived);
        TEST_ASSERT_FALSE(evidence.approachComplete);
        TEST_ASSERT_FALSE(evidence.captureComplete);
        TEST_ASSERT_FALSE(evidence.pushComplete);
    }

    // Every named refusal has its own spelling, so a diagnostic can tell them
    // apart.
    for (size_t first = 0U; first < statusCount; ++first) {
        for (size_t second = first + 1U; second < statusCount; ++second) {
            TEST_ASSERT_TRUE(std::strcmp(names[first], names[second]) != 0);
        }
    }

    // The holder's lifecycle: before an announcement there is no plan, an
    // adopted plan is the run's plan, and a new-run boundary returns to none.
    ev::RunExecutionPlanHolder holder;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::NotPlanned),
                          static_cast<int>(holder.plan().status));
    TEST_ASSERT_FALSE(holder.plan().valid);
    holder.adopt(fixturePlanFor(evscenario::validRunConfig()).plan);
    TEST_ASSERT_TRUE(holder.plan().valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::Planned),
                          static_cast<int>(holder.plan().status));
    holder.clear();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::NotPlanned),
                          static_cast<int>(holder.plan().status));
    TEST_ASSERT_FALSE(holder.plan().valid);
}

void test_timing_state_is_recorded_honestly_across_the_legal_space(void) {
    const ev::RunConfig valid = evscenario::validRunConfig();

    // The production shape: not timing-ready, so both numbers are zero no matter
    // what an unvalidated estimate would have claimed.
    const ev::RunExecutionPlanResult productionShaped = ev::buildRunExecutionPlan(
        valid, fixtureBottleConfig(), fixtureDependencies(), false,
        evscenario::kFixturePredictedMotionTimeS, evscenario::kFixtureInitialDelayS);
    TEST_ASSERT_TRUE(productionShaped.valid);
    TEST_ASSERT_FALSE(productionShaped.plan.timingReady);
    TEST_ASSERT_TRUE(productionShaped.plan.predictedMotionTimeS == 0.0);
    TEST_ASSERT_TRUE(productionShaped.plan.initialDelayS == 0.0);

    for (size_t index = 0U; index < 3U; ++index) {
        const ev::RunConfig config = gridConfig(kLevels[index], 0U, 0U, 0U);
        const ev::RunExecutionPlanResult result = ev::buildRunExecutionPlan(
            config, fixtureBottleConfig(), fixtureDependencies(), false, 20.0, 1.0);
        TEST_ASSERT_TRUE(result.valid);
        TEST_ASSERT_FALSE(result.plan.timingReady);
        TEST_ASSERT_TRUE(result.plan.predictedMotionTimeS == 0.0);
        TEST_ASSERT_TRUE(result.plan.initialDelayS == 0.0);
    }

    // A validated timing model records exactly what it was given, nonnegative.
    const ev::RunExecutionPlanResult timingReady = ev::buildRunExecutionPlan(
        valid, fixtureBottleConfig(), fixtureDependencies(), true, 12.5, 0.25);
    TEST_ASSERT_TRUE(timingReady.valid);
    TEST_ASSERT_TRUE(timingReady.plan.timingReady);
    TEST_ASSERT_TRUE(timingReady.plan.predictedMotionTimeS == 12.5);
    TEST_ASSERT_TRUE(timingReady.plan.initialDelayS == 0.25);
    TEST_ASSERT_TRUE(timingReady.plan.initialDelayS >= 0.0);

    // Malformed timing inputs on a timing-ready plan are no plan at all.
    const auto refusesTiming = [&](double predictedMotionTimeS, double initialDelayS) {
        const ev::RunExecutionPlanResult result = ev::buildRunExecutionPlan(
            valid, fixtureBottleConfig(), fixtureDependencies(), true, predictedMotionTimeS,
            initialDelayS);
        return !result.valid && result.plan.valid == false &&
               result.status == ev::RunExecutionPlanStatus::NonFiniteValue;
    };
    TEST_ASSERT_TRUE(refusesTiming(kNaN, 0.10));
    TEST_ASSERT_TRUE(refusesTiming(-1.0, 0.10));
    TEST_ASSERT_TRUE(refusesTiming(kInf, 0.10));
    TEST_ASSERT_TRUE(refusesTiming(15.0, -0.10));
    TEST_ASSERT_TRUE(refusesTiming(15.0, kInf));
}

// --- State integration over representative legal configurations ----------------

void test_representative_legal_configurations_reach_done_through_the_real_chain(void) {
    const std::vector<ScenarioCase> cases = scenarioCases();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(15U, static_cast<uint32_t>(cases.size()),
                                     "the representative scenario set changed size");
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (const ScenarioCase& scenarioCase : cases) {
        runLegalScenarioToDone(scenarioCase);
        ++g_report.scenarioRuns;
    }
    g_report.scenarioMs += elapsedMs(start);
}

void test_representative_scenario_traces_replay_identically(void) {
    const std::vector<ScenarioCase> cases = scenarioCases();
    // The widest configuration of each level plus one intermediate: the cases
    // where a drift in the chain would show up first.
    const uint32_t replayIndices[4] = {1U, 6U, 11U, 13U};
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (size_t index = 0U; index < sizeof(replayIndices) / sizeof(replayIndices[0]); ++index) {
        const ScenarioCase& scenarioCase = cases[replayIndices[index]];
        const std::vector<evscenario::CycleRecord> first = runLegalScenarioToDone(scenarioCase);
        const std::vector<evscenario::CycleRecord> second = runLegalScenarioToDone(scenarioCase);

        const ev::RunConfig config(scenarioCase.level, scenarioCase.targetDistanceM,
                                   scenarioCase.targetTimeS, scenarioCase.bottleLineDistanceM,
                                   ev::bottleStartDistanceM(scenarioCase.level));
        char detail[240];
        describeCase(detail, sizeof(detail), config);

        TEST_ASSERT_TRUE_MESSAGE(first.size() > 100U, detail);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(first.size()),
                                         static_cast<uint32_t>(second.size()), detail);
        for (size_t cycle = 0U; cycle < first.size() && cycle < second.size(); ++cycle) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(first[cycle].nowMs, second[cycle].nowMs, detail);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[cycle].phase),
                                          static_cast<int>(second[cycle].phase), detail);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[cycle].command),
                                          static_cast<int>(second[cycle].command), detail);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[cycle].reversalPhase),
                                          static_cast<int>(second[cycle].reversalPhase), detail);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[cycle].commandedMode),
                                          static_cast<int>(second[cycle].commandedMode), detail);
            TEST_ASSERT_EQUAL_UINT16(first[cycle].commandedPermille,
                                     second[cycle].commandedPermille);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(first[cycle].outcome),
                                          static_cast<int>(second[cycle].outcome), detail);
            TEST_ASSERT_TRUE_MESSAGE(
                first[cycle].runAxisPositionM == second[cycle].runAxisPositionM, detail);
        }
        g_report.scenarioRuns += 2U;
    }
    g_report.scenarioMs += elapsedMs(start);
}

// --- The sweep's own totals ----------------------------------------------------

// One line of measured coverage for the stage report. It is output, not an
// assertion: what must hold is asserted above.
void printSweepReport(void) {
    std::printf(
        "[legal-sweep] grid(regional=%u state=%u national=%u total=%u) random=%u "
        "invalid_fuzz=%u dependency_fuzz=%u scenario_runs=%u | grid_ms=%lld random_ms=%lld "
        "fuzz_ms=%lld scenario_ms=%lld\n",
        static_cast<unsigned>(g_report.regionalCases), static_cast<unsigned>(g_report.stateCases),
        static_cast<unsigned>(g_report.nationalCases),
        static_cast<unsigned>(g_report.regionalCases + g_report.stateCases +
                              g_report.nationalCases),
        static_cast<unsigned>(g_report.randomCases),
        static_cast<unsigned>(g_report.invalidConfigFuzzCases),
        static_cast<unsigned>(g_report.dependencyFuzzCases),
        static_cast<unsigned>(g_report.scenarioRuns), g_report.gridMs, g_report.randomMs,
        g_report.fuzzMs, g_report.scenarioMs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_regional_grid_is_fully_swept_and_every_plan_is_well_formed);
    RUN_TEST(test_the_state_grid_is_fully_swept_and_every_plan_is_well_formed);
    RUN_TEST(test_the_national_grid_is_fully_swept_and_every_plan_is_well_formed);
    RUN_TEST(test_random_legal_intermediate_configurations_are_well_formed);
    RUN_TEST(test_malformed_configuration_fuzz_is_always_refused);
    RUN_TEST(test_dependency_fuzz_never_produces_a_plan_from_invalid_inputs);
    RUN_TEST(test_illegal_announcements_are_refused_by_name);
    RUN_TEST(test_execution_dependencies_are_refused_by_name_and_accepted_at_their_boundaries);
    RUN_TEST(test_a_refused_plan_carries_no_landmark_no_leg_target_and_no_evidence);
    RUN_TEST(test_timing_state_is_recorded_honestly_across_the_legal_space);
    RUN_TEST(test_representative_legal_configurations_reach_done_through_the_real_chain);
    RUN_TEST(test_representative_scenario_traces_replay_identically);
    printSweepReport();
    return UNITY_END();
}
