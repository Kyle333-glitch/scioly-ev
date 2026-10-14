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
    // A travel phase stopped making progress toward the leg's required execution
    // target: the run was travelling, but the position it acts on did not advance
    // toward the landmark its completion depends on for the configured
    // `RunPhaseConfig::travelStallTimeoutS`. This is the bounded terminal outcome
    // for a run that has commanded the vehicle's stop (the target shaper reached
    // zero inside the brake offset) and whose vehicle did not coast the remaining
    // distance to the landmark - the short-stop the taper/landmark contract leaves
    // open. It is deliberately distinct from `PhaseTimeout`: the phase bound waits
    // for a phase's whole budget to expire, while this names the specific fact
    // that the travel itself is no longer going anywhere, and it is deliberately
    // not `RunDurationExceeded`, which bounds the run as a whole. It is a
    // software-only identity: it carries no measured electrical meaning, so its
    // response is the same safe fault stop as every other non-electrical fault.
    TravelStalled,
    // Accepted powered travel stopped showing encoder progress: control cycles
    // kept authorizing a driving mode while the signed tick count stopped
    // advancing in the expected direction for the configured
    // `RunPhaseConfig::encoderProgressTimeoutS`. This is the bounded response to
    // lost feedback (frozen counts, wrong-sign counts, or incoherent counts
    // under power) - the frozen-encoder defect the October audits record. It is
    // deliberately distinct from `TravelStalled`: that bound watches the
    // plan-owned remaining distance from brake entry and names a vehicle that
    // stopped short of its landmark, while this watches raw tick progress
    // during any accepted powered travel and names lost feedback. A fresh
    // sample of an unchanged count is not progress here, and neither is an
    // invalid velocity estimate a reason to pause: the monitor consumes ticks
    // only. It is a software-only identity with the same safe fault stop as
    // every other non-electrical fault.
    EncoderFeedbackLost,
};

// A fault together with when it happened and whether it is latched. Fault
// handling behavior belongs to the safety boundary; this type only carries the
// data.
struct FaultState {
    FaultCode code = FaultCode::None;
    bool latched = false;
    uint32_t raisedAtMs = 0;
};

// Selects the fault of record for operator-event attribution (R3/L2): the run
// policy's fault first, else the safety authority's latch. The precedence
// mirrors what the control tick itself reports - `safetyState.detectedFault`
// prefers the run's fault over the cycle's - so an event's `fault` field and
// the tick snapshot that surrounds it cannot disagree about which fault is
// current. A historical fault a reset just cleared is therefore never
// reported as current: the caller reads this after the reset took effect,
// while the discarded fault travels in the event's `detail`, not its `fault`.
inline FaultCode currentFaultOfRecord(FaultCode runFault, FaultCode authorityFault) {
    return runFault != FaultCode::None ? runFault : authorityFault;
}

}  // namespace ev
