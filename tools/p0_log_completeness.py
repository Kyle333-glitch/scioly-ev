#!/usr/bin/env python3
"""P0 offline log-completeness validator (no serial, no hardware, no upload).

Reads a session directory of raw *.log captures plus a manifest file and reports
which mandatory evidence is missing. All verdicts are offline/paper checks;
a PASS here is never a hardware qualification.

Checks:
 1. At least one raw *.log exists (incomplete-session detection).
 2. A BOOT or VERSION identity line is present somewhere.
 3. STATUS/bench-health counter fields (responses_lost/serial_abandoned) parse
    and never go backwards (device-side continuity).
 4. Command chronology: lines starting with '> ' or 'CMD:' without a following
    reply line are reported as possible missing replies (heuristic, offline).
 5. Provenance marking: manifest must contain PHYSICAL or SYNTHETIC.
 6. STOP/reset keywords surfaced for the operator to disposition.

Exit 0 = check ran (report may list MISSING items); exit 2 = bad invocation.
"""
import argparse
import re
import sys
from pathlib import Path

BOOT_RE = re.compile(r"^BOOT,")
VER_RE = re.compile(r"^VERSION,")
COUNTER_RE = re.compile(r"responses_lost=(\d+).*?serial_abandoned=(\d+)")
CMD_RE = re.compile(r"^(> |CMD:)(.*)")


def main() -> int:
    ap = argparse.ArgumentParser(description="P0 offline log completeness check")
    ap.add_argument("--session", required=True, help="directory with raw *.log files")
    ap.add_argument("--manifest", required=True, help="manifest file to check for provenance")
    args = ap.parse_args()

    sess = Path(args.session)
    mani = Path(args.manifest)
    findings = []

    if not sess.is_dir():
        print(f"MISSING: session dir not found: {sess}")
        return 0

    logs = sorted(sess.glob("*.log"))
    if not logs:
        findings.append("MISSING: no raw *.log in session dir (incomplete session)")
    else:
        findings.append(f"OK: {len(logs)} raw log(s)")

    has_boot = has_ver = False
    prev_lost = prev_ab = None
    pending_cmd = None
    gaps = 0
    for log in logs:
        try:
            text = log.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as e:
            findings.append(f"MISSING: unreadable {log.name}: {e}")
            continue
        for i, line in enumerate(text):
            s = line.strip()
            if BOOT_RE.search(s):
                has_boot = True
            if VER_RE.search(s):
                has_ver = True
            m = COUNTER_RE.search(s)
            if m:
                lost, ab = int(m.group(1)), int(m.group(2))
                if prev_lost is not None and (lost < prev_lost or ab < prev_ab):
                    findings.append(
                        f"MISSING: counter regression in {log.name}:{i+1} "
                        f"(lost {prev_lost}->{lost}, abandoned {prev_ab}->{ab})"
                    )
                prev_lost, prev_ab = lost, ab
            c = CMD_RE.match(s)
            if c:
                if pending_cmd is not None:
                    gaps += 1
                    findings.append(f"MISSING: possible missing reply before {log.name}:{i+1} for '{pending_cmd}'")
                pending_cmd = c.group(2).strip()
            elif s.startswith(("OK,", "ERR,", "BOOT,", "VERSION,", "STATUS", "BENCH,", "EV")):
                pending_cmd = None
        if pending_cmd is not None:
            gaps += 1
            findings.append(f"MISSING: trailing command with no reply in {log.name}: '{pending_cmd}'")
            pending_cmd = None

    findings.append("OK: BOOT line present" if has_boot else "MISSING: no BOOT line in any log")
    findings.append("OK: VERSION line present" if has_ver else "MISSING: no VERSION line in any log")

    if not mani.is_file():
        findings.append(f"MISSING: manifest not found: {mani}")
    else:
        body = mani.read_text(encoding="utf-8", errors="replace")
        if "PHYSICAL" in body or "SYNTHETIC" in body:
            findings.append("OK: provenance marked (PHYSICAL/SYNTHETIC present)")
        else:
            findings.append("MISSING: manifest lacks PHYSICAL/SYNTHETIC provenance mark")
        if "STOP" in body:
            findings.append("OK: STOP field present in manifest (verify content by hand)")

    missing = sum(1 for f in findings if f.startswith("MISSING"))
    for f in findings:
        print(f)
    print(f"summary: {missing} missing item(s), {gaps} reply gap(s); SYNTHETIC unless session was on hardware")
    return 0


if __name__ == "__main__":
    sys.exit(main())
