#pragma once

#include <cstdint>

namespace ev {

// Run-state vocabulary lives in exactly one place: `RunPhase` in
// `CompetitionStateMachine.h`. It replaced the earlier provisional
// `CompetitionState` list, whose names were explicitly vocabulary-only and were
// frozen into a real, tested state machine in the run-policy stage. There is no
// second, competing phase list here: a phase name that exists in two headers is
// a name that will disagree with itself.

// What went wrong. This is fault *identity* only; the electrical response to a
// fault is a separate decision (`FaultStopPolicy` in SafetyAuthority.h).
//
// Faults raised before the run policy existed are all data/configuration
// problems. The run policy adds the sequencing faults: a phase that never got
// its evidence, a reversal that never verified a stop, a run that lost its armed
// state, and an inconsistent sequencing request. None of these describes a
// measured electrical condition, so none of them carries an electrical behavior
// of its own.
enum class FaultCode : uint8_t {
    // No fault. The value a default-constructed FaultState carries.
    None = 0,
    // The announced run configuration failed rule validation.
    InvalidRunConfig,
    // A required calibration value is still provisional.
    ProvisionalCalibration,
    // A required safety limit is still provisional.
    MissingSafetyLimits,
    // A commanded motor effort was invalid.
    InvalidMotorEffort,
    // A run phase exceeded its configured bound without receiving the evidence
    // that ends it.
    PhaseTimeout,
    // The near-zero stop a reversal depends on was not verified within the
    // configured reversal timeout.
    ReversalNotVerified,
    // The safety authority was disarmed while the run was underway, so nothing
    // that was still being asked for could be powered.
    DisarmedDuringRun,
    // The run policy observed a sequencing inconsistency it must not act on
    // (a direction change requested that is not a change of direction, or a
    // reversal requested while one is already being sequenced).
    RunSequencingError,
    // The planned driver's fault output reported a fault. This identity is
    // produced only when the line's polarity has been measured
    // (`DriverFault.h`); until then the raw level is reported and no fault is
    // claimed, because an unverified polarity would turn a healthy driver into a
    // fault or a real fault into a log entry nobody acts on.
    DriverFaultBar,
    // A run exceeded its validated maximum duration (`RunDurationGuard.h`): the
    // whole run, measured from its actuation, went on longer than
    // `SafetyLimits::maxRunDurationS` allows. This is deliberately a distinct
    // identity from `PhaseTimeout`: a phase bound catches one phase that never
    // received its evidence, while this catches the run as a whole however many
    // phases it passed through. It is raised only when the configured maximum is
    // a `Measured` duration, so a placeholder can never stop a run for a reason
    // nobody established.
    RunDurationExceeded,
};

// A fault together with when it happened and whether it is latched. Fault
// handling behavior belongs to the safety boundary; this type only carries the
// data.
struct FaultState {
    FaultCode code = FaultCode::None;
    bool latched = false;
    uint32_t raisedAtMs = 0;
};

}  // namespace ev
