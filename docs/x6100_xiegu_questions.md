# X6100: findings and questions for Xiegu

Status: 2026-11-07. From work on x6100_trx, an open front end for the X6100 on BASE
1.1.9 (stock, unpatched), APP 1.2.0 for comparison. All measured on one radio. The CI-V
side is in `x6100_civ_compare.md` (same folder).

## 1. A birdie comb of about 7.58 kHz spacing

A receive-only scan (no antenna, 36 kHz windows, 32 averaged spectrum frames each, peaks
at least 16 dB over the noise) over all bands, done four times on 2026-11-06 / 07:

| Scan | Display | Supply | USB | WiFi | Bluetooth |
|---|---|---|---|---|---|
| 1, 2 | on | external | connected | on | off |
| 3 | off | battery | none | on | off |
| 4 | off | battery | none | off | off |

Almost all birdies from 16 dB came back in every scan, at the same frequency (within
about 10 Hz) and nearly the same strength (most within 2 dB; a few near 16 dB came and
went). Display, external supply, USB, WiFi and Bluetooth make no difference: the sources
are inside the radio.

On several bands they form a comb of the same spacing, about **7.58 kHz**:

| Band | Birdies (kHz, dB over the noise) |
|---|---|
| 40 m | 7005.7, 7013.3, 7020.8, 7028.4 (20), 7043.6 (20), 7051.2, 7058.7, 7066.3 |
| 20 m | 14034.1, 14041.7 (24), 14049.3, 14056.8 (21), **14064.4 (43)**, 14079.6, 14087.2, 14094.7, 14102.3 |
| 4 m | 70329.7, 70337.3 (27), 70344.8 (33), **70352.4 (54)**, 70367.6, 70375.2 (37), 70382.7 (32), 70390.3 |

On 15 m a similar run of about 8.44 kHz (21300.3, 21308.7, 21317.2). The strongest single
ones: **21339.3 kHz (48 dB)**, **51165.4 kHz (53 dB)**, 7092.7 kHz (35 dB).

The same spacing in Hz on every band points to one source with a 7.58 kHz modulation or
clock mixing with the oscillators. **Question:** which oscillator, clock or switching
regulator in the radio runs at, or modulates with, about 7.58 kHz (and 8.44 kHz)? Is it
known, and can it be moved or filtered?

The full list (about 130 birdies from 16 dB, 1.8-70.5 MHz) is available on request.

## 2. Baseband

- **IQ span:** the 512-bin IQ in the flow is 100 kHz wide (195.3 Hz bins), as the stock
  UI's FFT SPAN 100k; we had assumed 96 kHz and drew the scope 4 % off. What is the ADC
  rate and the decimation to the 100 kHz IQ?
- **AGC AUTO:** register 28 holds three time constants (slow / mid / fast); SLOW and FAST
  follow them, AUTO follows none (changing the middle one or FAST made no difference in
  AUTO; it sounds like FAST in SSB). How does AUTO choose its time, per mode? Can it be
  set?
- **VOX:** register 22 is written as the stock UI does, but on BASE 1.1.9 VOX never keys;
  APP 1.2.0 has no VOX setting either. Is VOX supported by the baseband?
- **Charging flag:** flow flag bit 4 (called "charging" in some third-party code) stays 0
  while the battery charges from 98 to 100 % on external power. Is there a charging bit?
- **Battery gauge:** it reads 99 when full; the stock UI shows 100. Is 99 the top?
- **Mic to Linux:** the microphone reaches the baseband only; for digital voice modes
  (RADE / FreeDV) on the radio the mic samples would have to come to Linux, e.g. over
  the flow UART. Is that possible?
- **A hang:** once, after hours idle (5450 kHz USB, receive only), the baseband stopped:
  no flow frames and NAKs on I2C for 6.8 hours, a new start got no answer at all; only a
  power cycle helped (a pulse on the reset line, GPIO 204, brings a healthy baseband
  back; not yet tried on a hang). Is such a hang known, and what may cause it?

## 3. Terms

When the baseband protocol and register list come: may we describe them in libx6100
(GPL-3.0), or only use them?
