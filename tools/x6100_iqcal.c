/*
 * x6100_iqcal - RX IQ image calibration from Linux (registers 48 rphscmp, 49 rmagcmp)
 *
 * The factory calibration block of the test unit has rmagcmp / rphscmp = 0 for every
 * band: no IQ correction, so every strong signal shows a mirror image around the
 * VFO. This tool measures the image and adjusts the two registers; it never writes
 * the calibration block.
 *
 * Needs one steady carrier: a broadcast carrier, or another rig at low power into a
 * dummy load next to the X6100, or a signal generator. The VFO is placed <offset> Hz
 * below the carrier, so the carrier shows at +offset and its image at -offset.
 *
 *   x6100_iqcal <carrier_hz> probe [offset_hz]
 *       step each register over 0, +-10, +-100, +-1000, +-10000 and print the image
 *       rejection: shows the scale and sign of both registers
 *
 *   x6100_iqcal <carrier_hz> search [offset_hz] [start_step]
 *       coordinate search for the best rejection; prints the values to keep
 *
 *   x6100_iqcal <carrier_hz> set <rmagcmp> <rphscmp> [offset_hz]
 *       apply given values and measure (verify a result, or try by hand)
 *
 * Default offset 12000 Hz, default start step 1024.
 * RX only: register 12 stays 0; aborts if the baseband reports TX.
 * Stop the GUI first (scripts/x6100_ui.sh stop).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPAN_HZ		96000
#define FRAMES		24	/* frames averaged per measurement, ~0.9 s */
#define SETTLE_FRAMES	6	/* frames dropped after a register change */
#define BIN_SPREAD	2	/* bins either side summed (Hann main lobe) */
#define MIN_SNR_DB	40.0

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void) sig;
	g_stop = 1;
}

typedef struct
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_flow_t flow;
	int offset_bins;
} cal_t;

typedef struct
{
	double carrier_db;
	double image_db;
	double floor_db;
} meas_t;

//
// In-place radix-2 FFT, n a power of two
//

static void fft(float (*v)[2], int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;

		j ^= bit;

		if (i < j) {
			float tr = v[i][0], ti = v[i][1];

			v[i][0] = v[j][0];
			v[i][1] = v[j][1];
			v[j][0] = tr;
			v[j][1] = ti;
		}
	}

	for (int len = 2; len <= n; len <<= 1) {
		double ang = -2.0 * M_PI / len;
		float wr = (float) cos(ang), wi = (float) sin(ang);

		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;

			for (int k = 0; k < len / 2; k++) {
				float *a = v[i + k], *b = v[i + k + len / 2];
				float tr = b[0] * cr - b[1] * ci;
				float ti = b[0] * ci + b[1] * cr;
				float nr;

				b[0] = a[0] - tr;
				b[1] = a[1] - ti;
				a[0] += tr;
				a[1] += ti;
				nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

//
// Read one flow frame, blocking up to 1 s; aborts on TX
//

static int next_frame(cal_t *c, x6100_flow_frame_t *f)
{
	for (int tries = 0; tries < 200 && !g_stop; tries++) {
		int rc = x6100_flow_read(&c->flow, f);

		if (rc > 0) {
			if (f->flags & X6100_FLOW_TX) {
				fprintf(stderr, "ABORT: baseband reports TX\n");
				g_stop = 1;
				return -1;
			}

			return 0;
		}

		x6100_ctl_sync(&c->ctl);
		usleep(5000);
	}

	return -1;
}

static double band_power(const double *pow, int centre, int n)
{
	double sum = 0.0;

	for (int d = -BIN_SPREAD; d <= BIN_SPREAD; d++)
		sum += pow[((centre + d) % n + n) % n];

	return sum;
}

//
// Average power spectrum over FRAMES frames; carrier / image at +-offset from the
// centre bin (the side is taken from whichever is stronger, so the IQ mirroring of
// the flow does not matter)
//

static int measure(cal_t *c, meas_t *m)
{
	static float buf[X6100_FLOW_SAMPLES][2];
	static double pow[X6100_FLOW_SAMPLES];
	const int n = X6100_FLOW_SAMPLES;
	x6100_flow_frame_t f;
	double a, b, floor_p;

	for (int i = 0; i < SETTLE_FRAMES; i++) {
		if (next_frame(c, &f) < 0)
			return -1;
	}

	memset(pow, 0, sizeof(pow));

	for (int fr = 0; fr < FRAMES; fr++) {
		if (next_frame(c, &f) < 0)
			return -1;

		for (int i = 0; i < n; i++) {
			float w = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (float) i / (float) (n - 1));

			buf[i][0] = f.iq[i][0] * w;
			buf[i][1] = f.iq[i][1] * w;
		}

		fft(buf, n);

		for (int i = 0; i < n; i++)
			pow[i] += (double) buf[i][0] * buf[i][0] + (double) buf[i][1] * buf[i][1];
	}

	//
	// FFT bin 0 is the centre; +offset and -offset bins
	//
	a = band_power(pow, c->offset_bins, n);
	b = band_power(pow, -c->offset_bins, n);

	//
	// Floor: mean of 16 bins around -24 kHz, away from centre, carrier and image
	//
	floor_p = 0.0;

	for (int i = n / 4 - 8; i < n / 4 + 8; i++)
		floor_p += pow[(n / 2 + i) % n];

	floor_p = floor_p / 16.0 * (2 * BIN_SPREAD + 1);

	m->carrier_db = 10.0 * log10((a > b ? a : b) + 1e-30);
	m->image_db = 10.0 * log10((a > b ? b : a) + 1e-30);
	m->floor_db = 10.0 * log10(floor_p + 1e-30);
	return 0;
}

static void apply_iq(cal_t *c, int32_t mag, int32_t phs)
{
	x6100_ctl_set(&c->ctl, X6100_REG_RMAGCMP, (uint32_t) mag);
	x6100_ctl_set(&c->ctl, X6100_REG_RPHSCMP, (uint32_t) phs);
	x6100_ctl_sync(&c->ctl);
}

//
// Image rejection in dB for a register pair; < 0 on error
//

static double rejection(cal_t *c, int32_t mag, int32_t phs, meas_t *m)
{
	apply_iq(c, mag, phs);

	if (measure(c, m) < 0)
		return -1.0;

	return m->carrier_db - m->image_db;
}

static void print_meas(const char *tag, int32_t mag, int32_t phs, const meas_t *m)
{
	printf("%-8s rmagcmp %7d  rphscmp %7d   carrier %6.1f dB  image %6.1f dB  floor %6.1f dB  rejection %5.1f dB\n",
		tag, mag, phs, m->carrier_db, m->image_db, m->floor_db, m->carrier_db - m->image_db);
	fflush(stdout);
}

static int check_carrier(const meas_t *m)
{
	if (m->carrier_db - m->floor_db < MIN_SNR_DB) {
		fprintf(stderr, "carrier only %.1f dB above the floor (need %.0f): no steady carrier at the given frequency?\n",
			m->carrier_db - m->floor_db, MIN_SNR_DB);
		return -1;
	}

	return 0;
}

static int do_probe(cal_t *c)
{
	static const int32_t steps[] = { 0, 10, -10, 100, -100, 1000, -1000, 10000, -10000 };
	meas_t m;

	if (rejection(c, 0, 0, &m) < 0)
		return 1;

	print_meas("base", 0, 0, &m);

	if (check_carrier(&m) < 0)
		return 1;

	for (size_t i = 1; i < sizeof(steps) / sizeof(steps[0]) && !g_stop; i++) {
		if (rejection(c, steps[i], 0, &m) < 0)
			return 1;

		print_meas("mag", steps[i], 0, &m);
	}

	for (size_t i = 1; i < sizeof(steps) / sizeof(steps[0]) && !g_stop; i++) {
		if (rejection(c, 0, steps[i], &m) < 0)
			return 1;

		print_meas("phase", 0, steps[i], &m);
	}

	return 0;
}

static int do_search(cal_t *c, int32_t step)
{
	int32_t mag = 0, phs = 0;
	meas_t m;
	double best = rejection(c, mag, phs, &m);

	if (best < 0.0)
		return 1;

	print_meas("start", mag, phs, &m);

	if (check_carrier(&m) < 0)
		return 1;

	while (step >= 1 && !g_stop) {
		bool improved = false;

		for (int axis = 0; axis < 2 && !g_stop; axis++) {
			for (int dir = -1; dir <= 1 && !g_stop; dir += 2) {
				int32_t tm = mag + (axis == 0 ? dir * step : 0);
				int32_t tp = phs + (axis == 1 ? dir * step : 0);
				double r = rejection(c, tm, tp, &m);

				if (r < 0.0)
					return 1;

				if (r > best + 0.3) {
					best = r;
					mag = tm;
					phs = tp;
					improved = true;
					print_meas("better", mag, phs, &m);
					break;
				}
			}
		}

		if (!improved)
			step /= 2;
	}

	apply_iq(c, mag, phs);
	printf("\nbest: rmagcmp %d  rphscmp %d  rejection %.1f dB\n", mag, phs, best);
	printf("keep per band; verify with: x6100_iqcal <carrier_hz> set %d %d\n", mag, phs);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <carrier_hz> probe [offset_hz]\n"
		"       %s <carrier_hz> search [offset_hz] [start_step]\n"
		"       %s <carrier_hz> set <rmagcmp> <rphscmp> [offset_hz]\n", argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	static cal_t c;
	x6100_cal_t cal;
	uint32_t carrier, vfo;
	int offset = 12000;
	int32_t step = 1024, mag = 0, phs = 0;
	int rc;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	carrier = (uint32_t) strtoul(argv[1], NULL, 10);

	if (strcmp(argv[2], "probe") == 0) {
		offset = argc > 3 ? atoi(argv[3]) : 12000;
	} else if (strcmp(argv[2], "search") == 0) {
		offset = argc > 3 ? atoi(argv[3]) : 12000;
		step = argc > 4 ? atoi(argv[4]) : 1024;
	} else if (strcmp(argv[2], "set") == 0 && argc >= 5) {
		mag = (int32_t) strtol(argv[3], NULL, 10);
		phs = (int32_t) strtol(argv[4], NULL, 10);
		offset = argc > 5 ? atoi(argv[5]) : 12000;
	} else {
		usage(argv[0]);
		return 1;
	}

	if (offset < 2000 || offset > 40000 || step < 1 || carrier < 500000 + (uint32_t) offset || carrier > 56000000) {
		usage(argv[0]);
		return 1;
	}

	vfo = carrier - (uint32_t) offset;
	c.offset_bins = (int) lround((double) offset * X6100_FLOW_SAMPLES / SPAN_HZ);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if ((rc = x6100_i2c_open(&c.i2c, NULL)) < 0 || (rc = x6100_flow_open(&c.flow, NULL)) < 0) {
		fprintf(stderr, "open: %s\n", strerror(-rc));
		return 1;
	}

	x6100_ctl_init(&c.ctl, &c.i2c);

	if (x6100_cal_read(&c.i2c, &cal) == 0)
		x6100_cal_apply(&c.ctl, &cal, x6100_band_index(vfo));

	x6100_set_freq(&c.ctl, 0, vfo);
	x6100_set_freq(&c.ctl, 1, vfo);
	x6100_set_mode(&c.ctl, 0, X6100_MODE_USB);
	x6100_set_mode(&c.ctl, 1, X6100_MODE_USB);
	x6100_ctl_set(&c.ctl, X6100_REG_RXVOL, 0);
	x6100_ctl_set(&c.ctl, X6100_REG_FLAGS, 0);

	if ((rc = x6100_ctl_refresh(&c.ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	printf("carrier %u Hz, VFO %u Hz, offset %d Hz (%d bins), %d frames per point\n", carrier, vfo, offset,
		c.offset_bins, FRAMES);

	if (strcmp(argv[2], "probe") == 0) {
		rc = do_probe(&c);
	} else if (strcmp(argv[2], "search") == 0) {
		rc = do_search(&c, step);
	} else {
		meas_t m;

		rc = rejection(&c, mag, phs, &m) < 0;

		if (rc == 0)
			print_meas("set", mag, phs, &m);
	}

	x6100_flow_close(&c.flow);
	x6100_i2c_close(&c.i2c);
	return rc;
}
