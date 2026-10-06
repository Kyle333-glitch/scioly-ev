#pragma once

#include "Units.h"
#include "VehicleState.h"

#include <cmath>
#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The run-scoped maximum-duration bound
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The one runtime consumer of `SafetyLimits::maxRunDurationS`: the global
//   bound on how long a single run may last, measured from the run's own
//   actuation. It answers one question per control cycle - "has this run been
//   going longer than its validated maximum?" - and it answers it from the
//   clock reading the tick was given, nothing else.
// * **One run-scoped elapsed timer, and no second copy of the run's start.**
//   The run's actuation reading has exactly one owner, `CompetitionStateMachine`
//   (`RunTimeline::actuatedAtMs`, S8); this module is handed that reading and
//   never stamps a start of its own. A timer that kept its own start could
//   disagree with the run about when the run began, which is exactly the defect
//   the `RunTimeline` was consolidated to remove.
// * Independent of the per-phase timeouts. A phase bound catches a phase that
//   never received its evidence; this bound catches a run that is still asking
//   for motion after the whole run should have been over, whichever phases it
//   passed through and however many times it changed phase. It is evaluated
//   every cycle, in every phase, and it raises its own fault identity
//   (`FaultCode::RunDurationExceeded`), never `PhaseTimeout`.
// * Independent of the serial interface. The verdict is a function of the clock
//   and the run's own recorded facts; no host, no command and no USB state
//   enters it, and it is computed in the control tick whether or not any host is
//   attached.
//
// What this module is NOT
//
// * It does not enforce anything itself. It returns a verdict; the vehicle layer
//   feeds the resulting fault identity into the run policy and the safety
//   boundary exactly as it feeds any other observed fault, so the stop is the
//   authority's single deterministic fault response and not a second stopping
//   path.
// * It does not decide when a run starts or ends. `runActive` is a fact the run
//   layer states from its own timeline and phase (`runDurationClockActive()` in
//   `CompetitionStateMachine.h`); this module bounds the interval it is given.
// * It is not a measurement of the vehicle. The elapsed time is the firmware's
//   own clock, and the limit is a configured ceiling - until that ceiling is
//   `Measured` no run is bounded by it.

// Whether the configured maximum run duration may bound a run: a measured,
// finite, positive duration.
//
// Only a validated limit bounds anything. A `Provisional` placeholder is data
// with no runtime effect - enforcing a guessed duration would stop a legal run
// for a reason nobody measured - and a non-finite or non-positive value is not a
// duration at all. This is deliberately the same `Measured`-only rule the rest
// of the chain uses (`validateSafetyLimits()`, `validateRunPhaseConfig()`), so a
// limit that has not been established cannot silently start stopping runs.
inline bool runDurationLimitUsable(const Sourced<double>& maxRunDurationS) {
    return maxRunDurationS.isMeasured() && std::isfinite(maxRunDurationS.value) &&
           maxRunDurationS.value > 0.0;
}

// The run's elapsed-duration verdict for one cycle.
struct RunDurationVerdict {
    // The run's own clock is running: it has been actuated, it has not finished
    // and it has not faulted. Supplied by the run layer.
    bool runActive;

    // Whole milliseconds since the run's actuation reading, computed with
    // unsigned subtraction so it stays correct across the `millis()` wrap at
    // ~49.7 days (`SOFTWARE_ARCHITECTURE.md` section 4). Zero when the run's
    // clock is not running, so an inactive run never reports an elapsed time.
    uint32_t elapsedMs;

    // The configured maximum is a validated bound (`runDurationLimitUsable()`).
    bool limitUsable;

    // The bound was exceeded: `runActive && limitUsable && elapsedMs >= the
    // bound`. False whenever either precondition is false, so a placeholder
    // limit and an inactive run both report "not exceeded".
    bool exceeded;
};

// The run's maximum-duration verdict for this cycle.
//
// `runStartedAtMs` is the run's own actuation reading (`RunTimeline::
// actuatedAtMs`) - the single owner of when the run began. It is meaningful only
// when `runActive` is true, which is why an inactive run ignores it entirely
// rather than computing an elapsed time from a stale stamp.
//
// Wrap safety: `nowMs - runStartedAtMs` is an unsigned difference, so a run that
// spans the `uint32` wrap still measures its true elapsed milliseconds. A stamp
// from the future (a reset clock) makes the difference enormous, which reports
// the bound as exceeded - the safe direction, and exactly the same treatment
// `sampleAge()` gives a future stamp.
//
// Reset and re-arm: nothing here is remembered. A run boundary clears the run's
// actuation record (`CompetitionStateMachine::reset()`), so the next evaluation
// reports `runActive == false`; a new announcement re-stamps the actuation
// reading, and the fresh run is measured from that stamp. There is no way for a
// previous run's elapsed time to leak into a later one.
inline RunDurationVerdict runDurationVerdict(bool runActive, uint32_t runStartedAtMs,
                                             uint32_t nowMs,
                                             const Sourced<double>& maxRunDurationS) {
    RunDurationVerdict verdict{};
    verdict.runActive = runActive;
    verdict.limitUsable = runDurationLimitUsable(maxRunDurationS);
    verdict.elapsedMs = 0U;
    verdict.exceeded = false;
    if (!runActive) {
        return verdict;
    }
    verdict.elapsedMs = static_cast<uint32_t>(nowMs - runStartedAtMs);
    if (verdict.limitUsable) {
        // The one conversion from the configured seconds to the clock's
        // milliseconds (`Units.h`), so every timer in the firmware measures the
        // same way. A measured duration positive but below one millisecond
        // truncates to a zero-millisecond bound, so the first active cycle is
        // already over it - the safe direction for a bound nobody would set.
        const uint32_t limitMs = msFromSeconds(maxRunDurationS.value);
        verdict.exceeded = verdict.elapsedMs >= limitMs;
    }
    return verdict;
}

// The fault identity a verdict raises, or `FaultCode::None` while the run is
// within its bound. This is the one place the duration bound names a fault, so
// the vocabulary stays in `VehicleState.h` and the comparison stays here.
inline FaultCode runDurationFault(const RunDurationVerdict& verdict) {
    return verdict.exceeded ? FaultCode::RunDurationExceeded : FaultCode::None;
}

}  // namespace ev
