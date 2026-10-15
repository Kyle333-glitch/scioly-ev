# E0 — Inspection Record Template (BLANK — print and fill by hand)

**No result is pre-filled. Any copy of this form claiming PASS without
measurements is void.** File completed forms under `data/evidence/Block-E-<date>/`.

Session header (one per session): date/time, inspector (print/sign),
supervisor/countersign, vehicle identity, firmware revision of record
(`005c55a`, dirty/clean N/A — no flash in Block E), meter make/model/fuse,
photo device, procedure revision (this file's commit SHA).

## Form A — Installed-parts inventory (from E-01/E-06)

| Item | Marking transcribed verbatim | Revision | Condition | Photo ID | Verdict |
|------|------------------------------|----------|-----------|----------|---------|
| Nano R4 board | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Driver carrier | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Motor (+encoder housing) | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Battery holder + cells (count, model) | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Switch/disconnect device | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Trigger assembly | | | | E01- | PASS / FAIL / NOT PERFORMED |
| Bottle mechanism | | | | E01- | PASS / FAIL / NOT PERFORMED |

## Form B — As-built pin-by-pin connections (from E-26/E-27/E-34/E-38)

One row per W-ID. "Observed" is what the inspector traced, not what intent says.

| W-ID | MCU/pin end (observed) | Net color/gauge/connector | Device/pad end (observed) | Matches intent (§3 of inventory) | Ω (if metered) | Photo | Verdict |
|------|------------------------|---------------------------|---------------------------|----------------------------------|----------------|-------|---------|
| W-01…W-20 (copy all 20 rows) | | | | MATCH / DEVIATION / NOT PERFORMED | | | PASS / FAIL / NOT PERFORMED |

Deviation detail (any MATCH≠yes): W-ID, description, photo, inspector initials.

## Form C — Supply and ground routing (from E-03/E-33)

Rail path sketch reference (photo ID): ____. Pack+ route: ____. Pack− route:
____. Fed Nano input pin (VIN/5V/other): ____. Ground continuity (Ω):
pack−→driver GND: ____; driver GND→Nano GND: ____; Nano GND→encoder GND: ____;
Nano GND→trigger return: ____. Pack+→ground isolation (open/beep): ____.

## Form D — Mechanical inspection (from E-20…E-24)

Chassis: ____. Per assembly (Nano/driver/motor/holder/trigger/bottle):
rock-test result ____. Hand-rotation feel (both directions): ____.
Wheel diameters (mm, L/R): ____ / ____. Retention (pull-test): ____.
Trigger mechanical action: ____. Pinch points marked (photo): ____.

## Form E — Continuity/isolation results (from E-32…E-36)

| Check | Meter mode/range | Reading + units | Criterion | Photo | Verdict |
|-------|------------------|-----------------|-----------|-------|---------|
| E-32 rail absence (pack+, VM, VIN) | DCV >15 V | V / V / V | each <0.5 V | | PASS / FAIL |
| E-33 grounds (4 nets) | continuity | Ω ×4 | indicated + recorded (no fixed Ω; outliers vs siblings = finding) | | PASS / FAIL |
| E-33 pack+ isolation | continuity | open / beep | OPEN | | PASS / FAIL |
| E-34 signals (6 nets) | continuity | Ω ×6 | indicated + recorded (same outlier rule) | | PASS / FAIL / NOT PERFORMED+reason |
| E-35 PMODE tie | continuity | Ω | indicated + recorded | | PASS / FAIL |
| E-35 PMODE→Nano | continuity | open / beep | OPEN | | PASS / FAIL |
| E-36 switch OFF | continuity | open / beep | OPEN | | PASS / FAIL |
| E-36 switch ON (pack absent) | continuity | open / beep | BEEP | | PASS / FAIL |

## Form F — Power-disconnect inspection (from E-02/E-06/E-36/E-40)

Pack location during session: ____. Terminal covers: Y/N. Switch identity:
____. Switch rating (from body): ____. OFF=open verified: Y/N. ON=continuous
verified (pack absent): Y/N. One-motion drill time/s: ____. Reachable with
covers on: Y/N. USB absent start/end: Y/N / Y/N.

## Form G — Component rating verification (from E-01 labels + compatibility review)

| Question | Label/citation observed | Compatible Y/N/UNKNOWN | Blocks F prerequisite ID |
|----------|------------------------|------------------------|--------------------------|
| Carrier VM rating vs ≈11.6 V peak | | | F-P1 |
| Nano fed input + its rating | | | F-P2a/b |
| Driver VIH/VIL vs 3V3 logic | | | F-P4 |
| Encoder supply + levels | | | F-P5 |
| Carrier pulls on nSLEEP/PH/EN | | | F-P6 |
| Motor stall vs limit plan | | | F-P8 |
| Thinnest supply conductor | | | F-P9 |
| Switch DC rating | | | F-P10 |
| Heatsink/airflow | | | F-P11 |
| USB+pack mux permission | | | F-P16 |

## Form H — Hazard/discrepancy ledger (from E-25/E-43)

| ID (H-xx or DEV-nn) | Description | Severity | Evidence (photo/form) | Disposition: CLEARED / OPEN-BLOCKS-E / OPEN-BLOCKS-F / DEFERRED-TO-protocol | Inspector |
|---|---|---|---|---|---|

## Form I — Inspector signoff (from E-09/E-39/E-45)

E-09 isolation declared: ____ (sign/time). E-39 closing sweep: ____ (sign/time).
Overall verdict (E-44): PASS / FAIL / INCOMPLETE. Inspector: ____ date: ____.
Supervisor countersign: ____ date: ____. Photo set archived at: ____.

## Form J — Block E readiness summary

Mandatory checks executed: __/45. NOT PERFORMED (permitted, with reasons):
____. Critical deviations: ____. Mechanical FAILs: ____. Verdict: ____.

## Form K — Block F prerequisite register (copied from handoff at execution)

| F-P ID | Requirement | Evidence now held | Still required | Owner phase | Blocking |
|--------|-------------|-------------------|----------------|-------------|----------|
(F-P1…F-P11, F-P16 rows from `E0-block-f-handoff.md`; fill "Evidence now held"
from Forms G/H, leave the rest to the authorizing reviewer.)
