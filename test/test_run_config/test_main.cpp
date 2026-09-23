#include <cmath>
#include <cstdint>
#include <limits>

#include <unity.h>

#include <RunConfig.h>

// Rule-validity tests for RunConfig. Every boundary here comes from the
// official 2027 Division C rules for the Electric Vehicle event (pages C26-C31):
//   rule 8.h  Target Distance 5.00-9.00 m, interval 0.25/0.10/0.01 m by level
//   rule 8.i  Target Time 10.0-20.0 s, interval 0.5 s
//   rule 8.j  Bottle Line Distance 1.00-3.00 m, same interval as Target Distance
//   rule 7.c.ii.(5)  bottle placed 2.0/3.0/4.0 m from the Start Point by level

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr ev::TournamentLevel kRegional = ev::TournamentLevel::RegionalInvitational;
constexpr ev::TournamentLevel kState = ev::TournamentLevel::State;
constexpr ev::TournamentLevel kNational = ev::TournamentLevel::National;

const double kNaN = std::nan("");
const double kInf = std::numeric_limits<double>::infinity();

// Builds a configuration whose bottle placement already matches the level, so
// each test only has to vary the field under test.
ev::RunConfig configFor(ev::TournamentLevel level, double targetDistanceM, double targetTimeS,
                        double bottleLineDistanceM) {
    return ev::RunConfig(level, targetDistanceM, targetTimeS, bottleLineDistanceM,
                         ev::bottleStartDistanceM(level));
}

void expectValid(const ev::RunConfig& config) {
    const ev::RunConfigValidation result = ev::validateRunConfig(config);
    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::ConfigError::None),
                          static_cast<int>(result.error));
}

void expectInvalid(ev::ConfigError expected, const ev::RunConfig& config) {
    const ev::RunConfigValidation result = ev::validateRunConfig(config);
    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result.error));
}

}  // namespace

void test_official_range_boundaries_are_accepted(void) {
    expectValid(configFor(kRegional, 5.00, 10.0, 1.00));
    expectValid(configFor(kRegional, 9.00, 20.0, 3.00));
    expectValid(configFor(kState, 5.00, 10.0, 1.00));
    expectValid(configFor(kState, 9.00, 20.0, 3.00));
    expectValid(configFor(kNational, 5.00, 10.0, 1.00));
    expectValid(configFor(kNational, 9.00, 20.0, 3.00));
}

void test_just_outside_official_ranges_are_rejected(void) {
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kRegional, 4.99, 10.0, 1.0));
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kRegional, 9.01, 10.0, 1.0));
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kState, 4.99, 10.0, 1.0));
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kNational, 9.01, 10.0, 1.0));

    expectInvalid(ev::ConfigError::TargetTimeOutOfRange, configFor(kRegional, 7.00, 9.9, 1.0));
    expectInvalid(ev::ConfigError::TargetTimeOutOfRange, configFor(kRegional, 7.00, 20.1, 1.0));

    expectInvalid(ev::ConfigError::BottleLineDistanceOutOfRange,
                  configFor(kRegional, 7.00, 10.0, 0.99));
    expectInvalid(ev::ConfigError::BottleLineDistanceOutOfRange,
                  configFor(kRegional, 7.00, 10.0, 3.01));
}

void test_announced_grid_per_tournament_level(void) {
    // 0.25 m grid at Regionals/Invitationals.
    expectValid(configFor(kRegional, 5.25, 10.0, 1.25));
    expectValid(configFor(kRegional, 8.75, 20.0, 2.75));
    expectInvalid(ev::ConfigError::TargetDistanceOffInterval, configFor(kRegional, 5.10, 10.0, 1.0));

    // 0.10 m grid at States.
    expectValid(configFor(kState, 5.10, 10.0, 1.10));
    expectValid(configFor(kState, 7.30, 14.0, 2.00));
    expectInvalid(ev::ConfigError::TargetDistanceOffInterval, configFor(kState, 5.05, 10.0, 1.0));

    // 0.01 m grid at Nationals.
    expectValid(configFor(kNational, 5.01, 10.0, 1.01));
    expectValid(configFor(kNational, 8.99, 20.0, 2.99));
    expectInvalid(ev::ConfigError::TargetDistanceOffInterval,
                  configFor(kNational, 5.005, 10.0, 1.0));
}

void test_bottle_line_uses_the_target_distance_grid(void) {
    expectInvalid(ev::ConfigError::BottleLineDistanceOffInterval,
                  configFor(kRegional, 7.00, 10.0, 1.10));
    expectValid(configFor(kState, 7.00, 10.0, 1.10));
    expectInvalid(ev::ConfigError::BottleLineDistanceOffInterval,
                  configFor(kState, 7.00, 10.0, 1.05));
    expectValid(configFor(kNational, 7.00, 10.0, 1.05));
}

void test_target_time_uses_the_half_second_grid(void) {
    expectValid(configFor(kRegional, 7.00, 10.5, 1.0));
    expectValid(configFor(kNational, 7.00, 20.0, 1.0));
    expectInvalid(ev::ConfigError::TargetTimeOffInterval, configFor(kRegional, 7.00, 10.3, 1.0));
    expectInvalid(ev::ConfigError::TargetTimeOffInterval, configFor(kState, 7.00, 19.9, 1.0));
}

void test_bottle_start_must_match_the_tournament_level(void) {
    // A State run with a Regional bottle distance is internally inconsistent.
    expectInvalid(ev::ConfigError::BottleStartNotLevelDistance,
                  ev::RunConfig(kState, 7.00, 14.0, 1.0, ev::rules2027::kBottleStartRegionalM));
    expectInvalid(ev::ConfigError::BottleStartNotLevelDistance,
                  ev::RunConfig(kNational, 7.00, 14.0, 1.0, ev::rules2027::kBottleStartStateM));
    expectInvalid(ev::ConfigError::BottleStartNotLevelDistance,
                  ev::RunConfig(kRegional, 7.00, 14.0, 1.0, ev::rules2027::kBottleStartNationalM));

    // A negative bottle distance is not one of the three announced placements.
    expectInvalid(ev::ConfigError::BottleStartNotLevelDistance,
                  ev::RunConfig(kState, 7.00, 14.0, 1.0, -2.00));

    // The matching pairing is accepted.
    expectValid(ev::RunConfig(kState, 7.00, 14.0, 2.0, ev::rules2027::kBottleStartStateM));
}

void test_bottle_start_must_precede_the_target_distance(void) {
    expectInvalid(ev::ConfigError::BottleStartNotBeforeTarget,
                  ev::RunConfig(kNational, 5.00, 10.0, 1.0, 5.00));
    expectInvalid(ev::ConfigError::BottleStartNotBeforeTarget,
                  ev::RunConfig(kNational, 5.00, 10.0, 1.0, 8.00));
}

void test_non_finite_values_are_rejected(void) {
    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, kNaN, 10.0, 1.0));
    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, 7.00, kNaN, 1.0));
    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, 7.00, 10.0, kNaN));
    expectInvalid(ev::ConfigError::NonFiniteValue, ev::RunConfig(kState, 7.0, 10.0, 1.0, kNaN));

    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, kInf, 10.0, 1.0));
    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, 7.00, -kInf, 1.0));
    expectInvalid(ev::ConfigError::NonFiniteValue, configFor(kState, 7.00, 10.0, kInf));
    expectInvalid(ev::ConfigError::NonFiniteValue, ev::RunConfig(kState, 7.0, 10.0, 1.0, -kInf));
}

void test_negative_and_zero_values_are_rejected(void) {
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kState, 0.0, 10.0, 1.0));
    expectInvalid(ev::ConfigError::TargetDistanceOutOfRange, configFor(kState, -5.0, 10.0, 1.0));

    expectInvalid(ev::ConfigError::TargetTimeOutOfRange, configFor(kState, 7.0, 0.0, 1.0));
    expectInvalid(ev::ConfigError::TargetTimeOutOfRange, configFor(kState, 7.0, -10.0, 1.0));

    expectInvalid(ev::ConfigError::BottleLineDistanceOutOfRange, configFor(kState, 7.0, 10.0, 0.0));
    expectInvalid(ev::ConfigError::BottleLineDistanceOutOfRange, configFor(kState, 7.0, 10.0, -1.0));
}

void test_unknown_tournament_level_is_rejected(void) {
    expectInvalid(ev::ConfigError::InvalidTournamentLevel,
                  configFor(static_cast<ev::TournamentLevel>(99), 7.0, 10.0, 1.0));
}

void test_finiteness_is_checked_before_everything_else(void) {
    // Both the level and the distance are invalid; finiteness is reported first
    // because no other check is meaningful on a NaN.
    expectInvalid(ev::ConfigError::NonFiniteValue,
                  configFor(static_cast<ev::TournamentLevel>(99), kNaN, 10.0, 1.0));
}

void test_level_is_checked_before_field_ranges(void) {
    // An unknown level has no announced grid, and is reported ahead of ranges.
    expectInvalid(ev::ConfigError::InvalidTournamentLevel,
                  ev::RunConfig(static_cast<ev::TournamentLevel>(99), 1.0, 1.0, 1.0, 1.0));
}

void test_announced_bottle_distances_match_the_rules(void) {
    TEST_ASSERT_TRUE(std::fabs(ev::bottleStartDistanceM(kRegional) - 2.0) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(ev::bottleStartDistanceM(kState) - 3.0) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(ev::bottleStartDistanceM(kNational) - 4.0) < 1e-12);

    TEST_ASSERT_TRUE(std::fabs(ev::announcedIntervalM(kRegional) - 0.25) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(ev::announcedIntervalM(kState) - 0.10) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(ev::announcedIntervalM(kNational) - 0.01) < 1e-12);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_official_range_boundaries_are_accepted);
    RUN_TEST(test_just_outside_official_ranges_are_rejected);
    RUN_TEST(test_announced_grid_per_tournament_level);
    RUN_TEST(test_bottle_line_uses_the_target_distance_grid);
    RUN_TEST(test_target_time_uses_the_half_second_grid);
    RUN_TEST(test_bottle_start_must_match_the_tournament_level);
    RUN_TEST(test_bottle_start_must_precede_the_target_distance);
    RUN_TEST(test_non_finite_values_are_rejected);
    RUN_TEST(test_negative_and_zero_values_are_rejected);
    RUN_TEST(test_unknown_tournament_level_is_rejected);
    RUN_TEST(test_finiteness_is_checked_before_everything_else);
    RUN_TEST(test_level_is_checked_before_field_ranges);
    RUN_TEST(test_announced_bottle_distances_match_the_rules);
    return UNITY_END();
}
