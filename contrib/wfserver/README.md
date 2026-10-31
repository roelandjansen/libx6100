# wfserver on the X6100

wfview's `wfserver` runs on the radio and serves it over the network the Icom LAN way
(UDP 50001-50003). wfview on a PC then connects to the X6100 as it would to a networked
IC-705: CI-V control, scope, RX audio and TX audio. x6100_trx stays the only owner of the
baseband; wfserver is a CI-V client of it over the CI-V pty (`/tmp/x6100-civ`, always served) and moves the audio through the
R16 codec (ALSA card 0).

```
wfview (PC) --UDP 50001-50003--> wfserver (radio) --/tmp/x6100-civ--> x6100_trx --> baseband
                                    |  ALSA card 0 capture  <-- RX audio
                                    |  ALSA card 0 playback --> codec TX (mod input USB)
```

## Build

`build.sh` cross-builds wfserver for the R16 (armhf, Cortex-A7 NEON) on an Ubuntu 24.04
x86-64 host and bundles it with all its libraries and its own glibc loader, so it does
not depend on anything in the radio's image (which has no Qt).

Why not zig, as for the rest of libx6100: wfserver is C++ against the prebuilt Qt 5.15
from Ubuntu's armhf packages, which is built with GCC's libstdc++; `zig c++` uses libc++
and cannot link against it safely. So the build uses Ubuntu's cross g++.

On any other host (openSUSE), run it in a container, as a normal user:

```
git clone https://gitlab.com/eliggett/wfview.git
contrib/wfserver/container.sh <wfview source tree>
```

- Engine: podman (rootless, no daemon: `zypper install podman`), else docker (needs
  access to the docker socket); `X6100_ENGINE=docker` picks one.
- The first run starts from `ubuntu:24.04`, installs the cross tools and keeps that as a
  local image `x6100-wfserver-build` (a few minutes plus the package downloads). Later
  runs use the image; with nothing changed a run takes seconds.
- Works in `<repo>/out`: sysroot and objects in `out/wfserver-work`, the bundle in
  `out/wfserver-armhf.tar.gz`, where `build.sh deploy` picks it up. Files end up owned
  by the caller (with docker they are handed back at the end, also after a failure).
- Remove the image to start afresh: `podman rmi x6100-wfserver-build`.

On an Ubuntu 24.04 host directly:

```
cd <empty work dir, e.g. <repo>/out>
build.sh <wfview source tree> [output dir]
```

- Runs as root (apt): it adds the armhf architecture with ports.ubuntu.com as its source,
  installs the cross g++ and host Qt tools, and unpacks the armhf Qt 5.15 / Opus /
  PortAudio / RtAudio packages into a sysroot under `wfserver-work/`.
- Output: `wfserver-armhf/` and `wfserver-armhf.tar.gz` (about 33 MB, 68 MB unpacked).
- The bundle contains `X6100.rig` from `contrib/wfview` next to wfview's own rig files.
- A second run reuses the sysroot and the object files.

Built and checked 2026-10-28 from wfview `v3-initial` (wfserver 2.11): the ARM binary
starts under qemu-user with every library from the bundle, loads the settings and finds
all rig files including X6100 (61). Not run on the radio yet.

## Install on the radio

With `build.sh deploy` (repository root): it installs the bundle when
`out/wfserver-armhf.tar.gz` exists (or `$X6100_WFSERVER` names it), so build it there:

```
cd <libx6100>/out
../contrib/wfserver/build.sh <wfview source tree>
cd .. && bash build.sh deploy
```

deploy compares the bundle's sha256 with `/opt/wfserver/.bundle` on the radio and only
sends it when it differs, with a progress line (`pv` when installed, else `dd`): unpacked
from gzip on the PC and streamed as tar over `ssh -C`
(the image's busybox tar has no gzip). It unpacks next to the old bundle and swaps the
directories; a failed unpack leaves the old one in place. A running wfserver carries on
with the old files until it is restarted. `/etc/x6100/wfserver.ini` is put in from the
example only when the radio has none (a backed-up one is restored first).

By hand (the image's busybox tar has no `-z`: unpack the gzip on the PC, stream the tar):

```
gzip -dc wfserver-armhf.tar.gz | ssh -C root@<radio> 'mkdir -p /opt && cd /opt && tar -x && mv wfserver-armhf wfserver'
ssh root@<radio> 'cp /opt/wfserver/wfserver.ini.example /etc/x6100/wfserver.ini'
```

The bundle expects to live in `/opt/wfserver` (the rig files are looked up in
`/opt/wfserver/share/wfview/rigs`). Start it with the launcher, never `bin/wfserver`
directly: `wfserver.sh` runs it with the bundled loader and libraries and points Qt and
ALSA at the bundled plugin and configuration.

## Set up x6100_trx

On the GEN panel:

| Row | Value | Why |
|---|---|---|
| CAT CI-V | any | the pty `/tmp/x6100-civ` is served next to the DEV port since 2026-10-31; the row only sets the DEV port |
| CI-V ADDRESS | X6100 61 | matches `RigCIVuInt=97` in the ini; IC-705 A4 works too (then 164) |
| DATA MOD | USB | TX audio in USB-D / LSB-D from the network (the default) |
| DATA OFF MOD | USB for voice over the network, MIC for the radio's own microphone | |

With the input on USB, a PTT from wfview (or the PTT key) keys with codec TX: the audio
wfserver plays into ALSA card 0 is the modulation. The hand microphone's PTT always uses
the microphone. x6100_trx sets up the codec mixer (capture and playback routing) as soon
as either input is USB.

## Set up wfserver

```
/opt/wfserver/wfserver.sh --setup
```

The wizard edits the ini: users and passwords (stored in wfserver's own encoding), the
radio's serial port (type `/tmp/x6100-civ`; not `auto`: that looks for an Icom USB port, never
opens the pty and leaves you with audio but no control), CI-V address (61, or `auto`: x6100_trx answers
the 19 00 broadcast), PTT type `civ` (not `rts`: the pty has no RTS line) and the audio
devices (it lists them; pick the X6100 codec, card 0, for both). CI-V transceive is on
in x6100_trx by default (1A 05 0131). Then:

```
/opt/wfserver/wfserver.sh -l /tmp/wfserver.log
```

Or automatically: `x6100_ui.sh stop` (handing the radio to x6100_trx) starts a detached
watcher that runs wfserver as above while x6100_trx serves `/tmp/x6100-civ`. It starts
wfserver when the pty appears, restarts it when the pty changes (x6100_trx restarted)
or wfserver ends, and `x6100_ui.sh start` (back to the GUI) stops both. It does this
only when wfserver is installed and set up (a user in the ini). `x6100_ui.sh wfserver
off` switches the autostart off (`/etc/x6100/wfserver.off`) and stops it, `on` switches
it on and starts it; `x6100_ui.sh status` shows it.

The launcher adds `-s /etc/x6100/wfserver.ini` when no `-s` is given (wfserver's own default
would be `~/.config/wfview/wfserver.conf`). For another file, use an absolute path with
`-s`: a bare file name is taken relative to wfserver's data
directory (`~/.local/share/wfview`). `-d` adds debug logging, `-b` runs it in the
background.

In wfview on the PC: Settings, Radio access: LAN; the radio's IP address, control port
50001, the user and password from the wizard.

## Notes

- The codec is one device. While wfserver has it open, the x6100_trx audio scope (F3)
  and voice messages cannot open it (the scope says "codec capture not available").
  The RF scope and CW messages are not affected.
- Without any sound card wfserver crashes after "no default device found, aborting!":
  `servermain` indexes the device list with -1 (wfview, `audiodevices.cpp` /
  `servermain.cpp`). The radio has card 0, so this only shows on a host test.
- The ini's `[General] AudioSystem=0` is Qt audio (ALSA plugin only; no PulseAudio on
  the radio). PortAudio (1) and RtAudio (2) are in the bundle as well.
- wfserver has its own 3-minute transmit limit; x6100_trx's TOT applies as well.
- Scope: keep `1\WaterfallFormat=0` in the ini. With 1 (combined; the wizard may set it)
  wfserver merges the 11 scope divisions itself and wfview shows no scope (seen
  2026-10-28; why the merge fails is open). `ServerScopePort` (50004, from the wizard)
  is not used for an Icom-type radio: the scope travels with CI-V on 50002.
- `x6100_ui.sh wfserver off` waits until wfserver has let go of the codec, so `on`
  right after it gets the sound device; before, the new wfserver could start while the
  old one still held it (no audio until a reboot).
- `build.sh` in the repository root backs up and restores `/etc/x6100/wfserver.ini`
  with the other settings.
