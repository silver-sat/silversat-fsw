#!/usr/bin/env python3
"""
Smoke-test the SilverSat flatsat NRST controller.

Speaks the command vocabulary from the flatsat controller specification, so
this same script works against the CircuitPython prototype and against the
student-built board that replaces it later.

    python3 nrst_test.py                  # auto-detect the data port
    python3 nrst_test.py /dev/ttyACM1     # or name it explicitly
    python3 nrst_test.py --hold 10        # hold reset longer, to measure NRST
    python3 nrst_test.py --list           # just show candidate ports

Exits 0 if every check passed, 1 otherwise, so it can gate a CI job.
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

ADAFRUIT_VID = 0x239A
BAUD = 115200          # ignored over USB CDC, but pyserial wants a number
REPLY_TIMEOUT = 2.0


class ControllerError(Exception):
    """Anything that stops us talking to the controller."""


def candidate_ports():
    """CircuitPython boards, sorted so the order is stable."""
    return sorted(
        (p for p in list_ports.comports() if p.vid == ADAFRUIT_VID),
        key=lambda p: p.device,
    )


def find_data_port():
    """CircuitPython puts the console on USB interface 0 and the data port on
    interface 2, so the data port sorts second on both macOS and Linux."""
    ports = candidate_ports()
    if len(ports) == 2:
        return ports[1].device
    if not ports:
        raise ControllerError(
            "no CircuitPython board found (looked for USB VID 0x%04X).\n"
            "Is it plugged in? Did boot.py enable the data port, and has the "
            "board been hard-reset since?" % ADAFRUIT_VID
        )
    raise ControllerError(
        "expected 2 CircuitPython ports, found %d:\n  %s\n"
        "Pass the data port explicitly."
        % (len(ports), "\n  ".join(p.device for p in ports))
    )


def command(port, text, timeout=REPLY_TIMEOUT):
    """Send one line, return one line of reply."""
    port.reset_input_buffer()
    port.write((text + "\n").encode())
    port.flush()

    deadline = time.monotonic() + timeout
    line = bytearray()
    while time.monotonic() < deadline:
        char = port.read(1)
        if not char:
            continue
        if char == b"\n":
            return line.decode("utf-8", "replace").strip()
        line += char
    raise ControllerError(
        "no reply to %r within %.1fs - wrong port, or code.py is not running "
        "(check the console port for a traceback)" % (text, timeout)
    )


def check(port, text, expected="ok"):
    """Run one command, report pass/fail, return the verdict."""
    reply = command(port, text)
    passed = reply.startswith(expected)
    print("  %-22s -> %-26s %s" % (text, reply, "PASS" if passed else "FAIL"))
    if not passed:
        print("  %-22s    expected a reply starting %r" % ("", expected))
    return passed


def run_checks(port, hold):
    results = []

    results.append(check(port, "version"))

    # Establish a known state before testing transitions.
    results.append(check(port, "restore"))
    results.append(check(port, "get_status", "ok reset=released"))

    results.append(check(port, "set_reset asserted"))
    results.append(check(port, "get_status", "ok reset=asserted"))
    print("  holding for %.1fs - NRST should read 0 V, target held in reset" % hold)
    time.sleep(hold)

    results.append(check(port, "set_reset released"))
    results.append(check(port, "get_status", "ok reset=released"))
    print("  released - NRST should read 3.3 V, target should boot")

    # A malformed command must be rejected, not tidied up.
    results.append(check(port, "nonsense", "err"))

    return results


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("device", nargs="?",
                        help="serial device; omit to auto-detect")
    parser.add_argument("--hold", type=float, default=2.0, metavar="SEC",
                        help="seconds to hold reset asserted (default: 2)")
    parser.add_argument("--list", action="store_true",
                        help="list candidate ports and exit")
    args = parser.parse_args()

    if args.list:
        ports = candidate_ports()
        if not ports:
            print("no CircuitPython boards found")
            return 1
        for i, p in enumerate(ports):
            role = {0: "console", 1: "data"}.get(i, "?")
            print("%-24s %-8s %s" % (p.device, role, p.description))
        return 0

    try:
        device = args.device or find_data_port()
    except ControllerError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    print("using %s\n" % device)

    try:
        with serial.Serial(device, BAUD, timeout=0.1) as port:
            time.sleep(0.2)          # let the port settle after opening
            results = run_checks(port, args.hold)
    except serial.SerialException as exc:
        print("error: could not open %s: %s" % (device, exc), file=sys.stderr)
        if "Permission denied" in str(exc):
            print("hint: you are probably not in the device's group. Try:\n"
                  "  ls -l %s\n"
                  "  sudo usermod -aG dialout $USER\n"
                  "  then log out and back in, or run: newgrp dialout"
                  % device, file=sys.stderr)
        else:
            print("hint: the device path may have changed; try --list",
                  file=sys.stderr)
        return 1    except ControllerError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    passed, total = sum(results), len(results)
    print("\n%d/%d checks passed" % (passed, total))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())