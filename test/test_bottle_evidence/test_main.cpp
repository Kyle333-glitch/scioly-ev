#include <cmath>
#include <cstdint>
#include <limits>

#include <unity.h>

#include <BottleEvidence.h>

// Host tests for the forward progression evidence
// (lib/evcore/src/BottleEvidence.h): the mapping from a run-axis position to the
// three progression facts the run policy waits for, and the provenance rule that
// keeps a placeholder interaction distance from ever reaching it.
//
// The three thresholds below arrive as arguments on purpose: the run's execution
// plan owns them (`test_execution_plan` pins that, including that approach
// completes at the plan's earliest credible contact position and capture at its
// capture guarantee). This suite therefore tests *how a position becomes a fact*,
// which is the one thing this module owns.
//
// Every number below is a fixture. None is a mechanism measurement: this vehicle
// has no bottle sensor, the pusher's reach and the bottle's behavior are
// unmeasured (VERIFIED_STATE.md #11), and the numbers here are not tuning
// suggestions. A passing test says the mapping is deterministic - it is not
// evidence that any bottle is contacted, captured or pushed.

void setUp(void) {}
void tearDown(void) {}

namespace {

ev::Sourced<double> measured(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Measured);
}

ev::Sourced<double> provisional(double value) {
    return ev::Sourced<double>(value, ev::ValueSource::Provisional);
}

// Fixture thresholds: the credible capture window opens 0.12 m before nominal
// contact, capture is guaranteed 0.20 m after nominal contact, and the push
// completes at the run's forward execution target.
ev::ForwardProgressionThresholds thresholds() {
    return ev::ForwardProgressionThresholds(1.88, 2.10, 6.15);
}

void expectFacts(const ev::BottleEvidence& evidence, bool approach, bool captured, bool pushed) {
    TEST_ASSERT_EQUAL_INT(approach ? 1 : 0, evidence.approachComplete ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(captured ? 1 : 0, evidence.captureComplete ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(pushed ? 1 : 0, evidence.pushComplete ? 1 : 0);
}

}  // namespace

// One fact per threshold, each appearing exactly at its own threshold and staying
// reported as the travel continues: a phase that has been entered is entered for
// good, and no fact is reported early.
void test_each_fact_appears_at_its_own_threshold_and_stays(void) {
    const ev::ForwardProgressionThresholds t = thresholds();
    expectFacts(ev::deriveForwardProgression(true, 1.5, true, t), false, false, false);
    expectFacts(ev::deriveForwardProgression(true, 1.8799, true, t), false, false, false);
    expectFacts(ev::deriveForwardProgression(true, 1.88, true, t), true, false, false);
    expectFacts(ev::deriveForwardProgression(true, 2.0999, true, t), true, false, false);
    expectFacts(ev::deriveForwardProgression(true, 2.10, true, t), true, true, false);
    expectFacts(ev::deriveForwardProgression(true, 6.1499, true, t), true, true, false);
    expectFacts(ev::deriveForwardProgression(true, 6.15, true, t), true, true, true);
    expectFacts(ev::deriveForwardProgression(true, 6.5, true, t), true, true, true);

    // Approach complete is a statement about the travel, not an observation of the
    // bottle: at the earliest credible contact position the run reports "the capture
    // window is open" and reports that capture is NOT yet complete.
    const ev::BottleEvidence atWindowOpen = ev::deriveForwardProgression(true, 1.88, true, t);
    TEST_ASSERT_TRUE(atWindowOpen.approachComplete);
    TEST_ASSERT_FALSE(atWindowOpen.captureComplete);
}

// The answer carries back the thresholds it was given, so a log can show what the
// firmware believed those three positions were next to the facts it produced.
void test_the_answer_reports_the_thresholds_it_was_given(void) {
    const ev::ForwardProgressionThresholds other =
        ev::ForwardProgressionThresholds(3.90, 4.10, 12.05);
    const ev::BottleEvidence evidence = ev::deriveForwardProgression(true, 4.0, true, other);
    TEST_ASSERT_TRUE(evidence.derived);
    TEST_ASSERT_TRUE(std::fabs(evidence.approachCompletePositionM - 3.90) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.captureCompletePositionM - 4.10) < 1e-12);
    TEST_ASSERT_TRUE(std::fabs(evidence.pushCompletePositionM - 12.05) < 1e-12);
    expectFacts(evidence, true, false, false);
}

// An unusable position is "nothing observed", not a zero: the difference is what
// keeps a run with a placeholder scale, an unmeasured tick polarity or a stale
// sample from advancing its phases on a distance that does not exist. The
// thresholds are still reported, because the geometry a run was working with is a
// different fact from the evidence it could derive from it.
void test_an_unusable_position_reports_nothing(void) {
    const ev::BottleEvidence evidence = ev::deriveForwardProgression(true, 9.0, false, thresholds());
    TEST_ASSERT_FALSE(evidence.derived);
    expectFacts(evidence, false, false, false);
    TEST_ASSERT_TRUE(std::fabs(evidence.approachCompletePositionM - 1.88) < 1e-12);
}

// A position that is not a number is no position, and a threshold that is not a
// number is no threshold. Either one reports nothing rather than comparing against
// an infinity, which would report every fact at once.
void test_a_non_finite_value_reports_nothing(void) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();

    const ev::BottleEvidence nanPosition = ev::deriveForwardProgression(true, nan, true, thresholds());
    TEST_ASSERT_FALSE(nanPosition.derived);
    expectFacts(nanPosition, false, false, false);

    const ev::BottleEvidence infinitePosition =
        ev::deriveForwardProgression(true, infinity, true, thresholds());
    TEST_ASSERT_FALSE(infinitePosition.derived);
    expectFacts(infinitePosition, false, false, false);

    const ev::ForwardProgressionThresholds nanThreshold =
        ev::ForwardProgressionThresholds(nan, 2.10, 6.15);
    const ev::BottleEvidence nanThresholdEvidence =
        ev::deriveForwardProgression(true, 100.0, true, nanThreshold);
    TEST_ASSERT_FALSE(nanThresholdEvidence.derived);
    expectFacts(nanThresholdEvidence, false, false, false);
}

// The reverse leg reports nothing: the bottle interaction is a forward-leg event,
// and a leg travelling back toward the Target Point is not driving into the bottle,
// however far the axis position reads.
void test_the_reverse_leg_reports_nothing(void) {
    const ev::BottleEvidence evidence = ev::deriveForwardProgression(false, 9.0, true, thresholds());
    TEST_ASSERT_FALSE(evidence.derived);
    expectFacts(evidence, false, false, false);
}

// A placeholder interaction distance produces no usable configuration at all, which
// is what keeps those distances from ever establishing a plan threshold - the plan
// refuses to be built from them (`RunExecutionPlan.h`), so no phase can exit on a
// guessed number.
void test_a_placeholder_distance_produces_no_usable_configuration(void) {
    const ev::BottleEvidenceConfig placeholder =
        ev::BottleEvidenceConfig(provisional(0.10), measured(0.20), measured(0.05));
    TEST_ASSERT_FALSE(ev::validateBottleEvidenceConfig(placeholder).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BottleEvidenceConfigError::ValuesAreNotMeasured),
                          static_cast<int>(ev::validateBottleEvidenceConfig(placeholder).error));
}

void test_a_negative_distance_is_refused_rather_than_reinterpreted(void) {
    const ev::BottleEvidenceConfig negative =
        ev::BottleEvidenceConfig(measured(-0.10), measured(0.20), measured(0.05));
    TEST_ASSERT_FALSE(ev::validateBottleEvidenceConfig(negative).ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::BottleEvidenceConfigError::NegativeDistance),
                          static_cast<int>(ev::validateBottleEvidenceConfig(negative).error));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_each_fact_appears_at_its_own_threshold_and_stays);
    RUN_TEST(test_the_answer_reports_the_thresholds_it_was_given);
    RUN_TEST(test_an_unusable_position_reports_nothing);
    RUN_TEST(test_a_non_finite_value_reports_nothing);
    RUN_TEST(test_the_reverse_leg_reports_nothing);
    RUN_TEST(test_a_placeholder_distance_produces_no_usable_configuration);
    RUN_TEST(test_a_negative_distance_is_refused_rather_than_reinterpreted);

    return UNITY_END();
}
