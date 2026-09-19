# SilverSat Flight Software

Zephyr RTOS flight software for the SilverSat cubesat program.

This is a [west "T2" workspace application][t2]: this repository is the west
manifest repository, and Zephyr itself is fetched alongside it.

[t2]: https://docs.zephyrproject.org/latest/develop/west/workspaces.html

## Layout

```
.
├── app/                 Application: entry point, board overlays, config
├── drivers/             Out-of-tree device drivers and their emulators
├── dts/bindings/        Devicetree bindings for SilverSat hardware
├── include/silversat/   Public headers shared across the module
├── tests/               ztest suites, run by twister
├── zephyr/module.yml    Declares this repo as a Zephyr module
├── CMakeLists.txt       Module build entry (NOT the application's)
├── Kconfig              Module Kconfig entry
└── west.yml             Manifest: pins the Zephyr version
```

## Getting started

### In a Codespace (recommended)

Press `.` or use the **Code → Codespaces** button. The devcontainer has the
Zephyr SDK and west already installed. Then:

```sh
west init -l app
west update
west build -b native_sim app
```

### Locally

native_sim requires a **Linux** host — Zephyr's POSIX architecture is
designed and tested for Linux only. On macOS or Windows, use a Codespace or
run the devcontainer locally under Docker or OrbStack. Cross-compiling for
the Nucleo works fine natively on any host; it is specifically native_sim
that needs Linux.

## Everyday commands

```sh
# Run the whole test suite under emulation
west twister -p native_sim -T tests --inline-logs

# Run one suite
west twister -p native_sim -T tests/drivers/mag --inline-logs

# Build and run the app under emulation
west build -b native_sim app && ./build/zephyr/zephyr.exe

# Build for the flatsat target (does not flash)
west build --pristine -b nucleo_f446re app
```

## How we work

Start by creating a branch for your changes:

```sh
git switch -c fix/whatever
```
Our branch naming convention is a prefix: fix/, feat/, docs/; plus a few words. This makes git branch readable.

Every pull request runs the emulated test suite. A PR needs a green check
and one review before it merges. No pushing to `main`.

Two rules that are not negotiable, because everything else depends on them:

**No `#ifdef CONFIG_BOARD_NATIVE_SIM` in driver or application code.** The
difference between the emulated part and the real one belongs entirely in
the devicetree overlay. If you find yourself reaching for that ifdef, the
design is wrong — stop and ask.

**When hardware finds a bug, the fix ships with a test that would have
caught it.** If an emulated test genuinely cannot catch it, say so in the
PR and put the test in the hardware suite instead. This is how the
emulation suite gets good over a season.

## Testing tiers

| Tier | Runs | Catches |
|------|------|---------|
| `native_sim` | Every push, seconds | Logic, protocol encode/decode, memory safety, long-duration scenarios, injected faults |
| Target build | Every push | Devicetree and Kconfig breakage, footprint |
| Flatsat (HIL) | Merge and nightly | Real SPI timing, DMA, power and reset behavior, sensor physics |

Simulated time in native_sim means a full orbit's worth of mode transitions
runs in milliseconds. Use it. A test that takes an hour of wall clock on
hardware can take a few hundred lines and no time at all here.

## Related repositories

- [`basilisk-sim`](https://github.com/Silver-Sat) — orbital dynamics, and the
  source of truth data used by sensor tests here.

## License

Apache-2.0, matching Zephyr.
