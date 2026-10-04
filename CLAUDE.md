# SilverSat 2 Flight Software

Flight software for SilverSat 2, a student CubeSat built by high school students with volunteer mentors. Zephyr RTOS on STM32 (Nucleo-F446RE flatsat; flight MCU to be right-sized later).

## Read first

- `docs/silversat2-fsw-design-decisions.md` is the authoritative specification. Entries are numbered DS-nn with a status: **Specified** (settled), **Proposed** (recommended, pending mentor confirmation), **Open**, **Monitor**. Cite DS numbers in commits, PR descriptions, and code comments where a design choice isn't obvious.
- `docs/design-rationale.md` explains why, lists rejected alternatives, and shows the code patterns to follow. Read it before writing a new app, touching FRAM, or changing command handling.
- Do not implement anything marked **Open** without asking. For **Proposed** items, follow the proposal but flag it in the PR.

## Audience

Code is read and extended by high school students working a few hours a week. Prefer plain, explicit C over clever C. Every app should look like every other app. Comments explain *why*; a student should be able to follow one app end to end.

## Language and platform

- C, not C++. C strings; no dynamic allocation after init.
- Zephyr APIs over hand-rolled equivalents (drivers, `sensor`, `eeprom`, `hwinfo`, `rtc`, `wdt`, zbus).
- Classic sensor API only: `sensor_sample_fetch` / `sensor_channel_get`. Not the RTIO read/decode API.
- Everything static: channels, subscribers, threads, pools sized at build time. All thread priorities, stack sizes, pool sizes, and UART assignments come from the single resource map header (DS-07); never hard-code them in an app.

## Architecture rules

- cFS-style apps on zbus (DS-10). One app = one directory, one thread, one pending point (`zbus_sub_wait_msg`).
- Each app has a command channel, a housekeeping channel, and a status channel (step counter, accepted/rejected command counters) (DS-14). Every app publishes its housekeeping at least once per major frame (at slot 0, or when it changes); telemetry output reads the latest value, and nobody requests housekeeping.
- Apps share data **only** through channels. No globals shared across apps; no `extern` reaching into another app's directory (DS-12).
- Commands use message subscribers (never lost). Telemetry and sensor data use last-value channels (DS-11).
- Periodic work is driven only by the **frame manager** (not "scheduler"; that name belongs to the Zephyr kernel) (DS-20). Apps never start their own timers.
- Timestamp data from the frame tick (`met`, `uptime`), not by calling the clock (DS-25).
- Publish with `K_NO_WAIT` from the frame manager and command ingest; never block the frame. Nothing publishes from an ISR.
- Subsystem apps (antenna, radio, payload, power) never block waiting on a peer: waiting is a state in a per-frame state machine (DS-33).
- Commands set state; they do not toggle it. One-shot actions (antenna deploy, key rotation) use arm-then-fire with persisted state (DS-35).
- Only command ingest accepts ground commands. Internal commands are published directly to the target's command channel.
- Only the mode manager publishes `mode_chan`.

## Messages

- All commands, telemetry, events, and channel payloads come from the YAML message definitions and the generator (DS-60, DS-61). Never hand-write a message struct, ID, encoder, or routing entry. To change a message, edit the YAML and regenerate.
- Include only your app's generated header and the shared header (DS-68).
- Never copy a struct onto the wire; use generated encode/decode. Little-endian (DS-64).
- CRC-32C with the parameters in DS-65, computed before KISS escaping.
- Do not delete or reuse message IDs; use the deprecation lifecycle (DS-62).

## FRAM

- Access only through the FRAM service with a region handle defined `static` in the owning app (DS-74). Never address FRAM directly.
- Region addresses come from the generated map (`nvm_map.yaml`); never assign addresses by hand.
- Every record must have a flash default and a defined behavior when FRAM is unavailable (DS-75). Code must work with FRAM degraded.
- Command counter comparisons are plain unsigned 64-bit; never serial-number arithmetic, never `floor + 1` where it can overflow (DS-53).

## Testing

- Every change needs tests that run on native_sim. Test layers: `tests/unit/`, `tests/drivers/` (against emulators), `tests/app/`, `tests/hil/` (hardware only).
- No `#ifdef CONFIG_BOARD_NATIVE_SIM` (or any board check) in driver or application code. Differences belong in devicetree overlays and Kconfig.
- Use native_sim simulated time for long scenarios. Tests with external Python simulators run in real time; keep them short.
- Fault paths matter as much as the happy path: every rejection, timeout, and recovery step should have a test.
- CI must stay green on `native_sim` and `native_sim/native/64` with ASan/UBSan, and `nucleo_f446re` build-only. Keep the native_sim run under about three minutes.

## Build variants

- Development build: shell, verbose logging, asserts.
- Flight build: events only, no shell. Nothing development-only may be required for the flight build to function.
- Never commit keys. Development uses the published test key; the flight key is injected only for flight builds.

## Working style

- Small, reviewable PRs; squash merges. Explain design choices in the PR with DS references.
- When a change would alter a Specified entry, stop and ask rather than working around it.
- If you notice the spec and code disagree, say so; don't silently pick one.
