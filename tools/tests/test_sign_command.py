# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/sign_command.py. Run with `make test-python`."""

import hashlib
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import sign_command as sc  # noqa: E402

KEY = bytes(range(32))
SALT = bytes(range(1, 9))


def test_blake2s_known_answers():
    # RFC 7693 Appendix B: BLAKE2s-256("abc").
    assert hashlib.blake2s(b"abc").hexdigest() == (
        "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982")
    # BLAKE2 reference KAT (blake2s-kat.txt): keyed, key 00..1f, empty input.
    assert hashlib.blake2s(b"", key=KEY).hexdigest() == (
        "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49")


def test_packet_layout():
    packet = sc.sign("noop", KEY, 0x0102030405060708, SALT)
    assert len(packet) == sc.HEADER_CHARS + 4
    assert packet[64:80] == "0102030405060708"           # salt
    assert packet[80:96] == "0102030405060708"           # counter, most significant first
    assert packet[96:] == "noop"
    assert all(c in "0123456789abcdef" for c in packet[:96])   # lowercase hex


def test_tag_covers_everything_after_it():
    packet = sc.sign("noop", KEY, 7, SALT)
    expected = hashlib.blake2s(packet[64:].encode(), key=KEY).hexdigest()
    assert packet[:64] == expected


def test_each_field_changes_the_tag():
    base = sc.sign("noop", KEY, 7, SALT)[:64]
    assert sc.sign("noop", KEY, 8, SALT)[:64] != base
    assert sc.sign("noop", KEY, 7, bytes(8))[:64] != base
    assert sc.sign("nooq", KEY, 7, SALT)[:64] != base
    assert sc.sign("noop", bytes(32), 7, SALT)[:64] != base


def test_random_salt_by_default():
    assert sc.sign("noop", KEY, 7)[64:80] != sc.sign("noop", KEY, 7)[64:80]


def test_limits():
    assert len(sc.sign("x" * sc.TEXT_MAX, KEY, sc.COUNTER_MAX, SALT)) == sc.PACKET_MAX
    with pytest.raises(ValueError, match="1 to 159"):
        sc.sign("x" * (sc.TEXT_MAX + 1), KEY, 1)
    with pytest.raises(ValueError, match="1 to 159"):
        sc.sign("", KEY, 1)
    with pytest.raises(ValueError, match="printable"):
        sc.sign("tab\there", KEY, 1)
    with pytest.raises(ValueError, match="64 bits"):
        sc.sign("noop", KEY, sc.COUNTER_MAX + 1)
    with pytest.raises(ValueError, match="salt"):
        sc.sign("noop", KEY, 1, b"short")


def test_default_counter_is_increasing():
    now = sc.default_counter()
    assert sc.default_counter(last=now + 1000) == now + 1001


def test_counter_too_far_ahead():
    now = 1_790_812_800_000
    sc.check_not_too_far_ahead(now + sc.FUTURE_MARGIN_MS, now)
    with pytest.raises(ValueError, match="more than a day ahead"):
        sc.check_not_too_far_ahead(now + sc.FUTURE_MARGIN_MS + 1, now)
    with pytest.raises(ValueError, match="check its units"):
        sc.check_not_too_far_ahead(now * 1000, now)       # microseconds by mistake


def test_main_refuses_a_counter_far_ahead(capsys):
    far = sc.now_ms() * 1000
    assert sc.main(["--counter", str(far), "noop"]) == 1
    assert "more than a day ahead" in capsys.readouterr().err
    assert sc.main(["--counter", str(far), "--allow-future", "noop"]) == 0


def test_c_packets(tmp_path):
    spec = tmp_path / "packets.yaml"
    spec.write_text("packets:\n"
                    "  - {name: ping, text: typed_app ping, counter: 5, key: slot0}\n"
                    "  - {name: other_key, text: typed_app ping, counter: 6, key: slot1}\n")
    header = sc.c_packets(spec)
    assert "#define PKT_PING_COUNTER 0x0000000000000005ULL" in header
    slot0 = sc.load_key(sc.TEST_KEY_FILES["slot0"])
    expected = sc.sign("typed_app ping", slot0, 5, sc.VECTOR_SALT)
    assert f'#define PKT_PING "{expected}"' in header
    assert "slot1, counter 6" in header

    out = tmp_path / "out" / "packets.h"
    assert sc.main(["--c-packets", str(spec), str(out)]) == 0
    assert out.read_text() == header


def test_load_key(tmp_path):
    assert len(sc.load_key(sc.TEST_KEY_FILE)) == sc.KEY_LEN
    bad = tmp_path / "short.txt"
    bad.write_text("# comment\n0011\n")
    with pytest.raises(ValueError, match="32 bytes"):
        sc.load_key(bad)


def test_c_vectors_are_deterministic():
    assert sc.c_vectors(KEY) == sc.c_vectors(KEY)
    assert '"counter_max"' in sc.c_vectors(KEY)


def test_main(tmp_path, capsys):
    assert sc.main(["--counter", "5", "noop"]) == 0
    packet = capsys.readouterr().out.strip()
    assert packet[80:96] == "0000000000000005"
    assert packet[:64] == sc.sign("noop", sc.load_key(sc.TEST_KEY_FILE), 5,
                                  bytes.fromhex(packet[64:80]))[:64]

    out = tmp_path / "v" / "vectors.h"
    assert sc.main(["--c-vectors", str(out)]) == 0
    assert out.read_text().startswith("/* GENERATED")

    assert sc.main(["--counter", "-1", "noop"]) == 1
    assert "sign_command: error" in capsys.readouterr().err
