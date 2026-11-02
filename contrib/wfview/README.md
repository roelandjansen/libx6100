# wfview rig file for the X6100 (x6100_trx)

`X6100.rig` (rig file format 2.10) lets wfview talk to x6100_trx as an **X6100** instead of an
IC-705. It is made from wfview's own `rigs/IC-705.rig` by `mkrig.sh` (bash + awk), so
it follows the IC-705 file when wfview changes it.

Made from: wfview `v3-initial`, commit 00c62ce (2026-06-02), `rigs/IC-705.rig`
(wfview, GPL-3.0; the rig file stays under its licence).

## Use

1. Copy `X6100.rig` into wfview's rig directory (next to `IC-705.rig`), or load it as a
   user rig file.
2. On the radio: GEN, row CI-V ADDRESS, turn to X6100 61 (at once, kept), or `civaddr 61` in
   `/etc/x6100/x6100_trx.conf` (then the GEN row is fixed).
3. In wfview: CI-V address 61 (hex, decimal 97), or auto: x6100_trx answers the 19 00
   broadcast with its address and wfview picks the rig file that has it.

With `civaddr a4` (the default) x6100_trx stays an IC-705 and wfview uses its IC-705 file.

## What differs from the IC-705 file

The X6100 works the Icom way: the IC-705 commands and polls stay in the file, except for
the 29 functions the X6100 does not have (2026-11-02; before, they stayed and x6100_trx
answered NG): tones / CTCSS / DTCS (16 42 43 5D, 1B 00 01 02 07), APF and twin peak
(14 05, 16 32 4F), DIGI-SEL, drive gain, the COMP and Id meters, RX antenna, filter
shape, SSB TX bandwidth, power control (18), REF adjust, USB / LAN mod level, XFC,
CI-V Output, scope RBW, voice TX (28), scan (0E), date / time / UTC offset. wfview
polled some of them every few seconds for an FA (16 5D, the FM tones) and showed the
others as controls that did nothing. Every command left is answered by x6100_trx (a
read without its parameter, as 1A 00, answers NG).
The commands, the 1A 00 memory layout (A and B side and name, but no group bytes) and the scope
data are the IC-705's. Only the identity and what the radio can do differ:

| Item | IC-705 file | X6100.rig |
|---|---|---|
| Model, CI-V address | IC-705, A4 | X6100, 61 (odd: never a real Icom; the Xiegu ID 1D 19 is 61 00) |
| rigctld model | 3085 | 3087 (hamlib `RIG_MODEL_X6100`) |
| LAN, WiFi | yes | no (network: wfserver on the radio over `cat pty`) |
| Commands | 124 | the same 124, plus Notch Filter (14 0D, notch position) |
| CI-V transceive | 1A 05 0112 | 1A 05 0131 (the IC-705 item, as hamlib has it; 0112 is the IC-7610's) |
| Tuning step | codes 0-8 | 0-13 (as the IC-705's step list in the same file) |
| Scope spans | ±2.5-500 kHz | ±1.5, 3, 6, 12, 24, 48 kHz (the 96 kHz IQ); Scope Span max 5 |
| Bands | HF, 6 m, 2 m, 70 cm, air, WFM, LF | HF, 60 m, 6 m; Gen 0.5-54 MHz |
| Modes | 10 | LSB, USB, AM, CW, FM, CW-R; AM filter up to 6 kHz |
| Preamp | off, 1, 2 | off, on (Preamp Status max 1) |
| Inputs | MIC, USB, M/U, WLAN | MIC, USB |
| Memories | 100 groups, 1A 00 gg gg cc cc | one bank, channels 1-99, 1A 00 cc cc (MemGroups=0, MemFormat without the group: wfview then sends no group, as for an IC-7300; with the group field it timed out, 2026-11-02) |

Fixed edges (27 16 / 27 1E) are kept: wfview counts the band `Range` thresholds to get
the IC-705 range code, which gives the same 12 HF ranges as x6100_trx.

## Make it again

```
bash mkrig.sh <wfview>/rigs/IC-705.rig > X6100.rig
```

The bands, modes and changes are at the top of the awk program in `mkrig.sh`.

## Status

Built 2026-10-27, not tried with wfview yet. To check: wfview loads it and connects
(auto and 61), scope and fixed edges, memory window (read, write, name, split TX side),
and that the NG answers on the items we lack do not upset it.
