# SPDX-License-Identifier: Apache-2.0
"""Tests for sim/radio_sim.py. Run with `make test-python`.

A pseudo-terminal pair stands in for the radio UART: the simulator opens one
end, and the test reads and writes the other end with the link codec, the
way the radio app does.
"""

import os
import select
import sys
import tty
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))

import link_codec  # noqa: E402
import radio_sim  # noqa: E402


@pytest.fixture
def link():
    """(simulator, avionics end of the line)."""
    avionics, radio = os.openpty()
    tty.setraw(avionics)
    sim = radio_sim.RadioSim(os.ttyname(radio))
    os.close(radio)
    yield sim, avionics
    sim.close()
    os.close(avionics)


def read_all(fd, wait=0.3):
    data = b""
    while select.select([fd], [], [], wait)[0]:
        data += os.read(fd, 4096)
    return data


def test_send(link):
    sim, avionics = link
    sim.send(b"hello")
    assert link_codec.decode(read_all(avionics)) == [
        ("packet", link_codec.Packet(radio_sim.TYPE_DATA, 0, b"hello"))]


def test_sequence_numbers_count_up(link):
    sim, avionics = link
    assert [sim.send(b"x") for _ in range(3)] == [0, 1, 2]
    assert [p.seq for _, p in link_codec.decode(read_all(avionics))] == [0, 1, 2]


@pytest.mark.parametrize("fault, expect", [
    ("drop", []),
    ("corrupt_crc", ["bad_crc"]),
    ("garbage_first", ["packet"]),
    ("split", ["packet"]),
    ("repeat", ["packet", "packet"]),
])
def test_faults(link, fault, expect):
    sim, avionics = link
    sim.send(b"hello", fault)
    assert [r for r, _ in link_codec.decode(read_all(avionics))] == expect


def test_repeat_uses_one_sequence_number(link):
    sim, avionics = link
    sim.send(b"hello", "repeat")
    assert [p.seq for _, p in link_codec.decode(read_all(avionics))] == [0, 0]


def test_other_type(link):
    sim, avionics = link
    sim.send(b"hello", "other_type")
    assert [p.type for _, p in link_codec.decode(read_all(avionics))] == [0x01]


def test_skip_seq(link):
    sim, avionics = link
    sim.send(b"a")
    sim.send(b"b", "skip_seq")
    assert [p.seq for _, p in link_codec.decode(read_all(avionics))] == [0, 2]


def test_unknown_fault(link):
    sim, _ = link
    with pytest.raises(ValueError, match="unknown fault"):
        sim.send(b"x", "bitflip")


def test_receive(link):
    sim, avionics = link
    os.write(avionics, link_codec.encode(link_codec.Packet(0x00, 5, b"ACK 01 ok")) +
             link_codec.encode(link_codec.Packet(0x00, 6, b"ACK 02 ok")))
    assert sim.replies(timeout=1.0, count=2) == ["ACK 01 ok", "ACK 02 ok"]


def test_replies_set_housekeeping_aside(link):
    sim, avionics = link
    hk = b"H\x01" + bytes(8) + b"\x2a"
    os.write(avionics, link_codec.encode(link_codec.Packet(0x00, 5, hk)) +
             link_codec.encode(link_codec.Packet(0x00, 6, b"NAK 01 replay")))
    assert sim.replies(timeout=1.0) == ["NAK 01 replay"]
    assert sim.housekeeping == [hk]


def test_receive_records_bad_frames(link):
    sim, avionics = link
    os.write(avionics, bytes([0xC0, 0x00, 0x01, 0xC0]))
    assert sim.receive(timeout=0.3) == []
    assert sim.errors == ["bad_length"]


def test_receive_times_out(link):
    sim, _ = link
    assert sim.receive(timeout=0.1) == []
