#pragma once

#include <cmath>
#include <cstdint>

#include "Motion.h"
#include "Units.h"
#include "VehicleState.h"

namespace ev {

// ---------------------------------------------------------------------------
// Encoder-progress supervision for accepted powered travel (S18C/P4)
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The one bounded answer to "the encoder was sampled but the vehicle may
//   not be moving": while control cycles keep accepting powered travel, the
//   signed tick count must keep advancing in the expected direction. When it
//   does not for the configured timeout, the monitor reports a violation and
//   the vehicle layer raises `FaultCode::EncoderFeedbackLost` through the
//   existing observed-fault path (run policy latch + safety-authority latch +
//   single fault stop), exactly like `RunDurationGuard`.
// * A guard on *accepted* powered travel, not on requests: `poweredAccepted`
//   is whether the safety authority's returned command of the previous cycle
//   was a driving mode. A regulator request that the boundary refused, a
//   shaped target, or a run intention is never sufficient to start or sustain
//   the interval.
// * A guard on *edges*, not on samples: a fresh sample of an unchanged count
//   is not progress. Only a newly observed signed tick advance in the leg's
//   expected direction (the leg mapped through the measured positive-ticks
//   polarity) resets the interval. Repeated reads of one count, an invalid
//   velocity estimate, a wrong-sign tick move, or old movement beyond the last
//   progress never resets it.
// * An accumulator of *powered* time without progress, not wall time: cycles
//   without accepted powered travel (Disabled/brake/dwell/stop) neither
//   accumulate nor reset - they follow the position with the reference so a
//   later powered cycle is judged on movement under power, not on coasting or
//   handling during the pause. Toggling between powered and unpowered therefore
//   cannot renew the allowance indefinitely: only genuine progress resets it.
// * Inert without a validated bound: a `Provisional` (or non-finite or
//   non-positive) timeout or an unmeasured/unknown tick polarity pauses the
//   monitor instead of faulting. Production therefore gains no new refusal
//   while its placeholders are unmeasured - and no run may be armed on them
//   either (`validateRunPhaseConfig()`). A dropped sample with a validated
//   bound is NOT a pause: sustained-invalid exposure during accepted power
//   consumes the same allowance a frozen count consumes (R2/S18R-M1), so
//   repeated invalid readings can withhold evidence but never renew power.
//
// What this module is not
//
// * It enforces nothing itself. It returns a boolean; the vehicle layer (and
//   the scenario harness, which mirrors it) feeds the resulting fault identity
//   into the run policy and the safety boundary exactly as any other observed
//   fault, so the stop is the authority's single deterministic fault response
//   and not a second stopping path.
// * It is not a standstill detector, a slip detector, or a scale check. One
//   motor encoder cannot prove the vehicle stopped, distinguish slip from
//   motion, or detect a coherently wrong scale: a single dithering tick still
//   counts as progress here, which is why the progress quantum and the timeout
//   both require physical qualification (P-H5/H7/H9) before any production run.
// * It is not the brake-entry short-stop bound (`TravelStalled` in
//   `CompetitionStateMachine.h`): that bound watches the plan-owned remaining
//   distance from brake entry and names a vehicle that stopped short of its
//   landmark. This monitor watches raw tick progress during any accepted
//   powered travel and names lost feedback. The two are distinct failure
//   mechanisms with distinct identities.
// * It owns no timeout value. The bound arrives every update as the caller's
//   `Sourced<double>` seconds (the run policy's `encoderProgressTimeoutS`),
//   exactly like `runDurationVerdict()` is handed its limit.

// Whether the configured progress timeout may bound anything: a measured,
// finite, positive duration. A placeholder is data with no runtime effect, and
// a non-finite or non-positive value is not a duration at all.
inline bool encoderProgressLimitUsable(const Sourced<double>& timeoutS) {
    return timeoutS.isMeasured() && std::isfinite(timeoutS.value) && timeoutS.value > 0.0;
}

// Whether the tick polarity may decide an expected direction: a measured,
// known direction. An unmeasured or unknown encoding agrees with nothing, so
// the monitor pauses rather than guessing a sign.
inline bool encoderProgressPolarityUsable(
    const Sourced<MotionDirection>& positiveTicksDirection) {
    return positiveTicksDirection.isMeasured() &&
           isKnownMotionDirection(positiveTicksDirection.value);
}

// The expected tick sign for the leg being travelled: +1 when the leg's
// direction is the decoder's positive convention, -1 otherwise.
inline int8_t expectedTickSignForLeg(bool legForward,
                                     const Sourced<MotionDirection>& positiveTicksDirection) {
    const bool positiveIsForward =
        positiveTicksDirection.value == MotionDirection::Forward;
    const bool legIsPositive = (legForward == positiveIsForward);
    return legIsPositive ? 1 : -1;
}

// Bounded supervision of encoder progress during accepted powered travel.
//
// Call once per control cycle, before the run policy is stepped, with the
// previous cycle's accepted-powered fact (the authority's returned command
// was a driving mode). Returns true exactly when the bound is violated and
// the caller must raise `FaultCode::EncoderFeedbackLost`.
//
// Timing uses the caller's measured `dtMs` (the control interval that
// actually elapsed), accumulated only on powered cycles without progress, so
// the bound measures powered exposure rather than wall time and stays correct
// across the `millis()` wrap: a wrap changes the stamp, never the accumulated
// sum. A zero `dtMs` (a repeated stamp) accumulates nothing and faults
// nothing; a very large `dtMs` (a long scheduling gap) accumulates at once
// and faults on the next powered cycle without progress, which is the safe
// direction.
class EncoderProgressMonitor {
 public:
    EncoderProgressMonitor() = default;

    // Returns the monitor to its post-construction state: no reference, no
    // accumulated powered time. The new-run boundary calls this; nothing
    // resumes automatically and no previous attempt survives.
    void reset() {
        hasReference_ = false;
        referenceTicks_ = 0;
        poweredMsWithoutProgress_ = 0U;
    }

    // Ticks observed at the last progress, for diagnostics. Zero when no
    // reference has been taken yet.
    int32_t referenceTicks() const { return referenceTicks_; }

    // Powered milliseconds accumulated without progress. Zero after any
    // genuine progress, while paused, and after reset().
    uint32_t poweredMsWithoutProgress() const { return poweredMsWithoutProgress_; }

    bool update(int32_t ticks, bool sampleFresh, bool poweredAccepted, bool legForward,
                const Sourced<MotionDirection>& positiveTicksDirection,
                const Sourced<double>& timeoutS, uint32_t dtMs) {
        // No enforcement without a validated bound: a placeholder timeout or
        // polarity is data with no runtime effect. Follow the position so a
        // later powered cycle is judged on movement under power, and report
        // no violation. This pauses rather than faults, so placeholders can
        // only withhold protection, never stop a run for a reason nobody
        // established.
        if (!encoderProgressLimitUsable(timeoutS) ||
            !encoderProgressPolarityUsable(positiveTicksDirection)) {
            referenceTicks_ = ticks;
            hasReference_ = true;
            return false;
        }
        // Not powered travel: the vehicle is stationary on purpose
        // (Disabled/brake/dwell/stop) or the boundary refused the request.
        // Follow the position, freeze the accumulation, report no violation.
        // Only accepted powered travel consumes the allowance.
        if (!poweredAccepted) {
            referenceTicks_ = ticks;
            hasReference_ = true;
            return false;
        }
        // The startup allowance: the first powered cycle after reset, after a
        // pause configuration became usable, or after any unobserved interval
        // takes its reference now and owns the full timeout from here. There
        // is no separate startup constant: the allowance is the same bound,
        // baselined at the first accepted authorization rather than at
        // announce/arm (which are operator decisions, not motion). An invalid
        // first sample baselines identically - it owns the allowance, not a
        // fault - and every invalid cycle after it consumes the allowance
        // below instead of pausing (R2/S18R-M1).
        if (!hasReference_) {
            referenceTicks_ = ticks;
            hasReference_ = true;
            poweredMsWithoutProgress_ = 0U;
            return false;
        }
        // R2/S18R-M1: sustained-invalid exposure. No usable observation on a
        // powered cycle with a validated bound is powered exposure without
        // progress evidence, so it consumes the SAME allowance a frozen count
        // consumes: accumulate the elapsed control interval and fault at the
        // bound. Repeated short invalid readings accumulate across cycles -
        // only genuine expected-direction progress resets - and alternating
        // invalid with frozen-but-valid cannot renew power either. The
        // reference still follows the reported count, so a later powered
        // cycle with fresh samples is judged on movement under power from
        // here, not on the invalid interval. A single missed sample (tens of
        // milliseconds against a hundreds-of-milliseconds bound) never faults:
        // a temporarily missing sample and a sustained invalid condition are
        // distinguished by the same bound.
        if (!sampleFresh) {
            referenceTicks_ = ticks;
            return consume(dtMs, timeoutS);
        }
        // Signed advance in the expected direction, formed in 64 bits so a
        // full-range int32 reading pair is not undefined behaviour. Strictly
        // positive (>= 1 tick net from the reference) counts; anything else -
        // stationary, wrong-sign, or back below the reference - does not.
        const int64_t rawDelta =
            static_cast<int64_t>(ticks) - static_cast<int64_t>(referenceTicks_);
        const int8_t expectSign = expectedTickSignForLeg(legForward, positiveTicksDirection);
        const int64_t advance = (expectSign > 0) ? rawDelta : -rawDelta;
        if (advance > 0) {
            referenceTicks_ = ticks;
            poweredMsWithoutProgress_ = 0U;
            return false;
        }
        // No progress under power: consume the allowance by the interval that
        // actually elapsed, saturating instead of wrapping on extreme gaps.
        return consume(dtMs, timeoutS);
    }

    // Consumes the powered-without-progress allowance by the interval that
    // actually elapsed - saturating instead of wrapping on extreme gaps -
    // and reports whether the validated bound is reached. Shared by the
    // frozen/wrong-sign path above and the sustained-invalid path (R2/S18R-M1)
    // so both shapes of missing progress evidence drain one allowance.
    bool consume(uint32_t dtMs, const Sourced<double>& timeoutS) {
        const uint64_t grown =
            static_cast<uint64_t>(poweredMsWithoutProgress_) + static_cast<uint64_t>(dtMs);
        poweredMsWithoutProgress_ =
            grown >= 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(grown);
        // The one conversion from configured seconds to clock milliseconds
        // (`Units.h`), so every timer measures the same way. A measured
        // duration below one millisecond truncates to zero and the first
        // powered cycle without progress is already over it - the safe
        // direction for a bound nobody would set.
        const uint32_t boundMs = msFromSeconds(timeoutS.value);
        return poweredMsWithoutProgress_ >= boundMs;
    }

 private:
    bool hasReference_ = false;
    int32_t referenceTicks_ = 0;
    uint32_t poweredMsWithoutProgress_ = 0U;
};

// The fault identity this monitor raises, or `FaultCode::None` while the
// bound holds. This is the one place the progress bound names a fault, so the
// vocabulary stays in `VehicleState.h` and the comparison stays here.
inline FaultCode encoderProgressFault(bool violated) {
    return violated ? FaultCode::EncoderFeedbackLost : FaultCode::None;
}

}  // namespace ev
