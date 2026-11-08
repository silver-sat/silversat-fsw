# The flight software on a Circuit Playground Bluefruit

The student learning and exploration tier (DS-04). You run the real flight
software on your own Adafruit Circuit Playground Bluefruit (CPB), attached to
your own computer. The same board still runs CircuitPython and Arduino.

This is for learning. The CPB's nRF52840 isn't the flight processor, so a
test that matters still belongs on the flatsat (docs/flatsat.md).

## What you need

- A Circuit Playground Bluefruit and a USB cable that carries data.
- An Adafruit I2C FRAM breakout, 32 KB (ID 1895). It keeps the boot count,
  the mode and the mission time when the board resets or loses power.

Wire the breakout to the CPB's pads:

| FRAM breakout | CPB |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SCL | A4 |
| SDA | A5 |

## Get the firmware

Build it in a Codespace:

```sh
make cpb        # the first time also fetches the Nordic HAL, about 300 MB
```

The result is `build-cpb/zephyr/zephyr.uf2`. Right-click it in the VS Code
Explorer and choose Download. Or skip building: every CI run keeps the UF2
as an artifact named `zephyr-circuitplayground_bluefruit`.

## Load it

1. Double-tap the reset button. A drive named `CPLAYBTBOOT` appears.
2. Copy the UF2 onto it. Dragging it in the file manager works. On a Mac,
   from a terminal, use `-X`:

   ```sh
   cp -X zephyr.uf2 /Volumes/CPLAYBTBOOT/
   ```

   Without `-X`, `cp` can report `could not copy extended attributes`: the
   board restarts as soon as it has the firmware, and the drive vanishes
   under `cp`. The firmware loaded anyway.

The drive disappears and the board starts. To go back to CircuitPython or
Arduino, double-tap reset and load theirs the same way.

## Watch it run

A USB serial port appears (`/dev/cu.usbmodem…` on a Mac, `/dev/ttyACM…` on
Linux, a COM port on Windows). Open it with any serial terminal, for example
`tio /dev/cu.usbmodem114201`. You'll see `alive` every 10 seconds and a
status line from health every minute:

```
health: boot 4, MET 235 s, mode test (test_signal), least stack 300 B (radio), stalls 0, short runs 3, stopped 0x0
```

docs/nucleo-on-your-desk.md explains these lines. USB connects a moment after
the board starts, so you can miss the very first ones.

## Choose the boot mode

The slide switch sets the mode. The board reads it once, as it starts.

- **One position is test mode:** everything runs at once. The status line
  says `mode test`.
- **The other is the flight sequence:** deploy mode for 45 minutes, then
  safe mode (DS-42). The status line says `mode deploy`.

Move the switch, then press reset once.

## What to know

- **The FRAM is what remembers.** This chip has no backup SRAM, so without
  the breakout, or if it isn't wired right, every start is boot 1.
- **`short runs` and `reset_loop` are normal while you're experimenting.**
  Every run under 10 minutes counts as short. At three in a row health asks
  for safe mode, and at six it stops the nvm app (`stopped 0x80`). Let the
  board run for 10 minutes and the count clears.
- **No telemetry over USB yet.** The radio UART is on the TX and RX pads at
  19200 baud, and Zephyr's automatic USB setup makes only one serial port.
  To send commands and read telemetry you need a USB-serial link to those
  pads at 3.3 V: an adapter, or another CircuitPython board running a
  USB-to-UART bridge, wired TX to RX, RX to TX, GND to GND. Then run
  `sim/radio_sim.py` as in docs/nucleo-on-your-desk.md, in a clone of this
  repository. Telemetry has been read this way, through a Metro M0.
- **The temperature sensor is read in test and nominal mode.** It's in the
  `temperature` line of the telemetry: `temperature_mc` is millidegrees
  Celsius. The thermistor sits on the board, so expect a little above room
  temperature, and for it to rise if you hold a finger on the sensor.
  `apps/temperature` is the pattern for a device app (DS-30).
- **The motion and light sensors aren't read yet.**

## Debugging

There's no debug probe on the board. Printing to the console is the easy
way. For a debugger, the back of the board has three pads below the word
GROUND in the PLAYGROUND label: SWCLK, SWDIO and RESET. With wires soldered to
those, plus GND and 3.3 V, a J-Link works. Never run a chip erase: it removes
the UF2 bootloader.
