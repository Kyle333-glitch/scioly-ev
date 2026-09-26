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

- Telemetry is machine-readable and emitted in a form that does not disturb
  timing-sensitive behavior.
- Serial logging is never performed inside an interrupt handler.
- Serial output is not flooded so heavily that it changes control-loop timing.

## 10. Deliberately undecided

The following are **not** decided and must not be pre-built or guessed. They
depend on facts tracked in `VERIFIED_STATE.md`:

- fixed hardware configuration: pin assignments, wiring, logic levels;
- the motor driver interface, its truth table, and how `MotorMode` maps onto it;
- encoder interpretation beyond the decoding scheme fixed in section 11:
  counts per revolution, gearing, and which count direction is physical forward
  travel;
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
- telemetry schema;
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
  (`VERIFIED_STATE.md` #4).
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
- Where a position is needed, the run origin is an argument
  (`originPositionM`, in metres), never a constant or a hidden global.
- `positionFromOriginM()` adds a caller-supplied run-axis (physical) origin to
  travel that inherits the encoder's tick-counting convention. The
  reconciliation between the two conventions is the vehicle layer's and must be
  applied once, through the measured polarity fact; no module in `lib/` applies
  or invents a polarity. Until that fact is measured, a physical origin must not
  be combined with tick-derived travel, and the forward and reverse legs of a
  run must not mix the two conventions (`remainingDistanceM` in MotionProfile.h
  carries the same warning from the other side).
- Sample age is computed once, by `ev::sampleAge()` in `Snapshots.h`:
  `ageMs = nowMs - sampleMs` with unsigned subtraction, fresh up to a
  caller-supplied `maxAgeMs` inclusive, so a stamp from the future is never
  fresh. No staleness timeout lives in `lib/`: none has been measured.
- `ev::odometryReadiness()` is the single place that combines sample existence,
  estimate validity and freshness into one verdict. Motion and safety consume
  that verdict - mapped onto `SafetySystemState::requiredDataFresh` - instead of
  re-deriving it from the raw pieces.
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
    Nothing acts on that margin: spending slack, waiting, or going faster is a run
    strategy decision that has not been made.
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
- **The two effort ceilings must be equal.**
  `VelocityControllerConfig::maxEffortPermille` is derived from
  `SafetyLimits::maxEffortPermille`, and `ev::effortCeilingsAgree()` is the
  check the vehicle layer runs before arming. The regulator's anti-windup and
  `outputSaturated` report are computed against its own ceiling only, so a
  ceiling above the safety ceiling would split one saturation across
  `outputSaturated == false` in one log and `AuthorizedClamped` in another, and
  would integrate headroom the regulator does not have. The authority still
  clamps independently as defence in depth.
- **The two polarity encodings must agree.**
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
  near-zero evidence packet and any fault the vehicle layer observed. `RunStatus`
  carries the phase, one `RunMotionCommand`, the fault of record, the
  `stopVerified` verdict, the reversal phase and the time in phase. No phase
  transition derives geometry from the announced `RunConfig`: the announced
  parameters are validated at the announcement boundary (through the single
  entry point, `ev::validateRunConfig()`) and then not stored. Where the vehicle
  is on the run axis, what the bottle line means for the mechanism and how far
  the vehicle must travel before a push is complete are run-layer calculations on
  measured, still-UNVERIFIED facts (#4, #6, #11); the state machine consumes
  their results as evidence.
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
  fault.** Contact, capture and push are not inferred from distance or from
  elapsed time; each of their phases waits for the vehicle layer's evidence under
  its own configured bound, and a bound that expires raises `PhaseTimeout`. The
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
  non-powered stop has none. The vehicle layer maps every stop command onto a
  non-powered request (a zero-shaped target for the profile, a non-Driving mode
  with zero effort for the authority) and must never derive a
  `PoweredDirection` from the tag. The tag is invisible to `SafetyAuthority`,
  which refuses a direct flip independently, and a stop is never "completed"
  with a drive in the tagged direction.
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
  today.
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
