/*
 * x6100_rx - minimal console receiver driven by the X6100 front panel
 *
 * Uses libx6100 for the baseband and the front panel input devices directly.
 * Status is printed to the console (ssh); the display is not used yet.
 *
 *   VFO knob       tune, step set by FST (10 / 100 / 1000 Hz)
 *   VOL knob       speaker volume, push = mute
 *   MFK knob       RF gain, push = reset to 100
 *   BAND- / BAND+  previous / next band, last frequency per band kept
 *   SSB / CW / AM-FM   USB<->LSB / CW<->CWR / AM<->NFM
 *   PRE            preamp on/off
 *   AGC            off / slow / fast / auto
 *   LOCK           lock knobs and keys (LOCK again to unlock)
 *   PWR            quit (as does Ctrl-C)
 *   PTT            ignored: this tool never transmits
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "x6100_keys.h"

#define FREQ_MIN	500000
#define FREQ_MAX	56000000
#define POLL_MS		5
#define REFRESH_MS	1000
#define METER_MS	500

typedef enum
{
	IN_KEYPAD = 0,
	IN_VFO,
	IN_VOL,
	IN_MFK,
	IN_POWER,
	IN_COUNT
} input_t;

typedef struct
{
	const char *name;
	uint32_t low;
	uint32_t high;
	uint32_t def;
} band_t;

typedef struct
{
	uint32_t freq;
	x6100_mode_t mode;
	x6100_agc_t agc;
	int vol;
	int rfg;
	int step;
	int band;
	bool pre;
	bool mute;
	bool lock;
	uint32_t band_freq[11];
} rx_t;

static const band_t g_bands[] = {
	{ "160m",  1800000,  2000000,  1840000 },
	{ "80m",   3500000,  4000000,  3573000 },
	{ "60m",   5330500,  5405000,  5357000 },
	{ "40m",   7000000,  7300000,  7074000 },
	{ "30m",  10100000, 10150000, 10136000 },
	{ "20m",  14000000, 14350000, 14074000 },
	{ "17m",  18068000, 18168000, 18100000 },
	{ "15m",  21000000, 21450000, 21074000 },
	{ "12m",  24890000, 24990000, 24915000 },
	{ "10m",  28000000, 29700000, 28074000 },
	{ "6m",   50000000, 54000000, 50313000 },
};

#define BAND_COUNT	((int) (sizeof(g_bands) / sizeof(g_bands[0])))

static const char *g_input_names[IN_COUNT] = {
	X6100_INPUT_KEYPAD, X6100_INPUT_VFO, X6100_INPUT_VOL, X6100_INPUT_MFK, X6100_INPUT_POWER,
};

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

//
// Open each front panel input device by its kernel name
//

static int open_inputs(int fds[IN_COUNT])
{
	int found = 0;

	for (int i = 0; i < IN_COUNT; i++)
		fds[i] = -1;

	for (int n = 0; n < 16; n++) {
		char path[32];
		char name[64] = "";
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", n);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd < 0)
			continue;

		ioctl(fd, EVIOCGNAME(sizeof(name)), name);

		for (int i = 0; i < IN_COUNT; i++) {
			if (fds[i] < 0 && strcmp(name, g_input_names[i]) == 0) {
				fds[i] = fd;
				fd = -1;
				found++;
				break;
			}
		}

		if (fd >= 0)
			close(fd);
	}

	return found;
}

static const char *mode_name(x6100_mode_t mode)
{
	static const char *names[] = { "LSB", "LSB-D", "USB", "USB-D", "CW", "CWR", "AM", "NFM" };

	return (unsigned) mode < 8 ? names[mode] : "?";
}

static const char *agc_name(x6100_agc_t agc)
{
	static const char *names[] = { "OFF", "SLOW", "FAST", "AUTO" };

	return (unsigned) agc < 4 ? names[agc] : "?";
}

static int band_of(uint32_t freq)
{
	for (int i = 0; i < BAND_COUNT; i++) {
		if (freq >= g_bands[i].low && freq <= g_bands[i].high)
			return i;
	}

	return -1;
}

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
// Push the receiver state into the register shadow
//

static void apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, const rx_t *rx)
{
	uint32_t low, high;

	mode_filter(rx->mode, &low, &high);

	x6100_set_freq(ctl, 0, rx->freq);
	x6100_set_freq(ctl, 1, rx->freq);
	x6100_set_mode(ctl, 0, rx->mode);
	x6100_set_mode(ctl, 1, rx->mode);
	x6100_ctl_set(ctl, X6100_REG_VFOA_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOB_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOA_PRE, rx->pre);
	x6100_ctl_set(ctl, X6100_REG_VFOB_PRE, rx->pre);
	x6100_ctl_set(ctl, X6100_REG_FILTER1_LOW, low);
	x6100_ctl_set(ctl, X6100_REG_FILTER1_HIGH, high);
	x6100_ctl_set(ctl, X6100_REG_FILTER2_LOW, low);
	x6100_ctl_set(ctl, X6100_REG_FILTER2_HIGH, high);
	x6100_ctl_set(ctl, X6100_REG_RXVOL, rx->mute ? 0 : (uint32_t) rx->vol);
	x6100_ctl_update(ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF, (uint32_t) rx->rfg);
	x6100_cal_apply(ctl, cal, x6100_band_index(rx->freq));
}

static void show(const rx_t *rx)
{
	printf("\r%9.3f kHz %-5s %-4s step %4d  vol %3d%s  rfg %3d  pre %s  agc %-4s%s      ",
		rx->freq / 1000.0, rx->band >= 0 ? g_bands[rx->band].name : "-", mode_name(rx->mode),
		rx->step, rx->vol, rx->mute ? " (mute)" : "", rx->rfg, rx->pre ? "on " : "off",
		agc_name(rx->agc), rx->lock ? "  LOCK" : "");
	fflush(stdout);
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void set_freq(rx_t *rx, int64_t freq)
{
	if (freq < FREQ_MIN)
		freq = FREQ_MIN;

	if (freq > FREQ_MAX)
		freq = FREQ_MAX;

	rx->freq = (uint32_t) freq;
	rx->band = band_of(rx->freq);

	if (rx->band >= 0)
		rx->band_freq[rx->band] = rx->freq;
}

static void band_step(rx_t *rx, int dir)
{
	int band = rx->band;

	if (band < 0) {
		band = 0;

		while (band < BAND_COUNT - 1 && g_bands[band].high < rx->freq)
			band++;

		if (dir < 0 && g_bands[band].low > rx->freq && band > 0)
			band--;
	} else {
		band = (band + dir + BAND_COUNT) % BAND_COUNT;
	}

	set_freq(rx, rx->band_freq[band]);
	rx->mode = rx->freq < 10000000 ? X6100_MODE_LSB : X6100_MODE_USB;
}

//
// Key handling; returns true when the state changed
//

static bool on_key(rx_t *rx, int key)
{
	if (rx->lock && key != X6100_KEY_LOCK)
		return false;

	switch (key) {
	case X6100_KEY_LOCK:
		rx->lock = !rx->lock;
		break;
	case X6100_KEY_FST:
		rx->step = rx->step == 10 ? 100 : rx->step == 100 ? 1000 : 10;
		break;
	case X6100_KEY_BAND_UP:
		band_step(rx, +1);
		break;
	case X6100_KEY_BAND_DOWN:
		band_step(rx, -1);
		break;
	case X6100_KEY_SSB:
		rx->mode = rx->mode == X6100_MODE_USB ? X6100_MODE_LSB : X6100_MODE_USB;
		break;
	case X6100_KEY_CW:
		rx->mode = rx->mode == X6100_MODE_CW ? X6100_MODE_CWR : X6100_MODE_CW;
		break;
	case X6100_KEY_AMFM:
		rx->mode = rx->mode == X6100_MODE_AM ? X6100_MODE_NFM : X6100_MODE_AM;
		break;
	case X6100_KEY_PRE:
		rx->pre = !rx->pre;
		break;
	case X6100_KEY_AGC:
		rx->agc = (x6100_agc_t) ((rx->agc + 1) % 4);
		break;
	case X6100_KEY_VOL_PUSH:
		rx->mute = !rx->mute;
		break;
	case X6100_KEY_MFK_PUSH:
		rx->rfg = 100;
		break;
	default:
		return false;
	}

	return true;
}

static bool on_knob(rx_t *rx, input_t in, int value)
{
	if (rx->lock)
		return false;

	switch (in) {
	case IN_VFO:
		set_freq(rx, (int64_t) rx->freq - rx->freq % (uint32_t) rx->step + (int64_t) value * X6100_DIR_VFO * rx->step);
		return true;
	case IN_VOL:
		rx->vol = clampi(rx->vol + value * X6100_DIR_VOL, 0, 100);
		return true;
	case IN_MFK:
		rx->rfg = clampi(rx->rfg + value * X6100_DIR_MFK * 2, 0, 100);
		return true;
	default:
		return false;
	}
}

static double iq_level_db(const x6100_flow_frame_t *f)
{
	double sum = 0.0;

	for (int i = 0; i < X6100_FLOW_SAMPLES; i++)
		sum += (double) f->iq[i][0] * f->iq[i][0] + (double) f->iq[i][1] * f->iq[i][1];

	sum /= X6100_FLOW_SAMPLES;
	return sum > 0.0 ? 10.0 * log10(sum) : -999.0;
}

int main(void)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	int fds[IN_COUNT];
	struct pollfd pfd[IN_COUNT];
	int npfd = 0;
	input_t pmap[IN_COUNT];
	rx_t rx = { .vol = 20, .rfg = 100, .step = 100, .agc = X6100_AGC_AUTO, .mode = X6100_MODE_USB };
	int rc;

	for (int i = 0; i < BAND_COUNT; i++)
		rx.band_freq[i] = g_bands[i].def;

	set_freq(&rx, 14074000);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (open_inputs(fds) < IN_COUNT)
		fprintf(stderr, "warning: not all front panel devices found\n");

	for (int i = 0; i < IN_COUNT; i++) {
		if (fds[i] < 0)
			continue;

		pfd[npfd].fd = fds[i];
		pfd[npfd].events = POLLIN;
		pmap[npfd] = (input_t) i;
		npfd++;
	}

	if ((rc = x6100_i2c_open(&i2c, NULL)) < 0 || (rc = x6100_flow_open(&flow, NULL)) < 0) {
		fprintf(stderr, "open: %s\n", strerror(-rc));
		return 1;
	}

	if (x6100_cal_read(&i2c, &cal) < 0) {
		fprintf(stderr, "warning: calibration invalid, using defaults\n");
		memset(&cal, 0, sizeof(cal));
		cal.bias_drive = X6100_CAL_BIAS_DRIVE_DEF;
		cal.bias_final = X6100_CAL_BIAS_FINAL_DEF;
	}

	x6100_ctl_init(&ctl, &i2c);
	apply(&ctl, &cal, &rx);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

	if ((rc = x6100_ctl_refresh(&ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	printf("x6100_rx: PWR or Ctrl-C to quit\n");
	show(&rx);

	uint64_t last_refresh = now_ms();
	uint64_t last_meter = last_refresh;
	double level = 0.0;
	int level_n = 0;

	while (!g_stop) {
		bool changed = false;

		if (poll(pfd, (nfds_t) npfd, POLL_MS) < 0 && errno != EINTR)
			break;

		for (int p = 0; p < npfd; p++) {
			struct input_event ev;

			if (!(pfd[p].revents & POLLIN))
				continue;

			while (read(pfd[p].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
				if (pmap[p] == IN_POWER && ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
					g_stop = 1;
				else if (pmap[p] == IN_KEYPAD && ev.type == EV_KEY && ev.value == 1)
					changed |= on_key(&rx, (int) ev.code - X6100_KEY_BASE);
				else if (ev.type == EV_REL)
					changed |= on_knob(&rx, pmap[p], ev.value);
			}
		}

		if (changed) {
			apply(&ctl, &cal, &rx);
			show(&rx);
		}

		//
		// Never transmit
		//
		x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

		uint64_t t = now_ms();

		if (t - last_refresh >= REFRESH_MS) {
			rc = x6100_ctl_refresh(&ctl);
			last_refresh = t;
		} else {
			rc = x6100_ctl_sync(&ctl);
		}

		if (rc < 0) {
			fprintf(stderr, "\ni2c: %s\n", strerror(-rc));
			break;
		}

		while ((rc = x6100_flow_read(&flow, &frame)) > 0) {
			if (frame.flags & X6100_FLOW_TX) {
				fprintf(stderr, "\nABORT: baseband reports TX\n");
				g_stop = 1;
				break;
			}

			level += iq_level_db(&frame);
			level_n++;
		}

		if (t - last_meter >= METER_MS && level_n > 0) {
			show(&rx);
			printf("iq %6.1f dB  vbat %.1f", level / level_n, frame.vbat * 0.1);
			fflush(stdout);
			level = 0.0;
			level_n = 0;
			last_meter = t;
		}
	}

	x6100_ctl_set(&ctl, X6100_REG_RXVOL, 0);
	x6100_ctl_sync(&ctl);

	printf("\n");
	x6100_flow_close(&flow);
	x6100_i2c_close(&i2c);

	for (int i = 0; i < IN_COUNT; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}

	return 0;
}
