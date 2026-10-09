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

A Codespace joins the tailnet on its own when it starts, if the
`TS_AUTH_KEY` Codespaces secret is set (see "Setting it up"). Check:

```sh
tailscale status            # flatsat should be listed
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
for example in the team chat, before you flash it. CI flashes it too, when
it runs its hardware check (below).

### Things that still need hands

Until the box has a USB power switch and a relay on the Nucleo's reset pin
(DS-04), someone at the box:

- **Holds the blue button (B1) through a reset** to boot test mode, or
- **Unplugs the board** to clear the backup SRAM (boot 1, deploy mode).

`make flash` resets the board itself, so ordinary loading needs no one.

## Setting it up

Once, by a mentor. Three parts: the tailnet, the box, and GitHub.

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
       {"action": "accept", "src": ["autogroup:admin"], "dst": ["tag:flatsat"],
        "users": ["autogroup:nonroot", "root"]},
     ],
   }
   ```

3. **An auth key for Codespaces** (Settings → Keys → Generate auth key):
   reusable, ephemeral (a Codespace's device disappears when it stops),
   pre-approved, tagged `tag:codespace`. Copy it for step 3 below.
4. **A federated identity for CI** (workload identity federation: GitHub
   vouches for the workflow, so there is no secret to store or rotate). On
   the Trust credentials page, choose Credential → OpenID Connect:
   - Issuer: **GitHub Actions**.
   - Subject: `repo:silver-sat/silversat-fsw:*`, which matches this
     repository's own workflow runs, for pull requests and on main.
   - Scope: `auth_keys` (write), with tag `tag:ci`. The policy above must
     already define `tag:ci`, or the page won't offer it.

   Generate it, and copy its **Client ID** and **Audience**. Neither is a
   secret.

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
```

**Install OpenOCD and ser2net:**

```sh
sudo apt install openocd ser2net
```

The openocd package installs the USB permissions for the ST-LINK.

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
    -c "bindto 0.0.0.0" -c "gdb_port 3333" \
    -c "telnet_port disabled" -c "tcl_port disabled"
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now openocd-flatsat
systemctl status openocd-flatsat      # "Listening on port 3333 for gdb connections"
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
    connector: serialdev,/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_066B...-if02,115200n81,local

# The radio UART (USART1), through the FT232H.
connection: &radio
    accepter: tcp,4001
    enable: on
    options:
      kickolduser: true
    connector: serialdev,/dev/serial/by-id/usb-FTDI_..._-if00-port0,19200n81,local
```

```sh
sudo systemctl restart ser2net
```

**Answer only on the tailnet.** OpenOCD and ser2net listen on every
network, so a firewall keeps those ports off the box's local network:

```sh
sudo ufw allow OpenSSH                                   # keep local logins
sudo ufw allow in on tailscale0 to any port 3333,4000,4001 proto tcp
sudo ufw enable
```

**Check it** from another device on the tailnet:

```sh
make console FLATSAT=flatsat     # from a Codespace: the board's log lines
```

### 3. GitHub

In the repository's settings (or the organization's, for every repository):

- **Codespaces secret** `TS_AUTH_KEY`: the auth key from step 1. New
  Codespaces join the tailnet as they start; a running one joins after a
  rebuild or restart.
- **Actions variables** `TS_CLIENT_ID` and `TS_AUDIENCE`: the federated
  identity's Client ID and Audience from step 1, for CI. They are variables,
  not secrets: they prove nothing on their own, because only this
  repository's workflows can get the GitHub token they are checked against.
- **Actions variable** `FLATSAT_ENABLED` set to `true`, once the box is
  working, to turn on CI's hardware check.

## CI's hardware check

The `flatsat (HIL)` job loads the firmware built for the pull request onto
the flatsat and watches the console for 45 seconds. It passes if the board
starts (`SilverSat FSW starting`) and health's status line appears, with no
fault and no stack warning. It runs only from this repository's own
branches, never from forks, one run at a time, and it doesn't block merging
until it has run cleanly for a while (DS-05).

## When something's wrong

- **`can't find 'flatsat'`:** the Codespace isn't on the tailnet. Check
  `tailscale status`; if `TS_AUTH_KEY` was added after the Codespace was
  created, rebuild it.
- **`flatsat:3333 doesn't answer`:** OpenOCD isn't running on the box:
  `systemctl status openocd-flatsat`. It stops retrying if the Nucleo is
  unplugged for long; replug it, or restart the service.
- **The console shows nothing:** the board may be in deploy mode, which
  prints little after boot; `make flash` again to watch a boot. Check
  `systemctl status ser2net`.
- **Repeated boot banners:** the watchdog is resetting the board. See "What
  to know" in docs/nucleo-on-your-desk.md.
