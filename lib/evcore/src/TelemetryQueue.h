#pragma once

#include <cstdint>

#include "TelemetryRecord.h"

namespace ev {

// ---------------------------------------------------------------------------
// The bounded telemetry queue - drop, never block
// ---------------------------------------------------------------------------
//
// Telemetry is a *consumer* of the control loop, never a participant in it. The
// queue exists so that a host which is slow, stalled, absent or deliberately
// ignored cannot make the firmware wait: every push is bounded work, every push
// either succeeds or is counted as dropped, and nothing here blocks, retries,
// allocates, or reads a clock. A disconnected serial port is therefore, from the
// control loop's point of view, indistinguishable from a fast one - which is
// exactly the property `SOFTWARE_ARCHITECTURE.md` section 9 requires and which
// the "ignoring serial cannot alter run behavior" scenario rests on.
//
// **Two lanes, not one queue.** Samples arrive at the control cadence and events
// arrive rarely, and the two have opposite loss characteristics: losing a sample
// costs one row of a trace, while losing an event costs a transition that the
// reader cannot reconstruct. Sharing one ring would let a burst of samples -
// which is exactly what a stalled host produces - evict the events a
// post-mortem needs most. Separate lanes bound that: an event can only ever be
// dropped by another event.
//
// **Drop-newest, and count it.** When a lane is full the arriving record is
// dropped and the lane's counter is incremented. Dropping the newest rather than
// overwriting the oldest keeps the lane a FIFO with no rewriting: the records
// that come out are in exactly the order they went in, which is what makes the
// event ordering a property a test can pin. The counters travel inside every
// emitted record (`TelemetryDrops`), so a log states whether it lost anything
// instead of looking complete when it is not.

// The store-and-forward depth per lane, in records. Both are policy, not
// measurement: they are chosen so that one full lane is a few hundred bytes of
// RAM and a lane can absorb a missed drain of several loop passes.
constexpr uint16_t kTelemetrySampleLaneCapacity = 8U;
constexpr uint16_t kTelemetryEventLaneCapacity = 16U;

// The records themselves (`TelemetryRecordKind`, `TelemetryRecord`) are part of
// the schema and live in `TelemetryRecord.h`: the lanes store them, but the
// formatter renders them and neither owns their shape.

// What a push did.
enum class TelemetryPushResult : uint8_t {
    Queued = 0,   // stored; it will be emitted in the order it was pushed
    DroppedFull,  // the lane is full; the arriving record was dropped and counted
    Disabled,     // telemetry is off: nothing is stored, and nothing was lost
};

// The record lanes. One instance serves one stream; it is not reentrant and has
// no thread-safety ambitions, because only the main loop ever touches it.
class TelemetryQueue {
public:
    bool empty() const { return sampleCount_ == 0U && eventCount_ == 0U; }
    uint16_t sampleCount() const { return sampleCount_; }
    uint16_t eventCount() const { return eventCount_; }

    uint32_t droppedSamples() const { return droppedSamples_; }
    uint32_t droppedEvents() const { return droppedEvents_; }
    TelemetryDrops drops() const {
        TelemetryDrops counters;
        counters.samples = droppedSamples_;
        counters.events = droppedEvents_;
        return counters;
    }

    // Appends a sample, stamping it with the current drop counters. The counters
    // are stamped here and not by the caller so a record can never describe a
    // loss that happened after it was stored, or miss one that happened before.
    TelemetryPushResult pushSample(const TelemetrySample& sample) {
        if (sampleCount_ >= kTelemetrySampleLaneCapacity) {
            ++droppedSamples_;
            return TelemetryPushResult::DroppedFull;
        }
        TelemetrySample stored = sample;
        stored.drops = drops();
        samples_[laneIndex(sampleHead_, sampleCount_, kTelemetrySampleLaneCapacity)] = stored;
        ++sampleCount_;
        return TelemetryPushResult::Queued;
    }

    TelemetryPushResult pushEvent(const TelemetryEvent& event) {
        if (eventCount_ >= kTelemetryEventLaneCapacity) {
            ++droppedEvents_;
            return TelemetryPushResult::DroppedFull;
        }
        TelemetryEvent stored = event;
        stored.drops = drops();
        events_[laneIndex(eventHead_, eventCount_, kTelemetryEventLaneCapacity)] = stored;
        ++eventCount_;
        return TelemetryPushResult::Queued;
    }

    // Takes the next record out, oldest first, **events before samples**. The
    // priority is deliberate: an event describes a transition that the samples
    // after it cannot be read correctly without, so a lane that is one drain
    // behind delivers its events first. Within a lane the order is exactly the
    // push order.
    bool popNext(TelemetryRecord& out) {
        if (eventCount_ > 0U) {
            out.kind = TelemetryRecordKind::Event;
            out.event = events_[eventHead_];
            eventHead_ = static_cast<uint16_t>((eventHead_ + 1U) % kTelemetryEventLaneCapacity);
            --eventCount_;
            return true;
        }
        if (sampleCount_ > 0U) {
            out.kind = TelemetryRecordKind::Sample;
            out.sample = samples_[sampleHead_];
            sampleHead_ = static_cast<uint16_t>((sampleHead_ + 1U) % kTelemetrySampleLaneCapacity);
            --sampleCount_;
            return true;
        }
        return false;
    }

    bool hasEvent() const { return eventCount_ > 0U; }
    bool hasSample() const { return sampleCount_ > 0U; }

    // Empties both lanes and clears the counters. Used by the session's own
    // boundaries (enable/disable); a deliberate operator action is not a loss and
    // must not leave a drop count behind that reads like one.
    void clear() {
        sampleHead_ = 0U;
        sampleCount_ = 0U;
        eventHead_ = 0U;
        eventCount_ = 0U;
        droppedSamples_ = 0U;
        droppedEvents_ = 0U;
    }

private:
    static uint16_t laneIndex(uint16_t head, uint16_t count, uint16_t capacity) {
        return static_cast<uint16_t>((head + count) % capacity);
    }

    TelemetrySample samples_[kTelemetrySampleLaneCapacity]{};
    TelemetryEvent events_[kTelemetryEventLaneCapacity]{};
    uint16_t sampleHead_ = 0U;
    uint16_t sampleCount_ = 0U;
    uint16_t eventHead_ = 0U;
    uint16_t eventCount_ = 0U;
    uint32_t droppedSamples_ = 0U;
    uint32_t droppedEvents_ = 0U;
};

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------
//
// The telemetry stream's own state: enabled or not, and how often a sample is
// taken. Nothing here is run-control state - the run phase is an input to a
// record and never an input to a decision - which is why disabling telemetry
// cannot change how the vehicle behaves and why the control path never reads
// anything from this class.
//
// Enabled from the start, by decision: a bench session should produce a log
// without an operator having to remember a command, and the bounded queue means
// an absent host costs nothing but dropped records. `telemetry off` disables it
// for an operator who wants a quiet link (or a control-loop timing measurement
// without a serial producer attached).
class TelemetrySession {
public:
    // The default is on, sampling every control tick (one record per tick at the
    // control cadence).
    bool enabled() const { return enabled_; }
    uint32_t strideTicks() const { return strideTicks_; }

    void enable() {
        enabled_ = true;
        strideCounter_ = 0U;
    }

    // Disables the stream and empties both lanes. Records that were queued but
    // not yet sent are discarded rather than kept for a later drain: an operator
    // who turned logging off is not asking for a backlog to appear when it is
    // turned back on, and a stale record would be read as current.
    void disable() {
        enabled_ = false;
        strideCounter_ = 0U;
        queue_.clear();
    }

    // Sets the sample stride in control ticks. Zero is treated as one: "every
    // tick" is the finest rate there is, and a silent zero would mean "never",
    // which is what `disable()` is for.
    void setStrideTicks(uint32_t strideTicks) {
        strideTicks_ = strideTicks == 0U ? 1U : strideTicks;
        strideCounter_ = 0U;
    }

    // Called exactly once per control tick. True when this tick's sample is due
    // under the current stride: with a stride of N, one tick in N, so a fixed
    // cadence rather than a drifting average.
    bool sampleDue() {
        if (!enabled_) {
            return false;
        }
        if (strideTicks_ <= 1U) {
            return true;
        }
        ++strideCounter_;
        if (strideCounter_ >= strideTicks_) {
            strideCounter_ = 0U;
            return true;
        }
        return false;
    }

    TelemetryPushResult pushSample(const TelemetrySample& sample) {
        if (!enabled_) {
            return TelemetryPushResult::Disabled;
        }
        return queue_.pushSample(sample);
    }

    // Events are refused while disabled just like samples: "disabled" means
    // nothing is emitted, and a lane that kept filling would make the flag a
    // lie. Nothing is counted as dropped either - the operator's own action is
    // not a loss.
    TelemetryPushResult pushEvent(const TelemetryEvent& event) {
        if (!enabled_) {
            return TelemetryPushResult::Disabled;
        }
        return queue_.pushEvent(event);
    }

    bool popNext(TelemetryRecord& out) {
        if (!enabled_) {
            return false;
        }
        return queue_.popNext(out);
    }

    bool hasPending() const { return enabled_ && !queue_.empty(); }
    uint16_t pendingSamples() const { return queue_.sampleCount(); }
    uint16_t pendingEvents() const { return queue_.eventCount(); }
    TelemetryDrops drops() const { return queue_.drops(); }

    const TelemetryQueue& queue() const { return queue_; }

private:
    bool enabled_ = true;
    uint32_t strideTicks_ = 1U;
    uint32_t strideCounter_ = 0U;
    TelemetryQueue queue_{};
};

}  // namespace ev
