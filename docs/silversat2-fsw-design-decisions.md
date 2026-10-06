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
- Keep the native_sim run under about three minutes. Since 2026-10-06 the tests run as four CI jobs in parallel, each platform in two halves (twister `--subset`), each with its own compiler cache: most of a build is CMake configuration, which no cache speeds up, so fewer builds per job is what shortens the run. A job named `native_sim tests`, which the branch rules require, passes when all four do. Tests that need no boot of their own share a build (`tests/unit/libs`).
- No `#ifdef CONFIG_BOARD_NATIVE_SIM` in driver or application code.

**DS-06 Build variants: Specified.**
- Development build: Zephyr shell, verbose logging, asserts enabled.
- Flight build: events only, no shell, defined assert policy.
- CI builds both.

**DS-07 Resource map: Specified.** A single header defines all thread priorities, stack sizes, zbus message pool sizes, and UART assignments.
- A UART assignment is a devicetree alias (for example `radio-uart`) named in the header and bound to a UART in each board's overlay: on native_sim a pseudo-terminal, on the Nucleo a USART and its pins.
- It also holds one const attribute row per app: the protected flag, the stall threshold, and the re-enable policy (DS-43). The frame manager and health both read this row, so each app's attributes are stated once.
- The zbus message pool size is computed in the header from the app counts: `FRAME_MAX_PENDING` for each frame-driven app, plus `CMD_MAX_PENDING` for each app's command channel (ground commands) and for each pair of sending app and target app (internal commands, DS-68), plus one buffer for each thread that publishes, because every publish holds a buffer while it runs (DS-22). Zephyr takes the pool size from Kconfig, so a build check fails if the Kconfig value is smaller than the computed size.
- Every buffer must hold the largest message on any channel, observed or not, because every publish takes one. The generated channel definitions fail the build if a message does not fit.

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
- zbus messages stay small. Every buffer in the shared pool is sized for the largest message on any channel (DS-07), so one large message would enlarge them all. Raw link frames (uplink and downlink, up to 255 bytes, DS-66) travel between a link app and the app that uses them through a static `k_msgq` sized in the resource map, not through zbus.

**DS-12 Data sharing: Proposed.** Apps share data only through channels, never through globals or each other's structs. Any `extern` reaching into another app's directory is a review flag.

**DS-13 Single address space: Specified.** This is accepted as the norm for microcontroller environments. Mitigations: the health app's heartbeat checks, the hardware watchdog, sanitizers in CI, and the MPU stack guard. Zephyr userspace/MPU isolation is held in reserve.

**DS-14 App pattern: Specified.** Every app has:
- a command channel
- a housekeeping channel
- a status channel carrying a step counter and accepted/rejected command counters
- one thread

Apps timestamp their data from the frame tick, not by calling the clock.

Every app publishes its housekeeping at least once per major frame (at slot 0, or whenever it changes), so the latest value on the channel is never more than a second old. Telemetry output reads that latest value; nobody requests housekeeping.

## 3. Frame manager

**DS-20 Name: Specified.** The app is called the "frame manager," to avoid confusion with the Zephyr kernel scheduler.

**DS-21 Table-phased cyclic design: Specified.**
- A major frame of 1 second, divided into minor frames (initially 10 slots of 100 ms).
- A const table maps each slot to the channels published in it.
- Phasing is intentional: for example, sensors read in slot 0 and ADCS runs in slot 1.
- If an app needs a higher rate, increase the slot count rather than giving it a separate timer.

**DS-22 Mechanism: Proposed.** A `k_timer` wakes a high-priority thread, and the thread publishes. Nothing is published from the ISR. Publishes use `K_NO_WAIT`.
- Each app receives on a single message subscriber (one pending point), so every wakeup is a queued copy holding a buffer from the zbus pool, which all channels share. A stuck app's copies pile up until the pool is nearly empty. Then a copy that cannot be allocated makes the publish return an error, but another publish in flight at that moment cannot get its own buffer and faults (see the rationale, section 2).
- Overruns are therefore detected by per-wakeup counting. The frame manager counts the wakeups it has delivered to each app and reads the app's step counter from its status channel. The counts are per app, not per table entry, because an app that runs in several slots has one step counter. If delivered minus steps has reached `FRAME_MAX_PENDING` (initially 2, the same for every app), the frame manager skips the publish and counts an overrun for that app. This bounds each app's share of the pool.
- If the frame manager itself falls behind, so that more than one minor frame has started since it last ran, it skips the minor frames it missed and counts them. The slot stays in step with time, and the skipped slots' wakeups are not sent.

**DS-23 Enable/disable entries: Specified.** Table entries can be enabled and disabled by command, for recovery and power management. All of one app's entries, in every mode, can be stopped or started at once (`set_app_enabled`, used by health, DS-43). Entries for protected apps (DS-43) cannot be disabled; the command is rejected. There is one table per mode (see DS-40), and table switches happen at major-frame boundaries, except entry into safe mode.

**DS-24 Thread priorities: Proposed.** Rate-monotonic assignment: the shorter an app's period, the higher its priority. The frame manager is highest, then 10 Hz apps (for example, ADCS), then 1 Hz apps, then housekeeping and telemetry output. Event-driven apps (command ingest, subsystem receive paths) are placed by required response time. All values live in the resource map (DS-07).

**DS-25 Time in the frame tick: Proposed.** Each tick carries the frame count, slot, uptime (elapsed time since this boot, 64-bit milliseconds, so it never wraps), and MET (mission elapsed time: total time since deployment, carried across resets from FRAM). RTC time is attached by telemetry output and events, not by apps.

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
- The radio app, decided 2026-10-04: woken every minor frame, in the slot before command ingest. The UART interrupt only moves bytes between the UART and two static ring buffers, one each way; nothing is published from it. Each frame the app decodes what arrived, passes ground frames to the uplink queue, drops a repeated frame, counts lost ones from sequence gaps, and encodes waiting downlink frames for the interrupt to send.
- Transmit inhibit, decided 2026-10-04: `radio set_transmit <true|false>` sets whether avionics transmits at all. While it is false the radio app throws away every downlink frame (replies, telemetry, and the beacon) and counts them, and keeps receiving, so ground commands still work. Stopping takes effect after one more radio step, which sends what is queued by then. Command ingest queues the reply after routing, in the same minor frame, so the ground sees the ACK and then silence, whichever of the two equal-priority threads runs first. Only a ground command sets it back to true. The same command meets the licensing requirement that the ground can stop all emissions. The setting is stored in FRAM and its mirror (`radio_state`, decided 2026-10-06), so a reset never turns transmission back on after the ground stopped it. When the radio is selected, the radio board's own transmitter (for example, a built-in beacon) must be silenced by the same state. Deployment (DS-42): nothing transmits in deploy mode, because the radio is not in the deploy frame table; `set_transmit` is only the ground's gate. Transmit at full power only when the antenna is deployed, or when recovering from a failed antenna deployment (decided 2026-10-05; the radio's power control arrives with the radio selection). The antenna will likely also have a hardware safety mechanism.
- Acknowledgement, decided 2026-10-02: commands to a peer ("get status") expect a response, and the subsystem app's state machine retries on timeout. Data frames (uplink, downlink) are not acknowledged on the link; command ACKs are end-to-end already (DS-50), and telemetry is periodic.
- Mirror the peer's state, including how fresh it is. After a link timeout, report "unknown."
- Detect peer resets through a boot counter in the peer's telemetry.
- Payload firmware must answer status requests while a job runs. The fallback is a declared job duration.
- Recovery ladder: retry, then soft reset, then rail cycle through the power app's command channel, then mark failed.

**DS-34 Antenna deployment: Specified.** The entire deployment interface is in avionics, with no radio involvement. (SilverSat 1's radio had access to the secondary burn wire, which was a hardware artifact not to be repeated.) Deployment uses arm-then-fire, with the attempt count persisted in FRAM.

**DS-35 Idempotent commands: Proposed.** Commands set state rather than toggling it. One-shot actions carry their own guards.

**DS-36 Payload radio routing: Open.** Either the payload shares the radio independently, or payload traffic flows through avionics. Addressed packets are adopted now so either works. If independent, avionics retains authority: it controls the payload rail and can inhibit transmission. Radio arbitration is expressed through a payload-active frame table.

## 5. Health, modes, and safe mode

**DS-40 Mode manager: Proposed.**
- It is the sole publisher of `mode_chan`. Requests from other apps arrive on `mode_req_chan`, which uses a message subscriber; the channel is added with the first app that requests (health). The ground sets the mode with `mode_manager set_mode <mode>`.
- Transitions are defined in a const table: from, to, and the reasons (`mode_reason`) allowed to cause it. A guard function column is added with the first guard (for example, battery state from the power app).
- Actions are defined in a second const table, keyed by event: entering a mode, leaving a mode, or a trigger firing. Each action sends an internal command (DS-68). A trigger's actions run whether or not the mode changes, and are retried each major frame until they have all been sent, so each must be safe to repeat (DS-35).
- Routing triggers, decided 2026-10-04: a condition whose response changes the mode or commands other apps goes through the mode manager, so the response is stated once, in its tables. Other apps send a request with a reason; the mode manager's own timers (command loss) are checked there each major frame. An app may still act at once on its own condition when the response stays inside that app (the power app cutting an over-current load, the radio reinitialising its UART), reporting it in housekeeping, and also requesting a mode change if one is needed. An app does not command a different app in response to a fault.
- The frame manager selects its table from `mode_chan`. The mode manager runs in the last slot of each major frame and republishes the mode every major frame.

**DS-41 Safe mode: Proposed.**
- Any source may request entry. Only a ground command exits.
- Safe mode is sticky across resets: the mode manager persists the current mode and reason in a two-slot FRAM record on every transition, and reads it at boot. Built 2026-10-06 (`mode_state`, also in the mirror): after deployment every boot starts in safe mode (DS-42); a spacecraft that was in safe mode keeps its stored reason (for example, command loss), and any other mode boots into safe mode with reason `boot`.
- The safe frame table contains only trusted apps.
- Triggers: low battery (with hysteresis), reset loop, critical app failure, command-loss timer, ground command.
- The beacon carries the mode and the reason for it.

**DS-42 Boot and deployment modes: Specified.** Decided 2026-10-04 and 2026-10-05.
- Every boot starts in `deploy` mode: `mode_chan`'s initial value, so the frame manager runs the deploy table even before the mode manager first publishes. The deploy frame table holds nothing that transmits or takes ground commands (no radio, command ingest, or telemetry output), so nothing can transmit, and no ground command is accepted. No command lists `deploy` in its modes.
- After the separation delay (45 minutes, `CONFIG_SS_SEPARATION_DELAY_MINUTES`, from the launch provider's interface document; timed from MET, DS-45), the mode manager enters safe mode with reason `deployment_complete`, and waits for the ground; only `mode_manager set_mode nominal` leaves safe mode. There is no promotion to nominal.
- For now the delay alone ends deploy mode. When the antenna app exists, it will deploy the antenna within deploy mode after its own wait. Whether a deployed antenna also gates the exit, or the spacecraft moves to safe mode after the delay regardless (giving more chances to recover from a failed deployment), is decided from the antenna mechanism's robustness and its response to retries.
- `test` mode is for ground testing. The mode manager reads an external signal (the `test-mode-signal` devicetree alias; the physical signal is to be determined) once, at its first wakeup after boot; if it is present, it enters test mode with reason `test_signal`. Test mode skips the separation delay, may run self checks (still to come), never deploys the antenna (the antenna app stays out of its frame table), and accepts ground commands; the ground may set safe or nominal from it. Reading the signal only at boot means a glitch in flight can never enter test mode. A board without the alias never enters test mode.
- No transition leads into `deploy`, and the only ones into `test` are for the test signal, which is read once, at boot; so each is used at most once per boot. Deploy mode must be used only once in the mission. Built 2026-10-06: the `deployment` record (FRAM and its mirror) is stored as the delay ends, before the mode changes, and once it is set every boot starts in safe mode instead of deploy. With neither FRAM nor the mirror, every reset re-enters deploy mode and waits out the delay again (late, never early). On a later boot the test signal still selects test mode, from safe mode, so a bench unit keeps it after its first deployment.

**DS-43 Health and watchdog: Proposed.**
- Health checks each app's step counter against the number of wakeups the frame manager delivered. Decided 2026-10-05: at the start of each major frame the frame manager publishes `frame_report_chan`, one bit per app (bit n is app n, so app ids are 1 to 63): `stuck`, set when an app has still not finished the wakeups delivered before the previous report, a whole major frame behind; and `disabled`, the apps it has been told to stop. If an app's status can't be read without waiting at that moment (the app is part-way through publishing it), its bit stays as it was in the last report, so bad timing never resets health's count. The message stays small however many apps there are, and the frame manager only reports what it already counts; the policy stays in health.
- Health runs once a major frame, in slot 0 of every mode's table, at the lowest app priority, so an app that hogs the CPU starves health too. It counts how many reports in a row show each app stuck; when the count reaches the app's `stall_threshold` (its attribute row, DS-07; 3 for the flight apps so far, 0 for apps health can't watch), it responds.
- Graded response: event, then disable the app by commanding the frame manager to stop delivering its wakeups (`frame_manager set_app_enabled <app> false`, also a ground command), then reset if a protected app stalled. Individual threads are not restarted. Until the events channel exists (DS-10), health counts stalls in housekeeping.
- Protected apps are never disabled (the frame manager refuses); a stall goes straight to reset. The protected apps are health, mode manager, command ingest, radio, and telemetry output. Rule, decided 2026-10-05: an app on the path a ground command takes (the radio, command ingest) is protected, because once stopped the ground could never send the command to start it again; a reset brings it back.
- Reset, decided 2026-10-05: health stops feeding the watchdog, and the watchdog resets the spacecraft. That is the same path as health or the frame manager stalling, so one reset path serves every fault, and it can be tested on native_sim.
- Re-enable policy is per app, from the app's attribute row (DS-07): `NEVER`, `GROUND` (ground command only), or `AUTO` (after a cooldown, re-enable and watch; after a set number of automatic re-enables, fall back to `GROUND` with an event). Still to come, with the safe-mode request for a critical app (DS-41, through `mode_req_chan`). Meanwhile, the ground restarts a stopped app with `set_app_enabled <app> true`, and health gives it its whole threshold again.
- Health is the only feeder of the hardware watchdog (IWDG, the `watchdog0` alias), which forms the chain frame manager, then health, then IWDG. The timeout is 3 s (`CONFIG_SS_WATCHDOG_TIMEOUT_MS`, decided 2026-10-05), so one late feed is tolerated. A board without a watchdog still has health watching the apps.
- Use `WDT_OPT_PAUSE_HALTED_BY_DBG` on the flatsat.

**DS-44 Boot handling: Proposed.**
- Record the reset cause (`hwinfo`).
- Keep a FRAM boot log, and boot into the safe table after repeated short runs.
- Record which app triggered a health-initiated reset.
- Report each app's stack high-water mark in housekeeping.

**DS-45 Launch timers: Specified.** Time since deployment is kept in FRAM, so a reset neither restarts the post-ejection waits nor skips them. Values come from the launch provider's interface document.

**DS-46 Command-loss timer: Proposed.** Measures time since the last *accepted* ground command, not the health of the command ingest app (health covers that through its step counter, DS-43). Command ingest stores the MET of acceptance with the counter floor and publishes it on `ground_contact_chan` (a data channel, since the mode manager may not include command ingest's header, DS-68). Every command that passes the counter check counts, whatever it does; replays and forgeries do not. The timer starts at first contact, so the spacecraft keeps transmitting until the ground has found it. The mode manager compares it against the timeout (7 days, from Kconfig) and, when it expires, requests safe mode and sends `radio set_transmit false` (DS-33). Transmission stays off until a ground command turns it on; an accepted command restarts the timer but does not turn transmission on. Because the value is in FRAM, resets do not restart the timer. Built so far (2026-10-06): whether contact has happened is stored, so the timer keeps running after a reset, but it restarts from the boot, because MET does not yet survive a reset (DS-25); it becomes exact when health persists mission time.

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

Command text, decided 2026-10-02:
- The text is `<app> <command> <arguments>`, using the YAML names, for example `frame_manager set_entry_enabled nominal 3 false`.
- Words are separated by exactly one space, with none at either end. Anything else is rejected rather than tidied, so a command means exactly what was signed.
- Integers are decimal, with no leading zeros and `-` only for negatives (never `-0`), and are range-checked against their type. Bools are `true` or `false`. Enums are the value's name.
- Float arguments are not supported yet; they arrive with the first command that needs one.
- Every command lists the modes it is allowed in (`modes:` in the YAML). There is no default.
- Decoding and routing are generated from the YAML (DS-60). The ground formats and checks text from the same definitions, and a shared set of vectors keeps the two in step.

Replies and scheduling, decided 2026-10-02:
- Command ingest is woken by the frame manager every minor frame and empties the uplink queue, so a command waits at most one minor frame.
- Frames that fail the shape or signature check get no reply, only a counter: there is nothing to tell a forger, and noise needs no answer.
- Every authenticated command gets exactly one reply, sent after routing, as text in the downlink queue: `ACK <counter> <result>` (for example `ACK 0000018f2c4d5e6f ok`, or `... bad_arg 2`), or `NAK <counter> replay|jump|store` if the counter check or floor store fails. This folds stage 5's ACK into the routing result. The floor is still stored before anything is sent, and the counter is used up whatever the routing result.
- An app that already has `CMD_MAX_PENDING` ground commands it hasn't handled gets `busy` instead of another one, so the zbus pool stays within its budget (DS-07).

**DS-51 On-orbit software update: Specified (plan).**
- Plan for MCUboot with signed images and revert-on-failure.
- The flatsat runs in two modes: development mode (direct flash plus GDB) and update mode (signed image uplinked through the flight path).
- **Open:** the F446 sector layout limits the image to about 128 KB with swap. Options are a secondary slot in external SPI flash, or weighing update support in the flight MCU choice. Measure the image size early.

**DS-52 Signing: Specified.** Keyed BLAKE2s, continuing the SilverSat 1 approach:
- The shared secret is the BLAKE2s key: BLAKE2's built-in MAC mode, giving a 32-byte tag (in Python, `hashlib.blake2s(data, key=secret)`). SilverSat 1 flew HMAC-BLAKE2s instead; SilverSat 2 uses the keyed mode, which takes one pass and is what BLAKE2 was designed for.
- Commands are signed, not encrypted.
- Commands are printable ASCII. Binary fields travel as lowercase hex.
- Wire format, in SilverSat 1's order: `<tag: 64 hex><salt: 16 hex><counter: 16 hex><command text>`. The salt is 8 random bytes per command. The counter is DS-53's 64-bit counter. The command text is at most 159 characters, so a whole packet stays under 256 bytes (DS-66).
- The tag covers everything after it, exactly as received: the salt, counter, and command text as ASCII characters (DS-50).
- On the MCU, use the BLAKE2 reference implementation (CC0), vendored into this repository, since Zephyr's crypto stack doesn't include BLAKE2s. (Monocypher implements only BLAKE2b.)

**DS-53 Replay protection: Specified / Proposed.**
- *Specified:* a 64-bit monotonic counter, accepted if strictly greater than the floor, with gaps allowed. After a loss, the ground jumping ahead re-establishes the floor, as in SilverSat 1. The ACK is sent on acceptance, with no retry mechanism.
- *Proposed:* the floor is persisted in FRAM before execution. The counter is epoch milliseconds, and the ground sends the larger of the current time and the last value plus one. Command ingest stores the floor through a small floor-store interface, which since 2026-10-06 writes it to FRAM and its mirror (`command_state`) before the ACK. If the write reaches neither, the floor is kept in RAM for the rest of the boot and the failure counted, so replays stay refused and commanding goes on (DS-75); refusing the command instead would leave a spacecraft with failed storage unable to be commanded.
- *Proposed:* the counter never wraps. Comparison is plain unsigned 64-bit (not the serial-number arithmetic used for FRAM generations), and code must never compute `floor + 1` where it could overflow; a unit test covers the maximum value. A command carrying the maximum value is accepted once, if it is within the maximum jump of the floor, and sets the floor to the maximum, after which all commands are rejected. Wrapping to zero would make every previously recorded command valid again.
- *Proposed:* guard against an erroneous large counter in two places. The ground software refuses to send a counter more than one day ahead of its clock. The spacecraft rejects any counter more than 10 years above the floor. That catches a counter sent in the wrong units (microseconds are thousands of times too large) while still allowing the SilverSat 1 style jump-ahead recovery, even after years without a command.
- *Proposed:* the floor's default, used when nothing is stored, is the mission epoch, 2026-01-01T00:00:00Z in epoch milliseconds, not zero. From zero, today's epoch-millisecond counter would be more than 50 years above the floor, and the jump guard would reject every command.
- *Proposed:* the floor is stored per key slot, and no floor is ever reset, by rotation or anything else (decided 2026-10-02). Rotating to the other key (DS-54) uses that slot's own floor, so it recovers from a floor set too high on the active slot. Rotating back later cannot make the old slot's recorded commands valid again.

**DS-54 Keys: Specified / Proposed.**
- *Specified:* the flight key is kept outside Git. This is not a high-security environment, and the realistic risk is accidental commanding, not a sophisticated adversary.
- *Specified:* separate test and flight keys, so flatsat and Codespace setups cannot command the flight vehicle.
- *Proposed:* flight builds get the key from a CI secret. Each key slot's key comes from a file named in Kconfig (`CONFIG_SS_CMD_KEY_FILE_SLOT0` and `_SLOT1`), defaulting to the two published test keys in `tools/`. The build turns them into a header in the build directory. A flight build (`CONFIG_SS_FLIGHT_BUILD`) points them at files written from CI secrets, and the build fails if either is a published test key.
- *Proposed:* two key slots allow rotation as people leave, and provide recovery from a maxed counter floor (DS-53). Rotation is signed by the other slot and uses arm-then-fire.
- *Proposed:* keys are compiled into flash; FRAM holds only which slot is active and the per-slot floors, so a FRAM failure cannot lose the keys (DS-75).
- *Proposed:* rotation switches between the two compiled-in keys; it cannot load a new one. Commands are signed, not encrypted (DS-52), so a key sent in a command could be read by anyone listening. Installing new keys takes a new flight image (DS-51). Two slots therefore give one spare key, for a key that has left with someone or for a maxed-out floor.
- *Proposed:* rotation, decided 2026-10-02. Two commands, `command_ingest arm_key_rotation <slot>` then `command_ingest rotate_key <slot>`, both signed with the key of the slot being switched to and checked against that slot's floor, so rotation works even when the active slot's floor is maxed out. The slot named must be the slot that signed. An arm lasts 10 minutes of MET. Command ingest handles both commands itself, since only it knows which key signed a command.
- *Proposed:* the spare slot's key is accepted only for those two commands. Anything else it signs is refused with `NAK <counter> wrong_key` before its counter is used. A rotation command signed with the active key gets `ACK <counter> wrong_key`, and a fire with no live arm gets `ACK <counter> not_armed`.
- *Proposed:* the active slot is stored in FRAM and its mirror (`command_state`, 2026-10-06), so a rotation survives a reset; with neither, a reboot returns to slot 0 (DS-75). An arm is held in RAM: a reset clears it, and the ground arms again.
- *Proposed:* tags are compared in constant time, mainly as a lesson.

## 7. Messages and links

**DS-60 Single source of truth: Specified.** All commands, telemetry, events, and channel payloads are defined in YAML. Changes must propagate easily to avionics, simulators, flatsat, ground station, and other teams' subsystems.

**DS-61 Generated outputs: Specified (requirement) / Proposed (design).** A Python generator using Jinja2 produces:
- C headers plus encode, decode, and validate functions
- Python classes plus text formatting and parsing
- a resolved JSON dictionary for other languages
- a generated interface control document
- golden vectors, checked by both the C and Python test suites
- housekeeping encoders for telemetry output, and each app's housekeeping wire layout (field, type, offset, size, enum values) in the JSON dictionary for the ground decoder (`tools/telemetry.py`). Fields are written one by one, little-endian, with no padding (DS-64); each app's housekeeping must fit in one frame

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

- Frame layout on every avionics serial link, decided 2026-10-02: `type` (1 byte, the KISS type byte), `seq` (1 byte, the sender's sequence number, wrapping after 255), `len` (1 byte, the payload length), the payload (0 to 255 bytes), then the CRC. The whole frame is KISS-escaped and wrapped in FEND bytes. The codec is `lib/link_codec` in C and `tools/link_codec.py` in Python (DS-91), kept in step by shared vectors (DS-92).
- The CRC is transmitted as 4 bytes, little-endian (DS-64).
- It is computed over the frame's sequence number, length, KISS command/type byte, and payload, before KISS escaping, and checked after unescaping.
- The fallback, the common IEEE CRC-32 used by Ethernet and zlib, is used only if a subsystem cannot support CRC-32C. It is chosen per link and recorded in that link's interface document; there is no negotiation on the wire.
- The golden vectors (DS-61) include CRC test frames for every link.

**DS-66 KISS framing: Specified / Open.** The radio is not yet selected (2026-10-02), so the link codec is built generically. Ground traffic uses the standard data frame, `0x00`, in both directions (decided 2026-10-04); no other type byte is assigned yet.
- The first payload byte of every packet avionics sends to the ground is a printable letter giving its kind (decided 2026-10-04): `A` and `N` for command replies (`ACK`, `NAK`, DS-50) and `H` for housekeeping. A housekeeping packet is `H`, the app id (one byte), MET in milliseconds (8 bytes, little-endian), then the app's housekeeping in its generated wire layout (DS-61).
- *Specified:* keep KISS on all serial links.
- *Specified:* support the standard data frame (0x00) if required.
- *Specified:* command packets are under 256 bytes.
- *Open:* printable KISS command bytes. The SilverSat 1 lessons learned recommended printable values as KISS command bytes, because non-printable values required extra code for logging and debugging. Leaving this open reverses that recommendation pending the radio decision. The trade-off: in standard KISS, the type byte's high nibble is the port and the low nibble the command, so a printable byte such as 0x41 ('A') reads as "port 4, command 1" (TX delay). With off-the-shelf radios or any stock KISS device in the path, printable type bytes may be acted on as radio parameters. The alternative keeps readability everywhere but one fixed byte: use SetHardware (0x06) with a printable subcommand as the first payload byte. Printable type bytes are safe only if we control the firmware at both ends of every link. Decide when the radio is selected.

**DS-68 Per-app message ownership: Proposed.** Every message in the YAML names an owning app (`owner:`, plus `dest:` for commands).
- The generator emits one header per app containing only that app's messages, plus a shared header for common types (tick, status, events).
- The generator also emits the channel definitions (in the owner's module) and command ingest's routing table, so ownership is stated once.
- CI lint: an app includes only its own generated header and the shared one, and only the owner publishes its telemetry and housekeeping channels. Publishing to another app's *command* channel remains allowed; that is how internal commands work.
- Internal commands, decided 2026-10-04: an app lists the commands it sends under `sends:` in its YAML (`radio.set_transmit`). The generator writes `send_<app>_<command>()` into the sender's own header, so the sender never includes the target's header, a misspelled command fails the build, and the dictionary shows who commands whom. Each sender keeps at most `CMD_MAX_PENDING` unhandled commands to each target (`-EBUSY` otherwise), the same bound command routing uses for ground commands (DS-07).

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
- Built 2026-10-05 as `lib/nvm` (`silversat/nvm.h`). The FRAM is the devicetree alias `fram`, on SPI once a part is chosen; Zephyr has drivers for the likely families (Infineon FM25, Fujitsu MB85RS). Until then the Nucleo build has no `fram` alias and runs with FRAM unavailable (DS-75); native_sim uses the board's simulated EEPROM, kept in a file between runs. The map assumes 32 KB (256 Kbit). One mutex covers every operation. The service is ready before any app's thread starts. The nvm app is still to come.

**DS-71 FRAM layout and atomicity: Proposed.**
- A fixed region map, with no filesystem.
- Two-slot records with magic, version, generation, and CRC; the valid slot with the newest generation wins, using serial-number arithmetic. Each slot is magic (2 bytes, 0x5353), version (1), payload length (1), generation (4), the payload, and a CRC-32C (4) over everything before it, little-endian. A write goes to the slot not holding the newest record, with the next generation, and is read back before it counts as stored. A record of another version reads as no record (its default), never as a fault, so a new software version starts from defaults for any record it changes.
- The boot log is a headless ring: each entry is written once, placed at `boot_num % N`, and carries the previous run's duration from the checkpoint record.
- Hardware write-protect on the configuration region. Keys are not stored in FRAM; they are compiled into flash (DS-54).

**DS-72 Time: Specified.**
- Record RTC time (via Zephyr's RTC API), most recent elapsed boot time, and total mission time, which is persisted in FRAM.
- The RTC is reset from the ground periodically rather than calibrated.
- UTC drift is monitored manually on the ground. This is mission dependent.
- *Proposed:* log every time set as an event with old and new values.

**DS-73 Telemetry budget and store-and-forward: Open.** To be determined after mission selection. It may require external flash and a data storage app.
- *Interim (2026-10-04):* telemetry output sends one app's housekeeping per major frame, taking the apps in turn, so each app's housekeeping goes down every N seconds with N apps.

**DS-74 FRAM ownership: Proposed.** FRAM records are data sharing, and follow the same rule as channels (DS-12): each region has exactly one owning app, which alone writes it. Other apps learn the contents through the owner's channels, never by reading the record.
- The region map is controlled from a single file, `nvm_map.yaml` (in `messages/`), maintained alongside the message definitions and processed by the same tooling (DS-61). For each region it gives the name, owner, record size and version, slot count, write-protect flag, and flash default (DS-75). The generator computes addresses and emits `nvm_map.h` (addresses, sizes, owners, and per-owner handle declarations), the ground dump decoder, and a table in the interface document. CI fails on overlap, on exceeding the FRAM size, on a changed record without a version bump, and on write-protected regions outside the protected block range. Nobody assigns addresses by hand. Built 2026-10-05: the generator lays regions out in file order on 16-byte boundaries, fails on a map larger than `fram_size`, and writes `nvm/<owner>.h` for each app: each record's struct, little-endian encode and decode, its encoded default, the `static` region handle, and typed read and write functions. The map is also in the JSON dictionary for the ground's dump decoder. Still to come: the version-bump check, write protection, the dump decoder, and the interface-document table.
- Region handles are defined `static` in the owner's source file, so no other app can name them; development builds add a runtime check of the calling thread against the owner in the region map.
- Initial map:

| Region | Owner |
|---|---|
| Boot log, run checkpoint, mission time | Health |
| Command counter floor per key slot, active key slot, last-accepted MET | Command ingest |
| Current mode and reason | Mode manager |
| Launch/deployment timers | Mode manager |
| Transmit setting (`set_transmit`) | Radio |
| Antenna deploy attempts and arm state | Antenna app |
| Persistent subsystem state (for example, payload job ID) | The owning subsystem app |
| Calibration tables | The owning device app, loaded by command |

Keys are not in FRAM: they are compiled into flash (DS-54).

- Peer boards (payload, power, antenna, radio) never access avionics FRAM; their persistent state lives on their own hardware, and bulk data goes to store-and-forward (DS-73), not FRAM.
- The ground reads FRAM through housekeeping and a read-only memory-dump command against the fixed map. Ground writes go through the owning app's commands (for example, calibration load), never raw writes.

**DS-75 Operation without FRAM: Proposed.** FRAM is valuable but unproven in flight, so the spacecraft must keep operating if it fails, accepting lower security and lost information.
- Every record has a const default in flash, generated from `nvm_map.yaml`. A record that cannot be read falls back to its default, never to garbage.
- The F446's 4 KB backup SRAM and backup registers (VBAT domain) form a second tier. They survive resets and, if VBAT is powered, main power loss. Critical small records are mirrored there: counter floors, mode, boot counter, launch timers, deploy attempts. Built 2026-10-06: a record marked `mirror: true` in `nvm_map.yaml` is also kept in the mirror (the `nvm-mirror` alias: on the Nucleo, a `zephyr,retained-ram` device on the backup SRAM) in the same two-slot form. Both copies of a write carry the same generation, a read takes the newest valid record from either, and the mirror degrades separately from FRAM. The mirror's 4 KB is laid out and checked by the generator (`mirror_size`). Mirrored so far: command state, mode, deployment, and the transmit setting.
- The FRAM service detects failure from the device ID at boot, SPI errors, and both slots failing CRC. It then marks FRAM degraded, stops using it, raises an event, and sets a beacon flag. The ground can command a retry. Built 2026-10-05: a device that isn't ready at boot (the driver checks the ID), a device error, a write that doesn't read back, or both slots of a record failing CRC marks FRAM degraded; one bad slot (a torn write) and a blank part do not. `nvm_retry()` tries the device again. The event, the beacon flag and the retry command come with the nvm app.
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
- The first is `sim/radio_sim.py`. Its fault menu drops, corrupts, repeats, splits, or renumbers a frame, sends noise first, or uses an unhandled type byte. `tests/app/radio` runs the flight apps against it in real time (DS-92).

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
| DS-51 | Image size versus F446 flash layout for updates |
| DS-63 | Message definitions repository timing |
| DS-66 | Printable KISS command bytes versus off-the-shelf radio compatibility |
| DS-73 | Telemetry budget and store-and-forward |
| DS-69 | Beacon size, interval, and Morse subset, after radio and mission selection |
| — | Mission objectives and form factor (1U expected) |
| — | Minor frame rate once ADCS requirements are known |
| — | Pin mux check for five UARTs on the Nucleo-F446RE. So far: radio on USART1 at PA9/PA10, because the default PB6 is reserved for the magnetometer's chip select |
| — | Driver and emulator support for the selected IMU and FRAM parts in the pinned Zephyr version |
| DS-10 | The events channel: owner, how its queued copies are bounded, and its effect on the zbus pool. Until then, apps count rejections in housekeeping |
| DS-07 | A separate zbus pool for command channels (`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_ISOLATION`), so telemetry can never starve commands |
| DS-68 | Data channel types that only some apps read. For now a data channel's type must be in `common.yaml` |
| DS-25 | MET at boot: health publishes mission time from FRAM. Until then each boot's MET starts at zero |
| DS-26 | Measure idle time for the frame manager's housekeeping |

---

## Revision history

| Date | Change |
|---|---|
| 2026-09-30 | Initial draft from architecture discussions |
| 2026-09-30 | Renamed to specifications (DS-nn, "Specified"); Renode optional; generic remotely managed USB switch; added DS-24 priorities, DS-25 tick time and MET, DS-46 command-loss timer, DS-68 message ownership, DS-74 FRAM ownership; safe-mode persistence in DS-41 |
| 2026-09-30 | Added DS-26 low power, DS-75 operation without FRAM, single-point FRAM region map (DS-74), counter bounds and per-key floors (DS-53); test/flight key separation Specified; CRC parameters and IEEE fallback in DS-65; removed DS-67 (belongs to the radio specification) |
| 2026-09-30 | DS-66 printable KISS command bytes marked Open, with reversal of the SilverSat 1 lesson and trade-off noted; FRAM bus reversal stated in DS-75 |
| 2026-09-30 | Added DS-69 beacon contents |
| 2026-09-30 | Moved to `docs/`. DS-07 gains per-app attribute rows and pool sizing. DS-22 overrun detection by per-wakeup counting with a pending bound, because message subscribers give no queue-full signal and pool exhaustion faults. DS-23 protected entries. DS-25 64-bit uptime. DS-43 disable via the frame manager, protected apps, per-app re-enable policy |
| 2026-10-01 | DS-07 pool size adds one buffer per publishing thread and is enforced by a build check against Kconfig; buffers sized for the largest message on any channel. DS-22 corrected: a failed delivery copy returns an error; a publish that cannot get its own buffer faults. Both verified by `tests/unit/zbus_pool` |
| 2026-10-01 | DS-22: delivered and overrun counts are per app, not per entry; a frame manager that falls behind skips and counts the missed minor frames. Frame manager implemented in `apps/frame_manager` |
| 2026-10-01 | DS-11 adds the small-message rule: raw link frames use a static `k_msgq`, not zbus. DS-52 states the keyed-BLAKE2s MAC (SilverSat 1 flew HMAC-BLAKE2s), the wire format, and the vendored reference implementation (Monocypher has no BLAKE2s). DS-53 floor store with a RAM stand-in until the FRAM service. Open items gain the decisions deferred while building the frame manager |
| 2026-10-01 | DS-53: the floor defaults to the mission epoch (2026-01-01) rather than zero, and the spacecraft's maximum jump is 10 years, because a floor of zero with a one-year guard would reject every command (first command, after a long silence, after key rotation). The counter-at-maximum rule applies within the jump |
| 2026-10-01 | DS-54: rotation switches between the two compiled-in keys and cannot load new ones (one spare key); a rotation command signed by the other slot is checked against that slot's floor. DS-71 and DS-74 no longer place keys in FRAM, matching DS-54 and DS-75 |
| 2026-10-02 | DS-50: command text syntax (`<app> <command> <arguments>`, single spaces, words for bool and enum), required `modes:` per command, floats deferred; decoding and routing generated from the YAML |
| 2026-10-02 | DS-50: command ingest is woken every minor frame; one reply per authenticated command after routing (`ACK <counter> <result>` or `NAK <counter> <reason>`), none for shape or signature failures; `busy` when an app has `CMD_MAX_PENDING` unhandled ground commands. DS-54: keys from Kconfig key files, and a flight build refuses the published test keys. Open item for key rotation and floor reset |
| 2026-10-02 | DS-53: no floor is ever reset, including by rotation (the floor default no longer mentions rotation). DS-54: rotation commands, signing by the new slot, the 10-minute arm, replies for the spare key, and the active slot in RAM until FRAM. Rotation open item closed |
| 2026-10-02 | DS-65: frame layout (type, seq, len, payload, CRC) on every avionics serial link, implemented in `lib/link_codec` and `tools/link_codec.py`. DS-33: commands to a peer expect a response and are retried by the app; data frames are not acknowledged on the link. DS-66: radio not yet selected; no type byte values assigned |
| 2026-10-04 | DS-07: UART assignments are devicetree aliases named in the resource map. DS-33: the radio app (every minor frame, interrupt-driven ring buffers). DS-66: ground traffic uses data frame 0x00 both ways. DS-90: the radio simulator and its fault menu. Open items: the radio's Nucleo pins |
| 2026-10-04 | Telemetry output. DS-07: the pool no longer budgets housekeeping requests, which are dropped. DS-14: every app publishes housekeeping at least once per major frame. DS-61: generated housekeeping encoders and wire layouts in the dictionary. DS-66: downlink packet kind letters (`A`, `N`, `H`) and the housekeeping packet layout. DS-73: interim rate of one app per major frame |
| 2026-10-04 | DS-14 confirmed and marked Specified |
| 2026-10-04 | DS-33: radio transmit inhibit (`set_transmit`), silencing replies, telemetry, and the beacon; only a ground command turns it back on. DS-46: the command-loss timeout is 7 days and also stops transmission |
| 2026-10-04 | Mode manager. DS-40: transition and action tables, trigger routing, `set_mode`; `mode_req_chan` deferred to the first requester. DS-41: mode in RAM until FRAM. DS-42 decided (boot waits in safe mode) and marked Specified. DS-46: `ground_contact_chan`; the timer starts at first contact. DS-68: internal commands through generated senders (`sends:`). DS-07: the pool budgets `CMD_MAX_PENDING` per sending pair. DS-33: post-deployment transmit wait noted as still to come |
| 2026-10-05 | DS-42 rewritten: boot and deployment modes. Every boot starts in `deploy` (`mode_chan`'s initial value; nothing transmits, no ground commands), which ends after a 45-minute separation delay in safe mode; `test` mode from an external signal read once at boot skips the delay and accepts ground commands; neither can be re-entered; deploy only once per mission when FRAM exists. Antenna gating of the deploy exit still to decide. DS-33: full-power transmit only with the antenna deployed or in antenna-failure recovery |
| 2026-10-05 | DS-43: health, first part. The frame manager's `frame_report_chan` (stuck and disabled masks, one bit per app; app ids 1 to 63) closes the open item on how health learns each app's progress. Stall thresholds in major frames; stalled apps stopped with `frame_manager set_app_enabled`; a protected app's stall stops the watchdog feed; 3 s watchdog timeout. Open items: DS-42 row removed (Specified since 2026-10-04) |
| 2026-10-05 | DS-43: the radio is protected, and the rule: an app on the ground command path is protected, since a stopped one could never be restarted from the ground |
| 2026-10-05 | DS-70, DS-71, DS-74, DS-75: the FRAM service and region map built (no app uses them yet). SPI assumed, part not chosen, 32 KB map; the Nucleo runs with FRAM unavailable. Slot layout, read-back on write, a record of another version reads as its default, and what marks FRAM degraded |
| 2026-10-06 | FRAM, second part. DS-75: the mirror built (records marked `mirror:` also kept in backup SRAM through the retained-memory API; one generation across both copies; the newest wins), so the Nucleo keeps mirrored records across a reset without FRAM. DS-33, DS-41, DS-42, DS-46, DS-53, DS-54, DS-74: the transmit setting, mode and reason, deployment, contact, counter floors and active key slot persist; a store failure never refuses a command; the test signal reaches test mode from safe mode on a later boot. DS-46's timer restarts at boot until MET persists |
| 2026-10-06 | DS-05: the native_sim tests run as four CI jobs in parallel (each platform in two halves), with a `native_sim tests` job that passes when all do; the generated-message tests join the shared library build |
