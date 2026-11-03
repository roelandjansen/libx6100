# libx6100 / x6100_trx — to do

Status: 2026-11-03 (v0.17.4). Summary and next steps: `STATUS.md`.

## Display

- Clock: amber while the time is not known (NTP failed / off / running, or before 2026),
  console `time` to set it by hand (built and verified 2026-10-31: grey after start, time
  works). Later
  maybe: a GPS (NMEA on USB) as time source in the field.

## Radio

- Band keys (2026-11-02): GEN sits in the band order at its frequency (5450 kHz: 60m,
  GEN, 40m; was after 6m); verified on the radio 2026-11-02.
- wfview memory window (2026-11-02): as IC-705 (A4) 1A 00 takes the group as two bytes
  (gg gg cc cc), as wfview sends it; as X6100 (61) no group (X6100.rig MemFormat without
  it: wfview sent no group and timed out). The IC-705 file has Memory Contents as
  \\x1a\\00, so wfview sent 1A 5C 30 30: fixed in X6100.rig; patch for wfview made (to
  send upstream). Writes and clears from CAT only with catmem rw (GEN row CAT MEMORIES;
  default read only: NG). Read all verified 2026-11-02 (X6100.rig). To check on the
  radio: with rw write one, clear one; with ro both NG.
- CI-V 28 00 (voice TX, 2026-11-03): 01-08 sends message M1-M8, 00 stops; for hamlib
  `send_voice_mem` (wfview has no control for it). To check on the radio: `rigctl -m 3085
  -r /tmp/x6100-civ` then `\send_voice_mem 1` (a voice message in USB) and `\stop_voice_mem`.
- GEN rows saved 2 s after a change (2026-11-03; before only at exit: a GEN BLUETOOTH
  OFF was lost once). `bt off` in the state after a quit and Bluetooth off after a reboot: verified
  2026-11-03. Still to check: switch a row, wait 2 s, kill x6100_trx: the state has it.
- Scope frequency scale (2026-11-03): the IQ is 100 kHz wide, not 96 (OpenX6X00 and the
  stock UI: FFT SPAN 100k). With 96 a carrier was drawn 4 % of its distance from the LO
  short: with the IF shift the station sat left of the dial line (1053 kHz AM: -500 Hz
  at ifshift 10000, -614 at 20000, -1561 at 40000; on the line with ifshift 0). SPAN_HZ
  100000 now (also x6100_iqcal). Verified 2026-11-03: the carrier on the dial line at 10 / 20 / 40
  kHz shift (a few Hz at most). Birdies found before this have their frequency off by up to 4 % of their
  distance from the scan centre (up to about 2 kHz): scan again (GEN BIRDIE SCAN).
- LAN and CAT indicators under the scope clock (2026-11-03): LAN while a wfview client is
  connected through wfserver (UDP rate from /proc/net/snmp, wfserver running), CAT while
  CI-V frames arrive (3 s; the pty only without wfserver, which polls it with no client
  too: CAT was on with no client, 2026-11-03). To check: LAN comes when wfview connects over the network and
  goes about 3 s after it disconnects; CAT with rigctl on the DEV port; no LAN with only
  NTP / DNS traffic.
- AGC SLOW / FAST times and HANG (AGC long panel, rows 7-9) and LINE IN / LINE OUT
  (TX panel), as the stock UI 1.2.0 (2026-11-03): registers 28, 27 bit 12, 20 [15:0].
  To check on the radio: SLOW 250 vs 50 audible on a fading signal, HANG, LINE IN with
  wfserver TX audio (DATA MOD USB: ALC / power follow it).
- Battery (2026-11-03): the gauge reads 99 when full (stock UI and IU1WXY show 100):
  shown as 100 now. Flow flag bit 4 ("charging" in X6100Control) tested on the radio: it
  stayed 0 while charging from 98 to 100 % on external power, so not used; CHARGING below
  100 % on external power, else FULL, as before. Verified: 98 % CHARGING, then FULL 100 %.
- Hand mic keys (to do): the flow frame carries them in `hkey` (X6100Control: F1 0x48,
  F2 0x44, UP 0x84, DOWN 0x04, V/M 0x81, 0-9, MODE, FIL, ...); the stock UI 1.2.0 lets
  HANDLE F1 / F2 do PRE, NR etc. First: log hkey on the radio while pressing them.
- Clock from CAT (2026-11-03): 1A 05 0165 / 0166 / 0170 (wfview Set Clock, set radio time
  on connect) set the Linux clock while NTP does not keep it (cattime unsynced, the
  default; GEN row CAT CLOCK). To check on the radio: no WiFi after a cold start (clock
  amber), wfview Set Clock: the clock right and grey, status page "set by CAT"; with NTP
  synced: NG in the wfview log, the clock unchanged; Use UTC off (local time) and on.
  Open: does the IU1WXY image (S48sntp / S49ntp) leave the kernel flag unsynced without
  network? If it marks the clock synced anyway, CAT is refused there (cattime on then).
- ATU key on (2026-11-02): a toast with the tuner memory in use (kHz, L, C, S) or
  ATU ON: NO MEMORY; verified on the radio 2026-11-02.
- TX ranges config file and transverter profile (frequency offset, TX enable).
- Repeater duplex (AUTO / OFF / DUP- / DUP+, AGC long): test on the radio (TX on
  29.520 from 29.620 verified with the PTT key and the hand mic; still: CAT, memories,
  CI-V 0F 1x).
- CTCSS for the repeaters: no tone encoder known in the baseband, and the microphone
  goes to the baseband directly (PTT + codec audio does not mix). To look for: an
  unmapped bit in registers 21 / 36 / 38 (stock UI / IU1WXY trace in FM), or a
  baseband with a sub-audible tone; else codec TX with Linux mic capture (none yet).
- Birdies: scan, store, scope ticks and automatic notch built (2026-10-25, own design;
  the idea from the IU1WXY v0.128 release notes: their software is closed and its
  terms forbid reproducing or imitating its screens). Host simulation of 80 m with the
  list below: all 7 found within 3 Hz, no false hits. To check on the radio:
  - the scan on 80 m against this IU1WXY reference scan of the same radio (2026-10-25,
    antenna off): 3773.987 (-122 dB), 3833.664 (-118), 3834.641 (-118), 3837.014
    (-96), 3840.834 (-112), 3908.134 (-116), 3959.191 (-120); 9 found, 7 in the photo
  - whether the IQ centre is the dial frequency exactly (an offset would shift every
    birdie and the notch) and whether the yellow tick sits on the scope line: the scope
    mapped bins as n - 1 - i, the scan as n - i: a host simulation (tones on exact bins,
    both sides) shows n - i is right; the scope uses it since 2026-10-27 (was 187.5 Hz off)
  - the notch: 3837.014 in LSB at dial 3838.000 = 986 Hz; does a 40 Hz notch take it
    out, how far off may it be; then BIRDIE NOTCH on as the default
  - birdies with PRE / ATT, and drift with temperature (scan again later)
  - OpenX6X00 (Guru-RF, LGPL-2.1, github.com/Guru-RF/OpenX6X00, src/birdie.c) has an
    open Birdie Cal: two snapshots per step (a peak must be in both), 14 dB over the 25th
    percentile, notch only in USB / LSB on the birdie nearest 1 kHz audio (not the
    strongest). Worth taking: the nearest-the-middle choice; compare scans on the radio.
  - first scan on the radio, 20 m (2026-10-25, x6100_trx): 14035612 (15 dB),
    14042891 (24), 14050168 (13), 14057446 (21), 14064726 (43), 14079275 (22),
    14086552 (26), 14093832 (19), 14101110 (24), 14126967 (10), 14161026 (16),
    14174899 (14), 14182106 (13), 14186699 (26), 14207901 (12), 14245300 (19).
    14035-14101 kHz is a comb about 7.278 kHz apart (one oscillator's harmonics;
    14072.0 is missing, below 10 dB); spacing steady to a few Hz
  - next: pick a strong birdie that is also clearly seen / heard on the radio, then
    tick on the scope line, its audio pitch on the audio scope, the notch and its
    tolerance (as for 14064726: USB dial 14.064.000 = 726 Hz)
- Memory mode, Icom style (2026-10-31, for wfview's V/M button, which did nothing):
  V/M key and CI-V 08 keep the VFO and tune a channel (empty: the next stored), V/M
  again and CI-V 07 / 07 00 / 01 put the VFO back; M and the channel on the dial; 0A
  copies the channel into the VFO; the panel and 09 write the kept VFO; the state keeps
  the VFO. Verified 2026-10-31: V/M goes to M1 and back to the VFO.
- AM snaps to the channel raster on the mode keys (5 kHz SW, 9 / 10 kHz MW; built
  2026-10-30): to check on the radio. (The AM tone off the carrier is solved by the IF
  shift: STATUS.)
- AM PBT and the filter panel PBT box (2026-10-29): to check on the radio (PBT in USB
  from wfview verified). AM passband now: upper edge at Icom's H, lower 2 x offset
  inside; the RF scope shows where it sits.
- Start (2026-11-01): "calibration invalid, using defaults" and x6100_trx did not start
  until a reboot: the baseband did not answer on I2C. Now the calibration read and the
  first refresh are tried 10 times over 5 s, and x6100_trx refuses to start while
  another program drives the baseband (IU1WXY / R1CBU GUI, a second x6100_trx). To see
  when it happens again: the console lines (calibration read / refresh, again n/10).
- SWR scan works on the radio (2026-11-01); the frequency readout follows the scan
  step (it showed the dial): verified.
- 2026-11-01: HOLD from wfview (27 17) now freezes the radio's scope and waterfall (was:
  only the CAT stream to that client), console hold, reported by transceive; and the
  audio scope (F3 / F3 long) froze the display when another program held the codec
  capture (a blocking open): x6100_pcm_open no longer waits, AUDIO SCOPE: CODEC BUSY.
  HOLD verified (the meters slowed at first: fixed, the screen redraws every frame and
  only the waterfall line stops). Codec busy: no freeze, AUDIO SCOPE: CODEC BUSY (it was
  cut short by the labels on the right: the toast is drawn after them now), verified.
- wfserver dropouts: Bluetooth on costs WiFi airtime on the shared rtl8723bu (one
  antenna, the vendor driver's coexistence); with Bluetooth off the dropouts went
  (2026-11-02). x6100_trx now keeps the BLUETOOTH row (state bt) and applies it at
  start: verified 2026-11-03 (bt off in the state, Bluetooth off after a reboot).
- wfserver load measured 2026-10-31 (wfview connected, scope Fast, 20 m): wfserver 33 %
  of the CPU (about 1.3 of the 4 cores), x6100_trx 13 %, 52 % idle, load 1.6-1.8,
  1008 MHz; no dropouts that day. Watch for dropouts again; if they come: top -H per
  thread, wfview RX codec / rate / latency, scope Speed. Open: why the combined scope
  mode fails (WaterfallFormat 0 works).
  GEN row WFSERVER (2026-10-31): off works; on / start brings wfserver up, but wfview
  sometimes needs up to three connects before it works, with nothing changed on the
  X6100: a wfview reconnect / recovery matter, for the wfview project (Phil, Elliott,
  Roeland). CI-V on the DEV port next to the pty verified (rigctl on the USB cable
  while wfview is connected over the network, 2026-10-31).
  wfview NR / NB sliders: they do send (14 06 / 14 12 in the --cat-log, accepted; the
  first test missed the connection after an x6100_trx restart). On the radio the NR
  level sounds almost like on / off: the baseband's NR. wfview did not follow NR / NB set
  on the radio: transceive now also reports levels and switches (2026-10-31): wfview
  follows now (verified).
- S-meter calibration (signal generator): SMETER_CAL_DB is provisional (-21: -15 from
  an IC-7851 comparison, then 6 dB lower); also the overload point with PRE / ATT.
- Codec level vs ALC test (x6100_tx codec at -40 / -30 / -20 dBFS).
- Fixed scope edges (IC-705 style, built 2026-10-27; work well on the radio, 2026-10-31): F2 / F2
  long in FIX and SCROLL-F, the scope label, a band change moves the window, edges kept
  over a restart; wfview fixed edge window (27 16 / 27 1E set and read), a 200 kHz edge
  from wfview cut to 96 kHz (75 with the IF shift). Edge panel (F2 long: lower / upper,
  1 kHz steps, as an Icom; verified 2026-10-31). Open: whether
  wfview sends 27 1E for a range we do not cover (above 60 MHz).
  SCROLL-F now pages by the window width on the edge grid, as an Icom (2026-10-31,
  was a sliding window): to check on the radio. At start SCROLL-F showed no scope
  until the VFO moved (the window not yet on the VFO page): fixed, the page set at start.
  FIX with the VFO outside the window: SCOPE OUT OF RANGE with << / >> on the VFO side,
  as an Icom (2026-10-31; the font has < and > now): verified.
- Register 35 LO for FIX / SCROLL-F (built 2026-10-31; SCROLL-F showed a black right
  part on 6.920-6.976 with the VFO at 6.933): the LO goes as near the window centre as
  the shift allows (10-40 kHz either way), the IQ then covers the window: a 56 kHz
  window always, a 96 kHz one 89-100 %: edges now at most 75 kHz (default too), always
  covered with the shift 10-37.5 kHz (with ifshift 0 still 96).
  Verified 2026-10-31: the window painted; a VFO left in the window uses a negative
  shift (LO above): works, the station heard is the one on the cursor. TX
  keeps the plain shift (a key-down in FIX / SCROLL-F moves the LO back).
- liquid-dsp: built static (v1.8.3, `third_party/liquid-dsp`, `x6100_liquid` check), not
  linked into x6100_trx yet. Use per function where audio passes through Linux anyway:
  RADE / network audio (resampling, TX audio shaping), recordings (image lowpass),
  decoders later. Local listening stays on the baseband DSP (NR / NB / notch).

## From the stock UI, not yet in x6100_trx

- GEN: more rows (see key plan: region, LAN server); KEY, MSG, APP are done.
- CI-V rate: 19200 is the limit of the DEV port (9600 / 19200 work, 38400 and up do
  not, with the divisor written and the PC side checked); faster CAT goes over the
  network: the CI-V pty (/tmp/x6100-civ, any speed; since 2026-10-31 served next to the
  DEV port, so wfserver and a logger on the cable work at once: verified with rigctl), or
  the own LAN server (`x6100_netaudio_design.md`, option A vs B reopened).
- TX panel (F4 long): mic select and gains, compressor, VOX, monitor, squelch built;
  ranges and register 22 layout now from X6100Control v0.16.1 (VOX: [6:0] gain, [14:8]
  anti-VOX, [29:16] delay, [30] on; confirmed by the IU1WXY trace 2026-10-30). VOX keys
  nothing, in IU1WXY neither, nor with register 21 [3:2] (pttmode) at 1-3: not on BASE
  1.1.9; the panel shows VOX N/A (the register is still written). Test the rest on the
  radio. Line in / out levels (register 20 [7:0] / [15:8]) still fixed at 10.
- Messages and SWR scan (built 2026-10-27, not tried on the radio): CW message keying
  and level against ALC (USB tone, -10 dBFS), voice WAV in LSB / AM / NFM (codec TX is
  verified in USB / USB-D only), the turnaround (100 ms silence first), repeat, stop by
  key / PTT / TOT (CW keying, ALC, repeat and stop verified 2026-10-27); SWR scan on a
  dummy load and an antenna, the step time (300 ms) and
  whether the swrscan flag keys at once (no TX flag needed); CI-V 17 from a logger. Then:
  recording a voice message on the radio: x6100_gui records the codec capture with
  register 12 `voice_rec` 0x8 on and both mic gains at 0, so the capture likely carries
  the microphone then (measure: capture with 0x8, speak into each mic). The `swrscan`
  flag: traced 2026-10-27 (x6100_control_interface.md), the scan now uses it.
- RX / TX equaliser (baseband or liquid-dsp; GEN TX panel).
- Digital modes on the radio (FT8, CW / RTTY decoder) as APP entries; a recorder.
- RADE RX on the radio to a Bluetooth headset (A2DP works, 2026-10-31). Plan: the codec
  capture (0x8 off) -> RADE decoder -> the headset's pulseaudio sink, in place of
  IU1WXY's RX loopback (btaudio-ctl rx off while RADE runs; latency about 400 ms as
  theirs). A selection in x6100_trx (an APP entry); the RADE code (contrib/rade,
  rade_rx_mt and the FFT pilot search patch) as a helper process started by x6100_trx,
  not linked into it. RADE TX on the radio: no mic path (the built-in and hand mic go
  to the baseband, HFP / SCO does not work on this radio), so TX stays on the PC over
  the network (wfview). RADE inside wfview is out of scope.
- x6100_tx micrec: the power / ALC telemetry reads 0.0 W / 100 while transmitting (seen
  2026-10-29, the IC-7851 heard the mic): read the flow in its own non-blocking step or
  thread instead of between blocking capture reads.

## Key plan (GEN / KEY / MSG / APP / DFN / DFL)

| Key | Short | Long | Status |
|---|---|---|---|
| GEN | settings panel: brightness, CAT, NTP, WiFi, Bluetooth (done); next: region, LAN server (wfview) (TX panel: F4 long, done) | status page: IP, uptime, battery, CAT, NTP, build (done); next: temperature, baseband version | partly done |
| KEY | key type straight / paddle / reversed | keyer panel: speed, iambic A / B, pitch, sidetone, break-in delay, ratio | done |
| MSG | message panel: send CW / voice message M1-M8 | send the selected one again every msgrepeat s | built 2026-10-27 |
| APP | app panel: SWR scan (next: FT8, CW / RTTY decoder, recorder) | SWR scan | built 2026-10-27 |
| DFN | NR on / off | NB on / off | done |
| DFL | notch on / off | DSP panel (NR, NB, notch values) | done |

Rules: short = the one thing used most, long = its panel; a panel is closed by its
own long press, another key or 10 s idle; F1-F5 stay scope / span / audio / meter /
filter. TX-side settings (mic, compressor, VOX, monitor, TX EQ) go in a TX panel
under GEN until a better key turns up.

## CAT / network

- ATT / preamp from wfview: work (2026-10-27); with the IC-705 rig file wfview shows PRE
  1 / 2, X6100.rig has off / on only: check there.
- PTT in USB-D with the C-Media sound card (CAT PTT, rear USB audio).
- wfview IC-705 rig file, checked 2026-10-27 (wfview `v3-initial`, 00c62ce, `rigs/IC-705.rig`):
  - Fixed edge ranges: no table; each `Bands\N\Range` is a threshold in MHz and wfview
    counts the thresholds at or below the window's lower edge, plus 1 (icomcommander.cpp,
    spectrumBounds). For HF this gives exactly our 12 ranges (14 MHz -> 06 = 11-15).
  - Maximum edge width: not in the rig file. wfview sends 27 1E with the window it shows
    (ReceiverController::toFixedEdge), then 27 16 and 27 14 01; with the IC-705 spans
    (up to ±500 kHz) that can be 1 MHz: our 96 kHz cut applies. It never reads 27 1E
    back (the reply is ignored), so our read is for other clients only.
  - CI-V transceive: the rig file has 1A 05 0112 (on the IC-7610 0112 is transceive; on
    the IC-705 0131, as we answer; hamlib ic7300.c ic705_extcmds also uses 0131).
    Looks like a copy from the IC-7610: check against
    the IC-705 CI-V reference, then fix it in wfview (and in our X6100 rig file).
    Over a pty (VSP) wfview answers 1A 05 transceive itself and does not forward it.
- wfview rig file for the X6100: built 2026-10-27, `contrib/wfview/X6100.rig` (made by
  `mkrig.sh` from the IC-705 file; README there). Address 61 (odd: never a real Icom;
  Icom addresses are all even), `civaddr 61` in x6100_trx.conf, 19 00 also answered to
  a broadcast (wfview auto detect). The Icom way (wfview is Icom centred): the IC-705
  commands and polls kept but the 28 functions the X6100 lacks (2026-11-02, no FA polls
  any more); transceive 0131; our spans,
  bands, modes. 1A 00 now the full IC-705 layout (A + B side, name at 100; B = split
  TX side), so the IC-705 file reads names too. Tested in wfview (2026-10-29). To do:
  - the same memory window with the IC-705 file (A4)
  - hamlib, logging programs with address 61 (hamlib X6100 model 3087 has its own
    default address: check what it needs)
  - offer it to wfview upstream (with the IC-705 1A 05 0112 / 0131 question)
- Icom LAN server (ports 50001-50003): faster scope, network audio; then RADE via
  network audio. Design: `x6100_netaudio_design.md`.

## Image and distribution

- Seen on the IU1WXY image (top, 2026-10-31): pulseaudio --system with btaudio-ctl and
  btcat (Bluetooth audio: the way for RADE on the radio with a headset, RX and TX), gpsd
  (a GPS as time source in the field), ntpd -g running (the clock stays synced).
- Bluetooth headset (2026-10-31, Sony WH-1000XM2): with WiFi connected the scan finds
  nothing (the rtl8723bu vendor WiFi driver's BT coexistence); with wlan0 down it finds
  the headset at once. Paired with WiFi off, in one bluetoothctl session (scan on, scan
  off, pair, trust, connect; separate bluetoothctl calls lose the unpaired device):
  A2DP connects and plays, also with WiFi back on. The XM2 offers Handsfree / Headset
  (111E / 1108); the radio advertises A2DP / AVRCP only, no HFP / HSP gateway: the
  headset mic (RADE TX) needs HFP in pulseaudio first, then SCO over USB on the chip.
  The sound heard is the RX audio: IU1WXY's btaudio-ctl (closed terms, read for its
  behaviour only) keeps a pulseaudio loopback from the codec capture
  (alsa_input.platform-sound.stereo-fallback) to each A2DP headphone sink, latency 400
  ms (lower ran dry when WiFi held the shared chip), and forces every headset card to
  a2dp_sink. HFP / HSP are switched off on purpose (btaudio.pa: headset=ofono without
  ofono, ag=false): per IU1WXY, SCO gives no audio on this radio and drops the WiFi.
  So no headset mic: RADE TX on the radio over Bluetooth is out, RADE RX to the
  headset (A2DP) is in.
  Autoconnect built 2026-10-31 (btauto: the headset connected again every 20 s while
  not, detached check-then-connect): verified on the radio, as connect / disconnect on the
  GEN row works). HFP mic test: scripts/
  x6100_bt_hfp_test.sh (we test ourselves what IU1WXY states).
  HFP test 2026-10-31 (pulseaudio 16.1 restarted with headset=native; reloading the
  discover module live aborts it): the XM2 gets handsfree_head_unit, an eSCO link
  and a 16 kHz (mSBC) source, but no samples arrive (15 s: 44 bytes, the WAV header):
  the voice data does not cross USB, as IU1WXY found (MUSB host, isochronous). WiFi
  stayed up. CVSD (8 kHz, enable_msbc=false: x6100_bt_hfp_test.sh MAC 15 cvsd): also
  44 bytes. Confirmed for ourselves: no Bluetooth headset mic on this radio, neither
  wideband nor narrowband. RADE TX on the radio is out; RX to the headset (A2DP) stays.
- Now: the IU1WXY image (OS, kernel 6.1.82, stock baseband BASE 1.1.9) is the base;
  x6100_trx and its tools run on it (`build.sh deploy`). wfserver for the R16 is built
  as a self-contained bundle (`contrib/wfserver`, Ubuntu 24.04 armhf Qt, own glibc
  loader; 2026-10-28), so it does not need the IU1WXY buildroot tree. Runs on the radio
  (2026-10-29: wfview RX / TX audio, scope, CAT verified). To do: report
  the no-audio-device crash to wfview (findInput -1). `build.sh deploy` installs the
  bundle (out/wfserver-armhf.tar.gz) when it changed; `x6100_ui.sh stop` starts it
  (watcher on the CI-V pty), `start` stops it.
- Later: our own buildroot, self-contained, no longer depending on the IU1WXY or other
  images. Two deliverables:
  - full image: a microSD image with OS, x6100_trx, wfserver and the tools;
  - applications only: x6100_trx, wfserver and the tools as a package to put on an
    existing image (as `build.sh deploy` does now).
- To settle on the way: kernel (6.x mainline with the R16 device tree; the R_UART CAT
  divisor and the DEV port limit stay a hardware matter), init, WiFi / Bluetooth stack,
  update path, settings partition kept across updates (as `/etc/x6100` with backup /
  restore now).

## From Xiegu

- 2026-11-03: Xiegu (Jin) is refactoring the X6100 APP code, done by the end of December,
  with the CI-V part rewritten; a beta for us, and a copy of the baseband communication
  protocol and register list. To do when it comes: check the flow format, register 35
  (IF shift), zoom / calibration and the mic path to Linux (RADE TX) against what we
  measured; ask under which terms the protocol may be used / published in libx6100
  (GPL); test the beta CI-V against `x6100_civ_compare.md` (send him that list first,
  so the rewrite can cover the known gaps: 07 A0, 0F 10-12, 1C 03).

## Parked

- Baseband (STM32) clean-room firmware: `x6100_baseband_handover.md`.
  Requirement from RADE (2026-11-01): the built-in and hand mic go to the baseband only,
  no path to Linux (measured; the X6200 the same): RADE TX on the radio needs the
  baseband to send the mic samples to Linux, e.g. over the flow UART (16 kHz x 16 bit,
  about 256 kbit/s next to the IQ at 1.152 Mbaud). Linux encodes RADE (0.42-0.49 RTF on
  the R16) and sends the TX audio back by codec playback with codec TX (works today).
  The same for RX to the speaker: decoded audio from Linux played by the baseband.
- RX IQ balance (registers 48 / 49): `tools/x6100_iqcal.c`.
- Hand mic PTT is invisible to Linux (cannot abort a tune); evtest / GPIO not checked.
- Band edge markers on the scope from the region table (`region`, done: r1 / r2 / r3 / nl;
  TX check uses it; scope markers done 2026-10-26, fixed edges 2026-10-27). Next: signal edges in the check, N registration, more countries.
