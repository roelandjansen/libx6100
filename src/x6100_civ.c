/*
 * x6100_civ.c - CI-V command engine (see x6100_civ.h, docs/x6100_civ.md)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100_civ.h"

#include <string.h>

#include "x6100_regs.h"

#define CIV_PRE		0xFE
#define CIV_END		0xFD
#define CIV_OK		0xFB
#define CIV_NG		0xFA

//
// Reply builder
//

typedef struct
{
	uint8_t *buf;
	size_t max;
	size_t len;
	bool overflow;
} out_t;

static void out_byte(out_t *o, uint8_t b)
{
	if (o->len < o->max)
		o->buf[o->len++] = b;
	else
		o->overflow = true;
}

static void out_head(out_t *o, uint8_t to, uint8_t from)
{
	o->len = 0;
	o->overflow = false;
	out_byte(o, CIV_PRE);
	out_byte(o, CIV_PRE);
	out_byte(o, to);
	out_byte(o, from);
}

static size_t out_end(out_t *o)
{
	out_byte(o, CIV_END);

	return o->overflow ? 0 : o->len;
}

//
// BCD helpers
//

static uint8_t bcd_byte(unsigned v)
{
	return (uint8_t) (((v / 10) % 10) << 4 | (v % 10));
}

static int bcd_val(uint8_t b)
{
	int hi = b >> 4, lo = b & 0x0F;

	return hi > 9 || lo > 9 ? -1 : hi * 10 + lo;
}

// Frequency: 5 bytes, least significant pair first (Table 2-1)
static void out_freq(out_t *o, uint32_t hz)
{
	uint64_t v = hz;

	for (int i = 0; i < 5; i++) {
		out_byte(o, bcd_byte((unsigned) (v % 100)));
		v /= 100;
	}
}

static bool get_freq_bcd(const uint8_t *d, uint32_t *hz)
{
	uint64_t v = 0, mul = 1;

	for (int i = 0; i < 5; i++) {
		int b = bcd_val(d[i]);

		if (b < 0)
			return false;

		v += (uint64_t) b * mul;
		mul *= 100;
	}

	if (v > UINT32_MAX)
		return false;

	*hz = (uint32_t) v;

	return true;
}

// Levels and meters: 2 bytes, 0000-0255, most significant first (Icom)
static void out_level(out_t *o, int v)
{
	v = v < 0 ? 0 : v > 255 ? 255 : v;
	out_byte(o, bcd_byte((unsigned) v / 100));
	out_byte(o, bcd_byte((unsigned) v % 100));
}

static int get_level_bcd(const uint8_t *d)
{
	int hi = bcd_val(d[0]), lo = bcd_val(d[1]);

	if (hi < 0 || lo < 0)
		return -1;

	int v = hi * 100 + lo;

	return v > 255 ? 255 : v;
}

//
// Modes: CI-V mode byte + data flag <-> x6100_mode_t
//

static bool mode_to_civ(int mode, uint8_t *civ, bool *data)
{
	*data = mode == X6100_MODE_LSB_DIG || mode == X6100_MODE_USB_DIG;

	switch (mode) {
	case X6100_MODE_LSB:
	case X6100_MODE_LSB_DIG:
		*civ = 0x00;
		return true;
	case X6100_MODE_USB:
	case X6100_MODE_USB_DIG:
		*civ = 0x01;
		return true;
	case X6100_MODE_AM:
		*civ = 0x02;
		return true;
	case X6100_MODE_CW:
		*civ = 0x03;
		return true;
	case X6100_MODE_NFM:
		*civ = 0x05;
		return true;
	case X6100_MODE_CWR:
		*civ = 0x07;
		return true;
	default:
		return false;
	}
}

static int mode_from_civ(uint8_t civ, bool data)
{
	switch (civ) {
	case 0x00:
		return data ? X6100_MODE_LSB_DIG : X6100_MODE_LSB;
	case 0x01:
		return data ? X6100_MODE_USB_DIG : X6100_MODE_USB;
	case 0x02:
		return X6100_MODE_AM;
	case 0x03:
		return X6100_MODE_CW;
	case 0x05:
		return X6100_MODE_NFM;
	case 0x07:
		return X6100_MODE_CWR;
	default:
		return -1;
	}
}

//
// IF filter width index (Table 2-5)
//

static uint8_t width_index(int mode, int32_t hz)
{
	int i;

	if (mode == X6100_MODE_AM || mode == X6100_MODE_NFM) {
		i = (hz - 200 + 100) / 200;
		i = i < 0 ? 0 : i > 49 ? 49 : i;
	} else if (hz <= 500) {
		i = (hz - 50 + 25) / 50;
		i = i < 0 ? 0 : i;
	} else {
		i = 10 + (hz - 600 + 50) / 100;
		i = i < 10 ? 10 : i > 40 ? 40 : i;
	}

	return (uint8_t) i;
}

//
// Tuning step codes (command 10, IC-705): 00 = OFF (10 Hz or 1 Hz)
//

static const int32_t g_steps[] = { 10, 100, 500, 1000, 5000, 6250, 8330, 9000, 10000, 12500, 20000, 25000,
	50000, 100000 };

//
// Tables: level (14 xx) and switch (16 xx) subcommands
//

typedef struct
{
	uint8_t sub;
	x6100_civ_item_t item;
} sub_map_t;

static const sub_map_t g_levels[] = {
	{ 0x01, X6100_CIV_LVL_AF },
	{ 0x02, X6100_CIV_LVL_RF },
	{ 0x03, X6100_CIV_LVL_SQL },
	{ 0x06, X6100_CIV_LVL_NR },
	{ 0x07, X6100_CIV_LVL_PBT1 },
	{ 0x08, X6100_CIV_LVL_PBT2 },
	{ 0x09, X6100_CIV_LVL_CW_PITCH },
	{ 0x0A, X6100_CIV_LVL_PWR },
	{ 0x0B, X6100_CIV_LVL_MIC },
	{ 0x0C, X6100_CIV_LVL_KEY_SPEED },
	{ 0x0D, X6100_CIV_LVL_DNF },
	{ 0x0E, X6100_CIV_LVL_COMP },
	{ 0x0F, X6100_CIV_LVL_QSK },
	{ 0x12, X6100_CIV_LVL_NB },
	{ 0x15, X6100_CIV_LVL_MONI },
	{ 0x16, X6100_CIV_LVL_VOX },
	{ 0x17, X6100_CIV_LVL_ANTIVOX },
	{ 0x19, X6100_CIV_LVL_BACKLIGHT },
};

static const sub_map_t g_meters[] = {
	{ 0x02, X6100_CIV_MTR_S },
	{ 0x11, X6100_CIV_MTR_PWR },
	{ 0x12, X6100_CIV_MTR_SWR },
	{ 0x13, X6100_CIV_MTR_ALC },
	{ 0x15, X6100_CIV_MTR_VOLT },
};

static const sub_map_t g_switches[] = {
	{ 0x02, X6100_CIV_PRE },
	{ 0x12, X6100_CIV_AGC },
	{ 0x22, X6100_CIV_NB },
	{ 0x40, X6100_CIV_NR },
	{ 0x41, X6100_CIV_DNF },	/* Xiegu: DNF; IC-705: auto notch */
	{ 0x48, X6100_CIV_DNF },	/* IC-705: manual notch */
	{ 0x44, X6100_CIV_COMP },
	{ 0x45, X6100_CIV_MONI },
	{ 0x46, X6100_CIV_VOX },
	{ 0x50, X6100_CIV_LOCK },
};

static bool sub_lookup(const sub_map_t *map, size_t n, uint8_t sub, x6100_civ_item_t *item)
{
	for (size_t i = 0; i < n; i++) {
		if (map[i].sub == sub) {
			*item = map[i].item;
			return true;
		}
	}

	return false;
}

#define COUNT(a)	(sizeof(a) / sizeof((a)[0]))

//
// Request context
//

typedef struct
{
	x6100_civ_t *civ;
	out_t *o;
	uint8_t to;		/* reply destination (the requester) */
	uint8_t cmd;
	const uint8_t *d;	/* bytes after cmd, before FD */
	size_t n;
} req_t;

static bool get(req_t *r, x6100_civ_item_t item, int vfo, int32_t *v)
{
	return r->civ->ops.get && r->civ->ops.get(r->civ->ops.ctx, item, vfo, v);
}

static bool set(req_t *r, x6100_civ_item_t item, int vfo, int32_t v)
{
	return r->civ->ops.set && r->civ->ops.set(r->civ->ops.ctx, item, vfo, v);
}

static void reply_head(req_t *r)
{
	out_head(r->o, r->to, r->civ->addr);
}

static size_t reply_code(req_t *r, bool ok)
{
	reply_head(r);
	out_byte(r->o, ok ? CIV_OK : CIV_NG);

	return out_end(r->o);
}

static size_t reply_ng(req_t *r)
{
	return reply_code(r, false);
}

static size_t reply_ok(req_t *r, bool ok)
{
	return reply_code(r, ok);
}

//
// Read mode of a VFO as CI-V mode, data flag, filter group
//

static bool get_mode(req_t *r, int vfo, uint8_t *civ, bool *data, uint8_t *fil)
{
	int32_t m, f = 1;

	if (!get(r, X6100_CIV_MODE, vfo, &m) || !mode_to_civ(m, civ, data))
		return false;

	if (!get(r, X6100_CIV_FIL, vfo, &f) || f < 1 || f > 3)
		f = 1;

	*fil = (uint8_t) f;

	return true;
}

// Set mode keeping the data flag unless given; filter 0 = keep
static bool set_mode(req_t *r, int vfo, uint8_t civ, int data, uint8_t fil)
{
	int32_t cur;
	bool cur_data = false;

	if (data < 0) {
		uint8_t c;

		if (get(r, X6100_CIV_MODE, vfo, &cur))
			mode_to_civ(cur, &c, &cur_data);
	}

	int mode = mode_from_civ(civ, data < 0 ? cur_data : data != 0);

	if (mode < 0 || !set(r, X6100_CIV_MODE, vfo, mode))
		return false;

	if (fil >= 1 && fil <= 3)
		return set(r, X6100_CIV_FIL, vfo, fil);

	return true;
}

//
// Command handlers; return reply length (0 = no reply)
//

static size_t cmd_freq(req_t *r)
{
	int32_t hz;

	switch (r->cmd) {
	case 0x00:	/* transceive-style set, no reply */
	case 0x05: {
		uint32_t f;

		if (r->n < 5 || !get_freq_bcd(r->d, &f))
			return r->cmd == 0x00 ? 0 : reply_ng(r);

		bool ok = set(r, X6100_CIV_FREQ, 0, (int32_t) f);

		return r->cmd == 0x00 ? 0 : reply_ok(r, ok);
	}
	case 0x03:
		if (!get(r, X6100_CIV_FREQ, 0, &hz))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x03);
		out_freq(r->o, (uint32_t) hz);

		return out_end(r->o);
	default:
		return reply_ng(r);
	}
}

static size_t cmd_mode(req_t *r)
{
	uint8_t civ, fil;
	bool data;

	switch (r->cmd) {
	case 0x01:
	case 0x06: {
		if (r->n < 1)
			return r->cmd == 0x01 ? 0 : reply_ng(r);

		// IC-705: filter may be skipped, then FIL1 (01) / the mode's default
		// filter (06) is selected; we use FIL1 for both. Data mode goes off.
		bool ok = set_mode(r, 0, r->d[0], 0, r->n >= 2 ? r->d[1] : 1);

		return r->cmd == 0x01 ? 0 : reply_ok(r, ok);
	}
	case 0x04:
		if (!get_mode(r, 0, &civ, &data, &fil))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x04);
		out_byte(r->o, civ);
		out_byte(r->o, fil);

		return out_end(r->o);
	default:
		return reply_ng(r);
	}
}

static size_t cmd_vfo(req_t *r)
{
	if (r->n == 0)
		return reply_ok(r, true);	/* "VFO mode": we are always in VFO mode */

	switch (r->d[0]) {
	case 0x00:
	case 0x01:
		return reply_ok(r, set(r, X6100_CIV_VFO, 0, r->d[0]));
	case 0xA0:
		return reply_ok(r, set(r, X6100_CIV_VFO_EQUAL, 0, 1));
	case 0xB0:
		return reply_ok(r, set(r, X6100_CIV_VFO_SWAP, 0, 1));
	default:
		return reply_ng(r);
	}
}

// 0F: 00 / 01 split off / on, 10 / 11 / 12 simplex / DUP- / DUP+. A read gives
// 11 / 12 while a duplex shift is in use, else the split state.
static size_t cmd_split(req_t *r)
{
	int32_t v, dup = 0;

	if (r->n >= 1) {
		if (r->d[0] <= 0x01)
			return reply_ok(r, set(r, X6100_CIV_SPLIT, 0, r->d[0]));

		return reply_ok(r, r->d[0] >= 0x10 && r->d[0] <= 0x12 && set(r, X6100_CIV_DUPLEX, 0, r->d[0] - 0x10));
	}

	if (!get(r, X6100_CIV_SPLIT, 0, &v))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, r->cmd);

	if (!v && get(r, X6100_CIV_DUPLEX, 0, &dup) && (dup == 1 || dup == 2))
		out_byte(r->o, (uint8_t) (0x10 + dup));
	else
		out_byte(r->o, v ? 0x01 : 0x00);

	return out_end(r->o);
}

static size_t cmd_step(req_t *r)
{
	int32_t v;

	if (r->n >= 1) {
		int code = bcd_val(r->d[0]);	/* codes are BCD 00-13 */

		return reply_ok(r, code >= 0 && (size_t) code < COUNT(g_steps)
			&& set(r, X6100_CIV_STEP, 0, g_steps[code]));
	}

	if (!get(r, X6100_CIV_STEP, 0, &v))
		return reply_ng(r);

	if (v < 10)
		v = 10;		/* 00 = OFF covers 10 Hz and 1 Hz */

	for (size_t i = 0; i < COUNT(g_steps); i++) {
		if (g_steps[i] == v) {
			reply_head(r);
			out_byte(r->o, 0x10);
			out_byte(r->o, bcd_byte((unsigned) i));

			return out_end(r->o);
		}
	}

	return reply_ng(r);
}

// 11: ATT. Set: 00 off, any other value on (IC-705 sends 20). Get: 00 / 20.
static size_t cmd_att(req_t *r)
{
	int32_t v;

	if (r->n >= 1)
		return reply_ok(r, set(r, X6100_CIV_ATT, 0, r->d[0] != 0));

	if (!get(r, X6100_CIV_ATT, 0, &v))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x11);
	out_byte(r->o, v ? 0x20 : 0x00);

	return out_end(r->o);
}

static size_t cmd_level(req_t *r)
{
	x6100_civ_item_t item;
	int32_t v;

	if (r->n < 1 || !sub_lookup(g_levels, COUNT(g_levels), r->d[0], &item))
		return reply_ng(r);

	if (r->n >= 3) {
		int lv = get_level_bcd(r->d + 1);

		return reply_ok(r, lv >= 0 && set(r, item, 0, lv));
	}

	if (!get(r, item, 0, &v))
		v = r->d[0] == 0x07 || r->d[0] == 0x08 ? 128 : 0;	/* unsupported: 0000, PBT centre 0128 */

	reply_head(r);
	out_byte(r->o, 0x14);
	out_byte(r->o, r->d[0]);
	out_level(r->o, v);

	return out_end(r->o);
}

static size_t cmd_meter(req_t *r)
{
	x6100_civ_item_t item;
	int32_t v;

	if (r->n < 1)
		return reply_ng(r);

	// 01 noise / S-meter squelch, 05 various squelch: both the squelch gate here;
	// 07 OVF: from the OVF item, 00 when not supported
	if (r->d[0] == 0x01 || r->d[0] == 0x05 || r->d[0] == 0x07) {
		if (r->d[0] == 0x07) {
			if (!get(r, X6100_CIV_OVF, 0, &v))
				v = 0;
		} else if (!get(r, X6100_CIV_SQL_OPEN, 0, &v)) {
			return reply_ng(r);
		}

		reply_head(r);
		out_byte(r->o, 0x15);
		out_byte(r->o, r->d[0]);
		out_byte(r->o, v ? 0x01 : 0x00);

		return out_end(r->o);
	}

	if (!sub_lookup(g_meters, COUNT(g_meters), r->d[0], &item) || !get(r, item, 0, &v))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x15);
	out_byte(r->o, r->d[0]);
	out_level(r->o, v);

	return out_end(r->o);
}

static size_t cmd_switch(req_t *r)
{
	x6100_civ_item_t item;
	int32_t v;

	if (r->n < 1 || !sub_lookup(g_switches, COUNT(g_switches), r->d[0], &item))
		return reply_ng(r);

	if (r->n >= 2) {
		uint8_t val = r->d[1];

		if (item == X6100_CIV_PRE)
			val = val != 0;		/* 01 or 02 = on */
		else if (item == X6100_CIV_AGC ? val < 1 || val > 3 : val > 1)	/* AGC 01-03 */
			return reply_ng(r);

		return reply_ok(r, set(r, item, 0, val));
	}

	if (!get(r, item, 0, &v))
		v = 0;	/* functions we do not have read OFF (IC-705 clients expect a value) */

	reply_head(r);
	out_byte(r->o, 0x16);
	out_byte(r->o, r->d[0]);
	out_byte(r->o, (uint8_t) v);

	return out_end(r->o);
}

static size_t cmd_id(req_t *r)
{
	if (r->n < 1 || r->d[0] != 0x00)
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x19);
	out_byte(r->o, 0x00);
	out_byte(r->o, r->civ->addr);

	return out_end(r->o);
}

// 1D 19: Xiegu radio ID (Table 2-7)
static size_t cmd_xiegu_id(req_t *r)
{
	if (r->n < 1 || r->d[0] != 0x19)
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x1D);
	out_byte(r->o, 0x19);
	out_byte(r->o, 0x61);
	out_byte(r->o, 0x00);

	return out_end(r->o);
}

// 18: power. 18 01 (on) is accepted, we are on; 18 00 (off) is refused
static size_t cmd_power(req_t *r)
{
	return reply_ok(r, r->n >= 1 && r->d[0] == 0x01);
}

//
// 1A 05 menu items IC-705 clients read at connect; fixed values, a set to the
// same value is accepted
//

typedef struct
{
	uint16_t item;
	uint8_t value;
} menu_t;

static const menu_t g_menu[] = {
	{ 0x0045, 0x00 },	/* Quick SPLIT: OFF */
	{ 0x0118, 0x00 },	/* DATA OFF MOD: MIC */
	{ 0x0119, 0x01 },	/* DATA MOD: USB (rear USB sound card) */
};

static size_t cmd_menu(req_t *r)
{
	if (r->n < 3)
		return reply_ng(r);

	uint16_t item = (uint16_t) (r->d[1] << 8 | r->d[2]);

	for (size_t i = 0; i < COUNT(g_menu); i++) {
		if (g_menu[i].item != item)
			continue;

		if (r->n >= 4)
			return reply_ok(r, r->d[3] == g_menu[i].value);

		reply_head(r);
		out_byte(r->o, 0x1A);
		out_byte(r->o, 0x05);
		out_byte(r->o, r->d[1]);
		out_byte(r->o, r->d[2]);
		out_byte(r->o, g_menu[i].value);

		return out_end(r->o);
	}

	return reply_ng(r);
}

// IF filter width index -> Hz (Table 1A 03); -1 = invalid for the mode
static int32_t width_hz(int mode, int i)
{
	if (mode == X6100_MODE_AM)
		return i >= 0 && i <= 49 ? 200 + i * 200 : -1;

	if (mode == X6100_MODE_NFM)
		return -1;

	if (i >= 0 && i <= 9)
		return 50 + i * 50;

	return i >= 10 && i <= 40 ? 600 + (i - 10) * 100 : -1;
}

static size_t cmd_misc(req_t *r)
{
	int32_t v, m;
	uint8_t civ, fil;
	bool data;

	if (r->n < 1)
		return reply_ng(r);

	switch (r->d[0]) {
	case 0x03:	/* IF filter width of the selected filter, BCD index */
		if (!get(r, X6100_CIV_MODE, 0, &m) || m == X6100_MODE_NFM)
			return reply_ng(r);

		if (r->n >= 2) {
			int i = bcd_val(r->d[1]);
			int32_t hz = i < 0 ? -1 : width_hz(m, i);

			return reply_ok(r, hz > 0 && set(r, X6100_CIV_FIL_WIDTH, 0, hz));
		}

		if (!get(r, X6100_CIV_FIL_WIDTH, 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1A);
		out_byte(r->o, 0x03);
		out_byte(r->o, bcd_byte(width_index(m, v)));

		return out_end(r->o);
	case 0x01: {	/* band stacking register: band code, register code (BCD) */
		int band = r->n >= 3 ? bcd_val(r->d[1]) : -1, reg = r->n >= 3 ? bcd_val(r->d[2]) : -1;
		int32_t hz, mode, f = 1;

		if (band < 1 || band > 15 || reg < 1 || reg > 3)
			return reply_ng(r);

		// Set: content from byte 4 on (frequency, mode, filter, data, ...)
		if (r->n >= 3 + 8) {
			uint32_t fhz;
			const uint8_t *c = r->d + 3;
			int m = mode_from_civ(c[5], c[7] != 0);

			if (!get_freq_bcd(c, &fhz) || m < 0)
				return reply_ng(r);

			bool ok = set(r, X6100_CIV_BAND_FREQ, band, (int32_t) fhz)
				&& set(r, X6100_CIV_BAND_MODE, band, m);

			if (ok && c[6] >= 1 && c[6] <= 3)
				set(r, X6100_CIV_BAND_FIL, band, c[6]);

			return reply_ok(r, ok);
		}

		if (!get(r, X6100_CIV_BAND_FREQ, band, &hz) || !get(r, X6100_CIV_BAND_MODE, band, &mode))
			return reply_ng(r);

		get(r, X6100_CIV_BAND_FIL, band, &f);

		uint8_t civ_mode;
		bool data;

		if (!mode_to_civ(mode, &civ_mode, &data))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1A);
		out_byte(r->o, 0x01);
		out_byte(r->o, r->d[1]);
		out_byte(r->o, r->d[2]);
		out_freq(r->o, (uint32_t) hz);			/* 6-10 frequency */
		out_byte(r->o, civ_mode);			/* 11 mode */
		out_byte(r->o, (uint8_t) (f >= 1 && f <= 3 ? f : 1));	/* 12 filter */
		out_byte(r->o, data ? 0x01 : 0x00);		/* 13 data mode */
		out_byte(r->o, 0x00);				/* 14 duplex, tone off */
		out_byte(r->o, 0x00);				/* 15 digital squelch off */

		// 16-21 repeater / TSQL tone 88.5 Hz, 22-24 DTCS 023, 25 DV CSQL,
		// 26-28 duplex offset, 29-52 UR / R1 / R2 call signs (blank)
		static const uint8_t rest[] = {
			0x00, 0x08, 0x85, 0x00, 0x08, 0x85, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00,
		};

		for (size_t i = 0; i < sizeof(rest); i++)
			out_byte(r->o, rest[i]);

		for (int i = 0; i < 24; i++)
			out_byte(r->o, ' ');

		return out_end(r->o);
	}
	case 0x05:
		return cmd_menu(r);
	case 0x06:	/* data mode + filter group */
		if (r->n >= 3) {
			if (!get_mode(r, 0, &civ, &data, &fil))
				return reply_ng(r);

			if (r->d[1] != 0 && civ != 0x00 && civ != 0x01)
				return reply_ng(r);	/* data only on LSB / USB */

			// Data off is sent with filter 00: keep the filter
			return reply_ok(r, set_mode(r, 0, civ, r->d[1] != 0, r->d[2]));
		}

		if (!get_mode(r, 0, &civ, &data, &fil))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1A);
		out_byte(r->o, 0x06);
		out_byte(r->o, data ? 0x01 : 0x00);
		out_byte(r->o, data ? fil : 0x00);

		return out_end(r->o);
	default:
		// 00 memory, 02 keyer, 04 AGC time constant,
		// 05 menu settings: not supported
		return reply_ng(r);
	}
}

static size_t cmd_tx(req_t *r)
{
	int32_t v;

	if (r->n == 0 || (r->n == 1 && r->d[0] == 0x00)) {
		if (!get(r, X6100_CIV_TX, 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1C);
		out_byte(r->o, 0x00);
		out_byte(r->o, v ? 0x01 : 0x00);

		return out_end(r->o);
	}

	switch (r->d[0]) {
	case 0x00:
		return reply_ok(r, r->d[1] <= 1 && set(r, X6100_CIV_PTT, 0, r->d[1]));
	case 0x01:
		if (r->n >= 2)
			return reply_ok(r, r->d[1] <= 2 && set(r, X6100_CIV_ATU, 0, r->d[1]));

		if (!get(r, X6100_CIV_ATU, 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1C);
		out_byte(r->o, 0x01);
		out_byte(r->o, (uint8_t) v);

		return out_end(r->o);
	case 0x03: {	/* read transmit frequency: unselected VFO when split */
		int32_t split = 0;

		get(r, X6100_CIV_SPLIT, 0, &split);

		if (!get(r, X6100_CIV_FREQ, split ? 1 : 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x1C);
		out_byte(r->o, 0x03);
		out_freq(r->o, (uint32_t) v);

		return out_end(r->o);
	}
	default:
		return reply_ng(r);
	}
}

// 21: RIT / XIT (Table 2-6)
static size_t cmd_rit(req_t *r)
{
	int32_t v;

	if (r->n < 1)
		return reply_ng(r);

	if (r->d[0] == 0x00) {
		if (r->n >= 4) {
			int lo = bcd_val(r->d[1]), hi = bcd_val(r->d[2]);

			if (lo < 0 || hi < 0)
				return reply_ng(r);

			v = hi * 100 + lo;

			return reply_ok(r, set(r, X6100_CIV_RIT_HZ, 0, r->d[3] ? -v : v));
		}

		if (!get(r, X6100_CIV_RIT_HZ, 0, &v))
			return reply_ng(r);

		int a = v < 0 ? -v : v;

		a = a > 9999 ? 9999 : a;
		reply_head(r);
		out_byte(r->o, 0x21);
		out_byte(r->o, 0x00);
		out_byte(r->o, bcd_byte((unsigned) a % 100));
		out_byte(r->o, bcd_byte((unsigned) a / 100));
		out_byte(r->o, v < 0 ? 0x01 : 0x00);

		return out_end(r->o);
	}

	if (r->d[0] != 0x01 && r->d[0] != 0x02)
		return reply_ng(r);

	x6100_civ_item_t item = r->d[0] == 0x01 ? X6100_CIV_RIT : X6100_CIV_XIT;

	if (r->n >= 2)
		return reply_ok(r, r->d[1] <= 1 && set(r, item, 0, r->d[1]));

	if (!get(r, item, 0, &v))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x21);
	out_byte(r->o, r->d[0]);
	out_byte(r->o, v ? 0x01 : 0x00);

	return out_end(r->o);
}

// 25: selected / unselected VFO frequency
static size_t cmd_vfo_freq(req_t *r)
{
	int vfo = r->n >= 1 && r->d[0] != 0 ? 1 : 0;
	int32_t hz;

	if (r->n >= 6) {
		uint32_t f;

		if (!get_freq_bcd(r->d + 1, &f))
			return reply_ng(r);

		return reply_ok(r, set(r, X6100_CIV_FREQ, vfo, (int32_t) f));
	}

	if (!get(r, X6100_CIV_FREQ, vfo, &hz))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x25);
	out_byte(r->o, (uint8_t) vfo);
	out_freq(r->o, (uint32_t) hz);

	return out_end(r->o);
}

// 26: selected / unselected VFO mode, data, filter
static size_t cmd_vfo_mode(req_t *r)
{
	int vfo = r->n >= 1 && r->d[0] != 0 ? 1 : 0;
	uint8_t civ, fil;
	bool data;

	// Set: data and filter may be skipped: data off, FIL1 (IC-705)
	if (r->n >= 2) {
		uint8_t dm = r->n >= 3 ? r->d[2] : 0, fl = r->n >= 4 ? r->d[3] : 1;

		if (dm != 0 && r->d[1] != 0x00 && r->d[1] != 0x01)
			return reply_ng(r);

		return reply_ok(r, set_mode(r, vfo, r->d[1], dm != 0, fl));
	}

	if (!get_mode(r, vfo, &civ, &data, &fil))
		return reply_ng(r);

	reply_head(r);
	out_byte(r->o, 0x26);
	out_byte(r->o, (uint8_t) vfo);
	out_byte(r->o, civ);
	out_byte(r->o, data ? 0x01 : 0x00);
	out_byte(r->o, fil);

	return out_end(r->o);
}

//
// 27: scope settings (IC-705). Most subcommands carry a leading 00 (main scope).
//

typedef struct
{
	uint8_t sub;
	x6100_civ_item_t item;
	bool selector;		/* data starts with 00 (main scope) */
} scope_map_t;

static const scope_map_t g_scope[] = {
	{ 0x10, X6100_CIV_SCOPE_ON, false },
	{ 0x11, X6100_CIV_SCOPE_OUT, false },
	{ 0x14, X6100_CIV_SCOPE_MODE, true },
	{ 0x16, X6100_CIV_SCOPE_EDGE, true },
	{ 0x17, X6100_CIV_SCOPE_HOLD, true },
	{ 0x1A, X6100_CIV_SCOPE_SPEED, true },
	{ 0x1D, X6100_CIV_SCOPE_VBW, true },
};

static size_t cmd_scope(req_t *r)
{
	int32_t v;

	if (r->n < 1)
		return reply_ng(r);

	uint8_t sub = r->d[0];
	const uint8_t *a = r->d + 1;
	size_t m = r->n - 1;

	switch (sub) {
	case 0x12:	/* main / sub: main only */
	case 0x13:	/* single / dual: single only */
		if (m >= 1)
			return reply_ok(r, a[0] == 0x00);

		reply_head(r);
		out_byte(r->o, 0x27);
		out_byte(r->o, sub);
		out_byte(r->o, 0x00);

		return out_end(r->o);
	case 0x15: {	/* span: 00 + 5 bytes frequency format (half span) */
		uint32_t hz;

		if (m >= 6)
			return reply_ok(r, a[0] == 0x00 && get_freq_bcd(a + 1, &hz)
				&& set(r, X6100_CIV_SCOPE_SPAN, 0, (int32_t) hz));

		if (!get(r, X6100_CIV_SCOPE_SPAN, 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x27);
		out_byte(r->o, 0x15);
		out_byte(r->o, 0x00);
		out_freq(r->o, (uint32_t) v);

		return out_end(r->o);
	}
	case 0x19: {	/* reference level: 00, dB (BCD), 0.1 dB << 4, sign */
		if (m >= 4) {
			int db = bcd_val(a[1]), tenth = a[2] >> 4;

			if (a[0] != 0x00 || db < 0 || tenth > 9)
				return reply_ng(r);

			v = db * 10 + tenth;

			return reply_ok(r, set(r, X6100_CIV_SCOPE_REF, 0, a[3] ? -v : v));
		}

		if (!get(r, X6100_CIV_SCOPE_REF, 0, &v))
			return reply_ng(r);

		int abs10 = v < 0 ? -v : v;

		reply_head(r);
		out_byte(r->o, 0x27);
		out_byte(r->o, 0x19);
		out_byte(r->o, 0x00);
		out_byte(r->o, bcd_byte((unsigned) abs10 / 10));
		out_byte(r->o, (uint8_t) ((abs10 % 10) << 4));
		out_byte(r->o, v < 0 ? 0x01 : 0x00);

		return out_end(r->o);
	}
	default:
		break;
	}

	for (size_t i = 0; i < COUNT(g_scope); i++) {
		const scope_map_t *s = &g_scope[i];

		if (s->sub != sub)
			continue;

		if (m >= (s->selector ? 2u : 1u)) {
			if (s->selector && a[0] != 0x00)
				return reply_ng(r);

			int val = bcd_val(a[s->selector ? 1 : 0]);

			return reply_ok(r, val >= 0 && set(r, s->item, 0, val));
		}

		if (!get(r, s->item, 0, &v))
			return reply_ng(r);

		reply_head(r);
		out_byte(r->o, 0x27);
		out_byte(r->o, sub);

		if (s->selector)
			out_byte(r->o, 0x00);

		out_byte(r->o, bcd_byte((unsigned) v));

		return out_end(r->o);
	}

	return reply_ng(r);	/* 00 (read only, streamed), 1B, 1C, 1E, 20 */
}

//
// Public
//

void x6100_civ_init(x6100_civ_t *civ, const x6100_civ_ops_t *ops)
{
	memset(civ, 0, sizeof(*civ));
	civ->addr = X6100_CIV_ADDR_IC705;
	civ->ctrl = X6100_CIV_ADDR_CTRL;

	if (ops)
		civ->ops = *ops;
}

size_t x6100_civ_process(x6100_civ_t *civ, const uint8_t *frame, size_t len, uint8_t *reply, size_t max)
{
	if (len < 6 || frame[0] != CIV_PRE || frame[1] != CIV_PRE || frame[len - 1] != CIV_END)
		return 0;

	uint8_t to = frame[2], from = frame[3];

	if ((to != civ->addr && to != X6100_CIV_ADDR_ALL) || from == civ->addr)
		return 0;	/* not for us, or our own echo */

	if (to == civ->addr)
		civ->ctrl = from;

	out_t o = { .buf = reply, .max = max };
	req_t r = { .civ = civ, .o = &o, .to = from, .cmd = frame[4], .d = frame + 5, .n = len - 6 };
	size_t n;

	switch (r.cmd) {
	case 0x00:
	case 0x03:
	case 0x05:
		n = cmd_freq(&r);
		break;
	case 0x01:
	case 0x04:
	case 0x06:
		n = cmd_mode(&r);
		break;
	case 0x07:
		n = cmd_vfo(&r);
		break;
	case 0x0F:
		n = cmd_split(&r);
		break;
	case 0x10:
		n = cmd_step(&r);
		break;
	case 0x11:
		n = cmd_att(&r);
		break;
	case 0x14:
		n = cmd_level(&r);
		break;
	case 0x15:
		n = cmd_meter(&r);
		break;
	case 0x16:
		n = cmd_switch(&r);
		break;
	case 0x19:
		n = cmd_id(&r);
		break;
	case 0x1A:
		n = cmd_misc(&r);
		break;
	case 0x1C:
		n = cmd_tx(&r);
		break;
	case 0x1D:
		n = cmd_xiegu_id(&r);
		break;
	case 0x21:
		n = cmd_rit(&r);
		break;
	case 0x25:
		n = cmd_vfo_freq(&r);
		break;
	case 0x26:
		n = cmd_vfo_mode(&r);
		break;
	case 0x18:
		n = cmd_power(&r);
		break;
	case 0x27:
		n = cmd_scope(&r);
		break;
	default:
		n = reply_ng(&r);	/* 02, memories, ...: not yet */
		break;
	}

	// Broadcasts (to 00) are acted on but never answered
	return to == X6100_CIV_ADDR_ALL ? 0 : n;
}

size_t x6100_civ_announce_freq(const x6100_civ_t *civ, uint32_t hz, uint8_t *out, size_t max)
{
	out_t o = { .buf = out, .max = max };

	out_head(&o, X6100_CIV_ADDR_ALL, civ->addr);
	out_byte(&o, 0x00);
	out_freq(&o, hz);

	return out_end(&o);
}

size_t x6100_civ_announce_mode(const x6100_civ_t *civ, int mode, int fil, uint8_t *out, size_t max)
{
	out_t o = { .buf = out, .max = max };
	uint8_t c;
	bool data;

	if (!mode_to_civ(mode, &c, &data))
		return 0;

	out_head(&o, X6100_CIV_ADDR_ALL, civ->addr);
	out_byte(&o, 0x01);
	out_byte(&o, c);
	out_byte(&o, (uint8_t) (fil >= 1 && fil <= 3 ? fil : 1));

	return out_end(&o);
}

void x6100_civ_framer_reset(x6100_civ_framer_t *f)
{
	f->len = 0;
}

size_t x6100_civ_framer_feed(x6100_civ_framer_t *f, uint8_t byte, uint8_t *frame, size_t max)
{
	// CI-V data never contains FE: a preamble inside a frame starts a new one
	if (byte == CIV_PRE && f->len > 2)
		f->len = 0;

	if (f->len < 2) {
		if (byte != CIV_PRE) {
			f->len = 0;
			return 0;
		}

		f->buf[f->len++] = byte;

		return 0;
	}

	if (f->len == 2 && byte == CIV_PRE)
		return 0;	/* extra preamble bytes are allowed */

	if (f->len >= sizeof(f->buf)) {
		f->len = 0;
		return 0;
	}

	f->buf[f->len++] = byte;

	if (byte != CIV_END)
		return 0;

	size_t n = f->len;

	f->len = 0;

	if (n < 6 || n > max)
		return 0;

	memcpy(frame, f->buf, n);

	return n;
}

size_t x6100_civ_scope_frames(const x6100_civ_t *civ, const x6100_civ_scope_t *s, uint8_t *out, size_t max)
{
	const size_t chunk = 50;
	size_t len = s->out_of_range || !s->wave ? 0 : s->len;
	unsigned divs = 1 + (unsigned) ((len + chunk - 1) / chunk);
	out_t o = { .buf = out, .max = max };
	bool center = s->mode == 0 || s->mode == 2;

	if (divs > 99)
		return 0;

	// Frame 1: wave information, no data
	o.len = 0;
	out_byte(&o, CIV_PRE);
	out_byte(&o, CIV_PRE);
	out_byte(&o, civ->ctrl);
	out_byte(&o, civ->addr);
	out_byte(&o, 0x27);
	out_byte(&o, 0x00);
	out_byte(&o, 0x00);
	out_byte(&o, 0x01);
	out_byte(&o, bcd_byte(divs));
	out_byte(&o, (uint8_t) s->mode);
	out_freq(&o, center ? s->centre : s->lo);
	out_freq(&o, center ? s->half_span : s->hi);
	out_byte(&o, s->out_of_range ? 0x01 : 0x00);
	out_byte(&o, CIV_END);

	// Frames 2..: waveform, 50 points each (points are binary 0-160)
	for (unsigned d = 2; d <= divs; d++) {
		size_t from = (d - 2) * chunk;
		size_t to = from + chunk < len ? from + chunk : len;

		out_byte(&o, CIV_PRE);
		out_byte(&o, CIV_PRE);
		out_byte(&o, civ->ctrl);
		out_byte(&o, civ->addr);
		out_byte(&o, 0x27);
		out_byte(&o, 0x00);
		out_byte(&o, 0x00);
		out_byte(&o, bcd_byte(d));
		out_byte(&o, bcd_byte(divs));

		for (size_t i = from; i < to; i++)
			out_byte(&o, s->wave[i] > 160 ? 160 : s->wave[i]);

		out_byte(&o, CIV_END);
	}

	return o.overflow ? 0 : o.len;
}
