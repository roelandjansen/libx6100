# libx6100 / x6100_trx — to do

Status: 2026-10-24.

## Display

- Clock: own time source / sync check (now: system time, one `ntpd -q` at start).
- PWR short press = display off: check the backlight sysfs path on the radio, and
  that the power key reports press and release (hold 1.5 s quits).

## Radio

- TX ranges config file and transverter profile (frequency offset, TX enable).
- Split and RIT / XIT (TX / RX frequency handling).
- S-meter calibration (signal generator): SMETER_CAL_DB is provisional (-21: -15 from
  an IC-7851 comparison, then 6 dB lower); also the overload point with PRE / ATT.
- Codec level vs ALC test (x6100_tx codec at -40 / -30 / -20 dBFS).
- Register 35 LO offset for FIX mode; span edges in FIX mode.
- Linux-side DSP with liquid-dsp (static build).

## From the stock UI, not yet in x6100_trx

- Settings menu (GEN key); keyer settings (KEY), messages (MSG), apps (APP).
- NB, NR, DNF (notch) with levels; compressor; VOX / anti-VOX; squelch; monitor;
  mic select and gain; line in / out levels (registers known, defaults set at start).
- CW keyer: speed, iambic A / B, sidetone, break-in delay.
- VFO A / B, memory channels.
- SWR scan over a band (register 12 `swrscan`).
- Brightness; battery / charge status.
- Digital modes on the radio (FT8, CW / RTTY decoder); CW / voice message memories;
  RX / TX equaliser.

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
