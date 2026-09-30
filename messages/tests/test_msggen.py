# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the message generator. Run with `make test-messages`.

Most tests build a small set of definitions in a temporary directory, so
they do not change when the flight definitions change. One test checks the
flight definitions themselves.
"""

import copy
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

MESSAGES_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MESSAGES_DIR / "generator"))

import msggen  # noqa: E402

COMMON = {
    "enums": [{
        "name": "severity",
        "type": "uint8",
        "description": "How serious an event is.",
        "values": [{"name": "info", "value": 1}, {"name": "error", "value": 3}],
    }],
    "structs": [
        {"name": "frame_tick", "description": "The tick.", "fields": [
            {"name": "count", "type": "uint32"},
            {"name": "slot", "type": "uint8"},
        ]},
        {"name": "app_status", "description": "Status.", "fields": [
            {"name": "steps", "type": "uint32"},
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
        {"name": "set_rate", "id": 1, "description": "Set the rate.",
         "fields": [{"name": "hz", "type": "uint16", "description": "Rate."}]},
        {"name": "reinit", "id": 2, "description": "Reinitialize."},
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


def write_defs(root, common=None, apps=None, raw=None):
    """Write definitions under root and return root.

    common and apps default to the valid set above. raw maps a relative
    path to literal file text, for tests that need invalid YAML.
    """
    (root / "apps").mkdir(parents=True, exist_ok=True)
    (root / "common.yaml").write_text(yaml.safe_dump(COMMON if common is None else common))
    for app in [SENSOR, QUIET] if apps is None else apps:
        (root / "apps" / f"{app['app']}.yaml").write_text(yaml.safe_dump(app))
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
        "src/msg_quiet.c",
        "src/msg_sensor.c",
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


def test_app_header(tmp_path):
    header = generate(write_defs(tmp_path))["include/msg/sensor.h"]
    assert '#include "msg/common.h"' in header
    assert "SENSOR_CMD_SET_RATE = 1," in header
    assert "SENSOR_CMD_REINIT = 2," in header
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
    app = app_with(commands=[{"name": "reinit", "id": 1, "description": "Reinitialize."}])
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


@pytest.mark.skipif(shutil.which("gcc") is None, reason="needs a host C compiler")
def test_generated_c_compiles(tmp_path):
    """Compile the output with strict warnings against a stand-in zbus.h.

    The stand-in checks only that the declarations are valid C (for example,
    no empty enum or union when an app has no commands). The native_sim test
    in tests/unit/messages checks them against real zbus.
    """
    out = tmp_path / "out"
    msggen.write_outputs(generate_paths(write_defs(tmp_path / "defs")), out)
    stub = out / "include" / "zephyr" / "zbus"
    stub.mkdir(parents=True)
    (stub / "zbus.h").write_text(
        "#define ZBUS_CHAN_DECLARE(name) extern const int name\n"
        "#define ZBUS_OBSERVERS_EMPTY\n"
        "#define ZBUS_MSG_INIT(val, ...) {val, ##__VA_ARGS__}\n"
        "#define ZBUS_CHAN_DEFINE(name, type, v, u, o, init) type name##_msg = init\n")
    for source in sorted((out / "src").glob("*.c")):
        result = subprocess.run(
            ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-fsyntax-only", "-I", str(out / "include"), str(source)],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr


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
    ("app id zero", {"id": 0}, "outside 1..255"),
    ("app id too big", {"id": 256}, "outside 1..255"),
    ("app id is a bool", {"id": True}, "whole number"),
    ("wakeup not a bool", {"wakeup": "yes"}, "wakeup must be true"),
    ("commands not a list", {"commands": {"name": "x"}}, "must be a list"),
    ("duplicate command id", {"commands": [
        {"name": "a", "id": 1, "description": "d"},
        {"name": "b", "id": 1, "description": "d"}]}, "duplicate command id 1"),
    ("duplicate command name", {"commands": [
        {"name": "a", "id": 1, "description": "d"},
        {"name": "a", "id": 2, "description": "d"}]}, "duplicate command name 'a'"),
    ("command id zero", {"commands": [{"name": "a", "id": 0, "description": "d"}]},
     "outside 1..65535"),
    ("reserved command name", {"commands": [{"name": "hk", "id": 1, "description": "d"}]},
     "reserved"),
    ("empty description", {"description": "  "}, "non-empty text"),
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
    ("enum value too big", {"enums": enum([{"name": "a", "value": 256}])}, "outside 0..255"),
    ("negative enum value", {"enums": enum([{"name": "a", "value": -1}])}, "outside 0..255"),
    ("duplicate enum value", {"enums": enum([{"name": "a", "value": 1},
                                             {"name": "b", "value": 1}])}, "duplicate value 1"),
    ("signed enum storage", {"enums": enum([{"name": "a", "value": 1}], type="int8")},
     "stored as one of"),
    ("enum with no values", {"enums": enum([])}, "at least one value"),
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
        {"name": "bar_baz", "id": 1, "description": "d", "fields": [field()]}])
    foo_bar = app_with(app="foo_bar", id=5, commands=[
        {"name": "baz", "id": 1, "description": "d", "fields": [field()]}])
    root = write_defs(tmp_path, apps=[foo, foo_bar])
    with pytest.raises(msggen.DefinitionError, match="generated name 'foo_bar_baz'"):
        msggen.load_definitions(root)


def test_generated_name_collides_with_common(tmp_path):
    common = common_with(structs=COMMON["structs"] + [
        {"name": "sensor_hk", "description": "d", "fields": [field()]}])
    root = write_defs(tmp_path, common=common)
    with pytest.raises(msggen.DefinitionError, match="generated name 'sensor_hk'"):
        msggen.load_definitions(root)


def test_no_apps(tmp_path):
    root = write_defs(tmp_path, apps=[])
    with pytest.raises(msggen.DefinitionError, match="no app definitions"):
        msggen.load_definitions(root)
