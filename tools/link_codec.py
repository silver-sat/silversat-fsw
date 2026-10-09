#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Frames for the serial links between avionics and its peer boards.

The Python twin of include/silversat/link_codec.h (DS-91), for the subsystem
simulators and the ground. Every frame has the same layout:

    type  seq  len  payload (0 to 255 bytes)  CRC-32C (4 bytes, LSB first)

KISS-escaped and wrapped in FEND bytes. The CRC covers type, seq, len and
payload, and is computed before escaping (DS-65).

    python3 tools/link_codec.py --vectors VECTORS.yaml --c-vectors OUT.h

writes the shared vectors as a C header for tests/unit/libs.
"""

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
PAYLOAD_MAX = 255
HEADER_LEN, CRC_LEN = 3, 4
RAW_MAX = HEADER_LEN + PAYLOAD_MAX + CRC_LEN

# The same order and names as enum link_decode_result.
RESULTS = ("none", "packet", "bad_crc", "bad_length", "bad_escape", "too_long")


def _crc32c_table():
    table = []
    for n in range(256):
        crc = n
        for _ in range(8):
            crc = (crc >> 1) ^ 0x82F63B78 if crc & 1 else crc >> 1
        table.append(crc)
    return table


_TABLE = _crc32c_table()


def crc32c(data):
    """CRC-32C with the parameters in DS-65."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc = _TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


@dataclass
class Packet:
    type: int
    seq: int
    payload: bytes = b""

    def raw(self):
        """The unescaped frame: header, payload, CRC."""
        if len(self.payload) > PAYLOAD_MAX:
            raise ValueError(f"payload is longer than {PAYLOAD_MAX} bytes")
        body = bytes([self.type, self.seq, len(self.payload)]) + self.payload
        return body + crc32c(body).to_bytes(CRC_LEN, "little")


def escape(raw):
    out = bytearray()
    for byte in raw:
        if byte == FEND:
            out += bytes([FESC, TFEND])
        elif byte == FESC:
            out += bytes([FESC, TFESC])
        else:
            out.append(byte)
    return bytes(out)


def encode(packet):
    """A complete KISS frame for packet."""
    return bytes([FEND]) + escape(packet.raw()) + bytes([FEND])


class Decoder:
    """Byte-at-a-time decoder, with the same rules as link_decode_byte()."""

    def __init__(self):
        self.in_frame = False
        self._start()

    def _start(self):
        self.raw = bytearray()
        self.escaped = False
        self.broken = None

    def _finish(self):
        if self.broken:
            return self.broken, None
        if self.escaped:
            return "bad_escape", None
        raw = bytes(self.raw)
        if len(raw) < HEADER_LEN + CRC_LEN or raw[2] != len(raw) - HEADER_LEN - CRC_LEN:
            return "bad_length", None
        if int.from_bytes(raw[-CRC_LEN:], "little") != crc32c(raw[:-CRC_LEN]):
            return "bad_crc", None
        return "packet", Packet(raw[0], raw[1], raw[HEADER_LEN:-CRC_LEN])

    def feed(self, byte):
        """Feed one byte; returns (result, packet or None)."""
        if byte == FEND:
            if not self.in_frame or (not self.raw and not self.escaped and not self.broken):
                self.in_frame = True
                self._start()
                return "none", None
            result = self._finish()
            self._start()
            return result
        if not self.in_frame or self.broken:
            return "none", None
        if self.escaped:
            self.escaped = False
            if byte == TFEND:
                byte = FEND
            elif byte == TFESC:
                byte = FESC
            else:
                self.broken = "bad_escape"
                return "none", None
        elif byte == FESC:
            self.escaped = True
            return "none", None
        if len(self.raw) == RAW_MAX:
            self.broken = "too_long"
            return "none", None
        self.raw.append(byte)
        return "none", None


def decode(data, decoder=None):
    """Every result other than "none" from feeding data, in order."""
    decoder = decoder or Decoder()
    results = []
    for byte in data:
        result = decoder.feed(byte)
        if result[0] != "none":
            results.append(result)
    return results


class Sequence:
    """Sequence numbers for one link, the same as struct link_seq."""

    def __init__(self, first=0):
        self.next_tx = first % 256
        self.last_rx = None

    def next(self):
        number = self.next_tx
        self.next_tx = (self.next_tx + 1) % 256
        return number

    def received(self, number):
        """Frames lost just before this one, or -1 if it repeats the last."""
        if self.last_rx is None:
            self.last_rx = number
            return 0
        if number == self.last_rx:
            return -1
        lost = (number - self.last_rx - 1) % 256
        self.last_rx = number
        return lost


# --- Shared vectors -------------------------------------------------------


def _payload(spec):
    if "payload_text" in spec:
        return spec["payload_text"].encode("ascii")
    if "payload_fill" in spec:
        byte, count = spec["payload_fill"]
        return bytes([byte]) * count
    return bytes.fromhex(spec.get("payload_hex", ""))


def load_vectors(path):
    """The vectors file, with each packet built and each stream assembled."""
    import yaml

    data = yaml.safe_load(Path(path).read_text())
    packets = {}
    for p in data["packets"]:
        packets[p["name"]] = Packet(p["type"], p["seq"], _payload(p))
    streams = []
    for s in data["streams"]:
        stream = bytearray()
        for part in s["parts"]:
            if "hex" in part:
                stream += bytes.fromhex(part["hex"])
            elif "fill" in part:
                byte, count = part["fill"]
                stream += bytes([byte]) * count
            elif "packet" in part:
                stream += encode(packets[part["packet"]])
            elif "continues" in part:
                # Shares the previous frame's closing FEND as its opening one.
                stream += escape(packets[part["continues"]].raw()) + bytes([FEND])
            elif "damaged" in part:
                raw = bytearray(packets[part["damaged"]].raw())
                for index, value in part.get("set", {}).items():
                    raw[int(index)] = value
                if part.get("fix_crc"):
                    body = bytes(raw[:-CRC_LEN])
                    raw[-CRC_LEN:] = crc32c(body).to_bytes(CRC_LEN, "little")
                stream += bytes([FEND]) + escape(bytes(raw)) + bytes([FEND])
        streams.append({"name": s["name"], "bytes": bytes(stream), "expect": s["expect"]})
    return packets, streams


def _c_bytes(data):
    return "{" + ", ".join(f"0x{b:02x}" for b in data) + "}" if data else "{0}"


def c_vectors(path):
    """The vectors as a C header, with Python's own encoding of each packet."""
    packets, streams = load_vectors(path)
    names = list(packets)
    out = ["/* GENERATED by tools/link_codec.py --c-vectors. Do not edit. */", "",
           "#include <stddef.h>", "#include <stdint.h>", "",
           '#include "silversat/link_codec.h"', ""]
    for i, name in enumerate(names):
        p = packets[name]
        out.append(f"static const uint8_t packet_{i}_payload[] = {_c_bytes(p.payload)};")
        out.append(f"static const uint8_t packet_{i}_encoded[] = {_c_bytes(encode(p))};")
    out += ["", "struct packet_vector {", "\tconst char *name;", "\tuint8_t type;",
            "\tuint8_t seq;", "\tuint8_t len;", "\tconst uint8_t *payload;",
            "\tconst uint8_t *encoded;", "\tsize_t encoded_len;", "};", "",
            "static const struct packet_vector packet_vectors[] = {"]
    for i, name in enumerate(names):
        p = packets[name]
        out.append(f'\t{{"{name}", 0x{p.type:02x}, {p.seq}, {len(p.payload)}, '
                   f"packet_{i}_payload, packet_{i}_encoded, sizeof(packet_{i}_encoded)}},")
    out += ["};", ""]

    out += ["/* One expected decoder result; packet is an index into packet_vectors, or -1. */",
            "struct expected_result {", "\tenum link_decode_result result;", "\tint packet;",
            "};", ""]
    for i, s in enumerate(streams):
        out.append(f"static const uint8_t stream_{i}_bytes[] = {_c_bytes(s['bytes'])};")
        expect = []
        for e in s["expect"]:
            if isinstance(e, dict):
                expect.append(f"{{LINK_DECODE_PACKET, {names.index(e['packet'])}}}")
            else:
                expect.append(f"{{LINK_DECODE_{e.upper()}, -1}}")
        out.append(f"static const struct expected_result stream_{i}_expect[] = "
                   f"{{{', '.join(expect)}}};")
    out += ["", "struct stream_vector {", "\tconst char *name;", "\tconst uint8_t *bytes;",
            "\tsize_t len;", "\tconst struct expected_result *expect;",
            "\tsize_t expect_count;", "};", "",
            "static const struct stream_vector stream_vectors[] = {"]
    for i, s in enumerate(streams):
        out.append(f'\t{{"{s["name"]}", stream_{i}_bytes, sizeof(stream_{i}_bytes), '
                   f"stream_{i}_expect, {len(s['expect'])}}},")
    out += ["};", ""]
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vectors", type=Path, required=True, help="shared vectors (YAML)")
    parser.add_argument("--c-vectors", type=Path, required=True, metavar="FILE",
                        help="write the vectors as a C header to FILE")
    args = parser.parse_args(argv)
    args.c_vectors.parent.mkdir(parents=True, exist_ok=True)
    args.c_vectors.write_text(c_vectors(args.vectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
