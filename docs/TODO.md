# libx6100 / x6100_trx — to do

Status: 2026-10-27 (v0.13.0). Summary and next steps: `STATUS.md`.

## Display

- Clock: own time source / sync check (now: system time, one `ntpd -q` at start).

## Radio

- TX ranges config file and transverter profile (frequency offset, TX enable).
- Repeater duplex (AUTO / OFF / DUP- / DUP+, AGC long): test on the radio (TX on
  29.520 from 29.620 verified with the PTT key and the hand mic; still: CAT, memories,
  CI-V 0F 1x).
- CTCSS for the repeaters: no tone encoder known in the baseband, and the microphone
  goes to the baseband directly (PTT + codec audio does not mix). To look for: an
  unmapped bit in registers 21 / 36 / 38 (stock UI / IU1WXY trace in FM), or a
  baseband with a sub-audible tone; else codec TX with Linux mic capture (none yet).
- Hand mic PTT with split: x6100_trx follows the baseband TX flag (verified), the first
  ms go out on the RX side; the baseband split flag (register 12 b1, VFO A RX / VFO B TX)
  could make that clean: to test.
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
- S-meter calibration (signal generator): SMETER_CAL_DB is provisional (-21: -15 from
  an IC-7851 comparison, then 6 dB lower); also the overload point with PRE / ATT.
- Codec level vs ALC test (x6100_tx codec at -40 / -30 / -20 dBFS).
- Lost I2C writes and TX timeout (built 2026-10-27, not tried on the radio): a failed
  write is resent at once (3 tries, the rest of the block still goes out), the whole
  block after every un-key and write error, and every 250 ms while the baseband stays
  in TX 0.3-2 s after an un-key; TOT off / 1 / 3 / 5 min (TX panel), hand mic / VOX TX
  to 0.1 W at the timeout. To check: the console lines on a band change NAK, normal
  un-keys without the stuck-TX line, TOT 1 min with the PTT key, CAT and the hand mic.
  Since 2026-10-27 evening (not tried): power to the minimum when the baseband stays in
  TX 2 s into a watch armed by a write error (OpenX6X00 tx_watch_power); after 2 s of
  I2C failures TX drops and is refused, x6100_trx keeps trying instead of quitting.
- Fixed scope edges (IC-705 style, built 2026-10-27, not tried on the radio): F2 / F2
  long in FIX and SCROLL-F, the scope label, a band change moves the window, edges kept
  over a restart; wfview fixed edge window (27 16 / 27 1E set and read), a 200 kHz edge
  from wfview cut to 96 kHz. Then: a panel to edit an edge on the radio (MFK lower /
  upper), and whether wfview sends 27 1E for a range we do not cover (above 60 MHz).
- Register 35 LO offset for FIX mode: an edge off the IQ centre now shows only the part
  of the 96 kHz IQ it covers; moving the LO would fill the whole window.
- liquid-dsp: built static (v1.8.3, `third_party/liquid-dsp`, `x6100_liquid` check), not
  linked into x6100_trx yet. Use per function where audio passes through Linux anyway:
  RADE / network audio (resampling, TX audio shaping), recordings (image lowpass),
  decoders later. Local listening stays on the baseband DSP (NR / NB / notch).

## From the stock UI, not yet in x6100_trx

- GEN: more rows (see key plan); keyer settings (KEY), messages (MSG), apps (APP).
- CI-V rate: 19200 is the limit of the DEV port (9600 / 19200 work, 38400 and up do
  not, with the divisor written and the PC side checked); faster CAT goes over the
  network: `cat pty` (CI-V on /tmp/x6100-civ, any speed) for wfserver on the radio, or
  the own LAN server (`x6100_netaudio_design.md`, option A vs B reopened).
- TX panel (F4 long): mic select and gains, compressor, VOX, monitor, squelch built;
  ranges and register 22 layout now from X6100Control v0.16.1 (VOX: [6:0] gain, [14:8]
  anti-VOX, [29:16] delay, [30] on; the old layout never switched VOX on). Test on the
  radio. Line in / out levels (register 20 [7:0] / [15:8]) still fixed at 10.
- Messages and SWR scan (built 2026-10-27, not tried on the radio): CW message keying
  and level against ALC (USB tone, -10 dBFS), voice WAV in LSB / AM / NFM (codec TX is
  verified in USB / USB-D only), the turnaround (100 ms silence first), repeat, stop by
  key / PTT / TOT; SWR scan on a dummy load and an antenna, the step time (300 ms) and
  whether the swrscan flag keys at once (no TX flag needed); CI-V 17 from a logger. Then:
  recording a voice message on the radio: x6100_gui records the codec capture with
  register 12 `voice_rec` 0x8 on and both mic gains at 0, so the capture likely carries
  the microphone then (measure: capture with 0x8, speak into each mic). The `swrscan`
  flag: traced 2026-10-27 (x6100_control_interface.md), the scan now uses it.
- RX / TX equaliser (baseband or liquid-dsp; GEN TX panel).
- Digital modes on the radio (FT8, CW / RTTY decoder); CW / voice message memories.

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
  a broadcast (wfview auto detect). The Icom way (wfview is Icom centred): all IC-705
  commands and polls kept, NG where we have no function; transceive 0131; our spans,
  bands, modes. 1A 00 now the full IC-705 layout (A + B side, name at 100; B = split
  TX side), so the IC-705 file reads names too. To do:
  - try it: wfview loads it and connects (address 61 and auto), the NG answers do not
    upset it, scope, fixed edges, memories (read, write, name, split TX side), transceive
  - the same memory window with the IC-705 file (A4)
  - hamlib, logging programs with address 61 (hamlib X6100 model 3087 has its own
    default address: check what it needs)
  - offer it to wfview upstream (with the IC-705 1A 05 0112 / 0131 question)
- Icom LAN server (ports 50001-50003): faster scope, network audio; then RADE via
  network audio. Design: `x6100_netaudio_design.md`.

## Image and distribution

- Now: the IU1WXY image (OS, kernel 6.1.82, stock baseband BASE 1.1.9) is the base;
  x6100_trx and its tools run on it (`build.sh deploy`). A wfserver build for the R16
  starts from the IU1WXY buildroot tree (in its GPL sources).
- Later: our own buildroot, self-contained, no longer depending on the IU1WXY or other
  images. Two deliverables:
  - full image: a microSD image with OS, x6100_trx, wfserver and the tools;
  - applications only: x6100_trx, wfserver and the tools as a package to put on an
    existing image (as `build.sh deploy` does now).
- To settle on the way: kernel (6.x mainline with the R16 device tree; the R_UART CAT
  divisor and the DEV port limit stay a hardware matter), init, WiFi / Bluetooth stack,
  update path, settings partition kept across updates (as `/etc/x6100` with backup /
  restore now).

## Parked

- Baseband (STM32) clean-room firmware: `x6100_baseband_handover.md`.
- RX IQ balance (registers 48 / 49): `tools/x6100_iqcal.c`.
- Hand mic PTT is invisible to Linux (cannot abort a tune); evtest / GPIO not checked.
- Band edge markers on the scope from the region table (`region`, done: r1 / r2 / r3 / nl;
  TX check uses it; scope markers done 2026-10-26, fixed edges 2026-10-27). Next: signal edges in the check, N registration, more countries.
