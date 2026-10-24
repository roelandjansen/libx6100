# libx6100 / x6100_trx — to do

Status: 2026-10-24.

## Display

- Clock: own time source / sync check (now: system time, one `ntpd -q` at start).
- PWR short press = display off: check the backlight sysfs path on the radio, and
  that the power key reports press and release (1.5-4 s quits, 4 s powers off).
- Brightness (GEN): the backlight is inverted (0 = full); check that 10 % is still readable.

## Radio

- TX ranges config file and transverter profile (frequency offset, TX enable).
- Split and RIT / XIT (TX / RX frequency handling).
- 60 m channels (the allowed channel / segment list as a band with fixed steps).
- 10 m repeater split (FM, -100 kHz shift, CTCSS), with split above.
- S-meter calibration (signal generator): SMETER_CAL_DB is provisional (-21: -15 from
  an IC-7851 comparison, then 6 dB lower); also the overload point with PRE / ATT.
- Codec level vs ALC test (x6100_tx codec at -40 / -30 / -20 dBFS).
- Register 35 LO offset for FIX mode; span edges in FIX mode.
- liquid-dsp (static), only per function where audio passes through Linux anyway:
  RADE / network audio (resampling, TX audio shaping), recordings (image lowpass),
  decoders later. Local listening stays on the baseband DSP (NR / NB / notch).

## From the stock UI, not yet in x6100_trx

- GEN: more rows (see key plan); keyer settings (KEY), messages (MSG), apps (APP).
- CI-V rate: the example conf has `cat 19200`; raise it once ttyS2 runs faster.
- Compressor; VOX / anti-VOX; squelch; monitor;
  mic select and gain; line in / out levels (registers known, defaults set at start).
- CW keyer: speed, iambic A / B, sidetone, break-in delay.
- VFO A / B, memory channels.
- SWR scan over a band (register 12 `swrscan`).
- Battery / charge status.
- Digital modes on the radio (FT8, CW / RTTY decoder); CW / voice message memories;
  RX / TX equaliser.

## Key plan (GEN / KEY / MSG / APP / DFN / DFL)

| Key | Short | Long | Status |
|---|---|---|---|
| GEN | settings panel: brightness, CAT, NTP (done); next: network, WiFi, Bluetooth, LAN server (wfview), TX panel | status page: IP, uptime, battery, CAT, NTP, build (done); next: temperature, baseband version | partly done |
| KEY | keyer on / off (straight / iambic) | keyer panel: speed, iambic A / B, sidetone, break-in delay, CW pitch | planned |
| MSG | message list: play CW / voice memory | record / edit a message | planned |
| APP | app list: FT8, CW / RTTY decoder, SWR scan, recorder | last app | planned |
| DFN | NR on / off | NB on / off | done |
| DFL | notch on / off | DSP panel (NR, NB, notch values) | done |

Rules: short = the one thing used most, long = its panel; a panel is closed by its
own long press, another key or 10 s idle; F1-F5 stay scope / span / audio / meter /
filter. TX-side settings (mic, compressor, VOX, monitor, TX EQ) go in a TX panel
under GEN until a better key turns up.

## CAT / network

- ATT / preamp from wfview: compare a socat trace with IU1WXY (see `x6100_civ.md` §7).
- Band buttons (1A 01): retest with wfview.
- 1A 05 0131 CI-V transceive on / off.
- PTT in USB-D with the C-Media sound card (CAT PTT, rear USB audio).
- Icom LAN server (ports 50001-50003): faster scope, network audio; then RADE via
  network audio. Design: `x6100_netaudio_design.md`.
- Faster serial CAT needs our own kernel / device tree (stock image: 19200 only).

## Parked

- Baseband (STM32) clean-room firmware: `x6100_baseband_handover.md`.
- RX IQ balance (registers 48 / 49): `tools/x6100_iqcal.c`.
- Hand mic PTT is invisible to Linux (cannot abort a tune); evtest / GPIO not checked.
- Band edge markers on the scope, per region / licence: a table in the conf (first:
  the Netherlands, full licence, e.g. 80 m 3500-3800 kHz, 40 m 7000-7200 kHz, ...),
  later region presets (IARU 1 / 2 / 3) and the TX band check from the same table.
