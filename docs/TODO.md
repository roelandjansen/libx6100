# libx6100 / x6100_trx — to do

Status: 2026-10-24.

## Display

- TX overlay panel (as the IU1WXY UI): during TX a framed box over the scope with
  TX badge, mode + frequency, TX timer (mm:ss), and segmented bars for PWR (0-10 W
  scale), SWR (white / yellow / red zones) and ALC, value on the right of each bar.
- Time / date display in ISO 8601 (`2026-10-24T11:57Z` or local `2026-10-24 11:57`).
- PWR short press: LCD on / off (long press stays quit).

## System

- NTP sync: set the clock from the network when WiFi / Ethernet is up (check
  first what the image already runs, and whether the RTC keeps time).

## Radio

- TX ranges config file and transverter profile (frequency offset, TX enable).
- AM tuning steps 9 / 10 / 5 kHz.
- Split and RIT / XIT (TX / RX frequency handling).
- S-meter calibration (signal generator), overload point in dBm with PRE / ATT.
- Codec level vs ALC test (x6100_tx codec at -40 / -30 / -20 dBFS).
- Register 35 LO offset for FIX mode; F2 (span) edges in FIX mode.
- Linux-side DSP with liquid-dsp (static build).

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
