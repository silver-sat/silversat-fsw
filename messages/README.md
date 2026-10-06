# Message definitions

Every command, telemetry message, and channel payload is defined here in YAML,
and C code is generated from it at build time (DS-60, DS-61, DS-68 in
[the specification](../docs/silversat2-fsw-design-decisions.md)). Never write a
message struct, command ID, or channel definition by hand.

| File | What it defines | Generated |
|---|---|---|
| `common.yaml` | Types shared by every app: the frame tick, app status, events, enums, and data channel types | `msg/common.h` |
| `apps/<app>.yaml` | One app's commands, housekeeping, data channels, and the internal commands it sends | `msg/<app>.h`, its channels and senders, its share of command routing and housekeeping encoding |
| `nvm_map.yaml` | FRAM records, each owned by one app | `nvm/<app>.h` |

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
2. List the modes it is allowed in, for example `modes: [safe, nominal]`.
   There is no default (DS-50).
3. Commands set state; they do not toggle it (DS-35).
4. Rebuild. The generator checks your change and stops with a message naming
   the file and the entry if something is wrong.

Field types are `bool`, `uint8` to `uint64`, `int8` to `int64`, `float32`,
`float64`, or the name of an enum in `common.yaml`. Command arguments can't
be floats yet.

## Data channels and initial values

A data channel (`data_channels:`) is a last-value channel carrying a type
from `common.yaml`, which any app may read. Until its owner first publishes,
it holds zeros, or the values given under `initial:`:

```yaml
data_channels:
  - name: mode_chan
    type: mode_state
    description: The current mode.
    initial:
      mode: deploy
      reason: boot
```

Give an initial value when zero would be the wrong thing for a reader to see
before the first publish.

## Sending a command to another app

An app sends internal commands only through functions the generator writes
for it (DS-68). List each command under `sends:` in your app's YAML:

```yaml
sends:
  - radio.set_transmit
```

and `msg/<your app>.h` gains

```c
int send_radio_set_transmit(bool enabled);
```

with one parameter per argument, in YAML order. Your app still includes only
its own header and `msg/common.h`. A misspelled app or command fails the
build. Each call returns 0 if the command was sent, or `-EBUSY` if your app
already has `CMD_MAX_PENDING` commands that app hasn't handled; try again
next frame. Commands set state rather than toggling it (DS-35), so sending
one again is safe.

## Command text

The ground sends a command as text: the app, the command, then the
arguments in YAML order, separated by single spaces:

```
frame_manager set_entry_enabled nominal 3 false
```

Integers are decimal, bools are `true` or `false`, and enums are the value's
name. The generator writes the satellite's decoder (`src/cmd_routes.c`), and
`tools/command_text.py` checks text on the ground using the same YAML:

```
python3 tools/command_text.py frame_manager set_entry_enabled nominal 3 false
```

## Build checks

The generated channel files fail the build if any message is bigger than a
zbus buffer (`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE`, set in
`messages/Kconfig`). Every publish takes a buffer, so this applies to every
channel. `include/silversat/resource_map.h` uses `APP_COUNT` and
`APP_WAKEUP_COUNT` from `msg/common.h` to check the size of the buffer pool.

## FRAM records

Records that survive a reset are defined in `nvm_map.yaml` (DS-71, DS-74),
each with one owning app, a version, and fields with defaults. For an owner
`foo`, the generator writes `nvm/foo.h`, which only `foo` includes:

```c
struct nvm_bar record;

nvm_bar_read(&record);   /* 0 from FRAM; -ENOENT or -EIO: the default */
record.count++;
nvm_bar_write(&record);  /* 0 once stored; -EIO if FRAM has failed */
```

A read always fills the record: when FRAM is blank, missing, or failed, it
gets the default from the map (DS-75). To change a record's fields, change
its version too.

## Test-only apps

App ids run from 1 to 63: the frame manager's report to health has one
bit per app (DS-43). Flight apps count up from 1; test-only apps use 48 to
63, so they never collide with a flight app.

An app test can define apps of its own, generated exactly like flight apps
but only into that test's build. Put their YAML files in a directory in the
test and name it in the test's `CMakeLists.txt` before `find_package(Zephyr)`:

```cmake
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)
```

`tests/app/harness` is the example.

## Testing the generator

```
make test-python
```

This runs `tests/test_msggen.py`, along with the tests for `tools/`.
`make test` runs it too.
