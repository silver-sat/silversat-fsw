# SilverSat flatsat NRST controller - CircuitPython prototype.
#
# Drives the target's NRST line as an open-drain output, emulated by
# switching pin direction: INPUT (high-Z) = released, OUTPUT LOW = asserted.
# The pin is never driven high, so it can never fight the target's own
# internal reset, and an unprogrammed or crashed board leaves the target
# running.
#
# The command vocabulary matches the flatsat controller specification, so
# the host script does not change when the student-built board replaces
# this one.
#
# Note on portability: CircuitPython on the SAMD21 (QT Py M0) has no
# bytes.decode(), so everything here stays in bytes and compares against
# byte literals. Do not add .decode() to this file without checking.

import board
import digitalio
import time
import usb_cdc

NRST_PIN = board.A0
HOLD_TIMEOUT = 30.0            # bench safety: never hold reset forever
VERSION = b"flatsat-nrst 0.2"

serial = usb_cdc.data
if serial is None:
    raise RuntimeError(
        "usb_cdc.data is None - boot.py did not take effect. "
        "Hard-reset the board; saving the file is not enough."
    )

nrst = digitalio.DigitalInOut(NRST_PIN)
nrst.direction = digitalio.Direction.INPUT     # high-Z == released
nrst.pull = None

asserted_at = None


def assert_reset():
    global asserted_at
    nrst.direction = digitalio.Direction.OUTPUT
    nrst.value = False                         # only ever sinks, never sources
    asserted_at = time.monotonic()


def release_reset():
    global asserted_at
    nrst.direction = digitalio.Direction.INPUT
    nrst.pull = None
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
