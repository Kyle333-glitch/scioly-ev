#pragma once

#include "Odometry.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Candidate low-speed velocity estimation - pure logic, no scale of its own
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One window estimator: the velocity behind a window is the signed tick
//   difference across that window, divided by the time the window covers, times
//   the caller's effective distance per tick. Nothing else is estimated and no
//   other quantity is derived from it.
// * Parameterized, not duplicated. `windowMs` is a construction argument, so
//   comparing candidate effective windows is several configurations of this one
//   module rather than several copies of an algorithm. The 20/40/60/80 ms
//   windows used in the tests are bench candidates, not measured values and not
//   a selection.
// * A sliding window, not a tumbling one. The retained samples are pruned to the
//   shortest span that still covers `windowMs`, and the span actually used is
//   reported with every number, so an effective window is always visible
//   instead of implied.
// * Explicit about every way a number can be absent. In particular, an encoder
//   that has not changed for `noEdgeTimeoutMs` is reported as an explicit
//   zero-speed claim, not as a window that happens to contain no ticks.
//
// What this module is not
//
// * It owns no scale, exactly like Odometry.h. The effective distance per
//   encoder tick arrives on every update as the caller's `Sourced<double>` (the
//   value `VehicleCalibration::distancePerEncoderTickM` holds), and the scale
//   rule itself is `distanceScaleStatus()` from Odometry.h: only
//   `ValueSource::Measured`, finite and positive may produce a valid number.
// * It is not an edge-timestamp estimator. `SensorSnapshot` carries one stamp
//   per sample and no per-edge timestamps, so edge-period timing would need a
//   timestamp source that does not exist yet; manufacturing one here would be
//   inventing data. A later sampling layer may legitimately add it.
// * It declares no physical direction. Positive ticks stay the decoder's
//   Positive ticks: which counting convention is physical forward travel is
//   UNVERIFIED (VERIFIED_STATE.md #4), and nothing in this layer may decide it.
// * It decides nothing about safety. `zeroSpeedClaimed` is an explicit software
//   timeout on observed encoder change, not a measurement of the vehicle, and
//   no estimator configuration in this stage is marked physically adequate.
//   Which window is usable, and what evidence a zero-speed claim needs before it
//   may gate a direction change, are questions for a physical test that has not
//   happened (VERIFIED_STATE.md #5, #10).
// * It does not decide whether a number may be *used* by itself. Freshness is
//   owned here in exactly one place: `velocityReadiness()` below is the single
//   verdict combining estimate existence, validity and age, and
//   `VelocityEstimate::elapsedMs` is the stamp it judges.
// * No filtering, debounce, interpolation, extrapolation or prediction beyond
//   the window arithmetic itself. A window averages over its span - that is what
//   a window is, and it is why low-speed quantization exists - so the honest
//   description is: the only processing is the net tick difference over the
//   covered span times the supplied scale. Nothing smooths, extrapolates or
//   predicts, and no signal-quality policy is invented.

// Estimator configuration. Both values are supplied by the caller; neither has a
// default, because neither has been measured.
struct VelocityEstimatorConfig {
    constexpr VelocityEstimatorConfig(uint32_t windowMs_, uint32_t noEdgeTimeoutMs_)
        : windowMs(windowMs_), noEdgeTimeoutMs(noEdgeTimeoutMs_) {}

    // Requested window span, milliseconds. This is the shortest span a reported
    // number will cover; the realized span is reported as `effectiveWindowMs`,
    // which is at least this and less than this plus the spacing between the two
    // oldest retained samples. This is a bench candidate value, not a measured
    // property of any vehicle.
    uint32_t windowMs;

    // How long without an encoder change before the explicit zero-speed claim is
    // made, milliseconds. This is a software policy, not a measured stopping
    // behavior.
    uint32_t noEdgeTimeoutMs;
};

enum class VelocityEstimatorConfigError : uint8_t {
    None = 0,
    WindowNotPositive,           // a zero window could never cover a span
    NoEdgeTimeoutNotPositive,    // a zero timeout would claim zero speed on the first sample
};

struct VelocityEstimatorConfigValidation {
    // Invariant: error == None exactly when ok is true.
    bool ok;
    VelocityEstimatorConfigError error;
};

inline VelocityEstimatorConfigValidation validateVelocityEstimatorConfig(
    const VelocityEstimatorConfig& config) {
    // A zero window is refused rather than interpreted as "instantaneous": there
    // is no such thing as a zero-span measurement, and an all-zero window would
    // divide by zero. An explicit zero timeout is refused because it would make
    // every sample a zero-speed claim, including the first one.
    if (config.windowMs == 0U) {
        return {false, VelocityEstimatorConfigError::WindowNotPositive};
    }
    if (config.noEdgeTimeoutMs == 0U) {
        return {false, VelocityEstimatorConfigError::NoEdgeTimeoutNotPositive};
    }
    return {true, VelocityEstimatorConfigError::None};
}

// Why the estimator returned what it did. This is the refusal vocabulary for
// every way a velocity number can be absent.
enum class VelocityEstimateStatus : uint8_t {
    Valid = 0,               // a window-derived, finite number with a measured scale
    ConfigIsNotValid,        // the estimator configuration failed validation
    ScaleIsNotMeasured,      // the supplied distance-per-tick scale is not Measured
    ScaleIsNotFinite,        // NaN or infinity
    ScaleIsNotPositive,      // zero or negative; would invert or annihilate the sign
    NoEdgeTimeout,           // no encoder change for at least noEdgeTimeoutMs
    WarmingUp,               // window not covered yet: a new number does not exist yet
    WindowExceedsHistory,    // retained history cannot cover windowMs at this update rate
    TimeDidNotAdvance,       // sample stamp not strictly newer than the previous sample
    SpeedIsNotFinite,        // the window conversion overflowed
};

// One estimator answer. There is no default constructor: a valid-looking
// estimate with no reason behind it must be impossible to write.
struct VelocityEstimate {
    constexpr VelocityEstimate(double velocityMps_, bool valid_, bool zeroSpeedClaimed_,
                               VelocityEstimateStatus status_, uint32_t elapsedMs_,
                               uint32_t effectiveWindowMs_, int64_t effectiveWindowTicks_)
        : velocityMps(velocityMps_),
          valid(valid_),
          zeroSpeedClaimed(zeroSpeedClaimed_),
          status(status_),
          elapsedMs(elapsedMs_),
          effectiveWindowMs(effectiveWindowMs_),
          effectiveWindowTicks(effectiveWindowTicks_) {}

    // Signed speed in the encoder's own counting convention, metres per second.
    // 0.0 whenever no number exists, so a refusal never carries a value that
    // looks like a measurement.
    double velocityMps;

    // Invariant: valid == (status == Valid). The only field a control path may
    // act on for a velocity value.
    bool valid;

    // An explicit claim that no encoder change has been observed for at least
    // `noEdgeTimeoutMs`. Independent of `valid`: it is a statement about
    // observed non-change over a longer horizon than the window, not a
    // window-derived speed, so it never sets `valid`. A consumer that needs "is
    // it stopped" must read this field and must treat it as a candidate policy
    // until a physical test has measured it.
    //
    // It is never sufficient on its own to declare the vehicle stopped, and it
    // is never `SafetySystemState::atZeroSpeed` (SOFTWARE_ARCHITECTURE.md
    // section 13). Whoever produces that field must combine this claim with a
    // fresh valid estimate and with the interval over which the stop was
    // observed.
    bool zeroSpeedClaimed;

    // Why the answer is what it is.
    VelocityEstimateStatus status;

    // Clock reading of the update() call that produced this answer,
    // milliseconds. This is the stamp a consumer ages with `sampleAge()` /
    // `velocityReadiness()`: a last-valid estimate reused across cycles ages
    // instead of looking fresh forever. 0 when the answer carries neither a
    // number nor a zero-speed claim (a refusal has nothing to age). Not a
    // physical duration.
    uint32_t elapsedMs;

    // The span actually behind `velocityMps`, milliseconds. This is the realized
    // effective window: at least the configured `windowMs`, and less than
    // `windowMs` plus the spacing between the two oldest retained samples. 0 when
    // no number exists.
    uint32_t effectiveWindowMs;

    // The signed tick difference actually behind `velocityMps`. 0 when no number
    // exists. 64-bit so a full-range int32 reading pair inside one window cannot
    // overflow the difference.
    int64_t effectiveWindowTicks;
};

// ---------------------------------------------------------------------------
// VelocityEstimator
// ---------------------------------------------------------------------------
//
// Feeds on periodic snapshots - the same `(elapsedMs, motorEncoderTicks)` pair
// `SensorSnapshot` carries - and nothing else. It has no clock, so `nowMs` is an
// argument on every update and the class stays deterministic and host-testable.
//
// The zero value of a default-built config is `{0, 0}`, which fails validation:
// a forgotten configuration produces refusals, never numbers.
class VelocityEstimator {
public:
    // Compile-time capacity of the retained sample history. This is a memory
    // bound, not a calibration value. With samples P milliseconds apart, 16
    // samples can cover at most 15 * P of span, so an 80 ms window needs
    // P >= 5.33 ms and a 20 ms window needs P >= 1.33 ms. A control period
    // faster than the configured window can be covered at is reported as
    // WindowExceedsHistory rather than silently returning a shorter window, and
    // the estimator recovers by itself once the sample spacing is wide enough
    // again, because the refusal is a property of the retained history and not a
    // latched state.
    static constexpr uint8_t kMaxRetainedSamples = 16;

    explicit VelocityEstimator(const VelocityEstimatorConfig& config) : config_(config) {}

    const VelocityEstimatorConfig& config() const { return config_; }

    bool configValid() const { return validateVelocityEstimatorConfig(config_).ok; }

    // Consumes one snapshot and returns the estimator's answer now.
    //
    // `distancePerTickM` obeys the Odometry scale rule; a non-measured, non-
    // finite or non-positive scale produces a refusal with no number, because a
    // window-derived speed multiplied by a placeholder would look like a
    // measurement of the vehicle.
    VelocityEstimate update(uint32_t nowMs, int32_t ticks,
                            const Sourced<double>& distancePerTickM) {
        if (!configValid()) {
            return refuse(VelocityEstimateStatus::ConfigIsNotValid);
        }

        // The scale rule has exactly one implementation, in Odometry.h, so this
        // module cannot drift away from it.
        const OdometryEstimateStatus scaleStatus = distanceScaleStatus(distancePerTickM);
        if (scaleStatus != OdometryEstimateStatus::Valid) {
            return refuse(velocityStatusForScaleStatus(scaleStatus));
        }

        // A sample that does not advance the clock is refused, and it does not
        // enter the window: two samples at the same stamp would report an
        // infinite or zero speed depending on their ticks, neither of which was
        // measured. A rewound stamp is refused the same way through signed
        // difference, and a stamp close to the uint32 wraparound is accepted
        // because the difference stays small and positive.
        if (hasLastStamp_) {
            const int32_t sinceLastMs = static_cast<int32_t>(nowMs - lastStampMs_);
            if (sinceLastMs <= 0) {
                ++rejectedStamps_;
                return refuse(VelocityEstimateStatus::TimeDidNotAdvance);
            }
        }

        pushSample(nowMs, ticks);

        if (!hasPreviousTicks_) {
            // First accepted sample: it only establishes the tick reference and
            // starts the no-edge clock. Nothing has been observed to change yet.
            hasPreviousTicks_ = true;
            lastTickChangeMs_ = nowMs;
        } else if (ticks != previousTicks_) {
            lastTickChangeMs_ = nowMs;
            ++tickChanges_;
        }
        previousTicks_ = ticks;

        lastStampMs_ = nowMs;
        hasLastStamp_ = true;
        ++acceptedSamples_;

        // The explicit zero-speed claim is checked before window coverage: a
        // vehicle that has not moved since the estimator started never covers a
        // window with ticks, and "no change for noEdgeTimeoutMs" is the honest
        // statement about it. A zero-speed claim is never made on an unusable
        // scale (checked above), because a claim is a control-relevant statement
        // and must not rest on a placeholder.
        if (static_cast<uint32_t>(nowMs - lastTickChangeMs_) >= config_.noEdgeTimeoutMs) {
            return VelocityEstimate{0.0, false, true, VelocityEstimateStatus::NoEdgeTimeout, nowMs,
                                    0U, 0};
        }

        const uint32_t coveredMs = static_cast<uint32_t>(nowMs - sampleMsAt(0));
        if (coveredMs < config_.windowMs) {
            // Without a covered window there is no number at all: reporting the
            // tick difference over a shorter span would silently report a
            // different, unrequested effective window.
            return refuse(count_ >= kMaxRetainedSamples
                              ? VelocityEstimateStatus::WindowExceedsHistory
                              : VelocityEstimateStatus::WarmingUp);
        }

        // Tick differences are formed in 64 bits before conversion, so a
        // full-range int32 reading pair is not undefined behaviour, exactly as
        // in Odometry.h. `coveredMs` is at least 1 here because windowMs is.
        const int64_t windowTicks =
            static_cast<int64_t>(sampleTicksAt(count_ - 1)) - static_cast<int64_t>(sampleTicksAt(0));
        const double windowS = static_cast<double>(coveredMs) / 1000.0;
        const double velocityMps =
            static_cast<double>(windowTicks) * distancePerTickM.value / windowS;
        if (!std::isfinite(velocityMps)) {
            // Finite inputs, non-finite product: fail closed instead of
            // reporting an infinity or a NaN as a speed.
            return refuse(VelocityEstimateStatus::SpeedIsNotFinite);
        }

        return VelocityEstimate{velocityMps, true, false, VelocityEstimateStatus::Valid, nowMs,
                                coveredMs, windowTicks};
    }

    // Accepted samples since construction/reset().
    uint32_t acceptedSamples() const { return acceptedSamples_; }

    // Updates whose stamp did not advance. Expected when a caller repeats a
    // sample; it never enters the window.
    uint32_t rejectedStamps() const { return rejectedStamps_; }

    // Accepted samples whose tick count differed from the previous accepted
    // sample. A count that stops rising with the vehicle still moving is a
    // signal-quality question for bring-up (VERIFIED_STATE.md #5), not something
    // this layer adjudicates.
    uint32_t tickChanges() const { return tickChanges_; }

    // Samples currently retained to back the window.
    uint8_t retainedSamples() const { return count_; }

    // Returns the estimator to its post-construction state: no samples, no
    // reference, no claim, no counters. Nothing resumes automatically and no
    // previous window survives, so a run always starts from a fresh window.
    void reset() {
        head_ = 0;
        count_ = 0;
        hasLastStamp_ = false;
        lastStampMs_ = 0;
        hasPreviousTicks_ = false;
        previousTicks_ = 0;
        lastTickChangeMs_ = 0;
        acceptedSamples_ = 0;
        rejectedStamps_ = 0;
        tickChanges_ = 0;
    }

private:
    static VelocityEstimateStatus velocityStatusForScaleStatus(OdometryEstimateStatus status) {
        switch (status) {
            case OdometryEstimateStatus::Valid:
                return VelocityEstimateStatus::Valid;
            case OdometryEstimateStatus::ScaleIsNotMeasured:
                return VelocityEstimateStatus::ScaleIsNotMeasured;
            case OdometryEstimateStatus::ScaleIsNotFinite:
                return VelocityEstimateStatus::ScaleIsNotFinite;
            case OdometryEstimateStatus::ScaleIsNotPositive:
                return VelocityEstimateStatus::ScaleIsNotPositive;
            case OdometryEstimateStatus::OriginIsNotFinite:
            case OdometryEstimateStatus::DistanceIsNotFinite:
                break;
        }
        // Not reachable from distanceScaleStatus(), which only ever reports the
        // three scale verdicts or Valid. A new enumerator must be mapped
        // deliberately; until then it fails closed.
        return VelocityEstimateStatus::SpeedIsNotFinite;
    }

    // A refusal carries no stamp: it produced neither a number nor a claim, so
    // there is nothing for a consumer to age.
    static VelocityEstimate refuse(VelocityEstimateStatus status) {
        return VelocityEstimate{0.0, false, false, status, 0U, 0U, 0};
    }

    uint32_t sampleMsAt(uint8_t index) const {
        return sampleMs_[static_cast<uint8_t>((head_ + index) % kMaxRetainedSamples)];
    }

    int32_t sampleTicksAt(uint8_t index) const {
        return sampleTicks_[static_cast<uint8_t>((head_ + index) % kMaxRetainedSamples)];
    }

    void dropFront() {
        head_ = static_cast<uint8_t>((head_ + 1U) % kMaxRetainedSamples);
        --count_;
    }

    void pushSample(uint32_t nowMs, int32_t ticks) {
        if (count_ == kMaxRetainedSamples) {
            // Capacity bound. This cannot silently shorten the reported window:
            // the coverage check below reports WindowExceedsHistory when the
            // retained history is full and still short of windowMs.
            dropFront();
        }
        const uint8_t at = static_cast<uint8_t>((head_ + count_) % kMaxRetainedSamples);
        sampleMs_[at] = nowMs;
        sampleTicks_[at] = ticks;
        ++count_;

        // Keep the shortest span that still covers windowMs: drop a front
        // sample only while the next sample is still old enough to cover it.
        // Unsigned subtraction keeps this correct across uint32 wraparound.
        while (count_ > 1U) {
            const uint32_t spanWithoutFront = static_cast<uint32_t>(nowMs - sampleMsAt(1));
            if (spanWithoutFront >= config_.windowMs) {
                dropFront();
            } else {
                break;
            }
        }
    }

    VelocityEstimatorConfig config_;
    uint32_t sampleMs_[kMaxRetainedSamples] = {};
    int32_t sampleTicks_[kMaxRetainedSamples] = {};
    uint8_t head_ = 0;
    uint8_t count_ = 0;
    bool hasLastStamp_ = false;
    uint32_t lastStampMs_ = 0;
    bool hasPreviousTicks_ = false;
    int32_t previousTicks_ = 0;
    uint32_t lastTickChangeMs_ = 0;
    uint32_t acceptedSamples_ = 0;
    uint32_t rejectedStamps_ = 0;
    uint32_t tickChanges_ = 0;
};

// ---------------------------------------------------------------------------
// Whether an estimate may be used for control right now
// ---------------------------------------------------------------------------
//
// The velocity path needs the same three facts the position path decides in
// `odometryReadiness()`: whether an estimate exists at all, whether it is valid,
// and whether it is still fresh. Keeping the rule here, next to the estimate it
// judges, is what stops a last-valid estimate reused across cycles from being
// treated as a current measurement: the estimate carries its own production
// stamp (`elapsedMs`), the caller ages it with `sampleAge()`, and this function
// turns the pair into one verdict. The vehicle layer maps that verdict onto the
// regulator's `measurementUsable` argument instead of passing a bare boolean.

enum class VelocityUsability : uint8_t {
    // An estimate exists, it is valid, and its stamp is within the caller's
    // limit. This is the only usable reason, and the zero value.
    Usable = 0,
    // No estimator answer has been produced yet (for example before the first
    // control cycle). There is nothing to judge.
    NoEstimate = 1,
    // An estimate exists but it is not valid - including a zero-speed claim,
    // which is an explicit statement about observed non-change and never a
    // measurement of zero speed. A fresh stamp does not fix that.
    EstimateInvalid = 2,
    // A valid estimate exists but its stamp is older than the caller's limit,
    // for example because the last-valid answer was reused across cycles.
    SampleStale = 3,
};

// The one answer a consumer needs: may this estimate be used for control, why
// not when it may not, and how old the estimate was.
//
// Invariant, pinned by every test: usable == (reason == Usable).
struct VelocityReadiness {
    constexpr VelocityReadiness(bool usable_, VelocityUsability reason_, uint32_t ageMs_)
        : usable(usable_), reason(reason_), ageMs(ageMs_) {}

    bool usable;
    VelocityUsability reason;

    // Age of the estimate's stamp in milliseconds, as computed by sampleAge().
    // Zero when there is no estimate.
    uint32_t ageMs;
};

static_assert(!std::is_default_constructible<VelocityReadiness>::value,
              "no silent defaults; a readiness answer must state its reason");

// Combines the three independent facts a control loop needs: whether an
// estimate exists at all, whether it is valid, and whether it is fresh.
//
// `age` is ignored when hasEstimate is false; the caller may pass a
// default-built SampleAge, which is never fresh. The age limit itself is caller
// policy (see sampleAge()): no staleness timeout lives in this layer, because
// none has been measured.
//
// Check order is deliberate and matches `odometryReadiness()`: existence first
// (there is nothing to judge yet), then estimate validity (an invalid estimate
// is a correctness problem, not a timing problem), then age. A zero-speed claim
// therefore reports EstimateInvalid rather than being mistaken for a usable
// zero, and a stale estimate reports SampleStale rather than being mistaken for
// a fresh one.
inline VelocityReadiness velocityReadiness(bool hasEstimate, const VelocityEstimate& estimate,
                                           const SampleAge& age) {
    if (!hasEstimate) {
        return VelocityReadiness(false, VelocityUsability::NoEstimate, 0U);
    }
    if (!estimate.valid) {
        return VelocityReadiness(false, VelocityUsability::EstimateInvalid, age.ageMs);
    }
    if (!age.fresh) {
        return VelocityReadiness(false, VelocityUsability::SampleStale, age.ageMs);
    }
    return VelocityReadiness(true, VelocityUsability::Usable, age.ageMs);
}

// Pin the "no silent answer" contract: an estimate without a stated reason must
// not be constructible, and the permissive zero values must stay pinned so a
// future reorder cannot remap them.
static_assert(!std::is_default_constructible<VelocityEstimate>::value,
              "an estimate must never default-construct into a valid-looking value");
static_assert(static_cast<int>(VelocityEstimateStatus::Valid) == 0,
              "the valid status must stay the zero value");
static_assert(static_cast<int>(VelocityUsability::Usable) == 0,
              "the usable verdict must stay the zero value");

}  // namespace ev
