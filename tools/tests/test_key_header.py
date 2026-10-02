# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/key_header.py. Run with `make test-python`."""

import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import key_header as kh  # noqa: E402

SLOT0, SLOT1 = kh.PUBLISHED_TEST_KEYS


def key_file(tmp_path, name, key=None):
    path = tmp_path / name
    path.write_text("# a key\n" + (key or secrets.token_bytes(32)).hex() + "\n")
    return path


def test_test_keys_make_a_header(tmp_path):
    out = tmp_path / "gen" / "cmd_keys.h"
    assert kh.main(["--slot0", str(SLOT0), "--slot1", str(SLOT1), "--out", str(out)]) == 0
    text = out.read_text()
    assert "never commit" in text
    assert "static const uint8_t cmd_keys[2][32] = {" in text
    first = kh.load_key(SLOT0)
    assert ", ".join(f"0x{b:02x}" for b in first) in text


def test_published_keys_differ():
    assert kh.load_key(SLOT0) != kh.load_key(SLOT1)


def test_flight_refuses_a_published_test_key(tmp_path, capsys):
    out = tmp_path / "cmd_keys.h"
    flight = key_file(tmp_path, "flight0.txt")
    assert kh.main(["--slot0", str(flight), "--slot1", str(SLOT1), "--out", str(out),
                    "--flight"]) == 1
    assert "slot 1 is a published test key" in capsys.readouterr().err
    assert not out.exists()


def test_flight_accepts_its_own_keys(tmp_path):
    out = tmp_path / "cmd_keys.h"
    assert kh.main(["--slot0", str(key_file(tmp_path, "a.txt")),
                    "--slot1", str(key_file(tmp_path, "b.txt")),
                    "--out", str(out), "--flight"]) == 0


def test_same_key_twice_is_refused(tmp_path, capsys):
    key = secrets.token_bytes(32)
    out = tmp_path / "cmd_keys.h"
    assert kh.main(["--slot0", str(key_file(tmp_path, "a.txt", key)),
                    "--slot1", str(key_file(tmp_path, "b.txt", key)), "--out", str(out)]) == 1
    assert "same key" in capsys.readouterr().err


def test_missing_or_bad_key_file(tmp_path, capsys):
    out = tmp_path / "cmd_keys.h"
    assert kh.main(["--slot0", str(tmp_path / "missing.txt"), "--slot1", str(SLOT1),
                    "--out", str(out)]) == 1
    short = tmp_path / "short.txt"
    short.write_text("0011\n")
    assert kh.main(["--slot0", str(short), "--slot1", str(SLOT1), "--out", str(out)]) == 1
    assert "32 bytes" in capsys.readouterr().err
