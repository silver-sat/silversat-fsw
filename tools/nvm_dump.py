#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decode FRAM and mirror contents by record name (DS-74).

The ground's side of the nvm app's dump command. Each D packet carries up
to 240 bytes of one store; assemble() puts the packets together into an
image of FRAM or the mirror, and decode_image() decodes every record in
the region map (the JSON dictionary's "nvm" section). It checks each slot
as the FRAM service does (magic, version, length, CRC-32C) and takes the
valid slot with the newest generation; a ring lists every valid entry.

    python3 tools/nvm_dump.py --image eeprom.bin

decodes a whole image file, for example native_sim's simulated FRAM
(eeprom.bin, kept between `make run`s). --store mirror decodes the
mirror's layout instead.

    python3 tools/nvm_dump.py --vectors V.yaml --c-image OUT.h

writes an image of the records in V.yaml, and their values, as C, which
tests/unit/libs reads back with the flight code: the ground and the
satellite are checked against the same bytes.
"""

import argparse
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import link_codec  # noqa: E402
import telemetry  # noqa: E402

MAGIC = 0x5353          # the slot header's first two bytes (DS-71)
BLANK = 0xFF            # what a blank part reads


def _regions(dictionary, store):
    """The regions held in a store, with that store's address."""
    for region in dictionary["nvm"]["regions"]:
        if store == "fram":
            yield region, region["address"]
        elif region["mirror_address"] is not None:
            yield region, region["mirror_address"]


def _store_size(dictionary, store):
    return dictionary["nvm"]["fram_size" if store == "fram" else "mirror_size"]


def _newer(a, b):
    """Serial-number arithmetic, as the FRAM service compares generations."""
    return 0 < ((a - b) & 0xFFFFFFFF) < 0x80000000


def encode_slot(dictionary, region, generation, values):
    """A slot as the FRAM service writes it: header, payload, CRC-32C."""
    payload = b"".join(telemetry._encode_field(f, values.get(f["name"], 0))
                       for f in region["fields"])
    body = (MAGIC.to_bytes(2, "little") + bytes([region["version"], region["size"]])
            + generation.to_bytes(4, "little") + payload)
    return body + link_codec.crc32c(body).to_bytes(dictionary["nvm"]["crc_len"], "little")


def decode_slot(dictionary, region, raw):
    """(generation, fields) for a valid slot, or why not: "empty",
    "other version" or "bad"."""
    header_len = dictionary["nvm"]["header_len"]
    crc_at = header_len + region["size"]
    if int.from_bytes(raw[0:2], "little") != MAGIC:
        return "empty"
    if raw[2] != region["version"]:
        return "other version"
    if raw[3] != region["size"] or \
            link_codec.crc32c(raw[:crc_at]) != int.from_bytes(raw[crc_at:region["slot_size"]],
                                                              "little"):
        return "bad"
    payload = raw[header_len:crc_at]
    return (int.from_bytes(raw[4:8], "little"),
            {f["name"]: telemetry._decode_field(f, payload) for f in region["fields"]})


def decode_image(dictionary, image, store="fram"):
    """Every record in an image, by name.

    A two-slot record is {"generation", "fields", "slots"}, with
    "fields" None if neither slot is valid. A ring is {"entries", "slots"},
    each entry {"number", "fields"}, oldest first. "slots" says what each
    slot holds: "valid", "empty", "other version" or "bad".
    """
    out = {}
    for region, start in _regions(dictionary, store):
        size = region["slot_size"]
        slots = [decode_slot(dictionary, region, image[start + i * size:start + (i + 1) * size])
                 for i in range(region["ring"] or 2)]
        states = ["valid" if isinstance(s, tuple) else s for s in slots]
        valid = [s for s in slots if isinstance(s, tuple)]
        if region["ring"]:
            entries = sorted(valid, key=lambda s: s[0])
            out[region["name"]] = {
                "entries": [{"number": n, "fields": f} for n, f in entries],
                "slots": states,
            }
            continue
        newest = None
        for slot in valid:
            if newest is None or _newer(slot[0], newest[0]):
                newest = slot
        out[region["name"]] = {
            "generation": newest[0] if newest else None,
            "fields": newest[1] if newest else None,
            "slots": states,
        }
    return out


def assemble(dictionary, packets):
    """{store: bytearray} from downlink packets, in any order. Packets
    that aren't dumps are skipped; bytes no dump covered read 0xFF."""
    images = {}
    for payload in packets:
        packet = telemetry.decode(dictionary, payload)
        if packet["kind"] != "dump":
            continue
        image = images.setdefault(
            packet["store"], bytearray([BLANK]) * _store_size(dictionary, packet["store"]))
        end = packet["address"] + len(packet["data"])
        if end > len(image):
            raise ValueError(f"dump past the end of {packet['store']}: {end}")
        image[packet["address"]:end] = packet["data"]
    return images


def build_image(dictionary, records, store="fram"):
    """An image holding records, the rest blank.

    records maps a two-slot record's name to its values (written to slot 0,
    generation 1), and a ring's name to a list of {"number", "values"}
    (each written at its number modulo the ring's length, as the service
    places it). Fields left out are 0.
    """
    image = bytearray([BLANK]) * _store_size(dictionary, store)
    held = {r["name"]: (r, start) for r, start in _regions(dictionary, store)}
    for name, spec in records.items():
        region, start = held[name]
        if region["ring"]:
            writes = [(e["number"] % region["ring"], e["number"], e["values"]) for e in spec]
        else:
            writes = [(0, 1, spec)]
        for slot, generation, values in writes:
            at = start + slot * region["slot_size"]
            raw = encode_slot(dictionary, region, generation, values)
            image[at:at + len(raw)] = raw
    return image


def _c_value(field, value):
    """A field's value as a C initializer."""
    if "values" in field and isinstance(value, str):
        value = next(v["value"] for v in field["values"] if v["name"] == value)
    if field["type"] == "bool":
        return "true" if value else "false"
    if field["type"] == "uint64":
        return f"{value}ULL"
    if field["type"] == "uint32":
        return f"{value}U"
    if field["type"] == "int64":
        return f"{value}LL"
    return str(value)


def _c_struct(region, values):
    return "{" + ", ".join(f".{f['name']} = {_c_value(f, values.get(f['name'], 0))}"
                           for f in region["fields"]) + "}"


def c_image(dictionary, records):
    """C source: the used part of the FRAM image of records
    (nvm_vector_image), and each record's values as the generated struct
    (nvm_vector_<name>), or for a ring an array of {number, entry}."""
    image = build_image(dictionary, records)
    used = max((r["address"] + r["slot_size"] * (r["ring"] or 2)
                for r in dictionary["nvm"]["regions"]), default=0)
    by_name = {r["name"]: r for r in dictionary["nvm"]["regions"]}
    lines = ["/* Generated by tools/nvm_dump.py --c-image. Do not edit. */",
             "",
             f"static const uint8_t nvm_vector_image[{used}] = {{"]
    for i in range(0, used, 12):
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in image[i:min(i + 12, used)]) + ",")
    lines.append("};")
    for name, spec in records.items():
        region = by_name[name]
        lines.append("")
        if region["ring"]:
            lines.append(f"static const struct {{\n\tuint32_t number;\n"
                         f"\tstruct nvm_{name} entry;\n}} nvm_vector_{name}[{len(spec)}] = {{")
            for e in spec:
                lines.append(f"\t{{{e['number']}, {_c_struct(region, e['values'])}}},")
            lines.append("};")
        else:
            lines.append(f"static const struct nvm_{name} nvm_vector_{name} = "
                         f"{_c_struct(region, spec)};")
    lines.append("")
    return "\n".join(lines)


def _print_records(records):
    for name, record in records.items():
        if "entries" in record:
            print(f"{name}: {len(record['entries'])} entries")
            for entry in record["entries"]:
                print(f"  {entry['number']}: {entry['fields']}")
        elif record["fields"] is None:
            print(f"{name}: no valid record (slots: {', '.join(record['slots'])})")
        else:
            print(f"{name} (generation {record['generation']}): {record['fields']}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Decode FRAM and mirror contents by record.")
    parser.add_argument("--image", type=Path, help="an image file of FRAM or the mirror")
    parser.add_argument("--store", choices=["fram", "mirror"], default="fram",
                        help="which store's layout the image has (default fram)")
    parser.add_argument("--vectors", type=Path, help="a YAML file of records to build an image of")
    parser.add_argument("--c-image", type=Path, help="with --vectors: write the image as C here")
    args = parser.parse_args(argv)

    dictionary = telemetry.load_dictionary()
    if args.vectors and args.c_image:
        records = yaml.safe_load(args.vectors.read_text())["records"]
        args.c_image.parent.mkdir(parents=True, exist_ok=True)
        args.c_image.write_text(c_image(dictionary, records))
        return 0
    if args.image:
        _print_records(decode_image(dictionary, args.image.read_bytes(), args.store))
        return 0
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
