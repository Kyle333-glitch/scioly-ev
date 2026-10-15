# E0 — Electrical Compatibility Review

**Status: STATIC ANALYSIS FROM SOURCES. Not a measurement. Block E: NOT EXECUTED.**

Purpose: check that the *intended* architecture can be electrically sound, cite
the primary sources that decide each question, and convert every unconfirmable
rating into a STOP prerequisite for Block F. A compatibility "pass" here is a
paper check only — it never verifies the vehicle.

Primary sources consulted or required (manufacturer-first):

- TI DRV8874 datasheet: <https://www.ti.com/lit/ds/symlink/drv8874.pdf>
  (already cited by `docs/hardware/motor-driver-drv8874.md`; PDF not in repo).
- Pololu DRV8874 carrier (#4035) product page/schematic — REQUIRED, not yet cited
  in repo. E-01 records the carrier revision from its silkscreen.
- Arduino Nano R4 documentation (Renesas RA4M1-based; VIN range, logic levels,
  regulator limits, ADC reference) — REQUIRED, not cited in repo beyond the
  core pin table noted in `src/PinMap.h`.
- Motor/encoder product page (#4862 or as labeled) for voltage, stall current,
  CPR, gear ratio — REQUIRED, entirely absent from repo.
- Cell/pack data for the actual AA NiMH cells (nominal 1.2 V, full-charge
  ≈1.4–1.45 V/cell) — chemistry knowledge; E-01 records the actual cell model.

## 1. Battery voltage range vs driver VM rating

- Pack (assignment context): 8 AA NiMH. Nominal 8 × 1.2 V = **9.6 V**; fully
  charged ≈ 8 × 1.4–1.45 V = **≈11.2–11.6 V**. The "approximately 12 V nominal"
  phrasing must not be used as a rail value — 12 V is closer to the
  full-charge peak than to nominal.
- DRV8874 VM absolute maximum is 40 V per datasheet family rating (TO BE
  CONFIRMED against the cited PDF, §absolute maximum ratings). An 11.6 V peak
  is far inside it **if the carrier follows the datasheet supply circuit**.
- STOP prerequisite F-P1: confirm the carrier's VM rating and any carrier-level
  capacitor voltage rating from the Pololu schematic before first power. A
  carrier electrolytic rated below the peak rail (heat-aged or misread) is the
  failure this check exists to catch.

## 2. Battery voltage vs controller input rating

- Which Nano input is fed (VIN vs 5V pin) is UNKNOWN (inventory W-04). This is
  the highest-severity paper gap: pack voltage into the 5V pin, or a VIN below
  the regulator's dropout-corrected minimum under load, are both damage or
  brown-out paths.
- STOP prerequisites F-P2a (identify the fed input pin) and F-P2b (cite the
  Nano R4 VIN range and regulator output current against the Nano + encoder +
  trigger load). No powered step until both cite a primary source.

## 3. Fully charged pack voltage (restated)

See §1. Block E records the cell model (E-01) and Block F measures the actual
pack open-circuit voltage before connection (prerequisite table item 3). Never
use nominal 9.6 V as the design peak.

## 4. Controller I/O logic levels

- Nano R4 (RA4M1) GPIO is 3.3 V logic. Whether the DRV8874 carrier's
  PH/EN/nSLEEP inputs accept 3.3 V logic (TI DRV8874: logic thresholds are
  TTL-compatible per datasheet — TO BE CONFIRMED against the PDF) decides if a
  level shifter is needed. `docs/hardware/motor-driver-drv8874.md` explicitly
  lists this as unclaimed.
- STOP prerequisite F-P4: cite the driver's VIH/VIL from the datasheet and the
  Nano's VOH from its documentation; H1A-A1 then meters the actual levels.

## 5. Encoder power and logic levels

- Encoder supply voltage and its source pin are UNKNOWN (W-12). Two-sided risk:
  a 5 V encoder fed from 3V3 (under-supply, weak edges) or a 3.3 V-only
  encoder fed from 5V (damage), plus logic-level mismatch into D2/D4 in either
  direction.
- STOP prerequisite F-P5: read the encoder/mode
l label (E-01/E-16), cite its
  supply and output levels, and confirm the harness matches before any powered
  encoder read (H2 needs this, not just H3).

## 6. Driver PH/PWM/nSLEEP/PMODE logic requirements

- Plan levels (`MotorDriverPlan.h`): Disabled/Coast = nSLEEP LOW, PH LOW, EN 0;
  Brake = nSLEEP HIGH, PH LOW, EN 0; DriveForward = HIGH/HIGH/duty;
  DriveReverse = HIGH/LOW/duty. PMODE tied GND selects PH/EN at power-up
  (datasheet PMODE description).
- Open question carried from the driver doc: whether awake-with-EN-low is a
  shorted bridge (brake) or high-Z (coast) — H1A-A2 decides; until then `Brake`
  must not be used as a hold.
- Power-up default: PMODE latch at power-up means the mode is fixed by wiring
  before firmware runs; firmware de-energizes first (`MotorDriver.h::failSafe`
  at startup). Floating nSLEEP/PWM/PH during Nano reset is an UNKNOWN (reset
  pin tristate vs carrier pull-downs) — F-P6 requires citing carrier pull
  resistors or metering the lines during reset in H1A.

## 7. Enable/sleep and power-up default states

- Firmware commands Disabled (nSLEEP LOW) before anything else runs
  (SOURCE-CONFIRMED, `src/main.cpp` startup order per `MotorDriver.h` docs).
- What the driver *outputs* do while nSLEEP is LOW but VM is present follows
  the datasheet sleep-state spec (outputs high-Z; TO BE CONFIRMED), not
  firmware. The MCU-pin read after watchdog resets (D28, 9/9 de-energized) is
  logic-level evidence only and says nothing about the outputs.

## 8. Motor current, stall current, driver limits

- Motor stall current: UNKNOWN (no motor datasheet in repo). DRV8874 current
  capability and the carrier's current-limit configuration (ITRIP/IMODE, if the
  carrier exposes it): UNKNOWN.
- STOP prerequisite F-P8: cite motor stall current and the carrier limit
  setting; the first-power supply current limit (H3) must be chosen below the
  driver's protection threshold AND below wire/switch ratings, recorded in the
  H3 preregistration. A stall with no limit established is a fire/hazard path,
  not a tuning question.

## 9. Wire and connector current ratings

- No gauge, connector, or solder/crimp information exists in the repo. E-10/E-15
  record conductor size, connector type, and any heating/discoloration; F-P9
  requires the thinnest supply conductor to exceed the §8 stall/limit current
  with margin, or the conductor is replaced.

## 10. Switch/disconnect suitability

- No switch is documented. E-10 identifies the actual device; F-P10 requires
  its DC voltage/current rating to cover the §3 peak and §8 stall, with
  accessible one-motion operation. A signal-only slide switch in the motor rail
  FAILS Block E outright (record as critical hazard H-19).

## 11. Heat dissipation and mounting

- Driver carrier mounting (heatsink, airflow, proximity to plastic/chassis):
  UNKNOWN. F-P11 requires the H3/H7 current-limit record plus a touch/thermal
  observation plan in the Block F procedure; sustained stall current on an
  un-heatsunk carrier is a STOP condition in every powered protocol.

## 12. Capacitors, protection, polarity-sensitive parts

- Carrier bulk capacitance, TVS/reverse-polarity protection: per-carrier facts,
  UNKNOWN until the Pololu schematic is cited. E-14 inspects for visible
  polarity markings and capacitor condition (bulge/leak); reverse-polarity
  protection must NOT be assumed (hazard H-02).

## 13. Reverse-polarity and short-circuit protection

- Assumed ABSENT until a primary source or a labeled device proves otherwise.
  E-10/E-12 verify keyed connectors where claimed; non-keyed battery
  connections are a critical hazard (H-02) and a Block F gate.

## 14. Common-ground requirements

- Intended single common ground (W-03/W-05/W-13) is unconfirmed. A missing
  Nano↔driver ground makes logic inputs float and direction/speed undefined —
  E-11/E-12 check it unpowered; Block F re-verifies it powered at low limit
  (H1A-A1 levels reference ground explicitly).

## 15. Voltage domain boundaries

- Domains: pack rail (≈9.6–11.6 V) → driver VM; Nano logic (3V3, possibly 5V
  USB during development); encoder supply (UNKNOWN); trigger pull-up (Nano
  internal). E-16/E-17 map each domain's source; any domain cross-connection
  found (e.g. pack rail on a logic pin) is an immediate STOP + FAIL.

## 16. Back-powering paths

- USB 5V vs pack supply: H0/H0W ran USB-only with the motor supply off. The
  vehicle must never be USB-connected with the pack connected unless the power
  architecture explicitly permits it — E-02 enforces USB absence during
  inspection, and Block F prerequisites forbid simultaneous USB + pack pending
  a cited Nano R4 power-mux source (F-P16).

## Verdict of this review

No incompatibility is proven on paper, because the paper is mostly blank: of
16 questions, 11 depend on UNKNOWN physical facts and 5 need primary-source
citations the repo does not yet contain. That is the honest result. The 9 STOP
prerequisites above (F-P1, F-P2a/b, F-P4, F-P5, F-P6, F-P8, F-P9, F-P10, F-P11,
F-P16) become rows of the Block F prerequisite register
(`E0-block-f-handoff.md`), each blocking first power until cited or measured.
