# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/flatsat.py. Run with `make test-python`.

A local TCP server stands in for the flatsat box's console port, and a
small script for GDB.
"""

import socket
import stat
import sys
import threading
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import flatsat  # noqa: E402


def serve(text, close=True):
    """A one-shot console server sending text. Returns its port."""
    server = socket.create_server(("127.0.0.1", 0))

    def run():
        # flatsat checks the port is reachable first, then connects to read.
        for _ in range(2):
            conn, _ = server.accept()
            conn.sendall(text)
            if close:
                conn.close()
        server.close()

    threading.Thread(target=run, daemon=True).start()
    return server.getsockname()[1]


def closed_port():
    server = socket.create_server(("127.0.0.1", 0))
    port = server.getsockname()[1]
    server.close()
    return port


# ---- Checking what the board prints -------------------------------------------

def test_watch_passes_when_every_expected_line_appears():
    lines = ["*** Booting Zephyr OS ***", "<inf> silversat: SilverSat FSW starting",
             "<inf> health: boot 1, MET 1 s"]
    printed = []
    passed, missing, forbidden = flatsat.watch(iter(lines), None,
                                               ["FSW starting", "health: boot"],
                                               ["FATAL"], out=printed.append)
    assert passed and missing == [] and forbidden == []
    assert printed == lines, "every line is printed"


def test_watch_fails_on_a_missing_line():
    passed, missing, _ = flatsat.watch(iter(["booting"]), None, ["FSW starting"], [],
                                       out=lambda line: None)
    assert not passed
    assert missing == ["FSW starting"]


def test_watch_fails_on_a_forbidden_line():
    lines = ["FSW starting", "<err> os: >>> ZEPHYR FATAL ERROR 2: Stack overflow"]
    passed, _, forbidden = flatsat.watch(iter(lines), None, ["FSW starting"], ["FATAL ERROR"],
                                         out=lambda line: None)
    assert not passed
    assert forbidden == [lines[1]]


def test_socket_lines_splits_and_finishes():
    server = socket.create_server(("127.0.0.1", 0))
    client = socket.create_connection(server.getsockname())
    conn, _ = server.accept()
    conn.sendall(b"one\r\ntwo\nthr")
    conn.sendall(b"ee")
    conn.close()
    assert list(flatsat.socket_lines(client, 2.0)) == ["one", "two", "three"]
    client.close()
    server.close()


def test_socket_lines_stops_after_its_time():
    server = socket.create_server(("127.0.0.1", 0))
    client = socket.create_connection(server.getsockname())
    conn, _ = server.accept()
    conn.sendall(b"partial line")
    assert list(flatsat.socket_lines(client, 0.3)) == ["partial line"]
    for s in (client, conn, server):
        s.close()


# ---- The console command ----------------------------------------------------------

def test_console_checks_the_board(capsys):
    port = serve(b"*** Booting ***\nSilverSat FSW starting\n")
    assert flatsat.main(["--host", "127.0.0.1", "console", "--port", str(port),
                         "--seconds", "2", "--expect", "FSW starting"]) == 0
    assert "SilverSat FSW starting" in capsys.readouterr().out


def test_console_fails_when_the_board_faults(capsys):
    port = serve(b"<err> os: ***** MPU FAULT *****\n")
    assert flatsat.main(["--host", "127.0.0.1", "console", "--port", str(port),
                         "--seconds", "2", "--forbid", "MPU FAULT"]) == 1
    assert "saw" in capsys.readouterr().err


def test_console_when_the_box_is_down(capsys):
    assert flatsat.main(["--host", "127.0.0.1", "console", "--port", str(closed_port())]) == 1
    assert "doesn't answer" in capsys.readouterr().err


def test_an_unknown_host_says_to_check_the_tailnet(capsys):
    ok, why = flatsat.reachable("no-such-host.invalid", 1)
    assert not ok
    assert "tailnet" in why


# ---- Flashing -------------------------------------------------------------------------

def test_gdb_loads_and_starts_the_board():
    commands = flatsat.gdb_commands("flatsat", 3333, "zephyr.elf")
    assert commands[0] == "-batch"
    assert commands[-1] == "zephyr.elf"
    sequence = [commands[i + 1] for i, c in enumerate(commands) if c == "-ex"]
    assert sequence == ["target extended-remote flatsat:3333", "monitor reset halt", "load",
                        "monitor reset run", "disconnect"]


def test_flash_runs_gdb(tmp_path):
    log = tmp_path / "args"
    fake_gdb = tmp_path / "gdb"
    fake_gdb.write_text(f"#!/bin/sh\necho \"$@\" > {log}\n")
    fake_gdb.chmod(fake_gdb.stat().st_mode | stat.S_IEXEC)
    port = serve(b"", close=True)
    assert flatsat.main(["--host", "127.0.0.1", "flash", "--port", str(port),
                         "--gdb", str(fake_gdb), "--elf", "zephyr.elf"]) == 0
    assert "target extended-remote 127.0.0.1" in log.read_text()


def test_flash_when_the_box_is_down(tmp_path, capsys):
    assert flatsat.main(["--host", "127.0.0.1", "flash", "--port", str(closed_port()),
                         "--gdb", "false", "--elf", "zephyr.elf"]) == 1
    assert "doesn't answer" in capsys.readouterr().err
