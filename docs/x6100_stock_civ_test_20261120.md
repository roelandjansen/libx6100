# X6100 stock firmware: CI-V and wfview test list

Date: 20261120

The stock X6100 (Xiegu firmware, original UI), its own wfserver, wfview as an IC-705
(CI-V address `0xA4`) over the LAN. What works, what does not: to fill in
`x6100_civ_compare.md` (the Xiegu column: what the stock firmware really answers, not
what the Xiegu reference lists) and to send to Jin with the known gaps.

Tick `[x]` when it works, `[-]` when it does not, and note what was seen. For a
command wfview has no button for, the hamlib line (`rigctl -m 3085 -r <port>`, IC-705)
or the raw CI-V frame is given.

## 0. Setup

- [ ] Stock firmware version (UI, BASE) noted: ______
- [ ] wfserver running on the X6100; wfview connects (Network, the X6100's address, its control port)
- [ ] wfview finds the radio as IC-705 with the CI-V address on auto (19 00 to a broadcast answered)
- [ ] The same with the address set by hand to `A4`
- [ ] RX audio in wfview: heard, no dropouts over a few minutes
- [ ] wfview stays connected 10 minutes without "Radio not responding"

## 1. Frequency, mode, VFO, split

- [ ] Frequency follows the X6100 VFO knob in wfview (transceive 00)
- [ ] wfview tunes the X6100 (05 / 00); a frequency outside the range gives NG, not a hang
- [ ] Mode from wfview: LSB, USB, CW, CW-R, AM, FM (06); the X6100 follows
- [ ] Mode changed on the X6100 shows in wfview (01)
- [ ] Filter FIL1 / FIL2 / FIL3 from wfview (06 with filter byte)
- [ ] Data mode: USB-D from wfview (1A 06)
- [ ] VFO A / B select (07 00 / 01)
- [ ] A=B button (07 A0): *not in the Xiegu reference*
- [ ] A<->B button (07 B0)
- [ ] Split on / off (0F 00 / 01); TX on the other VFO
- [ ] Duplex - / + / simplex (0F 11 / 12 / 10): *not in the Xiegu reference*; hamlib `R -` / `R +` / `R None`
- [ ] Both VFOs shown in wfview (25 00 / 01, 26 00 / 01)
- [ ] TX frequency read (1C 03): *not in the Xiegu reference*; with split on, wfview shows the TX side
- [ ] Tuning step (10); hamlib `N 1000`, `n`

## 2. Memories

- [ ] wfview memory window opens and lists the channels (1A 00). Note: wfview's own IC-705.rig has a broken Memory Contents string (`\\x1a\\00`): if the window times out on every channel, it may be that, not the X6100
- [ ] Memory mode on a channel (08 0001); back to VFO (07)
- [ ] Memory to VFO (0A)
- [ ] Memory write (09) and clear (0B): careful, overwrites a channel

## 3. Levels (14)

For each: the wfview slider moves the X6100 setting, and a change on the X6100 shows in
wfview.

- [ ] AF gain (14 01)
- [ ] RF gain (14 02)
- [ ] Squelch (14 03)
- [ ] NR level (14 06)
- [ ] Twin PBT inner / outer (14 07 / 08)
- [ ] CW pitch (14 09)
- [ ] RF power (14 0A)
- [ ] Mic gain (14 0B)
- [ ] Key speed (14 0C)
- [ ] Notch position (14 0D)
- [ ] Compressor level (14 0E)
- [ ] Break-in delay (14 0F)
- [ ] NB level (14 12)
- [ ] Monitor level (14 15)
- [ ] VOX gain / anti-VOX (14 16 / 17)

## 4. Meters (15)

- [ ] S-meter moves with the signal and matches the X6100 (15 02; S9 = 0120)
- [ ] Squelch status (15 01 / 05)
- [ ] OVF (15 07)
- [ ] While transmitting into a dummy load: Po (15 11), SWR (15 12), ALC (15 13)
- [ ] Vd (15 15)

## 5. Switches (16)

- [ ] Preamp (16 02)
- [ ] AGC fast / mid / slow (16 12)
- [ ] NB on / off (16 22)
- [ ] NR on / off (16 40)
- [ ] Notch / DNF (16 41; IC-705 manual notch 16 48)
- [ ] Compressor / monitor / VOX (16 44 / 45 / 46)
- [ ] Break-in (16 47; wfview polls it in CW)
- [ ] Dial lock (16 50)

## 6. Attenuator, ID, CW, misc

- [ ] ATT on / off (11)
- [ ] Transceiver ID (19 00) answers A4
- [ ] Xiegu radio ID (1D 19)
- [ ] Band buttons in wfview (1A 01, band stacking register)
- [ ] IF filter width (1A 03)
- [ ] CI-V transceive off / on (1A 05 0131)
- [ ] DATA OFF MOD / DATA MOD input (1A 05 0118 / 0119): which values the X6100 takes (MIC, USB, MIC+USB, WLAN)
- [ ] Dash ratio (1A 05 0252)
- [ ] wfview Set Clock (1A 05 0165 / 0166 / 0170)
- [ ] Send CW text from wfview (17), and stop (17 FF)
- [ ] Voice / message memory (28 00 01); hamlib `send_voice_mem 1`
- [ ] Scan (0E 01 / 02 / 00): hamlib `g VFO 0`, `g MEM 0`, `g STOP 0`

## 7. TX, tuner, RIT / XIT

Into a dummy load, low power.

- [ ] PTT from wfview keys the X6100 (1C 00 01) and unkeys it (1C 00 00)
- [ ] TX state read back (1C 00): wfview shows TX also when keyed on the radio
- [ ] Tuner on / off / tune (1C 01)
- [ ] RIT offset and on / off (21 00 / 01)
- [ ] XIT on / off (21 02)

## 8. TX audio over the LAN (reported broken by many users)

wfview sends the PC microphone (or a digital mode program's audio) to wfserver, which
should play it into the X6100's TX path. Check each step so we know where it stops.

- [ ] wfview Settings, Audio: a TX device chosen, TX codec noted: ______, sample rate: ______
- [ ] wfview's TX level meter moves while talking (the PC side works)
- [ ] USB mode, wfview PTT: the X6100 keys (TX shown on the radio)
- [ ] While keyed and talking: ALC moves on the X6100 / in wfview
- [ ] While keyed and talking: Po rises above the idle level
- [ ] The audio is heard on a second receiver (clean, no chopping, no wrong speed / pitch)
- [ ] The same in USB-D (data mode)
- [ ] DATA OFF MOD set to USB (X6100 menu or 1A 05 0118 01): does TX audio come then?
- [ ] DATA MOD set to USB (1A 05 0119 01) for USB-D
- [ ] The X6100 microphone is muted while LAN audio is sent (or mixed in: note which)
- [ ] TX audio with the PTT on the radio (its own key) instead of wfview: mic or LAN?
- [ ] Other TX codecs in wfview (PCM16, µ-law, Opus): any that work?
- [ ] Unkey: the X6100 goes back to RX at once, no stuck TX
- [ ] Notes on what fails (keys but no modulation, no keying at all, modulation late or chopped, wrong level): ______

## 9. Scope (27)

- [ ] Waterfall / spectrum shows in wfview (27 00, 27 10 / 11)
- [ ] Center / fixed / scroll mode (27 14)
- [ ] Span (27 15)
- [ ] Fixed edges (27 16, 27 1E)
- [ ] Hold (27 17)
- [ ] Reference level (27 19)
- [ ] Sweep speed (27 1A)
- [ ] Scope during TX (27 1B)
- [ ] Center type (27 1C), VBW (27 1D)

## 10. Behaviour

- [ ] A change on the X6100 (any level or switch above) shows in wfview without waiting for its poll (transceive)
- [ ] An unsupported command gets NG (FA), not silence: wfview does not stall on it
- [ ] Power on / off from wfview (18 01 / 18 00): what happens
- [ ] wfview disconnects and connects again without restarting wfserver or the radio
