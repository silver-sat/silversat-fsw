# A Nucleo on your desk

How to run the flight software on a Nucleo-F446RE board plugged into your
own computer. The flatsat, a Nucleo every Codespace can reach, is in
docs/flatsat.md.

You build in the Codespace, as always. The Codespace runs in the cloud and
can't see your computer's USB ports, so you copy the firmware to your
computer and load it from there. One USB cable does everything: it powers
the board, loads the firmware, and carries the console.

## What you need

- A Nucleo-F446RE, and a USB cable that carries data, not just power.
- A serial terminal program. These steps use `tio`:
  - macOS: `brew install tio`
  - Linux: `sudo apt install tio`
  - Windows: PuTTY works; use the COM port Device Manager shows for the
    board.
- To read telemetry and send commands (section 6): a 3.3 V USB-serial
  converter, three jumper wires, and Python 3 on your computer.

The Nucleo has a debugger built in, the ST-LINK. To your computer it looks
like a USB drive (for loading firmware) and a serial port (for the
console). The serial port is wired to the F446's USART2, which is Zephyr's
console on this board.

## 1. Build

In the Codespace:

```sh
make build
```

This writes `build/zephyr/zephyr.bin`, the firmware.

## 2. Copy the firmware to your computer

In VS Code's Explorer, open `build/zephyr/`, right-click `zephyr.bin`, and
choose **Download…**. Save it somewhere easy to find, like Downloads. Do
this again after every build.

## 3. Load it onto the board

Plug the board into your computer, using the USB connector on the Nucleo
itself (the ST-LINK end), not a connector on a carrier board. The ST-LINK's
LED lights, and a drive named **`NOD_F446RE`** appears.

Leave both CN2 jumpers on: they connect the ST-LINK to the F446.

Drag `zephyr.bin` onto the `NOD_F446RE` drive. The LED flickers while it
programs, then the drive disappears and comes back, and the board runs the
new firmware.

If Finder reports an error copying (recent macOS versions sometimes do,
even when the copy worked), use the Terminal instead:

```sh
cp -X ~/Downloads/zephyr.bin /Volumes/NOD_F446RE/
```

If the drive keeps misbehaving, load it over the debugger instead
(`brew install stlink` on macOS):

```sh
st-flash --reset write ~/Downloads/zephyr.bin 0x08000000
```

## 4. Watch the console

Find the board's serial port, then open it at 115200 baud. On a Mac, use
the port's `/dev/cu.` name. Each port also has a `/dev/tty.` name, which
waits for a modem's carrier-detect signal before it opens, and with these
boards that means forever.

```sh
ls /dev/cu.usbmodem*           # macOS; on Linux, ls /dev/ttyACM*
tio -b 115200 /dev/cu.usbmodem1234
```

To quit `tio`, press Ctrl-T, then Q.

Press the black reset button (B2). You should see something like this (the
numbers vary):

```
*** Booting Zephyr OS build ... ***
[00:00:00.005,000] <inf> silversat: SilverSat FSW starting on nucleo_f446re/stm32f446xx
[00:00:00.014,000] <inf> mode_manager: boot in deploy mode (boot)
[00:00:01.015,000] <inf> health: boot 1, MET 1 s, mode deploy (boot), least stack 480 B (telemetry_output), stalls 0, short runs 0, stopped 0x0
[00:00:10.013,000] <inf> silversat: alive
```

The **mode manager** logs the boot mode and every mode change. **Health**
logs a status line about a second after boot and then every minute:

| Part | Meaning |
|---|---|
| `boot 1` | Boots since the records were blank (DS-44) |
| `MET 2 s` | Mission elapsed time; it carries on across a reset if the records survive (DS-25) |
| `mode deploy (boot)` | The mode, and why the spacecraft is in it (DS-41, DS-42) |
| `least stack 412 B (radio)` | The least stack any app has never used, and which app. `LOW` after it means under 256 bytes: that app's stack size in the resource map should grow (DS-44) |
| `stalls 0` | Stalls health has seen (DS-43) |
| `short runs 0` | Runs in a row, up to this boot, that ended within 10 minutes: a reset loop (DS-44). See below |
| `stopped 0x0` | Apps health has stopped; bit n is app n (the app ids are in `messages/apps/`) |

The console also shows every **event** as it happens: something an app
tells the ground about, such as a mode change, a stalled app, or FRAM
failing:

```
<inf> event: mode_manager mode_changed (info) mode=test reason=test_signal
<inf> event: health app_stalled (warning) app=nvm
```

These lines are for development only. The flight build leaves them out;
the events themselves still go to the ground (section 6).

## 5. Choose the boot mode

- **Deploy mode** is what a plain reset gives you: the flight sequence. For
  45 minutes nothing transmits and no ground command is accepted (DS-42).
  Then the spacecraft moves to safe mode, and every later boot starts in
  safe mode.
- **Test mode** runs everything at once. Hold the blue button (B1), press
  and release the black reset button (B2), and keep holding B1 for about
  two seconds. The mode manager reads it once, in the first second after
  boot, and logs:

  ```
  <inf> mode_manager: mode deploy -> test (test_signal)
  ```

  (After the first deployment it says `safe -> test`.)

## 6. Read telemetry and send commands

Housekeeping and command replies go out on the radio UART, not the console:
USART1, at 19200 baud, in KISS frames. In test, safe and nominal mode,
telemetry output sends one app's housekeeping every second; deploy mode
sends nothing. To read it as the ground will, you need a 3.3 V USB-serial
converter (never 5 V). These steps use Adafruit's FT232H breakout.

**Wire it.** On the Nucleo the radio UART's pins are labelled with their
Arduino names, **D8** (PA9, the F446 transmits) and **D2** (PA10, the F446
receives); not D0 and D1, which belong to the console.

| FT232H | Nucleo |
|---|---|
| D0 (it transmits) | D2 (PA10) |
| D1 (it receives) | D8 (PA9) |
| GND | GND |

Don't connect any power pins: each board has its own USB cable. If the
FT232H has an I2C mode switch, set it off. It appears on a Mac as
`/dev/cu.usbserial-XXXXXXXX`.

**Check bytes arrive.** `tio -b 19200 /dev/cu.usbserial-XXXXXXXX` shows a
burst of binary every second. In hex (Ctrl-T then `?` lists tio's keys)
each frame starts and ends with `c0`, and housekeeping starts `48` (`H`).
Quit tio before the next step: only one program can have the port open.

**Decode it**, on your own computer, in your clone of this repository (it
needs Python 3 and `python3 -m pip install pyyaml jinja2`):

```sh
python3 sim/radio_sim.py --port /dev/cu.usbserial-XXXXXXXX --baud 19200 --listen
```

```
MET 311.513 s  health: disabled_by_health=0, auto_exhausted=0, feeds=312, stalls=0, ...
MET 305.513 s  mode_manager: mode=test, reason=test_signal, transitions=1, ...
```

Each line is one app's housekeeping, decoded by name from the message
definitions, or an event:

```
MET 0.910 s  EVENT mode_manager mode_changed (info) mode=test reason=test_signal
```

Ctrl-C stops it.

**Send a command** the same way. It is signed with the published test key,
which the development build accepts, and the reply is printed:

```sh
python3 sim/radio_sim.py --port /dev/cu.usbserial-XXXXXXXX --baud 19200 \
    --listen nvm retry
```

For example, `frame_manager set_app_enabled nvm true` starts the nvm app
again after a reset loop stopped it. If a command gets no reply and the radio's
`frames_repeated` went up, the radio took it for a repeat of the frame
before (each frame carries a sequence number, and one that repeats the last
is dropped, DS-65). Each run of the simulator starts its numbers somewhere
random, so this is rare; send the command again. Commands are accepted in test, safe
and nominal mode, never in deploy mode.

## What to know

- **Repeated boot banners a few seconds apart** mean the watchdog is
  resetting the board: health has stopped feeding it, because an app it
  protects stalled, or health itself stalled. Look at the line before the
  banner, and tell a mentor.
- **Resetting the board often looks like a reset loop.** Every run shorter
  than 10 minutes counts, so a few quick resets at the bench add up:
  - at 3 short runs in a row, health asks for safe mode (reason
    `reset_loop`);
  - at 6, it also stops every app it's allowed to stop (today, only the
    nvm app: `stopped 0x80`, bit 7).

  The status line shows `short runs`. Stopped apps stay stopped for that
  boot. To start counting again, let one run last 10 minutes; then the
  next boot sees a long run. Or unplug the board, which clears everything
  (see the next point).
- **No FRAM is fitted yet.** Only the records kept in the mirror (the
  F446's backup SRAM) survive a reset: the mode, the deployment, the
  boot count, MET, the boot log, command state. Unplugging the board clears
  them, so the next boot is boot 1 again, in deploy mode.
- **The watchdog is live** (3 seconds). It pauses while a debugger halts
  the chip, so stepping through code won't reset the board.
- **Telemetry goes out on the radio UART**, not the console. See below.

## Going further

- **A debugger.** `west flash` and `west debug` need a probe your computer
  can reach, which the Codespace can't. If you have a SEGGER J-Link, wire it
  to the board's SWD pins and use SEGGER's tools on your computer. Take the
  CN2 jumpers off first, so the on-board ST-LINK lets go of those pins.
  Check your carrier board's manual.
- **RTT** (SEGGER's Real Time Transfer) carries log output over the debug
  connection instead of a UART. pyOCD (`pyocd rtt`) and OpenOCD can read it
  through the on-board ST-LINK, over the same USB cable. It would free
  USART2 for one of the five UARTs the flight design needs; it isn't set up
  yet.
