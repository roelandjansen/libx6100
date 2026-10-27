/*
 * x6100_civ.h - CI-V command engine
 *
 * Transport-independent: takes one CI-V frame, returns the reply frame. Serial
 * CAT and the Icom LAN server (port 50002) both feed it. The radio itself is
 * reached through two callbacks on numbered items, so the engine owns no radio
 * state; the front end (one owner of the baseband) implements them.
 *
 * Command set: docs/x6100_civ.md (Xiegu CI-V reference v1.0, plus the IC-705
 * commands IC-705 clients need). The radio announces itself as an IC-705
 * (address 0xA4) so wfview and IC-705 apps accept it; 1D 19 still answers the
 * Xiegu radio ID 0x6100.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X6100_CIV_ADDR_IC705	0xA4
#define X6100_CIV_ADDR_X6100	0x61	/* own identity: wfview rig file contrib/wfview/X6100.rig */
#define X6100_CIV_ADDR_CTRL	0xE0
#define X6100_CIV_ADDR_ALL	0x00
#define X6100_CIV_FRAME_MAX	128	/* 1A 00 memory contents, IC-705 layout: 121 bytes */

//
// Items the engine reads or writes through the callbacks. Values:
//   FREQ               Hz
//   MODE               x6100_mode_t (data mode is the _DIG mode)
//   FIL                filter group 1-3
//   FIL_WIDTH          passband width of the selected filter in Hz
//   VFO                0 = A, 1 = B (set); VFO_SWAP / VFO_EQUAL: A <-> B / A = B
//   SPLIT, ATT, PRE, NB, NR, DNF, COMP, VOX, MONI, LOCK, RIT, XIT   0 / 1
//   DUPLEX             0 simplex, 1 DUP-, 2 DUP+ (0F 10 / 11 / 12)
//   AGC                CI-V code 1 fast, 2 mid, 3 slow (set 1-3; get may report 0 = off)
//   STEP               Hz
//   RIT_HZ             signed Hz
//   LVL_*, MTR_*       0-255 (meters read only), IC-705 scales: MTR_S 0 = S0,
//                      120 = S9, 241 = S9+60; MTR_PWR 143 = 50 %, 213 = 100 %;
//                      MTR_SWR 0 = 1.0, 48 = 1.5, 80 = 2.0, 120 = 3.0;
//                      MTR_ALC 0-120; MTR_VOLT 75 = 5 V, 241 = 16 V
//   SQL_OPEN           0 / 1 (read only)
//   TX                 0 RX, 1 TX (read); PTT 0 / 1 (write)
//   ATU                0 off, 1 on, 2 start tuning
//   OVF                0 / 1 overload (read only; unsupported reads 0)
//   SCOPE_ON, _OUT     scope on, wave data output on (27 10 / 27 11), 0 / 1
//   SCOPE_MODE         0 CENTER, 1 FIX, 2 SCROLL-C, 3 SCROLL-F (IC-705 order)
//   SCOPE_SPAN         half span in Hz (IC-705: 2500 = +-2.5 kHz)
//   SCOPE_EDGE         fixed edge set 1-4; SCOPE_HOLD 0 / 1
//   SCOPE_REF          reference level in 0.1 dB, -200 .. +200
//   SCOPE_SPEED        0 fast, 1 mid, 2 slow; SCOPE_VBW 0 narrow, 1 wide
//   BAND_FREQ / _MODE / _FIL   band stacking register (1A 01); the vfo argument
//                      carries the IC-705 band code 1-15 (01 = 1.9 .. 10 = 50 MHz)
//   MEM_CH             selected memory channel 1-99 (08 0001-0099 sets it, then MEM_TO_VFO)
//   MEM_WRITE, MEM_TO_VFO, MEM_CLEAR   set only, value = the channel: 09 / 0A / 0B
//   TRANSCEIVE         0 / 1: unasked 00 / 01 frames on a local change (1A 05 0131)
//   SCOPE_FEDGE_LO / _HI   fixed edge frequencies (27 1E), Hz; vfo = (range - 1) * 4 + edge - 1,
//                      range 1-12 (IC-705 codes), edge 1-4; a set sends LO, then HI
//
// The vfo argument: 0 = foreground (selected), 1 = background (unselected).
//

typedef enum
{
	X6100_CIV_FREQ,
	X6100_CIV_MODE,
	X6100_CIV_FIL,
	X6100_CIV_FIL_WIDTH,
	X6100_CIV_VFO,
	X6100_CIV_VFO_SWAP,
	X6100_CIV_VFO_EQUAL,
	X6100_CIV_SPLIT,
	X6100_CIV_DUPLEX,
	X6100_CIV_STEP,
	X6100_CIV_ATT,
	X6100_CIV_PRE,
	X6100_CIV_AGC,
	X6100_CIV_NB,
	X6100_CIV_NR,
	X6100_CIV_DNF,
	X6100_CIV_COMP,
	X6100_CIV_VOX,
	X6100_CIV_MONI,
	X6100_CIV_LOCK,
	X6100_CIV_RIT,
	X6100_CIV_XIT,
	X6100_CIV_RIT_HZ,
	X6100_CIV_LVL_AF,
	X6100_CIV_LVL_RF,
	X6100_CIV_LVL_SQL,
	X6100_CIV_LVL_NR,
	X6100_CIV_LVL_PBT1,
	X6100_CIV_LVL_PBT2,
	X6100_CIV_LVL_CW_PITCH,
	X6100_CIV_LVL_PWR,
	X6100_CIV_LVL_MIC,
	X6100_CIV_LVL_KEY_SPEED,
	X6100_CIV_LVL_DNF,
	X6100_CIV_LVL_COMP,
	X6100_CIV_LVL_QSK,
	X6100_CIV_LVL_NB,
	X6100_CIV_LVL_MONI,
	X6100_CIV_LVL_VOX,
	X6100_CIV_LVL_ANTIVOX,
	X6100_CIV_LVL_BACKLIGHT,
	X6100_CIV_MTR_S,
	X6100_CIV_MTR_PWR,
	X6100_CIV_MTR_SWR,
	X6100_CIV_MTR_ALC,
	X6100_CIV_MTR_VOLT,
	X6100_CIV_SQL_OPEN,
	X6100_CIV_TX,
	X6100_CIV_PTT,
	X6100_CIV_ATU,
	X6100_CIV_OVF,
	X6100_CIV_SCOPE_ON,
	X6100_CIV_SCOPE_OUT,
	X6100_CIV_SCOPE_MODE,
	X6100_CIV_SCOPE_SPAN,
	X6100_CIV_SCOPE_EDGE,
	X6100_CIV_SCOPE_HOLD,
	X6100_CIV_SCOPE_REF,
	X6100_CIV_SCOPE_SPEED,
	X6100_CIV_SCOPE_VBW,
	X6100_CIV_BAND_FREQ,
	X6100_CIV_BAND_MODE,
	X6100_CIV_BAND_FIL,
	X6100_CIV_MEM_CH,
	X6100_CIV_MEM_WRITE,
	X6100_CIV_MEM_TO_VFO,
	X6100_CIV_MEM_CLEAR,
	X6100_CIV_TRANSCEIVE,
	X6100_CIV_SCOPE_FEDGE_LO,
	X6100_CIV_SCOPE_FEDGE_HI,
	X6100_CIV_ITEM_COUNT
} x6100_civ_item_t;

//
// One memory channel (1A 00). The IC-705 layout has an A and a B side: B is the TX
// side of a split channel (tx_*), else the same as A.
//

typedef struct
{
	bool used;
	uint32_t hz;
	int mode;		/* x6100_mode_t; data mode is the _DIG mode */
	int fil;		/* 1-3 */
	bool split;
	uint32_t tx_hz;		/* split: B side, the TX frequency, mode and filter */
	int tx_mode;
	int tx_fil;
	int dup;		/* 0 none, 1 DUP-, 2 DUP+ */
	uint32_t dup_hz;	/* duplex offset */
	char name[17];		/* up to 16 characters */
} x6100_civ_mem_t;

//
// Callbacks. get: false = not supported (reply NG), else *value set. set: false =
// refused or not supported (reply NG). mem_get / mem_set (optional): channel 1-99;
// mem_set with used false clears the channel.
//

typedef struct
{
	void *ctx;
	bool (*get)(void *ctx, x6100_civ_item_t item, int vfo, int32_t *value);
	bool (*set)(void *ctx, x6100_civ_item_t item, int vfo, int32_t value);
	bool (*mem_get)(void *ctx, int ch, x6100_civ_mem_t *m);
	bool (*mem_set)(void *ctx, int ch, const x6100_civ_mem_t *m);
} x6100_civ_ops_t;

typedef struct
{
	uint8_t addr;		/* our address, X6100_CIV_ADDR_IC705 by default */
	uint8_t ctrl;		/* last controller that addressed us (scope data goes there) */
	x6100_civ_ops_t ops;
} x6100_civ_t;

//
// Byte-stream framer (serial CAT): collects FE FE ... FD
//

typedef struct
{
	uint8_t buf[X6100_CIV_FRAME_MAX];
	size_t len;
} x6100_civ_framer_t;

//
// Engine
//

void x6100_civ_init(x6100_civ_t *civ, const x6100_civ_ops_t *ops);
size_t x6100_civ_process(x6100_civ_t *civ, const uint8_t *frame, size_t len, uint8_t *reply, size_t max);

//
// Transceive broadcasts (to address 00) after a local change
//

size_t x6100_civ_announce_freq(const x6100_civ_t *civ, uint32_t hz, uint8_t *out, size_t max);
size_t x6100_civ_announce_mode(const x6100_civ_t *civ, int mode, int fil, uint8_t *out, size_t max);

//
// Framer
//

void x6100_civ_framer_reset(x6100_civ_framer_t *f);
size_t x6100_civ_framer_feed(x6100_civ_framer_t *f, uint8_t byte, uint8_t *frame, size_t max);

//
// Scope waveform (27 00), IC-705 serial format: one information frame, then the
// waveform in 50-point frames (475 points: 11 frames in all). CENTER sends
// centre + half span, FIX, SCROLL-C and SCROLL-F the lower and upper edge (wfview reads
// every mode but CENTER as edges: SCROLL-C froze its waterfall). Points 0-160.
//

#define X6100_CIV_SCOPE_POINTS	475

typedef struct
{
	int mode;		/* 0 CENTER, 1 FIX, 2 SCROLL-C, 3 SCROLL-F */
	uint32_t centre;	/* center modes */
	uint32_t half_span;
	uint32_t lo;		/* fixed modes */
	uint32_t hi;
	bool out_of_range;
	const uint8_t *wave;
	size_t len;
} x6100_civ_scope_t;

size_t x6100_civ_scope_frames(const x6100_civ_t *civ, const x6100_civ_scope_t *s, uint8_t *out, size_t max);
