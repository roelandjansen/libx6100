# libx6100 / x6100_trx — status

Snapshot: 2026-11-14. Last tag: v0.17.16 (2026-11-14; remote head transmits:
--remote-tx-dummy, voice TX on the rig with the X6100 mic through a dummy load
(REMOTE TRX; the mic audio band-passed 300-2700 Hz and gated, LAN audio at 16 kHz;
mic AUTO acts as HAND; verified on an IC-7851), the rig's RF power under the TX box;
wfserver held off automatically while x6100_trx --remote runs, x6100_ui.sh trx
stop|start|restart; wfview followed on GitHub main (v3); X6200 at CI-V 0x63; the CW
decoder on the remote head too), on v0.17.15 (2026-11-12; CW decoder (scope panel CW
DECODER, decodes on the band); remote head: REMOTE RX note, CW-R, the rig's RF gain /
NR / NB / ATT / PRE back to the head, ID asked at the rig's address, own quick poll,
REMOTE: WAIT FOR RIG; X6200.rig and CI-V 0x63 reserved; baseband hang log; notes not
over panels), on v0.17.14 (2026-11-07; scope markers A / B / T as the
IC-7851 (MARKERS choice, B and T through the waterfall, no T where TX is refused); the
tune line in the waterfall and in 3D; the squelch mutes wfview too, SQL push; bbreset
without a console (MFK push, the command pipe /tmp/x6100_trx.cmd); screenshots (shot,
LOCK long); oob / nooob saved in the state; UK 60 m segments; the portrait frequency at
a fixed width), on v0.17.13 (2026-11-07; panels as a radio menu (MFK turn
selects, push switches or edits); scripts/x6100_giveaway.sh; own S-meter squelch with a
marker on the bar and the arc (verified in NFM); the TX audio source and OOB on the arc
meter in TX; LAN as TX audio input: wfview keys the network audio, the PTT key and the
hand mic the mic (verified, with X6100.rig LAN on PC and wfserver)), on v0.17.12
(2026-11-07; faster VFO tuning when the knob
turns fast (x2 / x5 / x10 on steps up to 1 kHz, GEN row VFO SPEED, verified); the
gimmick: a 3D waterfall, scope panel WATERFALL 3D (works)), on v0.17.11 (2026-11-07;
OpenX6X00 (ON6URE) as the base
image instead of IU1WXY (README: why); x6100_trx at boot via S96x6100_trx and
`x6100_ui.sh boot trx|gui`, quit brings the GUI back, power off does not), on v0.17.10
(2026-11-07; x6100_trx goes on when the ssh
session closes (SIGHUP); the birdie list 1024 entries (256 filled up on 6 m), the scan
threshold 16 dB, `birdiescan [all] [quiet]` (WiFi and Bluetooth off meanwhile); the WiFi
symbol (verified); the birdie comb of 7.58 kHz shown to be the radio's own (display,
supply, USB, WiFi, BT ruled out)), on v0.17.9 (2026-11-06; the start names the IP addresses and the
wfserver UDP ports (control, CI-V, audio, from wfserver.ini), again when an address or
the wfserver state changes), on v0.17.8 (2026-11-05; the VOL knob as VOL / SQL / RFG (VOL
long, back to VOL after 10 s), mute, volume and squelch free under LOCK; the portrait footer in
fixed columns: lock, RFG, SQL, VOL; the auto lock released on the way to landscape;
portrait, auto lock, frame rate and console help verified), on v0.17.7 (2026-11-05;
portrait verified on the radio
(rotation 180, photo in the README); layout portrait | landscape on the command line and
the console (also while locked); --help and console help), on v0.17.6 (2026-11-05; the
portrait layout for handheld use
(GEN LAYOUT, red TX line, arc meter, small scope, auto lock 20 s); baseband watchdog
(BASEBAND NOT RESPONDING) and `bbreset` / `--bbreset` over its GPIO reset line (works on a
healthy baseband); the VFO goes round at the range ends; 9 kHz step on LW / MW in any mode;
the AGC MID row dropped (AUTO is by mode in the baseband); the volume free under LOCK),
on v0.17.5 (2026-11-04; LAN and CAT indicators (verified);
tuning range 148.5 kHz - 75 MHz (`freqmin` / `freqmax`, RX and TX found working to 75
MHz); the 4 m band (band keys, regions nl and uk, X6100.rig 4m button); 10 and 100 kHz
steps; PRE key short = ATT, long = PRE; AGC MID / AUTO; the charging flag dropped
(not on 1.1.9); RADE deployed only when changed), on v0.17.4 (2026-11-03; the scope
frequency scale fixed: the
IQ is 100 kHz wide, not 96 (verified); AGC SLOW / FAST times and HANG, LINE IN / OUT as the
stock UI 1.2.0; CI-V 28 00 voice TX (messages M1-M8) and the clock from CAT (1A 05 0165 /
0166 / 0170, `cattime`); GEN rows saved 2 s after a change; battery 99 shown as 100 %, the
charging flag; Bluetooth state after a reboot verified), on v0.17.3 (2026-11-03; X6100.rig:
16 12 as AGC Status, not
AGC Time Constant; the ATU toast verified; x6100_civ_compare.md updated for memories;
Xiegu APP refactor and baseband protocol expected end of December, in TODO), on v0.17.2
(2026-11-02; band keys with GEN in the band order
at its frequency, ATU on shows the tuner memory in use, the wfview memory window works:
1A 00 group as wfview sends it, X6100.rig without the group and with the Memory Contents
string fixed (the IC-705 file has it wrong: patch for wfview made), CAT memories read only
by default (`catmem`), 60 m channels in the memories example), on v0.17.1 (2026-11-01; a
new README photo, the RADE mic requirement for a clean-room baseband in TODO), on v0.17.0
(memory mode Icom style, SCROLL-F pages, the fixed window painted with the LO near its
centre, edges at most 75 kHz and an edge panel, SCOPE OUT OF RANGE, HOLD from CI-V,
transceive of levels and switches, no freeze on a busy codec, the SWR scan readout, a
more patient start). Before: v0.16.0
(2026-10-31; CI-V on the DEV port and the pty at once, GEN row WFSERVER, clock, Bluetooth
headset autoconnect, no headset mic), v0.15.0 (2026-10-30; baseband IF shift, AM tone
gone, AM raster, VOX N/A), v0.14.0 (2026-10-29, on 030ff5f; wfserver, twin PBT, scope
panel, MSG / APP keys, TOT, RADE studies).
Tags are made by hand: they cannot be pushed from the cloud sessions.
Open work in detail: `TODO.md`. Use: `x6100_trx_quickref.md`. CAT: `x6100_civ.md`.

## Where we are

Radio: X6100 on the OpenX6X00 image (since 2026-11-07; before: the IU1WXY image), stock
baseband BASE 1.1.9. Why the IU1WXY image was left: README, "Why the IU1WXY image was
left" (0.2 has no ssh; the image refuses to start once a file is changed; it is closed).
`x6100_ui.sh stop` stops the GUI (x6100_gui in the x6100_daemon loop); `x6100_trx` then
owns the baseband. `x6100_ui.sh boot trx` starts x6100_trx at boot (S96x6100_trx).

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
- band keys with GEN in the band order at its frequency (GEN 5450 kHz: 60m, GEN, 40m;
  2026-11-02)
- wfview memory window with X6100.rig: all channels read, with names and the split TX
  side (2026-11-02, after the 1A 00 group and Memory Contents string fixes)
- ATU key on: the toast with the tuner memory in use (2026-11-02)
- Bluetooth state (GEN row, state `bt`) applied at start: off after a reboot (2026-11-03)
- LAN indicator with wfview over the network; CAT blank with only wfserver; battery
  CHARGING below 100 % (2026-11-04)
- PRE key short = ATT, long = PRE (2026-11-04)
- birdie scans: repeatable from 16 dB, the same with display off, battery, no USB, WiFi
  and BT off (2026-11-06 / 07); the WiFi symbol (2026-11-07)
- bbreset: the baseband resets over its GPIO line and comes back (2026-11-05, on a
  healthy baseband; on a real hang 2026-11-07: BASEBAND NOT RESPONDING, bbreset, back)
- portrait layout (rotation 180, GEN row and command line, auto lock, frame rate) and the
  console help (2026-11-05)
- tuning range: LW 198 kHz received (MW bleeds through at 77.5 kHz), RX and TX at 75 MHz
  (2026-11-04)
- scope frequency scale with the IQ 100 kHz wide: the carrier on the dial line with the IF
  shift at 10, 20 and 40 kHz (2026-11-03)
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
  - SCROLL-F: pages by the window width, the scope there at start; FIX: SCOPE OUT OF
    RANGE; TX in FIX / SCROLL-F on the dial frequency; edge panel; negative IF shift
  - Bluetooth headset (Sony WH-1000XM2): with WiFi connected the scan finds nothing
    (rtl8723bu coexistence); paired with WiFi briefly off, then A2DP plays the RX audio
    (IU1WXY's btaudio-ctl loopback) with WiFi on. No headset mic: HFP with the native
    pulseaudio backend sets up eSCO and a source, but no samples arrive, neither mSBC
    (16 kHz) nor CVSD (8 kHz) (scripts/x6100_bt_hfp_test.sh): RADE TX stays on the PC
  - Bluetooth headset: connect / disconnect on the GEN row BT DEVICE and autoconnect
    (`btauto`: after Bluetooth off / on, the headset off / on, a restart) work
  - CI-V on the DEV port next to the pty: rigctl (IC-705 model 3085, 19200, civaddr 61)
    on the USB cable sets and reads the frequency while wfview runs over wfserver
  - memory mode (V/M key and wfview's V/M): to M1 and back to the VFO as it was
  - CI-V transceive of levels and switches: wfview follows NR / NB set on the radio
- radio test of 2026-11-01:
  - HOLD from wfview freezes the radio's scope and waterfall, the meters run on
  - audio scope with wfview connected: AUDIO SCOPE: CODEC BUSY, no freeze
  - SWR scan: the frequency readout follows the scan step
  - lost I2C writes: taken care of (no false stuck-TX lines since 2026-10-27; the power
    cut on a TX stuck after a write error stays in the code, it cannot be provoked)
  - long run (2026-11-02): about 24 h untouched with wfserver / wfview connected, no
    crash or hang; five I2C NAKs in that day, each recovered by the resend (once after
    a second round, 0.0 s), no dropped or stuck TX; a tune on 60 m in between
  - start: "calibration invalid" and no start until a reboot (seen once); now retried
    10 times over 5 s, and refused while another program drives the baseband (to see)

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
9. wfview with CI-V memories: read works (X6100.rig, 2026-11-02); still write, recall,
   clear with `catmem rw`, NG for both with `catmem ro` (the default), and the IC-705
   file on A4 once the Memory Contents string is fixed there (patch made); and with
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

19. (done) ATU key on: the toast with the tuner memory in use (kHz, L, C, S), or ATU ON: NO
    MEMORY away from any memory: verified 2026-11-02.

20. CI-V 28 00 (voice TX) from hamlib: `\send_voice_mem 1` in USB, `\stop_voice_mem`.
21. Clock from CAT: wfview Set Clock with the clock amber (no WiFi after a cold start),
    Use UTC on and off; with NTP synced NG. Does the IU1WXY image mark the clock synced
    without network (then `cattime on`)?
22. AGC SLOW / FAST / HANG audible; LINE IN with wfserver TX audio (DATA MOD USB).
23. (done) Battery: flow bit 4 is not a charging flag on BASE 1.1.9; CHARGING below 100 %.
24. Birdie scan again (the old list was found with the 96 kHz scale).
25. Hand mic keys: log the flow `hkey` while pressing F1 / F2 (stock HANDLE F1 / F2).
26. (done) LAN / CAT indicators: LAN with wfview over the network and CAT with a client
    on the DEV port verified 2026-11-04 / 2026-11-06.

27. 4 m: the band key and the X6100.rig 4m button in wfview work (2026-11-04); still
    receive on 70.0-70.5 MHz; TX only after a spectrum
    check (above the 6 m low-pass filters).
28. FST cycle 10 / 100 / 1k / 5k / 10k / 100k. (AGC AUTO: by mode in the baseband, our time
    constants do not change it, 2026-11-04.)
29. Region uk: add the UK 60 m segments (Ofcom licence terms).

30. (done) Portrait: rotation 180, scope and waterfall, the GEN row and the command line,
    the auto lock, the frame rate, and landscape while a panel is open (portrait again
    when it closes or times out) verified 2026-11-05.
31. (done 2026-11-07) A real baseband hang: BASEBAND NOT RESPONDING, `bbreset` brought it
    back. The red box then scrolled down with the new waterfall lines: the waterfall is
    now cleared when the baseband answers again. What ran before the hang: not noted.

32. (done) VOL knob: VOL / SQL / RFG switching, the fall-back to VOL after 10 s, mute (also
    locked) and the portrait footer verified 2026-11-05. Locked, the knob does VOL and
    SQL (VOL long switches between them; RF gain stays locked), verified 2026-11-05.

33. (done) Network line at the start: address and ports right, verified 2026-11-06.

34. OpenX6X00 as the base (2026-11-07): the first start (calibration, CAT, network, the
    ntp line: no S48sntp / S49ntp, x6100_trx syncs itself), the codec next to PulseAudio
    (F3 audio scope, a voice message). Verified: the boot (the GUI first via S95gui, then
    S96x6100_trx switches to x6100_trx), PWR 1.5-4 s (quit) brings the GUI back, PWR held
    4 s powers the radio off without the GUI (the marker /tmp/x6100_trx.poweroff).
35. Faster VFO tuning and the 3D waterfall (2026-11-07). Verified: VFO SPEED (x2 / x5 /
    x10 from 12 / 25 / 45 detents per second, the thresholds right for the knob), the 3D
    waterfall works. Still to check: top for the CPU with 3D at speed FAST, 3D in
    portrait and with F3 long; a photo for the README.
36. Panels as a radio menu (2026-11-07; first test: better for setting things): in GEN, KEY, scope, TX, RIT / AGC, DSP, filter
    and edge MFK turn selects the row, push switches an on / off row or starts editing
    (amber frame, MFK adjusts), push again ends it; hold default as before. To check:
    each panel, a toggle row (WATERFALL 3D, WIFI, RIT), an edit row (WF PEAK LEVEL, CAT,
    RIT offset), the filter in AM / FM (rows skipped), the edge panel, an edit left open
    when the panel closes (next time not editing).
37. Give-away cleanup (2026-11-07): scripts/x6100_giveaway.sh (busybox sh) removes what
    is personal or this radio's own (x6100_trx state / conf / birdies / memories /
    messages / voice, wfserver.ini, WiFi networks, BT pairings, authorized_keys,
    known_hosts, history; -k host keys, -l logs), lists and asks first. Tested on the
    PC (dash and busybox sh, a WiFi file with spaces). -n on the radio: ok (2026-11-07). To check: a run
    after a backup; with -k that ssh comes back with new host keys; the
    OpenX6X00 GUI settings (callsign) still by hand.
38. Squelch (2026-11-07): the baseband's threshold (21 [15:8]) did nothing in NFM; now
    x6100_trx's own S-meter squelch in every mode: SQL = the position on the S bar (40
    about S6, 60 S9), muted below it (opens at once, closes after 0.3 s), a marker on
    the bar and the arc meter (amber closed, green open), CI-V 15 01 reports it.
    Verified 2026-11-07: NFM squelch and the markers. Still to check: SSB, wfview's
    squelch status.
39. TX audio source on the arc meter (2026-11-07; the internal mic was silent: TX panel
    MIC on HAND): while transmitting, top left of the dial MIC INT / AUTO / HAND (red),
    USB, CW, MSG, TUNE, landscape and portrait. To check: PTT key with MIC AUTO and
    HAND, a DATA mode with USB, CW, a message. OOB now also in TX (under the source) and
    in portrait: verified 2026-11-07.
40. LAN as TX audio input (2026-11-07): GEN DATA OFF MOD / DATA MOD MIC / USB / LAN;
    LAN: CAT PTT (wfview over wfserver) sends the codec (network audio), the PTT key and
    the hand mic the mic, so local and remote work without switching. CI-V 1A 05 0118 /
    0119: 03 = LAN (IC-705 WLAN), X6100.rig has LAN as third input (mkrig.sh). The arc
    meter shows LAN or the mic. To check: wfview voice TX in USB with LAN (PC audio out,
    LAN on the meter), then the PTT key (mic); wfview's input menu shows LAN and sets it (Data Off, not Data 1).
    2026-11-07: WLAN came out as 00 (MIC): wfserver on the radio builds the CI-V from its
    own X6100.rig (old, no WLAN); build.sh deploy now updates that copy too. The input
    is now named LAN in X6100.rig. Verified 2026-11-07: wfview Data Off = LAN, voice TX
    from wfview works, LAN on the meter; the hand mic on LAN sends the mic. First come,
    first served: a LAN TX stays LAN while the hand mic PTT is pressed, a hand mic TX
    stays the mic while wfview keys. A wfview reconnect sends 18 03 / 19 03: LAN stays
    (the earlier 18 00 at connect was wfview's MIC choice then). Done.
41. UK 60 m and the tune line (2026-11-07): region uk has the 11 UK 60 m segments (TX
    check, OOB, FST CH through their lower edges; regions now hold up to 24 segments).
    Tune line: the VFO frequency as a red line through the waterfall while tuning, 10 s
    after the last change (pixels under it restored each frame: no trace left, tested on
    the PC). To check: region uk on 60 m (TX in / between segments, CH), the tune line in
    CENTER and SCROLL modes, with split / RIT, in portrait, with F3 long. 2026-11-07: in
    3D it seemed to scroll away: that was the 3D cursor, 3D had no tune line; now the
    tune line runs across the 3D floor too, and on up to the scope (verified, then
    lengthened). Scope markers M / S / T as the IC-7851 (verified); dashed lines in solid
    colour (half transparent was too faint), the gaps show the signal. Squelch from
    wfview: the squelch muted the speaker but not wfview (the codec capture is before
    the RX volume); Mic1 capture off did not do it (wfview still open); now the digital
    AIF1 ADC capture switch goes off while closed: wfview silent, verified. SQL both ways verified:
    wfview's SQ slider moves the radio's marker, the radio's SQL moves wfview's slider
    (2026-11-07).
42. bbreset without a console (2026-11-07; started at boot there is no console): MFK push
    while BASEBAND NOT RESPONDING shows does the bbreset (the box says so), and the
    command pipe /tmp/x6100_trx.cmd takes console lines over ssh (`printf 'bbreset\n' >
    /tmp/x6100_trx.cmd`), replies in /tmp/x6100_trx.log; bbreset also while locked. The
    waterfall is cleared when the baseband answers again. The pipe verified 2026-11-07
    (vol 30) and bbreset by pipe (2026-11-07). MFK push on a real hang verified 2026-11-09:
    the baseband came back, the box went (the waterfall cleared). Done.
43. SQL push (2026-11-07): with the VOL knob on SQL (VOL long) a push sets the squelch just
    above the noise now (the highest bar position of the last second, + 2; toast SQL nn);
    on VOL it still mutes. Verified 2026-11-07.
44. Screenshot (2026-11-07): `shot` (console, command pipe) or LOCK long writes the screen
    as seen as a PNG to /tmp/x6100_shot_<UTC>.png (uncompressed, no zlib; tested on the
    PC: identical to the canvas, landscape and portrait). LOCK short now acts on release.
    Verified 2026-11-07: a real screenshot from the radio (GEN 5.450, A B T markers).
45. Markers as the IC-7851 MARKER key (2026-11-07, from photos of it): scope panel row
    MARKERS A / A B / A T / A B T (default A B T), `markers a|ab|at|abt`; tagged A = VFO A, B =
    VFO B (the IC-7851 M / S; before: the VFO in use and the other), T = TX; B and T dashed through the
    flat waterfall (no trace left, tested on the PC). No legend chips (the scope top is
    full). No T where TX is refused (outside the allocations without oob). To check:
    each MARKERS value, A/B swap (A stays on VFO A), split. Verified 2026-11-07: no T on
    GEN 5.450 without oob; nooob switches the band check on again at runtime. oob / nooob
    now saved in the state (the conf wins, --oob over both): both survive a restart
    (verified 2026-11-07).
46. Baseband hang log (2026-11-09): every hang appends a HANG line to
    /etc/x6100/bb_hangs.log (time, run time, frequency / mode / band, TX, wfview /
    wfserver / CAT, supply and battery, I2C retries) and a BACK line when the flow is back
    (how long, reset by bbreset from console / pipe / MFK push, or by itself). Tested on
    the PC; x6100_giveaway.sh removes it. To check: the lines at the next hang.
47. Remote head (2026-11-11 / 12, another session; handover 2026-11-12): `x6100_trx --remote
    <host> <udp port> <user> <password> [--rigs <dir>]` makes the X6100 a receive-only
    control head for a remote Icom (IC-7851) through wfserver or its LAN port; code in
    tools/x6100_remote.c / .h, libx6100_app core as the submodule third_party/libx6100_app
    (out/libx6100_core.a, -DX6100_REMOTE). The baseband only plays the audio (play mode,
    TX enable off, TX power 0). Verified on the radio 2026-11-11: frequency and VFO both
    ways, mode / filter, S meter, RX audio (16 kHz stream, SCHED_FIFO audio thread: no
    crackle), scope and waterfall from the 7851 (centre +- 50 kHz), the REMOTE RX note.
    UDP in the port names (48bad70). Open points: TODO, Remote head.
    Tested 2026-11-12: REMOTE RX stands, VFO / A-B both ways, S meter, audio clean,
    scope the same, PRE (PRE1), ATT (off / 12 dB), RF gain, NR, NB from the head work.
    Fixed then: CW-R from the head gave RTTY-R and the 7851's CW-R was not seen (wfview
    mode Num 6, not the CI-V 7); RF gain, NR, NB, ATT, PRE and AGC changed on the 7851 now
    come back to the head (AGC only once at connect: not in the IC-785x poll list).
    Later that day the scope and controls worked again (the earlier failing run: no CI-V
    ID from the 7851 for 10 s, not seen since). AGC head -> 7851: SLOW slow, FAST fast,
    AUTO mid, OFF stays mid (Icom AGC Status has FAST / MID / SLOW only). REMOTE RX was
    drawn over the status page and panels: no longer while one is open. Short notes over
    REMOTE RX come and go. CW / CW-R both ways verified. From the 7851 RF gain, NR, NB,
    ATT (18 dB seen) and PRE (PRE2 seen) arrive (log lines "remote: rig ..."); NR / NB
    and RF gain now polled by the head about once a second (the rig file: 10 s, NR / NB
    for both receivers into one field, which flips them). The ID is now also asked at the
    login address (0x8e): some runs had CI-V frames but no ID, so no rig file. To check:
    the rig file loads every time, RF gain on the head follows within a second, from
    the 7851.
    Start soon after the last session (2026-11-12): the 7851 does not answer its CI-V
    ID for about 90 s (its LAN indicator goes off in seconds, its CI-V frames do come;
    the core closes every stream as wfview does); it heals by itself in the same session.
    The scope note says REMOTE: WAIT FOR RIG meanwhile. To compare: wfview disconnect
    and reconnect at once, does it wait too?
    Once (2026-11-12, after many quick restarts): only white noise on the head's speaker,
    the stream running (no xruns, overruns climbing); a power cycle fixed it. Next time
    first: x6100_mixer list (playback / DAC / mixer routes) to compare.
48. CW decoder (2026-11-12): scope panel CW DECODER, `cwdec on|off`; in CW / CW-R the
    codec audio (as the F3 scope; codec busy: tried again every 10 s), Goertzel on the
    keyer PITCH in 8 ms blocks, noise floor and peak, dit length followed (5-50 WPM); the
    text and WPM at the foot of the RF scope, each word on the console / log. PC tests:
    6-35 WPM decode (first letter while locking), 45 WPM after a few letters, noise only
    nothing. On the band 2026-11-12: decodes (PA3ME, 14 WPM, S9+10: CQ TEST ... DE
    PA3ME), stray E / T while it locks on at the start. To do: that start, QSB, a second
    signal nearby. As a remote head (2026-11-12): fed from the stream (the audio thread's
    left channel through a small ring) at the rig's CW pitch (CW Pitch in the head's own
    poll): works (2026-11-12).
49. Remote TX through a dummy load (2026-11-14; `--remote-tx-dummy` with `--remote`): the
    X6100 keys itself at 0.1 W into a DUMMY LOAD on its antenna port, its monitor (the
    mic, processed) comes back on the codec capture and goes to the rig as TX audio; the
    rig keys 150 ms later over LAN (heartbeat 500 ms, rig time-out 180 s) and lets go
    first. Fixed on applying: codec busy left the X6100 keyed with the rig never keyed
    (now dropped, REMOTE TX: CODEC BUSY). Test plan (first both radios into dummy loads):
    a. Before: X6100 dummy on its antenna port; the 7851's DATA OFF MOD (SSB) on LAN, else
       it sends its own mic; wfserver off on the X6100 (the codec; automatic since 2026-11-14:
       held off while x6100_trx --remote runs); note oob in the state.
    b. Start with --remote-tx-dummy: the note reads REMOTE TRX (was REMOTE, TX VIA DUMMY 0.1 W), also while we transmit (the TX LED says it); REMOTE TX only when another client keys the rig. Fixed 2026-11-14: the note was gone after PTT until a few more PTTs (PTT clears the note; only a link change set it again).
    c. PTT while REMOTE: WAIT FOR RIG: refused (REMOTE LINK).
    d. PTT held and tapped (latched): X6100 TX at 0.1 W, the 7851 keys ~150 ms later; its
       PO / ALC follow the voice. Mic gain (TX panel) for the 7851's ALC; listen for
       clipping (Mic1 boost +24 dB in SSB) on a second receiver.
    e. Release: the 7851 first, then the X6100; no tail of carrier.
    f. Link lost while keyed (WiFi off): TX dropped (REMOTE LINK), the rig released by the
       heartbeat within 0.5 s. Quit while keyed: rig first. kill -9 while keyed: the rig
       lets go (heartbeat); check the X6100 baseband is not left keyed.
    g. wfserver running, PTT: REMOTE TX: CODEC BUSY, nothing keyed.
    h. SQL above the noise: the TX audio still goes. USB, LSB, AM, FM; CW (not meant for
       it: see what happens). Hand mic PTT: nothing should key (TX enable off unkeyed).
    i. Hold over 180 s: the rig's time-out; the X6100's TOT.
    j. Without --remote-tx-dummy: receive only as before, PTT refused (REMOTE RX).
    k. Speaker muted while keyed (added 2026-11-14): no monitor on the X6100 speaker, no
       feedback with the internal mic, and the TX audio to the rig still there (the
       capture sits before the RX volume: confirmed 2026-11-14, n); the RX audio back after release.
    l. Under the TX box (added 2026-11-14, after "5W shown"): the rig's RF power setting
       in percent (Icom 0-255, polled by the rig file), -- until known; changing it on
       the 7851 changes it here within a few seconds. Not the X6100's 5.0 W.
    m. Hand mic PTT (2026-11-14): does not key, the PTT key on top does. Logged: the
       baseband reports nothing for it with TX enable off (flow TX flag and hand mic key
       code stay 0), so it cannot be a PTT source here; the log is removed again. The
       hand mic as microphone works (meter follows) when keyed with the PTT key.
    n. No audio at the 7851 (2026-11-14: it keys, DATA OFF MOD LAN, its MONI silent; the
       0 % under the TX box with 200 W set). Logged now: while keyed, once a second
       `remote TX: capture .. dBFS, sent .. packets ..` (the capture level here and what
       the session sent), and `remote: rig RF power N (0-255)` on every change (RF Power
       added to our own poll). Console `notxdmute` keeps the speaker on while keyed:
       does the monitor (and so the TX audio) come back without the mute (k)? Raise VOL
       for it (the monitor may follow it); hand mic, against feedback.
       Result 2026-11-14: the capture carried the monitor (-19 to -33 dBFS), about 100
       packets a second went out, but every block was silent (peak -120 dBFS): the
       session's TX gain (profile mic_gain) was left at 0 by the memset. Fixed: 100 %.
       The capture level was the same with notxdmute: the mute (k) does not touch the
       TX audio, so it stays on (k confirmed). RF power: right after RF Power was added
       to our poll. To test again: the 7851 hears the voice (d).
    o. TX audio quality (2026-11-14: it arrives, but noisy and hardly understandable;
       the capture sits at about -33 dBFS without speech, the voice peaks about -20: some
       13 dB above the noise). Console, live while keyed: `txdboost 0` first (Mic1 +24 dB
       overdrives the codec input on AM, maybe on the monitor too), then `txdmoni` up
       (voice above a fixed noise), `txdgain` for the level at the rig (its ALC). Compare
       the capture level without speech and the voice peaks in the `remote TX:` line, and
       how the 7851 sounds. Open as well: the TX stream is LPCM 48 kHz (about 770 kbit/s
       over the internet); choppy rather than noisy would point there (16 kHz as RX).
       Result 2026-11-14: the earlier tests ran on the internal mic (MIC AUTO: the hand
       mic only with its own PTT, which cannot key here; now AUTO acts as HAND in this
       mode). Hand mic: txdboost -1 / 1 no difference; txdmoni 100 the best voice over
       the crackle; txdgain above 1-3 % overdrives the 7851 (LAN input 100 %; at 50 %
       and txdgain 3 about 150 W PEP). Still a crackling noise under the voice. Some
       30 dB more level than the -20 dBFS peaks suggest: the stream format (codec, rate)
       is now in the log line; console txdrec records what is sent (/tmp/x6100_txd.wav):
       crackle in the recording is the monitor / capture side, clean is the way to the rig.
       Recordings 2026-11-14, mic unplugged, no speech: txdrec (X6100, -51 dBFS) and the
       7851's own (16 kHz, -43.5 dBFS): the same noise at both ends, the same spectrum
       (strongest 50-150 Hz, falling to -25 dB at 3 kHz), the same pops (about 12 a
       second at random intervals, not on the capture's 1024-frame or the 20 ms block
       boundaries). No gaps, no clicks, no pitch shift: the way to the rig is clean; the
       crackle is made in the X6100's analog path (monitor, baseband audio out or codec
       input). Next: txdmoni 0 with txdrec (gone: the monitor / TX chain; still there:
       the audio out / codec input); the X6100 compressor off.
       txdmoni 0 (and boost 0), mic unplugged: the same noise and pops (-51 dBFS, about
       15 a second): not the monitor; they are on the baseband's audio out to the codec
       or in the codec input. Next: the voice far above that floor before the codec
       (the TX panel LINE OUT level, moni 100), else a noise gate on our side.
       TX stream since 2026-11-14: 16 kHz (was 48 kHz) as RX; the core resamples.
       LINE OUT 10-36: no difference heard. So the TX audio is cleaned before it is sent:
       band-pass 300-2700 Hz (on recordings: noise -10 dB, pop peaks -8..9 dB, the voice
       band untouched) and a gate at -40 dBFS after the filter, 20 dB down, hold 250 ms
       (the code run on the no-mic recording: -51 to -87 dBFS without speech, a 1 kHz
       tone at -23 dBFS passed unchanged, the pops never opened it). Console txdbp
       (toggle), txdgate N (0 off). To hear on the 7851; txdgain 3-5 at LAN input 50 %.
       First test with the gate (7851 in NFM): sent -80..-90 dBFS without speech, still
       crackle heard at the 7851: not in our stream any more. Open: the same with the
       7851 in SSB, and keyed from wfview (LAN, silent) or its own PTT without mic: the
       7851's own chain or RF into its audio?
       Correction (2026-11-14): it is ours (gone at txdgain 0; 7851 under 5 W, MONI; also
       in SSB and CW). The log showed only the last 20 ms block; the pops passed the gate
       at -20 dB, about -69 dBFS, and the 7851 takes our level some 30 dB hot (txdgain 100
       in that test). The gate now mutes (-60 dB; pauses -118 dBFS on the recording) and
       the log shows the loudest sample sent each second (after gain).
       Verified 2026-11-14: works. Pauses -133..-146 dBFS sent, the voice -44..-48 dBFS
       after txdgain 3, the 250 ms tail -70..-75. Defaults since: txdmoni 100, txdgain 3
       (the 7851, LAN input 50 %); the log line only with console txdlog.
    Blind spots: the monitor (level 70) also played on the X6100 speaker while keyed
    (now muted, k); CW is not handled;
    the hand mic PTT cannot key (m: not reported); a real antenna instead of the dummy radiates 0.1 W on
    the rig's frequency (with oob anywhere); the audio level path is fixed (TXD_MONI).

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
6. Later: own buildroot, self-contained (now the OpenX6X00 image is the base): a full image
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
