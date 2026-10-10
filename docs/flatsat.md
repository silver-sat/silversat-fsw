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
                                    └────────────────────────────────────────┘
```

- **OpenOCD** is the GDB server for the Nucleo's on-board ST-LINK: `make
  flash` loads firmware through it, and `make debug` debugs through it.
- **ser2net** puts the board's two serial ports on TCP ports: the console
  (the ST-LINK's serial port, USART2) and the radio UART (USART1, through an
  FT232H). `make console` reads the first; the radio simulator talks to the
  second.
- **Tailscale** connects them. Codespaces and CI can reach only those three
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
each merge to main, nightly, and when someone runs it on demand (below).
Close your `make debug` when you finish: while it's attached, OpenOCD
refuses everyone else, CI included.

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
       // Codespaces and CI reach only the flatsat's GDB and serial ports.
       {"action": "accept", "src": ["tag:codespace", "tag:ci"],
        "dst": ["tag:flatsat:3333,4000,4001"]},
       // Mentors reach everything, to look after the box.
       {"action": "accept", "src": ["autogroup:admin"], "dst": ["*:*"]},
     ],
     "ssh": [
       // Mentors log in to the box with Tailscale SSH: no keys to manage.
       // "check" makes them sign in to Tailscale again (at most every 12
       // hours), so an unattended laptop isn't enough. No root: each mentor
       // logs in as themselves and uses sudo, which records who did what.
       {"action": "check", "src": ["autogroup:admin"], "dst": ["tag:flatsat"],
        "users": ["autogroup:nonroot"], "checkPeriod": "12h"},
     ],
   }
   ```

   Each mentor needs an account on the box (`sudo adduser <name>`, and
   `sudo usermod -aG sudo <name>`). They log in from a computer on the
   tailnet with plain `ssh <name>@flatsat`, and open the link it prints to
   sign in again.

3. **An auth key for Codespaces** (Settings → Keys → Generate auth key):
   reusable, ephemeral (a Codespace's device disappears when it stops),
   pre-approved, tagged `tag:codespace`. Copy it for part 4 below. It
   expires after at most 90 days; note the date, because Codespaces stop
   joining when it does.
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
5. **Device approval** (Settings → Device management): turn it on, so a
   leaked key can't quietly add a device. The Codespaces key is
   pre-approved (item 3), so Codespaces still join without waiting. After
   turning it on, run the `flatsat` workflow once (below) to check that CI
   still joins too.

### 2. The box

Ubuntu 24.04 or later. Plug the Nucleo into it by its own USB connector,
and the FT232H by USB, wired to the Nucleo as in docs/nucleo-on-your-desk.md
(FT232H D0 to Nucleo D2, D1 to D8, GND to GND).

**Name it** `flatsat`, the name everything else uses:

```sh
sudo hostnamectl set-hostname flatsat
```

**Join the tailnet**, as the flatsat, with Tailscale SSH:

```sh
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

**OpenOCD as a service**, so its GDB server is always there and restarts if
the board is unplugged. Create `/etc/systemd/system/openocd-flatsat.service`:

```ini
[Unit]
Description=OpenOCD GDB server for the flatsat's Nucleo
After=network-online.target

[Service]
User=flatsat
ExecStart=/usr/bin/openocd -f board/st_nucleo_f4.cfg \
    -c "reset_config srst_only srst_nogate connect_assert_srst" \
    -c "stm32f4x.cpu configure -event gdb-detach { resume }" \
    -c "bindto 0.0.0.0" -c "gdb_port 3333" \
    -c "telnet_port disabled" -c "tcl_port disabled"
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

The `reset_config` line holds the board in reset while OpenOCD connects.
OpenOCD checks the chip only once, as it starts, and refuses GDB from then
on if that check failed; firmware that is asleep or stuck can make it fail.
It also means starting the service restarts the board.

The `gdb-detach` line starts the board again whenever GDB lets go of it.
Leaving GDB usually means pressing Ctrl-C, which halts the board, then
`quit`; without this line the board stays halted, and its console and
telemetry go silent as if it were dead.

```sh
sudo systemctl enable --now openocd-flatsat
systemctl status openocd-flatsat      # "Listening on port 3333 for gdb connections"
journalctl -u openocd-flatsat -n 20   # no "examination failed"
```

**ser2net**, for the two serial ports. Find their stable names:

```sh
ls /dev/serial/by-id/
# usb-STMicroelectronics_STM32_STLink_066B...-if02   the console
# usb-FTDI_..._-if00-port0                            the FT232H
```

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
```

```sh
sudo systemctl restart ser2net
```

`mdns: false` keeps ser2net from announcing the ports on the local network.

**Answer only on the tailnet.** OpenOCD and ser2net listen on every
network, so a firewall keeps those ports off the box's local network:

```sh
sudo ufw allow 41641/udp      # Tailscale's direct connections (else it relays, slowly)
sudo ufw allow in on tailscale0 to any port 22,3333,4000,4001 proto tcp
grep IPV6 /etc/default/ufw    # IPV6=yes, or the ports stay open over IPv6
sudo ufw enable
sudo ufw status verbose       # each rule listed for v4 and v6
```

Port 22 on the tailnet is for mentors' Tailscale SSH. The box needs no
other SSH server (see the next part).

**Check it** from another device on the tailnet:

```sh
make console FLATSAT=flatsat     # from a Codespace: the board's log lines
```

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
# A throwaway user that exists only while the service runs. The openocd
# package's USB rules put both serial ports in plugdev, not dialout.
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
someone can hold B1. In the BIOS, set the box to power on again after a
power cut.

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

- **`can't find 'flatsat'`:** the Codespace isn't on the tailnet. Check
  `tailscale status`. If it says `Logged out`, run
  `bash .devcontainer/tailscale-up.sh`. If that says there's no
  `TS_AUTH_KEY`, the secret was added after the Codespace was created, or
  the Codespace's repository isn't allowed to use it; fix the secret, then
  restart the Codespace. If `tailscale up` fails, the key may have expired:
  make a new one (part 1) and update the secret.
- **`flatsat:3333 doesn't answer`:** OpenOCD isn't running on the box:
  `systemctl status openocd-flatsat`. It stops retrying if the Nucleo is
  unplugged for long; replug it, or restart the service.
- **`Remote connection closed` or `Connection reset by peer`** from
  `make debug` or `make flash`: the box answers, but OpenOCD refused GDB.
  On the box, `journalctl -u openocd-flatsat -n 40`. `examination failed`
  or `Target not examined yet` means OpenOCD couldn't reach the chip when it
  started: check the service has the `reset_config` line above, then
  `sudo systemctl restart openocd-flatsat`. `no more connections allowed`
  means someone else's GDB is attached; OpenOCD takes one at a time.
- **Console and telemetry both silent:** the board isn't running. In
  `make debug`, `monitor targets` shows its state:
  - `halted`: usually a GDB session that ended while it was stopped. Check
    the OpenOCD service has the `gdb-detach` line above.
  - `reset`: OpenOCD is holding it in reset, which can happen after
    `openocd-flatsat` restarts.

  Either way, `monitor reset run` then `quit` starts it (or `make flash`).
  A reset keeps the backup SRAM, so the board doesn't go back to deploy
  mode. (Deploy mode isn't silent: the console still shows `alive` every
  10 s.)
- **`Device open failure: Permission denied`** on the console: ser2net
  can't open the serial port. Check the device's group
  (`ls -lL /dev/serial/by-id/`) is in the drop-in's `SupplementaryGroups`,
  and that `/run/lock` holds no `LCK..tty*` files owned by root.
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
