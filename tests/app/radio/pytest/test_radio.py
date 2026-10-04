# SPDX-License-Identifier: Apache-2.0
"""Radio end-to-end tests (DS-32, DS-33, DS-50, DS-90, DS-92).

The flight path for a ground command, every piece real except the radio:

    radio simulator --PTY--> radio app --> uplink queue --> command ingest
        --> frame manager, and the reply back the same way.

Telemetry output also sends one app's housekeeping each second on the same
downlink; the simulator sets those packets aside while it waits for replies.

Twister starts the native_sim image; these tests play the radio with
sim/radio_sim.py. They run in real time, so each one is short.
"""

import re
import sys
import time
from pathlib import Path

import pytest
from twister_harness import DeviceAdapter

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))

import radio_sim  # noqa: E402
import sign_command  # noqa: E402
import telemetry  # noqa: E402

KEY = sign_command.load_key(sign_command.TEST_KEY_FILE)
COMMAND = "frame_manager set_entry_enabled nominal 0 true"

# Each signed command needs a counter above the last; the floor starts at
# the mission epoch at every boot.
_last_counter = 0


def signed(text=COMMAND, key=KEY):
    global _last_counter
    _last_counter = sign_command.default_counter(_last_counter)
    return sign_command.sign(text, key, _last_counter).encode("ascii"), _last_counter


def ack(counter, result="ok"):
    return f"ACK {counter:016x} {result}"


@pytest.fixture(scope="session")
def sim(dut: DeviceAdapter):
    lines = dut.readlines_until(regex="radio test ready", timeout=20)
    paths = [m.group(1) for line in lines
             if (m := re.search(r"uart_1 connected to pseudotty: (\S+)", line))]
    assert paths, "the radio UART's pseudo-terminal was not announced"
    radio = radio_sim.RadioSim(paths[0])
    yield radio
    radio.close()


@pytest.fixture(autouse=True)
def quiet_line(sim):
    """Start each test with nothing left over from the last."""
    sim.receive(timeout=0.3, count=100)
    sim.errors.clear()
    sim.housekeeping.clear()


def test_command_round_trip(sim):
    packet, counter = signed()
    sim.send(packet)
    assert sim.replies(timeout=1.0) == [ack(counter)]
    assert sim.errors == [], "every frame from avionics was good"


def test_replay_is_refused(sim):
    packet, counter = signed()
    sim.send(packet)
    assert sim.replies(timeout=1.0) == [ack(counter)]
    sim.send(packet)
    assert sim.replies(timeout=1.0) == [f"NAK {counter:016x} replay"]


def test_corrupted_frame_is_dropped(sim):
    packet, _ = signed()
    sim.send(packet, fault="corrupt_crc")
    assert sim.replies(timeout=0.5) == [], "a frame with a bad CRC never reaches command ingest"

    packet, counter = signed()
    sim.send(packet)
    assert sim.replies(timeout=1.0) == [ack(counter)]


def test_noise_and_split_frames(sim):
    packet, counter = signed()
    sim.send(packet, fault="garbage_first")
    assert sim.replies(timeout=1.0) == [ack(counter)]

    packet, counter = signed()
    sim.send(packet, fault="split")
    assert sim.replies(timeout=1.0) == [ack(counter)]


def test_repeated_frame_is_passed_on_once(sim):
    # The radio app drops the second copy, so command ingest never sees it
    # and there is no "replay" reply.
    packet, counter = signed()
    sim.send(packet, fault="repeat")
    assert sim.replies(timeout=1.0, count=2) == [ack(counter)]


def test_lost_frame_does_not_stop_the_link(sim):
    packet, counter = signed()
    sim.send(packet, fault="skip_seq")
    assert sim.replies(timeout=1.0) == [ack(counter)]


def test_forged_command_gets_no_reply(sim):
    packet, _ = signed(key=bytes(range(32)))
    sim.send(packet)
    assert sim.replies(timeout=0.5) == []


HK_LINE = re.compile(r"radio hk: (.*)")


def radio_hk(dut, lines=1):
    """The radio's housekeeping, read fresh from the console. The test image
    prints it once a second, and the radio publishes it at each major frame,
    so the second fresh line is sure to include everything that happened
    before this call."""
    dut.clear_buffer()  # drop the lines printed during earlier tests
    for _ in range(lines):
        found = dut.readlines_until(regex=HK_LINE.pattern, timeout=3)
    fields = HK_LINE.search(found[-1]).group(1)
    return {name: int(value) for name, value in
            (pair.split("=") for pair in fields.split())}


def test_link_counters(sim, dut):
    """Each fault shows up in the radio's housekeeping (DS-33)."""
    before = radio_hk(dut, lines=2)
    for fault in ("corrupt_crc", "skip_seq", "repeat", "other_type"):
        sim.send(signed()[0], fault=fault)
        sim.replies(timeout=0.3, count=5)
    after = radio_hk(dut, lines=2)

    def delta(name):
        return after[name] - before[name]

    assert delta("bad_crc") == 1
    # The corrupted frame used a sequence number but never decoded, and
    # skip_seq jumped one more: the gap before skip_seq's frame is two.
    assert delta("lost") == 2
    assert delta("repeated") == 1
    assert delta("unknown_type") == 1
    assert delta("received") == 4, "skip_seq, both copies of repeat, other_type"
    # Replies to skip_seq and repeat, plus one housekeeping packet a second
    # from telemetry output, which this test can't line up exactly with the
    # console's once-a-second snapshot.
    assert delta("sent") >= 2
    assert after["uplink_dropped"] == 0 and after["rx_overrun"] == 0


def test_downlink_sequence_numbers_count_up(sim):
    """Every downlink packet takes the next sequence number, replies and
    housekeeping alike. receive() can return more than count packets when
    housekeeping arrives in the same read, so check all of them."""
    for _ in range(3):
        sim.send(signed()[0])
    packets = sim.receive(timeout=1.5, count=3)
    assert len(packets) >= 3
    first = packets[0].seq
    assert [p.seq for p in packets] == [(first + i) % 256 for i in range(len(packets))]


def test_housekeeping_from_every_app(sim):
    """Telemetry output sends each app's housekeeping in turn, one app a
    second, and the ground decodes it with the dictionary (DS-61, DS-69)."""
    dictionary = telemetry.load_dictionary()
    apps = {app["name"] for app in dictionary["apps"]}
    seen = {}
    deadline = time.monotonic() + len(apps) + 1.5
    while set(seen) != apps and time.monotonic() < deadline:
        for packet in sim.receive(timeout=0.5, count=1):
            decoded = telemetry.decode(dictionary, packet.payload)
            if decoded["kind"] == "hk":
                seen[decoded["app"]] = decoded
    assert set(seen) == apps
    assert sim.errors == []

    # One packet a major frame; MET comes from the frame tick (DS-25).
    order = sorted(seen.values(), key=lambda hk: hk["met_ms"])
    gaps = [b["met_ms"] - a["met_ms"] for a, b in zip(order, order[1:])]
    assert all(950 <= gap <= 1050 for gap in gaps), gaps
    assert seen["frame_manager"]["fields"]["frame_count"] > 0
    assert seen["radio"]["fields"]["frames_sent"] > 0
