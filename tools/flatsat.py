#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Use the flatsat over the network: load firmware, and read its console (DS-04).

The flatsat is a Nucleo-F446RE plugged into an Ubuntu box on the tailnet
(docs/flatsat.md). The box runs OpenOCD's GDB server for the Nucleo's
on-board ST-LINK, and ser2net, which puts the console (the ST-LINK's serial
port) and the radio UART on TCP ports. From a Codespace:

    python3 tools/flatsat.py flash --elf build/zephyr/zephyr.elf

loads the firmware through GDB and starts it (`make flash`), and

    python3 tools/flatsat.py console

prints the console until Ctrl-C (`make console`). For CI, console can also
check what the board prints:

    python3 tools/flatsat.py console --seconds 30 \\
        --expect "SilverSat FSW starting" --forbid "FATAL ERROR"

passes only if every --expect pattern appears and no --forbid pattern does.
The radio UART is read by sim/radio_sim.py --port tcp://flatsat:4001.
"""

import argparse
import re
import socket
import subprocess
import sys
import time

GDB_PORT = 3333
CONSOLE_PORT = 4000
CONNECT_TIMEOUT_S = 10


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

    args = parser.parse_args(argv)
    if args.command == "flash":
        return flash(args.gdb, args.host, args.port, args.elf)
    return console(args.host, args.port, args.seconds, args.expect, args.forbid)


if __name__ == "__main__":
    sys.exit(main())
