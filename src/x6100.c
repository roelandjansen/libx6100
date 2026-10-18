/*
 * x6100.c - Xiegu X6100 baseband control library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define X6100_I2C_MAX		(2 + X6100_CAL_SIZE)

_Static_assert(sizeof(x6100_flow_frame_t) == 4136, "x6100_flow_frame_t size");

//
// STM32 CRC-32: poly 0x04C11DB7, init 0xFFFFFFFF, 32-bit words MSB first
//

uint32_t x6100_crc32(const void *buf, size_t words)
{
	const uint8_t *p = buf;
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < words; i++) {
		uint32_t w;

		memcpy(&w, p + i * 4, sizeof(w));
		crc ^= w;

		for (int b = 0; b < 32; b++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
	}

	return crc;
}

//
// I2C transport: 16-bit big-endian address, data in one message;
// reads set the pointer in a separate transaction (no repeated start)
//

int x6100_i2c_open(x6100_i2c_t *i2c, const char *dev)
{
	i2c->fd = open(dev != NULL ? dev : X6100_I2C_DEV, O_RDWR | O_CLOEXEC);
	i2c->addr = X6100_I2C_ADDR;

	return i2c->fd < 0 ? -errno : 0;
}

void x6100_i2c_close(x6100_i2c_t *i2c)
{
	if (i2c->fd >= 0)
		close(i2c->fd);

	i2c->fd = -1;
}

static int i2c_xfer(x6100_i2c_t *i2c, uint16_t flags, uint8_t *buf, size_t len)
{
	struct i2c_msg msg = { .addr = i2c->addr, .flags = flags, .len = (uint16_t) len, .buf = buf };
	struct i2c_rdwr_ioctl_data xfer = { .msgs = &msg, .nmsgs = 1 };

	return ioctl(i2c->fd, I2C_RDWR, &xfer) < 0 ? -errno : 0;
}

int x6100_i2c_write(x6100_i2c_t *i2c, uint16_t addr, const void *buf, size_t len)
{
	uint8_t tmp[X6100_I2C_MAX];

	if (len > X6100_I2C_MAX - 2)
		return -EINVAL;

	tmp[0] = addr >> 8;
	tmp[1] = addr & 0xFF;

	if (len > 0)
		memcpy(tmp + 2, buf, len);

	return i2c_xfer(i2c, 0, tmp, len + 2);
}

int x6100_i2c_read(x6100_i2c_t *i2c, uint16_t addr, void *buf, size_t len)
{
	int rc = x6100_i2c_write(i2c, addr, NULL, 0);

	if (rc < 0)
		return rc;

	return i2c_xfer(i2c, I2C_M_RD, buf, len);
}

//
// Status block
//

int x6100_base_version(x6100_i2c_t *i2c, char *buf, size_t len)
{
	char ro[X6100_RORAM_LEN];
	int rc = x6100_i2c_read(i2c, X6100_RORAM_ADDR, ro, sizeof(ro));

	if (rc < 0)
		return rc;

	snprintf(buf, len, "%.*s", (int) strnlen(ro, sizeof(ro)), ro);
	return 0;
}

//
// Register file
//

void x6100_ctl_init(x6100_ctl_t *ctl, x6100_i2c_t *i2c)
{
	memset(ctl, 0, sizeof(*ctl));
	ctl->i2c = i2c;

	ctl->want[X6100_REG_VFOA_BAND] = 1;
	ctl->want[X6100_REG_VFOA_FREQ] = 14074000;
	ctl->want[X6100_REG_VFOA_MODE] = X6100_MODE_USB;
	ctl->want[X6100_REG_VFOA_AGC] = X6100_AGC_AUTO;

	ctl->want[X6100_REG_VFOB_BAND] = 1;
	ctl->want[X6100_REG_VFOB_FREQ] = 14074000;
	ctl->want[X6100_REG_VFOB_MODE] = X6100_MODE_USB;
	ctl->want[X6100_REG_VFOB_AGC] = X6100_AGC_AUTO;

	ctl->want[X6100_REG_VI_BAND_VM] = X6100_R13(0, x6100_band_index(14074000), 0);
	ctl->want[X6100_REG_RFG_TXPWR_FFTDEC] = X6100_R15(100, 50, 0);
	ctl->want[X6100_REG_MIC_PTT_SQL] = X6100_R21(2, 0, 0, 0, 0, 0);
	ctl->want[X6100_REG_AGC] = X6100_R27(-60, 6, 0);
	ctl->want[X6100_REG_AGC_TC] = X6100_R28(250, 50, 5);
	ctl->want[X6100_REG_BIAS] = X6100_R41(X6100_CAL_BIAS_DRIVE_DEF, X6100_CAL_BIAS_FINAL_DEF);
	ctl->want[X6100_REG_FILTER1_LOW] = 50;
	ctl->want[X6100_REG_FILTER1_HIGH] = 2950;
	ctl->want[X6100_REG_FILTER2_LOW] = 50;
	ctl->want[X6100_REG_FILTER2_HIGH] = 2950;
	ctl->want[X6100_REG_PWRSYNC] = 2000000;
	ctl->want[X6100_REG_LAST] = 0x100001;
}

void x6100_ctl_set(x6100_ctl_t *ctl, x6100_reg_t reg, uint32_t val)
{
	if ((unsigned) reg < X6100_REG_COUNT)
		ctl->want[reg] = val;
}

void x6100_ctl_update(x6100_ctl_t *ctl, x6100_reg_t reg, uint32_t mask, uint32_t val)
{
	if ((unsigned) reg < X6100_REG_COUNT)
		ctl->want[reg] = (ctl->want[reg] & ~mask) | (val & mask);
}

uint32_t x6100_ctl_get(const x6100_ctl_t *ctl, x6100_reg_t reg)
{
	return (unsigned) reg < X6100_REG_COUNT ? ctl->want[reg] : 0;
}

int x6100_ctl_sync(x6100_ctl_t *ctl)
{
	for (int n = 0; n < X6100_REG_COUNT; n++) {
		if (ctl->want[n] == ctl->sent[n])
			continue;

		uint32_t val = ctl->want[n];
		int rc = x6100_i2c_write(ctl->i2c, X6100_REG_ADDR(n), &val, sizeof(val));

		if (rc < 0)
			return rc;

		ctl->sent[n] = val;
	}

	return 0;
}

int x6100_ctl_refresh(x6100_ctl_t *ctl)
{
	int rc = x6100_i2c_write(ctl->i2c, X6100_REG_ADDR(0), ctl->want, sizeof(ctl->want));

	if (rc == 0)
		memcpy(ctl->sent, ctl->want, sizeof(ctl->sent));

	return rc;
}

int x6100_ctl_send_cmd(x6100_ctl_t *ctl, const void *buf, size_t len)
{
	return x6100_i2c_write(ctl->i2c, X6100_CMD_ADDR, buf, len);
}

//
// Radio helpers
//

uint8_t x6100_band_index(uint32_t freq)
{
	static const uint32_t edges[][2] = {
		{  1800000,  2000000 }, {  3500000,  4000000 }, {  5330500,  5405000 },
		{  7000000,  7300000 }, { 10100000, 10150000 }, { 14000000, 14350000 },
		{ 18068000, 18168000 }, { 21000000, 21450000 }, { 24890000, 24990000 },
		{ 28000000, 29700000 }, { 50000000, 54000000 },
	};
	uint8_t idx = 0;

	for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		if (freq < edges[i][0])
			return idx;

		if (freq <= edges[i][1])
			return idx + 1;

		idx += 2;
	}

	return idx;
}

void x6100_set_freq(x6100_ctl_t *ctl, int vfo, uint32_t freq)
{
	uint8_t band = x6100_band_index(freq);
	x6100_reg_t base = vfo == 0 ? X6100_REG_VFOA_BAND : X6100_REG_VFOB_BAND;

	x6100_ctl_set(ctl, base + 0, band & 1);
	x6100_ctl_set(ctl, base + 1, freq);

	if (vfo == (int) (ctl->want[X6100_REG_VI_BAND_VM] & 0xFF))
		x6100_ctl_update(ctl, X6100_REG_VI_BAND_VM, 0xFF00, (uint32_t) band << 8);
}

void x6100_set_mode(x6100_ctl_t *ctl, int vfo, x6100_mode_t mode)
{
	x6100_ctl_set(ctl, vfo == 0 ? X6100_REG_VFOA_MODE : X6100_REG_VFOB_MODE, mode);
}

void x6100_set_ptt(x6100_ctl_t *ctl, bool on)
{
	x6100_ctl_update(ctl, X6100_REG_FLAGS, X6100_FLAG_IPTT, on ? X6100_FLAG_IPTT : 0);
}

//
// Calibration
//

int x6100_cal_read(x6100_i2c_t *i2c, x6100_cal_t *cal)
{
	int rc = x6100_i2c_read(i2c, X6100_CAL_ADDR, cal, sizeof(*cal));

	if (rc < 0)
		return rc;

	return x6100_cal_valid(cal) ? 0 : -EBADMSG;
}

bool x6100_cal_valid(const x6100_cal_t *cal)
{
	return cal->magic == X6100_CAL_MAGIC
		&& cal->crc == x6100_crc32(cal, X6100_CAL_CRC_WORDS);
}

void x6100_cal_apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, uint8_t band)
{
	if (band >= X6100_CAL_BANDS)
		return;

	x6100_ctl_set(ctl, X6100_REG_BIAS, X6100_R41(cal->bias_drive, cal->bias_final));
	x6100_ctl_set(ctl, X6100_REG_RMAGCMP, (uint32_t) cal->rx[band].rmagcmp);
	x6100_ctl_set(ctl, X6100_REG_RPHSCMP, (uint32_t) cal->rx[band].rphscmp);
	x6100_ctl_set(ctl, X6100_REG_TXIOFS, (uint32_t) cal->tx[band].txiofs);
	x6100_ctl_set(ctl, X6100_REG_TXQOFS, (uint32_t) cal->tx[band].txqofs);
}

//
// Flow (UART): magic-synchronised frames, CRC checked
//

int x6100_flow_open(x6100_flow_t *flow, const char *dev)
{
	struct termios tio;

	flow->len = 0;
	flow->fd = open(dev != NULL ? dev : X6100_FLOW_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

	if (flow->fd < 0)
		return -errno;

	if (tcgetattr(flow->fd, &tio) < 0)
		goto fail;

	cfmakeraw(&tio);
	cfsetispeed(&tio, B1152000);
	cfsetospeed(&tio, B1152000);

	if (tcsetattr(flow->fd, TCSANOW, &tio) < 0)
		goto fail;

	tcflush(flow->fd, TCIFLUSH);
	return 0;

fail:
	{
		int err = -errno;

		close(flow->fd);
		flow->fd = -1;
		return err;
	}
}

void x6100_flow_close(x6100_flow_t *flow)
{
	if (flow->fd >= 0)
		close(flow->fd);

	flow->fd = -1;
}

int x6100_flow_read(x6100_flow_t *flow, x6100_flow_frame_t *frame)
{
	static const uint32_t magic = X6100_FLOW_MAGIC;
	ssize_t n = read(flow->fd, flow->buf + flow->len, sizeof(flow->buf) - flow->len);

	if (n < 0 && errno != EAGAIN)
		return -errno;

	if (n > 0)
		flow->len += (size_t) n;

	for (;;) {
		uint8_t *start = memmem(flow->buf, flow->len, &magic, sizeof(magic));

		if (start == NULL) {
			size_t keep = flow->len < 3 ? flow->len : 3;

			memmove(flow->buf, flow->buf + flow->len - keep, keep);
			flow->len = keep;
			return 0;
		}

		size_t skip = (size_t) (start - flow->buf);

		if (skip > 0) {
			memmove(flow->buf, start, flow->len - skip);
			flow->len -= skip;
		}

		if (flow->len < sizeof(*frame))
			return 0;

		memcpy(frame, flow->buf, sizeof(*frame));

		if (frame->crc == x6100_crc32(frame, sizeof(*frame) / 4 - 1)) {
			flow->len -= sizeof(*frame);
			memmove(flow->buf, flow->buf + sizeof(*frame), flow->len);
			return 1;
		}

		memmove(flow->buf, flow->buf + 1, flow->len - 1);
		flow->len--;
	}
}
