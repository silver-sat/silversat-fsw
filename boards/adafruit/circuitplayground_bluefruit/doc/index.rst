.. zephyr:board:: adafruit_circuitplayground_bluefruit

Overview
********

The Adafruit Circuit Playground Bluefruit provides support for the Nordic
Semiconductor nRF52840 ARM Cortex-M4F CPU and the following devices:

* :abbr:`ADC (Analog to Digital Converter)`
* CLOCK
* FLASH
* :abbr:`GPIO (General Purpose Input Output)`
* :abbr:`I2C (Inter-Integrated Circuit)`
* :abbr:`MPU (Memory Protection Unit)`
* :abbr:`NVIC (Nested Vectored Interrupt Controller)`
* :abbr:`PWM (Pulse Width Modulation)`
* RADIO (Bluetooth Low Energy and 802.15.4)
* :abbr:`RTC (nRF RTC System Clock)`
* Segger RTT (RTT Console)
* :abbr:`SPI (Serial Peripheral Interface)`
* :abbr:`UART (Universal asynchronous receiver-transmitter)`
* :abbr:`USB (Universal Serial Bus)`
* :abbr:`WDT (Watchdog Timer)`

Hardware
********

- nRF52840 ARM Cortex-M4F processor at 64 MHz
- 1 MB flash memory and 256 KB of SRAM
- 2 MB QSPI flash memory
- LIS3DH triple-axis accelerometer
- Thermistor temperature sensor
- Phototransistor light sensor
- MEMS microphone
- Mini speaker with class D amplifier
- 2 user buttons (A and B)
- Slide switch
- 1 red LED
- 10 NeoPixel LEDs
- 8 alligator-clip input/output pads

Supported Features
==================

.. zephyr:board-supported-hw::

Connections and IOs
===================

The `Adafruit Circuit Playground Bluefruit Learn site`_ has detailed
information about the board including `pinouts`_ and the `schematic`_.

LED
---

* LED0 (red) = P1.14

Push buttons and switches
-------------------------

* Button A = P1.02
* Button B = P1.15
* Slide switch = P1.06
* RESET = P0.18

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Flashing
========

The Circuit Playground Bluefruit ships with the `Adafruit nRF52 Bootloader`_
which supports flashing using `UF2`_. This allows easy flashing of new images,
but does not support debugging the device.

#. Build the Zephyr kernel and the :zephyr:code-sample:`blinky` sample
   application.

   .. zephyr-app-commands::
      :zephyr-app: samples/basic/blinky
      :board: adafruit_circuitplayground_bluefruit/nrf52840/uf2
      :goals: build
      :compact:

#. Connect the board to your host computer using USB.

#. Tap the reset button twice quickly to enter bootloader mode. A mass
   storage device named ``CPLAYBTBOOT`` should appear on the host. Ensure this
   is mounted.

#. Flash the image.

   .. zephyr-app-commands::
      :zephyr-app: samples/basic/blinky
      :board: adafruit_circuitplayground_bluefruit/nrf52840/uf2
      :goals: flash
      :compact:

#. You should see the red LED blink.

References
**********

.. target-notes::

.. _Adafruit Circuit Playground Bluefruit Learn site:
    https://learn.adafruit.com/adafruit-circuit-playground-bluefruit

.. _pinouts:
    https://learn.adafruit.com/adafruit-circuit-playground-bluefruit/pinouts

.. _schematic:
    https://learn.adafruit.com/adafruit-circuit-playground-bluefruit/downloads

.. _Adafruit nRF52 Bootloader:
    https://github.com/adafruit/Adafruit_nRF52_Bootloader

.. _UF2:
    https://github.com/microsoft/uf2
