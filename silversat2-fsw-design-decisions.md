# SilverSat 2 Flight Software — Design Specifications

| | |
|---|---|
| Status | Draft for mentor review |
| Last updated | 2026-09-30 |
| Scope | Avionics flight software, message definitions, simulators, flatsat, ground interface |

**Status legend**

- **Specified**: agreed; change only by revising this document.
- **Proposed**: recommended and not objected to; confirm at mentor review.
- **Open**: requires a decision; recommendation noted where one exists.
- **Monitor**: not adopted; watch for progress and ideas.

Specifications are numbered (DS-nn) so commits, issues, and reviews can reference them. When a specification changes, edit it in place and note the change in the revision history at the end, so `git log` and `git blame` show when and why.

---

## 1. Platform and environment

**DS-01 Zephyr RTOS: Specified.** Full reimplementation on Zephyr, not a port of the SilverSat 1 Arduino/SAMD21 code. C, not C++; C strings.

**DS-02 Prototype hardware: Specified.** STM32 Nucleo-F446RE is the flatsat target. The flight MCU will be right-sized later from the prototype. On-orbit update constraints (DS-51) feed into that choice.

**DS-03 Development environment: Specified.** GitHub Codespaces with a custom slim devcontainer. Code is housed in the Silver-Sat GitHub organization (`silver-sat/silversat-fsw`).

**DS-04 Test tiers: Specified.**
- native_sim plus Zephyr's emulator framework is the primary development and CI environment.
- Renode is an optional middle tier for advanced student work.
- A permanent networked flatsat (Mac Mini host, remotely managed USB switch for per-port power, relay on NRST, Tailscale) is for hardware tests.

**DS-05 CI: Proposed.**
- Every push: twister on `native_sim` and `native_sim/native/64` with ASan, UBSan, and coverage; a build-only check for `nucleo_f446re`; Zephyr compliance checks.
- Flatsat: runs on merge to main, nightly, and on demand.
- Keep the native_sim run under about three minutes.
- No `#ifdef CONFIG_BOARD_NATIVE_SIM` in driver or application code.

**DS-06 Build variants: Specified.**
- Development build: Zephyr shell, verbose logging, asserts enabled.
- Flight build: events only, no shell, defined assert policy.
- CI builds both.

**DS-07 Resource map: Specified.** A single header defines all thread priorities, stack sizes, zbus message pool sizes, and UART assignments.

## 2. Architecture

**DS-10 cFS-style architecture on zbus: Specified.** Adopt the cFS architecture and vocabulary without running cFS itself:
- a software bus
- apps as independent units
- housekeeping telemetry
- event messages
- command tables

zbus channels replace the software bus. Each app is one thread with one pending point.

**DS-11 Channel semantics: Specified.** zbus channels are last-value, not queues.
- Commands use message subscribers, because commands must not be lost.
- Telemetry and sensor data use plain subscribers or reads, where last-value is the desired behavior.

**DS-12 Data sharing: Proposed.** Apps share data only through channels, never through globals or each other's structs. Any `extern` reaching into another app's directory is a review flag.

**DS-13 Single address space: Specified.** This is accepted as the norm for microcontroller environments. Mitigations: the health app's heartbeat checks, the hardware watchdog, sanitizers in CI, and the MPU stack guard. Zephyr userspace/MPU isolation is held in reserve.

**DS-14 App pattern: Proposed.** Every app has:
- a command channel
- a housekeeping channel
- a status channel carrying a step counter and accepted/rejected command counters
- one thread

Apps timestamp their data from the frame tick, not by calling the clock.

## 3. Frame manager

**DS-20 Name: Specified.** The app is called the "frame manager," to avoid confusion with the Zephyr kernel scheduler.

**DS-21 Table-phased cyclic design: Specified.**
- A major frame of 1 second, divided into minor frames (initially 10 slots of 100 ms).
- A const table maps each slot to the channels published in it.
- Phasing is intentional: for example, sensors read in slot 0 and ADCS runs in slot 1.
- If an app needs a higher rate, increase the slot count rather than giving it a separate timer.

**DS-22 Mechanism: Proposed.** A `k_timer` wakes a high-priority thread, and the thread publishes. Nothing is published from the ISR. Publishes use `K_NO_WAIT`, and a full observer queue is counted as an overrun.

**DS-23 Enable/disable entries: Specified.** Table entries can be enabled and disabled by command, for recovery and power management. There is one table per mode (see DS-40), and table switches happen at major-frame boundaries, except entry into safe mode.

**DS-24 Thread priorities: Proposed.** Rate-monotonic assignment: the shorter an app's period, the higher its priority. The frame manager is highest, then 10 Hz apps (for example, ADCS), then 1 Hz apps, then housekeeping and telemetry output. Event-driven apps (command ingest, subsystem receive paths) are placed by required response time. All values live in the resource map (DS-07).

**DS-25 Time in the frame tick: Proposed.** Each tick carries the frame count, slot, uptime (elapsed time since this boot), and MET (mission elapsed time: total time since deployment, carried across resets from FRAM). RTC time is attached by telemetry output and events, not by apps.

**DS-26 Low power between frames: Proposed.** Enable Zephyr power management (`CONFIG_PM`, `CONFIG_TICKLESS_KERNEL`). The CPU enters a low-power state only when every thread is blocked, so an app still working at the end of its slot simply keeps the CPU awake; nothing is interrupted, and an app still busy at its next wakeup is counted as an overrun (DS-22).
- Nominal: Sleep mode (WFI) between frames. Peripherals, clocks, and UART reception keep running; wake latency is negligible.
- Low-power frame tables (for example, safe mode): deeper Stop mode is permitted only when no link transaction is in flight. Apps hold a power-state lock (`pm_policy_state_lock_get`) during SPI or UART transactions and while awaiting a reply. On the F446, Stop mode halts the high-speed clocks and ordinary UART reception, so its use depends on the subsystem links being quiet or on wake-up by a GPIO/EXTI line; the flight MCU choice (DS-02) should consider low-power UART support.
- The system clock frequency is set as low as the frame budget allows, and the frame manager's housekeeping reports idle percentage so the budget is measured, not guessed.

## 4. Device apps and subsystem apps

**DS-30 Device apps: Specified.** Sensors and simple peripherals (IMU, magnetometer, sun sensor, MCU die temperature, shift register) each get their own app, which drives a Zephyr driver. Student device apps use the classic sensor API (`sensor_sample_fetch` / `sensor_channel_get`).

**DS-31 Device app contents: Proposed.**
- A data channel with calibrated SI values, MET, sequence number, and validity flag.
- Housekeeping including raw values.
- Commands: enable, reinit, self-test, calibration load, range.
- A fault policy: N consecutive failures leads to reinit; M reinits marks the device failed and emits an event.

**DS-32 Subsystem apps: Specified.** Antenna, radio, payload, and power are subsystem apps: proxies for intelligent peers. Each connects over UART with CRC, unless the selected antenna or power system mandates its own protocol, in which case only that app's message layer changes.

**DS-33 Subsystem app behavior: Proposed.**
- Never block. Waiting is a state in a per-frame state machine.
- A shared link library handles framing, CRC, sequence numbers, and retries. Each subsystem app implements only its own message set.
- Mirror the peer's state, including how fresh it is. After a link timeout, report "unknown."
- Detect peer resets through a boot counter in the peer's telemetry.
- Payload firmware must answer status requests while a job runs. The fallback is a declared job duration.
- Recovery ladder: retry, then soft reset, then rail cycle through the power app's command channel, then mark failed.

**DS-34 Antenna deployment: Specified.** The entire deployment interface is in avionics, with no radio involvement. (SilverSat 1's radio had access to the secondary burn wire, which was a hardware artifact not to be repeated.) Deployment uses arm-then-fire, with the attempt count persisted in FRAM.

**DS-35 Idempotent commands: Proposed.** Commands set state rather than toggling it. One-shot actions carry their own guards.

**DS-36 Payload radio routing: Open.** Either the payload shares the radio independently, or payload traffic flows through avionics. Addressed packets are adopted now so either works. If independent, avionics retains authority: it controls the payload rail and can inhibit transmission. Radio arbitration is expressed through a payload-active frame table.

## 5. Health, modes, and safe mode

**DS-40 Mode manager: Proposed.**
- It is the sole publisher of `mode_chan`. Requests arrive on `mode_req_chan`, which uses a message subscriber.
- Transitions are defined in a const table: from, to, allowed sources, and a guard function.
- Entry and exit actions are commands published to other apps.
- The frame manager selects its table from `mode_chan`.

**DS-41 Safe mode: Proposed.**
- Any source may request entry. Only a ground command exits.
- Safe mode is sticky across resets: the mode manager persists the current mode and reason in a two-slot FRAM record on every transition, and reads it at boot.
- The safe frame table contains only trusted apps.
- Triggers: low battery (with hysteresis), reset loop, critical app failure, command-loss timer, ground command.
- The beacon carries the mode and the reason for it.

**DS-42 Boot promotion: Open.** Either boot waits in safe mode for the ground, or it promotes itself to nominal after a clean reset with good power. The recommendation is to start conservative and make the promotion row enable-able by command.

**DS-43 Health and watchdog: Proposed.**
- Health checks each app's step counter against the number of wakeups the frame manager delivered.
- Graded response: event, then disable the app, then reset if a critical app stalled. Individual threads are not restarted.
- Health is the only feeder of the hardware watchdog (IWDG), which forms the chain frame manager, then health, then IWDG.
- Use `WDT_OPT_PAUSE_HALTED_BY_DBG` on the flatsat.

**DS-44 Boot handling: Proposed.**
- Record the reset cause (`hwinfo`).
- Keep a FRAM boot log, and boot into the safe table after repeated short runs.
- Record which app triggered a health-initiated reset.
- Report each app's stack high-water mark in housekeeping.

**DS-45 Launch timers: Specified.** Time since deployment is kept in FRAM, so a reset neither restarts the post-ejection waits nor skips them. Values come from the launch provider's interface document.

**DS-46 Command-loss timer: Proposed.** Measures time since the last *accepted* ground command, not the health of the command ingest app (health covers that through its step counter, DS-43). Command ingest stores the MET of acceptance with the counter floor and publishes it in its status. The mode manager compares it against the timeout and requests safe mode. Because the value is in FRAM, resets do not restart the timer.

## 6. Commanding and security

**DS-50 Command ingest pipeline: Proposed.** Stages, in order:
1. Shape check
2. Signature verification over the exact received bytes
3. Counter check (after the signature, so forged packets can't raise the floor)
4. Persist the new floor to FRAM
5. ACK acceptance, carrying the counter
6. Decode (the parser only sees authenticated input)
7. Route through the table

Mode gating lives in the routing table. Parameter validation is done by the target channel's validator. Every rejection stage has its own counter and event. Internal commands bypass command ingest.

**DS-51 On-orbit software update: Specified (plan).**
- Plan for MCUboot with signed images and revert-on-failure.
- The flatsat runs in two modes: development mode (direct flash plus GDB) and update mode (signed image uplinked through the flight path).
- **Open:** the F446 sector layout limits the image to about 128 KB with swap. Options are a secondary slot in external SPI flash, or weighing update support in the flight MCU choice. Measure the image size early.

**DS-52 Signing: Specified.** Keyed BLAKE2, continuing from SilverSat 1:
- A shared secret initializes the MAC.
- The tag covers salt, sequence, and command.
- Commands are signed, not encrypted.
- Commands and tags are printable ASCII, with tags as hex.
- Use BLAKE2s on the MCU, from the reference implementation or Monocypher, since Zephyr's crypto stack doesn't include it.

**DS-53 Replay protection: Specified / Proposed.**
- *Specified:* a 64-bit monotonic counter, accepted if strictly greater than the floor, with gaps allowed. After a loss, the ground jumping ahead re-establishes the floor, as in SilverSat 1. The ACK is sent on acceptance, with no retry mechanism.
- *Proposed:* the floor is persisted in FRAM before execution. The counter is epoch milliseconds, and the ground sends the larger of the current time and the last value plus one.
- *Proposed:* the counter never wraps. Comparison is plain unsigned 64-bit (not the serial-number arithmetic used for FRAM generations), and code must never compute `floor + 1` where it could overflow; a unit test covers the maximum value. A command carrying the maximum value is accepted once and sets the floor to the maximum, after which all commands are rejected. Wrapping to zero would make every previously recorded command valid again.
- *Proposed:* guard against an erroneous large counter in two places. The ground software refuses to send a counter more than a set margin (for example, one day) ahead of its clock. The spacecraft rejects any counter more than a maximum jump (for example, one year in milliseconds) above the floor, with an event, which still allows the SilverSat 1 style jump-ahead recovery.
- *Proposed:* the floor is stored per key slot. Rotating to the other key (DS-54) resets that slot's floor to zero, because commands recorded under the old key no longer verify. This is the recovery if the floor is ever set too high.

**DS-54 Keys: Specified / Proposed.**
- *Specified:* the flight key is kept outside Git. This is not a high-security environment, and the realistic risk is accidental commanding, not a sophisticated adversary.
- *Specified:* separate test and flight keys, so flatsat and Codespace setups cannot command the flight vehicle.
- *Proposed:* flight builds get the key from a CI secret.
- *Proposed:* two key slots allow rotation as people leave, and provide recovery from a maxed counter floor (DS-53). Rotation is signed by the other slot and uses arm-then-fire.
- *Proposed:* keys are compiled into flash; FRAM holds only which slot is active and the per-slot floors, so a FRAM failure cannot lose the keys (DS-75).
- *Proposed:* tags are compared in constant time, mainly as a lesson.

## 7. Messages and links

**DS-60 Single source of truth: Specified.** All commands, telemetry, events, and channel payloads are defined in YAML. Changes must propagate easily to avionics, simulators, flatsat, ground station, and other teams' subsystems.

**DS-61 Generated outputs: Specified (requirement) / Proposed (design).** A Python generator using Jinja2 produces:
- C headers plus encode, decode, and validate functions
- Python classes plus text formatting and parsing
- a resolved JSON dictionary for other languages
- a generated interface control document
- golden vectors, checked by both the C and Python test suites

The dictionary version and hash are included in the beacon.

**DS-62 Deprecation: Specified (requirement) / Proposed (design).**
- Lifecycle: active, then deprecated, then retired.
- Deprecated messages emit compiler and Python warnings.
- Retired IDs remain as tombstones, and command ingest reports them as "retired" rather than "unknown."
- CI enforces the path: no deleting active messages, a minimum of one release in deprecated status, and no ID reuse.
- The ground keeps every dictionary version the spacecraft might be running.

**DS-63 Repository: Open.** The recommendation is a separate repo now, consumed via west, pip, and release artifacts. If deferred, keep the definitions in a self-contained directory with versioning enforced in CI.

**DS-64 Wire encoding: Proposed.** Encode explicitly, field by field; never copy structs onto the wire. Little-endian (Cortex-M native). The ground absorbs any accommodation.

**DS-65 CRC: Proposed.** All inter-board links and FRAM records use CRC-32C (Castagnoli). Other teams' subsystems must implement exactly these parameters:

| Parameter | CRC-32C (primary) | CRC-32/ISO-HDLC (fallback) |
|---|---|---|
| Polynomial | 0x1EDC6F41 (reflected 0x82F63B78) | 0x04C11DB7 (reflected 0xEDB88320) |
| Initial value | 0xFFFFFFFF | 0xFFFFFFFF |
| Reflect in / out | Yes / Yes | Yes / Yes |
| Final XOR | 0xFFFFFFFF | 0xFFFFFFFF |
| Check value (ASCII "123456789") | 0xE3069283 | 0xCBF43926 |
| Implementations | Zephyr `crc32_c()`; Python `crc32c` package or a 20-line table | Zephyr `crc32_ieee()`; Python `zlib.crc32` |

- The CRC is transmitted as 4 bytes, little-endian (DS-64).
- It is computed over the frame's sequence number, length, KISS command/type byte, and payload, before KISS escaping, and checked after unescaping.
- The fallback, the common IEEE CRC-32 used by Ethernet and zlib, is used only if a subsystem cannot support CRC-32C. It is chosen per link and recorded in that link's interface document; there is no negotiation on the wire.
- The golden vectors (DS-61) include CRC test frames for every link.

**DS-66 KISS framing: Specified / Open.**
- *Specified:* keep KISS on all serial links.
- *Specified:* support the standard data frame (0x00) if required.
- *Specified:* command packets are under 256 bytes.
- *Open:* printable KISS command bytes. The SilverSat 1 lessons learned recommended printable values as KISS command bytes, because non-printable values required extra code for logging and debugging. Leaving this open reverses that recommendation pending the radio decision. The trade-off: in standard KISS, the type byte's high nibble is the port and the low nibble the command, so a printable byte such as 0x41 ('A') reads as "port 4, command 1" (TX delay). With off-the-shelf radios or any stock KISS device in the path, printable type bytes may be acted on as radio parameters. The alternative keeps readability everywhere but one fixed byte: use SetHardware (0x06) with a printable subcommand as the first payload byte. Printable type bytes are safe only if we control the firmware at both ends of every link. Decide when the radio is selected.

**DS-68 Per-app message ownership: Proposed.** Every message in the YAML names an owning app (`owner:`, plus `dest:` for commands).
- The generator emits one header per app containing only that app's messages, plus a shared header for common types (tick, status, events).
- The generator also emits the channel definitions (in the owner's module) and command ingest's routing table, so ownership is stated once.
- CI lint: an app includes only its own generated header and the shared one, and only the owner publishes its telemetry and housekeeping channels. Publishing to another app's *command* channel remains allowed; that is how internal commands work.

**DS-69 Beacon contents: Proposed.** The beacon answers, in one reception, "is the spacecraft alive, what state is it in, and can I command it?" It is defined in the message definitions like any other telemetry (DS-60), transmitted unencrypted, and generated by telemetry output from housekeeping channels, so no app builds beacon bytes itself. Contents, in priority order so a truncated or short beacon keeps the most important fields:

| Group | Fields | Why |
|---|---|---|
| Identification | Callsign; beacon format version; dictionary version and hash (DS-61) | Regulatory identification; tells the ground which dictionary decodes everything else |
| Mode | Current mode; reason for entry; time in mode | First thing an operator needs on a pass (DS-41) |
| Time | RTC time and validity flag; MET; uptime | Correlates ground and spacecraft time; shows recent resets (DS-72) |
| Resets | Boot count; last reset cause; health-initiated reset source, if any | Reveals reset loops and watchdog activity (DS-44) |
| Power | Battery voltage; state of charge or current; rail on/off bitmask; EPS link state and data age | Explains most safe-mode entries |
| Commanding | Active key slot; last accepted counter; MET of last accepted command; command ingest rejection counts (summary) | Lets the ground pick the next counter and diagnose rejected commands without a round trip (DS-50, DS-53, DS-54) |
| Health | Bitmask of disabled or failed apps; watchdog, overrun, and stack warning flags | One-glance fault summary (DS-43) |
| Storage | FRAM status (OK, degraded, failed); scrub repair count | Flags operation without FRAM (DS-75) |
| Software | Image version and git hash; boot slot; image confirmed flag | Confirms an update took, or that it reverted (DS-51) |
| Subsystems | Antenna deploy state and attempt count; payload job state; radio state; peer link status for each subsystem | Status of each proxy's view of its peer (DS-33, DS-34) |
| Environment | MCU die temperature; battery temperature | Basic thermal health |

- Two forms. The Morse beacon carries a short, fixed subset (callsign, mode, battery, boot count, a status bitmask) readable by ear. The packet beacon carries the full set, rendered as printable text for readability, or binary if the link budget requires it.
- Interval and form are set per frame table, so safe mode can beacon more often, at a lower rate, or Morse only.
- The beacon never contains key material or data that would let someone forge commands. The last accepted counter is not secret: without the key it cannot be used.
- Beacon contents, including the Morse text, are reviewed for regulatory compliance before flight.
- Fields are added or retired through the normal message lifecycle (DS-62); the beacon format version changes whenever the layout does.

## 8. Storage and time

**DS-70 FRAM service: Proposed.**
- A synchronous library, not a frame-driven app, since command ingest must commit before ACK and health reads at boot.
- A small `nvm` app handles housekeeping and scrubbing.
- A `k_mutex` covers each record operation.
- Uses Zephyr's EEPROM API at the device level, with a custom record layer rather than settings, NVS, or ZMS.

**DS-71 FRAM layout and atomicity: Proposed.**
- A fixed region map, with no filesystem.
- Two-slot records with magic, version, generation, and CRC; the valid slot with the newest generation wins, using serial-number arithmetic.
- The boot log is a headless ring: each entry is written once, placed at `boot_num % N`, and carries the previous run's duration from the checkpoint record.
- Hardware write-protect on the key and configuration region.

**DS-72 Time: Specified.**
- Record RTC time (via Zephyr's RTC API), most recent elapsed boot time, and total mission time, which is persisted in FRAM.
- The RTC is reset from the ground periodically rather than calibrated.
- UTC drift is monitored manually on the ground. This is mission dependent.
- *Proposed:* log every time set as an event with old and new values.

**DS-73 Telemetry budget and store-and-forward: Open.** To be determined after mission selection. It may require external flash and a data storage app.

**DS-74 FRAM ownership: Proposed.** FRAM records are data sharing, and follow the same rule as channels (DS-12): each region has exactly one owning app, which alone writes it. Other apps learn the contents through the owner's channels, never by reading the record.
- The region map is controlled from a single file, `nvm_map.yaml`, maintained alongside the message definitions and processed by the same tooling (DS-61). For each region it gives the name, owner, record size and version, slot count, write-protect flag, and flash default (DS-75). The generator computes addresses and emits `nvm_map.h` (addresses, sizes, owners, and per-owner handle declarations), the ground dump decoder, and a table in the interface document. CI fails on overlap, on exceeding the FRAM size, on a changed record without a version bump, and on write-protected regions outside the protected block range. Nobody assigns addresses by hand.
- Region handles are defined `static` in the owner's source file, so no other app can name them; development builds add a runtime check of the calling thread against the owner in the region map.
- Initial map:

| Region | Owner |
|---|---|
| Boot log, run checkpoint, mission time | Health |
| Command counter floor, last-accepted MET | Command ingest |
| Current mode and reason | Mode manager |
| Launch/deployment timers | Mode manager |
| Antenna deploy attempts and arm state | Antenna app |
| Persistent subsystem state (for example, payload job ID) | The owning subsystem app |
| Calibration tables | The owning device app, loaded by command |
| Keys | Command ingest (read-only; write-protected) |

- Peer boards (payload, power, antenna, radio) never access avionics FRAM; their persistent state lives on their own hardware, and bulk data goes to store-and-forward (DS-73), not FRAM.
- The ground reads FRAM through housekeeping and a read-only memory-dump command against the fixed map. Ground writes go through the owning app's commands (for example, calibration load), never raw writes.

**DS-75 Operation without FRAM: Proposed.** FRAM is valuable but unproven in flight, so the spacecraft must keep operating if it fails, accepting lower security and lost information.
- Every record has a const default in flash, generated from `nvm_map.yaml`. A record that cannot be read falls back to its default, never to garbage.
- The F446's 4 KB backup SRAM and backup registers (VBAT domain) form a second tier. They survive resets and, if VBAT is powered, main power loss. Critical small records are mirrored there: counter floors, mode, boot counter, launch timers, deploy attempts.
- The FRAM service detects failure from the device ID at boot, SPI errors, and both slots failing CRC. It then marks FRAM degraded, stops using it, raises an event, and sets a beacon flag. The ground can command a retry.
- Degraded behavior by record:

| Record | Degraded behavior |
|---|---|
| Command counter floor | Use the backup SRAM copy. If that is also lost, use RTC time minus a tolerance as the floor, which still rejects old recorded commands if the counter is epoch milliseconds; if the RTC is invalid, accept any counter above the first one accepted this boot (replays from earlier boots possible) |
| Mode | Boot into safe mode |
| Boot log and reset-loop detection | Boot counter in backup registers; history lost |
| Launch/deployment timers | Backup SRAM copy; if lost, restart the wait (delays deployment, never violates the required wait) |
| Antenna deploy attempts | Backup SRAM copy; if lost, require ground authorization for further attempts |
| Mission time | Lost; reconstructed on the ground from RTC and beacon history |
| Command-loss timer | Restarts each boot |
| Keys | Unaffected (in flash); active slot reverts to the default |
| Calibration tables | Flash defaults; reload by command |

- This reverses a SilverSat 1 lesson. There, FRAM was "good to have, not used," and the recommendation was to put it on a non-critical bus. SilverSat 2 places FRAM on the main SPI bus and relies on it for security and recovery state. The reversal is accepted because SPI removes the I2C hang-on-reset concern that motivated isolating it, and because this specification ensures FRAM loss degrades operation rather than ending it.
- A native_sim test configuration runs the full suite with the FRAM emulator failing, to prove the spacecraft still boots, commands, and reaches safe mode.

## 9. Reliability

**DS-80 Radiation mitigation: Specified.**
- Hangs and crashes: the watchdog chain, graded recovery, and sticky safe mode.
- RAM upsets (no ECC on the F446): const tables in flash, CRC on long-lived RAM state, and voting for critical values.
- Storage: two-slot FRAM records, scrubbing, and write-protect.
- Latch-up and stuck peripherals: reinit, then rail cycle.
- Visibility: upset and repair counters in housekeeping.

## 10. Simulation and ground

**DS-90 Subsystem simulators: Specified.**
- Written in Python as external processes, connected over native_sim PTY UARTs in the Codespace.
- On the flatsat, the same simulators run on the Mac Mini through USB-serial adapters.
- The flight code is unchanged across stages; only the far end of the wire changes.
- Each simulator has a fault menu.

**DS-91 Shared Python package: Specified.** One Python codebase provides KISS, CRC, sequencing, and signatures for both the satellite radio simulator and the ground station interface. The local serial link layer and the end-to-end space link layer are kept distinct.

**DS-92 Simulation timing: Proposed.** External-peer tests run in real time (`CONFIG_NATIVE_SIM_SLOWDOWN_TO_REAL_TIME`) through twister's pytest harness. Minimal C in-image fakes are used only for long simulated-time scenarios. Golden frames keep the C and Python implementations aligned.

**DS-93 Ground station: Specified.**
- Extend the existing ground solution known to mentors and returning students.
- Student exercises: generate Doppler corrections without gpredict (for example, with Skyfield) and incorporate additional telemetry.
- *Proposed:* make the radio connection pluggable, serial for the real radio and TCP for simulated paths. The Codespace uses local connections, a laptop uses `gh codespace ports forward` or VS Code forwarding, and the flatsat is reached over Tailscale.

**DS-94 Frameworks: Monitor.** F Prime (including fprime-zephyr and the PROVES Kit), YAMCS, and the cFS Zephyr OSAL port are not adopted. Watch them for progress and ideas.

## 11. Student ownership

| Area | Owner |
|---|---|
| Device apps: die temperature, shift register, IMU, sun sensor, magnetometer | Students (progression in roughly that order) |
| Sensor and device drivers (for example, magnetometer, FM25 FRAM) | Stronger students, against emulators |
| FRAM record layer, including exhaustive torn-write tests | Students, mentor-reviewed |
| Python simulators and fault menus | Students |
| Message tooling: Python generator, JSON output, docs, golden vectors, schema validation, compatibility checker | Students |
| Link-layer codecs and state-machine tests | Students |
| Ground station Doppler and telemetry extensions | Students |
| Frame manager, health, mode manager, command ingest, telemetry output | Mentors (students write rejection-path and transition tests) |
| Subsystem apps | Mentors or strongest students |
| YAML schema semantics, C templates, FRAM region map, resource map | Mentors |

## 12. Open items

| ID | Item |
|---|---|
| DS-36 | Payload radio routing |
| DS-42 | Boot promotion policy |
| DS-51 | Image size versus F446 flash layout for updates |
| DS-63 | Message definitions repository timing |
| DS-66 | Printable KISS command bytes versus off-the-shelf radio compatibility |
| DS-73 | Telemetry budget and store-and-forward |
| DS-69 | Beacon size, interval, and Morse subset, after radio and mission selection |
| — | Mission objectives and form factor (1U expected) |
| — | Minor frame rate once ADCS requirements are known |
| — | Pin mux check for five UARTs on the Nucleo-F446RE |
| — | Driver and emulator support for the selected IMU and FRAM parts in the pinned Zephyr version |

---

## Revision history

| Date | Change |
|---|---|
| 2026-09-30 | Initial draft from architecture discussions |
| 2026-09-30 | Renamed to specifications (DS-nn, "Specified"); Renode optional; generic remotely managed USB switch; added DS-24 priorities, DS-25 tick time and MET, DS-46 command-loss timer, DS-68 message ownership, DS-74 FRAM ownership; safe-mode persistence in DS-41 |
| 2026-09-30 | Added DS-26 low power, DS-75 operation without FRAM, single-point FRAM region map (DS-74), counter bounds and per-key floors (DS-53); test/flight key separation Specified; CRC parameters and IEEE fallback in DS-65; removed DS-67 (belongs to the radio specification) |
| 2026-09-30 | DS-66 printable KISS command bytes marked Open, with reversal of the SilverSat 1 lesson and trade-off noted; FRAM bus reversal stated in DS-75 |
| 2026-09-30 | Added DS-69 beacon contents |
