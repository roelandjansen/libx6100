/*
 * x6100_keys.h - Xiegu X6100 front panel input map
 *
 * Verified on hardware with x6100_input (2026-10-18).
 *
 *   /dev/input/event0  matrix_keypad   EV_KEY, code = X6100_KEY_BASE + index
 *   /dev/input/event1  rotary0         EV_REL REL_X  VFO knob
 *   /dev/input/event2  rotary1         EV_REL REL_Y  VOL/SQL/RFG knob
 *   /dev/input/event3  rotary3         EV_REL REL_Z  MFK knob
 *   /dev/input/event4  axp20x-pek      EV_KEY KEY_POWER  PWR key
 *
 * Event numbers follow probe order; open by device name, not by number.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#define X6100_KEY_BASE		0x2C0	/* BTN_TRIGGER_HAPPY1 */
#define X6100_KEY_COUNT		30
#define X6100_KEY_COLS		6

#define X6100_INPUT_KEYPAD	"matrix_keypad"
#define X6100_INPUT_VFO		"rotary0"
#define X6100_INPUT_VOL		"rotary1"
#define X6100_INPUT_MFK		"rotary3"
#define X6100_INPUT_POWER	"axp20x-pek"

typedef enum
{
	//
	// Top row: function keys
	//
	X6100_KEY_GEN = 0,
	X6100_KEY_KEY = 1,
	X6100_KEY_DFN = 2,
	X6100_KEY_APP = 6,
	X6100_KEY_MSG = 7,
	X6100_KEY_DFL = 8,

	//
	// Soft keys under the display, and lock
	//
	X6100_KEY_F1 = 12,
	X6100_KEY_F2 = 13,
	X6100_KEY_F3 = 14,
	X6100_KEY_F4 = 18,
	X6100_KEY_F5 = 19,
	X6100_KEY_LOCK = 24,

	//
	// PTT and band
	//
	X6100_KEY_PTT = 3,
	X6100_KEY_BAND_DOWN = 4,
	X6100_KEY_BAND_UP = 5,

	//
	// Mode
	//
	X6100_KEY_AMFM = 9,
	X6100_KEY_CW = 10,
	X6100_KEY_SSB = 11,

	//
	// Radio functions
	//
	X6100_KEY_AB = 15,
	X6100_KEY_PRE = 16,
	X6100_KEY_ATU = 17,
	X6100_KEY_VM = 21,
	X6100_KEY_AGC = 22,
	X6100_KEY_FST = 23,

	//
	// Encoder push buttons
	//
	X6100_KEY_VOL_PUSH = 20,
	X6100_KEY_MFK_PUSH = 26
} x6100_key_t;

//
// Encoder direction: multiply the raw REL value to get +1 = clockwise.
// VOL and MFK report clockwise as -1, VFO as +1.
//

#define X6100_DIR_VFO		(+1)
#define X6100_DIR_VOL		(-1)
#define X6100_DIR_MFK		(-1)
