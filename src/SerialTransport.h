#pragma once

#include <Arduino.h>

#include <cstdint>

#include <BoundedText.h>

// ---------------------------------------------------------------------------
// The serial transport - staged lines, and nothing that blocks
// ---------------------------------------------------------------------------
//
// Why this module exists at all, rather than `Serial.print` at each call site
//
//   On this board `Serial` is USB-CDC, and its `write()` **waits** for the host
//   to make room: the core's implementation loops until the endpoint's buffer has
//   accepted every byte (`framework-arduinorenesas-uno`, `usb/SerialUSB.cpp`).
//   A single `Serial.print()` of a telemetry record therefore hands the control
//   loop's schedule to whatever is reading - or not reading - the other end of
//   the cable. That is exactly the coupling that `AGENTS.md` section 13 and
//   `SOFTWARE_ARCHITECTURE.md` section 9 forbid, so this firmware never calls a
//   bare write.
//
// What it does instead
//
//   * **A line is staged, then pumped.** A caller formats one complete line into
//     the transport's own buffer (`workingBuffer()`) and stages it. `pump()`
//     sends only as many bytes as the port can accept right now, and the rest go
//     out on later passes. No call can wait.
//   * **The buffer is the staging area.** One copy of `kSerialLineBufferBytes`
//     holds the line being formatted and the line being sent, because they are
//     the same line: nothing is copied twice, and the firmware's serial RAM cost
//     is one buffer.
//   * **The terminator belongs to the transport**, not to the formatters
//     (`TelemetryFormat.h`, `OperatorResponse.h` both document that), and it is
//     sent as two more gated bytes rather than as part of the line.
//   * **Two priorities, deliberately unequal.** An operator response is urgent:
//     staging one *replaces* a staged telemetry line, because a human who typed a
//     command is reading the answer and a dropped log row is a lost diagnostic,
//     not a lost command. A telemetry line is not urgent, and the drain refuses
//     to stage one while the port is busy at all (see the vehicle layer), so a
//     record is never popped out of the queue only to be thrown away here.
//
// What it does not do
//
//   * It does not parse, format or interpret anything: it moves bytes.
//   * It does not decide what to drop or how often to sample. Those are the
//     queue's and the session's decisions (`TelemetryQueue.h`).
//   * It does not touch the watchdog, the control tick or any vehicle state. It
//     is low-priority work by construction, and the loop calls it after the
//     checkpoint.
//
// The one piece of knowledge about the board that this module encodes is the
// *size* of the endpoint buffer: 256 bytes on this variant
// (`variants/NANOR4/tusb_config.h`, `CFG_TUD_CDC_TX_BUFSIZE`). Lines here are
// longer than that, which is precisely why `pump()` is allowed to stop in the
// middle of a line.

namespace evhw {
namespace serialTransport {

// How many received bytes one pass will consume. A full command line is 96
// characters, so one pass can read a whole line plus a terminator; a host that
// sends more waits for the next pass rather than holding the loop.
constexpr uint8_t kReadBytesPerPass = 96U;

class LineTransport {
public:
    // Opens the port and forgets any staged line. On this board the baud rate is
    // accepted and ignored (USB-CDC); it is passed so the call site reads the way
    // every Arduino sketch does.
    void begin(unsigned long baud) {
        Serial.begin(baud);
        staged_ = false;
        bodyLength_ = 0U;
        sent_ = 0U;
        terminatorSent_ = 0U;
    }

    // The buffer a caller formats one line into. Capacity is
    // `ev::kSerialLineBufferBytes` bytes; the longest line any formatter produces
    // is `ev::kSerialLineCapacity` characters plus its terminator.
    char* workingBuffer() { return staging_; }
    static constexpr uint16_t bufferBytes() { return ev::kSerialLineBufferBytes; }

    // Publishes the line currently in `workingBuffer()`. `length` excludes the
    // terminator, which the transport adds.
    //
    // `urgent` is the priority described above: a response replaces a staged
    // telemetry line (and the abandoned line is counted), while a telemetry line
    // is refused rather than allowed to replace anything. A refusal returns false
    // and the caller decides what the loss means.
    bool stage(uint16_t length, bool urgent) {
        if (staged_) {
            if (!urgent) {
                ++telemetryLinesRefused_;
                return false;
            }
            ++linesAbandoned_;
        }
        bodyLength_ = length;
        sent_ = 0U;
        terminatorSent_ = 0U;
        staged_ = true;
        ++linesStaged_;
        return true;
    }

    // True while a line is staged, whether or not it has begun to move.
    bool busy() const { return staged_; }

    // Bytes of the staged line that have not left the port yet, terminator
    // included. Zero when idle.
    uint16_t pendingBytes() const {
        if (!staged_) {
            return 0U;
        }
        return static_cast<uint16_t>(bodyLength_ - sent_ + (2U - terminatorSent_));
    }

    // Sends what fits right now. Returns true when nothing is staged any more.
    //
    // Every write is gated on the port's own free-space report, so no call can
    // wait for the host; the loop between the gates is bounded by the staged line
    // and the endpoint size. A port that is not connected returns zero from
    // `write()` and the line simply stays staged.
    bool pump() {
        if (!staged_) {
            return true;
        }

        while (sent_ < bodyLength_) {
            const int room = Serial.availableForWrite();
            if (room <= 0) {
                return false;
            }
            uint16_t chunk = static_cast<uint16_t>(bodyLength_ - sent_);
            if (chunk > static_cast<uint16_t>(room)) {
                chunk = static_cast<uint16_t>(room);
            }
            const size_t written =
                Serial.write(reinterpret_cast<const uint8_t*>(staging_ + sent_), chunk);
            if (written == 0U) {
                return false;
            }
            sent_ = static_cast<uint16_t>(sent_ + written);
        }

        static const char kTerminator[] = "\r\n";
        while (terminatorSent_ < 2U) {
            if (Serial.availableForWrite() <= 0) {
                return false;
            }
            const size_t written = Serial.write(
                reinterpret_cast<const uint8_t*>(&kTerminator[terminatorSent_]), 1U);
            if (written == 0U) {
                return false;
            }
            ++terminatorSent_;
        }

        staged_ = false;
        ++linesSent_;
        return true;
    }

    // One received byte, or -1 when nothing is waiting. Non-blocking by
    // definition: it is `Serial.read()`.
    int16_t readByte() {
        const int value = Serial.read();
        return value < 0 ? static_cast<int16_t>(-1) : static_cast<int16_t>(value);
    }

    // Lines completed (terminator sent), lines staged, and the two ways a line
    // can fail to go out: abandoned by a response, or refused because a line was
    // already in flight. The last two are evidence, not errors: they are what a
    // session that outran its host looks like from the firmware's side.
    uint32_t linesSent() const { return linesSent_; }
    uint32_t linesStaged() const { return linesStaged_; }
    uint32_t linesAbandoned() const { return linesAbandoned_; }
    uint32_t telemetryLinesRefused() const { return telemetryLinesRefused_; }

private:
    char staging_[ev::kSerialLineBufferBytes];
    uint16_t bodyLength_ = 0U;
    uint16_t sent_ = 0U;
    uint8_t terminatorSent_ = 0U;
    bool staged_ = false;

    uint32_t linesSent_ = 0U;
    uint32_t linesStaged_ = 0U;
    uint32_t linesAbandoned_ = 0U;
    uint32_t telemetryLinesRefused_ = 0U;
};

// The host's control-line state: DTR and RTS, the two signals a USB CDC host
// asserts when it opens the port. Reading them is the only presence question the
// firmware can ask of a host.
//
// `dtr` is the core's own `connected()`, the predicate `Serial.write()` gates on
// (`framework-arduinorenesas-uno`, `usb/SerialUSB.cpp`), so it means exactly "a
// staged line can move right now" - the sense `pump()` needs. Why the firmware
// needs to ask at all: *staged* is not *delivered*. A line staged while nothing
// is reading sits in this transport indefinitely, and the port's own free-space
// report does not reveal that - `availableForWrite()` describes the USB FIFO,
// which has room whether or not a host exists. On the first hardware session the
// boot line was staged into an unattended port and the capture that attached
// later never saw it (finding F-4), so the vehicle layer asks before it commits a
// line that must not be lost.
//
// `rts` is the second half of the same line state, read through `Serial.rts()`,
// which is a plain read of the value `tud_cdc_line_state_cb` stored - it has no
// side effect. `Serial.dtr()` is deliberately *not* used: reading it flips the
// core's `ignore_dtr`, after which `connected()` - and therefore `write()` -
// succeeds with no host attached, which is exactly the silent loss this exists to
// avoid.
//
// Two bits rather than one, because a host can present a poor attach edge
// (finding F-4): DTR is all the firmware can see of "a reader exists", and on
// Windows a host that closes a port and reopens it for another reader process
// does not always deassert it, so a line-state reading can be unchanged across a
// genuinely new reader. An edge in *either* bit is therefore treated as an
// attach; what the vehicle layer does with it, and why even that cannot be a
// guarantee, is in `VehicleRuntime::serviceStagedText()` and
// `data/evidence/P-H0-2026-09-28/d22-f4-attach-observations.txt`.
//
// `operator bool()` runs the USB task and, the first time it reports a connected
// host, waits 10 ms (`SerialUSB.cpp`, its `first_call` guard). With the bounded
// wait in `setup()` that normally happens during initialization, and a host that
// attaches much later costs one 10 ms pass - which is why the vehicle layer
// samples this at a bounded rate rather than on every pass (`kHostPollPasses`).
struct HostLineState {
    bool dtr = false;
    bool rts = false;
};

inline HostLineState hostLineState() {
    HostLineState state;
    // DTR first: `operator bool()` runs the USB task, so a control-line change
    // still queued is processed before the RTS bit below is read.
    state.dtr = static_cast<bool>(Serial);
    state.rts = Serial.rts();
    return state;
}

// The one instance. It has static storage because the main loop is its only
// caller and the buffer is too large for a stack frame, and because the ISR has
// nothing to do with it either way.
inline LineTransport& port() {
    static LineTransport instance;
    return instance;
}

// Opens the port. Called once from `setup()`, before anything may want to write.
inline void begin(unsigned long baud) { port().begin(baud); }

}  // namespace serialTransport
}  // namespace evhw
