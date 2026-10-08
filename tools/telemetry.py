#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decode SilverSat 2 downlink packets (DS-61, DS-64).

Every downlink packet starts with a printable kind byte:

    'A', 'N'  command replies, as text: "ACK <counter> ok", "NAK <counter> replay"
    'H'       housekeeping: 'H', app id, MET (8 bytes), then the app's
              housekeeping fields, little-endian, packed in YAML order
    'D'       a memory dump from the nvm app: 'D', store (0 FRAM, 1 the
              mirror), address (2 bytes), length, then the bytes as stored.
              tools/nvm_dump.py puts dumps together and decodes the records.
    'E'       an event (DS-10): 'E', then struct event from common.yaml: MET
              (8 bytes), app id, severity, event id (2 bytes), and two
              arguments (4 bytes each, signed), decoded by the event's name
              and its arguments' names from the app's YAML.

The housekeeping layouts come from the same YAML as the flight encoders
(include/silversat/tlm_encode.h), through the generator's dictionary.

    python3 tools/telemetry.py --vectors VECTORS.yaml --c-vectors OUT.h

writes housekeeping vectors as a C header for tests/unit/libs.
"""

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import command_text  # noqa: E402

KIND_HK = ord("H")
KIND_DUMP = ord("D")
KIND_EVENT = ord("E")
REPLY_KINDS = (ord("A"), ord("N"))
HK_HEADER_LEN = 10
DUMP_HEADER_LEN = 5          # 'D', store, address (2, little-endian), length
DUMP_STORES = {0: "fram", 1: "mirror"}

FLOAT_FORMATS = {"float32": "<f", "float64": "<d"}


def load_dictionary(extra_app_dirs=()):
    return command_text.load_dictionary(extra_app_dirs)


def _app(dictionary, key, value):
    app = next((a for a in dictionary["apps"] if a[key] == value), None)
    if app is None:
        raise ValueError(f"no app with {key} {value!r}")
    return app


def _decode_field(field, data):
    raw = data[field["offset"]:field["offset"] + field["size"]]
    kind = field["type"]
    if "values" in field:
        number = int.from_bytes(raw, "little")
        return next((v["name"] for v in field["values"] if v["value"] == number), number)
    if kind == "bool":
        return raw[0] != 0
    if kind in FLOAT_FORMATS:
        return struct.unpack(FLOAT_FORMATS[kind], raw)[0]
    return int.from_bytes(raw, "little", signed=kind.startswith("int"))


def _encode_field(field, value):
    kind = field["type"]
    if "values" in field:
        if isinstance(value, str):
            value = next(v["value"] for v in field["values"] if v["name"] == value)
        return value.to_bytes(field["size"], "little")
    if kind == "bool":
        return bytes([1 if value else 0])
    if kind in FLOAT_FORMATS:
        return struct.pack(FLOAT_FORMATS[kind], value)
    return value.to_bytes(field["size"], "little", signed=kind.startswith("int"))


def decode(dictionary, payload):
    """One downlink packet, as a dict with "kind": "reply", "hk", "dump" or "event"."""
    if not payload:
        raise ValueError("empty packet")
    if payload[0] in REPLY_KINDS:
        return {"kind": "reply", "text": payload.decode("ascii")}
    if payload[0] == KIND_DUMP:
        return _decode_dump(payload)
    if payload[0] == KIND_EVENT:
        return _decode_event(dictionary, payload)
    if payload[0] != KIND_HK:
        raise ValueError(f"unknown packet kind {payload[0]:#04x}")
    if len(payload) < HK_HEADER_LEN:
        raise ValueError("housekeeping packet too short")
    app = _app(dictionary, "id", payload[1])
    hk = app["housekeeping"]
    data = payload[HK_HEADER_LEN:]
    if len(data) != hk["size"]:
        raise ValueError(f"{app['name']} housekeeping is {hk['size']} bytes, got {len(data)}")
    return {
        "kind": "hk",
        "app": app["name"],
        "met_ms": int.from_bytes(payload[2:HK_HEADER_LEN], "little", signed=True),
        "fields": {f["name"]: _decode_field(f, data) for f in hk["fields"]},
    }


def _event_arg(dictionary, arg, value):
    """An event argument's value: an app's name, an enum value's name, or the
    number, as the argument's type says."""
    if arg["type"] == "app":
        return next((a["name"] for a in dictionary["apps"] if a["id"] == value), value)
    if arg["type"] != "int32":
        values = dictionary["enums"].get(arg["type"], [])
        return next((v["name"] for v in values if v["value"] == value), value)
    return value


def _decode_event(dictionary, payload):
    """An 'E' packet (DS-10), by name."""
    layout = dictionary["event"]
    data = payload[1:]
    if len(data) != layout["size"]:
        raise ValueError(f"event packet is {len(data)} bytes after its kind, not {layout['size']}")
    raw = {f["name"]: _decode_field(f, data) for f in layout["fields"]}
    app = _app(dictionary, "id", raw["app"])
    event = next((e for e in app["events"] if e["id"] == raw["id"]), None)
    if event is None:
        raise ValueError(f"{app['name']} has no event {raw['id']}")
    values = [raw["arg0"], raw["arg1"]]
    return {
        "kind": "event",
        "app": app["name"],
        "name": event["name"],
        "severity": raw["severity"],
        "met_ms": raw["met_ms"],
        "args": {a["name"]: _event_arg(dictionary, a, v) for a, v in zip(event["args"], values)},
    }


def _decode_dump(payload):
    """A 'D' packet from the nvm app's dump command (DS-74)."""
    if len(payload) < DUMP_HEADER_LEN:
        raise ValueError("dump packet too short")
    if payload[1] not in DUMP_STORES:
        raise ValueError(f"unknown store {payload[1]}")
    length = payload[4]
    data = payload[DUMP_HEADER_LEN:]
    if len(data) != length:
        raise ValueError(f"dump says {length} bytes, carries {len(data)}")
    return {
        "kind": "dump",
        "store": DUMP_STORES[payload[1]],
        "address": int.from_bytes(payload[2:4], "little"),
        "data": bytes(data),
    }


def encode_hk(dictionary, app_name, met_ms, values):
    """The 'H' packet the flight encoder sends for these values."""
    app = _app(dictionary, "name", app_name)
    body = b"".join(_encode_field(f, values[f["name"]]) for f in app["housekeeping"]["fields"])
    return bytes([KIND_HK, app["id"]]) + met_ms.to_bytes(8, "little", signed=True) + body


def encode_event(dictionary, app_name, event_name, met_ms, args):
    """The 'E' packet the flight encoder sends for this event (DS-10). args
    maps each argument's name to a number, or to a name for an argument of
    type app or an enum."""
    app = _app(dictionary, "name", app_name)
    event = next(e for e in app["events"] if e["name"] == event_name)
    numbers = []
    for arg in event["args"]:
        value = args[arg["name"]]
        if arg["type"] == "app":
            value = _app(dictionary, "name", value)["id"] if isinstance(value, str) else value
        elif arg["type"] != "int32" and isinstance(value, str):
            value = next(v["value"] for v in dictionary["enums"][arg["type"]]
                         if v["name"] == value)
        numbers.append(value)
    numbers += [0] * (2 - len(numbers))
    values = {"met_ms": met_ms, "app": app["id"], "severity": event["severity"],
              "id": event["id"], "arg0": numbers[0], "arg1": numbers[1]}
    return bytes([KIND_EVENT]) + b"".join(_encode_field(f, values[f["name"]])
                                          for f in dictionary["event"]["fields"])


# --- Shared vectors -------------------------------------------------------


def _c_value(field, value):
    if "values" in field:
        return str(next(v["value"] for v in field["values"] if v["name"] == value))
    if field["type"] == "bool":
        return "true" if value else "false"
    if field["type"] == "float32":
        return f"{float(value)!r}f"
    if field["type"] == "float64":
        return repr(float(value))
    if field["type"] == "int64" and value == -2**63:
        return "INT64_MIN"
    if field["type"] == "uint64":
        return f"{value}ULL"
    if field["type"] == "int64":
        return f"{value}LL"
    return str(value)


def _c_int32(value):
    """An int32 as C: the smallest needs INT32_MIN, which a literal can't write."""
    return "INT32_MIN" if value == -2**31 else str(value)


def c_vectors(dictionary, path):
    """Housekeeping vectors as a C header: each a struct to publish and the
    packet the flight encoder must produce."""
    import yaml

    vectors = yaml.safe_load(Path(path).read_text())["vectors"]
    out = ["/* GENERATED by tools/telemetry.py --c-vectors. Do not edit. */", "",
           "#include <stdbool.h>", "#include <stdint.h>", ""]
    rows = []
    for i, v in enumerate(vectors):
        app = _app(dictionary, "name", v["app"])
        fields = app["housekeeping"]["fields"]
        init = ", ".join(f".{f['name']} = {_c_value(f, v['values'][f['name']])}" for f in fields)
        packet = encode_hk(dictionary, v["app"], v["met_ms"], v["values"])
        out.append(f"static const struct {v['app']}_hk hk_{i} = {{{init}}};")
        out.append(f"static const uint8_t packet_{i}[] = "
                   f"{{{', '.join(f'0x{b:02x}' for b in packet)}}};")
        rows.append(f'\t{{"{v["name"]}", &hk_{i}, {v["met_ms"]}LL, packet_{i}, sizeof(packet_{i})}},')
    out += ["", "struct hk_vector {", "\tconst char *name;", f"\tconst struct {vectors[0]['app']}_hk *hk;",
            "\tint64_t met_ms;", "\tconst uint8_t *packet;", "\tsize_t len;", "};", "",
            "static const struct hk_vector hk_vectors[] = {"] + rows + ["};", ""]

    # Events: the struct an emit function fills in, and the 'E' packet.
    event_rows = []
    for i, v in enumerate(yaml.safe_load(Path(path).read_text()).get("events", [])):
        packet = encode_event(dictionary, v["app"], v["event"], v["met_ms"], v["args"])
        decoded = {f["name"]: _decode_field(f, packet[1:]) for f in dictionary["event"]["fields"]}
        severity = next(f for f in dictionary["event"]["fields"] if f["name"] == "severity")
        init = ", ".join([f".met_ms = {v['met_ms']}LL", f".app = {decoded['app']}",
                          f".severity = {_c_value(severity, decoded['severity'])}",
                          f".id = {decoded['id']}", f".arg0 = {_c_int32(decoded['arg0'])}",
                          f".arg1 = {_c_int32(decoded['arg1'])}"])
        out.append(f"static const struct event event_{i} = {{{init}}};")
        out.append(f"static const uint8_t event_packet_{i}[] = "
                   f"{{{', '.join(f'0x{b:02x}' for b in packet)}}};")
        event_rows.append(f'\t{{"{v["name"]}", &event_{i}, event_packet_{i}, '
                          f'sizeof(event_packet_{i})}},')
    out += ["struct event_vector {", "\tconst char *name;", "\tconst struct event *event;",
            "\tconst uint8_t *packet;", "\tsize_t len;", "};", "",
            "static const struct event_vector event_vectors[] = {"] + event_rows + ["};", ""]
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vectors", type=Path, required=True,
                        help="housekeeping vectors (YAML); all for one app")
    parser.add_argument("--c-vectors", type=Path, required=True, metavar="FILE")
    parser.add_argument("--extra-apps", type=Path, action="append", default=[],
                        metavar="DIR", help="a directory of test-only <app>.yaml files")
    args = parser.parse_args(argv)
    dictionary = load_dictionary(args.extra_apps)
    args.c_vectors.parent.mkdir(parents=True, exist_ok=True)
    args.c_vectors.write_text(c_vectors(dictionary, args.vectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
