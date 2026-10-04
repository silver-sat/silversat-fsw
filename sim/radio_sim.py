#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Radio simulator: plays the radio board on the far end of the radio UART.

On native_sim the radio UART is a pseudo-terminal, and native_sim prints its
path at startup ("uart_1 connected to pseudotty: /dev/pts/N"). This opens
it and speaks the link codec (DS-65): ground commands go to avionics as
KISS data frames (type 0x00), and avionics' replies come back the same way
(DS-66). On the flatsat, the same simulator talks to a USB-serial adapter
instead (DS-90).

    python3 sim/radio_sim.py --pty /dev/pts/2 frame_manager set_entry_enabled nominal 0 true

signs the command with the published test key, sends it, and prints the
replies. --fault picks a fault from the menu (FAULTS) to test the link.
"""

import argparse
import os
import select
import sys
import termios
import time
import tty
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import link_codec  # noqa: E402

# Ground traffic, both ways: the standard KISS data frame (DS-66).
TYPE_DATA = 0x00

# The fault menu (DS-90): what can go wrong between the radio and avionics.
FAULTS = {
    "drop": "the frame is never sent",
    "corrupt_crc": "one payload byte changes after the CRC was computed",
    "garbage_first": "line noise arrives before the frame",
    "split": "the frame arrives in three pieces, 20 ms apart",
    "repeat": "the same frame, with the same sequence number, arrives twice",
    "skip_seq": "the sequence number jumps by two, as if a frame were lost",
    "other_type": "the frame has a type byte avionics doesn't handle (0x01)",
}


class RadioSim:
    """The radio board's side of the radio UART."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.seq = link_codec.Sequence()
        self.decoder = link_codec.Decoder()
        # Anything other than a good frame that arrives from avionics.
        self.errors = []
        # Housekeeping packets that arrived while replies() was waiting.
        self.housekeeping = []

    def close(self):
        os.close(self.fd)

    def _write(self, data):
        while data:
            sent = os.write(self.fd, data)
            data = data[sent:]

    def send(self, payload, fault=None):
        """Send payload to avionics as a ground data frame, with an optional
        fault from FAULTS. Returns the sequence number used."""
        if fault is not None and fault not in FAULTS:
            raise ValueError(f"unknown fault {fault!r}; the faults are {', '.join(FAULTS)}")
        if fault == "skip_seq":
            self.seq.next()
        frame_type = 0x01 if fault == "other_type" else TYPE_DATA
        packet = link_codec.Packet(frame_type, self.seq.next(), bytes(payload))
        frame = link_codec.encode(packet)

        if fault == "drop":
            return packet.seq
        if fault == "corrupt_crc":
            raw = bytearray(packet.raw())
            raw[link_codec.HEADER_LEN] ^= 0x01
            frame = bytes([link_codec.FEND]) + link_codec.escape(bytes(raw)) + \
                bytes([link_codec.FEND])
        if fault == "garbage_first":
            self._write(b"\x13\x37noise")
        if fault == "split":
            third = len(frame) // 3
            for piece in (frame[:third], frame[third:2 * third], frame[2 * third:]):
                self._write(piece)
                time.sleep(0.02)
            return packet.seq
        self._write(frame)
        if fault == "repeat":
            self._write(frame)
        return packet.seq

    def receive(self, timeout=1.0, count=1):
        """Wait up to timeout seconds for count good frames from avionics.
        Returns the packets received, which may be fewer."""
        packets = []
        deadline = time.monotonic() + timeout
        while len(packets) < count:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                break
            for result, packet in link_codec.decode(os.read(self.fd, 1024), self.decoder):
                if result == "packet":
                    packets.append(packet)
                else:
                    self.errors.append(result)
        return packets

    def replies(self, timeout=1.0, count=1):
        """Wait up to timeout seconds for count command replies, and return
        their text as strings. The first byte of every downlink packet says
        what it is: replies start with 'A' (ACK) or 'N' (NAK). Housekeeping
        packets ('H') that arrive meanwhile go into self.housekeeping, so
        telemetry never gets mistaken for a reply."""
        texts = []
        deadline = time.monotonic() + timeout
        while len(texts) < count:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            for packet in self.receive(left, count=1):
                if packet.type != TYPE_DATA:
                    continue
                if packet.payload[:1] == b"H":
                    self.housekeeping.append(packet.payload)
                else:
                    texts.append(packet.payload.decode("ascii"))
        return texts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pty", required=True, help="the radio UART's pseudo-terminal")
    parser.add_argument("--fault", choices=list(FAULTS), help="a fault from the menu")
    parser.add_argument("--wait", type=float, default=1.0, help="seconds to wait for replies")
    parser.add_argument("command", nargs="+", help="command text, for example: "
                        "frame_manager set_entry_enabled nominal 0 true")
    args = parser.parse_args(argv)

    import sign_command

    key = sign_command.load_key(sign_command.TEST_KEY_FILE)
    packet = sign_command.sign(" ".join(args.command), key, sign_command.default_counter())
    sim = RadioSim(args.pty)
    try:
        sim.send(packet.encode("ascii"), args.fault)
        for reply in sim.replies(args.wait, count=10):
            print(reply)
        for error in sim.errors:
            print(f"(bad frame from avionics: {error})")
    finally:
        sim.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
