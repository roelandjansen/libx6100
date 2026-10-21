# X6100 baseband firmware — handover

Status: 2026-10-21. Starting point for a separate conversation about an open,
clean-room replacement for the X6100 baseband (STM32) firmware. Parked in the
libx6100 work; this file carries the context.

## 1. Owner and conventions

- Roeland Jansen, PA3MET. C for code, bash for scripts; no Python or Perl in
  deliverables. Docs always `.md`.
- Bash: `set -euo pipefail`, tabs, `printf` (format string a single-quoted
  literal), UPPERCASE variables with `G_` / `L_` / `C_` prefixes, `[[ ]]`, `$( )`,
  function comment blocks; no sudo.
- C headers: `#pragma once`; grouped declarations behind a `//` comment block;
  own header first in the includes, then system, library, project.
- Git: repo `github.com/roelandjansen/libx6100`, default branch `master`, never
  create `main`. Commits are authored by Roeland Jansen
  `<roeland.jansen69@gmail.com>`, with no Co-Authored-By or other assistant lines.
- No agents / subagents (security choice). Claude is used as critic and sounding board.
- Builds: zig cc (0.15.x on openSUSE Tumbleweed), static, `arm-linux-musleabihf`,
  Cortex-A7; private zig caches under `out/`.

## 2. Why a baseband rewrite comes up

libx6100 now controls the stock Xiegu baseband (BASE V1.1.9) completely from Linux
(RX, TX, ATU, codec TX, display, front panel). What cannot be fixed from Linux,
because it happens inside the STM32:

| Issue | Evidence |
|---|---|
| Speaker audio images | baseband audio runs at 12 kHz; images of the audio at 6-11 kHz, 45-50 dB under the audio, audible as sizzle on AM. Measured on 1467 kHz. Recordings can be low-passed in Linux; the speaker cannot (see §5) |
| AGC AUTO squashes AM | 50 ms RMS range 4 dB with AUTO vs 26 dB with SLOW (worked around: SLOW default for AM / FM) |
| No filter shape control | registers 44-47 carry only low / high edges; no SOFT / SHARP |
| RX IQ correction | registers 48 / 49 (`rphscmp`, `rmagcmp`) are 0 in the factory calibration of this unit; could not be measured via the flow IQ |
| No continuous IQ to Linux | flow IQ is 512-sample bursts, ~27 / s, ~30 dB usable range; `auxiqen` (reg 21 bit 6) does not put IQ on the codec. So no own demodulation (e.g. synchronous AM) on the Linux side |

## 3. Architecture (verified)

```
Allwinner R16 (Linux)                      STM32F427 baseband (closed, encrypted .xgf)
  libx6100  -- I2C /dev/i2c-0, 0x72 ---->  register file 56 x u32, BE16 address = reg x 4
            <-- UART /dev/ttyS1 1152000 --  flow frames 4136 B, magic 0xAA5555AA:
                                            512 complex-float IQ (96 kHz span), flags,
                                            telemetry (pwr, swr, alc, vext, vbat), atu_params
  ALSA card 0 (sun8i-a33 codec)
    capture  <-- RX audio from the baseband (Mic1 input, mono, 48 kHz)
    playback --> TX audio to the baseband (headphone out), used with codec TX
  speaker    <-- driven by the baseband directly (not through Linux)
```

- Status block at I2C `0x0000` (128 B, version string); calibration at `0xF000`
  (816 B, STM32 CRC32 over 203 words); command mailbox at `0xFFFE`.
- Firmware update: `.xgf` (encrypted, decrypted on the STM32) pushed over xmodem
  after setting register 12 bit `0x4000`; the same bit is also "codec TX" in normal use.

## 4. The interface a replacement must implement

Complete map in `docs/x6100_control_interface.md` (§1 registers, §2 flow, §7
calibration, §8 register file, §9 verification, §11 TX / ATU). Behaviour found so
far that is not obvious from the register names:

| Item | Behaviour |
|---|---|
| Registers 0 / 6 bit 0 | TX enable per VFO ("ham band"); PTT refused when 0; ATU tune carrier not blocked. Stock: band plan, or 1 everywhere with `fullband-tx=enable` |
| Register 12 | `0x40000` PTT (always takes the microphone), `0x4000` codec TX (TX audio = codec playback, no PTT needed, works in USB and USB-D), `0x2000` ATU tune, `0x1000` ATU enable, `0x8` play mode (speaker plays codec playback; codec capture loses RX audio) |
| Registers 44-47 | filter 1 / 2 low / high. AM / FM: low must be -high (absolute values pick the I and Q low-pass filters); otherwise an off-tune carrier becomes a beat tone. Filter 2 = filter 1 widened 40 Hz each side |
| Register 15 | [7:0] RF gain, [15:8] TX power x10 W, [23:16] FFT decimation |
| Registers 20 / 21 | line-in / line-out / int mic / hand mic gains; mic select, PTT mode, charger, aux IQ |
| Registers 27 / 28 | AGC knee / slope / hang; time constants |
| Register 17 | ATU network L / C / S; tune: set 0x2000 + 0x1000, wait for flow flag bit 2, read `atu_params`, write reg 17 |
| Registers 36 / 38 | written by the R1CBU / IU1WXY UI (0x4A02; 160 / 3000 Hz, presumably TX passband); no audible effect on stock BASE 1.1.9, probably for the patched base |
| ATT | about 12 dB |
| ALC telemetry | 100 = no audio arriving (not overdrive) |

## 5. Negative results worth knowing

- Speaker through Linux: with `0x8` set, codec capture no longer carries RX audio
  (-30 -> -54 dBFS within a second). Real-time filtering of the speaker from
  Linux is not possible (`tools/x6100_spk.c`, kept for reference).
- PTT never takes codec audio, in any mode or reg 20 / 21 combination.
- Flow IQ is not a measuring instrument for image rejection (registers 48 / 49
  showed no measurable effect through it).

## 6. Prior art and its status

| Project | What | Usable? |
|---|---|---|
| gdyuldin/x6100_base_patch | binary patch of Xiegu BASE 1.1.6: TX compressor, RX IQ balancing, AM DC removal, filters, IF shift, notch, DAC gain, FFT span | No licence shown, derivative of Xiegu code: hints only |
| strijar/x6100-base | Ghidra reverse engineering of BASE 1.1.9 (decrypted image), aim: independent open replacement; early (startup verified); states OEM decompiled output must not be copied | No licence shown; strijar is R1CBU's handle. Watch, do not copy |
| AetherRadio/X6100Study, jcyfkimi/X6100_Study, X6100-TOADs | hardware / software notes | Possible hardware info (board, parts) |
| liquid-dsp (jgaeddert) | C DSP library, MIT | Yes, static build planned for Linux-side DSP |

## 7. Agenda for the new conversation

1. **Legal / method.** A clean-room approach: specification from observable
   behaviour (our docs, measurements), implementation by people who have not
   read the decompiled firmware. How to keep that separation documented.
2. **Hardware discovery.** Board layout, SWD pads, ADC and DAC parts, clock
   tree, RF control lines (filters, preamp, attenuator, PA bias, ATU relays).
   Any schematics in the study repos?
3. **Recovery first.** Backup of the original STM32 flash (readout protection?),
   unbrick path, and whether the Xiegu bootloader can take our own image or SWD is
   the only way in.
4. **Milestones.** Blinky -> I2C register interface (slave 0x72, the register
   file) -> UART flow frames -> RX chain (ADC, mixing, decimation, filters, AGC,
   demod, audio out with a proper reconstruction filter) -> TX chain -> ATU.
5. **Contract.** libx6100 stays unchanged: the register interface and flow
   protocol documented in `docs/x6100_control_interface.md` are the spec the new
   firmware must meet.
6. **Decision.** Go, or stay parked and keep working around the stock baseband.

## 8. Files

- `docs/x6100_control_interface.md` — the interface, verification table
- `docs/x6100_netaudio_design.md` — network audio / wfview design (draft)
- `tools/` — `x6100_trx` (transceiver UI; `x6100_rx` is the same source,
  receive only), `x6100_tx`, `x6100_audio`, `x6100_i2csniff.c`, `x6100_spk.c`,
  `x6100_iqcal.c`
