# SilverSat 2 Avionics Team Plan

| | |
|---|---|
| Status | Draft for mentor and student review |
| Owner | Lee Congdon (avionics mentor) |
| Last updated | 2026-11-17 |
| Horizon | About two years: November 2026 to late 2028 |
| Proposal | [NASA CSLI](https://www.nasa.gov/announcement-of-partnership-opportunity-for-cubesat-launch-initiative/): next solicitation expected in 2028 |

This is the avionics team's plan for the next two years: what we'll learn, what we'll build, and where our work depends on the other teams. SilverSat 2 will have four or five teams. This document covers the avionics part and describes each place where it connects to the hardware, radio, payload, and project management teams.

My main lesson from SilverSat 1 was that the program needs a long-term plan to work from, so we don't decide everything session by session. This is that plan for avionics. It's in Git so changes can be reviewed and tracked like code. To change it, open a pull request. If you disagree with something, open an issue or comment on the pull request.

The flight software design itself is in [silversat2-fsw-design-decisions.md](silversat2-fsw-design-decisions.md) (the DS-nn entries). This plan says *when* and *in what order*. The design specification says *what*.

---

## 1. The shape of the two years

```
            2026        2027                                2028
            Nov  Dec    Jan ─── Mar   Apr ──────── Dec      Jan ──────────── late 2028
Onboarding  ████ ██     (and for anyone who joins later)
Mission          ◆ Dec 12: mission selected
Basilisk               █████████
Controller                        ██████████ (with hardware team)
Device                            ███████████████████████   ████████████
interfaces
Flatsat v2                                   (design) ░░░   ████████████████ → may run into 2029
Proposal               ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░   ████████████████ ◆ CSLI submission
```

The work comes in five stages. Onboarding and simulation come first. After those, two threads run side by side, because they need different skills and different people:

1. **Onboarding**: CircuitPython on a Circuit Playground Bluefruit.
2. **Simulation**: spacecraft simulation in Basilisk.
3. **Remote USB/reset controller**: a student-built board, built with the hardware team, that replaces the current interim reset controller.
4. **Mission device interfaces** (runs alongside 3): Zephyr drivers and apps for the sensors and devices the selected mission needs.
5. **Flatsat version 2**: a complete prototype flatsat, built with the hardware team. It can slip past the two-year window. Until it's ready, we add to the existing flatsat.

**The proposal** runs in parallel the whole time. NASA's CubeSat Launch Initiative (CSLI) page says the agency expects to release its next solicitation in 2028, with no month given yet, and will publish eligibility and application details before then (page updated October 29, 2026). Recent rounds opened in the summer with proposals due in November (for example, June and November 2024), so we plan on a late-2028 deadline until NASA says otherwise. Most of the education, development, and testing in this plan doubles as evidence for that proposal.

### Calendar anchors

| Date | Event | Status |
|---|---|---|
| November 2026 | Onboarding starts (the remote student began first) | Underway |
| December 12, 2026 | Students select the mission | Scheduled |
| Early 2027 | Basilisk unit, adapted to the selected mission | Planned |
| Spring 2027 | Controller board design starts with the hardware team | Planned |
| 2027 | Mission device interfaces on emulators, then on hardware | Planned |
| 2028 | Flatsat version 2 built and brought up | Target; may slip |
| 2028 | NASA releases the next CSLI solicitation | NASA says 2028; no date published |
| Late 2028 | CSLI proposal submitted | Assumed from past rounds |

All dates after December 2026 are targets. We'll update them here as they firm up.

### The mission decision

The two leading candidates are:

- **An ionospheric radio propagation experiment**: a dual-frequency beacon whose differential delay measures the ionosphere along the path to the ground.
- **Lightning sensing**: detecting lightning-generated VLF radio waves.

The choice drives stage 4 (which devices we interface to), parts of the Basilisk unit (what we simulate), the payload team's interface, and the radio team's requirements. Until the mission is selected, anything that depends on it is marked *mission-dependent* below.

---

## 2. Stage by stage

### Stage 1: Onboarding with CircuitPython

**Where:** each student's own laptop and a Circuit Playground Bluefruit (about $25). The first student's plan is in `silver-sat/onboarding-Hrushikesan` (`ONBOARDING.md`). We'll generalize it for later students.

**What:** four weeks, one a week:

| Week | Topic | Connects to |
|---|---|---|
| 1 | The REPL, loading code, non-blocking loops | How every embedded program is structured |
| 2 | Sensors, timing, logging over serial | Housekeeping telemetry |
| 3 | A UART link with text commands, and moving from Mu to VS Code | The controller's text interface (stage 3) |
| 4 | A reliable link: CRC-32C and KISS framing | The flight software's link layer (DS-65) and the shared Python package (DS-91) |

**Why CircuitPython first:** a student gets results in the first hour, with no build step and no wiring. By week 4 the student has written a small version of the controller's command interface and the flight software's link framing, so stage 3 builds on something they've already done.

**Notebooks:** each student keeps an engineering notebook in their SilverSat GitHub repo.

**Later arrivals:** this is the onboarding path for *anyone* who joins in year 1 or 2, not only this fall's cohort. The same board runs Arduino and the real flight software (DS-04, [cpb.md](cpb.md)), so a student who finishes onboarding can keep using it.

### Stage 2: Spacecraft simulation in Basilisk

**Where:** `silver-sat/basilisk-sim`, a GitHub Codespace, so Windows, Mac, and Chromebook users all get the same environment with nothing to install. Vizard visualizations are generated in the Codespace and downloaded to view.

**What:** about five exercises, written and tested against Basilisk 2.11:

| Exercise | Topic |
|---|---|
| 1 | Orbits and ground station access |
| 2 | Sensors, noise, and failure modes (sun direction, eclipse detection, gyro drift, smoothing) |
| 3 | Detumbling with magnetorquers (B-dot control) |
| 4 | Attitude pointing and momentum management |
| 5 | Fault injection and mode management, with Monte Carlo runs |

**Adapting to the mission (mission-dependent):** the exercises are attitude determination and control (ADCS) material. ADCS is the best teaching material we have, and every mission needs some of it. After December 12 we'll tilt the later exercises toward the selected mission:

- *Ionospheric beacon:* pass geometry, elevation angles, and how long a ground station sees the beacon. The pointing requirements are probably modest.
- *Lightning sensing:* ground tracks over storm regions, nadir pointing, and how attitude error affects the measurement.

We'll also replace the placeholder ground station coordinates in exercise 1 with real ones.

**Bridge to flight software:** Basilisk is the source of the reference data our sensor tests use (see the README). Later, the "bridge from orbital simulation to flight software" lesson feeds simulated sensor data into native_sim.

### Stage 3: The remote USB/reset controller (with the hardware team)

**Why:** SilverSat has no dedicated space, so we set up and tear down hardware at every session. The flatsat fixes that by living somewhere permanent and being reachable from every Codespace and from CI ([flatsat.md](flatsat.md)). It needs a device that can switch power and drive reset remotely.

**Where we are now (flatsat version 1):** a Nucleo-F446RE on an Ubuntu box on the SilverSat tailnet. A Raspberry Pi Pico running CircuitPython (`flatsat/`) drives the Nucleo's NRST line through a transistor.

Power switching already works too. The USB hub built into the Ubuntu box's keyboard can actually cut power to its ports (most hubs claim they can and don't). A mentor connects over Tailscale SSH and power-cycles the Nucleo with `uhubctl`:

```sh
sudo uhubctl -l 3-4 -p 1 -a cycle -d 5
```

[flatsat.md](flatsat.md) has the details. This has some limits. It needs someone with SSH and `sudo` on the box, rather than a command from a Codespace or CI. It switches the one port the Nucleo is on. And it depends on that particular keyboard hub: check any replacement hub before trusting it.

**What students build:** a proper controller board:

- Two USB ports, each with switchable power, and a reset output, plus a few spare GPIO outputs and inputs for other teams' boards
- A text command interface first, then the satellite protocol syntax (KISS, CRC-32C)
- Commands *set* state and never toggle it. Anything dangerous uses arm-then-fire (the same rule as DS-35)
- Outputs fail safe when the controller is unpowered, halted, or being reflashed

**Where the work lives:**

- Board design in KiCad, in its own board repo. Students use the KiCad Codespace (`silver-sat/kicad-codespace-starter`), which works on locked-down school Chromebooks.
- Firmware in this repo, `flatsat/`.

**Why it's a good project:** the controller is a subsystem board in miniature. It has a command interface over a serial link, a state machine, and fail-safe behavior. Students who build it will understand the radio, payload, and power boards much better.

**Not on the critical path:** the Pico handles reset, and the keyboard hub and `uhubctl` handle power. Together they can serve as the long-term solution if the controller board is delayed, not only as a stopgap. If we later need more switched ports than the keyboard hub has, a commercial hub that is verified to cut power can add them. If the controller takes longer than planned, nothing downstream waits for it.

### Stage 4: Mission device interfaces (in parallel with stage 3)

**What:** Zephyr drivers and apps for the devices the mission needs, in the order DS-04 and the student ownership table (section 11 of the design spec) set out:

1. **Practice devices**, before the mission is chosen or while parts are on order: die temperature, a shift register, and then the IMU, sun sensor, and magnetometer as ADCS parts are selected.
2. **Mission devices (mission-dependent):**
   - *Ionospheric beacon:* the beacon transmitter's control interface, a stable time and frequency reference, and its telemetry.
   - *Lightning sensing:* the VLF receiver or digitizer interface, event triggering, data buffering, and possibly GPS time for event timestamps.
3. **The payload app:** the avionics side of the payload interface. This is flight software on the avionics board, not the payload's own software. It talks to the payload board over a UART link with CRC-32C, like the other subsystem apps (DS-33). It turns the payload on and off, sends it commands, watches its health, and collects its data for downlink. The payload team writes and owns the software that runs on the payload itself, and the two teams agree on the interface between them.

**How:**

- Students write drivers against software emulators first (native_sim and Zephyr's emulator framework), using the classic sensor API.
- Then they move to real breakouts on a learning-tier board on their own desk ([cpb.md](cpb.md), [metro-rp2350.md](metro-rp2350.md)).
- Then to the flatsat.

Every driver comes with tests that run in CI. Students use three commands, `make test`, `make run`, and `make flash`, and the build system stays out of their way.

**Interim add-ons:** mission sensor breakouts and a payload-side test board can be wired to the existing Nucleo flatsat. That way, stage 4 doesn't wait for flatsat version 2.

### Stage 5: Flatsat version 2 (with the hardware team)

**What:** a complete prototype flatsat, with these parts:

- an avionics board closer to flight (with the flight microcontroller sized from what we learn on the Nucleo, DS-02)
- the mission devices from stage 4
- the radio, or a radio stand-in
- the payload board, or a payload stand-in
- the student-built controller from stage 3

It's reachable the same way the current one is. We'd like two copies: a fixed one for Codespaces and CI, and a portable one for hands-on testing.

**When:** it starts once the controller and a first set of mission interfaces exist, probably in 2028. It can run past the two-year window. The proposal needs evidence of working hardware and software, not a finished flatsat, and version 1 with add-ons provides that.

### The proposal track (throughout)

The CSLI proposal is a whole-program effort led by project management. Avionics contributes:

- The flight software architecture and design specification, already written and under review
- Test evidence: CI results on emulation, and hardware-in-the-loop results from the flatsat
- Simulation results from Basilisk that show the mission concept works (mission-dependent)
- A description of how students learned and built the system, using material from stages 1–5

We'll schedule specific sessions for proposal writing once the solicitation timing is known, so the proposal doesn't get squeezed in at the end.

---

## 3. Integration with the other teams

These are the places where avionics depends on decisions it doesn't own, or produces something another team depends on. Dates are targets.

### Hardware team

| Avionics needs | When |
|---|---|
| Schematic review, layout help, and supervised assembly for the controller board | From spring 2027 |
| A shared approach to KiCad: version, libraries, and the "one person per board at a time" rule | Before the controller board starts |
| **Component libraries:** an agreed place for symbols, footprints, and 3D models, and a rule for who adds and checks them. Options are the existing `silver-sat/kicad_library` repo, or a library inside each board's repo. We expect this to be a problem: newer SPI sensors and mission-specific parts often have no KiCad library, vendor and third-party footprints vary in quality, and a wrong footprint costs a board spin | Before the controller board starts, and for every new part after that |
| Input on avionics part selection (microcontroller, buses, sensors, FRAM). For each candidate part, we need to know whether a checked KiCad symbol and footprint exist, or who will make them | 2027, as stage 4 picks parts |
| Design and build of flatsat version 2 | 2028, possibly later |

| Avionics provides | |
|---|---|
| The KiCad Codespace for Chromebook users | Available now |
| Controller firmware, and a test procedure for each board | With each board |

### Radio team

| Avionics needs | When |
|---|---|
| Whether we buy a radio or build one, and which. This decides whether ground commands stay printable (DS-66) | 2027 |
| A description of how the radio behaves, so we can keep the radio simulator faithful (DS-90) | 2027 |
| Review of the avionics-to-radio serial protocol, which avionics proposes | 2027 |
| Beacon size, interval, and Morse subset (DS-69). For the ionospheric mission, this also covers how the science beacon relates to the radio | After mission selection |

| Avionics provides | |
|---|---|
| The shared Python package for KISS, CRC, sequencing, and signatures (DS-91), used by both the radio simulator and the ground station | Started |
| The radio UART on the flatsat, reachable over the tailnet | Available now |

### Payload team

| Avionics needs | When |
|---|---|
| Whether payload data reaches the ground through avionics or shares the radio directly (DS-36). This changes the architecture | Soon after mission selection |
| The payload interface, power draw, duty cycle, and data volume (which feeds DS-73, the telemetry budget) | 2027 |
| The payload's needs from the controller: reset, enable, inhibit, or nothing | Before the controller board is laid out |

| Avionics provides | |
|---|---|
| A payload simulator, written by students in Python, with a fault menu (DS-90) | 2027 |
| The payload app: avionics flight software that commands the payload and collects its telemetry and data over the agreed interface. It is not the payload's own software, which the payload team writes | Stage 4 |

### Project management team

| Avionics needs | When |
|---|---|
| The program calendar: session dates, and any planned breaks and holidays | Each term |
| The proposal schedule and the owner of each section, once CSLI timing is known | 2027–2028 |
| A place to track cross-team decisions, such as the open DS items above | Now |

| Avionics provides | |
|---|---|
| This plan, kept current in Git | Ongoing |
| Status against the milestones in section 1, and the risks below | Each term |
| The avionics sections of the proposal | 2028 |

### Every team

- **Message definitions:** every message that crosses between boards comes from one set of YAML definitions, and the encoders and decoders are generated from it (DS-60). Avionics proposes the definitions, and each team reviews the ones for its board. Treat this as an interface contract.
- **Burn wire safety:** if the antenna board gets a bench interlock on its burn wire, that line defaults to *inhibited* whenever the controller is unpowered, halted, or being reflashed. That's the opposite of every other output. After SilverSat 1's antenna deployment, we should agree on this early.

---

## 4. The environments, in one place

| Environment | Used for | Where | Runs on |
|---|---|---|---|
| CircuitPython on a Circuit Playground Bluefruit | Onboarding | `silver-sat/onboarding-*` | The student's own laptop |
| Basilisk Codespace | Simulation | `silver-sat/basilisk-sim` | Any browser |
| Zephyr Codespace | Flight software, drivers, tests | `silver-sat/silversat-fsw` | Any browser |
| Learning-tier boards (CPB, Metro RP2350) | The real flight software on your own desk | `silver-sat/silversat-fsw` ([cpb.md](cpb.md), [metro-rp2350.md](metro-rp2350.md)) | The student's own laptop |
| Flatsat (Nucleo-F446RE on the tailnet) | Hardware tests from a Codespace and from CI | [flatsat.md](flatsat.md) | Reached from a Codespace |
| KiCad Codespace | Board design | `silver-sat/kicad-codespace-starter` and board repos | Any browser, including school Chromebooks |

Codespaces run on each student's free GitHub allowance. Students need a GitHub account (minimum age 13) with two-factor authentication.

---

## 5. Students graduate

A two-year plan will outlast some of the students who start it. Several things help someone who arrives in the middle get productive within a month:

- **Stage 1 is the onboarding path for every new arrival.** We run it again whenever someone joins, not only in the first year.
- **The documents are the onboarding path:** this plan, the design specification, the README, and the `docs/` guides. If you had to ask a mentor something that should have been written down, open a pull request that writes it down.
- **Notebooks in GitHub** let a departing student's reasoning stay with the project.
- **Small pull requests and CI** mean nobody has to understand the whole system before contributing to one part of it.

## 6. Risks

| Risk | Mitigation |
|---|---|
| The mission decision slips past December 12 | The Basilisk unit and the practice devices are useful for either mission |
| Nobody from the hardware team can supervise the controller board | The Pico (reset) and the keyboard hub with `uhubctl` (power) keep the flatsat working indefinitely. The board waits |
| The toolchain overwhelms students working a few hours a week | Make targets hide west, CMake, devicetree, and Kconfig. Students start in CircuitPython. Mentors own the infrastructure (design spec section 11) |
| CSLI timing or eligibility changes | Check NASA's CSLI page each term. Most of the work stands on its own whatever the launch route |
| A selected part has no usable KiCad symbol or footprint | Ask about library support during part selection. Check every new footprint against the datasheet before layout |
| Flatsat version 2 runs long | It's allowed to. Version 1 with add-ons supports the proposal |

## 7. Questions for mentors

1. **Hardware team:** who can supervise the controller board, and when can they start?
2. **Radio and payload teams:** when can you answer DS-66 (buy or build the radio) and DS-36 (payload routing)? They affect stage 4 more than anything else.
3. **Project management:** who owns the proposal, and where should cross-team decisions be tracked?
4. **Everyone:** does stage 2 need to change for either mission candidate in ways this plan misses?

---

## Revision history

| Date | Change |
|---|---|
| 2026-11-17 | Initial draft. Builds on the multi-year avionics plan drafted for mentors in November 2026 (not sent), and replaces its November 2027 proposal assumption with an anticipated late-2028 CSLI solicitation |
| 2026-11-17 | Flatsat power switching through the keyboard hub and `uhubctl` as an ongoing fallback for the controller. KiCad component libraries added to the hardware team's needs and to the risks. The payload app described as the avionics side of the payload interface |
| 2026-11-17 | CSLI timing from NASA's page: next solicitation expected in 2028, no date yet |
