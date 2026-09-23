# Verified State

This file records what the project **knows**, as opposed to what it assumes.
It exists to stop guesses from being treated as measured facts.

## The rule

A hardware or physical fact may be marked **VERIFIED** only when it is supported
by direct **on-target or physical evidence** that has been recorded (command or
test procedure plus raw observation/data), and the promotion is logged in
`DEVELOPMENT_LOG.md`.

None of the following make a fact verified:

- code that compiles, links, or uploads;
- a datasheet, product listing, wiring diagram, or vendor example;
- a host-side test, simulation, or unit test;
- an assumption, convention, or value that seems obvious;
- a value copied from another project;
- recollection or verbal description.

**Default status is UNVERIFIED.** A fact that is not listed here is unverified.

Downstream rule: code must not silently depend on an UNVERIFIED fact. Where a
value depends on one, it is configurable and clearly marked as a placeholder
(see `SOFTWARE_ARCHITECTURE.md` section 7).

## Current hardware availability

No vehicle hardware is available to the team at this stage. Every hardware fact
below is therefore UNVERIFIED, and no motion-related behavior may be written as
though these were known.

## Hardware facts

| # | Fact | Status | Evidence | What would verify it |
|---|------|--------|----------|----------------------|
| 1 | Wiring and pin assignments | UNVERIFIED | none | Continuity check against a recorded wiring table; observed correct behavior per pin |
| 2 | Motor driver modes / truth table | UNVERIFIED | none | Driver powered with the drivetrain unloaded; each input combination observed and tabulated |
| 3 | Motor polarity (sign of a positive command) | UNVERIFIED | none | Low conservative PWM command; observed wheel direction recorded |
| 4 | Encoder direction sign (forward vs reverse counting) | UNVERIFIED | none | Hand-rotated wheel in each direction; raw count sign recorded |
| 5 | Encoder signal quality (noise, missed/extra counts) | UNVERIFIED | none | Repeated manual revolutions with count comparison; scope or count-delta check |
| 6 | Rollout (effective distance per encoder tick) | UNVERIFIED | none | Measured travel over multiple revolutions on a real surface, averaged |
| 7 | Sensor scaling (any sensor to engineering units) | UNVERIFIED | none | Reference measurement compared against reported value at several points |
| 8 | Control gains / tuning values | UNVERIFIED | none | Tuning against recorded closed-loop response data |
| 9 | Braking behavior and stopping distance | UNVERIFIED | none | Repeated measured stopping distance at recorded speeds |
| 10 | Wheel slip (does the motor encoder track ground travel?) | UNVERIFIED | none | Motor-encoder distance compared against physically measured travel |
| 11 | Bottle interaction behavior | UNVERIFIED | none | Recorded runs observing contact, push distance, and outcome |
| 12 | Benefit of a ground / dead-wheel encoder | UNVERIFIED | none | Measured slip magnitude large enough to justify the added hardware |

## Software and toolchain facts

These are verified by reproducible commands, **not** by physical testing. They
say nothing about the vehicle.

| Fact | Status | Evidence |
|------|--------|----------|
| `env:nano_r4` firmware builds | VERIFIED | `pio run` succeeds (see `DEVELOPMENT_LOG.md`) |
| Host logic tests build and run with the `native` environment | VERIFIED | `pio test -e native` succeeds (see `DEVELOPMENT_LOG.md`) |
| The pure core compiles for the ARM target | VERIFIED | `arm-none-eabi-g++ -fsyntax-only` over all `lib/evcore/src` headers, no warnings |

## Rule source

The event rules are an official document, not a measurement. A rule citation
establishes what the event requires and is recorded as `ValueSource::RuleDerived`,
never as `Measured`.

| Item | Status | Evidence |
|------|--------|----------|
| Official event rules for the 2027 season | AVAILABLE | `docs/rules/2027-div-c-electric-vehicle.pdf` (Division C rules; Electric Vehicle event on pages C26-C31). Extracted values are recorded in `docs/rules/README.md`; the authoritative copy in code is `ev::rules2027`. |

The rule-derived facts used by the firmware are Target Distance (rule 8.h),
Target Time (rule 8.i), Bottle Line Distance (rule 8.j), and bottle placement
(rule 7.c.ii.(5)). They constrain the *event*, not the vehicle, so they do not
appear in the hardware table above.

## How to promote a fact

1. Perform the test on real hardware, following the safety rules in `AGENTS.md`
   (unloaded drivetrain, conservative PWM, one behavior at a time).
2. Record the raw measurement or observation, not just a conclusion.
3. Update the row: status **VERIFIED**, evidence pointer, and date.
4. Add an entry to `DEVELOPMENT_LOG.md` describing what was tested and observed.
5. If the result contradicts an assumption, fix the assumption; do not adjust
   the numbers to preserve it.
