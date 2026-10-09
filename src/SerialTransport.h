#pragma once

#include <Arduino.h>

#include <cstdint>

#include <BoundedText.h>
#include <SerialLineFraming.h>

// ---------------------------------------------------------------------------
// The serial transport - the adapter over the pure line framer
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
//   * **The framing is a pure module.** `ev::LineFramer`
//     (`lib/evcore/src/SerialLineFraming.h`) owns the two line slots and the
//     bounded urgent policy; this adapter supplies the port and nothing else. The
//     split exists because the M2/B5 defect lived in the framing decision - what
//     may replace what, and what must finish as the record it was staged as - and
//     a decision that lives in a pure module is one the host suite can pin.
//   * **A line is staged, then pumped.** A caller formats one complete line into
//     the framer's working buffer and stages it. `pump()` sends only as many
//     bytes as the port can accept right now, and the rest go out on later
//     passes. No call can wait.
//   * **Two slots, one active and one waiting.** The formatter composes the next
//     line in the slot that is *not* being transmitted, so an urgent response can
//     never overwrite a partially emitted telemetry line. An urgent response
//     staged while a line is in flight waits in the second slot and goes out
//     after the active line's terminator; a second urgent response replaces the
//     *waiting* line (which has emitted nothing) and the displaced line is
//     counted. The full policy and its counters are documented on
//     `ev::LineFramer`.
//   * **The terminator belongs to the framer**, not to the formatters
//     (`TelemetryFormat.h`, `OperatorResponse.h` both document that), and it is
//     sent as two more gated bytes rather than as part of the line.
//   * **Every write is gated on the port's own free-space report**, and a short
//     or zero write resumes from where it stopped: `Serial.write` is never
//     assumed to accept everything it was offered.
//
// The one piece of knowledge about the board that this module still encodes is
// the *size* of the endpoint buffer: 256 bytes on this variant
// (`variants/NANOR4/tusb_config.h`, `CFG_TUD_CDC_TX_BUFSIZE`). Lines here are
// longer than that, which is precisely why `pump()` is allowed to stop in the
// middle of a line.
//
// Memory: the framer holds two `ev::kSerialLineBufferBytes` slots, because one
// line in flight plus one waiting line is the whole bounded urgent policy. That
// is a deliberate, static cost (+577 bytes of `.bss` on this variant, from 577
// to 1154), not dynamic allocation: the former single buffer *was* the defect.

namespace evhw {
namespace serialTransport {

// How many received bytes one pass will consume. A full command line is 96
// characters, so one pass can read a whole line plus a terminator; a host that
// sends more waits for the next pass rather than holding the loop.
constexpr uint8_t kReadBytesPerPass = 96U;

// The board's own write port, in the shape `ev::LineFramer::pump()` expects. It
// is the only place in the firmware that calls `Serial.write`, so the
// "never assume a write accepted everything" rule has one home.
struct SerialWritePort {
    int availableForWrite() { return Serial.availableForWrite(); }

    size_t write(const uint8_t* bytes, size_t count) { return Serial.write(bytes, count); }
};

class LineTransport {
public:
    // Opens the port and forgets any staged line. On this board the baud rate is
    // accepted and ignored (USB-CDC); it is passed so the call site reads the way
    // every Arduino sketch does.
    void begin(unsigned long baud) {
        Serial.begin(baud);
        framer_.begin();
    }

    // The buffer a caller formats one line into. Capacity is
    // `ev::kSerialLineBufferBytes` bytes; the longest line any formatter produces
    // is `ev::kSerialLineCapacity` characters plus its terminator.
    char* workingBuffer() { return framer_.workingBuffer(); }
    static constexpr uint16_t bufferBytes() { return ev::LineFramer::bufferBytes(); }

    // Publishes the line currently in `workingBuffer()`. `length` excludes the
    // terminator, which the framer adds.
    //
    // `urgent` is the priority described above: a response staged while a line is
    // in flight waits for it (and replaces only a line that is itself still
    // waiting), while a telemetry line is refused rather than allowed to displace
    // anything. A refusal returns false and the caller decides what the loss
    // means.
    bool stage(uint16_t length, bool urgent) { return framer_.stage(length, urgent); }

    // True while a line is staged, whether or not it has begun to move.
    bool busy() const { return framer_.busy(); }

    // Bytes of the staged lines that have not left the port yet, terminators
    // included. Zero when idle.
    uint16_t pendingBytes() const { return framer_.pendingBytes(); }

    // Sends what fits right now. Returns true when nothing is staged any more.
    // Every write is gated on the port's own free-space report, so no call can
    // wait for the host; a port that is not connected returns zero from `write()`
    // and the line simply stays staged.
    bool pump() { return framer_.pump(port_); }

    // One received byte, or -1 when nothing is waiting. Non-blocking by
    // definition: it is `Serial.read()`.
    int16_t readByte() {
        const int value = Serial.read();
        return value < 0 ? static_cast<int16_t>(-1) : static_cast<int16_t>(value);
    }

    // Lines completed (terminator sent), lines staged, and the ways a line can
    // fail to go out: displaced from the waiting slot before a byte moved,
    // refused because a line was already in flight, or refused for being wider
    // than the buffer. They are evidence, not errors: they are what a session
    // that outran its host looks like from the firmware's side.
    uint32_t linesSent() const { return framer_.linesSent(); }
    uint32_t linesStaged() const { return framer_.linesStaged(); }
    uint32_t linesAbandoned() const { return framer_.linesAbandoned(); }
    uint32_t telemetryLinesRefused() const { return framer_.telemetryLinesRefused(); }
    uint32_t oversizeLinesRefused() const { return framer_.oversizeLinesRefused(); }

private:
    ev::LineFramer framer_{};
    SerialWritePort port_{};
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
// caller and the buffers are too large for a stack frame, and because the ISR has
// nothing to do with it either way.
inline LineTransport& port() {
    static LineTransport instance;
    return instance;
}

// Opens the port. Called once from `setup()`, before anything may want to write.
inline void begin(unsigned long baud) { port().begin(baud); }

}  // namespace serialTransport
}  // namespace evhw
