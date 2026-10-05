# Software Architecture

**Status: FROZEN CONTRACTS.**

This file states only decisions the project has already committed to. It is not a
roadmap, a research log, or a design history. Design exploration and why-decisions
belong in `DEVELOPMENT_LOG.md`. Physical facts and their evidence belong in
`VERIFIED_STATE.md`.

Changing a contract in this file is a deliberate act. Record the reason in
`DEVELOPMENT_LOG.md` in the same change.

---

## 1. Target contract

- The vehicle firmware targets the **Arduino Nano R4** through PlatformIO
  environment `nano_r4` (Arduino framework). `pio run` builds this firmware.
- Competition behavior runs entirely on the microcontroller. No runtime
  dependency on a host computer, network, cloud service, or external API.
- The toolchain is not replaced without a specific technical reason and
  explicit approval.

## 2. Layering and dependency contract

Two source roots exist, and their responsibilities do not overlap:

- `src/` — vehicle firmware. This is the only place allowed to include Arduino
  headers (`Arduino.h`, `Serial`, `pinMode`, `millis()`, and so on).
- `lib/` — pure logic modules. **`lib/` code must not include Arduino headers,
  and must not depend on `src/`.**

Dependency direction is one-way: `src/` → `lib/`. Never `lib/` → `src/` and never
`lib/` → Arduino.

`test/` holds host tests. It is not shipped code and is never included by the
vehicle firmware.

## 3. Host-testability contract

- Any module placed in `lib/` must compile under PlatformIO environment
  `native` and must have at least one host test in `test/`.
- Host tests run with `pio test -e native`.
- The `native` environment must never compile `src/` (`test_build_src = no`).
  Arduino-dependent code cannot and must not be forced into host tests.
- Host tests verify logic only. A passing host test is never evidence that the
  vehicle behaves correctly; see `VERIFIED_STATE.md`.

## 4. Nonblocking execution contract

- The main loop must not block on long `delay()` calls.
- Elapsed-time logic uses `millis()` with **unsigned subtraction**, which is
  correct across `uint32_t` wraparound.
- Short waits required by hardware during initialization are the only
  permitted exception, and they must be bounded.
- The control loop must stay responsive enough to detect faults, update
  measurements, advance state, update motor commands, and emit telemetry.
- One cycle has one clock reading. The estimator, the profile and the regulator
  all take `nowMs`/`dtMs` as arguments rather than reading a clock, and the
  vehicle layer must pass the reading from the same `millis()` call to all of
  them. The modules deliberately respond to a non-advancing stamp in different
  ways - the estimator refuses the sample, the profile holds the target, the
  regulator freezes integration - and none of those is by itself an error
  signal; deciding that the clock is faulted belongs to the vehicle layer.

## 5. Safety contract

These invariants are frozen and are not negotiable for convenience:

- The motor is commanded OFF or disabled during boot, during initialization,
  after reset, after an unrecoverable error, when an invalid state is detected,
  before entering an armed state, and after a command/communication timeout.
- The firmware never resumes a previous motor command after reboot or reset.
- Powered motion requires an intentional transition into an armed/run state.
- Diagnostic and test entry points obey the same constraints. Malformed or
  unexpected input must never produce uncontrolled motion.

## 6. Units and naming contract

- Identifiers for physical quantities carry their units, and the unit is never
  implied: `targetDistanceM`, `targetTimeS`, `velocityMps`, `batteryV`,
  `maxEffortPermille`.
- Pure-core code uses SI: metres, seconds, metres per second, volts. The event
  rules are written in these units, so rule values and firmware values stay
  directly comparable. `AGENTS.md` section 10 gives millimetre examples, which
  remain valid for code written in millimetres, but a single module never mixes
  prefixes internally.
- Clock readings taken from `millis()` keep an `Ms` suffix and stay in
  milliseconds (`elapsedMs`), because that is what the platform provides. A
  timestamp is a clock reading, not a physical duration; durations use seconds.
- Ambiguous names such as `dist`, `time`, `speedVal`, and `offset` are not used.
- Numeric literals that represent calibration, rule, or hardware facts are not
  scattered through logic; see sections 7 and 8.

## 7. Calibration, limits, and provenance contract

- Fixed hardware configuration, measured vehicle calibration
  (`VehicleCalibration`), safety ceilings (`SafetyLimits`), and per-run event
  parameters (`RunConfig`) are separate concepts with separate types. They are
  never merged into one bag of values.
- Every calibration or limit value carries its evidence in the type
  (`Sourced<T>` with `ValueSource`): `Measured`, `RuleDerived`, or
  `Provisional`. There are no default values, so an unmeasured quantity cannot
  be silently filled in.
- A `Provisional` value is a placeholder, never a measured fact. It stays
  visible as such and is never sufficient to authorize powered motion.
  `VehicleCalibration::isFullyMeasured()` and
  `SafetyLimits::isFullyEstablished()` exist so a later stage can refuse to arm
  on incomplete data. They are data-completeness checks, not the arming
  decision, and not a substitute for the arming authority.
- The chain consumes only `SafetyLimits::maxEffortPermille` today, through the
  vehicle layer's derivation of the velocity regulator's own ceiling
  (`ev::effortCeilingsAgree()` is that check; section 13). Maximum velocity per
  direction, maximum run duration and minimum battery voltage have no
  enforcement point yet: the profile/ceiling selection, the run timer and the
  battery check that will own them are not built. They stay data with no
  runtime effect, and nothing treats them as satisfied.

## 8. Rule source contract

- The authoritative event rules are the official 2027 Division C rules at
  `docs/rules/`. Values taken from them cite the rule number where they are
  defined.
- Rule-derived numbers live in exactly one place, `ev::rules2027`. No other
  code restates a rule value locally.
- Per-run configuration validation has exactly one entry point,
  `ev::validateRunConfig()`. No other module re-implements those checks.
- A rule citation establishes what the event requires. It is never evidence
  about the vehicle: physical facts remain governed by `VERIFIED_STATE.md`.

## 9. Telemetry contract

Telemetry is a **consumer** of the control loop, never a participant in it.

- **Machine-readable, one line per record.** `ev::TelemetryFormat`
  (`lib/evcore/src/TelemetryFormat.h`) writes each record as `key=value` fields
  separated by `,` and nothing else: no prose, no indentation, no partial records.
  The schema that freezes the prefixes, the field set and the spelling of every
  enumerated value is `ev::TelemetryRecord`
  (`lib/evcore/src/TelemetryRecord.h`), versioned by
  `ev::kTelemetrySchemaVersion` (currently 2, after the D32 driver-fault
  capability change): `EVS<version>` is one control tick, `EVE<version>`
  a transition or an operator action.
- **All or nothing.** A record that does not fit the line buffer is not emitted as
  a shortened line - truncation would eat exactly the tail fields a post-mortem
  needs (the fault, the authorization outcome, the failing check, the drop
  counters). It is dropped and counted instead.
- **Drops, never waits.** `ev::TelemetryQueue` (`lib/evcore/src/TelemetryQueue.h`)
  holds two fixed lanes, samples and events, drops the newest record when a lane is
  full, and stamps every drop count into the records that do go out, so a log says
  whether it is complete. Losing a sample costs one row of a trace; losing an event
  costs a transition, which is why the lanes are separate and why events drain
  first.
- **Nothing in the transport may block.** Serial output goes through
  `src/SerialTransport.h`, which stages one line and writes only what the port
  reports room for. On this target `Serial` is USB-CDC and its `write()` waits for
  the host, so a bare `Serial.print()` of a record would hand the control loop's
  schedule to whoever is - or is not - reading the port. No formatter, and no other
  module, may write to it.
- **Never inside an interrupt, and never in the tick's timing budget.** The encoder
  ISR calls the pure decoder and nothing else; serial work happens in the loop,
  after the watchdog checkpoint.
- **Not flooded.** A sample is taken every control tick by default, and
  `telemetry rate <hz>` throttles it by a whole number of ticks; a host that cannot
  keep up is what the drop counters are for.
- **Disabling it changes nothing else.** `telemetry off` stops the producer and
  empties the lanes. The control path never reads the session, so no vehicle
  behavior depends on it (section 16, and the `test_serial_independence` scenario
  that pins it).

## 10. Deliberately undecided

The following are **not** decided and must not be pre-built or guessed. They
depend on facts tracked in `VERIFIED_STATE.md`:

- fixed hardware configuration: pin assignments, wiring, logic levels. The
  centralized map in `src/PinMap.h` carries the team's stated V5 harness pins and
  is not a wiring record: each role's pin was checked against the installed
  framework's own pin table for the capability the role needs, no continuity
  check has been performed, and the map is UNVERIFIED (`VERIFIED_STATE.md` #1);
  section 15 fixes what it may and may not be used for;
- the motor driver's *electrical* behavior and truth table. What is now decided
  is narrower: a **planned** input mapping exists in one place,
  `ev::drv8874InputsFor()` (`lib/evcore/src/MotorDriverPlan.h`), implemented by the
  one adapter allowed to write the pins (`src/MotorDriver.h`), and recorded with
  its bench tests in `docs/hardware/motor-driver-drv8874.md`. Every level
  combination in it is UNVERIFIED (`VERIFIED_STATE.md` #1-#3), it is derived from
  a planned PH/EN interface rather than from a measurement, and it may not be
  cited as a driver fact; section 15 fixes the ownership rule;
- encoder interpretation beyond the decoding scheme fixed in section 11:
  counts per revolution, gearing, and which count direction is physical forward
  travel (an uncaptured observation of the direction exists from 2026-10-02,
  recorded as OBSERVED-not-verified in `VERIFIED_STATE.md` D36; the fact stays
  UNVERIFIED, so it stays on this list);
- the effective distance per encoder tick (rollout). The conversion *arithmetic*
  is fixed in section 12, but no ticks-to-metres factor may be written down
  until a rollout measurement exists;
- every measured calibration value;
- every `SafetyLimits` value;
- the choice and adequacy of any control configuration. Section 13 fixes the
  responsibilities and refusal behavior of the candidate estimator, velocity
  regulator and target shaper, but not which estimator window is usable, not one
  gain or limit value, and not whether any candidate is good enough for an event
  run;
- the run policy's *values*: every near-zero threshold, dwell, phase bound,
  reversal timeout and final-approach distance. Section 14 fixes the states, the
  transitions, the refusal behavior and the ownership of those values, but not
  one number in them - none has been measured;
- the producers of the bottle-interaction evidence. Section 14 fixes that the run
  waits for contact, capture and push evidence under a configured bound, but how
  a contact or a finished push is detected, and whether the mechanism can detect
  one at all, depends on the bottle-pushing mechanism that does not exist yet
  (`VERIFIED_STATE.md` #11);
- any *future* addition to the telemetry schema, the command vocabulary or the
  response vocabulary. Schema version 1 and the command set are frozen in sections
  9 and 16; adding a field, a command or an event is a deliberate change that
  records itself in `DEVELOPMENT_LOG.md`, and renaming a field or an event is a
  schema change that bumps `ev::kTelemetrySchemaVersion`, not a refactor;
- whether any additional sensing (ground/dead-wheel encoder, IMU) is justified.

Adding any of these requires either verified evidence or an explicit statement
that the value is an unverified, configurable placeholder.

## 11. Encoder decoding contract

- Incremental quadrature decoding is owned by exactly one pure-logic module,
  `ev::QuadratureDecoder` in `lib/evcore/src/QuadratureDecoder.h`. It is
  Arduino-free and has no clock, so it is host-testable before any encoder
  exists (section 3).
- The decoding scheme is fixed: **A-channel CHANGE x2** — one count per
  transition of channel A, on both of its edges. The B level read with the
  transition, together with the A level the transition lands on, decides the
  sign. The sign is therefore a function of the whole landing `(A, B)` state and
  is never derived from B alone.
- The module has one internal counting convention: the Gray cycle
  `00 -> 10 -> 11 -> 01 -> 00` counts as `Positive`. This is a software
  convention, not a physical fact. Nothing in `lib/` may assume which convention
  corresponds to forward travel; that mapping is UNVERIFIED
  (`VERIFIED_STATE.md` #4). The uncaptured 2026-10-02 observation (D36) changes
  nothing here: without a recorded procedure and raw data it is not evidence, #4
  stays UNVERIFIED, and this rule stands as written.
- The module reports raw encoder ticks only, as `int32_t` to match
  `SensorSnapshot::motorEncoderTicks`. Counts per revolution, gearing, wheel
  geometry and any ticks-to-displacement conversion are forbidden in this module
  and belong to a later configuration/odometry layer.
- Sampling the channels (pin reads, interrupts), signal filtering or debounce,
  and velocity estimation are not part of this contract. Odometry is not part of
  this contract either, but the part of it that is not a hardware question - the
  tick-to-displacement arithmetic and its validity/freshness semantics - is
  fixed in section 12.

## 12. Odometry and sample-age contract

- Tick-to-displacement arithmetic is owned by exactly one pure-logic module,
  `ev::Odometry` in `lib/evcore/src/Odometry.h`. It is Arduino-free, has no
  clock, no pin read and no interrupt, so it is host-testable before any encoder
  exists (section 3).
- The module owns **no scale**. The effective distance per encoder tick enters
  every call as an argument, as the caller's `Sourced<double>` (the value held by
  `VehicleCalibration::distancePerEncoderTickM`). No wheel circumference, gear
  ratio, counts-per-revolution or x1/x2/x4 factor may be written into this
  module, and a theoretical geometry is never a substitute for the measured
  value. `VERIFIED_STATE.md` #6 stays UNVERIFIED until rollout is measured.
- Tick readings stay in the counting convention they arrive in. The sign of the
  result inherits the encoder's direction convention, which is UNVERIFIED
  (`VERIFIED_STATE.md` #4); nothing in this layer may declare a sign to be
  forward travel.
- A scale must be finite and positive to produce any number at all, and must be
  `ValueSource::Measured` for the result to be marked valid. A `Provisional` (or
  `RuleDerived`) scale still yields a number so diagnostics can print it, but the
  estimate is invalid; a non-finite or non-positive scale yields no number at
  all. `valid` is the only field a control path may act on, and
  `valid == (status == Valid)` is an invariant, not a convention.
- Tick differences are formed in 64 bits before conversion, so a full-range
  int32 reading pair cannot be undefined behaviour, and an unusable or
  overflowing conversion fails closed instead of reporting an infinity.
- Where a position is needed, the run origin is an argument, never a constant or
  a hidden global. `positionFromOriginM()` takes a caller-supplied run-axis
  (physical) origin in metres; the run axis below takes the *tick reading* the
  run started at instead, because its axis zero is the Start Point by the
  event's definition and a second, caller-supplied axis origin would be a second
  convention for the same axis.
- `positionFromOriginM()` adds a physical origin to travel that inherits the
  encoder's tick-counting convention. That reconciliation is owned by
  `RunAxis.h` below and applied in exactly one place; until the polarity fact is
  measured, a physical origin must not be combined with tick-derived travel, and
  the forward and reverse legs of a run must not mix the two conventions
  (`remainingDistanceM` in MotionProfile.h carries the same warning from the
  other side).
- **The run axis has one owner.** `ev::RunAxisGeometry`, `ev::runAxisPositionM()`,
  `ev::remainingDistanceTo()` and `ev::remainingDistanceToClamped()` in
  `lib/evcore/src/RunAxis.h` are the production owner of the official *rule*
  geometry and of where the vehicle is on it:
  - `RunAxisGeometry::fromRunConfig()` is the one mapping from the announced
    `RunConfig` to the axis: the Target Point position (rule 8.h), the Bottle
    Line Distance (rule 8.j) and the Bottle Start Point (rule 7.c.ii.(5)), with
    the Bottle Line position as the Target Point plus the announced Bottle Line
    Distance. The Bottle Line is an official **rule landmark**, not the vehicle's
    physical turnaround, forward-leg endpoint or forward stop target - that is
    execution geometry, owned by the immutable `RunExecutionPlan` below, whose
    single forward stop target S4 wires into motion shaping. The state machine
    deliberately stores no geometry, so the run layer has to hold it, and holding
    it without an owner is how two places start disagreeing about where a rule
    landmark is.
  - `runAxisPositionM()` applies the measured tick-sign polarity **once**, to the
    travel rather than to a position, so an origin that is not zero cannot invert
    the sign. Its `RunAxisStatus` separates the two refusals - no tick travel at
    all, and a tick-sign polarity that is not `Measured` - and the run layer
    combines its own `usable` with `odometryReadiness()` into the one position
    verdict it hands to the boundary and the run policy. A placeholder polarity
    still reports a diagnostic number under the placeholder sign, exactly like a
    `Provisional` scale, and may not reach control.
  - `remainingDistanceTo()` is the generic remaining-distance arithmetic and takes
    an **explicit, caller-supplied target** - the caller names the landmark (or,
    once S4 consumes it, the execution plan's target) it is measuring to, so the
    arithmetic cannot substitute a rule landmark for the target a consumer asked
    about. It is signed and deliberately unclamped (an overshoot is evidence, not
    something to hide from a log); `remainingDistanceToClamped()` is the same
    arithmetic for the travel taper and is never negative. Both take the leg as an
    argument and are computed from the leg the run named in the previous cycle,
    which is the leg whose distance the run consumes this cycle; neither decides
    that a position was reached and neither stops anything.
  - `temporaryLegExecutionTargetM()` is the one **temporary pre-S3/S4
    compatibility seam**: it maps the current leg's execution target to the Bottle
    Line rule landmark (forward) or the rule Target Point (reverse), so the
    firmware keeps its current deterministic behaviour until the execution plan
    exists. It is a compatibility mapping, not a physical claim, and **S4 removes
    it** when the run reads the execution plan's forward stop target instead.
  - **The execution targets have their own owner.** `ev::RunExecutionPlan`,
    `ev::RunExecutionDependencies` and `ev::buildRunExecutionPlan()` in
    `lib/evcore/src/RunExecutionPlan.h` own the *execution* geometry of one
    announced run: the explicit forward stop target, the reverse stop target and
    the forward-axis interaction landmarks, as an immutable value built once at
    the announcement boundary from the validated `RunAxisGeometry`, the
    `BottleEvidenceConfig` landmark distances and the physical dependencies in
    `RunExecutionDependencies`. It consumes rule geometry and never redefines it:
    the Bottle Line stays a rule landmark, the reverse stop is the announced
    Target Point, and the forward stop is the Bottle Line plus a **measured**
    margin rather than the Bottle Line itself. Construction fails closed with a
    named `RunExecutionPlanStatus` when the announcement is illegal, when a
    required physical dependency is not `Measured` (a `Provisional` placeholder
    never authorizes an execution plan that depends on it), when a value is not
    finite, or when the derived landmarks are not in a physically possible order
    (`earliestContact <= latestContact <= captureGuaranteed <= forwardStop` and
    `pushComplete <= forwardStop`). The plan records `timingReady`,
    `predictedMotionTimeS` and `initialDelayS` honestly: with no empirical timing
    evidence the first is false and both numbers are 0.0. It has no default
    constructor, no setters and only `const` fields, so an announcement produces
    one plan and a run cannot adjust it. **No measured forward-stop margin exists
    yet**, so a production construction refuses today, and **runtime consumption
    is deliberately not wired**: S4 is what makes motion shaping and forward
    progression read the plan's one forward stop target.
- Sample age is computed once, by `ev::sampleAge()` in `Snapshots.h`:
  `ageMs = nowMs - sampleMs` with unsigned subtraction, fresh up to a
  caller-supplied `maxAgeMs` inclusive, so a stamp from the future is never
  fresh. No staleness timeout lives in `lib/`: none has been measured.
- `ev::odometryReadiness()` is the single place that combines sample existence,
  estimate validity and freshness into one verdict; `RunAxis.h` above owns the
  conversion's own usability verdict, and the run layer combines the two into the
  position verdict it acts on. Motion and safety consume that combined verdict -
  mapped onto `SafetySystemState::requiredDataFresh` - instead of re-deriving it
  from the raw pieces. It is deliberately the *position* verdict
  that gates that field: a stationary vehicle's speed evidence is the estimator's
  explicit zero-speed claim, whose `valid` is false by design, so gating
  `requiredDataFresh` on `velocityReadiness().usable` would make a restart from a
  verified stop impossible. The speed verdict belongs to the regulator's
  `measurementUsable` argument (section 13). Reason recorded in
  `DEVELOPMENT_LOG.md` (D11).
- `OdometrySnapshot::velocityMps` is deliberately unfilled: this layer owns no
  velocity arithmetic. Speed is owned by `VelocityEstimate` and
  `velocityReadiness()` (section 13); no consumer may copy a velocity into the
  snapshot as a second source, and telemetry reads the estimator's answer
  together with its freshness verdict.
- Not in this layer, and not to be pre-built: channel sampling and interrupts,
  filtering/debounce policy, run-origin bookkeeping for competition, and any
  authority for a ground or dead-wheel encoder (section 10). Velocity and
  zero-speed estimation are no longer unbuilt - their contract is fixed in
  section 13 - but nothing they produce may feed control until a physical test
  exists for it.
- Raw sensor readings stay raw. No battery or current raw-to-engineering-unit
  conversion exists, because the circuit relationship behind one (ADC reference,
  divider ratio, shunt value) is not documented in the project's sources and
  `VERIFIED_STATE.md` #7 is UNVERIFIED. Such a helper may be added only once
  that relationship is authoritative here.

## 13. Velocity estimation, velocity control, and target shaping contract

Three pure-logic modules own three separate responsibilities. None may absorb
another's, and none is a run state machine.

- **Velocity estimation** (`ev::VelocityEstimator` in
  `lib/evcore/src/VelocityEstimator.h`) is the only place a speed is derived from
  ticks. It owns no scale: the effective distance per tick enters every update as
  the caller's `Sourced<double>` and obeys section 12's scale rule through
  `distanceScaleStatus()`. It has no clock (`nowMs` is an argument) and no
  hardware.
  - The estimator is parameterized by a window span, not duplicated per
    candidate: comparing effective windows means configuring this one module
    several ways, and the realized span behind every number is reported with it.
  - Only whole ticks over the covered span, times the supplied scale, may produce
    a number: no filtering, interpolation, extrapolation or prediction.
  - An encoder that has not changed for `noEdgeTimeoutMs` earns an explicit
    `zeroSpeedClaimed`. That is a software policy statement about observed
    non-change, never a measurement and never a validity flag. A covered window
    containing no change is a valid zero, which is a different fact.
  - **No estimator configuration is adequate until a physical test says so**
    (`VERIFIED_STATE.md` #5, #6, #10). Nothing in this layer may set
    `SafetySystemState::atZeroSpeed`, and no consumer may treat a window-derived
    number, or the claim, as a measured stopping behavior.
  - Edge-period timing is deliberately absent: `SensorSnapshot` carries one stamp
    per sample and no per-edge timestamps, so it cannot be computed without
    inventing timestamps.
- **Velocity control** (`ev::VelocityController` in
  `lib/evcore/src/VelocityController.h`) is the only place an effort is computed
  from a velocity error. It produces a `MotorEffort` request and nothing else: it
  cannot authorize, cannot touch a pin, and cannot construct an
  `AuthorizedMotorCommand`.
  - Feed-forward + proportional + integral. No derivative term, and no model of
    motor, drivetrain, friction or battery.
  - Elapsed time is always the caller's measured dt, never a nominal loop period.
    A non-advancing stamp freezes integration while feed-forward and proportional
    terms continue; a stall is bounded by a configured maximum integration step.
  - Anti-windup is conditional integration plus an integral clamp. Integration is
    skipped while the demand is at a limit and the error pushes further into it.
  - Powered effort is only ever requested in the commanded direction: the effort
    interval is one-sided, so an overspeed reduces effort toward zero and never
    asks for powered effort the other way. Reversal remains a deliberate run-state
    sequence (AGENTS.md section 15).
  - An effort that quantizes to zero is emitted as `Disabled`, never as a Drive
    with magnitude 0, because a zero-effort Drive would still latch a direction in
    `SafetyAuthority`.
  - The regulator is disabled on construction and after `reset()`; enabling is
    explicit, and disabling or resetting drops the integral, the time base and the
    remembered direction. Nothing resumes automatically.
  - Gains, effort ceiling, integral ceiling and the sign polarity arrive in a
    `Sourced` configuration. A value that is not `Measured`, or is malformed,
    produces a non-powered output and `enable()` refuses. No gain may be written
    into `lib/` until it is measured (`VERIFIED_STATE.md` #8).
- **Target shaping** (`ev::MotionProfile` in `lib/evcore/src/MotionProfile.h`) is
  the only place a target velocity is shaped from remaining distance and elapsed
  time. It holds no state and commands nothing.
  - The target magnitude is the smallest of the cruise ceiling, the
    remaining-distance taper and the rate brackets reachable in the measured dt.
    The taper uses the caller's deceleration limit and the caller's measured brake
    offset for the direction being travelled.
  - A commanded reversal, and an explicit stop request, are ramped to zero at the
    deceleration limit; the sign never flips inside one step. The settle, the
    brake and the competition sequence remain undecided (section 10).
  - `WithinBrakeOffset` is a statement about the distance it was handed, not a
    claim about where the vehicle is, and not a stop command.
  - Target Time and an optional intentional delay are carried explicitly, and the
    estimated travel duration and its margin against Target Time are reported.
    **The strategy that spends the margin is now decided (D34):**
    `ev::planRunTiming()` in this module is the run layer's timing owner - it takes
    the announced Target Time, the announced rule geometry (the whole course: out
    to the Bottle Line and back) and the measured limits, and produces the cruise
    the run is shaped with (the *slowest* cruise whose estimated travel fits the
    announced time, never above the configured or safety ceiling, never below the
    configured planned-cruise floor) and the deliberate pre-travel delay that
    spends the remaining slack. An announced Target Time shorter than the fastest
    admissible travel is refused at the announcement boundary, never met by
    exceeding a limit. The chosen values reach the control path as the run's
    `MotionProfileLimits` and as `CompetitionStateMachine::plannedDelayS()`; the
    bounded `MotionTimingIntent`/`motionTimingSummary()` estimate remains the
    vocabulary for a caller that has limits but no announced run, and both use the
    one `travelDurationS()` arithmetic so they cannot disagree about a trip. The
    Target Time and the geometry stay announced (operator-typed) inputs.
  - Every limit is `Sourced`; a non-measured or malformed limit produces a zero
    target rather than a shaped one.
  - The run's target/time contract has exactly one owner: `MotionProfileInputs`
    (remaining distance, elapsed time) plus `MotionTimingIntent` (Target Time and
    the intentional delay). `MotionRequest` in `Motion.h` is vocabulary only and
    must not be wired into a run; nothing else may carry a competing time budget.
  - The caller-held previous target is `ev::MotionProfileTarget`, whose zero
    value is a zero target and whose `resetForNewRun()` is the only way a run
    boundary clears it. The two directional limit sets are selected by direction
    through `ev::DirectionalMotionProfileLimits`, so a leg cannot be handed the
    wrong pair of forward/reverse values by hand.
- **One sign chain, two configurable boundaries.** Tick signs stay the decoder's
  convention (section 11), and the estimator only ever reports that convention.
  `MotionProfileLimits::positiveCommandDirection` maps a physical
  `MotionDirection` to the command sign;
  `VelocityControllerConfig::positiveEffortMode` maps the command sign to a
  powered driver mode. Both are `Sourced`, and both are the UNVERIFIED polarity
  fact (`VERIFIED_STATE.md` #4). Neither may be marked `Measured` until that
  polarity is actually measured, and until then the regulator refuses to drive -
  which is the intended failure mode, not an obstacle to work around.
- **The intended wiring is one direction.** The estimator produces the measured
  speed, the profile produces the target from the remaining distance, the
  regulator turns the two into a `MotorEffort`, and
  `SafetyAuthority::authorize()` turns that request into the only type a future
  driver may accept. No module in this section may bypass the authority, treat
  its own ceiling as an authorization, or decide a stop.
- **A new run resets the whole chain through one sequence.**
  `ev::resetMotionChainForNewRun()` in `lib/evcore/src/MotionChainReset.h` is the
  single place that returns every module's per-run state to nothing: the run
  policy (idle, no announcement, no fault of record, and the reversal sequencer
  and stop verifier it owns), the regulator (disable-then-enable, because
  `enable()` on a running regulator is a deliberate no-op), the decoder reference
  and count, the estimator window/claim/counters, the caller-held target, and the
  safety boundary (disarmed, no fault of record, no remembered direction). It
  never arms and never commands motion; it reports whether the regulator could be
  re-enabled, which fault it discarded and which run phase the previous run ended
  in, so the run log can record what ended it. Arming stays a separate,
  deliberate transition, a run still requires a fresh announcement (section 14),
  and the caller re-establishes the decoder reference with one stationary sample
  before motion.
- **Velocity freshness has an owner.** `VelocityEstimate::elapsedMs` is the
  clock reading that produced an answer, and `ev::velocityReadiness()` in
  `VelocityEstimator.h` is the single verdict combining estimate existence,
  validity and age, mirroring `odometryReadiness()` for position. The vehicle
  layer maps that verdict onto the regulator's `measurementUsable` argument
  instead of passing a bare boolean, so a last-valid estimate reused across
  cycles is detected as stale rather than silently trusted.
- **`zeroSpeedClaimed` is never `atZeroSpeed`.** The estimator's claim is an
  explicit software statement about observed encoder non-change, not a
  measurement that the vehicle stopped. A producer of
  `SafetySystemState::atZeroSpeed` must combine a fresh valid estimate, the
  claim and the interval over which the stop was observed, and must be able to
  say which facts it used; the full contract is stated in `SafetyAuthority.h`.
  Section 14 names that producer: the near-zero verdict of
  `ev::NearZeroVerifier`, which combines exactly those three facts, reports which
  of them held, and is earned only over a continuous dwell. `lib/` still does not
  write the safety field: the vehicle layer maps the verdict onto it.
  Those three facts do not arrive in one estimator answer: a window-derived
  number never carries the claim and the claim never carries a number. Their
  pairing is itself production: `ev::composeStopEvidence()` composes the packet
  from the current cycle's claim (accepted only within the control path's own
  staleness limit) and the last window-derived estimate (aged against the
  packet's own, longer limit), and `ev::stopEvidenceLimitSpansDwell()` is the
  configuration-build check that refuses a limit shorter than the stop dwell
  before the chain is armed. Without that check the run would arm into a stop it
  could only time out waiting for. Reason recorded in `DEVELOPMENT_LOG.md`
  (D11, D12); pinned by `test_near_zero_verifier` and `test/test_scenario/`.
  The dwell itself is credited only across updates the caller actually made: two
  stamps do not say whether the loop ran in between, so a gap wider than the
  configured `NearZeroConfig::maxUpdateGapMs` restarts the interval and reports
  `NearZeroStatus::UpdateGapExceeded` instead of being credited as continued
  evidence. That limit is a `Sourced` value like the threshold and the dwell, and
  its measured source is the maximum control-loop stall H0W records - **28 ms,
  measured in the D28 bench campaign** (`kMaxDwellGapMs`, `Measured` and cited,
  `src/VehicleConfig.h`) - so a stall cannot silently earn the interval a stop is
  verified over; a loaded run that stalls longer only restarts the verifier's
  interval. Reason recorded in `DEVELOPMENT_LOG.md` D19 and D28; pinned by
  `test_near_zero_verifier`.
- **The two effort ceilings must be equal.**
  `VelocityControllerConfig::maxEffortPermille` is derived from
  `SafetyLimits::maxEffortPermille`, and `ev::effortCeilingsAgree()` is the
  check the vehicle layer runs before arming. The regulator's anti-windup and
  `outputSaturated` report are computed against its own ceiling only, so a
  ceiling above the safety ceiling would split one saturation across
  `outputSaturated == false` in one log and `AuthorizedClamped` in another, and
  would integrate headroom the regulator does not have. The authority still
  clamps independently as defence in depth.
- **The three polarity encodings must agree.**
  `MotionProfileLimits::positiveCommandDirection` and
  `VelocityControllerConfig::positiveEffortMode` are the same UNVERIFIED
  physical fact (`VERIFIED_STATE.md` #4) written in two vocabularies, and
  `ev::polarityAgrees()` (`lib/evcore/src/SafetyLimits.h`) is the check the
  vehicle layer runs before it arms the chain. A pair that is `Measured` but
  opposite passes every per-module validation, and the profile's taper and the
  regulator's effort would then drive the vehicle in opposite directions for one
  command sign. The comparison is over the physical directions the two values
  name, never their numeric values, and it is a configuration-build check rather
  than a runtime clamp - like `effortCeilingsAgree()`, and for the same reason:
  one recorded verdict the vehicle layer can refuse to arm on, instead of an
  assumption each module makes about the other.
- **The measured tick sign is the third encoding of that same fact.** Which
  direction the decoder's positive count corresponds to is a separate
  measurement from the two command-sign encodings, and `ev::tickPolarityAgrees()`
  ties it to `MotionProfileLimits::positiveCommandDirection`. It matters from
  both sides: the regulator forms `target - measured`, so a positive command
  sign that means the opposite physical direction from a positive measured speed
  has the loop's error sign inverted - positive feedback, with no per-module
  validation able to see it - and a run travelling its forward leg must see its
  position grow (`RunAxis.h`), which holds exactly when positive ticks are
  forward travel. All three encodings must name one physical direction before
  the chain may be armed; the check is a configuration-build fact, not a runtime
  clamp, and the run-axis conversion refuses separately to produce a usable
  position under a placeholder tick sign.
- **A travel request is presented as powered only while the shape points along
  the commanded leg.** `MotionProfile` ramps a commanded reversal to zero before
  it flips the sign (`MotionProfileStatus::DirectionChangeHold`), so the travel
  command that opens the next leg can be shaped at a small target still on the
  *previous* direction's side. Handing that target to the regulator makes it
  compute a powered effort in the direction the run has just stopped travelling
  in - and after a verified stop the authority has no latch left to refuse it
  with, because the stop that earned the reversal is exactly what cleared that
  latch. The verdict is production: `ev::targetVelocityPointsAlongLeg()`
  (`lib/evcore/src/MotionProfile.h`) answers it from the shaped target, the leg
  being travelled and the direction's own limits, treating a zero target as
  along the leg and a non-finite one as not. The vehicle layer must present its
  own unpowered output while it is false, and present the regulator's effort
  only once it is true. An addition to the obligations recorded in D10; found by
  the P6 scripted scenarios (`test/test_scenario/`), which pin it as a
  whole-trace property. Reason recorded in `DEVELOPMENT_LOG.md` (D11, D12).
- **One authority, two request sources, no arbiter yet.** A run will present
  two kinds of request to `SafetyAuthority::authorize()`: the regulator's
  `MotorEffort` and the run layer's own deliberate Brake/Coast stop. The run
  layer owns which single request is presented each cycle; no caller may call
  `authorize()` twice in a cycle and act on the later result, because the
  direction memory and the fault response would then depend on call order. A
  latched fault overrides whatever request arrives.
  - These are obligations of the vehicle layer, which does not exist yet, so no
    code enforces them today. They are recorded here and in
    `DEVELOPMENT_LOG.md` (D10) as build-time acceptance criteria, not as
    conventions a later implementer may reinterpret: when that layer is built,
    it must be the only caller of `authorize()`, it must present exactly one
    request per control cycle, and every run-originated request it presents
    must have passed `ev::runIntentionIsCurrentFor()` for that cycle's clock
    reading (section 14).
- **A zero target is not a brake.** A target of zero - from a stop request, from
  the taper reaching the measured brake offset, or from a refusal - means no
  powered effort is asked for. The run layer that owns the stop commands its own
  deliberate decelerate/settle/brake sequence, and nothing in this section may
  assume the vehicle actually followed the taper. A refusal-derived stop is still
  a stop the run layer knows about: a Disabled request taken while zero speed is
  reported clears the authority's direction latch exactly like a deliberate one
  (SafetyAuthority.h), and the run layer owns that consequence.

## 14. Run policy contract: near-zero verification, reversal sequencing, run states

Three responsibilities, three pure-logic modules. None may absorb another's, and
none actuates anything: the run policy's output is an intention, and the only
path from an intention to a powered command still runs through the regulator and
`SafetyAuthority::authorize()` (sections 5 and 13).

- **Near-zero verification** (`ev::NearZeroVerifier` in
  `lib/evcore/src/NearZeroVerifier.h`) answers one question: has the vehicle been
  within a configured near-zero speed threshold, continuously, for a configured
dwell interval? It combines exactly the three facts the safety contract names -
  a fresh, valid velocity estimate (`velocityReadiness().usable`), the measured
  speed within the threshold, and the estimator's explicit `zeroSpeedClaimed` -
  and reports which of them held, so a refused reversal can be attributed to the
  fact that was missing rather than guessed at. The dwell must be continuous: an
  update without the evidence clears it, so a vehicle oscillating on the
  threshold never verifies a stop. There is no threshold, dwell or timeout value
  in this module and no clock: the policy arrives as `NearZeroConfig` with
  `Sourced` threshold and dwell, and a configuration that is not `Measured`
  produces a refusal instead of a verdict. It produces a verdict; it does not set
  `SafetySystemState::atZeroSpeed`, and the vehicle layer maps it onto that field.
- **Reversal sequencing** (`ev::ReversalSequencer` in
  `lib/evcore/src/ReversalSequencer.h`) owns the sequence that earns a powered
  direction change: `RampingDown` -> `Braking` -> `VerifyingNearZero` ->
  `DirectionChangeReady`, or `StopRequired` when the near zero is not verified
  inside `reversalTimeoutS`. That single configured bound covers the ramp-down
  and the verification together; the permission is latched once earned, so a
  later measurement cannot un-earn a verified stop; and a sequence is never
  restarted while it runs, because restarting would restart its timeout. An
  unusable configuration refuses immediately instead of waiting, and a request
  that is not a change of direction is refused rather than sequenced. The module
  commands nothing, knows no competition phase, and its permission is never an
  authorization: whether a powered command may follow is `SafetyAuthority`'s
  decision, and the authority refuses a direct flip independently.
- **Run policy** (`ev::CompetitionStateMachine` in
  `lib/evcore/src/CompetitionStateMachine.h`) is the only owner of the run's
  phase sequence, and its phases are the frozen vocabulary:

      Idle -> ParameterValid -> Armed -> OptionalDelay -> Approach -> Capture
           -> Push -> ForwardDecel -> BrakeZeroSpeed -> Reverse -> FinalApproach
           -> FinalBrake -> Done,                               (or Fault)

  `RunPhase` replaces the provisional `CompetitionState` list (`VehicleState.h`
  no longer keeps a second, competing vocabulary). `Idle` is the zero value,
  `Done` and `Fault` are terminal, and `Fault` is reachable from every phase -
  including `Idle` and `Done`. Only an explicit `reset()` leaves `Done` or
  `Fault`; there is no path that re-enters a run without a fresh announcement and
  a fresh arming decision.
- **The run policy consumes evidence and emits intent, nothing else.**
  `RunInputs` carries the armed fact, the start trigger, bottle contact/capture/
  push evidence, the run-axis remaining distance with its own usability, the
  near-zero evidence packet and any fault the vehicle layer observed.  `RunStatus`
  carries the phase, one `RunMotionCommand`, the fault of record, the
  `stopVerified` verdict, the reversal phase and the time in phase. No phase
  transition derives geometry from the announced `RunConfig`: the announced
  parameters are validated at the announcement boundary (through the single
  entry point, `ev::validateRunConfig()`) and then not stored. Where the vehicle
  is on the run axis is owned by `RunAxis.h` (section 12); what the bottle line
  means for the mechanism and how far the vehicle must travel before a push is
  complete remain run-layer calculations on measured, still-UNVERIFIED facts
  (#11); the state machine consumes their results as evidence.
- **Every bound is an input.** `RunPhaseConfig` supplies each phase timeout, the
  optional delay, the final-approach distance and the final-stop verification as
  `Sourced` values, and an unusable one refuses the announcement instead of
  sequencing a run with placeholders. ForwardDecel and BrakeZeroSpeed
  deliberately have no separate bound: the reversal sequence's timeout owns both.
  Because those two phases have no bound of their own, a sequence that is neither
  running nor refusing while the run is in them (a sequencer that is `Idle`, or a
  refusal that names no cause) faults as `RunSequencingError` instead of becoming
  a wait that nothing can end. A positive bound that converts to zero whole
  milliseconds expires immediately rather than becoming an unbounded wait.
- **The bottle interaction waits for evidence, and a bound that expires is a
  fault.** Each of their phases waits for the vehicle layer's evidence under its
  own configured bound, and a bound that expires raises `PhaseTimeout`. **That
  evidence is distance-derived, deliberately and with the claim stated (D34).**
  This vehicle's mechanism has no detection of its own, so
  `ev::deriveBottleEvidence()` (`BottleEvidence.h`) derives contact, capture and
  push from the run-axis position and the announced geometry - a lead distance
  before the Bottle Start Point, a capture advance beyond contact, and the
  Bottle Line position plus a configured margin. Every distance arrives as
  `Sourced` configuration and must be `Measured` before the run policy is usable,
  so the inference never rests on a placeholder; and it is the firmware's
  position-derived *belief*, not an observation of the bottle. The
  bottle-interaction row (`VERIFIED_STATE.md` #11) stays UNVERIFIED until P-H8
  records independently observed bottle positions against it. This supersedes the
  earlier "not inferred from distance or from elapsed time" wording, which was
  written when no mechanism existed; contact, capture and push are still never
  inferred from *elapsed time*. The
  same rule covers the reverse leg's closing distance and the final stop.
- **The reversal is requested through the reversal contract, never
  re-implemented.** `Push` begins a reversal sequence on the way into
  `ForwardDecel`; `ForwardDecel` ramps the forward leg to zero and moves to
  `BrakeZeroSpeed` when the vehicle is within the near-zero threshold;
  `BrakeZeroSpeed` consumes `DirectionChangeReady`. A refusal from the sequencer
  faults (`ReversalNotVerified`, or `ProvisionalCalibration` when the refusal is
  the configuration). Both timers and both refusals are the sequencer's; the
  state machine does not keep a second copy of the sequence.
  - The two vocabularies of that half of the run are deliberately separate and
    frozen: `ForwardDecel` and `BrakeZeroSpeed` are what the run *presents* the
    vehicle layer, while `RampingDown`, `Braking`, `VerifyingNearZero` and
    `DirectionChangeReady` are what the owned sequencer has *established*. The
    mapping is `ForwardDecel`/`RampingDown` and `BrakeZeroSpeed`/(`Braking`,
    `VerifyingNearZero`, `DirectionChangeReady`), it lives in one place
    (`CompetitionStateMachine::advanceReversal()`), and `RunStatus::reversalPhase`
    reports the evidence for diagnostics. Neither vocabulary may be collapsed
    into the other: one is a request, the other is a fact.
- **A stop command is a leg tag, not a direction.** `RunMotionCommand::
  BrakeForward`/`BrakeReverse` - and `RampDownForward`/`RampDownReverse` - name
  which leg the stop belongs to, never a powered direction: a deliberate
  non-powered stop has none. Both readings of that vocabulary are production and
  live beside the commands, in `CompetitionStateMachine.h`:
  `ev::runCommandNamesForwardLeg()` is the one leg-tag mapping (a travel command
  names the leg it travels, a stop names the leg it belongs to, and `None` names
  no leg at all, so a caller must keep the leg it had rather than apply the
  answer), and `ev::runStopEffort()` maps `RampDown*` to a `Disabled` request and
  `Brake*` to a `Brake` request - the deliberate non-powered hold - dropping the
  tag exactly as it must. A command that is not a stop command maps to
  `Disabled` with zero effort, so a caller bug cannot turn a travel command into
  a second, competing request. The vehicle layer must never derive a
  `PoweredDirection` from either reading. The tag is invisible to
  `SafetyAuthority`, which refuses a direct flip independently, and a stop is
  never "completed" with a drive in the tagged direction.
- **The direction handoff presents a stop cycle before asking for reverse.** A
  non-powered stop taken while zero speed is reported is what clears the
  authority's remembered powered direction (section 13), so the run presents its
  deliberate stop for one full cycle after the permission is earned, reports the
  verified stop for that cycle, and asks for the reverse travel only on the next
  one. The authority still refuses independently: if the handoff was not true -
  no stop presented, no zero reported - the travel is denied, and the run's own
  phase bound then faults it rather than letting it hang. The reported verdict is
  derived from the phase the run is presenting - the reversal gate's stop in
  `BrakeZeroSpeed`, the final-stop verifier's in `FinalBrake` and `Done` - and is
  false in every travel phase, so a stop verified for one leg can never be read
  as the vehicle being stopped while the next leg is driven. Ownership is one
  verifier and one latch per stop, at the level that can see the evidence: the
  reversal gate's `NearZeroVerifier` and latch live in `ReversalSequencer`, the
  final stop's `NearZeroVerifier` and latch live in `CompetitionStateMachine`,
  and `RunStatus::stopVerified` only reports the latch of the stop being
  presented - it is never a re-derivation, and never an OR across the run.
- **An intention is valid for one cycle.** `RunMotionIntention` carries the clock
  reading that produced it and `ev::runIntentionIsCurrentFor()` is the single
  verdict a consumer uses; a Travel command held across a skipped cycle, a
  stalled loop or an ended run must be refused. `Done` and `Fault` never emit a
  travel command at all, so a stale one can only come from a caller that kept it.
  Like the single-request rule (section 13), the enforcement point is the
  vehicle layer, which does not exist yet: when it is built, no run-originated
  request may be presented to `SafetyAuthority` unless the intention is current
  for that cycle's clock reading. This is recorded as a build-time acceptance
  criterion (`DEVELOPMENT_LOG.md` D10) rather than a check the chain can enforce
  today; the scenario harness exercises it by presenting a held intention of its
  own (`test/test_scenario/`), which is the only way the gate can be observed at
  all.
- **The final stop is the event's Complete Stop.** `FinalBrake` holds a stop and
  waits for the near-zero verdict before `Done`, under `finalStopTimeoutS`; the
  dwell the vehicle layer configures for it is the run's Complete Stop interval
  (rule 8.p, `ev::rules2027::kCompleteStopS`), which is a different ask from the
  reversal gate's much shorter stop.
- **Run faults are identity, not response.** `FaultCode` gained `PhaseTimeout`,
  `ReversalNotVerified` and `DisarmedDuringRun` (a run that loses the armed state
  it started with is stopped and latched rather than left asking for motion the
  authority is refusing), plus `RunSequencingError` for a sequencing
  inconsistency the policy must not act on. All of them are software conditions
  with no measured electrical answer, so `faultStopPolicyFor()` maps every one to
  `Disable`, exactly like the faults that came before them.

## 15. Runtime, scheduling, and the hardware boundary contract

The vehicle layer now exists in `src/`, and these are its frozen obligations. It
is the implementation of the wiring obligations sections 12-14 recorded
(`DEVELOPMENT_LOG.md` D10, D12) and the harness exercised in test form.

- **One control tick, one frozen stage order.** The stage vocabulary and its
  order are owned by `ev::ControlTickStage` and checked by
  `ev::ControlTickOrderGuard` (`lib/evcore/src/ControlTickOrder.h`):
  encoder snapshot, odometry and velocity, sensor snapshot, fault evaluation, run
  state machine, run motion request, motion control, safety authorization, motor
  command, telemetry capture. A tick that does not present every stage once, in
  that order, is reported as violated and is not trusted. The guard is pure
  bookkeeping; it has no clock and no hardware.
- **One clock reading per tick.** The runtime reads no clock: the pass is handed
  the caller's `millis()` reading, and the scheduler's measured `dtMs` is what
  every module that needs elapsed time receives. No module in the tick may read a
  clock or use a nominal period.
- **Scheduling is a decision, never a wait.** `ev::ControlScheduler`
  (`lib/evcore/src/ControlScheduler.h`) owns the deadline logic: a tick is due when
  the reading reaches the next deadline (a signed difference of two `uint32`
  readings, so it is correct across the ~49.7 day wrap), and the cadence re-arms
  from the tick that actually ran. Missed deadlines are dropped, counted and
  reported - never caught up with a burst of back-to-back ticks - and the measured
  `dtMs`, lateness, dropped deadlines, remaining wait and a rewound reading are
  all reported. A layer that never starts the schedule runs no control tick at
  all. There is no RTOS, no hardware-timer scheduler, no `delay()`-driven
  sequencing and no blocking wait anywhere in the pass.
- **The vehicle layer is the only caller of `authorize()`, once per cycle.** One
  request is presented per control tick - the regulator's effort, or the run's own
  deliberate stop through `ev::runStopEffort()`, never both - and the single
  authorized command is the only thing that reaches the motor driver. A
  run-originated request must be current for that cycle's reading
  (`ev::runIntentionIsCurrentFor()`), and a travel request is presented as
  powered only while the shaped target points along the leg the run named
  (`ev::targetVelocityPointsAlongLeg()`). Faults are reported to the run policy
  and the boundary in the same cycle.
- **The measured data that gates a drive is the position verdict.**
  `SafetySystemState::requiredDataFresh` is the combined odometry/run-axis
  position verdict; the speed verdict belongs to the regulator's
  `measurementUsable` argument (section 13), and `atZeroSpeed` comes from the
  run's own verified-stop latch (section 14). None of these may be re-derived
  from a raw piece of evidence in the vehicle layer.
- **The motor outputs have one writer.** Only `src/MotorDriver.h` may write the
  planned PH/EN/nSLEEP pins, and the only mapping from a command to input levels
  is `ev::drv8874InputsFor()` (section 10). No other file may write those pins and
  no `lib/` code can (`lib/` has no Arduino headers). The driver's de-energized
  state is reachable without a command - `beginDisabled()` at startup and
  `failSafe()` for a cycle that is not trustworthy - and neither path can produce
  powered output. Startup drives the driver de-energized before anything else can
  ask for motion.
- **The encoder has exactly one interrupt, and it does one thing.**
  `src/EncoderInput.cpp` attaches one A-channel CHANGE interrupt whose handler
  reads two digital pins and calls the pure decoder's `update()`. It performs no
  serial I/O, no ADC, no state machine, no controller work, no floating point, no
  allocation and no blocking wait, and it refreshes nothing else in the system.
  The control cycle reads the count through a snapshot taken with interrupts
  briefly disabled (PRIMASK saved and restored), so the fields it acts on are one
  coherent instant; on this core an aligned `int32` read is itself atomic, so the
  section is about coherence between fields, not about tearing.
- **Raw inputs stay raw, and an unverified input is not invented into a fault.**
  The battery, current and trigger inputs are sampled as ADC counts and logic
  levels and are consumed as such (section 12). The driver's fault output is
  interpreted by one module, `lib/evcore/src/DriverFault.h`, and that module is
  **capability-aware**: `DriverFaultCapability` says whether the external nFAULT
  signal exists on the vehicle at all, and the interpretation is a function of the
  capability *and* the polarity. On the assembled vehicle the line is **not
  installed** (`NotInstalled`), so `SensorInputs.h` does not configure or sample
  the pin, no level is consumed, and telemetry writes `nfault=NotInstalled` with
  the level field absent - an unavailable signal is never spelled as a healthy
  level. On a vehicle that *does* wire the line, the behavior is unchanged from
  what it was: while the polarity is `Unverified` no level becomes a `FaultCode`
  and the raw level is reported and counted instead, and the capability-aware
  `DriverFaultConfigurationValid` check keeps the vehicle unarmable until H1B has
  measured it. So an armed vehicle can never be one whose fault input nobody
  reads. Reason recorded in `DEVELOPMENT_LOG.md` D13, D19 and D32.
- **The watchdog has one checkpoint, one centralized setting, and one open.**
  Only a main-loop pass that finished - and whose control tick, if one ran,
  completed in the frozen stage order - may refresh the watchdog. The refresh
  lives in exactly one place, `src/WatchdogCheckpoint.h`, and is called from
  exactly one place, the firmware's loop, after the pass returns. Nothing in the
  control tick, in a module, in the safety boundary, in a telemetry hook or in an
  interrupt may refresh it: a component that could refresh the watchdog by itself
  could hide its own hang. A pass that is not refreshable counts as a withheld
  refresh, so firmware whose control cycle ran wrongly stops feeding the watchdog
  and the watchdog's reset then returns the vehicle to the de-energized state it
  boots into.
  The timer is opened by a separate, deliberate call at the **end** of
  initialization, after the one bounded boot wait, so the interval between the
  request and the first refresh - which happens at the end of the first completed
  pass - is small and bounded. Opening it at the start of `setup()` would let a
  timeout chosen from steady-state refresh intervals alone expire during boot,
  which is a board that never comes up rather than a safe stop. The two gaps a
  timeout must cover, and the margin, are one pure rule
  (`lib/evcore/src/WatchdogPlan.h`), and the requested constants are asserted
  against it at compile time, so enablement cannot be flipped with a timeout that
  cannot survive boot. The measured boot gap
  (`WatchdogCheckpoint::timeToFirstRefreshMs()`) is reported to the bench for
  H0W's timeout choice.
  Enablement is one centralized expression and it is **off** in the vehicle image;
  what has been observed (`env:bench-watchdog`, D26-D28) belongs to the bench
  qualification and not to this build, and compiling and linking the adapter is not
  validation of any of it. Its value comes from a build define that only the
  bench environment for H0W sets (`env:bench-watchdog`), rather than from a literal
  in a header both images compile - enabling the watchdog for a bench
  qualification must not be able to change the vehicle build's decision, and
  `src/main.cpp` refuses to compile if the production image ever receives the
  define.
- **The configuration's provenance is the arming refusal.** The vehicle layer's
  configuration (`src/VehicleConfig.h`) is where cadence policy, provisional
  placeholders and rule-derived values are kept separate, and
  `ConfigurationBuildChecks` reports each check the chain must pass before
  anything may be armed (`effortCeilingsAgree()`, both polarity agreements, the
  stop-evidence limit spanning both stop dwells, every per-module validation). No
  single boolean may summarize them: a refused arm must be attributable to the
  fact that failed. The run's final stop dwell is the rule-derived Complete Stop
  (rule 8.p); every other value is `Provisional` and stays visible as such.
  Two checks are about provenance rather than arithmetic. `MeasuredValuesCiteEvidence`
  is false for as long as a promoted (non-placeholder) value names no evidence, so
  the edit that lets a value authorize motion is also the edit that has to point at
  the measurement (`Sourced::citesEvidence()`). `DriverFaultConfigurationValid` is
  false until a vehicle that *has* the driver's fault line has measured its
  polarity; on a vehicle whose external nFAULT signal is not installed, polarity
  verification is not applicable and the check reports the capability instead - see
  the raw-inputs contract above. It was named `DriverFaultPolarityVerified` until
  D32, when the vehicle's wiring made the old name a claim about a signal that does
  not exist.
- **The operator interface is a consumer of the same chain, and only a consumer.**
  `serviceSerial()` runs in the loop's low-priority section, after the watchdog
  checkpoint: it moves staged bytes out, reads received bytes, applies at most one
  command, answers it, then drains the telemetry queue - in that order, so an
  operator's answer outranks the log. It owns no control state: the decisions come
  from `ev::SerialCommandParser` and `ev::OperatorPolicy`, the text from
  `ev::OperatorResponse`, and the effects go through the chain's own entry points
  (`announceRun()`, `SafetyAuthority::arm()`/`disarm()`,
  `ev::resetMotionChainForNewRun()`). No command commands an effort, a mode or a
  direction, and none starts a run: the start trigger is a physical input. Section
  16 fixes the contract.
  The one exception is a separate image, not a command: bench instrument B2
  (`src/bench/`, `lib/evcore/src/BenchDrive.h`) can apply a bounded, latched effort
  for the bring-up protocols that must measure what the vehicle does before a run
  exists. It is excluded from the production image by the build graph, it may write
  the driver only where `ev::benchApplyVerdict()` permits (no latched fault, no
  violated tick order, no armed run, no run under way - and *not* merely
  `!armed`), its output reaches the driver through a separate adapter call that
  takes a type no production module can construct, and it expires on its own. See
  section 17 and `docs/bringup/README.md`.
- **The run boundary is the operator's deliberate decision, and the checks are what
  refuse it.** `announce` consumes the held configuration through the single
  rule-validation entry point, and `arm` reads the configuration-build report
  through `ev::armVerdictFor()` before `SafetyAuthority::arm()` is asked. With the
  provisional configuration in `src/VehicleConfig.h` every arming intent is refused
  and is attributable to the first failing check - the intended state of this
  firmware. Nothing may auto-arm, resume a command after a reset, or treat a
  placeholder as measured.
- **A telemetry record is built from the tick's own snapshot.** Stage 10 formats the
  sample the session is due from the values the tick's stages produced, so a log
  cannot disagree with the control path about what happened. The record is queued,
  never written: no formatter and no queue operation touches the port, and a
  transport may not run inside the tick.

## 16. Operator interface contract

The operator interface is a **consumer** of the chain, like telemetry (section
9), and its three responsibilities are three separate pure modules with no
overlap. None of them may hold run-control state, and no control code may read
any of them.

- **Parsing is one module, and it only decides what the line asked for.**
  `ev::SerialCommandParser` (`lib/evcore/src/SerialCommandParser.h`) owns the
  bounded line reader (`ev::CommandLineReader`), the command vocabulary
  (`ev::OperatorCommandId`), the `config set` field set (`ev::RunConfigFields`)
  and the one parse entry point, `ev::parseOperatorCommand(line, length)`. A
  malformed line produces a named `ev::CommandError` and the id `None`; it never
  reaches the policy, and it never produces a partial request a caller might act
  on. The reader is bounded by construction - `kCommandLineCapacity` characters,
  `kCommandTokenCapacity` tokens, `kCommandNumberMaxMagnitude` and
  `kCommandFractionDigitsMax` - so no input can overflow a buffer, and there is
  no dynamic allocation anywhere in it.
- **Admissibility is one module, and it is the only place a command is allowed or
  refused.** `ev::OperatorPolicy` (`lib/evcore/src/OperatorPolicy.h`) answers,
  for the current `OperatorSystemState` (run phase, armed fact, latched fault and
the configuration-build report), whether a command may be applied and, when it
may not, why (`ev::CommandRefusal`, named by `commandRefusalName()`). It also owns
the two run-boundary verdicts (`armVerdictFor()`, `announceVerdictFor()`), the
held-configuration verdict (`heldRunConfigVerdict()`) and the telemetry rate to a
whole-tick stride mapping (`telemetryStrideForRate()`). Nothing else may decide
that a command is acceptable.
- **The command vocabulary is frozen, and it is inspection plus intent.**
  `help`, `version`, `status`, `checks`, `boot`, `config show`, `config check`,
  `config set`, `config clear`, `announce`, `arm`, `disarm`, `reset`,
  `telemetry on`, `telemetry off`, `telemetry status`, `telemetry rate <hz>`.
  (`boot` re-sends the boot line; like the other inspection commands it changes
  no run state, so it is admissible in every phase.)
  **No command commands an effort, a mode or a direction, and none starts a run.**
  The start trigger is a physical input; the closest any command comes to motion
  is the arming intent, which is still a decision the chain's own checks refuse
  or accept.
- **The state restriction is deliberate, not an omission.** Once a run's phases
  are underway, only the inspection commands, `disarm` and the telemetry commands
  are admissible (`CommandRefusal::RunUnderway`); everything else is refused. This
  is what makes "serial input cannot alter an in-progress run" a property of one
  function rather than a distribution of guards, and it is the restriction the
  `test_serial_independence` scenario pins from the outside.
- **`reset` is admitted before the fault gate, and only there.** A latched fault
  is of record and refuses everything else (`CommandRefusal::FaultLatched`);
  clearing it is exactly what `reset` is for, so the reset is checked first. It is
  still refused while a run's phases are underway, because discarding a fault is
  not the same thing as abandoning a moving run: the arming/disarming decision is
the operator's, and `disarm` is the command for that.
- **One announcement per run, and arming reads the checks.** `announce` consumes
  the held configuration through the single rule-validation entry point
  (`ev::validateRunConfig()`); a second announcement is refused as
  `RunAlreadyActive`, and arming with no announcement is refused as
  `NoRunAnnounced`. `arm` reads the configuration-build report through
  `armVerdictFor()` *before* `SafetyAuthority::arm()` is asked, so a refused arm
  names the first failing check instead of producing an armed state with
  unverified configuration behind it.
- **Formatting is one module, and it emits one line per response.**
  `ev::OperatorResponse` (`lib/evcore/src/OperatorResponse.h`) owns every response
  spelling: the prefix vocabulary (`OK`, `ERR`, `STATUS`, `CONFIG`, `CHECK`,
  `TELEMETRY`, `VERSION`), the `key=value` field order, the refusal and
  arm-refusal text, the `config set` acknowledgement, and the help table. Like a
  telemetry record (section 9), a response is **all or nothing**: it fits the line
  buffer or it is dropped and counted, never emitted truncated. Its worst case is
  bounded and statically checked against the line capacity
  (`kOperatorResponseWorstCaseChars`).
- **The transport is the board layer's, it is non-blocking, and the operator's
  answer outranks the log.** `src/SerialTransport.h` stages one line and writes
  only what the port reports room for (section 9), so nothing in the operator path
  may block. `serviceSerial()` runs in the loop's low-priority section, after the
  watchdog checkpoint, in a fixed order: move staged bytes out, read received
  bytes, apply and answer at most **one** command, then drain the telemetry queue.
  A staged operator response therefore displaces staged telemetry rather than
  waiting behind it, and a pass that is out of room simply keeps the response
  staged for the next one.
- **The operator interface changes nothing the control path can see.** No stage of
  the control tick reads the parser, the policy, the session or the transport, and
  no phase transition or arming decision waits on a host. A host that is absent,
  slow, or sending nonsense changes the log and the counters, and nothing else. A
  run that is underway keeps its phase, its target and its arm state regardless of
  what arrives on the port, which the `test_serial_independence` scenario proves
  by running the identical scripted run twice - once with the consumer hierarchy
  fully driven by a stalled host and rejected commands, once without it - and
  comparing the control traces.
- **Not decided here.** Adding a command, a field or an event is a deliberate
  change that records itself in `DEVELOPMENT_LOG.md`, and renaming an existing one
  is a schema change (section 10), not a refactor. No command for a diagnostic
  motion mode exists and none is implied: `AGENTS.md` section 17 lists such modes
  as examples, and the ones with any control authority would be a new contract.

## 17. Bench instrumentation contract

Some bring-up measurements need the firmware to report or do something the
vehicle image deliberately cannot: a raw tick count, a raw input level, the loop
and watchdog counters, the driver pins' levels, the MCU's reset-status registers,
the watchdog's own configuration registers, a bounded way to command effort outside
a run, and a way to stall the loop on purpose (H0W Step 3). Those are bench
instruments **B1-B4** plus the readouts the later H0W steps added (`bench pins`,
`bench resetsrc`, `bench wdt`) and the deliberate-hang hook
(`docs/bringup/README.md`), and they are a separate build, not a mode of the vehicle
image.

- **Exclusion is a property of the build graph, not of a macro.** The bench tree
  lives in `src/bench/`, and the two environments partition it: `env:nano_r4`
  compiles `src/` without `bench/`, and `env:bench` compiles `src/` without the
  vehicle image's `main.cpp` and adds the bench entry point. A source file the
  production environment does not compile cannot run, and a macro that is merely
  absent cannot be relied on the same way. The partition is verifiable without
  running anything: a symbol search over the two images finds the bench console's
  symbols in the bench image and none in the vehicle image.
- **The line grammar is pure and host-tested too.** Splitting a line, recognizing
  the verb and parsing `bench drive`'s arguments are
  `lib/evcore/src/BenchCommand.h` (`test_bench_command`), not the console: the
  console owns the port, the dispatch and the text, and the parts that can be wrong
  without a bench session noticing are the parts that can be tested without one. A
  line with more tokens than the grammar allows is refused rather than acted on in
  part.
- **The three readouts are read-only.** B1 reports the watchdog checkpoint's
  counters, the measured boot-to-first-refresh gap and the scheduler's cadence
  numbers (which H0 and H0W record); B3 reports the decoder's raw signed ticks and
  its progress counters (H2, H3, H4, H5); B4 reports the raw trigger and fault
  levels and the two ADC counts (H1B). None of them writes anything, and none is a
  control input.
- **The one motion-capable instrument is bounded, latched, armed and limited, and
  it is `lib/evcore` pure logic.** `ev::BenchDrive`
  (`lib/evcore/src/BenchDrive.h`) decides what may be applied: a command does
  nothing before a deliberate arm, every command carries its own bounded duration
  and expires on its own, effort, duration and direction are checked against hard
  ceilings before anything is applied, a per-arm command budget bounds a confused
  console, and an operator abort or an encoder that stops changing while driving
  de-energizes the motor and latches until a deliberate reset. All of it is host
  tested (`test_bench_drive`).
- **It cannot act while anything outranks it, and the test is not `!armed`.**
  `ev::benchApplyVerdict()` (`lib/evcore/src/MotorArbitration.h`, pure and
  host-tested by `test_bench_arbitration`) is asked once per pass, **before** the
  control tick, and it refuses the bench when a fault is latched, when the
  control-tick order guard has recorded a violation, when a run is armed, or when
  a run is under way. It deliberately does **not** ask `!armed`: a latched fault
  clears arming (`SafetyAuthority::authorize()`) and a violated tick order calls
  the driver's fail-safe write and then disarms, so the two states that most need
  the motor off are exactly the states `!armed` cannot distinguish from a free
  motor. A refusal aborts the instrument (latched), rather than disarming it.
- **One writer per pass.** While the verdict permits it, the bench takes ownership
  of the three motor pins before the tick (`motorDriver::setMotorPinsOwned()`), so
  the tick's own write yields and the pins see one writer's intent rather than a
  sleep/wake edge once per control period - a 50 Hz `nSLEEP` toggle is a pin state
  neither writer intends, and no measurement bounds what the driver does with it.
  The bench's write happens after the tick; ownership is released on the next pass,
  or immediately if the pass's tick ran out of order, so the tick is still the
  backstop: if the bench layer stops running, the next tick de-energizes the
  driver, and a fail-safe write is never overwritten by a bench command. It writes
  through a separate adapter entry point that takes a type no production module can
  construct.
- **The physical kill switch remains the primary stop.** No bench instrument
  replaces the motor-supply disconnect, and no protocol may use one with the
  switch out of reach.
- **A bench instrument is not evidence.** Bench output is not a vehicle behavior,
  and nothing it reports may be recorded in `VERIFIED_STATE.md` until the protocol
  that uses it has run and its raw capture is committed
  (`docs/bringup/README.md`).
- **Not decided here.** The bench line format (`BENCH,...`) is not the telemetry
  schema and is not versioned with it: it is read by a person during a bench
  session, and the analyzer treats a line that is not a telemetry record as an
  ignored line. No bench verb is a control input, and adding one that could change
  run behavior would be a new contract, not a new verb.
