#include <cstdint>
#include <cstring>

#include <unity.h>

#include <FirmwareIdentity.h>
#include <VehicleCalibration.h>

// R3/L3: reproducible firmware build identity.
//
// The constant firmware string could not distinguish source revisions,
// image variants, dirty trees or calibration configurations. These tests pin
// the replacement contract: exact rendering, variant vocabulary,
// clean/dirty/unknown handling, deterministic fingerprinting, sensitivity to
// changed configuration, and truncation safety. They use disposable probe
// identities throughout - no test modifies real version control or build
// state.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::FirmwareIdentity probe(const char* rev, bool dirty, ev::FirmwareVariant variant,
                           uint32_t fingerprint) {
    ev::FirmwareIdentity identity;
    identity.family = ev::kFirmwareFamily;
    identity.revision = rev;
    identity.dirty = dirty;
    identity.variant = variant;
    identity.configFingerprint = fingerprint;
    identity.configSchema = ev::kFirmwareConfigSchema;
    return identity;
}

uint16_t render(const ev::FirmwareIdentity& identity, char* buffer, uint16_t capacity) {
    return ev::formatFirmwareIdentity(identity, buffer, capacity);
}

}  // namespace

// --- Exact rendering -------------------------------------------------------

void test_identity_renders_the_full_fragment(void) {
    char buffer[256];
    const ev::FirmwareIdentity identity = probe("d5b2651", false, ev::FirmwareVariant::Bench,
                                                0x9F3AC2E1U);
    TEST_ASSERT_TRUE(render(identity, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_EQUAL_STRING(
        "fw=ev-nano-r4,rev=d5b2651,dirty=0,variant=bench,cfg=9f3ac2e1,cfg_schema=1", buffer);
}

void test_variant_names_cover_all_three_images(void) {
    TEST_ASSERT_EQUAL_STRING("production",
                             ev::firmwareVariantName(ev::FirmwareVariant::Production));
    TEST_ASSERT_EQUAL_STRING("bench", ev::firmwareVariantName(ev::FirmwareVariant::Bench));
    TEST_ASSERT_EQUAL_STRING("bench-watchdog",
                             ev::firmwareVariantName(ev::FirmwareVariant::BenchWatchdog));
    TEST_ASSERT_EQUAL_STRING("unknown",
                             ev::firmwareVariantName(ev::FirmwareVariant::Unknown));
}

// --- Commit A vs commit B ---------------------------------------------------

void test_different_revisions_render_differently(void) {
    char first[256];
    char second[256];
    render(probe("aaaaaaa", false, ev::FirmwareVariant::Production, 1U), first,
           sizeof(first));
    render(probe("bbbbbbb", false, ev::FirmwareVariant::Production, 1U), second,
           sizeof(second));
    TEST_ASSERT_TRUE(std::strcmp(first, second) != 0);
}

void test_same_source_config_renders_identically(void) {
    char first[256];
    char second[256];
    const ev::FirmwareIdentity identity = probe("d5b2651", false,
                                                ev::FirmwareVariant::Production, 0x12345678U);
    render(identity, first, sizeof(first));
    render(identity, second, sizeof(second));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

// --- Clean vs dirty ---------------------------------------------------------

void test_dirty_build_never_renders_as_clean(void) {
    char clean[256];
    char dirty[256];
    render(probe("d5b2651", false, ev::FirmwareVariant::Production, 1U), clean,
           sizeof(clean));
    render(probe("d5b2651", true, ev::FirmwareVariant::Production, 1U), dirty,
           sizeof(dirty));
    TEST_ASSERT_TRUE(std::strcmp(clean, dirty) != 0);
    TEST_ASSERT_TRUE(std::strstr(dirty, "dirty=1") != nullptr);
    TEST_ASSERT_TRUE(std::strstr(clean, "dirty=1") == nullptr);
}

void test_clean_build_predicate_requires_known_revision_variant_and_clean_tree(void) {
    TEST_ASSERT_TRUE(ev::firmwareIdentityIsCleanBuild(
        probe("d5b2651", false, ev::FirmwareVariant::Production, 1U)));
    TEST_ASSERT_FALSE(ev::firmwareIdentityIsCleanBuild(
        probe("d5b2651", true, ev::FirmwareVariant::Production, 1U)));
    TEST_ASSERT_FALSE(ev::firmwareIdentityIsCleanBuild(
        probe(ev::kFirmwareRevisionUnknown, false, ev::FirmwareVariant::Production, 1U)));
    TEST_ASSERT_FALSE(ev::firmwareIdentityIsCleanBuild(
        probe("d5b2651", false, ev::FirmwareVariant::Unknown, 1U)));
    TEST_ASSERT_FALSE(ev::firmwareIdentityIsCleanBuild(
        probe(nullptr, false, ev::FirmwareVariant::Production, 1U)));
}

// --- Missing metadata --------------------------------------------------------

void test_missing_revision_renders_unknown_and_is_never_clean(void) {
    char buffer[256];
    const ev::FirmwareIdentity identity = probe(nullptr, true, ev::FirmwareVariant::Unknown,
                                                0U);
    TEST_ASSERT_TRUE(render(identity, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_TRUE(std::strstr(buffer, "rev=unknown") != nullptr);
    TEST_ASSERT_TRUE(std::strstr(buffer, "variant=unknown") != nullptr);
    TEST_ASSERT_FALSE(ev::firmwareIdentityIsCleanBuild(identity));
}

// --- Image variants ------------------------------------------------------------

void test_all_variants_render_distinctly(void) {
    char production[256];
    char bench[256];
    char watchdog[256];
    render(probe("d5b2651", false, ev::FirmwareVariant::Production, 1U), production,
           sizeof(production));
    render(probe("d5b2651", false, ev::FirmwareVariant::Bench, 1U), bench, sizeof(bench));
    render(probe("d5b2651", false, ev::FirmwareVariant::BenchWatchdog, 1U), watchdog,
           sizeof(watchdog));
    TEST_ASSERT_TRUE(std::strcmp(production, bench) != 0);
    TEST_ASSERT_TRUE(std::strcmp(bench, watchdog) != 0);
    TEST_ASSERT_TRUE(std::strcmp(production, watchdog) != 0);
}

// --- Fingerprint ---------------------------------------------------------------

void test_fingerprint_fold_is_deterministic(void) {
    ev::FingerprintFold first;
    first.mixUint32(0x12345678U);
    first.mixDouble(0.0001);
    first.mixUint8(2U);
    ev::FingerprintFold second;
    second.mixUint32(0x12345678U);
    second.mixDouble(0.0001);
    second.mixUint8(2U);
    TEST_ASSERT_EQUAL_UINT32(first.done(), second.done());
}

void test_fingerprint_fold_is_sensitive_to_a_changed_value(void) {
    ev::FingerprintFold first;
    first.mixDouble(0.0001);
    ev::FingerprintFold second;
    second.mixDouble(0.000100000001);
    TEST_ASSERT_TRUE(first.done() != second.done());
}

void test_fingerprint_fold_is_sensitive_to_source_provenance(void) {
    ev::FingerprintFold first;
    first.mixUint8(static_cast<uint8_t>(ev::ValueSource::Provisional));
    ev::FingerprintFold second;
    second.mixUint8(static_cast<uint8_t>(ev::ValueSource::Measured));
    TEST_ASSERT_TRUE(first.done() != second.done());
}

void test_fingerprint_fold_is_order_sensitive(void) {
    ev::FingerprintFold first;
    first.mixUint32(1U);
    first.mixUint32(2U);
    ev::FingerprintFold second;
    second.mixUint32(2U);
    second.mixUint32(1U);
    TEST_ASSERT_TRUE(first.done() != second.done());
}

void test_changed_calibration_values_change_the_fold(void) {
    // End-to-end changed-configuration sensitivity through the public fold
    // API over real calibration structs: one value differs, the hash must.
    const ev::VehicleCalibration first{
        ev::Sourced<double>(0.0001, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.05, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.05, ev::ValueSource::Provisional)};
    const ev::VehicleCalibration second{
        ev::Sourced<double>(0.0002, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.05, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.05, ev::ValueSource::Provisional)};
    ev::FingerprintFold foldFirst;
    foldFirst.mixDouble(first.distancePerEncoderTickM.value);
    foldFirst.mixUint8(static_cast<uint8_t>(first.distancePerEncoderTickM.source));
    foldFirst.mixDouble(first.forwardBrakeOffsetM.value);
    foldFirst.mixDouble(first.reverseBrakeOffsetM.value);
    ev::FingerprintFold foldSecond;
    foldSecond.mixDouble(second.distancePerEncoderTickM.value);
    foldSecond.mixUint8(static_cast<uint8_t>(second.distancePerEncoderTickM.source));
    foldSecond.mixDouble(second.forwardBrakeOffsetM.value);
    foldSecond.mixDouble(second.reverseBrakeOffsetM.value);
    TEST_ASSERT_TRUE(foldFirst.done() != foldSecond.done());
}

void test_cfg_field_is_eight_lowercase_hex_digits(void) {
    char buffer[256];
    render(probe("d5b2651", false, ev::FirmwareVariant::Production, 0x00000AB5U), buffer,
           sizeof(buffer));
    TEST_ASSERT_TRUE(std::strstr(buffer, ",cfg=00000ab5,") != nullptr);
}

// --- Truncation -----------------------------------------------------------------

void test_identity_worst_case_holds_a_full_rendering(void) {
    // The widest allowed field widths must fit the stated worst case.
    char buffer[ev::kFirmwareIdentityWorstCaseChars + 1U];
    ev::FirmwareIdentity identity;
    identity.family = "123456789012345";   // 15, the capped width
    identity.revision = "1234567890123456";  // 16, the capped width
    identity.dirty = true;
    identity.variant = ev::FirmwareVariant::BenchWatchdog;  // longest name
    identity.configFingerprint = 0xFFFFFFFFU;
    identity.configSchema = 255U;
    TEST_ASSERT_TRUE(render(identity, buffer, sizeof(buffer)) > 0U);
    TEST_ASSERT_EQUAL_UINT16(ev::kFirmwareIdentityWorstCaseChars,
                             static_cast<uint16_t>(std::strlen(buffer)));
}

void test_truncated_identity_reports_did_not_fit_with_an_empty_buffer(void) {
    char buffer[16];
    std::memset(buffer, 'X', sizeof(buffer));
    const ev::FirmwareIdentity identity = probe("d5b2651", false,
                                                ev::FirmwareVariant::Production, 1U);
    TEST_ASSERT_EQUAL_UINT16(ev::kBoundedLineDidNotFit, render(identity, buffer, 10U));
    TEST_ASSERT_EQUAL_STRING("", buffer);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_identity_renders_the_full_fragment);
    RUN_TEST(test_variant_names_cover_all_three_images);
    RUN_TEST(test_different_revisions_render_differently);
    RUN_TEST(test_same_source_config_renders_identically);
    RUN_TEST(test_dirty_build_never_renders_as_clean);
    RUN_TEST(test_clean_build_predicate_requires_known_revision_variant_and_clean_tree);
    RUN_TEST(test_missing_revision_renders_unknown_and_is_never_clean);
    RUN_TEST(test_all_variants_render_distinctly);
    RUN_TEST(test_fingerprint_fold_is_deterministic);
    RUN_TEST(test_fingerprint_fold_is_sensitive_to_a_changed_value);
    RUN_TEST(test_fingerprint_fold_is_sensitive_to_source_provenance);
    RUN_TEST(test_fingerprint_fold_is_order_sensitive);
    RUN_TEST(test_changed_calibration_values_change_the_fold);
    RUN_TEST(test_cfg_field_is_eight_lowercase_hex_digits);
    RUN_TEST(test_identity_worst_case_holds_a_full_rendering);
    RUN_TEST(test_truncated_identity_reports_did_not_fit_with_an_empty_buffer);
    return UNITY_END();
}
