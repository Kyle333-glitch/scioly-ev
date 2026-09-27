# Motor driver: planned DRV8874 PH/EN interface

**Status: PLANNED INTERFACE. Every electrical fact below is UNVERIFIED.**

This file records what the firmware *intends* to do with the motor driver, why
that plan was chosen, and exactly which measurements would replace it with a
known fact. It is not a wiring record and it is not evidence about the vehicle.

`VERIFIED_STATE.md` governs: a datasheet, a product page, a vendor example and a
successful compile do not make anything verified. Rows **#1** (wiring and pin
assignments), **#2** (driver truth table) and **#3** (motor polarity) stay
UNVERIFIED until the bench tests at the bottom of this page are run and recorded.

## The device and the interface this plan assumes

The planned driver is a **TI DRV8874** single H-bridge, used in **PH/EN mode**.

Facts about the *chip* that the plan rests on (datasheet-derived design facts,
recorded here as the basis for the plan - **never** as measurements of this
vehicle):

| Chip fact | Where it comes from |
|---|---|
| The control interface is latched at power-up from the PMODE pin; PMODE tied low selects PH/EN mode. | DRV8874 datasheet, PMODE description. |
| In PH/EN mode, `EN/IN1` is the speed (PWM) input and `PH/IN2` is the direction input. | DRV8874 datasheet, PH/EN control-mode description. |
| `nSLEEP` is an active-low enable: taking it low puts the device into its low-power sleep state, and the device is enabled by taking it high again. | DRV8874 datasheet; the `tSLEEP`/wake description. |
| `nFAULT` is an open-drain, active-low fault output. `IPROPI` is an analog output proportional to the load current. | DRV8874 datasheet, pin functions. |

Reference: TI DRV8874 product datasheet <https://www.ti.com/lit/ds/symlink/drv8874.pdf>
(the PDF is not stored in this repository).

## The planned input levels per command

This is the table the firmware implements. The authoritative copy in code is
`ev::drv8874InputsFor()` in `lib/evcore/src/MotorDriverPlan.h`; the only code that
writes the pins is `evhw::motorDriver` in `src/MotorDriver.h`.

| Authorized command | `nSLEEP` | `PH` | `EN` | Intent |
|---|---|---|---|---|
| `Disabled` | LOW | LOW | LOW (0 duty) | Driver asleep, outputs de-energized. The safe state, and the state startup initializes to. |
| `Coast` | LOW | LOW | LOW (0 duty) | Same de-energized state. The plan cannot name a distinct coast combination yet. |
| `Brake` | HIGH | LOW | LOW (0 duty) | Driver enabled with no drive level applied. **Which electrical state this is (brake or coast) is not claimed and is measured first.** |
| `DriveForward` | HIGH | HIGH | duty | Planned: PH high means the physical forward direction. **Unverified.** |
| `DriveReverse` | HIGH | LOW | duty | Planned: PH low means reverse. **Unverified.** |

Notes:

* the duty is `magnitudePermille` (0-1000) of full scale, mapped onto the PWM
  timer's counts (`ev::pwmCountFromPermille()`); the adapter programs the core's
  `analogWriteResolution()` explicitly to 8 bits rather than inheriting a default;
* an authorized Drive with a magnitude of 0 is a legal level combination and
  applies no drive. It is **not** a stop: only `Disabled`, `Coast` and `Brake` are;
* the mapping is the "fourth encoding" of the one polarity fact
  (`VERIFIED_STATE.md` #4). The other three encodings
  (`MotionProfileLimits::positiveCommandDirection`,
  `VelocityControllerConfig::positiveEffortMode`,
  `positiveTicksDirection`) are checked against each other by
  `ev::polarityAgrees()` / `ev::tickPolarityAgrees()`, but the PH level →
  direction sense is a *wiring* fact and is checked only by this bench test. If
  the bench result contradicts the table above, the fix is to change this table or
  the wiring - never to add a sign somewhere else.

## What the plan deliberately does not claim

* whether `EN` low (with the driver awake) brakes or coasts, and what decay path
  either uses;
* the effective PWM frequency the core produces for that pin and whether it suits
  the driver and the motor, and the resulting torque ripple, audible noise and
  heating;
* the logic levels the driver sees, and whether a level-shifter or pull-up/down is
  actually needed on any line;
* the sleep/wake timing, and whether a wake delay is required before a drive
  command is trusted;
* the current-regulation configuration (fixed off-time vs cycle-by-cycle, ITRIP,
  the IMODE pin state) and whether the board has current limiting at all;
* what `nFAULT` does when it asserts: whether the board pulls it up, when it
  releases, and whether the driver retries automatically;
* whether the firmware's `nFAULT` input is even connected to the driver's output.

None of the above is guessed at in firmware. The firmware *does* sample `nFAULT`
as a raw level, and it does **not** convert that level into a fault identity,
because no identity can be derived from an unverified pin
(see `DEVELOPMENT_LOG.md` D13).

## Bench tests that replace the plan with facts

All of them follow the safety rules in `AGENTS.md` section 5: **driven wheels off
the floor or the drivetrain otherwise unloaded**, one behavior at a time,
conservative effort, and a way to stop available and tested before any aggressive
motion. Nothing here is done until a wiring continuity check has been recorded.

1. **Wiring continuity (#1).** Continuity-check every pin in `src/PinMap.h`
   against the physical harness and record the table. Required before powering the driver:
   a wrong EN/PH pin is a direction and speed surprise.
2. **Idle levels, driver asleep.** With the motor supply off, measure `nSLEEP`,
   `PH`, `EN`, `nFAULT` at rest. Record whether the firmware's de-energized levels
   (`nSLEEP` low, `PH` low, `EN` low) are what the pins actually do, and whether
   `nFAULT` reads high with the internal pull-up.
3. **Truth table, drivetrain unloaded (#2).** With a low supply current limit,
   apply each row of the table above for a short bounded time and record the
   observed output state (both outputs measured or the shaft observed):
   sleep, `EN` low with `nSLEEP` high (brake or coast?), `PH` high drive, `PH` low
   drive, and the duty at 10% / 50% / 100%. Record the direction the shaft turns.
4. **Direction sense (#3).** From the row above, record which `PH` level turns the
   shaft in the vehicle's forward direction, and set
   `vehicleConfig::kPlannedPositiveCommandDirection` / `kPlannedPositiveEffortMode`
   from that one measurement (they are the same fact).
5. **nFAULT behavior.** With the current limit deliberately low, provoke a trip
   and record what `nFAULT` does, how long it stays low, whether the driver
   recovers by itself, and what the outputs do meanwhile. Only after this is
   recorded may the firmware map the raw level onto a fault identity and a stop
   policy.
6. **PWM behavior.** Record the actual switching frequency at `EN` and the
   smallest duty at which the shaft moves from rest. Both are needed before the
   effort scale means anything.

Each completed test updates `VERIFIED_STATE.md` (#1-#3 as applicable), this file's
tables, and `DEVELOPMENT_LOG.md`, in one change.
