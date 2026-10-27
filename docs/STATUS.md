# libx6100 / x6100_trx — status

Snapshot: 2026-10-27. Last tag: v0.13.0 (2026-10-27; band edges and IC-705 fixed edges
on the scope, CI-V memories and transceive switch, CI-V the Icom way with the full
IC-705 memory layout, wfview rig file X6100.rig on address 61, VOX register fixed).
Before: v0.12.0 on 0fab380 (AM receive offset, wfview SCROLL-C fix, CAT over the LAN),
v0.11.0 on c3e536f. Tags are made by hand: they cannot be pushed from the cloud sessions.
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
- band edges on the RF scope from the region table (red bar outside the allocations,
  dashed line at each edge)
- fixed edges, IC-705 style (2026-10-27): 4 per range on the 12 IC-705 ranges, at most
  96 kHz; F2 in FIX / SCROLL-F picks one, F2 long stores the window round the VFO;
  state, console `edge` / `fixedge`, CI-V 27 16 / 27 1E (host tests pass)
- lost I2C writes (2026-10-27): a failed write resent at once (3 tries, the rest of the
  block still goes out), the whole register block after every un-key and write error,
  stuck-TX watch after an un-key; TX timeout off / 1 / 3 / 5 min in the TX panel (`tot`); later the
  same day: power to the minimum on a TX stuck after a write error, no quit on I2C
  failures (TX dropped and refused, still trying)
- scope panel (F1 long, 2026-10-27; the CENTER / FIX family switch is its MODE row): VBW,
  averaging, waveform type, max hold, colours of fill / line / max hold (R, G, B), waterfall
  on / off, speed, size and peak level, max hold off / decay / 10 s / on, center type
  (filter / carrier), scope during TX, as the IC-7851 scope set menu; state file,
  console, CI-V 27 1B / 1C / 1D; the audio scope with AF rows of its own (waveform,
  averaging, max hold, colours; copied from the RF rows on the first start)
- messages (MSG, 2026-10-27): M1-M8 in messages.conf, CW (USB tone, carrier on the dial)
  or voice (WAV), sent with codec TX, also from CI-V 17; MSG long repeats; SWR scan (APP):
  the swrscan flag (AM, 5 W, as x6100_gui)
  over the band segment, 41 points, curve on the scope; host tests of the Morse timing,
  the WAV reader and the CW level
- wfview rig file for the X6100 (2026-10-27): `contrib/wfview/X6100.rig` from the IC-705
  one by `mkrig.sh`; `civaddr 61` in the settings file makes x6100_trx an X6100 on
  address 61 (default stays A4 / IC-705); 19 00 answered to a broadcast. The Icom way:
  all IC-705 commands and polls in the file, NG where we lack the function; 1A 00 in
  the full IC-705 layout (A + B side, B = split TX side, name at 100) (host tests pass)
- CI-V memories (08 select / tune, 09 write, 0A to VFO, 0B clear, 1A 00 contents in the
  IC-705 layout) and the transceive switch 1A 05 0131; host tests and a pty test pass

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
8. AM offset: MW station tuned exactly, sound as with RIT +500; AGC long row AM OFFSET
   (20 / 200 / 500 Hz, hold = 500, panel scrolls to row 6); signals on the RF scope
   at the right place in AM and with RIT on; wfview SCROLL-C waterfall runs.
9. wfview with CI-V memories (its memory window: read, write, recall, clear) and with
   transceive off (mouse-wheel tuning without jumps?).
10. Fixed edges: F2 / F2 long in FIX and SCROLL-F, the label, a new band moves the window,
    the band edge markers on 40 / 20 m, edges after a restart; wfview fixed edge window
    (27 1E set / read), a 200 kHz edge from wfview is cut to 96 kHz.
11. wfview with X6100.rig: `civaddr 61`, the rig file in wfview, connect on 61 and on
    auto; NG answers do not upset it; scope, fixed edges, memories with names and a
    split TX side (also with the IC-705 file on A4), transceive.
12. Lost I2C writes and TX timeout. TOT verified 2026-10-27 (1 min): PTT key TX drops;
    hand mic TX: TX enable off stops it at once, it stays off with the mic PTT held, a key
    ends the block and the next mic PTT transmits (the mic PTT itself cannot, unseen by
    Linux). 4.4 clean 2026-10-27: no i2c lines on PTT / CAT / tune / split un-keys, band
    changes or the hand mic. TOT kept over a restart (state file). Still open: CAT PTT
    timeout (4.2).
13. Scope panel: each row on the radio (VBW, averaging, waveform, colours, max hold,
    waterfall off / speed / size / peak level, center type, scope during TX off), the
    audio scope AF rows (own colours etc.), values back after a restart.
14. Messages. Verified 2026-10-27: CW message in CW mode at zero beat on the IC-7851,
    clean keying, ALC fine at -10 dBFS. Repeat (MSG long): countdown fine, but the repeat
    hit TOT at once (start a ms after the loop time: unsigned wrap), fixed, to retest.
    Still: stop by key / PTT,
    CI-V 17 from a logger, a voice WAV in USB, LSB, AM and NFM.
15. SWR scan: on a dummy load (flat 1.0-1.2) and on an antenna, against the stock UI's
    SWR scan; the carrier (AM, 5 W, swrscan flag) and the curve.

## Next steps (suggested order)

1. Radio checks above: 10-15 planned for the evening of 2026-10-27 (checklist handed over
   separately, order: scope panel, fixed edges, wfview X6100.rig, TX safety, messages,
   SWR scan).
2. Network CAT and audio: wfview's wfserver on the radio over `cat pty` (decided
   2026-10-27): build it for the R16 (Qt5, opus; buildroot package), run, measure.
   Own Icom LAN server only as the fallback (`x6100_netaudio_design.md`).
3. liquid-dsp in use: network audio / RADE (Icom LAN server design in
   `x6100_netaudio_design.md`), recordings, RX / TX equaliser.
4. Measurements: S-meter calibration, codec level against ALC.
5. CTCSS for the repeaters (see TODO: no tone encoder known, mic bypasses Linux).
6. Later: own buildroot, self-contained (now the IU1WXY image is the base): a full image
   and an applications-only package (TODO, Image and distribution).

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
