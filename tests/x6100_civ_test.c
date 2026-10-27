/*
 * x6100_civ_test.c - host test of the CI-V engine against a mock radio
 *
 *   cc -std=c11 -Wall -Wextra -Iinclude -o civ_test tests/x6100_civ_test.c src/x6100_civ.c
 *   ./civ_test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100_civ.h"

#include <stdio.h>
#include <string.h>

#include "x6100_regs.h"

//
// Mock radio
//

typedef struct
{
	int32_t v[X6100_CIV_ITEM_COUNT][2];
	bool supported[X6100_CIV_ITEM_COUNT];
	x6100_civ_mem_t mem[100];	/* channels 1-99 */
} mock_t;

static bool mock_get(void *ctx, x6100_civ_item_t item, int vfo, int32_t *value)
{
	mock_t *m = ctx;

	vfo = vfo > 1 ? 0 : vfo;	/* band items carry a band code */

	if (!m->supported[item])
		return false;

	*value = m->v[item][vfo];

	return true;
}

static bool mock_set(void *ctx, x6100_civ_item_t item, int vfo, int32_t value)
{
	mock_t *m = ctx;

	vfo = vfo > 1 ? 0 : vfo;	/* band items carry a band code */

	if (!m->supported[item])
		return false;

	// Memories: write the VFO, recall into it, clear
	if (item == X6100_CIV_MEM_WRITE || item == X6100_CIV_MEM_TO_VFO || item == X6100_CIV_MEM_CLEAR) {
		x6100_civ_mem_t *c = &m->mem[value];

		if (item == X6100_CIV_MEM_TO_VFO) {
			if (!c->used)
				return false;

			m->v[X6100_CIV_FREQ][0] = (int32_t) c->hz;
			m->v[X6100_CIV_MODE][0] = c->mode;
		} else {
			memset(c, 0, sizeof(*c));
			c->used = item == X6100_CIV_MEM_WRITE;
			c->hz = (uint32_t) m->v[X6100_CIV_FREQ][0];
			c->mode = m->v[X6100_CIV_MODE][0];
			c->fil = 1;
		}

		return true;
	}

	if (item == X6100_CIV_VFO_SWAP) {
		static const x6100_civ_item_t per_vfo[] = { X6100_CIV_FREQ, X6100_CIV_MODE, X6100_CIV_FIL };

		for (size_t i = 0; i < sizeof(per_vfo) / sizeof(per_vfo[0]); i++) {
			int32_t t = m->v[per_vfo[i]][0];

			m->v[per_vfo[i]][0] = m->v[per_vfo[i]][1];
			m->v[per_vfo[i]][1] = t;
		}

		return true;
	}

	if (item == X6100_CIV_PTT)
		m->v[X6100_CIV_TX][0] = value;	/* the radio follows PTT */

	m->v[item][vfo] = value;

	return true;
}

static bool mock_mem_get(void *ctx, int ch, x6100_civ_mem_t *mem)
{
	*mem = ((mock_t *) ctx)->mem[ch];
	return true;
}

static bool mock_mem_set(void *ctx, int ch, const x6100_civ_mem_t *mem)
{
	((mock_t *) ctx)->mem[ch] = *mem;
	return true;
}

static int g_fail;
static int g_run;

//
// Send a request (hex string without FE FE / FD) to an address, compare the reply
//

static void check_to(x6100_civ_t *civ, uint8_t to, const char *req, const char *want)
{
	uint8_t in[X6100_CIV_FRAME_MAX] = { 0xFE, 0xFE, to, 0xE0 }, out[X6100_CIV_FRAME_MAX];
	size_t n = 4;
	char got[600] = "";
	unsigned b;
	int pos;

	for (const char *p = req; sscanf(p, "%2x%n", &b, &pos) == 1; p += pos)
		in[n++] = (uint8_t) b;

	in[n++] = 0xFD;

	size_t r = x6100_civ_process(civ, in, n, out, sizeof(out));

	// Reply without FE FE E0 <us> ... FD
	for (size_t i = 4; r > 5 && i < r - 1; i++)
		snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%02X", i > 4 ? " " : "", out[i]);

	if (r > 0 && (out[2] != 0xE0 || out[3] != civ->addr || out[r - 1] != 0xFD))
		snprintf(got, sizeof(got), "bad header");

	g_run++;

	if (strcmp(got, want) != 0) {
		g_fail++;
		printf("FAIL %-22s want [%s] got [%s]\n", req, want, got);
	}
}

//
// The same, to our own address
//

static void check(x6100_civ_t *civ, const char *req, const char *want)
{
	check_to(civ, civ->addr, req, want);
}

int main(void)
{
	static mock_t m;
	x6100_civ_ops_t ops = { .ctx = &m, .get = mock_get, .set = mock_set, .mem_get = mock_mem_get, .mem_set = mock_mem_set };
	x6100_civ_t civ;

	for (int i = 0; i < X6100_CIV_ITEM_COUNT; i++)
		m.supported[i] = true;

	m.supported[X6100_CIV_LVL_BACKLIGHT] = false;
	m.supported[X6100_CIV_LVL_PBT1] = false;
	m.supported[X6100_CIV_NB] = false;
	m.v[X6100_CIV_FREQ][0] = 14074000;
	m.v[X6100_CIV_FREQ][1] = 7074000;
	m.v[X6100_CIV_MODE][0] = X6100_MODE_USB_DIG;
	m.v[X6100_CIV_MODE][1] = X6100_MODE_LSB;
	m.v[X6100_CIV_FIL][0] = 2;
	m.v[X6100_CIV_FIL][1] = 1;
	m.v[X6100_CIV_FIL_WIDTH][0] = 2400;
	m.v[X6100_CIV_STEP][0] = 100;
	m.v[X6100_CIV_LVL_AF][0] = 51;
	m.v[X6100_CIV_MTR_S][0] = 120;
	m.v[X6100_CIV_AGC][0] = 3;
	m.v[X6100_CIV_RIT_HZ][0] = -150;

	x6100_civ_init(&civ, &ops);

	// Frequency
	check(&civ, "03", "03 00 40 07 14 00");
	check(&civ, "05 00 50 07 14 00", "FB");
	check(&civ, "03", "03 00 50 07 14 00");
	check(&civ, "05 00 5A 07 14 00", "FA");
	check(&civ, "00 00 40 07 14 00", "");		/* transceive: no reply */

	// Mode: data kept, filter given
	check(&civ, "04", "04 01 02");
	check(&civ, "06 00 03", "FB");
	check(&civ, "1A 06", "1A 06 00 00");		/* 06 clears data mode */
	check(&civ, "06 02", "FB");
	check(&civ, "04", "04 02 01");			/* filter skipped: FIL1 */
	check(&civ, "1A 06 01 01", "FA");		/* data not allowed on AM */
	check(&civ, "06 01 01", "FB");
	check(&civ, "1A 06 01 01", "FB");
	check(&civ, "1A 06", "1A 06 01 01");

	// VFO, split, step
	check(&civ, "25 01", "25 01 00 40 07 07 00");
	check(&civ, "26 01", "26 01 00 00 01");
	check(&civ, "25 01 00 50 07 07 00", "FB");
	check(&civ, "25 01", "25 01 00 50 07 07 00");
	check(&civ, "26 01 05 00 02", "FB");
	check(&civ, "26 01", "26 01 05 00 02");
	check(&civ, "07 B0", "FB");
	check(&civ, "03", "03 00 50 07 07 00");
	check(&civ, "0F 01", "FB");
	check(&civ, "0F", "0F 01");
	check(&civ, "0F 00", "FB");
	check(&civ, "0F 11", "FB");			/* DUP- */
	check(&civ, "0F", "0F 11");
	check(&civ, "0F 12", "FB");			/* DUP+ */
	check(&civ, "0F", "0F 12");
	check(&civ, "0F 10", "FB");			/* simplex */
	check(&civ, "0F", "0F 00");
	check(&civ, "0F 13", "FA");
	check(&civ, "0F 02", "FA");
	check(&civ, "0F 01", "FB");
	check(&civ, "10", "10 01");
	check(&civ, "10 04", "FB");
	check(&civ, "10", "10 04");
	check(&civ, "10 09", "FB");			/* 12.5 kHz */
	check(&civ, "10", "10 09");
	check(&civ, "10 14", "FA");

	// ATT, levels, meters, switches
	check(&civ, "11 20", "FB");
	check(&civ, "11", "11 20");
	check(&civ, "14 01", "14 01 00 51");
	check(&civ, "14 01 02 55", "FB");
	check(&civ, "14 01", "14 01 02 55");
	check(&civ, "14 19", "14 19 00 00");		/* unsupported level reads 0000 */
	check(&civ, "14 20", "FA");
	check(&civ, "15 02", "15 02 01 20");
	check(&civ, "16 12", "16 12 03");
	check(&civ, "16 12 04", "FA");
	check(&civ, "16 12 00", "FA");		/* AGC 01-03 only */
	check(&civ, "15 07", "15 07 00");
	check(&civ, "16 02 02", "FB");
	check(&civ, "16 02", "16 02 01");

	// ID, lock, IF width, TX, ATU, RIT
	check(&civ, "19 00", "19 00 A4");
	check_to(&civ, X6100_CIV_ADDR_ALL, "19 00", "19 00 A4");	/* broadcast: answered (auto detect) */
	check_to(&civ, X6100_CIV_ADDR_ALL, "03", "");		/* other broadcasts: not */
	check_to(&civ, X6100_CIV_ADDR_X6100, "03", "");		/* not our address */
	civ.addr = X6100_CIV_ADDR_X6100;
	check_to(&civ, X6100_CIV_ADDR_ALL, "19 00", "19 00 61");
	check(&civ, "1D 19", "1D 19 61 00");
	check_to(&civ, X6100_CIV_ADDR_IC705, "03", "");
	civ.addr = X6100_CIV_ADDR_IC705;
	check(&civ, "1D 19", "1D 19 61 00");
	check(&civ, "1A 05 00 62 01", "FA");		/* IC-705 menu: not supported */
	check(&civ, "1A 03", "FA");			/* NFM: no IF width */
	check(&civ, "06 01", "FB");
	check(&civ, "1A 03 15", "FB");		/* 1100 Hz */
	check(&civ, "1A 03", "1A 03 15");
	check(&civ, "07 A0", "FB");
	check(&civ, "1C 03", "1C 03 00 40 07 14 00");
	check(&civ, "26 00 03", "FB");
	check(&civ, "26 00", "26 00 03 00 01");
	check(&civ, "1C 00", "1C 00 00");
	check(&civ, "1C 00 01", "FB");
	check(&civ, "1C", "1C 00 01");
	check(&civ, "1C 01 02", "FB");
	check(&civ, "21 00", "21 00 50 01 01");
	check(&civ, "21 00 34 12 00", "FB");
	check(&civ, "21 00", "21 00 34 12 00");
	check(&civ, "21 01 01", "FB");
	check(&civ, "21 01", "21 01 01");
	check(&civ, "27 10 01", "FB");
	check(&civ, "27 10", "27 10 01");
	check(&civ, "27 14 00 02", "FB");
	check(&civ, "27 14", "27 14 00 02");
	check(&civ, "27 14 01 02", "FA");		/* sub scope */
	check(&civ, "27 15 00 00 00 05 00 00", "FB");	/* +-50 kHz */
	check(&civ, "27 15", "27 15 00 00 00 05 00 00");
	check(&civ, "27 19 00 10 50 01", "FB");	/* -10.5 dB */
	check(&civ, "27 19", "27 19 00 10 50 01");
	check(&civ, "27 12", "27 12 00");
	check(&civ, "27 00", "FA");
	check(&civ, "14 07", "14 07 01 28");		/* PBT centre */
	check(&civ, "16 22", "16 22 00");		/* unsupported function reads OFF */
	check(&civ, "16 22 01", "FA");
	check(&civ, "18 01", "FB");
	check(&civ, "18 00", "FA");
	check(&civ, "1A 05 01 19", "1A 05 01 19 01");
	check(&civ, "1A 05 01 18 00", "FB");
	check(&civ, "1A 05 00 01", "FA");
	check(&civ, "1A 01 05 01 00 00 25 07 00 00 01 00 00 00 00 08 85 00 08 85 00 00 23 00 00 00 00 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20", "FB");
	check(&civ, "1A 01 05 01", "1A 01 05 01 00 00 25 07 00 00 01 00 00 00 00 08 85 00 08 85 00 00 23 00 00 00 00 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20");
	check(&civ, "1A 01 16 01", "FA");


	// Scope frames: 475 points = 1 info + 10 data frames (9 x 50 + 25)
	{
		static uint8_t wave[X6100_CIV_SCOPE_POINTS], out[1024];
		x6100_civ_scope_t s = { .mode = 0, .centre = 14074000, .half_span = 48000, .wave = wave,
			.len = X6100_CIV_SCOPE_POINTS };
		size_t n, frames = 0, first_len = 0, last_len = 0, start = 0;

		for (int i = 0; i < X6100_CIV_SCOPE_POINTS; i++)
			wave[i] = (uint8_t) (i % 200);

		n = x6100_civ_scope_frames(&civ, &s, out, sizeof(out));

		for (size_t i = 0; i < n; i++) {
			if (out[i] == 0xFD) {
				if (frames == 0)
					first_len = i + 1 - start;

				last_len = i + 1 - start;
				frames++;
				start = i + 1;
			}
		}

		g_run++;

		// info: 4 head + 27 00 + 00 01 11 mode + 10 + oor + FD = 22; last: 9 + 25 + 1 = 35
		if (frames != 11 || first_len != 22 || last_len != 35 || out[8] != 0x11 || out[2] != 0xE0) {
			g_fail++;
			printf("FAIL scope frames: %zu frames, first %zu, last %zu\n", frames, first_len, last_len);
		}

		// SCROLL-C (2) sends the edges as FIX / SCROLL-F do; only CENTER (0) sends centre +
		// half span (wfview reads every other mode as edges)
		static const uint8_t edges[10] = { 0x00, 0x00, 0x05, 0x14, 0x00, 0x00, 0x80, 0x09, 0x14, 0x00 };

		s.mode = 2;
		s.lo = 14050000;
		s.hi = 14098000;
		s.half_span = 24000;
		n = x6100_civ_scope_frames(&civ, &s, out, sizeof(out));
		g_run++;

		if (n < 22 || out[9] != 0x02 || memcmp(out + 10, edges, sizeof(edges)) != 0) {
			g_fail++;
			printf("FAIL scope SCROLL-C: edges expected\n");
		}
	}
	// Fixed edges (27 1E, wfview's layout: range, edge, lower, upper) and the edge number (27 16)
	check(&civ, "27 1E 06 02 00 00 05 14 00 00 60 14 14 00", "FB");	/* 14.050-14.146 MHz */
	check(&civ, "27 1E 06 02", "27 1E 06 02 00 00 05 14 00 00 60 14 14 00");
	check(&civ, "27 1E 06 05", "FA");					/* edge 5 */
	check(&civ, "27 1E 00 01", "FA");					/* range 00 */
	check(&civ, "27 16 00 03", "FB");
	check(&civ, "27 16 00", "27 16 00 03");

	// Memories (08-0B, 1A 00) and the transceive switch (1A 05 0131)
	{
		static const char calls[] = " 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20";
		static const char name_blank[] = " 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20";
		static const char ch5[] = " 00 40 07 14 00 01 01 00 00 00 00 08 85 00 08 85 00 00 23 00 00 00 00";
		static const char rpt[] = " 00 00 66 29 00 05 01 00 10 00 00 08 85 00 08 85 00 00 23 00 00 10 00";
		static const char spl_a[] = " 00 40 07 14 00 01 01 00 00 00 00 08 85 00 08 85 00 00 23 00 00 00 00";
		static const char spl_b[] = " 00 40 17 14 00 01 02 00 00 00 00 08 85 00 08 85 00 00 23 00 00 00 00";
		char want[600], req[600];

		m.v[X6100_CIV_FREQ][0] = 14074000;
		m.v[X6100_CIV_MODE][0] = X6100_MODE_USB;

		check(&civ, "08 00 05", "FA");			/* empty: selected, not tuned */
		check(&civ, "09", "FB");			/* VFO into channel 5 */
		snprintf(want, sizeof(want), "1A 00 00 00 05 00%s%s%s%s%s", ch5, calls, ch5, calls, name_blank);
		check(&civ, "1A 00 00 00 05", want);		/* B side as A */
		check(&civ, "0A", "FB");
		check(&civ, "0B", "FB");
		check(&civ, "1A 00 00 00 05", "1A 00 00 00 05 FF");
		check(&civ, "08 01 00", "FA");			/* channel 100 */
		check(&civ, "1A 00 01 00 05", "FA");		/* group 01 */
		check(&civ, "09 01", "FA");

		// 10 m repeater, NFM, DUP- 100 kHz, named: written, read back the same, tuned
		snprintf(req, sizeof(req), "1A 00 00 00 07 00%s%s%s%s 31 30 4D 20 52 50 54 20 20 20 20 20 20 20 20 20",
			rpt, calls, rpt, calls);
		check(&civ, req, "FB");
		snprintf(want, sizeof(want), "%s", req);
		check(&civ, "1A 00 00 00 07", want);
		g_run++;

		if (m.mem[7].dup != 1 || m.mem[7].dup_hz != 100000 || strcmp(m.mem[7].name, "10M RPT") != 0) {
			g_fail++;
			printf("FAIL memory 7: dup %d %u name [%s]\n", m.mem[7].dup, (unsigned) m.mem[7].dup_hz, m.mem[7].name);
		}

		check(&civ, "08 00 07", "FB");
		check(&civ, "03", "03 00 00 66 29 00");
		check(&civ, "1A 00 00 00 07 FF", "FB");		/* clear */
		check(&civ, "1A 00 00 00 07", "1A 00 00 00 07 FF");

		// Split channel: RX 14.074 USB FIL1 (A), TX 14.174 USB FIL2 (B)
		snprintf(req, sizeof(req), "1A 00 00 00 08 10%s%s%s%s%s", spl_a, calls, spl_b, calls, name_blank);
		check(&civ, req, "FB");
		g_run++;

		if (!m.mem[8].split || m.mem[8].tx_hz != 14174000 || m.mem[8].tx_mode != X6100_MODE_USB || m.mem[8].tx_fil != 2) {
			g_fail++;
			printf("FAIL memory 8: split %d tx %u mode %d fil %d\n", m.mem[8].split, (unsigned) m.mem[8].tx_hz,
				m.mem[8].tx_mode, m.mem[8].tx_fil);
		}

		check(&civ, "1A 00 00 00 08", req);
		check(&civ, "1A 00 00 00 08 FF", "FB");

		m.v[X6100_CIV_TRANSCEIVE][0] = 1;
		check(&civ, "1A 05 01 31", "1A 05 01 31 01");
		check(&civ, "1A 05 01 31 00", "FB");
		check(&civ, "1A 05 01 31", "1A 05 01 31 00");
		check(&civ, "1A 05 01 31 02", "FA");
	}

	// Announcements and framer
	{
		uint8_t out[32], frame[64];
		size_t n = x6100_civ_announce_freq(&civ, 7074000, out, sizeof(out));
		static const uint8_t want[] = { 0xFE, 0xFE, 0x00, 0xA4, 0x00, 0x00, 0x40, 0x07, 0x07, 0x00, 0xFD };
		x6100_civ_framer_t f;
		static const uint8_t stream[] = { 0x12, 0xFE, 0xFE, 0xA4, 0xFE, 0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD, 0x00 };
		size_t got = 0;

		g_run++;

		if (n != sizeof(want) || memcmp(out, want, n) != 0) {
			g_fail++;
			printf("FAIL announce_freq\n");
		}

		x6100_civ_framer_reset(&f);

		for (size_t i = 0; i < sizeof(stream); i++) {
			size_t k = x6100_civ_framer_feed(&f, stream[i], frame, sizeof(frame));

			if (k)
				got = k;
		}

		g_run++;

		if (got != 6 || frame[4] != 0x03) {
			g_fail++;
			printf("FAIL framer (got %zu)\n", got);
		}
	}

	printf("%d / %d passed\n", g_run - g_fail, g_run);

	return g_fail ? 1 : 0;
}
