# The flight software on a Metro RP2350

The other board in the student learning and exploration tier (DS-04). You run
the real flight software on an Adafruit Metro RP2350, attached to your own
computer. The same board still runs CircuitPython and Arduino.

This is for learning. The RP2350 isn't the flight processor, so a test that
matters still belongs on the flatsat (docs/flatsat.md).

## What you need

- A Metro RP2350 and a USB-C cable that carries data.
- An Adafruit I2C FRAM breakout, 32 KB (ID 1895), and a Stemma QT cable with
  male pins on the other end. The FRAM keeps the boot count, the mode and the
  mission time when the board resets or loses power.
- A jumper wire from D2 to GND, for test mode.

Wire the breakout with the Stemma QT cable:

| FRAM breakout | Cable wire |
|---|---|
| VCC | red |
| GND | black |
| SCL | yellow |
| SDA | blue |

## Get the firmware

Build it in a Codespace:

```sh
make metro      # the first time also fetches the Raspberry Pi HAL, about 31 MB
```

The result is `build-metro/zephyr/zephyr.uf2`. Right-click it in the VS Code
Explorer and choose Download. Or skip building: every CI run keeps the UF2 as
an artifact named `zephyr-metro_rp2350`.

## Load it

1. Hold the Boot button, tap Reset, and let go of Boot. A drive named
   `RP2350` appears.
2. Copy the UF2 onto it. Dragging it in the file manager works. On a Mac, from
   a terminal, use `-X`, for the reason in docs/cpb.md:

   ```sh
   cp -X zephyr.uf2 /Volumes/RP2350/
   ```

The drive disappears and the board starts. To go back to CircuitPython or
Arduino, load theirs the same way.

## Watch it run

A USB serial port appears. Open it with any serial terminal, for example
`tio /dev/cu.usbmodem11301`. You'll see `alive` every 10 seconds and a status
line from health every minute. docs/nucleo-on-your-desk.md explains these
lines. USB connects a moment after the board starts, so you can miss the very
first ones.

## Choose the boot mode

The board reads D2 once, as it starts.

- **D2 wired to GND is test mode:** everything runs at once. The status line
  says `mode test`.
- **D2 left open is the flight sequence:** deploy mode for 45 minutes, then
  safe mode (DS-42). The status line says `mode deploy`.

Change the wire, then press Reset once.

## Telemetry and commands

These use the radio UART on D0 and D1, at 19200 baud and 3.3 V, not USB. You
need a USB-serial link to those pins. Both directions have been tested through
a Metro M0 running CircuitPython as a USB-to-UART bridge (docs/cpb.md).

Set the board's RX/TX switch to the position closest to the Boot button, where
the board reads `RX 1 TX 0`. Adafruit ships the switch in either position, so
check it. Then wire it like this, whatever the label seems to say:

| Metro RP2350 | Metro M0 bridge |
|---|---|
| D1 | D0 |
| D0 | D1 |
| GND | GND |

A USB-serial adapter should wire the same way, its RX to D1 and its TX to D0,
but that hasn't been tried yet. Then:

```sh
python3 sim/radio_sim.py --port /dev/cu.usbmodemXXXXX --baud 19200 --listen
```

Add a command to the end of that line to send one, for example
`temperature set_enabled false`.

A CircuitPython bridge has two USB ports. Use the data port, which on a Mac is
usually the one with the higher number. If you see only one port and the
bridge's NeoPixel flashes three times, it is in safe mode and the bridge isn't
running. Open its port and press Enter to read the reason, before you reset
it: a reset loses that message. Then take the TX and RX wires off and press
its Reset button once.

## What to know

- **The FRAM is what remembers.** Without it, or if it isn't wired right,
  every start is boot 1. Moved from another board, it keeps that board's boot
  count and mission time.
- **`short runs` and `reset_loop` are normal while you're experimenting.** The
  count is worked out once, at each start: a run under 10 minutes counts as
  short, so run the board for 10 minutes and the next start shows 0. docs/cpb.md
  says what happens at three and six.
- **Temperature is the chip's own.** The Metro has no other temperature
  sensor, so `temperature_mc` in the telemetry reads above room temperature
  and follows how hard the chip is working (DS-30).
- **No telemetry over USB.** See the section above.

## Known limits

- **No memory protection.** Turning on the MPU stops the board soon after
  `main` starts, even in Zephyr's blinky sample, so a stack overflow here is
  not caught as it is on the flatsat (DS-13).
- **The reset cause can be wrong.** After a watchdog reset, the boot log and
  telemetry say the reset pin caused it (DS-44). The watchdog itself works.
- **Two corrections live in our overlay**, in
  `app/boards/adafruit_metro_rp2350_rp2350b_m33.overlay`: the code is linked
  at the start of flash so the UF2 starts, and SCL's input is enabled so the
  FRAM answers.

## Debugging

Printing to the console is the easy way. The board's three-pin debug port
takes a Raspberry Pi Debug Probe, which we haven't tried yet.
