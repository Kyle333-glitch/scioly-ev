#pragma once

#include <cstdint>

#include "CompetitionStateMachine.h"
#include "ConfigurationChecks.h"
#include "Motion.h"
#include "RunConfig.h"
#include "SafetyAuthority.h"
#include "VehicleState.h"

namespace ev {

// ---------------------------------------------------------------------------
// The telemetry schema - records, wire names, and the version identifier
// ---------------------------------------------------------------------------
//
// This header freezes the *shape* of what the firmware streams and the
// vocabulary inside it, because a log is only evidence if the same bytes keep
// meaning the same thing. What is frozen:
//
// * the record kinds and their prefixes, including the schema version;
// * the field set of each record and the exact spelling of every field name;
// * the wire spelling of every enumerated value that appears in a record.
//
// What is deliberately *not* frozen here: the numeric formatting, the drop
// policy and the transport. Those live in `TelemetryFormat.h`,
// `TelemetryQueue.h` and the vehicle layer, and they can change without
// invalidating an old log, because they do not change what a field means.
//
// Two record kinds, on purpose:
//
// * a **sample** is one control tick's complete state, emitted periodically;
// * an **event** is a transition worth reading on its own - a phase change, an
//   arming change, a fault appearing, an operator action and its outcome.
//
// A sample train alone hides transitions (50 Hz of `phase=Approach` then 50 Hz
// of `phase=Capture` says nothing about *when* the change happened relative to a
// fault), and an event-only log cannot show what the vehicle was actually doing.
// A reader that wants a timeline reads events; a reader that wants behavior
// reads samples.
//
// Defaults: these records describe; they never authorize. A default-constructed
// record therefore may not claim a permission, which is why `outcome` below
// defaults to a *denied* outcome rather than to the zero value of its
// enumerator (`AuthorizedDrive`). Every field the control path acts on is filled
// by the vehicle layer before a record is stored; a default value here is what a
// never-filled record looks like, and it must read as "nothing happened".

// The schema version. Bump it when a field name, a field's meaning or the wire
// vocabulary changes incompatibly; an analysis script can then refuse a log it
// does not understand instead of misreading a column.
constexpr uint8_t kTelemetrySchemaVersion = 1U;

// Record prefixes. The digit is the schema version, so a record is
// self-describing: a log that was written by a firmware whose schema changed
// cannot be mistaken for this one, even if the version is the only difference.
// The `EV` root keeps the convention `AGENTS.md` section 16 illustrates.
constexpr const char* kTelemetrySamplePrefix = "EVS1";
constexpr const char* kTelemetryEventPrefix = "EVE1";

// A record that is missing fields carries this marker instead of a number or a
// name, so a truncation is visible in the bytes rather than inferred from a
// short line.
constexpr const char* kTelemetryMissingField = "-";

// The width of every metres and metres-per-second field, in decimals: 0.1 mm and
// 0.1 mm/s. It lives with the schema rather than with either formatter because
// both serial consumers write these quantities - the telemetry records and the
// operator responses - and the same fact written at two resolutions would be a
// discrepancy a reader could not explain. `BoundedText::appendFixed()` clamps it
// to the four decimals it supports.
constexpr uint8_t kTelemetryFieldDecimals = 4U;

// ---------------------------------------------------------------------------
// Wire names
// ---------------------------------------------------------------------------
//
// Every name below is part of the schema. Adding an enumerator adds a name;
// renaming one changes the schema and requires a version bump.

inline const char* telemetryPhaseName(RunPhase phase) {
    switch (phase) {
        case RunPhase::Idle:
            return "Idle";
        case RunPhase::ParameterValid:
            return "ParameterValid";
        case RunPhase::Armed:
            return "Armed";
        case RunPhase::OptionalDelay:
            return "OptionalDelay";
        case RunPhase::Approach:
            return "Approach";
        case RunPhase::Capture:
            return "Capture";
        case RunPhase::Push:
            return "Push";
        case RunPhase::ForwardDecel:
            return "ForwardDecel";
        case RunPhase::BrakeZeroSpeed:
            return "BrakeZeroSpeed";
        case RunPhase::Reverse:
            return "Reverse";
        case RunPhase::FinalApproach:
            return "FinalApproach";
        case RunPhase::FinalBrake:
            return "FinalBrake";
        case RunPhase::Done:
            return "Done";
        case RunPhase::Fault:
            return "Fault";
    }
    return "?";
}

inline const char* telemetryRunCommandName(RunMotionCommand command) {
    switch (command) {
        case RunMotionCommand::None:
            return "none";
        case RunMotionCommand::DriveForward:
            return "DriveForward";
        case RunMotionCommand::DriveReverse:
            return "DriveReverse";
        case RunMotionCommand::RampDownForward:
            return "RampDownForward";
        case RunMotionCommand::RampDownReverse:
            return "RampDownReverse";
        case RunMotionCommand::BrakeForward:
            return "BrakeForward";
        case RunMotionCommand::BrakeReverse:
            return "BrakeReverse";
    }
    return "?";
}

inline const char* telemetryMotorModeName(MotorMode mode) {
    switch (mode) {
        case MotorMode::Disabled:
            return "Disabled";
        case MotorMode::Coast:
            return "Coast";
        case MotorMode::Brake:
            return "Brake";
        case MotorMode::DriveForward:
            return "DriveForward";
        case MotorMode::DriveReverse:
            return "DriveReverse";
    }
    return "?";
}

inline const char* telemetryFaultName(FaultCode code) {
    switch (code) {
        case FaultCode::None:
            return "None";
        case FaultCode::InvalidRunConfig:
            return "InvalidRunConfig";
        case FaultCode::ProvisionalCalibration:
            return "ProvisionalCalibration";
        case FaultCode::MissingSafetyLimits:
            return "MissingSafetyLimits";
        case FaultCode::InvalidMotorEffort:
            return "InvalidMotorEffort";
        case FaultCode::PhaseTimeout:
            return "PhaseTimeout";
        case FaultCode::ReversalNotVerified:
            return "ReversalNotVerified";
        case FaultCode::DisarmedDuringRun:
            return "DisarmedDuringRun";
        case FaultCode::RunSequencingError:
            return "RunSequencingError";
    }
    return "?";
}

inline const char* telemetryAuthzOutcomeName(AuthzOutcome outcome) {
    switch (outcome) {
        case AuthzOutcome::AuthorizedDrive:
            return "AuthorizedDrive";
        case AuthzOutcome::AuthorizedClamped:
            return "AuthorizedClamped";
        case AuthzOutcome::AuthorizedStop:
            return "AuthorizedStop";
        case AuthzOutcome::ForcedFaultStop:
            return "ForcedFaultStop";
        case AuthzOutcome::DeniedNotArmed:
            return "DeniedNotArmed";
        case AuthzOutcome::DeniedNotReady:
            return "DeniedNotReady";
        case AuthzOutcome::DeniedStaleData:
            return "DeniedStaleData";
        case AuthzOutcome::DeniedIllegalEffort:
            return "DeniedIllegalEffort";
        case AuthzOutcome::DeniedUnsafeDirectionChange:
            return "DeniedUnsafeDirectionChange";
    }
    return "?";
}

// The rule-error name of a rejected configuration, as the operator and the log
// see it.
inline const char* telemetryConfigErrorName(ConfigError error) {
    switch (error) {
        case ConfigError::None:
            return "None";
        case ConfigError::NonFiniteValue:
            return "NonFiniteValue";
        case ConfigError::InvalidTournamentLevel:
            return "InvalidTournamentLevel";
        case ConfigError::TargetDistanceOutOfRange:
            return "TargetDistanceOutOfRange";
        case ConfigError::TargetDistanceOffInterval:
            return "TargetDistanceOffInterval";
        case ConfigError::TargetTimeOutOfRange:
            return "TargetTimeOutOfRange";
        case ConfigError::TargetTimeOffInterval:
            return "TargetTimeOffInterval";
        case ConfigError::BottleLineDistanceOutOfRange:
            return "BottleLineDistanceOutOfRange";
        case ConfigError::BottleLineDistanceOffInterval:
            return "BottleLineDistanceOffInterval";
        case ConfigError::BottleStartNotBeforeTarget:
            return "BottleStartNotBeforeTarget";
        case ConfigError::BottleStartNotLevelDistance:
            return "BottleStartNotLevelDistance";
    }
    return "?";
}

// The events the interface and the control path emit. Each one marks a moment
// that a reader cannot reconstruct from the sample train with certainty.
enum class TelemetryEventCode : uint8_t {
    Boot = 0,             // the firmware started; the first record of a session
    TelemetryEnabled,     // telemetry was enabled (the session's own boundary)
    PhaseChange,          // the run policy changed phase (`phase` is the new one)
    Armed,                // the authority reports armed
    Disarmed,             // the authority reports disarmed
    FaultLatched,         // a fault became the fault of record
    AnnounceAccepted,     // an announcement was accepted by the run policy
    AnnounceRefused,      // an announcement was refused (`detail` names the refusal)
    ArmRefused,           // an arming intent was refused (`detail` names the first failing check)
    ChainReset,           // the new-run boundary was taken
    ConfigSet,            // a configuration field was stored
    ConfigCleared,        // the held configuration was discarded
    CommandRejected,      // a parsed command was refused (`detail` names the reason)
};

inline const char* telemetryEventName(TelemetryEventCode code) {
    switch (code) {
        case TelemetryEventCode::Boot:
            return "Boot";
        case TelemetryEventCode::TelemetryEnabled:
            return "TelemetryEnabled";
        case TelemetryEventCode::PhaseChange:
            return "PhaseChange";
        case TelemetryEventCode::Armed:
            return "Armed";
        case TelemetryEventCode::Disarmed:
            return "Disarmed";
        case TelemetryEventCode::FaultLatched:
            return "FaultLatched";
        case TelemetryEventCode::AnnounceAccepted:
            return "AnnounceAccepted";
        case TelemetryEventCode::AnnounceRefused:
            return "AnnounceRefused";
        case TelemetryEventCode::ArmRefused:
            return "ArmRefused";
        case TelemetryEventCode::ChainReset:
            return "ChainReset";
        case TelemetryEventCode::ConfigSet:
            return "ConfigSet";
        case TelemetryEventCode::ConfigCleared:
            return "ConfigCleared";
        case TelemetryEventCode::CommandRejected:
            return "CommandRejected";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// How many records the stream has lost, per lane, since boot. Carried inside
// every record so a log says whether it is complete: a dropped record is
// invisible in the records that survive, and a signal that silently went missing
// is worse than one that is absent.
struct TelemetryDrops {
    uint32_t samples = 0U;
    uint32_t events = 0U;
};

// One control tick's state, as the schema defines it.
//
// Every field is a value the control path produced or a raw reading the vehicle
// layer took; nothing is recomputed for display, so a log cannot disagree with
// the control path about what happened. Raw sensor fields stay raw: `battery` and
// `current` are ADC counts and `driverFaultBarLow` is a logic level, because no
// raw-to-engineering-unit relationship is documented
// (`SOFTWARE_ARCHITECTURE.md` section 12) and inventing one in a log would make
// an unverified conversion look like a measurement.
struct TelemetrySample {
    uint32_t tMs = 0U;              // the tick's `millis()` reading
    uint16_t dtMs = 0U;             // the measured interval the tick was given
    RunPhase phase = RunPhase::Idle;
    RunMotionCommand command = RunMotionCommand::None;  // what the run asked for
    bool armed = false;             // the authority's armed state

    double positionM = 0.0;         // run-axis position, metres from the Start Point
    bool positionUsable = false;    // the combined position verdict
    double measuredVelocityMps = 0.0;
    bool velocityUsable = false;    // the estimator's freshness verdict
    double requestedVelocityMps = 0.0;  // the shaped target this cycle
    double remainingDistanceM = 0.0;    // signed remaining distance to the Target Point

    MotorMode motorMode = MotorMode::Disabled;  // the effort requested this cycle
    uint16_t effortPermille = 0U;

    uint16_t batteryCounts = 0U;    // raw ADC counts
    uint16_t currentCounts = 0U;    // raw ADC counts
    bool driverFaultBarLow = false; // raw nFAULT level, uninterpreted

    FaultCode fault = FaultCode::None;
    // Explicitly the denied default, never the zero value of the enumerator: a
    // record may not claim a granted drive it was not given.
    AuthzOutcome outcome = AuthzOutcome::DeniedNotArmed;
    // The most fundamental configuration check that failed, or `Count` ("none").
    // This is why a drive was refused, in one attributable field.
    ConfigurationCheckId firstFailedCheck = ConfigurationCheckId::Count;

    TelemetryDrops drops{};
};

// A moment worth its own record. `detail` is a *static* vocabulary name (the
// caller passes one of the `Name()` results above, or a literal such as `-`),
// never a dynamically built string: a record is stored by value in a
// fixed-size lane, so nothing in it may outlive a caller's buffer.
struct TelemetryEvent {
    uint32_t tMs = 0U;
    RunPhase phase = RunPhase::Idle;
    bool armed = false;
    TelemetryEventCode event = TelemetryEventCode::Boot;
    const char* detail = kTelemetryMissingField;
    FaultCode fault = FaultCode::None;
    TelemetryDrops drops{};
};

// The unit of transfer: one record of either kind, as the lane stores it and the
// transport sends it. The kind travels with the record because both lanes drain
// through one path, and both members are present rather than unioned: these are
// records, not a variant to be reasoned about, and the cost is a few dozen bytes
// on a stack that already holds the batch.
enum class TelemetryRecordKind : uint8_t {
    Sample = 0,
    Event = 1,
};

struct TelemetryRecord {
    TelemetryRecordKind kind = TelemetryRecordKind::Sample;
    TelemetrySample sample{};
    TelemetryEvent event{};
};

// ---------------------------------------------------------------------------
// Transition detection
// ---------------------------------------------------------------------------
//
// The sample train already carries every field of every event, so an event is
// not new information - it is the *transition* made explicit, which is the part
// a reader cannot afford to miss among 50 records a second. Detecting it needs
// one comparison against the previous tick, and that is all this class does: it
// owns no run state, commands nothing and cannot change what the run does.
//
// The batch is bounded at three because a single tick can reveal at most three
// independent transitions (a fault, an arming change and a phase change) and a
// bounded batch is what keeps the consumer allocation-free.

constexpr uint8_t kTelemetryEventBatchCapacity = 3U;

struct TelemetryEventBatch {
    TelemetryEvent events[kTelemetryEventBatchCapacity]{};
    uint8_t count = 0U;
};

class TelemetryEventDetector {
public:
    // Observes one tick's sample and reports the transitions it revealed, in a
    // fixed order: a newly latched fault first, then an arming change, then a
    // phase change. The first observation after construction (or after
    // `reset()`) reports nothing: the sample train's first record establishes
    // the baseline, and emitting the boot state as a "change" would be a
    // transition that did not happen.
    TelemetryEventBatch observe(const TelemetrySample& sample) {
        TelemetryEventBatch batch;
        if (!primed_) {
            prime(sample);
            return batch;
        }

        if (sample.fault != fault_) {
            if (sample.fault != FaultCode::None) {
                batch.events[batch.count] = makeEvent(sample, TelemetryEventCode::FaultLatched,
                                                     telemetryFaultName(sample.fault));
                ++batch.count;
            }
            fault_ = sample.fault;
        }

        if (sample.armed != armed_) {
            batch.events[batch.count] =
                makeEvent(sample, sample.armed ? TelemetryEventCode::Armed : TelemetryEventCode::Disarmed,
                          kTelemetryMissingField);
            ++batch.count;
            armed_ = sample.armed;
        }

        if (sample.phase != phase_) {
            batch.events[batch.count] = makeEvent(sample, TelemetryEventCode::PhaseChange,
                                                 telemetryPhaseName(sample.phase));
            ++batch.count;
            phase_ = sample.phase;
        }

        return batch;
    }

    void reset() { primed_ = false; }

private:
    void prime(const TelemetrySample& sample) {
        phase_ = sample.phase;
        armed_ = sample.armed;
        fault_ = sample.fault;
        primed_ = true;
    }

    TelemetryEvent makeEvent(const TelemetrySample& sample, TelemetryEventCode code,
                             const char* detail) const {
        TelemetryEvent event;
        event.tMs = sample.tMs;
        event.phase = sample.phase;
        event.armed = sample.armed;
        event.event = code;
        event.detail = detail;
        event.fault = sample.fault;
        event.drops = sample.drops;
        return event;
    }

    bool primed_ = false;
    RunPhase phase_ = RunPhase::Idle;
    bool armed_ = false;
    FaultCode fault_ = FaultCode::None;
};

}  // namespace ev
