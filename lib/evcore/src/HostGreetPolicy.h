#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// When the boot greeting is owed to a reader
// ---------------------------------------------------------------------------
//
// The vehicle greets whoever is reading its port with one `BOOT` line. *Owed* is
// a decision about a host that cannot be asked directly: the firmware sees USB
// control lines, not applications, and a line staged into a port nobody holds is
// not a delivered line (H0 finding F-4). This type is that decision, kept pure so
// its edges and its quiet period can be tested without a vehicle or a host; the
// vehicle layer supplies the two inputs it cannot see for itself - the sampled
// control lines and the clock - and commits the line when `owed()` says so.
//
// Two observations make the greeting owed, and both are evidence of a reader
// rather than a state the firmware can observe directly:
//
//   * **A newly asserted control line.** DTR or RTS going to 1 is a host opening
//     the port. Two bits, not one, because a host can present a poor attach edge:
//     on Windows a host that closes a port and reopens it for another reader
//     process does not always deassert DTR, so DTR alone can be unchanged across
//     a genuinely new reader (`SerialTransport.h` has the core-level detail).
//   * **Host silence.** A host quiet for `kSilenceMs` is treated as a reader that
//     (re)appeared, and - because a silence that has no end is not one event -
//     the greeting is owed again at that cadence while the silence continues.
//     This is the only rule that covers the attach no edge can report, and it is
//     what turns "the greeting may have been lost" into "the greeting is repeated
//     until the host speaks".
//
// A third observation does the opposite of owing it: **a host byte** restarts the
// silence interval (`observeHostByte()` below), because whoever is typing is
// certainly reading, which makes a typed session one session rather than a
// sequence of new ones. Owing the greeting on every byte would greet before every
// command. The cost of that choice is recorded rather than hidden: a reader that
// arrives with no control-line edge and then types continuously postpones the
// silence deadline by `kSilenceMs` per byte and may see no greeting at all while
// it types. `boot` is the manual remedy (`owed()` immediately, and admissible in
// every state), and `docs/limitations.md` carries the limitation.
//
// What it deliberately is not: an inference that a *reader* exists. `owed()` does
// not mean "someone is listening", it means "the greeting is due to whoever is
// listening". Whether a write can actually leave is a separate question, asked of
// the transport (`serialTransport::hostLineState().dtr`), and the vehicle layer
// only stages the line when both are true.
//
// A host that presents no control-line edge *and* never writes is still
// indistinguishable from the previous reader: this policy bounds that case (the
// greeting goes out at each silence deadline) rather than removing it, and a host
// that presents no edge but writes continuously defers it.
class HostGreetPolicy {
public:
    // How long the host must be silent before it is treated as a (re)engaging
    // reader, and the cadence at which the greeting repeats while the silence
    // continues. Ten seconds is long enough that an operator typing commands is
    // one session - any received byte restarts the interval - and short enough
    // that an unattended capture sees the greeting in its opening lines rather
    // than never. Nothing here is a deadline: while the host stays silent the
    // whole cost is one greeting per interval.
    static constexpr uint32_t kSilenceMs = 10000U;

    // True while the greeting is owed. Nothing is owed until something is
    // observed or asked for: a policy that starts owed would greet on the first
    // pass of a loop that has not yet sampled a host at all.
    bool owed() const { return owed_; }

    // The greeting went out. It is no longer owed, and the silence interval
    // restarts so the next repeat is a full `kSilenceMs` away rather than
    // immediate.
    void delivered(uint32_t nowMs) {
        owed_ = false;
        lastActivityMs_ = nowMs;
    }

    // An operator asked for the greeting (`boot`, or the vehicle's own first
    // line at initialization). Also the single way a trigger below says "owed
    // now": the three reasons to greet end in the same state, so there is only
    // one place where that state is set.
    void requestGreeting(uint32_t nowMs) {
        owed_ = true;
        lastActivityMs_ = nowMs;
    }

    // The sampled control lines. A bit that has newly gone to 1 is a host opening
    // the port; the levels themselves are remembered so the next sample can tell
    // an edge from a level.
    void observeLines(bool dtr, bool rts, uint32_t nowMs) {
        if ((dtr && !dtr_) || (rts && !rts_)) {
            requestGreeting(nowMs);
        }
        dtr_ = dtr;
        rts_ = rts;
    }

    // A byte arrived from the host: whoever sent it is reading, so the silence
    // interval restarts. This is what keeps a typed session to one greeting.
    void observeHostByte(uint32_t nowMs) { lastActivityMs_ = nowMs; }

    // The clock, sampled every pass. Silence at or past `kSilenceMs` owes the
    // greeting and restarts the interval, so the repeat is periodic rather than
    // once: the reader being waited for may attach at any time.
    void observeClock(uint32_t nowMs) {
        if (nowMs - lastActivityMs_ >= kSilenceMs) {
            requestGreeting(nowMs);
        }
    }

private:
    bool dtr_ = false;
    bool rts_ = false;
    bool owed_ = false;
    uint32_t lastActivityMs_ = 0U;
};

}  // namespace ev
