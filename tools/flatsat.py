#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Use the flatsat over the network: load firmware, read its console, reset it (DS-04).

The flatsat is a Nucleo-F446RE plugged into an Ubuntu box on the tailnet
(docs/flatsat.md). The box runs OpenOCD's GDB server for the Nucleo's
on-board ST-LINK, and ser2net, which puts the console (the ST-LINK's serial
port), the radio UART, and the reset controller on TCP ports. From a
Codespace:

    python3 tools/flatsat.py flash --elf build/zephyr/zephyr.elf

loads the firmware through GDB and starts it (`make flash`), and

    python3 tools/flatsat.py console

prints the console until Ctrl-C (`make console`). For CI, console can also
check what the board prints:

    python3 tools/flatsat.py console --seconds 30 \\
        --expect "SilverSat FSW starting" --forbid "FATAL ERROR"

passes only if every --expect pattern appears and no --forbid pattern does.
The radio UART is read by sim/radio_sim.py --port tcp://flatsat:4001.

    python3 tools/flatsat.py reset

pulses the board's NRST line through the flatsat controller (`make reset`),
which is how a wedged board is recovered without anyone walking over to it.
"""

import argparse
import re
import socket
import subprocess
import sys
import time

GDB_PORT = 3333
CONSOLE_PORT = 4000
RESET_PORT = 4002
CONNECT_TIMEOUT_S = 10
# The controller answers in milliseconds. If it doesn't answer in two
# seconds it isn't running the firmware.
REPLY_TIMEOUT_S = 2.0
# Long enough for the STM32 to see it (the datasheet wants microseconds),
# short enough that a test loop doesn't crawl.
HOLD_S = 0.1


class ControllerError(Exception):
    """The reset controller did not do as it was told."""


def reachable(host, port):
    """Whether the flatsat answers on this port, and if not, why."""
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S):
            return True, ""
    except socket.gaierror:
        return False, (f"can't find {host!r}: is this Codespace on the tailnet? "
                       "Run `tailscale status` (docs/flatsat.md)")
    except OSError as e:
        return False, f"{host}:{port} doesn't answer ({e}): is the flatsat box up?"


def gdb_commands(host, port, elf):
    """GDB, in batch mode: connect, stop the board, load, and start it."""
    return ["-batch",
            "-ex", f"target extended-remote {host}:{port}",
            "-ex", "monitor reset halt",
            "-ex", "load",
            "-ex", "monitor reset run",
            "-ex", "disconnect",
            elf]


def flash(gdb, host, port, elf):
    ok, why = reachable(host, port)
    if not ok:
        print(f"flatsat: {why}", file=sys.stderr)
        return 1
    result = subprocess.run([gdb] + gdb_commands(host, port, elf))
    return result.returncode


def watch(stream, seconds, expect, forbid, out=print):
    """Print lines from stream (an iterable of lines) for up to seconds,
    and check them. Returns (passed, missing expected patterns, forbidden
    lines seen). With no patterns, it just prints."""
    expect = [re.compile(p) for p in expect]
    forbid = [re.compile(p) for p in forbid]
    seen = set()
    forbidden = []
    deadline = None if seconds is None else time.monotonic() + seconds
    for line in stream:
        out(line)
        for i, pattern in enumerate(expect):
            if pattern.search(line):
                seen.add(i)
        if any(p.search(line) for p in forbid):
            forbidden.append(line)
        if deadline is not None and time.monotonic() >= deadline:
            break
    missing = [p.pattern for i, p in enumerate(expect) if i not in seen]
    return not missing and not forbidden, missing, forbidden


def socket_lines(sock, seconds):
    """Lines of text from a socket, until it closes or seconds pass."""
    deadline = None if seconds is None else time.monotonic() + seconds
    pending = b""
    if deadline is None:
        # Wait as long as it takes. The socket still has its connect
        # timeout, which would end the console after a quiet spell.
        sock.settimeout(None)
    while True:
        if deadline is not None:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            sock.settimeout(left)
        try:
            data = sock.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        pending += data
        *lines, pending = pending.split(b"\n")
        for line in lines:
            yield line.decode("utf-8", "replace").rstrip("\r")
    if pending:
        yield pending.decode("utf-8", "replace").rstrip("\r")


def console(host, port, seconds, expect, forbid):
    ok, why = reachable(host, port)
    if not ok:
        print(f"flatsat: {why}", file=sys.stderr)
        return 1
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S) as sock:
            passed, missing, forbidden = watch(socket_lines(sock, seconds), seconds,
                                               expect, forbid)
    except KeyboardInterrupt:
        return 0
    for pattern in missing:
        print(f"flatsat: never saw {pattern!r}", file=sys.stderr)
    for line in forbidden:
        print(f"flatsat: saw {line!r}", file=sys.stderr)
    return 0 if passed else 1


def command(sock, line, timeout=REPLY_TIMEOUT_S):
    """Send one command to the reset controller and return its reply.

    The controller also sends `event ...` lines nobody asked for, when a
    hold times out, say. Those are not replies: show them and keep reading.
    """
    sock.sendall(line.encode() + b"\n")
    deadline = time.monotonic() + timeout
    pending = b""
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            raise ControllerError(
                f"no reply to {line!r} in {timeout:g}s: is code.py running on "
                "the controller? (docs/flatsat.md)")
        sock.settimeout(left)
        try:
            data = sock.recv(256)
        except socket.timeout:
            continue
        if not data:
            raise ControllerError(f"the controller hung up after {line!r}")
        pending += data
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            reply = raw.decode("utf-8", "replace").strip()
            if not reply:
                continue
            if reply.startswith("event "):
                print(f"flatsat: controller {reply}", file=sys.stderr)
                continue
            return reply


def checked(sock, line):
    """Send a command, and fail loudly unless the controller accepted it."""
    reply = command(sock, line)
    if not reply.startswith("ok"):
        raise ControllerError(f"the controller refused {line!r}: {reply}")
    return reply


def reset(host, port, action, hold):
    """Drive the target's NRST line through the flatsat controller.

    The controller has no pulse command: commands set state and the caller
    composes them (DS-35), the same way the spacecraft's own commands work.
    """
    ok, why = reachable(host, port)
    if not ok:
        print(f"flatsat: {why}", file=sys.stderr)
        return 1
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S) as sock:
            if action == "status":
                print(checked(sock, "get_status"))
            elif action == "assert":
                checked(sock, "set_reset asserted")
                print("flatsat: NRST asserted, the board is held in reset")
            elif action == "release":
                checked(sock, "set_reset released")
                print("flatsat: NRST released")
            else:
                checked(sock, "set_reset asserted")
                try:
                    time.sleep(hold)
                finally:
                    # Release even on Ctrl-C. If this throws anyway, the
                    # controller's own hold timeout is the backstop.
                    checked(sock, "set_reset released")
                print(f"flatsat: reset pulse, NRST low for {hold:g}s")
    except KeyboardInterrupt:
        # The `finally` above has already released NRST.
        return 0
    except ControllerError as e:
        print(f"flatsat: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"flatsat: lost the controller on {host}:{port} ({e})", file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Use the flatsat over the network.")
    parser.add_argument("--host", default="flatsat", help="the flatsat box (default flatsat)")
    commands = parser.add_subparsers(dest="command", required=True)

    p = commands.add_parser("flash", help="load firmware through the box's GDB server")
    p.add_argument("--elf", required=True, help="the firmware, for example build/zephyr/zephyr.elf")
    p.add_argument("--gdb", default="arm-zephyr-eabi-gdb", help="the GDB to use")
    p.add_argument("--port", type=int, default=GDB_PORT, help=f"GDB port (default {GDB_PORT})")

    p = commands.add_parser("console", help="print the board's console")
    p.add_argument("--port", type=int, default=CONSOLE_PORT,
                   help=f"console port (default {CONSOLE_PORT})")
    p.add_argument("--seconds", type=float, help="stop after this long (default: Ctrl-C)")
    p.add_argument("--expect", action="append", default=[], metavar="REGEX",
                   help="fail unless a line matches (repeatable)")
    p.add_argument("--forbid", action="append", default=[], metavar="REGEX",
                   help="fail if a line matches (repeatable)")

    p = commands.add_parser("reset", help="reset the board through the flatsat controller")
    p.add_argument("action", nargs="?", default="pulse",
                   choices=["pulse", "assert", "release", "status"],
                   help="default: pulse, which asserts NRST, holds, and releases")
    p.add_argument("--hold", type=float, default=HOLD_S, metavar="SEC",
                   help=f"how long a pulse holds NRST low (default {HOLD_S:g})")
    p.add_argument("--port", type=int, default=RESET_PORT,
                   help=f"controller port (default {RESET_PORT})")

    args = parser.parse_args(argv)
    if args.command == "flash":
        return flash(args.gdb, args.host, args.port, args.elf)
    if args.command == "reset":
        return reset(args.host, args.port, args.action, args.hold)
    return console(args.host, args.port, args.seconds, args.expect, args.forbid)


if __name__ == "__main__":
    sys.exit(main())
