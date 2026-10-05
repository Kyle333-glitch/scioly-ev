#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <unity.h>

#include <OperatorPolicy.h>

// Contract tests for the operator policy (lib/evcore/src/OperatorPolicy.h): which
// command may be applied in which state, what a held configuration means under
// the event rules, what an arming intent reads, and how a requested telemetry
// rate maps onto control ticks.
//
// This is a decision test, not an effect test. A pass means the policy draws the
// right line between "allowed" and "refused" for the state it was handed - it says
// nothing about whether the vehicle layer then applies an allowed command, and
// nothing about the vehicle moving.

void setUp(void) {}
void tearDown(void) {}

namespace {

const ev::RunPhase kAllPhases[] = {
    ev::RunPhase::Idle,          ev::RunPhase::ParameterValid, ev::RunPhase::Armed,
    ev::RunPhase::OptionalDelay, ev::RunPhase::Approach,       ev::RunPhase::Capture,
    ev::RunPhase::Push,          ev::RunPhase::ForwardDecel,   ev::RunPhase::BrakeZeroSpeed,
    ev::RunPhase::Reverse,       ev::RunPhase::FinalApproach,  ev::RunPhase::FinalBrake,
    ev::RunPhase::Done,          ev::RunPhase::Fault,
};
const uint8_t kAllPhaseCount = static_cast<uint8_t>(sizeof(kAllPhases) / sizeof(kAllPhases[0]));

// A valid State-level configuration: 0.10 m grid, 1.00 m Bottle Line Distance.
ev::RunConfigFields validStateConfig() {
    ev::RunConfigFields fields;
    fields.hasLevel = true;
    fields.levelIndex = 1U;
    fields.hasTargetDistanceM = true;
    fields.targetDistanceM = 7.50;
    fields.hasTargetTimeS = true;
    fields.targetTimeS = 15.0;
    fields.hasBottleLineDistanceM = true;
    fields.bottleLineDistanceM = 1.00;
    return fields;
}

ev::ConfigurationBuildChecks allChecksPassing() {
    ev::ConfigurationBuildChecks checks;
    for (uint8_t index = 0U; index < ev::kConfigurationCheckCount; ++index) {
        checks.setPassed(static_cast<ev::ConfigurationCheckId>(index), true);
    }
    return checks;
}

ev::Sourced<double> measuredValue(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

// The plan the vehicle layer builds for the fixture configuration, through the
// production arithmetic (`planRunTiming()`) rather than a hand-built struct: State
// level with a 7.50 m target and a 1.00 m Bottle Line Distance means 9.50 m of
// announced travel (rule 8.j puts the Bottle Line a bottle-line beyond the Target
// Point and the run comes back), and 15.0 s is an announced time those limits can
// meet.
ev::RunTimingPlan planForTargetTime(double targetTimeS) {
    return ev::planRunTiming(ev::RunTimingPlanInputs(
        9.50, ev::Sourced<double>(targetTimeS, ev::ValueSource::RuleDerived, "rules2027 8.i"),
        measuredValue(1.20), measuredValue(1.20), measuredValue(0.50), measuredValue(1.00),
        measuredValue(0.10), measuredValue(0.20), measuredValue(30.0)));
}

// Unity's double assertions are compiled out of this test build, so a value is
// compared through its difference. The tolerance is far below what the rules
// distinguish and far above the rounding of a double.
void expectValue(double expected, double actual) {
    TEST_ASSERT_TRUE(fabs(expected - actual) <= 1.0e-9);
}

ev::OperatorSystemState stateOf(ev::RunPhase phase) {
    ev::OperatorSystemState state;
    state.phase = phase;
    return state;
}

void expectAdmissible(ev::OperatorCommandId id, const ev::OperatorSystemState& state) {
    const ev::CommandAdmissibility verdict = ev::admissibilityOf(id, state);
    TEST_ASSERT_TRUE_MESSAGE(verdict.allowed, ev::operatorCommandName(id));
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::CommandRefusal::None),
                                  static_cast<int>(verdict.refusal),
                                  ev::operatorCommandName(id));
}

void expectRefused(ev::OperatorCommandId id, const ev::OperatorSystemState& state,
                   ev::CommandRefusal expected) {
    const ev::CommandAdmissibility verdict = ev::admissibilityOf(id, state);
    TEST_ASSERT_FALSE_MESSAGE(verdict.allowed, ev::operatorCommandName(id));
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(verdict.refusal),
                                  ev::operatorCommandName(id));
}

}  // namespace

// ---------------------------------------------------------------------------
// The held configuration
// ---------------------------------------------------------------------------

// An index names one of the three levels, and an index that names nothing falls
// back to the first level rather than to an undefined value. The index can only
// come from the parser's own table, so this is defence against a caller bug.
void test_tournament_level_mapping_is_total(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TournamentLevel::RegionalInvitational),
                          static_cast<int>(ev::tournamentLevelForIndex(0U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TournamentLevel::State),
                          static_cast<int>(ev::tournamentLevelForIndex(1U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TournamentLevel::National),
                          static_cast<int>(ev::tournamentLevelForIndex(2U)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TournamentLevel::RegionalInvitational),
                          static_cast<int>(ev::tournamentLevelForIndex(200U)));
}

// The Bottle Start Point is derived from the level (rule 7.c.ii.(5)), so it
// cannot be typed and cannot disagree with the level it belongs to.
void test_completed_configuration_derives_the_bottle_start_from_the_level(void) {
    for (uint8_t index = 0U; index < ev::kTournamentLevelCount; ++index) {
        ev::RunConfigFields fields = validStateConfig();
        fields.levelIndex = index;
        const ev::RunConfig config = ev::completedRunConfig(fields);
        expectValue(ev::bottleStartDistanceM(ev::tournamentLevelForIndex(index)),
                    config.bottleStartDistanceM);
        expectValue(fields.targetDistanceM, config.targetDistanceM);
        expectValue(fields.targetTimeS, config.targetTimeS);
        expectValue(fields.bottleLineDistanceM, config.bottleLineDistanceM);
    }
}

// Three separate facts: is the configuration filled in, do the rules accept it,
// and which rule error is it. An incomplete configuration is never "valid so
// far".
void test_held_configuration_verdict_reports_all_three_facts(void) {
    ev::RunConfigFields incomplete;
    incomplete.hasLevel = true;
    incomplete.levelIndex = 1U;
    ev::HeldRunConfigVerdict verdict = ev::heldRunConfigVerdict(incomplete);
    TEST_ASSERT_FALSE(verdict.complete);
    TEST_ASSERT_FALSE(verdict.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None),
                          static_cast<int>(verdict.error));

    ev::RunConfigFields invalid = validStateConfig();
    invalid.targetDistanceM = 3.00;  // below the 5.00 m minimum (rule 8.h)
    verdict = ev::heldRunConfigVerdict(invalid);
    TEST_ASSERT_TRUE(verdict.complete);
    TEST_ASSERT_FALSE(verdict.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::TargetDistanceOutOfRange),
                          static_cast<int>(verdict.error));

    const ev::RunConfigFields valid = validStateConfig();
    verdict = ev::heldRunConfigVerdict(valid);
    TEST_ASSERT_TRUE(verdict.complete);
    TEST_ASSERT_TRUE(verdict.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None),
                          static_cast<int>(verdict.error));
}

// A field supplied to `config set` is judged before it is stored: an
// out-of-range or non-finite value is refused with the rule error it broke, and
// nothing about the *held* configuration changes (finding F-5 - a held value is
// echoed back by `config show`, so it cannot be told apart from a value the
// operator meant).
void test_supplied_fields_are_checked_against_the_rules_before_storage(void) {
    // A complete, rules-valid configuration passes field by field.
    const ev::RunConfigValidation valid = ev::validateSuppliedConfigFields(validStateConfig());
    TEST_ASSERT_TRUE(valid.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None),
                          static_cast<int>(valid.error));

    // An empty `config set` supplies nothing, so there is nothing to refuse.
    const ev::RunConfigValidation empty = ev::validateSuppliedConfigFields(ev::RunConfigFields{});
    TEST_ASSERT_TRUE(empty.ok);

    // One field at a time, out of range in both directions and off the real
    // number line. Each is refused as the rule it broke, not as a generic error.
    struct Case {
        ev::RunConfigFields fields;
        ev::ConfigError expected;
    };
    Case cases[6];

    cases[0].fields = validStateConfig();
    cases[0].fields.targetDistanceM = 9.50;  // above the 9.00 m maximum (rule 8.h)
    cases[0].expected = ev::ConfigError::TargetDistanceOutOfRange;

    cases[1].fields = validStateConfig();
    cases[1].fields.targetDistanceM = 4.99;  // below the 5.00 m minimum
    cases[1].expected = ev::ConfigError::TargetDistanceOutOfRange;

    cases[2].fields = validStateConfig();
    cases[2].fields.targetTimeS = 25.0;  // above the 20.0 s maximum (rule 8.i)
    cases[2].expected = ev::ConfigError::TargetTimeOutOfRange;

    cases[3].fields = validStateConfig();
    cases[3].fields.bottleLineDistanceM = 0.50;  // below the 1.00 m minimum (rule 8.j)
    cases[3].expected = ev::ConfigError::BottleLineDistanceOutOfRange;

    cases[4].fields = ev::RunConfigFields{};
    cases[4].fields.hasTargetDistanceM = true;
    cases[4].fields.targetDistanceM = std::numeric_limits<double>::infinity();
    cases[4].expected = ev::ConfigError::NonFiniteValue;

    cases[5].fields = ev::RunConfigFields{};
    cases[5].fields.hasLevel = true;
    cases[5].fields.levelIndex = ev::kTournamentLevelCount;  // no such level
    cases[5].expected = ev::ConfigError::InvalidTournamentLevel;

    for (uint8_t index = 0U; index < 6U; ++index) {
        const ev::RunConfigValidation verdict =
            ev::validateSuppliedConfigFields(cases[index].fields);
        TEST_ASSERT_FALSE(verdict.ok);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(cases[index].expected),
                              static_cast<int>(verdict.error));
    }

    // Boundary values are inside the rules: the check is the rule range, not a
    // stricter test invented here.
    ev::RunConfigFields atMinimums;
    atMinimums.hasTargetDistanceM = true;
    atMinimums.targetDistanceM = ev::rules2027::kTargetDistanceMinM;
    atMinimums.hasTargetTimeS = true;
    atMinimums.targetTimeS = ev::rules2027::kTargetTimeMinS;
    atMinimums.hasBottleLineDistanceM = true;
    atMinimums.bottleLineDistanceM = ev::rules2027::kBottleLineDistanceMinM;
    TEST_ASSERT_TRUE(ev::validateSuppliedConfigFields(atMinimums).ok);

    ev::RunConfigFields atMaximums;
    atMaximums.hasTargetDistanceM = true;
    atMaximums.targetDistanceM = ev::rules2027::kTargetDistanceMaxM;
    atMaximums.hasTargetTimeS = true;
    atMaximums.targetTimeS = ev::rules2027::kTargetTimeMaxS;
    atMaximums.hasBottleLineDistanceM = true;
    atMaximums.bottleLineDistanceM = ev::rules2027::kBottleLineDistanceMaxM;
    TEST_ASSERT_TRUE(ev::validateSuppliedConfigFields(atMaximums).ok);
}

// The per-field check is deliberately not the whole rule set: the announced grid
// and the cross-field geometry need the level and the rest of the configuration,
// so a value that is in range but off the level's grid is still a value the door
// accepts and the single rule entry point rejects. This is the boundary between
// the two, and it is asserted so neither check is assumed to cover the other.
void test_in_range_but_off_grid_is_left_to_the_whole_configuration_check(void) {
    ev::RunConfigFields offGrid = validStateConfig();
    offGrid.targetDistanceM = 7.53;  // State's grid is 0.10 m (rule 8.h)
    TEST_ASSERT_TRUE(ev::validateSuppliedConfigFields(offGrid).ok);

    const ev::HeldRunConfigVerdict held = ev::heldRunConfigVerdict(offGrid);
    TEST_ASSERT_TRUE(held.complete);
    TEST_ASSERT_FALSE(held.rulesOk);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::TargetDistanceOffInterval),
                          static_cast<int>(held.error));
}

// The rule geometry the run will use is reported, not left for the operator to
// reconstruct: the Target Point, the announced Bottle Line Distance, the Bottle
// Start Point the level fixes, and the Bottle Line rule landmark the first two
// define.
void test_run_axis_geometry_matches_the_configuration(void) {
    const ev::RunConfigFields fields = validStateConfig();
    const ev::RunAxisGeometry geometry = ev::runAxisGeometryFor(fields);

    expectValue(fields.targetDistanceM, geometry.targetPointPositionM);
    expectValue(fields.bottleLineDistanceM, geometry.bottleLineDistanceM);
    expectValue(ev::bottleStartDistanceM(ev::TournamentLevel::State),
                geometry.bottleStartPositionM);
    expectValue(fields.targetDistanceM + fields.bottleLineDistanceM,
                geometry.bottleLinePositionM());
}

// ---------------------------------------------------------------------------
// State restriction
// ---------------------------------------------------------------------------

// Reading state never changes run behavior, so no phase gates it. Telemetry
// control is the stream's own state, not the run's, so it is in the same class.
void test_inspection_and_telemetry_are_allowed_in_every_state(void) {
    const ev::OperatorCommandId inspection[] = {
        ev::OperatorCommandId::Help,        ev::OperatorCommandId::Status,
        ev::OperatorCommandId::Version,     ev::OperatorCommandId::Checks,
        ev::OperatorCommandId::Boot,
        ev::OperatorCommandId::ConfigShow,  ev::OperatorCommandId::ConfigCheck,
        ev::OperatorCommandId::TelemetryOn, ev::OperatorCommandId::TelemetryOff,
        ev::OperatorCommandId::TelemetryStatus,
        ev::OperatorCommandId::TelemetryRate,
    };
    const uint8_t inspectionCount =
        static_cast<uint8_t>(sizeof(inspection) / sizeof(inspection[0]));

    for (uint8_t phaseIndex = 0U; phaseIndex < kAllPhaseCount; ++phaseIndex) {
        ev::OperatorSystemState state = stateOf(kAllPhases[phaseIndex]);
        state.armed = true;
        state.faultLatched = true;
        for (uint8_t index = 0U; index < inspectionCount; ++index) {
            expectAdmissible(inspection[index], state);
            TEST_ASSERT_TRUE(ev::isInspectionCommand(inspection[index]) ||
                             ev::isTelemetryCommand(inspection[index]));
        }
    }
}

// A stop is never gated: a command that ends powered motion must always be
// available, in every phase, armed or not, faulted or not.
void test_disarm_is_never_gated(void) {
    for (uint8_t phaseIndex = 0U; phaseIndex < kAllPhaseCount; ++phaseIndex) {
        ev::OperatorSystemState state = stateOf(kAllPhases[phaseIndex]);
        state.armed = true;
        state.faultLatched = true;
        expectAdmissible(ev::OperatorCommandId::Disarm, state);
    }
}

// Once the run has started it owns its behavior: every command that would change
// run-control state is refused, so serial input cannot alter the run after the
// start trigger.
void test_run_control_commands_are_refused_once_underway(void) {
    const ev::OperatorCommandId changing[] = {
        ev::OperatorCommandId::ConfigSet,  ev::OperatorCommandId::ConfigClear,
        ev::OperatorCommandId::Announce,   ev::OperatorCommandId::Arm,
        ev::OperatorCommandId::Reset,
    };
    const uint8_t changingCount = static_cast<uint8_t>(sizeof(changing) / sizeof(changing[0]));

    for (uint8_t phaseIndex = 0U; phaseIndex < kAllPhaseCount; ++phaseIndex) {
        const ev::RunPhase phase = kAllPhases[phaseIndex];
        if (!ev::isRunPhaseUnderway(phase)) {
            continue;
        }
        // The faulted case is covered first by the phase rule, which is the
        // reason that outranks the rest.
        ev::OperatorSystemState state = stateOf(phase);
        state.armed = true;
        for (uint8_t index = 0U; index < changingCount; ++index) {
            expectRefused(changing[index], state, ev::CommandRefusal::RunUnderway);
        }
    }
}

// A latched fault is cleared by `reset` and by nothing else. A command that would
// resume operation must not clear it as a side effect of being typed.
void test_a_latched_fault_refuses_everything_but_reset(void) {
    ev::OperatorSystemState state = stateOf(ev::RunPhase::Fault);
    state.faultLatched = true;

    expectRefused(ev::OperatorCommandId::ConfigSet, state, ev::CommandRefusal::FaultLatched);
    expectRefused(ev::OperatorCommandId::ConfigClear, state, ev::CommandRefusal::FaultLatched);
    expectRefused(ev::OperatorCommandId::Announce, state, ev::CommandRefusal::FaultLatched);
    expectRefused(ev::OperatorCommandId::Arm, state, ev::CommandRefusal::FaultLatched);
    expectAdmissible(ev::OperatorCommandId::Reset, state);
    expectAdmissible(ev::OperatorCommandId::Disarm, state);
    expectAdmissible(ev::OperatorCommandId::Status, state);

    // The same in a phase that is not Fault: latching is what refuses, not the
    // phase name.
    state = stateOf(ev::RunPhase::Idle);
    state.faultLatched = true;
    expectRefused(ev::OperatorCommandId::ConfigSet, state, ev::CommandRefusal::FaultLatched);
    expectAdmissible(ev::OperatorCommandId::Reset, state);
}

// `reset` is the new-run boundary, so it is available in every phase that is not
// underway - including Done and Fault.
void test_reset_is_available_at_every_boundary(void) {
    for (uint8_t phaseIndex = 0U; phaseIndex < kAllPhaseCount; ++phaseIndex) {
        const ev::RunPhase phase = kAllPhases[phaseIndex];
        if (ev::isRunPhaseUnderway(phase)) {
            continue;
        }
        expectAdmissible(ev::OperatorCommandId::Reset, stateOf(phase));
    }
}

// Configuration and announcement belong to the Idle boundary. Any other phase
// means a run is announced, and the held configuration must not drift away from
// what that run was announced with.
void test_configuration_changes_require_the_idle_boundary(void) {
    const ev::RunPhase announced[] = {ev::RunPhase::ParameterValid, ev::RunPhase::Armed,
                                      ev::RunPhase::Done};
    for (uint8_t index = 0U; index < 3U; ++index) {
        const ev::OperatorSystemState state = stateOf(announced[index]);
        expectRefused(ev::OperatorCommandId::ConfigSet, state,
                      ev::CommandRefusal::RunAlreadyActive);
        expectRefused(ev::OperatorCommandId::ConfigClear, state,
                      ev::CommandRefusal::RunAlreadyActive);
        expectRefused(ev::OperatorCommandId::Announce, state,
                      ev::CommandRefusal::RunAlreadyActive);
    }

    expectAdmissible(ev::OperatorCommandId::ConfigSet, stateOf(ev::RunPhase::Idle));
    expectAdmissible(ev::OperatorCommandId::ConfigClear, stateOf(ev::RunPhase::Idle));
    expectAdmissible(ev::OperatorCommandId::Announce, stateOf(ev::RunPhase::Idle));
}

// Arming needs a run to arm *for*: the announcement is the deliberate second
// step, and arming without it would arm a vehicle with nothing to run.
void test_arming_requires_an_announced_run_and_is_refused_when_already_armed(void) {
    expectAdmissible(ev::OperatorCommandId::Arm, stateOf(ev::RunPhase::ParameterValid));

    ev::OperatorSystemState armed = stateOf(ev::RunPhase::ParameterValid);
    armed.armed = true;
    expectRefused(ev::OperatorCommandId::Arm, armed, ev::CommandRefusal::AlreadyArmed);

    expectRefused(ev::OperatorCommandId::Arm, stateOf(ev::RunPhase::Idle),
                  ev::CommandRefusal::NoRunAnnounced);
    expectRefused(ev::OperatorCommandId::Arm, stateOf(ev::RunPhase::Done),
                  ev::CommandRefusal::NoRunAnnounced);
}

// A request that was never parsed carries no command, and is refused as such
// rather than being treated as some command that happened to be zero.
void test_an_unparsed_request_is_refused_as_nothing(void) {
    const ev::CommandAdmissibility verdict =
        ev::admissibilityOf(ev::OperatorCommandId::None, stateOf(ev::RunPhase::Idle));
    TEST_ASSERT_FALSE(verdict.allowed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::CommandRefusal::NothingToApply),
                          static_cast<int>(verdict.refusal));
}

// Every refusal spelling is a comma-free token: the response formatter writes
// these names into comma-separated lines.
void test_refusal_spellings_are_wire_safe(void) {
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::CommandRefusal::NoRunAnnounced);
         ++index) {
        const char* name = ev::commandRefusalName(static_cast<ev::CommandRefusal>(index));
        TEST_ASSERT_NOT_NULL(name);
        const size_t length = strlen(name);
        TEST_ASSERT_TRUE(length > 0U);
        TEST_ASSERT_TRUE(length <= 30U);
        TEST_ASSERT_NULL(strchr(name, ','));
        TEST_ASSERT_NULL(strchr(name, ' '));
        TEST_ASSERT_NULL(strchr(name, '='));
    }
}

// ---------------------------------------------------------------------------
// The announcement gate
// ---------------------------------------------------------------------------

// The gate reports the first thing to fix: an incomplete configuration before a
// rule error, and a rule error before the run policy's own readiness.
void test_announcement_gate_reports_the_first_thing_to_fix(void) {
    const ev::ConfigurationBuildChecks passing = allChecksPassing();
    const ev::RunTimingPlan plan = planForTargetTime(15.0);
    TEST_ASSERT_TRUE(plan.valid);

    ev::RunConfigFields incomplete;
    incomplete.hasLevel = true;
    incomplete.levelIndex = 1U;
    ev::AnnounceVerdict verdict = ev::announceVerdictFor(incomplete, passing, plan);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::ConfigIncomplete),
                          static_cast<int>(verdict.refusal));

    ev::RunConfigFields invalid = validStateConfig();
    invalid.bottleLineDistanceM = 4.00;  // beyond the 3.00 m maximum (rule 8.j)
    verdict = ev::announceVerdictFor(invalid, passing, plan);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::ConfigInvalid),
                          static_cast<int>(verdict.refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::BottleLineDistanceOutOfRange),
                          static_cast<int>(verdict.config.error));

    ev::ConfigurationBuildChecks unusable = passing;
    unusable.runPolicyUsable = false;
    verdict = ev::announceVerdictFor(validStateConfig(), unusable, plan);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::PolicyNotUsable),
                          static_cast<int>(verdict.refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::RunPolicyUsable),
                          static_cast<int>(verdict.firstFailedCheck));

    verdict = ev::announceVerdictFor(validStateConfig(), passing, plan);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::None),
                          static_cast<int>(verdict.refusal));
}

// An announced Target Time the run cannot meet inside the configured limits is
// refused by name, and the plan's own status travels with the refusal so the
// operator is told which fact behind the plan refused it. Nothing is silently
// violated: the alternative would be a run that either exceeds a limit or times
// out mid-course.
void test_an_unreachable_target_time_is_refused_with_the_plan_reason(void) {
    const ev::ConfigurationBuildChecks passing = allChecksPassing();
    const ev::RunTimingPlan impossible = planForTargetTime(1.0);
    TEST_ASSERT_FALSE(impossible.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunTimingPlanStatus::TargetTimeTooShort),
                          static_cast<int>(impossible.status));

    const ev::AnnounceVerdict verdict =
        ev::announceVerdictFor(validStateConfig(), passing, impossible);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::AnnounceRefusal::TargetTimeUnreachable),
                          static_cast<int>(verdict.refusal));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunTimingPlanStatus::TargetTimeTooShort),
                          static_cast<int>(verdict.plan.status));

    // The refusal names a wire-safe reason, like every other refusal spelling.
    const char* name = ev::announceRefusalName(verdict.refusal);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_NULL(strchr(name, ','));
    TEST_ASSERT_EQUAL_STRING("TargetTimeUnreachable", name);
}

// ---------------------------------------------------------------------------
// The arming gate
// ---------------------------------------------------------------------------

// Three checks feed the three readiness fields the authority reads; the mask and
// the first failure carry the rest.
void test_arm_readiness_maps_the_three_data_completeness_checks(void) {
    ev::ConfigurationBuildChecks failing = allChecksPassing();
    failing.configurationValid = false;
    failing.calibrationMeasured = false;
    failing.safetyLimitsValid = false;
    failing.runPolicyUsable = false;

    const ev::SafetyReadiness readiness = ev::armReadinessFor(failing);
    TEST_ASSERT_FALSE(readiness.configurationValid);
    TEST_ASSERT_FALSE(readiness.calibrationUsable);
    TEST_ASSERT_FALSE(readiness.limitsUsable);

    const ev::SafetyReadiness ready = ev::armReadinessFor(allChecksPassing());
    TEST_ASSERT_TRUE(ready.configurationValid);
    TEST_ASSERT_TRUE(ready.calibrationUsable);
    TEST_ASSERT_TRUE(ready.limitsUsable);
}

// Every check must pass: there is no partial "close enough", and a refusal is
// attributable to the first failing check and to the whole mask.
void test_arming_requires_every_check(void) {
    const ev::ArmVerdict pass = ev::armVerdictFor(allChecksPassing());
    TEST_ASSERT_TRUE(pass.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::Count),
                          static_cast<int>(pass.firstFailedCheck));
    TEST_ASSERT_EQUAL_UINT16(0U, pass.failedCheckMask);

    ev::ConfigurationBuildChecks oneFailure = allChecksPassing();
    oneFailure.estimatorConfigValid = false;
    const ev::ArmVerdict fail = ev::armVerdictFor(oneFailure);
    TEST_ASSERT_FALSE(fail.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigurationCheckId::EstimatorConfigValid),
                          static_cast<int>(fail.firstFailedCheck));
    TEST_ASSERT_EQUAL_UINT16(1U << static_cast<uint8_t>(ev::ConfigurationCheckId::EstimatorConfigValid),
                             fail.failedCheckMask);
    TEST_ASSERT_TRUE(fail.readiness.configurationValid);
}

// ---------------------------------------------------------------------------
// The telemetry rate
// ---------------------------------------------------------------------------

// The stride is a whole number of control ticks, and the realized rate is
// reported next to it because a request that does not divide the cadence cannot
// be met exactly.
void test_telemetry_rate_maps_to_a_whole_stride(void) {
    // Every tick is the finest rate there is.
    ev::TelemetryRateVerdict verdict = ev::telemetryStrideForRate(50.0, 50U);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_UINT32(1U, verdict.strideTicks);
    expectValue(50.0, verdict.effectiveRateHz);

    // An exact divisor is exact.
    verdict = ev::telemetryStrideForRate(10.0, 50U);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_UINT32(5U, verdict.strideTicks);
    expectValue(10.0, verdict.effectiveRateHz);

    // One sample a second is the slowest cadence offered, and it is offered.
    verdict = ev::telemetryStrideForRate(1.0, 50U);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_UINT32(50U, verdict.strideTicks);
    expectValue(1.0, verdict.effectiveRateHz);

    // An inexact request is rounded to the nearest whole stride, and the rate
    // that stride actually produces is what is reported.
    verdict = ev::telemetryStrideForRate(3.0, 50U);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_UINT32(17U, verdict.strideTicks);
    expectValue(50.0 / 17.0, verdict.effectiveRateHz);

    // 30 Hz on a 50 Hz loop: 50/30 = 1.67 ticks, rounded to 2, which is 25 Hz -
    // not the 30 that was asked for, which is exactly why the realized rate is
    // reported.
    verdict = ev::telemetryStrideForRate(30.0, 50U);
    TEST_ASSERT_TRUE(verdict.ok);
    TEST_ASSERT_EQUAL_UINT32(2U, verdict.strideTicks);
    expectValue(25.0, verdict.effectiveRateHz);
}

// A rate the loop cannot honor is refused by name rather than silently adjusted
// to something else.
void test_telemetry_rate_refusals_are_named(void) {
    ev::TelemetryRateVerdict verdict = ev::telemetryStrideForRate(0.5, 50U);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRateRefusal::BelowMinimum),
                          static_cast<int>(verdict.refusal));

    verdict = ev::telemetryStrideForRate(0.0, 50U);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRateRefusal::BelowMinimum),
                          static_cast<int>(verdict.refusal));

    // A non-finite request is below the minimum rather than a special case: the
    // comparison that decides the minimum is false for a NaN, which is why the
    // order of the checks matters.
    verdict = ev::telemetryStrideForRate(std::numeric_limits<double>::quiet_NaN(), 50U);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRateRefusal::BelowMinimum),
                          static_cast<int>(verdict.refusal));

    // Faster than the loop samples is refused, because the effective rate would
    // silently differ from the request.
    verdict = ev::telemetryStrideForRate(51.0, 50U);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRateRefusal::AboveControlCadence),
                          static_cast<int>(verdict.refusal));

    // A cadence of zero means no loop to sample from.
    verdict = ev::telemetryStrideForRate(10.0, 0U);
    TEST_ASSERT_FALSE(verdict.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRateRefusal::AboveControlCadence),
                          static_cast<int>(verdict.refusal));
}

// The refusal spellings travel into response lines, so they are checked for the
// same wire-safety property as the state refusals.
void test_telemetry_and_announcement_refusal_spellings_are_wire_safe(void) {
    for (uint8_t index = 0U;
         index <= static_cast<uint8_t>(ev::TelemetryRateRefusal::AboveControlCadence); ++index) {
        const char* name = ev::telemetryRateRefusalName(static_cast<ev::TelemetryRateRefusal>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
        TEST_ASSERT_NULL(strchr(name, ' '));
    }
    for (uint8_t index = 0U; index <= static_cast<uint8_t>(ev::AnnounceRefusal::PolicyNotUsable);
         ++index) {
        const char* name = ev::announceRefusalName(static_cast<ev::AnnounceRefusal>(index));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0U);
        TEST_ASSERT_NULL(strchr(name, ','));
        TEST_ASSERT_NULL(strchr(name, ' '));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tournament_level_mapping_is_total);
    RUN_TEST(test_completed_configuration_derives_the_bottle_start_from_the_level);
    RUN_TEST(test_held_configuration_verdict_reports_all_three_facts);
    RUN_TEST(test_supplied_fields_are_checked_against_the_rules_before_storage);
    RUN_TEST(test_in_range_but_off_grid_is_left_to_the_whole_configuration_check);
    RUN_TEST(test_run_axis_geometry_matches_the_configuration);
    RUN_TEST(test_inspection_and_telemetry_are_allowed_in_every_state);
    RUN_TEST(test_disarm_is_never_gated);
    RUN_TEST(test_run_control_commands_are_refused_once_underway);
    RUN_TEST(test_a_latched_fault_refuses_everything_but_reset);
    RUN_TEST(test_reset_is_available_at_every_boundary);
    RUN_TEST(test_configuration_changes_require_the_idle_boundary);
    RUN_TEST(test_arming_requires_an_announced_run_and_is_refused_when_already_armed);
    RUN_TEST(test_an_unparsed_request_is_refused_as_nothing);
    RUN_TEST(test_refusal_spellings_are_wire_safe);
    RUN_TEST(test_announcement_gate_reports_the_first_thing_to_fix);
    RUN_TEST(test_arm_readiness_maps_the_three_data_completeness_checks);
    RUN_TEST(test_arming_requires_every_check);
    RUN_TEST(test_telemetry_rate_maps_to_a_whole_stride);
    RUN_TEST(test_telemetry_rate_refusals_are_named);
    RUN_TEST(test_telemetry_and_announcement_refusal_spellings_are_wire_safe);
    return UNITY_END();
}
