#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Message generator for the SilverSat flight software (DS-60, DS-61, DS-68).

Reads the YAML message definitions and writes C headers and zbus channel
definitions. The build runs this automatically (see messages/CMakeLists.txt).
The output goes in the build directory and is never committed.

    python3 msggen.py --defs messages --out build/silversat_msg

Input:
    <defs>/common.yaml       shared types     -> include/msg/common.h
    <defs>/apps/<app>.yaml   one app's types  -> include/msg/<app>.h
                             and its channels -> src/msg_<app>.c

Leave out --out to check the definitions without writing anything.

A test can add apps of its own with --extra-apps <dir>, a directory of
<app>.yaml files. They are generated exactly like the flight apps.

This version emits C declarations and channel definitions only. Encode and
decode functions, Python classes, the interface document, and golden vectors
come later, with command ingest.
"""

import argparse
import re
import shutil
import sys
import textwrap
from dataclasses import dataclass
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

# "common" would collide with msg/common.h.
RESERVED_APP_NAMES = {"common"}

# common.yaml must define these; every app's channels use them.
REQUIRED_STRUCTS = ("frame_tick", "app_status")

APP_ID_MAX = 0xFF          # struct event carries the app as uint8
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


@dataclass
class Struct:
    name: str
    description: str
    fields: list[Field]


@dataclass
class Command:
    name: str
    id: int
    description: str
    fields: list[Field]
    constant: str             # C name of the id, for example FRAME_MANAGER_CMD_SET_ENTRY_ENABLED
    struct: str               # C name of the arguments struct


@dataclass
class App:
    """One app, and every C name the generator derives from it (DS-14)."""

    name: str
    id: int
    description: str
    wakeup: bool
    commands: list[Command]
    housekeeping: Struct
    source: str               # the YAML file, for the generated banner and errors

    @property
    def id_constant(self):
        return f"APP_ID_{self.name.upper()}"

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
        return pairs

    @property
    def commands_with_args(self):
        return [c for c in self.commands if c.fields]

    def generated_names(self):
        """Every C identifier this app's generated code defines."""
        names = [self.id_constant, self.cmd_id_enum, self.cmd_struct,
                 self.hk_struct, self.msg_union, self.cmd_chan, self.hk_chan,
                 self.status_chan]
        if self.wakeup:
            names.append(self.wakeup_chan)
        for c in self.commands:
            names.append(c.constant)
            if c.fields:
                names.append(c.struct)
        return names


@dataclass
class Definitions:
    enums: list[Enum]
    structs: list[Struct]
    apps: list[App]

    @property
    def wakeup_apps(self):
        return [a for a in self.apps if a.wakeup]


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
    return enums, structs


def _parse_command(node, where, app_name, enums):
    _check_keys(node, where, ("name", "id", "description"), ("fields",))
    name = _name(node["name"], f"{where}.name")
    if name in RESERVED_COMMAND_NAMES:
        raise DefinitionError(
            f"{where}.name: {name!r} is reserved for the generator's own "
            f"{app_name}_{name} type")
    return Command(
        name=name,
        id=_integer(node["id"], f"{where}.id", 1, COMMAND_ID_MAX),
        description=_text(node["description"], f"{where}.description"),
        fields=_parse_fields(_sequence(node, "fields", where), f"{where}.fields", enums,
                             allow_empty=True),
        constant=f"{app_name}_cmd_{name}".upper(),
        struct=f"{app_name}_{name}",
    )


def _source_label(path, root):
    """path relative to root (the repository, in practice) if it is inside it."""
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def _parse_app(path, enums, root):
    data = _load_yaml(path)
    where = str(path)
    _check_keys(data, where, ("app", "id", "description", "wakeup", "housekeeping"),
                ("commands",))

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

    return App(
        name=name,
        id=_integer(data["id"], f"{where}: id", 1, APP_ID_MAX),
        description=_text(data["description"], f"{where}: description"),
        wakeup=data["wakeup"],
        commands=commands,
        housekeeping=housekeeping,
        source=_source_label(path, root),
    )


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
    apps = [_parse_app(p, by_name, root) for p in app_paths]

    names = {}
    for app in apps:
        if app.name in names:
            raise DefinitionError(
                f"app {app.name!r} is defined in both {names[app.name]} and {app.source}")
        names[app.name] = app.source

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
        [v.constant for e in enums for v in e.values] + \
        ["app_id", "APP_COUNT", "APP_WAKEUP_COUNT", "APP_ID_MAX"]
    for source, names in [("common.yaml", common_names)] + \
            [(a.source, a.generated_names()) for a in apps]:
        for n in names:
            if n in defined:
                raise DefinitionError(
                    f"generated name {n!r} comes from both {defined[n]} and {source}; "
                    "rename one of them")
            defined[n] = source

    return Definitions(enums, structs, apps)


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

    outputs = {
        Path("include/msg/common.h"): env.get_template("common.h.j2").render(defs=defs),
    }
    for app in defs.apps:
        outputs[Path(f"include/msg/{app.name}.h")] = \
            env.get_template("app.h.j2").render(app=app)
        outputs[Path(f"src/msg_{app.name}.c")] = \
            env.get_template("app_channels.c.j2").render(app=app)
    return outputs


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
    args = parser.parse_args(argv)

    try:
        defs = load_definitions(args.defs, args.extra_apps)
    except DefinitionError as e:
        print(f"msggen: error: {e}", file=sys.stderr)
        return 1
    if args.out:
        write_outputs(render(defs), args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
