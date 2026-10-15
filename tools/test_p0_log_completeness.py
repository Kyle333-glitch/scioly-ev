#!/usr/bin/env python3
"""P1R adversarial regression tests for tools/p0_log_completeness.py.

Offline only. SYNTHETIC fixtures in temp dirs; never touches data/evidence/
(read-only historical evidence), never opens serial, never modifies inputs.
Standard library only (unittest, subprocess, tempfile, hashlib).
Run: python -m unittest tools.test_p0_log_completeness -v
     python tools/test_p0_log_completeness.py
"""
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "p0_log_completeness.py"
BLANK_TEMPLATE = REPO / "docs" / "bringup" / "P0-first-session-manifest-blank.md"

BOOT_PROD = "BOOT,fw=ev-nano-r4,rev=abc123,dirty=0,variant=production,cfg=deadbeef,cfg_schema=1"
VER_PROD = "VERSION,fw=ev-nano-r4,rev=abc123,dirty=0,variant=production,cfg=deadbeef,cfg_schema=1"
BOOT_BENCH = "BOOT,fw=ev-nano-r4,rev=abc123,dirty=0,variant=bench,cfg=deadbeef,cfg_schema=1"
VER_BENCH = "VERSION,fw=ev-nano-r4,rev=abc123,dirty=0,variant=bench,cfg=deadbeef,cfg_schema=1"

def status_line(accepted=10, rejected=0, lost=0, aband=0, ref=0, over=0, sent=50, staged=50):
    return ("STATUS,phase=Idle,armed=0,cmd_accepted=%d,cmd_rejected=%d,responses_lost=%s,"
            "serial_abandoned=%s,serial_refused=%d,serial_oversize=%d,serial_sent=%d,serial_staged=%d"
            % (accepted, rejected, lost, aband, ref, over, sent, staged))

def health_line(lost=0, acc=5, ref=0, fwd=0):
    return ("BENCH,health=1,image=bench,enable=0,lines_accepted=%d,lines_refused=%d,"
            "lines_forwarded=%d,responses_lost=%d" % (acc, ref, fwd, lost))

def serial_line(lost=0, aband=0, ref=0, over=0, sent=50, staged=50):
    return ("BENCH,serial=1,responses_lost=%d,serial_abandoned=%d,serial_refused=%d,"
            "serial_oversize=%d,serial_sent=%d,serial_staged=%d"
            % (lost, aband, ref, over, sent, staged))

def populated_manifest(provenance):
    assert provenance in ("PHYSICAL", "SYNTHETIC")
    gen = "generator: p1r-synthetic-fixture" if provenance == "SYNTHETIC" else "generator: n/a (hardware session)"
    return "\n".join([
        "# SYNTHETIC test manifest (not a measurement)",
        "- Session ID: P1R-TEST-001",
        "- Stage / protocol + revision: P-H0 @ abc123",
        "- Date / location / operators: 2026-10-15 / lab / tester",
        "- Firmware identity (`VERSION` verbatim): `VERSION,fw=ev-nano-r4,rev=abc123,dirty=0,variant=production,cfg=deadbeef,cfg_schema=1`",
        "- `git rev-parse HEAD`: abc123 ; `dirty=` 0 ; `variant=` production ; `cfg=` deadbeef",
        "- Image flashed (`nano_r4` / `bench` / `bench-watchdog`): nano_r4 ; upload log (`Download done.` bytes): Download done. 79904 bytes",
        "- `BOOT` first-line present? Y ; attach time: 2026-10-15T00:00:00Z ; host/reader: test-reader",
        "- Hardware config: pack V n/a-synthetic ; limit n/a ; motor connected? N ; wheels loaded? N ; disconnect device: n/a",
        "- Raw logs (relative paths + SHA256): a.log <synthetic>",
        "- Command chronology (cmd -> reply -> counters delta): n/a synthetic",
        "- Replies missing (count + which cmds): 0 ; `responses_lost` start/end: 0/0 ; `serial_abandoned` start/end: 0/0",
        "- `STATUS`/`bench health` samples (start/end verbatim): synthetic",
        "- Encoder evidence (ticks start/end, sign, repeats): n/a synthetic",
        "- Trigger levels (idle/active V): n/a synthetic",
        "- Watchdog / reset events (resetsrc, gaps): none synthetic",
        "- Operator events (time + reason + rail state after): none synthetic",
        "- Unexpected resets (time + last lines before): none synthetic",
        "- Timestamps + wrap note (`t_ms` start/end, rollover? Y/N): 0/1000, rollover? N",
        "- Provenance: " + provenance + " ; if synthetic, " + gen,
        "- Incomplete-session? N ; missing items: none",
        "- Verdict (protocol acceptance rule, quoted): synthetic pass",
        "- Signatures: tester Date: 2026-10-15",
        "",
    ])

def run_validator(session_dir, manifest_path, extra=()):
    cmd = [sys.executable, str(TOOL), "--session", str(session_dir), "--manifest", str(manifest_path)] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=str(REPO))
    return p.returncode, p.stdout + p.stderr

class P1RValidatorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.sess = self.root / "sess"
        self.sess.mkdir()
        self.mani_phys = self.root / "manifest-phys.md"
        self.mani_phys.write_text(populated_manifest("PHYSICAL"), encoding="utf-8")
        self.mani_synth = self.root / "manifest-synth.md"
        self.mani_synth.write_text(populated_manifest("SYNTHETIC"), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def write_log(self, name, lines):
        p = self.sess / name
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return p

    # 1. valid production STATUS
    def test01_valid_production_status(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(lost=0, aband=0),
                                 status_line(accepted=11, lost=0, aband=0, sent=51, staged=51)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertIn("VALIDATED: STATUS counters:", out)
        self.assertNotIn("INVALID:", out)
        self.assertNotIn("INCOMPLETE:", out)

    # 2. valid bench health
    def test02_valid_bench_health(self):
        self.write_log("b.log", [BOOT_BENCH, VER_BENCH, health_line(lost=0), health_line(lost=0, acc=6)])
        rc, out = run_validator(self.sess, self.mani_synth)
        self.assertEqual(rc, 0, out)
        self.assertIn("VALIDATED: bench health counters:", out)
        self.assertNotIn("INVALID:", out)

    # 3. valid bench serial
    def test03_valid_bench_serial(self):
        self.write_log("c.log", [BOOT_BENCH, VER_BENCH, serial_line(), serial_line(sent=51, staged=51)])
        rc, out = run_validator(self.sess, self.mani_synth)
        self.assertEqual(rc, 0, out)
        self.assertIn("VALIDATED: bench serial counters:", out)

    # 4. normal increasing counters
    def test04_increasing_counters(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(lost=1, aband=0, sent=10, staged=10),
                                 status_line(accepted=11, lost=2, aband=1, sent=11, staged=11)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("INVALID:", out)

    # 5. equal repeated counters
    def test05_equal_counters(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(lost=3, aband=1),
                                 status_line(accepted=11, lost=3, aband=1, sent=51, staged=51)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("INVALID:", out)

    # 6. genuine regression without proven reset
    def test06_genuine_regression(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(lost=5, aband=2),
                                 status_line(accepted=11, lost=1, aband=2, sent=51, staged=51)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 1, out)
        self.assertIn("INVALID: STATUS counters: responses_lost regression 5->1", out)

    # 7. valid UINT32 wrap
    def test07_uint32_wrap(self):
        self.write_log("w.log", [BOOT_PROD, VER_PROD,
                                 status_line(lost=4294967295, aband=4294967295, sent=4294967295, staged=4294967295),
                                 status_line(accepted=11, lost=0, aband=0, sent=0, staged=0)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertNotIn("INVALID:", out, out)
        self.assertIn("plausible UINT32 wrap", out)

    # 8. ambiguous BOOT discontinuity
    def test08_boot_discontinuity(self):
        self.write_log("r.log", [BOOT_PROD, VER_PROD,
                                 status_line(lost=9, aband=4, sent=200, staged=200),
                                 BOOT_PROD,
                                 status_line(accepted=11, lost=0, aband=0, sent=5, staged=5)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertNotIn("INVALID:", out, out)
        self.assertIn("after BOOT boundary", out)
        self.assertIn("session boundary", out)

    # 9. missing counter family -> NOT CHECKED
    def test09_missing_counter_family(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertIn("NOT CHECKED: bench serial counters", out)
        self.assertIn("NOT CHECKED: bench health counters", out)

    # 10. malformed counter -> INVALID
    def test10_malformed_counter(self):
        self.write_log("m.log", [BOOT_PROD, VER_PROD,
                                 "STATUS,phase=Idle,armed=0,responses_lost=abc,serial_abandoned=2"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 1, out)
        self.assertIn("INVALID: STATUS counters: malformed", out)

    # 11. overflowing counter -> INVALID
    def test11_overflow_counter(self):
        self.write_log("m.log", [BOOT_PROD, VER_PROD,
                                 "STATUS,phase=Idle,armed=0,responses_lost=99999999999999999999,serial_abandoned=0"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 1, out)
        self.assertIn("INVALID: STATUS counters: out-of-range", out)

    # 12. truncated line -> WARNING
    def test12_truncated_line(self):
        self.write_log("t.log", [BOOT_PROD, VER_PROD,
                                 "STATUS,phase=Idle,armed=0,",
                                 status_line()])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("WARNING: record integrity: truncated", out)
    # 13. unrecognized record tolerated
    def test13_unrecognized_record(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(),
                                 "SOME_FUTURE_LINE,foo=1",
                                 "EVS1,ver=1,t_ms=20,note=old-telemetry"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertIn("tolerated-unrecognised", out)

    # 14. pre-S19 log -> NOT CHECKED
    def test14_pres19_log(self):
        self.write_log("old.log", [BOOT_PROD, VER_PROD,
                                   "STATUS,phase=Idle,armed=0,pos_m=0.0000,motor=Disabled"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("NOT CHECKED: STATUS counters", out)
        self.assertIn("pre-S19", out)

    # 15. empty log -> INCOMPLETE
    def test15_empty_log(self):
        (self.sess / "empty.log").write_text("", encoding="utf-8")
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: log integrity: empty log", out)

    # 16. missing log (no *.log) -> INCOMPLETE
    def test16_missing_log(self):
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: log presence: no raw *.log", out)

    # 17. missing manifest -> INCOMPLETE
    def test17_missing_manifest(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, self.root / "does-not-exist.md")
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: manifest presence: manifest not found", out)

    # 18. blank manifest template rejected
    def test18_blank_manifest(self):
        self.assertTrue(BLANK_TEMPLATE.is_file(), "blank template missing")
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, BLANK_TEMPLATE)
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: provenance:", out)
    # 19. populated physical manifest accepted
    def test19_physical_manifest(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc, 0, out)
        self.assertIn("VALIDATED: provenance: explicit 'PHYSICAL'", out)

    # 20. populated synthetic manifest accepted
    def test20_synthetic_manifest(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, self.mani_synth)
        self.assertEqual(rc, 0, out)
        self.assertIn("VALIDATED: provenance: explicit 'SYNTHETIC'", out)

    # 21. instructional words only -> INCOMPLETE
    def test21_instructional_words_only(self):
        m = self.root / "instr.md"
        m.write_text("# notes\nSynthetic vs physical provenance must be marked.\n"
                     "- Session ID: P1R-TEST-001\n", encoding="utf-8")
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, m)
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: provenance:", out)

    # 22. partial metadata -> INCOMPLETE
    def test22_partial_metadata(self):
        m = self.root / "partial.md"
        full = populated_manifest("PHYSICAL")
        partial = full.replace("- Session ID: P1R-TEST-001",
                               "- Session ID: ____________________")
        m.write_text(partial, encoding="utf-8")
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc, out = run_validator(self.sess, m)
        self.assertEqual(rc, 1, out)
        self.assertIn("INCOMPLETE: session metadata:", out)

    # 23. missing command echo -> NOT CHECKED
    def test23_missing_command_echo(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(),
                                 "OK,cmd=status", "EVS3,ver=3,note=telemetry"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("NOT CHECKED: REPLY CORRELATION: NOT CHECKED", out)
        self.assertNotIn("INVALID: STATUS", out)

    # 24. multiple sessions concatenated -> WARNING
    def test24_multiple_sessions(self):
        self.write_log("m.log", [BOOT_PROD, VER_PROD, status_line(sent=100, staged=100),
                                 BOOT_PROD, VER_PROD, status_line(sent=5, staged=5)])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("WARNING: session boundary:", out)
        self.assertNotIn("INVALID:", out, out)
    # 25. mixed firmware variants -> WARNING
    def test25_mixed_variants(self):
        self.write_log("m.log", [BOOT_PROD, VER_PROD, status_line(),
                                 BOOT_BENCH, VER_BENCH, status_line()])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("WARNING: firmware variants: mixed", out)

    # 26. read-only input preservation
    def test26_readonly_preservation(self):
        lp = self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        before = [(p.name, hashlib.sha256(p.read_bytes()).hexdigest())
                  for p in sorted(self.sess.glob("*.log"))]
        mh = hashlib.sha256(self.mani_phys.read_bytes()).hexdigest()
        run_validator(self.sess, self.mani_phys)
        after = [(p.name, hashlib.sha256(p.read_bytes()).hexdigest())
                 for p in sorted(self.sess.glob("*.log"))]
        self.assertEqual(before, after)
        self.assertEqual(mh, hashlib.sha256(self.mani_phys.read_bytes()).hexdigest())
        self.assertTrue(lp.is_file())

    # 27. deterministic output
    def test27_deterministic_output(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line(),
                                 health_line(), serial_line()])
        rc1, out1 = run_validator(self.sess, self.mani_phys)
        rc2, out2 = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc1, rc2)
        self.assertEqual(out1, out2)

    # 28. exit semantics incl. bad invocation
    def test28_exit_semantics(self):
        self.write_log("a.log", [BOOT_PROD, VER_PROD, status_line()])
        rc_ok, _ = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc_ok, 0)
        self.write_log("bad.log", [BOOT_PROD, VER_PROD,
                                   status_line(lost=5), status_line(lost=1)])
        rc_bad, _ = run_validator(self.sess, self.mani_phys)
        self.assertEqual(rc_bad, 1)
        p = subprocess.run([sys.executable, str(TOOL), "--session", str(self.sess)],
                           capture_output=True, text=True, cwd=str(REPO))
        self.assertEqual(p.returncode, 2)

    # bench-health regression now detected (PHFR D-item)
    def test29_bench_health_regression_detected(self):
        self.write_log("b.log", [BOOT_BENCH, VER_BENCH,
                                 health_line(lost=7), health_line(lost=3, acc=6)])
        rc, out = run_validator(self.sess, self.mani_synth)
        self.assertEqual(rc, 1, out)
        self.assertIn("INVALID: bench health counters: responses_lost regression 7->3", out)

    # echo-format reply gap is heuristic WARNING, not INVALID
    def test30_echo_gap_heuristic(self):
        self.write_log("c.log", [BOOT_PROD, VER_PROD,
                                 "> status", "> health",
                                 "OK,cmd=status"])
        rc, out = run_validator(self.sess, self.mani_phys)
        self.assertIn("WARNING: reply correlation:", out)
        self.assertNotIn("INVALID:", out, out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
