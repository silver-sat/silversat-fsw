# SilverSat flatsat NRST controller - CircuitPython prototype.
#
# Runs on a Raspberry Pi Pico. NRST_PIN drives an NPN transistor (MPS2222A
# or 2N3904) whose collector is on the target's NRST and whose emitter is
# on ground (docs/flatsat.md, "Reset controller"):
#
#   pin high -> transistor on  -> NRST pulled low  -> target held in reset
#   pin low  -> transistor off -> NRST left alone  -> target runs
#
# The transistor is there for when this board has no power. A pin wired
# straight to NRST pulls it low through the pin's protection diode whenever
# the controller is unpowered, holding the target in reset; with the
# transistor, an unpowered, unprogrammed or crashed controller leaves the
# target running. A 100 kOhm resistor from base to ground keeps the
# transistor off while this board boots and its pin floats.
#
# The command vocabulary matches the flatsat controller specification, so
# the host script does not change when the student-built board replaces
# this one.
#
# Note on portability: CircuitPython on the SAMD21 (the QT Py M0 this
# replaced) has no bytes.decode(), so everything here stays in bytes and
# compares against byte literals. Keep it that way, so the file runs on
# either board.

import board
import digitalio
import time
import usb_cdc

NRST_PIN = board.GP15          # Pico pin 20; ground is pin 18
HOLD_TIMEOUT = 30.0            # bench safety: never hold reset forever
# Change VERSION whenever this file changes: `make reset` compares it with
# the repository's copy, and warns when the board runs something else.
VERSION = b"flatsat-nrst 0.3"

serial = usb_cdc.data
if serial is None:
    raise RuntimeError(
        "usb_cdc.data is None - boot.py did not take effect. "
        "Hard-reset the board; saving the file is not enough."
    )

# Released from the start: the pin low keeps the transistor off.
nrst = digitalio.DigitalInOut(NRST_PIN)
nrst.switch_to_output(value=False)

asserted_at = None


def assert_reset():
    global asserted_at
    nrst.value = True          # transistor on: NRST low
    asserted_at = time.monotonic()


def release_reset():
    global asserted_at
    nrst.value = False         # transistor off: NRST back to its pull-up
    asserted_at = None


def trim(raw):
    """Strip surrounding whitespace using only slicing, so this works on
    builds with a reduced set of bytes methods."""
    while raw and raw[-1:] in (b"\r", b"\n", b" ", b"\t"):
        raw = raw[:-1]
    while raw and raw[:1] in (b" ", b"\t"):
        raw = raw[1:]
    return raw


def handle(cmd):
    """Return the reply for one command, as bytes.

    Commands set state; nothing here toggles. A power cycle or a reset
    pulse is two commands composed by the caller.
    """
    if cmd == b"set_reset asserted":
        assert_reset()
        return b"ok"

    if cmd == b"set_reset released":
        release_reset()
        return b"ok"

    if cmd == b"restore":
        release_reset()
        return b"ok"

    if cmd == b"get_status":
        if asserted_at is None:
            return b"ok reset=released"
        return b"ok reset=asserted"

    if cmd == b"version":
        return b"ok " + VERSION

    return b"err syntax"


buf = b""

while True:
    # Bench safety: a forgotten assert releases itself, so a crashed host
    # or a closed laptop cannot leave the target held in reset.
    if asserted_at is not None and time.monotonic() - asserted_at > HOLD_TIMEOUT:
        release_reset()
        serial.write(b"event reset_timeout\n")

    if serial.in_waiting:
        buf += serial.read(serial.in_waiting)

        idx = buf.find(b"\n")
        while idx >= 0:
            cmd = trim(buf[:idx])
            buf = buf[idx + 1:]
            if cmd:
                serial.write(handle(cmd) + b"\n")
            idx = buf.find(b"\n")

    time.sleep(0.01)
