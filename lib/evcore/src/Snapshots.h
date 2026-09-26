#pragma once

#include <cstdint>

namespace ev {

// Raw inputs exactly as they were at one instant. No derived values and no unit
// conversion happen here.
struct SensorSnapshot {
    constexpr SensorSnapshot(uint32_t elapsedMs_, int32_t motorEncoderTicks_,
                             uint16_t batteryMv_, bool batteryReadingValid_)
        : elapsedMs(elapsedMs_),
          motorEncoderTicks(motorEncoderTicks_),
          batteryMv(batteryMv_),
          batteryReadingValid(batteryReadingValid_) {}

    // Clock reading from millis() when the snapshot was taken, milliseconds.
    uint32_t elapsedMs;

    // Signed cumulative motor-encoder count. The sign convention is UNVERIFIED
    // (VERIFIED_STATE.md #4).
    int32_t motorEncoderTicks;

    // Battery voltage in millivolts. Only meaningful when batteryReadingValid
    // is true; battery sensing is not verified yet.
    uint16_t batteryMv;
    bool batteryReadingValid;
};

// Motion estimate derived from a sensor snapshot and the vehicle calibration.
// A snapshot computed from provisional calibration is not usable for control,
// so validity travels with the data instead of being assumed by the consumer.
struct OdometrySnapshot {
    constexpr OdometrySnapshot(uint32_t elapsedMs_, double distanceM_, double velocityMps_,
                               bool valid_)
        : elapsedMs(elapsedMs_),
          distanceM(distanceM_),
          velocityMps(velocityMps_),
          valid(valid_) {}

    // Clock reading matching the source SensorSnapshot, milliseconds.
    uint32_t elapsedMs;

    // Distance travelled from the run origin, metres, signed by direction.
    double distanceM;

    // Estimated speed, metres per second, signed by direction.
    //
    // Deliberately unfilled by the odometry layer (Odometry.h), which owns no
    // velocity arithmetic. Speed is owned by `VelocityEstimate` and
    // `velocityReadiness()` in VelocityEstimator.h, and it must not be copied
    // into this field as a convenient second copy: control and telemetry read
    // the estimator's answer together with its freshness verdict, and a second
    // velocity value with no owner is how two logs start disagreeing. A producer
    // leaves this 0.0 unless a later contract explicitly nominates it.
    double velocityMps;

    // False when the estimate rests on missing sensor data or on provisional
    // calibration. Consumers must not use an invalid estimate for control.
    bool valid;
};

// ---------------------------------------------------------------------------
// Sample age and freshness
// ---------------------------------------------------------------------------
//
// How old a reading is, and whether that is acceptable, is the same question
// for every snapshot, so the rule lives here once instead of being re-derived
// by each consumer. Motion and safety both need it: a correct measurement that
// stopped updating is not a measurement.
//
// There is deliberately no built-in or default age limit. How old a reading may
// be before it is useless depends on the control-loop period and on what is
// being measured, and neither has been measured yet, so the limit is supplied
// by the caller every time (the same approach as
// SafetySystemState::requiredDataFresh, which is likewise the caller's verdict
// rather than a number this layer invents).
//
// The zero value of SampleAge is {0, false}: an age that was never computed
// reports not fresh, so a forgotten call fails closed instead of looking like a
// brand-new reading.
struct SampleAge {
    // Whole milliseconds between the sample's stamp and nowMs, taken from a
    // snapshot's `elapsedMs`. Computed with unsigned subtraction, so it stays
    // correct across millis() wraparound at ~49.7 days
    // (SOFTWARE_ARCHITECTURE.md section 4).
    uint32_t ageMs;

    // True when ageMs <= maxAgeMs.
    bool fresh;
};

// Ages a sample stamped at `sampleMs` against `nowMs`, keeping it fresh up to
// `maxAgeMs` inclusive.
//
// A stamp from the future (sampleMs ahead of nowMs) is never fresh: unsigned
// subtraction makes the difference enormous, which is the safe direction. That
// covers a clock that was reset and a stamp taken from a different time base.
//
// A maxAgeMs of 0 means only a sample stamped in this exact millisecond is
// fresh; it does not mean "always fresh".
inline SampleAge sampleAge(uint32_t nowMs, uint32_t sampleMs, uint32_t maxAgeMs) {
    const uint32_t ageMs = nowMs - sampleMs;
    return SampleAge{ageMs, ageMs <= maxAgeMs};
}

}  // namespace ev
