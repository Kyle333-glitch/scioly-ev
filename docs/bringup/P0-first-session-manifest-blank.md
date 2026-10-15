# P0 — First-Session Evidence Manifest (BLANK — fill by hand or by validator)

**No result pre-filled. A copy claiming PASS without raw logs is void. Synthetic
vs physical provenance must be marked. Attach raw serial logs under
`data/evidence/<session-id>/`.**

- Session ID: ____________________ (e.g. `P-H0-2026-XX-XX`)
- Stage / protocol + revision: ____________________ (e.g. `P-H0 @ 20aa4a1`)
- Date / location / operators: ____________________ / ____________________ / ___________
- Firmware identity (`VERSION` verbatim): `________________________________________`
- `git rev-parse HEAD`: ____________________ ; `dirty=` ______ ; `variant=` ______ ; `cfg=` ______
- Image flashed (`nano_r4` / `bench` / `bench-watchdog`): __________ ; upload log (`Download done.` bytes): ______
- `BOOT` first-line present? Y / N ; attach time: ______ ; host/reader: ______
- Hardware config: pack V ______ ; limit ______ ; motor connected? Y / N ; wheels loaded? Y / N ; disconnect device: ______
- Raw logs (relative paths + SHA256): ________________________________________
- Command chronology (cmd → reply → counters delta): _________________________
- Replies missing (count + which cmds): ______ ; `responses_lost` start/end: ____/____ ; `serial_abandoned` start/end: ____/____
- `STATUS`/`bench health` samples (start/end verbatim): ______________________
- Encoder evidence (ticks start/end, sign, repeats): _________________________
- Trigger levels (idle/active V): __________ / __________
- Watchdog / reset events (resetsrc, gaps): __________________________________
- Operator STOP events (time + reason + rail state after): ____________________
- Unexpected resets (time + last lines before): _______________________________
- Timestamps + wrap note (`t_ms` start/end, rollover? Y/N): ___________________
- Provenance: PHYSICAL / SYNTHETIC (circle one); if synthetic, generator: _____
- Incomplete-session? Y / N ; missing items: _________________________________
- Verdict (protocol acceptance rule, quoted): _________________________________
- Signatures: ____________________ Date: __________

Validator: `python tools/p0_log_completeness.py --session <dir> --manifest <file>`
reports explicit per-check VALIDATED / WARNING / INCOMPLETE / NOT CHECKED / INVALID
for log presence, BOOT/VERSION presence, per-family counter continuity (`STATUS` 8,
`bench health` responses_lost, `bench serial` 6), reply correlation (NOT CHECKED when
no `> `/`CMD:` echoes exist), structured provenance + session metadata — offline and
read-only, never opens serial. Exit 1 on INVALID/INCOMPLETE, 0 otherwise (--advisory
forces 0 for human triage). Exit 0 is never safety approval, delivery proof, or
Block E/F qualification. `rev/dirty/variant/cfg` identify source only; every powered
session still owes its upload log plus `VERSION`-after-flash plus `bench health image=`
read-back (no binary attestation exists).
