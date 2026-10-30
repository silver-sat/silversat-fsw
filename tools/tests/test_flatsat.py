# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/flatsat.py. Run with `make test-python`.

A local TCP server stands in for the flatsat box's console port, and a
small script for GDB.
"""

import socket
import stat
import sys
import threading
import time
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


def controller(reply=lambda command: b"ok", unprompted=b""):
    """A stand-in for the reset controller on its ser2net port.

    Returns (port, log); log fills with the commands it was sent, in order.
    `unprompted` is sent on connect, for the lines the controller emits
    that nobody asked for.
    """
    log = []
    server = socket.create_server(("127.0.0.1", 0))

    def run():
        # reset() checks the port is reachable, then connects to talk.
        server.accept()[0].close()
        conn, _ = server.accept()
        if unprompted:
            conn.sendall(unprompted)
        pending = b""
        try:
            while True:
                data = conn.recv(256)
                if not data:
                    break
                pending += data
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    log.append(raw.decode().strip())
                    conn.sendall(reply(log[-1]) + b"\n")
        except OSError:
            pass
        finally:
            conn.close()
            server.close()

    threading.Thread(target=run, daemon=True).start()
    return server.getsockname()[1], log


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


def test_socket_lines_without_a_time_waits_through_quiet_spells():
    # The board can be quiet for longer than the connect timeout, for example
    # in deploy mode. Without --seconds, the console waits for it.
    server = socket.create_server(("127.0.0.1", 0))
    client = socket.create_connection(server.getsockname(), timeout=0.1)
    conn, _ = server.accept()

    def send_late():
        time.sleep(0.4)
        conn.sendall(b"after a quiet spell\n")
        conn.close()

    threading.Thread(target=send_late, daemon=True).start()
    assert list(flatsat.socket_lines(client, None)) == ["after a quiet spell"]
    client.close()
    server.close()


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


# ---- Resetting the board ---------------------------------------------------------------

def run_reset(port, *args):
    return flatsat.main(["--host", "127.0.0.1", "reset", "--port", str(port), *args])


def test_reset_pulses_nrst():
    # The controller has no pulse command: setting state twice is the pulse.
    port, log = controller()
    assert run_reset(port, "--hold", "0.05") == 0
    assert log == ["set_reset asserted", "set_reset released"]


def test_reset_can_assert_without_releasing():
    # For holding a board down while something else is rewired.
    port, log = controller()
    assert run_reset(port, "assert") == 0
    assert log == ["set_reset asserted"]


def test_reset_can_release_on_its_own():
    port, log = controller()
    assert run_reset(port, "release") == 0
    assert log == ["set_reset released"]


def test_reset_reports_status(capsys):
    port, log = controller(reply=lambda command: b"ok reset=released")
    assert run_reset(port, "status") == 0
    assert log == ["get_status"]
    assert "reset=released" in capsys.readouterr().out


def test_reset_fails_when_the_controller_refuses(capsys):
    port, _ = controller(reply=lambda command: b"err syntax")
    assert run_reset(port) == 1
    assert "refused" in capsys.readouterr().err


def test_reset_ignores_lines_nobody_asked_for(capsys):
    # A hold that timed out earlier announces itself whenever we next
    # connect. It is not the reply to our command.
    port, log = controller(unprompted=b"event reset_timeout\n")
    assert run_reset(port, "--hold", "0.05") == 0
    assert log == ["set_reset asserted", "set_reset released"]
    assert "reset_timeout" in capsys.readouterr().err


def test_reset_releases_nrst_even_when_interrupted(monkeypatch):
    # Ctrl-C during the hold must not leave the board stuck in reset.
    port, log = controller()
    monkeypatch.setattr(flatsat.time, "sleep", _raise_keyboard_interrupt)
    assert run_reset(port) == 0
    assert log == ["set_reset asserted", "set_reset released"]


def test_reset_gives_up_when_the_controller_is_silent():
    # ser2net answers, but nothing is running on the QT Py behind it.
    server = socket.create_server(("127.0.0.1", 0))
    with socket.create_connection(server.getsockname()) as sock:
        conn, _ = server.accept()
        with pytest.raises(flatsat.ControllerError, match="code.py"):
            flatsat.command(sock, "version", timeout=0.2)
        conn.close()
    server.close()


def test_reset_when_the_box_is_down(capsys):
    assert run_reset(closed_port()) == 1
    assert "doesn't answer" in capsys.readouterr().err


def _raise_keyboard_interrupt(_seconds):
    raise KeyboardInterrupt


# ---- Reading the boot log ------------------------------------------------------

def test_reset_cause_names_reads_zephyrs_flags():
    # An STM32 power-up sets pin, brownout and power-on together.
    assert flatsat.reset_cause_names(0b1101) == "pin, brownout, power-on"
    assert flatsat.reset_cause_names(1 << 4) == "watchdog"
    assert flatsat.reset_cause_names(0) == "none"
    assert flatsat.reset_cause_names((1 << 20) | 1) == "pin, 0x100000"


def test_bootlog_dumps_the_whole_mirror_and_lets_the_board_go():
    commands = flatsat.bootlog_gdb_commands("flatsat", 3333, "m.bin", 4096)
    sequence = [commands[i + 1] for i, c in enumerate(commands) if c == "-ex"]
    assert sequence == ["target extended-remote flatsat:3333",
                        "dump binary memory m.bin 0x40024000 0x40025000",
                        "detach"]


def fake_gdb_writing(tmp_path, image):
    """A stand-in GDB that writes image wherever it's told to dump."""
    source = tmp_path / "image.bin"
    source.write_bytes(image)
    gdb = tmp_path / "gdb"
    gdb.write_text(f"""#!{sys.executable}
import shutil, sys
for arg in sys.argv[1:]:
    if arg.startswith("dump binary memory "):
        shutil.copy({str(source)!r}, arg.split()[3])
""")
    gdb.chmod(gdb.stat().st_mode | stat.S_IEXEC)
    return gdb


def mirror_image(records):
    import nvm_dump
    import telemetry
    return nvm_dump.build_image(telemetry.load_dictionary(), records, "mirror")


def run_bootlog(gdb):
    port = serve(b"", close=True)
    return flatsat.main(["--host", "127.0.0.1", "bootlog", "--port", str(port),
                         "--gdb", str(gdb)])


def test_bootlog_prints_each_boot_and_why_it_happened(tmp_path, capsys):
    image = mirror_image({
        "run_checkpoint": {"boot_number": 30, "met_ms": 487000, "uptime_ms": 421000},
        "boot_log": [
            {"number": 29, "values": {"reset_cause": 0b1101, "previous_run_ms": 0}},
            {"number": 30, "values": {"reset_cause": 1, "previous_run_ms": 2000}},
            {"number": 31 - 16, "values": {"reset_cause": 1 << 4, "previous_run_ms": 900000,
                                           "reset_app": 3}},
        ],
    })
    assert run_bootlog(fake_gdb_writing(tmp_path, image)) == 0
    out = capsys.readouterr().out.splitlines()
    assert out[0] == "boot 30: up 421 s, MET 487 s at the last checkpoint"
    assert "boot 15: reset by watchdog; the run before lasted 900 s; reset for app 3's stall" in out
    assert "boot 29: reset by pin, brownout, power-on; the run before lasted 0 s" in out
    assert "boot 30: reset by pin; the run before lasted 2 s" in out
    # Oldest first, as the ring is read.
    assert [line.split(":")[0] for line in out[1:]] == ["boot 15", "boot 29", "boot 30"]


def test_bootlog_of_a_blank_mirror_says_so(tmp_path, capsys):
    assert run_bootlog(fake_gdb_writing(tmp_path, mirror_image({}))) == 0
    assert "no run checkpoint" in capsys.readouterr().out


def test_bootlog_of_all_zeros_suggests_the_board_is_held_in_reset(tmp_path, capsys):
    # What OpenOCD reads from a board held in reset.
    assert run_bootlog(fake_gdb_writing(tmp_path, bytes(4096))) == 1
    assert "held in reset" in capsys.readouterr().err


def test_bootlog_when_gdb_fails(tmp_path, capsys):
    assert run_bootlog("false") == 1
    assert "GDB couldn't read the mirror" in capsys.readouterr().err


def test_bootlog_when_the_box_is_down(capsys):
    assert flatsat.main(["--host", "127.0.0.1", "bootlog", "--port", str(closed_port()),
                         "--gdb", "false"]) == 1
    assert "doesn't answer" in capsys.readouterr().err
