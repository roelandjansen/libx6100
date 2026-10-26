# libx6100 / x6100_trx — status

Snapshot: 2026-10-25. Last tag: v0.10.0 (2026-10-25, on 40d00ac; repeater duplex, TX
frequency on the display, CW pitch / sidetone / paddle fixes, iambic B, state saved after
a tune). Before: v0.9.0 on 33b74aa. Tags are made by hand: they cannot be pushed from the
cloud sessions.
Open work in detail: `TODO.md`. Use: `x6100_trx_quickref.md`. CAT: `x6100_civ.md`.

## Where we are

Radio: X6100 on the IU1WXY OS / GUI image, stock baseband BASE 1.1.9.
`x6100_ui.sh stop` stops the IU1WXY GUI; `x6100_trx` then owns the baseband.

Verified on the radio:

- RX / TX, ATU tune with L / C / S, PTT key, CAT PTT; hand mic PTT keys the baseband
- scope modes, span, audio scope, split RF / AF view, reference levels (F2 long)
- NR / NB / notch, DSP panel, filter panel, S-meter, OVF, meters
- VFO A / B with A=B and notes; split (PTT key and hand mic), RIT / XIT
- settings files (`x6100_trx.conf` yours, `x6100_trx.state` last state), backup / restore
- GEN panel: brightness (inverted backlight, 5-100 %), WiFi / Bluetooth rows built;
  status page; NTP left to the image (S48sntp / S49ntp)
- PWR: display off, quit, power off
- CI-V as IC-705 on the DEV USB-C (ttyS2; 19200 is the port's limit: 9600 / 19200 work,
  38400 and up do not, checked 2026-10-25 with the divisor written), wfview:
  frequency, mode, filters, gains, power, ATU, tune, meters, scope, band buttons (1A 01)
- CAT on a pseudo-terminal (`cat pty`): /tmp/x6100-civ, served over the LAN with socat
  (TCP 4532, one client) to a virtual port on the PC; wfview works through it (2026-10-26)
- memory channels (V/M), `memr` list; console with own line editing
- liquid-dsp v1.8.3 static build, `x6100_liquid` runs on the radio
- CW keyer (KEY short key type, KEY long panel; registers 33 / 34; CI-V 14 09 / 0C / 0F):
  pitch 300-900 Hz (600 out of the box), sidetone 0-100 % over the full register; key
  types straight / paddle (dot left) / reversed work (polarity fixed 2026-10-25);
  iambic A and B both verified (B out of the box)
- repeater duplex (AGC long panel): AUTO -100 kHz in NFM on 29.620-29.680 MHz, manual
  DUP- / DUP+ (also in SSB), console status line; PTT key and front (hand) mic; no CTCSS
- battery charge / external supply: percentage and CHARGING plausible (info line, status
  page, console)
- TX readings folded into the console status line
- WiFi: connecting to a saved network from the GEN panel
- 60 m channel mode on the MFK (FST: CH)
- TX frequency on the display while keyed with a duplex shift (AUTO, DUP- / DUP+; PTT
  key, mic PTT, tune), with split (TX VFO), without XIT (as Icom; console shows the
  real TX frequency), DUPLEX OFF stays on RX; the scope stays on RX
- tuner memories per frequency (nearest within 0.5 % of the TX frequency): relays click
  when the VFO crosses into another memory with the tuner in, none with it out
- state saved right after every tune: the new atumem is on disk while x6100_trx runs
  (stored at the TX frequency, e.g. 29530000 for a tune on 29.630 with AUTO duplex)

Built, not yet tried on the radio (2026-10-25):

- TX panel (F4 long): mic select and gains, compressor, VOX, monitor, squelch; state,
  console, CI-V 14 03 / 0B / 0E / 15-17 and 16 44-46
- birdies: scan (GEN BIRDIE SCAN / `birdiescan`), birdies.conf, yellow ticks on the
  scope, automatic notch in USB / LSB / AM (BIRDIE NOTCH, off for now); host simulation
  of 80 m finds the IU1WXY reference list within 3 Hz
- AM receive offset: 500 Hz above the dial (AGC long row AM OFFSET, `am_offset`), filter opened by as much (MW test
  with RIT: 20 Hz already better, 500 Hz clean); the RF scope now maps the IQ around
  the real receive frequency (RIT and the AM offset no longer shift the trace)
- CI-V scope: SCROLL-C sends its edges (the wfview waterfall froze in SCROLL-C)

## Open items to check on the radio

1. Tuner memories: SWR when keying on a recalled memory (saved and back after a restart:
   verified).
2. Battery low notes at 20 / 10 % (on battery only).
3. GEN WiFi / Bluetooth rows: WiFi / Bluetooth on / off, pair a headset (may need a
   bluez agent; see S39btpairings), panel stutter from polling.
4. wfview over CAT: VFO A / B, split, duplex, RIT / XIT, keyer levels;
   ATT / PRE (socat trace against IU1WXY).
5. Repeater duplex, the rest: CAT PTT, mem 22 (AUTO) and a memory with dup- 100, state
   after a restart, split over duplex, MFK hold resets, panel layout. Scope TX line:
   only when in view (by design).
6. TX panel: each row audible / visible on TX (mic gain on ALC, compressor, VOX keys
   and drops with the delay, anti-VOX against the speaker, monitor heard while keyed,
   squelch closes in FM); the gain ranges (0-50, 0-100) against the stock UI.
7. Birdies: 80 m scan against the IU1WXY list (TODO), tick on the scope line, the
   notch on 3837.014 (LSB, dial 3838.000: 986 Hz), PRE / ATT.
8. AM offset: MW station tuned exactly, sound as with RIT +500; signals on the RF scope
   at the right place in AM and with RIT on; wfview SCROLL-C waterfall runs.

## Next steps (suggested order)

1. Radio checks above.
2. CI-V memory commands (08-0B, 1A 00) and 1A 05 0131 transceive switch.
3. Band edge markers on the scope from the region table.
4. Network CAT and audio: wfserver on the radio over `cat pty` (Qt5 build for the R16?)
   or the own Icom LAN server (option A vs B in `x6100_netaudio_design.md`).
5. liquid-dsp in use: network audio / RADE (Icom LAN server design in
   `x6100_netaudio_design.md`), recordings, RX / TX equaliser.
6. Measurements: S-meter calibration, codec level against ALC.
7. CTCSS for the repeaters (see TODO: no tone encoder known, mic bypasses Linux).

## Working notes

- Build: `bash build.sh` (pull, fingerprint, zig; liquid-dsp needs cmake and the
  submodule: `git submodule update --init --depth 1`). `bash build.sh deploy` to
  `$X6100_HOST:/root`, seeds missing settings / memories from the examples.
- Settings on the radio live in `/etc/x6100`; a new SD image wipes them: `build.sh
  backup` before flashing, `build.sh restore` after.
- Commits as Roeland Jansen, no attribution lines, branch master. Tags are made locally
  and pushed with `git push origin <tag>` (v0.5.0-v0.7.0 were local only until 2026-10-24).
- Keyer since v0.9.0: `cwvol` in the state file is now 0-100 %; an old 0-50 value is read
  as a percentage (quieter), and a `cwpitch` above 900 is refused and falls back to 600.
- Cloud sessions: the container's global git identity is Claude; set the repo identity
  first (`git config user.name "Roeland Jansen"`, `git config user.email` as above) so
  no commit or tag is attributed to Claude.
- Never `git push --mirror` from a local clone: on 2026-10-26 it reset GitHub master to
  an old local state and removed tags v0.5.0-v0.10.0 (master restored, tags re-pushed
  by hand). Pull first; push tags by name.
