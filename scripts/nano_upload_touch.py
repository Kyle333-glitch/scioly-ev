"""Pre-upload 1200-bps touch for the Arduino Nano R4.

Why this exists (H0 finding F-1)
--------------------------------
`pio run -t upload` alone cannot put this board into its bootloader.
PlatformIO's `renesas-ra` builder wires its `BeforeUpload` step - the one that
performs a `use_1200bps_touch` - only into the `sam-ba` branch; the `dfu` branch
runs `dfu-util` with nothing before it. The Arduino core does implement the touch
(`cores/arduino/usb/SerialUSB.cpp`: `CheckSerialReset()` -> `goBootloader()` when
the line coding is 1200 bps and DTR is low), so a host-issued touch works: it was
used for seven of the eight uploads in the first hardware session.

This script is that touch, attached to the `upload` target, so the documented
`pio run -t upload` / `pio run -e bench -t upload` is a complete instruction on
this machine.

Why it is registered as a `post:` extra script (`platformio.ini`)
-----------------------------------------------------------------
`env.AddPreAction("upload", ...)` only reaches the upload alias if the alias
already exists: SCons resolves the name through its alias namespace and, when it
finds nothing, falls back to creating a *filesystem* node of the same name and
attaches the action there. From a `pre:` extra script the platform's builder has
not created the alias yet, so the action attaches to that stand-in node and never
runs - silently, which is the worst way for a safety-relevant convenience to
fail. Measured, not assumed: a standalone SCons probe with the same ordering
(`pre:` ordering -> the returned node is an `Entry` and the pre-action never ran;
`post:` ordering -> an `Alias`, and it ran). `_attach()` below re-checks the node
type at load time so a future change to that ordering or to SCons cannot disable
the touch without saying so.

What it does not do
-------------------
It is not a substitute for the platform's own upload flags, and it changes no
device behaviour: it only puts the board into the bootloader it already has.

What it does
------------
1. Finds the board's application-mode serial port (`$UPLOAD_PORT`, or the
   autodetected port - the board's `build.hwids` covers both the application
   (2341:0074) and the bootloader (2341:0374) USB ids).
2. Opens it at 1200 bps with DTR and RTS low and closes it. That is the touch the
   core watches for; the port then disappears as the board resets into its
   bootloader.
3. Waits for `dfu-util -l` to report a DFU-capable device, so the upload that
   follows starts against a device that is actually there.

What it deliberately does not do
--------------------------------
* It does not fail the upload. If no DFU device appears it prints what was seen
  and lets `dfu-util` give the authoritative error, because a device that appears
  a moment after this timeout is still worth uploading to.
* It does not attempt to recover the one failure this session could not explain
  (finding F-3): a board left on the bus as
  `Unknown USB Device (Device Descriptor Request Failed)` with
  `USB\\VID_0000&PID_0002\\...`, no COM port and no DFU device. No software path
  reached that board; a physical USB power cycle did. The message below says so
  rather than pretending a retry is a fix.
* It is not attached to `env:native` (host tests, no board) and it does nothing
  unless `$UPLOAD_PROTOCOL` is `dfu`, so a `sam-ba` upload keeps using the
  platform's own touch.
"""

import os
import subprocess
import time

from pathlib import Path

Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

# How long to wait for the bootloader to enumerate after a touch, and how many
# times to touch. One touch is normally enough; a second attempt costs a second
# and covers a port that was still being torn down when the first one ran.
TOUCH_ATTEMPTS = 2
DFU_WAIT_TIMEOUT_S = 10.0
DFU_POLL_INTERVAL_S = 0.25


def _dfu_util():
    """The dfu-util the platform's own uploader will use."""
    package_dir = env.PioPlatform().get_package_dir("tool-dfuutil-arduino")  # noqa: F821
    if not package_dir:
        return None
    executable = Path(package_dir) / "dfu-util"
    if executable.exists():
        return executable
    windows = executable.with_suffix(".exe")
    return windows if windows.exists() else None


def _dfu_device_present():
    """True when `dfu-util -l` reports a DFU-capable (bootloader) device.

    The application-mode device also exposes a DFU *runtime* interface, which
    `dfu-util -l` lists as `Found Runtime:`; only `Found DFU` means the
    bootloader is running and a download can start.
    """
    executable = _dfu_util()
    if executable is None:
        return False
    try:
        listed = subprocess.run(
            [str(executable), "-l"], capture_output=True, text=True, timeout=30.0
        ).stdout
    except Exception:  # a missing or non-runnable tool is not this hook's failure
        return False
    return "Found DFU" in listed


def _configured_port():
    try:
        env.AutodetectUploadPort()  # noqa: F821
    except Exception:
        return ""
    return env.subst("$UPLOAD_PORT").strip()  # noqa: F821


def _touch(port):
    """The 1200-bps touch itself. A port that vanishes here is the point."""
    try:
        import serial
    except ImportError:
        # pyserial ships with PlatformIO's own Python (`python -c "import serial"`
        # in the penv works, per the H0 tool transcript), so this means this script
        # is not running under that interpreter - not that the board is at fault.
        print(
            "nano_touch: pyserial is not importable from this environment, so the "
            "1200-bps touch cannot be issued and the upload below will most likely "
            "fail to find a device. Touch the port manually (or upload once from "
            "the Arduino IDE, which performs the touch itself), or run the upload "
            "with PlatformIO's own Python: `python -m platformio run -t upload`."
        )
        return False

    try:
        handle = serial.Serial(port, 1200)
        handle.dtr = False
        handle.rts = False
        time.sleep(0.2)
        handle.close()
    except Exception as exc:
        # Expected: the CDC port goes away while the board resets. Reported
        # because a *different* failure (a port some other program holds) looks
        # like this too, and that one is worth seeing.
        print("nano_touch: %s closed during the touch: %s" % (port, exc))
    return True


def _wait_for_bootloader():
    deadline = time.monotonic() + DFU_WAIT_TIMEOUT_S
    while time.monotonic() < deadline:
        if _dfu_device_present():
            return True
        time.sleep(DFU_POLL_INTERVAL_S)
    return False


def before_upload(target, source, env):  # noqa: ARG001 - SCons action signature
    """The pre-action on the `upload` target. Never raises: `dfu-util` is the judge."""
    if env.subst("$UPLOAD_PROTOCOL") != "dfu":
        return

    if _dfu_device_present():
        print("nano_touch: bootloader already present; no touch needed")
        return

    port = _configured_port()
    if not port:
        print(
            "nano_touch: no application-mode serial port found to touch (no "
            "$UPLOAD_PORT and nothing autodetected). If the board is already in its "
            "bootloader this is expected; otherwise check the cable, the port, and "
            "that no other program is holding it."
        )
        return

    for attempt in range(1, TOUCH_ATTEMPTS + 1):
        print(
            "nano_touch: touching %s at 1200 bps (attempt %d/%d)"
            % (port, attempt, TOUCH_ATTEMPTS)
        )
        if not _touch(port):
            return
        if _wait_for_bootloader():
            print("nano_touch: bootloader present; uploading")
            return
        # A retry is worth one try: the first touch can be missed while the host is
        # still tearing the port down. It retries the same port name rather than
        # searching for another one - if the port is gone, `_touch` says so, and
        # guessing a different port would be inventing information. This is not a
        # fix for anything - see the message below.

    print(
        "nano_touch: no DFU device appeared after %d touch attempt(s); continuing "
        "anyway so dfu-util reports the authoritative error.\n"
        "  If no VID_2341 device is present at all, the board may be in the state "
        "the first hardware session could not recover from by software (finding "
        "F-3): 'Unknown USB Device (Device Descriptor Request Failed)', "
        "USB\\VID_0000&PID_0002\\..., no COM port, dfu-util -l listing nothing. "
        "A physical USB power cycle recovered that board; no software path did.\n"
        "  If a VID_2341&PID_0374 device is present but dfu-util cannot use it, the "
        "bootloader driver is missing instead (finding F-2): run the signed "
        "driver package under the installed framework (renesas.inf / dpinst)."
        % TOUCH_ATTEMPTS
    )


def _attach():
    """Registers the pre-action, and says so loudly if it did not land."""
    # The second argument is only the label the upload prints; without it SCons
    # prints the function's own repr, which reads like an error to an operator.
    action = env.Action(before_upload, "Touch the port for the bootloader")  # noqa: F821
    nodes = env.AddPreAction("upload", action)  # noqa: F821
    # The node type is the check that matters: an `Alias` is the upload target
    # the platform declared, and a plain `Entry` is the stand-in node described
    # above, whose pre-action is never executed.
    if nodes and os.environ.get("NANO_TOUCH_REPORT"):
        print("nano_touch: attached the touch to a %s target" % type(nodes[0]).__name__)
    if not nodes or type(nodes[0]).__name__ != "Alias":
        print(
            "nano_touch: WARNING - the 1200-bps touch could not be attached to the "
            "upload target (got %s for 'upload'). The touch will NOT run, so the "
            "upload may fail with 'No DFU capable USB device available'. This "
            "script must be a `post:` extra script, so that the upload target "
            "already exists when it runs."
            % ([type(node).__name__ for node in nodes] or "no node",)
        )
        return


_attach()
