# E0 — Block E → Block F Handoff

**Block E completion does NOT authorize first power. Block F is NOT AUTHORIZED
by this package, by a Block E PASS, or by any agent session — it needs a
separate human authorization against this register.**

Staged safety pathway (no circular gates): Block E (unpowered) → authorized
**bounded first-power** H1A-A1/A2 (MCU-only levels, then low-limit rail,
motor STILL disconnected) → H1B Parts 1/2/4 → H2 (hand-driven, de-energized)
→ authorized H3 Step 0 (B2 expiry/latch demo) → H3 powered. Each stage's exit
criteria are owned by its protocol (`docs/bringup/P-H*.md`), not by this file.
This register lists what Block F entry needs, not what proves the vehicle ready.

## Prerequisite register

| ID | Requirement | Evidence today | Required evidence | Owner phase | Blocking F-entry |
|----|-------------|----------------|-------------------|-------------|------------------|
| F-P1 | Carrier VM/cap ratings cover ≈11.6 V peak | None (no schematic cited) | Pololu schematic citation + E-01 carrier revision | Human review pre-F | YES |
| F-P2a | Fed Nano input pin identified | UNKNOWN (W-04) | E-03/E-26 trace + Form C | Block E | YES |
| F-P2b | Nano input rating covers rail | No citation | Nano R4 VIN/regulator citation | Human review pre-F | YES |
| F-P3 | Actual pack open-circuit voltage measured | No pack data | DMM reading at F-entry, both polarities checked | F-entry procedure | YES |
| F-P4 | 3V3 logic accepted by driver inputs | Datasheet question open | VIH/VIL citation + H1A-A1 metered levels | H1A-A1 | YES |
| F-P5 | Encoder supply/levels compatible | UNKNOWN (W-12) | Label + rating citation before H2 powered reads | Block E + review | YES |
| F-P6 | Reset/power-up pin behavior known | UNKNOWN (carrier pulls) | Carrier pull citation or H1A reset metering plan | H1A | Gates H1A-A2, not F-entry (wheels unloaded + limit cover it) |
| F-P7 | Motor direction truth (PH level → forward) | Informal observation only (D36, not evidence) | H1A-A2 unloaded mapping, then H3 polarity (#3) | H1A/H3 | Gates H3, not F-entry |
| F-P8 | Stall current + supply limit chosen | UNKNOWN | Motor citation + recorded limit in H3 prereg | H3 prereg | YES (limit must exist before any power) |
| F-P9 | Conductors cover stall/limit current | UNKNOWN gauges | E-15 record or replacement | Block E + review | YES |
| F-P10 | One-motion rail disconnect, rated | UNKNOWN device | E-36/E-40 drill + rating | Block E | YES |
| F-P11 | Thermal plan (carrier + motor) | UNKNOWN mounting | Mounting record + touch-check in F procedure | Block E + H3 | Gates sustained runs, not first bounded power |
| F-P12 | Run image vs bench image discipline | SOURCE-CONFIRMED (`platformio.ini` separation; vehicle has no bench drive) | `bench health` image check at each session | Every powered session | YES (procedural) |
| F-P13 | Watchdog posture understood | VERIFIED for bench load (H0W/D28); vehicle image ships disabled by design | State which image + watchdog env in every prereg | Every powered session | Procedural (no surprise enables) |
| F-P14 | 0.5 s feedback allowance still Provisional | SOURCE-CONFIRMED Provisional (`kPlannedEncoderProgressTimeoutS`) | P-H5/H7/H9 measurement; treat as unvalidated tripwire only | H5/H7/H9 | Gates H10, not F-entry |
| F-P15 | Killed-switch + current limit as stop authority; nFAULT unseen | SOURCE-CONFIRMED (capability NotInstalled) | Recorded limit + switch reach at each session | Every powered session | YES (procedural) |
| F-P16 | USB+pack simultaneity forbidden until mux cited | H0 precedent (USB-only sessions) | Nano R4 power-mux citation or continued prohibition | Review pre-F | YES |
| F-P17 | Rules re-check before competition freeze | `docs/rules/README.md` UNVERIFIED vs corrections | Corrections-page check recorded | Pre-freeze, not F-entry | NO (informational here) |

## Calibration-bootstrap dependency (no firmware change)

- Measurable WITHOUT any powered run (Level 0): hand-driven H2 sign/counts
  (needs only B3 reads of a hand-turned wheel — but B3 needs the bench image
  flashed over USB with the rail ABSENT; that flash is a Block F human
  authorization, not Block E); manual wheel/tape geometry (E-21 diameters are
  dimensions, not calibrations); trigger mechanical action.
- Needs BOUNDED authorized bench power (Level 1, B2): H3 polarity + minimum
  effort; H4 scale; H5 speed relation; H6 feed-forward + Provisional P/I
  candidate (stays Provisional — R3/M6: a computed P/I pair that never ran
  closed-loop cannot be margined into Measured); H7 offsets/braking.
- Gains stay Provisional until P-H7 Part L evaluates them on the run path;
  everything armed stays blocked until then. The timing predictor stays
  disabled (`kRunTimingModelValidated = false`) until its two-leg structure
  review + Run Time comparison exist.
- Arming-while-tuning deadlock resolved WITHOUT bypass: conservative Measured
  bootstraps (recorded bounded-trial worst case + pre-registered safe margin,
  flagged untested-on-run-path) for timeouts/ceilings/plan dependencies per
  `docs/bringup/README.md` Level 1; gains EXCLUDED from bootstrapping; the S16
  `ExecutionPlanNotEstablished` refusal stays in force throughout.
- Approvals required before any new test capability: same bar as any firmware
  change (review + validation re-run); Block E proposes none.

## Authorization rule

A human operator authorizes Block F by signing Form K (every F-P row either
evidenced or explicitly waived with reason — waivers on F-P1…P10, P15, P16 are
strongly discouraged and must state the compensating control). The signed Form K
is filed with the Block E evidence. No agent session may sign it.
