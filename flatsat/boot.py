# Runs only on a HARD reset (reset button, or a full power cycle).
# Saving a file triggers an auto-reload, which re-runs code.py but does
# NOT re-enumerate USB, so changes here need a real reset to take effect.

import usb_cdc

usb_cdc.enable(console=True, data=True)

# Proof of execution. CircuitPython copies this into boot_out.txt under
# "boot.py output:", which is the only reliable way to tell whether this
# file ran - the filesystem has no clock, so every file's timestamp reads
# as 1999 and tells you nothing.
print("usb_cdc: console + data enabled")
