#pragma once

#include <cmath>
#include <cstdint>

#include "CompetitionStateMachine.h"
#include "ConfigurationChecks.h"
#include "RunAxis.h"
#include "RunConfig.h"
#include "SafetyAuthority.h"
#include "SerialCommandParser.h"

namespace ev {

// ---------------------------------------------------------------------------
// The operator policy - what a parsed command means, and when it is allowed
// ---------------------------------------------------------------------------
//
// The decision half of the operator interface. It answers three questions and
// holds no state of its own:
//
// 1. **Is this command allowed in the system's current state?** A run that has
//    started owns its behavior; once `isRunPhaseUnderway()` is true, only
//    inspection commands, `disarm` and telemetry control are accepted, so serial
//    input cannot alter run-control behavior after the start trigger. `disarm`
//    is deliberately never gated: a stop must always be available.
// 2. **What does the held configuration mean under the event rules, and may it
//    be announced?** The rule verdict comes from `ev::validateRunConfig()`, the
//    single validation entry point (`SOFTWARE_ARCHITECTURE.md` section 8); this
//    module never re-implements a rule check.
// 3. **What does an arming intent read?** `armReadinessFor()` is the one mapping
//    from the configuration-build report onto `SafetyReadiness`, and the verdict
//    carries the named failing check with it, because a refused arm has to be
//    attributable to the fact that failed.
//
// What this module is not: it is not the arming authority
// (`SafetyAuthority` decides), it is not the run policy (`CompetitionStateMachine`
// owns the phases), and it is not a state holder. Every call takes the current
// state as an argument, so there is no second copy of "armed" or "in which
// phase" that could disagree with the chain. That is also why nothing here can
// drive, arm, or clear a fault: it returns verdicts, and the vehicle layer
// applies them through the same boundary everything else uses.

// ---------------------------------------------------------------------------
// The held configuration
// ---------------------------------------------------------------------------

// The rule verdict for the configuration an operator has entered so far.
struct HeldRunConfigVerdict {
    // Every field a `RunConfig` needs has been supplied.
    bool complete;
    // `validateRunConfig()` accepted the completed configuration. False when the
    // configuration is incomplete, so an incomplete configuration is never
    // "valid so far".
    bool rulesOk;
    // The rule error of record, or `ConfigError::None`. Meaningful only when
    // `complete` is true and `rulesOk` is false.
    ConfigError error;
};

// The tournament level an index names. An out-of-range index names the first
// level rather than an undefined value; the index is only ever produced by the
// parser's name table, so this is defence against a caller bug, not input.
inline TournamentLevel tournamentLevelForIndex(uint8_t index) {
    return static_cast<TournamentLevel>(index < kTournamentLevelCount ? index : 0U);
}

// The complete `RunConfig` a set of fields describes. Precondition:
// `fields.complete()`. The Bottle Start Point is derived from the tournament
// level (rule 7.c.ii.(5)) rather than stored, so there is no fifth value that
// can disagree with the level.
inline RunConfig completedRunConfig(const RunConfigFields& fields) {
    const TournamentLevel level = tournamentLevelForIndex(fields.levelIndex);
    return RunConfig(level, fields.targetDistanceM, fields.targetTimeS,
                     fields.bottleLineDistanceM, bottleStartDistanceM(level));
}

// The run-axis geometry the announced configuration implies: the Target Point
// (rule 8.h), the Bottle Line Distance (rule 8.j) and the turn-around point
// beyond the Target Point. Reported to the operator so the geometry a run will
// use is visible, not inferred from the four typed values.
inline RunAxisGeometry runAxisGeometryFor(const RunConfigFields& fields) {
    return RunAxisGeometry::fromRunConfig(completedRunConfig(fields));
}

// Runs the rule validation through the single entry point and reports the three
// facts separately: whether the configuration is complete, whether the rules
// accept it, and - when they do not - which rule error it is.
inline HeldRunConfigVerdict heldRunConfigVerdict(const RunConfigFields& fields) {
    HeldRunConfigVerdict verdict{false, false, ConfigError::None};
    if (!fields.complete()) {
        return verdict;
    }
    verdict.complete = true;
    const RunConfigValidation validation = validateRunConfig(completedRunConfig(fields));
    verdict.rulesOk = validation.ok;
    verdict.error = validation.error;
    return verdict;
}

// Checks the fields one `config set` supplied, *before* any of them is stored.
//
// Why this exists: a held value is echoed back by `config show` and is
// indistinguishable from a value the operator meant, so a mistyped number must
// not become state at all. On the first hardware session an out-of-range Target
// Distance was held and reported as held (`OK,cmd=config set` with
// `rules=0,error=TargetDistanceOutOfRange`) - named and still unarmable, but the
// held configuration had changed, which is not what the protocol says a refused
// value does (finding F-5).
//
// Scope: only the checks a single field can fail on its own - a non-finite
// number, one of the rule ranges, and a level index that names a real level. The
// announced grid and the cross-field geometry checks (Bottle Start before
// Target, Bottle Start fixed by the level) need the level *and* the rest of the
// configuration, so they stay with the single rule entry point
// (`validateRunConfig()` through `heldRunConfigVerdict()`) that `config show`,
// `config check` and `announce` already run. Two entry points, deliberately: this
// one refuses a value at the door, that one judges a whole configuration.
inline RunConfigValidation validateSuppliedConfigFields(const RunConfigFields& supplied) {
    if (supplied.hasLevel && supplied.levelIndex >= kTournamentLevelCount) {
        return {false, ConfigError::InvalidTournamentLevel};
    }
    if (supplied.hasTargetDistanceM) {
        if (!std::isfinite(supplied.targetDistanceM)) {
            return {false, ConfigError::NonFiniteValue};
        }
        if (supplied.targetDistanceM < rules2027::kTargetDistanceMinM ||
            supplied.targetDistanceM > rules2027::kTargetDistanceMaxM) {
            return {false, ConfigError::TargetDistanceOutOfRange};
        }
    }
    if (supplied.hasTargetTimeS) {
        if (!std::isfinite(supplied.targetTimeS)) {
            return {false, ConfigError::NonFiniteValue};
        }
        if (supplied.targetTimeS < rules2027::kTargetTimeMinS ||
            supplied.targetTimeS > rules2027::kTargetTimeMaxS) {
            return {false, ConfigError::TargetTimeOutOfRange};
        }
    }
    if (supplied.hasBottleLineDistanceM) {
        if (!std::isfinite(supplied.bottleLineDistanceM)) {
            return {false, ConfigError::NonFiniteValue};
        }
        if (supplied.bottleLineDistanceM < rules2027::kBottleLineDistanceMinM ||
            supplied.bottleLineDistanceM > rules2027::kBottleLineDistanceMaxM) {
            return {false, ConfigError::BottleLineDistanceOutOfRange};
        }
    }
    return {true, ConfigError::None};
}

// ---------------------------------------------------------------------------
// State-restricted commands
// ---------------------------------------------------------------------------

// Why a command may not be applied in the current state. Every one of these is
// reported to the operator, so a refusal says what to do next (usually `reset`)
// rather than only saying no.
enum class CommandRefusal : uint8_t {
    None = 0,
    NothingToApply,    // no command at all: a caller passed a request that was never parsed
    RunUnderway,       // the run has started; only inspection, disarm and telemetry are accepted
    RunAlreadyActive,  // a run is announced; reset before changing or announcing a configuration
    FaultLatched,      // a fault is of record; reset clears it, and nothing else may
    AlreadyArmed,      // the authority already reports armed
    NoRunAnnounced,    // arm without a valid announcement
};

inline const char* commandRefusalName(CommandRefusal refusal) {
    switch (refusal) {
        case CommandRefusal::None:
            return "None";
        case CommandRefusal::NothingToApply:
            return "NothingToApply";
        case CommandRefusal::RunUnderway:
            return "RunUnderway";
        case CommandRefusal::RunAlreadyActive:
            return "RunAlreadyActive";
        case CommandRefusal::FaultLatched:
            return "FaultLatched";
        case CommandRefusal::AlreadyArmed:
            return "AlreadyArmed";
        case CommandRefusal::NoRunAnnounced:
            return "NoRunAnnounced";
    }
    return "None";
}

// The vehicle layer's view of the system, as the policy needs it. Filled from
// the chain each time a command arrives: readings only, never decisions.
struct OperatorSystemState {
    RunPhase phase = RunPhase::Idle;
    bool armed = false;
    bool faultLatched = false;
    ConfigurationBuildChecks checks{};
};

struct CommandAdmissibility {
    bool allowed = false;
    CommandRefusal refusal = CommandRefusal::None;
};

// True for the commands that only read state: they can never alter run behavior,
// so they are accepted in every state, including mid-run.
// Telemetry control belongs here too: the sample stride and the enable flag are
// the telemetry stream's own state, never the run's.
inline bool isInspectionCommand(OperatorCommandId id) {
    switch (id) {
        case OperatorCommandId::Help:
        case OperatorCommandId::Status:
        case OperatorCommandId::Version:
        case OperatorCommandId::Checks:
        case OperatorCommandId::Boot:
        case OperatorCommandId::ConfigShow:
        case OperatorCommandId::ConfigCheck:
            return true;
        default:
            return false;
    }
}

inline bool isTelemetryCommand(OperatorCommandId id) {
    switch (id) {
        case OperatorCommandId::TelemetryOn:
        case OperatorCommandId::TelemetryOff:
        case OperatorCommandId::TelemetryStatus:
        case OperatorCommandId::TelemetryRate:
            return true;
        default:
            return false;
    }
}

// The state restriction, in one place.
//
// The order of the checks is the order of the reasons: a run that has started
// outranks every other state fact, a latched fault outranks the phase a run was
// in when it latched, and the phase-specific rules come last.
inline CommandAdmissibility admissibilityOf(OperatorCommandId id, const OperatorSystemState& state) {
    CommandAdmissibility verdict;
    if (id == OperatorCommandId::None) {
        verdict.refusal = CommandRefusal::NothingToApply;
        return verdict;
    }

    // Inspection and telemetry control: nothing here can change how the vehicle
    // moves, so no state gates them.
    if (isInspectionCommand(id) || isTelemetryCommand(id)) {
        verdict.allowed = true;
        return verdict;
    }

    // A stop is never gated. Disarming during a run ends that run safely - the
    // run policy faults with DisarmedDuringRun rather than continuing to ask for
    // motion the authority is refusing - and that is the intended behavior.
    if (id == OperatorCommandId::Disarm) {
        verdict.allowed = true;
        return verdict;
    }

    // From here on the command would change run-control state, so a run that has
    // started refuses all of it.
    if (isRunPhaseUnderway(state.phase)) {
        verdict.refusal = CommandRefusal::RunUnderway;
        return verdict;
    }

    // `reset` is the new-run boundary, and it is also the *only* thing that
    // clears a latched fault - which is why it is checked before the fault gate
    // rather than after it. A latched fault that no command could clear would be
    // a vehicle that has to be power-cycled. It is deliberately still refused
    // during a run (the check above): a run that has started must be ended by
    // stopping it, not by discarding the record of what it was doing.
    if (id == OperatorCommandId::Reset) {
        verdict.allowed = true;
        return verdict;
    }

    // A latched fault is cleared by `reset` and by nothing else: a command that
    // would resume operation must not clear it as a side effect of being typed.
    if (state.faultLatched) {
        verdict.refusal = CommandRefusal::FaultLatched;
        return verdict;
    }

    switch (id) {
        case OperatorCommandId::Reset:
            break;  // handled above

        case OperatorCommandId::ConfigSet:
        case OperatorCommandId::ConfigClear:
        case OperatorCommandId::Announce:
            // Configuration and announcement belong to the Idle boundary. Any
            // other phase means a run is announced, and the held configuration
            // must not drift away from what that run was announced with.
            if (state.phase != RunPhase::Idle) {
                verdict.refusal = CommandRefusal::RunAlreadyActive;
                return verdict;
            }
            verdict.allowed = true;
            return verdict;

        case OperatorCommandId::Arm:
            if (state.armed) {
                verdict.refusal = CommandRefusal::AlreadyArmed;
                return verdict;
            }
            // Arming requires an announcement to arm *for*: the run policy moves
            // out of ParameterValid once the authority reports armed, and arming
            // with nothing announced would arm a vehicle with no run to run.
            if (state.phase != RunPhase::ParameterValid) {
                verdict.refusal = CommandRefusal::NoRunAnnounced;
                return verdict;
            }
            verdict.allowed = true;
            return verdict;

        case OperatorCommandId::None:
        case OperatorCommandId::Help:
        case OperatorCommandId::Status:
        case OperatorCommandId::Version:
        case OperatorCommandId::Checks:
        case OperatorCommandId::Boot:
        case OperatorCommandId::ConfigShow:
        case OperatorCommandId::ConfigCheck:
        case OperatorCommandId::Disarm:
        case OperatorCommandId::TelemetryOn:
        case OperatorCommandId::TelemetryOff:
        case OperatorCommandId::TelemetryStatus:
        case OperatorCommandId::TelemetryRate:
            break;
    }
    // Unreachable: every command above either returned or is inspection-only.
    // A command added later without a rule lands here and is refused.
    verdict.refusal = CommandRefusal::RunAlreadyActive;
    return verdict;
}

// ---------------------------------------------------------------------------
// The announcement gate
// ---------------------------------------------------------------------------

// Why a configuration may not be offered to a run. `None` means it may.
enum class AnnounceRefusal : uint8_t {
    None = 0,
    ConfigIncomplete,   // a field has not been set: announce would be a partial configuration
    ConfigInvalid,      // the rules rejected it: `ConfigError` below names which
    PolicyNotUsable,    // the run policy may not sequence a run with this configuration
};

inline const char* announceRefusalName(AnnounceRefusal refusal) {
    switch (refusal) {
        case AnnounceRefusal::None:
            return "None";
        case AnnounceRefusal::ConfigIncomplete:
            return "ConfigIncomplete";
        case AnnounceRefusal::ConfigInvalid:
            return "ConfigInvalid";
        case AnnounceRefusal::PolicyNotUsable:
            return "PolicyNotUsable";
    }
    return "None";
}

struct AnnounceVerdict {
    bool ok = false;
    AnnounceRefusal refusal = AnnounceRefusal::None;
    HeldRunConfigVerdict config{};
    // The check that refuses a `PolicyNotUsable` announcement, so the operator is
    // told which fact is missing rather than that the run is "not ready".
    ConfigurationCheckId firstFailedCheck = ConfigurationCheckId::Count;
};

// The announcement gate. Checked in order, so the reported reason is the first
// thing the operator should fix: an incomplete configuration is reported before
// a rule error (there is nothing to validate yet), and a rule error before the
// policy check (a configuration the rules reject is not worth sequencing).
//
// A refusal here latches nothing. The run policy's own `announceRun()` faults on
// a rejection because a later call must not start a half-corrected run; this gate
// is what the command layer uses so that a *typed* configuration that cannot be
// announced yet is a refusal to report rather than a fault to clear. The
// policy's refusal stays as defence in depth for any future caller.
inline AnnounceVerdict announceVerdictFor(const RunConfigFields& fields,
                                          const ConfigurationBuildChecks& checks) {
    AnnounceVerdict verdict;
    verdict.config = heldRunConfigVerdict(fields);
    if (!verdict.config.complete) {
        verdict.refusal = AnnounceRefusal::ConfigIncomplete;
        return verdict;
    }
    if (!verdict.config.rulesOk) {
        verdict.refusal = AnnounceRefusal::ConfigInvalid;
        return verdict;
    }
    if (!checks.runPolicyUsable) {
        verdict.refusal = AnnounceRefusal::PolicyNotUsable;
        verdict.firstFailedCheck = ConfigurationCheckId::RunPolicyUsable;
        return verdict;
    }
    verdict.ok = true;
    return verdict;
}

// ---------------------------------------------------------------------------
// The arming gate
// ---------------------------------------------------------------------------

// The one mapping from the configuration-build report onto the readiness the
// authority reads. Three checks feed three fields because those are exactly the
// data-completeness questions `SafetyReadiness` asks; the remaining checks are
// conditions of the chain being built correctly at all, and they gate arming
// through `allPass()` below rather than through readiness fields that would have
// to be invented for them.
inline SafetyReadiness armReadinessFor(const ConfigurationBuildChecks& checks) {
    SafetyReadiness readiness;
    readiness.configurationValid = checks.configurationValid;
    readiness.calibrationUsable = checks.calibrationMeasured;
    readiness.limitsUsable = checks.safetyLimitsValid;
    return readiness;
}

struct ArmVerdict {
    // True only when every configuration-build check passed. There is no partial
    // "close enough", and this is the same `allPass()` the architecture names.
    bool ok = false;
    // What the authority would be given. Carried even when `ok` is false, so the
    // operator can see which readiness fact is missing.
    SafetyReadiness readiness{};
    ConfigurationCheckId firstFailedCheck = ConfigurationCheckId::Count;
    uint16_t failedCheckMask = 0U;
};

inline ArmVerdict armVerdictFor(const ConfigurationBuildChecks& checks) {
    ArmVerdict verdict;
    verdict.ok = checks.allPass();
    verdict.readiness = armReadinessFor(checks);
    verdict.firstFailedCheck = checks.firstFailure();
    verdict.failedCheckMask = checks.failureMask();
    return verdict;
}

// ---------------------------------------------------------------------------
// Telemetry rate
// ---------------------------------------------------------------------------

// Why a requested telemetry rate was refused.
enum class TelemetryRateRefusal : uint8_t {
    None = 0,
    BelowMinimum,        // a rate under 1 Hz: too sparse to describe a run
    AboveControlCadence, // faster than the loop can sample; the effective rate would silently differ
};

inline const char* telemetryRateRefusalName(TelemetryRateRefusal refusal) {
    switch (refusal) {
        case TelemetryRateRefusal::None:
            return "None";
        case TelemetryRateRefusal::BelowMinimum:
            return "BelowMinimum";
        case TelemetryRateRefusal::AboveControlCadence:
            return "AboveControlCadence";
    }
    return "None";
}

// The minimum accepted sample rate, samples per second.
constexpr double kTelemetryRateMinHz = 1.0;

struct TelemetryRateVerdict {
    bool ok = false;
    TelemetryRateRefusal refusal = TelemetryRateRefusal::None;
    // One sample every this many control ticks. Always >= 1.
    uint32_t strideTicks = 1U;
    // The rate the stride actually produces at the supplied cadence. Reported so
    // the operator sees the realized rate, not the requested one: a 30 Hz request
    // on a 50 Hz loop is 25 Hz in practice.
    double effectiveRateHz = 0.0;
};

// Maps a requested sample rate onto a whole number of control ticks.
//
// The mapping is here, and not in the queue or in the transport, because both of
// those are rate-agnostic: the queue stores records, the transport moves bytes,
// and neither knows what a second is. The loop cadence arrives as an argument
// for the same reason - the pure core reads no clock.
inline TelemetryRateVerdict telemetryStrideForRate(double requestedRateHz, uint16_t controlCadenceHz) {
    TelemetryRateVerdict verdict;
    if (!(requestedRateHz >= kTelemetryRateMinHz)) {
        // Also rejects a non-finite or negative request.
        verdict.refusal = TelemetryRateRefusal::BelowMinimum;
        return verdict;
    }
    if (controlCadenceHz == 0U || requestedRateHz > static_cast<double>(controlCadenceHz)) {
        verdict.refusal = TelemetryRateRefusal::AboveControlCadence;
        return verdict;
    }

    const double rawStride = static_cast<double>(controlCadenceHz) / requestedRateHz;
    uint32_t stride = static_cast<uint32_t>(std::floor(rawStride + 0.5));
    if (stride < 1U) {
        stride = 1U;
    }
    if (stride > static_cast<uint32_t>(controlCadenceHz)) {
        stride = static_cast<uint32_t>(controlCadenceHz);
    }
    verdict.ok = true;
    verdict.strideTicks = stride;
    verdict.effectiveRateHz = static_cast<double>(controlCadenceHz) / static_cast<double>(stride);
    return verdict;
}

}  // namespace ev
