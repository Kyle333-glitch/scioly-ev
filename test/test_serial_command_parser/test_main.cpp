#include <cmath>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include <SerialCommandParser.h>

// Contract tests for the operator command parser and its line reader
// (lib/evcore/src/SerialCommandParser.h): the accepted vocabulary, every named
// rejection, the strict whitespace rules, the numeric grammar, the bounds, and
// the property that a partial or rejected line is never executed.
//
// A pass means a typed line is either understood exactly or refused by name. It
// says nothing about what a command does: the state restrictions live in
// `OperatorPolicy.h` and the effects live in the vehicle layer.

void setUp(void) {}
void tearDown(void) {}

namespace {

// A line buffer sized for the reader under test, plus a little past the bound so
// an overlong line can be fed deliberately.
const uint16_t kTestLineCapacity = ev::kCommandLineCapacity;

// Unity's double assertions are compiled out of this test build, so a parsed
// value is compared through its difference. The tolerance is far below any value
// the grammar can distinguish and far above the rounding of a double.
void expectValue(double expected, double actual) {
    TEST_ASSERT_TRUE(fabs(expected - actual) <= 1.0e-9);
}

void expectCommand(const char* line, ev::OperatorCommandId expected) {
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));
    TEST_ASSERT_TRUE_MESSAGE(result.ok, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(result.request.id),
                                  line);
}

void expectError(const char* line, ev::CommandError expected) {
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));
    TEST_ASSERT_FALSE_MESSAGE(result.ok, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(result.error), line);
    // A rejected line names no command at all: nothing downstream can act on a
    // rejection that still carried a request.
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(ev::OperatorCommandId::None),
                                  static_cast<int>(result.request.id), line);
}

}  // namespace

// ---------------------------------------------------------------------------
// The line reader
// ---------------------------------------------------------------------------

// A line arrives one byte at a time and is reported once, complete. No byte
// before the terminator is ever reported as a line.
void test_reader_reports_a_complete_line_once(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    const char* text = "status";
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                              static_cast<int>(reader.push(*cursor)));
    }
    // The content is already readable, but no line is "ready" until the
    // terminator: a caller that only acts on LineReady cannot act on a prefix.
    TEST_ASSERT_EQUAL_STRING("status", reader.line());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("status", reader.line());
    TEST_ASSERT_EQUAL_UINT16(6U, reader.lineLength());

    // A byte after a completed line starts the next one rather than extending it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                          static_cast<int>(reader.push('a')));
    TEST_ASSERT_EQUAL_STRING("a", reader.line());
}

// Whitespace, tabs and non-printable bytes never survive to the parser, so a
// command is executed only when the bytes arrived as the operator sent them.
void test_reader_discards_a_line_with_a_non_printable_byte(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                          static_cast<int>(reader.push('s')));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                          static_cast<int>(reader.push('\t')));
    // The tab discarded the partial line silently, and the rest of it is dropped
    // too: nothing of this line can be executed.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                          static_cast<int>(reader.push('t')));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::InvalidCharacter),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("", reader.line());
    TEST_ASSERT_EQUAL_UINT32(1U, reader.rejectedLines());

    // The reader is ready again: the next line is read normally.
    const char* text = "status";
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        reader.push(*cursor);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                          static_cast<int>(reader.push('\r')));
    TEST_ASSERT_EQUAL_STRING("status", reader.line());
    TEST_ASSERT_EQUAL_UINT32(1U, reader.rejectedLines());
}

// An overlong line is discarded whole. The alternative - keeping the first
// `capacity` characters - would turn a mistyped line into a valid command.
void test_reader_discards_an_overlong_line_whole(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    for (uint16_t index = 0U; index < kTestLineCapacity; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                              static_cast<int>(reader.push('x')));
    }
    // The line is full; the next content byte starts the discard.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                          static_cast<int>(reader.push('y')));
    // The discard continues to the terminator without accumulating anything.
    for (uint16_t index = 0U; index < 50U; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::None),
                              static_cast<int>(reader.push('z')));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineTooLong),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("", reader.line());
    TEST_ASSERT_EQUAL_UINT32(1U, reader.rejectedLines());

    // A legitimate command after the discarded line works.
    const char* text = "reset";
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        reader.push(*cursor);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("reset", reader.line());
}

// A terminator with nothing before it is reported as an empty line and yields no
// command. The `\n` of a `\r\n` pair is the common case, not an error.
void test_reader_reports_empty_lines_separately(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::EmptyLine),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("", reader.line());

    // The `\n` of a `\r\n` pair is the common case, not an error: the line was
    // already reported at the `\r`, and the second terminator is an empty line.
    reader.push('a');
    reader.push('r');
    reader.push('m');
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                          static_cast<int>(reader.push('\r')));
    TEST_ASSERT_EQUAL_STRING("arm", reader.line());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::EmptyLine),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_STRING("", reader.line());
    // Neither terminator is a rejection: an empty line carries no command to
    // reject, and the operator's line was understood.
    TEST_ASSERT_EQUAL_UINT32(0U, reader.rejectedLines());
}

// Nothing that was buffered survives a reset, and a reset is not a rejection: it
// is the caller's own boundary (a boot, a mode change), not the operator's input.
void test_reader_reset_drops_buffered_content_without_counting_it(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    reader.push('c');
    reader.push('o');
    reader.reset();
    TEST_ASSERT_EQUAL_STRING("", reader.line());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::EmptyLine),
                          static_cast<int>(reader.push('\n')));
    TEST_ASSERT_EQUAL_UINT32(0U, reader.rejectedLines());
}

// ---------------------------------------------------------------------------
// The accepted vocabulary
// ---------------------------------------------------------------------------

// Every command spelling the interface documents parses to exactly one command.
void test_every_command_spelling_parses(void) {
    expectCommand("help", ev::OperatorCommandId::Help);
    expectCommand("status", ev::OperatorCommandId::Status);
    expectCommand("version", ev::OperatorCommandId::Version);
    expectCommand("checks", ev::OperatorCommandId::Checks);
    expectCommand("boot", ev::OperatorCommandId::Boot);
    expectCommand("config show", ev::OperatorCommandId::ConfigShow);
    expectCommand("config check", ev::OperatorCommandId::ConfigCheck);
    expectCommand("config clear", ev::OperatorCommandId::ConfigClear);
    expectCommand("plan", ev::OperatorCommandId::Plan);
    expectCommand("announce", ev::OperatorCommandId::Announce);
    expectCommand("arm", ev::OperatorCommandId::Arm);
    expectCommand("disarm", ev::OperatorCommandId::Disarm);
    expectCommand("reset", ev::OperatorCommandId::Reset);
    expectCommand("telemetry on", ev::OperatorCommandId::TelemetryOn);
    expectCommand("telemetry off", ev::OperatorCommandId::TelemetryOff);
    expectCommand("telemetry status", ev::OperatorCommandId::TelemetryStatus);
}

// Every field of a complete configuration, with the values the event publishes.
void test_config_set_parses_every_field(void) {
    const char* line = "config set level=national distance=7.25 time=12.5 bottle_line=2.00";
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));

    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OperatorCommandId::ConfigSet),
                          static_cast<int>(result.request.id));
    TEST_ASSERT_TRUE(result.request.fields.hasLevel);
    TEST_ASSERT_EQUAL_UINT8(2U, result.request.fields.levelIndex);
    TEST_ASSERT_TRUE(result.request.fields.hasTargetDistanceM);
    expectValue(7.25, result.request.fields.targetDistanceM);
    TEST_ASSERT_TRUE(result.request.fields.hasTargetTimeS);
    expectValue(12.5, result.request.fields.targetTimeS);
    TEST_ASSERT_TRUE(result.request.fields.hasBottleLineDistanceM);
    expectValue(2.0, result.request.fields.bottleLineDistanceM);
    TEST_ASSERT_TRUE(result.request.fields.complete());
}

// A partial `config set` is legitimate: it supplies the fields the operator
// named and marks the rest absent, so the accumulated configuration is one line
// away from being complete rather than a whole retype.
void test_config_set_accepts_a_subset(void) {
    const char* line = "config set distance=6";
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));

    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_FALSE(result.request.fields.complete());
    TEST_ASSERT_TRUE(result.request.fields.hasTargetDistanceM);
    expectValue(6.0, result.request.fields.targetDistanceM);
    TEST_ASSERT_FALSE(result.request.fields.hasLevel);
    TEST_ASSERT_FALSE(result.request.fields.hasTargetTimeS);
    TEST_ASSERT_FALSE(result.request.fields.hasBottleLineDistanceM);

    const char* levelOnly = "config set level=regional";
    const ev::CommandParseResult levelResult =
        ev::parseOperatorCommand(levelOnly, static_cast<uint16_t>(strlen(levelOnly)));
    TEST_ASSERT_TRUE(levelResult.ok);
    TEST_ASSERT_TRUE(levelResult.request.fields.hasLevel);
    TEST_ASSERT_EQUAL_UINT8(0U, levelResult.request.fields.levelIndex);
    TEST_ASSERT_FALSE(levelResult.request.fields.hasTargetDistanceM);
}

// The telemetry rate is the one numeric argument that is not a configuration
// field, and it travels on the request rather than in the field set.
void test_telemetry_rate_parses_a_number(void) {
    const char* line = "telemetry rate 10.5";
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));

    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OperatorCommandId::TelemetryRate),
                          static_cast<int>(result.request.id));
    expectValue(10.5, result.request.telemetryRateHz);
    // The field set is meaningless for this command and stays untouched, so no
    // caller can read a stale configuration out of it.
    TEST_ASSERT_FALSE(result.request.fields.complete());
    TEST_ASSERT_FALSE(result.request.fields.hasLevel);
}

// ---------------------------------------------------------------------------
// The rejections
// ---------------------------------------------------------------------------

// Whitespace is strict on purpose: the line the operator typed and the line the
// firmware executed must not differ by invisible bytes.
void test_whitespace_is_rejected_rather_than_normalized(void) {
    expectError("", ev::CommandError::EmptyCommand);
    expectError(" status", ev::CommandError::EmptyToken);
    expectError("status ", ev::CommandError::EmptyToken);
    expectError("config  show", ev::CommandError::EmptyToken);
    expectError("config set  distance=5", ev::CommandError::EmptyToken);
    expectError("config show\t", ev::CommandError::InvalidCharacter);
}

// A command or subcommand that does not exist is refused, never approximated.
void test_unknown_commands_and_subcommands_are_named(void) {
    expectError("frobnicate", ev::CommandError::UnknownCommand);
    expectError("Status", ev::CommandError::UnknownCommand);
    expectError("config", ev::CommandError::UnknownSubcommand);
    expectError("config frobnicate", ev::CommandError::UnknownSubcommand);
    expectError("telemetry", ev::CommandError::UnknownSubcommand);
    expectError("telemetry frobnicate", ev::CommandError::UnknownSubcommand);
    // A truncated command is not a command.
    expectError("config se", ev::CommandError::UnknownSubcommand);
    expectError("tele", ev::CommandError::UnknownCommand);
}

// S21: the diagnostic spellings from the bench vocabulary are not production
// commands, and they are refused by name rather than silently ignored.
//
// This is the offline half of the S21 serial matrix: `health` and `ticks` are
// `bench`-image verbs (`lib/evcore/src/BenchCommand.h`, `src/bench/BenchConsole.h`),
// and the production grammar below has never contained them. A host that types
// them at the vehicle image gets the same named rejection as any other unknown
// word - the firmware does not go quiet.
//
// What a pass does **not** prove: that a board on the bench was running this
// image, that a host's bytes reached it, or that a reply left the port. Those are
// wiring and session facts (`docs/bringup/S21-hardware-return-serial-runbook.md`).
void test_diagnostic_verbs_are_not_production_commands(void) {
    expectError("health", ev::CommandError::UnknownCommand);
    expectError("ticks", ev::CommandError::UnknownCommand);
    expectError("Health", ev::CommandError::UnknownCommand);
    // With an argument, or with the bench prefix: the first token is unknown, so
    // the whole line is one rejection and no prefix of it is executed.
    expectError("health status", ev::CommandError::UnknownCommand);
    expectError("ticks 1", ev::CommandError::UnknownCommand);
    expectError("bench", ev::CommandError::UnknownCommand);
    expectError("bench health", ev::CommandError::UnknownCommand);
    expectError("bench ticks", ev::CommandError::UnknownCommand);
    // The neighbouring real commands still parse, so the refusals above are the
    // grammar's decision and not a parser that rejects everything.
    expectCommand("status", ev::OperatorCommandId::Status);
    expectCommand("telemetry status", ev::OperatorCommandId::TelemetryStatus);
    expectCommand("reset", ev::OperatorCommandId::Reset);
}

// The right command with the wrong shape of arguments: counted, not guessed.
void test_wrong_token_counts_are_rejected(void) {
    expectError("status extra", ev::CommandError::WrongTokenCount);
    expectError("boot now", ev::CommandError::WrongTokenCount);
    expectError("help me", ev::CommandError::WrongTokenCount);
    expectError("arm on", ev::CommandError::WrongTokenCount);
    expectError("config show extra", ev::CommandError::WrongTokenCount);
    expectError("config clear extra", ev::CommandError::WrongTokenCount);
    expectError("telemetry rate", ev::CommandError::WrongTokenCount);
    expectError("telemetry rate 5 extra", ev::CommandError::WrongTokenCount);
    expectError("telemetry on extra", ev::CommandError::WrongTokenCount);
    // `plan` takes nothing: it reads the plan the run layer already holds, so an
    // argument would name a run the firmware is not running.
    expectError("plan extra", ev::CommandError::WrongTokenCount);
    expectError("config set", ev::CommandError::MissingArgument);
}

// Arguments are `key=value`, keys are from a fixed set, and a key may appear once.
void test_argument_shape_keys_and_duplicates_are_rejected(void) {
    expectError("config set distance", ev::CommandError::MalformedArgument);
    expectError("config set =5", ev::CommandError::MalformedArgument);
    expectError("config set speed=5", ev::CommandError::UnknownKey);
    expectError("config set level=state distance=5 level=state",
                ev::CommandError::DuplicateKey);
    expectError("config set distance=5 distance=6", ev::CommandError::DuplicateKey);
    expectError("config set time=12 time=13", ev::CommandError::DuplicateKey);
    expectError("config set bottle_line=2 bottle_line=3", ev::CommandError::DuplicateKey);
}

// The numeric grammar is the firmware's own: digits with one optional dot, no
// sign, no exponent, no unit suffix, no locale.
void test_the_numeric_grammar_rejects_everything_else(void) {
    expectError("config set distance=abc", ev::CommandError::InvalidValue);
    expectError("config set distance=", ev::CommandError::InvalidValue);
    expectError("config set distance=.", ev::CommandError::InvalidValue);
    expectError("config set distance=7.1.2", ev::CommandError::InvalidValue);
    // A trailing dot is unambiguous (seven) and is accepted; what is rejected is
    // punctuation with no number in it at all.
    expectCommand("config set distance=7.", ev::OperatorCommandId::ConfigSet);
    expectError("config set distance=-5", ev::CommandError::InvalidValue);
    expectError("config set distance=+5", ev::CommandError::InvalidValue);
    expectError("config set distance=1e3", ev::CommandError::InvalidValue);
    expectError("config set distance=7.1m", ev::CommandError::InvalidValue);
    expectError("config set distance=7,5", ev::CommandError::InvalidValue);
    // Ten fractional digits is one more than the grammar accepts.
    expectError("config set distance=7.1234567890", ev::CommandError::InvalidValue);
    expectError("telemetry rate abc", ev::CommandError::InvalidValue);
    expectError("telemetry rate -5", ev::CommandError::InvalidValue);
}

// A non-finite spelling is reported as what it is, and a magnitude beyond the
// representability bound is reported separately: the operator learns which
// mistake was made.
void test_non_finite_and_out_of_range_values_are_distinguished(void) {
    expectError("config set distance=nan", ev::CommandError::ValueNotFinite);
    expectError("config set distance=inf", ev::CommandError::ValueNotFinite);
    expectError("config set distance=+infinity", ev::CommandError::ValueNotFinite);
    expectError("config set distance=1000001", ev::CommandError::ValueOutOfRange);
    expectError("telemetry rate 1000001", ev::CommandError::ValueOutOfRange);

    // The bound itself is representable, and nine fractional digits are accepted.
    expectCommand("config set distance=1000000", ev::OperatorCommandId::ConfigSet);
    const char* line = "config set distance=1000000.000000001";
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(line, static_cast<uint16_t>(strlen(line)));
    TEST_ASSERT_TRUE(result.ok);
    // Nine fractional digits on a seven-digit integer part: exact to the grammar,
    // and the same double the target machine computes (integer part plus one
    // division by a power of ten). The tolerance is the spacing of doubles at this
    // magnitude, not a claim about the parser's precision.
    TEST_ASSERT_TRUE(fabs(1000000.000000001 - result.request.fields.targetDistanceM) <= 1.0e-6);
}

// A tournament level is matched exactly: the published spelling, no case folding
// and no abbreviations.
void test_tournament_levels_are_exact(void) {
    expectError("config set level=State", ev::CommandError::UnknownLevel);
    expectError("config set level=STATE", ev::CommandError::UnknownLevel);
    expectError("config set level=reg", ev::CommandError::UnknownLevel);
    expectError("config set level=", ev::CommandError::UnknownLevel);
    expectError("config set level=provincial", ev::CommandError::UnknownLevel);

    expectCommand("config set level=regional", ev::OperatorCommandId::ConfigSet);
    expectCommand("config set level=state", ev::OperatorCommandId::ConfigSet);
    expectCommand("config set level=national", ev::OperatorCommandId::ConfigSet);
}

// The bounds are checked before anything is interpreted: an overlong line and too
// many tokens are their own named rejections, not a partial parse.
void test_the_line_and_token_bounds_are_enforced(void) {
    char overlong[ev::kCommandLineCapacity + 2U];
    for (uint16_t index = 0U; index < ev::kCommandLineCapacity; ++index) {
        overlong[index] = 'x';
    }
    overlong[ev::kCommandLineCapacity] = '\n';
    overlong[ev::kCommandLineCapacity + 1U] = '\0';
    const ev::CommandParseResult longResult =
        ev::parseOperatorCommand(overlong, ev::kCommandLineCapacity + 1U);
    TEST_ASSERT_FALSE(longResult.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::CommandError::LineTooLong),
                          static_cast<int>(longResult.error));

    // A line of exactly the capacity with a rejected first token is still read to
    // its end rather than cut: the content is what was sent.
    char atCapacity[ev::kCommandLineCapacity + 1U];
    for (uint16_t index = 0U; index < ev::kCommandLineCapacity; ++index) {
        atCapacity[index] = 'y';
    }
    atCapacity[ev::kCommandLineCapacity] = '\0';
    const ev::CommandParseResult atCapacityResult =
        ev::parseOperatorCommand(atCapacity, ev::kCommandLineCapacity);
    TEST_ASSERT_FALSE(atCapacityResult.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::CommandError::UnknownCommand),
                          static_cast<int>(atCapacityResult.error));

    // Nine tokens is one more than the grammar accepts.
    expectError("config set a=1 b=2 c=3 d=4 e=5 f=6 g=7", ev::CommandError::TooManyTokens);
}

// The parser reads no byte past the length it was given, which is what makes it
// safe to hand it a fixed buffer that holds more than one line.
void test_parser_never_reads_past_its_length(void) {
    const char line[] = {'s', 't', 'a', 't', 'u', 's', 'Z', 'Z', 'Z'};
    const ev::CommandParseResult result = ev::parseOperatorCommand(line, 6U);

    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OperatorCommandId::Status),
                          static_cast<int>(result.request.id));

    // A null line is a rejection, not a crash.
    const ev::CommandParseResult nullResult = ev::parseOperatorCommand(nullptr, 0U);
    TEST_ASSERT_FALSE(nullResult.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::CommandError::EmptyCommand),
                          static_cast<int>(nullResult.error));
}

// The parser is stateless: the same line parses identically however many times it
// arrives, and one command's fields never leak into the next command's request.
void test_the_parser_is_stateless_across_commands(void) {
    const char* setLine = "config set distance=7";
    const ev::CommandParseResult first =
        ev::parseOperatorCommand(setLine, static_cast<uint16_t>(strlen(setLine)));
    const ev::CommandParseResult second =
        ev::parseOperatorCommand(setLine, static_cast<uint16_t>(strlen(setLine)));

    TEST_ASSERT_TRUE(first.ok);
    TEST_ASSERT_TRUE(second.ok);
    TEST_ASSERT_TRUE(second.request.fields.hasTargetDistanceM);
    expectValue(first.request.fields.targetDistanceM, second.request.fields.targetDistanceM);

    const char* statusLine = "status";
    const ev::CommandParseResult status =
        ev::parseOperatorCommand(statusLine, static_cast<uint16_t>(strlen(statusLine)));
    TEST_ASSERT_TRUE(status.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OperatorCommandId::Status),
                          static_cast<int>(status.request.id));
    TEST_ASSERT_FALSE(status.request.fields.hasTargetDistanceM);
    TEST_ASSERT_FALSE(status.request.fields.complete());

    // A rejected line leaves nothing behind either.
    const char* badLine = "config set distance=-1";
    TEST_ASSERT_FALSE(ev::parseOperatorCommand(badLine, static_cast<uint16_t>(strlen(badLine))).ok);
    TEST_ASSERT_TRUE(ev::parseOperatorCommand(statusLine, 6U).ok);
}

// The reader and the parser compose the way the console uses them: bytes in,
// either a typed command or a named rejection, and never a half-executed command.
void test_reader_and_parser_compose(void) {
    char buffer[kTestLineCapacity + 1U];
    ev::CommandLineReader reader(buffer, kTestLineCapacity);

    const char* text = "telemetry rate 10";
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        reader.push(*cursor);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                          static_cast<int>(reader.push('\n')));
    const ev::CommandParseResult result =
        ev::parseOperatorCommand(reader.line(), reader.lineLength());
    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::OperatorCommandId::TelemetryRate),
                          static_cast<int>(result.request.id));
    expectValue(10.0, result.request.telemetryRateHz);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_reader_reports_a_complete_line_once);
    RUN_TEST(test_reader_discards_a_line_with_a_non_printable_byte);
    RUN_TEST(test_reader_discards_an_overlong_line_whole);
    RUN_TEST(test_reader_reports_empty_lines_separately);
    RUN_TEST(test_reader_reset_drops_buffered_content_without_counting_it);
    RUN_TEST(test_every_command_spelling_parses);
    RUN_TEST(test_config_set_parses_every_field);
    RUN_TEST(test_config_set_accepts_a_subset);
    RUN_TEST(test_telemetry_rate_parses_a_number);
    RUN_TEST(test_whitespace_is_rejected_rather_than_normalized);
    RUN_TEST(test_unknown_commands_and_subcommands_are_named);
    RUN_TEST(test_diagnostic_verbs_are_not_production_commands);
    RUN_TEST(test_wrong_token_counts_are_rejected);
    RUN_TEST(test_argument_shape_keys_and_duplicates_are_rejected);
    RUN_TEST(test_the_numeric_grammar_rejects_everything_else);
    RUN_TEST(test_non_finite_and_out_of_range_values_are_distinguished);
    RUN_TEST(test_tournament_levels_are_exact);
    RUN_TEST(test_the_line_and_token_bounds_are_enforced);
    RUN_TEST(test_parser_never_reads_past_its_length);
    RUN_TEST(test_the_parser_is_stateless_across_commands);
    RUN_TEST(test_reader_and_parser_compose);
    return UNITY_END();
}
