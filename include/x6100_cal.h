/*
 * x6100_cal.h - Xiegu X6100 baseband calibration block
 *
 * Location: I2C slave 0x72, remote RAM 0xF000, 816 bytes, little-endian.
 * Layout from DialogFactoryMode in stock x6100_ui_v100 (APP 1.2.0 / BASE 1.1.9),
 * verified against a live dump (CRC match).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>

#define X6100_CAL_ADDR		0xF000
#define X6100_CAL_SIZE		816
#define X6100_CAL_MAGIC		0xAA5555AAu
#define X6100_CAL_BANDS		50
#define X6100_CAL_CRC_WORDS	203

#define X6100_CAL_BIAS_DRIVE_DEF	500
#define X6100_CAL_BIAS_FINAL_DEF	650

typedef struct __attribute__((__packed__))
{
	//
	// RX IQ imbalance compensation (registers 49 / 48)
	//
	int32_t rmagcmp;
	int32_t rphscmp;
} x6100_cal_rx_t;

typedef struct __attribute__((__packed__))
{
	//
	// TX IQ DC offset (registers 50 / 51)
	//
	int32_t txiofs;
	int32_t txqofs;
} x6100_cal_tx_t;

typedef struct __attribute__((__packed__))
{
	//
	// Header
	//
	uint32_t magic;			/* X6100_CAL_MAGIC; UI writes defaults if absent */
	int32_t bias_drive;		/* register 41 [15:0] */
	int32_t bias_final;		/* register 41 [31:16] */

	//
	// Per band, indexed by band index of register 13 [15:8]; odd = inside a ham band
	//
	x6100_cal_rx_t rx[X6100_CAL_BANDS];	/* 0x00C */
	x6100_cal_tx_t tx[X6100_CAL_BANDS];	/* 0x19C */

	//
	// Integrity: STM32 CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, 32-bit words,
	// MSB first, no reflection, no final XOR) over words 0..202
	//
	uint32_t crc;			/* 0x32C */
} x6100_cal_t;

_Static_assert(sizeof(x6100_cal_t) == X6100_CAL_SIZE, "x6100_cal_t size");
