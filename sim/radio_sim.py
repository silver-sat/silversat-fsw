#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Radio simulator: plays the radio board on the far end of the radio UART.

On native_sim the radio UART is a pseudo-terminal, and native_sim prints its
path at startup ("uart_1 connected to pseudotty: /dev/pts/N"). On a Nucleo
on the bench it is a USB-serial adapter wired to the radio UART's pins
(docs/nucleo-on-your-desk.md), at 19200 baud. This opens either and speaks
the link codec (DS-65): ground commands go to avionics as KISS data frames
(type 0x00), and avionics' replies and telemetry come back the same way
(DS-66, DS-90).

    python3 sim/radio_sim.py --port /dev/pts/2 frame_manager set_entry_enabled nominal 0 true

signs the command with the published test key, sends it, and prints the
replies. --fault picks a fault from the menu (FAULTS) to test the link.

    python3 sim/radio_sim.py --port /dev/cu.usbserial-1234 --baud 19200 --listen

On the flatsat (docs/flatsat.md), the radio UART is a TCP port on the
flatsat box, through ser2net:

    python3 sim/radio_sim.py --port tcp://flatsat:4001 --listen

prints every packet avionics sends, decoded by name (tools/telemetry.py):
housekeeping, events, command replies and memory dumps, until Ctrl-C or
--duration seconds. With a command as well, it sends the command first.
Decoding names needs the message dictionary, generated from messages/ on
the fly: Python 3 with PyYAML and Jinja2.
"""

import argparse
import fcntl
import os
import random
import socket
import select
import sys
import termios
import time
import tty
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import link_codec  # noqa: E402
import telemetry  # noqa: E402

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


def open_port(path):
    """Open a serial port for reading and writing, without waiting for a
    modem. On macOS, opening a port's /dev/tty.* name blocks until the
    carrier-detect line is asserted, which a USB-serial adapter wired only
    to TX, RX and ground never does; /dev/cu.* doesn't wait. So open without
    blocking, tell the line to ignore carrier detect (CLOCAL), then go back
    to ordinary blocking reads. Either name then works."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL   # cflag: no modem control lines
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return fd


def set_baud(fd, baud):
    """Set a serial port's speed, both ways. A pseudo-terminal accepts any."""
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        raise ValueError(f"unsupported baud rate {baud}")
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = speed   # input and output speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def describe(dictionary, payload):
    """One downlink packet as a line of text, decoded by name (DS-66)."""
    try:
        packet = telemetry.decode(dictionary, payload)
    except ValueError as e:
        return f"undecodable ({e}): {payload.hex(' ')}"
    if packet["kind"] == "reply":
        return packet["text"]
    if packet["kind"] == "event":
        args = "".join(f" {name}={value}" for name, value in packet["args"].items())
        return (f"MET {packet['met_ms'] / 1000:.3f} s  EVENT {packet['app']} "
                f"{packet['name']} ({packet['severity']}){args}")
    if packet["kind"] == "dump":
        return (f"dump {packet['store']} 0x{packet['address']:04x}, "
                f"{len(packet['data'])} bytes: {packet['data'].hex(' ')}")
    fields = ", ".join(f"{name}={value}" for name, value in packet["fields"].items())
    return f"MET {packet['met_ms'] / 1000:.3f} s  {packet['app']}: {fields}"


class RadioSim:
    """The radio board's side of the radio UART."""

    def __init__(self, path, baud=None, first_seq=None):
        self.sock = None
        if path.startswith("tcp://"):
            # The flatsat's radio UART, through ser2net: the box sets the
            # baud rate, so --baud doesn't apply.
            host, _, port = path[len("tcp://"):].rpartition(":")
            self.sock = socket.create_connection((host, int(port)), timeout=10)
            self.sock.settimeout(None)
            self.fd = self.sock.fileno()
        else:
            self.fd = open_port(path)
            tty.setraw(self.fd)
            if baud is not None:
                set_baud(self.fd, baud)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        # The radio drops a frame whose sequence number repeats the last one
        # it received (DS-65), so a run that started at 0 like the run before
        # would have its first command dropped. Start somewhere random: a
        # clash is 1 in 256, and costs only that command.
        self.seq = link_codec.Sequence(random.randrange(256) if first_seq is None else first_seq)
        self.decoder = link_codec.Decoder()
        # Anything other than a good frame that arrives from avionics.
        self.errors = []
        # Housekeeping packets that arrived while replies() was waiting.
        self.housekeeping = []
        # Any other packets that did (events, dumps).
        self.other = []

    def close(self):
        if self.sock is not None:
            self.sock.close()
        else:
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
        packets ('H') that arrive meanwhile go into self.housekeeping, and
        any other kind (events, dumps) into self.other, so telemetry never
        gets mistaken for a reply."""
        texts = []
        deadline = time.monotonic() + timeout
        while len(texts) < count:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            for packet in self.receive(left, count=1):
                if packet.type != TYPE_DATA:
                    continue
                if packet.payload[:1] in (b"A", b"N"):
                    texts.append(packet.payload.decode("ascii"))
                elif packet.payload[:1] == b"H":
                    self.housekeeping.append(packet.payload)
                else:
                    self.other.append(packet.payload)
        return texts

    def listen(self, dictionary, out=print, duration=None):
        """Print each packet avionics sends, decoded by name, and each bad
        frame, until duration seconds pass (or forever, if None)."""
        deadline = None if duration is None else time.monotonic() + duration
        while deadline is None or time.monotonic() < deadline:
            wait = 0.5 if deadline is None else max(0.0, min(0.5, deadline - time.monotonic()))
            for packet in self.receive(wait, count=1):
                if packet.type != TYPE_DATA:
                    out(f"(frame of type {packet.type:#04x}, not ground data)")
                else:
                    out(describe(dictionary, packet.payload))
            for error in self.errors:
                out(f"(bad frame from avionics: {error})")
            self.errors.clear()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", "--pty", dest="port", required=True,
                        help="the radio UART: native_sim's pseudo-terminal, a USB-serial "
                             "adapter, or tcp://host:port on the flatsat")
    parser.add_argument("--baud", type=int,
                        help="set the port's speed: 19200 for the Nucleo's radio UART")
    parser.add_argument("--fault", choices=list(FAULTS), help="a fault from the menu")
    parser.add_argument("--wait", type=float, default=1.0, help="seconds to wait for replies")
    parser.add_argument("--listen", action="store_true",
                        help="print every packet avionics sends, decoded, until Ctrl-C")
    parser.add_argument("--duration", type=float, help="with --listen: stop after this many seconds")
    parser.add_argument("command", nargs="*", help="command text, for example: "
                        "frame_manager set_entry_enabled nominal 0 true")
    args = parser.parse_args(argv)
    if not args.command and not args.listen:
        parser.error("give a command, --listen, or both")

    sim = RadioSim(args.port, args.baud)
    try:
        if args.command:
            import sign_command

            key = sign_command.load_key(sign_command.TEST_KEY_FILE)
            packet = sign_command.sign(" ".join(args.command), key,
                                       sign_command.default_counter())
            sim.send(packet.encode("ascii"), args.fault)
            if not args.listen:
                for reply in sim.replies(args.wait, count=10):
                    print(reply)
                for error in sim.errors:
                    print(f"(bad frame from avionics: {error})")
        if args.listen:
            sim.listen(telemetry.load_dictionary(), duration=args.duration)
    except KeyboardInterrupt:
        pass
    finally:
        sim.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
