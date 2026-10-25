# x6100_trx — quick reference

Transceiver front end for the X6100 on stock BASE 1.1.9, driven by the front
panel. `x6100_rx` is the same program, receive only (PTT and tune refused).

## Start

Stop the stock GUI first (`scripts/x6100_ui.sh stop`).

```
x6100_trx [--oob] [--no-limit] [--cat[=baud]] [--cat-log] [--probe] [freq] [mode] [agc] [att|noatt] [pre|nopre] [vol 0-100]
x6100_trx 7074000 lsb vol 30
x6100_trx --oob 5450000 usb slow
```

| Option | Effect |
|---|---|
| `--oob` | no band check before TX, baseband TX enabled everywhere (transverter, tests; legal limits are yours) |
| `--no-limit` | tune 10 kHz - 100 MHz instead of 0.5 - 56 MHz (baseband probe); put it before the frequency |
| `--cat[=baud]` | CI-V on /dev/ttyS2 (DEV USB-C, PC side the second CH342 port, e.g. ttyACM1), default 19200, as an IC-705: see `docs/x6100_civ.md` |
| `--cat-log` | as `--cat`, and print CI-V commands (`>`) with our OK / NG reply (`<`); polls answered with data are not shown |
| `--probe` | print the flow flag word on change, and every second the IQ peak and all flag bits seen (overflow hunt) |

Out-of-range frequency arguments are refused (a stray `20` is not 20 Hz).

## Front panel

| Control | Short | Long |
|---|---|---|
| VFO knob | tune | |
| FST | step 10 / 100 / 1000 / 5000 Hz; AM on LW / MW: 100 / 1k / 5k / 9k / 10k (channel raster); 60 m: 10 / 100 / 1k / 5k / CH (MFK picks the 60 m channel, the VFO knob fine-tunes in 100 Hz) | 1 Hz step on / off |
| VOL knob | volume; push = mute | |
| MFK knob | RF gain or TX power | |
| MFK push | RF gain 100 / TX power 5.0 W | switch RF gain <-> TX power |
| BAND- / BAND+ | previous / next band (last frequency per band kept) | |
| SSB / CW / AM-FM | USB <-> LSB / CW <-> CWR / AM <-> NFM | |
| A/B | VFO A <-> B (frequency, mode, filter) | A=B: the other VFO takes the one in use |
| PRE | preamp on / off | attenuator on / off (~12 dB) |
| DFN | NR (noise reduction) on / off | NB (noise blanker) on / off |
| DFL | notch on / off | DSP panel (NR, NB, notch levels) |
| AGC | off / slow / fast / auto, per mode group | RIT / XIT panel |
| V/M | memory panel (channels 1-99) | split on / off: RX on the VFO in use, TX on the other |
| F1 | scroll on / off: CENTER <-> SCROLL-C, FIX <-> SCROLL-F | CENTER family <-> FIX family |
| F2 | span ±48 / 24 / 12 / 6 / 3 / 1.5 kHz; audio scope: 0-3 / 0-6 kHz | reference level: MFK turn = ±1 dB (±30), MFK push = RF <-> AF scope; F2 long, other key or 10 s idle closes |
| F3 | audio scope on / off (RF <-> AF) | RF scope left (5/8) + audio scope right (3/8), on / off |
| F4 | TX meter: PO / SWR / ALC | |
| F5 | filter FIL1 / FIL2 / FIL3 | filter panel |
| PTT | hold = TX; tap (< 400 ms) = latch, next press drops | |
| ATU | tuner in / out | tune (keys TX at tune power) |
| LOCK | lock knobs and keys (PTT still works) | |
| PWR | display on / off | release after 1.5-4 s: quit; hold 4 s: power off (as the stock UI). TX is dropped and filters saved first |
| GEN | settings panel: brightness, CAT, NTP (MFK turn = change, push = next row, long = default; GEN, other key or 10 s idle closes) | status page: IP, uptime, battery, CAT, NTP result, build (any key or 30 s closes) |
| KEY / MSG / APP | not used yet (see the key plan in `TODO.md`) | |

## Scope modes

| Mode | Behaviour |
|---|---|
| CENTER | tuned frequency in the middle, spectrum moves with tuning |
| SCROLL-C | as CENTER, waterfall shifts with tuning |
| FIX | fixed window at the frequency where FIX was entered |
| SCROLL-F | fixed window that slides when tuning past an edge |

## Filter panel (F5 long, IC-705 style)

| Control | Action |
|---|---|
| MFK turn | adjust BW or SFT |
| MFK push | select BW <-> SFT |
| MFK long | DEF (default for this FIL) |
| F5 | next FIL |
| F5 long, other key, 10 s idle | close and save |

Per mode group (SSB, SSB-D, CW, AM, FM), saved in `/etc/x6100/filters.conf`.
Maximum 6000 Hz (baseband audio runs at 12 kHz). AM / FM are symmetric.

| Group | FIL1 | FIL2 | FIL3 |
|---|---|---|---|
| AM / FM | ±3 kHz | ±2.5 kHz | ±2 kHz |

Saved values override the defaults; MFK long (DEF) restores them.

## DSP panel (DFL long)

The baseband's noise reduction, noise blanker and manual notch on one sheet.

| Control | Action |
|---|---|
| MFK turn | adjust the selected value |
| MFK push | next value: NR level, NB level, NB width, notch, notch width |
| MFK long | selected value back to its default |
| DFN short / long | NR / NB on, off (panel stays open) |
| DFL short | notch on, off (panel stays open) |
| DFL long, another key, 10 s idle | close |

Values are kept in the last state (see Settings); defaults NR 20, NB 10 / 10, notch 1000 / 50 Hz.

## Display

- A or B right of the TX box: the VFO in use; A/B and A=B also show a 1.5 s note in the middle of the scope (`A COPIED TO B`)
- TX box left of the frequency: red outline / red TX = RX, red fill / white TX = TX
- STEP and status fields right-aligned, fixed width
- S-meter: S0-S9, then S9+10 .. S9+60 in rounded 10 dB steps; dBm line exact
- AGC default AUTO; AM / FM SLOW (AUTO squashes AM)
- F1-F5 tags along the bottom edge: SCOPE, SPAN, AUDIO, METER, FIL
- Audio scope (F3; F3 long: next to the RF scope, RF left / AF right): spectrum and waterfall of the demodulated RX audio (codec capture, 48 kHz), 0-3 or 0-6 kHz (F2), normalised dBFS scale (80 dB window, its bottom 10 dB under the noise floor measured inside the receive passband, so the picture stays put with volume and band noise; shifted by the AF reference level), receive passband shaded, MN line at the notch when on. The capture is opened only while the audio scope is shown
- OVF (red, top left of the dial): flow IQ at its clip level
- OOB (red, under OVF): band check off (`--oob` or `oob` in the conf) and tuned outside the region's allocations; where you transmit is your responsibility
- NB / NR / MN (blue, top right of the dial) in fixed places, e.g. `NB    MN`, `   NR   `, `NB NR MN`; MN = manual notch, as on Icom radios
- Tune panel (ATU long press / CAT): amber TUNING with live PO and SWR bars (SWR green / yellow / red), then green TUNED for 4 s with the time taken and the network (L, C, S in decimal, relay bits and hex)
- Clock under the filter label: system time, UTC, ISO 8601 (`2026-10-24T10:08:27Z`); hidden behind the filter panel while that is open. At start x6100_trx runs one NTP sync with the image's busybox `ntpd -q` (server: `ntp` in the conf, else /etc/ntp.conf, else pool.ntp.org) and reports the result on the console (a failure as a warning); skipped when the image syncs itself (init.d S48sntp / S49ntp, as on IU1WXY), when a time daemon runs, or with `ntp off`

## Settings panel (GEN)

![GEN settings](images/x6100_trx_gen.png)

| Row | Values | When |
|---|---|---|
| BRIGHTNESS | 5-100 % in steps of 5 (default 100) | at once |
| CAT (CI-V) | OFF, 4800 ... 115200 (default 19200; the stock image only runs 19200) | next start |
| NTP SYNC | AT START / OFF | next start |
| WIFI | ON / OFF (NetworkManager); shows network, IP and signal | at once |
| WIFI NETWORK | turn: choose a saved network, hold MFK: connect. New networks: `nmtui` over ssh | at once |
| BLUETOOTH | ON / OFF | at once |
| BT PAIRING | turn right: visible as X6100 for 120 s to pair, left: hidden | at once |
| BT DEVICE | turn: choose a paired device, hold MFK: connect / disconnect | at once |

The panel shows five rows and scrolls with the selection (position top right). WiFi and
Bluetooth are the image's own (NetworkManager, bluez) and keep their state themselves;
brightness, CAT and NTP go to `x6100_trx.state` at exit. A row set in `x6100_trx.conf` shows CONF and
cannot be changed here: the conf wins at start, so change it there.

GEN long: status page (IP address, uptime, battery, CAT, last NTP result, build).

![GEN status](images/x6100_trx_status.png)

## Settings

Two files in `/etc/x6100`, both in console tokens (`#` = comment):

| File | Written by | Holds |
|---|---|---|
| `x6100_trx.state` | x6100_trx, at exit | last state: frequency, mode, filter, VFO B, split, RIT / XIT, volume / mute, gains, AGC per mode group, DSP, scope, REF, band memories, tuned ATU networks |
| `x6100_trx.conf` | you, never x6100_trx | what must be the same at every start (example: `docs/x6100_trx.conf.example`) |
| `filters.conf` | filter panel | filter widths and shifts per mode group |
| `memories.conf` | memory panel / console | memory channels 1-99 |

At start: defaults, then `x6100_trx.state`, then `x6100_trx.conf`, then the command line;
later wins. Command-line values apply to that run only (but end up in the state at exit).

A new SD card image wipes `/etc/x6100`. On the PC:

```
sh ./build.sh backup     # radio -> ~/.config/x6100 (or $X6100_CONFIG), WiFi profiles included
sh ./build.sh restore    # ~/.config/x6100 -> radio, overwriting; restarts NetworkManager
                         # for the WiFi profiles (an ssh session over WiFi drops)
sh ./build.sh deploy     # also puts back settings the radio lacks (conf, memories: the examples)
```

## Console (ssh)

Type a line + Enter while running; tokens can be combined (`14074000 usb`).

What you type is shown at the end of the status line (`> memw 5`), so redraws do not
wipe it; Backspace and ^U (clear) work, Enter runs it and keeps it on its own line.
The status line is cut to the terminal width.

The status line starts with the VFO in use (A / B); with split, RIT or XIT it adds
`RX 7123.000 TX 7115.000 LSB SPLIT`.

| Token | Effect |
|---|---|
| `7074000`, `7.074`, `7074k` | frequency (Hz, MHz, kHz) |
| `usb lsb cw cwr am nfm` | mode |
| `off slow fast auto` | AGC |
| `att noatt pre nopre` | attenuator / preamp |
| `center scrollc fix scrollf` | scope mode |
| `fil1 fil2 fil3` | receive filter |
| `step 1\|10\|100\|1000\|5000\|9000\|10000` | tuning step (9000 / 10000 on LW / MW AM only) |
| `pwr 0.1-10` | TX power in W |
| `vol 0-100` | volume (also unmutes) |
| `nr nonr nb nonb notch nonotch` | noise reduction / blanker / notch on, off |
| `nrl 0-60`, `nbl 0-100`, `nbw 0-100` | NR level, NB level, NB width (start: 20, 10, 10) |
| `ntf 100-3000`, `ntw 10-100` | notch frequency, notch width in Hz (start: 1000, 50) |
| `ref -30-30`, `aref -30-30` | RF / audio scope reference level in dB (+ = trace up) |
| `rfg 0-100` | RF gain |
| `atu noatu` | tuner in / out |
| `rf af split`, `af3 af6` | scope view, audio range |
| `span 96000-3000` | scope span in Hz |
| `band 40m 7074000` | remembered frequency of a band |
| `agc ssb|ssbd|cw|am|fm off|slow|fast|auto` | AGC of a mode group |
| `atunet 40m L C S` | tuned network of a band |
| `mute nomute` | audio mute |
| `cat 19200`, `cat off` | CI-V at start (settings file; `--cat` overrides) |
| `ntp <server>`, `ntp on`, `ntp off` | NTP sync at start (settings file) |
| `bright 5-100` | display brightness in % |
| `ab`, `a=b` | VFO A <-> B, A=B |
| `txsplit`, `notxsplit` | split on / off |
| `rit`, `norit`, `xit`, `noxit`, `ritofs -9990-9990` | RIT / XIT on / off, offset in Hz |
| `step ch` | 60 m channel steps |
| `memr` | list the used memory channels |
| `memr 5`, `memw 5`, `memc 5` | recall / write the VFO into / clear memory channel 5 |
| `mem 5 7074000 lsb fil1 [tx 7076000 lsb] [name text]` | set a memory channel (as in memories.conf) |
| `vfob 7074000 lsb fil1`, `vfo a\|b` | the VFO not in use; which one is in use (state file) |
| `region r1\|r2\|r3\|nl` | transmit allocations for the TX check and the OOB marker (default r1) |
| `oob`, `nolimit` | as `--oob` / `--no-limit` (settings file): warned at every start, red OOB on the dial |

## TX safety

- TX only inside the allocations of the region (`region` in the conf: r1, r2, r3, nl; default r1) and a band the baseband transmits in, unless `oob`; TX timeout
- SWR above 3.0 for 300 ms drops TX
- any key except PTT and F1-F5 drops a latched TX; tune aborts on any key
- hand mic PTT keys the baseband directly; the display follows the TX flag. It does
  not abort a tune: Linux never sees it (flow flags show only b1 TX, 2026-10-24).
  Abort a tune with the PTT key, any other key or CAT PTT
- transient I2C errors are retried; quits after 2 s of failures
  (the baseband keeps its last state, so audio continues)

## Memory channels (V/M)

![memory](images/x6100_trx_memory.png)

| Control | Action |
|---|---|
| V/M | open / close the memory panel |
| MFK turn | channel 1-99; a used channel is tuned at once (listen while scrolling) |
| MFK long | write the VFO in use into the channel (frequency, mode, filter; with split also the TX side); a rewrite keeps the name |
| MFK push, twice within 3 s | clear the channel |
| other key, 10 s idle | close; the VFO stays on the last channel tuned |

Kept in `/etc/x6100/memories.conf`, one channel per line, written on every change and
included in `build.sh backup`:

```
mem 3 7074000 lsb fil1 name FT8 40m
mem 5 29620000 nfm fil2 tx 29520000 nfm name RPT 10m
```

Names are set in that file or from the console (`mem ...`); the radio has no keyboard.
`build.sh deploy` installs `docs/x6100_memories.conf.example` (FT8 on 160-6 m, a 60 m
spot, a 10 m repeater as a split channel) when the radio has no memories.conf and
there is no local backup.

`memr` on the console lists them:

```
 6   14074.000 kHz USB  fil1  FT8 20m
20   29620.000 kHz NFM  fil1  TX 29520.000 NFM  10m RPT 29.62
13 of 99 channels used
```

## Split, RIT / XIT

![split](images/x6100_trx_splittx.png)

- V/M long: split. RX on the VFO in use (+ RIT), TX on the other VFO with its mode and
  filter (+ XIT). The TX check and the OOB marker use the TX frequency.
- AGC long: RIT / XIT panel (green). One offset for both, as on Icom radios: MFK turn
  = offset in 10 Hz steps (±9990), on the RIT / XIT rows on (right) / off (left); MFK
  push = next row; MFK long = offset 0 (CLEAR); AGC long, other key or 10 s closes.
- Top left of the spectrum (amber): `SPLIT TX 7.120.00 LSB  RIT +120` while in use; on
  the RF scope a green line at the RX frequency and a red one, marked TX, at the TX
  frequency (the receive passband shading follows RIT).
- The baseband gets the TX frequency and mode just before PTT goes out and keeps it
  until the PTT drop has gone out. Front panel PTT, CAT PTT and tune follow split;
  the hand mic PTT keys the baseband directly: x6100_trx moves to the TX side as soon
  as the baseband reports TX, so the first few ms go out on the RX frequency (avoid
  the hand mic PTT with split across a band edge). Verified on the radio 2026-10-24:
  PTT key and hand mic both transmit on the split TX frequency.
- CI-V: 0F (split), 21 00 / 01 / 02 (offset, RIT, XIT).

## 60 m channels (FST: CH)

FST to CH on 60 m: the MFK knob steps through the channels (RF gain / power wait
until you leave CH), the VFO knob fine-tunes around them in 100 Hz steps.

| Region | Channels (dial, USB) |
|---|---|
| r2 | 5330.5, 5346.5, 5357.0, 5371.5, 5403.5 (US; outside 5351.5-5366.5 TX needs oob) |
| r1, r3, nl | 5351.5, 5354.0, 5357.0, 5360.0, 5363.0 (no official channels in the WRC-15 segment: edge, FT8 and a 3 kHz raster as a convenience) |

## Regions (TX allocations)

| | 160 m | 80 m | 60 m | 40 m | 30-10 m | 6 m |
|---|---|---|---|---|---|---|
| r1 | 1810-2000 | 3500-3800 | 5351.5-5366.5 | 7000-7200 | 10100-10150, 14000-14350, 18068-18168, 21000-21450, 24890-24990, 28000-29700 | 50-52 MHz |
| r2 | 1800-2000 | 3500-4000 | 5351.5-5366.5 | 7000-7300 | as r1 | 50-54 MHz |
| r3 | 1800-2000 | 3500-3900 | 5351.5-5366.5 | 7000-7300 | as r1 | 50-54 MHz |
| nl | 1810-1880 | 3500-3800 | 5351.5-5366.5 | 7000-7200 | as r1 | 50-52 MHz |

kHz unless stated. The dial frequency is checked, not the edges of the signal. nl: F
registration (stcrt 2015-3772, 2021-31799; 60 m per WRC-15). National rules and power
limits are the operator's to know; national 60 m channels (e.g. US) are not in r2.
