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
  `rade_rx_wav`, `input_sample.wav` (rade_c's 9.8 s speech sample, 16 kHz) and
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

## Status

Written 2026-10-29. Not built or run yet: the build needs media.xiph.org, which the
cloud session could not reach; the benchmark script was checked with stand-in tools.

Published figures for comparison: the FreeDV team gives a Pi 4 as the smallest
comfortable machine (RADE V1 FlexRadio waveform about 30 % of one core with the C port);
one user reports a Pi Zero 2 W (4x Cortex-A53, 1 GHz) at about 35 % over 4 cores.
