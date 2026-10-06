#pragma once

#include <cstdint>

#include "RunConfig.h"

namespace ev {

// ---------------------------------------------------------------------------
// The serial command parser - bounded, strict, and stateless
// ---------------------------------------------------------------------------
//
// The input half of the operator interface. One line of bytes becomes either
// one typed `OperatorRequest` or one named rejection, and that is all this
// module does: it holds no configuration, arms nothing, drives nothing, reads no
// clock, links no Arduino header, and knows no run phase.
//
// Design rules, and why each one exists
//
// * **Bounded.** A line is at most `kCommandLineCapacity` characters and a
//   command is at most `kCommandTokenCapacity` tokens. Both bounds are checked
//   before anything is interpreted, so a hostile or broken host cannot make the
//   firmware work longer or allocate anything.
// * **A rejected line is discarded whole, never truncated into a valid
//   prefix.** An overlong line, or one containing a tab or another
//   non-printable byte, is consumed to its terminator and then reported as one
//   rejection. Executing the first half of a mistyped command is exactly the
//   failure mode a command interface must not have.
// * **Strict separators.** Exactly one space between tokens: no leading space,
//   no trailing space, no run of spaces, no tabs. Whitespace-heavy input is
//   rejected rather than normalized, because "the line the operator sent" and
//   "the line the firmware executed" should never differ by invisible bytes.
// * **Explicit confirmation is a separate command.** `config set` stores values
//   and echoes them; it never announces a run. `announce` is the deliberate
//   second step, which is why it exists as its own command.
// * **Digits, not `strtod`.** Numbers are parsed by hand as unsigned decimals
//   with at most `kCommandFractionDigitsMax` fractional digits, so parsing does
//   not depend on locale, on the C library's float conversion, or on the host
//   and target agreeing about either. A sign character is rejected: every
//   quantity this grammar sets (Target Distance, Target Time, Bottle Line
//   Distance, telemetry rate) is non-negative by rule, so a sign is a typo, not
//   a value.
//
// What this module deliberately does not do: it does not check a value against
// the event rules. Range and announced-grid checks have exactly one owner,
// `ev::validateRunConfig()` (`SOFTWARE_ARCHITECTURE.md` section 8), and the
// operator policy calls it. A magnitude bound here only keeps the value
// representable and the formatting bounded; it is not a rule check.

// The longest command line the firmware accepts, in characters, excluding the
// terminator. The longest legitimate command (`config set` with all four
// fields, or `telemetry rate`) is well under half of it.
constexpr uint16_t kCommandLineCapacity = 96U;

// The most tokens one command may contain. `config set` uses six.
constexpr uint16_t kCommandTokenCapacity = 8U;

// A value's integer part may not exceed this magnitude. Far above any rule
// value (Target Distance is 5.00-9.00 m, Target Time 10.0-20.0 s) and far below
// the point where a double stops being exact: this is a representability bound,
// not a rule.
constexpr double kCommandNumberMaxMagnitude = 1.0e6;

// Fractional digits accepted. The rules publish 0.01 m and 0.1 s, so nine
// places is generous; a tenth is a mistake (or a pasted timestamp), not input.
constexpr uint8_t kCommandFractionDigitsMax = 9U;

// What one byte did to the line buffer.
enum class LineEvent : uint8_t {
    None = 0,        // the byte was accepted as line content
    EmptyLine,       // a terminator arrived with no content (also the `\n` of `\r\n`)
    LineReady,       // a complete, bounded line is available
    LineTooLong,     // the line exceeded the buffer; it was discarded whole
    InvalidCharacter // a non-printable byte; the line was discarded whole
};

// The bounded line accumulator. The caller owns the storage, so this module
// allocates nothing and holds nothing that survives a line.
//
// One instance serves one input stream. It is not reentrant and is not meant to
// be: the firmware reads serial input from the main loop, never from an
// interrupt (`SOFTWARE_ARCHITECTURE.md` section 15).
class CommandLineReader {
public:
    // `capacity` is the longest accepted line, excluding the terminator, so the
    // caller's buffer must hold `capacity + 1` bytes.
    CommandLineReader(char* buffer, uint16_t capacity)
        : buffer_(buffer),
          capacity_(capacity),
          length_(0U),
          discarding_(false),
          lineReady_(false),
          pending_(LineEvent::None),
          rejectedLines_(0U) {
        if (buffer_ != nullptr && capacity_ > 0U) {
            buffer_[0] = '\0';
        }
    }

    // Feeds one received byte.
    //
    // A terminator (`\n` or `\r`) always ends the current line. Anything else is
    // either accepted as content or - if it is not printable ASCII, or the line
    // is already full - starts the discard of the whole line, which continues
    // silently to the next terminator. A caller may therefore ignore every
    // event except `LineReady` and still never execute a partial line.
    LineEvent push(char byte) {
        // A byte arriving after a completed line means the caller has had its
        // chance to read that line, so this one starts fresh. That is what makes
        // a partial command safe: bytes that never reach a terminator are held, 
        // never executed, and the buffer is bounded by construction.
        if (lineReady_) {
            clearLine();
        }

        const bool terminator = (byte == '\n' || byte == '\r');
        if (terminator) {
            return finishLine();
        }

        if (discarding_) {
            return LineEvent::None;
        }

        // Printable ASCII only. A tab, a control byte, or any non-ASCII byte is
        // not whitespace to normalize away - it means the line did not arrive as
        // the operator sent it.
        const uint8_t raw = static_cast<uint8_t>(byte);
        if (raw < 0x20U || raw > 0x7EU) {
            startDiscard(LineEvent::InvalidCharacter);
            return LineEvent::None;
        }

        if (length_ >= capacity_) {
            startDiscard(LineEvent::LineTooLong);
            return LineEvent::None;
        }

        buffer_[length_] = byte;
        ++length_;
        buffer_[length_] = '\0';
        return LineEvent::None;
    }

    // The completed line. Valid after `LineReady` and until the next byte is
    // pushed, NUL-terminated, and empty when no line is pending.
    const char* line() const { return buffer_ == nullptr ? "" : buffer_; }

    uint16_t lineLength() const { return length_; }

    // Lines discarded because of a bound or a bad byte, since construction.
    // Counted rather than logged: a rejection response is emitted once, at the
    // terminator, and this is the cumulative evidence a diagnostics reader can
    // ask for.
    uint32_t rejectedLines() const { return rejectedLines_; }

    // Discards whatever is buffered and returns to expecting a fresh line. The
    // counters are evidence and are deliberately not cleared.
    void reset() {
        discarding_ = false;
        pending_ = LineEvent::None;
        clearLine();
    }

private:
    void clearLine() {
        length_ = 0U;
        lineReady_ = false;
        if (buffer_ != nullptr && capacity_ > 0U) {
            buffer_[0] = '\0';
        }
    }

    void startDiscard(LineEvent reason) {
        discarding_ = true;
        pending_ = reason;
        // The partial content is dropped now, so nothing downstream can read it
        // even by accident.
        clearLine();
    }

    LineEvent finishLine() {
        if (discarding_) {
            const LineEvent reason = pending_;
            discarding_ = false;
            pending_ = LineEvent::None;
            clearLine();
            ++rejectedLines_;
            return reason;
        }
        if (length_ == 0U) {
            return LineEvent::EmptyLine;
        }
        // The content and its length stay readable until the next byte arrives
        // (`push`) or the caller resets, so `line()` and `lineLength()` agree
        // for as long as the caller is meant to be reading them.
        lineReady_ = true;
        return LineEvent::LineReady;
    }

    char* buffer_;
    uint16_t capacity_;
    uint16_t length_;
    bool discarding_;
    bool lineReady_;
    LineEvent pending_;
    uint32_t rejectedLines_;
};

// Why a line was not accepted as a command. Every one of these is reported to
// the operator by name, so a rejection says what to change rather than that
// something went wrong.
enum class CommandError : uint8_t {
    None = 0,
    EmptyCommand,        // a line with no tokens at all
    EmptyToken,          // a leading space, a trailing space, or two separators
    InvalidCharacter,    // a non-printable byte (the line was discarded whole)
    LineTooLong,         // the line exceeded kCommandLineCapacity
    TooManyTokens,       // more than kCommandTokenCapacity tokens
    UnknownCommand,      // the first token is not a command
    UnknownSubcommand,   // e.g. `config frobnicate`
    WrongTokenCount,     // the right command with the wrong number of arguments
    MissingArgument,     // `config set` with no `key=value` at all
    MalformedArgument,   // an argument that is not `key=value`
    UnknownKey,          // a `config set` key that does not exist
    DuplicateKey,        // the same key twice in one command
    InvalidValue,        // a value that is not a number in this grammar
    ValueNotFinite,      // `nan`, `inf`, `infinity`
    ValueOutOfRange,     // a number beyond kCommandNumberMaxMagnitude
    UnknownLevel,        // a tournament level name that does not exist
};

// The stable wire spelling of a rejection, used in operator responses and in
// the telemetry schema. Treat these strings as part of the schema.
inline const char* commandErrorName(CommandError error) {
    switch (error) {
        case CommandError::None:
            return "None";
        case CommandError::EmptyCommand:
            return "EmptyCommand";
        case CommandError::EmptyToken:
            return "EmptyToken";
        case CommandError::InvalidCharacter:
            return "InvalidCharacter";
        case CommandError::LineTooLong:
            return "LineTooLong";
        case CommandError::TooManyTokens:
            return "TooManyTokens";
        case CommandError::UnknownCommand:
            return "UnknownCommand";
        case CommandError::UnknownSubcommand:
            return "UnknownSubcommand";
        case CommandError::WrongTokenCount:
            return "WrongTokenCount";
        case CommandError::MissingArgument:
            return "MissingArgument";
        case CommandError::MalformedArgument:
            return "MalformedArgument";
        case CommandError::UnknownKey:
            return "UnknownKey";
        case CommandError::DuplicateKey:
            return "DuplicateKey";
        case CommandError::InvalidValue:
            return "InvalidValue";
        case CommandError::ValueNotFinite:
            return "ValueNotFinite";
        case CommandError::ValueOutOfRange:
            return "ValueOutOfRange";
        case CommandError::UnknownLevel:
            return "UnknownLevel";
    }
    return "UnknownCommand";
}

// Every command the interface understands. The vocabulary is deliberately small
// and frozen: a command that does not exist cannot be mistyped into one that
// does. Note what is absent - there is no command that starts a run (the start
// trigger is a physical input) and none that commands an effort or a direction
// (only `SafetyAuthority` authorizes those, and nothing in the operator
// interface may bypass it).
enum class OperatorCommandId : uint8_t {
    None = 0,
    Help,
    Status,
    Version,
    Checks,
    Boot,           // re-send the boot line (this firmware's identity and state)
    ConfigShow,     // echo the held configuration and its rule verdict
    ConfigCheck,    // re-run the rule validation now and echo it
    Plan,           // read back the announced run's execution plan, or why there is none
    ConfigSet,      // store the supplied fields (never announces anything)
    ConfigClear,    // discard the held configuration
    Announce,       // the deliberate second step: offer the configuration to a run
    Arm,            // the deliberate arming intent
    Disarm,         // always available
    Reset,          // return the chain to its new-run boundary
    TelemetryOn,
    TelemetryOff,
    TelemetryStatus,
    TelemetryRate,  // sample throttle, samples per second
};

inline const char* operatorCommandName(OperatorCommandId id) {
    switch (id) {
        case OperatorCommandId::None:
            return "none";
        case OperatorCommandId::Help:
            return "help";
        case OperatorCommandId::Status:
            return "status";
        case OperatorCommandId::Version:
            return "version";
        case OperatorCommandId::Checks:
            return "checks";
        case OperatorCommandId::Boot:
            return "boot";
        case OperatorCommandId::ConfigShow:
            return "config show";
        case OperatorCommandId::ConfigCheck:
            return "config check";
        case OperatorCommandId::Plan:
            return "plan";
        case OperatorCommandId::ConfigSet:
            return "config set";
        case OperatorCommandId::ConfigClear:
            return "config clear";
        case OperatorCommandId::Announce:
            return "announce";
        case OperatorCommandId::Arm:
            return "arm";
        case OperatorCommandId::Disarm:
            return "disarm";
        case OperatorCommandId::Reset:
            return "reset";
        case OperatorCommandId::TelemetryOn:
            return "telemetry on";
        case OperatorCommandId::TelemetryOff:
            return "telemetry off";
        case OperatorCommandId::TelemetryStatus:
            return "telemetry status";
        case OperatorCommandId::TelemetryRate:
            return "telemetry rate";
    }
    return "none";
}

// The three tournament levels, indexed. The index order is the order of
// `TournamentLevel` itself (Regional/Invitational, State, National), so an index
// and the enumerator it names cannot drift apart.
constexpr uint8_t kTournamentLevelCount = 3U;

// The level name an operator types, and the only spelling accepted for it.
inline const char* tournamentLevelName(uint8_t index) {
    switch (index) {
        case 0U:
            return "regional";
        case 1U:
            return "state";
        case 2U:
            return "national";
        default:
            return "?";
    }
}

// Exact, case-sensitive match against the three accepted spellings. No
// abbreviations beyond these and no case folding: `State` is a rejection, not an
// approximation of `state`.
inline bool tournamentLevelIndexFromName(const char* name, uint8_t& index) {
    if (name == nullptr) {
        return false;
    }
    for (uint8_t candidate = 0U; candidate < kTournamentLevelCount; ++candidate) {
        const char* spelling = tournamentLevelName(candidate);
        uint8_t position = 0U;
        while (spelling[position] != '\0' && name[position] == spelling[position]) {
            ++position;
        }
        if (spelling[position] == '\0' && name[position] == '\0') {
            index = candidate;
            return true;
        }
    }
    return false;
}

// The configuration fields the interface can set.
//
// **One type for both the request and the held configuration.** `config set`
// supplies only the fields the operator named, and the same type holds the
// accumulated configuration, so there is no second "draft" type to keep in
// agreement with the first. A field is present exactly when its `has` flag is
// true; a field that was never set cannot be mistaken for a zero, which is why
// the value below it is meaningless until the flag says otherwise.
//
// The Bottle Start Point is not a field: rule 7.c.ii.(5) fixes it from the
// tournament level, so it is derived at the announcement boundary rather than
// typed, which is one fewer number to mistype.
struct RunConfigFields {
    bool hasLevel = false;
    uint8_t levelIndex = 0U;
    bool hasTargetDistanceM = false;
    double targetDistanceM = 0.0;
    bool hasTargetTimeS = false;
    double targetTimeS = 0.0;
    bool hasBottleLineDistanceM = false;
    double bottleLineDistanceM = 0.0;

    // True when every field a `RunConfig` needs has been supplied. Only such a
    // configuration may be announced.
    bool complete() const {
        return hasLevel && hasTargetDistanceM && hasTargetTimeS && hasBottleLineDistanceM;
    }
};

// One accepted command, typed. Values that do not apply to the command's id are
// meaningless and are never read: `fields` is read only for `ConfigSet`, and
// `telemetryRateHz` only for `TelemetryRate`.
struct OperatorRequest {
    OperatorCommandId id = OperatorCommandId::None;
    RunConfigFields fields{};
    double telemetryRateHz = 0.0;
};

struct CommandParseResult {
    // Invariant: error == CommandError::None exactly when ok is true.
    bool ok = false;
    CommandError error = CommandError::None;
    OperatorRequest request{};
};

namespace commandParserDetail {

struct Token {
    const char* text;
    uint16_t length;
};

inline bool tokenEquals(const Token& token, const char* text) {
    uint16_t position = 0U;
    while (text[position] != '\0' && position < token.length && token.text[position] == text[position]) {
        ++position;
    }
    return text[position] == '\0' && position == token.length;
}

// A value that is not a number at all: `nan`, `inf`, `infinity`, with either
// sign prefix. Rejected as its own error so a non-finite entry is reported as
// what it is rather than as generic garbage.
inline bool tokenIsNonFiniteSpelling(const Token& token) {
    static const char* const kSpellings[] = {"nan", "inf", "infinity"};
    for (uint8_t index = 0U; index < 3U; ++index) {
        const char* spelling = kSpellings[index];
        if (token.length == 0U) {
            return false;
        }
        const bool signedSpelling = token.text[0] == '-' || token.text[0] == '+';
        const uint16_t offset = signedSpelling ? 1U : 0U;
        uint16_t position = 0U;
        while (spelling[position] != '\0' && offset + position < token.length &&
               token.text[offset + position] == spelling[position]) {
            ++position;
        }
        if (spelling[position] == '\0' && offset + position == token.length) {
            return true;
        }
    }
    return false;
}

// Splits on exactly one space per separator. Returns false with a reason when
// the line cannot be tokenized: a run of separators, a leading or trailing
// separator, too many tokens, or any byte that is not printable ASCII.
inline bool tokenize(const char* line, uint16_t length, Token* tokens, uint16_t capacity,
                     uint16_t& tokenCount, CommandError& error) {
    tokenCount = 0U;
    error = CommandError::None;
    if (length == 0U) {
        error = CommandError::EmptyCommand;
        return false;
    }
    if (length > kCommandLineCapacity) {
        error = CommandError::LineTooLong;
        return false;
    }

    uint16_t index = 0U;
    while (index < length) {
        const uint8_t raw = static_cast<uint8_t>(line[index]);
        if (raw < 0x20U || raw > 0x7EU) {
            error = CommandError::InvalidCharacter;
            return false;
        }
        if (line[index] == ' ') {
            // A separator is only legal between two tokens: not at the start,
            // not at the end, and never twice in a row.
            error = CommandError::EmptyToken;
            return false;
        }

        const uint16_t start = index;
        while (index < length && line[index] != ' ') {
            const uint8_t inner = static_cast<uint8_t>(line[index]);
            if (inner < 0x20U || inner > 0x7EU) {
                error = CommandError::InvalidCharacter;
                return false;
            }
            ++index;
        }
        if (tokenCount >= capacity) {
            error = CommandError::TooManyTokens;
            return false;
        }
        tokens[tokenCount].text = line + start;
        tokens[tokenCount].length = static_cast<uint16_t>(index - start);
        ++tokenCount;

        if (index < length) {
            ++index;  // consume the single separator
            if (index == length) {
                error = CommandError::EmptyToken;  // a trailing separator
                return false;
            }
        }
    }
    return true;
}

// Parses an unsigned decimal with at most `kCommandFractionDigitsMax`
// fractional digits.
//
// The syntax is scanned to completion before the value is computed, so a
// syntactically broken value is always reported as `InvalidValue` even when its
// digits also overflow: the reported reason is about the text the operator
// typed, not about the order the checks happened to run in.
inline CommandError parseUnsignedDecimal(const Token& token, double& value) {
    if (token.length == 0U) {
        return CommandError::InvalidValue;
    }
    if (tokenIsNonFiniteSpelling(token)) {
        return CommandError::ValueNotFinite;
    }

    uint64_t whole = 0U;
    uint64_t fraction = 0U;
    uint8_t fractionDigits = 0U;
    bool seenDot = false;
    bool sawDigit = false;
    bool overflow = false;

    for (uint16_t index = 0U; index < token.length; ++index) {
        const char character = token.text[index];
        if (character == '.') {
            if (seenDot) {
                return CommandError::InvalidValue;  // a second dot
            }
            seenDot = true;
            continue;
        }
        if (character < '0' || character > '9') {
            // Rejects signs, exponents (`1e3`), unit suffixes (`7.1m`) and
            // anything else that is not this grammar's number.
            return CommandError::InvalidValue;
        }
        sawDigit = true;
        if (!seenDot) {
            if (whole > static_cast<uint64_t>(kCommandNumberMaxMagnitude)) {
                overflow = true;  // keep scanning: the syntax may still be wrong
            } else {
                whole = whole * 10U + static_cast<uint64_t>(character - '0');
            }
        } else {
            if (fractionDigits >= kCommandFractionDigitsMax) {
                return CommandError::InvalidValue;
            }
            fraction = fraction * 10U + static_cast<uint64_t>(character - '0');
            ++fractionDigits;
        }
    }

    if (!sawDigit) {
        return CommandError::InvalidValue;  // "." alone, or empty
    }
    if (overflow || whole > static_cast<uint64_t>(kCommandNumberMaxMagnitude)) {
        return CommandError::ValueOutOfRange;
    }

    // Integer parts are exact; the fraction is one division by a power of ten,
    // which is the same IEEE-754 operation on the host and on the target.
    uint64_t scale = 1U;
    for (uint8_t index = 0U; index < fractionDigits; ++index) {
        scale *= 10U;
    }
    value = static_cast<double>(whole) +
            static_cast<double>(fraction) / static_cast<double>(scale);
    return CommandError::None;
}

// Splits one `config set` argument at its single '='.
inline bool splitArgument(const Token& token, Token& key, Token& value) {
    for (uint16_t index = 0U; index < token.length; ++index) {
        if (token.text[index] == '=') {
            key.text = token.text;
            key.length = index;
            value.text = token.text + index + 1U;
            value.length = static_cast<uint16_t>(token.length - index - 1U);
            return key.length > 0U;
        }
    }
    return false;
}

}  // namespace commandParserDetail

// Parses one complete line into one command or one rejection.
//
// `line` must be NUL-terminated or `length` bytes long; the parser reads no
// byte past `length`, and it treats `length` as authoritative so a caller that
// holds a longer buffer cannot hand it more of a line than was received.
inline CommandParseResult parseOperatorCommand(const char* line, uint16_t length) {
    using namespace commandParserDetail;

    CommandParseResult result{};
    if (line == nullptr) {
        result.error = CommandError::EmptyCommand;
        return result;
    }

    Token tokens[kCommandTokenCapacity];
    uint16_t tokenCount = 0U;
    if (!tokenize(line, length, tokens, kCommandTokenCapacity, tokenCount, result.error)) {
        return result;
    }

    // `help`
    if (tokenEquals(tokens[0], "help")) {
        if (tokenCount != 1U) {
            result.error = CommandError::WrongTokenCount;
            return result;
        }
        result.ok = true;
        result.request.id = OperatorCommandId::Help;
        return result;
    }

    if (tokenEquals(tokens[0], "status") || tokenEquals(tokens[0], "version") ||
        tokenEquals(tokens[0], "checks") || tokenEquals(tokens[0], "boot") ||
        tokenEquals(tokens[0], "plan") || tokenEquals(tokens[0], "announce") ||
        tokenEquals(tokens[0], "arm") || tokenEquals(tokens[0], "disarm") ||
        tokenEquals(tokens[0], "reset")) {
        if (tokenCount != 1U) {
            result.error = CommandError::WrongTokenCount;
            return result;
        }
        result.ok = true;
        result.request.id = tokenEquals(tokens[0], "status")     ? OperatorCommandId::Status
                            : tokenEquals(tokens[0], "version")  ? OperatorCommandId::Version
                            : tokenEquals(tokens[0], "checks")   ? OperatorCommandId::Checks
                            : tokenEquals(tokens[0], "boot")     ? OperatorCommandId::Boot
                            : tokenEquals(tokens[0], "plan")     ? OperatorCommandId::Plan
                            : tokenEquals(tokens[0], "announce") ? OperatorCommandId::Announce
                            : tokenEquals(tokens[0], "arm")      ? OperatorCommandId::Arm
                            : tokenEquals(tokens[0], "disarm")   ? OperatorCommandId::Disarm
                                                                 : OperatorCommandId::Reset;
        return result;
    }

    // `config <subcommand> [fields...]`
    if (tokenEquals(tokens[0], "config")) {
        if (tokenCount < 2U) {
            result.error = CommandError::UnknownSubcommand;
            return result;
        }
        if (tokenEquals(tokens[1], "show") || tokenEquals(tokens[1], "check") ||
            tokenEquals(tokens[1], "clear")) {
            if (tokenCount != 2U) {
                result.error = CommandError::WrongTokenCount;
                return result;
            }
            result.ok = true;
            result.request.id = tokenEquals(tokens[1], "show")   ? OperatorCommandId::ConfigShow
                                : tokenEquals(tokens[1], "check") ? OperatorCommandId::ConfigCheck
                                                                  : OperatorCommandId::ConfigClear;
            return result;
        }
        if (!tokenEquals(tokens[1], "set")) {
            result.error = CommandError::UnknownSubcommand;
            return result;
        }
        if (tokenCount < 3U) {
            result.error = CommandError::MissingArgument;
            return result;
        }

        RunConfigFields fields{};
        for (uint16_t index = 2U; index < tokenCount; ++index) {
            Token key;
            Token value;
            if (!splitArgument(tokens[index], key, value)) {
                result.error = CommandError::MalformedArgument;
                return result;
            }
            if (tokenEquals(key, "level")) {
                if (fields.hasLevel) {
                    result.error = CommandError::DuplicateKey;
                    return result;
                }
                // The level is a name, not a number, so it is matched exactly.
                char name[16];
                if (value.length == 0U || value.length >= sizeof(name)) {
                    result.error = CommandError::UnknownLevel;
                    return result;
                }
                for (uint16_t position = 0U; position < value.length; ++position) {
                    name[position] = value.text[position];
                }
                name[value.length] = '\0';
                uint8_t levelIndex = 0U;
                if (!tournamentLevelIndexFromName(name, levelIndex)) {
                    result.error = CommandError::UnknownLevel;
                    return result;
                }
                fields.hasLevel = true;
                fields.levelIndex = levelIndex;
                continue;
            }

            // The three numeric fields.
            const bool isDistance = tokenEquals(key, "distance");
            const bool isTime = tokenEquals(key, "time");
            const bool isBottle = tokenEquals(key, "bottle_line");
            if (!isDistance && !isTime && !isBottle) {
                result.error = CommandError::UnknownKey;
                return result;
            }
            if ((isDistance && fields.hasTargetDistanceM) || (isTime && fields.hasTargetTimeS) ||
                (isBottle && fields.hasBottleLineDistanceM)) {
                result.error = CommandError::DuplicateKey;
                return result;
            }
            double number = 0.0;
            const CommandError numberError = parseUnsignedDecimal(value, number);
            if (numberError != CommandError::None) {
                result.error = numberError;
                return result;
            }
            if (isDistance) {
                fields.hasTargetDistanceM = true;
                fields.targetDistanceM = number;
            } else if (isTime) {
                fields.hasTargetTimeS = true;
                fields.targetTimeS = number;
            } else {
                fields.hasBottleLineDistanceM = true;
                fields.bottleLineDistanceM = number;
            }
        }

        result.ok = true;
        result.request.id = OperatorCommandId::ConfigSet;
        result.request.fields = fields;
        return result;
    }

    // `telemetry <subcommand> [rate]`
    if (tokenEquals(tokens[0], "telemetry")) {
        if (tokenCount < 2U) {
            result.error = CommandError::UnknownSubcommand;
            return result;
        }
        if (tokenEquals(tokens[1], "on") || tokenEquals(tokens[1], "off") ||
            tokenEquals(tokens[1], "status")) {
            if (tokenCount != 2U) {
                result.error = CommandError::WrongTokenCount;
                return result;
            }
            result.ok = true;
            result.request.id = tokenEquals(tokens[1], "on")     ? OperatorCommandId::TelemetryOn
                                : tokenEquals(tokens[1], "off")  ? OperatorCommandId::TelemetryOff
                                                                : OperatorCommandId::TelemetryStatus;
            return result;
        }
        if (!tokenEquals(tokens[1], "rate")) {
            result.error = CommandError::UnknownSubcommand;
            return result;
        }
        if (tokenCount != 3U) {
            result.error = CommandError::WrongTokenCount;
            return result;
        }
        double rate = 0.0;
        const CommandError rateError = parseUnsignedDecimal(tokens[2], rate);
        if (rateError != CommandError::None) {
            result.error = rateError;
            return result;
        }
        result.ok = true;
        result.request.id = OperatorCommandId::TelemetryRate;
        result.request.telemetryRateHz = rate;
        return result;
    }

    result.error = CommandError::UnknownCommand;
    return result;
}

}  // namespace ev
