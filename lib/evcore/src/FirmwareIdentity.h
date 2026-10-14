#pragma once

// Reproducible firmware build identity (R3/L3).
//
// The constant firmware string this replaces (`ev-nano-r4-p7b`) could not tell
// two materially different images apart: same string for different source
// revisions, for bench vs production images, for dirty vs clean trees and for
// different calibration configurations. This header defines what a
// device-reported identity carries so a reviewer can associate a serial log
// with the image that produced it.
//
// What the identity is, and is not
//
//   * `family` names the firmware family (`ev-nano-r4`). It is stable across
//     builds on purpose: it says which project this is, not which build.
//   * `revision` names the source revision the image was built from (a short
//     commit hash supplied by the build script), or `unknown` when no revision
//     metadata was available. It identifies the SOURCE revision, never the
//     flashed binary: a rebuild from the same source with a different
//     toolchain is a different binary with the same revision.
//   * `dirty` tells a dirty (or unknown-provenance) tree from a clean
//     committed one. A dirty build never masquerades as a clean image.
//   * `variant` tells the three PlatformIO images apart (production, bench,
//     watchdog-enabled bench). It comes from a per-environment build define,
//     never from probing the running image.
//   * `configFingerprint` is a 32-bit FNV-1a hash over the calibration and
//     configuration values the build carries, so two images built from the
//     same source with different tuning report different identities.
//     `configSchema` is bumped whenever the fingerprinted set changes, so a
//     fingerprint from an older schema cannot be compared silently.
//   * No wall-clock timestamps (they would break reproducible builds), no
//     absolute paths, no secrets, no environment variables.
//
// The serial rendering is one comma-field fragment shared by every identity
// report (`VERSION`, `BOOT`): `fw=<family>,rev=<rev>,dirty=<0|1>,
// variant=<name>,cfg=<8 hex>,cfg_schema=<n>`. It is bounded
// (`kFirmwareIdentityWorstCaseChars`) and truncation-safe: a buffer too small
// yields `kBoundedLineDidNotFit` with the buffer emptied, never a shortened
// identity whose missing tail reads as a different build.

#include <cstring>

#include <BoundedText.h>

namespace ev {

// The image variant, as the build environment defines it. Numeric values are
// stable: they travel in no serial field today, and stability keeps any
// future use comparable.
enum class FirmwareVariant : uint8_t {
    Production = 0,    // env:nano_r4, the vehicle image
    Bench = 1,         // env:bench, console + bounded drive instruments
    BenchWatchdog = 2, // env:bench-watchdog, bench tree with the timer open
    Unknown = 3,       // no variant define reached the build; never a guess
};

inline const char* firmwareVariantName(FirmwareVariant variant) {
    switch (variant) {
        case FirmwareVariant::Production:
            return "production";
        case FirmwareVariant::Bench:
            return "bench";
        case FirmwareVariant::BenchWatchdog:
            return "bench-watchdog";
        case FirmwareVariant::Unknown:
            return "unknown";
    }
    return "unknown";
}

// The revision vocabulary when no revision metadata was available.
constexpr const char* kFirmwareRevisionUnknown = "unknown";

// The firmware family. Stable across builds by design.
constexpr const char* kFirmwareFamily = "ev-nano-r4";

// Bumped whenever the set of values `configFingerprint` covers changes.
constexpr uint8_t kFirmwareConfigSchema = 1U;

// One device-reported build identity. `family`/`revision` are compile-time
// string literals owned by the vehicle layer; nothing here copies them.
struct FirmwareIdentity {
    const char* family = kFirmwareFamily;
    const char* revision = kFirmwareRevisionUnknown;
    bool dirty = true;
    FirmwareVariant variant = FirmwareVariant::Unknown;
    uint32_t configFingerprint = 0U;
    uint8_t configSchema = kFirmwareConfigSchema;
};

// True when the identity describes a clean build from a known revision of a
// known image variant: the only identity a release record may cite.
inline bool firmwareIdentityIsCleanBuild(const FirmwareIdentity& identity) {
    return !identity.dirty && identity.revision != nullptr &&
           std::strcmp(identity.revision, kFirmwareRevisionUnknown) != 0 &&
           identity.variant != FirmwareVariant::Unknown;
}

// Worst-case rendering, in characters:
//
//   `fw=` 3 + family 15 + `,rev=` 5 + revision 16 + `,dirty=` 7 + 1 +
//   `,variant=` 9 + 14 (`bench-watchdog`) + `,cfg=` 5 + 8 + `,cfg_schema=` 12
//   + 3 = 98.
//
// The family/revision caps are forward tolerance, not current widths
// (today: 10 and 7). A longer literal still renders - `BoundedText`
// truncates safely - but exceeding these caps breaks this bound visibly in
// the test that pins it, which is the point: identity growth must stay
// inside the serial line budget (`kSerialLineCapacity`) on purpose.
constexpr uint16_t kFirmwareIdentityWorstCaseChars = 98U;

// 32-bit FNV-1a over arbitrary bytes. The fold behind the configuration
// fingerprint: order-sensitive, deterministic for identical inputs on any
// target, with no tables and no allocation.
class FingerprintFold {
  public:
    FingerprintFold() : hash_(kFnvOffsetBasis) {}

    void mixBytes(const void* data, uint16_t length) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (uint16_t i = 0U; i < length; ++i) {
            hash_ ^= bytes[i];
            hash_ *= kFnvPrime;
        }
    }

    void mixUint8(uint8_t value) { mixBytes(&value, 1U); }

    void mixUint16(uint16_t value) {
        // Little-endian byte order, fixed: the fingerprint must not depend
        // on the host that computed it.
        const uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFFU),
                                  static_cast<uint8_t>((value >> 8) & 0xFFU)};
        mixBytes(bytes, 2U);
    }

    void mixUint32(uint32_t value) {
        const uint8_t bytes[4] = {
            static_cast<uint8_t>(value & 0xFFU),
            static_cast<uint8_t>((value >> 8) & 0xFFU),
            static_cast<uint8_t>((value >> 16) & 0xFFU),
            static_cast<uint8_t>((value >> 24) & 0xFFU)};
        mixBytes(bytes, 4U);
    }

    void mixDouble(double value) {
        // The value's eight IEEE-754 bytes, not a decimal rendering: two
        // builds from the same source hash the same value, and values that
        // differ by one ulp hash differently.
        uint64_t bits = 0U;
        static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
        std::memcpy(&bits, &value, sizeof(bits));
        for (uint8_t i = 0U; i < 8U; ++i) {
            mixUint8(static_cast<uint8_t>((bits >> (8U * i)) & 0xFFU));
        }
    }

    uint32_t done() const { return hash_; }

  private:
    static constexpr uint32_t kFnvOffsetBasis = 2166136261U;
    static constexpr uint32_t kFnvPrime = 16777619U;
    uint32_t hash_;
};

namespace detail {

inline void appendHexByte(BoundedText& text, uint8_t value) {
    constexpr char kDigits[] = "0123456789abcdef";
    text.appendChar(kDigits[(value >> 4) & 0xFU]);
    text.appendChar(kDigits[value & 0xFU]);
}

}  // namespace detail

// Renders the identity fragment into `text` (`fw=...,rev=...,...`). Comma
// handling follows the schema convention: the first field has no leading
// comma because the writer is empty, so this fragment composes inside any
// line (`VERSION`, `BOOT`) without the caller managing separators.
inline void appendFirmwareIdentity(const FirmwareIdentity& identity, BoundedText& text) {
    // Null-proof by construction: the vehicle layer only ever passes
    // compile-time literals, and a null here renders as the family/unknown
    // marker rather than as an empty field a reader could misparse.
    text.field("fw").appendText(identity.family == nullptr ? kFirmwareFamily
                                                           : identity.family);
    text.field("rev").appendText(identity.revision == nullptr ? kFirmwareRevisionUnknown
                                                              : identity.revision);
    text.field("dirty").appendBool(identity.dirty);
    text.field("variant").appendText(firmwareVariantName(identity.variant));
    {
        BoundedText& hashed = text.field("cfg");
        (void)hashed;
        // Eight lowercase hex digits, zero-padded: fixed width, fixed case,
        // so logs compare with plain string equality.
        for (int8_t shift = 28; shift >= 0; shift -= 4) {
            constexpr char kDigits[] = "0123456789abcdef";
            text.appendChar(kDigits[(identity.configFingerprint >>
                                     static_cast<uint32_t>(shift)) &
                                    0xFU]);
        }
    }
    text.field("cfg_schema").appendUnsigned(identity.configSchema);
}

// One identity fragment as one line, NUL-terminated. Returns the length, or
// `kBoundedLineDidNotFit` (with the buffer emptied) when it did not fit.
inline uint16_t formatFirmwareIdentity(const FirmwareIdentity& identity, char* buffer,
                                       uint16_t capacity) {
    BoundedText text(buffer, capacity);
    appendFirmwareIdentity(identity, text);
    return finishBoundedLine(text, buffer);
}

}  // namespace ev
