#include <cstdint>
#include <cstring>

#include <unity.h>

#include <BoundedText.h>
#include <ControlScheduler.h>
#include <OperatorPolicy.h>
#include <OperatorResponse.h>
#include <SafetyAuthority.h>
#include <SafetyLimits.h>
#include <SerialCommandParser.h>
#include <TelemetryFormat.h>
#include <TelemetryQueue.h>
#include <Units.h>

// The scenario the operator interface's contract rests on: **ignoring the serial
// port cannot change what the vehicle does.**
//
// The scenario runs one deterministic control loop twice. In both runs the loop is
// identical: a 50 Hz schedule, a scripted request each tick, and one
// `SafetyAuthority` verdict per request, recorded. In the second run the serial
// hierarchy the operator interface is built from is also driven, against the worst
// case a host can present:
//
//   * nothing is ever drained, so the telemetry lanes fill and drop;
//   * every tick is formatted into the wire format, events are detected and
//     queued, and a scripted command stream is read, parsed, admitted or refused,
//     and answered into a bounded buffer;
//   * commands are typed while the run is under way - the ones that would change
//     run-control state are refused by the state restriction.
//
// The assertions are that the two recorded traces are identical and that every
// verdict is still a refusal. A pass means the consumers are consumers: no
// telemetry path and no refused command can reach the control path or the safety
// boundary. It says nothing about the vehicle - there is no vehicle here, no
// motor, and no serial port, because a host test has none of those.

void setUp(void) {}
void tearDown(void) {}

namespace {

const uint32_t kPeriodMs = 20U;    // 50 Hz, the planned cadence
const uint16_t kTickCount = 60U;   // 1.2 s of scripted run
const uint16_t kControlCadenceHz = 50U;

// The scripted phase each tick is in: a run announced, armed and then under way,
// so the state restriction has every kind of state to act on - and the commands
// typed after the start trigger must be refused by it.
ev::RunPhase phaseForTick(uint16_t tick) {
    if (tick < 5U) {
        return ev::RunPhase::Idle;
    }
    if (tick < 10U) {
        return ev::RunPhase::ParameterValid;
    }
    if (tick < 15U) {
        return ev::RunPhase::Armed;
    }
    if (tick < 40U) {
        return ev::RunPhase::Approach;
    }
    if (tick < 50U) {
        return ev::RunPhase::Push;
    }
    return ev::RunPhase::Done;
}

// The scripted request each tick presents. What matters for this test is only that
// the verdicts it produces are the same with and without the serial hierarchy.
ev::MotorEffort requestForTick(uint16_t tick) {
    if (tick % 4U == 0U) {
        return ev::MotorEffort(ev::MotorMode::Disabled, 0U);
    }
    if (tick % 4U == 1U) {
        return ev::MotorEffort(ev::MotorMode::Coast, 0U);
    }
    if (tick % 4U == 2U) {
        return ev::MotorEffort(ev::MotorMode::Brake, 0U);
    }
    return ev::MotorEffort(ev::MotorMode::DriveForward, static_cast<uint16_t>(400U + tick));
}

// The scripted command stream, one line on the ticks it names. Between them are
// the legitimate commands (inspection, telemetry control, `disarm`), the ones the
// state restriction refuses, and two the parser refuses.
const char* commandForTick(uint16_t tick) {
    switch (tick) {
        case 3U:
            return "status";
        case 6U:
            return "config set level=state";
        case 8U:
            return "config set distance=7.5";
        case 11U:
            return "config show";
        case 14U:
            return "telemetry rate 10";
        case 17U:
            return "config set time=15";
        case 20U:
            return "announce";
        case 24U:
            return "arm";
        case 28U:
            return "config clear";
        case 32U:
            return "reset";
        case 36U:
            return "telemetry status";
        case 40U:
            return "bogus command";
        case 44U:
            return "config set distance=abc";
        case 48U:
            return "disarm";
        case 52U:
            return "checks";
        case 56U:
            return "telemetry off";
        default:
            return nullptr;
    }
}

// One recorded verdict plus the command it produced: the control path's output,
// and the whole point of the test is that it does not depend on who is reading the
// serial port.
struct RecordedVerdict {
    ev::AuthzOutcome outcome;
    ev::MotorMode mode;
    uint16_t permille;
};

struct ControlRun {
    RecordedVerdict verdicts[kTickCount];
    uint32_t ticks;
    uint32_t droppedDeadlines;
};

// The consumer half: what the operator interface did while the control loop ran.
struct ConsumerRun {
    uint32_t ticksWithSampleDue;
    uint32_t samplesQueued;
    uint32_t samplesDropped;
    uint32_t samplesFormatted;
    uint32_t eventsFormatted;
    uint32_t responsesFormatted;
    uint32_t commandsAdmitted;
    uint32_t commandsRefused;
    uint32_t formatFailures;
    // The peak the queue reached, not its final value: `telemetry off` at the end
    // of the run empties the lanes and clears the counters by design, so the
    // evidence that the stalled host lost records has to be taken while it was
    // still true.
    ev::TelemetryDrops drops;
    char lastResponse[ev::kSerialLineBufferBytes];
};

// Runs the scenario. `withConsumers` adds the serial hierarchy beside the control
// loop and changes nothing else: same clock, same requests, same authority.
ControlRun runScenario(bool withConsumers, ConsumerRun* consumers) {
    ev::ControlScheduler scheduler(kPeriodMs);
    ev::SafetyAuthority authority;

    // A provisional limit set, exactly like the firmware's own: nothing measured,
    // so no powered motion may be authorized from it. That is the state this test
    // asserts stays true with the whole consumer hierarchy running.
    // `Sourced` has no default constructor, deliberately: an unmeasured limit has
    // to be written out as unmeasured.
    const ev::SafetyLimits limits{
        ev::Sourced<uint16_t>(0U, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.0, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.0, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.0, ev::ValueSource::Provisional),
        ev::Sourced<double>(0.0, ev::ValueSource::Provisional),
    };

    // The consumer hierarchy, constructed in both runs so that neither run's
    // construction differs - only whether it is driven.
    ev::TelemetrySession session;
    ev::TelemetryEventDetector detector;
    ev::RunConfigFields heldConfig;
    char responseBuffer[ev::kSerialLineBufferBytes] = {0};
    char recordBuffer[ev::kSerialLineBufferBytes] = {0};
    char commandBuffer[ev::kCommandLineCapacity + 1U];
    ev::CommandLineReader reader(commandBuffer, ev::kCommandLineCapacity);

    ControlRun control;
    control.ticks = 0U;
    control.droppedDeadlines = 0U;
    scheduler.start(0U);

    for (uint16_t tick = 0U; tick < kTickCount; ++tick) {
        // The first deadline is one period after `start()`, so tick `n` runs at
        // `(n + 1) * period`.
        const uint32_t nowMs = static_cast<uint32_t>(tick + 1U) * kPeriodMs;
        const ev::ControlTickDecision decision = scheduler.decide(nowMs);
        TEST_ASSERT_TRUE(decision.due);
        scheduler.advance(nowMs);
        ++control.ticks;
        control.droppedDeadlines = decision.droppedTicks;

        const ev::RunPhase phase = phaseForTick(tick);
        const ev::MotorEffort requested = requestForTick(tick);

        // Readiness is deliberately incomplete, exactly like the shipped
        // configuration: nothing may be authorized until a deliberate operator
        // action and real measurements exist.
        ev::SafetySystemState state;
        state.readiness.configurationValid = false;
        state.readiness.calibrationUsable = false;
        state.readiness.limitsUsable = false;
        state.requiredDataFresh = true;
        state.atZeroSpeed = true;
        state.nowMs = nowMs;
        state.detectedFault = ev::FaultCode::None;

        TEST_ASSERT_FALSE(authority.isArmed());
        const ev::AuthorizationResult authorized = authority.authorize(requested, state, limits);
        control.verdicts[tick].outcome = authorized.outcome;
        control.verdicts[tick].mode = authorized.command.mode();
        control.verdicts[tick].permille = authorized.command.magnitudePermille();

        if (!withConsumers) {
            continue;
        }

        // ---- the telemetry producer -------------------------------------------
        ev::TelemetrySample sample;
        sample.tMs = nowMs;
        sample.dtMs = static_cast<uint16_t>(kPeriodMs);
        sample.phase = phase;
        sample.armed = authority.isArmed();
        sample.positionM = 0.1 * static_cast<double>(tick);
        sample.positionUsable = true;
        sample.measuredVelocityMps = 0.5;
        sample.velocityUsable = true;
        sample.requestedVelocityMps = 0.6;
        sample.remainingDistanceM = 7.5 - sample.positionM;
        sample.motorMode = requested.mode;
        sample.effortPermille = requested.magnitudePermille;
        sample.batteryCounts = static_cast<uint16_t>(700U + tick);
        sample.currentCounts = static_cast<uint16_t>(500U + tick);
        sample.fault = ev::FaultCode::None;
        sample.outcome = authorized.outcome;
        sample.firstFailedCheck = ev::ConfigurationCheckId::ConfigurationValid;

        if (session.sampleDue()) {
            ++consumers->ticksWithSampleDue;
            const ev::TelemetryPushResult pushed = session.pushSample(sample);
            if (pushed == ev::TelemetryPushResult::Queued) {
                ++consumers->samplesQueued;
            } else if (pushed == ev::TelemetryPushResult::DroppedFull) {
                ++consumers->samplesDropped;
            } else {
                TEST_FAIL_MESSAGE("a sample was refused by an enabled session");
            }
        }

        // The bytes a reader would receive, composed for the tick's own sample and
        // for every transition it revealed - whether or not the lanes still have
        // room, which is precisely the work a stalled host cannot stop.
        const uint16_t sampleLength =
            ev::formatTelemetrySample(sample, recordBuffer, ev::kSerialLineCapacity);
        TEST_ASSERT_TRUE(sampleLength > 0U);
        ++consumers->samplesFormatted;

        const ev::TelemetryEventBatch batch = detector.observe(sample);
        for (uint8_t index = 0U; index < batch.count; ++index) {
            const uint16_t eventLength =
                ev::formatTelemetryEvent(batch.events[index], recordBuffer, ev::kSerialLineCapacity);
            TEST_ASSERT_TRUE(eventLength > 0U);
            ++consumers->eventsFormatted;
            session.pushEvent(batch.events[index]);
        }

        // The stalled host: nothing is drained, so the lanes fill and every later
        // record is dropped and counted.
        const ev::TelemetryDrops current = session.drops();
        if (current.samples > consumers->drops.samples) {
            consumers->drops.samples = current.samples;
        }
        if (current.events > consumers->drops.events) {
            consumers->drops.events = current.events;
        }

        // ---- the operator half ------------------------------------------------
        const char* command = commandForTick(tick);
        if (command == nullptr) {
            continue;
        }

        const uint16_t length = static_cast<uint16_t>(strlen(command));
        for (uint16_t index = 0U; index < length; ++index) {
            reader.push(command[index]);
        }
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ev::LineEvent::LineReady),
                              static_cast<int>(reader.push('\n')));

        const ev::CommandParseResult parsed =
            ev::parseOperatorCommand(reader.line(), reader.lineLength());
        ev::OperatorSystemState systemState;
        systemState.phase = phase;
        systemState.armed = authority.isArmed();
        systemState.faultLatched = false;
        systemState.checks = ev::ConfigurationBuildChecks{};

        uint16_t written = 0U;
        if (!parsed.ok) {
            ++consumers->commandsRefused;
            written = ev::formatOperatorRefusal(ev::OperatorCommandId::None,
                                                ev::commandErrorName(parsed.error), responseBuffer,
                                                ev::kSerialLineCapacity);
        } else {
            const ev::CommandAdmissibility admissible =
                ev::admissibilityOf(parsed.request.id, systemState);
            if (!admissible.allowed) {
                ++consumers->commandsRefused;
                written = ev::formatOperatorRefusal(parsed.request.id,
                                                    ev::commandRefusalName(admissible.refusal),
                                                    responseBuffer, ev::kSerialLineCapacity);
            } else {
                ++consumers->commandsAdmitted;
                switch (parsed.request.id) {
                    case ev::OperatorCommandId::Status: {
                        ev::OperatorStatusView view;
                        view.state = sample;
                        view.config = heldConfig;
                        view.configVerdict = ev::heldRunConfigVerdict(heldConfig);
                        written = ev::formatStatusLine(view, responseBuffer, ev::kSerialLineCapacity);
                        break;
                    }
                    case ev::OperatorCommandId::ConfigShow:
                    case ev::OperatorCommandId::ConfigCheck:
                        written = ev::formatConfigEcho(heldConfig, ev::heldRunConfigVerdict(heldConfig),
                                                       responseBuffer, ev::kSerialLineCapacity);
                        break;
                    case ev::OperatorCommandId::Checks:
                        written = ev::formatChecksLine(systemState.checks, responseBuffer,
                                                       ev::kSerialLineCapacity);
                        break;
                    case ev::OperatorCommandId::TelemetryStatus: {
                        ev::OperatorTelemetryView view;
                        view.enabled = session.enabled();
                        view.strideTicks = session.strideTicks();
                        view.controlCadenceHz = kControlCadenceHz;
                        view.drops = session.drops();
                        written = ev::formatTelemetryState(view, responseBuffer,
                                                           ev::kSerialLineCapacity);
                        break;
                    }
                    case ev::OperatorCommandId::TelemetryRate: {
                        const ev::TelemetryRateVerdict rate = ev::telemetryStrideForRate(
                            parsed.request.telemetryRateHz, kControlCadenceHz);
                        TEST_ASSERT_TRUE(rate.ok);
                        session.setStrideTicks(rate.strideTicks);
                        written = ev::formatTelemetryRateAck(rate, responseBuffer,
                                                             ev::kSerialLineCapacity);
                        break;
                    }
                    case ev::OperatorCommandId::TelemetryOff: {
                        session.disable();
                        ev::OperatorTelemetryView view;
                        view.enabled = session.enabled();
                        view.strideTicks = session.strideTicks();
                        view.controlCadenceHz = kControlCadenceHz;
                        view.drops = session.drops();
                        written = ev::formatTelemetryAck(parsed.request.id, view, responseBuffer,
                                                         ev::kSerialLineCapacity);
                        break;
                    }
                    case ev::OperatorCommandId::Disarm:
                        // A stop is always available. Here it stops a run that was
                        // never armed, and the control path is unchanged by it.
                        authority.disarm();
                        written = ev::formatOperatorAck(parsed.request.id, responseBuffer,
                                                        ev::kSerialLineCapacity);
                        break;
                    default:
                        TEST_FAIL_MESSAGE("a command this scenario did not expect was admitted");
                        break;
                }
            }
        }

        if (written == ev::kBoundedLineDidNotFit) {
            ++consumers->formatFailures;
        } else {
            ++consumers->responsesFormatted;
            std::memcpy(consumers->lastResponse, responseBuffer, static_cast<size_t>(written) + 1U);
        }
    }

    return control;
}

}  // namespace

// The scenario itself: with the whole consumer hierarchy running against a host
// that never reads a byte, the control path's every verdict is unchanged.
void test_ignoring_serial_cannot_change_the_control_path(void) {
    ConsumerRun consumers = ConsumerRun{};
    const ControlRun without = runScenario(false, &consumers);
    const ControlRun with = runScenario(true, &consumers);

    TEST_ASSERT_EQUAL_UINT32(kTickCount, without.ticks);
    TEST_ASSERT_EQUAL_UINT32(without.ticks, with.ticks);
    TEST_ASSERT_EQUAL_UINT32(without.droppedDeadlines, with.droppedDeadlines);

    for (uint16_t tick = 0U; tick < kTickCount; ++tick) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(without.verdicts[tick].outcome),
                                      static_cast<int>(with.verdicts[tick].outcome), "outcome");
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(without.verdicts[tick].mode),
                                      static_cast<int>(with.verdicts[tick].mode), "mode");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(without.verdicts[tick].permille,
                                        with.verdicts[tick].permille, "permille");
    }
}

// Nothing in the consumer hierarchy can arm: with the shipped provisional
// configuration, no tick ever authorizes powered effort, however many commands
// were typed beside it. A *stop* is granted as asked (that is the authority's own
// design - a stop is always safe), which is why the assertion is about powered
// drive rather than about refusals in general.
void test_no_consumer_path_can_arm_the_authority(void) {
    ConsumerRun consumers = ConsumerRun{};
    const ControlRun control = runScenario(true, &consumers);

    for (uint16_t tick = 0U; tick < kTickCount; ++tick) {
        const ev::AuthzOutcome outcome = control.verdicts[tick].outcome;
        TEST_ASSERT_TRUE(outcome != ev::AuthzOutcome::AuthorizedDrive);
        TEST_ASSERT_TRUE(outcome != ev::AuthzOutcome::AuthorizedClamped);

        const ev::MotorMode mode = control.verdicts[tick].mode;
        TEST_ASSERT_TRUE(mode == ev::MotorMode::Disabled || mode == ev::MotorMode::Coast ||
                         mode == ev::MotorMode::Brake);
        TEST_ASSERT_EQUAL_UINT16(0U, control.verdicts[tick].permille);
    }
}

// A host that never reads is a *visible* condition, not a silent one: the lane
// holds exactly its capacity, every later record is counted as dropped, and the
// commands were still answered.
void test_a_stalled_host_is_counted_not_hidden(void) {
    ConsumerRun consumers = ConsumerRun{};
    (void)runScenario(true, &consumers);

    TEST_ASSERT_EQUAL_UINT16(ev::kTelemetrySampleLaneCapacity,
                             static_cast<uint16_t>(consumers.samplesQueued));
    TEST_ASSERT_TRUE(consumers.samplesDropped > 0U);
    TEST_ASSERT_EQUAL_UINT32(consumers.samplesQueued + consumers.samplesDropped,
                             consumers.ticksWithSampleDue);
    TEST_ASSERT_TRUE(consumers.drops.samples > 0U);
    TEST_ASSERT_TRUE(consumers.samplesFormatted >= consumers.ticksWithSampleDue);
    TEST_ASSERT_TRUE(consumers.eventsFormatted > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, consumers.formatFailures);

    // Both kinds of answer were produced: the ones the state admitted and the ones
    // it refused. The throttle took effect (the stride is no longer 1), and the
    // stream ended up off - all while the control trace stayed what it was.
    TEST_ASSERT_TRUE(consumers.commandsAdmitted > 0U);
    TEST_ASSERT_TRUE(consumers.commandsRefused > 0U);
    TEST_ASSERT_TRUE(consumers.responsesFormatted > 0U);
    TEST_ASSERT_TRUE(consumers.ticksWithSampleDue < kTickCount);
}

// The last answer the consumer side produced is a complete line reporting the
// realized telemetry state, not the requested one: the stride the rate mapping
// chose is what it says, and a deliberate `off` is not a loss, so its counters are
// zero.
void test_the_consumer_side_reports_the_realized_state(void) {
    ConsumerRun consumers = ConsumerRun{};
    (void)runScenario(true, &consumers);

    TEST_ASSERT_EQUAL_STRING(
        "OK,cmd=telemetry off,enabled=0,stride=5,rate_hz=10.0000,drop_s=0,drop_e=0",
        consumers.lastResponse);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ignoring_serial_cannot_change_the_control_path);
    RUN_TEST(test_no_consumer_path_can_arm_the_authority);
    RUN_TEST(test_a_stalled_host_is_counted_not_hidden);
    RUN_TEST(test_the_consumer_side_reports_the_realized_state);
    return UNITY_END();
}
