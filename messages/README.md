# Message definitions

Every command, telemetry message, and channel payload is defined here in YAML,
and C code is generated from it at build time (DS-60, DS-61, DS-68 in
[the specification](../docs/silversat2-fsw-design-decisions.md)). Never write a
message struct, command ID, or channel definition by hand.

| File | What it defines | Generated |
|---|---|---|
| `common.yaml` | Types shared by every app: the frame tick, app status, events, and enums | `msg/common.h` |
| `apps/<app>.yaml` | One app's commands and housekeeping | `msg/<app>.h`, and its channels |

The generated files are written to `<build>/silversat_msg/` and are never
committed. To see them, build anything that sets `CONFIG_SS_MESSAGES=y` (for
example `west build -b native_sim tests/unit/messages`) and look there.

## What each app gets

For an app named `foo`, the generator writes the four channels every app has
(DS-14):

| Channel | Type | Semantics |
|---|---|---|
| `foo_cmd_chan` | `struct foo_cmd`: an `id` plus the command's arguments | Receive with a message subscriber, so no command is lost |
| `foo_hk_chan` | `struct foo_hk` | Last value |
| `foo_status_chan` | `struct app_status` (step and command counters) | Last value |
| `foo_wakeup_chan` | `struct frame_tick`, only if `wakeup: true` | Published by the frame manager |

It also writes `union foo_msg`, which is big enough and aligned for every
message `foo` receives. Pass it to `zbus_sub_wait_msg()`.

Your app includes only `msg/foo.h` and `msg/common.h`.

## Adding a command

1. Add an entry under `commands:` in `apps/<app>.yaml`, with a new `id`. Never
   reuse an ID or delete a command that has flown (DS-62).
2. Commands set state; they do not toggle it (DS-35).
3. Rebuild. The generator checks your change and stops with a message naming
   the file and the entry if something is wrong.

Field types are `bool`, `uint8` to `uint64`, `int8` to `int64`, `float32`,
`float64`, or the name of an enum in `common.yaml`.

## Build checks

The generated channel files fail the build if any message is bigger than a
zbus buffer (`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE`, set in
`messages/Kconfig`). Every publish takes a buffer, so this applies to every
channel. `include/silversat/resource_map.h` uses `APP_COUNT` and
`APP_WAKEUP_COUNT` from `msg/common.h` to check the size of the buffer pool.

## Test-only apps

An app test can define apps of its own, generated exactly like flight apps
but only into that test's build. Put their YAML files in a directory in the
test and name it in the test's `CMakeLists.txt` before `find_package(Zephyr)`:

```cmake
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)
```

`tests/app/harness` is the example.

## Testing the generator

```
make test-messages
```

This runs `tests/test_msggen.py`. `make test` runs it too.
