# E0 — Unpowered Hardware Verification Protocol (Block E)

**Status: PROCEDURE READY FOR HUMAN REVIEW. NOT EXECUTED. No result recorded.**
**Outcome as of authoring: BLOCK E NOT EXECUTED.**

For: the exact vehicle at `005c55a4ec53892f5847a3298929e9b28bd2593b`
(`work/block-e-protocol`). Companion docs:
`E0-wiring-and-component-inventory.md` (intent),
`E0-electrical-compatibility-review.md` (paper analysis),
`E0-hazard-register.md` (H-xx), `E0-inspection-record-template.md` (blank forms),
`E0-block-f-handoff.md` (what happens after).

Repository meaning of "Block E" (reconciled, §9 of assignment):
the repo's bring-up plan (`docs/bringup/README.md`) uses **E = electrical gate**
(a check class, some of which need bounded power, e.g. H1A-A2) and scatters
unpowered work across H1A-A0, H1B visual parts, and H2 setup. This Block E
procedure collects **only the strictly unpowered subset** — visual, mechanical,
and meter checks with every energy source absent — into one chronological
inspection. Anything needing the rail, USB, or a metered output state stays in
H1A/H1B/H2 and is entered only through the Block F handoff. No powered
qualification is inside this file.

## 0. Rules of execution (read first)

1. **Order is load-bearing.** Perform steps in ID order E-01…E-45. A step's
   prerequisites name earlier steps; never skip forward.
2. **Power states used:** `ABSENT` (source physically elsewhere, verified by
   sight + touch of empty bays/connectors) and `ISOLATED` (source on site but
   disconnected, terminals covered). No other power state exists in Block E.
3. **"De-energized" ≠ "safe to touch the rail."** Firmware levels are logic;
   the rail is made safe only by physical absence/isolation of the pack.
4. **Continuity/resistance mode injects current.** Use it only where the step
   allows, only on the nets named, and only after E-02 isolation is confirmed.
   Never resistance-probe IC pins directly (probe the wire/connector side).
   Never meter in current mode across any source — current mode is FORBIDDEN
   in Block E (H-22).
5. **Every mandatory check ends PASS, FAIL, or NOT PERFORMED with reason.**
   NOT PERFORMED is honest and allowed where the step permits it; a mandatory
   check left blank fails the inspection.
6. **STOP means stop.** §STOP lists conditions that halt the session. A STOP
   is recorded, the vehicle is left isolated, and resumption needs a new
   session entry. Correcting wiring (re-soldering, re-routing) is NOT part of
   Block E — record and halt; the fix is a separate reviewed action.
7. **Evidence per check:** check ID, date/time, inspector, meter model + lead
   verification (E-04), observed result + units, criterion, verdict, photo ID.
   Blank forms: `E0-inspection-record-template.md`.

## STOP conditions (any one halts the session immediately)

- S-1: any energy source found connected where ABSENT/ISOLATED is required.
- S-2: pack, switch, or rail polarity unidentifiable or contradictory marks.
- S-3: any conductor hot, smelling, discolored, or damaged such that handling
  is unsafe (pack damage included).
- S-4: no one-motion rail isolation method exists, or the "switch" is not in
  the rail path (H-19).
- S-5: a meter must be used in current mode to answer the step (forbidden).
- S-6: a resistance/continuity reading demands probing a live circuit.
- S-7: mechanical damage (cracked chassis/mount, split coupler, binding that
  suggests breakage) beyond a FAIL note.
- S-8: inspector uncertainty about which pin/net is being touched — stop,
  re-identify, never guess.

## PHASE E0 — OPERATOR AND WORKSPACE PREPARATION

### E-01 Component-label capture
- Objective: record what is actually installed before touching anything.
- Why: product numbers, ratings, and revisions in the repo are OPERATOR-REPORTED
  or UNKNOWN (inventory §1, §5.1); labels decide the compatibility citations.
- Equipment: camera, flashlight, tweezers/brush (dust only), labels + pen.
- Prerequisites: none. Power: vehicle as found — DO NOT connect or disconnect
  anything yet; photograph first.
- Actions: (1) photograph the whole vehicle top/side/bottom; (2) close-up each
  label: Nano R4 silkscreen, driver carrier front/back, motor label, encoder
  housing, battery holder + one cell's label, switch body, trigger assembly;
  (3) transcribe every legible marking into form A verbatim, flagging
  illegible fields as ILLEGIBLE rather than guessing.
- Expected: all labels legible and matching inventory intent (DRV8874 carrier,
  25D-class motor, 8 AA NiMH).
- Pass: every installed unit identified to model/revision, or explicitly
  ILLEGIBLE/ABSENT. Fail/STOP: a major unit (driver, motor, pack) missing or
  a different architecture than inventoried → STOP S-8, record, halt.
- Record: form A rows + photo IDs E01-*. Evidence: photos.
- Recovery: none needed (non-contact). Next: E-02.

### E-02 Energy-source sweep and removal
- Objective: establish the no-power boundary for the whole session.
- Why: every later meter step assumes ABSENT/ISOLATED; an undiscovered source
  (USB cable, pack in bay, charged capacitor bank) voids them.
- Equipment: none (eyes + hands).
- Prerequisites: E-01. Power: as found.
- Actions: (1) confirm NO USB cable attached to the Nano (look + feel);
  (2) confirm the battery bay/holder empty OR pack removed to the isolation
  tray with terminals covered; (3) confirm no bench supply leads attached;
  (4) confirm no second pack, 9 V block, or power bank on the bench
  (clear the bench of all sources not in use); (5) state aloud "vehicle
  unpowered, pack isolated" and log the pack location.
- Expected: empty bay or empty connectors; no USB; bench clear.
- Pass: all sources ABSENT from the vehicle; pack location logged.
  Fail/STOP: any source still connected → remove if safe, else STOP S-1.
- Record: form F (disconnect inspection) + photo E02-bay.
- Recovery: re-seat covers, re-log. Next: E-03.

### E-03 Supply-path trace (eyes only)
- Objective: map the rail path pack → switch → driver VM → Nano input without
  touching meter leads.
- Why: fed-pin and switch identity are UNKNOWN (W-01…W-05); this trace decides
  whether E-10/E-11 can proceed.
- Equipment: wiring intent table (inventory §3), marker flags.
- Prerequisites: E-02. Power: ABSENT.
- Actions: follow each supply conductor end-to-end by sight/touch; flag W-01…
  W-05 with the numbered tags; note conductor colors, gauges (by marking, not
  by stripping), connector types, and every polarity mark.
- Expected: one continuous + path and one continuous − path, both traceable.
- Pass: every W-01…W-05 row has a physical conductor identified or is marked
  MISSING. Fail/STOP: untraceable rat's-nest or contradictory polarity → STOP
  S-2/S-8.
- Record: form B rows W-01…W-05 + form C supply section. Next: E-04.

### E-04 Meter commissioning
- Objective: prove the meter trustworthy before it judges anything.
- Why: H-22/H-26 — a wrong jack or dead fuse fabricates every later reading.
- Equipment: DMM (REQUIRED), insulated probes; spare fuse if available.
- Prerequisites: E-02. Power: ABSENT (meter tests its own leads on its own
  internal source only).
- Actions: (1) jacks: black→COM, red→VΩ (confirm aloud; current jacks must be
  EMPTY — tape them if the meter allows); (2) dial to continuity, touch probes:
  expect beep/<1 Ω; (3) dial to DCV, read a known source SEPARATE from the
  vehicle pack (a spare AA or the meter's own test — never the vehicle pack:
  a fresh alkaline AA reads ≈1.5 V, a fresh NiMH AA reads ≈1.3–1.4 V; either
  is acceptable if plausible for its chemistry); (4) inspect probe insulation
  for nicks; (5) log meter make/model/fuse state.
- Expected: beep test <1 Ω; known-source voltage plausible.
- Pass: both self-tests recorded with values. Fail: replace leads/fuse/meter;
  do not proceed on a failed self-test.
- Record: form header (equipment block). Next: E-05.

### E-05 Workspace and supervision check
- Objective: confirm the session may legally and practically proceed.
- Why: battery/electrical work needs competent supervision per assignment §10.
- Equipment: none.
- Actions: confirm (1) adult/lab supervision present as required; (2) clear
  stable bench, nonconductive mat if available; (3) conductive clutter removed
  (tools not in use off the mat, jewelry off); (4) adequate lighting;
  (5) phone/camera charged; (6) printed forms + intent table at hand;
  (7) STOP list read aloud.
- Pass: all seven confirmed. Fail: fix the workspace, re-log. Next: E-06.

## PHASE E1 — POWER ISOLATION (verification, not assumption)

### E-06 Pack isolation verification
- Objective: prove the pack cannot feed the vehicle.
- Equipment: eyes, terminal covers/tape.
- Prerequisites: E-02. Power: ISOLATED or ABSENT.
- Actions: (1) if pack on site: count cells (expect 8), note chemistry/model,
  cover both pack terminals; (2) confirm holder empty (photo); (3) confirm
  vehicle-side pack connector empty and covered; (4) log storage location.
- Expected: 8 cells, NiMH, terminals covered, holder empty.
- Pass: cell count + chemistry logged, both sides covered.
  Fail/STOP: leaking/swollen cells or damaged holder → STOP S-3.
- Record: forms A + F. Next: E-07.

### E-07 USB and external-source absence re-check
- Objective: close the back-powering path (H-21) for the session.
- Actions: re-confirm no USB, no bench leads, no power bank within reach of
  the vehicle. Sign the E-07 line.
- Pass: confirmed. (This re-check is deliberate, not redundant: cables get
  plugged in "just for a photo" mid-session.) Next: E-08.

### E-08 Stored-energy survey
- Objective: identify anything that holds charge with sources removed.
- Actions: visually identify capacitors (driver carrier bulk cap, Nano
  decoupling — look, don't probe); note any supercap/battery-backed module
  (none expected); record that small-signal stored energy needs no discharge
  action for visual/continuity work, and that rail-capacitor charge is a Block F
  (pre-power) consideration, not a Block E discharge task.
- Pass: survey logged; no unexpected energy store. STOP S-3 on bulged/leaking
  caps. Next: E-09.

### E-09 Isolation state declaration
- Objective: commit the session boundary to the record.
- Actions: inspector signs: "Pack ISOLATED/ABSENT, USB ABSENT, no bench
  supply. All E1 steps PASS." Any exception keeps E1 open and blocks E2+.
- Next: Phase E2.

## PHASE E2 — COMPONENT AND MECHANICAL INSPECTION

### E-20 Chassis and fastener survey
- Objective: structural soundness.
- Equipment: correct-size drivers/hex keys (fit-check only first), flashlight.
- Prerequisites: E-09. Power: ABSENT.
- Actions: (1) inspect chassis for cracks/bends; (2) attempt to rock each
  major assembly (Nano mount, driver mount, motor mount, battery holder,
  trigger mount, bottle mechanism) — movement beyond elastic flex is a FAIL;
  (3) check each visible fastener present; tighten ONLY fasteners whose spec
  is obvious AND which are clearly loose — else record TORQUE-UNKNOWN and
  leave; (4) note threadlocker presence/absence without applying any.
- Pass: no cracks, no loose assembly, all fasteners present.
- Fail: cracked chassis/mount or missing fastener → FAIL (Block E cannot pass
  until repaired in a separate action). Next: E-21.

### E-21 Gearmotor, geartrain, wheels
- Objective: mechanical freedom and retention (H-15/H-16).
- Actions: (1) motor mount: rock test + fastener check; (2) rotate wheels by
  hand both directions: expect smooth motion, note drag/grit spots by wheel
  angle; (3) wheel/coupler retention: pull-test gently along the axle (no
  yanking), check set-screws/flats; (4) gear-mesh visual: debris, chipped
  teeth, lubricant state; (5) record wheel diameters with calipers/ruler
  (geometry for later H4 ground truth — a dimension, not a calibration).
- Expected: smooth rotation, no axial play beyond a hair, no debris.
- Pass: free smooth rotation both ways, retained wheels/couplers.
  Fail/STOP: binding/grinding → FAIL (STOP S-7 if breakage suspected).
- Record: form D + wheel-diameter values. Next: E-22.

### E-22 Motor-output lead dress
- Objective: inspect W-10/W-11 routing and connector (H-06).
- Actions: trace both motor leads driver→motor; note connector type, latch
  state, slack/strain relief, chafing; photograph. DO NOT disconnect (P-H1A
  owns the disconnection record when its session comes).
- Pass: connector latched, no chafe, routing clear of wheels/gears.
  Fail: chafed insulation or unlatched connector → FAIL. Next: E-23.

### E-23 Cable routing, strain relief, clearances
- Objective: routing that survives motion (Q/R).
- Actions: check every harness section for: minimum bend respect, clearance
  from wheels/gears/pinch points at full steering/suspension travel (move by
  hand), strain relief at both ends, no conductor resting on sharp edges.
- Pass: all sections clear and relieved. Fail: interference found → FAIL with
  photo. Next: E-24.

### E-24 Bottle mechanism and trigger mechanics
- Objective: mechanical soundness of the mission hardware.
- Actions: (1) bottle pusher/funnel: mount check, free movement by hand,
  no cracks; (2) trigger ("pencil") assembly: pivot/spring/contact action by
  hand — expect a crisp mechanical click/feel; (3) confirm trigger linkage
  cannot jam the drivetrain at any position.
- Pass: mechanism moves freely, trigger actuates crisply.
  Fail: jammed/cracked/binding → FAIL. (Electrical side is E-17/E-30.)
  Next: E-25.

### E-25 Moving-part and pinch review + as-built photos
- Objective: close the mechanical phase with hazards marked and evidence kept.
- Actions: (1) mark pinch points on the E25 sketch/photo; (2) take the
  as-built photo set: overall ×4 sides, harness overview, each connector
  close-up, switch, pack bay, trigger, bottle mechanism (min 12 frames, IDs
  logged); (3) list every mechanical defect/unknown in the discrepancy ledger.
- Next: Phase E3.

## PHASE E3 — VISUAL ELECTRICAL INSPECTION

### E-26 Supply-conductor identification (W-01…W-05 reconciled)
- Objective: eyes-only reconciliation of the rail against intent.
- Actions: for each W-01…W-05: confirm conductor exists, color/gauge/connector
  noted, polarity mark present and consistent end-to-end.
- Pass: all five traced with consistent marks. Fail/STOP: contradiction → S-2.
- Record: form B. Next: E-27.

### E-27 Logic/encoder/trigger conductor identification (W-06…W-09, W-12…W-17)
- Actions: trace each signal conductor Nano↔driver/encoder/trigger; confirm
  connector orientation (key/latch/pin-1 mark) and that D2/D4/D8/D9/D10/D12
  land where intent says.
- Pass: all traced, orientations keyed or explicitly marked.
  Fail: any signal untraceable or mislanded → FAIL (map-vs-vehicle finding).
  Next: E-28.

### E-28 Absence confirmation (W-18, W-19)
- Objective: prove the "not installed" claims physically (D32/S10).
- Actions: (1) confirm no conductor leaves the driver carrier toward a
  fault-output pad/header (inspect carrier for unpopulated nFAULT pad vs
  wired stub — a stub tied anywhere is a finding); (2) confirm no divider/shunt
  circuitry and that Nano A1/A2 have no harness conductor.
- Pass: absences physically confirmed and photographed.
  Fail: unexpected circuitry found → FAIL (capability statements must be
  revisited outside Block E). Next: E-29.

### E-29 Solder-joint and connector workmanship
- Objective: catch the rework defect (2026-10-02 harness rework precedent).
- Actions: loupe-check every reworked/visible joint: wetting, bridges,
  whiskers, cold joints; tug-test each crimp gently; confirm no stray strands
  at screw terminals.
- Pass: no bridges/strays/cold joints. Fail: any bridge or stray strand → FAIL
  (STOP S-3 if rail-involved and unsafe to leave). Next: E-30.

### E-30 Trigger electrical construction (W-16/W-17)
- Actions: confirm contact type (NO/NC by construction), both terminals
  identified, return terminal's destination (ground net assumed — VERIFY by
  trace to the Nano GND pin or ground plane, eyes only this phase), wire
  dress clear of moving parts.
- Pass: both terminals traced, return confirmed to ground by trace.
  Fail: return to a non-ground net → FAIL. Next: E-31.

### E-31 Exposed-contact and chassis-short survey (H-20)
- Actions: (1) list every exposed conductor (headers, screw terminals, USB
  shell); (2) confirm no exposed rail conductor can touch chassis/hardware by
  a finger-press deflection test (gentle); (3) note chassis conductivity
  (metal vs plastic) for the record.
- Pass: rail conductors shrouded or positively clear; logic exposures noted.
  Fail: exposed rail or chassis-short path → FAIL. Next: Phase E4.

## PHASE E4 — APPROVED UNPOWERED METER CHECKS

General preconditions for E-32…E-37: E-02/E-06/E-07 isolation still in force
(re-verify by sight before the first probe touch); meter per E-04 with current
jacks EMPTY; one probe anchored before the other touches; never probe two
adjacent header pins with both tips simultaneously (H-26); never resistance-
probe an IC body pin — probe the wire/connector side of the net.

### E-32 Isolation re-verification (meter-assisted, non-invasive)
- Objective: meter-confirmed absence of voltage before continuity work.
- Actions: (0) label every intended probe point on a copy of the E-01
  close-up photo and confirm each is a wire, connector, or board-edge pad —
  never an IC body pin; if no wire-side access exists for a point, record
  NOT PERFORMED for that point with reason (self-audit fix A2). (1) dial DCV
  (range above 15 V); black probe on vehicle ground net (alligator anchored);
  touch red to: pack connector + terminal, driver VM conductor (wire side),
  Nano input pin (board edge, single-pin touch). Expect ≈0 V everywhere
  (residual millivolts acceptable, record values).
- Pass: all readings <0.5 V. STOP S-6/S-1 on any rail-level voltage.
- Record: form E (voltage column, all rows). Next: E-33.

### E-33 Ground-continuity net (W-03/W-05/W-13/W-17 return)
- Objective: prove the single reference (H-05).
- Actions: continuity mode; verify continuity INDICATED (beep) and record the Ω value for: pack− → driver
  GND; driver GND → Nano GND pin; Nano GND → encoder GND wire (connector
  side); Nano GND → trigger return terminal. No absolute Ω is prescribed (self-audit fix A1 — expected resistances are not invented): the criterion is an indicated continuity with the value recorded, and any net reading an order of magnitude above its sibling nets is a finding to investigate, not an auto-fail. Then confirm NO beep pack+ → any
  ground (isolation).
- Pass: all four grounds show indicated continuity with values recorded, pack+ isolated from ground.
  Fail: open ground → FAIL (Block F gate); pack+ beeping to ground → STOP S-3.
- Record: form E with Ω values. Next: E-34.

### E-34 Signal-continuity set (W-06/W-07/W-08, W-14/W-15, W-16)
- Objective: continuity proof of the stated V5 map (P-H1A step A0 equivalent).
- Actions: continuity mode, wire/connector side only: Nano D8→driver PH pad;
  D9→EN pad; D10→nSLEEP pad; D2→encoder-A wire; D4→encoder-B wire;  D12→trigger contact terminal 1. Record Ω each; no absolute Ω is prescribed (self-audit fix A1). Where a net has a legitimate parallel path (H-23), record AMBIGUOUS + reason instead of forcing a verdict.
- Pass: each net shows indicated continuity with value recorded, with no AMBIGUOUS on a rail- or
  direction-critical net. Fail: open or mislanded net → FAIL.
- Record: form B sign-off + form E. Next: E-35.

### E-35 PMODE-tie and mode-pin check (W-09)
- Actions: continuity carrier PMODE pad → ground net (expect beep); confirm no
  conductor from PMODE toward the Nano (expect open — beep here is a FAIL).
- Pass: tied low, no MCU connection. Fail: otherwise → FAIL. Next: E-36.

### E-36 Emergency-disconnect path check (W-01/W-02 through switch)
- Objective: prove the switch actually breaks the rail (H-18/H-19).
- Actions: (1) switch OFF/removed: continuity across the rail path pack+ →
  driver VM must be OPEN; (2) switch ON (pack STILL absent — operate the
  switch mechanism only): continuity pack+ → VM must BEEP; return switch OFF;
  (3) record one-motion drill time and hand used; (4) post-drill re-verify:
  with the switch left OFF, repeat the OFF=open continuity check once and
  confirm the rail path is OPEN before leaving the station (E-39 re-sweeps
  isolation again at closeout).
- Pass: OFF=open, ON=continuous, switch left OFF with OFF=open re-verified.
  The recorded drill time is assessed, not auto-failed: <3 s one-handed is an
  author-selected, unvalidated planning criterion (E0V F-E3), NOT a physical
  qualification — a slower time is recorded with its reason and assessed by
  the inspector, never a PASS/FAIL by itself.
  Fail/STOP: no break in OFF → STOP S-4 (no isolation method — Block E FAILS).
- Record: form F + E. Next: E-37.

### E-37 Unexpected-reading protocol (conditional)
- Objective: a defined response when any E-32…E-36 reading surprises.
- Actions: halt probing on that net; re-verify meter (E-04 beep test);
  re-verify isolation (E-32 on that net); re-seat probes and repeat once. If
  still unexpected: record NOT PERFORMED/FAIL with both readings, photograph,
  move on — do not improvise alternate probe points on IC pins.
- This step produces no PASS of its own; it protects the honesty of the rest.

## PHASE E5 — SYSTEM-LEVEL UNPOWERED RECONCILIATION

### E-38 Intent-vs-vehicle reconciliation
- Objective: the single table where truth is decided.
- Actions: for every W-row: mark MATCH, DEVIATION (describe + photo), or NOT
  PERFORMED (reason). A DEVIATION on any rail, ground, mode, or direction net
  fails Block E. Cosmetic deviations (wire color) are logged, not failed.
- Pass: zero failing deviations. Next: E-39.

### E-39 Nothing-left-connected sweep
- Actions: repeat E-02/E-07 as a closing sweep; confirm meter back in case
  (leads removed); confirm pack still isolated and covered; sign the line.
- Next: E-40.

### E-40 Emergency-isolation accessibility ruling (NOT a meter check — no meter used)
- Actions: with the vehicle posed as on the bench, demonstrate reaching the
  disconnect blind/with eyes averted (muscle memory), confirm access with the
  body/chassis fully assembled (covers on), record obstructions or PASS.
- Fail: unreachable with covers on → FAIL. Next: E-41.

### E-41 Deferred-to-powered ledger
- Actions: list every check this inspection could NOT answer by construction:
  H-09/H-11 (reset/power-up levels), H-12 (stall current), awake-EN-low
  brake-vs-coast, PWM frequency, trigger levels/bounce, encoder sign/counts,
  direction sense. Each gets its owning protocol (H1A/H1B/H2/H3) and its
  F-prerequisite ID from the handoff doc.
- This ledger is required for a PASS — "no open items" is only credible if
  the deferred items are named. Next: Phase E6.

## PHASE E6 — BLOCK E CLOSEOUT

### E-42 Evidence completeness review
- Actions: confirm every mandatory check has a verdict + photo ID + inspector
  initials; count NOT PERFORMED rows and confirm each is permitted its reason
  (E-37/E-23 citations). Missing evidence → session stays INCOMPLETE.
- Next: E-43.

### E-43 Discrepancy and hazard sign-off
- Actions: walk the H-ledger: each hazard H-01…H-26 marked
  CLEARED / OPEN-BLOCKS-E / OPEN-BLOCKS-F / DEFERRED-TO-PROTOCOL with evidence
  pointer. Any CRITICAL marked OPEN-BLOCKS-E fails the inspection.
- Next: E-44.

### E-44 Block E verdict
- Verdict rule (strict): PASS requires every mandatory check executed (or
  permitted NOT PERFORMED), zero unresolved critical electrical mismatches,
  zero unresolved mandatory mechanical hazards, supply polarity/routing fully
  accounted for, wiring reconciled with intent (E-38), no unknown powered
  conductor path, emergency isolation identified and drilled, unpowered
  measurements completed safely, deferred powered items ledgered (E-41),
  evidence recorded, human signoff (E-45). Else FAIL (defect found) or
  INCOMPLETE (work remaining). A PASS here authorizes NOTHING powered — see
  the handoff doc.
- Next: E-45.

### E-45 Inspector signoff and archiving
- Actions: inspector name (print + signature), date, supervisor countersign;
  photo set archived with IDs; forms filed under
  `data/evidence/Block-E-<date>/` (future commit, separate review); this
  procedure file itself is NOT edited with results.
- Inspection closed.

## Step-ID cross-reference (inventory §3 / hazard register → this procedure)

IDs E-10 through E-19 are FUNCTIONAL ALIASES, not additional chronological
execution steps. Only the 35 chronological step IDs defined above are executed,
in order; forms record chronological IDs only. An operator holding only the
inventory or hazard table must resolve every E-10…E-19 citation through this
mapping — never execute an alias as an extra physical step.

The inventory wiring table and hazard register cite functional step IDs E-10…
E-18 and E-20. They map onto the chronological steps above as follows:
E-10 (switch/rail isolation) → E-03 + E-36/E-40; E-11/E-12 (grounds) → E-33;
E-13 (signal tracing) → E-27/E-34; E-14 (PMODE/carrier) → E-35 (+ E-01 photo);
E-15 (motor leads/exposure) → E-22/E-31; E-16 (encoder supply/signals) →
E-27/E-33; E-17 (trigger wiring) → E-30/E-34; E-18 (absence proof) → E-28;
E-20 (continuity set) → E-33/E-34. The E-10…E-19 numeric block is otherwise
unnumbered here to avoid collision with the rail-energized H1A/H1B steps,
which are NOT part of Block E.

## Check census (E0V F-C1/F-C2 corrected)

35 CHRONOLOGICAL checks are executed: E-01…E-09 (9) + E-20…E-25 (6) +
E-26…E-31 (6) + E-32…E-37 (6) + E-38…E-45 (8) = 35. IDs E-10…E-19 (10) are
functional aliases resolved by the cross-reference above — they are NOT
additional execution steps. “45” is the count of identifiers (35 steps + 10
aliases), never the count of physical steps; Form J records __/35.

Of the 35 chronological steps: 6 are meter procedures, 21 are
visual/tactile-only, and 8 are record/decision steps (E-09, E-37, E-38,
E-41, E-42, E-43, E-44, E-45).

Meter-operation breakdown (E0V F-C2): E-04 is meter COMMISSIONING (2 self-test
probe operations: continuity beep test + known-source DCV read) and is counted
as 1 setup procedure, not as a measurement. The 18 measurement operations in
E-32…E-36 are: E-32 x3 (pack+ terminal, driver VM wire side, Nano input —
DCV isolation proof), E-33 x5 (4 ground continuities + pack+ to ground
isolation), E-34 x6 (D8/D9/D10/D2/D4/D12 signal continuities), E-35 x2
(PMODE to GND tie + PMODE to Nano open), E-36 x2 (switch OFF=open + switch
ON=continuous, pack absent). E-37 is a CONDITIONAL protocol producing no PASS
of its own. E-40 (blind-reach accessibility ruling) uses NO meter and is NOT
a meter check — its drill time is recorded in E-36/Form F.
Checks that cannot yet be fully specified: encoder-supply routing (E-16 folded
into E-27/E-33 — exact source pin UNKNOWN until traced), switch drill details
(E-36/E-40 adapt to whatever E-03 finds), carrier-specific probe points
(E-32…E-35 name nets, not pads — the E-01 carrier photo decides exact points
at execution time, recorded then).
