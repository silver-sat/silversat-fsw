# SilverSat 2 Flight Software — Design Rationale and Patterns

Companion to `silversat2-fsw-design-decisions.md`. The specification says *what*; this document records *why*, what was considered and rejected, and the code patterns every app should follow. When the two disagree, the specification wins and this document should be corrected.

---

## 1. Why this architecture

### Starting point: SilverSat 1

SilverSat 1 ran an Arduino loop on a SAMD21: each C++ class was "touched" every pass and either did work or returned. The loop order determined priority. It worked, but:

- A slow class stretched everyone's timing, usually unnoticed.
- There was no way to phase work deliberately or to disable work for recovery or power.
- The processor ran continuously.
- The loop design was stretched to its limit by the command, radio, and board interfaces.

### Why a cFS-style design on zbus, not cFS itself

NASA's cFS is the reference architecture for small spacecraft (software bus, apps with housekeeping, events, command tables). Running cFS itself is not realistic on a Cortex-M with a few hundred KB of RAM; its OS abstraction targets Linux, VxWorks, and RTEMS, and the Zephyr port is not mature enough to depend on. We take the vocabulary and structure and map it onto Zephyr:

| cFS | Here |
|---|---|
| Software Bus, Message IDs | zbus channels |
| App main loop pending on a pipe | One thread pending on one zbus subscriber |
| Scheduler (SCH) | Frame manager |
| Executive Services | Kernel init plus the health app |
| Event Services | Events channel |
| Table Services | Const tables in flash, validated copies in FRAM |
| CI / TO | Command ingest / telemetry output |

### Rejected alternatives

- **F Prime** (JPL), including fprime-zephyr and the PROVES Kit reference. Closest existing match to this design, with flight heritage and a ground system. Rejected because it adds C++ and its own modeling language on top of an already deep toolchain for students working a few hours a week, uses point-to-point ports rather than pub/sub, and has its own framing. Status: Monitor.
- **cFS on Zephyr, NOS3, cFS Basecamp.** See above. Basecamp remains a possible teaching aid on the ground side. Status: Monitor.
- **YAMCS** for mission control. We have an existing ground station (Flask, SQLite, browser UI) that mentors and returning students know. Status: Monitor.
- **CCSDS packets and frames.** Professional standard, but big-endian headers and a poor fit with KISS, text commands, and the amateur ecosystem. A deliberate divergence.
- **Protocol Buffers / Kaitai Struct** for message definitions. Protobuf's variable-length encoding suits neither fixed telemetry frames nor text commands; Kaitai is effectively decode-only. A small custom generator is the right size.

---

## 2. zbus semantics: the key difference from cFS

The cFS software bus copies every message into each subscriber's pipe. A plain zbus channel is a **single shared buffer holding the latest value**; publishing overwrites it. zbus has three observer kinds:

- **Listeners**: callback runs synchronously in the publisher's context. Fast, but must not block. Avoid in student code.
- **Subscribers**: notified the channel changed, then read the current value. A slow subscriber sees the newest value and misses intermediate ones.
- **Message subscribers**: get their own copy of each message from a `net_buf` pool. Closest to cFS pipes.

Rule: **commands** use message subscribers because a dropped command is a bug; **telemetry and sensor data** use last-value semantics because the newest value is what consumers want.

### What an app actually receives

The channel itself is always last-value: `zbus_chan_read` returns the newest publish. What an *app* receives depends on its observer type, and each app has exactly one pending point, so it has exactly one observer type:

- With a plain subscriber, two commands published back to back would collapse into one read of the newest value. A command would be lost.
- So every app uses a message subscriber, and **every** channel it observes, including its frame wakeup, arrives as a queued copy. Nothing is overwritten.

Three consequences, checked against the Zephyr v4.4.0 source (`subsys/zbus/zbus.c`) and pinned down by `tests/unit/zbus_pool`:

1. Every publish on every channel, observed or not, takes one buffer from a pool that all channels share, and holds it until the publish returns. Each message subscriber's copy takes one more buffer, held until the app reads it.
2. A message subscriber's queue is an unbounded `k_fifo`. A stuck app gives the publisher no error while the pool lasts; its wakeups just pile up, each holding a buffer.
3. When the pool runs out, the result depends on which buffer could not be allocated. If it is a subscriber's copy, `zbus_chan_pub` returns `-ENOMEM`. If it is the publish's own buffer, zbus asserts: a development build panics, and a flight build dereferences NULL. That happens when another publish is in flight at the same moment, for example a low-priority app preempted part-way through publishing its status. So one stuck app could crash the spacecraft through some other app's publish.

The `-ENOMEM` is not a usable queue-full signal: by the time it appears, the shared pool is nearly empty for everyone. The fix is to never publish a copy the app can't consume. The frame manager counts wakeups delivered to each app and compares them with the app's step counter; once `FRAME_MAX_PENDING` are outstanding it skips the publish and counts an overrun (DS-22). With every app bounded, the pool size is simple arithmetic in the resource map (DS-07): every copy that can be queued, plus one buffer for each thread that publishes.

Channel validators run at publish time in the publisher's context. That is how parameter validation reaches command ingest: the target app owns the rules, and command ingest learns immediately if the publish was rejected.

---

## 3. Frame manager

### Why phasing

A table of slots within a fixed major frame makes data flow through each second in a known order: sensors read in slot 0, so ADCS in slot 1 always has fresh data; telemetry output encodes housekeeping in slot 5 and the radio ships it at the next minor frame. Independent timers per app drift relative to each other in ways that are hard to predict.

### Why a thread, not the timer ISR

zbus permits publishing from an ISR, but any listener would then run in interrupt context, and eventually someone puts a blocking call in one. A `k_timer` wakes a high-priority thread, and the thread publishes.

### Why `K_NO_WAIT`

The frame manager must never stall. If an app still has `FRAME_MAX_PENDING` wakeups it hasn't stepped through, the app is overrunning: skip its wakeup, count it, move on. Health decides whether a repeated overrun means the app is stuck (DS-43). The same reasoning applies to command ingest: one wedged app must not block the command that would fix it.

### Contrast with the Arduino loop

The loop gave mutual exclusion for free, since one class ran at a time. Preemptive threads remove that guarantee. Sharing data only through channels (zbus locks the channel during publish and read) restores most of it.

### Thread priorities: rate-monotonic

Shorter period, higher priority (Liu and Layland, 1973): this is the optimal fixed-priority assignment for periodic tasks. For many tasks, total CPU utilization under about 69 percent guarantees all deadlines are met. In Zephyr, a lower number means a higher priority.

### Low power

The CPU enters a low-power state only when every thread is blocked, so an app still working simply keeps the CPU awake; nothing is interrupted. Use Sleep (WFI) nominally. Stop mode halts the F446's high-speed clocks and ordinary UART reception, so it is only for quiet link conditions, with apps holding a power-state lock during transactions.

---

## 4. Health, watchdog, and modes

### Step counters, not pings

A running thread stuck in an infinite loop inside its step function still exists; its step counter stops advancing. Health compares each app's steps against the wakeups the frame manager delivered, so a deliberately disabled app is never flagged.

### Why no individual thread restarts

In a single address space, a thread killed mid-step may hold a zbus channel lock or leave a driver half-configured. Disabling the app is safe; a full reset is honest. Anything in between is where subtle bugs live.

### Why the plain watchdog, not Zephyr's task watchdog

The task watchdog (`CONFIG_TASK_WDT`) resets on any missed per-thread feed, which removes the graded middle steps (event, disable app). With health as the only feeder, and health running in a frame slot, the chain frame manager → health → IWDG covers every failure: if either stops, the watchdog isn't fed.

Set `WDT_OPT_PAUSE_HALTED_BY_DBG` on the flatsat, or the board resets under a student sitting at a breakpoint.

### Payload app vs. payload health

The payload app never stops stepping, even during a long job; waiting is a state. Health watches the *app* (step counter). The payload app watches the *payload* (status polls, deadlines). Keeping these separate stops a flaky payload from resetting the spacecraft.

### Safe mode

Anything can enter safe mode; only the ground exits it. Autonomy's job is to reach a stable state and wait, because the triggering fault may still be present. Safe mode is sticky across resets so a reset doesn't erase the ground's decision to hold.

---

## 5. Commanding and security

### Threat model

Not a high-security environment; students and mentors come and go. The realistic risk is **accidental commanding** (a test script or student ground setup pointed at the real satellite), not a sophisticated adversary. Separate test and flight keys address the realistic risk better than any cryptographic subtlety.

### Pipeline order is the security design

Signature before counter check: a forged packet must never be able to raise the floor and lock out the real ground station. Decode after authentication: the text parser, the most bug-prone code in the path, is unreachable without the key. Persist the floor before sending the ACK: otherwise a reset between them reopens the replay window.

### Counter behavior

- Accept if strictly greater than the floor; gaps allowed. The ground jumping ahead re-establishes the floor after a loss (SilverSat 1 practice).
- Epoch milliseconds as the counter: monotonic without the ground remembering anything; ground sends `max(now_ms, last + 1)` to survive clock steps.
- **Never wrap.** Wrapping to zero would make every recorded command valid again. Plain unsigned 64-bit comparison; do *not* reuse the serial-number comparison from FRAM generations, which would treat a huge jump as going backward.
- Bounded jump on the spacecraft and a margin check on the ground protect against an erroneous huge value. Recovery if the floor is ever set too high: rotate keys (floors are per key slot).

### Lost ACKs and retries

If a command executes but its ACK is lost, the operator resends with a new counter and it runs twice. The fix is in command design, not protocol: commands set state rather than toggle it, and one-shot actions carry their own guards.

### Signing details

Keyed BLAKE2s: the secret is BLAKE2s's key, so the MAC takes one pass. It is not a hash of secret-plus-message, which is unsafe for many hashes, and not HMAC, which SilverSat 1 flew and which BLAKE2 doesn't need. Tags travel as hex so commands stay printable. The tag covers the received characters exactly, so the satellite never re-encodes anything before checking it. Compare tags in constant time; not a realistic threat here, but a free habit and a good lesson.

---

## 6. Messages and links

### Why one source of truth

Messages are consumed by C flight code, Python simulators, the ground station, and possibly other teams in other languages. Hand-maintained copies drift. The generator produces all of them, plus golden vectors both C and Python must decode identically.

### Why never copy structs onto the wire

Padding and field sizes differ between the Cortex-M4, 32-bit native_sim, and 64-bit native_sim.

### Why telemetry output reads the latest housekeeping instead of requesting it

The cFS pattern is a housekeeping request: telemetry output asks, each app answers. On zbus every request is a queued copy for every app, each holding a pool buffer, and the answer is one more publish, all to fetch a value that is already sitting on the app's housekeeping channel. A last-value channel (DS-11) is made for this: each app publishes its housekeeping at least once a major frame (DS-14), and telemetry output reads whichever value is latest without waking anyone. The pool loses its request budget, and an app that has stalled still has its last housekeeping read and sent, which is what the ground wants to see.

The encoders are generated (DS-61) so telemetry output never includes another app's header (DS-68), and the ground decoder reads the same layout from the JSON dictionary. Each downlink packet starts with a printable letter giving its kind (DS-66), so a hex dump or a terminal shows at a glance whether a packet is a reply or telemetry.

### Why CRC before KISS escaping

1. The CRC bytes themselves must be escaped; appending a CRC after escaping would occasionally put a raw frame delimiter in the stream.
2. The CRC should protect content, not its transport encoding.
3. The CRC code stays independent of the framing.

The KISS type byte is included in the CRC because a flipped bit there could turn a local radio command into a remote one, or the reverse.

### Why CRC-32C

Better error detection than IEEE CRC-32 at frame lengths of a few hundred bytes. IEEE CRC-32 is the fallback for a subsystem that cannot support it.

### Two layers in the shared Python package

- **Local serial link** (software ↔ its own radio): KISS, local/remote command field, CRC, sequence. Used by the ground station and mirrored by the satellite radio simulator.
- **End-to-end space link** (ground software ↔ command ingest): command encoding, signature, counter, telemetry decoding. Radios carry it opaquely.

---

## 7. FRAM

### Why a synchronous service, not a frame-driven app

Command ingest must know the floor is stored before sending the ACK, and health needs FRAM at boot before the frame manager runs. A small `nvm` app handles background scrubbing and housekeeping.

### Why not Zephyr settings, NVS, or ZMS

They are designed around flash (erase blocks, wear leveling, garbage collection), none of which FRAM needs, and they make the on-chip layout opaque to ground dumps. The record layer is about 200 lines with fully explainable behavior.

### Why two slots, generation, and CRC

Power can fail mid-write and tear a multi-byte record. Writing alternately to the older slot means a torn write can only damage the slot being written; the other still holds the previous good value. The commit point is when the new slot's CRC becomes valid.

### Why the boot log has no head pointer

A separate head index is a second value that can tear independently of the entry it points to. Each entry carries its own boot number; the newest valid entry is found by scanning.

### Why operation without FRAM

FRAM is unproven in flight for this team. SilverSat 1 lessons recommended keeping it on a non-critical bus; SilverSat 2 reverses that (SPI removes the I2C hang concern) but requires that FRAM loss degrades operation rather than ending it: flash defaults, backup SRAM mirrors, and defined per-record fallbacks.

---

## 8. Simulation

Subsystem simulators are Python processes connected over UART. On native_sim the UART is a host PTY; on the flatsat it is a real USART through a USB-serial adapter to the same Python process. Migrating to real hardware means unplugging an adapter and plugging in the board; flight code and build do not change.

native_sim's simulated time runs ahead of the wall clock, so tests with external Python peers must use real-time pacing. Long-duration scenarios (days of command loss, reset storms) use minimal in-image C fakes instead.

Principle: **native_sim tells you whether the logic is right; the flatsat tells you whether assumptions about the physical world are right.**

---

## 9. Code patterns

These are sketches of the shape, not final code. Real message types, channel names, and priorities come from the generator and the resource map.

### App skeleton

```c
/* Channels and structs come from the generated header for this app. */
#include "msg/adcs.h"
#include "msg/common.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(adcs_sub);
ZBUS_CHAN_ADD_OBS(adcs_wakeup_chan, adcs_sub, 3);
ZBUS_CHAN_ADD_OBS(adcs_cmd_chan, adcs_sub, 3);

static struct app_status status;

/*
 * One buffer that can hold any message this app receives. A union is
 * aligned for every member, so reading a struct out of it is safe. Casting
 * a plain uint8_t array to a struct pointer is a misaligned access that
 * UBSan reports. (The generator emits this union for each app.)
 */
union adcs_msg {
    struct frame_tick tick;
    struct adcs_cmd cmd;
};

static void adcs_main(void *a, void *b, void *c)
{
    const struct zbus_channel *chan;
    union adcs_msg msg;

    adcs_init();
    while (zbus_sub_wait_msg(&adcs_sub, &chan, &msg, K_FOREVER) == 0) {
        if (chan == &adcs_wakeup_chan) {
            adcs_step(&msg.tick);
            status.steps++;
            if (msg.tick.slot == 0) {
                /* At least once a major frame; telemetry output reads the latest (DS-14). */
                zbus_chan_pub(&adcs_hk_chan, &adcs_hk, K_NO_WAIT);
            }
        } else if (chan == &adcs_cmd_chan) {
            if (adcs_dispatch(&msg.cmd) == 0) {
                status.cmd_accepted++;
            } else {
                status.cmd_rejected++;
            }
        }
        zbus_chan_pub(&adcs_status_chan, &status, K_NO_WAIT);
    }
}
K_THREAD_DEFINE(adcs_tid, ADCS_STACK_SIZE, adcs_main, NULL, NULL, NULL,
                ADCS_PRIORITY, 0, 0);
```

### Frame manager loop

The frame manager is in `apps/frame_manager/frame_manager.c`; read it there rather than as a sketch. Each minor frame it:

1. waits on its timer, skipping any minor frames it fell behind on (DS-22);
2. builds the tick: count, slot, 64-bit uptime, and MET (DS-25);
3. handles any commands waiting for it, without waiting for more;
4. reads `mode_chan`, switching tables at once for safe mode and at the next major frame otherwise (DS-23, DS-40);
5. for each enabled entry in this slot, publishes the tick, unless the app already has `FRAME_MAX_PENDING` wakeups it hasn't stepped through, in which case it counts an overrun (DS-22);
6. publishes its status, and its housekeeping at the start of each major frame.

### Device app step

```c
static void mag_step(const struct frame_tick *tick)
{
    struct sensor_value raw[3];
    int rc = sensor_sample_fetch(mag_dev);

    if (rc == 0) {
        rc = sensor_channel_get(mag_dev, SENSOR_CHAN_MAGN_XYZ, raw);
    }
    if (rc != 0) {
        mag_fault(rc);                 /* count; reinit after N; mark failed after M */
        mag_out.valid = false;
    } else {
        mag_apply_cal(raw, mag_out.b_body);
        mag_out.valid = mag_range_check(mag_out.b_body);
        mag_hk.consecutive_errors = 0;
    }
    mag_out.met = tick->met;
    mag_out.seq++;
    zbus_chan_pub(&mag_data_chan, &mag_out, K_NO_WAIT);
}
```

### Subsystem app state machine

```c
static void payload_step(const struct frame_tick *tick)
{
    switch (pl.state) {
    case PL_IDLE:
        break;
    case PL_START_SENT:
        if (reply_received(PL_MSG_START_ACK)) {
            pl.state = PL_RUNNING;
            pl.deadline = tick->met + pl.job.max_duration;
        } else if (tick->met > pl.sent_at + START_TIMEOUT) {
            pl_fault(PL_ERR_NO_START_ACK);
        }
        break;
    case PL_RUNNING:
        if (tick->count % STATUS_POLL_FRAMES == 0) {
            pl_send(PL_MSG_STATUS_REQ);
        }
        if (reply_received(PL_MSG_JOB_DONE)) {
            pl.state = PL_TRANSFERRING;
        } else if (tick->met > pl.deadline || pl.missed_polls > MAX_MISSED) {
            pl_fault(PL_ERR_TIMEOUT);
        }
        break;
    case PL_TRANSFERRING:
        pl_transfer_step();
        break;
    case PL_FAULT:
        pl_recover_step();             /* retry, soft reset, rail cycle via eps_cmd, mark failed */
        break;
    }
    pl_publish_status(tick);
}
```

### FRAM two-slot record

```c
static inline bool gen_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;       /* FRAM generations only; never the command counter */
}

int nvm_read(const struct nvm_region *r, void *buf)
{
    /* Read both slots; discard bad magic/version/CRC; return the valid one with
     * the newer generation. If neither is valid, copy the flash default and
     * return -ENODATA so the owner knows it is running on defaults. */
}

int nvm_write(const struct nvm_region *r, const void *buf)
{
    /* Under the service mutex: pick the slot with the older generation,
     * write header (gen = newest + 1) + payload + CRC, read back and verify.
     * The CRC becoming valid is the commit point. */
}
```

### Command counter check

```c
/* Plain unsigned comparison. No wrap, no serial arithmetic, no floor + 1. */
static int counter_check(uint64_t c, uint64_t floor)
{
    if (c <= floor) {
        return COUNTER_REPLAY;         /* no retransmission handling (DS-53) */
    }
    if (c - floor > COUNTER_MAX_JUMP) {
        return COUNTER_JUMP_TOO_LARGE;
    }
    return COUNTER_OK;
}
```

### Constant-time tag comparison

```c
static bool tag_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}
```

---

## 10. Student progression

Device apps come first because they are self-contained, have a clear input and output, and test entirely on native_sim: MCU die temperature (the app pattern alone), shift register (an output device with readback verification), IMU, sun sensor (real math and a meaningful validity flag), magnetometer (SPI, calibration, fault injection; strong students write the driver against the emulator). Then link-layer codecs and tests, Python simulators with fault menus, message tooling, and FRAM record layer torn-write tests. Framework apps (frame manager, health, mode manager, command ingest, telemetry output) and subsystem apps stay with mentors or the strongest students, with students writing their rejection-path and transition tests.
