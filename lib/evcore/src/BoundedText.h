#pragma once

#include <cmath>
#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// Bounded text composition - one writer for both operator text paths
// ---------------------------------------------------------------------------
//
// Every line this firmware sends to a serial port is composed through this
// class: the operator responses and the telemetry records. Two properties are
// the whole reason it exists.
//
// * **Bounded by construction.** The caller supplies a fixed buffer and its
//   usable capacity; the writer never writes past it, always leaves the content
//   NUL-terminated, and reports `truncated()` when something did not fit. There
//   is no allocation, no locale, no stream and no `printf`.
// * **Integer-only number formatting.** `%f` support depends on the linker's
//   floating-point `printf` selection, which this project has not chosen, and a
//   formatting failure would be silent in a log. `appendFixed()` therefore
//   converts a double to a fixed number of decimals with integer arithmetic
//   around one `floor()` call, and writes digits itself. The same code produces
//   the same bytes on the host and on the ARM target, which is what makes the
//   formatting host-testable at all.
//
// The number vocabulary is deliberately small, because a bounded writer's
// guarantee is only as good as its worst-case field: see `appendFixed()` for
// the one sentinel each out-of-range case produces.
//
// This class is not a schema and holds no state beyond the buffer it writes
// into. It decides no field order and no field name; both live with the caller
// that owns the record.
class BoundedText {
public:
    // `capacity` is the number of characters this writer may produce, excluding
    // the terminator, so the caller's buffer must hold `capacity + 1` bytes.
    // A capacity of zero makes every write a no-op that reports truncation.
    BoundedText(char* buffer, uint16_t capacity)
        : buffer_(buffer), capacity_(capacity), length_(0U), truncated_(false) {
        if (buffer_ != nullptr && capacity_ > 0U) {
            buffer_[0] = '\0';
        }
    }

    // Characters written so far, excluding the terminator.
    uint16_t length() const { return length_; }

    // True when at least one write did not fit. The content is a valid,
    // NUL-terminated prefix in that case, never a partial number: a write either
    // fits whole or contributes nothing.
    bool truncated() const { return truncated_; }

    // Room left, in characters. Never negative.
    uint16_t remaining() const { return static_cast<uint16_t>(capacity_ - length_); }

    // The buffer as written, NUL-terminated. Null only when the writer was
    // constructed with a null buffer.
    const char* c_str() const { return buffer_; }

    BoundedText& appendChar(char value) {
        if (length_ < capacity_) {
            buffer_[length_] = value;
            ++length_;
            buffer_[length_] = '\0';
        } else {
            truncated_ = true;
        }
        return *this;
    }

    // Appends a NUL-terminated string. A null pointer appends nothing and is not
    // a truncation: it means the caller had no text, not that text was lost.
    BoundedText& appendText(const char* text) {
        if (text == nullptr) {
            return *this;
        }
        for (const char* cursor = text; *cursor != '\0'; ++cursor) {
            appendChar(*cursor);
        }
        return *this;
    }

    // Appends `,<key>=`, or `<key>=` when this is the first thing written. Call
    // it once per field, then append that field's value: the separators and the
    // key/value punctuation then exist in exactly one place per schema rather
    // than once per field.
    BoundedText& field(const char* key) {
        if (length_ > 0U) {
            appendChar(',');
        }
        return appendText(key).appendChar('=');
    }

    // Unsigned decimal, no padding.
    BoundedText& appendUnsigned(uint32_t value) {
        char digits[10];
        uint8_t count = 0U;
        // At least one digit, so zero prints as "0".
        do {
            digits[count] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
            ++count;
        } while (value != 0U);
        while (count > 0U) {
            --count;
            appendChar(digits[count]);
        }
        return *this;
    }

    // A boolean as the schema's one-character form.
    BoundedText& appendBool(bool value) { return appendChar(value ? '1' : '0'); }

    // Fixed-point decimal with `decimals` places (0..4; more is clamped to 4).
    // The value is written as sign, integer part, '.', then zero-padded
    // fraction, so the byte length of a finite in-range value is bounded by
    // 1 + 7 + 1 + 4 = 13 characters.
    //
    // Out-of-range and non-finite inputs produce a fixed sentinel instead of a
    // number, so a bad value can never widen a record: `nan`, `inf`, `-inf`, and
    // `ovf` for a finite magnitude at or above 1e7 (10 000 km, 10 000 000 m/s -
    // no physical quantity here is remotely that large, so the sentinel means a
    // bug or uninitialized data rather than a measurement).
    //
    // A negative value that rounds to zero prints as `0.0000`, never as `-0`.
    BoundedText& appendFixed(double value, uint8_t decimals) {
        if (decimals > 4U) {
            decimals = 4U;
        }
        if (!std::isfinite(value)) {
            if (value != value) {
                return appendText("nan");
            }
            return appendText(value < 0.0 ? "-inf" : "inf");
        }
        if (std::fabs(value) >= 1.0e7) {
            return appendText("ovf");
        }

        // 10^decimals as an integer, without a floating-point power call.
        uint32_t scale = 1U;
        for (uint8_t index = 0U; index < decimals; ++index) {
            scale *= 10U;
        }

        // One floor() converts to a scaled integer; the remaining arithmetic is
        // integer, so the digits below are exact.
        const double scaledValue = std::floor(value * static_cast<double>(scale) + 0.5);
        int64_t scaled = static_cast<int64_t>(scaledValue);
        if (scaled < 0) {
            appendChar('-');
            scaled = -scaled;
        }

        const uint32_t whole = static_cast<uint32_t>(scaled / static_cast<int64_t>(scale));
        const uint32_t fraction = static_cast<uint32_t>(scaled % static_cast<int64_t>(scale));
        appendUnsigned(whole);
        if (decimals == 0U) {
            return *this;
        }

        appendChar('.');
        // Zero-pad the fraction to exactly `decimals` places.
        uint32_t divisor = scale / 10U;
        while (divisor > 0U) {
            appendChar(static_cast<char>('0' + (fraction / divisor) % 10U));
            divisor /= 10U;
        }
        return *this;
    }

private:
    char* buffer_;
    uint16_t capacity_;
    uint16_t length_;
    bool truncated_;
};

// ---------------------------------------------------------------------------
// The two facts the serial consumers must agree on
// ---------------------------------------------------------------------------
//
// The telemetry formatter (`TelemetryFormat.h`) and the operator response
// formatter (`OperatorResponse.h`) both produce one line from one record, both
// refuse to emit a partial line, and are both written through the same buffer by
// the transport. What they must agree on therefore lives here rather than in
// either module: what "did not fit" returns, and how long a line may be.

// Returned instead of a length when a line did not fit. Zero is safe as the
// failure value because a completed line always begins with its prefix.
constexpr uint16_t kBoundedLineDidNotFit = 0U;

// Leaves a complete line in `buffer` and returns its length, or empties the
// buffer and reports `kBoundedLineDidNotFit`. A truncated writer is never
// emitted as a shortened line: the tail of a record is where its diagnosis lives
// - a fault, an authorization outcome, a failing check, a drop counter - and a
// reader could not tell a missing column from an unremarkable one.
inline uint16_t finishBoundedLine(const BoundedText& text, char* buffer) {
    if (text.truncated()) {
        if (buffer != nullptr) {
            buffer[0] = '\0';
        }
        return kBoundedLineDidNotFit;
    }
    return text.length();
}

// The longest line either consumer may produce, in characters, excluding the
// terminator, so the transport's one buffer holds `kSerialLineBufferBytes`
// bytes. The bound sits above the worst case each module derives from its own
// tables - 418 characters for a telemetry sample, 384 for the full
// configuration-check report - with room to spare, so a table entry that grows
// has to grow a long way before a line stops fitting.
constexpr uint16_t kSerialLineCapacity = 448U;
constexpr uint16_t kSerialLineBufferBytes = kSerialLineCapacity + 1U;

}  // namespace ev
