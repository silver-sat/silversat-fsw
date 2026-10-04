# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/link_codec.py. Run with `make test-python`.

The shared vectors are the same ones the C codec is tested against
(tests/unit/libs), so passing both means the two agree.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import link_codec as lc  # noqa: E402

VECTORS = REPO / "tests" / "unit" / "libs" / "link_vectors.yaml"
PACKETS, STREAMS = lc.load_vectors(VECTORS)


def test_crc32c_check_value():
    # DS-65: the check value for ASCII "123456789".
    assert lc.crc32c(b"123456789") == 0xE3069283


def test_frame_layout():
    frame = lc.encode(lc.Packet(0x07, 0x12, b"ab"))
    body = bytes([0x07, 0x12, 2]) + b"ab"
    assert frame == bytes([lc.FEND]) + body + lc.crc32c(body).to_bytes(4, "little") + \
        bytes([lc.FEND])


def test_escaping():
    assert lc.escape(bytes([0xC0, 0xDB, 0x41])) == bytes([0xDB, 0xDC, 0xDB, 0xDD, 0x41])


@pytest.mark.parametrize("name", list(PACKETS))
def test_every_packet_round_trips(name):
    packet = PACKETS[name]
    assert lc.decode(lc.encode(packet)) == [("packet", packet)]


def test_crc_escapes_vector_needs_escaping():
    crc = PACKETS["crc_escapes"].raw()[-4:]
    assert lc.FEND in crc and lc.FESC in crc


@pytest.mark.parametrize("stream", STREAMS, ids=[s["name"] for s in STREAMS])
def test_shared_streams(stream):
    got = lc.decode(stream["bytes"])
    want = [("packet", PACKETS[e["packet"]]) if isinstance(e, dict) else (e, None)
            for e in stream["expect"]]
    assert got == want


def test_vectors_cover_every_result():
    seen = {e if isinstance(e, str) else "packet" for s in STREAMS for e in s["expect"]}
    assert seen == set(lc.RESULTS) - {"none"}


def test_payload_too_long():
    with pytest.raises(ValueError, match="255"):
        lc.Packet(0, 0, bytes(256)).raw()


def test_sequence_numbers():
    seq = lc.Sequence()
    assert [seq.next() for _ in range(3)] == [0, 1, 2]
    seq.next_tx = 255
    assert seq.next() == 255 and seq.next() == 0, "wraps after 255"

    rx = lc.Sequence()
    assert rx.received(10) == 0, "the first frame sets the starting point"
    assert rx.received(11) == 0
    assert rx.received(14) == 2, "12 and 13 were lost"
    assert rx.received(14) == -1, "a repeat"
    rx.received(255)
    assert rx.received(0) == 0, "255 then 0 loses nothing"
    rx.received(254)
    assert rx.received(1) == 2, "255 and 0 were lost across the wrap"


def test_c_vectors(tmp_path):
    out = tmp_path / "v" / "link_vectors.h"
    assert lc.main(["--vectors", str(VECTORS), "--c-vectors", str(out)]) == 0
    header = out.read_text()
    assert "packet_vectors[]" in header and "stream_vectors[]" in header
    assert "{LINK_DECODE_BAD_CRC, -1}" in header
