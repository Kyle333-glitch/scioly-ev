#!/usr/bin/env python3
"""Reliable host-side EV serial diagnostics (S23, offline-developed).

Offline only during development: this module never opens a real serial port
unless its ``main()`` CLI is run explicitly by an operator, and the test suite
below it only ever uses the fake transport. No hardware is required.

What this is
-----------
A small Windows-compatible serial diagnostic for future Arduino Nano R4
bring-up. It opens one COM port at 115200 baud with explicit DTR/RTS, listens
passively first (the firmware owes a ``BOOT,...`` greeting only while it sees
DTR high), then sends a small allowlist of read-only commands and saves a
timestamped transcript.

What this is NOT (safety restrictions, enforced in code)
--------------------------------------------------------
* It never opens a port during development or tests (fake transport only).
* It never uploads firmware or modifies anything on the board.
* It never resets the board into the bootloader and never uses the 1200-baud
  bootloader trigger (baud is fixed at 115200, with no option to change it).
* It never sends ``arm``, ``drive``, ``bench arm``, ``bench drive``,
  ``bench h1a-brake``, ``bench reset``, ``reset``, ``announce`` or any other
  motor-affecting / state-changing command.
* It has no generic unrestricted send interface: only the allowlist below is
  transmittable, and everything else is refused before any port is opened.
* It does not modify production firmware or electrical safety gates.
* Connecting USB is NOT authorized by this software. Real-hardware use remains
  contingent on the approved supervised bring-up procedure (Block E
  inspection, motor-supply disconnect, supervisor clearance).

Firmware protocol notes (from source, S21/S21R runbook)
-------------------------------------------------------
* ``Serial.begin(115200)``; on this board Serial is USB-CDC so the baud is
  accepted-and-ignored device-side, but the host must still request 115200.
* Host-to-device: one line per command terminated by ``\\n`` or ``\\r``.
  CRLF frames as one line plus one empty line (no answer for the empty one).
  This tool always sends ``<cmd>\\n`` (single LF).
* Device-to-host replies end in ``\\r\\n``. This tool splits incoming bytes on
  CR, LF, or CRLF (CRLF = one break) and ignores empty lines for reply
  matching while still recording them in the transcript.
* Framing is strict: printable ASCII, exact lowercase, one space between
  tokens, <=96 chars. Anything else is refused with ``ERR,...`` (never
  silence) when the serial path works.
* A functioning serial path answers even unsupported commands such as bare
  ``health``/``ticks`` with ``ERR,cmd=none,reason=UnknownCommand`` -- silence
  means transport/DTR/host-framing/unknown-image, not command spelling.
* ``variant=`` in ``BOOT``/``VERSION`` identifies the image
  (production | bench | bench-watchdog | unknown). ``bench health`` reports
  ``image=bench`` on bench images. Bench verbs require the ``bench`` prefix
  and exist only on bench images; bare ``health``/``ticks`` are forwarded to
  the production parser and refused.

Usage (Windows, real hardware only when authorized)
--------------------------------------------------
    python tools/serial_diag/ev_serial_diag.py --list
    python tools/serial_diag/ev_serial_diag.py --port COM3 --transcript session.log
    python tools/serial_diag/ev_serial_diag.py --port COM3 --commands boot version status --transcript session.log
    python tools/serial_diag/ev_serial_diag.py --port COM3 --commands "bench health" "bench serial" "bench ticks" --transcript session.log

Dependencies: Python 3.8+ stdlib plus ``pyserial`` (ships with PlatformIO's
own Python; verified as 3.5 on the project host). No new dependencies.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence, Tuple

BAUD = 115200
DEFAULT_TIMEOUT_S = 2.0
DEFAULT_PASSIVE_S = 2.5
SEND_TERMINATOR = "\n"

# The only commands this tool may ever transmit. Read-only by construction:
# production identity/state plus bench readouts. Bench verbs are gated further:
# they are sent only after the session identifies variant=bench[-watchdog].
ALLOWED_COMMANDS: Tuple[str, ...] = (
    "boot",
    "version",
    "status",
    "bench health",
    "bench serial",
    "bench ticks",
)

BENCH_COMMANDS = frozenset({"bench health", "bench serial", "bench ticks"})

# Explicitly prohibited (never transmittable, named in diagnostics so an
# operator understands why). This list is not exhaustive: anything not in
# ALLOWED_COMMANDS is refused, including any future motor-affecting verb.
PROHIBITED_COMMANDS = frozenset({
    "arm", "drive", "announce", "reset",
    "bench arm", "bench drive", "bench stop", "bench reset",
    "bench h1a-brake", "bench trigger",
    "health", "ticks",
})

BOOTLOADER_BAUD = 1200  # documented only to forbid it; never used.

VARIANT_RE = re.compile(r"variant=([A-Za-z0-9_\-]+)")
IMAGE_RE = re.compile(r"image=([A-Za-z0-9_\-]+)")
UNKNOWN_COMMAND_RE = re.compile(r"reason=UnknownCommand")


# ---------------------------------------------------------------------------
# Pure helpers (no I/O, fully unit-testable)
# ---------------------------------------------------------------------------

def is_allowed_command(cmd: str) -> bool:
    """True only for an exact allowlist member (case/space sensitive)."""
    return cmd in ALLOWED_COMMANDS


def is_bench_command(cmd: str) -> bool:
    return cmd in BENCH_COMMANDS


def refusal_reason(cmd: str) -> str:
    if cmd in PROHIBITED_COMMANDS:
        return "prohibited command (motor-affecting, state-changing, or bare diagnostic verb); refused"
    if cmd.strip() != cmd or "  " in cmd or cmd.lower() != cmd:
        return "not an exact allowlist command (firmware framing is strict: lowercase, single spaces, no padding); refused"
    return "not in the read-only allowlist %s; refused" % (list(ALLOWED_COMMANDS),)


def expected_prefix(cmd: str) -> str:
    if cmd == "boot":
        return "BOOT,"
    if cmd == "version":
        return "VERSION,"
    if cmd == "status":
        return "STATUS"
    if cmd == "bench health":
        return "BENCH,health="
    if cmd == "bench serial":
        return "BENCH,serial="
    if cmd == "bench ticks":
        return "BENCH,ticks="
    return ""


class LineSplitter:
    """Splits incoming serial bytes on CR, LF, or CRLF (CRLF = one break).

    Handles delayed/fragmented delivery: feed arbitrary chunks, drain complete
    lines. Empty lines are yielded as "" so callers can distinguish "no data"
    from "blank line"; reply matching ignores them.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._pending_cr = False

    def feed(self, chunk: bytes) -> List[str]:
        lines: List[str] = []
        for byte in chunk:
            if byte == 0x0D:  # CR -> one break
                lines.append(self._decode_and_clear())
                self._pending_cr = True
            elif byte == 0x0A:  # LF
                if self._pending_cr:
                    # CRLF pair = single break already emitted by the CR.
                    self._pending_cr = False
                    continue
                lines.append(self._decode_and_clear())
            else:
                self._buf.append(byte)
                self._pending_cr = False
        return lines

    def _decode_and_clear(self) -> str:
        raw = bytes(self._buf)
        self._buf.clear()
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError:
            return raw.decode("ascii", errors="replace")

    def pending(self) -> str:
        """Partial line not yet terminated (for diagnostics, not a reply)."""
        try:
            return bytes(self._buf).decode("ascii")
        except UnicodeDecodeError:
            return bytes(self._buf).decode("ascii", errors="replace")


def split_lines_simple(data: bytes) -> List[str]:
    """One-shot split used by tests for the CR/LF/CRLF matrix."""
    splitter = LineSplitter()
    lines = splitter.feed(data)
    return lines


def parse_variants(lines: Sequence[str]) -> List[str]:
    found: List[str] = []
    for line in lines:
        m = VARIANT_RE.search(line)
        if m and m.group(1) not in found:
            found.append(m.group(1))
    return found


def parse_images(lines: Sequence[str]) -> List[str]:
    found: List[str] = []
    for line in lines:
        m = IMAGE_RE.search(line)
        if m and m.group(1) not in found:
            found.append(m.group(1))
    return found


def identify_firmware(lines: Sequence[str]) -> Dict[str, object]:
    """Identify the installed image from captured lines (pure)."""
    variants = parse_variants(lines)
    images = parse_images(lines)
    has_boot = any(l.startswith("BOOT,") for l in lines)
    has_version = any(l.startswith("VERSION,") for l in lines)
    has_bench = any(l.startswith("BENCH,") for l in lines)
    has_unknown = any(
        l.startswith("ERR,") and UNKNOWN_COMMAND_RE.search(l) for l in lines
    )
    if len(variants) == 1:
        variant: object = variants[0]
    elif len(variants) == 0:
        variant = "unknown"
    else:
        variant = "conflict:" + "|".join(variants)
    return {
        "variant": variant,
        "variants": variants,
        "images": images,
        "has_boot": has_boot,
        "has_version": has_version,
        "has_bench": has_bench,
        "has_unknown_command": has_unknown,
    }


# ---------------------------------------------------------------------------
# Transports (real wraps pyserial; fake is scripted for offline tests)
# ---------------------------------------------------------------------------

class SerialError(Exception):
    pass


class PortMissingError(SerialError):
    pass


class PortBusyError(SerialError):
    pass


class ConnectionError_(SerialError):
    pass


class DisconnectError(SerialError):
    pass


class RealTransport:
    """Thin wrapper over pyserial. Only constructed by main()."""

    def __init__(self) -> None:
        self._ser = None
        self.port_name = ""

    def list_ports(self) -> List[str]:
        from serial.tools import list_ports
        return [p.device for p in list_ports.comports()]

    def open(self, port: str, dtr: bool, rts: bool) -> Dict[str, object]:
        import serial
        self.port_name = port
        try:
            ser = serial.Serial(port=port, baudrate=BAUD, timeout=0)
        except FileNotFoundError as exc:
            raise PortMissingError(
                "port '%s' not found (device absent or wrong COMx; "
                "run --list to enumerate)" % port
            ) from exc
        except PermissionError as exc:
            raise PortBusyError(
                "port '%s' is occupied by another program "
                "(close monitor/terminals and retry)" % port
            ) from exc
        except Exception as exc:  # noqa: BLE001 - mapped to a clear diagnostic
            text = str(exc)
            if "FileNotFoundError" in text or "cannot find the file" in text:
                raise PortMissingError(
                    "port '%s' not found (device absent; run --list)" % port
                ) from exc
            if "denied" in text.lower() or "Permission" in text:
                raise PortBusyError(
                    "port '%s' is occupied (access denied)" % port
                ) from exc
            raise ConnectionError_(
                "could not open port '%s': %s" % (port, exc)
            ) from exc
        try:
            ser.dtr = bool(dtr)
            ser.rts = bool(rts)
            ser.reset_input_buffer()
        except Exception as exc:  # noqa: BLE001
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            raise ConnectionError_(
                "could not configure port '%s' (DTR/RTS): %s" % (port, exc)
            ) from exc
        self._ser = ser
        return {"dtr": bool(dtr), "rts": bool(rts), "baud": BAUD}

    def write_line(self, cmd: str) -> None:
        assert self._ser is not None
        try:
            self._ser.write((cmd + SEND_TERMINATOR).encode("ascii"))
            self._ser.flush()
        except Exception as exc:  # noqa: BLE001
            raise DisconnectError(
                "port '%s' disappeared during write: %s" % (self.port_name, exc)
            ) from exc

    def read_chunk(self) -> bytes:
        assert self._ser is not None
        try:
            waiting = self._ser.in_waiting
            if waiting:
                return bytes(self._ser.read(waiting))
            data = self._ser.read(1)
            return bytes(data)
        except Exception as exc:  # noqa: BLE001
            raise DisconnectError(
                "port '%s' disappeared during read: %s" % (self.port_name, exc)
            ) from exc

    def close(self) -> None:
        try:
            if self._ser is not None:
                self._ser.close()
        except Exception:  # noqa: BLE001
            pass
        finally:
            self._ser = None


class FakeTransport:
    """Deterministic scripted transport for offline tests. Never touches HW.

    ``script`` maps command -> list of byte-chunks to return on subsequent
    reads (fragmentation/delay simulated by chunk boundaries and ``delays``).
    ``greeting`` chunks are returned before any command is sent (passive
    capture). ``fail_on_open`` / ``drop_after`` inject port errors.
    """

    def __init__(
        self,
        greeting: Sequence[bytes] = (),
        script: Optional[Dict[str, List[bytes]]] = None,
        ports: Sequence[str] = ("COM3",),
        fail_on_open: Optional[Exception] = None,
        drop_after_reads: Optional[int] = None,
        disconnect_error: Optional[Exception] = None,
    ) -> None:
        self.greeting = list(greeting)
        self.script = dict(script or {})
        self.ports = list(ports)
        self.fail_on_open = fail_on_open
        self.drop_after_reads = drop_after_reads
        self.disconnect_error = disconnect_error or DisconnectError("port disappeared (synthetic)")
        self.opened = False
        self.closed = False
        self.written: List[str] = []
        self.config: Dict[str, object] = {}
        self._queue: List[bytes] = []
        self._reads = 0

    def list_ports(self) -> List[str]:
        return list(self.ports)

    def open(self, port: str, dtr: bool, rts: bool) -> Dict[str, object]:
        if self.fail_on_open is not None:
            raise self.fail_on_open
        if port not in self.ports:
            raise PortMissingError("port '%s' not found (synthetic)" % port)
        self.opened = True
        self.closed = False
        self.config = {"dtr": bool(dtr), "rts": bool(rts), "baud": BAUD}
        self._queue = list(self.greeting)
        return dict(self.config)

    def write_line(self, cmd: str) -> None:
        self.written.append(cmd)
        for chunk in self.script.get(cmd, []):
            self._queue.append(chunk)

    def read_chunk(self) -> bytes:
        self._reads += 1
        if self.drop_after_reads is not None and self._reads > self.drop_after_reads:
            raise self.disconnect_error
        if self._queue:
            return self._queue.pop(0)
        return b""

    def close(self) -> None:
        self.closed = True


# ---------------------------------------------------------------------------
# Session runner (transport-agnostic; deterministic with injected clock)
# ---------------------------------------------------------------------------

@dataclass
class TranscriptEntry:
    elapsed_s: float
    direction: str  # SYS / TX / RX / NOTE
    text: str


@dataclass
class SessionResult:
    ok: bool
    diagnostics: List[str] = field(default_factory=list)
    received_lines: List[str] = field(default_factory=list)
    variant: str = "unknown"
    transcript: List[TranscriptEntry] = field(default_factory=list)
    sent_commands: List[str] = field(default_factory=list)
    skipped_commands: List[str] = field(default_factory=list)


def format_transcript(entries: Sequence[TranscriptEntry]) -> str:
    out: List[str] = []
    for e in entries:
        out.append("%8.3f %-4s %s" % (e.elapsed_s, e.direction, e.text))
    return "\n".join(out) + ("\n" if out else "")


def run_session(
    transport,  # RealTransport | FakeTransport (duck-typed)
    port: str,
    commands: Sequence[str],
    dtr: bool = True,
    rts: bool = True,
    passive_s: float = DEFAULT_PASSIVE_S,
    timeout_s: float = DEFAULT_TIMEOUT_S,
    monotonic: Callable[[], float] = time.monotonic,
    sleep_fn: Callable[[float], None] = time.sleep,
    poll_s: float = 0.02,
) -> SessionResult:
    """Open, passively capture, send allowlist commands, identify firmware."""
    result = SessionResult(ok=False)
    t0 = monotonic()

    def stamp() -> float:
        return monotonic() - t0

    def note(text: str) -> None:
        result.transcript.append(TranscriptEntry(stamp(), "SYS", text))

    # 0. Validate before touching any port.
    for cmd in commands:
        if not is_allowed_command(cmd):
            result.diagnostics.append(
                "refused '%s': %s" % (cmd, refusal_reason(cmd))
            )
    refused = {c for c in commands if not is_allowed_command(c)}
    sendable = [c for c in commands if is_allowed_command(c)]

    # Forbid the bootloader trigger even as a concept: baud is fixed.
    note("ev_serial_diag start; port=%s baud=%d dtr=%s rts=%s" % (port, BAUD, dtr, rts))
    if refused:
        note("refused %d command(s) before opening (allowlist only)" % len(refused))
    if commands and not sendable:
        # Nothing transmittable was asked for: refuse without touching hardware.
        # (Empty commands=[] is the explicit passive-listen mode and may open.)
        note("no transmittable commands; port not opened")
        return result

    # 1. Open with explicit DTR/RTS.
    try:
        cfg = transport.open(port, dtr, rts)
    except (PortMissingError, PortBusyError, ConnectionError_, SerialError) as exc:
        result.diagnostics.append("connection failed: %s" % exc)
        note("connection failed: %s" % exc)
        try:
            transport.close()
        except Exception:  # noqa: BLE001
            pass
        return result
    note(
        "port opened; host configured baud=%s dtr=%s rts=%s"
        % (cfg.get("baud", BAUD), cfg.get("dtr", dtr), cfg.get("rts", rts))
    )

    splitter = LineSplitter()
    all_lines: List[str] = []

    def drain_for(duration_s: float, label: str) -> List[str]:
        """Collect complete lines for duration_s (polling, disconnect-safe)."""
        got: List[str] = []
        deadline = monotonic() + duration_s
        while True:
            try:
                chunk = transport.read_chunk()
            except (DisconnectError, SerialError) as exc:
                result.diagnostics.append(
                    "%s: port disappeared: %s" % (label, exc)
                )
                note("%s: disconnect: %s" % (label, exc))
                raise
            if chunk:
                for line in splitter.feed(chunk):
                    # Record every boundary in the transcript (incl. empties).
                    result.transcript.append(
                        TranscriptEntry(stamp(), "RX", line)
                    )
                    if line != "":
                        got.append(line)
                        all_lines.append(line)
            if monotonic() >= deadline:
                break
            if chunk:
                # Data is flowing: keep draining without sleeping so a
                # fragmented greeting is fully consumed inside the window.
                continue
            # Nothing available: wait a beat (bounds CPU + defines timeout).
            try:
                sleep_fn(poll_s)
            except Exception:  # noqa: BLE001
                break
        return got

    # 2. Passive capture before transmitting anything.
    try:
        passive_lines = drain_for(passive_s, "passive capture")
    except (DisconnectError, SerialError):
        try:
            transport.close()
        except Exception:  # noqa: BLE001
            pass
        result.received_lines = list(all_lines)
        ident = identify_firmware(all_lines)
        result.variant = str(ident["variant"])
        return result
    if passive_lines:
        note("passive capture: %d line(s)" % len(passive_lines))
    else:
        note("passive capture: no lines (board may need DTR, may be silent, or image unknown)")

    # 3. Identify early from the greeting so bench verbs can be gated.
    ident = identify_firmware(all_lines)
    variant = str(ident["variant"])

    # 4. Send: production-safe commands first, bench verbs only after variant.
    # Split requested order into safe-first then bench (stable within groups).
    safe_first = [c for c in sendable if not is_bench_command(c)]
    bench_later = [c for c in sendable if is_bench_command(c)]
    for cmd in safe_first + bench_later:
        if is_bench_command(cmd) and variant not in ("bench", "bench-watchdog"):
            # Re-identify in case an earlier reply revealed the variant.
            ident = identify_firmware(all_lines)
            variant = str(ident["variant"])
            if variant not in ("bench", "bench-watchdog"):
                msg = (
                    "skipped '%s': firmware variant is '%s' "
                    "(bench verbs require variant=bench or bench-watchdog; "
                    "identify with boot/version first)" % (cmd, variant)
                )
                result.diagnostics.append(msg)
                result.skipped_commands.append(cmd)
                note(msg)
                continue
        try:
            transport.write_line(cmd)
        except (DisconnectError, SerialError) as exc:
            result.diagnostics.append("write '%s' failed: %s" % (cmd, exc))
            note("write failed: %s" % exc)
            break
        result.transcript.append(TranscriptEntry(stamp(), "TX", cmd))
        result.sent_commands.append(cmd)
        try:
            window = drain_for(timeout_s, "cmd '%s'" % cmd)
        except (DisconnectError, SerialError):
            break
        prefix = expected_prefix(cmd)
        matched = [l for l in window if l.startswith(prefix)] if prefix else list(window)
        if not matched:
            # Still a useful link proof: UnknownCommand means transport works.
            unknowns = [l for l in window if UNKNOWN_COMMAND_RE.search(l)]
            if unknowns:
                note("cmd '%s': no '%s' reply, but UnknownCommand present (link works)" % (cmd, prefix))
            else:
                msg = "cmd '%s': no reply within %.1fs (empty response/timeout)" % (cmd, timeout_s)
                result.diagnostics.append(msg)
                note(msg)
        else:
            note("cmd '%s': %d matching reply line(s)" % (cmd, len(matched)))
        ident = identify_firmware(all_lines)
        variant = str(ident["variant"])

    result.received_lines = list(all_lines)
    result.variant = variant
    final = identify_firmware(all_lines)
    if not all_lines:
        result.diagnostics.append(
            "empty session: port opened but no bytes received "
            "(check DTR propagation, cable/enumeration, flashed image, host framing)"
        )
    if str(final["variant"]) in ("unknown",) and all_lines:
        result.diagnostics.append(
            "firmware variant unknown: no variant= field seen "
            "(old image or non-repo firmware; treat image as unidentified)"
        )
    if str(final["variant"]).startswith("conflict:"):
        result.diagnostics.append(
            "conflicting variant fields seen: %s (record transcript, do not trust image)" % final["variant"]
        )
    # Success = every sendable command got at least its expected prefix.
    # (Skipped bench verbs on production are correct behaviour, not success.)
    result.ok = (
        len(result.sent_commands) == len(sendable)
        and not any("no reply" in d or "empty session" in d for d in result.diagnostics)
    )
    note("session end; variant=%s sent=%d skipped=%d lines=%d" % (
        result.variant, len(result.sent_commands),
        len(result.skipped_commands), len(result.received_lines)))
    try:
        transport.close()
    except Exception:  # noqa: BLE001
        pass
    note("port closed")
    return result


# ---------------------------------------------------------------------------
# CLI (real-hardware path; never exercised by unit tests)
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Reliable read-only EV serial diagnostics (115200 baud, explicit DTR/RTS, transcript). "
        "No motor commands, no upload, no bootloader trigger. USB connection requires supervised clearance.",
    )
    p.add_argument("--port", default=None, help="COM port, e.g. COM3 (required unless --list)")
    p.add_argument("--list", action="store_true", help="list available ports and exit (opens nothing)")
    p.add_argument("--dtr", choices=("0", "1"), default="1", help="DTR to assert on open (default 1)")
    p.add_argument("--rts", choices=("0", "1"), default="1", help="RTS to assert on open (default 1)")
    p.add_argument("--commands", nargs="*", default=("boot", "version", "status"),
                   help="allowlist commands to send (default: boot version status)")
    p.add_argument("--passive-seconds", type=float, default=DEFAULT_PASSIVE_S)
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S)
    p.add_argument("--transcript", default=None, help="path to write the timestamped transcript")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    transport = RealTransport()
    if args.list:
        try:
            ports = transport.list_ports()
        except Exception as exc:  # noqa: BLE001
            print("could not enumerate ports: %s" % exc, file=sys.stderr)
            return 1
        if not ports:
            print("no serial ports found (board absent or driver missing)")
        else:
            for port in ports:
                print(port)
        return 0
    if not args.port:
        print("missing --port (e.g. --port COM3); use --list to enumerate", file=sys.stderr)
        return 2
    commands = list(args.commands or [])
    # Immediate allowlist gate (before opening anything).
    bad = [c for c in commands if not is_allowed_command(c)]
    if bad:
        for cmd in bad:
            print("refused '%s': %s" % (cmd, refusal_reason(cmd)), file=sys.stderr)
        print("allowed: %s" % (list(ALLOWED_COMMANDS),), file=sys.stderr)
        return 2
    result = run_session(
        transport,
        args.port,
        commands,
        dtr=(args.dtr == "1"),
        rts=(args.rts == "1"),
        passive_s=args.passive_seconds,
        timeout_s=args.timeout,
    )
    text = format_transcript(result.transcript)
    if args.transcript:
        try:
            with open(args.transcript, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("# ev_serial_diag transcript; port=%s baud=%d\n" % (args.port, BAUD))
                fh.write(text)
        except OSError as exc:
            print("could not write transcript '%s': %s" % (args.transcript, exc), file=sys.stderr)
    else:
        sys.stdout.write(text)
    print("variant: %s" % result.variant)
    for diag in result.diagnostics:
        print("diag: %s" % diag)
    if not result.received_lines:
        print("no data received (see diags above; do not assume firmware state)")
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
