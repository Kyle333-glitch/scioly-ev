#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// When the control tick runs - pure deadline logic
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The scheduling decision for one pass of the vehicle's main loop: has the
//   next control deadline arrived, and what did the clock say about it.
// * Arduino-free and clock-free. Every call takes the caller's millisecond
//   reading (`millis()` on the vehicle). Nothing here reads time, so the whole
//   module is host-testable with a fake clock and needs no vehicle to exist
//   (SOFTWARE_ARCHITECTURE.md section 3).
// * Wrap-safe, and never blocking: it decides, it does not wait. The loop does
//   the waiting by simply returning and being called again.
//
// What this module is not
//
// * It is not the control tick. It never samples a sensor, runs a state
//   machine, regulates, authorizes or writes a pin. The order of the work that
//   runs when a tick is due is owned by `ControlTickOrder.h`.
// * It owns no hardware timer and no RTOS. The vehicle loop polls it; there is
//   no interrupt-driven scheduler and nothing here runs in an ISR.
// * It owns no period value. The period is configuration and arrives in the
//   constructor (the vehicle layer's own configuration, not a constant here),
//   so the cadence can be changed without touching this arithmetic.
//
// The two scheduling decisions this module makes
//
//   1. **Deadline, not "elapsed since last".** A tick is due when the clock
//      reading has reached `nextDeadlineMs`. That test is a signed difference
//      of two uint32 readings, which is correct across the ~49.7 day wrap:
//
//          (int32_t)(nowMs - nextDeadlineMs) >= 0
//
//      A reading that is up to 2^31-1 ms past a deadline reads as due; one that
//      is behind it reads as not due. This is the same unsigned-subtraction
//      discipline the rest of the project uses (SOFTWARE_ARCHITECTURE.md
//      section 4), with the sign taken only to decide "past or not yet".
//
//   2. **A missed deadline is dropped, counted and reported - never caught
//      up.** If one pass overruns by three whole periods, the loop owes three
//      ticks; running them back to back would sample three times with almost no
//      time between samples, and would push the following work later still. So
//      the schedule re-arms from the tick that actually ran and reports how many
//      deadlines went by unseen. The average cadence is therefore the period
//      *plus* whatever a pass costs, and telemetry can see that cost as
//      lateness instead of it being hidden inside a burst.
//
//   *Per-decision* numbers answer "what did this pass do". They cannot answer
//   "what was the worst this loop has been", which is the question a soak test
//   asks (P-H0 Step 7). So the scheduler also keeps the two running extremes
//   over the session since `start()`: the worst lateness any committed tick
//   reported, and the cumulative count of deadlines dropped. Both are
//   observations about the loop, not faults, and neither changes the decision.
//
// A rewound clock reading
//
//   A reading that is older than the previous one the scheduler saw is reported
//   (`clockRewind`) and cannot make anything happen: deadlines are in the past
//   relative to the old reading, so the new one is not due and the schedule
//   waits until the clock catches up. Nothing here latches a fault over it -
//   which reading is trustworthy is the vehicle layer's decision, and the run
//   policy already has its own, stated answer for a rewound clock.

// The measured facts behind one scheduling decision. A default-constructed
// value reports "not due" with no numbers claimed, so a forgotten assignment
// fails closed instead of looking like a tick at millisecond zero.
struct ControlTickDecision {
    constexpr ControlTickDecision(bool due_, uint32_t dtMs_, uint32_t lateByMs_,
                                  uint32_t droppedTicks_, uint32_t waitMs_, bool clockRewind_)
        : due(due_),
          dtMs(dtMs_),
          lateByMs(lateByMs_),
          droppedTicks(droppedTicks_),
          waitMs(waitMs_),
          clockRewind(clockRewind_) {}

    // True when the next deadline has arrived or passed: run the control tick.
    bool due = false;

    // Measured milliseconds since the previous completed control tick, for the
    // modules that take an elapsed time (the profile and the regulator). Before
    // the first tick it is the interval since `start()`. It is a measurement,
    // never the nominal period: a pass that took longer says so. 0 when this
    // decision is not due, because a pass that runs no tick has no interval to
    // claim and `dtMs` must never be read as one.
    uint32_t dtMs = 0U;

    // How far past the deadline this pass is, in milliseconds. 0 when not due.
    // The loop's own overshoot, reported rather than absorbed.
    uint32_t lateByMs = 0U;

    // Whole periods that the deadline passed through without a tick, dropped by
    // this decision rather than caught up. Non-zero exactly when the loop
    // overran by at least one full period.
    uint32_t droppedTicks = 0U;

    // Milliseconds until the next deadline. 0 when due. A value of hours here
    // is what a rewound or replaced clock looks like to this module.
    uint32_t waitMs = 0U;

    // True when this reading is older than the previous reading the scheduler
    // decided from. Reported only; the decision above already refuses to run.
    bool clockRewind = false;
};
// The deadline schedule for one control loop. One instance, one period, one
// caller: the vehicle layer's pass, once per loop iteration.
class ControlScheduler {
public:
    // `periodMs` is the nominal control interval. A period of 0 is treated as
    // 1 ms: a zero period would make every pass due and would divide by zero
    // when counting dropped deadlines, and neither is a cadence anyone asked
    // for. The nominal period is never used as an elapsed time - `dtMs` is
    // always measured.
    explicit ControlScheduler(uint32_t periodMs)
        : periodMs_(periodMs == 0U ? 1U : periodMs) {}

    uint32_t periodMs() const { return periodMs_; }

    // Arms the schedule. The first deadline is one period after `nowMs`, and the
    // first tick's dtMs is measured from `nowMs`. Deliberately explicit and
    // separate from construction: before `start()` no tick is ever due, so a
    // layer that forgets to start the scheduler does no control work at all
    // rather than doing it at millisecond zero.
    void start(uint32_t nowMs) {
        started_ = true;
        hasLastTick_ = false;
        lastTickMs_ = nowMs;
        hasLastDecision_ = false;
        lastDecisionMs_ = nowMs;
        nextDeadlineMs_ = nowMs + periodMs_;
        ticks_ = 0U;
        droppedTotal_ = 0U;
        worstLateByMs_ = 0U;
        worstStallMs_ = 0U;
    }

    // The decision for this pass, from this pass's clock reading.
    //
    // Call once per pass: the reading is remembered so a rewound one can be
    // recognized, so calling this twice for one pass would make the second call's
    // reading the reference. It changes no deadline - only `advance()` does,
    // after the tick has actually run.
    ControlTickDecision decide(uint32_t nowMs) {
        const bool clockRewind =
            hasLastDecision_ && static_cast<int32_t>(nowMs - lastDecisionMs_) < 0;
        lastDecisionMs_ = nowMs;
        hasLastDecision_ = true;

        if (!started_) {
            // Not armed: no deadline exists, so nothing is ever due.
            return ControlTickDecision(false, 0U, 0U, 0U, 0U, clockRewind);
        }

        const int32_t sinceDeadlineMs = static_cast<int32_t>(nowMs - nextDeadlineMs_);
        if (sinceDeadlineMs < 0) {
            // Not yet due. The remaining wait is the wrapped distance to the
            // deadline, which stays within one period for a monotonic clock.
            // Formed with unsigned arithmetic (R2/L4): negating the signed
            // difference is undefined behavior exactly at INT32_MIN, while the
            // unsigned deadline-minus-now distance is defined for every
            // reading and identical everywhere else. Eligibility is unchanged:
            // this branch is still exactly the negative half-range.
            return ControlTickDecision(false, 0U, 0U, 0U, nextDeadlineMs_ - nowMs,
                                       clockRewind);
        }

        const uint32_t lateByMs = static_cast<uint32_t>(sinceDeadlineMs);
        const uint32_t droppedTicks = lateByMs / periodMs_;
        return ControlTickDecision(true, dtMsFrom(nowMs), lateByMs, droppedTicks, 0U,
                                   clockRewind);
    }

    // Commits a control tick that has run, from the same clock reading the
    // decision was made with. This is the only place the schedule moves.
    //
    // The next deadline is one full period after this reading: the schedule
    // re-arms from the tick that actually ran, so a late tick is never followed
    // by an extra-short interval. The dropped deadlines are counted, not run.
    void advance(uint32_t nowMs) {
        if (!started_) {
            return;
        }
        const int32_t sinceDeadlineMs = static_cast<int32_t>(nowMs - nextDeadlineMs_);
        if (sinceDeadlineMs > 0) {
            const uint32_t lateByMs = static_cast<uint32_t>(sinceDeadlineMs);
            if (lateByMs > worstLateByMs_) {
                worstLateByMs_ = lateByMs;
            }
            droppedTotal_ += lateByMs / periodMs_;
        }
        // The gap between two *consecutive committed ticks* - the other extreme
        // P-H0W asks for, and the one that bounds how much near-zero dwell may be
        // credited without observation (`NearZeroVerifier`,
        // `kMaxDwellGapMs`). Excluded on the first tick, by definition: a
        // gap needs two control updates, and the interval before the first one is
        // already `dtMs`' own boot gap, which is a different fact and a much
        // larger number. Deliberately separate from `worstLateByMs_`: lateness is
        // measured against the deadline, a stall against the previous tick.
        if (hasLastTick_) {
            const uint32_t stallMs = static_cast<uint32_t>(nowMs - lastTickMs_);
            if (stallMs > worstStallMs_) {
                worstStallMs_ = stallMs;
            }
        }
        lastTickMs_ = nowMs;
        hasLastTick_ = true;
        ++ticks_;
        nextDeadlineMs_ = nowMs + periodMs_;
    }

    // Control ticks committed since `start()`.
    uint32_t ticks() const { return ticks_; }

    // Deadlines that went by without a tick, since `start()`. A steadily growing
    // value means the loop cannot keep the cadence and is evidence about loop
    // cost, not a fault.
    uint32_t droppedDeadlines() const { return droppedTotal_; }

    // The worst lateness any committed tick has reported since `start()`: the
    // maximum, not the value of the most recent tick. 0 until a late tick runs.
    // This is the number a soak test reports instead of a spot reading.
    uint32_t worstLateByMs() const { return worstLateByMs_; }

    // The longest interval observed between two consecutive committed control
    // ticks, since `start()`. The first tick is excluded (see `advance()`), so this
    // is a steady-state number and the boot gap cannot dominate it. 0 until two
    // ticks have run; equal to the period when nothing ever stalls.
    uint32_t worstStallMs() const { return worstStallMs_; }

private:
    // Measured interval since the previous completed tick, or since `start()`
    // before the first one.
    uint32_t dtMsFrom(uint32_t nowMs) const {
        return static_cast<uint32_t>(nowMs - lastTickMs_);
    }

    uint32_t periodMs_;
    bool started_ = false;
    bool hasLastTick_ = false;
    uint32_t lastTickMs_ = 0U;
    bool hasLastDecision_ = false;
    uint32_t lastDecisionMs_ = 0U;
    uint32_t nextDeadlineMs_ = 0U;
    uint32_t ticks_ = 0U;
    uint32_t droppedTotal_ = 0U;
    uint32_t worstLateByMs_ = 0U;
    uint32_t worstStallMs_ = 0U;
};

}  // namespace ev
