# Operating the EV without a laptop

This is the operator guide for the laptop-free images:

| Image | Environment | What it is |
|---|---|---|
| **Immediate standalone (default)** | `env:nano_r4` | Open-loop, pencil-triggered movement sequence (`POWER_OFF → INITIALIZING → WAITING_FOR_TRIGGER → FORWARD → STOPPING → REVERSE → FINISHED`). No laptop, no serial arming, no 1.5 s hold gesture, and no competition calibration gates. |
| **Standalone diagnostic** | `env:standalone-diag` | Supervised, wheels-raised test: one bounded, low-effort motor pulse from the physical pencil trigger. Proves the trigger, the driver enable path and the onboard battery supply work with no computer attached. |
| **Standalone competition** | `env:nano_r4-competition` | The calibrated competition image. Arms itself from the physical trigger (1.5 s hold), loads the competition preset, and runs the full competition state machine after a start press once calibration in section E is complete. |

Status of each claim in this file:

* **Software implemented and built** — both images compile, and the logic is
  covered by host tests (`test/test_standalone/`).
* **Bench testing outstanding** — no motor has been powered and no trigger has
  been pressed by a coding agent. Every physical step below is a step you perform
  and record.
* **Competition performance verified** — not yet, and it cannot be until
  section E is done on the real vehicle.

Two hard rules before anything else:

1. **The motor supply disconnect is the primary stop.** Keep it within reach and
   know how to open it. No firmware behavior in this document replaces it.
2. **Never power the motor driver and assume the Nano is powered** — and never
   feed the pack into the Nano until the supply-path check in section B1 is
   recorded. The repository states the Nano's supply pin as *unknown*
   (`docs/bringup/E0-wiring-and-component-inventory.md`, item **W-04**;
   `docs/bringup/E0-hazard-register.md`, hazard **H-03**). That is not a
   formality: pack voltage on the wrong Nano input destroys the board.

---

## A. Uploading the standalone diagnostic firmware

From PowerShell:

```powershell
Set-Location "$HOME\Projects\scioly-ev"
pio run -e standalone-diag
pio run -e standalone-diag -t upload
```

The upload step runs `scripts/nano_upload_touch.py`, which performs the
1200-bps touch the board's DFU bootloader needs and then flashes. The COM port
disappearing and reappearing during this is normal.

Optional confirmation that the right image is on the board (USB attached, before
you go untethered):

```powershell
pio device monitor
```

Expect a `BOOT,...` line whose identity contains `variant=standalone-diag`. Type
`status` if you want the live state; `Ctrl+C` leaves the monitor. **Close the
monitor before the untethered test** — a monitor holding the port is still a
laptop in the loop.

---

## B. Testing without the computer

### B1. Electrical checks before the first untethered run (once)

Record each answer; these are the facts this whole section depends on.

1. **Which Nano input is fed, and from what.** Trace the wire from the battery /
   switch output to the Nano. Record whether it lands on **VIN**, on the **5V**
   pin, or on USB, and what the measured voltage is there. Compare VIN against
   the Nano R4's documented VIN range, and confirm the 5V pin is only ever fed by
   a *regulated* 5 V source — feeding the pack into 5V destroys the board.
   (Open items W-04 in the wiring inventory and H-03 in the hazard register.)
2. **Ground.** Continuity between battery negative, driver ground and Nano
   ground.
3. **Motor supply disconnect.** A switch, connector or fuse you can open while
   the vehicle is powered, reachable without reaching past the wheels.
4. **Wheels up.** The driven wheel(s) off the floor, vehicle restrained, nothing
   in the drivetrain's path.
5. **Driver output.** Confirm the driver board is the DRV8874 in PH/EN mode as
   documented, and that PH/EN/nSLEEP reach D8/D9/D10
   (`src/PinMap.h`; still an unverified harness statement).
6. **Trigger.** With the Nano powered and the trigger released, D12 reads HIGH
   (internal pull-up). Pressing the pencil pulls it LOW. If it reads LOW with
   nothing touching it, fix that before continuing — see F4.

### B2. The untethered diagnostic sequence

1. Motor supply **connected** but the disconnect within reach; wheels raised.
2. **Unplug USB.**
3. Turn on the vehicle power switch.
4. Watch the onboard LED (D13 / `LED_BUILTIN`):
   * **Slow heartbeat** (100 ms on every second): booting, or waiting for the
     trigger to be seen released. If the pencil is resting on the trigger, take
     it off — a trigger held during boot is deliberately ignored.
   * **Steady on**: ready. The next press applies the pulse.
   * **Fast blink**: fault latched (see F3/F4). Turn the power off and
     investigate; nothing will retrigger until the board is power-cycled.
5. **Press the pencil trigger briefly and release.**
6. Expect **one** short motor pulse — 50/1000 effort for at most 250 ms — then
   the LED switches to the **double-blink "finished"** pattern. The motor is
   disabled and stays disabled.
7. Pressing the trigger again does nothing. This is one activation per power
   cycle by design.
8. **Turn the power off.**

If the pulse happened: the trigger, the driver enable path and the onboard
supply work. That is all it proves — no distance, braking or repeatability claim
comes from it. Record what you saw (pulse yes/no, direction, LED pattern,
approximate duration) in your test notes.

---

## C. Uploading the autonomous firmware

### C1. Immediate standalone movement sequence (default `env:nano_r4`)

```powershell
Set-Location "$HOME\Projects\scioly-ev"
pio run -e nano_r4
pio run -e nano_r4 -t upload
```

`pio run -e nano_r4 -t upload` installs the immediate open-loop standalone
sequence (`src/main.cpp` + `lib/evcore/src/ImmediateSequence.h`):

1. Power on with the pencil trigger released. The orange LED blinks a slow
   heartbeat for 100 ms while `INITIALIZING`, then turns **steady ON** in
   `WAITING_FOR_TRIGGER`.
2. Activate the pencil trigger **once** (≥ 40 ms debounced press; no 1.5 s hold
   required).
3. The vehicle executes `FORWARD` (150/1000 effort for 1,000 ms) → `STOPPING`
   (motor disabled for 500 ms) → `REVERSE` (150/1000 effort for 800 ms) →
   `FINISHED` (motor disabled, double-blink LED, retrigger locked out until
   reset/power cycle, 4,000 ms overall run timeout).

Adjust effort and durations in the `IMMEDIATE MOVEMENT SEQUENCE CONFIGURATION`
block in `src/main.cpp`. Note the 500 ms `STOPPING` interval is a bounded
motor-off pause for this wheels-raised test mode, not a measured standstill;
see the stop-interval note in `src/main.cpp` before running on the ground.

Optional USB check (before going untethered, or to tell images apart):

```powershell
Set-Location "$HOME\Projects\scioly-ev"
pio device monitor
```

Expect one `EV BOOT ... variant=immediate-standalone ...` line (within about
0.5 s of the monitor attaching), then one `EV state=... trig_raw=... mode=...
permille=... ph=... en=... nsleep=...` status line every 500 ms. The status
line names the live phase, the raw D12 reading, the debounced trigger state,
the commanded motor mode/effort and the requested D8/D9/D10 levels, so
`state=WAITING_FOR_TRIGGER` with `trig_raw=1` means the right firmware is
running but the trigger has not been pressed, while a missing `EV BOOT` line
or a different `variant=` value means a different image is on the board.
The diagnostic is output-only (no motor-drive command exists in this image)
and is never required: with no USB host attached nothing is sent and the
sequence runs identically. **Close the monitor before the untethered test.**

### C2. Calibrated competition firmware (`env:nano_r4-competition`)

```powershell
Set-Location "$HOME\Projects\scioly-ev"
pio run -e nano_r4-competition
pio run -e nano_r4-competition -t upload
```

Optional identity check over USB: `pio device monitor`, expect
`variant=production` in the `BOOT,...` line.

> Do **not** run the `nano_r4-competition` image on the ground until section E is
> complete and the 1.5 s arm gesture produces a steady LED. Until then the arm is
> refused by design and the vehicle cannot move.

---

## D. Running the EV autonomously (competition image only)

This section is the calibrated competition image (`env:nano_r4-competition`).
It does **not** apply to the default immediate image in C1, which needs only
the single press described there and has no arm gesture.

### D1. What the controls do

* **Power switch** — powers the Nano (and, per your harness, the motor driver).
  It does not arm anything: a power-on or reset always leaves the motor
  de-energized.
* **Pencil trigger on D12** — two different gestures, deliberately:
  * **Hold 1.5 s → arm.** This loads the competition preset
    (`lib/evcore/src/StandalonePreset.h`), announces it, and arms it, through the
    same rule validation, configuration-build checks and safety authority a typed
    `announce`/`arm` uses.
  * **Short press → start.** Only meaningful once the LED is steady.

### D2. The sequence

1. Place the vehicle on the Start Point, wheels down, trigger released.
2. Turn the power on. LED: **slow heartbeat** (waiting).
3. **Hold the pencil on the trigger for about 1.5 seconds**, then release.
   * **Steady on** = armed and ready. The run is announced and the safety
     authority is armed.
   * **Heartbeat continues** = the arm was refused. Nothing can move. Connect USB
     and type `checks` (or `status`) to see which configuration check failed; the
     first failing check is the thing to fix (section E).
   * If the announcement itself was refused (the usual case before section E is
     complete) the run is still in `Idle`, and a new gesture is accepted after
     the trigger has been released and requalified.
   * If the announcement succeeded but the *arm* was refused, the run stays
     announced and un-armed; power-cycle the vehicle (or type `reset` over USB)
     and repeat the gesture.
4. **Press the trigger briefly.** The run starts: LED blinks while the run is
   underway.
5. The competition state machine executes: forward acceleration, approach, bottle
   contact/push, deceleration, stop before reversal, reverse, final approach,
   final stop. LED: **double blink** once the run reaches `Done` — the motor is
   disabled and held.
6. **Fast blink** means a fault latched. The motor is disabled; note the LED and,
   if a laptop is available, read `status` before power-cycling.

### D3. Starting another run

One prepared run per power cycle, on purpose. To run again:

* **Power-cycle the vehicle** (switch off, wait, switch on) and repeat D2; or
* with USB attached, type `reset` (which is the firmware's new-run boundary) and
  then repeat the arm gesture.

A reset or power-cycle never resumes an interrupted run, and it never moves the
motor: the fresh run needs a fresh 1.5 s hold and then a fresh press.

### D4. What to expect before calibration is complete

The preset is event geometry (Regional/Invitational, 5.00 m, 20.0 s, bottle line
1.00 m beyond the target) and **not** calibration. While any value in
`src/VehicleConfig.h` is `Provisional`, the arm is refused and the LED keeps the
heartbeat. That is the correct behavior: the firmware will not drive on an
unmeasured tick scale, brake offset or gain, and it will not pretend to.

---

## E. Remaining hardware calibration

Everything here lives in **`src/VehicleConfig.h`**. A value becomes usable only
when it is (a) replaced with the measured number and (b) wrapped as
`Measured` with a citation to the evidence file, for example:

```cpp
// before
constexpr double kPlannedDistancePerEncoderTickM = 0.0001;

// after (the number and the citation are both required)
constexpr double kPlannedDistancePerEncoderTickM = <measured>;
...
inline ev::VehicleCalibration calibration() {
    return ev::VehicleCalibration{
        ev::Sourced<double>(kPlannedDistancePerEncoderTickM, ev::ValueSource::Measured,
                            "data/evidence/<run>/<file>.txt"),
        ...
```

The `MeasuredValuesCiteEvidence` check fails a `Measured` value with no
citation, and `kFirmwareConfigSchema` bumps if the fingerprinted set changes.
`Sourced` construction examples already in the file: `completeStopDwellS()` and
`measuredGapMs()`.

Ordered by what blocks the most:

| # | Measurement | Constant / builder | How to measure |
|---|---|---|---|
| 1 | Tick-to-distance scale | `kPlannedDistancePerEncoderTickM` (~line 174) → `calibration()` | Slow rollout over a measured ≥ 3 m; `ticks / distance`. Repeat three times, use the mean, record every run. |
| 2 | Tick direction and command polarity | `kPlannedPositiveTicksDirection`, `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode` (~line 340) → `positiveTicksDirection()`, `profileLimits()` | Slow powered forward pulse with wheels up: encoder count increases when the vehicle is pushed forward, and `DriveForward` moves it forward. Sets all three encodings at once. |
| 3 | Effort ceiling | `kPlannedMaxEffortPermille` (~line 181) → `safetyLimits()`, `controllerConfig()` | Highest effort you are willing to command; the regulator ceiling and the safety ceiling read this one value. |
| 4 | Forward / reverse brake offset | `kPlannedForwardBrakeOffsetM`, `kPlannedReverseBrakeOffsetM` (~line 178) → `calibration()` | Braking-distance characterization: coast distance after the command reaches zero, per direction, at the cruise you intend to use. |
| 5 | Velocity ceilings and run-duration bound | `kPlannedMaxForwardVelocityMps`, `kPlannedMaxReverseVelocityMps`, `kPlannedMaxRunDurationS` (~line 190) → `safetyLimits()` | Highest speed observed in a controlled run plus margin; duration longer than any legal run. |
| 6 | Controller gains | `kPlannedFeedForwardPermillePerMps`, `kPlannedProportionalPermillePerMps`, `kPlannedIntegralPermillePerMpsSecond`, `kPlannedMaxIntegralPermille` (~line 210) → `controllerConfig()` | Step-response / constant-speed trace at the intended cruise; this is tuning, so it comes after 1-4. |
| 7 | Cruise, accel, decel | `kPlannedCruiseMps`, `kPlannedAccelMps2`, `kPlannedDecelMps2` (~line 197) → `profileLimits()` | Slowest cruise that reliably moves the loaded vehicle; the acceleration/deceleration the drivetrain follows without slip. |
| 8 | Stop definition | `kPlannedReversalStopThresholdMps`, `kPlannedReversalStopDwellS` → `reversalStopVerification()`, `finalStopVerification()` | Slowest speed the encoder can resolve near zero; dwell from the rule's Complete Stop (3 s) is already `RuleDerived`. |
| 9 | Bottle interaction | `kPlannedBottleContactLeadM`, `kPlannedBottleCaptureAdvanceM`, `kPlannedBottlePushMarginM` (~line 315) → `bottleEvidenceConfig()`; `kPlannedContactWindowToleranceM`, `kPlannedForwardStopMarginBeyondBottleLineM` (~line 335) → `runExecutionDependencies()` | Where the pusher actually touches, where the bottle is captured and released, and how far the forward stop sits beyond the Bottle Line. The forward-stop margin is what gives the run a forward execution target. |
| 10 | Phase bounds | `kPlannedApproachTimeoutS`, `kPlannedCaptureTimeoutS`, `kPlannedPushTimeoutS`, `kPlannedReverseTimeoutS`, `kPlannedFinalApproachTimeoutS`, `kPlannedFinalStopTimeoutS`, `kPlannedTravelStallTimeoutS`, `kPlannedEncoderProgressTimeoutS`, `kPlannedFinalApproachDistanceM`, `kPlannedMinimumCruiseMps` (~line 340+) → `runPhaseConfig()` | Each is a bound the run waits under; measure the time the corresponding evidence takes, then add margin. |
| 11 | Bottle-phase speed ceilings | `kPlannedApproachCeilingMps`, `kPlannedCaptureCeilingMps`, `kPlannedPushCeilingMps` → `runPhaseConfig()` | Per-phase speeds from the bottle integration test. |
| 12 | Reversal timeout | `kPlannedReversalTimeoutS` → `reversalConfig()` | How long the vehicle needs to verify a standstill before reversing, plus margin. |

Verify with `checks` (USB attached) after each group: `checks_all=1` and a steady
LED after the hold gesture mean the configuration is armable. **Do not** mark a
value `Measured` without recorded evidence; the firmware refuses to arm on a
placeholder, and that refusal is the feature.

Two values are *not* calibration and must stay as they are until their own
protocols say otherwise: `kRunTimingModelValidated` (the 20.0 s preset is a
rule-legal announced value, not a validated duration) and the battery-voltage /
nFAULT capabilities (those circuits are not installed).

---

## F. Troubleshooting

Distinguish three failure classes before changing code:

* **Firmware / configuration** — the image is wrong, or a check refuses.
* **Electrical / wiring** — power, ground, a pin or the driver.
* **Calibration** — the vehicle moves, but not the way the run expects.

| # | Symptom | Most likely class | What to do |
|---|---|---|---|
| F1 | Arduino does not power on after USB is unplugged | Electrical | The Nano is still being powered from USB only. Trace the supply to VIN/5V (W-04). If the supply is the pack into VIN, check the VIN range for the Nano R4 before connecting. If it is into 5V, it must be a regulated 5 V source. Never connect the pack to 5V. Measure the rail with a meter; do not guess from the LED. |
| F2 | Firmware flashes, then the USB COM port disappears | Expected / electrical | The `renesas-ra` uploader enters the DFU bootloader for the flash; the application's USB CDC port can take a moment to re-enumerate, and it vanishes again on reset during an untethered run. Only treat it as a fault if the board also fails to run (no LED heartbeat) — then re-upload over USB and check the supply. |
| F3 | LED never leaves the fast-blink fault pattern | Firmware | A fault is latched. Power-cycle, then (USB attached) read `status` for the fault name. Common causes: control-tick order violation (a firmware defect — report it), or a diagnostic stall fault caused by a disconnected encoder. |
| F4 | Trigger is always active (pulse fires immediately, or the LED never becomes ready) | Electrical | D12 is being held LOW. Check for a short to ground, a mis-wired switch, or a trigger mechanism resting on the contact. With the pencil off the trigger, D12 must read HIGH. Firmware deliberately refuses a boot-held trigger rather than treating it as a press. |
| F5 | Motor does not move when triggered (diagnostic image) | Electrical or firmware | In order: (1) is the LED steady before the press? (2) is the motor supply connected and the disconnect closed? (3) is nSLEEP actually going high and EN receiving PWM — measure at the driver pins. (4) did you already use this power cycle's one activation? Power-cycle to reset it. The host test suite cannot tell you any of this: it is a measurement. |
| F5a | LED sequence runs but the motor does not move (immediate image) | Electrical | The firmware did its part if the LED went running → double-blink. In order: (1) over USB, confirm `EV BOOT variant=immediate-standalone` and that the status line shows `mode=DriveForward permille=150 nsleep=1` during the run — otherwise the wrong image is flashed. (2) motor supply connected and disconnect closed? (3) measure nSLEEP/PH/EN at the driver pins during FORWARD. (4) 15% PWM may not break static friction: raise effort in small steps in `src/main.cpp` (never above 500) with wheels raised; do not assume a nonzero command means rotation. |
| F6 | Motor does not move when triggered (competition image) | Configuration | The arm was refused. The LED stays in the heartbeat. Attach USB, `checks`, and fix the first failing check (section E). Note that arming also requires a usable execution plan, so an unmeasured forward-stop margin refuses the arm even with all checks otherwise green. |
| F7 | Encoder readings never change | Electrical | With wheels raised, push the vehicle by hand and read `status` (`enc=` / tick count). No change means the A/B channels are not reaching D2/D4, the pull-ups are wrong for the encoder's output type, or the encoder is unpowered. Do not tune the tick scale against a dead encoder. |
| F8 | Run fails readiness checks | Configuration | Expected until section E is complete; `checks` names the first failing check. Do not relabel a value `Measured` to get past it. |
| F9 | Motor starts in the wrong direction | Calibration / wiring | Stop immediately (open the motor disconnect). The tick/command polarity encodings (item 2 in section E) must be established together; a `DriveForward` that moves the vehicle backwards is a measurement to record, not a value to guess. |
| F10 | Motor does not stop at the target | Calibration | The brake offset and the tick scale are unmeasured or wrong, or the vehicle is slipping. Record requested vs actual distance from several runs; a consistent offset is calibratable, a scattered one is mechanical (traction, backlash, bottle contact) and must not be papered over with constants. |
| F11 | Nano resets while the motor is powered | Electrical | Supply sag or EMI from the motor. Check the 5 V rail with a meter during a pulse, check grounds are shared and solid, and check the driver's supply decoupling. A reset leaves the motor de-energized and disarmed by design — but find the cause; a brown-out during a competition run is a lost run. |
| F12 | The hold gesture never produces a steady LED, and `checks_all=1` | Firmware / wiring | Confirm the trigger actually reaches D12 (F4), and that the trigger is *released* for at least 100 ms before the hold (the gesture is not accepted while the contact is still qualifying after boot). |

---

## Related documents

* `VERIFIED_STATE.md` — what has and has not been physically verified.
* `docs/bringup/P-H14-calibration-checklist.md` — the calibration protocol.
* `docs/limitations.md` — what this hardware/firmware combination cannot do.
* `lib/evcore/src/StartTrigger.h`, `StandaloneArming.h`, `StandalonePreset.h`,
  `StandaloneDiagnostic.h` — the logic this guide operates, each with its own
  safety argument.
