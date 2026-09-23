#pragma once

#include <cmath>
#include <cstdint>

namespace ev {

// Tournament level. The announced Target Distance, Target Time, Bottle Line
// Distance and bottle placement all depend on it.
//
// Source: official 2027 Division C rules, event Electric Vehicle (pages
// C26-C31).
enum class TournamentLevel : uint8_t {
    RegionalInvitational = 0,
    State = 1,
    National = 2,
};

// Every rule-derived number the firmware uses, in one place, each citing the
// rule it comes from. Values are SI: metres and seconds.
namespace rules2027 {

// Rule 8.h: Target Distance is 5.00-9.00 m from the Start Point. The announced
// interval is 0.25 m at Regionals/Invitationals, 0.10 m at States and 0.01 m at
// Nationals.
constexpr double kTargetDistanceMinM = 5.00;
constexpr double kTargetDistanceMaxM = 9.00;
constexpr double kIntervalRegionalM = 0.25;
constexpr double kIntervalStateM = 0.10;
constexpr double kIntervalNationalM = 0.01;

// Rule 8.i: Target Time is 10.0-20.0 s. The interval is 0.5 s at all
// tournaments.
constexpr double kTargetTimeMinS = 10.0;
constexpr double kTargetTimeMaxS = 20.0;
constexpr double kTargetTimeIntervalS = 0.5;

// Rule 8.j: Bottle Line Distance is 1.00-3.00 m beyond the Target Point. The
// announced interval matches the Target Distance interval.
constexpr double kBottleLineDistanceMinM = 1.00;
constexpr double kBottleLineDistanceMaxM = 3.00;

// Rule 7.c.ii.(5): Water Bottle Location, measured from the Start Point along
// the imaginary centre line.
constexpr double kBottleStartRegionalM = 2.0;
constexpr double kBottleStartStateM = 3.0;
constexpr double kBottleStartNationalM = 4.0;

// The rules publish values to 0.01 m and 0.1 s. These tolerances are far below
// that, so they only absorb decimal floating-point representation error and
// never accept an off-grid value.
constexpr double kToleranceM = 1e-6;
constexpr double kToleranceS = 1e-6;

}  // namespace rules2027

inline bool isKnownTournamentLevel(TournamentLevel level) {
    switch (level) {
        case TournamentLevel::RegionalInvitational:
        case TournamentLevel::State:
        case TournamentLevel::National:
            return true;
    }
    return false;
}

// Announced value grid for the level (rules 8.h and 8.j).
inline double announcedIntervalM(TournamentLevel level) {
    switch (level) {
        case TournamentLevel::RegionalInvitational:
            return rules2027::kIntervalRegionalM;
        case TournamentLevel::State:
            return rules2027::kIntervalStateM;
        case TournamentLevel::National:
            return rules2027::kIntervalNationalM;
    }
    return 0.0;  // unknown level: no grid is defined
}

// Official bottle placement for the level (rule 7.c.ii.(5)).
inline double bottleStartDistanceM(TournamentLevel level) {
    switch (level) {
        case TournamentLevel::RegionalInvitational:
            return rules2027::kBottleStartRegionalM;
        case TournamentLevel::State:
            return rules2027::kBottleStartStateM;
        case TournamentLevel::National:
            return rules2027::kBottleStartNationalM;
    }
    return 0.0;
}

// True when `value` sits on an integer multiple of `interval` within
// `tolerance`.
inline bool isOnInterval(double value, double interval, double tolerance) {
    if (!(interval > 0.0)) {
        return false;
    }
    const double steps = value / interval;
    const double nearest = std::floor(steps + 0.5);
    return std::fabs(steps - nearest) * interval <= tolerance;
}

// What the event supervisor announces after impound (rule 4.b). These values
// are the competition geometry, so they live here rather than inside the run
// logic. Every field must be supplied; there is no default geometry.
struct RunConfig {
    constexpr RunConfig(TournamentLevel level_, double targetDistanceM_, double targetTimeS_,
                        double bottleLineDistanceM_, double bottleStartDistanceM_)
        : level(level_),
          targetDistanceM(targetDistanceM_),
          targetTimeS(targetTimeS_),
          bottleLineDistanceM(bottleLineDistanceM_),
          bottleStartDistanceM(bottleStartDistanceM_) {}

    TournamentLevel level;       // rule 4.b / 8.h-8.j
    double targetDistanceM;      // rule 8.h, 5.00-9.00 m
    double targetTimeS;          // rule 8.i, 10.0-20.0 s
    double bottleLineDistanceM;  // rule 8.j, 1.00-3.00 m beyond the Target Point
    double bottleStartDistanceM;  // rule 7.c.ii.(5), from the Start Point
};

enum class ConfigError : uint8_t {
    None = 0,
    NonFiniteValue,              // NaN or infinity
    InvalidTournamentLevel,      // not one of the three levels
    TargetDistanceOutOfRange,    // rule 8.h
    TargetDistanceOffInterval,   // rule 8.h grid
    TargetTimeOutOfRange,        // rule 8.i
    TargetTimeOffInterval,       // rule 8.i grid
    BottleLineDistanceOutOfRange,  // rule 8.j
    BottleLineDistanceOffInterval, // rule 8.j grid
    BottleStartNotBeforeTarget,  // bottle must sit between Start and Target Points
    BottleStartNotLevelDistance, // rule 7.c.ii.(5)
};

struct RunConfigValidation {
    bool ok;
    ConfigError error;
};

// The single entry point for run-configuration validation. Checks are ordered
// so the most fundamental problem is reported first: finiteness, then the
// level, then each rule range, then each announced grid, then the cross-field
// relationships.
inline RunConfigValidation validateRunConfig(const RunConfig& config) {
    if (!std::isfinite(config.targetDistanceM) || !std::isfinite(config.targetTimeS) ||
        !std::isfinite(config.bottleLineDistanceM) ||
        !std::isfinite(config.bottleStartDistanceM)) {
        return {false, ConfigError::NonFiniteValue};
    }

    if (!isKnownTournamentLevel(config.level)) {
        return {false, ConfigError::InvalidTournamentLevel};
    }

    // Target Distance (rule 8.h).
    if (config.targetDistanceM < rules2027::kTargetDistanceMinM ||
        config.targetDistanceM > rules2027::kTargetDistanceMaxM) {
        return {false, ConfigError::TargetDistanceOutOfRange};
    }
    if (!isOnInterval(config.targetDistanceM, announcedIntervalM(config.level),
                      rules2027::kToleranceM)) {
        return {false, ConfigError::TargetDistanceOffInterval};
    }

    // Target Time (rule 8.i).
    if (config.targetTimeS < rules2027::kTargetTimeMinS ||
        config.targetTimeS > rules2027::kTargetTimeMaxS) {
        return {false, ConfigError::TargetTimeOutOfRange};
    }
    if (!isOnInterval(config.targetTimeS, rules2027::kTargetTimeIntervalS,
                      rules2027::kToleranceS)) {
        return {false, ConfigError::TargetTimeOffInterval};
    }

    // Bottle Line Distance (rule 8.j).
    if (config.bottleLineDistanceM < rules2027::kBottleLineDistanceMinM ||
        config.bottleLineDistanceM > rules2027::kBottleLineDistanceMaxM) {
        return {false, ConfigError::BottleLineDistanceOutOfRange};
    }
    if (!isOnInterval(config.bottleLineDistanceM, announcedIntervalM(config.level),
                      rules2027::kToleranceM)) {
        return {false, ConfigError::BottleLineDistanceOffInterval};
    }

    // Rule 7.c.ii.(5) puts the bottle on the centre line between the Start
    // Point and the Target Point; the vehicle cannot push a bottle it has
    // already driven past.
    if (!(config.bottleStartDistanceM < config.targetDistanceM)) {
        return {false, ConfigError::BottleStartNotBeforeTarget};
    }

    // The bottle location is fixed by the tournament level, so a configuration
    // that pairs one level with another level's bottle distance is internally
    // inconsistent.
    if (std::fabs(config.bottleStartDistanceM - bottleStartDistanceM(config.level)) >
        rules2027::kToleranceM) {
        return {false, ConfigError::BottleStartNotLevelDistance};
    }

    return {true, ConfigError::None};
}

}  // namespace ev
