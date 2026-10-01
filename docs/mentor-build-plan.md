# SilverSat 2 mentor code: build plan

> **Temporary working document.** Delete it once the frame manager lands. Settled decisions live in the specification and rationale; this file records the build order and the review that produced them.
>
> **Status:** steps 1 and 2 are merged. Step 3, the resource map and app scaffolding, is on branch `feat/resource-map` for review. Next is step 4, the frame manager.

## Context
Lee wants to build the mentor-owned flight software (frame manager, command ingest, health, mode manager, telemetry output; §11 of the spec) incrementally with Claude Code, reviewing each piece and its tests before the next. Students first do six Basilisk exercises (basilisk-sim), then write device apps in this repo. This plan answers the environment question, records what the review of the docs and existing code turned up, and proposes the first few steps. No code gets written until we've talked it through.

## 1. Repo access and environment

**Access:** You don't need to open anything else. silversat-fsw is my working directory, and basilisk-sim is cloned at `~/Code/basilisk-sim` (on main, up to date with origin). I can read both. I can also fetch the public GitHub repos if needed.

**The limit is running code, not reading it.** On this Mac host I can edit files but can't run `make test`. For the review loop you want (write code, run tests, look at failures, iterate), Claude Code needs to run *inside* the Linux environment.

**Recommendation: use a Codespace and run Claude Code in it** (the VS Code extension or `claude` in its terminal).
- It's native x86_64 Linux, the same as CI and the students' setup (DS-03), and it has 4 cores.
- The devcontainer forces `linux/amd64`. On Apple silicon, a local container runs that under emulation (Rosetta or QEMU), which is slow for twister.
- I believe Rosetta for Linux doesn't run 32-bit x86 binaries. If so, the `-p native_sim` (32-bit) half of `make test` would fail or crawl locally, even though CI runs it.
- The local container is still a reasonable offline fallback, using `make test-quick` (64-bit only).
- Later we could add Claude Code to `devcontainer.json` as a feature. That's optional, and it's a mentor-only concern, so it may be better kept out of the students' image.

## 2. Review findings (spec vs. repo; flagging, not fixing)

1. **Doc paths disagree.** CLAUDE.md says `docs/silversat2-fsw-design-decisions.md`, but the file is at the repo root. `docs/design-rationale` has no `.md` extension, so it won't render on GitHub. Suggested fix: move both into `docs/` with `.md`, as a small docs PR.
2. **Messages aren't generated yet (DS-60/61/68), and CLAUDE.md forbids hand-written structs.** The frame manager needs `struct frame_tick` and its channels. This chicken-and-egg problem is the main decision before any app code (see §4).
3. **There's no resource map header yet (DS-07).** Every app needs one, so it should come first.
4. **zbus overrun detection in the rationale sketch doesn't match how zbus behaves.** This is verified in `subsys/zbus/zbus.c` at v4.4.0; see §2a. **Decided:** per-wakeup counting, and the frame manager stops delivering to an app that health reports as not responding.
5. **Casting the message buffer in the app skeleton will trip UBSan.** `uint8_t msg[MSG_MAX_SIZE]` cast to `const struct frame_tick *` is a misaligned access, and CI runs with UBSan. The pattern should use an aligned union of the app's message types. The rationale should be updated so students copy the right shape.
6. **`k_uptime_get_32()` wraps after about 49.7 days.** It appears in the frame manager sketch; the tick should carry a 64-bit uptime (or seconds).
7. **Naming.** The sketch uses `tick.minor`, while DS-25 says "slot". Pick one; I suggest `slot`.
8. **Small issues:**
   - `app/boards/native_sim.overlay` points to `tests/drivers/mag/boards/`, which doesn't exist (the test uses `app.overlay`).
   - `drivers/CMakeLists.txt` has a dangling "e.g.:".

## 2a. How zbus actually behaves (verified against the v4.4.0 source)
- **The channel is last-value.** `zbus_chan_read` always returns the newest publish.
- **What a subscriber receives depends on its observer type:**
  - A *plain subscriber* gets only a channel pointer, queued in a `k_msgq` of fixed depth. It then reads the current value, so intermediate values are overwritten. A full queue makes a `K_NO_WAIT` publish return an error.
  - A *message subscriber* gets its own `net_buf` copy of every publish, queued in an unbounded `k_fifo`. Nothing is overwritten.
- **One pending point forces one observer type per app.** With a plain subscriber, two commands published back to back would collapse into one, so commands would be lost. That's why the skeleton uses a message subscriber, and it means **wakeups and housekeeping requests are also queued copies, not overwrites.** The "overwrite" behavior you remembered is true of the channel, but not of what an app with one message subscriber receives.
- **A stuck app gets no queue-full signal.** Its wakeups pile up in the fifo, each holding one buffer from the pool shared by all channels.
- **Pool exhaustion faults, but not always.** (Corrected in step 3, after a test on native_sim.) Every publish on every channel takes one buffer for the length of the publish, and each message subscriber's copy takes another. A copy that cannot be allocated makes the publish return `-ENOMEM`. A publish that cannot get its own buffer hits `_ZBUS_ASSERT(buf != NULL)`: a development build panics, and a flight build dereferences NULL. That happens when another publish is in flight while a stuck app holds the rest of the pool. `tests/unit/zbus_pool` pins this down.

**Design (updates DS-22 and DS-43; both are Proposed):**
- **Per-wakeup counting.** The frame manager counts `delivered[i]` per table entry. It reads the target app's status channel (last-value, which is legitimate channel sharing under DS-12) for `steps`. If `delivered - steps` reaches `FRAME_MAX_PENDING` (from the resource map; 2 is likely), it skips the publish and counts an overrun for that entry. The frame manager never allocates a buffer the app can't consume. This check is the hard guard that keeps the pool bounded.
- **Health decides "not responding"** from the same counts over a longer window (DS-43). It then commands the frame manager to disable that app's entries: stop delivering, then an event, then a reset if the app is critical. Health can re-enable the entries if the app recovers. This is the policy layer, and it covers commands, which the frame manager doesn't send.
- **Pool sizing becomes arithmetic,** kept in the resource map: the sum of `FRAME_MAX_PENDING` over apps, plus the command and housekeeping-request depths. With command ingest, evaluate `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_ISOLATION` to give command channels their own pool.
- **Tests:** a stalled test app never pushes the pool past its bound; its overruns are counted per entry; other apps still get wakeups; health's disable command stops delivery.

**Uptime (finding 6).** `k_uptime_get()` is already 64-bit. `k_uptime_get_32()` just calls it and truncates the result, so the 64-bit call is the same work with the truncation removed. On the M4 a 64-bit value is two registers, which is negligible at 10 Hz. Using 64-bit needs no extra code. The tick carries `int64_t uptime_ms`.

**The message-type alias (finding 5).** Each app declares an aligned union of the message types it receives, and `zbus_sub_wait_msg` reads into it. The generator can emit that union per app, since it knows which channels each app observes.

## 3. Proposed structure for apps (for discussion)
- Apps live in the module, as `apps/<name>/` with a `Kconfig` symbol each (`CONFIG_SS_*`, one per app). The module `CMakeLists.txt` adds them, the same way `drivers/` works.
- This lets `tests/app/<name>/` enable one app in isolation, and lets `app/` enable all of them. `app/src/main.c` stays boring.
- The resource map goes at `include/silversat/resource_map.h`.

## 4. Proposed sequence (each is one small PR you review before the next)
0. **Environment:** open a Codespace, run `make test` to confirm it passes, and start Claude Code there.
1. **Docs cleanup PR:** finding 1, plus any spec and rationale edits you accept from findings 4–7.
2. **Message decision.** Either:
   - (a) a minimal generator now: YAML → C header for the shared types (tick, status, events) and per-app channels. It grows later and students extend it (§11). Or
   - (b) an interim hand-written `msg/common.h`, marked for replacement, which needs an explicit exception to CLAUDE.md.
   - DS-63 (which repo the definitions live in) is Open, and it affects (a).
3. **Resource map + app scaffolding:** the `apps/` layout, Kconfig and CMake wiring, and an empty `tests/app/` harness running on simulated time.
4. **Frame manager** (DS-20–25; the build follows Proposed DS-22/24/25, which will be flagged in the PR):
   - a const table per mode;
   - enable/disable entries by command;
   - table switch at the major-frame boundary, with immediate entry to safe mode;
   - a tick carrying count, slot, uptime, and MET (MET comes from an interface stubbed until the FRAM/health work exists);
   - overrun and delivered counters;
   - housekeeping and status channels.
5. **Frame manager tests** on native_sim with simulated time:
   - slot phasing over N major frames;
   - disabled entries not delivered;
   - table switch deferred to the boundary, and the safe-mode exception;
   - overrun counting with a stalled test subscriber;
   - rejected commands (bad index, bad table);
   - no drift over simulated hours.
6. Then command ingest (DS-50/53), with the counter-at-max unit test that DS-53 requires.

## Verification (per step)
- Run `make test` in the Codespace, on both native_sim targets with ASan and UBSan, and keep it under about 3 minutes.
- Run `make build` for nucleo_f446re (build only).
- You review the diff and the test list before we move to the next step.

## Decisions so far
- **Environment:** Codespace, with Claude Code running inside it. Step 0 happens there before any code is written.
- **Messages:** option 2(a), a minimal generator.
  - It lives in `messages/` in this repo; that's the DS-63 fallback, and the separate repo is deferred.
  - Python and Jinja2, run at build time by a CMake custom command, so generated files are never committed. Check that jinja2 is in the devcontainer.
  - The first PR covers only the frame manager's needs: the shared tick, status, and events types; one command; one housekeeping message; their channel definitions (DS-68); and generator unit tests.
  - Encode/decode, Python classes, the ICD, and golden vectors come later, with command ingest.

- **Docs PR (step 1)** — done from the Mac session:
  - `git mv silversat2-fsw-design-decisions.md docs/silversat2-fsw-design-decisions.md`
  - `git mv docs/design-rationale docs/design-rationale.md`
  - Fix references to them in CLAUDE.md, README, and the rationale.
  - **DS-22:** replace "full observer queue counted as overrun" with per-wakeup counting and a bounded pending count (§2a). Add a revision-history row.
  - **DS-43:** health disables an unresponsive app's frame entries.
  - **DS-25:** the tick field is `slot`, and uptime is 64-bit ms.
  - **Rationale §2:** add the message subscriber vs. plain subscriber point and the pool-exhaustion fault.
  - **Rationale §9:** the app skeleton uses an aligned union; the frame manager sketch uses `slot`, `k_uptime_get()`, and the pending check.
  - The small fixes from finding 8.
- **Finding 5:** an aligned union, generated per app.
- **Finding 6:** a 64-bit uptime, with no extra code (§2a).
- **Finding 7:** `slot`.

- **`apps/<name>/` layout:** agreed.
- **`FRAME_MAX_PENDING` = 2 for every app,** so the pool size is a compile-time expression in the resource map: `2 × N_APPS` plus the command and housekeeping-request depths. (Step 3 adds one buffer per publishing thread; see below.)
- **Protected apps.** A `protected` flag per app, meaning the same as "critical":
  - The frame manager rejects a disable command for a protected entry, with the rejected counter and an event.
  - Health escalates a stalled protected app straight to reset.
  - Candidates: health, mode manager, command ingest, telemetry output.
- **Re-enable policy per app, as data.** Each app has a const row with `protected`, the stall threshold, a re-enable policy (`NEVER`, `AUTO`, or `GROUND`), and the auto-retry cap.
  - `AUTO` means: cooldown, re-enable, watch; after the cap is reached, fall back to `GROUND` with an event.
  - Health keeps runtime state per app: when it was disabled and how many times it was re-enabled automatically.
  - Built with health, not the frame manager. The frame manager only needs `protected`.
- **Handoff to the Codespace.** Step 1 (the docs PR) is done from this Mac session: no build is needed, and CI checks it. It also adds this plan as `docs/mentor-build-plan.md`, to be removed after the frame manager lands. After it merges, Lee opens the Codespace, installs Claude Code, and starts from CLAUDE.md and `docs/`.

- **Per-app attribute row goes in the resource map.** Lee approved extending DS-07, which is Specified. The docs PR edits DS-07's text and adds a revision-history row. The frame manager and health both read the row.
- **Protected apps:** health, mode manager, command ingest, and telemetry output. This is recorded in DS-43 in the docs PR.

- **Step 3 decisions (2026-10-01):**
  - The pool size is a Kconfig number, with a build check in the resource map against the required size: every queued copy plus one buffer per publishing thread. Isolated pools are still to be evaluated with command ingest.
  - The DS-07, DS-22, and rationale §2 corrections land in the step 3 PR, with `tests/unit/zbus_pool` as the regression test.
  - Scaffolding is wiring plus a test harness only; the frame manager is the first real app.

## Next action
Review step 3 (`feat/resource-map`). Then step 4: the frame manager.
