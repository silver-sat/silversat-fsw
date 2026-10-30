# FlatSat Tools

This folder contains the tooling for the remote flatsat.

The flatsat can be reset without cycling the power using text commands to a remote microcontroller. The microcontroller, a Raspberry Pi Pico running CircuitPython (an Adafruit QT Py in the first version), accepts the commands proposed for the flatsat controller board and, through a transistor, pulls the NRST pin on the Nucleo flatsat microcontroller low, allowing testing of the reset function, simulation of a brown out, and recovery from a hung microcontroller. The transistor means the controller leaves the Nucleo running when the controller has no power.

## Setup

To setup the reset function, install CircuitPython on the Pico as described in the [Raspberry Pi Pico guide](https://learn.adafruit.com/getting-started-with-raspberry-pi-pico-circuitpython), and wire it as in "Reset controller" in [docs/flatsat.md](../docs/flatsat.md).

Then clone this `silver-sat.fsw` repo on the Ubuntu box, and from its top directory run `make deploy-controller`. Unplug the Pico and plug it back in afterwards, so `boot.py` takes effect, then check it with `python3 flatsat/nrst_test.py`.
