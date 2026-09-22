# AGENTS.md

## 1. Project Mission

This repository contains the firmware, development tooling, diagnostics, calibration utilities, and test support for a **2026–2027 Science Olympiad Division C Electric Vehicle**.

The goal is not to create the most complicated control system possible.

The goal is to produce a vehicle that is:

1. safe to develop and test;
2. reliable;
3. repeatable;
4. accurate;
5. easy to diagnose;
6. easy to calibrate;
7. fast enough for competition;
8. understandable by the student team.

Prefer evidence from physical testing over theoretical sophistication.

---

## 2. Development Environment

Primary development environment:

* Windows
* PowerShell 7
* Git
* GitHub
* VS Code
* PlatformIO
* Arduino framework
* Arduino Nano R4
* PlatformIO environment: `nano_r4`

Primary build command:

```
pio run
```

Upload command:

```
pio run -t upload
```

Serial monitor:

```
pio device monitor
```

Do not replace PlatformIO, the Arduino framework, or the current toolchain without a specific technical reason and explicit approval.

Do not install global software, modify system configuration, or add major dependencies merely for convenience.

---

## 3. Current Development Philosophy

Development must proceed incrementally.

Do not attempt to generate the entire competition firmware in one large implementation.

Use this progression unless physical testing justifies a different order:

1. development environment and build validation;
2. Nano R4 boot, LED, and serial diagnostics;
3. motor-driver bring-up with wheels safely unloaded;
4. encoder bring-up independently;
5. motor + encoder integration;
6. low-speed encoder-based distance control;
7. braking-distance characterization;
8. acceleration / cruise / deceleration profiles;
9. controlled stop, settle, and reversal;
10. complete competition state machine;
11. calibration and repeatability testing;
12. identify dominant remaining error sources;
13. add additional sensing or control complexity only if measurements justify it;
14. freeze and validate a competition build.

Each stage should leave behind a known-good version.

---

## 4. Hardware Baseline

Current architecture is centered on:

* Arduino Nano R4 controller;
* single primary drive motor;
* motor encoder;
* motor driver;
* common mechanical drivetrain / axle architecture;
* battery-powered vehicle;
* bottle-pushing mechanism required by the event.

Additional sensing such as a passive ground/dead-wheel encoder or IMU may be added later if testing demonstrates a meaningful need.

### Hardware uncertainty rule

Never silently invent:

* pin assignments;
* wiring;
* logic voltage;
* encoder CPR/PPR interpretation;
* gear ratios;
* wheel diameter;
* wheel circumference;
* motor electrical characteristics;
* driver truth tables;
* current limits;
* battery voltage;
* braking behavior;
* mechanical dimensions;
* direction conventions;
* sensor availability.

If a required hardware fact is unknown:

1. inspect existing repository documentation first;
2. inspect code/configuration for an established value;
3. if still unknown, explicitly identify the missing fact;
4. write code so the value is configurable when reasonable;
5. do not disguise a guess as a confirmed value.

Physical hardware measurements override assumptions.

---

## 5. Safety Is a Hard Requirement

Any code capable of commanding powered vehicle motion must fail safe.

### Required invariants

The motor must be commanded OFF or disabled:

* during boot;
* during initialization;
* after reset;
* after an unrecoverable error;
* when an invalid state is detected;
* before entering an armed state;
* when appropriate after a command/communication timeout.

The firmware must never automatically resume a previous motor command after reboot or reset.

Powered motion must require an intentional transition into an armed/run state.

### Initial hardware tests

For early motor tests:

* use conservative PWM;
* keep driven wheels off the floor or otherwise unload the drivetrain when appropriate;
* test one behavior at a time;
* do not immediately command full power;
* verify direction before high-speed testing;
* make stopping behavior available and tested before aggressive motion.

Do not bypass safety behavior to make testing more convenient.

---

## 6. Physical Testing Is the Source of Truth

A successful compile proves only that the firmware compiled.

It does **not** prove:

* the motor moves correctly;
* direction is correct;
* the encoder counts correctly;
* distance conversion is accurate;
* the vehicle tracks straight;
* braking works;
* reversal is safe;
* the vehicle stops at the desired location;
* the bottle interaction works.

Never report hardware behavior as verified unless it was actually observed or measured on the physical system.

When physical validation is required, clearly state:

* what should be tested;
* how to test it safely;
* what measurements should be recorded;
* what output/logs should be returned for analysis.

---

## 7. Experimental Development Loop

Use this loop whenever possible:

```
hypothesis
    ↓
smallest useful code change
    ↓
build
    ↓
bench test
    ↓
physical test
    ↓
collect telemetry / measurements
    ↓
analyze
    ↓
make next change
```

Do not repeatedly tune values based only on intuition.

For performance work, request data.

Examples:

* requested distance;
* actual measured distance;
* encoder count;
* velocity;
* commanded PWM;
* stopping distance;
* run time;
* battery voltage if available;
* direction;
* lateral drift;
* abnormal mechanical behavior.

Repeated measurements are more valuable than one apparently good run.

---

## 8. Diagnose Systematic vs Random Error

When analyzing accuracy, distinguish between:

### Systematic error

Example:

```
+31 mm
+29 mm
+32 mm
+30 mm
```

This is often correctable through calibration or control logic.

### Random error

Example:

```
-28 mm
+52 mm
-11 mm
+41 mm
```

This may indicate:

* wheel slip;
* inconsistent bottle contact;
* drivetrain backlash;
* mechanical flex;
* battery variation;
* encoder problems;
* inconsistent traction;
* measurement error;
* other physical effects.

Do not solve a mechanical/random-error problem by endlessly adding software constants.

---

## 9. Control-System Philosophy

Start simple.

Preferred progression:

```
open-loop diagnostic control
    ↓
encoder-based position measurement
    ↓
encoder-based stopping
    ↓
controlled acceleration/deceleration
    ↓
braking compensation
    ↓
repeatability analysis
    ↓
additional feedback only if justified
```

Do not introduce PID, sensor fusion, an IMU, additional encoders, advanced estimators, or complex control algorithms merely because they appear sophisticated.

Complexity must solve an observed problem.

A simple system with low variance is preferable to a complicated system that is difficult to calibrate.

---

## 10. Distance and Motion

Do not assume theoretical wheel geometry produces exact vehicle displacement.

Treat effective distance conversion as something that may require experimental calibration.

Keep physical quantities explicit.

Prefer names containing units, for example:

* `distanceMm`
* `velocityMmPerSec`
* `elapsedMs`
* `batteryMv`
* `encoderTicks`
* `brakeOffsetMm`

Avoid ambiguous names such as:

* `dist`
* `time`
* `speedVal`
* `offset`

Centralize calibration values rather than scattering magic numbers throughout the code.

---

## 11. Firmware Architecture

Prefer explicit, inspectable modules.

A likely eventual structure is conceptually:

```
hardware/
    motor
    encoder
    inputs
    telemetry

control/
    motion profile
    braking
    distance tracking

state/
    run state machine

config/
    hardware configuration
    calibration
    run parameters

diagnostics/
    serial commands
    test modes
```

The exact layout may evolve.

Do not create abstraction layers before they provide value.

Avoid a giant monolithic `main.cpp`, but also avoid unnecessary enterprise-style architecture.

---

## 12. Competition Run Logic

The eventual run should be represented by an explicit state machine rather than a long sequence of delays.

Likely states may include concepts such as:

```
SAFE / DISARMED
READY
FORWARD_ACCEL
FORWARD_CRUISE
FORWARD_DECEL
BOTTLE_APPROACH
BOTTLE_PUSH
FORWARD_BRAKE
SETTLE
REVERSE_ACCEL
REVERSE_CRUISE
FINAL_APPROACH
FINAL_BRAKE
FINISHED
FAULT
```

Names and exact states may change as the physical design becomes clearer.

State transitions should have understandable conditions and failure behavior.

Avoid timing an entire run using `delay()`.

Use measured state, encoder position, elapsed time, or other appropriate conditions.

---

## 13. Nonblocking Firmware

Prefer nonblocking logic.

Use mechanisms such as:

```
millis()
```

and state transitions rather than long blocking delays.

Short hardware-required waits during initialization may be acceptable when justified.

The main control loop should remain responsive enough to:

* detect faults;
* update measurements;
* process state transitions;
* update motor commands;
* generate useful telemetry.

---

## 14. Encoder Code

Encoder handling must be treated carefully.

Explicitly document:

* which signals are used;
* direction convention;
* counts per mechanical revolution;
* whether counts are ×1, ×2, or ×4 quadrature interpretation;
* relevant gearbox ratio;
* conversion from encoder counts to vehicle displacement.

Where interrupts are used:

* keep ISRs short;
* avoid serial output inside ISRs;
* correctly handle shared/volatile data;
* take safe snapshots of counters when required.

Before trusting encoder-based distance control, verify experimentally that:

* forward motion has the expected sign;
* reverse motion has the expected sign;
* repeated revolutions produce repeatable counts;
* count scaling matches the expected mechanical system.

---

## 15. Braking and Reversal

A commanded stop position is not necessarily the physical stopping position.

Expect braking behavior to depend on factors such as:

* velocity;
* vehicle mass;
* battery condition;
* traction;
* motor driver behavior;
* drivetrain compliance;
* backlash.

Characterize stopping distance experimentally.

For direction reversal, use a deliberate sequence such as:

```
decelerate
    ↓
brake / stop
    ↓
settle if required
    ↓
reverse
```

Do not abruptly command high forward torque directly into high reverse torque without explicit justification and testing.

---

## 16. Telemetry and Diagnostics

Diagnostic visibility is a first-class feature.

The firmware should make it possible to determine what it believed was happening during a test.

Useful telemetry may include:

* timestamp;
* current state;
* encoder ticks;
* calculated distance;
* calculated velocity;
* target distance;
* remaining distance;
* motor command;
* battery voltage if available;
* fault state;
* active run configuration.

Prefer machine-readable output.

Example style:

```
EV,t_ms=5832,state=DECEL,distance_mm=6214,velocity_mm_s=714,target_mm=6500,pwm=74
```

Do not flood serial output so heavily that logging changes timing-sensitive behavior.

Do not perform expensive formatting from interrupt handlers.

---

## 17. Diagnostic Modes

Before building automatic competition behavior, expose useful diagnostic operations where practical.

Examples may eventually include:

```
status
arm
disarm
motor <value>
brake
encoder
test distance <mm>
test brake <profile>
test run <profile>
```

Diagnostics must still obey safety constraints.

Do not allow malformed serial input to result in uncontrolled motion.

---

## 18. Calibration

Tune behavior through centralized configuration.

Avoid recompiling solely because a calibration constant was hidden inside unrelated logic when a cleaner configuration mechanism is reasonable.

Potential calibration values include:

* effective distance per encoder tick;
* acceleration profile;
* cruise command;
* approach speed;
* braking threshold;
* forward brake compensation;
* reverse brake compensation;
* settle interval;
* bottle-interaction parameters.

Calibration values must have:

* clear names;
* units;
* comments when their meaning is non-obvious.

Do not invent final tuning values before testing.

---

## 19. Data Analysis

Keep raw physical-test data separate from derived results when practical.

Future tooling may use Python or other scripts to calculate:

* mean error;
* median error;
* standard deviation;
* min/max;
* repeatability;
* braking-distance relationships;
* speed relationships;
* battery correlations;
* outliers.

Do not manually cherry-pick good runs.

Preserve enough context to explain rejected runs.

---

## 20. Optional Sensors Are Escalations

A passive ground/dead-wheel encoder may eventually provide a better estimate of actual vehicle displacement than a motor encoder if wheel slip becomes significant.

An IMU may eventually help characterize heading/drift.

Neither should be added merely because support can be coded.

Before proposing additional hardware, identify:

1. the measured problem;
2. its magnitude;
3. evidence that the existing system cannot adequately address it;
4. how the proposed hardware would improve the measurement/control loop;
5. added mechanical, electrical, and software complexity.

---

## 21. Offline and Deterministic Operation

Competition firmware must operate completely on the microcontroller.

Do not introduce runtime dependence on:

* cloud services;
* internet connectivity;
* remote APIs;
* external AI services;
* a connected development computer.

Development may use powerful external tools and coding agents, but deployed vehicle behavior must be self-contained.

---

## 22. Coding-Agent Workflow

Before making changes:

1. inspect relevant files;
2. inspect `git status`;
3. understand the current stage of bring-up;
4. identify whether requested behavior depends on unverified hardware facts.

While making changes:

1. keep the diff focused;
2. preserve known-good functionality;
3. avoid unrelated cleanup;
4. do not rewrite large working sections without a clear reason;
5. add diagnostics for new hardware behavior;
6. keep configuration explicit.

After making firmware changes:

1. run `pio run`;
2. inspect compiler warnings/errors;
3. run relevant automated tests if present;
4. summarize exactly what changed;
5. distinguish code verification from physical verification;
6. provide a concise physical-test procedure when hardware validation is still required.

---

## 23. Git Discipline

Before editing:

```
git status
```

Do not overwrite or discard unrelated uncommitted user work.

Do not run destructive Git operations such as:

```
git reset --hard
git clean -fd
git push --force
```

unless explicitly requested and the consequences are understood.

Do not commit automatically unless requested by the user or the active task explicitly calls for a commit.

When commits are requested:

* keep commits logically scoped;
* use descriptive commit messages;
* never commit build artifacts;
* never commit secrets;
* ensure the build succeeds unless the commit intentionally captures a known failure for debugging.

Maintain recoverable known-good checkpoints during hardware bring-up.

---

## 24. Repository Hygiene

Do not commit generated PlatformIO build output such as:

```
.pio/
```

Do not commit:

* temporary logs unless intentionally preserved;
* raw generated artifacts with no value;
* credentials;
* API tokens;
* private keys;
* machine-specific junk.

Do commit source-controlled configuration needed to reproduce the firmware build.

---

## 25. Dependency Policy

Prefer the Arduino framework and standard facilities when they are sufficient.

Before adding a library:

1. explain what problem it solves;
2. determine whether existing code/platform facilities already solve it;
3. consider maintenance and compatibility cost;
4. use a specific dependency version when reproducibility matters.

Do not add libraries for trivial functionality.

---

## 26. Resource Awareness

The Nano R4 is a microcontroller, not a desktop computer.

Avoid unnecessary:

* dynamic allocation;
* large buffers;
* repeated heap churn;
* large object graphs;
* verbose runtime abstractions.

Favor clear and predictable embedded code.

Do not prematurely micro-optimize code whose resource use is already negligible.

---

## 27. Warnings and Errors

Treat compiler warnings as signals worth investigating.

Do not suppress warnings merely to obtain a clean build unless the warning has been understood and suppression is justified.

Runtime faults should fail safely and expose enough information for diagnosis when possible.

---

## 28. Testing Philosophy

Use the smallest test capable of answering the current question.

Examples:

Instead of:

> Test the whole competition sequence.

prefer:

> Does the encoder count correctly for ten manual wheel rotations?

Then:

> Does the motor stop after a low-speed 250 mm command?

Then:

> How far does the vehicle coast after braking from this speed?

Build confidence layer by layer.

---

## 29. Simulation and Host-Side Tests

Host-side simulation or unit tests are encouraged for logic that does not require the real vehicle.

Good candidates include:

* state-machine transitions;
* calibration calculations;
* distance conversions;
* motion-profile generation;
* fault transitions;
* configuration validation.

Simulation does not replace physical testing.

A simulated success must never be reported as proof that the real vehicle behaves correctly.

---

## 30. Do Not Optimize for AI Convenience

The repository should be optimized for:

* reliable competition behavior;
* human understanding;
* reproducible testing;
* fast diagnosis.

Do not restructure working firmware merely because a coding agent prefers a particular architecture.

Agents are replaceable.

The vehicle and its test history are the source of truth.

---

## 31. Escalation Rule

When a problem appears, investigate in roughly this order:

1. measurement mistake;
2. configuration mistake;
3. wiring/electrical issue;
4. mechanical issue;
5. basic firmware bug;
6. calibration issue;
7. control-strategy limitation;
8. need for additional hardware or algorithmic complexity.

Do not jump directly to Step 8.

---

## 32. Definition of Done for a Firmware Change

A firmware change is not complete merely because code was produced.

For purely software changes, completion generally requires:

* implementation complete;
* `pio run` succeeds;
* relevant tests succeed;
* diff reviewed for unintended changes.

For hardware-dependent changes, completion additionally requires either:

* successful physical validation;

or

* a clearly stated pending physical test with exact instructions and expected evidence.

---

## 33. Primary Engineering Principle

When choosing between:

* clever code and measurable reliability;
* theoretical precision and experimental calibration;
* architectural elegance and fast diagnosis;
* additional features and a known-good vehicle;

prefer the option that increases **safe, repeatable, measurable competition performance**.
