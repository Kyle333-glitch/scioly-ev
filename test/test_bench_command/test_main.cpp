#include <cstdint>
#include <cstring>

#include <unity.h>

#include <BenchCommand.h>

// Contract tests for the bench console's line grammar
// (lib/evcore/src/BenchCommand.h): tokenizing without allocating, recognizing the
// verb, and parsing `bench drive <fwd|rev> <permille> <ms>`.
//
// This exists because the console that owns the serial port (`src/bench/`) cannot
// be host-tested, and an off-by-one token index there is invisible until a bench
// session refuses a command that looks correct. The first version of that console
// had exactly that bug; these tests are why it cannot come back.
//
// The tokenizer is fed buffers whose lifetime matches the console's: it keeps
// pointers into the caller's buffer and copies nothing, so each test keeps its line
// alive for the assertions that read the tokens.

void setUp(void) {}
void tearDown(void) {}

namespace {

// Splits a NUL-terminated line, as the reader would hand it over: the terminator is
// already gone, and `length` excludes it.
ev::BenchTokens split(const char* line) {
    return ev::splitBenchTokens(line, static_cast<uint16_t>(std::strlen(line)));
}

}  // namespace

// --- Tokenizing --------------------------------------------------------------

void test_tokens_are_split_on_spaces_and_tabs_without_copying(void) {
    static const char line[] = "bench   drive\tfwd 100 250";
    const ev::BenchTokens tokens = split(line);

    TEST_ASSERT_FALSE(tokens.overflow);
    TEST_ASSERT_EQUAL_UINT8(5U, tokens.count);
    TEST_ASSERT_EQUAL_STRING_LEN("bench", tokens.token(0), tokens.tokenLength(0));
    TEST_ASSERT_EQUAL_STRING_LEN("drive", tokens.token(1), tokens.tokenLength(1));
    TEST_ASSERT_EQUAL_STRING_LEN("fwd", tokens.token(2), tokens.tokenLength(2));
    TEST_ASSERT_EQUAL_STRING_LEN("100", tokens.token(3), tokens.tokenLength(3));
    TEST_ASSERT_EQUAL_STRING_LEN("250", tokens.token(4), tokens.tokenLength(4));
    // The tokens are views into the caller's buffer, not copies.
    TEST_ASSERT_TRUE(tokens.token(0) == line);
}

void test_an_empty_or_blank_line_has_no_tokens(void) {
    TEST_ASSERT_EQUAL_UINT8(0U, split("").count);
    TEST_ASSERT_EQUAL_UINT8(0U, split("   ").count);
    TEST_ASSERT_EQUAL_UINT8(0U, split("\t \t").count);
}

void test_trailing_and_leading_separators_do_not_add_tokens(void) {
    const ev::BenchTokens tokens = split("  bench  arm  ");
    TEST_ASSERT_EQUAL_UINT8(2U, tokens.count);
    TEST_ASSERT_EQUAL_STRING_LEN("bench", tokens.token(0), tokens.tokenLength(0));
    TEST_ASSERT_EQUAL_STRING_LEN("arm", tokens.token(1), tokens.tokenLength(1));
}

// More tokens than the grammar allows is reported, never quietly truncated: acting
// on the tokens that fit is how a command nobody typed gets applied.
void test_an_over_long_line_is_reported_rather_than_truncated(void) {
    const ev::BenchTokens tokens = split("bench drive fwd 100 250 extra tokens here");
    TEST_ASSERT_TRUE(tokens.overflow);
    TEST_ASSERT_EQUAL_UINT8(ev::kBenchTokenCapacity, tokens.count);
}

void test_token_access_past_the_end_is_empty_not_undefined(void) {
    const ev::BenchTokens tokens = split("bench");
    TEST_ASSERT_EQUAL_UINT8(1U, tokens.count);
    TEST_ASSERT_TRUE(tokens.token(1) == nullptr);
    TEST_ASSERT_EQUAL_UINT16(0U, tokens.tokenLength(1));
    TEST_ASSERT_TRUE(tokens.token(99) == nullptr);
}

// --- Verbs -------------------------------------------------------------------

void test_every_verb_is_recognized_by_its_whole_name(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Help), static_cast<int>(ev::benchVerbFor(split("bench"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Help), static_cast<int>(ev::benchVerbFor(split("bench help"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Arm), static_cast<int>(ev::benchVerbFor(split("bench arm"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Stop), static_cast<int>(ev::benchVerbFor(split("bench stop"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Reset), static_cast<int>(ev::benchVerbFor(split("bench reset"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Health), static_cast<int>(ev::benchVerbFor(split("bench health"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Ticks), static_cast<int>(ev::benchVerbFor(split("bench ticks"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Inputs), static_cast<int>(ev::benchVerbFor(split("bench inputs"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Inputs), static_cast<int>(ev::benchVerbFor(split("bench trigger"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Pins), static_cast<int>(ev::benchVerbFor(split("bench pins"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::ResetCause), static_cast<int>(ev::benchVerbFor(split("bench resetsrc"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Wdt), static_cast<int>(ev::benchVerbFor(split("bench wdt"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::DriveStatus), static_cast<int>(ev::benchVerbFor(split("bench drive-status"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Drive), static_cast<int>(ev::benchVerbFor(split("bench drive fwd 10 10"))));
}

// `drive` must not match `drive-status`, and a prefix of a verb is not a verb.
void test_a_prefix_of_a_verb_is_not_the_verb(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench dri"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench drive-stat"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench pin"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench wd"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench arming"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench ARM"))));
}

// `reset` (the drive latch) and `resetsrc` (the reset-status readout) share a
// prefix in both directions, so they are the case that shows the comparison is
// length-checked rather than a prefix match - and `resetsrc` must not be reachable
// by typing the shorter verb, or a session would read the wrong thing.
void test_the_reset_latch_and_the_reset_readout_do_not_alias(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Reset), static_cast<int>(ev::benchVerbFor(split("bench reset"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::ResetCause), static_cast<int>(ev::benchVerbFor(split("bench resetsrc"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench resets"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Unknown), static_cast<int>(ev::benchVerbFor(split("bench resetsrcx"))));
}

// A line that is not a bench line at all is `None`, which is what routes it to the
// production operator interface instead of being refused.
void test_a_non_bench_line_is_not_a_bench_verb(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::None), static_cast<int>(ev::benchVerbFor(split("status"))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::None), static_cast<int>(ev::benchVerbFor(split(""))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::None), static_cast<int>(ev::benchVerbFor(split("benches arm"))));
}

// --- Unsigned decimals -------------------------------------------------------

void test_decimals_are_plain_and_overflow_is_refused(void) {
    uint32_t value = 0U;
    TEST_ASSERT_TRUE(ev::parseUnsignedDecimal("0", 1U, value));
    TEST_ASSERT_EQUAL_UINT32(0U, value);
    TEST_ASSERT_TRUE(ev::parseUnsignedDecimal("1000", 4U, value));
    TEST_ASSERT_EQUAL_UINT32(1000U, value);
    TEST_ASSERT_TRUE(ev::parseUnsignedDecimal("4294967295", 10U, value));
    TEST_ASSERT_EQUAL_UINT32(4294967295U, value);

    // A value that would wrap into a small number is refused, not wrapped.
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("4294967296", 10U, value));
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("99999999999999", 14U, value));
    // Leading zeros are a typo nobody meant, and guessing which number it is is not
    // the parser's business.
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("07", 2U, value));
    // Anything that is not a digit.
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("-5", 2U, value));
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("5.0", 3U, value));
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("0x10", 4U, value));
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal("", 0U, value));
    TEST_ASSERT_FALSE(ev::parseUnsignedDecimal(nullptr, 3U, value));
}

// --- The drive request -------------------------------------------------------

void test_a_well_formed_drive_line_parses_to_the_request_as_typed(void) {
    const ev::BenchTokens forward = split("bench drive fwd 120 250");
    const ev::BenchDriveRequest forwardRequest = ev::parseBenchDriveRequest(forward);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::None),
                          static_cast<int>(forwardRequest.error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveForward),
                          static_cast<int>(forwardRequest.mode));
    TEST_ASSERT_EQUAL_UINT16(120U, forwardRequest.permille);
    TEST_ASSERT_EQUAL_UINT32(250U, forwardRequest.durationMs);

    const ev::BenchTokens reverse = split("bench drive rev 1000 1");
    const ev::BenchDriveRequest reverseRequest = ev::parseBenchDriveRequest(reverse);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::None),
                          static_cast<int>(reverseRequest.error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::DriveReverse),
                          static_cast<int>(reverseRequest.mode));
    TEST_ASSERT_EQUAL_UINT16(1000U, reverseRequest.permille);
    TEST_ASSERT_EQUAL_UINT32(1U, reverseRequest.durationMs);
}

void test_each_malformed_drive_line_is_refused_with_its_own_reason(void) {
    // The wrong verb, and every wrong token count.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::NotADriveLine),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench arm")).error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::NotADriveLine),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive")).error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::NotADriveLine),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd")).error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::NotADriveLine),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd 100")).error));
    // One token too many: the grammar is exact, because "close enough" here is a
    // command that is not the one that was typed.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::ExtraTokens),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd 100 250 now")).error));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::DirectionNotFwdOrRev),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive forward 100 250")).error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::EffortNotADecimal),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd ten 250")).error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::DurationNotADecimal),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd 100 soon")).error));
    // Full scale is the boundary of the *unit*; whether 1000 permille is within the
    // bench ceiling is `ev::BenchDrive`'s refusal to make.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::EffortAboveFullScale),
                          static_cast<int>(ev::parseBenchDriveRequest(split("bench drive fwd 1001 250")).error));
}

// A refused parse reports an error and no request, so a caller cannot accidentally
// act on a partially filled one.
void test_a_refused_parse_leaves_no_usable_request(void) {
    const ev::BenchDriveRequest request = ev::parseBenchDriveRequest(split("bench drive forward 100 250"));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::DirectionNotFwdOrRev),
                          static_cast<int>(request.error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::MotorMode::Disabled),
                          static_cast<int>(request.mode));
    TEST_ASSERT_EQUAL_UINT16(0U, request.permille);
    TEST_ASSERT_EQUAL_UINT32(0U, request.durationMs);
}

// Every reason has a name, and no reason names the "no error" spelling by mistake.
void test_every_argument_error_has_its_own_name(void) {
    const ev::BenchArgumentError all[] = {
        ev::BenchArgumentError::NotADriveLine,     ev::BenchArgumentError::ExtraTokens,
        ev::BenchArgumentError::DirectionNotFwdOrRev, ev::BenchArgumentError::EffortNotADecimal,
        ev::BenchArgumentError::DurationNotADecimal, ev::BenchArgumentError::EffortAboveFullScale,
    };
    for (uint8_t index = 0U; index < 6U; ++index) {
        const char* name = ev::benchArgumentErrorName(all[index]);
        TEST_ASSERT_TRUE(name != nullptr);
        TEST_ASSERT_TRUE(std::strlen(name) > 0U);
        TEST_ASSERT_TRUE(std::strcmp(name, "?") != 0);
    }
}

// --- M3: the exact token bound of every verb ----------------------------------

// A no-argument verb accepts exactly `bench <verb>` and nothing more. The console
// refuses a longer line from this table before dispatch, which is what stops
// `bench arm <junk>` from arming and `bench stop <junk>` from stopping.
void test_each_verb_has_its_exact_token_bound(void) {
    TEST_ASSERT_EQUAL_UINT8(2U, ev::benchVerbMaximumTokenCount(ev::BenchVerb::Help));
    TEST_ASSERT_EQUAL_UINT8(5U, ev::benchVerbMaximumTokenCount(ev::BenchVerb::Drive));
    TEST_ASSERT_EQUAL_UINT8(3U, ev::benchVerbMaximumTokenCount(ev::BenchVerb::ResetCause));

    const ev::BenchVerb noArgumentVerbs[] = {
        ev::BenchVerb::Arm,   ev::BenchVerb::Stop,         ev::BenchVerb::Reset,
        ev::BenchVerb::Health, ev::BenchVerb::DriveStatus, ev::BenchVerb::Ticks,
        ev::BenchVerb::Inputs, ev::BenchVerb::Pins,        ev::BenchVerb::Wdt,
    };
    for (uint8_t index = 0U; index < 9U; ++index) {
        TEST_ASSERT_EQUAL_UINT8(2U, ev::benchVerbMaximumTokenCount(noArgumentVerbs[index]));
    }

    // The two that are never dispatched as a bench command accept nothing.
    TEST_ASSERT_EQUAL_UINT8(0U, ev::benchVerbMaximumTokenCount(ev::BenchVerb::None));
    TEST_ASSERT_EQUAL_UINT8(0U, ev::benchVerbMaximumTokenCount(ev::BenchVerb::Unknown));
}

// The bound is judged against the actual split line: a verb whose tokens fit
// inside the tokenizer's capacity still has a bound, and a well-formed line is
// inside it.
void test_the_bound_is_measured_against_the_split_line(void) {
    static const char malformed[] = "bench arm trailing-argument";
    const ev::BenchTokens tokens = split(malformed);
    const ev::BenchVerb verb = ev::benchVerbFor(tokens);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Arm), static_cast<int>(verb));
    TEST_ASSERT_FALSE(tokens.overflow);
    TEST_ASSERT_EQUAL_UINT8(3U, tokens.count);
    TEST_ASSERT_TRUE(tokens.count > ev::benchVerbMaximumTokenCount(verb));

    static const char wellFormed[] = "bench arm";
    const ev::BenchTokens ok = split(wellFormed);
    TEST_ASSERT_TRUE(ok.count <= ev::benchVerbMaximumTokenCount(ev::benchVerbFor(ok)));
}

// The drive grammar's bound agrees with its own parser: an over-long drive line is
// refused by both, and the parser still names the specific reason.
void test_the_drive_bound_agrees_with_the_parser(void) {
    static const char tooLong[] = "bench drive fwd 100 250 extra";
    const ev::BenchTokens tokens = split(tooLong);
    const ev::BenchVerb verb = ev::benchVerbFor(tokens);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchVerb::Drive), static_cast<int>(verb));
    TEST_ASSERT_TRUE(tokens.count > ev::benchVerbMaximumTokenCount(verb));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BenchArgumentError::ExtraTokens),
                          static_cast<int>(ev::parseBenchDriveRequest(tokens).error));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tokens_are_split_on_spaces_and_tabs_without_copying);
    RUN_TEST(test_an_empty_or_blank_line_has_no_tokens);
    RUN_TEST(test_trailing_and_leading_separators_do_not_add_tokens);
    RUN_TEST(test_an_over_long_line_is_reported_rather_than_truncated);
    RUN_TEST(test_token_access_past_the_end_is_empty_not_undefined);
    RUN_TEST(test_every_verb_is_recognized_by_its_whole_name);
    RUN_TEST(test_a_prefix_of_a_verb_is_not_the_verb);
    RUN_TEST(test_the_reset_latch_and_the_reset_readout_do_not_alias);
    RUN_TEST(test_a_non_bench_line_is_not_a_bench_verb);
    RUN_TEST(test_decimals_are_plain_and_overflow_is_refused);
    RUN_TEST(test_a_well_formed_drive_line_parses_to_the_request_as_typed);
    RUN_TEST(test_each_malformed_drive_line_is_refused_with_its_own_reason);
    RUN_TEST(test_a_refused_parse_leaves_no_usable_request);
    RUN_TEST(test_every_argument_error_has_its_own_name);
    RUN_TEST(test_each_verb_has_its_exact_token_bound);
    RUN_TEST(test_the_bound_is_measured_against_the_split_line);
    RUN_TEST(test_the_drive_bound_agrees_with_the_parser);
    return UNITY_END();
}
