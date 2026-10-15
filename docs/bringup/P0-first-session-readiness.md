# P0 — First-Hardware Session Readiness (Block E pathway)

**Base:** `20aa4a1391a4dc9ac6340bc8db1b57e65234dcfd` (main, verified at P0 entry).
**Status:** PLAN, not evidence. Block E: NOT EXECUTED. Block F: NOT AUTHORIZED.
**Rule:** This file authorizes no power, no motion, no upload. Each stage is owned
by its protocol file; where this roadmap and a protocol disagree, the protocol
controls.

Entry-state note (P0 §1): local `main` equals the expected SHA; tracked tree and
index clean (`git diff HEAD` empty); the two preserved audits match their SHA256
(`ab74892d…`, `81cc805a…`) but are untracked (`docs/audits/`); local `main` is
38 ahead of `origin/main` (expected — do not push). No `S19V` independent-acceptance
artifact was found in `DEVELOPMENT_LOG.md`, `VERIFIED_STATE.md`,
`.agent-work/STATE.md`, or `.agent-work/reviews/` (searched 2026-10-15); S19 itself
is the HEAD commit (`20aa4a1`, response-loss accounting on STATUS + bench serial).
No `E0V` standalone audit file was found; E0V corrections are cited inline in
`docs/bringup/E0-*.md` (F-C1/F-C2, F-E2–F-E4, F-E3). Recovery branches
(`recovery/d32-d34-v5-snapshot`, `recovery/s2-overscope-snapshot`) and all six
pre-existing worktrees were preserved. A seventh worktree
(`../scioly-ev-p0`, branch `work/p0-prehardware-readiness`) was created for this audit.

## Staged pathway (actual H-stage order per `docs/bringup/README.md`)

```
E0 unpowered inspection (Block E)
  → H0 upload/boot/serial (S, USB only) [already passed 2026-09-28, repeat owed per session]
  → H0W watchdog qualification (S) [passed USB-only; loaded Step 8 owed after H7]
  → H1A driver electrical, motor DISCONNECTED (E)
  → H1B sense inputs Parts 1/2/4 (E) [Part 3 nFAULT = NOT APPLICABLE on this vehicle]
  → H2 encoder hand-driven, de-energized (P, no power)
  → bounded first-power approval (human, separate from Block E PASS)
  → H3 first low-duty drive, wheels UNLOADED (P, powered)
  → H4 rollout (P) → H5 velocity trace (P) → H6 feed-forward + PI bootstrap (P)
  → H7 distance/braking/reversal incl. Part L closed-loop check (P)
  → H0W-S8 loaded watchdog re-validation (S, owed before freeze)
  → H8 bottle integration (P) → H9 fault injection (P) → H10 repeatability (P)
  → H14 ordered calibration checklist (spans H-steps; one experiment unlocks the next)
Block F (first power beyond H1A-A1/A2 levels) is NOT AUTHORIZED by Block E PASS.
See `docs/bringup/E0-block-f-handoff.md` F-P1..F-P12 register.
```

## Per-stage table

| Stage | Prerequisites | Power? | Firmware variant | Instruments | Measurable outcomes | Expected diagnostics | Operator STOP | Evidence required | Human auth | Depends on |
|---|---|---|---|---|---|---|---|---|---|---|
| E0 Block E (E0-unpowered-inspection-protocol, E-01..E-40) | Workspace setup E-01; meter commissioned E-04; procedures printed | NO power (battery disconnected, USB disconnected for continuity) | NONE (no firmware) | DMM (commissioned), insulated tools, camera, printed E0 forms | Continuity table; polarity marks; carrier revision; conductor gauges; disconnect device identity; fastener/retention state | None from firmware; paper forms only | Exposed pack terminals; unmarked polarity; untraceable fed Nano pin; no one-motion rail isolation; binding mechanics | Filled E0 forms + photos per `E0-inspection-record-template.md`; F-P register gaps cited | Human review pre-F (F-P1..P12); Block E PASS does not authorize power | None; gates everything powered |
| H0 repeat (P-H0) | E0 passed for workspace/safety items; board in hand | USB only, motor supply OFF + disconnected | `env:nano_r4` for identity + `env:bench` for health (never mixed) | USB cable, host terminal 115200, `pio` | `BOOT,...` first line on attach; `VERSION` rev/dirty/variant/cfg; `status/checks` refusal shape; telemetry cadence 20 ms | `BOOT,fw=ev-nano-r4,rev=<sha>,dirty=0/1,variant=production,cfg=<hex>`; `VERSION` same; `STATUS` with S19 counters; `bench health` image=bench | Upload touch failure; no BOOT greeting root-caused per HostGreetPolicy limits; unexpected reset | Raw serial log + `VERSION` line + `git rev-parse HEAD` + image variant; manifest §Timestamps | Operator may run (no motion possible); H0 must pass before H1A/H2 | E0 workspace |
| H0W (P-H0W) | H0 passed | USB only (WDT bench image) | `env:bench-watchdog` (Step 1) then `env:bench` | Same + stopwatch for reset gaps | WDT timeout 114 ms realized ~174 ms; refresh gap ≤14 ms; hang demo resets; resetsrc record | `BENCH,health=...,enable=1,timeout_ms=114,wdt_realized_ms=174,...` | WDT never fires on hang; refresh gap > budget; resetsrc unreadable | Raw logs per H0W steps; Step 8 loaded re-validation DEFERRED until after H7 | Operator; loaded Step 8 needs H7 complete | H0 |
| H1A (P-H1A) | H0 (+H0W recommended); motor leads DISCONNECTED + recorded; wheels unloaded | MCU powered as in vehicle; rail ONLY low-limit bench supply for A2 (record limit); A0 continuity unpowered | `env:bench` (temporary `bench h1a-brake` holds awake-EN-low for metering) + `env:nano_r4` de-energized startup | DMM (V), bench supply w/ current limit, camera | A0 continuity map D2/D4/D8/D9/D10/D12 vs `src/PinMap.h`; A1 idle nSLEEP low/PH low/EN 0-duty in volts; A2 truth-table level-presence per input combo + ~0 supply current | `VERSION` identity; `bench health` image check; `bench h1a-brake` held state; production `status` phase=Idle/armed=0 | Any continuity mismatch; floating PMODE tie; EN/PH swapped; supply current > ~0 unloaded; reset pin behavior unknown (F-P6 → gate A2, not F-entry) | Continuity table; level table in volts; truth-table output-presence rows; limit record; photos | Human F-entry review (F-P4/F-P6); motor stays disconnected | H0, Block E |
| H1B Parts 1/2/4 (P-H1B) | H0, H1A | MCU on; rail as H1A; NO motor drive | `env:nano_r4` + `env:bench` raw-input readout (B4) | DMM | Raw A1/A2 counts recorded as counts (NOT volts — no divider/shunt installed); trigger idle/active levels; nFAULT Part 3 = N/A (`kPlannedDriverFaultCapability=NotInstalled`, `kMotorFaultBar=kNoPin`) | `status`; bench B4 raw inputs; telemetry `nfault=NotInstalled` (level absent) | Encoder supply unidentifiable; trigger levels ambiguous | Raw-count pairs + level table; N/A justification citing `src/VehicleConfig.h` + `src/PinMap.h` | Operator; F-P5 encoder rating citation before H2 powered reads | H0, H1A |
| H2 (P-H2) | H0; H1B supply check; pull-up setting noted as placeholder (`src/EncoderInput.h`) | De-energized (hand rotation only) | `env:nano_r4` (encoder count via `status`/telemetry) or `env:bench` readout | Hand, marked wheel, tape measure for rev counting | Sign: forward = increasing ticks (informal 2026-10-02 observation, NOT evidence); repeat counts per N hand revolutions; tug-test retention | Encoder ticks monotonic per direction; `status` distance field moves with correct sign; no validity claim on supply | Grinding/catching; count non-repeatability; cable unretained | Repeated counts table (≥ protocol floor); sign record; retention photo | Operator | H0 (+H1B for supply) |
| Bounded first-power approval | Block E PASS + H0/H0W + H1A/H1B/H2 passed + F-P1..P12 reviewed | — (decision gate, no power applied) | Declared image + `rev/dirty/variant/cfg` pre-registered | F-handoff register, E0 forms, H-records | Signed approval naming image identity, current limit (from motor stall citation F-P8), runout, kill-switch in reach | N/A | Any F-P blocker open; stall current unknown; no rated disconnect; conductors unknown | Approval record citing evidence pointers | HUMAN, separate from Block E PASS | All above |
| H3 (P-H3) | Approval + H1A + H1B + H2 | Motor power, LOW duty, wheels UNLOADED, current-limited | `env:bench` B2 `bench drive <fwd|rev> <permille≤150> <ms≤500>`, ≤8 cmds/arm | Current-limited supply, tachometer/observer, kill switch | Direction truth PH→forward (#3, first real evidence); min useful effort; B2 expiry/latch demo (Step 0) before any production run | `BENCH` replies (typed triple is the command record — `EVS3 motor/effort` stays Disabled/0 under bench ownership); `bench health` lines_accepted/refused + responses_lost/serial_abandoned | Unexpected direction surprise; current over limit; latch/expiry fails; any binding | Raw BENCH logs + typed triples + timestamps; current readings; direction record | Approver present | Approval |
| H4/H5 (P-H4/P-H5) | H2, H3 | H4 hand + low-power translation; H5 unloaded powered | H4 hand + bench; H5 `env:bench` open-loop trials | Tape/measured course, camera, DMM | H4 rollout mm/tick (replaces ~1.339 informal figure) + slip bound; H5 effort→velocity steady-state pairs | Encoder-derived distance vs tape; `EVS3` velocity fields (production path only where applicable) | Slip large/unbounded; non-linear plant unstated | Raw pairs + arithmetic; rejected runs kept | Operator + approver for powered translation | H2, H3 |
| H6 (P-H6) | H5 trace | Unloaded powered (bench open-loop) + later loaded run path | `env:bench` B2 for feed-forward; run path P-H7 Part L for closed loop | Same | Feed-forward slope Measured (open-loop valid); P/I stays Provisional until Part L | H5 pairs → slope derivation with citation; `checks` still refuses (`ControllerConfigValid==false`) | Bench used to claim closed-loop tuning (impossible — `VelocityController::update()` never runs under bench) | Derivation + input capture pointer; candidate labeled AnalyticalCandidate, never Measured | Operator | H5 |
| H7/H0W-S8/H8/H9/H10/H14 | H4+H6 (H7); H7 (loaded WDT, H8, H9); H7+H8+H9 (H10) | Powered loaded (runout + kill switch) | `env:nano_r4` production run path (H7 Part L is where the regulator runs) | Full course, bottle + witness marks/camera, timekeeper/video for Run Time | Braking offsets per direction; reversal safety; closed-loop P/I Measured (Part L criteria); loaded WDT dwell-gap; bottle positions believed vs observed; fault behavior; repeatability stats | `EVS3/EVE3` timeline; `PLAN` line; `status`; operator events with action-stamped `nowMs` (R3/L2) | Brake/reversal surprise; feedback-loss 0.5 s placeholder trips wrong (unvalidated); Run Time mismatch vs timekeeper | Full telemetry captures + manifest; pre-registered criteria; rejected runs with reason | Human per-protocol; competition freeze separate | As chained |

No circularity: E0/H0/H0W/H1A/H1B/H2 need no powered result; first power needs
them; calibration needs powered data; arming needs calibration (arm gate refuses
Provisional by construction — `ControllerConfigValid`, `ExecutionPlanNotEstablished`,
`MeasuredValuesCiteEvidence`).
