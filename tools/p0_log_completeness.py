#!/usr/bin/env python3
"""P1R offline log-completeness validator (no serial, no hardware, no upload).

Reads a session directory of raw *.log captures plus a manifest file and reports
explicit per-check offline evidence coverage. All verdicts concern OFFLINE
EVIDENCE ONLY, never vehicle safety, electrical safety, command delivery, or
Block E/F qualification.

Contract (offline evidence only):
  VALIDATED:   supported check performed with adequate evidence, no violation.
  WARNING:     suspicious but not conclusively invalid (plausible UINT32 wrap,
               ambiguous BOOT-boundary discontinuity, heuristic reply gap,
               truncated line, mixed variants/sessions).
  INCOMPLETE:  required evidence missing or a required manifest field is blank.
  NOT CHECKED: input lacks data for that check (no echoes, pre-S19 format,
               no bench-serial lines). Explicit, never silent.
  INVALID:     definite violation (genuine regression, malformed counter).

Honesty rules: missing command echo cannot prove replies arrived; if no
"> " / "CMD:" echoes exist, REPLY CORRELATION is NOT CHECKED. Transmission
counters are device-side only, never host-receipt proof. Counters are
cumulative uint32: wrap modulo 2**32, reset only on reboot/reset. A BOOT line
alone never proves reset cause; BOOT-adjacent drops are WARNING. Production
STATUS has all eight S19 counters; bench health has responses_lost; bench
serial has responses_lost + five serial_* counters. Pre-S19 lines are NOT
CHECKED for missing fields.

Exit: 0 = no INVALID/INCOMPLETE; 1 = INVALID or INCOMPLETE present; 2 = bad
invocation. Exit 0 is never safety approval or Block E/F qualification.
Pass --advisory to force exit 0 for human triage.
Read-only, stdlib only, deterministic (sorted logs, no randomness).
"""
import argparse
import re
import sys
from pathlib import Path

MAX_UINT32 = 0xFFFFFFFF
WRAP_MARGIN = 65535
WRAP_HIGH = MAX_UINT32 - WRAP_MARGIN
WRAP_LOW = WRAP_MARGIN

BOOT_RE = re.compile(r"^BOOT,")
VER_RE = re.compile(r"^VERSION,")
CMD_RE = re.compile(r"^(> |CMD:)(.*)")
STATUS_PREFIX = re.compile(r"^STATUS([, ]|$)")
HEALTH_PREFIX = re.compile(r"^BENCH,health=")
SERIAL_PREFIX = re.compile(r"^BENCH,serial=")
FIELD_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^,\s]*)")
VARIANT_RE = re.compile(r"variant=([^,\s]+)")
IMAGE_RE = re.compile(r"image=([^,\s]+)")

STATUS_COUNTERS = ["cmd_accepted", "cmd_rejected", "responses_lost",
    "serial_abandoned", "serial_refused", "serial_oversize",
    "serial_sent", "serial_staged"]
HEALTH_COUNTERS = ["responses_lost", "lines_accepted", "lines_refused",
    "lines_forwarded"]
SERIAL_COUNTERS = ["responses_lost", "serial_abandoned", "serial_refused",
    "serial_oversize", "serial_sent", "serial_staged"]
REPLY_CLEAR = ("OK,", "ERR,", "BOOT,", "VERSION,", "STATUS", "BENCH,", "EV")

def parse_fields(s):
    out = {}
    for m in FIELD_RE.finditer(s):
        out[m.group(1)] = m.group(2)
    return out

def parse_counter_value(raw):
    if raw is None:
        return (False, None, "absent")
    if not re.fullmatch(r"\d+", raw):
        return (False, None, "malformed")
    try:
        v = int(raw, 10)
    except ValueError:
        return (False, None, "malformed")
    if v < 0 or v > MAX_UINT32:
        return (False, None, "out-of-range")
    return (True, v, "")

def check_drop(prev, val, family, key, loc, boot_pending):
    if val >= prev:
        return None
    if boot_pending:
        return ("WARNING", family + ": " + key + " discontinuity " +
                str(prev) + "->" + str(val) + " in " + loc +
                " after BOOT boundary; reset cause NOT proven by BOOT alone")
    if prev >= WRAP_HIGH and val <= WRAP_LOW:
        return ("WARNING", family + ": " + key + " plausible UINT32 wrap " +
                str(prev) + "->" + str(val) + " in " + loc +
                " (modulo 2**32, no BOOT boundary)")
    return ("INVALID", family + ": " + key + " regression " +
            str(prev) + "->" + str(val) + " in " + loc +
            " without proven reset (definite discontinuity)")

def main():
    ap = argparse.ArgumentParser(description="P1R offline log completeness check")
    ap.add_argument("--session", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--advisory", action="store_true")
    args = ap.parse_args()
    sess = Path(args.session)
    mani = Path(args.manifest)
    findings = []
    def emit(v, t):
        findings.append(v + ": " + t)
    if not sess.is_dir():
        emit("INCOMPLETE", "log presence: session dir not found: " + str(sess))
        logs = []
    else:
        logs = sorted(sess.glob("*.log"))
        if not logs:
            emit("INCOMPLETE", "log presence: no raw *.log in session dir")
        else:
            emit("VALIDATED", "log presence: " + str(len(logs)) + " raw log(s) present")
    has_boot = has_ver = False
    boot_count = 0
    cmd_echoes = 0
    pending_cmd = None
    pending_loc = ""
    reply_gaps = 0
    truncated = 0
    unrecognized = 0
    recognized = 0
    variants = set()
    images = set()
    status_lines = 0
    status_samples = 0
    status_pres19 = 0
    health_lines = 0
    health_samples = 0
    serial_lines = 0
    serial_samples = 0
    empty_logs = []
    boot_pending = False
    prev_status = {}
    prev_health = {}
    prev_serial = {}
    for log in logs:
        try:
            raw_text = log.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as e:
            emit("INCOMPLETE", "log readability: unreadable " + log.name + ": " + str(e))
            continue
        if len(raw_text) == 0:
            empty_logs.append(log.name)
        for i, line in enumerate(raw_text):
            loc = log.name + ":" + str(i + 1)
            s = line.strip()
            if s == "":
                continue
            if BOOT_RE.search(s):
                has_boot = True
                boot_count += 1
                boot_pending = True
                recognized += 1
                m = VARIANT_RE.search(s)
                if m:
                    variants.add(m.group(1))
                pending_cmd = None
                continue
            if VER_RE.search(s):
                has_ver = True
                recognized += 1
                m = VARIANT_RE.search(s)
                if m:
                    variants.add(m.group(1))
                pending_cmd = None
                continue
            c = CMD_RE.match(s)
            if c:
                cmd_echoes += 1
                recognized += 1
                if pending_cmd is not None:
                    reply_gaps += 1
                    emit("WARNING", "reply correlation: possible missing reply before " +
                         loc + " for '" + pending_cmd + "' (heuristic only)")
                pending_cmd = c.group(2).strip()
                pending_loc = loc
                continue
            if STATUS_PREFIX.match(s):
                status_lines += 1
                recognized += 1
                pending_cmd = None
                fields = parse_fields(s)
                valid_here = 0
                for key in STATUS_COUNTERS:
                    raw = fields.get(key)
                    if raw is None:
                        continue
                    ok, val, reason = parse_counter_value(raw)
                    if not ok:
                        emit("INVALID", "STATUS counters: " + reason + " value for " +
                             key + " in " + loc + " ('" + raw + "')")
                        continue
                    valid_here += 1
                    if key in prev_status:
                        r = check_drop(prev_status[key], val, "STATUS counters",
                                       key, loc, boot_pending)
                        if r is not None:
                            emit(r[0], r[1])
                    prev_status[key] = val
                if valid_here > 0:
                    status_samples += 1
                elif s.endswith((",", "=")) or ("=" not in s):
                    truncated += 1
                    emit("WARNING", "record integrity: truncated/unparseable line in " + loc)
                else:
                    status_pres19 += 1
                boot_pending = False
                continue
            if HEALTH_PREFIX.match(s):
                health_lines += 1
                recognized += 1
                pending_cmd = None
                fields = parse_fields(s)
                m = IMAGE_RE.search(s)
                if m:
                    images.add(m.group(1))
                valid_here = 0
                for key in HEALTH_COUNTERS:
                    raw = fields.get(key)
                    if raw is None:
                        continue
                    ok, val, reason = parse_counter_value(raw)
                    if not ok:
                        emit("INVALID", "bench health counters: " + reason + " value for " +
                             key + " in " + loc + " ('" + raw + "')")
                        continue
                    valid_here += 1
                    if key in prev_health:
                        r = check_drop(prev_health[key], val, "bench health counters",
                                       key, loc, boot_pending)
                        if r is not None:
                            emit(r[0], r[1])
                    prev_health[key] = val
                if valid_here > 0:
                    health_samples += 1
                elif s.endswith((",", "=")) or ("=" not in s):
                    truncated += 1
                    emit("WARNING", "record integrity: truncated/unparseable line in " + loc)
                boot_pending = False
                continue
            if SERIAL_PREFIX.match(s):
                serial_lines += 1
                recognized += 1
                pending_cmd = None
                fields = parse_fields(s)
                valid_here = 0
                for key in SERIAL_COUNTERS:
                    raw = fields.get(key)
                    if raw is None:
                        continue
                    ok, val, reason = parse_counter_value(raw)
                    if not ok:
                        emit("INVALID", "bench serial counters: " + reason + " value for " +
                             key + " in " + loc + " ('" + raw + "')")
                        continue
                    valid_here += 1
                    if key in prev_serial:
                        r = check_drop(prev_serial[key], val, "bench serial counters",
                                       key, loc, boot_pending)
                        if r is not None:
                            emit(r[0], r[1])
                    prev_serial[key] = val
                if valid_here > 0:
                    serial_samples += 1
                elif s.endswith((",", "=")) or ("=" not in s):
                    truncated += 1
                    emit("WARNING", "record integrity: truncated/unparseable line in " + loc)
                boot_pending = False
                continue
            if s.startswith(REPLY_CLEAR):
                recognized += 1
                pending_cmd = None
                continue
            if s.startswith(("STATUS", "BENCH,", "BOOT,", "VERSION,")):
                fields = parse_fields(s)
                if not fields or s.endswith((",", "=")):
                    truncated += 1
                    emit("WARNING", "record integrity: truncated/unparseable line in " + loc)
                    pending_cmd = None
                    continue
            unrecognized += 1
        if pending_cmd is not None:
            reply_gaps += 1
            emit("WARNING", "reply correlation: trailing command with no reply in " +
                 log.name + ": '" + pending_cmd + "' at " + pending_loc + " (heuristic)")
            pending_cmd = None
    if logs:
        if has_boot:
            emit("VALIDATED", "BOOT presence: BOOT line present")
        else:
            emit("INCOMPLETE", "BOOT presence: no BOOT line in any log")
        if has_ver:
            emit("VALIDATED", "VERSION presence: VERSION line present")
        else:
            emit("INCOMPLETE", "VERSION presence: no VERSION line in any log")
    else:
        emit("NOT CHECKED", "BOOT presence: NOT CHECKED (no logs to inspect)")
        emit("NOT CHECKED", "VERSION presence: NOT CHECKED (no logs to inspect)")
    if empty_logs:
        emit("INCOMPLETE", "log integrity: empty log file(s): " + ", ".join(sorted(empty_logs)))
    if status_samples > 0:
        emit("VALIDATED", "STATUS counters: " + str(status_samples) +
             " sample(s) with S19 counters; zero values are observed zeros, not missing data")
    elif status_lines > 0:
        emit("NOT CHECKED", "STATUS counters: NOT CHECKED (" + str(status_lines) +
             " STATUS line(s) without S19 fields; pre-S19 cannot be re-analysed)")
    else:
        emit("NOT CHECKED", "STATUS counters: NOT CHECKED (no STATUS lines in input)")
    if health_samples > 0:
        emit("VALIDATED", "bench health counters: " + str(health_samples) +
             " sample(s) with responses_lost (health carries responses_lost only)")
    elif health_lines > 0:
        emit("NOT CHECKED", "bench health counters: NOT CHECKED (" + str(health_lines) +
             " bench-health line(s) without parseable responses_lost)")
    else:
        emit("NOT CHECKED", "bench health counters: NOT CHECKED (no bench-health lines)")
    if serial_samples > 0:
        emit("VALIDATED", "bench serial counters: " + str(serial_samples) +
             " sample(s) with serial transport counters")
    elif serial_lines > 0:
        emit("NOT CHECKED", "bench serial counters: NOT CHECKED (" + str(serial_lines) +
             " bench-serial line(s) without parseable counters)")
    else:
        emit("NOT CHECKED", "bench serial counters: NOT CHECKED (no bench-serial lines)")
    if status_pres19 > 0 and status_samples == 0:
        emit("NOT CHECKED", "pre-S19 compatibility: " + str(status_pres19) +
             " STATUS line(s) predate S19 fields; abandonment unsupported there")
    if boot_count > 1:
        emit("WARNING", "session boundary: " + str(boot_count) +
             " BOOT lines; counters may restart at each boundary but BOOT alone " +
             "does not prove cause (concatenated sessions or reset; separate them)")
    if len(variants) > 1:
        emit("WARNING", "firmware variants: mixed variant= " + str(sorted(variants)) +
             "; continuity across variants is ambiguous")
    if len(images) > 1:
        emit("WARNING", "bench images: mixed image= " + str(sorted(images)) +
             "; continuity across images is ambiguous")
    if cmd_echoes == 0:
        emit("NOT CHECKED", "REPLY CORRELATION: NOT CHECKED (no '> ' or 'CMD:' echoes; " +
             "serial-monitor captures carry no command chronology, so no missing-reply " +
             "claim is permitted)")
    else:
        if reply_gaps == 0:
            emit("VALIDATED", "reply correlation: " + str(cmd_echoes) +
                 " echoed command(s) each followed by a reply (heuristic; transmitted " +
                 "replies are not host-receipt proof)")
    if truncated == 0 and (recognized > 0 or unrecognized > 0):
        emit("VALIDATED", "record integrity: " + str(recognized) + " recognised, " +
             str(unrecognized) + " tolerated-unrecognised, 0 truncated")
    if not mani.is_file():
        emit("INCOMPLETE", "manifest presence: manifest not found: " + str(mani))
    else:
        try:
            body = mani.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            emit("INCOMPLETE", "manifest readability: unreadable manifest: " + str(e))
            body = None
        if body is not None:
            lines = body.splitlines()
            prov = [ln for ln in lines if re.search(r"^\s*(-\s*)?Provenance\s*:", ln, re.IGNORECASE)]
            if not prov:
                emit("INCOMPLETE", "provenance: no 'Provenance:' field (mere mention of " +
                     "PHYSICAL/SYNTHETIC elsewhere cannot establish selection)")
            else:
                pl = prov[-1]
                has_p = bool(re.search(r"\bPHYSICAL\b", pl))
                has_s = bool(re.search(r"\bSYNTHETIC\b", pl))
                if has_p and has_s:
                    emit("INCOMPLETE", "provenance: 'Provenance:' lists both PHYSICAL and " +
                         "SYNTHETIC (template text like 'PHYSICAL / SYNTHETIC (circle one)'); " +
                         "explicit single selection required")
                elif has_p != has_s:
                    chosen = "PHYSICAL" if has_p else "SYNTHETIC"
                    emit("VALIDATED", "provenance: explicit '" + chosen +
                         "' selection in 'Provenance:' field")
                else:
                    emit("INCOMPLETE", "provenance: 'Provenance:' has no explicit " +
                         "PHYSICAL or SYNTHETIC selection")
            required = [("Session ID:", r"Session ID\s*:"), ("Stage / protocol", r"Stage / protocol"),
                ("Date / location / operators", r"Date / location"), ("Firmware identity", r"Firmware identity"),
                ("git rev-parse HEAD", r"git rev-parse HEAD"), ("Image flashed", r"Image flashed"),
                ("BOOT first-line", r"BOOT.*first-line"), ("Raw logs", r"Raw logs"),
                ("Verdict", r"Verdict"), ("Signatures", r"Signatures")]
            missing_meta = []
            for label, pat in required:
                ml = [ln for ln in lines if re.search(pat, ln)]
                if not ml:
                    missing_meta.append(label + " (field absent)")
                    continue
                last = ml[-1]
                val = last.split(":", 1)[1] if ":" in last else ""
                if ("____" in last) or (val.strip() == "") or (val.strip() in ("_", "-", "______")):
                    missing_meta.append(label + " (blank)")
            if missing_meta:
                emit("INCOMPLETE", "session metadata: blank/missing: " + "; ".join(missing_meta) +
                     " (populated metadata is bookkeeping only, never qualification)")
            else:
                emit("VALIDATED", "session metadata: essential fields present with " +
                     "non-blank values (bookkeeping only, never qualification)")
            if re.search(r"\bSTOP\b", body):
                emit("WARNING", "operator stops: STOP keyword present (operator to " +
                     "disposition time + reason + rail state by hand)")
    n_invalid = sum(1 for f in findings if f.startswith("INVALID:"))
    n_incomplete = sum(1 for f in findings if f.startswith("INCOMPLETE:"))
    n_warning = sum(1 for f in findings if f.startswith("WARNING:"))
    n_valid = sum(1 for f in findings if f.startswith("VALIDATED:"))
    n_nc = sum(1 for f in findings if f.startswith("NOT CHECKED:"))
    for f in findings:
        print(f)
    print("summary: " + str(n_invalid) + " invalid, " + str(n_incomplete) + " incomplete, " +
          str(n_warning) + " warning(s), " + str(n_valid) + " validated, " + str(n_nc) +
          " not-checked; SYNTHETIC unless session was on hardware")
    print("offline evidence only: exit code is NOT safety approval, NOT delivery proof, " +
          "NOT Block E/F qualification")
    if args.advisory:
        return 0
    return 1 if (n_invalid > 0 or n_incomplete > 0) else 0

if __name__ == "__main__":
    sys.exit(main())
