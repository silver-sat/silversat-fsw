# Runs only on a HARD reset: the reset button (or BOOTSEL and a replug on a
# Pico), or a full power cycle. Saving a file triggers CircuitPython's
# auto-reload, which re-runs code.py but does NOT re-enumerate USB, so
# changes here need a real reset before they take effect.

import usb_cdc
import usb_hid
import usb_midi

# Nothing here uses HID or MIDI, so turn them off. On the SAMD21 (the QT Py
# M0 this board replaced) that was required: it has only eight USB endpoint
# pairs, and with HID and MIDI on, a second CDC function silently failed to
# enumerate even though usb_cdc.enable() succeeded. The Pico's RP2040 has
# room for all of them, but the controller needs neither, and fewer USB
# functions means less to go wrong.
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
