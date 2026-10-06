# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/nvm_dump.py and the D packet. Run with `make test-python`.

The image vectors (tests/unit/libs/nvm_vectors.yaml) are the same ones the
flight code reads back in tests/unit/libs/src/nvm.c, so passing both means
the ground decodes exactly what the satellite stores.
"""

import copy
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import link_codec  # noqa: E402
import nvm_dump  # noqa: E402
import telemetry as tm  # noqa: E402

RECORDS = yaml.safe_load(
    (REPO / "tests" / "unit" / "libs" / "nvm_vectors.yaml").read_text())["records"]


@pytest.fixture(scope="module")
def dictionary():
    return tm.load_dictionary()


def region(dictionary, name):
    return next(r for r in dictionary["nvm"]["regions"] if r["name"] == name)


def dump_packet(store, address, data):
    return bytes([ord("D"), store]) + address.to_bytes(2, "little") + bytes([len(data)]) + data


# ---- The D packet (telemetry.decode) ---------------------------------------

def test_decode_a_dump(dictionary):
    packet = tm.decode(dictionary, dump_packet(1, 0x1234, b"\x01\x02\x03"))
    assert packet == {"kind": "dump", "store": "mirror", "address": 0x1234,
                      "data": b"\x01\x02\x03"}


@pytest.mark.parametrize("payload, why", [
    (b"D\x00\x00\x00", "too short"),
    (dump_packet(2, 0, b"\x00"), "unknown store"),
    (b"D\x00\x00\x00\x02\x00", "says 2 bytes, carries 1"),
])
def test_a_malformed_dump_is_refused(dictionary, payload, why):
    with pytest.raises(ValueError):
        tm.decode(dictionary, payload)


# ---- Slots ------------------------------------------------------------------

def test_slot_layout(dictionary):
    r = region(dictionary, "deployment")
    raw = nvm_dump.encode_slot(dictionary, r, 0x01020304, {"complete": True})
    assert len(raw) == r["slot_size"]
    assert raw[0:2] == b"\x53\x53", "magic"
    assert raw[2] == r["version"]
    assert raw[3] == r["size"]
    assert raw[4:8] == bytes([4, 3, 2, 1]), "generation, little-endian"
    assert raw[8] == 1, "complete"
    assert raw[9:] == link_codec.crc32c(raw[:9]).to_bytes(4, "little")


def reseal(raw):
    """Fix a slot's CRC after changing it, so only the change is wrong."""
    raw[-4:] = link_codec.crc32c(bytes(raw[:-4])).to_bytes(4, "little")


@pytest.mark.parametrize("at, value, sealed, why", [
    (0, 0xff, True, "empty"),
    (2, 2, True, "other version"),
    (3, 2, True, "bad"),           # a length that isn't the record's, CRC intact
    (8, 0, False, "bad"),          # the CRC doesn't match
])
def test_a_slot_that_is_not_valid(dictionary, at, value, sealed, why):
    r = region(dictionary, "deployment")
    raw = bytearray(nvm_dump.encode_slot(dictionary, r, 1, {"complete": True}))
    raw[at] = value
    if sealed:
        reseal(raw)
    assert nvm_dump.decode_slot(dictionary, r, raw) == why


# ---- Images -----------------------------------------------------------------

def test_the_vector_image_decodes(dictionary):
    records = nvm_dump.decode_image(dictionary, nvm_dump.build_image(dictionary, RECORDS))
    for name in ("command_state", "run_checkpoint"):
        assert records[name]["generation"] == 1
        assert records[name]["fields"] == RECORDS[name], name
        assert records[name]["slots"] == ["valid", "empty"]
    log = records["boot_log"]
    assert [e["number"] for e in log["entries"]] == [2, 17, 20], "oldest first, not by place"
    assert [e["fields"] for e in log["entries"]] == [e["values"] for e in RECORDS["boot_log"]]
    assert [i for i, state in enumerate(log["slots"]) if state == "valid"] == [1, 2, 4], \
        "each at its number % 16"
    assert records["mode_state"]["fields"] is None, "not in the vectors"
    assert records["mode_state"]["slots"] == ["empty", "empty"]


def test_the_newest_generation_wins(dictionary):
    r = region(dictionary, "deployment")
    image = bytearray(b"\xff" * dictionary["nvm"]["fram_size"])
    for slot, generation, complete in ((0, 0xffffffff, False), (1, 0, True)):
        raw = nvm_dump.encode_slot(dictionary, r, generation, {"complete": complete})
        at = r["address"] + slot * r["slot_size"]
        image[at:at + len(raw)] = raw
    record = nvm_dump.decode_image(dictionary, image)["deployment"]
    assert record["generation"] == 0, "0 follows 0xffffffff"
    assert record["fields"] == {"complete": True}


def test_a_corrupt_slot_is_reported(dictionary):
    image = nvm_dump.build_image(dictionary, RECORDS)
    image[region(dictionary, "command_state")["address"] + 8] ^= 0x01
    record = nvm_dump.decode_image(dictionary, image)["command_state"]
    assert record["fields"] is None
    assert record["slots"] == ["bad", "empty"]


def test_the_mirror_layout(dictionary):
    image = nvm_dump.build_image(dictionary, {"mode_state": {"mode": "nominal"}}, "mirror")
    assert len(image) == dictionary["nvm"]["mirror_size"]
    records = nvm_dump.decode_image(dictionary, image, "mirror")
    assert records["mode_state"]["fields"]["mode"] == "nominal"
    held = {r["name"] for r in dictionary["nvm"]["regions"] if r["mirror_address"] is not None}
    assert set(records) == held


def test_the_mirror_holds_only_mirrored_regions(dictionary):
    d = copy.deepcopy(dictionary)
    region(d, "deployment")["mirror_address"] = None
    records = nvm_dump.decode_image(d, nvm_dump.build_image(d, {}, "mirror"), "mirror")
    assert "deployment" not in records
    assert "mode_state" in records


# ---- Putting dumps together -------------------------------------------------

def test_assemble_dumps_in_any_order(dictionary):
    image = nvm_dump.build_image(dictionary, RECORDS)
    used = 0x200
    packets = [dump_packet(0, at, bytes(image[at:at + 240])) for at in range(0, used, 240)]
    packets.append(b"A 7 ok")
    images = nvm_dump.assemble(dictionary, reversed(packets))
    assert set(images) == {"fram"}
    assert images["fram"][:used] == image[:used]
    assert images["fram"][used:] == b"\xff" * (len(image) - used), "not dumped"
    records = nvm_dump.decode_image(dictionary, images["fram"])
    assert records["command_state"]["fields"] == RECORDS["command_state"]


def test_assemble_refuses_a_dump_past_the_end(dictionary):
    with pytest.raises(ValueError):
        nvm_dump.assemble(dictionary, [dump_packet(1, 4090, bytes(8))])


# ---- The command line -------------------------------------------------------

def test_cli_decodes_an_image(dictionary, tmp_path, capsys):
    path = tmp_path / "eeprom.bin"
    path.write_bytes(nvm_dump.build_image(dictionary, RECORDS))
    assert nvm_dump.main(["--image", str(path)]) == 0
    out = capsys.readouterr().out
    assert "command_state (generation 1)" in out
    assert "boot_log: 3 entries" in out
    assert "mode_state: no valid record (slots: empty, empty)" in out


def test_cli_writes_the_c_image(tmp_path):
    vectors = REPO / "tests" / "unit" / "libs" / "nvm_vectors.yaml"
    out = tmp_path / "nvm_vectors.h"
    assert nvm_dump.main(["--vectors", str(vectors), "--c-image", str(out)]) == 0
    text = out.read_text()
    used = max(r["address"] + r["slot_size"] * (r["ring"] or 2)
               for r in tm.load_dictionary()["nvm"]["regions"])
    assert f"static const uint8_t nvm_vector_image[{used}] = {{" in text
    array = text.split("{", 1)[1].split("}", 1)[0]
    assert array.count("0x") == used, "exactly the used bytes"
    assert ".floor_slot1 = 18446744073709551615ULL" in text
    assert ".reset_cause = 2147483649U" in text
    assert ".last_accepted_met_ms = -2LL" in text


def test_cli_with_nothing_to_do():
    assert nvm_dump.main([]) == 2
