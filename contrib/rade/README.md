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

Third run, the receiver in two threads and with no signal (x6100_trx off / on):

| Step | V1 | V2 |
|---|---|---|
| decode, 2 threads (`rade_rx_mt`) | 0.65 / 0.69 | 0.66 / 0.71 |
| no signal (10 s of noise) | **1.49 / 1.50** | 0.04 / 0.04 |

With a signal the two-thread receiver fits. With no signal, V1 does not: while it is not
in sync, the receiver searches for the pilots once per 120 ms modem frame, and that
search costs more than decoding.

## Patch: V1 pilot search with FFTs (`rade_acq_fft.patch`)

What the search does (`rade_acq_detect_pilots()` in rade_c's `src/rade_acq.c`): while
the receiver is not in sync, once per 120 ms modem frame, for each of 960 time offsets t
and 40 frequency offsets (+-50 Hz in 2.5 Hz steps) it correlates 160 received samples
with the pilot shifted to that frequency, twice (at t and one frame later):

    Dt1[t][f] = sum over n of conj(rx[t + n]) * p_w[f][n],   n = 0..159
    Dt2[t][f] = the same at rx[t + 960 + n]

That is 960 x 40 x 2 x 160 = 12.3 million complex multiply-adds (49 million real ones)
per frame. On the R16 it takes 171 ms per call against a frame of 120 ms (`acq_test` on
the radio): about 3.4 cycles per multiply-add.

First tried (patch A, dropped): the same sums with NEON. Split real / imaginary arrays,
four and then eight running sums, the loops reordered for the L1 cache: the result was
the same, the time too (171 / 175 ms per call). The A7's NEON unit is 64 bits wide; a
four-float multiply-add takes about 4 cycles, so it processes floats no faster than the
scalar VFP. The amount of arithmetic is the limit, not the way it is written.

What the patch does instead: the same correlations through FFTs. For one frequency, Dt1
and Dt2 over all t are one cross-correlation of x = conj(rx) with p_w[f] over the lags
s = 0..2*960-1:

    c[s] = sum over n of x[s + n] * p_w[f][n] = IFFT(FFT(x) * conj(FFT(conj(p_w[f]))))[s]

- `rade_acq_init()` transforms each of the 40 shifted pilots once (4096-point FFT,
  conjugated, scaled by 1/4096) into `fft_pw` in `rade_acq` (40 x 4096 complex, about
  1.3 MB; with the FFT state and three work buffers about 1.4 MB more per receiver,
  inside `struct rade`, which `rade_open()` mallocs);
- per frame, `rade_acq_detect_pilots()` transforms the 2079 input samples it needs once,
  then per frequency multiplies by `fft_pw[f]` and does one FFT: 41 FFTs of 4096 points
  and 40 x 4096 complex multiplies, about 13 million operations instead of about 98
  million;
- Dt1[t][f] and Dt2[t][f] are read from that result (lags t and t + 960); the maximum
  search, the threshold from the noise statistics and the return value are unchanged.
  4096 >= 2 x 960 + 160 - 1, so the circular correlation does not wrap around;
- the FFT is rade_c's own kiss_fft (`src/kiss_fft.c`, already in rade_c, now also in
  the rade library's sources in `src/CMakeLists.txt`); its state lives inside
  `rade_acq` (no malloc, no pointer, so the struct stays copyable).

The result is the same apart from float rounding. Checked with `test/acq_test.c`: the
original function and the patched one on noise with a pilot at t = 333, frequency index
17 (-7.5 Hz): both find it there, the threshold agrees to 4 digits (35.2089), the
960 x 40 x 2 correlation grid differs by at most 7e-5 of values up to 162 (x86, and ARMv7
under qemu). On x86 a call takes 1.5 ms instead of 6.9 ms.

On the radio:

    /root/rade/acq_test       # PASS, ms per call of both, budget 120 ms (one frame)

`rade_acq_check_pilots()` and `rade_acq_refine()` (the checks while in sync, already
inside the measured decode times) are not changed.

To send upstream later: the patch applies to rade_c at `C_RADE_REV`; it helps every small
ARM board, at the price of about 1.4 MB per receiver.

## Comparison

Published figures: the FreeDV team gives a Pi 4 as the smallest
comfortable machine (RADE V1 FlexRadio waveform about 30 % of one core with the C port);
one user reports a Pi Zero 2 W (4x Cortex-A53, 1 GHz) at about 35 % over 4 cores.
