# RADE on the X6100: benchmark

Can the R16 (4x Cortex-A7, about 1.2 GHz, NEON-VFPv4) run FreeDV RADE in real time?
`build.sh` cross-builds the FreeDV C port, [rade_c](https://github.com/freedv/rade_c),
for the radio and `rade_bench.sh` measures it there.

## Build (on the PC, as a normal user)

```
contrib/rade/build.sh
bash build.sh deploy          # copies out/rade to /root/rade on the radio
```

- Needs zig (as the rest of libx6100), git, make, autoconf, automake, libtool and curl
  or wget: `zypper install zig git make autoconf automake libtool`.
- Fetches rade_c at a fixed commit (`C_RADE_REV`) and the Opus commit that rade_c pins
  (read from its `cmake/BuildOpus.cmake`), applies rade_c's Opus `nnet` patches and lets
  Opus download its neural network weights from media.xiph.org (sha256 checked by Opus's
  `dnn/download_model.sh`).
- Static musl binaries for `arm-linux-musleabihf`, `-mcpu=cortex_a7`: Opus's own
  configure picks its ARM NEON code paths. Output in `out/rade`: `rade_tx_wav`,
  `rade_rx_wav`, `rade_rx_mt` (the receiver in two threads), `lpcnet_demo` (FARGAN
  alone), `input_sample.wav` (rade_c's 9.8 s speech sample, 16 kHz) and
  `rade_bench.sh`. Sources and the Opus build stay in `out/rade-work`.

## Run (on the radio)

```
/root/rade/rade_bench.sh                 # rade_c's sample
/root/rade/rade_bench.sh my_speech.wav   # 16 kHz 16-bit mono
```

For RADE V1 and V2 it encodes the speech to modem audio (8 kHz) and decodes that back,
and prints the time against the audio length:

```
cpu: 4 x 0xc07, 1200 MHz; load 0.10 0.20 0.15
v1 encode      x.xx s for   9.82 s audio   RTF 0.xx
v1 decode      x.xx s for   9.80 s audio   RTF 0.xx
...
```

RTF (real-time factor) = processing time / audio length, on one core: rade_c runs in
one thread. Below 1.0 keeps up. A live radio needs encode (TX) or decode (RX) only, not
both at once, but beside x6100_trx and wfserver: below about 0.5 leaves room. Run it
once with x6100_trx stopped and once with x6100_trx and wfserver running.

The decode of a clean signal is the light case: on a real off-air signal the receiver
also searches for sync. rade_c's `FDV_offair.wav` is an off-air recording at 48 kHz;
resampled to 8 kHz it can be decoded with `rade_rx_wav` for that case.

`lpcnet_demo` is built as well: the benchmark runs the FARGAN vocoder alone on the
decoded features (`v1 fargan`), so decode minus FARGAN is the demodulator and the RADE
decoder. If both halves stay below 1.0, the receiver fits in real time as two threads on
two cores.

## Results

2026-10-29, X6100, R16 at 1008 MHz (cpufreq as the image sets it), rade_c c8a3dc1,
rade_c's 9.8 s sample, x6100_trx not running:

| Step | RTF (two runs) |
|---|---|
| V1 encode (TX) | 0.42, 0.45 |
| V1 decode (RX) | 1.08, 1.14 |
| V2 encode | 0.47, 0.54 |
| V2 decode | 1.15, 1.21 |

TX fits easily; RX misses real time by 8-20 % on one core. The build has NEON (build.sh
prints `NEON yes` for the zig flags).

Second run, with the FARGAN split (cpufreq: governor performance, the image's table
stops at 1008 MHz, so no higher clock without a device tree change):

| Step | RTF, x6100_trx off | x6100_trx running |
|---|---|---|
| V1 decode | 1.08 | 1.14 |
| V1 FARGAN alone | 0.57 | 0.61 |
| V1 demodulator + decoder (decode minus FARGAN) | 0.54 | 0.53 |
| V2 decode | 1.18 | 1.23 |
| V2 FARGAN alone | 0.59 | 0.63 |

Both halves stay near 0.55-0.6: as two threads on two cores the receiver fits.
`rade_rx_mt` (`rade_rx_mt.c`, derived from rade_c's `rade_rx_wav.c`) does that: the
demodulator and decoder in one thread, FARGAN in the other, a queue of feature frames
(up to 0.64 s) between them. The benchmark times it as `v1 2 threads`, and the receiver
on 10 s of noise as `v1 no signal` (searching for sync: its idle load on the air).


Published figures for comparison: the FreeDV team gives a Pi 4 as the smallest
comfortable machine (RADE V1 FlexRadio waveform about 30 % of one core with the C port);
one user reports a Pi Zero 2 W (4x Cortex-A53, 1 GHz) at about 35 % over 4 cores.
