#include <cmath>
#include <cstdint>
#include <limits>

#include <unity.h>

#include <CompetitionStateMachine.h>
#include <MotionProfile.h>
#include <RunAxis.h>
#include <RunExecutionPlan.h>

// Host tests for the diagnostic run timing estimate (lib/evcore/src/MotionProfile.h)
// and the run's delay ownership (S7): what an announced Target Time kinematically
// implies, when the estimate refuses instead, that the run spends its execution
// plan's own `initialDelayS` (bounded by the policy maximum), and the two
// per-direction / ramp-down behaviors the vehicle layer depends on.
//
// Every limit below is a fixture. None is a measured vehicle value and none is a
// tuning suggestion: the vehicle has no measured limits (VERIFIED_STATE.md #8,
// #9), so no firmware path may use these numbers. A passing test says the
// arithmetic and the refusals are deterministic - it is not evidence about any
// vehicle's motion, and it is not evidence that a run finishes near its Target
// Time.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

ev::Sourced<double> announced(double targetTimeS) {
    return ev::Sourced<double>(targetTimeS, ev::ValueSource::RuleDerived, "rules2027 8.i");
}

// A Regional-course planning fixture: 9.50 m of announced travel (5.00 m target
// plus a bottle line the run goes out to and comes back from), a 1.20 m/s ceiling,
// 0.50 m/s^2 of acceleration, 1.00 m/s^2 of deceleration, a 0.10 m/s planned-cruise
// floor, a 0.20 s reversal reserve and a 30 s maximum run duration.
ev::RunTimingPlanInputs planInputs(double travelDistanceM, double targetTimeS) {
    return ev::RunTimingPlanInputs(
        travelDistanceM, announced(targetTimeS), measured(1.20), measured(1.20), measured(0.50),
        measured(1.00), measured(0.10), measured(0.20), measured(30.0));
}

const ev::RunAxisGeometry kRegionalGeometry(5.0, 1.0, 2.0);

void expectRefused(ev::RunTimingPlanStatus expected, const ev::RunTimingPlan& plan) {
    TEST_ASSERT_FALSE(plan.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(plan.status));
    TEST_ASSERT_TRUE(plan.plannedCruiseMps == 0.0);
    TEST_ASSERT_TRUE(plan.plannedDelayS == 0.0);
}

}  // namespace

// The planned travel is the whole announced course, not just the Target Distance:
// the run goes out to the Bottle Line (the Target Point plus the announced Bottle
// Line Distance) and comes back to the Target Point.
void test_the_planned_travel_is_the_whole_announced_course(void) {
    TEST_ASSERT_TRUE(ev::plannedTravelDistanceM(kRegionalGeometry) == 7.0);
    const ev::RunAxisGeometry state(7.5, 1.0, 3.0);
    TEST_ASSERT_TRUE(ev::plannedTravelDistanceM(state) == 9.5);
}

// The whole point of the plan: a *different* announced Target Time produces a
// different planned motion. A tighter budget plans a faster cruise, and the tight
// budget moves the plan to the ceiling rather than over it.
void test_a_different_target_time_plans_a_different_cruise(void) {
    const ev::RunTimingPlan relaxed = ev::planRunTiming(planInputs(9.50, 20.0));
    const ev::RunTimingPlan tight = ev::planRunTiming(planInputs(9.50, 12.0));
    const ev::RunTimingPlan tighter = ev::planRunTiming(planInputs(9.50, 11.0));

    TEST_ASSERT_TRUE(relaxed.valid);
    TEST_ASSERT_TRUE(tight.valid);
    TEST_ASSERT_TRUE(tighter.valid);
    TEST_ASSERT_TRUE(relaxed.plannedCruiseMps < tight.plannedCruiseMps);
    TEST_ASSERT_TRUE(tight.plannedCruiseMps < tighter.plannedCruiseMps);

    // The ceiling is a hard bound: the tightest admissible plan never plans more
    // than the safety/strategy ceiling, at any announced time.
    TEST_ASSERT_TRUE(tighter.plannedCruiseMps <= 1.20);

    // And what the budget has left over is spent waiting rather than by crawling:
    // the generous announcement plans a deliberate delay and the tight ones do not.
    TEST_ASSERT_TRUE(relaxed.plannedDelayS > 0.0);
    TEST_ASSERT_TRUE(relaxed.intendedTotalS <= 20.0);
    TEST_ASSERT_TRUE(tight.plannedDelayS == 0.0);
    TEST_ASSERT_TRUE(tighter.plannedDelayS == 0.0);
}

// A Target Time shorter than the fastest admissible travel is refused by name. It
// is not met by exceeding a limit and it is not silently rounded up to one that
// would fit.
void test_an_impossible_target_time_is_refused(void) {
    expectRefused(ev::RunTimingPlanStatus::TargetTimeTooShort,
                  ev::planRunTiming(planInputs(9.50, 1.0)));

    // The fastest admissible travel for this fixture is a shade under 10 s, so
    // 10.0 s still fits and 9.0 s does not.
    TEST_ASSERT_TRUE(ev::planRunTiming(planInputs(9.50, 10.0)).valid);
    expectRefused(ev::RunTimingPlanStatus::TargetTimeTooShort,
                  ev::planRunTiming(planInputs(9.50, 9.0)));
}

// The planned-cruise floor is a floor: a very generous announcement plans the
// floor speed (never lower) and spends the rest of the time as a deliberate delay,
// which is the honest way to spend slack rather than asking the drivetrain to
// travel at a speed it may not move at.
void test_the_floor_takes_over_and_the_slack_becomes_delay(void) {
    const ev::RunTimingPlan plan = ev::planRunTiming(planInputs(9.50, 20.0));
    TEST_ASSERT_TRUE(plan.valid);
    TEST_ASSERT_TRUE(plan.plannedCruiseMps >= 0.10);
    TEST_ASSERT_TRUE(std::fabs(plan.intendedTotalS - 20.0) < 1.0e-6);
    TEST_ASSERT_TRUE(plan.marginS >= 0.0);

    // A floor above the planning ceiling is an inconsistent configuration, not
    // something to clamp silently.
    const ev::RunTimingPlanInputs floorAboveCeiling(
        9.50, announced(20.0), measured(1.20), measured(1.20), measured(0.50), measured(1.00),
        measured(2.00), measured(0.20), measured(30.0));
    expectRefused(ev::RunTimingPlanStatus::LimitsAreNotUsable,
                  ev::planRunTiming(floorAboveCeiling));
}

// A plan that would need an unreasonable wait is refused: the maximum run duration
// is a limit like any other, and the delay is what makes it reachable.
void test_a_plan_longer_than_the_maximum_run_duration_is_refused(void) {
    const ev::RunTimingPlanInputs shortRun(
        9.50, announced(20.0), measured(1.20), measured(1.20), measured(0.50), measured(1.00),
        measured(0.10), measured(0.20), measured(15.0));
    expectRefused(ev::RunTimingPlanStatus::PlanExceedsMaxRunDuration,
                  ev::planRunTiming(shortRun));
}

// No plan rests on a placeholder: every limit it uses must be measured, while the
// announced Target Time is a rule parameter and is carried as such.
void test_a_placeholder_limit_refuses_the_plan(void) {
    const ev::RunTimingPlanInputs provisionalAccel(
        9.50, announced(20.0), measured(1.20), measured(1.20), provisional(0.50), measured(1.00),
        measured(0.10), measured(0.20), measured(30.0));
    expectRefused(ev::RunTimingPlanStatus::LimitsAreNotUsable,
                  ev::planRunTiming(provisionalAccel));

    const ev::RunTimingPlanInputs badTime(
        9.50, announced(0.0), measured(1.20), measured(1.20), measured(0.50), measured(1.00),
        measured(0.10), measured(0.20), measured(30.0));
    expectRefused(ev::RunTimingPlanStatus::InvalidTargetTime, ev::planRunTiming(badTime));

    const ev::RunTimingPlanInputs noGeometry(
        0.0, announced(20.0), measured(1.20), measured(1.20), measured(0.50), measured(1.00),
        measured(0.10), measured(0.20), measured(30.0));
    expectRefused(ev::RunTimingPlanStatus::InvalidGeometry, ev::planRunTiming(noGeometry));
}

// The plan's travel estimate is the profile's own arithmetic, so a run's reported
// duration and the shaping layer's estimate cannot disagree about the same trip.
void test_the_plan_agrees_with_the_profile_duration(void) {
    const ev::RunTimingPlan plan = ev::planRunTiming(planInputs(9.50, 20.0));
    TEST_ASSERT_TRUE(plan.valid);
    const double durationS = ev::travelDurationS(9.50, plan.plannedCruiseMps, 0.50, 1.00);
    TEST_ASSERT_TRUE(std::fabs(durationS - plan.travelDurationS) < 1.0e-12);

    const double naive = ev::travelDurationS(9.50, 0.50, 0.50, 1.00);
    TEST_ASSERT_TRUE(ev::motionDurationS(9.50, measured(0.50), measured(0.50), measured(1.00))
                         .durationS == naive);
    TEST_ASSERT_TRUE(ev::motionDurationS(9.50, provisional(0.50), measured(0.50), measured(1.00))
                         .status == ev::MotionDurationStatus::LimitsAreNotUsable);
    TEST_ASSERT_TRUE(std::isnan(ev::travelDurationS(9.50, -1.0, 0.50, 1.00)));
}

// The two legs carry their own measured brake offsets, and the selection is a
// function of the direction: the forward offset can never be attached to a reverse
// leg by a caller that forgot which leg it was building.
void test_the_leg_limits_carry_that_legs_own_brake_offset(void) {
    const ev::MotionProfileLimits forward(
        measured(0.50), measured(1.20), measured(0.50), measured(1.00), measured(0.05),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Measured));
    const ev::MotionProfileLimits reverse(
        measured(0.50), measured(1.20), measured(0.50), measured(1.00), measured(0.40),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Measured));
    const ev::DirectionalMotionProfileLimits legs(forward, reverse);

    TEST_ASSERT_TRUE(legs.forDirection(ev::MotionDirection::Forward).brakeOffsetM.value == 0.05);
    TEST_ASSERT_TRUE(legs.forDirection(ev::MotionDirection::Reverse).brakeOffsetM.value == 0.40);

    // And the profile really uses the offset it was handed: at the same remaining
    // distance, the leg whose measured stopping distance already reaches the
    // target point asks for no travel while the other leg still shapes a target.
    ev::MotionProfileTarget target;
    const ev::MotionProfileCommand forwardAt300mm = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 0.30, 20U),
        legs.forDirection(ev::MotionDirection::Forward));
    TEST_ASSERT_TRUE(forwardAt300mm.status == ev::MotionProfileStatus::Ok);
    TEST_ASSERT_TRUE(forwardAt300mm.targetVelocityMps > 0.0);
    const ev::MotionProfileCommand reverseAt300mm = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Reverse, 0.30, 20U),
        legs.forDirection(ev::MotionDirection::Reverse));
    TEST_ASSERT_TRUE(reverseAt300mm.status == ev::MotionProfileStatus::WithinBrakeOffset);
    TEST_ASSERT_TRUE(reverseAt300mm.targetVelocityMps == 0.0);
}

// The builder's own selection step, exercised with a deliberately unequal fixture
// pair. `ev::brakeOffsetForDirection()` is the function
// `vehicleConfig::profileLimitsForOneDirection()` calls to fill `brakeOffsetM`, so
// this is the exact expression the historical defect lived in ("both legs used the
// forward brake offset"): if the reverse leg were built from the forward value
// again, the reverse assertions below fail while the forward ones still pass.
//
// The pair is a fixture, not a vehicle measurement. Production's
// `kPlannedForwardBrakeOffsetM` and `kPlannedReverseBrakeOffsetM` are currently
// numerically equal, which is exactly why the selection is tested through a
// deliberately unequal pair rather than through those constants - a test on two
// equal values could not tell the two selections apart.
void test_the_builder_selects_the_brake_offset_by_direction(void) {
    const ev::Sourced<double> forwardOffset(0.011, ev::ValueSource::Provisional);
    const ev::Sourced<double> reverseOffset(0.077, ev::ValueSource::Provisional);

    const ev::Sourced<double> forward = ev::brakeOffsetForDirection(
        ev::MotionDirection::Forward, forwardOffset, reverseOffset);
    const ev::Sourced<double> reverse = ev::brakeOffsetForDirection(
        ev::MotionDirection::Reverse, forwardOffset, reverseOffset);

    TEST_ASSERT_TRUE(forward.value == 0.011);
    TEST_ASSERT_TRUE(reverse.value == 0.077);

    // The pair's roles are positional: a swapped pair swaps the answer, so the two
    // arguments cannot be transposed and still pass.
    TEST_ASSERT_TRUE(ev::brakeOffsetForDirection(ev::MotionDirection::Forward, reverseOffset,
                                                 forwardOffset)
                         .value == 0.077);
    TEST_ASSERT_TRUE(ev::brakeOffsetForDirection(ev::MotionDirection::Reverse, reverseOffset,
                                                 forwardOffset)
                         .value == 0.011);

    // Provenance rides along with the value: the chosen Sourced object is returned
    // whole, so a leg receives the source as well as the number. The citation here
    // is a synthetic host fixture and is never evidence about the vehicle.
    const ev::Sourced<double> citedFixture(
        0.011, ev::ValueSource::Measured,
        "synthetic host fixture; not a vehicle measurement");
    TEST_ASSERT_TRUE(ev::brakeOffsetForDirection(ev::MotionDirection::Forward, citedFixture,
                                                 reverseOffset)
                         .isMeasured());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ValueSource::Provisional),
                          static_cast<int>(ev::brakeOffsetForDirection(
                                               ev::MotionDirection::Reverse, citedFixture,
                                               reverseOffset)
                                               .source));
}

// A ramp-down is a regulated target being brought to zero, not a dead stop: it is
// classified apart from a stop so the vehicle layer keeps regulating toward the
// shaped target, while the stop mapping itself still presents no powered effort.
void test_a_ramp_down_is_classified_apart_from_a_stop(void) {
    TEST_ASSERT_TRUE(ev::isRunRampDownCommand(ev::RunMotionCommand::RampDownForward));
    TEST_ASSERT_TRUE(ev::isRunRampDownCommand(ev::RunMotionCommand::RampDownReverse));
    TEST_ASSERT_FALSE(ev::isRunRampDownCommand(ev::RunMotionCommand::BrakeForward));
    TEST_ASSERT_FALSE(ev::isRunRampDownCommand(ev::RunMotionCommand::BrakeReverse));
    TEST_ASSERT_FALSE(ev::isRunRampDownCommand(ev::RunMotionCommand::DriveForward));
    TEST_ASSERT_FALSE(ev::isRunRampDownCommand(ev::RunMotionCommand::None));

    // Both are still stop commands - the leg tag and the stop mapping are
    // unchanged - so a ramp is never mistaken for a travel.
    TEST_ASSERT_TRUE(ev::isRunStopCommand(ev::RunMotionCommand::RampDownForward));
    TEST_ASSERT_TRUE(ev::isRunStopCommand(ev::RunMotionCommand::BrakeForward));
    TEST_ASSERT_FALSE(ev::isRunTravelCommand(ev::RunMotionCommand::RampDownForward));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::RampDownForward).mode));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::MotorMode::Brake),
        static_cast<int>(ev::runStopEffort(ev::RunMotionCommand::BrakeForward).mode));
}

// The profile ramps the target down at the deceleration limit and never crosses
// zero, which is what makes the regulated ramp-down safe: the target a ramp
// presents is always on the travel side of zero.
void test_a_stop_request_ramps_the_target_down_without_crossing_zero(void) {
    const ev::MotionProfileLimits limits(
        measured(0.50), measured(1.20), measured(0.50), measured(1.00), measured(0.05),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Measured));
    ev::MotionProfileTarget target;
    const ev::MotionProfileCommand cruise = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limits);
    target.accept(cruise);
    TEST_ASSERT_TRUE(target.targetVelocityMps() > 0.0);

    // Two deceleration steps at 1.00 m/s^2 over 20 ms each: 0.04 m/s of the target
    // comes off per step, so it takes several steps to reach zero and the sign
    // never flips.
    for (int step = 0; step < 20; ++step) {
        const ev::MotionProfileCommand ramped = ev::nextTargetVelocity(
            target.inputsFor(ev::MotionProfileIntent::Stop, ev::MotionDirection::Forward, 5.0, 20U),
            limits);
        target.accept(ramped);
        TEST_ASSERT_TRUE(target.targetVelocityMps() >= 0.0);
    }
    TEST_ASSERT_TRUE(target.targetVelocityMps() == 0.0);
}

// D35: the plan's cruise output is Derived, never Measured. A cited Derived
// cruise shapes a target; an uncited Derived or a placeholder does not, and the
// plan's inputs still require Measured (a Derived input is refused).
ev::Sourced<double> derivedCruise(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Derived,
                               "derived: planRunTiming() from measured limits and the announced "
                               "Target Time (rules2027 8.i)");
}

ev::MotionProfileLimits limitsWithCruise(ev::Sourced<double> cruise) {
    return ev::MotionProfileLimits(
        cruise, measured(1.20), measured(0.50), measured(1.00), measured(0.05),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Measured));
}

void test_a_computed_cruise_is_derived_never_measured(void) {
    const ev::Sourced<double> cruise = derivedCruise(0.50);
    TEST_ASSERT_FALSE(cruise.isMeasured());
    TEST_ASSERT_TRUE(cruise.isDerived());
    TEST_ASSERT_TRUE(cruise.citesEvidence());
    TEST_ASSERT_TRUE(ev::cruiseProvenanceIsEstablished(cruise));

    // Without a citation it is refused exactly like a placeholder.
    const ev::Sourced<double> uncited(0.50, ev::ValueSource::Derived);
    TEST_ASSERT_FALSE(uncited.citesEvidence());
    TEST_ASSERT_FALSE(ev::cruiseProvenanceIsEstablished(uncited));
    TEST_ASSERT_FALSE(provisional(0.50).isDerived());
    TEST_ASSERT_FALSE(ev::cruiseProvenanceIsEstablished(provisional(0.50)));

    // A genuinely measured cruise still passes, so the old path is not narrowed.
    TEST_ASSERT_TRUE(ev::cruiseProvenanceIsEstablished(measured(0.50)));
}

void test_profile_limits_accept_a_cited_derived_cruise_only(void) {
    TEST_ASSERT_TRUE(ev::validateMotionProfileLimits(limitsWithCruise(derivedCruise(0.50))).ok);

    // The profile really shapes from it: a travel request produces a target.
    ev::MotionProfileTarget target;
    const ev::MotionProfileCommand shaped = ev::nextTargetVelocity(
        target.inputsFor(ev::MotionProfileIntent::Travel, ev::MotionDirection::Forward, 5.0, 20U),
        limitsWithCruise(derivedCruise(0.50)));
    TEST_ASSERT_TRUE(shaped.status == ev::MotionProfileStatus::Ok);
    TEST_ASSERT_TRUE(shaped.targetVelocityMps > 0.0);

    // Uncited Derived and Provisional cruises are refused.
    TEST_ASSERT_FALSE(ev::validateMotionProfileLimits(
                          limitsWithCruise(ev::Sourced<double>(0.50, ev::ValueSource::Derived)))
                          .ok);
    TEST_ASSERT_FALSE(
        ev::validateMotionProfileLimits(limitsWithCruise(provisional(0.50))).ok);

    // Other limits remain Measured-only: a Derived brake offset is not accepted.
    const ev::MotionProfileLimits derivedOffset(
        measured(0.50), measured(1.20), measured(0.50), measured(1.00),
        ev::Sourced<double>(0.05, ev::ValueSource::Derived,
                            "derived: not a real derivation"),
        ev::Sourced<ev::MotionDirection>(ev::MotionDirection::Forward,
                                         ev::ValueSource::Measured));
    TEST_ASSERT_FALSE(ev::validateMotionProfileLimits(derivedOffset).ok);
}

void test_plan_inputs_still_require_measured_limits(void) {
    // A Derived cruise ceiling is not a measured input: the plan refuses it.
    const ev::RunTimingPlanInputs derivedCeiling(
        9.50, announced(20.0), derivedCruise(1.20), measured(1.20), measured(0.50),
        measured(1.00), measured(0.10), measured(0.20), measured(30.0));
    expectRefused(ev::RunTimingPlanStatus::LimitsAreNotUsable,
                  ev::planRunTiming(derivedCeiling));
}

// The run's delay has one owner - the run's immutable execution plan - and the
// policy's `maximumOptionalDelayS` is only the legal/safety maximum that bounds
// it. Two synthetic plans (fixtures, not measurements) state 1.0 s and 1.5 s: the
// over-maximum one cannot be announced, the other is the value the run spends,
// and the OptionalDelay phase ends exactly when that value has elapsed. Nothing
// reads a policy value as a delay.
void test_the_run_spends_the_execution_plans_delay_bounded_by_the_policy_maximum(void) {
    const ev::RunConfig run(ev::TournamentLevel::RegionalInvitational, 5.0, 10.0, 1.0, 2.0);
    const ev::BottleEvidenceConfig bottle(measured(0.10), measured(0.20), measured(0.05));
    const ev::RunExecutionDependencies deps(measured(0.01), measured(0.20));
    const ev::RunExecutionPlan planWithinBound =
        ev::buildRunExecutionPlan(run, bottle, deps, true, 12.0, 1.0).plan;
    const ev::RunExecutionPlan planOverBound =
        ev::buildRunExecutionPlan(run, bottle, deps, true, 12.0, 1.5).plan;
    TEST_ASSERT_TRUE(planWithinBound.valid);
    TEST_ASSERT_TRUE(planOverBound.valid);
    TEST_ASSERT_TRUE(planWithinBound.initialDelayS == 1.0);
    TEST_ASSERT_TRUE(planOverBound.initialDelayS == 1.5);

    const ev::RunPhaseConfig config(
        measured(1.0), measured(5.0), measured(1.0), measured(5.0), measured(5.0), measured(5.0),
        measured(5.0), measured(1.0),
        ev::NearZeroConfig(measured(0.05), measured(0.2),
                           ev::Sourced<uint32_t>(250U, ev::ValueSource::Measured)),
        measured(0.10), measured(0.30), measured(0.30), measured(0.30), bottle);
    const ev::ReversalConfig reversal(
        ev::NearZeroConfig(measured(0.05), measured(0.2),
                           ev::Sourced<uint32_t>(250U, ev::ValueSource::Measured)),
        measured(3.0));
    ev::CompetitionStateMachine machine(config, reversal);

    // Over the policy maximum, and negative, are both refused before anything is
    // sequenced; neither latches anything.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunStartStatus::InitialDelayNotUsable),
        static_cast<int>(machine.announceRun(run, ev::validateRunConfig(run),
                                             planOverBound.initialDelayS, 1000U)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunStartStatus::InitialDelayNotUsable),
        static_cast<int>(machine.announceRun(run, ev::validateRunConfig(run), -0.5, 1000U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Idle),
                          static_cast<int>(machine.phase()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::None),
                          static_cast<int>(machine.faultCode()));

    // The plan's own value is the delay the run is announced with and spends.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ev::RunStartStatus::Started),
        static_cast<int>(machine.announceRun(run, ev::validateRunConfig(run),
                                             planWithinBound.initialDelayS, 1000U)));
    TEST_ASSERT_TRUE(machine.initialDelayS() == planWithinBound.initialDelayS);

    // And it is spent before travel: the phase ends exactly at the plan's value,
    // not at the policy maximum it was bounded by.
    ev::RunInputs inputs;
    inputs.authorityArmed = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                          static_cast<int>(machine.update(1020U, inputs).phase));
    inputs.startTriggered = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::OptionalDelay),
                          static_cast<int>(machine.update(1040U, inputs).phase));
    inputs.startTriggered = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::OptionalDelay),
                          static_cast<int>(machine.update(1999U, inputs).phase));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Approach),
                          static_cast<int>(machine.update(2040U, inputs).phase));

    // And a new-run boundary clears it: no per-run delay survives a reset.
    machine.reset(3000U);
    TEST_ASSERT_TRUE(machine.initialDelayS() == 0.0);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_the_planned_travel_is_the_whole_announced_course);
    RUN_TEST(test_a_different_target_time_plans_a_different_cruise);
    RUN_TEST(test_an_impossible_target_time_is_refused);
    RUN_TEST(test_the_floor_takes_over_and_the_slack_becomes_delay);
    RUN_TEST(test_a_plan_longer_than_the_maximum_run_duration_is_refused);
    RUN_TEST(test_a_placeholder_limit_refuses_the_plan);
    RUN_TEST(test_the_plan_agrees_with_the_profile_duration);
    RUN_TEST(test_the_leg_limits_carry_that_legs_own_brake_offset);
    RUN_TEST(test_the_builder_selects_the_brake_offset_by_direction);
    RUN_TEST(test_a_ramp_down_is_classified_apart_from_a_stop);
    RUN_TEST(test_a_stop_request_ramps_the_target_down_without_crossing_zero);
    RUN_TEST(test_the_run_spends_the_execution_plans_delay_bounded_by_the_policy_maximum);
    RUN_TEST(test_a_computed_cruise_is_derived_never_measured);
    RUN_TEST(test_profile_limits_accept_a_cited_derived_cruise_only);
    RUN_TEST(test_plan_inputs_still_require_measured_limits);

    return UNITY_END();
}
