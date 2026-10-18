/*
 * x6100_tune - first write test: receive-only tuning via libx6100
 *
 * Sets frequency, mode, filter and speaker volume, applies the per-band
 * calibration, then keeps the register file in sync while printing flow
 * telemetry and the IQ level. Listen on the speaker to verify the tuning.
 *
 * TX is never enabled: the flag register is forced to 0 before every sync,
 * and the tool aborts if the baseband reports TX.
 *
 * Stop the GUI first (x6100_ui.sh stop).
 *
 * Usage: x6100_tune <freq_hz> [usb|lsb|cw|cwr|am|nfm] [seconds] [rxvol]
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

#define DEF_SECONDS	30
#define DEF_RXVOL	20
#define REFRESH_MS	1000
#define REPORT_MS	500
#define LOOP_US		2000

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void) sig;
	g_stop = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static int parse_mode(const char *s, x6100_mode_t *mode)
{
	static const struct { const char *name; x6100_mode_t mode; } modes[] = {
		{ "lsb", X6100_MODE_LSB }, { "usb", X6100_MODE_USB }, { "cw", X6100_MODE_CW },
		{ "cwr", X6100_MODE_CWR }, { "am", X6100_MODE_AM }, { "nfm", X6100_MODE_NFM },
	};

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcmp(s, modes[i].name) == 0) {
			*mode = modes[i].mode;
			return 0;
		}
	}

	return -1;
}

//
// Passband per mode, in Hz relative to the carrier
//

static void mode_filter(x6100_mode_t mode, uint32_t *low, uint32_t *high)
{
	switch (mode) {
	case X6100_MODE_CW:
	case X6100_MODE_CWR:
		*low = 450;
		*high = 950;
		break;
	case X6100_MODE_AM:
	case X6100_MODE_NFM:
		*low = 0;
		*high = 4000;
		break;
	default:
		*low = 50;
		*high = 2950;
		break;
	}
}

//
// Mean IQ power in dB (relative, uncalibrated)
//

static double iq_level_db(const x6100_flow_frame_t *f)
{
	double sum = 0.0;

	for (int i = 0; i < X6100_FLOW_SAMPLES; i++) {
		double re = f->iq[i][0];
		double im = f->iq[i][1];

		sum += re * re + im * im;
	}

	sum /= X6100_FLOW_SAMPLES;
	return sum > 0.0 ? 10.0 * log10(sum) : -999.0;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <freq_hz> [usb|lsb|cw|cwr|am|nfm] [seconds] [rxvol 0-100]\n", argv0);
}

int main(int argc, char **argv)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	x6100_mode_t mode = X6100_MODE_USB;
	uint32_t freq, low, high;
	int seconds = DEF_SECONDS;
	int rxvol = DEF_RXVOL;
	unsigned frames = 0;
	int rc;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	freq = (uint32_t) strtoul(argv[1], NULL, 10);

	if (freq < 100000 || freq > 60000000 || (argc > 2 && parse_mode(argv[2], &mode) < 0)) {
		usage(argv[0]);
		return 1;
	}

	if (argc > 3)
		seconds = atoi(argv[3]);

	if (argc > 4)
		rxvol = atoi(argv[4]);

	if (rxvol < 0 || rxvol > 100 || seconds < 1) {
		usage(argv[0]);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if ((rc = x6100_i2c_open(&i2c, NULL)) < 0) {
		fprintf(stderr, "i2c open: %s\n", strerror(-rc));
		return 1;
	}

	if ((rc = x6100_flow_open(&flow, NULL)) < 0) {
		fprintf(stderr, "flow open: %s\n", strerror(-rc));
		return 1;
	}

	x6100_ctl_init(&ctl, &i2c);

	if ((rc = x6100_cal_read(&i2c, &cal)) < 0) {
		fprintf(stderr, "calibration: %s, using defaults\n", strerror(-rc));
		memset(&cal, 0, sizeof(cal));
		cal.bias_drive = X6100_CAL_BIAS_DRIVE_DEF;
		cal.bias_final = X6100_CAL_BIAS_FINAL_DEF;
	}

	uint8_t band = x6100_band_index(freq);

	mode_filter(mode, &low, &high);

	x6100_set_freq(&ctl, 0, freq);
	x6100_set_freq(&ctl, 1, freq);
	x6100_set_mode(&ctl, 0, mode);
	x6100_set_mode(&ctl, 1, mode);
	x6100_ctl_set(&ctl, X6100_REG_FILTER1_LOW, low);
	x6100_ctl_set(&ctl, X6100_REG_FILTER1_HIGH, high);
	x6100_ctl_set(&ctl, X6100_REG_FILTER2_LOW, low);
	x6100_ctl_set(&ctl, X6100_REG_FILTER2_HIGH, high);
	x6100_ctl_set(&ctl, X6100_REG_RXVOL, (uint32_t) rxvol);
	x6100_cal_apply(&ctl, &cal, band);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

	printf("tune %u Hz, mode %s, band slot %u, filter %u-%u Hz, rxvol %d, %d s\n",
		freq, argc > 2 ? argv[2] : "usb", band, low, high, rxvol, seconds);

	if ((rc = x6100_ctl_refresh(&ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	uint64_t start = now_ms();
	uint64_t last_refresh = start;
	uint64_t last_report = start;
	double level = 0.0;
	unsigned level_n = 0;

	while (!g_stop && now_ms() - start < (uint64_t) seconds * 1000) {
		uint64_t t = now_ms();

		//
		// Never transmit
		//
		x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

		rc = t - last_refresh >= REFRESH_MS ? x6100_ctl_refresh(&ctl) : x6100_ctl_sync(&ctl);

		if (t - last_refresh >= REFRESH_MS)
			last_refresh = t;

		if (rc < 0) {
			fprintf(stderr, "i2c: %s\n", strerror(-rc));
			break;
		}

		rc = x6100_flow_read(&flow, &frame);

		if (rc < 0) {
			fprintf(stderr, "flow: %s\n", strerror(-rc));
			break;
		}

		if (rc > 0) {
			frames++;
			level += iq_level_db(&frame);
			level_n++;

			if (frame.flags & X6100_FLOW_TX) {
				fprintf(stderr, "ABORT: baseband reports TX\n");
				break;
			}

			if (frame.hkey != 0)
				printf("hkey %02X\n", frame.hkey);
		}

		if (t - last_report >= REPORT_MS && level_n > 0) {
			printf("t=%5.1fs frames=%4u iq=%6.1f dB swr=%.1f vext=%.1f vbat=%.1f\n",
				(t - start) / 1000.0, frames, level / level_n,
				frame.vswr * 0.1, frame.vext * 0.1, frame.vbat * 0.1);
			level = 0.0;
			level_n = 0;
			last_report = t;
		}

		usleep(LOOP_US);
	}

	x6100_ctl_set(&ctl, X6100_REG_RXVOL, 0);
	x6100_ctl_sync(&ctl);

	printf("%u frames in %.1f s\n", frames, (now_ms() - start) / 1000.0);
	x6100_flow_close(&flow);
	x6100_i2c_close(&i2c);
	return 0;
}
