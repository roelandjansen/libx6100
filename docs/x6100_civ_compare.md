# X6100 CI-V: Xiegu firmware and x6100_trx compared

Status: 2026-11-20 (the commands as of 2026-11-07, checked against `src/x6100_civ.c`).
For Xiegu, to bring the X6100 firmware's CI-V up to what clients
such as wfview, hamlib and loggers expect from an IC-705.

x6100_trx (an open front end for the X6100, `tools/x6100_trx.c`, engine
`src/x6100_civ.c`) answers CI-V as an **IC-705** (address `A4`) or as an **X6100**
(address `61`). Every command below is implemented and, where marked, verified with
wfview 3 (v3-initial) on the radio. Full byte formats: `x6100_civ.md` (same folder).

Columns:

- **IC-705**: in the Icom IC-705 CI-V Reference Guide.
- **Xiegu**: in the *XIEGU RADIO CI-V REFERENCE* v1.0 (X6100, G90, G106): **y** yes,
  **n** no, **?** not checked against the Xiegu reference (please fill in).
- **x6100_trx**: **y** implemented, **v** implemented and verified with wfview / rigctl
  on the radio, **NG** answered NG (no such function on the X6100).

## Stock firmware as tested (20261120)

The stock X6100 (Xiegu firmware and UI) with its own wfserver, wfview as an IC-705 at
`A4` over the LAN. What it really does, apart from what the Xiegu reference lists; the
full list and what is not tested yet: `x6100_stock_civ_test_20261120.md`. **Stock**: as
tested on the stock firmware (**ok**, **fails**, **slow**: only by wfview's poll, no
transceive). **x6100_trx**: the same function there.

| Cmd | Function | Stock | x6100_trx |
|---|---|---|---|
| LAN audio | RX audio over the LAN (wfserver) | **fails**: nothing heard (AF gain from wfview does change the radio) | ok |
| LAN audio | TX audio over the LAN (wfserver) | **fails**: none at all, though wfview PTT keys the radio | ok (DATA OFF MOD / DATA MOD LAN) |
| 00 / 05 | frequency, both ways | ok | ok |
| 25 / 26 | selected / unselected VFO | **fails**: the receiver tunes the VFO not shown (radio and wfview show 80 m, the audio is MW, and the other way round) | ok |
| 07 00 / 01 | select VFO A / B | works both ways, but the radio does odd things after it | ok |
| 07 B0 | exchange A / B | ok | ok |
| 01 / 06 | mode, FIL1-3 | ok from wfview; a change on the radio **slow** | ok, transceive |
| 1A 06 | data mode (USB-D, LSB-D) | ok from wfview | ok |
| 0F 00 / 01 | split | ok | ok |
| 10 | tuning step | **fails** both ways | ok |
| 1A 01 | band stacking register (wfview band buttons) | **fails**: 40 m to 20 m sends the radio to 500 kHz; band up / down on the radio with wfview connected makes the radio fail (once the UI restarted) | ok (IC-705 band codes) |
| 1A 00 | memories (wfview memory window) | **fails** (maybe also wfview's IC-705.rig bug) | ok |
| 1A 03 | filter width | **fails**: one fixed width in wfview | ok |
| 14 01 / 02 / 03 | AF, RF gain, squelch | ok from wfview; RF gain on the radio not seen, SQL **slow** | ok, transceive |
| 16 02, 11 | preamp, attenuator | ok from wfview; on the radio not seen | ok, transceive |
| 16 12 | AGC | **fails**: wfview MID sets SLOW, SLOW sets AUTO (FAST ok); on the radio not seen | ok |
| 16 22 / 40 | NB, NR on / off | ok from wfview; on the radio not seen | ok, transceive |
| 15 02 | S-meter | ok | ok |
| 1C 00 | PTT, TX state | ok both ways | ok |
| 1C 01 | tuner | tune ok; the on state **fails** in wfview (the radio shows ATU on) | ok |
| 21 00 / 01 | RIT | ok | ok |
| 18 00 / 01 | power off / on | ok | 18 01 ok, 18 00 NG (by choice) |
| 27 | scope and waterfall, reference level | ok | ok |
| 27 14 | scope center / fixed / scroll | radio -> wfview seen, but not in wfview's drop-down; **set from wfview fails** | ok |
| 27 17 | scope hold | **fails** | ok (freezes the radio's own scope too) |

## Known gaps in the Xiegu reference

Checked so far (section 1): these IC-705 commands are not in the Xiegu reference, while
x6100_trx has them and wfview uses them:

- `07 A0`: VFO A = VFO B (wfview A=B button)
- `0F 10 / 11 / 12`: simplex / DUP- / DUP+ (repeater duplex)
- `1C 03`: read TX frequency (split and duplex)

Sections 2-8 are not yet checked against the Xiegu reference (Xiegu column **?**):
every row there with **?** may be another gap.

The Xiegu column is what the Xiegu reference document lists, not what the stock
firmware actually answers: that needs the stock code (Jin's CI-V rewrite, TODO.md "From
Xiegu") or a test of each **?** row against a stock X6100.

## 1. Frequency, mode, VFO, split

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 00 | set frequency, no reply (transceive form) | y | y | v | |
| 01 | set mode [filter], no reply | y | y | v | |
| 03 | read frequency | y | y | v | |
| 04 | read mode | y | y | v | |
| 05 | set frequency | y | y | v | |
| 06 | set mode [filter] | y | y | v | |
| 07 | VFO mode | y | y | v | leaves memory mode, the VFO back as it was |
| 07 00 / 01 | select VFO A / B | y | y | v | |
| 07 A0 | VFO A = VFO B | y | **n** | v | wfview A=B button |
| 07 B0 | exchange A / B | y | y | v | wfview A<->B button |
| 0F | read split / duplex | y | y | v | |
| 0F 00 / 01 | split off / on | y | y | v | |
| 0F 10 / 11 / 12 | simplex / DUP- / DUP+ | y | **n** | v | repeater duplex |
| 10 | tuning step | y | y | y | |
| 25 00 / 01 | selected / unselected VFO frequency | y | y | v | wfview uses it for both VFOs |
| 26 00 / 01 | selected / unselected VFO mode, data, filter | y | y | v | |
| 1C 03 | read TX frequency | y | **n** | v | split and duplex |

## 2. Memories

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 08 | memory mode | y | ? | v | wfview V/M button: keeps the VFO, tunes the channel |
| 08 0001-0099 | select channel | y | ? | v | |
| 09 | memory write | y | ? | y | NG unless `catmem rw` (default read only, see section 9) |
| 0A | memory to VFO | y | ? | y | |
| 0B | memory clear | y | ? | y | NG unless `catmem rw` |
| 1A 00 | memory contents (IC-705 layout, 111 bytes: A side, B side, name) | y | ? | v | wfview memory window reads all channels (2026-11-02). As IC-705: 1A 00 gg gg cc cc (group two bytes); as X6100 (X6100.rig, no groups): 1A 00 cc cc, IC-7300 style. Empty channel: FF. Write NG unless `catmem rw` |

## 3. Levels (14, 0000-0255)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 14 01 | AF gain | y | ? | v | |
| 14 02 | RF gain | y | ? | v | |
| 14 03 | squelch | y | ? | y | |
| 14 06 | NR level | y | ? | v | wfview NR slider |
| 14 07 / 08 | twin PBT inner / outer | y | ? | v | 0128 centre, +-1250 Hz; also in AM |
| 14 09 | CW pitch | y | ? | y | |
| 14 0A | RF power | y | ? | y | |
| 14 0B | mic gain | y | ? | y | |
| 14 0C | key speed | y | ? | y | |
| 14 0D | notch position | y | ? | y | |
| 14 0E | compressor level | y | ? | y | |
| 14 0F | break-in delay | y | ? | y | |
| 14 12 | NB level | y | ? | v | wfview NB slider |
| 14 15 | monitor level | y | ? | y | |
| 14 16 / 17 | VOX gain / anti-VOX | y | ? | y | VOX itself does not key on BASE 1.1.9 |
| 14 19 | backlight | y | ? | y | read 0000, set NG (no backlight level from CAT) |

## 4. Meters (15, read)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 15 01 / 05 | squelch status | y | ? | y | |
| 15 02 | S-meter | y | ? | v | IC-705 scale: 0120 = S9 |
| 15 07 | OVF | y | ? | y | |
| 15 11 | Po | y | ? | y | |
| 15 12 | SWR | y | ? | y | |
| 15 13 | ALC | y | ? | y | |
| 15 15 | Vd | y | ? | y | |
| 15 14 / 16 | COMP / Id | y | ? | NG | |

## 5. Switches (16)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 16 02 | preamp | y | ? | v | |
| 16 12 | AGC | y | ? | y | 01 fast, 02 mid, 03 slow; in wfview rig files AGC Status (the IC-705 file has it as AGC Time Constant, which is 1A 04) |
| 16 22 | NB on / off | y | ? | v | |
| 16 40 | NR on / off | y | ? | v | |
| 16 41 | notch (Xiegu: DNF; IC-705: auto notch) | y | y | y | |
| 16 48 | manual notch (IC-705) | y | ? | y | same notch as 41 (the X6100 has one) |
| 16 44 / 45 / 46 | compressor / monitor / VOX | y | ? | y | |
| 16 47 | break-in (00 off, 01 semi, 02 full) | y | ? | y | semi only on the X6100; wfview polls it in CW |
| 16 50 | dial lock | y | ? | y | |

## 6. Attenuator, ID, CW, misc (11, 17, 19, 1A, 1D)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 11 | ATT off / on, read | y | ? | v | |
| 17 | send CW (text), FF stops | y | ? | y | buffered as on an Icom |
| 28 00 | voice TX memory T1-T8, 00 stops | y | ? | y | x6100_trx: message M1-M8 (voice WAV or CW); for hamlib `send_voice_mem`, the wfview GUI does not use it |
| 0E 00 / 01 / 02 / 22 | scan stop / programmed / memory / select memory | y | ? | y | x6100_trx (2026-11-20): band scan over the band the VFO is in, memory scan through the stored channels; the squelch holds it. 0E D0 / D3 resume off / on. Fine scan, delta-f spans, select marks NG |
| 19 00 | transceiver ID | y | y | v | also answered to a broadcast (00): wfview auto-detect needs it |
| 1A 01 | band stacking register | y | y (changelog) | v | wfview band buttons; IC-705 code 15 = GEN band |
| 1A 03 | IF filter width | y | ? | y | |
| 1A 05 0131 | CI-V transceive on / off | y | ? | y | |
| 1A 05 0118 / 0119 | DATA OFF MOD / DATA MOD (00 MIC, 01 USB, 02 MIC,USB, 03 LAN) | y | ? | v | 03 LAN (the IC-705 WLAN): TX audio from the network for wfview when CAT keys, the mic for the PTT key; 02 also sets LAN (2026-11-07) |
| 1A 05 0045 | Quick SPLIT | y | ? | y | read: off; fixed |
| 18 01 / 18 00 | power on / off | y | ? | y | 18 01 accepted (it is on), 18 00 NG: x6100_trx does not switch the radio off from CAT |
| 1A 05 0252 | dash ratio | y | ? | y | wfview polls it in CW |
| 1A 05 0165 / 0166 / 0170 | date, time, UTC offset | y | ? | y | wfview Set Clock / set radio time on connect; x6100_trx sets the Linux clock only while NTP does not keep it (`cattime`) |
| 1A 06 | data mode and filter | y | ? | y | |
| 1D 19 | Xiegu radio ID | n | y | y | |

## 7. TX, tuner, RIT / XIT (1C, 21)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 1C 00 | read TX / PTT on / off | y | ? | v | wfview TX audio over the network |
| 1C 01 | tuner off / on / tune, read | y | ? | v | wfview TUNE |
| 21 00 | RIT / XIT offset | y | ? | v | |
| 21 01 | RIT on / off | y | ? | v | |
| 21 02 | XIT on / off | y | ? | y | |

## 8. Scope (27)

| Cmd | Function | IC-705 | Xiegu | x6100_trx | Notes |
|---|---|---|---|---|---|
| 27 00 | waveform data, streamed | y | ? | v | IC-705 frame layout; FIX / SCROLL send the edges |
| 27 10 / 11 | scope on / off, wave output on / off | y | ? | v | |
| 27 12 / 13 | main / sub, single / dual | y | ? | y | main, single only |
| 27 14 | CENTER / FIX / SCROLL-C / SCROLL-F | y | ? | v | |
| 27 15 | span | y | ? | y | |
| 27 16 | fixed edge in use | y | ? | y | |
| 27 17 | hold | y | ? | v | freezes the radio's own scope too |
| 27 19 | reference level | y | ? | y | |
| 27 1A | sweep speed | y | ? | y | |
| 27 1B | scope during TX | y | ? | y | |
| 27 1C | center type | y | ? | y | |
| 27 1D | VBW | y | ? | y | |
| 27 1E | fixed edge frequencies | y | ? | y | wfview fixed edge window |

## 9. Behaviour clients rely on

These are not single commands but matter as much:

- **Transceive.** On every change the radio sends, to address 00: frequency (00) and
  mode (01), and also the levels and switches above (14 xx, 16 xx, 11, 0F, 27 17) when
  they change. A client that made the change does not get it echoed back. Without this
  wfview does not follow changes made on the radio (verified).
- **Broadcast.** 19 00 to address 00 is answered (wfview finds the radio this way); other
  commands to 00 are acted on but not answered.
- **NG, not silence.** A command the radio does not support is answered `FA`. A client
  waits for silence until its timeout; NG lets it move on.
- **Scope over serial.** 27 00 is paced by the port: a new sweep only when the previous
  one has left (about 3 sweeps / s at 19200 baud on the DEV port).
- **Memory group bytes.** wfview sends 1A 00 with a two-byte group for the IC-705 (gg gg
  cc cc) and none for a radio without groups (cc cc, as the IC-7300). Answering the wrong
  form with NG made the wfview memory window time out (found 2026-11-02).
- **wfview IC-705 rig file.** Its Memory Contents string is `\\x1a\\00` (the x missing),
  so wfview sends 1A 5C 30 30 and gets NG: with that file the memory window cannot work
  on any radio, a real IC-705 included. X6100.rig has it right; a one-line fix for wfview
  is in `contrib/wfview/patches`. Worth knowing when testing memory commands with wfview.
- **Memories read only from CAT.** Any client on the CI-V port can overwrite or clear all
  channels. x6100_trx answers writes and clears (09, 0B, 1A 00 with content) NG unless
  the user allows it (`catmem rw`, GEN row CAT MEMORIES); reading and using channels
  always works. A suggestion for the Xiegu firmware too: a menu switch for it.
- **Clock from CAT.** wfview can set the radio clock (1A 05 0170 offset, 0166 time at
  second 00, 0165 date). On the X6100 that is the Linux clock: x6100_trx takes it only
  while NTP does not keep the clock (no WiFi in the field), so a PC with a wrong clock
  cannot overwrite a synced one; otherwise NG.

## 10. Not on the X6100 (answered NG)

0C / 0D, 1B, 1F-24 (duplex offset, tones, D-STAR, GPS); 13 speech;
(these are also left out of the wfview rig file X6100.rig, with the other IC-705
functions the X6100 lacks, so wfview neither polls them nor shows their controls);
04 RTTY, 06 WFM, 08 RTTY-R, 17 DV modes; 15 14 / 16 meters.
