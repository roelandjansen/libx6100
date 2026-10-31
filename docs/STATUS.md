# libx6100 / x6100_trx — status

Snapshot: 2026-10-31. Last tag: v0.15.0 (2026-10-30; baseband IF shift in all modes and
TX, AM tone gone, AM snaps to the channel raster, scope bridges the DC spike, VOX N/A on
BASE 1.1.9). Before: v0.14.0 (2026-10-29, on d8fc40c from before the history rewrite;
wfserver for wfview, twin PBT incl. AM and the filter panel PBT box, scope panel,
MSG / APP keys, TOT and stuck-TX safety, RADE studies), v0.13.0 (band edges, fixed
edges, CI-V memories, wfview rig file on 61).
Tags are made by hand: they cannot be pushed from the cloud sessions.
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
- CAT on a pseudo-terminal: /tmp/x6100-civ (since 2026-10-31 always served next to the DEV
  port, `catpty off` turns it off), served over the LAN with socat
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
- radio test of 2026-10-27 evening (after v0.13.0):
  - TX timeout: PTT key TX drops; hand mic TX: TX enable off stops it at once, stays off
    with the mic PTT held, a key ends the block; TOT kept over a restart; the note on the
    scope clears after 10 s
  - lost I2C writes: no i2c lines on PTT / CAT / tune / split un-keys, band changes or
    the hand mic (no false stuck-TX watch)
  - TX meter choice (F4) kept over a restart
  - F1 short cycles CENTER -> SCROLL-C -> FIX -> SCROLL-F; GEN band on the band keys
    with its own frequency and mode
  - messages: CW message in CW mode at zero beat on the IC-7851, clean keying, ALC fine
    at -10 dBFS; MSG long repeats every 10 s with the countdown; any key and the PTT key
    stop it
- radio test of 2026-10-31 (after v0.15.0):
  - clock: grey when the time is known, amber when not; console `time` sets it
  - hand mic PTT with split: TX follows the split shown
  - wfserver: about 33 % of the CPU (1.3 of 4 cores), x6100_trx 13 %, 52 % idle at
    1008 MHz; no dropouts, no errors in its log. GEN row WFSERVER: off works; wfview
    sometimes needs up to three connects (a wfview reconnect matter, for the wfview
    project). Screenshot in the README
  - Bluetooth headset (Sony WH-1000XM2): with WiFi connected the scan finds nothing
    (rtl8723bu coexistence); paired with WiFi briefly off, then A2DP plays the RX audio
    (IU1WXY's btaudio-ctl loopback) with WiFi on. No headset mic: HFP with the native
    pulseaudio backend sets up eSCO and a source, but no samples arrive, neither mSBC
    (16 kHz) nor CVSD (8 kHz) (scripts/x6100_bt_hfp_test.sh): RADE TX stays on the PC
  - Bluetooth headset: connect / disconnect on the GEN row BT DEVICE and autoconnect
    (`btauto`: after Bluetooth off / on, the headset off / on, a restart) work
  - CI-V on the DEV port next to the pty: rigctl (IC-705 model 3085, 19200, civaddr 61)
    on the USB cable sets and reads the frequency while wfview runs over wfserver

Built, not yet tried on the radio (2026-10-25 .. 28):

- TX panel (F4 long): mic select and gains, compressor, VOX, monitor, squelch; state,
  console, CI-V 14 03 / 0B / 0E / 15-17 and 16 44-46
- birdies: scan (GEN BIRDIE SCAN / `birdiescan`), birdies.conf, yellow ticks on the
  scope, automatic notch in USB / LSB / AM (BIRDIE NOTCH, off for now); host simulation
  of 80 m finds the IU1WXY reference list within 3 Hz
- AM receive offset: 100 Hz above the dial (500 until 2026-10-29: a tone at the offset on weak
  stations; lower than 50 distorts) (AGC long row AM OFFSET, `am_offset`), filter:
  upper edge at the FIL width, lower edge 2 x offset inside (2026-10-29; before, opened
  by the offset on both sides let an adjacent carrier 5 kHz up whistle at FIL 4.5 kHz)
  (MW test with RIT: 20 Hz already better, 500 Hz clean); the RF scope
  maps the IQ around the real receive frequency (RIT and the AM offset no longer shift it)
- CI-V scope: SCROLL-C sends its edges (the wfview waterfall froze in SCROLL-C)
- band edges on the RF scope from the region table (red bar outside the allocations,
  dashed line at each edge)
- fixed edges, IC-705 style: 4 per range on the 12 IC-705 ranges, at most 96 kHz; F2 in
  FIX / SCROLL-F picks one, F2 long stores the window round the VFO; state, console
  `edge` / `fixedge`, CI-V 27 16 / 27 1E (host tests pass)
- lost I2C writes, the rest: power cut (TX enable off, minimum power) on a TX stuck after
  a write error; after 2 s of I2C failures TX drops and is refused, x6100_trx keeps
  trying instead of quitting
- scope panel (F1 long), IC-7851 style: mode, center type, REF LEVEL, VBW, averaging,
  waveform type, max hold off / decay / 10 s / on, colours of fill / line / max hold
  (R, G, B), waterfall on / off, speed, size and peak level, scope during TX; the audio
  scope with AF rows of its own (AF REF LEVEL, waveform, averaging, max hold, colours;
  copied from the RF rows on the first start); state file, console, CI-V 27 1B / 1C / 1D
- scope trace one FFT bin off (187.5 Hz): the mirrored bin is n - i (host simulation
  with tones on exact bins); fixed 2026-10-27, to see on the radio (zero-beat carrier on
  the VFO line)
- messages, the rest: CI-V 17 (send CW from a logger, text added while sending, 17 FF
  stops), voice WAV messages (48 kHz 16 bit, codec TX in the mode in use)
- SWR scan (APP): the baseband's own scan carrier (register 12 swrscan 0x10, AM, 5 W, no
  PTT, as R1CBU's x6100_gui), 41 points over the band segment, curve on the scope
- wfview rig file for the X6100: `contrib/wfview/X6100.rig` from the IC-705 one by
  `mkrig.sh`; `civaddr 61` makes x6100_trx an X6100 on address 61 (default stays A4 /
  IC-705); 19 00 answered to a broadcast. The Icom way: all IC-705 commands and polls in
  the file, NG where we lack the function; 1A 00 in the full IC-705 layout (A + B side,
  B = split TX side, name at 100) (host tests pass)
- CI-V memories (08 select / tune, 09 write, 0A to VFO, 0B clear, 1A 00 contents) and the
  transceive switch 1A 05 0131; host tests and a pty test pass
- GEN CI-V ADDRESS row (IC-705 A4 / X6100 61, at once, state)
- TX audio input, Icom DATA OFF MOD / DATA MOD (GEN rows, `modin`, CI-V 1A 05 0118 /
  0119): MIC or USB; on USB the PTT key and CAT PTT key with codec TX (the PTT bit stays
  off), so audio played into ALSA card 0 is the modulation; the codec mixer is routed
  once when either is USB. Default MIC / USB as an Icom (host tests pass)
- wfserver for the R16: `contrib/wfserver/build.sh` cross-builds wfview's wfserver (Qt
  5.15 from Ubuntu 24.04 armhf) into a self-contained bundle for /opt/wfserver (own
  libraries and glibc loader, ALSA config, X6100.rig); starts under qemu-user, loads
  the ini and all rig files. Setup in `contrib/wfserver/README.md`

## Open items to check on the radio

1. Tuner memories: SWR when keying on a recalled memory (saved and back after a restart:
   verified).
2. Battery low notes at 20 / 10 % (on battery only).
3. GEN WiFi / Bluetooth rows: WiFi / Bluetooth on / off, pair a headset (may need a
   bluez agent; see S39btpairings), panel stutter from polling.
4. wfview over CAT: keyer levels. VFO A / B, split, RIT / XIT, duplex DUP- / DUP+ and ATT /
   PRE work (with the IC-705 file wfview offered two PRE values, PREAMP 1 / 2: check
   with X6100.rig, which has off / on only, item 11).
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
    split TX side (also with the IC-705 file on A4), transceive; PRE shows off / on only.
12. TX timeout with CAT PTT (test 4.2).
13. Scope panel: each row on the radio (REF LEVEL in FIX, VBW, averaging, waveform,
    colours, max hold, waterfall off / speed / size / peak level, center type, scope
    during TX off), the AF rows, values back after a restart; a zero-beat carrier on the
    VFO line (bin fix).
14. Messages, the rest: CI-V 17 from a logger (5.5), a voice WAV in USB, LSB, AM and NFM
    (5.6, 5.7), a 44.1 kHz file refused (5.8).
15. SWR scan: on a dummy load (flat 1.0-1.2) and on an antenna, against the stock UI's
    SWR scan; the carrier (AM, 5 W, swrscan flag) and the curve.
16. GEN CI-V ADDRESS row: switch with wfview connected, kept after a restart.
17. wfserver on the radio: install the bundle, `--setup`, wfview over LAN (control,
    scope, RX audio, TX audio with DATA MOD USB in USB-D and DATA OFF MOD USB in USB);
    CPU load, audio dropouts; the audio scope is blocked while wfserver holds the codec.
    Autostart: `x6100_ui.sh stop`, then x6100_trx with cat pty: wfserver comes up;
    restart x6100_trx: wfserver restarts on the new pty; `x6100_ui.sh start` stops it.
    2026-10-28 first run: deploy, `--setup`, wfview over LAN connects; RX audio, VFO A / B,
    meters and (with WaterfallFormat 0) the scope work on address 61. Found on the way:
    SerialPortRadio must be /tmp/x6100-civ (not auto), WaterfallFormat 1 loses the
    scope, `wfserver off` / `on` lost the audio (fixed: stop waits for wfserver). Open:
    TX audio, CPU load, dropouts, the audio after the fix, why combined mode fails.
    2026-10-29: TX audio from wfview works (USB-D, DATA MOD USB: CAT PTT keys codec TX,
    PC audio heard on the IC-7851); tuner in / out and tune cycle from wfview work.
    Band buttons incl. GEN (code 15, added today), split and RIT from wfview work. Twin
    PBT (14 07 / 08) from wfview works in USB (2026-10-29: passband and FIL label width
    follow). AM PBT Icom style built 2026-10-29 (shift = receive offset, symmetric
    narrowing, carrier kept inside): to check; not in FM. Filter panel (F5 long) has a
    PBT box: MFK push to it, turn shifts both 50 Hz, hold = PBT CLR; edges show the
    passband after PBT (host render checked; to check on the radio).
    AM tone off the carrier (v0.13.0 too): the zero-IF DC / LO spike (S3-4 on the scope)
    beating with the carrier. 2026-10-30: AM snaps to the channel raster on the mode
    keys, and AM uses the baseband IF shift (register 35, ifshift 10000; AM offset then
    0): verified (spike 10 kHz left, carrier on the receive line; the scope bridges
    the spike); the tone off the carrier is gone. Shift in all modes, kept in TX: the
    baseband transmits on the dial frequency (CW on 7.030 verified), the TX scope on the
    TX line, CW keying clean, split (VFO B) and hand mic PTT work, also together (the
    hand mic TX follows the split shown, 2026-10-31). VOX: not on BASE
    1.1.9 (IU1WXY neither, same register 22); the TX panel shows VOX N/A.
    Open: CPU load, dropouts, why combined scope mode fails.
18. RADE in x6100_trx: the codec has one input and one output to the baseband, both
    switched by register 12 bit 0x8 (play mode). RX listening locally has no known path
    (decoding needs the RX audio in the capture, 0x8 off; the speaker plays Linux audio
    only with 0x8 on). TX depends on two tests (x6100_trx stopped, dummy load):
    E1 mic into Linux: `x6100_audio 14236000 usb 10 /tmp/e1.wav --play-mode --mic int`
    (then --mic hand, and each again with --micgain 0 as the stock UI records), speak into
    the mic: does the RMS follow the voice (and the WAV hold it)?
    E2 modulation in play mode: `x6100_tx 14236000 codec 5 1 1000 -30 --play-mode`: does
    the tone go out (second receiver, power / ALC), or to the speaker?
    Both yes: RADE TX from the built-in mic works (mic -> capture -> encode -> codec TX).
    Result 2026-10-29: E1 no. With 0x8 the capture stays flat (-37 dBFS at boost +24, -46
    at boost 0) whatever is said into either mic, gains 30 / 20 or 0; RX audio without 0x8
    -22.5. No mic and no speaker path for Linux: RADE inside x6100_trx is not possible with
    the known controls (E2 moot). RADE goes through the PC: wfview / wfserver + FreeDV,
    TX audio via DATA MOD USB (item 17).
    E1b, the other codec inputs (Mic1 carries the baseband RX audio): `x6100_mixer list`
    (which capture switches the codec has), then `x6100_audio 14236000 usb 10
    /tmp/e1b.wav --input mic2|line|mixer|aif2 [--play-mode]`, speaking into the hand mic.
    The list (2026-10-29): 41 controls; Mic2, Line In and the mixer exist; AIF2 (the second
    I2S interface) is muted and unrouted in the image, never listened to.
    E1b result: no input follows the voice (Mic2 -49.8, Line In -61.7 dBFS, steady tones;
    mixer and AIF2 deliver nothing). The mics go to the baseband only.
    E3 (2026-10-29): yes. Keyed with the PTT key (monitor on, level 70), the audio scope
    shows the mic, internal and hand: while keyed, the codec capture carries the monitor.
    E4, keyed but no RF (TX enable off, as the TOT lock): `x6100_tx 14200000 micrec 10
    /tmp/e4.wav` (no RF), `--rf` (with RF, compare), `--monilvl 0` (is it the monitor?).
    If E4 is yes: RADE store-and-forward (PTT: record the mic, no RF; release: send the
    over as RADE via codec TX), receive on the PC.
    E4a (2026-10-29): no. With TX enable off the baseband does not key (tx=0): the capture
    is the RX audio (-21.5 dBFS steady, SSB RX filter edge at 3 kHz), no monitor. The mic
    reaches Linux only while really transmitting. Open: E4b (--rf, the bandwidth of the
    monitor) and E4d (--rf --micgain 0: SSB sends nothing without mic audio; does the
    monitor tap before the gain?).
    E4b / E4d (2026-10-29): keyed with RF the IC-7851 heard the mic, also with both mic
    gains at 0 (checked in x6100_trx, TX panel): gain 0 is a minimum, not a mute. The
    capture while keyed was not RX audio (energy 100-800 Hz, no 3 kHz edge): the monitor.
    Result: the mic reaches Linux only while it is being transmitted. RADE inside the
    radio is not possible (store-and-forward would send each over in analog first); RADE
    runs on the PC (wfview / wfserver + FreeDV, TX audio via DATA MOD USB, item 17).
    x6100_tx micrec: its power / ALC columns read 0.0 W / 100 while transmitting (wrong:
    the flow telemetry is not read in time while the loop waits for audio); see TODO.

## Next steps (suggested order)

1. Radio checks above; the 2026-10-27 evening checklist (scope panel, fixed edges, wfview
   X6100.rig, TX safety, messages, SWR scan) is done up to 5.4: TX safety and the CW
   messages verified, the rest open (items 10, 11, 13-15).
2. Network CAT and audio: wfview's wfserver on the radio over `cat pty` (decided
   2026-10-27): built 2026-10-28 (`contrib/wfserver`, self-contained bundle); run it
   on the radio and measure (item 17). Own Icom LAN server only as the fallback
   (`x6100_netaudio_design.md`).
3. liquid-dsp in use: network audio / RADE (Icom LAN server design in
   `x6100_netaudio_design.md`), recordings, RX / TX equaliser.
   RADE on the R16: benchmark ready (`contrib/rade`: rade_c built with zig, `rade_bench.sh`
   on the radio prints the real-time factor of V1 / V2 encode and decode); build on the
   PC and run it, then decide (2026-10-29). First run: encode RTF 0.42-0.54, decode
   1.08-1.21 at 1008 MHz on one core: TX fits, RX just misses. NEON yes, clock capped at
   1008 MHz; FARGAN and demod + decoder about 0.55 each: two-thread receiver rade_rx_mt
   built: 0.65-0.71 with a signal; V1 with no signal 1.49 (pilot search, scalar code):
   NEON did not help (A7 NEON no faster than VFP: 171 ms per search, frame 120 ms);
   rade_acq_fft.patch does the search with FFTs (13 instead of 98 million operations,
   same detection): search 170 -> 51.5 ms per frame. Result 2026-10-29: V1 encode 0.42-0.45,
   2-thread decode 0.61-0.65, no signal 0.48-0.52 (x6100_trx off / on): RADE V1 runs on
   the R16. RADE in x6100_trx itself is blocked by the audio routing (item 18); RADE via
   the PC (wfserver + FreeDV) is the way.
4. Measurements: S-meter calibration, codec level against ALC.
5. CTCSS for the repeaters (see TODO: no tone encoder known, mic bypasses Linux).
6. Later: own buildroot, self-contained (now the IU1WXY image is the base): a full image
   and an applications-only package (TODO, Image and distribution).

## Working notes

- Build: `bash build.sh` (pull, fingerprint, zig; liquid-dsp needs cmake and the
  submodule: `git submodule update --init --depth 1`). `bash build.sh deploy` to
  `$X6100_HOST:/root`, seeds missing settings / memories from the examples, and installs
  the wfserver bundle `out/wfserver-armhf.tar.gz` (`contrib/wfserver/container.sh
  <wfview>`: Ubuntu 24.04 in podman / docker as a normal user; or build.sh, run in
  out/) as /opt/wfserver when it changed.
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
