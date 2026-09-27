#include <cstdint>

#include <unity.h>

#include <TelemetryQueue.h>

// Contract tests for the bounded telemetry queue, the session and the transition
// detector (lib/evcore/src/TelemetryQueue.h, lib/evcore/src/TelemetryRecord.h).
//
// The property under test is the one the control loop depends on: every push is
// bounded work that either stores a record or counts a loss, and nothing here can
// make a caller wait, allocate, or read a clock. A pass therefore says the
// control loop can keep its cadence while a host is absent or slow - it says
// nothing about how many bytes a real serial port moved.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::TelemetrySample sampleAt(uint32_t tMs) {
    ev::TelemetrySample sample;
    sample.tMs = tMs;
    return sample;
}

ev::TelemetryEvent eventOf(ev::TelemetryEventCode code, uint32_t tMs) {
    ev::TelemetryEvent event;
    event.event = code;
    event.tMs = tMs;
    return event;
}

}  // namespace

// A lane is a FIFO: what comes out is in the order it went in, so a reader can
// treat consecutive records as consecutive moments.
void test_sample_lane_is_fifo(void) {
    ev::TelemetryQueue queue;
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_EQUAL_UINT16(0U, queue.sampleCount());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Queued),
                          static_cast<int>(queue.pushSample(sampleAt(10U))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Queued),
                          static_cast<int>(queue.pushSample(sampleAt(20U))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Queued),
                          static_cast<int>(queue.pushSample(sampleAt(30U))));
    TEST_ASSERT_EQUAL_UINT16(3U, queue.sampleCount());
    TEST_ASSERT_FALSE(queue.empty());

    ev::TelemetryRecord record;
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRecordKind::Sample),
                          static_cast<int>(record.kind));
    TEST_ASSERT_EQUAL_UINT32(10U, record.sample.tMs);
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_UINT32(20U, record.sample.tMs);
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_UINT32(30U, record.sample.tMs);

    TEST_ASSERT_FALSE(queue.popNext(record));
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_EQUAL_UINT32(0U, queue.droppedSamples());
}

// A full lane drops the arriving record and counts it. Dropping the newest rather
// than overwriting the oldest is what keeps the lane a FIFO, and the counter is
// what makes the loss visible in the records that survive.
void test_sample_lane_drops_the_newest_and_counts_it(void) {
    ev::TelemetryQueue queue;
    for (uint16_t index = 0U; index < ev::kTelemetrySampleLaneCapacity; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Queued),
                              static_cast<int>(queue.pushSample(sampleAt(index))));
    }

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::DroppedFull),
                          static_cast<int>(queue.pushSample(sampleAt(1000U))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::DroppedFull),
                          static_cast<int>(queue.pushSample(sampleAt(1001U))));
    TEST_ASSERT_EQUAL_UINT32(2U, queue.droppedSamples());
    TEST_ASSERT_EQUAL_UINT16(ev::kTelemetrySampleLaneCapacity, queue.sampleCount());

    // The oldest records are the ones that survived, in order.
    ev::TelemetryRecord record;
    for (uint16_t index = 0U; index < ev::kTelemetrySampleLaneCapacity; ++index) {
        TEST_ASSERT_TRUE(queue.popNext(record));
        TEST_ASSERT_EQUAL_UINT32(index, record.sample.tMs);
    }
    TEST_ASSERT_FALSE(queue.popNext(record));
}

// Events drain before samples. An event names a transition the samples after it
// cannot be read correctly without, so a lane that is one drain behind still
// delivers its events first.
void test_events_drain_before_samples(void) {
    ev::TelemetryQueue queue;
    queue.pushSample(sampleAt(1U));
    queue.pushEvent(eventOf(ev::TelemetryEventCode::Boot, 2U));
    queue.pushSample(sampleAt(3U));
    queue.pushEvent(eventOf(ev::TelemetryEventCode::Armed, 4U));

    ev::TelemetryRecord record;
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRecordKind::Event),
                          static_cast<int>(record.kind));
    TEST_ASSERT_EQUAL_UINT32(2U, record.event.tMs);

    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRecordKind::Event),
                          static_cast<int>(record.kind));
    TEST_ASSERT_EQUAL_UINT32(4U, record.event.tMs);

    // Within a lane the order is still the push order.
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_UINT32(1U, record.sample.tMs);
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_UINT32(3U, record.sample.tMs);
    TEST_ASSERT_FALSE(queue.popNext(record));
}

// The reason there are two lanes: a burst of samples - which is exactly what a
// stalled host produces - cannot evict an event.
void test_a_sample_burst_cannot_evict_an_event(void) {
    ev::TelemetryQueue queue;
    queue.pushEvent(eventOf(ev::TelemetryEventCode::FaultLatched, 5U));
    for (uint16_t index = 0U; index < ev::kTelemetrySampleLaneCapacity * 2U; ++index) {
        queue.pushSample(sampleAt(index));
    }

    TEST_ASSERT_EQUAL_UINT16(1U, queue.eventCount());
    TEST_ASSERT_EQUAL_UINT32(ev::kTelemetrySampleLaneCapacity, queue.droppedSamples());
    TEST_ASSERT_EQUAL_UINT32(0U, queue.droppedEvents());

    // The event is still the first thing out, and it is the event that was
    // pushed, not the newest sample.
    ev::TelemetryRecord record;
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryRecordKind::Event),
                          static_cast<int>(record.kind));
    TEST_ASSERT_EQUAL_UINT32(5U, record.event.tMs);
}

// The counters are stamped when a record is stored, so a record can never
// describe a loss that happened after it was stored, and a record stored after a
// loss carries it.
void test_records_carry_the_counters_as_of_their_push(void) {
    ev::TelemetryQueue queue;

    // One sample stored before any loss.
    queue.pushSample(sampleAt(0U));

    // Then overflow the sample lane by one record: the last push is dropped.
    for (uint16_t index = 1U; index <= ev::kTelemetrySampleLaneCapacity; ++index) {
        queue.pushSample(sampleAt(index));
    }
    TEST_ASSERT_EQUAL_UINT32(1U, queue.droppedSamples());

    // A record stored after the loss carries it.
    queue.pushEvent(eventOf(ev::TelemetryEventCode::CommandRejected, 99U));

    ev::TelemetryRecord record;
    TEST_ASSERT_TRUE(queue.popNext(record));  // events drain first
    TEST_ASSERT_EQUAL_UINT32(99U, record.event.tMs);
    TEST_ASSERT_EQUAL_UINT32(1U, record.event.drops.samples);
    TEST_ASSERT_EQUAL_UINT32(0U, record.event.drops.events);

    // A record stored before the loss still describes none.
    TEST_ASSERT_TRUE(queue.popNext(record));
    TEST_ASSERT_EQUAL_UINT32(0U, record.sample.tMs);
    TEST_ASSERT_EQUAL_UINT32(0U, record.sample.drops.samples);
    TEST_ASSERT_EQUAL_UINT32(0U, record.sample.drops.events);

    // The same for the event lane: overflow it by one, make room, store one more.
    for (uint16_t index = 0U; index <= ev::kTelemetryEventLaneCapacity; ++index) {
        queue.pushEvent(eventOf(ev::TelemetryEventCode::Boot, index));
    }
    TEST_ASSERT_EQUAL_UINT32(1U, queue.droppedEvents());
    TEST_ASSERT_TRUE(queue.popNext(record));  // makes room
    queue.pushEvent(eventOf(ev::TelemetryEventCode::ChainReset, 500U));

    // The last record out is the one stored after both losses, so it describes
    // both of them.
    ev::TelemetryRecord last;
    for (uint16_t index = 0U; index < ev::kTelemetryEventLaneCapacity; ++index) {
        TEST_ASSERT_TRUE(queue.popNext(record));
        last = record;
    }
    TEST_ASSERT_EQUAL_UINT32(500U, last.event.tMs);
    TEST_ASSERT_EQUAL_UINT32(1U, last.event.drops.samples);
    TEST_ASSERT_EQUAL_UINT32(1U, last.event.drops.events);
}

// Telemetry off means nothing is emitted and nothing is lost: a disabled stream
// must not accumulate a backlog that reappears, and an operator's own action is
// not a dropped record.
void test_disabled_session_stores_nothing_and_counts_nothing(void) {
    ev::TelemetrySession session;
    TEST_ASSERT_TRUE(session.enabled());

    session.pushSample(sampleAt(1U));
    session.disable();

    TEST_ASSERT_FALSE(session.enabled());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Disabled),
                          static_cast<int>(session.pushSample(sampleAt(2U))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Disabled),
                          static_cast<int>(session.pushEvent(eventOf(ev::TelemetryEventCode::Boot, 3U))));
    TEST_ASSERT_FALSE(session.hasPending());
    TEST_ASSERT_EQUAL_UINT16(0U, session.pendingSamples());
    TEST_ASSERT_EQUAL_UINT16(0U, session.pendingEvents());
    TEST_ASSERT_EQUAL_UINT32(0U, session.drops().samples);

    ev::TelemetryRecord record;
    TEST_ASSERT_FALSE(session.popNext(record));
    TEST_ASSERT_FALSE(session.sampleDue());
}

// Disabling discards what was queued, and enabling starts a clean stream: a
// record from before the boundary would be read as current.
void test_disable_clears_the_queue_and_the_counters(void) {
    ev::TelemetrySession session;
    for (uint16_t index = 0U; index <= ev::kTelemetrySampleLaneCapacity; ++index) {
        session.pushSample(sampleAt(index));
    }
    TEST_ASSERT_TRUE(session.drops().samples > 0U);
    TEST_ASSERT_TRUE(session.hasPending());

    session.disable();
    session.enable();

    TEST_ASSERT_EQUAL_UINT16(0U, session.pendingSamples());
    TEST_ASSERT_EQUAL_UINT16(0U, session.pendingEvents());
    TEST_ASSERT_EQUAL_UINT32(0U, session.drops().samples);
    TEST_ASSERT_EQUAL_UINT32(0U, session.drops().events);
    TEST_ASSERT_FALSE(session.hasPending());
}

// A stride of N samples one tick in N: a fixed cadence rather than a drifting
// average, and the first sample is due N ticks in, not immediately.
void test_stride_sampling_is_a_fixed_cadence(void) {
    ev::TelemetrySession session;
    TEST_ASSERT_EQUAL_UINT32(1U, session.strideTicks());
    TEST_ASSERT_TRUE(session.sampleDue());  // every tick by default

    session.setStrideTicks(5U);
    TEST_ASSERT_EQUAL_UINT32(5U, session.strideTicks());
    for (uint8_t tick = 1U; tick <= 9U; ++tick) {
        const bool due = session.sampleDue();
        TEST_ASSERT_EQUAL_INT(tick % 5U == 0U ? 1 : 0, due ? 1 : 0);
    }

    // A stride of zero would mean "never", which is what `disable()` is for, so
    // it is normalized to the finest rate there is.
    session.setStrideTicks(0U);
    TEST_ASSERT_EQUAL_UINT32(1U, session.strideTicks());
    TEST_ASSERT_TRUE(session.sampleDue());
    TEST_ASSERT_TRUE(session.sampleDue());
}

// The cadence restarts at the session's own boundaries rather than continuing
// from a counter that was left over.
void test_stride_restarts_after_a_rate_change_or_reenable(void) {
    ev::TelemetrySession session;
    session.setStrideTicks(4U);
    TEST_ASSERT_FALSE(session.sampleDue());
    TEST_ASSERT_FALSE(session.sampleDue());
    TEST_ASSERT_FALSE(session.sampleDue());
    TEST_ASSERT_TRUE(session.sampleDue());  // fourth tick

    // Changing the rate restarts the counter, so the new cadence is measured from
    // the change rather than from a stale partial count.
    session.setStrideTicks(2U);
    TEST_ASSERT_FALSE(session.sampleDue());
    TEST_ASSERT_TRUE(session.sampleDue());

    session.disable();
    session.enable();
    TEST_ASSERT_FALSE(session.sampleDue());
    TEST_ASSERT_TRUE(session.sampleDue());
}

// The first observation after construction (or a reset) establishes the baseline.
// Emitting the boot state as a "change" would be a transition that did not
// happen, and the reader of the log would have to guess which of the two records
// is the real boundary.
void test_detector_first_observation_is_silent(void) {
    ev::TelemetryEventDetector detector;
    ev::TelemetrySample sample;
    sample.tMs = 100U;
    sample.phase = ev::RunPhase::Armed;
    sample.armed = true;
    sample.fault = ev::FaultCode::ProvisionalCalibration;

    const ev::TelemetryEventBatch boot = detector.observe(sample);
    TEST_ASSERT_EQUAL_UINT8(0U, boot.count);

    // A reset re-primes, so the next observation is a baseline again.
    detector.reset();
    const ev::TelemetryEventBatch afterReset = detector.observe(sample);
    TEST_ASSERT_EQUAL_UINT8(0U, afterReset.count);

    // A change after the baseline is reported.
    sample.phase = ev::RunPhase::Approach;
    const ev::TelemetryEventBatch change = detector.observe(sample);
    TEST_ASSERT_EQUAL_UINT8(1U, change.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::PhaseChange),
                          static_cast<int>(change.events[0].event));
}

// One tick can reveal at most three transitions, and the order is fixed: the
// fault first, then arming, then the phase. A reader that stops at the first
// event still learns the most important fact of the tick.
void test_detector_order_is_fault_then_arming_then_phase(void) {
    ev::TelemetryEventDetector detector;
    ev::TelemetrySample baseline;
    baseline.tMs = 1000U;
    baseline.phase = ev::RunPhase::Idle;
    baseline.armed = false;
    detector.observe(baseline);

    ev::TelemetrySample sample;
    sample.tMs = 1020U;
    sample.phase = ev::RunPhase::Armed;
    sample.armed = true;
    sample.fault = ev::FaultCode::PhaseTimeout;
    const ev::TelemetryEventBatch batch = detector.observe(sample);

    TEST_ASSERT_EQUAL_UINT8(ev::kTelemetryEventBatchCapacity, batch.count);
    TEST_ASSERT_EQUAL_UINT8(3U, batch.count);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::FaultLatched),
                          static_cast<int>(batch.events[0].event));
    TEST_ASSERT_EQUAL_STRING("PhaseTimeout", batch.events[0].detail);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                          static_cast<int>(batch.events[0].fault));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::Armed),
                          static_cast<int>(batch.events[1].event));
    TEST_ASSERT_EQUAL_STRING("-", batch.events[1].detail);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::PhaseChange),
                          static_cast<int>(batch.events[2].event));
    TEST_ASSERT_EQUAL_STRING("Armed", batch.events[2].detail);

    // Every event of the batch describes the tick it came from.
    for (uint8_t index = 0U; index < batch.count; ++index) {
        TEST_ASSERT_EQUAL_UINT32(1020U, batch.events[index].tMs);
        TEST_ASSERT_TRUE(batch.events[index].armed);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::RunPhase::Armed),
                              static_cast<int>(batch.events[index].phase));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::FaultCode::PhaseTimeout),
                              static_cast<int>(batch.events[index].fault));
    }
}

// A steady state produces no events: an event is a transition, not a periodic
// restatement, which is what keeps the event lane free of noise.
void test_detector_reports_nothing_while_the_state_is_unchanged(void) {
    ev::TelemetryEventDetector detector;
    ev::TelemetrySample sample;
    sample.phase = ev::RunPhase::Approach;
    sample.armed = true;
    sample.fault = ev::FaultCode::None;
    detector.observe(sample);

    for (uint16_t tick = 0U; tick < 20U; ++tick) {
        sample.tMs += 20U;
        TEST_ASSERT_EQUAL_UINT8(0U, detector.observe(sample).count);
    }
}

// A fault clearing is not itself an event: faults latch, so the only way the
// fault of record becomes `None` again is the new-run boundary, which the
// operator layer reports as `ChainReset`. A disarm, on the other hand, is a
// transition of its own and is reported.
void test_detector_reports_disarm_but_not_a_cleared_fault(void) {
    ev::TelemetryEventDetector detector;
    ev::TelemetrySample sample;
    sample.tMs = 10U;
    sample.armed = true;
    sample.fault = ev::FaultCode::PhaseTimeout;
    detector.observe(sample);

    sample.tMs = 20U;
    sample.armed = false;
    sample.fault = ev::FaultCode::None;
    const ev::TelemetryEventBatch batch = detector.observe(sample);

    TEST_ASSERT_EQUAL_UINT8(1U, batch.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::Disarmed),
                          static_cast<int>(batch.events[0].event));
}

// The batch is handed to the caller by value and is bounded, which is what keeps
// a caller allocation-free. The drained records go into the event lane unchanged.
void test_a_batch_can_be_pushed_into_the_event_lane(void) {
    ev::TelemetryEventDetector detector;
    ev::TelemetrySession session;

    ev::TelemetrySample baseline;
    detector.observe(baseline);

    ev::TelemetrySample sample;
    sample.tMs = 500U;
    sample.phase = ev::RunPhase::Push;
    sample.armed = true;
    sample.fault = ev::FaultCode::ReversalNotVerified;
    const ev::TelemetryEventBatch batch = detector.observe(sample);
    TEST_ASSERT_EQUAL_UINT8(3U, batch.count);

    for (uint8_t index = 0U; index < batch.count; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryPushResult::Queued),
                              static_cast<int>(session.pushEvent(batch.events[index])));
    }
    TEST_ASSERT_EQUAL_UINT16(3U, session.pendingEvents());

    // Drain order is the batch order, so a reader sees the fault first.
    ev::TelemetryRecord record;
    TEST_ASSERT_TRUE(session.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::FaultLatched),
                          static_cast<int>(record.event.event));
    TEST_ASSERT_TRUE(session.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::Armed),
                          static_cast<int>(record.event.event));
    TEST_ASSERT_TRUE(session.popNext(record));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::TelemetryEventCode::PhaseChange),
                          static_cast<int>(record.event.event));
    TEST_ASSERT_FALSE(session.popNext(record));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sample_lane_is_fifo);
    RUN_TEST(test_sample_lane_drops_the_newest_and_counts_it);
    RUN_TEST(test_events_drain_before_samples);
    RUN_TEST(test_a_sample_burst_cannot_evict_an_event);
    RUN_TEST(test_records_carry_the_counters_as_of_their_push);
    RUN_TEST(test_disabled_session_stores_nothing_and_counts_nothing);
    RUN_TEST(test_disable_clears_the_queue_and_the_counters);
    RUN_TEST(test_stride_sampling_is_a_fixed_cadence);
    RUN_TEST(test_stride_restarts_after_a_rate_change_or_reenable);
    RUN_TEST(test_detector_first_observation_is_silent);
    RUN_TEST(test_detector_order_is_fault_then_arming_then_phase);
    RUN_TEST(test_detector_reports_nothing_while_the_state_is_unchanged);
    RUN_TEST(test_detector_reports_disarm_but_not_a_cleared_fault);
    RUN_TEST(test_a_batch_can_be_pushed_into_the_event_lane);
    return UNITY_END();
}
