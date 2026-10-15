# E0 — Wiring and Component Inventory (reconstructed, NOT as-built)

**Status: DESIGN INTENT RECONSTRUCTION. Nothing here is a measured as-built record.**
**Block E status: NOT EXECUTED. No inspection result is recorded in this file.**

This document reconstructs the intended vehicle architecture from repository
sources only. Every row carries an evidence class. The Block E operator
procedure (`E0-unpowered-inspection-protocol.md`) verifies this reconstruction
against the physical vehicle; until then every physical connection below is
**UNVERIFIED**, even where the intended connection is stated confidently.

Evidence classes used here:

- **SOURCE-CONFIRMED** — stated in tracked source or accepted project record.
- **DOCUMENTED BUT NOT PHYSICALLY VERIFIED** — written in a doc, never measured.
- **OPERATOR-REPORTED** — team statement recorded in `VERIFIED_STATE.md`/`DEVELOPMENT_LOG.md`
  (recollection), not a measurement.
- **CONFLICTING** — two sources disagree; both cited, no winner chosen.
- **UNKNOWN** — no repository source establishes it; the inspector must identify it.
- **PHYSICALLY MEASURED WITH CITED EVIDENCE** — none in this file (that is what
  Block E produces).

Authoritative source precedence: live source (`src/PinMap.h`, `src/SensorInputs.h`,
`src/VehicleConfig.h`, `src/MotorDriver.h`) over prose; `VERIFIED_STATE.md` over
`DEVELOPMENT_LOG.md` narrative; manufacturer primary sources over either for
ratings (see `E0-electrical-compatibility-review.md`).

## 1. Intended bill of materials

| # | Component | Repository evidence | Class |
|---|-----------|---------------------|-------|
| 1 | Controller: Arduino Nano R4 (Renesas RA4M1) | `platformio.ini` (`env:nano_r4`), `src/PinMap.h` framework notes, H0/H0W on-target sessions (USB VID:PID 2341:0074) | SOURCE-CONFIRMED (board family; specific board revision/silkscreen UNKNOWN) |
| 2 | Motor driver: TI DRV8874, Pololu carrier product #4035 | Assignment context states #4035; repo names only "TI DRV8874" (`docs/hardware/motor-driver-drv8874.md`) and "the board" without a vendor product number | OPERATOR-REPORTED for #4035; SOURCE-CONFIRMED for DRV8874 PH/EN plan |
| 3 | Drive motor: Pololu 25D gearmotor with encoder, product #4862 | Assignment context states #4862; repo names only "25D gearmotor with encoder" equivalent function (encoder A/B, motor leads) without a product number, gear ratio, CPR, or voltage rating anywhere found | OPERATOR-REPORTED for #4862; all electrical/mechanical ratings UNKNOWN |
| 4 | Motor supply: 8 AA NiMH cells | Assignment context; repo rules mirror caps at 8 AA, 1.2–1.5 V each (`docs/rules/README.md`, rules 3.a/8.b) | OPERATOR-REPORTED (pack config, cell model, holder, wiring UNKNOWN) |
| 5 | Power switch / disconnect | No repository source found naming a switch, connector type, fuse, or disconnect method | UNKNOWN |
| 6 | Encoder (motor-integrated quadrature) | `src/PinMap.h` (A=D2, B=D4), `src/EncoderInput.*`; counts observed informally 2026-10-02 | DOCUMENTED BUT NOT PHYSICALLY VERIFIED |
| 7 | Start trigger ("pencil trigger") | `src/PinMap.h` (D12, pull-up, LOW = active plan); actuated informally 2026-10-02, levels never recorded | DOCUMENTED BUT NOT PHYSICALLY VERIFIED |
| 8 | nFAULT line | Explicitly NOT installed (D32; `kMotorFaultBar = kNoPin`, capability `NotInstalled`) | SOURCE-CONFIRMED (absence stated by team, not measured) |
| 9 | Battery divider / current shunt | Explicitly NOT installed (D32/S10; A1/A2 read unconnected inputs) | SOURCE-CONFIRMED (absence stated by team, not measured) |
| 10 | LCD / auxiliary modules | No repository source found | UNKNOWN (assume none until inspection shows otherwise) |
| 11 | Chassis, wheels, gear train, couplers, bottle mechanism | No repository source found describing mechanics | UNKNOWN |
| 12 | Wire gauges, connector types, solder vs crimp, strain relief | No repository source found | UNKNOWN |

No wiring diagram, schematic, photograph, or construction plan exists anywhere
in the repository (file search over image/schematic/CAD extensions returns only
the third-party rules PDF). The inspector creates the first visual record during
Block E.

## 2. Stated V5 pin plan (intent, NOT a wiring fact)

From `src/PinMap.h` (team's stated V5 harness; `VERIFIED_STATE.md` #1 UNVERIFIED):

| MCU pin | Net | Function | Firmware treatment |
|---------|-----|----------|--------------------|
| D2 | Encoder A | Quadrature A, CHANGE interrupt (channel 0) | Interrupt + x2 decode (`EncoderInput.*`) |
| D4 | Encoder B | Quadrature B, level read only (no interrupt channel on this core) | Level read at A transitions |
| D8 | DRV PH | Direction input, plain digital out | `MotorDriver.h` only writer |
| D9 | DRV EN | Speed input, **only PWM consumer** (`analogWrite`, 8-bit) | `MotorDriver.h` only writer |
| D10 | DRV nSLEEP | Active-low enable, plain digital out | `MotorDriver.h` only writer |
| D12 | Trigger | Start input, `INPUT_PULLUP`, LOW = active (plan) | `SensorInputs.h` raw read |
| A1 | (planned battery sense) | NOT INSTALLED — reads unconnected input | Raw counts only, never volts |
| A2 | (planned current sense) | NOT INSTALLED — reads unconnected input | Raw counts only, never amps |
| GND | PMODE tie | DRV PMODE tied to GND in harness → PH/EN mode | No MCU pin; firmware neither drives nor reads |
| — | nFAULT | NO PIN (`kNoPin`) — line not wired | Not configured, not sampled |

Framework facts recorded in `src/PinMap.h` (checked against the installed
Renesas core 1.9.0 `variants/NANOR4/` table — framework facts, not vehicle facts):
D2/P105 carries interrupt channel 0; D4/P103 has no interrupt entry; D9/P303
carries PWM channel 7-B; A0 is the DAC pin and is deliberately unused.

## 3. Point-to-point wiring table (to be verified, not asserted)

Each row is one connection the inspector must trace. "Intended" = what the
sources above imply. "Inspection" = the Block E step that checks it.
"Remains unverified" is filled at authoring time; the inspector replaces it
with a result reference, never by editing this file (results go in the
inspection record, `E0-inspection-record-template.md`).

| ID | Source component / terminal | Destination component / terminal | Function | Polarity / direction | Intended state | Block E step | Remains unverified |
|----|-----------------------------|----------------------------------|----------|----------------------|----------------|--------------|--------------------|
| W-01 | Battery pack (+) | Power switch/disconnect terminal 1 | Motor+logic supply feed | + toward vehicle | UNKNOWN (pack and switch both unidentified) | E-03, E-10 | Pack configuration, switch existence and rating |
| W-02 | Switch/disconnect terminal 2 | Driver VM (carrier VIN/VM pin) | Motor rail | + rail | UNKNOWN | E-03, E-10 | Routing, polarity, conductor rating |
| W-03 | Driver GND (carrier GND) | Battery pack (−) | Motor rail return | Return | Intended common ground, unconfirmed | E-11, E-12 | Continuity and single-point vs loop topology |
| W-04 | Battery/switch output | Nano VIN (or 5V — IDENTIFY) | Controller supply | + to Nano | UNKNOWN — which Nano input pin is fed is not documented | E-03, E-10 | Input pin used, voltage presented, regulator path |
| W-05 | Nano GND | Driver GND / battery (−) | Logic reference | Common 0 V | Intended common ground, unconfirmed | E-11, E-12 | Missing-ground hazard is a BLOCK F gate |
| W-06 | Nano D8 | Driver PH/IN2 | Direction | MCU → driver, HIGH = planned forward | Stated V5 | E-13, E-20 | Continuity, connector orientation |
| W-07 | Nano D9 | Driver EN/IN1 | Speed (PWM) | MCU → driver, duty = effort | Stated V5 | E-13, E-20 | Continuity; second-PWM-consumer ban (PinMap.h) |
| W-08 | Nano D10 | Driver nSLEEP | Enable, active low | MCU → driver, LOW = asleep | Stated V5 | E-13, E-20 | Continuity |
| W-09 | Harness GND | Driver PMODE | Mode select, tied low → PH/EN | Fixed low | Stated V5 | E-14 | Tie exists, tie point, no MCU connection |
| W-10 | Driver OUT1 | Motor lead 1 | H-bridge output | Bidirectional | UNKNOWN lead mapping | E-15, E-22 | Which lead, connector type, reconnection record |
| W-11 | Driver OUT2 | Motor lead 2 | H-bridge output | Bidirectional | UNKNOWN lead mapping | E-15, E-22 | Same as W-10 |
| W-12 | Motor encoder VCC (identify) | Nano 5V or 3V3 (identify) | Encoder supply | + to encoder | UNKNOWN (voltage and source pin) | E-16 | Supply voltage vs encoder rating (compatibility review §4) |
| W-13 | Motor encoder GND | Nano GND | Encoder return | Return | Intended | E-16 | Continuity |
| W-14 | Motor encoder A | Nano D2 | Quadrature A | Encoder → MCU | Stated V5 | E-13, E-16 | Continuity |
| W-15 | Motor encoder B | Nano D4 | Quadrature B | Encoder → MCU | Stated V5 | E-13, E-16 | Continuity |
| W-16 | Trigger contact terminal 1 | Nano D12 | Start input | Contact → MCU (LOW = active plan) | Stated V5 | E-17 | Continuity, contact construction |
| W-17 | Trigger contact terminal 2 | Nano GND (assumed — VERIFY) | Input return | Return | Assumed, not documented | E-17 | Return point; a trigger returned to a non-ground net is a FAIL |
| W-18 | nFAULT | — | NOT INSTALLED | — | Absence stated (D32) | E-18 | Confirm no conductor, no floating stub tied anywhere |
| W-19 | A1 / A2 | — | NOT INSTALLED | — | Absence stated (D32/S10) | E-18 | Confirm no divider/shunt circuitry, note what A1/A2 pins physically connect to (likely nothing) |
| W-20 | USB connector | Host PC (during development only) | Upload/serial | — | H0 evidence (VID:PID 2341:0074) | E-02 | Confirm USB cable ABSENT during all inspection steps |

## 4. Architecture items A–R (assignment §6)

| Item | Reconstruction | Class |
|------|---------------|-------|
| A. Battery pack and wiring | 8 AA NiMH per assignment context; holder, cell model, series wiring, fuse: none documented | UNKNOWN |
| B. Power switch / disconnect | No source names one; H0/H0W preregs say "motor supply off and disconnected" (procedure, not hardware) | UNKNOWN — E-03 must identify the actual isolation method or FAIL Block E |
| C. Controller power input | Which Nano input (VIN vs 5V vs USB) is fed in the vehicle is undocumented | UNKNOWN |
| D. Driver VM and logic connections | VM from pack (assumed); logic supply of Pololu carrier per its schematic (to be cited in compatibility review) | UNKNOWN |
| E. Grounding topology | Intended common ground (Nano GND = driver GND = pack −); topology undocumented | UNKNOWN |
| F. Motor-output connections | Two leads, mapping undocumented; P-H1A requires recording the disconnection method | UNKNOWN |
| G. Driver mode and sleep pins | PMODE→GND (PH/EN mode); nSLEEP=D10, LOW = asleep (plan) | DOCUMENTED BUT NOT PHYSICALLY VERIFIED |
| H. Encoder power and signals | A=D2, B=D4; supply voltage/pin undocumented | Partial: signals DOCUMENTED; supply UNKNOWN |
| I. Trigger wiring, polarity | D12, pull-up, LOW-active plan; idle/active levels never recorded (H1B Part 4 owed) | DOCUMENTED BUT NOT PHYSICALLY VERIFIED |
| J. LCD / other modules | None documented | UNKNOWN (verify absence) |
| K. Mounting and moving parts | Undocumented | UNKNOWN |
| L. Wheels, gear train, couplings, encoder | Undocumented (ratio, CPR, diameters all absent) | UNKNOWN |
| M. Emergency power-isolation method | No named kill switch in repo; bring-up docs call the "physical kill switch (the motor-supply disconnect)" the primary stop without specifying its construction | UNKNOWN — E-10 inspects whatever exists; absence FAILS Block E |
| N. Current-limit / protection config | DRV8874 carrier current-limit setting (if any), ITRIP/IMODE: undocumented; on-chip protection exists per datasheet but its configuration on this carrier is not recorded | UNKNOWN |
| O. Installed vs absent sensors | nFAULT/battery/current absent (stated); encoder + trigger present (stated, unverified) | SOURCE-CONFIRMED (absence); DOCUMENTED (presence) |
| P. External connectors / exposed conductors | Undocumented | UNKNOWN |
| Q. Cable strain relief and routing | Undocumented | UNKNOWN |
| R. Mechanical retention and protection | Undocumented | UNKNOWN |

## 5. Contradictions and uncertainties found in sources

1. **Product numbers (#4035 / #4862).** Assignment context states them; no
   repository file found names a Pololu product number, gear ratio, CPR, or
   motor voltage. E-01 requires reading the physical labels. Until then the
   numbers are OPERATOR-REPORTED, and every rating derived from them is UNKNOWN.
2. **"Kill switch" vs hardware.** Bring-up docs treat a physical motor-supply
   disconnect as the primary stop, but no document describes its construction.
   Block E must identify it or fail on item M.
3. **Trigger return (W-17).** `SensorInputs.h` enables the internal pull-up on
   D12, which implies a contact-to-ground design, but no source states where
   the second terminal goes. Inspection decides; this file assumes nothing.
4. **Battery voltage language.** "Approximately 12 V nominal" (assignment) vs
   8×1.2 V = 9.6 V NiMH nominal and ≈11.2–11.6 V fully charged (chemistry; see
   compatibility review). The "12 V" figure must not be used as a rail value. Keep three voltages distinct (E0V F-E4): NOMINAL 9.6 V
   (8 x 1.2 V), CHARGED open-circuit ≈11.2–11.6 V (8 x ≈1.4–1.45 V/cell), and
   ACTUAL measured pack voltage (DMM only, at Block F entry per F-P3).
5. **Controller supply path.** Nothing states VIN vs 5V feed. A pack-voltage
   feed into the wrong Nano input is a damage hazard (compatibility review §3).
6. **No conflicts found** between `src/PinMap.h`, `MotorDriverPlan.h`, and
   `docs/hardware/motor-driver-drv8874.md` on the intended PH/EN mapping; all
   three agree. The conflict surface is intent-vs-physical, not doc-vs-doc.

## 6. What Block E must deliver back to this file's table

For every W-row: continuity result or NOT PERFORMED with reason; a photograph
reference; and the as-built correction where intent was wrong. Corrections to
intent (e.g. a different D-pin) are recorded as inspection findings and
reconciled per `docs/bringup/README.md` ("fix the plan or the wiring — never
add a sign somewhere else"); this reconstruction file is not edited to match
the vehicle — the vehicle record and a follow-up source change are separate,
reviewed actions outside Block E.
