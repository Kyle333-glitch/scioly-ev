#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The order of one control tick - the frozen stage vocabulary and its guard
// ---------------------------------------------------------------------------
//
// Why the order is written down
//
//   One control tick is a dependency chain, not a set of independent jobs. The
//   odometry is only correct if the encoder was snapshotted first; faults are
//   only complete if every input was sampled; the run policy may only act on
//   this cycle's measurements; an effort may only be computed from this cycle's
//   target and measurement; and only the safety boundary may turn a request
//   into something a driver will accept. Written as a straight sequence in the
//   vehicle layer, that chain is readable but silently reorderable - nothing
//   fails if a stage is moved, dropped, or run before its inputs exist.
//
//   This module makes the order data. The vehicle layer's tick enters each
//   stage through the guard below, and a tick that does not present every stage
//   exactly once, in this order, is reported as violated rather than trusted.
//
// What the guard is for, and what it is not
//
// * It is the completeness fact behind one decision: whether the control loop's
//   pass may refresh the watchdog, and whether the cycle's drive is allowed to
//   reach the driver at all.
// * It is not a fault identity and it latches nothing. It reports; what the
//   vehicle layer does about a violation is its own decision.
// * It does not run, time or measure anything: it is pure bookkeeping with no
//   clock, so it is host-testable with no vehicle (SOFTWARE_ARCHITECTURE.md
//   section 3).
//
// The stages are an enum, so their order is the order of the enumerators below.
// The values are pinned by tests; reordering or dropping one is a deliberate
// change to this vocabulary, not a refactor.

enum class ControlTickStage : uint8_t {
    // 1. Snapshot the encoder: the raw tick count this cycle acts on. Read
    //    once, so every later stage of the cycle agrees on one position.
    EncoderSnapshot = 0,

    // 2. Update the odometry and the velocity estimate from that snapshot.
    //    Nothing derived may be computed from a tick count read later.
    OdometryAndVelocity = 1,

    // 3. Snapshot the other inputs: battery, current, driver nFAULT, trigger.
    //    Raw readings only; no unit conversion exists (SOFTWARE_ARCHITECTURE.md
    //    section 12).
    SensorSnapshot = 2,

    // 4. Evaluate and latch faults, from this cycle's sampled evidence.
    FaultEvaluation = 3,

    // 5. Update the run policy, which consumes this cycle's evidence and emits
    //    this cycle's motion request.
    RunStateMachine = 4,

    // 6. Consume the run's request: the shaped target that this cycle's effort
    //    will follow. The request is the run policy's `RunMotionIntention`,
    //    never the vocabulary-only `MotionRequest` type in Motion.h.
    RunMotionRequest = 5,

    // 7. Run the motion control: the velocity regulator, or the run's own
    //    deliberate stop mapping, and nothing else.
    MotionControl = 6,

    // 8. Present exactly one request to the safety boundary and take its one
    //    verdict: the only place a powered command can come into existence.
    SafetyAuthorization = 7,

    // 9. Write the authorized command to the motor driver - or, when the cycle
    //    is not trustworthy, the driver's de-energized state.
    MotorCommand = 8,

    // 10. Capture this cycle's telemetry snapshot and call the event hook.
    TelemetryCapture = 9,
};

// Where the frozen order ends. The guard refuses any stage value outside the
// vocabulary, so a corrupted or cast-in value cannot be recorded as a stage.
constexpr uint8_t kControlTickStageCount = 10U;

enum class ControlTickOrderError : uint8_t {
    // No violation.
    None = 0,
    // A stage (or the tick's completion) arrived with no tick begun.
    TickNotStarted,
    // The stage is not the next one in the frozen order - it was skipped,
    // repeated, or arrives after an earlier violation.
    StageOutOfOrder,
    // The tick was completed with stages still missing.
    TickIncomplete,
    // A new tick was begun while the previous one had not been completed.
    TickWasAbandoned,
};

// The verdict for one control tick. `error == None` exactly when `ok` is true,
// matching the other verdict types in the project.
struct ControlTickOrderVerdict {
    constexpr ControlTickOrderVerdict(bool ok_, ControlTickOrderError error_, ControlTickStage stage_)
        : ok(ok_), error(error_), stage(stage_) {}

    bool ok;
    ControlTickOrderError error;

    // The stage the violation names: the stage that arrived out of order, or the
    // first stage that was still missing when the tick was completed. The zero
    // stage when ok is true.
    ControlTickStage stage;
};

// Records one control tick's stages and reports the first thing that did not
// happen as the frozen order requires.
//
// Lifecycle, once per control tick:
//
//     guard.begin();
//     guard.enter(ControlTickStage::EncoderSnapshot);
//     ... every stage, in order ...
//     guard.finish();
//     if (!guard.tickInOrder()) { ... }
//
// A violated tick can never become clean: the first violation of a tick record
// is latched until that record is finished. That is deliberate - a tick whose
// stages arrived wrongly is not a tick whose results may be trusted, whatever a
// later stage reports.
//
// A tick that is begun while another is still unfinished is counted as
// abandoned, and the resuming tick's record starts dirty (`TickWasAbandoned`),
// so the abandonment cannot be silently forgotten. The stage named for that
// error is not meaningful and is reported as the first stage; `abandonedTicks()`
// is what carries the fact, and the resuming record is counted as violated when
// it finishes. The tick after that one starts clean, so a guard that once saw a
// structural bug can still report that later ticks are in order - the counters
// are the history, the per-tick verdict is the current cycle.
class ControlTickOrderGuard {
public:
    // Starts a tick record.
    void begin() {
        const bool previousTickUnfinished = inProgress_;
        inProgress_ = true;
        nextStageIndex_ = 0U;
        tickVerdict_ = ControlTickOrderVerdict(true, ControlTickOrderError::None,
                                              ControlTickStage::EncoderSnapshot);
        if (previousTickUnfinished) {
            // The unfinished tick is counted here, and the resuming tick's record
            // is dirtied so the abandonment is also visible in the verdict of
            // the tick that follows it. The abandoned tick itself is not counted
            // as violated: it never finished, so `violatedTicks()` stays a count
            // of finished records.
            ++abandonedTicks_;
            latch(ControlTickOrderError::TickWasAbandoned, ControlTickStage::EncoderSnapshot);
        }
    }

    // Records one stage. Any stage that is not the next one in the frozen order
    // is a violation, including a repeat of an earlier stage and a value outside
    // the vocabulary.
    void enter(ControlTickStage stage) {
        if (!inProgress_) {
            latch(ControlTickOrderError::TickNotStarted, stage);
            return;
        }
        if (static_cast<uint8_t>(stage) != nextStageIndex_) {
            latch(ControlTickOrderError::StageOutOfOrder, stage);
            return;
        }
        ++nextStageIndex_;
    }

    // Completes the tick. Missing stages are a violation naming the first one
    // that never arrived.
    void finish() {
        if (!inProgress_) {
            latch(ControlTickOrderError::TickNotStarted, ControlTickStage::EncoderSnapshot);
            return;
        }
        if (nextStageIndex_ < kControlTickStageCount) {
            latch(ControlTickOrderError::TickIncomplete, static_cast<ControlTickStage>(nextStageIndex_));
        }
        inProgress_ = false;
        if (tickVerdict_.ok) {
            ++completedTicks_;
        } else {
            ++violatedTicks_;
        }
    }

    // True while a tick has been begun and not yet finished.
    bool tickInProgress() const { return inProgress_; }

    // How many stages the tick in progress (or the tick just finished) has
    // presented. Equal to kControlTickStageCount for a complete tick.
    uint8_t stagesEntered() const { return nextStageIndex_; }

    // The verdict for the tick in progress or the one just finished.
    const ControlTickOrderVerdict& tickVerdict() const { return tickVerdict_; }

    // True when that tick presented every stage exactly once, in order.
    bool tickInOrder() const { return tickVerdict_.ok; }

    // Ticks since construction: completed in order, violated, and begun while a
    // previous tick was still unfinished. A non-zero `violatedTicks()` or
    // `abandonedTicks()` is a structural problem in the caller, not a measurement
    // of the vehicle.
    uint32_t completedTicks() const { return completedTicks_; }
    uint32_t violatedTicks() const { return violatedTicks_; }
    uint32_t abandonedTicks() const { return abandonedTicks_; }

private:
    // Latches the first violation of the tick. Later violations of the same tick
    // are not recorded: the first one is what the tick is reported as, and the
    // tick is already untrustworthy.
    void latch(ControlTickOrderError error, ControlTickStage stage) {
        if (!tickVerdict_.ok) {
            return;
        }
        tickVerdict_ = ControlTickOrderVerdict(false, error, stage);
    }

    ControlTickOrderVerdict tickVerdict_{true, ControlTickOrderError::None,
                                         ControlTickStage::EncoderSnapshot};
    uint8_t nextStageIndex_ = 0U;
    bool inProgress_ = false;
    uint32_t completedTicks_ = 0U;
    uint32_t violatedTicks_ = 0U;
    uint32_t abandonedTicks_ = 0U;
};

static_assert(static_cast<int>(ControlTickStage::EncoderSnapshot) == 0,
              "the first stage must stay the zero value");
static_assert(static_cast<int>(ControlTickStage::TelemetryCapture) ==
                  static_cast<int>(kControlTickStageCount) - 1,
              "the last stage must stay the last: the guard counts stages against it");
static_assert(static_cast<int>(ControlTickOrderError::None) == 0,
              "no violation must stay the zero value");

}  // namespace ev
