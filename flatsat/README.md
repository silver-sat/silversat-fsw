# FlatSat Tools

This folder contains the tooling for the remote flatsat. 

The flatsat can be reset without cycling the power using text commands to a remote microcontroller. The microcontroller, an Adafruit QT Py in the initial implementation, accepts the commands proposed for the flatsat controller board and pulls the NRST pin on the Nucleo flatsat microcontroller low, allowing testing of the reset function, simulation of a brown out, and recovery from a hung microcontroller. 

## Setup

To setup the reset function, initialize the QT Py as described in the [Adafruit Learning Guide for the QT PY](https://learn.adafruit.com/adafruit-qt-py). 

Then clone this `silver-sat.fsw` repo on the Ubuntu box, switch to the `flatsat` directory, and run `make deploy-controller`.

