# SPDX-License-Identifier: Apache-2.0
"""Tests for the flatsat reset controller's files in flatsat/. Run with
`make test-python`.

The firmware runs on a Pico under CircuitPython, and nrst_test.py on the
flatsat box, so neither runs here. These tests check what can be checked
without the hardware: every file compiles (a merged-line syntax error in
nrst_test.py once went unnoticed for weeks), and code.py drives the pin the
right way round for the transistor on NRST: high asserts reset, low
releases it, and it starts released.

code.py ends in its main loop, so the tests run only the part before it,
with stand-ins for CircuitPython's modules.
"""

import py_compile
import sys
import time
import types
from pathlib import Path

import pytest

FLATSAT = Path(__file__).resolve().parents[2] / "flatsat"
LOOP_START = '\nbuf = b""\n'


@pytest.mark.parametrize("name", ["boot.py", "code.py", "nrst_test.py"])
def test_compiles(name):
    py_compile.compile(str(FLATSAT / name), doraise=True)


class FakePin:
    """digitalio.DigitalInOut, recording every state the pin passes through."""

    def __init__(self, pin):
        self.pin = pin
        self.output = False
        self._value = False
        self.history = []

    def switch_to_output(self, value=False):
        self.output = True
        self.value = value

    def switch_to_input(self, pull=None):
        self.output = False
        self.history.append("input")

    @property
    def value(self):
        return self._value

    @value.setter
    def value(self, level):
        assert self.output, "set a level on a pin that isn't an output"
        self._value = level
        self.history.append(level)


@pytest.fixture
def controller(monkeypatch):
    """code.py's globals, with everything before its main loop run."""
    pins = []

    def make_pin(pin):
        pins.append(FakePin(pin))
        return pins[-1]

    modules = {
        "board": types.SimpleNamespace(GP15="GP15"),
        "digitalio": types.SimpleNamespace(DigitalInOut=make_pin),
        "usb_cdc": types.SimpleNamespace(data=object()),
        "time": time,
    }
    for name, module in modules.items():
        monkeypatch.setitem(sys.modules, name, module)

    source = (FLATSAT / "code.py").read_text()
    assert LOOP_START in source, "code.py's main loop moved; update LOOP_START"
    code = {}
    exec(compile(source.split(LOOP_START)[0], "code.py", "exec"), code)
    assert len(pins) == 1
    code["pin"] = pins[0]
    return code


def test_starts_released_with_the_transistor_off(controller):
    pin = controller["pin"]
    assert pin.pin == "GP15"
    assert pin.output and pin.value is False
    assert pin.history == [False]


def test_asserting_turns_the_transistor_on(controller):
    assert controller["handle"](b"set_reset asserted") == b"ok"
    assert controller["pin"].value is True
    assert controller["handle"](b"get_status") == b"ok reset=asserted"


def test_releasing_turns_it_off(controller):
    controller["handle"](b"set_reset asserted")
    assert controller["handle"](b"set_reset released") == b"ok"
    assert controller["pin"].value is False
    assert controller["handle"](b"get_status") == b"ok reset=released"


def test_restore_releases(controller):
    controller["handle"](b"set_reset asserted")
    assert controller["handle"](b"restore") == b"ok"
    assert controller["pin"].value is False


def test_the_pin_never_floats(controller):
    # With the transistor, a floating pin would leave its base to the
    # pull-down alone; driving it both ways is the firm choice.
    for command in (b"set_reset asserted", b"set_reset released", b"restore"):
        controller["handle"](command)
    assert "input" not in controller["pin"].history


def test_version_and_unknown_commands(controller):
    assert controller["handle"](b"version").startswith(b"ok flatsat-nrst ")
    assert controller["handle"](b"nonsense") == b"err syntax"


def test_trim_strips_line_endings_and_spaces(controller):
    assert controller["trim"](b"  get_status\r\n") == b"get_status"
