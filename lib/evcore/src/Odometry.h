#pragma once

#include "Snapshots.h"
#include "Units.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace ev {

// ---------------------------------------------------------------------------
// Tick -> displacement conversion - pure logic
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One conversion: a signed encoder-tick difference times an explicitly
//   supplied effective distance per tick, in metres per tick.
// * Pure, deterministic logic: no Arduino header, no pin read, no interrupt,
//   no clock, no allocation.
// * The distance scale enters as an argument every time. This module owns no
//   scale, no wheel circumference, no gear ratio and no counts-per-revolution
//   number. Those facts are UNVERIFIED (VERIFIED_STATE.md #4, #5, #6), and a
//   theoretical wheel circumference must never be treated as the truth here.
//   The value belongs in configuration (`VehicleCalibration`); the arithmetic
//   belongs here.
//
// What this module is not
//
// * It is not velocity estimation and it does not decide "at zero speed".
//   Both need a time base and a measured rollout, neither of which exists, so
//   `OdometrySnapshot::velocityMps` is deliberately left unfilled by this
//   layer.
// * It is not the sampling layer (pin reads, interrupts) and it does not decide
//   what the run origin is. It converts ticks it is handed.
// * It does not give a ground/dead-wheel encoder any authority. The encoder
//   assumed here is the motor encoder, and which count direction is physical
//   forward travel is UNVERIFIED (VERIFIED_STATE.md #4); the sign of the result
//   inherits that uncertainty and is never asserted to be forward.
// * It does not decide whether the vehicle may be armed. It answers only
//   whether a computed estimate may be *used for control*, which a later motion
//   or safety layer consumes.

// Why a tick-derived estimate can or cannot be used.
//
// Valid is deliberately the zero value, which is why nothing in this module
// hands back a status that no check produced: both result structs below have no
// default constructor, so a permissive status can never arrive by accident.
enum class OdometryEstimateStatus : uint8_t {
    // The scale is ValueSource::Measured, finite and positive, and the computed
    // value is finite. This is the only status under which valid is true.
    Valid = 0,
    // The scale is numerically usable but is not a measured fact (Provisional or
    // RuleDerived). A distance is still computed so a bench diagnostic can
    // display it, but the estimate is marked invalid: a placeholder must never
    // be treated as a vehicle fact and must never reach control.
    ScaleIsNotMeasured = 1,
    // The scale is NaN or an infinity. No distance exists, so none is reported.
    ScaleIsNotFinite = 2,
    // The scale is zero or negative. A zero scale is refused even though it
    // would multiply out to a plausible-looking zero, and a negative scale is
    // refused because it would secretly invert the tick sign convention that
    // this module does not own.
    ScaleIsNotPositive = 3,
    // The supplied run-origin position is NaN or an infinity.
    OriginIsNotFinite = 4,
    // The arithmetic produced a non-finite value: for example an enormous but
    // finite scale multiplied by a large tick delta. The value is not reported.
    DistanceIsNotFinite = 5,
};

// The outcome of one conversion. `valid` is the only field a control path may
// act on; `status` explains a refusal, and `distanceM` is a diagnostic value in
// the ScaleIsNotMeasured case and 0.0 whenever no meaningful number exists.
//
// Invariant, pinned by every test: valid == (status == Valid).
struct OdometryEstimate {
    constexpr OdometryEstimate(double distanceM_, bool valid_, OdometryEstimateStatus status_)
        : distanceM(distanceM_), valid(valid_), status(status_) {}

    // Signed travel, metres, in the sign convention of the encoder ticks: a
    // positive tick delta gives a positive distance. This is a direction
    // convention inherited from the counter, not a claim about forward travel
    // (VERIFIED_STATE.md #4).
    double distanceM;

    // True only for a measured, finite, positive scale and a finite result.
    // False means "do not use this for control", not "this number is narrow
    // enough to be harmless".
    bool valid;

    OdometryEstimateStatus status;
};

// A value without a source is impossible to write anywhere else in the pure
// core; the same holds for an estimate, which must never be conjured up as
// valid-with-no-reason.
static_assert(!std::is_default_constructible<OdometryEstimate>::value,
              "no silent defaults; an estimate must state how it was produced");

// The status of a distance-per-tick scale on its own, with no ticks involved.
// Exposed because a configuration/arming check wants the same answer this
// module uses internally.
//
// Order of checks: numerics first, because a non-finite or non-positive scale
// has no number at all, then provenance, because a numeric-but-unmeasured scale
// still produces a diagnostic value. This function returns Valid,
// ScaleIsNotMeasured, ScaleIsNotFinite or ScaleIsNotPositive; it never returns
// one of the two result-dependent statuses.
inline OdometryEstimateStatus distanceScaleStatus(const Sourced<double>& distancePerEncoderTickM) {
    if (!std::isfinite(distancePerEncoderTickM.value)) {
        return OdometryEstimateStatus::ScaleIsNotFinite;
    }
    // Written as !(x > 0) so a zero scale and a negative scale are both refused
    // by the same check.
    if (!(distancePerEncoderTickM.value > 0.0)) {
        return OdometryEstimateStatus::ScaleIsNotPositive;
    }
    if (!distancePerEncoderTickM.isMeasured()) {
        return OdometryEstimateStatus::ScaleIsNotMeasured;
    }
    return OdometryEstimateStatus::Valid;
}

// Signed distance travelled between two cumulative encoder readings, in metres.
//
// Only the difference of the two readings is used, so any origin tick value is
// legal and a run's origin tick reading does not have to be zero.
inline OdometryEstimate distanceTravelledM(int32_t originTicks, int32_t currentTicks,
                                           const Sourced<double>& distancePerEncoderTickM) {
    const OdometryEstimateStatus scaleStatus = distanceScaleStatus(distancePerEncoderTickM);
    if (scaleStatus == OdometryEstimateStatus::ScaleIsNotFinite ||
        scaleStatus == OdometryEstimateStatus::ScaleIsNotPositive) {
        // There is no number to report. Reporting 0.0 here is deliberate: a
        // distance of exactly zero is a value a consumer could mistake for a
        // real measurement, but it is also the only value that cannot be
        // mistaken for movement.
        return OdometryEstimate(0.0, false, scaleStatus);
    }

    // The difference is formed in 64 bits before the subtraction. In 32 bits a
    // full-range pair such as INT32_MIN to INT32_MAX is undefined behaviour, and
    // a large accumulated count is exactly what a long run produces. Any int32
    // pair is exact in int64, and that int64 value is itself exact as a double
    // (well inside 2^53).
    const int64_t tickDelta =
        static_cast<int64_t>(currentTicks) - static_cast<int64_t>(originTicks);
    const double distanceM = static_cast<double>(tickDelta) * distancePerEncoderTickM.value;
    if (!std::isfinite(distanceM)) {
        // A finite scale times a large delta can still overflow to an infinity.
        return OdometryEstimate(0.0, false, OdometryEstimateStatus::DistanceIsNotFinite);
    }

    return OdometryEstimate(distanceM, scaleStatus == OdometryEstimateStatus::Valid, scaleStatus);
}

// Position on the run axis, in metres, measured from the run origin: the origin
// position plus the travel since the origin tick reading.
//
// The origin position is an argument rather than a constant. Where the vehicle
// starts is the vehicle layer's business (the Start Point, a post-bottle
// reversal turn-around, a re-zeroed origin after a stall), and this module has
// no opinion on it.
//
// Sign reconciliation - a documented gap, and the vehicle layer's to own.
// `originPositionM` is a position on the run axis, a physical convention, while
// the travel added to it inherits the tick-counting convention (VERIFIED_STATE.md
// #4). This module applies no polarity, so the two conventions are reconciled
// nowhere yet: the vehicle layer must apply the measured polarity fact once,
// when it converts between run-axis distances and tick travel, rather than
// working in whichever convention a leg happens to hand it. Until that polarity
// is measured, this function must not be used to combine a physical origin with
// tick-derived travel, or the forward and reverse legs will disagree about which
// sign increases position - exactly the mix-up `remainingDistanceM` in
// MotionProfile.h warns about from the other side.
inline OdometryEstimate positionFromOriginM(double originPositionM, int32_t originTicks,
                                            int32_t currentTicks,
                                            const Sourced<double>& distancePerEncoderTickM) {
    if (!std::isfinite(originPositionM)) {
        return OdometryEstimate(0.0, false, OdometryEstimateStatus::OriginIsNotFinite);
    }

    const OdometryEstimate travelled =
        distanceTravelledM(originTicks, currentTicks, distancePerEncoderTickM);
    if (travelled.status == OdometryEstimateStatus::ScaleIsNotFinite ||
        travelled.status == OdometryEstimateStatus::ScaleIsNotPositive ||
        travelled.status == OdometryEstimateStatus::DistanceIsNotFinite) {
        // No meaningful travel was produced, so no position may be reported
        // either. A partial answer here would be worse than none: the origin
        // alone looks like a legitimate "has not moved yet" reading.
        return OdometryEstimate(0.0, false, travelled.status);
    }

    const double positionM = originPositionM + travelled.distanceM;
    if (!std::isfinite(positionM)) {
        return OdometryEstimate(0.0, false, OdometryEstimateStatus::DistanceIsNotFinite);
    }

    return OdometryEstimate(positionM, travelled.valid, travelled.status);
}

// ---------------------------------------------------------------------------
// Whether an estimate may be used for control right now
// ---------------------------------------------------------------------------
//
// A distance this module considers correct is still not usable if the snapshot
// it came from is stale, and an estimate that was never produced is not usable
// at all. Motion and safety both need that answer as one fact, so it is decided
// once, here, instead of being re-derived by each consumer.

enum class OdometryUsability : uint8_t {
    // A sample exists, its estimate is valid, and its age is within the
    // caller's limit. This is the only usable reason, and the zero value.
    Usable = 0,
    // No odometry sample has been produced yet (for example before the first
    // control cycle). There is nothing to judge.
    NoSample = 1,
    // A sample exists but its estimate is invalid, most importantly when it
    // rests on a provisional calibration. A fresh sample does not fix that.
    EstimateInvalid = 2,
    // A valid estimate exists but the sample is older than the caller's limit.
    SampleStale = 3,
};

// The one answer a consumer needs: may this estimate be used for control, why
// not when it may not, and how old the sample was.
//
// Invariant, pinned by every test: usable == (reason == Usable).
struct OdometryReadiness {
    constexpr OdometryReadiness(bool usable_, OdometryUsability reason_, uint32_t ageMs_)
        : usable(usable_), reason(reason_), ageMs(ageMs_) {}

    bool usable;
    OdometryUsability reason;

    // Age of the sample in milliseconds, as computed by sampleAge(). Zero when
    // there is no sample.
    uint32_t ageMs;
};

static_assert(!std::is_default_constructible<OdometryReadiness>::value,
              "no silent defaults; a readiness answer must state its reason");

// Combines the three independent facts a control loop needs: whether a sample
// exists at all, whether its estimate is valid, and whether it is fresh.
//
// `age` is ignored when hasSample is false; the caller may pass a default-built
// SampleAge, which is never fresh. The age limit itself is caller policy
// (see sampleAge()): no staleness timeout and no speed threshold live in the
// pure core, because neither has been measured.
//
// Check order is deliberate and documented: existence first (there is nothing
// to judge yet), then estimate validity (a provisional calibration is a
// correctness problem, not a timing problem), then age. A caller therefore
// learns the most fundamental problem first, and a stale provisional estimate
// reports EstimateInvalid rather than SampleStale.
inline OdometryReadiness odometryReadiness(bool hasSample, const OdometryEstimate& estimate,
                                           const SampleAge& age) {
    if (!hasSample) {
        return OdometryReadiness(false, OdometryUsability::NoSample, 0U);
    }
    if (!estimate.valid) {
        return OdometryReadiness(false, OdometryUsability::EstimateInvalid, age.ageMs);
    }
    if (!age.fresh) {
        return OdometryReadiness(false, OdometryUsability::SampleStale, age.ageMs);
    }
    return OdometryReadiness(true, OdometryUsability::Usable, age.ageMs);
}

}  // namespace ev
