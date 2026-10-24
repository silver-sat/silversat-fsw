# The flatsat

The flatsat is a Nucleo-F446RE plugged into an Ubuntu box, which sits on a
private network, a Tailscale *tailnet*, with every student's Codespace and
with CI (DS-04, DS-05). From a Codespace you build as usual, then load the
firmware onto the real board and watch it run:

```sh
make build
make flash          # load it onto the flatsat and start it
make console        # watch its console (Ctrl-C to stop)
```

```
 Codespace ──┐                      ┌─ Ubuntu box "flatsat" ─────────────────┐
 Codespace ──┼── tailnet ───────────┤  OpenOCD   :3333 ── USB ── Nucleo (ST-LINK)
 CI ─────────┘  (Tailscale)         │  ser2net   :4000 ── console (USART2)  │
                                    │  ser2net   :4001 ── FT232H ── radio UART
                                    │  ser2net   :4002 ── QT Py ── NRST     │
                                    └────────────────────────────────────────┘
```

- **OpenOCD** is the GDB server for the Nucleo's on-board ST-LINK: `make
  flash` loads firmware through it, and `make debug` debugs through it.
- **ser2net** puts three serial ports on TCP ports: the console (the
  ST-LINK's serial port, USART2), the radio UART (USART1, through an
  FT232H), and the reset controller. `make console` reads the first, the
  radio simulator talks to the second, and `make reset` drives the third.
- **Tailscale** connects them. Codespaces and CI can reach only those four
  ports on the flatsat, and nothing else on the box's network.

## Using it

### From a Codespace

A Codespace joins the tailnet on its own each time it starts, if the
`TS_AUTH_KEY` Codespaces secret is set (see "Setting it up"):
`.devcontainer/tailscale-up.sh` runs after every start. Check:

```sh
tailscale status            # flatsat should be listed
```

If it says `Logged out`, join by hand (or run the script again):

```sh
bash .devcontainer/tailscale-up.sh
```

Then:

```sh
make build                  # the Nucleo firmware, in build/
make flash                  # load it onto the flatsat and start it
make console                # watch the console
make debug                  # or debug it in GDB: break, continue, step...
```

To talk to the board over the radio link, as the ground would (signed
commands up, telemetry and events down):

```sh
python3 sim/radio_sim.py --port tcp://flatsat:4001 --listen
python3 sim/radio_sim.py --port tcp://flatsat:4001 --listen nvm retry
```

docs/nucleo-on-your-desk.md explains what the console and the listener
show, the boot modes, and reset loops.

### One board, many students

There is one flatsat. `make flash` replaces whatever is running, and a new
console connection takes over from an older one. Agree who has the board,
for example in the team chat, before you flash it. CI flashes it too, on
each merge to main, nightly, and when someone runs it on demand (below):
for about a minute it takes over the console, so your `make console`
ends, and it resets the board with the firmware from main.
Close your `make debug` when you finish: while it's attached, OpenOCD
refuses everyone else, CI included. If you forget, the box closes a GDB
session that has sent nothing for an hour, and the board carries on
running.

### Things that still need hands

Until the box has a USB power switch and a relay on the Nucleo's reset pin
(DS-04), someone at the box:

- **Holds the blue button (B1) through a reset** to boot test mode, or
- **Unplugs the board** to clear the backup SRAM (boot 1, deploy mode).

`make flash` resets the board itself, so ordinary loading needs no one.

## Setting it up

Once, by a mentor. Four parts: the tailnet, the box, hardening the box, and
GitHub.

### 1. The tailnet

Create a tailnet for SilverSat at tailscale.com (the free plan covers a
small team). In its admin console:

1. **DNS:** keep MagicDNS on, so `flatsat` is a name every member can use.
2. **Access controls:** replace the policy with:

   ```jsonc
   {
     // Who may give devices each tag.
     "tagOwners": {
       "tag:flatsat":   ["autogroup:admin"],
       "tag:codespace": ["autogroup:admin"],
       "tag:ci":        ["autogroup:admin"],
     },
     "acls": [
       // Codespaces and CI reach only the flatsat's GDB port, its serial
       // ports, and the reset controller.
       {"action": "accept", "src": ["tag:codespace", "tag:ci"],
        "dst": ["tag:flatsat:3333,4000,4001,4002"]},
       // Mentors reach everything, to look after the box.
       {"action": "accept", "src": ["autogroup:admin"], "dst": ["*:*"]},
     ],
     "ssh": [
       // Mentors log in to the box with Tailscale SSH: no keys to manage.
       // "check" makes them sign in to Tailscale again (at most every 12
       // hours, the default; changing it needs a paid plan), so an
       // unattended laptop isn't enough. No root: each mentor logs in as
       // themselves and uses sudo, which records who did what.
       {"action": "check", "src": ["autogroup:admin"], "dst": ["tag:flatsat"],
        "users": ["autogroup:nonroot"]},
     ],
   }
   ```

   Each mentor needs an account on the box (`sudo adduser <name>`, and
   `sudo usermod -aG sudo,adm <name>`: `sudo` to run commands as root,
   `adm` to read the services' logs with `journalctl` without it; it takes
   effect at the next login). `adduser` also asks for a full name and
   phone numbers; they're optional (press Enter), and anyone with an account
   on the box can read them. Mentors log in from a computer on the tailnet
   with plain `ssh <name>@flatsat`. Tailscale, not the box, checks who they
   are: it prints a link to sign in again when 12 hours have passed, and
   otherwise lets them straight in. The box never asks for the account's
   password at login; that's for `sudo`, and for the box's own keyboard.
   To edit a system file, `sudoedit <file>` is safer than `sudo nano`: the
   editor runs as you, on a copy that's put back when you save.

3. **An auth key for Codespaces** (Settings → Keys → Generate auth key):
   reusable, ephemeral (a Codespace's device disappears when it stops),
   pre-approved, tagged `tag:codespace`. Copy it for part 4 below. It
   expires after at most 90 days; note the date, because Codespaces stop
   joining when it does.

   Pre-approved matters once device approval is on (item 5): without it,
   every Codespace start waits for an admin to approve it. It also means
   the key alone is enough to join, so treat it as a password. A leaked key
   reaches only what `tag:codespace` may (the three flatsat ports); revoke
   it and make a new one (Settings → Keys) if it's ever shown or shared.
4. **A federated identity for CI** (workload identity federation: GitHub
   vouches for the workflow, so there is no secret to store or rotate). On
   the Trust credentials page, choose Credential → OpenID Connect:
   - Issuer: **GitHub Actions**.
   - Subject: `repo:silver-sat@46551018/silversat-fsw@1376033827:*`, which
     matches this repository's own workflow runs. GitHub puts the owner's
     and repository's numeric IDs in the
     subject, so a deleted and re-created repository of the same name
     doesn't match. To check the start of it (for a fork, say), run
     `gh api repos/OWNER/REPO/actions/oidc/customization/sub` and use its
     `sub_claim_prefix`, then `:*`. The plain `repo:OWNER/REPO:*` form
     doesn't match, and the job fails with `token exchange failed with
     status 403`.
   - Scope: `auth_keys` (write), with tag `tag:ci`. The policy above must
     already define `tag:ci`, or the page won't offer it.

   Generate it, and copy its **Client ID** and **Audience**. Neither is a
   secret.
5. **Device approval** (Settings → Device management): turn it on, so no
   device joins by signing in alone: a member's laptop, or anything using a
   key that isn't pre-approved, waits for an admin. It doesn't stop the
   pre-approved Codespaces key (item 3). After turning it on, run the
   `flatsat` workflow once (below) to check that CI still joins.

### 2. The box

Ubuntu 24.04 or later. Plug the Nucleo into it by its own USB connector,
and the FT232H by USB, wired to the Nucleo as in docs/nucleo-on-your-desk.md
(FT232H D0 to Nucleo D2, D1 to D8, GND to GND).

**Name it** `flatsat`, the name everything else uses:

```sh
sudo hostnamectl set-hostname flatsat
```

**Join the tailnet**, as the flatsat, with Tailscale SSH. Use Ubuntu's
`curl`, not a snap: snaps can't write outside their sandbox (`curl: (23)`),
and the curl snap isn't published by curl or Canonical.

```sh
snap list curl 2>/dev/null && sudo snap remove curl
sudo apt install curl
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --ssh --advertise-tags=tag:flatsat
sudo tailscale set --auto-update     # Tailscale updates itself; no reboot
```

**Install OpenOCD and ser2net:**

```sh
sudo apt install openocd ser2net
```

The openocd package installs the USB permissions for the ST-LINK.

**Update the ST-LINK's firmware** before anything else, from a computer
with STM32CubeProgrammer (ST's free tool; its ST-LINK panel has a
"Firmware upgrade" button), choosing "STM32 Debug + VCP". Old firmware
(V2J33 on ours) left the console port stuck after the box rebooted. The
update changes the
ST-LINK's name under `/dev/serial/by-id/`, so do it before setting up
ser2net below, or update `/etc/ser2net.yaml` after.

**A user to run OpenOCD**, with access to the ST-LINK:

```sh
sudo adduser --system --group flatsat
sudo usermod -aG plugdev,dialout flatsat
```

**A name for the ST-LINK**, so OpenOCD can follow it being unplugged and
plugged back in. OpenOCD doesn't notice its ST-LINK going away: it carries
on with a dead connection, accepting GDB but unable to reset, halt or read
the chip. So a udev rule names the ST-LINK for systemd, and starts OpenOCD
whenever it appears; the service below stops when it goes. Create
`/etc/udev/rules.d/70-flatsat-stlink.rules`:

```
# The Nucleo's ST-LINK (374b before a firmware update, 3752 after):
# name it for systemd, and start OpenOCD whenever it's plugged in.
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0483", ATTR{idProduct}=="374b|3752", TAG+="systemd", SYMLINK+="stlink", ENV{SYSTEMD_WANTS}+="openocd-flatsat.service"
```

```sh
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --action=add
systemctl status dev-stlink.device     # "active (plugged)"
```

If `dev-stlink.device` isn't found, check the ST-LINK's product ID with
`lsusb | grep 0483` and add it to the rule.

**OpenOCD as a service**, so its GDB server is always there. Create
`/etc/systemd/system/openocd-flatsat.service`:

```ini
[Unit]
Description=OpenOCD GDB server for the flatsat's Nucleo
After=network-online.target
# Stop when the ST-LINK is unplugged; the udev rule starts it again.
BindsTo=dev-stlink.device
After=dev-stlink.device

[Service]
User=flatsat
ExecStart=/usr/bin/openocd -f board/st_nucleo_f4.cfg \
    -c "reset_config srst_only srst_nogate connect_assert_srst" \
    -c "stm32f4x.cpu configure -event gdb-detach { resume }" \
    -c "bindto 0.0.0.0" -c "gdb_port 3333" \
    -c "telnet_port disabled" -c "tcl_port disabled" \
    -c "init" -c "reset run"
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

The `reset_config` line holds the board in reset while OpenOCD connects.
OpenOCD checks the chip only once, as it starts, and refuses GDB from then
on if that check failed; firmware that is asleep or stuck can make it fail.
Nothing then lets go of the reset until the next reset command, so the
last line, `init` then `reset run`, starts the board as OpenOCD finishes
starting. So starting the service restarts the board; a reset keeps the
backup SRAM, so it doesn't go back to deploy mode.

Unplugging the Nucleo stops the service, and plugging it back in starts a
fresh one, with no one on the box (`journalctl -u openocd-flatsat` shows
both).

The `gdb-detach` line starts the board again whenever GDB lets go of it.
Leaving GDB usually means pressing Ctrl-C, which halts the board, then
`quit`; without this line the board stays halted, and its console and
telemetry go silent as if it were dead.

```sh
sudo systemctl enable --now openocd-flatsat
systemctl status openocd-flatsat      # "Listening on port 3333 for gdb connections"
journalctl -u openocd-flatsat -n 20   # no "examination failed"
```

**ser2net**, for the three serial ports. Find their stable names — these
do not move when devices re-enumerate, which `/dev/ttyACM*` does:

```sh
ls /dev/serial/by-id/
# usb-STMicroelectronics_STM32_STLink_066B...-if02   the console
# usb-FTDI_..._-if00-port0                            the FT232H
# usb-Adafruit_Industries_LLC_QT_Py_M0_...-if02      the reset controller
```

The reset controller gets a `/dev/flatsat-nrst` symlink of its own from
the udev rule under "Reset controller" below; the config uses that.

Replace `/etc/ser2net.yaml` with, using those names:

```yaml
%YAML 1.1
---
# The Nucleo's console: the ST-LINK's serial port (USART2).
connection: &console
    accepter: tcp,4000
    enable: on
    options:
      kickolduser: true
      mdns: false
    connector: serialdev,/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_066B...-if02,115200n81,local

# The radio UART (USART1), through the FT232H.
connection: &radio
    accepter: tcp,4001
    enable: on
    options:
      kickolduser: true
      mdns: false
    connector: serialdev,/dev/serial/by-id/usb-FTDI_..._-if00-port0,19200n81,local

# The reset controller: the QT Py's second CDC port (-if02, the data port;
# -if00 is its REPL). Baud is meaningless over USB CDC, but ser2net wants one.
connection: &nrst-controller
    accepter: tcp,4002
    enable: on
    options:
      kickolduser: true
      mdns: false
      max-connections: 1
    connector: serialdev,/dev/flatsat-nrst,115200n81,local
```

`max-connections: 1` matters more here than on a console. Two clients
interleaving `set_reset` commands on a reset line is a bad time, and
`kickolduser` means a CI job that died without closing its socket does not
lock the rig out.

```sh
sudo systemctl restart ser2net
```

`mdns: false` keeps ser2net from announcing the ports on the local network.

**Answer only on the tailnet.** OpenOCD and ser2net listen on every
network, so a firewall keeps those ports off the box's local network:

```sh
sudo ufw allow 41641/udp      # Tailscale's direct connections (else it relays, slowly)
sudo ufw allow in on tailscale0 to any port 22,3333,4000,4001,4002 proto tcp
grep IPV6 /etc/default/ufw    # IPV6=yes, or the ports stay open over IPv6
sudo ufw enable
sudo ufw status verbose       # each rule listed for v4 and v6
```

Port 22 on the tailnet is for mentors' Tailscale SSH. The box needs no
other SSH server (see the next part).

**Check it** from another device on the tailnet:

```sh
make console FLATSAT_HOST=flatsat   # from a Codespace: the board's log lines
make reset   FLATSAT_HOST=flatsat   # the board reboots, and says so
```

## Reset controller (Adafruit QT Py)

`uhubctl` reports per-port power switching on this host's hubs but does not
actually drop VBUS on any of them except, oddly, an Apple keyboard hub. So
the rig resets the target by pulling NRST low rather than by cutting power.

An Adafruit QT Py M0 running CircuitPython does that job today. It is a
stand-in for the student-built flatsat controller board, and it speaks the
command vocabulary from the controller specification, so `tools/flatsat.py`
will not change when the real board replaces it.

Firmware lives in `flatsat/` in this repository.

### Wiring

| QT Py | Nucleo-F446RE | Notes |
|-------|---------------|-------|
| A0    | `RESET` on CN6, the Arduino power header | Open-drain. Driven low to assert, high-Z to release. |
| GND   | `GND` on CN6  | Run a real wire. Do not rely on a shared ground through the host's USB. |

**Count by the silkscreen, not by the end of the connector.** CN6's first
hole carries no label, so counting from the edge puts you one pin off
`RESET` — and the failure is silent: a released line reads 3.3 V whether or
not anything is connected, because that is the STM32's own 40 kΩ internal
pull-up. Confirm with a continuity check between A0 and the pin before
trusting the harness.

NRST is bidirectional and open-drain. The controller never drives it high;
it switches the pin between output-low and input-high-Z. That way the
on-board ST-LINK can still reset the target independently, and a crashed or
unprogrammed controller leaves the target running rather than held down.

### Host setup

The serial ports are owned by `dialout`, and the ST-LINK's ports are
relabelled into `plugdev` by the openocd package's udev rules. Mentors who
need to talk to either directly need both:

```sh
sudo usermod -aG plugdev,dialout <your-login>
```

Log out and back in. `newgrp dialout` gets you a working shell immediately
if you do not want to wait.

Give the controller a name of its own, so neither ser2net nor anyone at a
terminal has to care which `ttyACM` number it landed on this boot:

```sh
sudo tee /etc/udev/rules.d/71-flatsat-nrst.rules <<'EOF'
# The flatsat reset controller: the QT Py's data port (-if02). Its REPL
# (-if00) is left alone, for mentors with tio. Keyed on the board's serial
# so a second QT Py on the bench cannot claim the name.
SUBSYSTEM=="tty", ATTRS{idVendor}=="239a", \
  ATTRS{serial}=="08D29EF75259545020312E39200103FF", \
  ENV{ID_USB_INTERFACE_NUM}=="02", \
  GROUP="dialout", MODE="0660", SYMLINK+="flatsat-nrst"
EOF
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=tty
ls -l /dev/flatsat-nrst          # -> ttyACM<something>; the number varies
```

Replace the serial with your board's: `udevadm info -q property -n
/dev/ttyACM<n> | grep ID_SERIAL_SHORT`, or read it off the REPL.

Deploying firmware needs the CIRCUITPY volume mounted somewhere predictable
without a password prompt:

```sh
sudo mkdir -p /mnt/circuitpy
echo 'LABEL=CIRCUITPY /mnt/circuitpy vfat noauto,user,umask=000,flush 0 0' \
    | sudo tee -a /etc/fstab
```

Then:

```sh
flatsat/deploy.sh
```

Press the QT Py's physical reset button afterwards if `boot.py` changed.
USB descriptors are only rebuilt on a hard reset, and `boot.py` is what
enables the second CDC port.

### Verifying

```sh
python3 flatsat/nrst_test.py            # auto-detects the data port
python3 flatsat/nrst_test.py --hold 10  # long enough to read on a DMM
```

Nine checks, exit 0 on success. With a meter on NRST, asserted reads 0 V
and released reads 3.3 V; on the target's console you should see the
firmware reboot.

### Two CDC ports

The SAMD21 has eight USB endpoint pairs, and CircuitPython spends them by
default on the console, the CIRCUITPY drive, HID and MIDI. A second CDC
function does not fit, and it fails quietly: `usb_cdc.enable()` returns
successfully and `usb_cdc.data` hands back a live object, but the port
never enumerates. `boot.py` disables HID and MIDI to make room.

Both ports should be present:

```sh
ls -l /dev/serial/by-id/ | grep -i adafruit
```

`-if00` is the REPL and console, `-if02` is the command port.

### Troubleshooting

**`cp: No space left on device`, or `df: Input/output error`.** The mount
has gone stale. CircuitPython re-enumerates on every file save, and if the
board comes back as a different device node the old mount survives as a
corpse that reports nonsense. `flatsat/deploy.sh` detects and clears this;
by hand it is `sudo umount -l /mnt/circuitpy` and then `mount
/mnt/circuitpy`. `sudo fsck.vfat -n /dev/disk/by-label/CIRCUITPY` will show
the dirty bit left behind.

**The volume really is full.** It is 47 KB. Any Mac that mounts it leaves
`._`-prefixed AppleDouble files of 4 KB each, plus `.fseventsd` and
`.Trashes`. `deploy.sh` removes them. On a Mac, `mdutil -i off
/Volumes/CIRCUITPY` stops Spotlight re-creating its index directory.

**Starting over.** From the REPL on `-if00`:

```python
>>> import storage
>>> storage.erase_filesystem()
```

Reformats and reboots. CircuitPython itself is in firmware and survives;
re-run `deploy.sh` and press reset.

### 3. Hardening the box

Anyone who can reach port 3333 can give OpenOCD commands through GDB
(`monitor ...`), and some of those read and write files on the box. Every
student's Codespace can reach it, and so can anyone who copies the
Codespaces key. So the services run with as little as they need, and the
box runs nothing else that listens.

Make each change below while you still have another way in (the box's own
keyboard, or a shell that's already open), and check `make flash`,
`make console` and `make debug` afterwards.

**Sandbox OpenOCD.** `sudo systemctl edit openocd-flatsat` opens an
editor for a drop-in file, which adds to the service rather than replacing
it. Put these lines between the two marker comments; the copy of the unit
below them is only for reference:

```ini
[Service]
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
# AF_NETLINK: libusb watches for the ST-LINK being plugged in.
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
```

OpenOCD can then reach the ST-LINK, and write nowhere.

**Sandbox ser2net.** Ubuntu's ser2net runs as root. First look at its unit
(`systemctl cat ser2net`) for the `ExecStart` line and any `PIDFile`. Then
`sudo systemctl edit ser2net`:

```ini
[Service]
# A throwaway user that exists only while the service runs. BOTH groups
# are needed and it is not obvious why: the openocd package's USB rules
# claim the ST-LINK and the FT232H and put them in plugdev, while the QT
# Py matches no such rule and keeps Ubuntu's default dialout. Drop either
# group and one of the three ports stops opening.
DynamicUser=yes
SupplementaryGroups=dialout plugdev
ProtectHome=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
# Serial lock files.
ReadWritePaths=/run/lock
# A non-root user can't write /run/ser2net.pid; give it its own directory.
RuntimeDirectory=ser2net
ExecStart=
ExecStart=/usr/sbin/ser2net -n -c /etc/ser2net.yaml -P /run/ser2net/ser2net.pid
```

Copy `ExecStart` from the unit, changing only the `-P` path; the empty
`ExecStart=` first clears the old one. If the unit has a `PIDFile=`, add
`PIDFile=/run/ser2net/ser2net.pid` too. Before restarting, delete any lock
files the root-run ser2net left behind:

```sh
sudo systemctl stop ser2net
sudo rm -f /run/lock/LCK..ttyACM* /run/lock/LCK..ttyUSB*
sudo systemctl start ser2net
ps -o user=,cmd= -C ser2net      # a dynamic user, not root
```

`systemd-analyze security openocd-flatsat` (or `ser2net`) shows how much
each sandbox closes off. `sudo systemctl revert <service>` undoes one.

**Logins.** Mentors use Tailscale SSH (part 1), so remove the OpenSSH
server, if it's installed. First check you're logged in through Tailscale
SSH (`pstree -s $$` shows `tailscaled`, not `sshd`):

```sh
sudo apt purge openssh-server
sudo ufw status numbered          # delete any OpenSSH or 22/tcp rule not on tailscale0
getent group sudo                 # only mentors
getent passwd flatsat             # ends in /usr/sbin/nologin
```

Lock accounts nobody uses with `sudo passwd -l <name>`.

**Nothing else listening.** On an Ubuntu Desktop install, turn off what a
flatsat box doesn't need: network discovery (avahi), printing (cups),
ModemManager, remote desktop, and Bluetooth. ModemManager matters for the
board, not just security: it sends modem commands to every new serial port,
the Nucleo's and the FT232H's included.

```sh
sudo systemctl disable --now avahi-daemon.service avahi-daemon.socket \
    cups.service cups.socket ModemManager gnome-remote-desktop bluetooth
sudo systemctl mask avahi-daemon.service avahi-daemon.socket \
    cups.service cups.socket ModemManager gnome-remote-desktop
```

Masking stops other packages from starting them again. Don't
`apt purge avahi-daemon cups` on a desktop install without checking first
(`apt-get -s purge avahi-daemon cups | grep '^Remv'`): it can take the
`ubuntu-desktop` package with it, and `apt autoremove` then much of the
desktop. In Settings, also turn off Remote Desktop and automatic login,
and set the screen to lock after a few minutes.

**Check what listens:**

```sh
sudo ss -tlnp     # TCP: 3333, 4000, 4001, and local-only DNS
sudo ss -ulnp     # UDP: tailscaled's 41641, and local-only DNS
```

Then, from a computer on the local network with Tailscale off, each of
`nc -zv <box's LAN address> 3333` (and 4000, 4001, 22) should fail.
`ip -br addr` on the box shows its LAN address.

**Updates.** Let `unattended-upgrades` install security fixes, but not
reboot by itself: a reboot that cuts USB power starts the Nucleo over in
deploy mode, with its 45-minute wait (DS-42). In
`/etc/apt/apt.conf.d/50unattended-upgrades`, keep
`Unattended-Upgrade::Automatic-Reboot "false";`, and reboot by hand when
someone can hold B1. In the BIOS, set the box to restore its previous
power state after a power cut (it's always left on, so it comes back on),
and not to stop at boot for a setup prompt or a key press (for example,
"press F1 to continue" when no keyboard is plugged in): no one is there to
answer it.

**Close GDB sessions nobody is using.** OpenOCD takes one GDB connection
at a time and never times one out. A student who walks away, or whose
Codespace stops, keeps everyone else, CI included, off the board; if the
board was halted, OpenOCD doesn't even notice the other end has gone.
`tools/flatsat_gdb_reaper.py` closes each GDB connection that has sent
nothing for an hour, using `ss -K`; OpenOCD's `gdb-detach` event then
starts the board again. `ss -K` needs a kernel built with it:
`grep INET_DIAG_DESTROY /boot/config-$(uname -r)` should say `=y` (Ubuntu's
does). Install it from this repository:

```sh
sudo curl -fsSL -o /usr/local/sbin/flatsat-gdb-reaper \
    https://raw.githubusercontent.com/silver-sat/silversat-fsw/main/tools/flatsat_gdb_reaper.py
sudo chmod 755 /usr/local/sbin/flatsat-gdb-reaper
```

`/etc/systemd/system/flatsat-gdb-reaper.service`:

```ini
[Unit]
Description=Close flatsat GDB sessions idle for an hour

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/flatsat-gdb-reaper --idle-minutes 60
# Root, but only able to close connections (ss -K, over netlink).
CapabilityBoundingSet=CAP_NET_ADMIN
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_NETLINK AF_UNIX
```

`/etc/systemd/system/flatsat-gdb-reaper.timer`:

```ini
[Unit]
Description=Check for idle flatsat GDB sessions every 5 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min

[Install]
WantedBy=timers.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now flatsat-gdb-reaper.timer
systemctl list-timers flatsat-gdb-reaper     # next run within 5 minutes
```

To check it, attach `make debug` from a Codespace, then on the box:

```sh
sudo flatsat-gdb-reaper --idle-minutes 0 --dry-run   # "would close ... from <Codespace>"
sudo systemctl start flatsat-gdb-reaper              # a real run: nothing yet, not idle
journalctl -u flatsat-gdb-reaper -n 5                # closed sessions are listed here
```

### 4. GitHub

In the repository's settings (or the organization's, for every repository):

- **Codespaces secret** `TS_AUTH_KEY`: the auth key from part 1. Each
  Codespace joins the tailnet as it starts, through `tailscale-up.sh`; a
  running one joins after a restart, or when you run the script. (The
  Tailscale feature can't join by itself: it looks for the key before
  Codespaces has added its secrets.)

The three Actions variables below go under **Repository variables**
(Settings → Secrets and variables → Actions → Variables), not Environment
variables: the CI job names no environment, so it can't see those.

- **Actions variables** `TS_CLIENT_ID` and `TS_AUDIENCE`: the federated
  identity's Client ID and Audience from part 1, for CI. They are variables,
  not secrets: they prove nothing on their own, because only this
  repository's workflows can get the GitHub token they are checked against.
- **Actions variable** `FLATSAT_ENABLED` set to `true`, once the box is
  working, to turn on CI's hardware check.

## CI's hardware check

The `flatsat` workflow (`.github/workflows/flatsat.yml`) builds the
firmware, loads it onto the flatsat, and watches the console for 45
seconds. It passes if the board starts (`SilverSat FSW starting`) and
health's status line appears, with no fault and no stack warning (DS-05).

- **When it runs:** on every merge to main, nightly, and on demand. Pull
  requests don't run it, so it never blocks one.
- **On demand:** Actions → flatsat → Run workflow, and pick your branch, or
  `gh workflow run flatsat --ref <branch>`. The run's `flatsat-logs`
  artifact holds the console.
- **One at a time:** runs wait for the board rather than cancel each other.
  GitHub keeps only one run waiting, though: a third cancels the one
  already waiting.
- **When main fails:** the run fails, so GitHub emails whoever started it
  (for the nightly run, whoever last changed its schedule). The `report`
  job also opens an issue labelled `flatsat` with the last console lines,
  comments on it while main keeps failing, and closes it when main passes
  again. To hear about it, watch the repository's issues (Watch → Custom →
  Issues).

## When something's wrong

At the box, the Nucleo's LD1 (the red and green LED by the ST-LINK's USB
connector) gives a quick first look:

| LD1 | Meaning |
|---|---|
| Flashing red and green | OpenOCD is talking to the chip (it polls all the time): normal |
| Steady red | The ST-LINK is plugged in, but OpenOCD isn't talking to the chip: it isn't running, or has lost the ST-LINK (see `SRST error` below). Normal only for a few seconds after plugging in. |
| Blinking red | The ST-LINK is starting up over USB |
| Orange | The ST-LINK's last exchange with the chip failed |

The red power LED (LD3) is always on; the green user LED (LD2) stays off,
because the firmware doesn't use it.

- **`can't find 'flatsat'`:** the Codespace isn't on the tailnet. Check
  `tailscale status`. If it says `Logged out`, run
  `bash .devcontainer/tailscale-up.sh`. If that says there's no
  `TS_AUTH_KEY`, the secret was added after the Codespace was created, or
  the Codespace's repository isn't allowed to use it; fix the secret, then
  restart the Codespace. If `tailscale up` fails, the key may have expired:
  make a new one (part 1) and update the secret.
- **`Machine is not yet approved by tailnet admin`** from
  `tailscale status`, or the Codespace's start says it couldn't join: the
  Codespaces key isn't pre-approved, and device approval is on. Approve the
  Codespace on the admin console's Machines page, and it joins within a
  few seconds. To stop it happening to every Codespace, make a new key
  with "Pre-approved" ticked (part 1, item 3), and update the secret.
  The start gives up waiting after 60 seconds, so it never hangs.
- **`flatsat:3333 doesn't answer`:** OpenOCD isn't running on the box:
  `systemctl status openocd-flatsat`. It doesn't run while the Nucleo is
  unplugged; plug it in, and check `systemctl status dev-stlink.device`
  says `plugged`. If it's plugged and OpenOCD still isn't running,
  `journalctl -u openocd-flatsat -n 40` says why.
- **`SRST error`, `read_memory ... failed`, or `Error erasing flash`** from
  `make flash` or CI, and `monitor targets` shows `running` even after
  attaching: OpenOCD has lost its ST-LINK, usually because the Nucleo was
  unplugged while OpenOCD carried on. The udev rule and `BindsTo` above
  prevent this; without them, `sudo systemctl restart openocd-flatsat`.
- **`Remote connection closed` or `Connection reset by peer`** from
  `make debug` or `make flash`: the box answers, but OpenOCD refused GDB.
  On the box, `journalctl -u openocd-flatsat -n 40`. `examination failed`
  or `Target not examined yet` means OpenOCD couldn't reach the chip when it
  started: check the service has the `reset_config` line above, then
  `sudo systemctl restart openocd-flatsat`. `no more connections allowed`
  means someone else's GDB is attached; OpenOCD takes one at a time.
  If it comes in the middle of a session, at the first command after a
  break, the box closed the session because GDB had sent nothing for an
  hour; the board is running, so `quit` and `make debug` again. An
  unused session closes by itself within an hour and five minutes. To end
  it now, a mentor on the box finds it with
  `sudo ss -tn '( sport = :3333 )'` (`tailscale status` names the address),
  then closes it with `sudo flatsat-gdb-reaper --idle-minutes 0`, which
  closes every GDB session, or with
  `sudo ss -K '( sport = :3333 and dst <address>:<port> )'` for just one.
- **Console and telemetry both silent:** the board isn't running. In
  `make debug`, `monitor targets` shows its state:
  - `halted`: usually a GDB session that ended while it was stopped. Check
    the OpenOCD service has the `gdb-detach` line above.
  - `reset`: OpenOCD is holding it in reset. That happens after
    `openocd-flatsat` restarts if the service lacks the `init` and
    `reset run` at the end of its `ExecStart`.

  Either way, `monitor reset run` then `quit` starts it (or `make flash`).
  A reset keeps the backup SRAM, so the board doesn't go back to deploy
  mode. (Deploy mode isn't silent: the console still shows `alive` every
  10 s.)
- **`Device open failure: Permission denied`**: ser2net can't open that
  serial port. Check what the running service actually has, not what the
  drop-in was meant to say — the two can differ, and then one port works
  while another does not:

  ```sh
  ls -lL /dev/serial/by-id/            # each device's group
  getent group dialout plugdev         # dialout is 20, plugdev 46
  pid=$(systemctl show -p MainPID --value ser2net)
  grep ^Groups /proc/$pid/status       # want 20 and 46, plus its own
  ```

  The third GID is the transient group `DynamicUser=yes` allocates for the
  service itself; it changes on every start and is not a problem. If 20 or
  46 is missing, fix `SupplementaryGroups` in the drop-in and restart.
  Also check `/run/lock` holds no `LCK..tty*` owned by root or by a user
  who opened the port by hand with `tio`.

  Do not debug this by `ttyACM` number. They are assigned in enumeration
  order and move: the QT Py and the ST-LINK have traded places here more
  than once. Go by `/dev/serial/by-id/` or `/dev/flatsat-nrst`.
- **`mdns: Can't allocate avahi client`** and **`Unable to start mdns: Out
  of memory`** when ser2net starts: harmless. ser2net tries Avahi once at
  start, finds it missing (or blocked by the sandbox), and carries on;
  `mdns: false` means it wouldn't announce anything anyway.
- **The console shows nothing, but the board runs** (telemetry works, or
  `make debug` shows it running): first check `systemctl status ser2net`,
  and that no one else's `make console` has taken the port. Then read the
  port directly on the box, with ser2net out of the way:

  ```sh
  sudo systemctl stop ser2net
  P=$(ls /dev/serial/by-id/*STLink*)
  sudo stty -F "$P" 115200 raw -echo
  sudo timeout 15 cat "$P"          # expect "alive" lines
  sudo systemctl start ser2net
  ```

  If nothing comes, the ST-LINK's serial port has stuck. That happened
  with the old ST-LINK firmware (V2J33) after the box rebooted while the
  Nucleo kept power. `usbreset` doesn't clear it; unplugging the Nucleo
  does (hold B1 as you plug it back in, to skip deploy mode's wait). Then
  update the ST-LINK firmware (part 2).
- **No telemetry, console fine:** look for the mode in health's status
  line. `mode deploy` sends no telemetry by design (DS-42) until MET
  reaches the separation delay (45 minutes, `MET 2700 s`); then the board
  enters safe mode, stores that, and sends telemetry every second. After a
  power cycle the board starts again from deploy mode, and a test-mode
  boot (B1) lasts only until the next reset, a CI run or `make flash`
  included. So after a power cycle, leave the board running until MET
  passes 2700 s; MET carries on across resets, so the wait isn't
  restarted.
- **`dkms autoinstall ... failed for evdi`** during an upgrade: the
  DisplayLink driver doesn't build for the new kernel, and leaves the
  upgrade half-done. If no DisplayLink dock or screen is plugged in
  (`lsusb | grep -i displaylink`), remove it:
  `sudo dkms remove evdi/<version> --all`, `sudo apt purge evdi-dkms
  displaylink-driver` (whichever are installed), then
  `sudo dpkg --configure -a`.
- **Repeated boot banners:** the watchdog is resetting the board. See "What
  to know" in docs/nucleo-on-your-desk.md.
