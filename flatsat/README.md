# FlatSat Tools

This folder contains the tooling for the remote flatsat. 

The flatsat can be reset without cycling the power using text commands to a remote microcontroller. The microcontroller, an Adafruit QT Py in the initial implementation, accepts the commands proposed for the flatsat controller board and pulls the NRST pin on the Nucleo flatsat microcontroller low, allowing testing of the reset function, simulation of a brown out, and recovery from a hung microcontroller. 

## Setup

To setup the reset function, initialize the QT Py as described in the [Adafruit Learning Guide for the QT PY](https://learn.adafruit.com/adafruit-qt-py). Then copy the `boot.py` and `code.py` files in this directory to the QT Py CIRCUITPY volume.

`boot.py`gives you two endpoints for serial data. The console is available for debugging and `code.py` connects to the data port. `code.py` reads text commands on the serial port and sets NRST on the Nucleo, connected to A0 on the QT Py, as specified. 