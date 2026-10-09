#pragma once

#include <cstddef>
#include <cstdint>

#include "BoundedText.h"

namespace ev {

// ---------------------------------------------------------------------------
// The serial line framer - staged lines with a bounded urgent priority
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The pure framing state machine under `src/SerialTransport.h`. It owns the
//   line buffers and decides what may be staged, what waits, what is refused and
//   what has to go out next; it performs no I/O. The transport adapter supplies a
//   port (two functions: how many bytes are acceptable right now, and a write of
//   at most that many bytes) and this module never touches a pin or a `Serial`.
//
// Why it exists: the pre-S15 defect (audit M2/B5)
//
//   The old transport held ONE buffer, shared by the line being formatted and the
//   line being sent. An urgent operator response could therefore overwrite a
//   telemetry line that had *already partially left the port*: the host saw the
//   telemetry prefix and the reply as one malformed record with no terminator
//   between them, and an earlier operator reply could be silently transformed
//   into a different one. Nothing in the old design could distinguish "the buffer
//   is mine to reuse" from "bytes from this buffer are already on the wire".
//
// What it does instead
//
//   * **Two slots.** One slot holds the line being transmitted (`active`); the
//     other is where a formatter composes the next line. `workingBuffer()` never
//     returns the active slot, so formatting a line cannot touch bytes that are
//     already leaving the port. A record that has begun to move finishes as the
//     record it was staged as, terminator included; it is never spliced.
//   * **One waiting slot, a bounded policy.** While a line is active, an urgent
//     response is staged in the waiting slot and goes out immediately after the
//     active line completes. A second urgent response replaces the line *waiting*
//     there - which by construction has emitted zero bytes, so replacing it
//     cannot corrupt framing - and the displaced line is counted
//     (`linesAbandoned()`). More than one waiting line never accumulates.
//   * **Telemetry never displaces anything.** A non-urgent line is refused while
//     a line is active or waiting (`telemetryLinesRefused()`), which is the
//     discipline the vehicle layer's drain already applies before it pops a
//     record out of its queue.
//   * **Every loss has a counter.** `linesSent()`, `linesStaged()`,
//     `linesAbandoned()` (waiting lines displaced before any byte moved) and
//     `telemetryLinesRefused()` are the truthful record of what a stalled host
//     cost; a partial write is never silently dropped, it is resumed on later
//     pumps.
//
// What it does not do
//
//   * It does not parse or interpret anything: it moves bytes.
//   * It does not decide what to drop or how often to sample; those are the
//     queue's and the session's decisions (`TelemetryQueue.h`).
//   * It does not block. `pump()` sends only what the port accepts right now and
//     returns; the caller retries on a later pass.
//   * It allocates nothing. Both slots are members, so the instance has static
//     storage duration at its owner.
//
// The terminator belongs to this module, not to the formatters
// (`TelemetryFormat.h`, `OperatorResponse.h` both document that), and it is
// written as two more gated bytes rather than as part of the line.
class LineFramer {
public:
    static constexpr uint16_t bufferBytes() { return kSerialLineBufferBytes; }

    // The number of slots; two is the whole of the bounded policy: one active,
    // one waiting.
    static constexpr uint8_t kSlotCount = 2U;
    static constexpr uint16_t kNoSlot = 0xFFFFU;

    // Returns to the post-construction framing state. Counters are cumulative
    // session evidence and deliberately survive: this is called once, from the
    // transport's `begin()`.
    void begin() {
        active_ = kNoSlot;
        waiting_ = kNoSlot;
        bodyLength_ = 0U;
        sent_ = 0U;
        terminatorSent_ = 0U;
        waitingLength_ = 0U;
    }

    // The buffer a caller formats one line into. It is never the slot being
    // transmitted, so it is always safe to write. If the slot still holds a
    // staged-but-not-yet-transmitting line, that line is retired here: the
    // caller is about to overwrite it, and a line that never emitted a byte can
    // be replaced without corrupting anything - so the replacement is allowed,
    // bounded to one line, and counted as an abandonment rather than left
    // silent.
    char* workingBuffer() {
        formatSlot_ = (active_ == 0U) ? 1U : 0U;
        if (waiting_ == formatSlot_) {
            waiting_ = kNoSlot;
            waitingLength_ = 0U;
            ++linesAbandoned_;
        }
        return slots_[formatSlot_];
    }

    // Publishes the line currently in `workingBuffer()`. `length` excludes the
    // terminator, which this module adds.
    //
    //   * Idle: the line becomes the active one and `pump()` can send it.
    //   * Busy and `urgent`: the line waits in the other slot; it replaces any
    //     line waiting there (already retired and counted by `workingBuffer()`).
    //     An urgent line always starts moving with its own first byte, after the
    //     active line's terminator.
    //   * Busy and not urgent: refused and counted; the caller decides what the
    //     loss means.
    //
    // A length beyond `kSerialLineCapacity` is refused and counted separately
    // (`oversizeLinesRefused()`): a formatter that overflowed its own buffer
    // must not be able to move bytes this module did not store.
    bool stage(uint16_t length, bool urgent) {
        if (length > kSerialLineCapacity) {
            ++oversizeLinesRefused_;
            return false;
        }
        if (active_ == kNoSlot && waiting_ == kNoSlot) {
            active_ = formatSlot_;
            bodyLength_ = length;
            sent_ = 0U;
            terminatorSent_ = 0U;
            ++linesStaged_;
            return true;
        }
        if (!urgent) {
            ++telemetryLinesRefused_;
            return false;
        }
        waiting_ = formatSlot_;
        waitingLength_ = length;
        ++linesStaged_;
        return true;
    }

    // True while any line is staged, whether or not it has begun to move.
    // Callers use this to refuse to pop another record: a record popped while
    // this is true would have nowhere to go.
    bool busy() const { return active_ != kNoSlot || waiting_ != kNoSlot; }

    // Bytes not yet written to the port, both slots' terminators included. Zero
    // when idle.
    uint16_t pendingBytes() const {
        uint16_t pending = 0U;
        if (active_ != kNoSlot) {
            pending = static_cast<uint16_t>(bodyLength_ - sent_ + (2U - terminatorSent_));
        }
        if (waiting_ != kNoSlot) {
            pending = static_cast<uint16_t>(pending + waitingLength_ + 2U);
        }
        return pending;
    }

    // Sends what the port accepts right now. Returns true when nothing is staged
    // any more.
    //
    // `Port` is any type with:
    //
    //   * `int availableForWrite()` - how many bytes the port will accept right
    //     now; zero means "try again later" and is not an error;
    //   * `size_t write(const uint8_t*, size_t)` - writes at most the requested
    //     count and returns how many bytes it actually accepted.
    //
    // The loop is bounded by the staged lines (at most two) and by the port's own
    // reports: every iteration either returns on a full/short port or completes a
    // line. A short write resumes from `sent_` on the next call; a zero write
    // leaves the line staged for the next pass. Nothing here may block, and
    // nothing here assumes `write()` accepts everything it was offered.
    template <typename Port>
    bool pump(Port& port) {
        for (;;) {
            if (active_ == kNoSlot) {
                if (waiting_ == kNoSlot) {
                    return true;
                }
                // The waiting line starts moving only now, after the previous
                // line's terminator has fully left. This promotion is why an
                // urgent response can never join a partially emitted record.
                active_ = waiting_;
                bodyLength_ = waitingLength_;
                waiting_ = kNoSlot;
                waitingLength_ = 0U;
                sent_ = 0U;
                terminatorSent_ = 0U;
            }

            while (sent_ < bodyLength_) {
                const int room = port.availableForWrite();
                if (room <= 0) {
                    return false;
                }
                uint16_t chunk = static_cast<uint16_t>(bodyLength_ - sent_);
                if (chunk > static_cast<uint16_t>(room)) {
                    chunk = static_cast<uint16_t>(room);
                }
                const size_t written = port.write(
                    reinterpret_cast<const uint8_t*>(slots_[active_]) + sent_, chunk);
                if (written == 0U) {
                    return false;
                }
                sent_ = static_cast<uint16_t>(sent_ + written);
            }

            static const char kTerminator[] = "\r\n";
            while (terminatorSent_ < 2U) {
                if (port.availableForWrite() <= 0) {
                    return false;
                }
                const size_t written = port.write(
                    reinterpret_cast<const uint8_t*>(&kTerminator[terminatorSent_]), 1U);
                if (written == 0U) {
                    return false;
                }
                ++terminatorSent_;
            }

            active_ = kNoSlot;
            ++linesSent_;
            // Loop: a waiting line, if any, is promoted and offered in the same
            // call. At most one can be waiting, so this is bounded.
        }
    }

    // Lines whose terminator has fully left the port, lines staged, and the ways
    // a line can fail to go out: displaced from the waiting slot before any byte
    // moved, refused because a line was already in flight, or refused for being
    // wider than the buffer. They are evidence, not errors: they are what a
    // session that outran its host looks like from the firmware's side.
    uint32_t linesSent() const { return linesSent_; }
    uint32_t linesStaged() const { return linesStaged_; }
    uint32_t linesAbandoned() const { return linesAbandoned_; }
    uint32_t telemetryLinesRefused() const { return telemetryLinesRefused_; }
    uint32_t oversizeLinesRefused() const { return oversizeLinesRefused_; }

private:
    char slots_[kSlotCount][kSerialLineBufferBytes];
    uint16_t active_ = kNoSlot;
    uint16_t waiting_ = kNoSlot;
    uint16_t formatSlot_ = 0U;
    uint16_t bodyLength_ = 0U;
    uint16_t sent_ = 0U;
    uint16_t waitingLength_ = 0U;
    uint8_t terminatorSent_ = 0U;

    uint32_t linesSent_ = 0U;
    uint32_t linesStaged_ = 0U;
    uint32_t linesAbandoned_ = 0U;
    uint32_t telemetryLinesRefused_ = 0U;
    uint32_t oversizeLinesRefused_ = 0U;
};

}  // namespace ev
