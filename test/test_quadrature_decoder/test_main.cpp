#include <cstdint>

#include <unity.h>

#include <QuadratureDecoder.h>

// Contract tests for the pure A-channel CHANGE x2 quadrature decoder.
//
// These tests check the decoding logic against an independent reference model of
// the quadrature Gray cycle: the model tracks a position on the cycle ring, the
// decoder only ever sees a 2-bit (A, B) sample. They prove nothing about the
// vehicle. Which sense is physical forward travel is UNVERIFIED
// (VERIFIED_STATE.md #4) and can only be settled by hand-rotating a real encoder
// and recording which way the count moves.

// Pinned at compile time: the software sign convention this module and its tests
// are written against. Changing a value is a deliberate convention change that
// has to be logged.
static_assert(static_cast<int8_t>(ev::QuadratureTickSense::Negative) == -1,
              "the software counting convention is Negative == -1");
static_assert(static_cast<int8_t>(ev::QuadratureTickSense::None) == 0,
              "a sample that is not an A transition must contribute 0");
static_assert(static_cast<int8_t>(ev::QuadratureTickSense::Positive) == 1,
              "the software counting convention is Positive == +1");

void setUp(void) {}
void tearDown(void) {}

namespace {

// Independent reference model of the encoder: the quadrature Gray cycle as a
// 4-state ring, index -> channel levels. Advancing the index by one is one
// encoder edge in the Positive sense; decreasing it is one edge Negative.
//
//     0 -> 1 -> 2 -> 3 -> 0      (0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1))
//
// The decoder never sees this table. It derives a sense from the 2-bit state an
// edge lands in, while the model derives it from the direction of travel around
// the ring, so the two agree only when the decoding is right.
const bool kCycleA[4] = {false, true, true, false};
const bool kCycleB[4] = {false, false, true, true};

int cycleIndexOf(bool aLevel, bool bLevel) {
    for (int index = 0; index < 4; ++index) {
        if (kCycleA[index] == aLevel && kCycleB[index] == bLevel) {
            return index;
        }
    }
    return -1;
}

void expectSense(ev::QuadratureTickSense expected, ev::QuadratureTickSense actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

// A decoder whose reference state has already been established at `index`, i.e.
// the state it is in after initialization while the drivetrain is at rest. The
// establishing sample itself is covered by its own test.
ev::QuadratureDecoder decoderReferencedAt(int index) {
    ev::QuadratureDecoder decoder;
    decoder.update(kCycleA[index], kCycleB[index]);
    return decoder;
}

// A decoder plus the model's current ring position, so a test can walk the
// encoder edge by edge. Helpers here do not assert: their callers do.
struct Walk {
    ev::QuadratureDecoder decoder;
    int index;
};

// One encoder edge: moves the ring position and feeds the resulting sample.
ev::QuadratureTickSense step(Walk& walk, int direction) {
    walk.index = (walk.index + 4 + direction) % 4;
    return walk.decoder.update(kCycleA[walk.index], kCycleB[walk.index]);
}

// Walks in `direction` until an A transition is reported (at most two edges,
// because every quadrature cycle has two A edges and two B edges) and returns
// that sense.
ev::QuadratureTickSense nextAEdgeSense(Walk& walk, int direction) {
    for (int edge = 0; edge < 2; ++edge) {
        const ev::QuadratureTickSense sense = step(walk, direction);
        if (sense != ev::QuadratureTickSense::None) {
            return sense;
        }
    }
    return ev::QuadratureTickSense::None;
}

uint32_t nextRandom(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state >> 8;
}

}  // namespace

// ---------------------------------------------------------------------------
// Safe initial state and the reference sample
// ---------------------------------------------------------------------------

void test_a_fresh_decoder_has_no_reference_and_no_count(void) {
    ev::QuadratureDecoder decoder;
    TEST_ASSERT_FALSE(decoder.hasReference());
    TEST_ASSERT_EQUAL_INT(0, decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(0, decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(0, decoder.ignoredUpdates());
    TEST_ASSERT_FALSE(decoder.aLevel());
    TEST_ASSERT_FALSE(decoder.bLevel());
}

void test_the_first_sample_only_establishes_the_reference(void) {
    // Every possible first sample, including both channels high. No power-on
    // channel level is assumed, so the first sample can never look like an edge.
    const bool firstSamples[4][2] = {
        {false, false}, {true, false}, {false, true}, {true, true}};

    for (int sample = 0; sample < 4; ++sample) {
        ev::QuadratureDecoder decoder;
        expectSense(ev::QuadratureTickSense::None,
                    decoder.update(firstSamples[sample][0], firstSamples[sample][1]));

        TEST_ASSERT_TRUE(decoder.hasReference());
        TEST_ASSERT_EQUAL_INT(0, decoder.ticks());
        TEST_ASSERT_EQUAL_UINT32(0, decoder.countedEdges());
        // Establishing the reference is initialization, not an ignored update.
        TEST_ASSERT_EQUAL_UINT32(0, decoder.ignoredUpdates());
        TEST_ASSERT_EQUAL_INT(firstSamples[sample][0], decoder.aLevel());
        TEST_ASSERT_EQUAL_INT(firstSamples[sample][1], decoder.bLevel());
    }
}

// ---------------------------------------------------------------------------
// x2 resolution: only A transitions count
// ---------------------------------------------------------------------------

void test_only_a_channel_transitions_count(void) {
    // One full quadrature cycle is four encoder edges but only two A edges, so
    // this decode must produce exactly two counts per cycle (x1 would give one,
    // x4 would give four).
    Walk walk{decoderReferencedAt(0), 0};

    expectSense(ev::QuadratureTickSense::Positive, step(walk, 1));  // 00 -> 10, A rises
    TEST_ASSERT_EQUAL_INT(1, walk.decoder.ticks());

    expectSense(ev::QuadratureTickSense::None, step(walk, 1));      // 10 -> 11, B rises only
    TEST_ASSERT_EQUAL_INT(1, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(1, walk.decoder.ignoredUpdates());

    expectSense(ev::QuadratureTickSense::Positive, step(walk, 1));  // 11 -> 01, A falls
    expectSense(ev::QuadratureTickSense::None, step(walk, 1));      // 01 -> 00, B falls only

    TEST_ASSERT_EQUAL_INT(2, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_INT(0, walk.index);
    TEST_ASSERT_EQUAL_UINT32(2, walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(2, walk.decoder.ignoredUpdates());
}

void test_b_only_updates_never_move_the_count(void) {
    Walk walk{decoderReferencedAt(0), 0};

    // 00 -> 01 is a B edge: it is not an A transition, so it must not count, and
    // it must still refresh the tracked B level for the next A edge.
    expectSense(ev::QuadratureTickSense::None, walk.decoder.update(false, true));
    TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(0, walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(1, walk.decoder.ignoredUpdates());
    TEST_ASSERT_FALSE(walk.decoder.aLevel());
    TEST_ASSERT_TRUE(walk.decoder.bLevel());

    // The refreshed B level is what makes this next edge decode as (0,1) -> (1,1)
    // -> Negative. A stale B of 0 would have decoded the same sample as
    // (1,0) -> Positive and silently reversed the count.
    expectSense(ev::QuadratureTickSense::Negative, walk.decoder.update(true, true));
    TEST_ASSERT_EQUAL_INT(-1, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(1, walk.decoder.countedEdges());
}

void test_a_repeated_sample_is_not_a_transition(void) {
    Walk walk{decoderReferencedAt(0), 0};
    expectSense(ev::QuadratureTickSense::Positive, step(walk, 1));  // 00 -> 10

    for (int repeat = 0; repeat < 5; ++repeat) {
        expectSense(ev::QuadratureTickSense::None,
                    walk.decoder.update(kCycleA[walk.index], kCycleB[walk.index]));
    }

    TEST_ASSERT_EQUAL_INT(1, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(1, walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(5, walk.decoder.ignoredUpdates());
}

void test_both_channels_changing_between_counted_edges_is_normal(void) {
    // Between two A edges of one revolution B changes as well (that is what
    // quadrature is), so a sample in which both levels differ from the previous
    // sample is the normal case and must still decode. It is not an error
    // condition and no edge may be dropped for it.
    ev::QuadratureDecoder decoder = decoderReferencedAt(1);  // (1,0)
    expectSense(ev::QuadratureTickSense::Positive, decoder.update(false, true));  // -> (0,1)
    TEST_ASSERT_EQUAL_INT(1, decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(1, decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(0, decoder.ignoredUpdates());
}

// ---------------------------------------------------------------------------
// Direction comes from both channels, never from B alone
// ---------------------------------------------------------------------------

void test_direction_is_not_a_function_of_b_alone(void) {
    // From the (1,1) state both directions of travel start with an A fall and B
    // is high in both cases: edge type and preceding B level are identical, yet
    // the senses must be opposite. B alone - even B plus the edge type - cannot
    // produce that; the landing (A, B) state can.
    ev::QuadratureDecoder positiveFrom11 = decoderReferencedAt(2);  // (1,1)
    ev::QuadratureDecoder negativeFrom11 = decoderReferencedAt(2);  // (1,1)
    TEST_ASSERT_TRUE(positiveFrom11.bLevel());
    TEST_ASSERT_TRUE(negativeFrom11.bLevel());

    expectSense(ev::QuadratureTickSense::Positive,
                positiveFrom11.update(false, true));   // (1,1) -> (0,1)
    expectSense(ev::QuadratureTickSense::Negative,
                negativeFrom11.update(false, false));  // (1,1) -> (1,0)

    TEST_ASSERT_EQUAL_INT(1, positiveFrom11.ticks());
    TEST_ASSERT_EQUAL_INT(-1, negativeFrom11.ticks());

    // Nor does a fixed B level mean a fixed sense around the cycle: on the
    // Positive cycle the two counted A edges land with B low (1,0) and B high
    // (0,1), and both count Positive...
    ev::QuadratureDecoder positiveInto10 = decoderReferencedAt(0);
    expectSense(ev::QuadratureTickSense::Positive, positiveInto10.update(true, false));
    ev::QuadratureDecoder positiveInto01 = decoderReferencedAt(2);
    expectSense(ev::QuadratureTickSense::Positive, positiveInto01.update(false, true));
    TEST_ASSERT_FALSE(positiveInto10.bLevel());
    TEST_ASSERT_TRUE(positiveInto01.bLevel());

    // ...while on the Negative cycle they land with B high (1,1) and B low (0,0)
    // and both count Negative. So no reading of B on its own maps to a sense.
    ev::QuadratureDecoder negativeInto11 = decoderReferencedAt(3);
    expectSense(ev::QuadratureTickSense::Negative, negativeInto11.update(true, true));
    ev::QuadratureDecoder negativeInto00 = decoderReferencedAt(1);
    expectSense(ev::QuadratureTickSense::Negative, negativeInto00.update(false, false));
    TEST_ASSERT_TRUE(negativeInto11.bLevel());
    TEST_ASSERT_FALSE(negativeInto00.bLevel());
}

// ---------------------------------------------------------------------------
// Exhaustive single-edge decoding
// ---------------------------------------------------------------------------

void test_every_reachable_edge_transition_decodes_as_the_reference_model(void) {
    // All 8 (state, direction) transitions of the quadrature ring. Four of them
    // are A edges and must count with the sign of the travel direction; the
    // other four are B edges and must contribute nothing.
    for (int startIndex = 0; startIndex < 4; ++startIndex) {
        for (int direction = -1; direction <= 1; direction += 2) {
            const int landingIndex = (startIndex + 4 + direction) % 4;
            const bool aTransition = (kCycleA[startIndex] != kCycleA[landingIndex]);

            ev::QuadratureDecoder decoder = decoderReferencedAt(startIndex);
            const ev::QuadratureTickSense sense =
                decoder.update(kCycleA[landingIndex], kCycleB[landingIndex]);

            if (aTransition) {
                expectSense(direction > 0 ? ev::QuadratureTickSense::Positive
                                          : ev::QuadratureTickSense::Negative,
                            sense);
                TEST_ASSERT_EQUAL_INT(direction, decoder.ticks());
                TEST_ASSERT_EQUAL_UINT32(1, decoder.countedEdges());
                TEST_ASSERT_EQUAL_UINT32(0, decoder.ignoredUpdates());
            } else {
                expectSense(ev::QuadratureTickSense::None, sense);
                TEST_ASSERT_EQUAL_INT(0, decoder.ticks());
                TEST_ASSERT_EQUAL_UINT32(0, decoder.countedEdges());
                TEST_ASSERT_EQUAL_UINT32(1, decoder.ignoredUpdates());
            }

            TEST_ASSERT_EQUAL_INT(landingIndex,
                                  cycleIndexOf(decoder.aLevel(), decoder.bLevel()));
        }
    }
}

void test_all_four_landing_states_are_decodable(void) {
    // Every (A, B) state an A edge can land in, the state it comes from, and the
    // sense it must produce. Nothing here is an invalid transition, so nothing
    // may be rejected or swallowed: a future "reject the impossible edge" change
    // would lose real counts and has to fail this test.
    const int previousIndexForLanding[4] = {1, 0, 3, 2};
    const ev::QuadratureTickSense expectedSense[4] = {
        ev::QuadratureTickSense::Negative,  // lands in 00 after an A fall
        ev::QuadratureTickSense::Positive,  // lands in 10 after an A rise
        ev::QuadratureTickSense::Negative,  // lands in 11 after an A rise
        ev::QuadratureTickSense::Positive,  // lands in 01 after an A fall
    };

    for (int landing = 0; landing < 4; ++landing) {
        ev::QuadratureDecoder decoder = decoderReferencedAt(previousIndexForLanding[landing]);
        const ev::QuadratureTickSense sense = decoder.update(kCycleA[landing], kCycleB[landing]);

        expectSense(expectedSense[landing], sense);
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(ev::QuadratureTickSense::None),
                              static_cast<int>(sense));
        TEST_ASSERT_EQUAL_UINT32(1, decoder.countedEdges());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(static_cast<int8_t>(expectedSense[landing])),
                              decoder.ticks());
    }

    // The sense is a pure function of the landing state alone, with no
    // dependence on which edge type brought it there, which is exactly what
    // tickSenseForLandingState() is.
    for (int landing = 0; landing < 4; ++landing) {
        expectSense(expectedSense[landing],
                    ev::tickSenseForLandingState(kCycleA[landing], kCycleB[landing]));
    }
}

// ---------------------------------------------------------------------------
// Repeated cycles, reversal and cancellation
// ---------------------------------------------------------------------------

void test_repeated_cycles_accumulate_linearly(void) {
    const int cycles = 250;
    Walk walk{decoderReferencedAt(0), 0};

    for (int cycle = 0; cycle < cycles; ++cycle) {
        for (int edge = 0; edge < 4; ++edge) {
            step(walk, 1);
        }
    }

    TEST_ASSERT_EQUAL_INT(cycles * 2, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cycles * 2), walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cycles * 2), walk.decoder.ignoredUpdates());
    TEST_ASSERT_EQUAL_INT(0, walk.index);
    TEST_ASSERT_FALSE(walk.decoder.aLevel());
    TEST_ASSERT_FALSE(walk.decoder.bLevel());

    // The same number of edges the other way returns the count exactly to zero.
    for (int cycle = 0; cycle < cycles; ++cycle) {
        for (int edge = 0; edge < 4; ++edge) {
            step(walk, -1);
        }
    }

    TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cycles * 4), walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cycles * 4), walk.decoder.ignoredUpdates());
    TEST_ASSERT_EQUAL_INT(0, walk.index);
}

void test_reversal_from_every_state_inverts_the_next_counted_edge(void) {
    // After a reversal the first A edge must count the other way, from every
    // starting state. The first A edge after a reversal can be the same edge
    // type as before it (an A rise can go either way round the cycle), so the
    // test walks to the next A edge instead of assuming one.
    for (int index = 0; index < 4; ++index) {
        Walk forward{decoderReferencedAt(index), index};
        Walk backward{decoderReferencedAt(index), index};

        expectSense(ev::QuadratureTickSense::Positive, nextAEdgeSense(forward, 1));
        expectSense(ev::QuadratureTickSense::Negative, nextAEdgeSense(backward, -1));

        TEST_ASSERT_EQUAL_INT(1, forward.decoder.ticks());
        TEST_ASSERT_EQUAL_INT(-1, backward.decoder.ticks());
    }
}

void test_mid_run_reversal_from_every_state_cancels_the_count(void) {
    // Ten encoder edges out, the same ten edges back, from every state a
    // reversal can start in. Every A edge on the way out must count Positive and
    // every A edge on the way back must count Negative, and the channel state
    // and the count must end exactly where they started.
    for (int index = 0; index < 4; ++index) {
        Walk walk{decoderReferencedAt(index), index};
        uint32_t expectedCountedEdges = 0;

        for (int edge = 0; edge < 10; ++edge) {
            const int previousIndex = walk.index;
            const ev::QuadratureTickSense sense = step(walk, 1);
            if (kCycleA[previousIndex] != kCycleA[walk.index]) {
                expectSense(ev::QuadratureTickSense::Positive, sense);
                ++expectedCountedEdges;
            } else {
                expectSense(ev::QuadratureTickSense::None, sense);
            }
        }
        TEST_ASSERT_EQUAL_UINT32(expectedCountedEdges, walk.decoder.countedEdges());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expectedCountedEdges), walk.decoder.ticks());

        for (int edge = 0; edge < 10; ++edge) {
            const int previousIndex = walk.index;
            const ev::QuadratureTickSense sense = step(walk, -1);
            if (kCycleA[previousIndex] != kCycleA[walk.index]) {
                expectSense(ev::QuadratureTickSense::Negative, sense);
                ++expectedCountedEdges;
            } else {
                expectSense(ev::QuadratureTickSense::None, sense);
            }
        }

        TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());
        TEST_ASSERT_EQUAL_INT(index, walk.index);
        TEST_ASSERT_EQUAL_INT(kCycleA[index], walk.decoder.aLevel());
        TEST_ASSERT_EQUAL_INT(kCycleB[index], walk.decoder.bLevel());
        TEST_ASSERT_EQUAL_UINT32(expectedCountedEdges, walk.decoder.countedEdges());
    }
}

void test_a_bounce_oscillation_cancels_to_no_net_count(void) {
    // Backlash or a hesitant reversal shows up as pairs of opposite edges. They
    // must cancel exactly, so the count reports no net travel rather than a
    // phantom offset. Every state is exercised, and each oscillation is one edge
    // out and one edge back, which is the smallest physical hesitation there is.
    //
    // An oscillation that straddles an A edge counts out and back; one that
    // straddles a B edge never counts at all. Both must leave the count and the
    // channel state exactly as they were, so only those two are asserted here.
    for (int index = 0; index < 4; ++index) {
        Walk walk{decoderReferencedAt(index), index};

        for (int bounce = 0; bounce < 50; ++bounce) {
            step(walk, 1);
            step(walk, -1);
        }

        TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());
        TEST_ASSERT_EQUAL_INT(index, walk.index);
        TEST_ASSERT_EQUAL_INT(kCycleA[index], walk.decoder.aLevel());
        TEST_ASSERT_EQUAL_INT(kCycleB[index], walk.decoder.bLevel());
        // Exactly the two samples per bounce were fed: nothing was lost or
        // silently double-counted.
        TEST_ASSERT_EQUAL_UINT32(
            100, walk.decoder.countedEdges() + walk.decoder.ignoredUpdates());
    }
}

void test_reset_forgets_the_reference_the_count_and_the_levels(void) {
    Walk walk{decoderReferencedAt(1), 1};
    nextAEdgeSense(walk, 1);

    walk.decoder.reset();

    TEST_ASSERT_FALSE(walk.decoder.hasReference());
    TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(0, walk.decoder.countedEdges());
    TEST_ASSERT_EQUAL_UINT32(0, walk.decoder.ignoredUpdates());
    TEST_ASSERT_FALSE(walk.decoder.aLevel());
    TEST_ASSERT_FALSE(walk.decoder.bLevel());

    // The first sample after reset re-establishes the reference and never
    // counts, even though the channels moved while no reference existed.
    expectSense(ev::QuadratureTickSense::None, walk.decoder.update(true, false));
    TEST_ASSERT_TRUE(walk.decoder.hasReference());
    TEST_ASSERT_EQUAL_INT(0, walk.decoder.ticks());

    // Counting resumes from that new reference.
    expectSense(ev::QuadratureTickSense::Positive, walk.decoder.update(false, true));
    TEST_ASSERT_EQUAL_INT(1, walk.decoder.ticks());
    TEST_ASSERT_EQUAL_UINT32(1, walk.decoder.countedEdges());
}

// ---------------------------------------------------------------------------
// Randomized walk against the reference model
// ---------------------------------------------------------------------------

// Steps a random walk over the cycle in both directions, injects duplicate
// samples (the shape of a repeated call or a bounce), and checks after every
// single sample that the decoder agrees with the model on the count, the edge
// accounting and the observed levels.
void runRandomWalkPropertySearch(uint32_t seed) {
    uint32_t random = seed;
    int index = static_cast<int>(nextRandom(random) % 4U);
    ev::QuadratureDecoder decoder = decoderReferencedAt(index);

    int32_t expectedTicks = 0;
    uint32_t expectedCountedEdges = 0;
    uint32_t expectedIgnoredUpdates = 0;

    for (int iteration = 0; iteration < 4000; ++iteration) {
        // A quarter of the steps repeat the current sample instead of moving.
        const int direction = ((nextRandom(random) % 4U) == 0U)
                                  ? 0
                                  : (((nextRandom(random) & 1U) != 0U) ? 1 : -1);
        const int nextIndex = (index + 4 + direction) % 4;
        const bool aTransition = (kCycleA[index] != kCycleA[nextIndex]);

        const ev::QuadratureTickSense sense =
            decoder.update(kCycleA[nextIndex], kCycleB[nextIndex]);

        if (aTransition) {
            expectedTicks += (direction > 0) ? 1 : -1;
            ++expectedCountedEdges;
            expectSense(direction > 0 ? ev::QuadratureTickSense::Positive
                                      : ev::QuadratureTickSense::Negative,
                        sense);
        } else {
            ++expectedIgnoredUpdates;
            expectSense(ev::QuadratureTickSense::None, sense);
        }

        TEST_ASSERT_EQUAL_INT(expectedTicks, decoder.ticks());
        TEST_ASSERT_EQUAL_UINT32(expectedCountedEdges, decoder.countedEdges());
        TEST_ASSERT_EQUAL_UINT32(expectedIgnoredUpdates, decoder.ignoredUpdates());
        TEST_ASSERT_EQUAL_INT(nextIndex, cycleIndexOf(decoder.aLevel(), decoder.bLevel()));

        index = nextIndex;
    }
}

void test_no_sample_sequence_breaks_the_decoder(void) {
    // Fixed seeds for CI determinism; every invariant must hold for each one.
    const uint32_t seeds[] = {0x5EED1234U, 0xDEADBEEFU, 1U};
    for (uint32_t seed : seeds) {
        runRandomWalkPropertySearch(seed);
    }
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_a_fresh_decoder_has_no_reference_and_no_count);
    RUN_TEST(test_the_first_sample_only_establishes_the_reference);

    RUN_TEST(test_only_a_channel_transitions_count);
    RUN_TEST(test_b_only_updates_never_move_the_count);
    RUN_TEST(test_a_repeated_sample_is_not_a_transition);
    RUN_TEST(test_both_channels_changing_between_counted_edges_is_normal);

    RUN_TEST(test_direction_is_not_a_function_of_b_alone);

    RUN_TEST(test_every_reachable_edge_transition_decodes_as_the_reference_model);
    RUN_TEST(test_all_four_landing_states_are_decodable);

    RUN_TEST(test_repeated_cycles_accumulate_linearly);
    RUN_TEST(test_reversal_from_every_state_inverts_the_next_counted_edge);
    RUN_TEST(test_mid_run_reversal_from_every_state_cancels_the_count);
    RUN_TEST(test_a_bounce_oscillation_cancels_to_no_net_count);
    RUN_TEST(test_reset_forgets_the_reference_the_count_and_the_levels);

    RUN_TEST(test_no_sample_sequence_breaks_the_decoder);

    return UNITY_END();
}
