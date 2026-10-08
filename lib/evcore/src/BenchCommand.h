#pragma once

#include "BenchDrive.h"
#include "Motion.h"

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The bench console's line grammar - pure, bounded and host-tested
// ---------------------------------------------------------------------------
//
// What this module is
//
// * Everything about turning a `bench ...` line into a request: splitting it into
//   whitespace-separated tokens without allocating, recognizing the verb, and
//   parsing the one verb that takes arguments (`bench drive <fwd|rev> <permille>
//   <ms>`).
// * In `lib/` and host-tested, deliberately. The console that owns the serial port
//   lives in `src/bench/` and cannot be host-tested, and the one thing in it that
//   can be wrong in ways nobody notices on a bench - an off-by-one token index, a
//   number that overflows into a smaller one, a line with more tokens than the
//   grammar allows - is exactly this. The console keeps only the dispatch.
//
// What it is not
//
// * It does not decide whether a command may be applied: `ev::BenchDrive` owns the
//   arm, the ceilings, the budget and the latch (`BenchDrive.h`), and this module
//   only refuses input that is not a well-formed request for it.
// * It is not the production command grammar. `ev::SerialCommandParser` parses the
//   operator interface, whose commands are enumerated and whose numbers have their
//   own bounds; nothing here is shared with it, and no bench verb is a production
//   command.
//
// Kind of change this is
//
//   Bench-only instrumentation (`SOFTWARE_ARCHITECTURE.md` section 17). It is
//   compiled into the bench image only, because its only caller is in
//   `src/bench/`, which the production build does not compile.

// The most tokens any bench line may have: `bench`, a verb, and at most two
// arguments (`bench drive` takes three after the verb, so the capacity is one
// more than the longest well-formed line needs, which is what lets an over-long
// line be *reported* rather than silently truncated).
constexpr uint8_t kBenchTokenCapacity = 6U;

// A line split into tokens. `starts`/`lengths` are views into the caller's buffer:
// nothing is copied and nothing is allocated, so this is safe to build per line on
// a microcontroller.
struct BenchTokens {
    // `overflow` is true when the line had more tokens than the capacity, in which
    // case `count` is the capacity and the tokens present are still valid. A caller
    // must refuse an overflowing line rather than act on the tokens it can see:
    // "the parts I understood" is how a command nobody typed gets applied.
    bool overflow = false;
    uint8_t count = 0U;
    const char* starts[kBenchTokenCapacity] = {};
    uint16_t lengths[kBenchTokenCapacity] = {};

    // The spelling of token `index`, or nullptr when there is no such token. The
    // pointer is not NUL-terminated; use `tokenLength()` with it.
    const char* token(uint8_t index) const {
        return index < count ? starts[index] : nullptr;
    }

    uint16_t tokenLength(uint8_t index) const { return index < count ? lengths[index] : 0U; }
};

// Splits `line` on spaces and tabs. The line reader has already removed the
// terminator, so a trailing `\r` would be part of the last token - the caller's
// reader strips terminators, and a bench capture that shows a stray `\r` in an
// argument is a finding about the host, not something to paper over here.
inline BenchTokens splitBenchTokens(const char* line, uint16_t length) {
    BenchTokens tokens;
    if (line == nullptr) {
        return tokens;
    }
    uint16_t index = 0U;
    while (index < length) {
        while (index < length && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= length) {
            break;
        }
        if (tokens.count >= kBenchTokenCapacity) {
            tokens.overflow = true;
            break;
        }
        const uint16_t start = index;
        while (index < length && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        tokens.starts[tokens.count] = line + start;
        tokens.lengths[tokens.count] = static_cast<uint16_t>(index - start);
        ++tokens.count;
    }
    return tokens;
}

// True when the whole token is `expected`. Length-checked, so `drive` does not
// match `drive-status` and a token that is a prefix of a verb is not a verb.
inline bool benchTokenEquals(const char* token, uint16_t length, const char* expected) {
    if (token == nullptr || expected == nullptr) {
        return false;
    }
    uint16_t index = 0U;
    for (; index < length; ++index) {
        if (expected[index] == '\0' || token[index] != expected[index]) {
            return false;
        }
    }
    return expected[index] == '\0';
}

// An unsigned decimal, digits only, no leading zeros, no sign, no overflow.
//
// No `strtoul`: the input is a few characters typed by a person, this runs on a
// microcontroller, and an overflow that wrapped into a *small* number would turn a
// mistyped effort or duration into a command that is accepted and is not the one
// that was typed. Leading zeros are refused for the same reason - `07` is a typo
// nobody meant, and guessing which number it is is not this module's business.
inline bool parseUnsignedDecimal(const char* text, uint16_t length, uint32_t& value) {
    if (text == nullptr || length == 0U) {
        return false;
    }
    if (length > 1U && text[0] == '0') {
        return false;
    }
    uint32_t accumulated = 0U;
    for (uint16_t index = 0U; index < length; ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        const uint32_t digitValue = static_cast<uint32_t>(digit - '0');
        // The exact condition for `accumulated * 10 + digitValue` to fit, taken
        // per digit: a fixed bound on `accumulated` alone is off by one digit for
        // the largest digits, which is how `4294967296` parses as a wrapped zero.
        if (accumulated > (0xFFFFFFFFu - digitValue) / 10U) {
            return false;
        }
        accumulated = accumulated * 10U + digitValue;
    }
    value = accumulated;
    return true;
}

// The verb of a bench line, or None when the line is not a bench line at all
// (which the console hands back to the production operator interface) and Unknown
// when it is one but names nothing this console knows.
enum class BenchVerb : uint8_t {
    None = 0,   // not a `bench` line
    Help = 1,   // `bench`, `bench help`
    Arm = 2,    // `bench arm`
    Drive = 3,  // `bench drive <fwd|rev> <permille> <ms>`
    Stop = 4,   // `bench stop`
    Reset = 5,  // `bench reset`
    Health = 6,         // `bench health`
    DriveStatus = 7,    // `bench drive-status`
    Ticks = 8,          // `bench ticks`
    Inputs = 9,         // `bench inputs`, `bench trigger`
    Pins = 11,          // `bench pins`
    ResetCause = 12,    // `bench resetsrc [clear]`
    Wdt = 13,           // `bench wdt`
    Unknown = 10,       // a `bench` line with a verb nobody implements
};

// Recognizes the verb. A `bench` line with no verb is `Help`: typing the namespace
// and pressing enter is how an operator asks what is there, exactly as the
// production interface answers a bare unknown line with its usage.
inline BenchVerb benchVerbFor(const BenchTokens& tokens) {
    if (tokens.count == 0U) {
        return BenchVerb::None;
    }
    if (!benchTokenEquals(tokens.token(0), tokens.tokenLength(0), "bench")) {
        return BenchVerb::None;
    }
    if (tokens.count == 1U) {
        return BenchVerb::Help;
    }
    const char* verb = tokens.token(1);
    const uint16_t length = tokens.tokenLength(1);
    if (benchTokenEquals(verb, length, "help")) {
        return BenchVerb::Help;
    }
    if (benchTokenEquals(verb, length, "arm")) {
        return BenchVerb::Arm;
    }
    if (benchTokenEquals(verb, length, "drive")) {
        return BenchVerb::Drive;
    }
    if (benchTokenEquals(verb, length, "stop")) {
        return BenchVerb::Stop;
    }
    if (benchTokenEquals(verb, length, "reset")) {
        return BenchVerb::Reset;
    }
    if (benchTokenEquals(verb, length, "health")) {
        return BenchVerb::Health;
    }
    if (benchTokenEquals(verb, length, "drive-status")) {
        return BenchVerb::DriveStatus;
    }
    if (benchTokenEquals(verb, length, "ticks")) {
        return BenchVerb::Ticks;
    }
    if (benchTokenEquals(verb, length, "inputs") || benchTokenEquals(verb, length, "trigger")) {
        return BenchVerb::Inputs;
    }
    if (benchTokenEquals(verb, length, "pins")) {
        return BenchVerb::Pins;
    }
    if (benchTokenEquals(verb, length, "resetsrc")) {
        return BenchVerb::ResetCause;
    }
    if (benchTokenEquals(verb, length, "wdt")) {
        return BenchVerb::Wdt;
    }
    return BenchVerb::Unknown;
}

// The largest number of tokens a verb accepts, including the leading `bench`:
// `bench`, the verb, and its arguments. The console refuses a longer line before
// dispatch, because a trailing token on a verb that takes none is a refusal and
// not something to ignore (M3 - `bench arm <junk>` used to arm, and `bench stop
// <junk>` used to stop). `drive`'s five and `resetsrc`'s optional `clear` are in
// the table too, so every verb's token bound lives in one place; their own parsers
// still give the specific error for a malformed argument.
inline uint8_t benchVerbMaximumTokenCount(BenchVerb verb) {
    switch (verb) {
        case BenchVerb::Help:
            return 2U;  // `bench`, `bench help`
        case BenchVerb::Drive:
            return 5U;  // `bench drive <fwd|rev> <permille> <ms>`
        case BenchVerb::ResetCause:
            return 3U;  // `bench resetsrc [clear]`
        case BenchVerb::Arm:
        case BenchVerb::Stop:
        case BenchVerb::Reset:
        case BenchVerb::Health:
        case BenchVerb::DriveStatus:
        case BenchVerb::Ticks:
        case BenchVerb::Inputs:
        case BenchVerb::Pins:
        case BenchVerb::Wdt:
            return 2U;  // `bench <verb>` only
        case BenchVerb::None:
        case BenchVerb::Unknown:
            return 0U;  // never dispatched as a bench command
    }
    return 0U;
}

// Why a `bench drive` line could not be turned into a request. None means it was.
enum class BenchArgumentError : uint8_t {
    None = 0,
    NotADriveLine = 1,      // the wrong verb or the wrong token count
    ExtraTokens = 2,        // more tokens than the grammar allows
    DirectionNotFwdOrRev = 3,
    EffortNotADecimal = 4,
    DurationNotADecimal = 5,
    EffortAboveFullScale = 6,
};

inline const char* benchArgumentErrorName(BenchArgumentError error) {
    switch (error) {
        case BenchArgumentError::None:
            return "None";
        case BenchArgumentError::NotADriveLine:
            return "Usage: bench drive <fwd|rev> <permille> <ms>";
        case BenchArgumentError::ExtraTokens:
            return "TooManyTokens";
        case BenchArgumentError::DirectionNotFwdOrRev:
            return "DirectionMustBeFwdOrRev";
        case BenchArgumentError::EffortNotADecimal:
            return "EffortNotADecimal";
        case BenchArgumentError::DurationNotADecimal:
            return "DurationNotADecimal";
        case BenchArgumentError::EffortAboveFullScale:
            return "EffortAboveFullScale";
    }
    return "?";
}

// One parsed `bench drive`: either an error, or the request as typed. It carries no
// verdict from `ev::BenchDrive` - whether the request is within the bench ceilings
// is that module's decision, and this only says it is well formed.
struct BenchDriveRequest {
    BenchArgumentError error = BenchArgumentError::NotADriveLine;
    MotorMode mode = MotorMode::Disabled;
    uint16_t permille = 0U;
    uint32_t durationMs = 0U;
};

// Parses `bench drive <fwd|rev> <permille> <ms>`: five tokens, the direction spelled
// `fwd` or `rev`, two unsigned decimals. A permille above full scale is refused
// here; whether the effort is within the *bench* ceiling is `BenchDrive`'s refusal
// to make, so that the refusal an operator sees keeps naming the real limit.
inline BenchDriveRequest parseBenchDriveRequest(const BenchTokens& tokens) {
    BenchDriveRequest request;
    if (tokens.overflow || tokens.count > 5U) {
        request.error = BenchArgumentError::ExtraTokens;
        return request;
    }
    if (benchVerbFor(tokens) != BenchVerb::Drive || tokens.count != 5U) {
        request.error = BenchArgumentError::NotADriveLine;
        return request;
    }

    if (benchTokenEquals(tokens.token(2), tokens.tokenLength(2), "fwd")) {
        request.mode = MotorMode::DriveForward;
    } else if (benchTokenEquals(tokens.token(2), tokens.tokenLength(2), "rev")) {
        request.mode = MotorMode::DriveReverse;
    } else {
        request.error = BenchArgumentError::DirectionNotFwdOrRev;
        return request;
    }

    uint32_t permille = 0U;
    if (!parseUnsignedDecimal(tokens.token(3), tokens.tokenLength(3), permille)) {
        request.error = BenchArgumentError::EffortNotADecimal;
        return request;
    }
    if (permille > 1000U) {
        request.error = BenchArgumentError::EffortAboveFullScale;
        return request;
    }

    uint32_t durationMs = 0U;
    if (!parseUnsignedDecimal(tokens.token(4), tokens.tokenLength(4), durationMs)) {
        request.error = BenchArgumentError::DurationNotADecimal;
        return request;
    }

    request.error = BenchArgumentError::None;
    request.permille = static_cast<uint16_t>(permille);
    request.durationMs = durationMs;
    return request;
}

}  // namespace ev
