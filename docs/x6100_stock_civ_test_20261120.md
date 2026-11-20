# X6100 stock firmware: CI-V and wfview test list

Date: 20261120

The stock X6100 (Xiegu firmware, original UI), its own wfserver, wfview as an IC-705
(CI-V address `0xA4`) over the LAN. What works, what does not: to fill in
`x6100_civ_compare.md` (the Xiegu column: what the stock firmware really answers, not
what the Xiegu reference lists) and to send to Jin with the known gaps.

Marks: `[x]` works, `[-]` does not, `[ ]` not tested yet. Two directions where it
matters: **wfview -> X6100** (wfview sets it, the radio follows) and **X6100 -> wfview**
(changed on the radio, wfview shows it: transceive or wfview's poll). For a command
wfview has no button for, the hamlib line (`rigctl -m 3085 -r <port>`, IC-705) or the
raw CI-V frame is given.

## Results 20261120 (first round, wfview IC-705 rig file)

Broken or wrong:

- **RX audio**: AF gain from wfview works on the radio, but no audio is heard in wfview
- **TX audio** from wfview: none at all, although wfview PTT keys the radio (not debugged
  further on the stock firmware)
- **VFO A / B**: the radio and wfview both show 80 m (3630 kHz) but the audio is MW (1179
  kHz), and the other way round: the receiver tunes the VFO not shown (the VFO knob works
  both ways)
- **Band change**, with wfview connected: band up / down on the X6100 makes **the radio
  fail** (once the stock UI restarted); a band button in wfview (40 m to 20 m, 1A 01
  band stacking register) sends the radio to **500 kHz**
- **AGC values shifted**: wfview MID sets SLOW, SLOW sets AUTO, FAST sets FAST
  (16 12: 02 / 03 mapped differently from the IC-705 01 fast, 02 mid, 03 slow)
- **Tuning step** from wfview: no effect (10)
- **ATU**: tune from wfview works, but the ATU on state does not stay on in wfview while
  the radio shows ATU on (1C 01 read answers off or not at all)
- **Filter width**: wfview shows one fixed width, does not follow the radio's (1A 03; to
  confirm: the passband in wfview, or the scope width)
- **Memories**: the memory window does not work (1A 00; possibly wfview's own IC-705.rig
  bug, see section 2)
- **Transceive missing**: changes made on the radio do not reach wfview for AGC, tuning
  step, RF gain, PRE, ATT, NR, NB; the squelch and the mode do, but slowly (only wfview's poll, no
  transceive)

Works: frequency both ways (VFO knob, wfview tuning), USB-D / LSB-D from wfview, S-meter, PTT both ways (wfview keys
the radio, the radio's TX seen in wfview), power on / off, spectrum and waterfall, A <-> B, RIT, FIL1 / 2 / 3, mode select, split on / off, AF gain, RF gain, SQL,
ATT, PRE, NB, NR (all wfview -> X6100), reference level, TX state seen in wfview.

Not tested yet (what is left): setup checks,
VFO A / B select, A=B, duplex, TX frequency (1C 03), memory mode /
write; the other levels (NR level, PBT, CW pitch, RF power, mic gain, key speed, notch
position, compressor, break-in delay, NB level, monitor, VOX); the other meters (Po,
SWR, ALC, Vd, OVF, squelch status); notch, compressor, monitor, VOX, break-in, lock
switches; ID, transceive off / on, DATA OFF MOD / DATA MOD, dash ratio,
clock, CW text, voice memory, scan; XIT;
the scope apart from the display and the reference level; behaviour (NG, reconnect).

## 0. Setup

- [ ] Stock firmware version (UI, BASE) noted: ______
- [ ] wfserver running on the X6100; wfview connects (Network, the X6100's address, its control port)
- [ ] wfview finds the radio as IC-705 with the CI-V address on auto (19 00 to a broadcast answered)
- [ ] The same with the address set by hand to `A4`
- [-] RX audio in wfview: **nothing heard** (AF gain from wfview does change the radio)
- [ ] wfview stays connected 10 minutes without "Radio not responding"

## 1. Frequency, mode, VFO, split

- [x] Frequency follows the X6100 VFO knob in wfview (transceive 00)
- [x] wfview tunes the X6100 (05 / 00) (out of range NG: not tested)
- [x] Mode from wfview (06); the X6100 follows
- [x] Mode changed on the X6100 shows in wfview (01): **slowly** (wfview's poll, no transceive 01)
- [x] Filter FIL1 / FIL2 / FIL3 from wfview (06 with filter byte)
- [x] Data mode: USB-D and LSB-D from wfview (1A 06)
- [ ] VFO A / B select (07 00 / 01)
- [ ] A=B button (07 A0): *not in the Xiegu reference*
- [x] A<->B button (07 B0)
- [x] Split on / off (0F 00 / 01)
- [ ] Duplex - / + / simplex (0F 11 / 12 / 10): *not in the Xiegu reference*; hamlib `R -` / `R +` / `R None`
- [-] VFO A / B (25 00 / 01, 26 00 / 01): **the receiver is on the other VFO**: the radio and wfview both show 80 m (3630 kHz), the audio is MW (1179 kHz), and the other way round. Both displays agree, so the receiver itself tunes the VFO not shown
- [ ] TX frequency read (1C 03): *not in the Xiegu reference*; with split on, wfview shows the TX side
- [-] Tuning step from wfview (10): **no effect**; hamlib `N 1000`, `n`
- [-] Tuning step changed on the X6100 shows in wfview: **not seen**

## 2. Memories

- [-] wfview memory window opens and lists the channels (1A 00): **does not work**. Note: wfview's own IC-705.rig has a broken Memory Contents string (`\\x1a\\00`): if the window times out on every channel, it may be that, not the X6100. To check with hamlib: `rigctl ... E` / channel read
- [ ] Memory mode on a channel (08 0001); back to VFO (07)
- [ ] Memory to VFO (0A)
- [ ] Memory write (09) and clear (0B): careful, overwrites a channel

## 3. Levels (14)

For each: the wfview slider moves the X6100 setting (wfview -> X6100), and a change on
the X6100 shows in wfview (X6100 -> wfview).

- [x] AF gain (14 01), wfview -> X6100
- [x] RF gain (14 02), wfview -> X6100
- [-] RF gain, X6100 -> wfview: **not seen**
- [x] Squelch (14 03), wfview -> X6100
- [x] Squelch, X6100 -> wfview: seen, **slowly** (wfview's poll only, no transceive)
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

- [x] S-meter moves with the signal and matches the X6100 (15 02; S9 = 0120)
- [ ] Squelch status (15 01 / 05)
- [ ] OVF (15 07)
- [ ] While transmitting into a dummy load: Po (15 11), SWR (15 12), ALC (15 13)
- [ ] Vd (15 15)

## 5. Switches (16)

- [x] Preamp (16 02), wfview -> X6100
- [-] Preamp, X6100 -> wfview: **not seen**
- [-] AGC (16 12), wfview -> X6100: **shifted**, wfview MID sets SLOW, SLOW sets AUTO, FAST sets FAST
- [-] AGC, X6100 -> wfview: **not seen**
- [x] NB on / off (16 22), wfview -> X6100
- [-] NB, X6100 -> wfview: **not seen**
- [x] NR on / off (16 40), wfview -> X6100
- [-] NR, X6100 -> wfview: **not seen**
- [ ] Notch / DNF (16 41; IC-705 manual notch 16 48)
- [ ] Compressor / monitor / VOX (16 44 / 45 / 46)
- [ ] Break-in (16 47; wfview polls it in CW)
- [ ] Dial lock (16 50)

## 6. Attenuator, ID, CW, misc

- [x] ATT on / off (11), wfview -> X6100
- [-] ATT, X6100 -> wfview: **not seen**
- [ ] Transceiver ID (19 00) answers A4
- [ ] Xiegu radio ID (1D 19)
- [-] Band up / down on the X6100 with wfview connected: **the radio fails**; once the stock UI restarted
- [-] Band buttons in wfview (1A 01, band stacking register): 40 m to 20 m sends the radio to **500 kHz**
- [-] IF filter width (1A 03): **wfview shows one fixed width**, does not follow the radio (to confirm: the passband in wfview, or the scope)
- [ ] CI-V transceive off / on (1A 05 0131)
- [ ] DATA OFF MOD / DATA MOD input (1A 05 0118 / 0119): which values the X6100 takes (MIC, USB, MIC+USB, WLAN)
- [ ] Dash ratio (1A 05 0252)
- [ ] wfview Set Clock (1A 05 0165 / 0166 / 0170)
- [ ] Send CW text from wfview (17), and stop (17 FF)
- [ ] Voice / message memory (28 00 01); hamlib `send_voice_mem 1`
- [ ] Scan (0E 01 / 02 / 00): hamlib `g VFO 0`, `g MEM 0`, `g STOP 0`

## 7. TX, tuner, RIT / XIT

Into a dummy load, low power.

- [x] PTT from wfview keys the X6100 (1C 00 01) and unkeys it (1C 00 00)
- [x] TX state read back (1C 00): wfview shows TX when keyed on the radio
- [x] Tuner tune from wfview (1C 01 02)
- [-] Tuner on state (1C 01 read): **does not stay on in wfview** while the radio shows ATU on
- [x] RIT offset and on / off (21 00 / 01)
- [ ] XIT on / off (21 02)

## 8. TX audio over the LAN (reported broken by many users)

Result 20261120: **no TX audio at all** from wfview (wfview PTT keys the radio; RX audio
over the LAN does not work either). Not debugged further on the stock firmware: this is
how it ships. The steps below are left for a later firmware (Jin's rewrite).

wfview sends the PC microphone (or a digital mode program's audio) to wfserver, which
should play it into the X6100's TX path.

- [ ] wfview Settings, Audio: a TX device chosen, TX codec noted: ______, sample rate: ______
- [ ] wfview's TX level meter moves while talking (the PC side works)
- [x] USB mode, wfview PTT: the X6100 keys (TX shown on the radio); TX audio still none
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

- [x] Spectrum and waterfall show, in wfview and on the radio (27 00, 27 10 / 11)
- [ ] Center / fixed / scroll mode (27 14)
- [ ] Span (27 15)
- [ ] Fixed edges (27 16, 27 1E)
- [ ] Hold (27 17)
- [x] Reference level (27 19)
- [ ] Sweep speed (27 1A)
- [ ] Scope during TX (27 1B)
- [ ] Center type (27 1C), VBW (27 1D)

## 10. Behaviour

- [-] A change on the X6100 shows in wfview without waiting for its poll (transceive): **no** for AGC, step, RF gain, PRE, ATT, NR, NB; SQL only by wfview's poll
- [ ] An unsupported command gets NG (FA), not silence: wfview does not stall on it
- [x] Power on / off from wfview (18 01 / 18 00): works
- [ ] wfview disconnects and connects again without restarting wfserver or the radio
