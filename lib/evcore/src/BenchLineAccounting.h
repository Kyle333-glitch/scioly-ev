#pragma once

// Truthful bench-console command accounting (R3/L1).
//
// The bench health readout reports `lines_accepted`/`lines_refused` as
// evidence about the console's own outcomes, but the dispatch used to count
// only a subset: an accepted `bench arm` never incremented the accepted
// counter, a refused above-ceiling drive never incremented the refused one,
// read-only verbs counted nothing, and forwarded production lines counted
// nowhere. This header fixes the contract so every line the console answers
// is counted exactly once, in exactly one category:
//
//   * Accepted  - a bench verb the console executed (an ack or a readout).
//                 Acceptance is about the command, not the reply: a readout
//                 dropped for length still counts the command accepted (the
//                 loss is counted separately in `responses_lost`).
//   * Refused   - a bench verb refused for any reason (unknown verb, token
//                 errors, argument errors, ceiling violations, state refusals,
//                 framing errors). The reason travels on the wire
//                 (`BENCH,refused=1,reason=<name>`); the counter only says how
//                 many.
//   * Forwarded - a non-bench line handed to the production operator
//                 interface. Its outcome belongs to the production counters
//                 (`commandsAccepted_`/`commandsRejected_`); counting it here
//                 as accepted or refused would double-count one command.
//
// Deliberately uncounted: empty lines (no answer, exactly as the operator
// path treats them) and byte streams that never form a line (framing loss
// with no line to name is transport evidence, not a command outcome).
//
// `received()` is derived, not stored: accepted + refused + forwarded is
// every answered line, so a separate received counter could only disagree
// with the three it summarizes.

#include <cstdint>

namespace ev {

enum class BenchLineOutcome : uint8_t {
    Accepted = 0,
    Refused = 1,
    Forwarded = 2,
};

struct BenchLineCounters {
    uint32_t accepted = 0U;
    uint32_t refused = 0U;
    uint32_t forwarded = 0U;

    // Records one line's outcome. Exactly one call per answered line: the
    // dispatch records at its single choke point, never in the leaf verbs.
    void record(BenchLineOutcome outcome) {
        switch (outcome) {
            case BenchLineOutcome::Accepted:
                ++accepted;
                break;
            case BenchLineOutcome::Refused:
                ++refused;
                break;
            case BenchLineOutcome::Forwarded:
                ++forwarded;
                break;
        }
    }

    // Every answered line, in any category. Empty lines are excluded by
    // construction (they never reach `record()`), so this is "lines that
    // received an answer or a forwarding", not "bytes that arrived".
    uint32_t received() const { return accepted + refused + forwarded; }
};

}  // namespace ev
