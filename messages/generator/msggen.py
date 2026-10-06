#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Message generator for the SilverSat flight software.

DS-60, DS-61, DS-68, and DS-74 in docs/silversat2-fsw-design-decisions.md.
Reads the YAML definitions in messages/, checks them, and writes the C code
every app uses to talk to the others, plus a JSON dictionary for the ground.
The build runs this automatically (messages/CMakeLists.txt). The output goes
in the build directory and is never committed.

    python3 msggen.py --defs messages --out build/silversat_msg

Input, in <defs>/:
    common.yaml       shared types: enums (mode, severity, ...), the frame
                      tick, app status, and the types of data channels
    apps/<app>.yaml   one app: its id, whether the frame manager wakes it,
                      its commands (with the modes each is allowed in),
                      housekeeping, data channels (with initial values),
                      and the internal commands it sends (sends:)
    nvm_map.yaml      FRAM records, each owned by one app; which are also
                      kept in the mirror (backup SRAM); and rings of
                      records written once each, like the boot log (optional)

Output, in <out>/:
    include/msg/common.h     shared types; every app's status and wakeup
                             channel and every data channel; app ids and
                             the counts the resource map sizes the zbus
                             pool from (APP_COUNT, APP_SEND_PAIR_COUNT, ...)
    include/msg/<app>.h      the app's commands, housekeeping, message union,
                             and a send_<app>_<command>() for each sends: entry
    src/msg_<app>.c          the app's channels, its senders, and build checks
                             that every message fits a zbus buffer
    src/cmd_routes.c         decoding and routing of ground command text
                             (command ingest, DS-50)
    src/tlm_encode.c         each app's housekeeping, encoded little-endian for
                             the downlink (telemetry output, DS-61, DS-64)
    include/nvm/<app>.h      the app's FRAM records: struct, encoding, default,
                             region handle, typed read and write (DS-74);
                             one per app, empty if it has none

--json FILE also writes the dictionary (DS-61): every command with its
arguments, ranges and modes; each app's housekeeping layout; who sends which
internal command; and the FRAM map. The ground formats command text
(tools/command_text.py) and decodes telemetry (tools/telemetry.py) from it.

Leave out --out to check the definitions without writing anything.

Checks, each failing with a message that names the file and the entry:
unknown or missing keys (usually typos), names and C keywords, app ids 1 to
63, command ids, allowed modes, argument types and ranges, names that would
collide in C, housekeeping that wouldn't fit one downlink frame, sends: that
name no such app or command, and FRAM records that are too big or don't fit
in the FRAM or the mirror.

A test can add apps of its own with --extra-apps <dir>, a directory of
<app>.yaml files. They are generated exactly like the flight apps.

Still to come (DS-61, DS-74): Python classes, the interface document, the
CI check that a changed FRAM record has a new version, and write protection.
"""

import argparse
import json
import re
import shutil
import sys
import textwrap
from dataclasses import dataclass, field
from pathlib import Path

import jinja2
import yaml

TEMPLATE_DIR = Path(__file__).parent / "templates"

# YAML type name -> C type. Fixed-width types only, so a struct has the same
# fields on the M4 and on both native_sim targets.
SCALAR_TYPES = {
    "bool": "bool",
    "uint8": "uint8_t",
    "uint16": "uint16_t",
    "uint32": "uint32_t",
    "uint64": "uint64_t",
    "int8": "int8_t",
    "int16": "int16_t",
    "int32": "int32_t",
    "int64": "int64_t",
    "float32": "float",
    "float64": "double",
}

# Types an enum may be stored as, and the largest value each can hold. A
# struct field of enum type uses this fixed-width type, not the C enum type,
# because the size of a C enum depends on the compiler.
ENUM_STORAGE_MAX = {
    "uint8": 0xFF,
    "uint16": 0xFFFF,
    "uint32": 0xFFFFFFFF,
}

C_KEYWORDS = {
    "auto", "bool", "break", "case", "char", "const", "continue", "default",
    "do", "double", "else", "enum", "extern", "false", "float", "for", "goto",
    "if", "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "true", "typedef",
    "union", "unsigned", "void", "volatile", "while",
}

NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")

# The generator names an app's own types <app>_cmd, <app>_hk and <app>_msg,
# so a command cannot use those names.
RESERVED_COMMAND_NAMES = {"cmd", "hk", "msg"}

# The generator emits <ENUM>_MAX for each enum, so no value may be named max.
RESERVED_ENUM_VALUE_NAMES = {"max"}

# "common" would collide with msg/common.h.
RESERVED_APP_NAMES = {"common"}

# common.yaml must define these; every app's channels use them.
REQUIRED_STRUCTS = ("frame_tick", "app_status")

# common.yaml must define enum mode; every command lists the modes it is
# allowed in (DS-50).
REQUIRED_ENUMS = ("mode",)

# Fields the generated code reads from struct app_status (DS-14, DS-22):
# the frame manager reads steps, and command routing reads the counters.
REQUIRED_STATUS_FIELDS = ("steps", "cmd_accepted", "cmd_rejected")

# A command's text is "<app> <command> <arguments>", at most
# CMD_TEXT_WORDS_MAX (16) words in include/silversat/cmd_text.h.
COMMAND_ARGS_MAX = 14

# How each field type is written in command text, and its C range.
UNSIGNED_MAX = {"uint8": "UINT8_MAX", "uint16": "UINT16_MAX",
                "uint32": "UINT32_MAX", "uint64": "UINT64_MAX"}
SIGNED_RANGE = {"int8": ("INT8_MIN", "INT8_MAX"), "int16": ("INT16_MIN", "INT16_MAX"),
                "int32": ("INT32_MIN", "INT32_MAX"), "int64": ("INT64_MIN", "INT64_MAX")}
FLOAT_TYPES = ("float32", "float64")

# Bytes each type takes on the downlink: little-endian, no padding (DS-64).
WIRE_SIZE = {"bool": 1, "uint8": 1, "int8": 1, "uint16": 2, "int16": 2,
             "uint32": 4, "int32": 4, "uint64": 8, "int64": 8,
             "float32": 4, "float64": 8}
CTYPE_WIRE_SIZE = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4}   # enum storage

# A housekeeping packet: 'H', the app id, MET (8 bytes), then the fields. It
# must fit in one link frame's payload (LINK_PAYLOAD_MAX in link_codec.h).
TLM_HK_HEADER_LEN = 10
LINK_PAYLOAD_MAX = 255

# Bit n of the frame manager's stall mask is app n (DS-43), a uint64.
# Flight apps count up from 1; test-only apps use 48 to 63.
APP_ID_MAX = 63
COMMAND_ID_MAX = 0xFFFF    # struct <app>_cmd carries the id as uint16


class DefinitionError(Exception):
    """A problem in the YAML definitions. The message says where."""


# --- The model: what the templates see ------------------------------------


@dataclass
class Field:
    name: str
    type: str                 # YAML type name
    ctype: str                # C type used in the struct
    description: str
    enum: str | None = None   # the enum's name, if the type is an enum

    @property
    def text_kind(self):
        """How the field is written in command text."""
        if self.enum:
            return "enum"
        if self.type == "bool":
            return "bool"
        if self.type in UNSIGNED_MAX:
            return "unsigned"
        if self.type in SIGNED_RANGE:
            return "signed"
        return None

    @property
    def wire_size(self):
        """Bytes on the downlink (DS-64)."""
        return CTYPE_WIRE_SIZE[self.ctype] if self.enum else WIRE_SIZE[self.type]

    @property
    def wire_kind(self):
        """How the generated encoder writes the field."""
        if self.type == "bool":
            return "bool"
        if self.type in FLOAT_TYPES:
            return "f32" if self.type == "float32" else "f64"
        return {1: "byte", 2: "le16", 4: "le32", 8: "le64"}[self.wire_size]


@dataclass
class EnumValue:
    name: str
    value: int
    description: str
    constant: str             # C name, for example SEVERITY_INFO


@dataclass
class Enum:
    name: str
    ctype: str
    description: str
    values: list[EnumValue]

    @property
    def max_constant(self):
        return f"{self.name}_max".upper()

    @property
    def max_value(self):
        return max(v.value for v in self.values)


@dataclass
class Struct:
    name: str
    description: str
    fields: list[Field]

    @property
    def wire_layout(self):
        """(field, offset) for each field, packed in order."""
        layout, offset = [], 0
        for f in self.fields:
            layout.append((f, offset))
            offset += f.wire_size
        return layout

    @property
    def wire_size(self):
        return sum(f.wire_size for f in self.fields)


@dataclass
class Command:
    name: str
    id: int
    description: str
    fields: list[Field]
    constant: str             # C name of the id, for example FRAME_MANAGER_CMD_SET_ENTRY_ENABLED
    struct: str               # C name of the arguments struct
    modes: list[str]          # the modes it is allowed in (DS-50)


@dataclass
class DataChannel:
    """A last-value channel an app owns besides the four every app has."""
    name: str
    struct: str               # a struct from common.yaml
    description: str
    # (field, C expression) for each field given under initial:, the value
    # the channel holds before its owner first publishes. Other fields are 0.
    initial: list = field(default_factory=list)

    @property
    def init_expr(self):
        if not self.initial:
            return "0"
        return ", ".join(f".{name} = {expr}" for name, expr in self.initial)


@dataclass
class Send:
    """An internal command one app sends to another (DS-68): the generator
    writes send_<target>_<command>() into the sender's header."""
    target: "App"
    command: Command

    @property
    def function(self):
        return f"send_{self.target.name}_{self.command.name}"

    @property
    def params(self):
        """The C parameter list: one per argument, in YAML order."""
        if not self.command.fields:
            return "void"
        return ", ".join(f"{f.ctype} {f.name}" for f in self.command.fields)


@dataclass
class App:
    """One app, and every C name the generator derives from it (DS-14)."""

    name: str
    id: int
    description: str
    wakeup: bool
    commands: list[Command]
    housekeeping: Struct
    data_channels: list[DataChannel]
    source: str               # the YAML file, for the generated banner and errors
    # "app.command" strings from the YAML; load_definitions resolves them
    # into sends once every app is known.
    send_names: list[str] = field(default_factory=list)
    sends: list[Send] = field(default_factory=list)

    @property
    def id_constant(self):
        return f"APP_ID_{self.name.upper()}"

    @property
    def name_constant(self):
        return f"{self.name.upper()}_APP_NAME"

    @property
    def cmd_id_enum(self):
        return f"{self.name}_cmd_id"

    @property
    def cmd_struct(self):
        return f"{self.name}_cmd"

    @property
    def hk_struct(self):
        return self.housekeeping.name

    @property
    def msg_union(self):
        return f"{self.name}_msg"

    @property
    def cmd_chan(self):
        return f"{self.name}_cmd_chan"

    @property
    def hk_chan(self):
        return f"{self.name}_hk_chan"

    @property
    def status_chan(self):
        return f"{self.name}_status_chan"

    @property
    def wakeup_chan(self):
        return f"{self.name}_wakeup_chan" if self.wakeup else None

    @property
    def channels(self):
        """(channel, C type) for every channel this app owns."""
        pairs = [(self.cmd_chan, f"struct {self.cmd_struct}"),
                 (self.hk_chan, f"struct {self.hk_struct}"),
                 (self.status_chan, "struct app_status")]
        if self.wakeup:
            pairs.append((self.wakeup_chan, "struct frame_tick"))
        pairs += [(d.name, f"struct {d.struct}") for d in self.data_channels]
        return pairs

    @property
    def commands_with_args(self):
        return [c for c in self.commands if c.fields]

    @property
    def send_targets(self):
        """The apps this app sends internal commands to, each once."""
        targets = []
        for s in self.sends:
            if s.target not in targets:
                targets.append(s.target)
        return targets

    def generated_names(self):
        """Every C identifier this app's generated code defines."""
        names = [self.id_constant, self.cmd_id_enum, self.cmd_struct,
                 self.hk_struct, self.msg_union, self.cmd_chan, self.hk_chan,
                 self.status_chan]
        if self.wakeup:
            names.append(self.wakeup_chan)
        names += [d.name for d in self.data_channels]
        if self.commands:
            names.append(self.name_constant)
        for c in self.commands:
            names += [c.constant, f"{c.constant}_NAME"]
            if c.fields:
                names.append(c.struct)
        names += [f"send_{n.replace('.', '_')}" for n in self.send_names]
        return names


# FRAM regions (DS-71, DS-74): nvm_map.yaml. Must match silversat/nvm.h.
NVM_HEADER_LEN = 8
NVM_CRC_LEN = 4
NVM_PAYLOAD_MAX = 128
NVM_ALIGN = 16              # each region starts on a 16-byte boundary, for readable dumps
NVM_RING_MAX = 255          # struct nvm_region holds a ring's length as uint8
NVM_FRAM_SIZE_MAX = 0x10000  # struct nvm_region holds addresses as uint16


@dataclass
class NvmRegion:
    """One FRAM record and its place in the map."""
    name: str
    owner: "App"
    version: int
    description: str
    fields: list[Field]
    defaults: dict             # field name -> YAML default value
    address: int = 0
    source: str = ""
    mirrored: bool = False     # also kept in the mirror (DS-75's second tier)
    mirror_address: int = 0
    ring: int = 0              # entries in a ring (DS-71's boot log), or 0: two slots

    @property
    def struct(self):
        return Struct(f"nvm_{self.name}", self.description, self.fields)

    @property
    def size(self):
        """Payload bytes."""
        return self.struct.wire_size

    @property
    def slot_size(self):
        return NVM_HEADER_LEN + self.size + NVM_CRC_LEN

    @property
    def region_size(self):
        return (self.ring or 2) * self.slot_size

    def default_bytes(self, enums):
        """The encoded default payload, little-endian, as the C encoder writes it."""
        out = bytearray()
        for f in self.fields:
            value = self.defaults.get(f.name)
            if f.enum:
                names = {v.name: v.value for v in enums[f.enum].values}
                number = names[value] if value is not None else \
                    min(v.value for v in enums[f.enum].values)
                out += number.to_bytes(f.wire_size, "little")
            elif f.type == "bool":
                out += bytes([1 if value else 0])
            else:
                out += int(value or 0).to_bytes(f.wire_size, "little",
                                                signed=f.type.startswith("int"))
        return bytes(out)


@dataclass
class NvmMap:
    fram_size: int
    regions: list[NvmRegion]
    mirror_size: int = 0

    @property
    def used(self):
        return max((r.address + r.region_size for r in self.regions), default=0)

    @property
    def mirror_used(self):
        return max((r.mirror_address + r.region_size for r in self.regions if r.mirrored),
                   default=0)

    def regions_of(self, app):
        return [r for r in self.regions if r.owner is app]


@dataclass
class Definitions:
    enums: list[Enum]
    structs: list[Struct]
    apps: list[App]
    nvm: NvmMap = field(default_factory=lambda: NvmMap(0, []))

    @property
    def wakeup_apps(self):
        return [a for a in self.apps if a.wakeup]

    @property
    def send_pair_count(self):
        """(sender, target app) pairs. Each may have CMD_MAX_PENDING
        commands waiting, so the resource map budgets the pool for them."""
        return sum(len(a.send_targets) for a in self.apps)

    @property
    def enums_by_name(self):
        return {e.name: e for e in self.enums}

    @property
    def hk_wire_kinds(self):
        """Every wire_kind some app's housekeeping uses."""
        return {f.wire_kind for a in self.apps for f in a.housekeeping.fields}

    @property
    def command_enums(self):
        """Enums used by some command argument, in definition order."""
        used = {f.enum for a in self.apps for c in a.commands for f in c.fields if f.enum}
        return [e for e in self.enums if e.name in used]


# --- Checking helpers -----------------------------------------------------


def _load_yaml(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f)
    except yaml.YAMLError as e:
        raise DefinitionError(f"{path}: not valid YAML: {e}") from None
    if not isinstance(data, dict):
        raise DefinitionError(f"{path}: expected a mapping (key: value) at the top level")
    return data


def _check_keys(node, where, required, optional=()):
    """Reject missing keys and unknown keys. Unknown keys are usually typos."""
    if not isinstance(node, dict):
        raise DefinitionError(f"{where}: expected a mapping (key: value)")
    missing = [k for k in required if k not in node]
    if missing:
        raise DefinitionError(f"{where}: missing {', '.join(missing)}")
    allowed = list(required) + list(optional)
    unknown = sorted(str(k) for k in node if k not in allowed)
    if unknown:
        raise DefinitionError(
            f"{where}: unknown key {', '.join(unknown)} (allowed: {', '.join(allowed)})")


def _name(value, where):
    if not isinstance(value, str) or not NAME_RE.match(value):
        raise DefinitionError(
            f"{where}: {value!r} is not a valid name; use lower_snake_case "
            "(a lowercase letter, then lowercase letters, digits, or underscores)")
    if value in C_KEYWORDS:
        raise DefinitionError(f"{where}: {value!r} is a C keyword")
    return value


def _text(value, where):
    """Non-empty text, with line breaks and runs of spaces collapsed."""
    if not isinstance(value, str) or not value.strip():
        raise DefinitionError(f"{where}: expected non-empty text")
    return " ".join(value.split())


def _integer(value, where, lowest, highest):
    # YAML reads `true` as a bool, and Python's bool is an int; reject it.
    if isinstance(value, bool) or not isinstance(value, int):
        raise DefinitionError(f"{where}: expected a whole number, got {value!r}")
    if not lowest <= value <= highest:
        raise DefinitionError(f"{where}: {value} is outside {lowest}..{highest}")
    return value


def _sequence(node, key, where):
    """The list under key, or an empty list if the key is absent."""
    value = node.get(key, [])
    if value is None:
        return []
    if not isinstance(value, list):
        raise DefinitionError(f"{where}: {key} must be a list (lines starting with '-')")
    return value


def _check_unique(items, attribute, what, where):
    seen = set()
    for item in items:
        value = getattr(item, attribute)
        if value in seen:
            raise DefinitionError(f"{where}: duplicate {what} {value!r}")
        seen.add(value)


# --- Parsing --------------------------------------------------------------


def _parse_field(node, where, enums):
    _check_keys(node, where, ("name", "type"), ("description",))
    name = _name(node["name"], f"{where}.name")
    type_name = node["type"]
    if type_name in SCALAR_TYPES:
        ctype, enum = SCALAR_TYPES[type_name], None
    elif type_name in enums:
        ctype, enum = enums[type_name].ctype, type_name
    else:
        raise DefinitionError(
            f"{where}.type: unknown type {type_name!r}; use one of "
            f"{', '.join(SCALAR_TYPES)}, or an enum defined in common.yaml")
    description = _text(node["description"], f"{where}.description") \
        if "description" in node else ""
    return Field(name, type_name, ctype, description, enum)


def _parse_fields(nodes, where, enums, allow_empty):
    if not nodes and not allow_empty:
        raise DefinitionError(f"{where}: needs at least one field")
    fields = [_parse_field(n, f"{where}[{i}]", enums) for i, n in enumerate(nodes)]
    _check_unique(fields, "name", "field", where)
    return fields


def _parse_enum(node, where):
    _check_keys(node, where, ("name", "type", "description", "values"))
    name = _name(node["name"], f"{where}.name")
    if name in SCALAR_TYPES:
        raise DefinitionError(f"{where}.name: {name!r} is a built-in type")
    storage = node["type"]
    if storage not in ENUM_STORAGE_MAX:
        raise DefinitionError(
            f"{where}.type: an enum is stored as one of {', '.join(ENUM_STORAGE_MAX)}")
    highest = ENUM_STORAGE_MAX[storage]
    values = []
    nodes = _sequence(node, "values", where)
    if not nodes:
        raise DefinitionError(f"{where}.values: needs at least one value")
    for i, v in enumerate(nodes):
        vwhere = f"{where}.values[{i}]"
        _check_keys(v, vwhere, ("name", "value"), ("description",))
        vname = _name(v["name"], f"{vwhere}.name")
        if vname in RESERVED_ENUM_VALUE_NAMES:
            raise DefinitionError(
                f"{vwhere}.name: {vname!r} is reserved; the generator emits "
                f"{name.upper()}_MAX")
        value = _integer(v["value"], f"{vwhere}.value", 0, highest)
        description = _text(v["description"], f"{vwhere}.description") \
            if "description" in v else ""
        values.append(EnumValue(vname, value, description, f"{name}_{vname}".upper()))
    _check_unique(values, "name", "value name", f"{where}.values")
    _check_unique(values, "value", "value", f"{where}.values")
    return Enum(name, SCALAR_TYPES[storage], _text(node["description"], f"{where}.description"),
                values)


def _parse_struct(node, where, enums, allow_empty=False):
    _check_keys(node, where, ("name", "description", "fields"))
    name = _name(node["name"], f"{where}.name")
    fields = _parse_fields(_sequence(node, "fields", where), f"{where}.fields", enums,
                           allow_empty)
    return Struct(name, _text(node["description"], f"{where}.description"), fields)


def _parse_common(path):
    data = _load_yaml(path)
    where = str(path)
    _check_keys(data, where, ("structs",), ("enums",))

    enums = [_parse_enum(n, f"{where}: enums[{i}]")
             for i, n in enumerate(_sequence(data, "enums", where))]
    _check_unique(enums, "name", "enum", where)
    by_name = {e.name: e for e in enums}

    structs = [_parse_struct(n, f"{where}: structs[{i}]", by_name)
               for i, n in enumerate(_sequence(data, "structs", where))]
    _check_unique(structs, "name", "struct", where)
    for required in REQUIRED_STRUCTS:
        if required not in {s.name for s in structs}:
            raise DefinitionError(
                f"{where}: must define struct {required!r}; every app's channels use it")
    status = next(s for s in structs if s.name == "app_status")
    for name in REQUIRED_STATUS_FIELDS:
        if not any(f.name == name and f.type == "uint32" for f in status.fields):
            raise DefinitionError(
                f"{where}: struct app_status must have a uint32 field {name!r} (DS-14)")
    for required in REQUIRED_ENUMS:
        if required not in by_name:
            raise DefinitionError(
                f"{where}: must define enum {required!r}; every command lists its modes")
    return enums, structs


def _parse_modes(node, where, enums):
    """The modes a command is allowed in. Required: no default (DS-50)."""
    modes = node.get("modes")
    if not isinstance(modes, list) or not modes:
        raise DefinitionError(
            f"{where}: list the modes this command is allowed in, for example "
            "modes: [safe, nominal]")
    known = [v.name for v in enums["mode"].values]
    seen = set()
    for i, mode in enumerate(modes):
        if mode not in known:
            raise DefinitionError(
                f"{where}[{i}]: unknown mode {mode!r}; the modes are {', '.join(known)}")
        if mode in seen:
            raise DefinitionError(f"{where}[{i}]: mode {mode!r} is listed twice")
        seen.add(mode)
    return modes


def _parse_command(node, where, app_name, enums):
    # modes is required, but _parse_modes says so with an example.
    _check_keys(node, where, ("name", "id", "description"), ("fields", "modes"))
    name = _name(node["name"], f"{where}.name")
    if name in RESERVED_COMMAND_NAMES:
        raise DefinitionError(
            f"{where}.name: {name!r} is reserved for the generator's own "
            f"{app_name}_{name} type")
    fields = _parse_fields(_sequence(node, "fields", where), f"{where}.fields", enums,
                           allow_empty=True)
    for i, f in enumerate(fields):
        if f.type in FLOAT_TYPES:
            raise DefinitionError(
                f"{where}.fields[{i}]: {f.type} arguments are not supported in commands "
                "yet; they arrive with the first command that needs one")
    if len(fields) > COMMAND_ARGS_MAX:
        raise DefinitionError(
            f"{where}.fields: a command has at most {COMMAND_ARGS_MAX} arguments")
    return Command(
        name=name,
        id=_integer(node["id"], f"{where}.id", 1, COMMAND_ID_MAX),
        description=_text(node["description"], f"{where}.description"),
        fields=fields,
        constant=f"{app_name}_cmd_{name}".upper(),
        struct=f"{app_name}_{name}",
        modes=_parse_modes(node, f"{where}.modes", enums),
    )


def _initial_value(f, value, where, enums):
    """The C expression for one field's initial value."""
    if f.enum:
        names = {v.name: v.constant for v in enums[f.enum].values}
        if value not in names:
            raise DefinitionError(
                f"{where}: {value!r} is not a {f.enum}; use one of {', '.join(names)}")
        return names[value]
    if f.type == "bool":
        if not isinstance(value, bool):
            raise DefinitionError(f"{where}: expected true or false, got {value!r}")
        return "true" if value else "false"
    if f.type in FLOAT_TYPES:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise DefinitionError(f"{where}: expected a number, got {value!r}")
        return repr(float(value))
    if f.type in UNSIGNED_MAX:
        lowest, highest = 0, 2**int(f.type[4:]) - 1
    else:
        bits = int(f.type[3:])
        lowest, highest = -(2**(bits - 1)), 2**(bits - 1) - 1
    return str(_integer(value, where, lowest, highest))


def _parse_data_channel(node, where, structs, enums):
    _check_keys(node, where, ("name", "type", "description"), ("initial",))
    name = _name(node["name"], f"{where}.name")
    if not name.endswith("_chan"):
        raise DefinitionError(f"{where}.name: a channel name must end in _chan")
    if node["type"] not in structs:
        raise DefinitionError(
            f"{where}.type: {node['type']!r} is not a struct in common.yaml. Other apps "
            "read a data channel, and an app includes only its own header and "
            "msg/common.h (DS-68), so the type must be shared")
    initial = []
    given = node.get("initial", {})
    if not isinstance(given, dict):
        raise DefinitionError(f"{where}.initial: expected field: value pairs")
    fields = {f.name: f for f in structs[node["type"]].fields}
    for key in given:
        if key not in fields:
            raise DefinitionError(
                f"{where}.initial: {node['type']} has no field {key!r} "
                f"(its fields: {', '.join(fields)})")
    # In the struct's field order, whatever order the YAML gives them in.
    for field_name, f in fields.items():
        if field_name in given:
            initial.append((field_name, _initial_value(f, given[field_name],
                                                       f"{where}.initial.{field_name}", enums)))
    return DataChannel(name, node["type"], _text(node["description"], f"{where}.description"),
                       initial)


def _source_label(path, root):
    """path relative to root (the repository, in practice) if it is inside it."""
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def _parse_app(path, enums, structs, root):
    data = _load_yaml(path)
    where = str(path)
    _check_keys(data, where, ("app", "id", "description", "wakeup", "housekeeping"),
                ("commands", "data_channels", "sends"))

    name = _name(data["app"], f"{where}: app")
    if name != path.stem:
        raise DefinitionError(
            f"{where}: file name must match the app name; rename it to {name}.yaml")
    if name in RESERVED_APP_NAMES:
        raise DefinitionError(f"{where}: app name {name!r} is reserved")
    if not isinstance(data["wakeup"], bool):
        raise DefinitionError(
            f"{where}: wakeup must be true (the frame manager drives this app) or false")

    commands = [_parse_command(n, f"{where}: commands[{i}]", name, enums)
                for i, n in enumerate(_sequence(data, "commands", where))]
    _check_unique(commands, "name", "command name", f"{where}: commands")
    _check_unique(commands, "id", "command id", f"{where}: commands")

    hk_where = f"{where}: housekeeping"
    hk = data["housekeeping"]
    _check_keys(hk, hk_where, ("description", "fields"))
    housekeeping = _parse_struct({"name": f"{name}_hk", **hk}, hk_where, enums)
    room = LINK_PAYLOAD_MAX - TLM_HK_HEADER_LEN
    if housekeeping.wire_size > room:
        raise DefinitionError(
            f"{hk_where}: {housekeeping.wire_size} bytes on the downlink; housekeeping "
            f"must fit in one frame, at most {room} bytes")

    data_channels = [_parse_data_channel(n, f"{where}: data_channels[{i}]", structs, enums)
                     for i, n in enumerate(_sequence(data, "data_channels", where))]
    _check_unique(data_channels, "name", "data channel", f"{where}: data_channels")

    send_names = []
    for i, entry in enumerate(_sequence(data, "sends", where)):
        swhere = f"{where}: sends[{i}]"
        parts = entry.split(".") if isinstance(entry, str) else []
        if len(parts) != 2:
            raise DefinitionError(
                f"{swhere}: {entry!r} should be <app>.<command>, for example "
                "radio.set_transmit")
        _name(parts[0], swhere)
        _name(parts[1], swhere)
        if parts[0] == name:
            raise DefinitionError(
                f"{swhere}: an app doesn't send commands to itself; call the code directly")
        if entry in send_names:
            raise DefinitionError(f"{swhere}: {entry!r} is listed twice")
        send_names.append(entry)

    return App(
        name=name,
        id=_integer(data["id"], f"{where}: id", 1, APP_ID_MAX),
        description=_text(data["description"], f"{where}: description"),
        wakeup=data["wakeup"],
        commands=commands,
        housekeeping=housekeeping,
        data_channels=data_channels,
        source=_source_label(path, root),
        send_names=send_names,
    )


def _resolve_sends(apps):
    """Turn each app's "app.command" strings into Sends, now that every app
    is known."""
    by_name = {a.name: a for a in apps}
    for app in apps:
        for i, entry in enumerate(app.send_names):
            where = f"{app.source}: sends[{i}]"
            target_name, command_name = entry.split(".")
            target = by_name.get(target_name)
            if target is None:
                raise DefinitionError(f"{where}: there is no app {target_name!r}")
            command = next((c for c in target.commands if c.name == command_name), None)
            if command is None:
                known = ", ".join(c.name for c in target.commands) or "none"
                raise DefinitionError(
                    f"{where}: {target_name} has no command {command_name!r} "
                    f"(its commands: {known})")
            if any(f.name == "cmd" for f in command.fields):
                raise DefinitionError(
                    f"{where}: {entry} has an argument named 'cmd', which the generated "
                    "sender uses for the command itself; rename the argument")
            app.sends.append(Send(target, command))


def _parse_nvm_map(path, apps, enums):
    """nvm_map.yaml: FRAM regions laid out in file order (DS-74)."""
    data = _load_yaml(path)
    where = str(path)
    _check_keys(data, where, ("fram_size", "regions"), ("mirror_size",))
    fram_size = _integer(data["fram_size"], f"{where}: fram_size", 1, NVM_FRAM_SIZE_MAX)
    mirror_size = _integer(data.get("mirror_size", 0), f"{where}: mirror_size", 0,
                           NVM_FRAM_SIZE_MAX)
    by_name = {a.name: a for a in apps}
    regions = []
    address = 0
    mirror_address = 0
    for i, node in enumerate(_sequence(data, "regions", where)):
        rwhere = f"{where}: regions[{i}]"
        _check_keys(node, rwhere, ("name", "owner", "version", "description", "fields"),
                    ("mirror", "ring"))
        name = _name(node["name"], f"{rwhere}.name")
        owner = by_name.get(node["owner"])
        if owner is None:
            raise DefinitionError(
                f"{rwhere}.owner: there is no app {node['owner']!r}; a region's owner is "
                "the one app that writes it (DS-74)")
        defaults = {}
        field_nodes = []
        for j, fnode in enumerate(_sequence(node, "fields", rwhere)):
            fnode = dict(fnode) if isinstance(fnode, dict) else fnode
            if isinstance(fnode, dict) and "default" in fnode:
                defaults[fnode.get("name")] = fnode.pop("default")
            field_nodes.append(fnode)
        fields = _parse_fields(field_nodes, f"{rwhere}.fields", enums, allow_empty=False)
        for j, f in enumerate(fields):
            if f.type in FLOAT_TYPES:
                raise DefinitionError(
                    f"{rwhere}.fields[{j}]: {f.type} is not supported in FRAM records yet")
            if f.name in defaults:
                _initial_value(f, defaults[f.name], f"{rwhere}.fields[{j}].default", enums)
        region = NvmRegion(name, owner, _integer(node["version"], f"{rwhere}.version", 1, 255),
                           _text(node["description"], f"{rwhere}.description"), fields,
                           defaults, address, where)
        if region.size > NVM_PAYLOAD_MAX:
            raise DefinitionError(
                f"{rwhere}: {region.size} bytes; a record holds at most {NVM_PAYLOAD_MAX}")
        mirror = node.get("mirror", False)
        if not isinstance(mirror, bool):
            raise DefinitionError(f"{rwhere}.mirror: must be true or false")
        if "ring" in node:
            region.ring = _integer(node["ring"], f"{rwhere}.ring", 2, NVM_RING_MAX)
            if mirror:
                raise DefinitionError(
                    f"{rwhere}: a ring isn't mirrored; the mirror holds small records only")
        if mirror:
            region.mirrored = True
            region.mirror_address = mirror_address
            mirror_address += -(-region.region_size // NVM_ALIGN) * NVM_ALIGN
        regions.append(region)
        address += -(-region.region_size // NVM_ALIGN) * NVM_ALIGN
    _check_unique(regions, "name", "region", where)
    nvm = NvmMap(fram_size, regions, mirror_size)
    if nvm.used > fram_size:
        raise DefinitionError(
            f"{where}: the regions need {nvm.used} bytes, more than fram_size ({fram_size})")
    if nvm.mirror_used > mirror_size:
        raise DefinitionError(
            f"{where}: the mirrored regions need {nvm.mirror_used} bytes, more than "
            f"mirror_size ({mirror_size})")
    return nvm


def load_definitions(defs_dir, extra_app_dirs=()):
    """Read and check every definition under defs_dir, plus the app files in
    each of extra_app_dirs. Raises DefinitionError."""
    defs_dir = Path(defs_dir)
    common_path = defs_dir / "common.yaml"
    if not common_path.is_file():
        raise DefinitionError(f"{common_path}: not found")
    enums, structs = _parse_common(common_path)
    by_name = {e.name: e for e in enums}

    app_paths = sorted((defs_dir / "apps").glob("*.yaml"))
    if not app_paths:
        raise DefinitionError(f"{defs_dir / 'apps'}: no app definitions (*.yaml) found")
    for extra in extra_app_dirs:
        extra = Path(extra)
        if not extra.is_dir():
            raise DefinitionError(f"{extra}: extra app directory not found")
        app_paths += sorted(extra.glob("*.yaml"))
    root = defs_dir.parent
    structs_by_name = {s.name: s for s in structs}
    apps = [_parse_app(p, by_name, structs_by_name, root) for p in app_paths]

    names = {}
    for app in apps:
        if app.name in names:
            raise DefinitionError(
                f"app {app.name!r} is defined in both {names[app.name]} and {app.source}")
        names[app.name] = app.source

    _resolve_sends(apps)

    owners = {}
    for app in apps:
        if app.id in owners:
            raise DefinitionError(
                f"app id {app.id} is used by both {owners[app.id]} and {app.name}")
        owners[app.id] = app.name

    # A name built from one app can equal a name from another app, or from
    # common.yaml: app foo's command bar_baz and app foo_bar's command baz
    # are both foo_bar_baz. C would reject that with a confusing error.
    defined = {}
    common_names = [s.name for s in structs] + [e.name for e in enums] + \
        [v.constant for e in enums for v in e.values] + [e.max_constant for e in enums] + \
        ["app_id", "APP_COUNT", "APP_WAKEUP_COUNT", "APP_ID_MAX", "APP_SEND_PAIR_COUNT"]
    for source, names in [("common.yaml", common_names)] + \
            [(a.source, a.generated_names()) for a in apps]:
        for n in names:
            if n in defined:
                raise DefinitionError(
                    f"generated name {n!r} comes from both {defined[n]} and {source}; "
                    "rename one of them")
            defined[n] = source

    nvm_path = defs_dir / "nvm_map.yaml"
    nvm = _parse_nvm_map(nvm_path, apps, by_name) if nvm_path.is_file() else NvmMap(0, [])
    return Definitions(enums, structs, apps, nvm)


# --- Output ---------------------------------------------------------------


def _c_comment(text, indent=""):
    """text as a C comment, wrapped to fit 80 columns."""
    width = 80 - len(indent.expandtabs(8)) - 3
    lines = textwrap.wrap(text, width=width)
    if len(lines) == 1 and len(lines[0]) <= width - 4:
        return f"{indent}/** {lines[0]} */"
    return "\n".join([f"{indent}/**"] + [f"{indent} * {line}" for line in lines]
                     + [f"{indent} */"])


def render(defs):
    """Return {relative output path: file text} for every generated file."""
    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(TEMPLATE_DIR),
        undefined=jinja2.StrictUndefined,
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )
    env.filters["comment"] = _c_comment
    env.filters["unsigned_max"] = lambda t: UNSIGNED_MAX[t]
    env.filters["signed_min"] = lambda t: SIGNED_RANGE[t][0]
    env.filters["signed_max"] = lambda t: SIGNED_RANGE[t][1]

    outputs = {
        Path("include/msg/common.h"): env.get_template("common.h.j2").render(defs=defs),
    }
    for app in defs.apps:
        outputs[Path(f"include/msg/{app.name}.h")] = \
            env.get_template("app.h.j2").render(app=app)
        outputs[Path(f"src/msg_{app.name}.c")] = \
            env.get_template("app_channels.c.j2").render(app=app)
    outputs[Path("src/cmd_routes.c")] = \
        env.get_template("cmd_routes.c.j2").render(defs=defs)
    outputs[Path("src/tlm_encode.c")] = \
        env.get_template("tlm_encode.c.j2").render(defs=defs)
    # One per app, with or without records, so the build knows the outputs
    # from the app files alone.
    for app in defs.apps:
        outputs[Path(f"include/nvm/{app.name}.h")] = env.get_template("nvm_owner.h.j2").render(
            app=app, regions=defs.nvm.regions_of(app), enums=defs.enums_by_name)
    return outputs


def _field_entry(field, defs):
    entry = {"name": field.name, "type": field.type, "kind": field.text_kind}
    if field.enum:
        entry["values"] = [v.name for v in defs.enums_by_name[field.enum].values]
    elif field.type in UNSIGNED_MAX:
        bits = int(field.type[4:])
        entry["min"], entry["max"] = 0, 2**bits - 1
    elif field.type in SIGNED_RANGE:
        bits = int(field.type[3:])
        entry["min"], entry["max"] = -(2**(bits - 1)), 2**(bits - 1) - 1
    if field.description:
        entry["description"] = field.description
    return entry


def _hk_entry(field, offset, defs):
    entry = {"name": field.name, "type": field.type, "offset": offset,
             "size": field.wire_size}
    if field.enum:
        entry["values"] = [{"name": v.name, "value": v.value}
                           for v in defs.enums_by_name[field.enum].values]
    if field.description:
        entry["description"] = field.description
    return entry


def command_dictionary(defs):
    """Every command, housekeeping layout, internal command, and FRAM record,
    resolved, as plain data (DS-61, DS-74).
    The ground formats command text and decodes telemetry from this; --json
    writes it for other languages."""
    return {
        "modes": [v.name for v in defs.enums_by_name["mode"].values],
        "apps": [{
            "name": app.name,
            "id": app.id,
            "commands": [{
                "name": c.name,
                "id": c.id,
                "description": c.description,
                "modes": c.modes,
                "args": [_field_entry(f, defs) for f in c.fields],
            } for c in app.commands],
            "sends": app.send_names,
            "housekeeping": {
                "size": app.housekeeping.wire_size,
                "fields": [_hk_entry(f, offset, defs)
                           for f, offset in app.housekeeping.wire_layout],
            },
        } for app in defs.apps],
        # The FRAM map, for the ground's dump decoder (DS-74).
        "nvm": {
            "fram_size": defs.nvm.fram_size,
            "mirror_size": defs.nvm.mirror_size,
            "header_len": NVM_HEADER_LEN,
            "crc_len": NVM_CRC_LEN,
            "regions": [{
                "name": r.name,
                "owner": r.owner.name,
                "address": r.address,
                "slot_size": r.slot_size,
                "version": r.version,
                "size": r.size,
                "mirror_address": r.mirror_address if r.mirrored else None,
                "ring": r.ring or None,
                "fields": [_hk_entry(f, offset, defs) for f, offset in r.struct.wire_layout],
            } for r in defs.nvm.regions],
        },
    }


def write_outputs(outputs, out_dir):
    out_dir = Path(out_dir)
    # Start clean, so a removed app leaves no stale header behind.
    for sub in ("include", "src"):
        shutil.rmtree(out_dir / sub, ignore_errors=True)
    for rel, text in outputs.items():
        path = out_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--defs", type=Path, required=True,
                        help="directory holding common.yaml and apps/")
    parser.add_argument("--out", type=Path,
                        help="output directory; leave out to only check the definitions")
    parser.add_argument("--extra-apps", type=Path, action="append", default=[],
                        metavar="DIR", help="a directory of test-only <app>.yaml files")
    parser.add_argument("--json", type=Path, metavar="FILE",
                        help="also write the command dictionary as JSON (DS-61)")
    args = parser.parse_args(argv)

    try:
        defs = load_definitions(args.defs, args.extra_apps)
    except DefinitionError as e:
        print(f"msggen: error: {e}", file=sys.stderr)
        return 1
    if args.out:
        write_outputs(render(defs), args.out)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(command_dictionary(defs), indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
