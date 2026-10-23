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
#define X6100_CIV_ADDR_CTRL	0xE0
#define X6100_CIV_ADDR_ALL	0x00
#define X6100_CIV_FRAME_MAX	64

//
// Items the engine reads or writes through the callbacks. Values:
//   FREQ               Hz
//   MODE               x6100_mode_t (data mode is the _DIG mode)
//   FIL                filter group 1-3
//   FIL_WIDTH          passband width in Hz (read only)
//   VFO                0 = A, 1 = B (set); VFO_SWAP: any value swaps
//   SPLIT, ATT, PRE, NB, NR, DNF, COMP, VOX, MONI, LOCK, RIT, XIT   0 / 1
//   AGC                CI-V code: 0 off, 1 fast, 2 mid, 3 slow
//   STEP               Hz
//   RIT_HZ             signed Hz
//   LVL_*, MTR_*       0-255 (meters read only; MTR_S on the Icom scale:
//                      0 = S0, 120 = S9, 241 = S9+60)
//   SQL_OPEN           0 / 1 (read only)
//   TX                 0 RX, 1 TX (read); PTT 0 / 1 (write)
//   ATU                0 off, 1 on, 2 start tuning
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
	X6100_CIV_SPLIT,
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
	X6100_CIV_ITEM_COUNT
} x6100_civ_item_t;

//
// Callbacks. get: false = not supported (reply NG), else *value set. set: false =
// refused or not supported (reply NG).
//

typedef struct
{
	void *ctx;
	bool (*get)(void *ctx, x6100_civ_item_t item, int vfo, int32_t *value);
	bool (*set)(void *ctx, x6100_civ_item_t item, int vfo, int32_t value);
} x6100_civ_ops_t;

typedef struct
{
	uint8_t addr;		/* our address, X6100_CIV_ADDR_IC705 by default */
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
