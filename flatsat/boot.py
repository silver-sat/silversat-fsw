# Runs only on a HARD reset: the reset button, or a full power cycle.
# Saving a file triggers CircuitPython's auto-reload, which re-runs
# code.py but does NOT re-enumerate USB, so changes here need a real
# reset before they take effect.

import usb_cdc
import usb_hid
import usb_midi

# The SAMD21 has only eight USB endpoint pairs, and CircuitPython spends
# them by default on the CDC console, the CIRCUITPY drive, HID and MIDI.
# A second CDC function needs more than is left over, so the data port
# quietly fails to enumerate even though usb_cdc.enable() succeeds and
# usb_cdc.data returns a live object. Free two devices we do not need.
#
# Mass storage stays enabled - that is how firmware gets onto the board.
usb_hid.disable()
usb_midi.disable()

usb_cdc.enable(console=True, data=True)

# Proof of execution. CircuitPython copies this into boot_out.txt under
# "boot.py output:", which is the only reliable way to tell whether this
# file ran: the filesystem has no clock, so every file's timestamp reads
# as 1999 and tells you nothing.
print("usb_cdc: console + data enabled; hid and midi disabled")
