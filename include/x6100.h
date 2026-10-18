/*
 * x6100.h - Xiegu X6100 baseband control library
 *
 * Clean-room C implementation of the interface between the Allwinner R16 (Linux)
 * and the STM32 baseband, as used by stock APP 1.2.0 / BASE 1.1.9:
 *
 *   control  I2C /dev/i2c-0, slave 0x72, 56 x u32 register file (x6100_regs.h)
 *   status   I2C read 0x0000, 128 bytes (BASE version string)
 *   cal      I2C read 0xF000, 816 bytes (x6100_cal.h)
 *   flow     UART /dev/ttyS1, 1152000 baud, 4136-byte IQ + telemetry frames
 *
 * The library has no threads; the caller drives x6100_ctl_sync() and x6100_flow_read().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x6100_cal.h"
#include "x6100_regs.h"

#define X6100_FLOW_DEV		"/dev/ttyS1"
#define X6100_FLOW_MAGIC	0xAA5555AAu
#define X6100_FLOW_SAMPLES	512

typedef struct
{
	int fd;
	uint8_t addr;
} x6100_i2c_t;

typedef struct
{
	x6100_i2c_t *i2c;

	//
	// Register shadows: wanted state and last state written to the baseband
	//
	uint32_t want[X6100_REG_COUNT];
	uint32_t sent[X6100_REG_COUNT];
} x6100_ctl_t;

typedef struct __attribute__((__packed__))
{
	//
	// Sync and spectrum IQ
	//
	uint32_t magic;
	float iq[X6100_FLOW_SAMPLES][2];

	//
	// Status flags: b0 resync, b1 tx, b2 charging, b3 external power
	//
	uint32_t flags;

	//
	// Telemetry, values x10 unless noted
	//
	uint8_t reserved_1;
	uint8_t tx_power;
	uint8_t vswr;
	uint8_t alc_level;
	uint8_t vext;
	uint8_t vbat;
	uint8_t batcap;			/* percent */
	uint8_t reserved_2;
	uint32_t atu_params;
	uint32_t reserved_3[3];
	uint32_t hkey;			/* hand mic key code */

	//
	// Integrity: STM32 CRC-32 over all preceding words
	//
	uint32_t crc;
} x6100_flow_frame_t;

typedef struct
{
	int fd;
	size_t len;
	uint8_t buf[sizeof(x6100_flow_frame_t) * 3];
} x6100_flow_t;

#define X6100_FLOW_RESYNC	0x1u
#define X6100_FLOW_TX		0x2u
#define X6100_FLOW_CHG		0x4u
#define X6100_FLOW_VEXT		0x8u

//
// CRC
//

uint32_t x6100_crc32(const void *buf, size_t words);

//
// I2C transport
//

int x6100_i2c_open(x6100_i2c_t *i2c, const char *dev);
void x6100_i2c_close(x6100_i2c_t *i2c);
int x6100_i2c_write(x6100_i2c_t *i2c, uint16_t addr, const void *buf, size_t len);
int x6100_i2c_read(x6100_i2c_t *i2c, uint16_t addr, void *buf, size_t len);

//
// Status block
//

int x6100_base_version(x6100_i2c_t *i2c, char *buf, size_t len);

//
// Register file
//

void x6100_ctl_init(x6100_ctl_t *ctl, x6100_i2c_t *i2c);
void x6100_ctl_set(x6100_ctl_t *ctl, x6100_reg_t reg, uint32_t val);
void x6100_ctl_update(x6100_ctl_t *ctl, x6100_reg_t reg, uint32_t mask, uint32_t val);
uint32_t x6100_ctl_get(const x6100_ctl_t *ctl, x6100_reg_t reg);
int x6100_ctl_sync(x6100_ctl_t *ctl);
int x6100_ctl_refresh(x6100_ctl_t *ctl);
int x6100_ctl_send_cmd(x6100_ctl_t *ctl, const void *buf, size_t len);

//
// Radio helpers
//

uint8_t x6100_band_index(uint32_t freq);
void x6100_set_freq(x6100_ctl_t *ctl, int vfo, uint32_t freq);
void x6100_set_mode(x6100_ctl_t *ctl, int vfo, x6100_mode_t mode);
void x6100_set_ptt(x6100_ctl_t *ctl, bool on);

//
// Calibration
//

int x6100_cal_read(x6100_i2c_t *i2c, x6100_cal_t *cal);
bool x6100_cal_valid(const x6100_cal_t *cal);
void x6100_cal_apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, uint8_t band);

//
// Flow (UART)
//

int x6100_flow_open(x6100_flow_t *flow, const char *dev);
void x6100_flow_close(x6100_flow_t *flow);
int x6100_flow_read(x6100_flow_t *flow, x6100_flow_frame_t *frame);
