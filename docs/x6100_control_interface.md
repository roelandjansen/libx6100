# Xiegu X6100 — baseband control interface

Source: AetherRadio/X6100Control, commit 4159e11 (2023-12-09), LGPL-2.1-or-later.
Copyright Oleg Belousov R1CBU and Rui Oliveira CT7ALW. Derived by reverse-engineering
Xiegu's front-end app `x6100_ui_v100` (author Jet Yee).

Note: this is a fork of R1CBU's original code, not an independent reimplementation.
Register layout may lag newer base firmware / `x6100_base_patch` revisions; check
gdyuldin/x6100_gui for registers added after 2023.

## Architecture

```
Allwinner SoC (Linux)                         STM32 baseband (Xiegu fw, closed)
  ├─ /dev/i2c-0, slave 0x72  ── control ─────▶ register file (56 × uint32)
  ├─ /dev/ttyS1, 1152000 8N1 ◀── flow ─────── IQ + telemetry frames, ~35 ms
  └─ sysfs GPIO              ── wifi/usb/light/key/bb_reset
```

Audio (RX/TX speech) is not in this library; expected via the SoC's ALSA codec path
(unverified here).

## 1. Control channel — I2C

| Item       | Value                         |
|------------|-------------------------------|
| Bus        | `/dev/i2c-0`                  |
| Address    | `0x72`                        |
| Register   | 32-bit, little-endian payload |
| Address fmt| `reg_index * 4`, 16-bit, **big-endian** on the wire |

Write forms:

- Single register: `[addr_hi][addr_lo][arg (4 bytes LE)]` — 6 bytes
- Full refresh: `[0x00][0x00][arg[0] … arg[55]]` — 2 + 224 bytes

Write-only. The library keeps a shadow copy (`all_cmd`) and periodically resends the
whole file (`x6100_control_idle()`); read-modify-write on bitfields uses the shadow.

### Register map

| Idx | Name                                  | Layout / meaning |
|----:|---------------------------------------|------------------|
| 0   | vfoa_ham_band                         | band index |
| 1   | vfoa_freq                             | Hz |
| 2   | vfoa_att                              | 0 off, 1 on |
| 3   | vfoa_pre                              | 0 off, 1 on |
| 4   | vfoa_mode                             | see modes |
| 5   | vfoa_agc                              | 0 off, 1 slow, 2 fast, 3 auto |
| 6–11| vfob_*                                | same as 0–5 for VFO B |
| 12  | sple_atue_trx                         | flag word, see below |
| 13  | vi_vm                                 | [7:0] active VFO (0 A, 1 B); [15:8] band index |
| 14  | rxvol                                 | speaker volume |
| 15  | rfg_txpwr                             | [7:0] RF gain 0–100; [15:8] TX power ×10 W (0–100) |
| 17  | atu_network                           | ATU L/C state (opaque) |
| 20  | ling_loutg_imicg_hmicg                | [7:0] line-in; [15:8] line-out; [23:16] int. mic; [31:24] hand mic |
| 21  | micsel_pttmode_chge_spmode_auxiqgen_sqlthr | [1:0] mic sel (0 int, 1 hand, 2 auto); [4] charger en; [5] speaker/phone; [15:8] squelch 0–100; PTT mode / aux IQ gen bits unmapped |
| 22  | voxg_voxag_voxdly_voxe                | [6:0] gain; [13:7] anti-gain; [25:14] delay ms; [26] enable |
| 23  | nrthr_nbw_nbthr_nre_nbe               | [7:0] NR level; [15:8] NB width; [23:16] NB level; [24] NR en; [25] NB en |
| 24  | dnfcnt_dnfwidth_dnfe                  | [11:0] notch centre Hz; [23:12] width Hz; [24] enable |
| 25  | cmplevel_cmpe                         | compressor level / enable (unmapped in lib) |
| 27  | agcknee_agcslope_agchang              | [7:0] knee dB (int8); [11:8] slope dB; [12] hang |
| 28  | agctime                               | ms |
| 29  | monilevel                             | monitor level |
| 33  | ks_km_kimb_cwtone_cwvol_cwtrain       | [7:0] WPM; [9:8] key mode; [11:10] iambic A/B; [22:12] tone Hz; [28:23] volume; [29] trainer |
| 34  | qsktime_kr                            | [15:0] QSK ms; [31:16] dot/dash ratio ×10 |
| 41  | biasdrive_biasfinal                   | [15:0] driver bias; [31:16] final bias |
| 42  | rit                                   | Hz offset |
| 43  | xit                                   | Hz offset |
| 44  | filter1_low                           | Hz |
| 45  | filter1_high                          | Hz |
| 46  | filter2_low                           | Hz |
| 47  | filter2_high                          | Hz |
| 48  | rphscmp                               | RX phase compensation (IQ cal) |
| 49  | rmagcmp                               | RX magnitude compensation |
| 50  | txiofs                                | TX I DC offset |
| 51  | txqofs                                | TX Q DC offset |
| 53  | pwrsync                               | PSU switcher sync freq (default 2 000 000) |
| 55  | last                                  | init sentinel `0x100001` |

Gaps 16, 18–19, 26, 30–32, 35–40, 52, 54: unknown.

### Flag word, register 12 (`sple_atue_trx`)

| Bit mask  | Meaning |
|-----------|---------|
| `0x00002` | split |
| `0x00008` | voice rec: audio stream mic/speaker ↔ RX/TX |
| `0x00010` | SWR scan |
| `0x01000` | ATU enable |
| `0x02000` | ATU tune start |
| `0x04000` | modem: route serial to STM32 bootloader (fw update) |
| `0x08000` | calibration (after STM32 update) |
| `0x10000` | power off |
| `0x40000` | PTT |

### Modes (registers 4 / 10)

`0 LSB, 1 LSB-D, 2 USB, 3 USB-D, 4 CW, 5 CW-R, 6 AM, 7 NFM`

### Band index (register 13 [15:8])

Even = below/between bands, odd = inside a ham band:

| Index | Range |
|------:|-------|
| 0 | < 1.8 MHz |
| 1 | 160 m |
| 3 | 80 m |
| 5 | 60 m (5.3305–5.405) |
| 7 | 40 m |
| 9 | 30 m |
| 11 | 20 m |
| 13 | 17 m |
| 15 | 15 m (21.0–21.45) |
| 17 | 12 m |
| 19 | 10 m |
| 21 | 6 m |
| 22 | > 54 MHz |

Presumably selects LPF/BPF banks. The reversed code has a quirk: the 21.0–21.45 MHz
range falls through to the default `return 15`; index 14 is unreachable as written.

### Init defaults

VFO A/B 14.074 MHz USB, AGC auto, band 1, RF gain 64, TX power 1.0 W, filters 50–2950 Hz,
AGC knee/slope `0x6C4`, AGC time 500 ms, pwrsync 2 MHz.

## 2. Flow channel — UART

| Item     | Value |
|----------|-------|
| Device   | `/dev/ttyS1` |
| Speed    | 1 152 000 baud, raw |
| Rate     | one frame ≈ every 35 ms |
| Sync     | magic `0xAA5555AA` |
| Integrity| CRC-32, poly `0x04C11DB7`, init `0xFFFFFFFF`, MSB-first, over 32-bit words, no final XOR (STM32 hardware CRC style) |

Frame (packed, little-endian), 4 136 bytes:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0      | 4    | magic `0xAA5555AA` |
| 4      | 4096 | 512 × complex float IQ (spectrum/panadapter) |
| 4100   | 4    | flags: b0 resync, b1 tx, b2 charging, b3 ext. power |
| 4104   | 1    | reserved |
| 4105   | 1    | TX power ×10 W |
| 4106   | 1    | VSWR ×10 |
| 4107   | 1    | ALC ×10 |
| 4108   | 1    | V ext ×10 |
| 4109   | 1    | V bat ×10 |
| 4110   | 1    | battery capacity % |
| 4111   | 1    | reserved |
| 4112   | 4    | ATU params |
| 4116   | 12   | reserved |
| 4128   | 4    | handset key code (see below) |
| 4132   | 4    | CRC over words 0 … 1032 |

512 samples per ~35 ms ≈ 14.6 kS/s effective: a decimated spectrum feed, not wideband IQ.
The library notes the UART occasionally stalls and needs reopening (`x6100_flow_restart`).

### Handset (mic) key codes

| Key | Code | Key | Code | Key | Code |
|-----|------|-----|------|-----|------|
| SPCH | `0xC1` | 1 | `0xE1` | 7 | `0xA1` |
| TUNER | `0xC2` | 2 | `0xE2` | 8 | `0xA2` |
| XFC | `0xC4` | 3 | `0xE4` | 9 | `0xA4` |
| UP | `0x84` | 4 | `0x61` | . | `0x21` |
| DOWN | `0x04` | 5 | `0x62` | 0 | `0x22` |
| V/M | `0x81` | 6 | `0x64` | CE | `0x24` |
| N/W | `0x82` | MODE | `0xE8` | GENE | `0xA8` |
| F1 | `0x48` | FIL | `0x68` | FINP | `0x28` |
| F2 | `0x44` | | | | |

## 3. GPIO (sysfs, outputs)

| Function | GPIO |
|----------|-----:|
| WiFi power | 357 |
| USB | 138 |
| Backlight/light | 143 |
| Morse key | 203 |
| Baseband reset | 204 |

Uses legacy `/sys/class/gpio`; a new design should use the GPIO chardev (libgpiod).

## 4. Stock firmware APP V1.2.0 / BASE V1.1.9 (2025-12-13) — reference findings

Image: `sdcard.img`, MBR, 3 partitions:

| # | Type | Start sector | Size | Content |
|---|------|-------------:|-----:|---------|
| 1 | FAT16 | 2048 | 16 MB | boot (u-boot env/script) |
| 2 | ext4 | 34816 | 400 MB | rootfs, Buildroot 2020.02.9, kernel 5.8.9 |
| 3 | FAT32 | 854016 | 400 MB | data |

Rootfs facts:

- `/boot/zImage`, `/boot/sun8i-r16-x6100.dtb` — SoC Allwinner R16 (sun8i).
- `/usr/app_qt/x6100_ui_v100` — Qt5 UI, ARM EABI5, **not stripped**: full C/C++ symbols.
- `/usr/firmware/X6100_BBFW_V1.1.9_251127001.xgf` — STM32 baseband firmware, 392 048 bytes.
- `/etc/init.d/S11xgdaemon` — GPIO 143 keyboard backlight (PE15), GPIO 138 CI-V.
- `/etc/xgradio/xgradio.conf` — `fullband-tx=disable`.
- `/etc/asound.conf` — ALSA `hw:0,0`, dmix `mixout` / dsnoop `mixin`, period 4096.
- Device nodes used by the UI: `/dev/i2c-0`, `/dev/ttyS1` (flow), `/dev/ttyS2` (CAT/CI-V likely),
  `/dev/input/event0…4` (front panel, encoders), `/dev/rfcomm0` (BT CAT).

Baseband firmware `.xgf`:

- 128-bit block cipher in **ECB mode** (304 identical blocks = encrypted fill pattern);
  most likely AES-ECB.
- Pushed unchanged by the UI over xmodem to the STM32 bootloader (IAP:
  `xradio_set_modem_trx`, `xradio_start_xmodem`); decryption happens on the STM32.
  Expect no key on the Linux side.

Radio core API in the stock UI (C, from symbols) — superset of the 2023 library:

| Area | Functions |
|------|-----------|
| Transport | `ui2c_create/read/write/destroy`, `uart_create/thread_hook/destroy`, `xradio_send_cmd(buf,len)`, `_comm_send_cmd` |
| Memory access | `xradio_read_remote_ram(addr,buf,len)`, `xradio_write_remote_ram(addr,buf,len)`, `xradio_get_roram` |
| Telemetry | `xradio_get_report_{txpower,vswr,alclevel,vbat,vext,batcap}`, `get_trx`, `get_chgflag`, `get_vextflag`, `get_resync_flag` |
| ATU | `xradio_atu_tune`, `get_atu_status`, `get/set_atu_network(xatu_network_t*)` |
| Spectrum | `xradio_getfftmag`, `getfftpeak`, `set_fft_dec`, `set_rffftave`, `set_rffftrefoffset`, `set_fftpeak_search_border` |
| TRX control | `set_iptt`, `set_civptt`, `set_sple`, `set_swrscan_trx`, `set_calibration_trx`, `set_calflag`, `set_modem_trx`, `set_voice_recorde`, `poweroff`, `remote_poweroff` |
| Misc | `xradio_update_params`, `update_backlight`, `comm_valid`, `uartcomm_flush/suspend` |
| Network | `XRadioUdp::{init,sendControl,sendLoginRespond,sendConnectionInfo}` |

Key difference from the 2023 library: the protocol is read/write
(`ui2c_read`, `read_remote_ram`), not write-only, and has FFT-decimation and
remote-RAM access the library never mapped.

## 5. Diff: 2023 library vs stock APP 1.2.0 / BASE 1.1.9 (disassembly)

Verified in `x6100_ui_v100` (llvm-objdump, ARMv7):

| Item | 2023 lib | Stock 1.2.0 | Status |
|------|----------|-------------|--------|
| I2C bus / slave | `/dev/i2c-0`, `0x72` | `xradio_create(path, 114)` → `0x72` | same |
| I2C write format | `[addr BE16][data]` | `ui2c_write`: `rev16(addr)` + memcpy, one `I2C_RDWR` (`0x707`) msg | same |
| Register file | 56 × u32 = 224 B | two 224 B shadows at `xradio_t+36` (wanted) and `+260` (sent) | same size |
| Reg 12 flag word | ATU tune `0x2000` | `xradio_atu_tune` sets bit `0x2000` at `xradio_t+0x54` (= +36 + 12×4) | same |
| Update strategy | full 226 B resend on idle | dirty flags + diff of two shadows (`_update_regs`, `+0x277` dirty) | **new** |
| I2C read | none | `ui2c_read` (write addr, read N) | **new** |
| RO status block | none | `xradio_get_roram`: read 128 B at `0x0000` (used by System Info → BASE version) | **new** |
| Remote RAM | none | `read/write_remote_ram(addr,buf,len)`, generic 16-bit address space | **new** |
| Calibration block | none | factory mode: `0xF000`, 816 B read/modify/write | **new** |
| Command mailbox | none | `xradio_send_cmd` → `ui2c_write(0xFFFE, buf, len)`; factory mode sends 1 byte `0x01` after writing `0xF000` (likely "commit cal to flash") | **new** |
| UART | `/dev/ttyS1`, 1152000 | `uart_create(path, 0x1009)` = `B1152000` | same |
| Flow frame | magic `0xAA5555AA`, 4136 B | magic literal present; `thread_comm` checks len > `0x1027` (≥ 4136) | same |
| FFT controls | none | `set_fft_dec`, `set_rffftave` (clamped 0–10), `set_rffftrefoffset`, peak search border | host-side DSP on flow IQ |
| PTT sources | `iptt` only | `iptt` + `civptt` | **new** |
| Network | none | `XRadioUdp` login/control/connection-info | **new** |
| Firmware update | `modem_trx` bit | `set_modem_trx` + `start_xmodem`, encrypted `.xgf` | same mechanism |

Summary: the transport and core register file are unchanged; BASE 1.1.9 adds a
readable status area, a raw remote-RAM window (calibration at `0xF000`), and a
command mailbox at `0xFFFE`. The 2023 library is a write-only subset.

## 6. Step 1 — status block and I2C address space (verified)

I2C address space at slave `0x72` is split by direction:

| Address | Write | Read |
|---------|-------|------|
| `0x0000–0x00DF` | register file, 56 × u32 (reg *n* at *n*×4) | — |
| `0x0000–0x007F` | — | status block, 128 B ("roram") |
| `0xF000–0xF32F` | calibration block, 816 B (factory mode) | same |
| `0xFFFE` | command mailbox | — |

Read transaction (`ui2c_read`): a write of the 2-byte BE address with no data
(own transaction, STOP), then a separate `I2C_M_RD` message of N bytes. No repeated start.

Register push (`thread_remote_admin`, 1 ms loop):

- compares wanted (`xradio_t+36`) with sent (`+260`) for regs 0…55; each changed
  register is written alone: address `n*4`, 4 bytes;
- periodically writes all 224 bytes at address `0x0000`.

Status block content, as far as the UI uses it:

| Offset | Content | Source |
|-------:|---------|--------|
| 0 | NUL-terminated UTF-8 BASE version string | `DialogSystemInfo::exec`: `strlen` + `QString::fromUtf8`, shown as `BASE : %1` |
| 0x1C–0x7F | all zero on a live radio (BASE V1.1.8, RX) | live dump 2026-10-17 |

Live dump, BASE `V1.1.8 Sep 15 2024,15:22:37`: the string is 27 bytes plus NUL, the
rest of the 128 bytes is zero. After flashing stock v1.2.0: `V1.1.9 Nov 27 2025,10:31:57`,
same layout (27 bytes + NUL, rest zero). The block works as a version/ID area, not as telemetry;
telemetry comes over the UART flow frame.

The UI never reads any other field, so the rest of the block can only be decoded
from a real radio: `x6100_rodump` (read-only) writes `roram.bin`, and with `-c` also
`cal_f000.bin`. Dumps taken in different states (RX, TX into a dummy load, different band)
show which bytes are live telemetry.

## 7. Calibration block @ 0xF000 (verified, CRC match)

Layout from `DialogFactoryMode` (buffer at `this+0x90`), confirmed on a live
BASE 1.1.9 dump: the stored CRC `0x71FAE50F` matches the calculated one. C definition in `x6100_cal.h`.

| Offset | Size | Field | Live value |
|-------:|-----:|-------|-----------|
| 0x000 | 4 | magic `0xAA5555AA` | ok |
| 0x004 | 4 | bias_drive (reg 41 [15:0]), default 500 | 576 |
| 0x008 | 4 | bias_final (reg 41 [31:16]), default 650 | 715 |
| 0x00C | 50 × 8 | per band: rmagcmp, rphscmp (regs 49/48) | all 0 |
| 0x19C | 50 × 8 | per band: txiofs, txqofs (regs 50/51) | see below |
| 0x32C | 4 | CRC-32 STM32 over words 0…202 | `0x71FAE50F` |

Band slot = band index of reg 13 [15:8] (`XParamAdmin::bandIndex`); 50 slots, 23 in use,
values set only for the odd (in-band) indices.

TX DC offset, this radio:

| Slot | Band | txiofs | txqofs |
|-----:|------|-------:|-------:|
| 1 | 160 m | −540 | −610 |
| 3 | 80 m | −390 | −480 |
| 5 | 60 m | −350 | −360 |
| 7 | 40 m | −280 | −320 |
| 9 | 30 m | 0 | −240 |
| 11 | 20 m | 110 | −110 |
| 13 | 17 m | 440 | 0 |
| 15 | 15 m | 410 | 0 |
| 17 | 12 m | 750 | −280 |
| 19 | 10 m | 1260 | 50 |
| 21 | 6 m | −80 | −560 |

Write sequence (factory mode): recompute CRC → `write_remote_ram(0xF000, buf, 816)` →
`send_cmd(0xFFFE, {0x01})`, which most likely commits to STM32 flash.

Observations:

- RX IQ compensation is not calibrated on this unit (all zero).
- The UI reads the block; it is the source of bias and TX DC offset per band. The
  STM32 itself presumably applies them on band change (to verify: read registers 41, 50
  and 51 after a band switch).

## 8. Register file, BASE 1.1.9 (from `xradio_update_params`)

Shadow offset = 0x24 + reg × 4. C definitions in `x6100_regs.h`. Changes vs the 2023 library
are marked **new** / **changed**.

| Reg | Fields |
|----:|--------|
| 0–11 | VFO A/B: band, freq, att, pre, mode, agc (unchanged) |
| 12 | flags: sple 0x2, voice_rec 0x8, swrscan 0x10, atue 0x1000, atu_tune 0x2000, modem 0x4000, cal 0x8000, power_off 0x10000, iptt 0x40000 |
| 13 | [7:0] vi, [15:8] band index, **[23:16] vm** |
| 14 | rxvol |
| 15 | [7:0] rfg, [15:8] txpwr×10, **[23:16] fftdec** |
| 20 | ling, loutg, imicg, hmicg (8 bits each) |
| 21 | [1:0] micsel, **[3:2] pttmode**, [4] chge, [5] spmode, **[6] auxiqen**, [15:8] sqlthr |
| 22 | [6:0] voxg, [13:7] voxag, [25:14] voxdly, [26] voxe |
| 23 | [7:0] nrthr, [15:8] nbw, [23:16] nbthr, [24] nre, [25] nbe |
| 24 | [11:0] dnfcnt, [23:12] dnfwidth, [24] dnfe |
| 25 | **[3:0] cmplevel (0 off, 1:2, 1:4, 1:8), [4] cmpe** |
| 27 | [7:0] agcknee, [11:8] agcslope, [12] agchang |
| 28 | **changed:** [9:0] agc_tcslow, [19:10] agc_tcmid, [29:20] agc_tcfast, unit 10 ms (defaults 250/50/5 = 2.5 s/0.5 s/50 ms) — was a single agctime |
| 29 | monilevel |
| 33 | [7:0] ks, [9:8] km, [11:10] kimb, [22:12] cwtone, [28:23] cwvol, [29] cwtrain |
| 34 | [15:0] qsktime, [31:16] kr×10 |
| 35 | **new:** [23:0] LO/cursor offset (from UI state), [24] center_mode |
| 41 | [15:0] bias_drive, [31:16] bias_final |
| 42 / 43 | rit / xit |
| 44–47 | filter1/2 low/high (set elsewhere in the UI, not in update_params) |
| 48–51 | rphscmp, rmagcmp, txiofs, txqofs |
| 53 | pwrsync |

Notes:

- Regs 28 and 35 match the v1.2.0 changelog (AGC as time constants in ms, spectrum
  cursor mode with CENTER MODE off).
- The calibration values (41, 48–51) are pushed **by the UI** from its parameter store
  (`wparam`), not applied autonomously by the STM32. A replacement firmware must load
  them from the 0xF000 block per band and write the registers itself.
- Still unknown: 16, 18–19, 26, 30–32, 36–40, 52, 54 (never written by the UI → presumably unused).

## 9. Hardware verification, libx6100 (2026-10-18)

BASE 1.1.9, GUI stopped, own code only:

| Test | Result |
|------|--------|
| status block, calibration read + CRC | ok |
| flow frames, CRC | ok, 0 errors; ~27.5 frames/s (36 ms) with fftdec = 0, ~19/s under the R1CBU GUI |
| register write (full + diff) | ok, no I2C errors |
| RX 14.074 / 7.074 MHz USB | tuned, FT8 audible on speaker (rxvol 20) |
| IQ level, no antenna / antenna | −97 dB / −58…−63 dB |
| TX | not tested, blocked in tooling |

## Open items for a clean implementation

- Unknown registers (gaps above) and unmapped bits in 21 and 25.
- Audio path: ALSA device, sample rate, which codec; how `voice_rec` changes routing.
- ATU params / atu_network encoding.
- Diff against gdyuldin/x6100_gui for registers added with newer base firmware.
- Front-panel keys/encoders: not in this library (likely evdev/input on the SoC).
- Kernel/device tree: AetherRadio/X6100Buildroot and panel-jinglitai-jlt4013a.
