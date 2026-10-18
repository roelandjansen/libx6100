/*
 * x6100_info - read-only check of the X6100 baseband via libx6100
 *
 * Prints the BASE version, validates the calibration block, and decodes
 * flow frames for a few seconds. Never writes registers.
 * Stop the stock UI first.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FLOW_SECONDS	3

static const char *band_name(uint8_t band)
{
	static const char *names[] = {
		"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m",
	};

	return (band & 1) && band / 2 < sizeof(names) / sizeof(names[0]) ? names[band / 2] : "-";
}

static void show_cal(const x6100_cal_t *cal)
{
	printf("bias drive %d, final %d\n", cal->bias_drive, cal->bias_final);
	printf("slot band  txiofs txqofs rmagcmp rphscmp\n");

	for (uint8_t b = 0; b < X6100_CAL_BANDS; b++) {
		const x6100_cal_tx_t *tx = &cal->tx[b];
		const x6100_cal_rx_t *rx = &cal->rx[b];

		if (tx->txiofs == 0 && tx->txqofs == 0 && rx->rmagcmp == 0 && rx->rphscmp == 0)
			continue;

		printf("%4u %-5s %6d %6d %7d %7d\n", b, band_name(b), tx->txiofs, tx->txqofs, rx->rmagcmp, rx->rphscmp);
	}
}

static void show_flow(void)
{
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	time_t end = time(NULL) + FLOW_SECONDS;
	unsigned frames = 0;

	if (x6100_flow_open(&flow, NULL) < 0) {
		perror("flow open");
		return;
	}

	while (time(NULL) < end) {
		int rc = x6100_flow_read(&flow, &frame);

		if (rc < 0) {
			perror("flow read");
			break;
		}

		if (rc == 0) {
			usleep(5000);
			continue;
		}

		if (frames++ % 30 == 0)
			printf("tx=%u pwr=%.1f swr=%.1f alc=%.1f vext=%.1f vbat=%.1f bat=%u%% hkey=%02X\n",
				!!(frame.flags & X6100_FLOW_TX), frame.tx_power * 0.1, frame.vswr * 0.1,
				frame.alc_level * 0.1, frame.vext * 0.1, frame.vbat * 0.1, frame.batcap, frame.hkey);
	}

	printf("%u valid frames in %d s\n", frames, FLOW_SECONDS);
	x6100_flow_close(&flow);
}

int main(void)
{
	x6100_i2c_t i2c;
	x6100_cal_t cal;
	char ver[X6100_RORAM_LEN];
	int rc;

	if ((rc = x6100_i2c_open(&i2c, NULL)) < 0) {
		fprintf(stderr, "i2c open: %s\n", strerror(-rc));
		return 1;
	}

	if ((rc = x6100_base_version(&i2c, ver, sizeof(ver))) < 0)
		fprintf(stderr, "version: %s\n", strerror(-rc));
	else
		printf("BASE %s\n", ver);

	rc = x6100_cal_read(&i2c, &cal);
	printf("calibration: %s\n", rc == 0 ? "valid" : strerror(-rc));

	if (rc == 0)
		show_cal(&cal);

	x6100_i2c_close(&i2c);
	show_flow();
	return 0;
}
