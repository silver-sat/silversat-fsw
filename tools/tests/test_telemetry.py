# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/telemetry.py. Run with `make test-python`.

The housekeeping vectors are the same ones the flight encoder is tested
against (tests/unit/libs, src/tlm_encode.c), so passing both means the
ground decodes exactly what the satellite sends.
"""

import math
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import telemetry as tm  # noqa: E402

LIBS = REPO / "tests" / "unit" / "libs"
VECTORS = yaml.safe_load((LIBS / "tlm_vectors.yaml").read_text())["vectors"]


@pytest.fixture(scope="module")
def dictionary():
    return tm.load_dictionary([LIBS / "apps"])


@pytest.mark.parametrize("vector", VECTORS, ids=[v["name"] for v in VECTORS])
def test_vectors_round_trip(dictionary, vector):
    packet = tm.encode_hk(dictionary, vector["app"], vector["met_ms"], vector["values"])
    decoded = tm.decode(dictionary, packet)
    assert decoded["kind"] == "hk"
    assert decoded["app"] == vector["app"]
    assert decoded["met_ms"] == vector["met_ms"]
    for name, value in vector["values"].items():
        if isinstance(value, float):
            assert math.isclose(decoded["fields"][name], value, rel_tol=1e-6), name
        else:
            assert decoded["fields"][name] == value, name


def test_packet_layout(dictionary):
    values = dict(VECTORS[2]["values"])
    packet = tm.encode_hk(dictionary, "typed_app", -1, values)
    assert packet[0] == ord("H")
    assert packet[1] == 150, "typed_app's id"
    assert packet[2:10] == b"\xff" * 8, "MET -1, little-endian"
    body = packet[10:]
    assert body[0:4] == (1).to_bytes(4, "little"), "commands"
    assert body[4] == 1, "u8"
    assert body[5:7] == bytes([0x02, 0x01]), "u16 258, least significant byte first"


def test_every_flight_app_fits_and_decodes():
    dictionary = tm.load_dictionary()
    for app in dictionary["apps"]:
        values = {f["name"]: (f["values"][0]["name"] if "values" in f else
                              False if f["type"] == "bool" else
                              0.0 if f["type"].startswith("float") else 0)
                  for f in app["housekeeping"]["fields"]}
        packet = tm.encode_hk(dictionary, app["name"], 5, values)
        assert len(packet) <= 255, app["name"]
        assert tm.decode(dictionary, packet)["fields"] == values


def test_replies(dictionary):
    assert tm.decode(dictionary, b"ACK 0000018f2c4d5e6f ok") == {
        "kind": "reply", "text": "ACK 0000018f2c4d5e6f ok"}
    assert tm.decode(dictionary, b"NAK 0000018f2c4d5e6f replay")["kind"] == "reply"


@pytest.mark.parametrize("payload, message", [
    (b"", "empty"),
    (b"Zhello", "unknown packet kind"),
    (b"H\x96\x00", "too short"),
    (b"H\xfe" + bytes(8), "no app with id 254"),
    (b"H\x96" + bytes(8) + b"\x00", "typed_app housekeeping is"),
])
def test_bad_packets(dictionary, payload, message):
    with pytest.raises(ValueError, match=message):
        tm.decode(dictionary, payload)


def test_c_vectors(tmp_path):
    out = tmp_path / "v" / "tlm_vectors.h"
    assert tm.main(["--extra-apps", str(LIBS / "apps"), "--vectors",
                    str(LIBS / "tlm_vectors.yaml"), "--c-vectors", str(out)]) == 0
    text = out.read_text()
    assert "hk_vectors[]" in text and ".i64 = INT64_MIN" in text
