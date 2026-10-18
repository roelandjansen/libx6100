/*
 * x6100_regs.h - Xiegu X6100 baseband register file
 *
 * I2C slave 0x72 on /dev/i2c-0. 56 x uint32, little-endian payload, register n
 * at address n * 4, address sent as 16-bit big-endian. Write-only; reads of the
 * same address range return the status block (see x6100_cal.h / docs).
 *
 * Layout from xradio_update_params() in stock x6100_ui_v100 (APP 1.2.0 / BASE 1.1.9).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>

#define X6100_I2C_DEV		"/dev/i2c-0"
#define X6100_I2C_ADDR		0x72
#define X6100_REG_COUNT		56
#define X6100_REG_ADDR(n)	((uint16_t) ((n) * 4))
#define X6100_CMD_ADDR		0xFFFE
#define X6100_RORAM_ADDR	0x0000
#define X6100_RORAM_LEN		128

#define X6100_FIELD(v, shift, mask)	(((uint32_t) (v) & (mask)) << (shift))

typedef enum
{
	X6100_REG_VFOA_BAND = 0,
	X6100_REG_VFOA_FREQ = 1,
	X6100_REG_VFOA_ATT = 2,
	X6100_REG_VFOA_PRE = 3,
	X6100_REG_VFOA_MODE = 4,
	X6100_REG_VFOA_AGC = 5,

	X6100_REG_VFOB_BAND = 6,
	X6100_REG_VFOB_FREQ = 7,
	X6100_REG_VFOB_ATT = 8,
	X6100_REG_VFOB_PRE = 9,
	X6100_REG_VFOB_MODE = 10,
	X6100_REG_VFOB_AGC = 11,

	X6100_REG_FLAGS = 12,
	X6100_REG_VI_BAND_VM = 13,
	X6100_REG_RXVOL = 14,
	X6100_REG_RFG_TXPWR_FFTDEC = 15,
	X6100_REG_ATU_NETWORK = 17,
	X6100_REG_AUDIO_GAIN = 20,
	X6100_REG_MIC_PTT_SQL = 21,
	X6100_REG_VOX = 22,
	X6100_REG_NR_NB = 23,
	X6100_REG_DNF = 24,
	X6100_REG_CMP = 25,
	X6100_REG_AGC = 27,
	X6100_REG_AGC_TC = 28,
	X6100_REG_MONILEVEL = 29,
	X6100_REG_KEYER = 33,
	X6100_REG_QSK_KR = 34,
	X6100_REG_LO_CENTER = 35,
	X6100_REG_BIAS = 41,
	X6100_REG_RIT = 42,
	X6100_REG_XIT = 43,
	X6100_REG_FILTER1_LOW = 44,
	X6100_REG_FILTER1_HIGH = 45,
	X6100_REG_FILTER2_LOW = 46,
	X6100_REG_FILTER2_HIGH = 47,
	X6100_REG_RPHSCMP = 48,
	X6100_REG_RMAGCMP = 49,
	X6100_REG_TXIOFS = 50,
	X6100_REG_TXQOFS = 51,
	X6100_REG_PWRSYNC = 53,
	X6100_REG_LAST = 55
} x6100_reg_t;

typedef enum
{
	X6100_MODE_LSB = 0,
	X6100_MODE_LSB_DIG = 1,
	X6100_MODE_USB = 2,
	X6100_MODE_USB_DIG = 3,
	X6100_MODE_CW = 4,
	X6100_MODE_CWR = 5,
	X6100_MODE_AM = 6,
	X6100_MODE_NFM = 7
} x6100_mode_t;

typedef enum
{
	X6100_AGC_OFF = 0,
	X6100_AGC_SLOW = 1,
	X6100_AGC_FAST = 2,
	X6100_AGC_AUTO = 3
} x6100_agc_t;

//
// Register 12: flag word
//

#define X6100_FLAG_SPLE		0x00002u
#define X6100_FLAG_VOICE_REC	0x00008u
#define X6100_FLAG_SWRSCAN	0x00010u
#define X6100_FLAG_ATUE		0x01000u
#define X6100_FLAG_ATU_TUNE	0x02000u
#define X6100_FLAG_MODEM	0x04000u
#define X6100_FLAG_CALIBRATION	0x08000u
#define X6100_FLAG_POWER_OFF	0x10000u
#define X6100_FLAG_IPTT		0x40000u

//
// Register 13: [7:0] active VFO, [15:8] band index, [23:16] VFO/memory mode
//

#define X6100_R13(vi, band, vm)	(X6100_FIELD(vi, 0, 0xFF) | X6100_FIELD(band, 8, 0xFF) | X6100_FIELD(vm, 16, 0xFF))

//
// Register 15: [7:0] RF gain 0-100, [15:8] TX power x10 W, [23:16] FFT decimation
//

#define X6100_R15(rfg, txpwr10, fftdec)	(X6100_FIELD(rfg, 0, 0xFF) | X6100_FIELD(txpwr10, 8, 0xFF) | X6100_FIELD(fftdec, 16, 0xFF))

//
// Register 20: line in/out, internal and hand mic gain
//

#define X6100_R20(ling, loutg, imicg, hmicg)	(X6100_FIELD(ling, 0, 0xFF) | X6100_FIELD(loutg, 8, 0xFF) | X6100_FIELD(imicg, 16, 0xFF) | X6100_FIELD(hmicg, 24, 0xFF))

//
// Register 21: [1:0] mic select, [3:2] PTT mode, [4] charger, [5] speaker/phone,
// [6] aux IQ out enable, [15:8] squelch
//

#define X6100_R21(micsel, pttmode, chge, spmode, auxiqen, sqlthr)	(X6100_FIELD(micsel, 0, 0x3) | X6100_FIELD(pttmode, 2, 0x3) | X6100_FIELD(chge, 4, 0x1) | X6100_FIELD(spmode, 5, 0x1) | X6100_FIELD(auxiqen, 6, 0x1) | X6100_FIELD(sqlthr, 8, 0xFF))

//
// Register 22: [6:0] VOX gain, [13:7] anti-VOX, [25:14] delay ms, [26] enable
//

#define X6100_R22(voxg, voxag, voxdly, voxe)	(X6100_FIELD(voxg, 0, 0x7F) | X6100_FIELD(voxag, 7, 0x7F) | X6100_FIELD(voxdly, 14, 0xFFF) | X6100_FIELD(voxe, 26, 0x1))

//
// Register 23: [7:0] NR depth, [15:8] NB width, [23:16] NB level, [24] NR on, [25] NB on
//

#define X6100_R23(nrthr, nbw, nbthr, nre, nbe)	(X6100_FIELD(nrthr, 0, 0xFF) | X6100_FIELD(nbw, 8, 0xFF) | X6100_FIELD(nbthr, 16, 0xFF) | X6100_FIELD(nre, 24, 0x1) | X6100_FIELD(nbe, 25, 0x1))

//
// Register 24: [11:0] DNF centre Hz, [23:12] width Hz, [24] enable
//

#define X6100_R24(dnfcnt, dnfwidth, dnfe)	(X6100_FIELD(dnfcnt, 0, 0xFFF) | X6100_FIELD(dnfwidth, 12, 0xFFF) | X6100_FIELD(dnfe, 24, 0x1))

//
// Register 25: [3:0] compressor level (0 off, 1:2, 1:4, 1:8), [4] enable
//

#define X6100_R25(cmplevel, cmpe)	(X6100_FIELD(cmplevel, 0, 0xF) | X6100_FIELD(cmpe, 4, 0x1))

//
// AGC: register 27 knee/slope/hang, register 28 time constants in units of 10 ms (new in 1.2.0)
//

#define X6100_R27(knee, slope, hang)	(X6100_FIELD(knee, 0, 0xFF) | X6100_FIELD(slope, 8, 0xF) | X6100_FIELD(hang, 12, 0x1))
#define X6100_R28(tcslow, tcmid, tcfast)	(X6100_FIELD(tcslow, 0, 0x3FF) | X6100_FIELD(tcmid, 10, 0x3FF) | X6100_FIELD(tcfast, 20, 0x3FF))

//
// Keyer: register 33 speed/mode/iambic/tone/volume/trainer, register 34 QSK and ratio x10
//

#define X6100_R33(ks, km, kimb, cwtone, cwvol, cwtrain)	(X6100_FIELD(ks, 0, 0xFF) | X6100_FIELD(km, 8, 0x3) | X6100_FIELD(kimb, 10, 0x3) | X6100_FIELD(cwtone, 12, 0x7FF) | X6100_FIELD(cwvol, 23, 0x3F) | X6100_FIELD(cwtrain, 29, 0x1))
#define X6100_R34(qsktime, kr10)	(X6100_FIELD(qsktime, 0, 0xFFFF) | X6100_FIELD(kr10, 16, 0xFFFF))

//
// Register 35: [23:0] LO offset (spectrum cursor), [24] center mode (new in 1.2.0)
//

#define X6100_R35(lo, center)	(X6100_FIELD(lo, 0, 0xFFFFFF) | X6100_FIELD(center, 24, 0x1))

//
// Register 41: [15:0] driver bias, [31:16] final bias
//

#define X6100_R41(drive, final)	(X6100_FIELD(drive, 0, 0xFFFF) | X6100_FIELD(final, 16, 0xFFFF))
