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
EVENT_VECTORS = yaml.safe_load((LIBS / "tlm_vectors.yaml").read_text())["events"]


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
    assert packet[1] == 50, "typed_app's id"
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
    (b"H\x32\x00", "too short"),
    (b"H\xfe" + bytes(8), "no app with id 254"),
    (b"H\x32" + bytes(8) + b"\x00", "typed_app housekeeping is"),
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


# ---- Events (DS-10) ----------------------------------------------------------

@pytest.mark.parametrize("vector", EVENT_VECTORS, ids=[v["name"] for v in EVENT_VECTORS])
def test_event_vectors_round_trip(dictionary, vector):
    packet = tm.encode_event(dictionary, vector["app"], vector["event"], vector["met_ms"],
                             vector["args"])
    decoded = tm.decode(dictionary, packet)
    assert decoded["kind"] == "event"
    assert decoded["app"] == vector["app"]
    assert decoded["name"] == vector["event"]
    assert decoded["met_ms"] == vector["met_ms"]
    assert decoded["args"] == vector["args"]


def test_event_packet_layout(dictionary):
    packet = tm.encode_event(dictionary, "typed_app", "typed", 0x0102030405060708,
                             {"who": "health", "mode": "nominal"})
    assert packet[0] == ord("E")
    assert packet[1:9] == bytes([8, 7, 6, 5, 4, 3, 2, 1]), "MET, little-endian"
    assert packet[9] == 50, "typed_app's id"
    assert packet[10] == 2, "warning"
    assert packet[11:13] == bytes([1, 0]), "event id 1"
    assert packet[13:17] == bytes([6, 0, 0, 0]), "health's app id"
    assert packet[17:21] == bytes([1, 0, 0, 0]), "nominal"
    assert len(packet) == 21


def test_unused_event_arguments_are_zero(dictionary):
    """As the generated emit functions fill them (tests/unit/libs/src/event.c)."""
    packet = tm.encode_event(dictionary, "typed_app", "bare", 0, {})
    assert packet[13:21] == bytes(8)
    packet = tm.encode_event(dictionary, "typed_app", "plain", 0, {"count": -1})
    assert packet[17:21] == bytes(4)


def test_event_severity_and_arguments_by_name(dictionary):
    packet = tm.encode_event(dictionary, "typed_app", "plain", 5, {"count": -7})
    decoded = tm.decode(dictionary, packet)
    assert decoded["severity"] == "critical"
    assert decoded["args"] == {"count": -7}


def test_an_unknown_argument_value_is_a_number(dictionary):
    packet = bytearray(tm.encode_event(dictionary, "typed_app", "typed", 0,
                                       {"who": "health", "mode": "safe"}))
    packet[13] = 63   # no app has id 63
    packet[17] = 99   # no mode 99
    assert tm.decode(dictionary, bytes(packet))["args"] == {"who": 63, "mode": 99}


def test_a_malformed_event_is_refused(dictionary):
    packet = tm.encode_event(dictionary, "typed_app", "bare", 0, {})
    with pytest.raises(ValueError, match="not 20"):
        tm.decode(dictionary, packet[:-1])
    unknown = bytearray(packet)
    unknown[11:13] = (7).to_bytes(2, "little")
    with pytest.raises(ValueError, match="no event 7"):
        tm.decode(dictionary, bytes(unknown))


def test_every_flight_event_decodes():
    dictionary = tm.load_dictionary()
    count = 0
    for app in dictionary["apps"]:
        for event in app["events"]:
            args = {a["name"]: 1 for a in event["args"]}
            decoded = tm.decode(dictionary, tm.encode_event(dictionary, app["name"],
                                                            event["name"], 0, args))
            assert decoded["name"] == event["name"]
            count += 1
    assert count > 0
