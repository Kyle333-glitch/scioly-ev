#!/usr/bin/env python3
"""S23 offline tests for tools/serial_diag/ev_serial_diag.py.

Offline only: SYNTHETIC fixtures, fake transport, never opens a COM port,
never touches hardware. Standard library only (unittest).

Run from repo root:
    python -m unittest tools.serial_diag.tests.test_ev_serial_diag -v

Covers Phase 4:
 1. successful connection + firmware identification
 2. production response interpretation (+ bench gating)
 3. bench response interpretation
 4. CR / LF / CRLF framing
 5. delayed / fragmented replies
 6. no response despite opened port
 7. port disappearing mid-session
 8. unexpected / malformed responses
 9. transcript preservation
 10. prohibited-command rejection (incl. no port opened)
 11. deterministic timeouts + cleanup
"""

import unittest

from tools.serial_diag import ev_serial_diag as diag


# ---------------------------------------------------------------------------
# Explicitly synthetic fixtures shaped by firmware source (S21 runbook +
# VehicleRuntime::stageBootLine + FirmwareIdentity fragment). Not measurements.
# ---------------------------------------------------------------------------

BOOT_PROD = (
    "BOOT,fw=ev-nano-r4,rev=104f181,dirty=0,variant=production,"
    "cfg=deadbeef,cfg_schema=1,schema=3,cadence_ms=20,watchdog=0,"
    "checks_all=0,regulator=0,armed=0,telemetry=1,stride=1,ready=1"
)
VERSION_PROD = (
    "VERSION,fw=ev-nano-r4,rev=104f181,dirty=0,variant=production,"
    "cfg=deadbeef,cfg_schema=1"
)
STATUS_PROD = (
    "STATUS,phase=Idle,armed=0,cmd_accepted=3,cmd_rejected=0,"
    "responses_lost=0,serial_abandoned=0,serial_refused=0,"
    "serial_oversize=0,serial_sent=10,serial_staged=10"
)
BOOT_BENCH = BOOT_PROD.replace("variant=production", "variant=bench")
BOOT_WDT = BOOT_PROD.replace("variant=production", "variant=bench-watchdog")
VERSION_BENCH = VERSION_PROD.replace("variant=production", "variant=bench")
HEALTH_BENCH = (
    "BENCH,health=1,image=bench,enable=0,open_attempted=0,opened=0,"
    "lines_accepted=5,lines_refused=0,lines_forwarded=2,responses_lost=0"
)
SERIAL_BENCH = (
    "BENCH,serial=1,responses_lost=0,serial_abandoned=0,serial_refused=0,"
    "serial_oversize=0,serial_sent=20,serial_staged=20"
)
TICKS_BENCH = "BENCH,ticks=1,raw=1,ticks_raw=1494,counted_edges=1494,ignored_updates=3"
UNKNOWN_CMD = "ERR,cmd=none,reason=UnknownCommand"


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def run(port_ok=True, **kwargs):
    """Run a session with a deterministic clock; returns (result, transport, clock)."""
    clock = FakeClock()
    kwargs.setdefault("passive_s", 0.2)
    kwargs.setdefault("timeout_s", 0.2)
    result = diag.run_session(
        monotonic=clock.monotonic,
        sleep_fn=clock.sleep,
        poll_s=0.05,
        **kwargs,
    )
    return result, clock


class SuccessfulConnectionTest(unittest.TestCase):
    def test_production_identification(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={
                "boot": [(BOOT_PROD + "\r\n").encode("ascii")],
                "version": [(VERSION_PROD + "\r\n").encode("ascii")],
                "status": [(STATUS_PROD + "\r\n").encode("ascii")],
            },
        )
        result, _ = run(transport=t, port="COM3",
                        commands=["boot", "version", "status"])
        self.assertTrue(t.opened)
        self.assertEqual(t.config, {"dtr": True, "rts": True, "baud": 115200})
        self.assertEqual(result.variant, "production")
        self.assertEqual(result.sent_commands, ["boot", "version", "status"])
        self.assertTrue(result.ok)
        self.assertTrue(t.closed)
        self.assertIn(BOOT_PROD, result.received_lines)
        self.assertIn(VERSION_PROD, result.received_lines)


class ProductionInterpretationTest(unittest.TestCase):
    def test_bench_verbs_gated_on_production(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={
                "boot": [(BOOT_PROD + "\r\n").encode("ascii")],
                "version": [(VERSION_PROD + "\r\n").encode("ascii")],
            },
        )
        result, _ = run(transport=t, port="COM3",
                        commands=["boot", "version", "bench health", "bench ticks"])
        self.assertEqual(result.variant, "production")
        # Safe commands sent; bench verbs skipped, never written.
        self.assertEqual(result.sent_commands, ["boot", "version"])
        self.assertEqual(result.skipped_commands, ["bench health", "bench ticks"])
        self.assertNotIn("bench health", t.written)
        self.assertNotIn("bench ticks", t.written)
        self.assertTrue(any("bench" in d and "production" in d for d in result.diagnostics))

    def test_unknown_command_means_link_works(self):
        ident = diag.identify_firmware([UNKNOWN_CMD])
        self.assertTrue(ident["has_unknown_command"])
        # UnknownCommand with no variant => variant stays unknown, not a guess.
        self.assertEqual(ident["variant"], "unknown")


class BenchInterpretationTest(unittest.TestCase):
    def test_bench_variant_allows_bench_verbs(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_BENCH + "\r\n").encode("ascii")],
            script={
                "boot": [(BOOT_BENCH + "\r\n").encode("ascii")],
                "bench health": [(HEALTH_BENCH + "\r\n").encode("ascii")],
                "bench serial": [(SERIAL_BENCH + "\r\n").encode("ascii")],
                "bench ticks": [(TICKS_BENCH + "\r\n").encode("ascii")],
            },
        )
        result, _ = run(
            transport=t, port="COM3",
            commands=["boot", "bench health", "bench serial", "bench ticks"],
        )
        self.assertEqual(result.variant, "bench")
        self.assertEqual(
            result.sent_commands,
            ["boot", "bench health", "bench serial", "bench ticks"],
        )
        self.assertTrue(result.ok)
        ident = diag.identify_firmware(result.received_lines)
        self.assertIn("bench", ident["images"])

    def test_watchdog_variant_allows_bench_verbs(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_WDT + "\r\n").encode("ascii")],
            script={"bench ticks": [(TICKS_BENCH + "\r\n").encode("ascii")]},
        )
        result, _ = run(transport=t, port="COM3", commands=["bench ticks"])
        self.assertEqual(result.variant, "bench-watchdog")
        self.assertEqual(result.sent_commands, ["bench ticks"])
        self.assertTrue(result.ok)


class LineEndingTest(unittest.TestCase):
    def test_lf_cr_crlf_all_frame(self):
        for ending in (b"\n", b"\r", b"\r\n"):
            lines = diag.split_lines_simple(b"BOOT,x=1" + ending + b"VERSION,y=2" + ending)
            self.assertEqual(lines, ["BOOT,x=1", "VERSION,y=2"], msg=repr(ending))

    def test_greeting_with_lone_cr_is_parsed(self):
        t = diag.FakeTransport(
            greeting=[b"BOOT,fw=ev-nano-r4,rev=abc,dirty=0,variant=production,cfg=00\r"
                      b"VERSION,fw=ev-nano-r4,rev=abc,dirty=0,variant=production,cfg=00\r"],
            script={},
        )
        result, _ = run(transport=t, port="COM3", commands=[])
        self.assertEqual(result.variant, "production")

    def test_crlf_greeting_is_one_line_each(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={},
        )
        result, _ = run(transport=t, port="COM3", commands=[])
        self.assertIn(BOOT_PROD, result.received_lines)
        # No phantom empty reply lines recorded as received.
        self.assertNotIn("", result.received_lines)


class FragmentationTest(unittest.TestCase):
    def test_fragmented_reply_reassembled(self):
        full = (VERSION_PROD + "\r\n").encode("ascii")
        chunks = [full[:5], full[5:12], full[12:]]
        t = diag.FakeTransport(
            greeting=[],
            script={"version": chunks},
        )
        result, _ = run(transport=t, port="COM3", commands=["version"])
        self.assertIn(VERSION_PROD, result.received_lines)
        self.assertTrue(result.ok)

    def test_byte_at_a_time_still_frames(self):
        raw = (BOOT_PROD + "\r\n").encode("ascii")
        t = diag.FakeTransport(
            greeting=[bytes([b]) for b in raw],
            script={},
        )
        result, _ = run(transport=t, port="COM3", commands=[])
        self.assertIn(BOOT_PROD, result.received_lines)


class NoResponseTest(unittest.TestCase):
    def test_open_port_silence_reports_empty(self):
        t = diag.FakeTransport(greeting=[], script={})
        result, clock = run(transport=t, port="COM3",
                            commands=["boot", "version"])
        self.assertTrue(t.opened)
        self.assertFalse(result.ok)
        joined = "\n".join(result.diagnostics)
        self.assertIn("no reply", joined)
        self.assertIn("empty session", joined)
        self.assertTrue(t.closed)
        # Bounded: passive (0.2) + 2 commands x 0.2 + polling slack.
        self.assertLess(clock.now, 3.0)


class DisconnectTest(unittest.TestCase):
    def test_port_disappearing_mid_session(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={"boot": [(BOOT_PROD + "\r\n").encode("ascii")]},
            drop_after_reads=3,
        )
        result, _ = run(transport=t, port="COM3",
                        commands=["boot", "version", "status"])
        joined = "\n".join(result.diagnostics)
        self.assertIn("disappeared", joined)
        self.assertTrue(t.closed)
        # Transcript preserved despite the drop.
        text = diag.format_transcript(result.transcript)
        self.assertIn("RX", text)

    def test_missing_port_diagnostic(self):
        t = diag.FakeTransport(ports=("COM4",))
        result, _ = run(transport=t, port="COM3", commands=["boot"])
        self.assertFalse(t.opened)
        self.assertIn("not found", "\n".join(result.diagnostics))

    def test_busy_port_diagnostic(self):
        t = diag.FakeTransport(
            ports=("COM3",),
            fail_on_open=diag.PortBusyError("port 'COM3' is occupied (synthetic)"),
        )
        result, _ = run(transport=t, port="COM3", commands=["boot"])
        self.assertIn("occupied", "\n".join(result.diagnostics))


class MalformedTest(unittest.TestCase):
    def test_garbage_does_not_break_identification(self):
        t = diag.FakeTransport(
            greeting=[b"GARBAGE \x01\x02\r\n" + (BOOT_PROD + "\r\n").encode("ascii")],
            script={"version": [b"VER" + b"SION???" + b"\r\n"]},
        )
        result, _ = run(transport=t, port="COM3", commands=["version"])
        self.assertEqual(result.variant, "production")
        text = diag.format_transcript(result.transcript)
        self.assertIn("GARBAGE", text)

    def test_conflicting_variants_flagged(self):
        ident = diag.identify_firmware([
            BOOT_PROD,
            BOOT_BENCH,
        ])
        self.assertTrue(str(ident["variant"]).startswith("conflict:"))
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii"),
                      (BOOT_BENCH + "\r\n").encode("ascii")],
            script={},
        )
        result, _ = run(transport=t, port="COM3", commands=[])
        self.assertTrue(any("conflicting variant" in d for d in result.diagnostics))

    def test_legacy_line_without_variant_is_unknown(self):
        legacy = "BOOT,fw=ev-nano-r4-p7b,schema=1,cadence_ms=20,watchdog=0,ready=1"
        ident = diag.identify_firmware([legacy])
        self.assertEqual(ident["variant"], "unknown")
        self.assertTrue(ident["has_boot"])


class TranscriptTest(unittest.TestCase):
    def test_transcript_preserves_order_and_directions(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={"boot": [(BOOT_PROD + "\r\n").encode("ascii")]},
        )
        result, _ = run(transport=t, port="COM3", commands=["boot"])
        text = diag.format_transcript(result.transcript)
        self.assertIn("TX", text)
        self.assertIn("RX", text)
        self.assertIn("boot", text)
        self.assertIn(BOOT_PROD, text)
        # Timestamps monotonic in output order.
        stamps = [float(line.split()[0]) for line in text.splitlines()]
        self.assertEqual(stamps, sorted(stamps))


class ProhibitedCommandTest(unittest.TestCase):
    PROHIBITED = [
        "arm", "drive", "announce", "reset",
        "bench arm", "bench drive", "bench stop", "bench reset",
        "bench h1a-brake", "bench drive fwd 10 100",
        "health", "ticks",
        "config set distance=7.5", "telemetry on",
        "Boot", " boot", "boot ", "bench  health",
        "boot; reset",
    ]

    def test_each_prohibited_refused_without_opening(self):
        for cmd in self.PROHIBITED:
            with self.subTest(cmd=cmd):
                self.assertFalse(diag.is_allowed_command(cmd), cmd)
                t = diag.FakeTransport(
                    greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
                    script={"boot": [(BOOT_PROD + "\r\n").encode("ascii")]},
                )
                result, _ = run(transport=t, port="COM3", commands=[cmd])
                self.assertFalse(t.opened, cmd)
                self.assertEqual(t.written, [], cmd)
                self.assertTrue(any("refused" in d for d in result.diagnostics), cmd)

    def test_no_bootloader_baud_path(self):
        self.assertEqual(diag.BAUD, 115200)
        self.assertNotEqual(diag.BAUD, diag.BOOTLOADER_BAUD)

    def test_mixed_good_and_bad_sends_only_good(self):
        t = diag.FakeTransport(
            greeting=[(BOOT_PROD + "\r\n").encode("ascii")],
            script={"boot": [(BOOT_PROD + "\r\n").encode("ascii")]},
        )
        result, _ = run(transport=t, port="COM3", commands=["boot", "arm"])
        self.assertTrue(t.opened)
        self.assertEqual(t.written, ["boot"])
        self.assertTrue(any("refused 'arm'" in d for d in result.diagnostics))


class TimeoutCleanupTest(unittest.TestCase):
    def test_timeouts_are_bounded_and_port_closed(self):
        t = diag.FakeTransport(greeting=[], script={})
        result, clock = run(transport=t, port="COM3",
                            commands=["boot", "version", "status"],
                            passive_s=0.2, timeout_s=0.2)
        self.assertTrue(t.closed)
        self.assertFalse(result.ok)
        # 0.2 passive + 3 x 0.2 windows + slack < 3 s.
        self.assertLess(clock.now, 3.0)

    def test_dtr_rts_reported(self):
        t = diag.FakeTransport(greeting=[], script={})
        result, _ = run(transport=t, port="COM3", commands=[],
                        dtr=True, rts=True)
        text = diag.format_transcript(result.transcript)
        self.assertIn("dtr=True", text)
        self.assertIn("rts=True", text)
        self.assertIn("port closed", text)


if __name__ == "__main__":
    unittest.main()
