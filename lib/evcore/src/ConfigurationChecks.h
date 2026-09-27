#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// The configuration-build check report
// ---------------------------------------------------------------------------
//
// Every check the vehicle layer must pass before anything may be armed
// (`SOFTWARE_ARCHITECTURE.md` sections 13, 14 and 15), each named, each
// reported. The checks themselves are production functions that live with the
// modules they check; this header owns the *report* - which checks exist, what
// they are called, and how a refusal is attributed - so the report can be
// reasoned about, echoed to an operator and tested without a vehicle.
//
// Two rules are the reason this is a type and not a `bool`:
//
// * **No single boolean may summarize them.** A refused arm has to be
//   attributable to the fact that failed, which is what `firstFailure()` and
//   `failureMask()` exist for.
// * **The vocabulary is frozen once a log can contain it.** `Name()` below is
//   the wire spelling of each check, used by the operator responses and by the
//   telemetry schema, so it may gain entries but must not rename one.
//
// The order of the enumerators is the order of the checks as they are reported,
// which is the order in `configurationBuildChecks()` in the vehicle layer: the
// most fundamental reason first (an unannounced run, then calibration, then the
// limits and configurations the chain is built from).
enum class ConfigurationCheckId : uint8_t {
    ConfigurationValid = 0,   // an announced RunConfig passed rule validation
    CalibrationMeasured,      // VehicleCalibration::isFullyMeasured()
    EffortCeilingsAgree,      // regulator ceiling == safety ceiling
    CommandPolarityAgrees,    // profile command direction == regulator effort mode
    TickPolarityAgrees,       // measured tick direction == command direction
    StopEvidenceSpansReversalStop,
    StopEvidenceSpansFinalStop,
    SafetyLimitsValid,        // validateSafetyLimits()
    ControllerConfigValid,    // validateVelocityControllerConfig()
    EstimatorConfigValid,     // validateVelocityEstimatorConfig()
    ProfileLimitsValid,       // validateMotionProfileLimits(), both directions
    PhaseConfigValid,         // validateRunPhaseConfig()
    ReversalConfigValid,      // validateReversalConfig()
    RunPolicyUsable,          // the run policy's own configUsable()

    // Not a check: the count, and the value `firstFailure()` returns when every
    // check passed. Never used as an index into the report.
    Count,
};

constexpr uint8_t kConfigurationCheckCount = static_cast<uint8_t>(ConfigurationCheckId::Count);

// The stable wire spelling of a check. `Count` ("no failing check") spells as
// `none`. These strings are what a log and an operator see; treat them as part
// of the telemetry schema.
inline const char* configurationCheckName(ConfigurationCheckId id) {
    switch (id) {
        case ConfigurationCheckId::ConfigurationValid:
            return "ConfigurationValid";
        case ConfigurationCheckId::CalibrationMeasured:
            return "CalibrationMeasured";
        case ConfigurationCheckId::EffortCeilingsAgree:
            return "EffortCeilingsAgree";
        case ConfigurationCheckId::CommandPolarityAgrees:
            return "CommandPolarityAgrees";
        case ConfigurationCheckId::TickPolarityAgrees:
            return "TickPolarityAgrees";
        case ConfigurationCheckId::StopEvidenceSpansReversalStop:
            return "StopEvidenceSpansReversalStop";
        case ConfigurationCheckId::StopEvidenceSpansFinalStop:
            return "StopEvidenceSpansFinalStop";
        case ConfigurationCheckId::SafetyLimitsValid:
            return "SafetyLimitsValid";
        case ConfigurationCheckId::ControllerConfigValid:
            return "ControllerConfigValid";
        case ConfigurationCheckId::EstimatorConfigValid:
            return "EstimatorConfigValid";
        case ConfigurationCheckId::ProfileLimitsValid:
            return "ProfileLimitsValid";
        case ConfigurationCheckId::PhaseConfigValid:
            return "PhaseConfigValid";
        case ConfigurationCheckId::ReversalConfigValid:
            return "ReversalConfigValid";
        case ConfigurationCheckId::RunPolicyUsable:
            return "RunPolicyUsable";
        case ConfigurationCheckId::Count:
            return "none";
    }
    return "none";
}

// One field per check, in the enumerator order above. A default-constructed
// report is the report of a vehicle that has passed nothing, which is the safe
// direction: every field false means no arming.
struct ConfigurationBuildChecks {
    bool configurationValid = false;
    bool calibrationMeasured = false;
    bool effortCeilingsAgree = false;
    bool commandPolarityAgrees = false;
    bool tickPolarityAgrees = false;
    bool stopEvidenceSpansReversalStop = false;
    bool stopEvidenceSpansFinalStop = false;
    bool safetyLimitsValid = false;
    bool controllerConfigValid = false;
    bool estimatorConfigValid = false;
    bool profileLimitsValid = false;
    bool phaseConfigValid = false;
    bool reversalConfigValid = false;
    bool runPolicyUsable = false;

    // The verdict, by id. `Count` and an out-of-range id are false: an unknown
    // check is not a passed check.
    bool passed(ConfigurationCheckId id) const {
        switch (id) {
            case ConfigurationCheckId::ConfigurationValid:
                return configurationValid;
            case ConfigurationCheckId::CalibrationMeasured:
                return calibrationMeasured;
            case ConfigurationCheckId::EffortCeilingsAgree:
                return effortCeilingsAgree;
            case ConfigurationCheckId::CommandPolarityAgrees:
                return commandPolarityAgrees;
            case ConfigurationCheckId::TickPolarityAgrees:
                return tickPolarityAgrees;
            case ConfigurationCheckId::StopEvidenceSpansReversalStop:
                return stopEvidenceSpansReversalStop;
            case ConfigurationCheckId::StopEvidenceSpansFinalStop:
                return stopEvidenceSpansFinalStop;
            case ConfigurationCheckId::SafetyLimitsValid:
                return safetyLimitsValid;
            case ConfigurationCheckId::ControllerConfigValid:
                return controllerConfigValid;
            case ConfigurationCheckId::EstimatorConfigValid:
                return estimatorConfigValid;
            case ConfigurationCheckId::ProfileLimitsValid:
                return profileLimitsValid;
            case ConfigurationCheckId::PhaseConfigValid:
                return phaseConfigValid;
            case ConfigurationCheckId::ReversalConfigValid:
                return reversalConfigValid;
            case ConfigurationCheckId::RunPolicyUsable:
                return runPolicyUsable;
            case ConfigurationCheckId::Count:
                break;
        }
        return false;
    }

    // Sets one check's verdict by id. `Count` and an out-of-range id change
    // nothing, for the same reason `passed()` reports false for them: they are
    // not checks. The mapping lives here so no caller writes a second switch that
    // could disagree with `passed()`.
    void setPassed(ConfigurationCheckId id, bool value) {
        switch (id) {
            case ConfigurationCheckId::ConfigurationValid:
                configurationValid = value;
                return;
            case ConfigurationCheckId::CalibrationMeasured:
                calibrationMeasured = value;
                return;
            case ConfigurationCheckId::EffortCeilingsAgree:
                effortCeilingsAgree = value;
                return;
            case ConfigurationCheckId::CommandPolarityAgrees:
                commandPolarityAgrees = value;
                return;
            case ConfigurationCheckId::TickPolarityAgrees:
                tickPolarityAgrees = value;
                return;
            case ConfigurationCheckId::StopEvidenceSpansReversalStop:
                stopEvidenceSpansReversalStop = value;
                return;
            case ConfigurationCheckId::StopEvidenceSpansFinalStop:
                stopEvidenceSpansFinalStop = value;
                return;
            case ConfigurationCheckId::SafetyLimitsValid:
                safetyLimitsValid = value;
                return;
            case ConfigurationCheckId::ControllerConfigValid:
                controllerConfigValid = value;
                return;
            case ConfigurationCheckId::EstimatorConfigValid:
                estimatorConfigValid = value;
                return;
            case ConfigurationCheckId::ProfileLimitsValid:
                profileLimitsValid = value;
                return;
            case ConfigurationCheckId::PhaseConfigValid:
                phaseConfigValid = value;
                return;
            case ConfigurationCheckId::ReversalConfigValid:
                reversalConfigValid = value;
                return;
            case ConfigurationCheckId::RunPolicyUsable:
                runPolicyUsable = value;
                return;
            case ConfigurationCheckId::Count:
                break;
        }
    }

    // True only when every check passed. There is deliberately no partial
    // "close enough": this is the gate a deliberate arming decision reads.
    bool allPass() const {
        return configurationValid && calibrationMeasured && effortCeilingsAgree &&
               commandPolarityAgrees && tickPolarityAgrees && stopEvidenceSpansReversalStop &&
               stopEvidenceSpansFinalStop && safetyLimitsValid && controllerConfigValid &&
               estimatorConfigValid && profileLimitsValid && phaseConfigValid &&
               reversalConfigValid && runPolicyUsable;
    }

    bool anyFailure() const { return !allPass(); }

    // The most fundamental failing check, or `Count` when every check passed.
    // This is what an operator refusal names and what the telemetry sample
    // carries, so a refused drive has one attributable reason rather than a
    // list the reader has to interpret.
    ConfigurationCheckId firstFailure() const {
        for (uint8_t index = 0U; index < kConfigurationCheckCount; ++index) {
            const ConfigurationCheckId id = static_cast<ConfigurationCheckId>(index);
            if (!passed(id)) {
                return id;
            }
        }
        return ConfigurationCheckId::Count;
    }

    // Every failing check as a bitmask, bit `n` set for enumerator value `n`.
    // Fourteen checks fit in a `uint16_t`; the static assertion below keeps that
    // true if a check is added later.
    uint16_t failureMask() const {
        uint16_t mask = 0U;
        for (uint8_t index = 0U; index < kConfigurationCheckCount; ++index) {
            const ConfigurationCheckId id = static_cast<ConfigurationCheckId>(index);
            if (!passed(id)) {
                mask = static_cast<uint16_t>(mask | (static_cast<uint16_t>(1U) << index));
            }
        }
        return mask;
    }

    uint8_t failureCount() const {
        uint8_t count = 0U;
        for (uint8_t index = 0U; index < kConfigurationCheckCount; ++index) {
            if (!passed(static_cast<ConfigurationCheckId>(index))) {
                ++count;
            }
        }
        return count;
    }
};

static_assert(kConfigurationCheckCount <= 16U,
              "ConfigurationBuildChecks::failureMask() must be able to hold every check");

}  // namespace ev
