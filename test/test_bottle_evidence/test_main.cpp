#include <cmath>
#include <cstdint>

#include <unity.h>

#include <BottleEvidence.h>

// Host tests for the bottle-interaction evidence (lib/evcore/src/BottleEvidence.h):
// the derivation of contact, capture and push evidence from the run axis, the
// provenance rule that keeps a placeholder from producing evidence, and the two
// ways the derivation reports "nothing observed".
//
// Every distance below is a fixture. None is a mechanism measurement: this
// vehicle has no bottle sensor, the pusher's reach and the bottle's behavior are
// unmeasured (VERIFIED_STATE.md #11), and the numbers here are not tuning
// suggestions. A passing test says the mapping is deterministic - it is not
// evidence that any bottle is contacted or pushed.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

// Fixture policy: contact 0.10 m before the Bottle Start Point, capture complete
// 0.20 m after contact, push complete 0.05 m past the Bottle Line landmark.
ev::BottleEvidenceConfig fixtureConfig() {
    return ev::BottleEvidenceConfig(measured(0.10), measured(0.20), measured(0.05));
}

// A Regional run: 5.00 m Target Distance, 1.00 m Bottle Line Distance, bottle at
// 2.00 m (rules 8.h, 8.j, 7.c.ii.(5)). The Bottle Line is therefore 6.00 m.
ev::RunAxisGeometry geometry() {
    return ev::RunAxisGeometry(5.0, 1.0, 2.0);
}

void expectEvidence(const ev::BottleEvidence& evidence, bool contact, bool captured, bool pushed) {
    TEST_ASSERT_EQUAL_INT(contact ? 1 : 0, evidence.contactDetected ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(captured ? 1 : 0, evidence.captureComplete ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(pushed ? 1 : 0, evidence.pushComplete ? 1 : 0);
}

}  // namespace

// The three landmarks are the announced geometry moved by the configured
// distances, so a log can show what the firmware believed them to be.
void test_the_landmarks_are_the_geometry_moved_by_the_configured_distances(void) {
    const ev::BottleEvidence evidence =
        ev::deriveBottleEvidence(geometry(), true, 0.0, true, fixtureConfig());
    TEST_ASSERT_TRUE(ev::validateBottleEvidenceConfig(fixtureConfig()).ok);
    TEST_ASSERT_TRUE(evidence.derived);
    TEST_ASSERT_TRUE(std::fabs(evidence.contactPositionM - 1.90) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.capturePositionM - 2.10) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.pushPositionM - 6.05) < 1e-12);
}

// The evidence is monotone in the position: nothing is reported early, each fact
// appears at its own landmark, and once reported it stays reported as the vehicle
// travels on - a phase that has been entered is entered for good.
void test_each_fact_appears_at_its_own_landmark_and_stays(void) {
    const ev::BottleEvidenceConfig config = fixtureConfig();
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 1.5, true, config), false, false,
                   false);
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 1.9, true, config), true, false,
                   false);
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 2.0999, true, config), true, false,
                   false);
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 2.1, true, config), true, true,
                   false);
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 6.05, true, config), true, true,
                   true);
    expectEvidence(ev::deriveBottleEvidence(geometry(), true, 6.5, true, config), true, true,
                   true);
}

// A placeholder distance produces no evidence at all, which is what makes a phase
// wait (and then time out) instead of exiting on a guessed number.
void test_a_placeholder_distance_produces_no_evidence(void) {
    const ev::BottleEvidenceConfig placeholder =
        ev::BottleEvidenceConfig(provisional(0.10), measured(0.20), measured(0.05));
    TEST_ASSERT_FALSE(ev::validateBottleEvidenceConfig(placeholder).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BottleEvidenceConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateBottleEvidenceConfig(placeholder).error));

    const ev::BottleEvidence evidence =
        ev::deriveBottleEvidence(geometry(), true, 9.0, true, placeholder);
    TEST_ASSERT_FALSE(evidence.derived);
    expectEvidence(evidence, false, false, false);
}

void test_a_negative_distance_is_refused_rather_than_reinterpreted(void) {
    const ev::BottleEvidenceConfig negative =
        ev::BottleEvidenceConfig(measured(-0.10), measured(0.20), measured(0.05));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BottleEvidenceConfigError::NegativeDistance),
                          static_cast<int>(ev::validateBottleEvidenceConfig(negative).error));
    TEST_ASSERT_FALSE(
        ev::deriveBottleEvidence(geometry(), true, 9.0, true, negative).derived);
}

// An unusable position is "nothing observed", not a zero: the difference is what
// keeps a run with a placeholder scale or an unmeasured tick polarity from
// advancing its phases on a distance that does not exist.
void test_an_unusable_position_reports_nothing(void) {
    const ev::BottleEvidence evidence =
        ev::deriveBottleEvidence(geometry(), true, 9.0, false, fixtureConfig());
    TEST_ASSERT_FALSE(evidence.derived);
    expectEvidence(evidence, false, false, false);
}

// The reverse leg produces no bottle evidence: the bottle is a forward-leg
// interaction, and a leg travelling back toward the Target Point is not touching
// it, however far the axis position reads.
void test_the_reverse_leg_reports_nothing(void) {
    const ev::BottleEvidence evidence =
        ev::deriveBottleEvidence(geometry(), false, 9.0, true, fixtureConfig());
    TEST_ASSERT_FALSE(evidence.derived);
    expectEvidence(evidence, false, false, false);
}

// A different announcement moves every landmark with it: the evidence is the
// announced geometry's, so the same position means different things on a National
// course than on a Regional one.
void test_a_different_announcement_moves_every_landmark(void) {
    const ev::RunAxisGeometry national(9.0, 3.0, 4.0);  // rules 8.h, 8.j, 7.c.ii.(5)
    const ev::BottleEvidence evidence =
        ev::deriveBottleEvidence(national, true, 4.0, true, fixtureConfig());
    TEST_ASSERT_TRUE(evidence.derived);
    TEST_ASSERT_TRUE(std::fabs(evidence.contactPositionM - 3.90) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.capturePositionM - 4.10) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.pushPositionM - 12.05) < 1e-12);
    // On the same distance that is a Regional contact and capture, the National
    // course has only just made contact: the landmarks travelled with the
    // announcement, which is the point of deriving them from the geometry.
    expectEvidence(evidence, true, false, false);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_the_landmarks_are_the_geometry_moved_by_the_configured_distances);
    RUN_TEST(test_each_fact_appears_at_its_own_landmark_and_stays);
    RUN_TEST(test_a_placeholder_distance_produces_no_evidence);
    RUN_TEST(test_a_negative_distance_is_refused_rather_than_reinterpreted);
    RUN_TEST(test_an_unusable_position_reports_nothing);
    RUN_TEST(test_the_reverse_leg_reports_nothing);
    RUN_TEST(test_a_different_announcement_moves_every_landmark);

    return UNITY_END();
}
