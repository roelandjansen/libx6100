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
| FST | step 10 / 100 / 1000 / 5000 Hz | 1 Hz step on / off |
| VOL knob | volume; push = mute | |
| MFK knob | RF gain or TX power | |
| MFK push | RF gain 100 / TX power 5.0 W | switch RF gain <-> TX power |
| BAND- / BAND+ | previous / next band (last frequency per band kept) | |
| SSB / CW / AM-FM | USB <-> LSB / CW <-> CWR / AM <-> NFM | |
| PRE | preamp on / off | attenuator on / off (~12 dB) |
| AGC | off / slow / fast / auto, per mode group | |
| F1 | scroll on / off: CENTER <-> SCROLL-C, FIX <-> SCROLL-F | CENTER family <-> FIX family |
| F2 | span ±48 / 24 / 12 / 6 / 3 / 1.5 kHz | |
| F3 | free | |
| F4 | TX meter: PO / SWR / ALC | |
| F5 | filter FIL1 / FIL2 / FIL3 | filter panel |
| PTT | hold = TX; tap (< 400 ms) = latch, next press drops | |
| ATU | tuner in / out | tune (keys TX at tune power) |
| LOCK | lock knobs and keys (PTT still works) | |
| PWR | quit (drops TX first) | |

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

## Display

- TX box left of the frequency: red outline / red TX = RX, red fill / white TX = TX
- STEP and status fields right-aligned, fixed width
- S-meter: S0-S9, then S9+10 .. S9+60 in rounded 10 dB steps; dBm line exact
- AGC default AUTO; AM / FM SLOW (AUTO squashes AM)
- F1-F5 tags along the bottom edge: SCOPE, SPAN, (free), METER, FIL
- OVF (red, top left of the dial): flow IQ at its clip level

## Console (ssh)

Type a line + Enter while running; tokens can be combined (`14074000 usb`).

| Token | Effect |
|---|---|
| `7074000`, `7.074`, `7074k` | frequency (Hz, MHz, kHz) |
| `usb lsb cw cwr am nfm` | mode |
| `off slow fast auto` | AGC |
| `att noatt pre nopre` | attenuator / preamp |
| `center scrollc fix scrollf` | scope mode |
| `fil1 fil2 fil3` | receive filter |
| `step 1\|10\|100\|1000\|5000` | tuning step |
| `pwr 0.1-10` | TX power in W |
| `vol 0-100` | volume (also unmutes) |

## TX safety

- amateur bands only (unless `--oob`); TX timeout
- SWR above 3.0 for 300 ms drops TX
- any key except PTT and F1-F5 drops a latched TX; tune aborts on any key
- hand mic PTT keys the baseband directly; the display follows the TX flag
- transient I2C errors are retried; quits after 2 s of failures
  (the baseband keeps its last state, so audio continues)
