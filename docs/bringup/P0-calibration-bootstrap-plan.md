# P0 — PI Calibration Bootstrap Plan (noncircular, M6 as far as software permits)

**Problem (M6/R3):** production PI gains are Provisional; `ControllerConfigValid`
requires all three gains Measured; production arming refuses unqualified gains by
design (`ExecutionPlanNotEstablished` / `ControllerConfigValid==false`). Tuning
needs physical data, some of it powered. The plan must not bypass the gate,
promote gains, or invent actuation.

**Authoritative order:** `docs/bringup/README.md` bootstrap + `P-H14` checklist +
`P-H6` (tuning) + `P-H7 Part L` (closed-loop check). Where they disagree, the
bootstrap order controls (P-H14 C2).

## A–E: what each level of access can establish

| Level | What can be measured | Establishes (Measured, with citation) | Cannot establish |
|---|---|---|---|
| A. No power (E0/H2-hand) | Continuity; polarity marks; carrier/encoder labels; mechanical freedom; hand-driven tick counts | #1 wiring (continuity table); hand-count repeatability input to H4 | Any gain, voltage, timing, or powered behavior |
| B. Controller power only (H0/H1A-A1/H1B) | MCU pin idle levels (nSLEEP/PH/EN volts); trigger levels; raw A1/A2 counts (as counts); image identity | Input-level rows; trigger active level; de-energized-startup evidence | Plant dynamics; feed-forward; P/I (no motion, no loop) |
| C. Motor power, no rotation (H1A-A2) | Output-presence per input combo; supply current ~0; awake-EN-low state via `bench h1a-brake` | Truth-table presence rows; F-P6 reset behavior input | Direction truth under load; speed; braking |
| D. Rotation, no translation (H3/H5 unloaded, wheels off floor) | Direction truth PH→forward (#3 first evidence); min effort; open-loop effort→velocity steady-state pairs | #3 polarity; feed-forward slope source data (H5) | Loaded gains; braking distance; slip; closed-loop adequacy (bench never runs `VelocityController::update()`) |
| E. Vehicle translation (H4/H7/H8 loaded) | Rollout mm/tick vs tape (#6); slip bound (#10); braking offsets per direction (#9); reversal safety; closed-loop tracking error (Part L) | #4 sign (recorded), #6, #9, #10 inputs; feed-forward Measured; P/I Measured **only** via Part L criteria | Nothing further without bottle mechanism (#11 stays until H8) |

## F. Which existing bench modes collect useful data

- B2 `bench drive <fwd|rev> <permille> <ms>` (bounded: ≤150 permille, ≤500 ms,
  ≤8 cmds/arm): open-loop effort trials for H5/H6-part-1. The typed triple + `BENCH`
  replies + capture timestamps are the command record. `EVS3 motor/effort` is
  Disabled/0 under bench ownership — never read it as the applied command (P-H6 C1).
- B1 loop/watchdog readout, B3 raw tick readout, B4 raw input readout: cadence,
  tick, and level evidence. No bench command closes the velocity loop.

## G–J: candidate estimation → validation → prerequisites → provenance

- **G. Candidate estimation (software-only, offline):** from H5 steady-state pairs,
  fit feed-forward slope (Measured, open-loop valid). For P/I, compute an
  *AnalyticalCandidate* offline under a pre-registered rule (e.g. first-order fit +
  conservative margins, integral deliberately small/zero-start); label
  `Provisional` with derivation cited. A computation never becomes a measurement
  by acquiring a citation (P-H6 §Provenance, R3/M6).
- **H. Validation experiment:** **P-H7 Part L** on the loaded run path — the only
  place the regulator runs. Fixed command profile, fixed dwells, highest admissible
  telemetry rate, pre-registered tracking-error/stability criteria. Pass → promote
  P/I to Measured with capture pointer; fail → re-derive, never raise ceilings to
  pass (`kMaxEffortPermille` bench 150 / production-placeholder 300 are safety
  ceilings, not tuning knobs).
- **I. Safety prerequisites:** Block E PASS; H0/H0W; H1A (disconnect method +
  limit recorded); H1B (trigger + supply); H2 (sign + retention); bounded
  first-power human approval (F-P1..F-P17, stall citation F-P8, disconnect drill
  E-36, runout + kill switch); H3 Step 0 expiry/latch demo; H4/H5 complete; wheels
  unloaded for H6-part-1/H7-unloaded; loaded only in Part L with approver present.
- **J. Measured provenance:** raw capture under `data/evidence/` + derivation
  arithmetic + `VERIFIED_STATE.md` row + `DEVELOPMENT_LOG.md` entry + `Sourced`
  citation (`Sourced::citesEvidence()`); `MeasuredValuesCiteEvidence` must pass;
  `checks` must show the gate that opened; rejected runs kept with reason (H14 rules).

## K–L: can existing bench solve it, and if not what is needed

- **K. Answer: NO for P/I, YES for feed-forward.** The bench characterizes the
  open-loop plant (H5/H6-part-1) but cannot validate P/I because no bench command
  invokes the controller. The bootstrap is therefore noncircular *across* images:
  bench → candidate → production run path (Part L) → promotion. No new firmware is
  needed for this path; Part L already exists in `P-H7`.
- **L. If Part L cannot meet its criterion** (e.g. loaded refinement unsafe or
  inconclusive), the escalation is a **separate bounded closed-loop bench
  capability**: smallest instrumentation that runs `VelocityController::update()`
  against bounded effort/duration/budget with expiry + latch, specified in
  `INTERACTIVE-CALIBRATION-DESIGN.md` §5.4 option 2, subject to its own safety
  approval. It is NOT authorized by this plan, NOT implemented here (§10 forbids
  new actuation), and must never be a placeholder promotion or gate bypass.

## Dependency graph (text)

```
E0 ──→ H0 ──→ H0W ──→ H1A ──→ H1B ──→ H2 ──→ [bounded first-power APPROVAL] ──→ H3 ──→ H4 ──→ H5 ──┬──→ feed-forward Measured
                                                                                                  │
                                                                              P/I AnalyticalCandidate (Provisional, offline)
                                                                                                  │
                                                                        H7 Part L (loaded run path, regulator runs)
                                                                                                  ├── PASS → P/I Measured → ControllerConfigValid can pass → armable
                                                                                                  └── FAIL → re-derive (no ceiling raise) OR §L separate bench-loop spec
H7 ──→ H0W-S8 (loaded WDT) ──→ H8 (bottle) ──→ execution-plan deps Measured ──→ ExecutionPlanNotEstablished clears
All promotions: capture + derivation + VERIFIED_STATE row + log + citation (J).
```

## Status vocabulary (must appear on every claim)

- **Feasible proposed plan:** this file + P-H6/P-H7 Part L (paper, not run).
- **Implemented firmware capability:** B2 open-loop trials; production regulator +
  Part L path; provenance gates (all host-tested, none on hardware here).
- **Authorized physical experiment:** NONE under P0 (Block E not executed, Block F
  not authorized; H-series authorizations are per-protocol human approvals).
- **Completed physical measurement:** NONE under P0 (only informal 2026-10-02
  observations exist: D36, explicitly not evidence; ~1494 ticks / ~1.339 mm/tick
  is provisional, not a calibration).
