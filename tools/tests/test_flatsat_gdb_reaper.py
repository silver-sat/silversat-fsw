# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/flatsat_gdb_reaper.py. Run with `make test-python`.

The ss output below was captured from real connections (iproute2 6.1).
A stand-in for subprocess.run plays ss, so no test needs root or a real
OpenOCD.
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import flatsat_gdb_reaper as reaper  # noqa: E402

# Two connections to port 3333: one from a Codespace that last sent 2 s
# ago, one over IPv6 that last sent 75 minutes ago.
SS_OUTPUT = (
    "0      0      100.89.26.89:3333 100.97.186.84:34160\n"
    "\t cubic wscale:7,7 rto:200 rtt:0.014/0.007 ato:40 mss:32768 "
    "bytes_received:1 segs_out:1 segs_in:3 lastsnd:1502 lastrcv:2000 "
    "lastack:2 delivered:1 app_limited\n"
    "0      0      [fd7a:115c:a1e0::1835:1a5a]:3333 "
    "[fd7a:115c:a1e0::9]:34144\n"
    "\t cubic wscale:7,7 rto:200 rtt:0.023/0.011 lastsnd:1503 "
    "lastrcv:4500000 lastack:1502 delivered:1\n"
)


def test_parse_reads_each_peer_and_how_long_it_has_been_silent():
    assert reaper.parse_ss(SS_OUTPUT) == [
        ("100.97.186.84:34160", 2.0),
        ("[fd7a:115c:a1e0::9]:34144", 4500.0),
    ]


def test_parse_leaves_out_a_connection_without_lastrcv():
    # Never close what we can't measure.
    text = ("0 0 100.89.26.89:3333 100.97.186.84:1\n"
            "\t cubic lastsnd:5 lastack:5\n")
    assert reaper.parse_ss(text) == []


def test_parse_of_no_connections_is_empty():
    assert reaper.parse_ss("") == []


def test_parse_ignores_lastsnd_and_lastack():
    # Only time since GDB last sent counts; OpenOCD sending doesn't.
    text = ("0 0 a:3333 100.97.186.84:1\n"
            "\t lastsnd:9999999 lastrcv:1000 lastack:9999999\n")
    assert reaper.parse_ss(text) == [("100.97.186.84:1", 1.0)]


def test_idle_peers_uses_the_limit_inclusively():
    connections = [("a", 3599.0), ("b", 3600.0), ("c", 3601.0)]
    assert reaper.idle_peers(connections, 60) == ["b", "c"]


def test_close_command_names_exactly_one_connection():
    assert reaper.close_command(3333, "[fd7a::9]:34144") == [
        "ss", "-K", "-t", "state", "established",
        "( sport = :3333 and dst [fd7a::9]:34144 )"]


class FakeSs:
    """Plays ss: answers the listing, and records each close."""

    def __init__(self, listing=SS_OUTPUT, list_rc=0, close_rc=0):
        self.listing = listing
        self.list_rc = list_rc
        self.close_rc = close_rc
        self.closed = []

    def __call__(self, cmd, capture_output, text):
        if "-K" in cmd:
            self.closed.append(cmd[-1])
            return subprocess.CompletedProcess(cmd, self.close_rc, "",
                                               "Operation not permitted")
        return subprocess.CompletedProcess(cmd, self.list_rc, self.listing,
                                           "ss: bad filter")


def test_closes_only_the_idle_connection_and_says_so(monkeypatch, capsys):
    ss = FakeSs()
    monkeypatch.setattr(reaper.subprocess, "run", ss)
    assert reaper.main([]) == 0
    assert ss.closed == ["( sport = :3333 and dst [fd7a:115c:a1e0::9]:34144 )"]
    assert "closed GDB connection from [fd7a:115c:a1e0::9]:34144" in \
        capsys.readouterr().out


def test_a_shorter_limit_closes_both(monkeypatch):
    ss = FakeSs()
    monkeypatch.setattr(reaper.subprocess, "run", ss)
    assert reaper.main(["--idle-minutes", "0.01"]) == 0
    assert len(ss.closed) == 2


def test_nothing_idle_closes_nothing(monkeypatch, capsys):
    ss = FakeSs()
    monkeypatch.setattr(reaper.subprocess, "run", ss)
    assert reaper.main(["--idle-minutes", "120"]) == 0
    assert ss.closed == []
    assert capsys.readouterr().out == ""


def test_dry_run_reports_without_closing(monkeypatch, capsys):
    ss = FakeSs()
    monkeypatch.setattr(reaper.subprocess, "run", ss)
    assert reaper.main(["--dry-run"]) == 0
    assert ss.closed == []
    assert "would close idle GDB connection from [fd7a:115c:a1e0::9]:34144" \
        in capsys.readouterr().out


def test_a_failed_close_is_reported_and_fails_the_run(monkeypatch, capsys):
    monkeypatch.setattr(reaper.subprocess, "run", FakeSs(close_rc=1))
    assert reaper.main([]) == 1
    assert "couldn't close [fd7a:115c:a1e0::9]:34144: Operation not " \
        "permitted" in capsys.readouterr().err


def test_a_failed_listing_fails_the_run_and_closes_nothing(monkeypatch,
                                                          capsys):
    ss = FakeSs(list_rc=1)
    monkeypatch.setattr(reaper.subprocess, "run", ss)
    assert reaper.main([]) == 1
    assert ss.closed == []
    assert "ss failed: ss: bad filter" in capsys.readouterr().err


def test_another_port_is_listed_and_closed_on_that_port(monkeypatch):
    ss = FakeSs()
    calls = []

    def run(cmd, capture_output, text):
        calls.append(cmd)
        return ss(cmd, capture_output, text)

    monkeypatch.setattr(reaper.subprocess, "run", run)
    reaper.main(["--port", "4444"])
    assert calls[0][-1] == "( sport = :4444 )"
    assert ss.closed[0].startswith("( sport = :4444 and dst ")
