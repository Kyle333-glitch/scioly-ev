#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <unity.h>

#include <BottleEvidence.h>
#include <RunAxis.h>
#include <RunConfig.h>
#include <RunExecutionPlan.h>
#include <Units.h>

// Contract tests for the per-announcement execution plan: the immutable execution
// geometry of ONE announced run, its dependency gates, its refusal names and its
// honesty about timing readiness.
//
// S3 creates the plan abstraction and its pure construction/validation logic. It
// does NOT wire the plan through the runtime (S4 owns that), so nothing here
// asserts anything about vehicle behavior.
//
// Every value below is a **synthetic test fixture**, not a measurement and not an
// announced competition configuration. Nothing in this file is:
//
//   * a physical measurement,
//   * calibration evidence, or
//   * `VERIFIED_STATE.md` evidence.
//
// A passing test is pure arithmetic and gate verification only: it is not evidence
// that a pusher touches a bottle anywhere, that a bottle is captured, that a
// forward stop margin exists on this vehicle, or that the vehicle stops where it
// is commanded to. All twelve `VERIFIED_STATE.md` rows stay UNVERIFIED.

// Pinned at compile time: a plan, its dependencies and its result must not arrive
// without being built from something, and a plan must not be re-assignable after
// construction.
static_assert(!std::is_default_constructible<ev::RunExecutionPlan>::value,
              "no silent defaults; an execution plan must be built from an announcement");
static_assert(!std::is_copy_assignable<ev::RunExecutionPlan>::value,
              "an execution plan must be immutable: every field is const");
static_assert(!std::is_default_constructible<ev::RunExecutionDependencies>::value,
              "execution dependencies must state their provenance, never default");
static_assert(!std::is_default_constructible<ev::RunExecutionPlanResult>::value,
              "a construction result must come from a construction attempt");
static_assert(static_cast<int>(ev::RunExecutionPlanStatus::Planned) == 0,
              "the planned status must stay the zero value");

void setUp(void) {}
void tearDown(void) {}

namespace {

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Comparisons use an explicit absolute tolerance rather than a double equality
// assert, so this target does not depend on optional Unity double support.
const double kToleranceM = 1e-9;

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

ev::Sourced<double> derivedValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Derived, "synthetic fixture derivation");
}

// The Regional fixture announcement: 5.00 m Target Distance, 1.00 m Bottle Line
// Distance, the Regional 2.0 m bottle placement (rule 7.c.ii.(5)). Its rule
// landmarks are the Target Point at 5.0 m, the Bottle Start Point at 2.0 m and the
// Bottle Line at 6.0 m.
ev::RunConfig regionalAnnouncement() {
    return ev::RunConfig(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
}

// A State-grid fixture announcement: 5.20 m and 1.20 m (both on the State 0.10 m
// grid), with the State 3.0 m bottle placement - a different Target Point, Bottle
// Line and Bottle Start Point from the Regional fixture.
ev::RunConfig stateAnnouncement() {
    return ev::RunConfig(ev::TournamentLevel::State, 5.20, 10.0, 1.20, 3.0);
}

// Synthetic bottle-interaction calibration: a 0.02 m contact lead, a 0.05 m
// capture advance and a 0.01 m push margin. Established (ValueSource::Measured)
// *for the purposes of exercising the valid path only* - these are fixture
// numbers, not measurements of this vehicle, and they must never be written into
// VehicleConfig as real values.
ev::BottleEvidenceConfig measuredBottleConfig() {
    return ev::BottleEvidenceConfig(measured(0.02), measured(0.05), measured(0.01));
}

// Synthetic physical execution dependencies: a 0.01 m credible contact-window
// tolerance and a 0.15 m forward stop margin beyond the Bottle Line.
ev::RunExecutionDependencies measuredExecutionDependencies() {
    return ev::RunExecutionDependencies(measured(0.01), measured(0.15));
}

void expectPosition(double expectedM, double actualM) {
    TEST_ASSERT_TRUE(std::fabs(actualM - expectedM) <= kToleranceM);
}

// Every refusal path must carry the same invariants: not valid, a named (non-
// Planned) status, and no derived position at all.
void expectRefused(ev::RunExecutionPlanStatus expected, const ev::RunExecutionPlanResult& result) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.status));
    TEST_ASSERT_TRUE(result.status != ev::RunExecutionPlanStatus::Planned);
    TEST_ASSERT_EQUAL_INT(0, result.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(0, result.plan.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.plan.status));
    // The documented invariant, checked on every inspection.
    TEST_ASSERT_EQUAL_INT(result.status == ev::RunExecutionPlanStatus::Planned ? 1 : 0,
                          result.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(result.plan.status == ev::RunExecutionPlanStatus::Planned ? 1 : 0,
                          result.plan.valid ? 1 : 0);
    // A refusal carries zero positions: there is nothing to mistake for a plan.
    expectPosition(0.0, result.plan.earliestContactMpPositionM);
    expectPosition(0.0, result.plan.latestContactMpPositionM);
    expectPosition(0.0, result.plan.captureGuaranteedMpPositionM);
    expectPosition(0.0, result.plan.pushCompleteMpPositionM);
    expectPosition(0.0, result.plan.forwardStopMpPositionM);
    expectPosition(0.0, result.plan.reverseStopMpPositionM);
    TEST_ASSERT_EQUAL_INT(0, result.plan.timingReady ? 1 : 0);
    expectPosition(0.0, result.plan.predictedMotionTimeS);
    expectPosition(0.0, result.plan.initialDelayS);
    // A refusal must never name a status this module does not define, so a log
    // can attribute it.
    TEST_ASSERT_TRUE(ev::runExecutionPlanStatusName(expected)[0] != '?');
}

// Every successful construction must satisfy the plan's own invariants.
void expectValid(const ev::RunExecutionPlanResult& result) {
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_TRUE(result.plan.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::Planned),
                          static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::Planned),
                          static_cast<int>(result.plan.status));
}

}  // namespace

// --- 1. A valid synthetic execution plan --------------------------------------

void test_a_valid_synthetic_announcement_yields_a_complete_execution_plan(void) {
    const ev::RunExecutionPlanResult result = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), false,
        0.0, 0.0);

    expectValid(result);
    const ev::RunExecutionPlan& plan = result.plan;

    // The announced rule geometry is preserved by value, not re-derived.
    TEST_ASSERT_TRUE(plan.geometry.targetPointPositionM == 5.0);
    TEST_ASSERT_TRUE(plan.geometry.bottleStartPositionM == 2.0);
    TEST_ASSERT_TRUE(plan.geometry.bottleLinePositionM() == 6.0);

    // Nominal contact = Bottle Start Point - contact lead (2.0 - 0.02); the
    // credible window is the nominal position plus/minus the tolerance.
    expectPosition(1.97, plan.earliestContactMpPositionM);
    expectPosition(1.99, plan.latestContactMpPositionM);
    // Capture = nominal contact + capture advance; push complete = Bottle Line +
    // push margin; the forward stop = Bottle Line + the measured margin.
    expectPosition(2.03, plan.captureGuaranteedMpPositionM);
    expectPosition(6.01, plan.pushCompleteMpPositionM);
    expectPosition(6.15, plan.forwardStopMpPositionM);
    // The reverse stop is the announced Target Point (rule 8.h).
    expectPosition(5.0, plan.reverseStopMpPositionM);

    // The ordering invariant holds on a valid plan.
    TEST_ASSERT_TRUE(plan.earliestContactMpPositionM <= plan.latestContactMpPositionM);
    TEST_ASSERT_TRUE(plan.latestContactMpPositionM <= plan.captureGuaranteedMpPositionM);
    TEST_ASSERT_TRUE(plan.captureGuaranteedMpPositionM <= plan.forwardStopMpPositionM);
    TEST_ASSERT_TRUE(plan.pushCompleteMpPositionM <= plan.forwardStopMpPositionM);

    // No timing evidence behind this fixture, so the plan says so.
    TEST_ASSERT_EQUAL_INT(0, plan.timingReady ? 1 : 0);
    expectPosition(0.0, plan.predictedMotionTimeS);
    expectPosition(0.0, plan.initialDelayS);
}

// --- 2. Invalid announcement / invalid rule geometry --------------------------

void test_an_illegal_announcement_is_refused_before_any_execution_arithmetic(void) {
    // Off the Regional 0.25 m announced grid (rule 8.h).
    const ev::RunConfig offGrid(ev::TournamentLevel::RegionalInvitational, 5.03, 10.0, 1.0, 2.0);
    expectRefused(ev::RunExecutionPlanStatus::InvalidAnnouncement,
                  ev::buildRunExecutionPlan(offGrid, measuredBottleConfig(),
                                            measuredExecutionDependencies(), false, 0.0, 0.0));

    // Below the rule 8.h minimum Target Distance.
    const ev::RunConfig tooShort(ev::TournamentLevel::RegionalInvitational, 4.5, 10.0, 1.0, 2.0);
    expectRefused(ev::RunExecutionPlanStatus::InvalidAnnouncement,
                  ev::buildRunExecutionPlan(tooShort, measuredBottleConfig(),
                                            measuredExecutionDependencies(), false, 0.0, 0.0));

    // A bottle placement that is not this level's (rule 7.c.ii.(5)).
    const ev::RunConfig wrongBottle(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 3.0);
    expectRefused(ev::RunExecutionPlanStatus::InvalidAnnouncement,
                  ev::buildRunExecutionPlan(wrongBottle, measuredBottleConfig(),
                                            measuredExecutionDependencies(), false, 0.0, 0.0));

    // A non-finite announced value is an illegal announcement, not execution
    // geometry: nothing may be derived from it.
    const ev::RunConfig notANumber(ev::TournamentLevel::RegionalInvitational, kNaN, 10.0, 1.0, 2.0);
    expectRefused(ev::RunExecutionPlanStatus::InvalidAnnouncement,
                  ev::buildRunExecutionPlan(notANumber, measuredBottleConfig(),
                                            measuredExecutionDependencies(), false, 0.0, 0.0));
}

// --- 3. Non-finite input never becomes a valid plan ---------------------------

void test_a_non_finite_established_dependency_is_never_a_valid_plan(void) {
    // Measured provenance does not make a NaN a number: provenance says where a
    // value came from, not that it is one.
    const ev::RunExecutionDependencies nanTolerance(measured(kNaN), measured(0.15));
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            nanTolerance, false, 0.0, 0.0));

    // An infinite forward stop margin would put the forward stop nowhere.
    const ev::RunExecutionDependencies infiniteMargin(measured(0.01), measured(kInf));
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            infiniteMargin, false, 0.0, 0.0));

    // A non-finite bottle-interaction distance is refused by the bottle module's
    // own validation, surfaced under this plan's name.
    const ev::BottleEvidenceConfig nanLead(measured(kNaN), measured(0.05), measured(0.01));
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), nanLead,
                                            measuredExecutionDependencies(), false, 0.0, 0.0));

    const ev::BottleEvidenceConfig infiniteAdvance(measured(0.02), measured(kInf), measured(0.01));
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), infiniteAdvance,
                                            measuredExecutionDependencies(), false, 0.0, 0.0));
}

// --- 4. Missing / unestablished physical dependencies -------------------------

void test_an_unestablished_bottle_dependency_fails_closed_with_a_named_reason(void) {
    const ev::RunExecutionDependencies execution = measuredExecutionDependencies();

    // Each bottle-interaction distance on its own, as a placeholder.
    const ev::BottleEvidenceConfig provisionalLead(provisional(0.02), measured(0.05),
                                                  measured(0.01));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), provisionalLead, execution,
                                            false, 0.0, 0.0));

    const ev::BottleEvidenceConfig provisionalAdvance(measured(0.02), provisional(0.05),
                                                     measured(0.01));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), provisionalAdvance, execution,
                                            false, 0.0, 0.0));

    const ev::BottleEvidenceConfig provisionalPush(measured(0.02), measured(0.05),
                                                  provisional(0.01));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), provisionalPush, execution,
                                            false, 0.0, 0.0));

    // `Derived` is a computed planning value, not a measurement of the mechanism,
    // so it does not authorize the physical positions either.
    const ev::BottleEvidenceConfig derivedLead(derivedValue(0.02), measured(0.05), measured(0.01));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), derivedLead, execution, false,
                                            0.0, 0.0));

    // The credible contact window's own tolerance is part of the bottle
    // interaction, and a placeholder there refuses the same way.
    const ev::RunExecutionDependencies provisionalTolerance(provisional(0.01), measured(0.15));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            provisionalTolerance, false, 0.0, 0.0));
}

void test_an_unestablished_forward_execution_target_fails_closed(void) {
    // The bottle interaction is fully established here; only the forward stop's
    // own physical dependency is missing.
    const ev::BottleEvidenceConfig bottle = measuredBottleConfig();

    const ev::RunExecutionDependencies provisionalMargin(measured(0.01), provisional(0.15));
    expectRefused(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), bottle, provisionalMargin,
                                            false, 0.0, 0.0));

    const ev::RunExecutionDependencies derivedMargin(measured(0.01), derivedValue(0.15));
    expectRefused(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), bottle, derivedMargin, false,
                                            0.0, 0.0));
}

// --- 5. Contradictory interaction geometry ------------------------------------

void test_contradictory_interaction_geometry_is_refused(void) {
    // The capture advance does not cover the credible contact window, so the plan
    // would declare capture complete while contact is still uncertain.
    const ev::BottleEvidenceConfig captureTooEarly(measured(0.02), measured(0.01), measured(0.01));
    const ev::RunExecutionDependencies wideWindow(measured(0.05), measured(0.15));
    expectRefused(ev::RunExecutionPlanStatus::ContradictoryExecutionGeometry,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), captureTooEarly, wideWindow,
                                            false, 0.0, 0.0));

    // The push would not be complete by the commanded forward stop.
    const ev::BottleEvidenceConfig longPush(measured(0.02), measured(0.05), measured(0.10));
    const ev::RunExecutionDependencies shortMargin(measured(0.01), measured(0.05));
    expectRefused(ev::RunExecutionPlanStatus::ContradictoryExecutionGeometry,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), longPush, shortMargin, false,
                                            0.0, 0.0));

    // A negative forward stop margin would command the stop before the Bottle
    // Line - and therefore before capture and the push.
    const ev::RunExecutionDependencies behindTheLine(measured(0.01), measured(-0.5));
    expectRefused(ev::RunExecutionPlanStatus::ContradictoryExecutionGeometry,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            behindTheLine, false, 0.0, 0.0));

    // A credible contact window reaching behind the Start Point (the axis origin)
    // is impossible: the vehicle cannot touch the bottle before it starts.
    const ev::BottleEvidenceConfig noLead(measured(0.0), measured(0.05), measured(0.01));
    const ev::RunExecutionDependencies hugeWindow(measured(3.0), measured(0.15));
    expectRefused(ev::RunExecutionPlanStatus::ContradictoryExecutionGeometry,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), noLead, hugeWindow, false, 0.0,
                                            0.0));
}

// --- 6. The reverse stop is the announced Target Point ------------------------

void test_the_reverse_stop_is_the_announced_target_point(void) {
    // A rule-derived relationship, not a measurement: wherever the announcement
    // puts the Target Point, that is where the reverse leg stops.
    const ev::RunExecutionPlanResult regional = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), false,
        0.0, 0.0);
    expectValid(regional);
    TEST_ASSERT_TRUE(regional.plan.reverseStopMpPositionM ==
                     regional.plan.geometry.targetPointPositionM);
    expectPosition(5.0, regional.plan.reverseStopMpPositionM);

    const ev::RunExecutionPlanResult state = ev::buildRunExecutionPlan(
        stateAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), false, 0.0,
        0.0);
    expectValid(state);
    TEST_ASSERT_TRUE(state.plan.reverseStopMpPositionM ==
                     state.plan.geometry.targetPointPositionM);
    expectPosition(5.20, state.plan.reverseStopMpPositionM);
}

// --- 7. The forward stop follows the execution dependency, not the Bottle Line -

void test_the_forward_stop_is_not_assumed_to_be_the_bottle_line(void) {
    const ev::RunConfig announcement = regionalAnnouncement();
    const ev::BottleEvidenceConfig bottle = measuredBottleConfig();
    const double bottleLineM = ev::RunAxisGeometry::fromRunConfig(announcement).bottleLinePositionM();
    TEST_ASSERT_TRUE(bottleLineM == 6.0);

    // The same announcement and the same bottle interaction, two different
    // measured forward stop margins: the forward stop follows the margin.
    const ev::RunExecutionPlanResult nearMargin = ev::buildRunExecutionPlan(
        announcement, bottle, ev::RunExecutionDependencies(measured(0.01), measured(0.15)),
        false, 0.0, 0.0);
    const ev::RunExecutionPlanResult farMargin = ev::buildRunExecutionPlan(
        announcement, bottle, ev::RunExecutionDependencies(measured(0.01), measured(0.30)),
        false, 0.0, 0.0);
    expectValid(nearMargin);
    expectValid(farMargin);

    expectPosition(6.15, nearMargin.plan.forwardStopMpPositionM);
    expectPosition(6.30, farMargin.plan.forwardStopMpPositionM);
    TEST_ASSERT_TRUE(nearMargin.plan.forwardStopMpPositionM !=
                     farMargin.plan.forwardStopMpPositionM);
    // The forward stop is the Bottle Line *plus the measured margin*, not the
    // Bottle Line: the arithmetic is compared with a tolerance because the native
    // target may hold an intermediate in wider-than-double precision.
    expectPosition(bottleLineM + 0.15, nearMargin.plan.forwardStopMpPositionM);
    expectPosition(bottleLineM + 0.30, farMargin.plan.forwardStopMpPositionM);
    // The rule Bottle Line is unchanged by either margin, so the forward stop is
    // demonstrably not derived from it alone.
    TEST_ASSERT_TRUE(nearMargin.plan.geometry.bottleLinePositionM() == bottleLineM);
    TEST_ASSERT_TRUE(farMargin.plan.geometry.bottleLinePositionM() == bottleLineM);
    TEST_ASSERT_TRUE(nearMargin.plan.forwardStopMpPositionM != bottleLineM);
    TEST_ASSERT_TRUE(farMargin.plan.forwardStopMpPositionM != bottleLineM);
}

// --- 8. Run-specific independence ---------------------------------------------

void test_two_plans_from_two_announcements_are_independent(void) {
    const ev::RunExecutionPlanResult planA = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), false,
        0.0, 0.0);
    expectValid(planA);

    // Snapshot every field of plan A before building plan B.
    const double targetA = planA.plan.geometry.targetPointPositionM;
    const double bottleStartA = planA.plan.geometry.bottleStartPositionM;
    const double earliestA = planA.plan.earliestContactMpPositionM;
    const double latestA = planA.plan.latestContactMpPositionM;
    const double captureA = planA.plan.captureGuaranteedMpPositionM;
    const double pushA = planA.plan.pushCompleteMpPositionM;
    const double forwardA = planA.plan.forwardStopMpPositionM;
    const double reverseA = planA.plan.reverseStopMpPositionM;

    const ev::BottleEvidenceConfig differentBottle(measured(0.03), measured(0.06), measured(0.02));
    const ev::RunExecutionDependencies differentExecution(measured(0.02), measured(0.25));
    const ev::RunExecutionPlanResult planB = ev::buildRunExecutionPlan(
        stateAnnouncement(), differentBottle, differentExecution, false, 0.0, 0.0);
    expectValid(planB);

    // Plan B carries its own geometry and its own dependencies.
    TEST_ASSERT_TRUE(planB.plan.geometry.targetPointPositionM == 5.20);
    TEST_ASSERT_TRUE(planB.plan.geometry.bottleStartPositionM == 3.0);
    expectPosition(2.95, planB.plan.earliestContactMpPositionM);
    expectPosition(2.99, planB.plan.latestContactMpPositionM);
    expectPosition(3.03, planB.plan.captureGuaranteedMpPositionM);
    expectPosition(6.42, planB.plan.pushCompleteMpPositionM);
    expectPosition(6.65, planB.plan.forwardStopMpPositionM);
    expectPosition(5.20, planB.plan.reverseStopMpPositionM);

    // Building plan B must not have mutated plan A - there is no hidden shared or
    // global plan state, and plan A is immutable.
    TEST_ASSERT_TRUE(planA.plan.geometry.targetPointPositionM == targetA);
    TEST_ASSERT_TRUE(planA.plan.geometry.bottleStartPositionM == bottleStartA);
    TEST_ASSERT_TRUE(planA.plan.earliestContactMpPositionM == earliestA);
    TEST_ASSERT_TRUE(planA.plan.latestContactMpPositionM == latestA);
    TEST_ASSERT_TRUE(planA.plan.captureGuaranteedMpPositionM == captureA);
    TEST_ASSERT_TRUE(planA.plan.pushCompleteMpPositionM == pushA);
    TEST_ASSERT_TRUE(planA.plan.forwardStopMpPositionM == forwardA);
    TEST_ASSERT_TRUE(planA.plan.reverseStopMpPositionM == reverseA);
    expectPosition(6.15, planA.plan.forwardStopMpPositionM);
}

// --- 9. Timing readiness is recorded honestly ---------------------------------

void test_timing_readiness_is_recorded_honestly(void) {
    // Production-style fixture: no empirical timing evidence, so the plan carries
    // no predicted motion time and no deliberate delay even when a caller passes
    // the idealized model's numbers.
    const ev::RunExecutionPlanResult notReady = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), false,
        12.5, 3.0);
    expectValid(notReady);
    TEST_ASSERT_EQUAL_INT(0, notReady.plan.timingReady ? 1 : 0);
    expectPosition(0.0, notReady.plan.predictedMotionTimeS);
    expectPosition(0.0, notReady.plan.initialDelayS);

    // A synthetic timing-ready fixture (unmistakably a fixture: no timing evidence
    // exists for this vehicle) carries the numbers it was given.
    const ev::RunExecutionPlanResult ready = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(), measuredExecutionDependencies(), true, 12.5,
        3.0);
    expectValid(ready);
    TEST_ASSERT_EQUAL_INT(1, ready.plan.timingReady ? 1 : 0);
    expectPosition(12.5, ready.plan.predictedMotionTimeS);
    expectPosition(3.0, ready.plan.initialDelayS);

    // A timing-ready plan with a non-finite or negative time is still no plan.
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            measuredExecutionDependencies(), true, kNaN, 0.0));
    expectRefused(ev::RunExecutionPlanStatus::NonFiniteValue,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            measuredExecutionDependencies(), true, 12.5, -1.0));
}

// --- 10. No hidden valid defaults ---------------------------------------------

void test_a_refused_plan_is_never_a_hidden_valid_default(void) {
    // The construction-free refusal: explicitly named, explicitly invalid, and
    // carrying no derived position.
    const ev::RunExecutionPlan refused =
        ev::refusedExecutionPlan(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget);
    TEST_ASSERT_EQUAL_INT(0, refused.valid ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget),
                          static_cast<int>(refused.status));
    expectPosition(0.0, refused.forwardStopMpPositionM);
    expectPosition(0.0, refused.reverseStopMpPositionM);
    expectPosition(0.0, refused.geometry.targetPointPositionM);

    // The result form of the same refusal, through the builder.
    const ev::RunExecutionPlanResult refusedResult = ev::buildRunExecutionPlan(
        regionalAnnouncement(), measuredBottleConfig(),
        ev::RunExecutionDependencies(measured(0.01), provisional(0.15)), false, 0.0, 0.0);
    expectRefused(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget, refusedResult);

    // Every status this module defines spells as a name, so a refusal is always
    // attributable in a log.
    TEST_ASSERT_EQUAL_STRING("Planned",
                             ev::runExecutionPlanStatusName(ev::RunExecutionPlanStatus::Planned));
    TEST_ASSERT_EQUAL_STRING(
        "MissingForwardExecutionTarget",
        ev::runExecutionPlanStatusName(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget));
}

// --- 11. The production configuration cannot yet produce a plan ---------------

void test_the_production_shaped_dependencies_cannot_yet_produce_a_plan(void) {
    // This mirrors the production provenance *by construction*, because the native
    // environment cannot compile `src/` (`SOFTWARE_ARCHITECTURE.md` section 3,
    // `test_build_src = no`):
    //
    //   * `src/VehicleConfig.h` builds every bottle-interaction distance with
    //     `provisionalValue(...)` (`kPlannedBottleContactLeadM`,
    //     `kPlannedBottleCaptureAdvanceM`, `kPlannedBottlePushMarginM` are all
    //     Provisional placeholders), and
    //   * no measured forward-stop margin exists anywhere in the tree: the
    //     forward execution target's physical dependency has never been
    //     established (S4 owns its consumer; P-H8 and the braking characterization
    //     own the measurement).
    //
    // So a production call site today refuses. This test pins that refusal rather
    // than a guessed plan.
    const ev::RunExecutionDependencies productionShapedExecution(provisional(0.0),
                                                                provisional(0.0));
    const ev::BottleEvidenceConfig productionShapedBottle(provisional(0.0), provisional(0.0),
                                                          provisional(0.0));
    expectRefused(ev::RunExecutionPlanStatus::MissingBottleInteractionCalibration,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), productionShapedBottle,
                                            productionShapedExecution, false, 0.0, 0.0));

    // And if the bottle calibration - including the contact window's tolerance -
    // were established tomorrow, the forward execution target would still be
    // unavailable: the refusal is not merely a consequence of the first gate.
    const ev::RunExecutionDependencies noForwardTarget(measured(0.0), provisional(0.0));
    expectRefused(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget,
                  ev::buildRunExecutionPlan(regionalAnnouncement(), measuredBottleConfig(),
                                            noForwardTarget, false, 0.0, 0.0));

    // The same holds for a State-grid announcement: no announcement makes the
    // missing physical facts exist.
    expectRefused(ev::RunExecutionPlanStatus::MissingForwardExecutionTarget,
                  ev::buildRunExecutionPlan(stateAnnouncement(), measuredBottleConfig(),
                                            noForwardTarget, false, 0.0, 0.0));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_a_valid_synthetic_announcement_yields_a_complete_execution_plan);

    RUN_TEST(test_an_illegal_announcement_is_refused_before_any_execution_arithmetic);
    RUN_TEST(test_a_non_finite_established_dependency_is_never_a_valid_plan);
    RUN_TEST(test_an_unestablished_bottle_dependency_fails_closed_with_a_named_reason);
    RUN_TEST(test_an_unestablished_forward_execution_target_fails_closed);
    RUN_TEST(test_contradictory_interaction_geometry_is_refused);

    RUN_TEST(test_the_reverse_stop_is_the_announced_target_point);
    RUN_TEST(test_the_forward_stop_is_not_assumed_to_be_the_bottle_line);
    RUN_TEST(test_two_plans_from_two_announcements_are_independent);

    RUN_TEST(test_timing_readiness_is_recorded_honestly);
    RUN_TEST(test_a_refused_plan_is_never_a_hidden_valid_default);
    RUN_TEST(test_the_production_shaped_dependencies_cannot_yet_produce_a_plan);

    return UNITY_END();
}
