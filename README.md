<p align="center">
  <img src="logo.png" alt="FGG-XSense" width="820">
</p>

# FGG-XSense

**Use an Xbox controller on a jailbroken PlayStation 5.**

[![CI](https://github.com/FGGstore/FGG-XSense/actions/workflows/ci.yml/badge.svg)](https://github.com/FGGstore/FGG-XSense/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A PS5 ignores an Xbox controller completely. Plug one in and nothing happens —
no light, no input, not even a "unsupported device" message. The console
enumerates it and then leaves it alone.

FGG-XSense is a payload that picks it up and hands it to the system as an
ordinary PlayStation pad.

```
  Xbox pad ──USB──► FGG-XSense ──virtual pad──► games and system UI
```

Nothing in the system software is patched. Nothing is installed. When the
payload exits, the virtual pad is removed and the console is exactly as it was.

---

## Install

Grab `fgg-xsense.elf` from the [latest release](https://github.com/FGGstore/FGG-XSense/releases/latest).

**With Payload Manager** — copy `fgg-xsense.elf` and `fgg-xsense.elf.json` over
FTP into:

```
/data/pldmgr/payloads/fgg-xsense/
```

then launch **fgg xsense** from the Payload Manager page.

**Without it** — send `fgg-xsense.elf` to port **9021** with any payload sender.

## Use it

1. Launch the payload (or let Payload Manager autoload it)
2. Plug the controller into the PS5 over USB

The guide LED lights and the controller works. That is the whole workflow.

The payload stays resident: unplug the controller and plug it back in, or
plug in another one, and it is picked up again within a few seconds. Up to
four controllers are served at once, each as its own pad.

To stop it: **hold View + Menu together for two seconds** on any controller,
or create the file `/data/fgg-xsense/stop` over FTP.

Your DualSense keeps working throughout — this adds a pad, it does not replace
yours.

## Wireless, with the Xbox Wireless Adapter

Plug Microsoft's **Xbox Wireless Adapter for Windows** into the PS5 and
controllers connect to it wirelessly, just as on a PC. Tested with the original
adapter (`045E:02E6`) and an Xbox Series X|S controller on firmware 13.60; the
slim adapter (`02FE`) and the built-in laptop ones (`02F9`, `091E`) use the
same code with their own firmware.

**The adapter needs its firmware**, which is Microsoft's and cannot be shipped
here. Fetch it once from Windows Update with the script below, on any Mac or
Linux machine on the same network as the console — it checks the download
against the checksum the Linux drivers use and copies it over FTP:

```sh
tools/fetch_firmware.sh 02e6 <console-ip> 2121    # your adapter's product ID
```

It lands in `/data/fgg-xsense/firmware/`. The log names your adapter's ID if
you are not sure which it is.

**Pairing** works as on Windows: when the adapter's radio comes up, pairing is
open for 60 seconds and its LED blinks. Hold the pair button on the controller
until the Xbox logo flashes fast. To open pairing again later, press the button
on the adapter, or create `/data/fgg-xsense/pair` over FTP. A controller paired
once reconnects by itself whenever it is switched on.

Only controllers that paired while the window was open may connect; they are
remembered in `/data/fgg-xsense/controllers.txt`. Delete the file to forget
them all.

A controller remembers one Xbox host and one Bluetooth host. Using it with
another adapter or console means pairing again, and plugging it in over USB
switches it to the cable until it is unplugged.

**Knock-off adapters** work too. The log notes it if your adapter's address is
one shared by known clones (`62:45:b4:e7:a4:ef`); that only matters if two such
adapters are in range of each other.

If the adapter ever stops responding — the log says the firmware did not start
— unplug it and plug it back in; the payload starts it afresh.

---

## Mapping

| Xbox | PlayStation |
|---|---|
| A | ✗ Cross |
| B | ○ Circle |
| X | □ Square |
| Y | △ Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 — analogue, plus the digital bit |
| Left stick click | L3 |
| Right stick click | R3 |
| Menu | Options |
| View | Touchpad click |
| D-pad | D-pad |
| Left / right stick | Left / right stick |
| **Guide (Xbox button)** | **PS button — experimental; see below** |

Sticks are full 16-bit range. The Y axes are inverted on the way through,
because GIP counts up as positive and PlayStation counts down.

## The PS button

The normal kernel virtual-pad API cannot synthesise it: the PS button is
intercepted above that layer. An **experimental** route instead creates a
DualSense through Sony's `libScePad` and translates the Xbox Guide bit into its
PS bit.

Enable it only for this test by adding this to `/data/fgg-xsense/config.txt`:

```ini
ps_button=1
```

When the console asks who is using the newly created controller, FGG-XSense
asks SceShellUI itself to press that controller's PS button, then leaves account
selection to you. Choose your account with the Xbox controller. This uses a
short-lived ptrace call into SceShellUI and is therefore intentionally opt-in.
If the assignment screen does not appear or controller input remains busy, stop
the payload, remove `ps_button=1`, and use the DualSense for PS instead.

---

## Supported controllers

Anything that speaks **GIP**, the Xbox wire protocol: Xbox One, Xbox Series,
and the many third-party pads built to the same standard.

Devices are matched on the GIP interface signature — class `FF`, subclass `47`,
protocol `D0` — rather than on a list of vendor IDs, so a controller nobody has
tested is still recognised. Nothing that fails to match is opened, configured or
claimed.

Developed and verified against a GameSir pad (`3537:1010`) on firmware
**11.60**. Also verified with an official Microsoft Xbox Series X|S controller
(`045E:0B12`) on firmware **13.60** (kstuff lite 1.11).

> Xbox 360 controllers use an older, different protocol and are **not**
> supported.

---

## How it works

Two halves, both worked out by probing a console rather than by guessing.

**Reading the controller.** The PS5 enumerates a GIP pad but never configures
it, and the per-endpoint `/dev/ugenX.Y.Z` nodes do not exist on this system, so
transfers go through the `USB_FS_*` ioctls. The device needs a power-on message
before it streams input, an authentication message if it is not made by
Microsoft — without it a third-party pad powers itself down after a few seconds
— and a third message to light the guide LED.

**Publishing a virtual pad.** `libScePad`'s virtual-device API turns out to be a
thin wrapper over two ioctls on `/dev/hid`, and neither checks who is calling:

```
AddDevice     ioctl(fd, 0xC018482A, &req)
InsertData    ioctl(fd, 0x8018482C, &req)
DeleteDevice  ioctl(fd, 0x80104850, &req)
```

So a payload can register a controller without patching SceShellCore or
anything else. All eleven device types the kernel recognises were accepted.

**The wireless adapter.** Microsoft's adapter is a MediaTek MT76 radio running
Microsoft firmware that the host uploads on every power-up. The payload drives
it the way the reverse-engineered Linux drivers do — register access through
vendor control requests, firmware and commands on a bulk endpoint, radio frames
on another — and the controllers on it speak the same GIP as over a cable,
inside 802.11 frames. So the wired code's protocol handling serves both.

Two things here differ from the Linux drivers, both found on hardware with a
`02E6` adapter. A GIP acknowledgement must carry its own options byte, not the
acknowledged message's, or a wireless controller keeps resending. And the
adapter cannot be fully reset by software once its radio has run: restarting
its firmware in place leaves the radio unreliable, so a payload restart
mid-session asks for a replug. After a jailbreak the adapter has just powered
up and starts cleanly.

**One trap worth recording.** The report the kernel reads is *not* `ScePadData`.
`libScePad` translates its caller's `ScePadData` into a differently shaped
buffer first — buttons land at `+0x0C`, sticks at `+0x10`. Passing a
`ScePadData` straight through looks like it works, because the ioctl validates
only its 24-byte header and never the contents, while the kernel reads the
buttons out of what was actually the orientation quaternion. A pad appears, the
system reacts to it, and not one button does anything.

## Reliability

Controllers stall. The payload expects it and recovers rather than giving up:

- unanswered GIP messages are acknowledged, so the controller keeps talking
- the initialisation is resent every 20 seconds while everything is healthy
- three seconds of silence triggers a re-initialisation
- three fruitless re-initialisations rebuild the USB connection from scratch
- only 30 seconds of genuine silence is treated as an unplug

Measured on console: **110,000 input reports delivered over 25 minutes**
without intervention.

Only one copy runs at a time. Launching it twice is easy to do by accident, and
two copies is not merely wasteful — both claim the same USB interface and each
consumes the other's transfer completions, which looks exactly like failing
hardware.

## Building

Needs Linux or WSL and the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk):

```sh
sudo apt install clang lld llvm make unzip    # llvm is required, and easy to miss
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

Builds at `-Wall -Wextra -Werror`. No dependencies beyond the SDK, and nothing
is fetched at build time. `third_party/xone/mt76_defs.h` is vendored unchanged
from [dlundqvist/xone](https://github.com/dlundqvist/xone) (GPL-2.0-or-later);
see `third_party/xone/README.md`.

```sh
make check    # host tests: GIP decoding, translation, SHA-256, firmware parsing
make probe    # probe.elf: a read-only survey of the console's USB devices
```

Settings, all optional, go in `/data/fgg-xsense/config.txt` as `key=value`:

- `idle_off_minutes=N` switches a wireless controller off after N minutes
  untouched (default 15; `0` never does). A cabled one stays on.
- `adapter_channel=N` puts the adapter's radio on channel N instead of 36.
- `ps_button=1` enables the experimental Xbox Guide-to-PS route described above.
- `mirror=1` publishes every controller twice (for testing several pads).
- `trace_gip=1` logs each controller's first messages and button changes,
  `trace_mt76=1` every register access to the wireless adapter, and
  `trace_klog=1` the system log read while the PS-button route sets up a pad.

## Troubleshooting

**Nothing happens when I launch it.** Check `/data/fgg-xsense/xsense.log` over
FTP — every run is logged there, including why it gave up. Each run starts it
afresh; the run before is kept as `xsense.old.log`.

**"another copy is already running".** Hold View+Menu for two seconds to stop
the running one, or reboot if it will not respond.

**"adapter found, firmware missing".** Run `tools/fetch_firmware.sh` for your
adapter's product ID (the log names it).

**A wireless controller will not connect.** Open pairing (adapter button, or
create `/data/fgg-xsense/pair`) and hold the controller's pair button. The log
shows every pairing request and connection.

**The controller light never comes on.** Then it is not being recognised as a
GIP device. The log names every controller it finds; if yours is not listed, it
does not present the GIP interface signature.

## Licence

[GPL-3.0](LICENSE).

## Credits

Built by **FGG STORE**.

Standing on the work of [ps5-payload-dev](https://github.com/ps5-payload-dev)
for the SDK and `ftpsrv`, and on the Linux `xpad` driver, whose initialisation
packets documented what a third-party GIP controller needs in order to stay
awake.

Wireless support is a port of the reverse-engineered Linux drivers for the
Xbox Wireless Adapter by Severin von Wnuck-Lipinski —
[xow](https://github.com/medusalix/xow) and
[xone](https://github.com/medusalix/xone), with the fixes of
[dlundqvist/xone](https://github.com/dlundqvist/xone) — and on the
[mt76](https://github.com/openwrt/mt76) driver by Felix Fietkau, Lorenzo
Bianconi and Stanislaw Gruszka.

> Not affiliated with Sony Interactive Entertainment or Microsoft. For use with
> homebrew on consoles you own.
