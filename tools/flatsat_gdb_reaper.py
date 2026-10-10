#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Free the flatsat from GDB sessions nobody is using (DS-04).

Runs on the flatsat box, not in a Codespace. OpenOCD takes one GDB
connection at a time, so a session left open (a student who walked away, a
Codespace that stopped, a dropped network) keeps everyone else, CI included,
off the board until a mentor steps in. OpenOCD never times one out, and if
the board is halted it never writes to the connection, so it doesn't notice
the other end has gone.

A systemd timer runs this every few minutes (docs/flatsat.md). It closes
each GDB connection that hasn't sent anything for --idle-minutes, with
`ss -K`. OpenOCD treats that as GDB leaving, so its gdb-detach event starts
the board again. Each closed connection is printed, and so lands in the
journal: `journalctl -u flatsat-gdb-reaper`.

`ss -ti` already reports how long ago each connection last received data
(lastrcv, in milliseconds), so nothing needs remembering between runs.
Needs root (CAP_NET_ADMIN) to close connections; --dry-run only reports.
"""

import argparse
import re
import subprocess
import sys

GDB_PORT = 3333
IDLE_MINUTES = 60

LASTRCV = re.compile(r"\blastrcv:(\d+)\b")


def parse_ss(text):
    """(peer, idle seconds) for each connection in `ss -Htin state
    established` output.

    Each connection is two lines: the queues and addresses, then an
    indented line of details. With a state filter, ss leaves out the State
    column, so the peer is the fourth field, for example 100.97.186.84:51234
    or [fd7a:115c:a1e0::1]:51234. A connection whose details have no
    lastrcv is left out, so it's never closed by mistake.
    """
    connections = []
    peer = None
    for line in text.splitlines():
        if not line.strip():
            continue
        if not line[0].isspace():
            fields = line.split()
            peer = fields[3] if len(fields) >= 4 else None
            continue
        match = LASTRCV.search(line)
        if peer is not None and match:
            connections.append((peer, int(match.group(1)) / 1000))
        peer = None
    return connections


def idle_peers(connections, idle_minutes):
    """The peers of connections idle for at least idle_minutes."""
    limit_s = idle_minutes * 60
    return [peer for peer, idle_s in connections if idle_s >= limit_s]


def list_command(port):
    return ["ss", "-Htin", "state", "established", f"( sport = :{port} )"]


def close_command(port, peer):
    """ss -K for exactly one connection: our port, that peer's address and
    port."""
    return ["ss", "-K", "-t", "state", "established",
            f"( sport = :{port} and dst {peer} )"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=GDB_PORT,
                        help=f"OpenOCD's GDB port (default {GDB_PORT})")
    parser.add_argument("--idle-minutes", type=float, default=IDLE_MINUTES,
                        help="close connections silent this long "
                             f"(default {IDLE_MINUTES})")
    parser.add_argument("--dry-run", action="store_true",
                        help="report idle connections without closing them")
    args = parser.parse_args(argv)

    listing = subprocess.run(list_command(args.port), capture_output=True,
                             text=True)
    if listing.returncode != 0:
        print(f"ss failed: {listing.stderr.strip()}", file=sys.stderr)
        return 1

    failed = False
    for peer in idle_peers(parse_ss(listing.stdout), args.idle_minutes):
        if args.dry_run:
            print(f"would close idle GDB connection from {peer}")
            continue
        closing = subprocess.run(close_command(args.port, peer),
                                 capture_output=True, text=True)
        if closing.returncode == 0:
            print(f"closed GDB connection from {peer}: idle "
                  f"{args.idle_minutes:g} minutes or more")
        else:
            print(f"couldn't close {peer}: {closing.stderr.strip()}",
                  file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
