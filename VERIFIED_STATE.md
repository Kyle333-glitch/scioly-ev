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

The planned interface and the bench procedure that would verify rows #1-#3 are
recorded in `docs/hardware/motor-driver-drv8874.md`. That file is a plan, not
evidence: it changes no status in the table above, and the pins it names
(`src/PinMap.h`) are placeholders.

## Software and toolchain facts

These are verified by reproducible commands, **not** by physical testing. They
say nothing about the vehicle.

| Fact | Status | Evidence |
|------|--------|----------|
| `env:nano_r4` firmware builds | VERIFIED | `pio run` succeeds (see `DEVELOPMENT_LOG.md`) |
| Host logic tests build and run with the `native` environment | VERIFIED | `pio test -e native` succeeds (see `DEVELOPMENT_LOG.md`) |
| The vehicle-layer runtime shell and the hardware boundary compile and link into the firmware (driver adapter, encoder interrupt, raw inputs, scheduler, tick order, watchdog adapter) | VERIFIED | `pio run` succeeds with `src/` compiled (see `DEVELOPMENT_LOG.md` D13). This is a build fact only. |
| The watchdog adapter's library links into the firmware image | VERIFIED | `arm-none-eabi-nm` on `firmware.elf` lists the WDT symbols (see `DEVELOPMENT_LOG.md` D13). Linking is **not** evidence that the watchdog starts, counts, refreshes or resets. |
| Nothing but the driver adapter writes the planned motor pins | VERIFIED | A static search over `src/` for the three planned pin names matches only `src/PinMap.h` (the definitions) and `src/MotorDriver.h` (the writes), and the bench tree reaches the pins through `MotorDriver.h`'s `applyBench()` rather than writing them itself. The vehicle image does not contain the bench tree at all - see the `env:bench` row below (see `DEVELOPMENT_LOG.md` D13 and D19). A search is evidence about the source, not about the hardware. |
| The driver's fault output is interpreted only from a measured polarity | VERIFIED | `lib/evcore/src/DriverFault.h` maps a raw level to `FaultCode::DriverFaultBar` only when the configured polarity is `Measured`; the shipped `kPlannedDriverFaultPolarity` is `Unverified`, the raw level is reported instead, and the `DriverFaultPolarityVerified` configuration check keeps the vehicle unarmable until H1B has measured the line (see `DEVELOPMENT_LOG.md` D19). This is a logic fact: which level the line actually drives for each state is UNVERIFIED (row #1, #2 above). |
| The pure core compiles for the ARM target | VERIFIED | `arm-none-eabi-g++ -std=gnu++11 -Wall -Wextra -fsyntax-only` over all `lib/evcore/src` headers: exit 0. The only diagnostics are the expected `#pragma once in main file` artifacts of checking headers standalone (one per header); any other diagnostic is a real warning to investigate. |
| The bench image builds, and the vehicle image contains none of it | VERIFIED | `pio run -e bench` succeeds and `pio run -e nano_r4` succeeds, and a symbol search over the two `firmware.elf` files finds the bench console's symbols in the bench image and none in the vehicle image (see `DEVELOPMENT_LOG.md` D19); the two environments are disjoint by `build_src_filter` in `platformio.ini`. This is a build-and-link fact only: no bench instrument has been run, and nothing here says the readouts or the bench drive behave correctly on the board. |
| The vehicle image contains no bench-named symbol, and enabling the watchdog for a bench qualification cannot change it | VERIFIED | `arm-none-eabi-nm` over `.pio/build/nano_r4/firmware.elf` filtered for `bench|hang` matches nothing; over `.pio/build/bench/firmware.elf` it matches the console, the bench drive, the hang hook and the grammar. Watchdog enablement is derived from the build define `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`, which only `[env:bench-watchdog]` sets, and `src/main.cpp` has a `static_assert` that refuses to compile the vehicle image if the define is ever present (see `DEVELOPMENT_LOG.md` D20). Build facts only: nothing here says the bench instruments or the watchdog behave correctly on the board. |
| Only one writer commands the motor pins per pass, and the bench writer is gated on a host-tested predicate | VERIFIED | `src/MotorDriver.h` skips the tick's write while the bench layer holds the pins (`motorDriver::setMotorPinsOwned()`), and the bench layer sets that only where `ev::benchApplyVerdict()` (`lib/evcore/src/MotorArbitration.h`) permits - refusing on a latched fault, a violated control-tick order, an armed run or a run under way; `VehicleRuntime` exposes `faultLatched()`, `runUnderway()` and `orderViolationDetected()` for it, and `test_bench_arbitration` covers the predicate including the two states that are `!armed` while the motor is *not* free (see `DEVELOPMENT_LOG.md` D20). This is a logic-and-build fact. `failSafe()` deliberately ignores the flag, and whether the DRV8874's pins are commanded as intended is UNVERIFIED (rows #1-#3). |
| The driver-fault-low cycle counter is per run, not since boot | VERIFIED | `VehicleRuntime::cyclesWithDriverFaultLow_` (`src/VehicleRuntime.cpp`) is cleared on a successful `arm` and by `reset`, so H1B Part 3 and H9 read their own test's activity rather than a since-boot total (see `DEVELOPMENT_LOG.md` D20). A source fact only: the counter's value on hardware has not been observed. |
| The watchdog timeout rule is enforced at compile time | VERIFIED | `lib/evcore/src/WatchdogPlan.h` holds the rule (a timeout must be strictly longer than both the boot-to-first-refresh gap and the worst healthy refresh interval, plus margin) and `src/WatchdogCheckpoint.h` asserts the requested constants against it, so a build that requests enablement with a timeout that cannot survive boot fails to compile; `test_watchdog_plan` pins the arithmetic. This is a source-and-logic fact: how long the real boot gap or refresh interval is has not been measured, and neither placeholder is a measurement. |
| The operator interface and the telemetry consumer compile and link into the firmware (command parser, operator policy, response formatter, telemetry schema/formatter/queue, line transport) | VERIFIED | `pio run` succeeds with `src/` compiled (see `DEVELOPMENT_LOG.md` D14), and the pure modules are covered by host tests (`test_serial_command_parser`, `test_operator_policy`, `test_operator_response`, `test_telemetry_format`, `test_telemetry_queue`). This is a build-and-logic fact only; no host was connected. |
| No control path depends on the operator interface or the telemetry session, so ignoring or abusing serial cannot change what the chain decides | VERIFIED | `test_serial_independence` runs the same scripted run twice - once with the consumer hierarchy fully driven by a stalled host and a stream of admitted, rejected and refused commands, once without it - and asserts equal tick counts, equal dropped deadlines and an identical per-tick trace of authorization outcome, motor mode and effort; separately that no tick ever produced `AuthorizedDrive`/`AuthorizedClamped`, that the sample lane holds exactly `kTelemetrySampleLaneCapacity` before counting drops, and that every queued-plus-dropped sample is accounted for. This is evidence about the logic, not about a real serial link. |

### Behavior this build depends on

The first two rows are read from the **installed** Renesas core sources this build
actually compiles, so they are statements about the code the firmware will run
rather than about a datasheet or an example; the third is about the host test
build. None of them is a bench observation: how a *real* host drains the port was
not measured.

| Fact | Status | Evidence |
|------|--------|----------|
| On this target `Serial` is USB-CDC, and `_SerialUSB::write(const uint8_t*, size_t)` **blocks** until the host drains the buffer; it also disables the USB interrupt while it waits and returns 0 when no host is connected. `availableForWrite()` does not block. | VERIFIED (source inspection) | `~/.platformio/packages/framework-arduinorenesas-uno/cores/arduino/usb/SerialUSB.cpp`: `write(const uint8_t *buf, size_t size)` loops on `tud_cdc_write_available()` until every byte is sent, with `usbd_int_set(false)` around the loop. `src/SerialTransport.h` therefore gates every write on `availableForWrite()`, as `SOFTWARE_ARCHITECTURE.md` section 9 requires. |
| The USB-CDC endpoint buffers on this board are 256 bytes each way, with a 64-byte endpoint packet size. | VERIFIED (source inspection) | `~/.platformio/packages/framework-arduinorenesas-uno/variants/NANOR4/tusb_config.h` defines `CFG_TUD_CDC_RX_BUFSIZE` and `CFG_TUD_CDC_TX_BUFSIZE` as `(TUD_OPT_HIGH_SPEED ? 512 : 64) * 4` and `CFG_TUD_CDC_EP_BUFSIZE` as `(TUD_OPT_HIGH_SPEED ? 512 : 64)`. The high-speed branch does not apply here: the variant sets `CFG_TUD_MAX_SPEED` to `OPT_MODE_DEFAULT_SPEED`, which is `0x0000` (`tusb_option.h`), so `TUD_OPT_HIGH_SPEED` falls back to `TUP_RHPORT_HIGHSPEED`, and the RA family is not one of the MCUs that define it as `1` (`tusb_mcu.h`). The result is a small window, which is why a record is dropped rather than waited on. |
| Unity's double assertions are disabled in this project's test build, so `TEST_ASSERT_EQUAL_DOUBLE` cannot be used and host tests compare doubles with an explicit tolerance and `fabs`. | VERIFIED | The vendored Unity (`Unity/src/unity_internals.h`) defines `UNITY_EXCLUDE_DOUBLE` unless `UNITY_INCLUDE_DOUBLE` is defined, and this project's `unity_config.h` does not define it. |

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
   (unloaded drivetrain, conservative PWM, one behavior at a time). The exact
   procedures - prerequisites, variables, measurements, repetitions, safe-stop
   conditions and the acceptance rule - are in `docs/bringup/`. They are written
   ahead of the hardware, are not evidence, and change no row until they pass.
2. Record the raw measurement or observation, not just a conclusion.
3. Update the row: status **VERIFIED**, evidence pointer, and date.
4. Add an entry to `DEVELOPMENT_LOG.md` describing what was tested and observed.
5. If the result contradicts an assumption, fix the assumption; do not adjust
   the numbers to preserve it.
6. Carry the evidence pointer into the code: a value promoted to `Measured` (or
   `RuleDerived`) must name where its evidence is recorded, through
   `Sourced::citesEvidence()`, or the `MeasuredValuesCiteEvidence`
   configuration-build check reports false and the arm gate refuses. A promotion
   without a citation is not a promotion - it is the one edit that lets a
   provisional number start authorizing motion while looking like every other
   measured one.
