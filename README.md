# SilverSat Flight Software

Zephyr RTOS flight software for the SilverSat cubesat program.

## The Basics

Start by creating a branch for your changes:

```sh
git switch -c fix/whatever
```
You can do this from the command line in a terminal pane or using the VS Code interface to git.

Name your branch fix/, feat/, or docs/ plus a few words, so git branch is readable

After you are on your branch, create, test, and change your code. You will use four key commands to complete your work.

```sh
# Run the app under emulation — your fastest feedback loop
make run

# Run the emulated test suite
make test

# Build the flight software for the flatsat board
make build

# Load the firmware onto the flatsat (NOT YET CONNECTED)
make flash
```

Type `make` on its own to see everything available.

When your code is working, commit it to your branch, push it to GitHub, and create a pull request. Ask for an auto merge: if your code passes the integration tests, it will be automatically added to the main branch.

Then switch to main and pull the latest version.

```
git switch main
git pull
```

Now you're ready to start the next fix or feature.

## When Something Goes Wrong (and it will)

Stuck? Read the error from the top down — the first error is the real one; everything after it is fallout. If it mentions a file and line in your code, start there. If it mentions CMake, devicetree, or Kconfig, it's usually a config problem rather than a code problem, and worth asking about rather than guessing at.

## Our Configuration and Pipeline

We are using Zephyr to develop and run our avionics software. Zephyr uses a tool named `west` to assemble all of the software pieces needed. This is a [west "T2" workspace application][t2]: this repository is the west
manifest repository, and Zephyr itself is fetched alongside it.

[t2]: https://docs.zephyrproject.org/latest/develop/west/workspaces.html

## Layout

```
.
├── app/                 Application: entry point, board overlays, config
├── docs/                Design specifications (DS-nn) and rationale
├── drivers/             Out-of-tree device drivers and their emulators
├── dts/bindings/        Devicetree bindings for SilverSat hardware
├── include/silversat/   Public headers shared across the module
├── tests/               ztest suites, run by twister
├── zephyr/module.yml    Declares this repo as a Zephyr module
├── CMakeLists.txt       Module build entry (NOT the application's)
├── Kconfig              Module Kconfig entry
├── Makefile             Commands to run builds and tests
└── west.yml             Manifest: pins the Zephyr version
```

## Direct Tool Access

### In a Codespace (recommended)

Press . or use the Code → Codespaces button. The workspace is already set up — west, the Zephyr SDK, and Zephyr itself are installed and ready. Open a terminal and run `make`.

### Locally

native_sim requires a **Linux** host — Zephyr's POSIX architecture is
designed and tested for Linux only. On macOS or Windows, use a Codespace or
run the devcontainer locally under Docker or OrbStack. Cross-compiling for
the Nucleo works fine natively on any host; it is specifically native_sim
that needs Linux.

## Zephyr build commands

These are what the make targets actually run. You never need them, but they're here if you're curious or want to do something the Makefile doesn't cover. Here's an example:

```sh
# Run the whole test suite under emulation
west twister -p native_sim -T tests --inline-logs
```

Open the Makefile — every target is one west command, and they're short enough to read.

## How we work

Every pull request runs the emulated test suite. A PR needs a green check
before it merges. No pushing to `main`.

Two rules that are not negotiable, because everything else depends on them:

**No `#ifdef CONFIG_BOARD_NATIVE_SIM` in driver or application code.** The
difference between the emulated part and the real one belongs entirely in
the devicetree overlay. That's the way Zephyr works. If you find yourself reaching for that ifdef, the design is wrong — stop and ask.

**When hardware finds a bug, the fix ships with a test that would have
caught it.** If an emulated test genuinely cannot catch it, say so in the
PR and put the test in the hardware suite instead. This is how the
emulation suite gets good over time.

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

- https://github.com/silver-sat/basilisk-sim — orbital dynamics, and the
  source of truth data used by sensor tests here.

## License

Apache-2.0, matching Zephyr.
