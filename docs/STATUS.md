# libx6100 / x6100_trx — status

Snapshot: 2026-10-25. Last tag: v0.9.0 (2026-10-25, on 33b74aa; split, RIT / XIT,
memories, tuner memories, CW keyer, liquid-dsp). Tags are made by hand: they cannot be
pushed from the cloud sessions.
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
- CI-V as IC-705 on the DEV USB-C (ttyS2, 19200 only on the stock kernel), wfview:
  frequency, mode, filters, gains, power, ATU, tune, meters, scope
- memory channels (V/M), `memr` list; console with own line editing
- liquid-dsp v1.8.3 static build, `x6100_liquid` runs on the radio
- CW keyer (KEY short key type, KEY long panel; registers 33 / 34; CI-V 14 09 / 0C / 0F):
  pitch 300-900 Hz (600 out of the box), sidetone 0-100 % over the full register; key
  types straight / paddle (dot left) / reversed work (polarity fixed 2026-10-25);
  iambic B out of the box
- repeater duplex (AGC long panel): AUTO -100 kHz in NFM on 29.620-29.680 MHz, manual
  DUP- / DUP+ (also in SSB), console status line; PTT key and front (hand) mic; no CTCSS
- battery charge / external supply: percentage and CHARGING plausible (info line, status
  page, console)
- TX readings folded into the console status line
- WiFi: connecting to a saved network from the GEN panel
- 60 m channel mode on the MFK (FST: CH)

Built, not yet tried on the radio (2026-10-25):

- tuner memories per frequency (nearest within 0.5 % of the TX frequency)
- TX frequency on the display while transmitting with split or a duplex shift

## Open items to check on the radio

1. Tuner memories: a tune stores, a retune nearby replaces, recall within the window.
2. Battery low notes at 20 / 10 % (on battery only).
3. Keyer: iambic A and B differ
   (squeeze and release: B adds one opposite element).
4. GEN WiFi / Bluetooth rows: WiFi / Bluetooth on / off, pair a headset (may need a
   bluez agent; see S39btpairings), panel stutter from polling.
5. wfview over CAT: VFO A / B, split, duplex, RIT / XIT, band buttons, keyer levels;
   ATT / PRE (socat trace against IU1WXY).
6. Repeater duplex, the rest: display shows 29.520 while keyed (PTT key, hand mic, CAT
   PTT), mem 22 (AUTO) and a memory with dup- 100, state after a restart, split over
   duplex, MFK hold resets, panel layout. Scope TX line: only when in view (by design).

## Next steps (suggested order)

1. Radio checks above.
2. TX panel under GEN: mic select and gain, compressor, VOX, monitor, squelch.
3. CI-V memory commands (08-0B, 1A 00) and 1A 05 0131 transceive switch.
4. Band edge markers on the scope from the region table.
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
