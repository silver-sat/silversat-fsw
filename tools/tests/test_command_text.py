# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/command_text.py. Run with `make test-python`.

The shared vectors are the same ones the flight decoder is tested against
(tests/unit/command), so passing both means the ground and the satellite
agree on what every one of them means.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import command_text as ct  # noqa: E402

ROUTE_TEST = REPO / "tests" / "unit" / "command"
VECTORS = ct.load_vectors(ROUTE_TEST / "vectors.yaml")


@pytest.fixture(scope="module")
def dictionary():
    return ct.load_dictionary([ROUTE_TEST / "apps"])


@pytest.mark.parametrize("vector", VECTORS, ids=[f"{v['result']}:{v['text']!r}" for v in VECTORS])
def test_shared_vectors(dictionary, vector):
    parsed = ct.parse(dictionary, vector["text"], vector["mode"])
    assert parsed.result == vector["result"]
    if vector["result"] == "bad_arg":
        assert parsed.bad_arg == vector["bad_arg"]


def test_vectors_cover_every_result_from_text():
    # busy and publish_failed depend on the target app and zbus, not on the
    # text; the C tests cover them.
    assert {v["result"] for v in VECTORS} == set(ct.RESULTS) - {"busy", "publish_failed"}


def test_parse_values(dictionary):
    parsed = ct.parse(dictionary, "frame_manager set_entry_enabled nominal 3 false", "safe")
    assert (parsed.app, parsed.command, parsed.values) == (
        "frame_manager", "set_entry_enabled", ["nominal", 3, False])
    parsed = ct.parse(dictionary, "typed_app set_all 0 0 0 0 -128 -32768 -2147483648 "
                      "-9223372036854775808 true safe", "safe")
    assert parsed.values == [0, 0, 0, 0, -128, -32768, -2147483648, -2**63, True, "safe"]


def test_format_command(dictionary):
    assert ct.format_command(dictionary, "frame_manager", "set_entry_enabled",
                             "nominal", 3, False) == \
        "frame_manager set_entry_enabled nominal 3 false"
    assert ct.format_command(dictionary, "typed_app", "ping") == "typed_app ping"
    # A command allowed only in nominal mode still formats.
    assert ct.format_command(dictionary, "typed_app", "nominal_only", 7) == \
        "typed_app nominal_only 7"


@pytest.mark.parametrize("words, message", [
    (("no_such_app", "ping"), "unknown app"),
    (("typed_app", "pong"), "unknown command"),
    (("typed_app", "ping", 1), "bad arg count"),
    (("typed_app", "nominal_only", 256), "argument 1 is not valid"),
    (("frame_manager", "set_entry_enabled", "standby", 0, True), "argument 1 is not valid"),
    (("frame_manager", "set_entry_enabled", "safe", 0, "yes"), "argument 3 is not valid"),
])
def test_format_refuses_what_the_satellite_rejects(dictionary, words, message):
    with pytest.raises(ValueError, match=message):
        ct.format_command(dictionary, *words)


def test_format_refuses_text_too_long():
    long_app = {"modes": ["safe"], "apps": [{"name": "a" * 160, "id": 1, "commands": []}]}
    with pytest.raises(ValueError, match="longer than 159"):
        ct.format_command(long_app, "a" * 160, "x")


def test_c_vectors():
    header = ct.c_vectors(VECTORS)
    assert header.startswith("/* GENERATED")
    assert '{"typed_app ping", MODE_SAFE, CMD_ROUTE_OK, 0},' in header
    assert 'CMD_ROUTE_BAD_ARG, 9},' in header


def test_main(tmp_path, capsys):
    assert ct.main(["frame_manager", "set_entry_enabled", "nominal", "3", "false"]) == 0
    assert capsys.readouterr().out.strip() == "frame_manager set_entry_enabled nominal 3 false"

    assert ct.main(["frame_manager", "set_entry_enabled", "nominal", "3"]) == 1
    assert "bad arg count" in capsys.readouterr().err

    out = tmp_path / "v" / "route_vectors.h"
    assert ct.main(["--vectors", str(ROUTE_TEST / "vectors.yaml"), "--c-vectors", str(out)]) == 0
    assert "route_vectors[]" in out.read_text()
