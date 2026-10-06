# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the message generator. Run with `make test-python`.

Most tests build a small set of definitions in a temporary directory, so
they do not change when the flight definitions change. One test checks the
flight definitions themselves.
"""

import copy
import os
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

MESSAGES_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MESSAGES_DIR / "generator"))

import msggen  # noqa: E402

MODES = ["safe", "nominal"]

COMMON = {
    "enums": [{
        "name": "severity",
        "type": "uint8",
        "description": "How serious an event is.",
        "values": [{"name": "info", "value": 1}, {"name": "error", "value": 3}],
    }, {
        "name": "mode",
        "type": "uint8",
        "description": "Spacecraft mode.",
        "values": [{"name": "safe", "value": 0}, {"name": "nominal", "value": 1}],
    }],
    "structs": [
        {"name": "frame_tick", "description": "The tick.", "fields": [
            {"name": "count", "type": "uint32"},
            {"name": "slot", "type": "uint8"},
        ]},
        {"name": "app_status", "description": "Status.", "fields": [
            {"name": "steps", "type": "uint32"},
            {"name": "cmd_accepted", "type": "uint32"},
            {"name": "cmd_rejected", "type": "uint32"},
        ]},
        {"name": "event", "description": "An event.", "fields": [
            {"name": "severity", "type": "severity"},
        ]},
    ],
}

SENSOR = {
    "app": "sensor",
    "id": 2,
    "description": "A frame-driven app.",
    "wakeup": True,
    "commands": [
        {"name": "set_rate", "id": 1, "description": "Set the rate.", "modes": MODES,
         "fields": [{"name": "hz", "type": "uint16", "description": "Rate."}]},
        {"name": "reinit", "id": 2, "description": "Reinitialize.", "modes": MODES},
    ],
    "housekeeping": {"description": "Sensor housekeeping.", "fields": [
        {"name": "reads", "type": "uint32"},
        {"name": "last_severity", "type": "severity"},
    ]},
}

QUIET = {
    "app": "quiet",
    "id": 3,
    "description": "An app with no commands and no wakeup.",
    "wakeup": False,
    "housekeeping": {"description": "Quiet housekeeping.", "fields": [
        {"name": "count", "type": "uint8"},
    ]},
}


def write_defs(root, common=None, apps=None, raw=None, nvm=None):
    """Write definitions under root and return root.

    common and apps default to the valid set above. raw maps a relative
    path to literal file text, for tests that need invalid YAML.
    """
    (root / "apps").mkdir(parents=True, exist_ok=True)
    (root / "common.yaml").write_text(yaml.safe_dump(COMMON if common is None else common))
    for app in [SENSOR, QUIET] if apps is None else apps:
        (root / "apps" / f"{app['app']}.yaml").write_text(yaml.safe_dump(app))
    if nvm is not None:
        (root / "nvm_map.yaml").write_text(yaml.safe_dump(nvm))
    for rel, text in (raw or {}).items():
        (root / rel).write_text(text)
    return root


def generate_paths(root):
    return msggen.render(msggen.load_definitions(root))


def generate(root):
    """Generated files keyed by their path as a string."""
    return {str(k): v for k, v in generate_paths(root).items()}


def app_with(**changes):
    """A copy of SENSOR with some keys changed; a value of None removes the key."""
    app = copy.deepcopy(SENSOR)
    for key, value in changes.items():
        if value is None:
            app.pop(key)
        else:
            app[key] = value
    return app


# --- Valid definitions ----------------------------------------------------


def test_flight_definitions_are_valid():
    defs = msggen.load_definitions(MESSAGES_DIR)
    assert "frame_manager" in [a.name for a in defs.apps]
    outputs = msggen.render(defs)
    assert Path("include/msg/frame_manager.h") in outputs


def test_outputs_one_header_and_one_source_per_app(tmp_path):
    outputs = generate(write_defs(tmp_path))
    assert sorted(outputs) == [
        "include/msg/common.h",
        "include/msg/quiet.h",
        "include/msg/sensor.h",
        "include/nvm/map.h",
        "include/nvm/quiet.h",
        "include/nvm/sensor.h",
        "src/cmd_routes.c",
        "src/msg_quiet.c",
        "src/msg_sensor.c",
        "src/tlm_encode.c",
    ]


def test_output_is_deterministic(tmp_path):
    root = write_defs(tmp_path)
    assert generate(root) == generate(root)


def test_common_header(tmp_path):
    common = generate(write_defs(tmp_path))["include/msg/common.h"]
    assert "APP_ID_SENSOR = 2," in common
    assert "APP_ID_QUIET = 3," in common
    assert "SEVERITY_ERROR = 3," in common
    assert "struct frame_tick {" in common
    # An enum field is stored as its fixed-width type, not the C enum.
    assert "uint8_t severity; /* enum severity */" in common
    # Every app's status channel; wakeup channels only for frame-driven apps.
    assert "ZBUS_CHAN_DECLARE(sensor_status_chan);" in common
    assert "ZBUS_CHAN_DECLARE(quiet_status_chan);" in common
    assert "ZBUS_CHAN_DECLARE(sensor_wakeup_chan);" in common
    assert "quiet_wakeup_chan" not in common


def test_app_counts(tmp_path):
    common = generate(write_defs(tmp_path))["include/msg/common.h"]
    assert "#define APP_COUNT 2" in common
    assert "#define APP_WAKEUP_COUNT 1" in common
    assert "#define APP_ID_MAX 3" in common


def test_extra_app_dirs_add_test_apps(tmp_path):
    root = write_defs(tmp_path / "messages", apps=[QUIET])
    extra = tmp_path / "tests" / "apps"
    extra.mkdir(parents=True)
    (extra / "sensor.yaml").write_text(yaml.safe_dump(SENSOR))

    outputs = {str(k): v for k, v in
               msggen.render(msggen.load_definitions(root, [extra])).items()}
    assert "include/msg/sensor.h" in outputs
    assert "#define APP_COUNT 2" in outputs["include/msg/common.h"]
    # The banner names the file relative to the repository.
    assert "from tests/apps/sensor.yaml" in outputs["src/msg_sensor.c"]


def test_extra_app_dir_must_exist(tmp_path):
    root = write_defs(tmp_path)
    with pytest.raises(msggen.DefinitionError, match="extra app directory not found"):
        msggen.load_definitions(root, [tmp_path / "missing"])


def test_app_defined_twice(tmp_path):
    root = write_defs(tmp_path / "messages")
    extra = tmp_path / "extra"
    extra.mkdir()
    (extra / "sensor.yaml").write_text(yaml.safe_dump({**SENSOR, "id": 9}))
    with pytest.raises(msggen.DefinitionError, match="app 'sensor' is defined in both"):
        msggen.load_definitions(root, [extra])


def test_main_accepts_extra_apps(tmp_path):
    root = write_defs(tmp_path / "messages", apps=[QUIET])
    extra = tmp_path / "extra"
    extra.mkdir()
    (extra / "sensor.yaml").write_text(yaml.safe_dump(SENSOR))
    out = tmp_path / "out"
    assert msggen.main(["--defs", str(root), "--extra-apps", str(extra),
                        "--out", str(out)]) == 0
    assert (out / "include/msg/sensor.h").exists()


def test_enum_max(tmp_path):
    common = generate(write_defs(tmp_path))["include/msg/common.h"]
    assert "#define SEVERITY_MAX 3" in common


DATA = [{"name": "sensor_data_chan", "type": "event", "description": "Sensor data."}]


def test_data_channel(tmp_path):
    outputs = generate(write_defs(tmp_path, apps=[app_with(data_channels=DATA), QUIET]))
    # Declared where every reader can see it, defined in the owner's file.
    assert "ZBUS_CHAN_DECLARE(sensor_data_chan); /* owned by sensor */" \
        in outputs["include/msg/common.h"]
    source = outputs["src/msg_sensor.c"]
    assert "ZBUS_CHAN_DEFINE(sensor_data_chan, struct event," in source
    assert "BUILD_ASSERT(sizeof(struct event) <=" in source
    assert " *   sensor_data_chan" in outputs["include/msg/sensor.h"]


def test_app_header(tmp_path):
    header = generate(write_defs(tmp_path))["include/msg/sensor.h"]
    assert '#include "msg/common.h"' in header
    assert "SENSOR_CMD_SET_RATE = 1," in header
    assert "SENSOR_CMD_REINIT = 2," in header
    assert '#define SENSOR_APP_NAME "sensor"' in header
    assert '#define SENSOR_CMD_SET_RATE_NAME "set_rate"' in header
    assert "struct sensor_set_rate {" in header
    # A command without fields gets no arguments struct.
    assert "struct sensor_reinit" not in header
    assert "struct sensor_set_rate set_rate;" in header
    assert "struct sensor_hk {" in header
    assert "uint8_t last_severity; /* enum severity */" in header
    # The receive union holds the command and, for a frame-driven app, the tick.
    assert "union sensor_msg {" in header
    assert "struct sensor_cmd cmd;" in header
    assert "struct frame_tick tick;" in header
    assert "ZBUS_CHAN_DECLARE(sensor_cmd_chan);" in header
    assert "ZBUS_CHAN_DECLARE(sensor_hk_chan);" in header


def test_app_channels(tmp_path):
    source = generate(write_defs(tmp_path))["src/msg_sensor.c"]
    assert '#include "msg/sensor.h"' in source
    for chan, ctype in [("sensor_cmd_chan", "struct sensor_cmd"),
                        ("sensor_hk_chan", "struct sensor_hk"),
                        ("sensor_status_chan", "struct app_status"),
                        ("sensor_wakeup_chan", "struct frame_tick")]:
        assert f"ZBUS_CHAN_DEFINE({chan}, {ctype}," in source


def test_app_without_commands_or_wakeup(tmp_path):
    outputs = generate(write_defs(tmp_path))
    header = outputs["include/msg/quiet.h"]
    assert "enum quiet_cmd_id" not in header
    assert "args;" not in header
    assert "struct frame_tick tick;" not in header
    assert "wakeup_chan" not in outputs["src/msg_quiet.c"]


def test_commands_all_without_fields_have_no_args_union(tmp_path):
    app = app_with(commands=[{"name": "reinit", "id": 1, "description": "Reinitialize.", "modes": MODES}])
    header = generate(write_defs(tmp_path, apps=[app]))["include/msg/sensor.h"]
    assert "SENSOR_CMD_REINIT = 1," in header
    assert "args;" not in header


def test_multiline_description_becomes_one_comment(tmp_path):
    app = app_with(housekeeping={
        "description": "First line\nsecond line.",
        "fields": [{"name": "reads", "type": "uint32"}],
    })
    header = generate(write_defs(tmp_path, apps=[app]))["include/msg/sensor.h"]
    assert "/** First line second line. */" in header


REPO_INCLUDE = MESSAGES_DIR.parent / "include"


def compile_generated(tmp_path, data_size, apps=None, common=None, nvm=None):
    """Generate the test definitions and compile each source with strict
    warnings against stand-in Zephyr headers. Returns {file name: gcc result}.

    The stand-ins check only that the output is valid C (for example, no
    empty enum or union when an app has no commands) and that the size
    checks work. The native_sim tests in tests/unit/libs check the output
    against real zbus.
    """
    out = tmp_path / "out"
    msggen.write_outputs(
        generate_paths(write_defs(tmp_path / "defs", common=common, apps=apps, nvm=nvm)), out)
    # Headers compile only when included: one source includes every FRAM header.
    (out / "src" / "nvm_headers.c").write_text("".join(
        f'#include "nvm/{h.name}"\n' for h in sorted((out / "include" / "nvm").glob("*.h")))
        + "typedef int never_empty; /* ISO C forbids an empty file */\n")
    stub = out / "include" / "zephyr"
    (stub / "zbus").mkdir(parents=True)
    (stub / "sys").mkdir()
    (stub / "zbus" / "zbus.h").write_text(
        "#pragma once\n"
        "struct zbus_channel { int unused; };\n"
        "typedef int k_timeout_t;\n"
        "#define K_NO_WAIT 0\n"
        "int zbus_chan_pub(const struct zbus_channel *chan, const void *msg, k_timeout_t t);\n"
        "int zbus_chan_read(const struct zbus_channel *chan, void *msg, k_timeout_t t);\n"
        "#define ZBUS_CHAN_DECLARE(name) extern const struct zbus_channel name\n"
        "#define ZBUS_OBSERVERS_EMPTY\n"
        "#define ZBUS_MSG_INIT(val, ...) {val, ##__VA_ARGS__}\n"
        "#define ZBUS_CHAN_DEFINE(name, type, v, u, o, init) type name##_msg = init\n")
    # The real resource map names the flight apps, which these tests replace.
    (out / "include" / "silversat").mkdir()
    (out / "include" / "silversat" / "resource_map.h").write_text(
        "#pragma once\n#define CMD_MAX_PENDING 2\n")
    (stub / "sys" / "byteorder.h").write_text(
        "#pragma once\n#include <stdint.h>\n"
        "static inline uint16_t sys_get_le16(const uint8_t *s) { return s[0] | s[1] << 8; }\n"
        "static inline uint32_t sys_get_le32(const uint8_t *s) "
        "{ return sys_get_le16(s) | (uint32_t)sys_get_le16(s + 2) << 16; }\n"
        "static inline uint64_t sys_get_le64(const uint8_t *s) "
        "{ return sys_get_le32(s) | (uint64_t)sys_get_le32(s + 4) << 32; }\n"
        "static inline void sys_put_le16(uint16_t v, uint8_t *d) { d[0] = v; d[1] = v >> 8; }\n"
        "static inline void sys_put_le32(uint32_t v, uint8_t *d) "
        "{ sys_put_le16(v, d); sys_put_le16(v >> 16, d + 2); }\n"
        "static inline void sys_put_le64(uint64_t v, uint8_t *d) "
        "{ sys_put_le32(v, d); sys_put_le32(v >> 32, d + 4); }\n")
    (stub / "sys" / "util.h").write_text(
        "#pragma once\n"
        "#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)\n"
        "#define BIT(n) (1UL << (n))\n"
        "#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n"
        "#define ARG_UNUSED(x) (void)(x)\n")
    return {source.name: subprocess.run(
        # A real compile, not -fsyntax-only, which skips some warnings
        # (unused static functions among them).
        ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", "-o", os.devnull,
         "-DCONFIG_ZBUS_MSG_SUBSCRIBER_BUF_ALLOC_STATIC=1",
         f"-DCONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE={data_size}",
         "-I", str(out / "include"), "-I", str(REPO_INCLUDE), str(source)],
        capture_output=True, text=True)
        for source in sorted((out / "src").glob("*.c"))}


needs_gcc = pytest.mark.skipif(shutil.which("gcc") is None, reason="needs a host C compiler")


@needs_gcc
def test_generated_c_compiles(tmp_path):
    results = compile_generated(tmp_path, data_size=64)
    assert "cmd_routes.c" in results
    for name, result in results.items():
        assert result.returncode == 0, f"{name}: {result.stderr}"


@needs_gcc
def test_routes_compile_with_no_commands(tmp_path):
    result = compile_generated(tmp_path, data_size=64, apps=[QUIET])["cmd_routes.c"]
    assert result.returncode == 0, result.stderr


def test_routes(tmp_path):
    routes = generate(write_defs(tmp_path))["src/cmd_routes.c"]
    assert '#include "msg/sensor.h"' in routes
    assert '#include "msg/quiet.h"' in routes
    # Every app is known, even one with no commands.
    assert '{"quiet", APP_ID_QUIET},' in routes
    assert ".command = \"set_rate\"," in routes
    assert ".modes = BIT(MODE_SAFE) | BIT(MODE_NOMINAL)," in routes
    assert ".arg_count = 1," in routes
    assert "cmd_text_unsigned(t, 2, UINT16_MAX, &value)" in routes
    assert "return deliver(APP_ID_SENSOR, &sensor_cmd_chan, &sensor_status_chan, &cmd);" in routes


def test_routes_decode_each_kind(tmp_path):
    fields = [{"name": "u", "type": "uint32"}, {"name": "s", "type": "int8"},
              {"name": "b", "type": "bool"}, {"name": "m", "type": "mode"}]
    app = app_with(commands=[{"name": "all", "id": 1, "description": "d",
                              "modes": ["nominal"], "fields": fields}])
    routes = generate(write_defs(tmp_path, apps=[app]))["src/cmd_routes.c"]
    assert "cmd_text_unsigned(t, 2, UINT32_MAX, &value)" in routes
    assert "cmd_text_signed(t, 3, INT8_MIN, INT8_MAX," in routes
    assert "cmd_text_bool(t, 4, &value)" in routes
    assert "cmd_text_name(t, 5, mode_names, ARRAY_SIZE(mode_names)," in routes
    assert '{"nominal", MODE_NOMINAL},' in routes
    assert "info->bad_arg = 3;" in routes
    assert ".modes = BIT(MODE_NOMINAL)," in routes


def test_command_dictionary(tmp_path):
    defs = msggen.load_definitions(write_defs(tmp_path))
    d = msggen.command_dictionary(defs)
    assert d["modes"] == ["safe", "nominal"]
    sensor = next(a for a in d["apps"] if a["name"] == "sensor")
    set_rate = next(c for c in sensor["commands"] if c["name"] == "set_rate")
    assert set_rate["id"] == 1
    assert set_rate["modes"] == ["safe", "nominal"]
    assert set_rate["args"] == [{"name": "hz", "type": "uint16", "kind": "unsigned",
                                 "min": 0, "max": 65535, "description": "Rate."}]
    quiet = next(a for a in d["apps"] if a["name"] == "quiet")
    assert quiet["commands"] == []


def test_housekeeping_wire_layout(tmp_path):
    fields = [{"name": "b", "type": "bool"}, {"name": "u16", "type": "uint16"},
              {"name": "m", "type": "mode"}, {"name": "i64", "type": "int64"},
              {"name": "f", "type": "float32"}, {"name": "d", "type": "float64"}]
    app = app_with(housekeeping={"description": "d", "fields": fields})
    defs = msggen.load_definitions(write_defs(tmp_path, apps=[app]))
    hk = msggen.command_dictionary(defs)["apps"][0]["housekeeping"]
    # Packed in order, no padding (DS-64).
    assert [(f["name"], f["offset"], f["size"]) for f in hk["fields"]] == [
        ("b", 0, 1), ("u16", 1, 2), ("m", 3, 1), ("i64", 4, 8), ("f", 12, 4), ("d", 16, 8)]
    assert hk["size"] == 24
    assert hk["fields"][2]["values"] == [{"name": "safe", "value": 0},
                                         {"name": "nominal", "value": 1}]

    source = generate(write_defs(tmp_path / "again", apps=[app]))["src/tlm_encode.c"]
    assert "out[0] = hk.b ? 1 : 0;" in source
    assert "sys_put_le16((uint16_t)hk.u16, &out[1]);" in source
    assert "out[3] = (uint8_t)hk.m;" in source
    assert "sys_put_le64((uint64_t)hk.i64, &out[4]);" in source
    assert "put_f32(hk.f, &out[12]);" in source
    assert "put_f64(hk.d, &out[16]);" in source
    assert "return 24;" in source
    assert "{APP_ID_SENSOR, 24, encode_sensor}," in source


def test_housekeeping_must_fit_one_frame(tmp_path):
    fields = [{"name": f"x{i}", "type": "uint64"} for i in range(31)]   # 248 bytes
    app = app_with(housekeeping={"description": "d", "fields": fields})
    with pytest.raises(msggen.DefinitionError, match="248 bytes on the downlink"):
        msggen.load_definitions(write_defs(tmp_path, apps=[app]))
    fields = fields[:30] + [{"name": "y", "type": "uint32"}, {"name": "z", "type": "uint8"}]
    app = app_with(housekeeping={"description": "d", "fields": fields})
    msggen.load_definitions(write_defs(tmp_path / "fits", apps=[app]))   # 245 bytes


def test_command_dictionary_ranges_and_names(tmp_path):
    fields = [{"name": "s", "type": "int16"}, {"name": "m", "type": "mode"},
              {"name": "b", "type": "bool"}]
    app = app_with(commands=[{"name": "all", "id": 1, "description": "d",
                              "modes": ["safe"], "fields": fields}])
    d = msggen.command_dictionary(msggen.load_definitions(write_defs(tmp_path, apps=[app])))
    args = d["apps"][0]["commands"][0]["args"]
    assert args[0] == {"name": "s", "type": "int16", "kind": "signed",
                       "min": -32768, "max": 32767}
    assert args[1] == {"name": "m", "type": "mode", "kind": "enum",
                       "values": ["safe", "nominal"]}
    assert args[2] == {"name": "b", "type": "bool", "kind": "bool"}


def test_main_writes_json(tmp_path):
    root = write_defs(tmp_path / "defs")
    out = tmp_path / "dict" / "commands.json"
    assert msggen.main(["--defs", str(root), "--json", str(out)]) == 0
    assert json.loads(out.read_text())["modes"] == ["safe", "nominal"]


@needs_gcc
def test_message_too_big_for_zbus_buffer_fails_the_build(tmp_path):
    # sensor's housekeeping needs 24 bytes; quiet's largest message, its
    # 12-byte status, fits in 16.
    big_hk = app_with(housekeeping={"description": "d", "fields": [
        {"name": "a", "type": "uint64"}, {"name": "b", "type": "uint64"},
        {"name": "c", "type": "uint64"}]})
    results = compile_generated(tmp_path, data_size=16, apps=[big_hk, QUIET])
    quiet, sensor = results["msg_quiet.c"], results["msg_sensor.c"]
    assert quiet.returncode == 0, quiet.stderr
    assert sensor.returncode != 0
    assert "sensor_hk_chan: raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE" \
        in sensor.stderr


# --- Writing and the command line -----------------------------------------


def test_write_outputs_removes_stale_files(tmp_path):
    out = tmp_path / "out"
    msggen.write_outputs(generate_paths(write_defs(tmp_path / "a")), out)
    assert (out / "include/msg/quiet.h").exists()
    msggen.write_outputs(generate_paths(write_defs(tmp_path / "b", apps=[SENSOR])), out)
    assert not (out / "include/msg/quiet.h").exists()
    assert not (out / "src/msg_quiet.c").exists()
    assert (out / "include/msg/sensor.h").exists()


def test_main_writes_outputs(tmp_path):
    root = write_defs(tmp_path / "defs")
    assert msggen.main(["--defs", str(root), "--out", str(tmp_path / "out")]) == 0
    assert (tmp_path / "out/include/msg/common.h").exists()


def test_main_check_only_writes_nothing(tmp_path):
    root = write_defs(tmp_path / "defs")
    assert msggen.main(["--defs", str(root)]) == 0
    assert not (tmp_path / "out").exists()


def test_main_reports_errors(tmp_path, capsys):
    root = write_defs(tmp_path, apps=[app_with(id=0)])
    assert msggen.main(["--defs", str(root)]) == 1
    err = capsys.readouterr().err
    assert err.startswith("msggen: error: ")
    assert "sensor.yaml" in err


# --- Invalid definitions: each must fail with a message saying where ------


def field(name="x", type="uint8"):
    return {"name": name, "type": type}


APP_ERRORS = [
    # (description, changes to SENSOR, text expected in the error)
    ("missing key", {"description": None}, "missing description"),
    ("unknown key (a typo)", {"wakup": True}, "unknown key wakup"),
    ("name not snake_case", {"app": "Sensor"}, "not a valid name"),
    ("C keyword as field", {"housekeeping": {"description": "d", "fields": [field("int")]}},
     "C keyword"),
    ("unknown type", {"housekeeping": {"description": "d", "fields": [field(type="u8")]}},
     "unknown type 'u8'"),
    ("duplicate field", {"housekeeping": {"description": "d", "fields": [field(), field()]}},
     "duplicate field 'x'"),
    ("empty housekeeping", {"housekeeping": {"description": "d", "fields": []}},
     "at least one field"),
    ("app id zero", {"id": 0}, "outside 1..63"),
    ("app id too big", {"id": 64}, "outside 1..63"),
    ("app id is a bool", {"id": True}, "whole number"),
    ("wakeup not a bool", {"wakeup": "yes"}, "wakeup must be true"),
    ("commands not a list", {"commands": {"name": "x"}}, "must be a list"),
    ("duplicate command id", {"commands": [
        {"name": "a", "id": 1, "description": "d", "modes": MODES},
        {"name": "b", "id": 1, "description": "d", "modes": MODES}]}, "duplicate command id 1"),
    ("duplicate command name", {"commands": [
        {"name": "a", "id": 1, "description": "d", "modes": MODES},
        {"name": "a", "id": 2, "description": "d", "modes": MODES}]}, "duplicate command name 'a'"),
    ("command id zero", {"commands": [{"name": "a", "id": 0, "description": "d", "modes": MODES}]},
     "outside 1..65535"),
    ("modes missing", {"commands": [{"name": "a", "id": 1, "description": "d"}]},
     "list the modes this command is allowed in"),
    ("modes empty", {"commands": [{"name": "a", "id": 1, "description": "d", "modes": []}]},
     "list the modes this command is allowed in"),
    ("unknown mode", {"commands": [{"name": "a", "id": 1, "description": "d",
                                    "modes": ["standby"]}]}, "unknown mode 'standby'"),
    ("mode twice", {"commands": [{"name": "a", "id": 1, "description": "d",
                                  "modes": ["safe", "safe"]}]}, "listed twice"),
    ("float argument", {"commands": [{"name": "a", "id": 1, "description": "d", "modes": MODES,
                                      "fields": [field(type="float32")]}]},
     "float32 arguments are not supported in commands yet"),
    ("too many arguments", {"commands": [{"name": "a", "id": 1, "description": "d",
                                          "modes": MODES,
                                          "fields": [field(f"x{i}") for i in range(15)]}]},
     "at most 14 arguments"),
    ("reserved command name", {"commands": [{"name": "hk", "id": 1, "description": "d", "modes": MODES}]},
     "reserved"),
    ("empty description", {"description": "  "}, "non-empty text"),
    ("data channel type not shared", {"data_channels": [
        {"name": "sensor_data_chan", "type": "sensor_hk", "description": "d"}]},
     "not a struct in common.yaml"),
    ("data channel name", {"data_channels": [
        {"name": "sensor_data", "type": "event", "description": "d"}]}, "must end in _chan"),
    ("duplicate data channel", {"data_channels": DATA + DATA},
     "duplicate data channel 'sensor_data_chan'"),
    ("send without a command", {"sends": ["radio"]}, "should be <app>.<command>"),
    ("send not a string", {"sends": [{"radio": "x"}]}, "should be <app>.<command>"),
    ("send to itself", {"sends": ["sensor.reinit"]}, "doesn't send commands to itself"),
    ("send listed twice", {"sends": ["radio.x", "radio.x"]}, "listed twice"),
    ("send to an unknown app", {"sends": ["radio.set_transmit"]}, "there is no app 'radio'"),
]


@pytest.mark.parametrize("changes, expected",
                         [pytest.param(c, e, id=d) for d, c, e in APP_ERRORS])
def test_invalid_app(tmp_path, changes, expected):
    app = app_with(**changes)
    # Keep the file name in step with a renamed app, so only `changes` is wrong.
    root = write_defs(tmp_path, apps=[])
    (root / "apps" / "sensor.yaml").write_text(yaml.safe_dump(app))
    with pytest.raises(msggen.DefinitionError, match="sensor.yaml") as e:
        msggen.load_definitions(root)
    assert expected in str(e.value)


def common_with(**changes):
    common = copy.deepcopy(COMMON)
    common.update(changes)
    return common


def enum(values, type="uint8"):
    return [{"name": "severity", "type": type, "description": "d", "values": values}]


COMMON_ERRORS = [
    ("missing frame_tick", {"structs": COMMON["structs"][1:]}, "must define struct 'frame_tick'"),
    ("missing mode", {"enums": COMMON["enums"][:1]}, "must define enum 'mode'"),
    ("status without counters", {"structs": [
        COMMON["structs"][0],
        {"name": "app_status", "description": "d", "fields": [field("steps", "uint32")]},
    ] + COMMON["structs"][2:]}, "must have a uint32 field 'cmd_accepted'"),
    ("enum value too big", {"enums": enum([{"name": "a", "value": 256}])}, "outside 0..255"),
    ("negative enum value", {"enums": enum([{"name": "a", "value": -1}])}, "outside 0..255"),
    ("duplicate enum value", {"enums": enum([{"name": "a", "value": 1},
                                             {"name": "b", "value": 1}])}, "duplicate value 1"),
    ("signed enum storage", {"enums": enum([{"name": "a", "value": 1}], type="int8")},
     "stored as one of"),
    ("enum with no values", {"enums": enum([])}, "at least one value"),
    ("enum value named max", {"enums": enum([{"name": "max", "value": 1}])},
     "emits SEVERITY_MAX"),
    ("duplicate struct", {"structs": COMMON["structs"] + [COMMON["structs"][0]]},
     "duplicate struct 'frame_tick'"),
]


@pytest.mark.parametrize("changes, expected",
                         [pytest.param(c, e, id=d) for d, c, e in COMMON_ERRORS])
def test_invalid_common(tmp_path, changes, expected):
    root = write_defs(tmp_path, common=common_with(**changes))
    with pytest.raises(msggen.DefinitionError, match="common.yaml") as e:
        msggen.load_definitions(root)
    assert expected in str(e.value)


def test_invalid_yaml(tmp_path):
    root = write_defs(tmp_path, raw={"apps/sensor.yaml": "app: [unclosed\n"})
    with pytest.raises(msggen.DefinitionError, match="not valid YAML"):
        msggen.load_definitions(root)


def test_file_name_must_match_app(tmp_path):
    root = write_defs(tmp_path, apps=[])
    (root / "apps" / "sensors.yaml").write_text(yaml.safe_dump(SENSOR))
    with pytest.raises(msggen.DefinitionError, match="rename it to sensor.yaml"):
        msggen.load_definitions(root)


def test_app_named_common_is_reserved(tmp_path):
    root = write_defs(tmp_path, apps=[app_with(app="common")])
    with pytest.raises(msggen.DefinitionError, match="reserved"):
        msggen.load_definitions(root)


def test_duplicate_app_id_across_files(tmp_path):
    root = write_defs(tmp_path, apps=[SENSOR, {**QUIET, "id": SENSOR["id"]}])
    with pytest.raises(msggen.DefinitionError, match="app id 2 is used by both"):
        msggen.load_definitions(root)


def test_generated_names_collide_across_apps(tmp_path):
    # foo's command bar_baz and foo_bar's command baz both make struct foo_bar_baz.
    foo = app_with(app="foo", id=4, commands=[
        {"name": "bar_baz", "id": 1, "description": "d", "modes": MODES, "fields": [field()]}])
    foo_bar = app_with(app="foo_bar", id=5, commands=[
        {"name": "baz", "id": 1, "description": "d", "modes": MODES, "fields": [field()]}])
    root = write_defs(tmp_path, apps=[foo, foo_bar])
    with pytest.raises(msggen.DefinitionError, match="generated name 'foo_bar_baz'"):
        msggen.load_definitions(root)


def test_generated_name_collides_with_common(tmp_path):
    common = common_with(structs=COMMON["structs"] + [
        {"name": "sensor_hk", "description": "d", "fields": [field()]}])
    root = write_defs(tmp_path, common=common)
    with pytest.raises(msggen.DefinitionError, match="generated name 'sensor_hk'"):
        msggen.load_definitions(root)


def test_data_channel_collides_with_generated_name(tmp_path):
    app = app_with(data_channels=[
        {"name": "sensor_hk_chan", "type": "event", "description": "d"}])
    with pytest.raises(msggen.DefinitionError, match="generated name 'sensor_hk_chan'"):
        msggen.load_definitions(write_defs(tmp_path, apps=[app]))


def test_no_apps(tmp_path):
    root = write_defs(tmp_path, apps=[])
    with pytest.raises(msggen.DefinitionError, match="no app definitions"):
        msggen.load_definitions(root)


# --- Internal command senders (DS-68) -------------------------------------

# QUIET, but sending both of SENSOR's commands.
SENDER = {**QUIET, "sends": ["sensor.set_rate", "sensor.reinit"]}


def test_senders(tmp_path):
    out = generate(write_defs(tmp_path, apps=[SENSOR, SENDER]))
    header = out["include/msg/quiet.h"]
    assert "int send_sensor_set_rate(uint16_t hz);" in header
    assert "int send_sensor_reinit(void);" in header
    assert '#include "msg/sensor.h"' not in header, "the sender's header stays its own (DS-68)"

    source = out["src/msg_quiet.c"]
    assert '#include "msg/sensor.h"' in source
    assert source.count("static struct cmd_pending pending_sensor;") == 1, "one per target app"
    assert "cmd.args.set_rate.hz = hz;" in source
    assert ("return cmd_deliver(&pending_sensor, &sensor_cmd_chan,\n"
            "\t\t\t   &sensor_status_chan, &cmd);") in source

    assert "#define APP_SEND_PAIR_COUNT 1" in out["include/msg/common.h"]
    assert "send_" not in out["include/msg/sensor.h"], "only the sender gets senders"


def test_send_pair_count(tmp_path):
    out = generate(write_defs(tmp_path))
    assert "#define APP_SEND_PAIR_COUNT 0" in out["include/msg/common.h"]


@needs_gcc
def test_senders_compile(tmp_path):
    results = compile_generated(tmp_path, data_size=64, apps=[SENSOR, SENDER])
    for name, result in results.items():
        assert result.returncode == 0, f"{name}: {result.stderr}"


def test_dictionary_lists_sends(tmp_path):
    dictionary = msggen.command_dictionary(
        msggen.load_definitions(write_defs(tmp_path, apps=[SENSOR, SENDER])))
    by_name = {a["name"]: a for a in dictionary["apps"]}
    assert by_name["quiet"]["sends"] == ["sensor.set_rate", "sensor.reinit"]
    assert by_name["sensor"]["sends"] == []


def test_send_unknown_command(tmp_path):
    root = write_defs(tmp_path, apps=[SENSOR, {**QUIET, "sends": ["sensor.ping"]}])
    with pytest.raises(msggen.DefinitionError, match="quiet.yaml") as e:
        msggen.load_definitions(root)
    assert "sensor has no command 'ping' (its commands: set_rate, reinit)" in str(e.value)


def test_send_argument_named_cmd(tmp_path):
    sensor = app_with(commands=[{"name": "go", "id": 1, "description": "d", "modes": MODES,
                                 "fields": [field("cmd")]}])
    root = write_defs(tmp_path, apps=[sensor, {**QUIET, "sends": ["sensor.go"]}])
    with pytest.raises(msggen.DefinitionError, match="argument named 'cmd'"):
        msggen.load_definitions(root)


# --- Initial values for data channels -------------------------------------

STATE = {"name": "state", "description": "A state.", "fields": [
    {"name": "mode", "type": "mode"}, {"name": "on", "type": "bool"},
    {"name": "level", "type": "int8"}, {"name": "gain", "type": "float32"},
]}


def defs_with_initial(tmp_path, initial):
    common = copy.deepcopy(COMMON)
    common["structs"].append(STATE)
    sensor = app_with(data_channels=[{"name": "state_chan", "type": "state",
                                      "description": "d", "initial": initial}])
    return write_defs(tmp_path, common=common, apps=[sensor, QUIET])


def test_data_channel_initial_value(tmp_path):
    out = generate(defs_with_initial(
        tmp_path, {"mode": "nominal", "on": True, "level": -3, "gain": 2}))
    assert ("ZBUS_MSG_INIT(.mode = MODE_NOMINAL, .on = true, .level = -3, .gain = 2.0)"
            in out["src/msg_sensor.c"])


def test_data_channel_without_initial_value_is_zero(tmp_path):
    out = generate(write_defs(tmp_path, apps=[app_with(data_channels=DATA), QUIET]))
    assert "ZBUS_MSG_INIT(0)" in out["src/msg_sensor.c"]


@pytest.mark.parametrize("initial, expected", [
    ({"speed": 1}, "state has no field 'speed' (its fields: mode, on, level, gain)"),
    ({"mode": "standby"}, "'standby' is not a mode; use one of safe, nominal"),
    ({"on": 1}, "expected true or false"),
    ({"level": 200}, "200 is outside -128..127"),
    ({"gain": "loud"}, "expected a number"),
    (["mode"], "expected field: value pairs"),
])
def test_data_channel_initial_value_errors(tmp_path, initial, expected):
    with pytest.raises(msggen.DefinitionError, match="sensor.yaml") as e:
        msggen.load_definitions(defs_with_initial(tmp_path, initial))
    assert expected in str(e.value)


@needs_gcc
def test_data_channel_initial_value_compiles(tmp_path):
    common = copy.deepcopy(COMMON)
    common["structs"].append(STATE)
    sensor = app_with(data_channels=[{"name": "state_chan", "type": "state",
                                      "description": "d", "initial": {"mode": "nominal"}}])
    results = compile_generated(tmp_path, data_size=64, apps=[sensor, QUIET], common=common)
    assert results["msg_sensor.c"].returncode == 0, results["msg_sensor.c"].stderr


# --- FRAM regions (DS-71, DS-74) --------------------------------------------

NVM = {
    "fram_size": 1024,
    "regions": [
        {"name": "sensor_cal", "owner": "sensor", "version": 2, "description": "Calibration.",
         "fields": [{"name": "gain", "type": "int8", "default": -3},
                    {"name": "offset", "type": "uint64", "default": 258},
                    {"name": "enabled", "type": "bool", "default": True},
                    {"name": "level", "type": "severity", "default": "error"}]},
        {"name": "sensor_count", "owner": "sensor", "version": 1, "description": "A count.",
         "fields": [{"name": "count", "type": "uint32"}]},
    ],
}


def nvm_defs(tmp_path, nvm=None):
    return msggen.load_definitions(write_defs(tmp_path, nvm=NVM if nvm is None else nvm))


def test_nvm_layout(tmp_path):
    cal, count = nvm_defs(tmp_path).nvm.regions
    assert (cal.size, cal.slot_size, cal.address) == (11, 23, 0)
    assert count.address == 48, "after two 23-byte slots, on a 16-byte boundary"
    assert (count.size, count.slot_size) == (4, 16)


def test_nvm_default_bytes(tmp_path):
    defs = nvm_defs(tmp_path)
    cal, count = defs.nvm.regions
    assert cal.default_bytes(defs.enums_by_name) == \
        bytes([0xfd]) + (258).to_bytes(8, "little") + bytes([1, 3])
    assert count.default_bytes(defs.enums_by_name) == bytes(4)


def test_nvm_headers(tmp_path):
    out = generate(write_defs(tmp_path, nvm=NVM))
    sensor = out["include/nvm/sensor.h"]
    assert "struct nvm_sensor_cal {" in sensor
    assert ".address = 0x0030," in sensor, "sensor_count's region"
    assert "static const uint8_t nvm_sensor_cal_defaults[11] = {" in sensor
    assert "253, 2, 1, 0, 0, 0, 0, 0, 0, 1, 3," in sensor
    assert "sys_put_le64((uint64_t)record->offset, &out[1]);" in sensor
    assert "record->gain = (int8_t)in[0];" in sensor
    assert "static inline int nvm_sensor_cal_write(" in sensor
    assert "has no FRAM records" in out["include/nvm/quiet.h"]
    assert "nvm_sensor" not in out["include/nvm/quiet.h"], "only the owner gets them"


def test_nvm_dictionary(tmp_path):
    nvm = msggen.command_dictionary(nvm_defs(tmp_path))["nvm"]
    assert nvm["fram_size"] == 1024
    assert [(r["name"], r["owner"], r["address"]) for r in nvm["regions"]] == \
        [("sensor_cal", "sensor", 0), ("sensor_count", "sensor", 48)]
    assert nvm["regions"][0]["fields"][1] == {"name": "offset", "type": "uint64", "offset": 1,
                                              "size": 8}


def test_no_nvm_map_means_no_regions(tmp_path):
    defs = msggen.load_definitions(write_defs(tmp_path))
    assert defs.nvm.regions == []


@needs_gcc
def test_nvm_headers_compile(tmp_path):
    results = compile_generated(tmp_path, data_size=64, nvm=NVM)
    assert results["nvm_headers.c"].returncode == 0, results["nvm_headers.c"].stderr


def nvm_with(**changes):
    nvm = copy.deepcopy(NVM)
    nvm["regions"][0].update(changes)
    return nvm


@pytest.mark.parametrize("nvm, expected", [
    (nvm_with(owner="nobody"), "there is no app 'nobody'"),
    (nvm_with(version=0), "outside 1..255"),
    (nvm_with(fields=[{"name": "x", "type": "float32"}]), "not supported in FRAM records"),
    (nvm_with(fields=[{"name": "x", "type": "uint8", "default": 300}]), "outside 0..255"),
    (nvm_with(fields=[{"name": "x", "type": "severity", "default": "loud"}]),
     "'loud' is not a severity"),
    (nvm_with(fields=[{"name": f"x{i}", "type": "uint64"} for i in range(17)]),
     "at most 128"),
    (nvm_with(name="sensor_count"), "duplicate region 'sensor_count'"),
    (nvm_with(colour="red"), "unknown key colour"),
    ({"fram_size": 40, "regions": NVM["regions"]}, "more than fram_size (40)"),
    ({"fram_size": 0x10001, "regions": []}, "outside 1..65536"),
])
def test_invalid_nvm_map(tmp_path, nvm, expected):
    with pytest.raises(msggen.DefinitionError, match="nvm_map.yaml") as e:
        nvm_defs(tmp_path, nvm)
    assert expected in str(e.value)


def test_flight_nvm_map():
    defs = msggen.load_definitions(MESSAGES_DIR)
    assert defs.nvm.fram_size == 32768
    assert {r.owner.name for r in defs.nvm.regions} <= {a.name for a in defs.apps}


# --- The mirror (DS-75's second tier) ---------------------------------------

def mirrored_nvm(mirror_size=4096):
    nvm = copy.deepcopy(NVM)
    nvm["mirror_size"] = mirror_size
    nvm["regions"][1]["mirror"] = True
    return nvm


def test_mirror_layout(tmp_path):
    cal, count = nvm_defs(tmp_path, mirrored_nvm()).nvm.regions
    assert not cal.mirrored
    assert (count.mirrored, count.mirror_address) == (True, 0), "first in the mirror"
    assert count.address == 48, "its FRAM address is unchanged"


def test_mirror_header_and_dictionary(tmp_path):
    out = generate(write_defs(tmp_path, nvm=mirrored_nvm()))
    sensor = out["include/nvm/sensor.h"]
    assert ".mirrored = true," in sensor
    assert ".mirror_address = 0x0000," in sensor
    assert sensor.count(".mirrored = true,") == 1, "only sensor_count"
    nvm = msggen.command_dictionary(nvm_defs(tmp_path / "d", mirrored_nvm()))["nvm"]
    assert nvm["mirror_size"] == 4096
    assert [r["mirror_address"] for r in nvm["regions"]] == [None, 0]


def test_mirror_must_fit(tmp_path):
    with pytest.raises(msggen.DefinitionError, match="more than mirror_size \\(16\\)"):
        nvm_defs(tmp_path, mirrored_nvm(mirror_size=16))


def test_mirror_flag_must_be_a_bool(tmp_path):
    with pytest.raises(msggen.DefinitionError, match="mirror: must be true or false"):
        nvm_defs(tmp_path, nvm_with(mirror="yes"))


def test_flight_mirror_holds_the_critical_records():
    defs = msggen.load_definitions(MESSAGES_DIR)
    mirrored = {r.name for r in defs.nvm.regions if r.mirrored}
    assert {"command_state", "mode_state", "deployment", "radio_state"} <= mirrored
    assert defs.nvm.mirror_used <= defs.nvm.mirror_size == 4096


# --- Rings (DS-71's boot log) -------------------------------------------------

def ring_nvm(**changes):
    nvm = copy.deepcopy(NVM)
    nvm["regions"][1].update({"ring": 4}, **changes)
    return nvm


def test_ring_layout(tmp_path):
    cal, count = nvm_defs(tmp_path, ring_nvm()).nvm.regions
    assert count.ring == 4
    assert count.region_size == 4 * count.slot_size == 64


def test_ring_header_and_dictionary(tmp_path):
    sensor = generate(write_defs(tmp_path, nvm=ring_nvm()))["include/nvm/sensor.h"]
    assert ".ring = 4," in sensor
    assert "static inline int nvm_sensor_count_write(uint32_t number," in sensor
    assert "static inline int nvm_sensor_count_read(uint8_t index," in sensor
    assert "nvm_ring_write(&nvm_sensor_count_region, number, payload)" in sensor
    assert "memcpy(payload, nvm_sensor_count_defaults, sizeof(payload));" in sensor, \
        "a ring read always fills the entry"
    assert "nvm_sensor_cal_write(const struct nvm_sensor_cal *record)" in sensor, \
        "a two-slot record keeps its usual functions"
    nvm = msggen.command_dictionary(nvm_defs(tmp_path / "d", ring_nvm()))["nvm"]
    assert [r["ring"] for r in nvm["regions"]] == [None, 4]


@needs_gcc
def test_ring_header_compiles(tmp_path):
    results = compile_generated(tmp_path, data_size=64, nvm=ring_nvm())
    assert results["nvm_headers.c"].returncode == 0, results["nvm_headers.c"].stderr


@pytest.mark.parametrize("changes, expected", [
    ({"ring": 1}, "outside 2..255"),
    ({"ring": 300}, "outside 2..255"),
])
def test_invalid_ring(tmp_path, changes, expected):
    nvm = copy.deepcopy(NVM)
    nvm["regions"][1].update(changes)
    with pytest.raises(msggen.DefinitionError, match="nvm_map.yaml") as e:
        nvm_defs(tmp_path, nvm)
    assert expected in str(e.value)


def test_a_mirrored_ring_takes_its_whole_size_in_the_mirror(tmp_path):
    nvm = ring_nvm(mirror=True)
    nvm["mirror_size"] = 4096
    cal, count = nvm_defs(tmp_path, nvm).nvm.regions
    assert count.mirrored and count.mirror_address == 0
    assert nvm_defs(tmp_path / "d", nvm).nvm.mirror_used == 4 * count.slot_size


def test_flight_boot_log_is_a_mirrored_ring():
    defs = msggen.load_definitions(MESSAGES_DIR)
    by_name = {r.name: r for r in defs.nvm.regions}
    assert by_name["boot_log"].ring == 16 and by_name["boot_log"].mirrored
    assert by_name["run_checkpoint"].mirrored
    assert defs.nvm.mirror_used <= defs.nvm.mirror_size


def test_nvm_map_header(tmp_path):
    out = generate(write_defs(tmp_path, nvm=mirrored_nvm()))["include/nvm/map.h"]
    assert "#define NVM_REGION_COUNT 2" in out
    assert "#define NVM_FRAM_SIZE 1024" in out and "#define NVM_MIRROR_SIZE 4096" in out
    assert '.name = "sensor_count",' in out and ".mirror_address = 0x0000," in out
    assert "_defaults" not in out, "read-only places only; the defaults stay with the owner"


def test_nvm_map_header_without_a_map(tmp_path):
    out = generate(write_defs(tmp_path))["include/nvm/map.h"]
    assert "#define NVM_REGION_COUNT 0" in out and "nvm_map[" not in out
