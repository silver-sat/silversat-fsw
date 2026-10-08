# SPDX-License-Identifier: Apache-2.0
"""Tests for sim/radio_sim.py. Run with `make test-python`.

A pseudo-terminal pair stands in for the radio UART: the simulator opens one
end, and the test reads and writes the other end with the link codec, the
way the radio app does.
"""

import os
import select
import sys
import termios
import threading
import tty
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))

import link_codec  # noqa: E402
import radio_sim  # noqa: E402
import telemetry  # noqa: E402


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


def test_replies_set_events_aside(link):
    sim, avionics = link
    event = b"E" + bytes(20)
    os.write(avionics, link_codec.encode(link_codec.Packet(0x00, 5, event)) +
             link_codec.encode(link_codec.Packet(0x00, 6, b"ACK 01 ok")))
    assert sim.replies(timeout=1.0) == ["ACK 01 ok"]
    assert sim.other == [event]


def test_receive_records_bad_frames(link):
    sim, avionics = link
    os.write(avionics, bytes([0xC0, 0x00, 0x01, 0xC0]))
    assert sim.receive(timeout=0.3) == []
    assert sim.errors == ["bad_length"]


def test_receive_times_out(link):
    sim, _ = link
    assert sim.receive(timeout=0.1) == []


# ---- A real serial port, and listening (DS-90) ------------------------------

@pytest.fixture(scope="module")
def dictionary():
    return telemetry.load_dictionary()


def hk_packet(dictionary, app_name, met_ms, **values):
    """An app's housekeeping packet: the values given, every other field zero."""
    app = next(a for a in dictionary["apps"] if a["name"] == app_name)
    every = {f["name"]: (f["values"][0]["name"] if "values" in f else
                         False if f["type"] == "bool" else 0)
             for f in app["housekeeping"]["fields"]}
    return telemetry.encode_hk(dictionary, app_name, met_ms, {**every, **values})


def frame(seq, payload, frame_type=0x00):
    return link_codec.encode(link_codec.Packet(frame_type, seq, payload))


def test_baud_is_set_both_ways():
    avionics, radio = os.openpty()
    try:
        sim = radio_sim.RadioSim(os.ttyname(radio), baud=19200)
        attrs = termios.tcgetattr(sim.fd)
        assert attrs[4] == termios.B19200 and attrs[5] == termios.B19200
        sim.close()
    finally:
        os.close(radio)
        os.close(avionics)


def test_the_port_ignores_carrier_detect_and_blocks_for_reads():
    """Opened without waiting for a modem (macOS's /dev/tty.* names wait for
    carrier detect, forever with a bare USB-serial adapter), then back to
    ordinary blocking reads."""
    import fcntl

    avionics, radio = os.openpty()
    try:
        sim = radio_sim.RadioSim(os.ttyname(radio))
        assert termios.tcgetattr(sim.fd)[2] & termios.CLOCAL
        assert not fcntl.fcntl(sim.fd, fcntl.F_GETFL) & os.O_NONBLOCK
        sim.close()
    finally:
        os.close(radio)
        os.close(avionics)


def test_an_unsupported_baud_is_refused():
    avionics, radio = os.openpty()
    try:
        with pytest.raises(ValueError, match="unsupported baud rate 12345"):
            radio_sim.RadioSim(os.ttyname(radio), baud=12345)
    finally:
        os.close(radio)
        os.close(avionics)


def test_describe_housekeeping(dictionary):
    payload = hk_packet(dictionary, "mode_manager", 125500,
                        mode="test", reason="test_signal", transitions=2)
    line = radio_sim.describe(dictionary, payload)
    assert line.startswith("MET 125.500 s  mode_manager: mode=test, reason=test_signal, "
                           "transitions=2, ")


def test_describe_reply_and_dump(dictionary):
    assert radio_sim.describe(dictionary, b"ACK 0000000000000001 ok") == \
        "ACK 0000000000000001 ok"
    dump = b"D\x01\x34\x12\x03\xaa\xbb\xcc"
    assert radio_sim.describe(dictionary, dump) == "dump mirror 0x1234, 3 bytes: aa bb cc"


def test_describe_an_event(dictionary):
    packet = telemetry.encode_event(dictionary, "health", "app_stalled", 1500, {"app": "nvm"})
    assert radio_sim.describe(dictionary, packet) == \
        "MET 1.500 s  EVENT health app_stalled (warning) app=nvm"


def test_describe_what_it_cannot_decode(dictionary):
    assert radio_sim.describe(dictionary, b"Z\x01\x02") == \
        "undecodable (unknown packet kind 0x5a): 5a 01 02"
    assert radio_sim.describe(dictionary, b"H\x02" + bytes(3)).startswith(
        "undecodable (housekeeping packet too short)")


def test_listen_prints_each_packet(link, dictionary):
    sim, avionics = link
    hk = hk_packet(dictionary, "health", 1000, feeds=7)
    os.write(avionics, frame(1, hk) + frame(2, b"NAK 01 replay") + frame(3, b"x", 0x01) +
             bytes([0xC0, 0x00, 0x01, 0xC0]))
    lines = []
    sim.listen(dictionary, out=lines.append, duration=0.5)
    assert lines[0].startswith("MET 1.000 s  health: ")
    assert "feeds=7" in lines[0]
    assert lines[1] == "NAK 01 replay"
    assert lines[2] == "(frame of type 0x01, not ground data)"
    assert lines[3] == "(bad frame from avionics: bad_length)"
    assert len(lines) == 4, "each bad frame reported once"


def test_listen_stops_after_its_duration(link, dictionary):
    sim, _ = link
    lines = []
    sim.listen(dictionary, out=lines.append, duration=0.2)
    assert lines == []


def test_cli_listens(dictionary, capsys):
    avionics, radio = os.openpty()
    tty.setraw(avionics)
    try:
        # Just after the simulator opens the port, which discards anything older.
        threading.Timer(0.2, os.write, (avionics, frame(1, b"ACK 01 ok"))).start()
        assert radio_sim.main(["--port", os.ttyname(radio), "--baud", "19200",
                               "--listen", "--duration", "0.5"]) == 0
        assert "ACK 01 ok" in capsys.readouterr().out
    finally:
        os.close(radio)
        os.close(avionics)


def test_cli_sends_a_command_and_listens(capsys):
    avionics, radio = os.openpty()
    tty.setraw(avionics)
    try:
        # --pty still works, for the native_sim instructions.
        assert radio_sim.main(["--pty", os.ttyname(radio), "--listen", "--duration", "0.2",
                               "nvm", "retry"]) == 0
        packets = link_codec.decode(read_all(avionics))
        assert len(packets) == 1 and packets[0][0] == "packet"
        assert packets[0][1].payload.endswith(b"nvm retry"), "signed with the test key"
    finally:
        os.close(radio)
        os.close(avionics)


def test_cli_needs_a_command_or_listen():
    with pytest.raises(SystemExit):
        radio_sim.main(["--port", "/dev/null"])
