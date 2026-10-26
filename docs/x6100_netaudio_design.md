# X6100 network audio — design

Status: draft, 2026-10-19. Nothing here is implemented yet.

Goal: RX audio, TX audio and control of the X6100 over the LAN, usable from wfview,
with the X6100 as its own rig identity (not IC-705 mimicry).

## 1. What the hardware gives us (verified)

| Path | How | Status |
|---|---|---|
| RX audio | ALSA card 0 capture, 48 kHz S16 stereo (L = R), demodulated by the baseband | verified, FT8 decodes |
| TX audio from Linux | ALSA card 0 playback, keyed with register 12 `0x4000` (codec TX) | verified, USB and USB-D |
| TX from the microphone | register 12 `0x40000` (PTT) | verified |
| TX from the rear line input | PTT in USB-D / LSB-D | presumed, untested |
| Control | libx6100 register file, flow telemetry | verified |
| Network | USB-C Ethernet (Dell adapter) works; built-in WiFi is weak | user report |

Network TX audio is therefore pure software: receive, buffer, play into card 0,
key with `0x4000`. No hardware obstacle.

## 2. Protocol choice

| Option | For | Against |
|---|---|---|
| A. Port wfserver (Qt) to the R16 | proven, X6200 ships it; the CI-V rig it expects now exists: `x6100_trx --cat=pty` (2026-10-25) | Qt + audio stack on a weak SoC (needs a buildroot package); QtAudio dropouts (X6200 forum) |
| **B. Own C daemon speaking the Icom LAN protocol (server side)** | wfview connects unchanged; CI-V engine sits directly on libx6100; small, static, no Qt | protocol must be reimplemented from wfview's client code |
| C. Own protocol + new wfview backend | simplest daemon | needs wfview changes, no other client works |

**Proposal: B.** The daemon presents itself as a networked Icom-protocol rig with an
X6100 CI-V identity; wfview gets an X6100 rig file describing exactly what the
X6100 supports.

Icom LAN protocol outline (UDP; details to be verified against wfview's
`icomudp*` sources before implementing):

| Port | Stream |
|---|---|
| 50001 | control: are-you-there / I-am-here / are-you-ready, ping, login, token, connection info (codecs, sample rates) |
| 50002 | CI-V frames, wrapped |
| 50003 | audio, both directions, sequence-numbered; retransmit requests |

Audio codecs offered by wfview include µ-law, 8/16-bit PCM and Opus, mono or stereo,
at 8–48 kHz. Opus mono is the default target (small, robust on WiFi).

## 3. Architecture

One process owns the baseband. The register file is written by exactly one owner;
two programs writing it (as the GUI and our tools did) is the failure mode we
already saw.

```
                 +--------------------- x6100d ---------------------+
 front panel --->| panel / display  |                                |
                 |                  |  radio state  -> libx6100 ---->| I2C, UART
 LAN (UDP) ----->| Icom LAN server  |  (one owner)                   |
                 |   CI-V engine ---+                                |
                 |   audio RX  <---- ALSA capture  (card 0) <--------| baseband RX audio
                 |   audio TX  ----> jitter buffer -> ALSA playback ->| codec TX (0x4000)
                 +---------------------------------------------------+
```

- `x6100_trx` becomes the panel/display part of `x6100d` (thread), not a separate
  program.
- Threads: radio loop (registers, flow, 5 ms), audio TX (ALSA write, highest
  priority), audio RX (ALSA read + encode), network (UDP), display.
- Shared state behind one mutex; audio buffers lock-free single-producer /
  single-consumer rings.

## 4. TX audio path

```
UDP audio -> reorder by sequence -> decode (Opus / PCM / µ-law) -> resample to 48 kHz
          -> jitter buffer -> drift control -> ALSA card 0 playback
```

| Item | Design |
|---|---|
| Jitter buffer | target depth configurable: 60 ms wired, 200–300 ms WiFi |
| Clock drift | client and codec clocks differ (typ. ±100 ppm); adaptive resampler steers buffer depth to target (±0.1 % max) |
| Gaps | missing packet: Opus PLC, else silence-fill; never let ALSA underrun while keyed |
| Level | fixed digital gain so the client's TX level maps to a known ALC range; calibrated once (codec level vs ALC test) |
| Mixer | `x6100_mixer_tx_setup()` at start and before every TX |

### Keying sequence (fail-safe)

| Event | Action |
|---|---|
| CI-V PTT on from client | state `TX_ARMED`; wait until the buffer reaches target depth (max 300 ms), then set `0x4000` |
| Audio flowing | keep keyed |
| CI-V PTT off | `TX_DRAIN`: play out what is buffered (bounded by target depth), then clear `0x4000` |
| Buffer empty > 300 ms while keyed | unkey (network stall) |
| No control ping > 2 s | unkey, drop session |
| TX longer than limit (default 180 s) | unkey |
| SWR > 3.0 for 300 ms | unkey, report to client |
| Out of band | refuse key, report |
| Local PTT / hand mic keys | baseband TX flag without our key: report TX to client, refuse LAN TX until released |

Rule: every path that stops audio also clears `0x4000` and PTT. Never stay keyed
with ALC at 100 and no power.

## 5. RX audio path

```
ALSA card 0 capture 48 kHz -> mono -> anti-alias + resample to client rate
                           -> encode -> UDP audio
```

Muted while transmitting unless the client asks for monitor.

## 6. TX audio source selection (rig identity)

Icom rigs select the data modulation source by CI-V ("DATA MOD": MIC / ACC / USB / LAN).
For the X6100 identity:

| Source | Meaning on the X6100 | Keying |
|---|---|---|
| LAN | network audio via codec | `0x4000` |
| MIC | internal / hand microphone | PTT `0x40000` |
| ACC | rear line input / USB sound card, USB-D / LSB-D | PTT `0x40000` + D-mode (to verify) |

The rig file lists only these. CAT PTT from the client keys with the selected source.

## 7. Security

- Bind to the wired interface (or a configured address); not on all interfaces by default.
- Icom-protocol login obfuscation is weak: treat the link as LAN-only, never expose
  to the internet; remote use goes over VPN.
- Credentials in a root-only config file; no default password.
- One TX-capable client at a time; additional clients RX-only.

## 8. Build and footprint

- C, static, zig cross build like the rest; libopus built static from source.
- No Qt, no PulseAudio, no libasound (raw ALSA ioctls already in libx6100).
- Expected CPU on the R16: Opus mono encode + decode well below one core.

## 9. Phases

| Phase | Deliverable | Test |
|---|---|---|
| 1 | `x6100_netaudio_test`: raw PCM over UDP into the TX path with jitter buffer, drift control and keying rules | PC script streams a tone / FT8 WAV; check on 7851, pull the cable mid-TX |
| 2 | `x6100d` skeleton: single owner, panel/display from `x6100_trx`, local only | same as x6100_trx |
| 3 | Icom LAN control + CI-V: connect, frequency, mode, PTT | wfview connects, tunes, keys |
| 4 | Audio RX + TX over the protocol, Opus | wfview audio both ways, FT8 via WSJT-X on the PC |
| 5 | Scope data from the flow IQ (CI-V 27) | wfview waterfall |
| 6 | X6100 rig file for wfview | wfview shows exactly the X6100 feature set |

## 10. Open questions

1. Option B (own daemon) vs wfserver (A): reopened 2026-10-25, now that x6100_trx offers
   CI-V on a pty and the DEV port is capped at 19200. Next: can wfserver (Qt5, opus,
   audio) be built for the R16 against the image (buildroot package), and how it runs.
   A third option: TCI. OpenX6X00 (Guru-RF, LGPL-2.1) dropped the inherited wfview (Icom
   LAN) server and serves TCI v2.0 over a WebSocket on port 50001 (VFO, mode, filter,
   AGC, RIT, drive, PTT with the client's audio, meters, RX audio); TCI clients (SDR
   software, loggers, the VFO-Knob) work with it, wfview does not.
2. CI-V address and model ID for the X6100 identity, and which wfview rig-file
   fields cover DATA MOD source selection.
3. Does wfview's Opus negotiation work against a non-wfserver server as-is?
4. Merge `x6100_trx` into `x6100d` now (phase 2) or keep it standalone until phase 3.
5. Default jitter buffer for the Dell adapter: start at 60 ms, measure.
