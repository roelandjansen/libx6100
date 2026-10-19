/*
 * x6100_rx - minimal console receiver driven by the X6100 front panel
 *
 * Uses libx6100 for the baseband and the front panel input devices directly.
 * Status on the display (frequency, mode, IQ meter, spectrum, waterfall)
 * and on the console (ssh).
 *
 *   VFO knob       tune, step set by FST (10 / 100 / 1000 / 5000 Hz);
 *                  long press FST: 1 Hz step on/off
 *   VOL knob       speaker volume, push = mute
 *   MFK knob       RF gain, push = reset to 100
 *   BAND- / BAND+  previous / next band, last frequency per band kept
 *   SSB / CW / AM-FM   USB<->LSB / CW<->CWR / AM<->NFM
 *   PRE            preamp on/off; long press: attenuator on/off
 *   AGC            off / slow / fast / auto
 *   LOCK           lock knobs and keys (LOCK again to unlock)
 *   PWR            quit (as does Ctrl-C)
 *   PTT            ignored: this tool never transmits
 *
 * Testing from the console (ssh):
 *   x6100_rx [freq] [mode]            start values, e.g. x6100_rx 7074000 lsb
 *   type a line + Enter while running:
 *     7074000 | 7.074 | 7074k         frequency (Hz, MHz with '.', or kHz with 'k')
 *     usb lsb cw cwr am nfm           mode
 *     step 1|10|100|1000|5000         tuning step
 *   tokens can be combined: "14074000 usb"
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "x6100_keys.h"
#include "x6100_lcd.h"

#define FREQ_MIN	500000
#define FREQ_MAX	56000000
#define POLL_MS		5
#define LONG_PRESS_MS	600
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
	bool att;
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
	x6100_ctl_set(ctl, X6100_REG_VFOA_ATT, rx->att);
	x6100_ctl_set(ctl, X6100_REG_VFOB_ATT, rx->att);
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
		rx->step = rx->step == 10 ? 100 : rx->step == 100 ? 1000 : rx->step == 1000 ? 5000 : 10;
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

//
// Long press: keys with a long function act on release (short) or after
// LONG_PRESS_MS held (long); all other keys act immediately on press
//

typedef struct
{
	int key;
	uint64_t since;
	bool fired;
} hold_t;

static bool has_long(int key)
{
	return key == X6100_KEY_PRE || key == X6100_KEY_FST;
}

static bool on_long_key(rx_t *rx, int key)
{
	if (rx->lock)
		return false;

	switch (key) {
	case X6100_KEY_PRE:
		rx->att = !rx->att;
		return true;
	case X6100_KEY_FST:
		rx->step = rx->step == 1 ? 10 : 1;
		return true;
	default:
		return false;
	}
}

static bool on_keypad(rx_t *rx, hold_t *hold, int key, int value)
{
	if (!has_long(key))
		return value == 1 ? on_key(rx, key) : false;

	if (value == 1) {
		hold->key = key;
		hold->since = now_ms();
		hold->fired = false;
		return false;
	}

	if (value == 0 && hold->key == key) {
		bool short_press = !hold->fired;

		hold->key = -1;
		return short_press ? on_key(rx, key) : false;
	}

	return false;
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

//
// Screen: 800x480 landscape
//

#define SCR_BG		0x0A0E14
#define SCR_FG		0xE6E6E6
#define SCR_DIM		0x7A8494
#define SCR_FREQ	0xF4F4F4
#define SCR_ACCENT	0x40C0FF
#define SCR_WARN	0xFF6040

//
// Scope colours, as set on the IC-7851 (Waveform Fill+Line, averaging 2)
//
#define SCOPE_FILL	0xFF3F7F
#define SCOPE_LINE	0xFFFF00
#define SCOPE_PEAK	0x007F7F
#define SCOPE_AVG	0.5f
#define PEAK_DECAY_DB	0.3f
#define WF_RANGE_DB	50.0f
#define WF_BELOW_DB	3.0f
#define LBL_W		28

#define TOP_Y		8
#define LEFT_W		492
#define FREQ_X		8
#define FREQ_Y		40
#define FREQ_SCALE	8	/* 5x7 fallback font scale, unused with the AA font */
#define INFO_Y		104
#define INFO_COL_STEP	12
#define INFO_COL_VOL	138
#define INFO_COL_RFG	252
#define INFO_COL_VBAT	378
#define SBAR_X		34
#define SBAR_W		410
#define SBAR_LBL_Y	121
#define SBAR_SCALE_Y	141
#define SBAR_BAR_Y	144
#define SBAR_BAR_H	6
#define SBAR_SWR_Y	153
#define SBAR_SWR_LBL_Y	158
#define TOP_H		176
#define VU_X		500
#define VU_Y		4
#define VU_W		296
#define VU_H		168
#define SPEC_Y		180
#define SPEC_H		150
#define WF_Y		(SPEC_Y + SPEC_H + 4)
#define DISP_MS		70
#define SPEC_DB_MIN	-130.0f
#define SPEC_DB_MAX	-30.0f
#define SPAN_HZ		96000
#define GRID_HZ		10000
#define GRID_DB		10

//
// Meter calibration: dBm = passband power in dB + SMETER_CAL_DB.
// Provisional -15 dB from an FT8 comparison with an IC-7851; refine with a steady carrier.
//
#define SMETER_CAL_DB	-15.0
#define VU_ATTACK_MS	40.0
#define VU_RELEASE_MS	400.0
#define VU_ANGLE	33.0
#define VU_PEAK_HOLD_MS	1500.0
#define VU_PEAK_FALL	0.0006

//
// Meter marker dot: rides on the arc of the active meter scale.
// RX: peak hold on the S arc. TX (later): Po or ALC arc.
//
typedef enum
{
	VU_SCALE_S = 0,
	VU_SCALE_PO,
	VU_SCALE_ALC
} vu_scale_t;

typedef struct
{
	bool on;
	x6100_lcd_t lcd;
	float spec[X6100_FLOW_SAMPLES];
	float peak[X6100_FLOW_SAMPLES];
	float floor_db;
	float pow[X6100_FLOW_SAMPLES];
	bool spec_valid;
	double meter_db;
	double sig_dbm;
	double needle;
	uint64_t needle_ms;
	double vu_peak;
	uint64_t vu_peak_ms;
	double vbat;
} screen_t;

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
		float ang = -2.0f * (float) M_PI / (float) len;
		float wr = cosf(ang), wi = sinf(ang);

		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;

			for (int k = 0; k < len / 2; k++) {
				float *a = v[i + k], *b = v[i + k + len / 2];
				float br = b[0] * cr - b[1] * ci;
				float bi = b[0] * ci + b[1] * cr;
				float t = cr * wr - ci * wi;

				b[0] = a[0] - br;
				b[1] = a[1] - bi;
				a[0] += br;
				a[1] += bi;
				ci = cr * wi + ci * wr;
				cr = t;
			}
		}
	}
}

//
// Windowed FFT of one flow frame, centred, exponentially averaged in dB
//

static void spectrum_update(screen_t *scr, const x6100_flow_frame_t *f)
{
	static float buf[X6100_FLOW_SAMPLES][2];
	const int n = X6100_FLOW_SAMPLES;

	for (int i = 0; i < n; i++) {
		float w = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (float) i / (float) (n - 1));

		buf[i][0] = f->iq[i][0] * w;
		buf[i][1] = f->iq[i][1] * w;
	}

	fft(buf, n);

	for (int i = 0; i < n; i++) {
		int k = (i + n / 2) % n;
		float p = buf[k][0] * buf[k][0] + buf[k][1] * buf[k][1];
		float db = p > 1e-20f ? 10.0f * log10f(p / (float) n) : -200.0f;

		scr->pow[i] = p / (float) n;

		scr->spec[i] = scr->spec_valid ? scr->spec[i] * (1.0f - SCOPE_AVG) + db * SCOPE_AVG : db;
		scr->peak[i] = scr->spec_valid && scr->peak[i] - PEAK_DECAY_DB > scr->spec[i]
			? scr->peak[i] - PEAK_DECAY_DB : scr->spec[i];
	}

	//
	// Noise floor: 20th percentile of the spectrum, smoothed
	//
	static float sorted[X6100_FLOW_SAMPLES];

	memcpy(sorted, scr->spec, sizeof(sorted));

	for (int i = 1; i < n; i++) {
		float v = sorted[i];
		int j = i - 1;

		while (j >= 0 && sorted[j] > v) {
			sorted[j + 1] = sorted[j];
			j--;
		}

		sorted[j + 1] = v;
	}

	float fl = sorted[n / 5];

	scr->floor_db = scr->spec_valid ? scr->floor_db * 0.95f + fl * 0.05f : fl;
	scr->spec_valid = true;
}

static uint32_t wf_colour(float t)
{
	static const struct { float t; uint32_t c; } stops[] = {
		{ 0.00f, 0x000018 }, { 0.20f, 0x00148C }, { 0.40f, 0x1E50F0 }, { 0.55f, 0x00B4FF },
		{ 0.70f, 0x30F060 }, { 0.82f, 0xFFF000 }, { 0.92f, 0xFF4000 }, { 1.00f, 0xFFFFFF },
	};

	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;

	for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); i++) {
		if (t > stops[i].t)
			continue;

		float k = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
		uint32_t a = stops[i - 1].c, b = stops[i].c;
		uint32_t r = (uint32_t) ((float) (a >> 16) + k * ((float) (b >> 16) - (float) (a >> 16)));
		uint32_t g = (uint32_t) ((float) (a >> 8 & 0xFF) + k * ((float) (b >> 8 & 0xFF) - (float) (a >> 8 & 0xFF)));
		uint32_t bl = (uint32_t) ((float) (a & 0xFF) + k * ((float) (b & 0xFF) - (float) (a & 0xFF)));

		return r << 16 | g << 8 | bl;
	}

	return stops[sizeof(stops) / sizeof(stops[0]) - 1].c;
}


//
// Spectrum value under screen column x, honouring the direction setting
//

static float spec_at(const screen_t *scr, int x, int w)
{
	int i = x * X6100_FLOW_SAMPLES / w;

	return scr->spec[X6100_FLOW_IQ_MIRRORED ? X6100_FLOW_SAMPLES - 1 - i : i];
}

static int hz_to_x(int hz, int w)
{
	return w / 2 + (int) ((int64_t) hz * w / SPAN_HZ);
}

//
// Receive passband relative to the dial frequency, in Hz
//

static void passband(const rx_t *rx, int *lo, int *hi)
{
	uint32_t low, high;

	mode_filter(rx->mode, &low, &high);

	switch (rx->mode) {
	case X6100_MODE_LSB:
	case X6100_MODE_LSB_DIG:
	case X6100_MODE_CWR:
		*lo = -(int) high;
		*hi = -(int) low;
		break;
	case X6100_MODE_AM:
	case X6100_MODE_NFM:
		*lo = -(int) high;
		*hi = (int) high;
		break;
	default:
		*lo = (int) low;
		*hi = (int) high;
		break;
	}
}

static float spec_norm(float db);

static int spec_y(float db)
{
	float v = spec_norm(db);

	v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	return SPEC_Y + SPEC_H - 1 - (int) (v * (float) (SPEC_H - 1));
}

static float peak_at(const screen_t *scr, int x, int w)
{
	int i = x * X6100_FLOW_SAMPLES / w;

	return scr->peak[X6100_FLOW_IQ_MIRRORED ? X6100_FLOW_SAMPLES - 1 - i : i];
}

static float spec_norm(float db)
{
	return (db - SPEC_DB_MIN) / (SPEC_DB_MAX - SPEC_DB_MIN);
}

//
// Power inside the receive passband, from the last frame, in dBm (calibrated by SMETER_CAL_DB)
//

static double passband_dbm(const screen_t *scr, const rx_t *rx)
{
	const double bw = (double) SPAN_HZ / X6100_FLOW_SAMPLES;
	const int n = X6100_FLOW_SAMPLES;
	int lo, hi;
	double sum = 0.0;

	passband(rx, &lo, &hi);

	for (int hz = lo; hz <= hi; hz += (int) bw) {
		int off = (int) lround(hz / bw);
		int i = n / 2 + (X6100_FLOW_IQ_MIRRORED ? -off : off);

		if (i >= 0 && i < n)
			sum += scr->pow[i];
	}

	return (sum > 1e-20 ? 10.0 * log10(sum) : -200.0) + SMETER_CAL_DB;
}

//
// Meter position 0..1: S0..S9 over the first 60 %, S9..S9+60 over the rest
//

static double meter_frac(double dbm)
{
	if (dbm <= -73.0) {
		double su = (dbm + 127.0) / 6.0;

		return su < 0.0 ? 0.0 : su / 9.0 * 0.6;
	}

	double over = dbm + 73.0;

	return 0.6 + (over > 60.0 ? 60.0 : over) / 60.0 * 0.4;
}

static void s_reading(double dbm, char *buf, size_t len)
{
	if (dbm <= -73.0) {
		int su = (int) ((dbm + 127.0) / 6.0 + 0.5);

		snprintf(buf, len, "S%d", su < 0 ? 0 : su);
	} else {
		snprintf(buf, len, "S9+%d", (int) (dbm + 73.0 + 0.5));
	}
}

//
// Bar S-meter with scale under the frequency
//

static void draw_sbar(x6100_lcd_t *lcd, double dbm, double peak)
{
	static const struct { double frac; const char *lbl; } s_marks[] = {
		{ 1.0 / 9 * 0.6, "1" }, { 3.0 / 9 * 0.6, "3" }, { 5.0 / 9 * 0.6, "5" },
		{ 7.0 / 9 * 0.6, "7" }, { 0.6, "9" },
	};
	static const struct { double frac; const char *lbl; } db_marks[] = {
		{ 0.6 + 20.0 / 60 * 0.4, "+20" }, { 0.6 + 40.0 / 60 * 0.4, "+40" }, { 1.0, "+60dB" },
	};
	static const struct { double frac; const char *lbl; } swr_marks[] = {
		{ 0.22, "1.5" }, { 0.38, "2" }, { 0.49, "2.5" }, { 0.6, "3" }, { 0.97, "INF" },
	};
	const uint32_t white = 0xF0F0F0, red = 0xE02828, grey = 0x9A9A9A;
	const int x0 = SBAR_X, x9 = SBAR_X + (int) (0.6 * SBAR_W), x1 = SBAR_X + SBAR_W;
	int fill = (int) (meter_frac(dbm) * SBAR_W);
	int pk = SBAR_X + (int) (peak * SBAR_W);

	x6100_lcd_fill(lcd, 0, SBAR_LBL_Y - 2, LEFT_W, SBAR_SWR_LBL_Y + 10 - SBAR_LBL_Y, 0x000000);

	//
	// S scale: white to S9, heavy red above; ticks every S unit and every 10 dB
	//
	x6100_lcd_text(lcd, 12, SBAR_LBL_Y + 4, 2, "S", white);
	x6100_lcd_hline(lcd, x0, x9, SBAR_SCALE_Y, white);
	x6100_lcd_fill(lcd, x9, SBAR_SCALE_Y - 1, x1 - x9 + 1, 3, red);

	for (int su = 1; su <= 9; su++) {
		int x = x0 + (int) (su / 9.0 * 0.6 * SBAR_W);

		x6100_lcd_vline(lcd, x, SBAR_SCALE_Y - (su % 2 ? 6 : 3), SBAR_SCALE_Y, white);
	}

	for (int db = 10; db <= 60; db += 10) {
		int x = x0 + (int) ((0.6 + db / 60.0 * 0.4) * SBAR_W);

		x6100_lcd_vline(lcd, x, SBAR_SCALE_Y - (db % 20 ? 3 : 6), SBAR_SCALE_Y, red);
	}

	for (size_t i = 0; i < sizeof(s_marks) / sizeof(s_marks[0]); i++) {
		int x = x0 + (int) (s_marks[i].frac * SBAR_W);

		x6100_lcd_text(lcd, x - x6100_lcd_text_width(s_marks[i].lbl, 2) / 2, SBAR_LBL_Y, 2, s_marks[i].lbl, white);
	}

	for (size_t i = 0; i < sizeof(db_marks) / sizeof(db_marks[0]); i++) {
		int x = x0 + (int) (db_marks[i].frac * SBAR_W);
		int tw = x6100_lcd_text_width(db_marks[i].lbl, 2);

		x = i + 1 == sizeof(db_marks) / sizeof(db_marks[0]) ? x - 24 : x - tw / 2;
		x6100_lcd_text(lcd, x, SBAR_LBL_Y, 2, db_marks[i].lbl, red);
	}

	//
	// Level bar between the scales, with peak hold marker
	//
	x6100_lcd_fill(lcd, x0, SBAR_BAR_Y, fill, SBAR_BAR_H, white);
	x6100_lcd_fill(lcd, pk - 1, SBAR_BAR_Y - 1, 3, SBAR_BAR_H + 2, 0x58B8F0);
	x6100_lcd_vline(lcd, x0, SBAR_SCALE_Y - 8, SBAR_SWR_Y + 3, white);

	//
	// SWR scale below: white to 3, double line above
	//
	x6100_lcd_text(lcd, 4, SBAR_SWR_Y - 2, 1, "SWR", grey);
	x6100_lcd_hline(lcd, x0, x1, SBAR_SWR_Y, white);
	x6100_lcd_hline(lcd, x0 + (int) (0.6 * SBAR_W), x1, SBAR_SWR_Y + 3, white);

	for (int i = 0; i <= 12; i++)
		x6100_lcd_vline(lcd, x0 + (int) (i / 12.0 * 0.6 * SBAR_W), SBAR_SWR_Y, SBAR_SWR_Y + (i % 4 ? 2 : 4), white);

	for (size_t i = 0; i < sizeof(swr_marks) / sizeof(swr_marks[0]); i++) {
		int x = x0 + (int) (swr_marks[i].frac * SBAR_W);

		x6100_lcd_vline(lcd, x, SBAR_SWR_Y, SBAR_SWR_Y + 4, white);
		x6100_lcd_text(lcd, x - x6100_lcd_text_width(swr_marks[i].lbl, 1) / 2, SBAR_SWR_LBL_Y, 1, swr_marks[i].lbl, white);
	}
}

//
// Multi-scale transceiver meter (Icom style): S / Po, SWR, COMP, ALC on
// concentric arcs around a pivot below the visible area, white needle
//

#define VU_CX		(VU_X + VU_W / 2 + 6)
#define VU_CY		(VU_Y + VU_H + 95)
#define VU_R_S		205.0
#define VU_R_SWR	172.0
#define VU_R_COMP	146.0
#define VU_R_ALC	138.0

static void vu_point(double frac, double r, int *x, int *y)
{
	double a = (-VU_ANGLE + 2.0 * VU_ANGLE * frac) * M_PI / 180.0;

	*x = VU_CX + (int) lround(r * sin(a));
	*y = VU_CY - (int) lround(r * cos(a));
}

static void vu_arc(x6100_lcd_t *lcd, double f0, double f1, double r, int width, uint32_t rgb)
{
	int x, y;

	for (int i = 0; i <= 600; i++) {
		double f = f0 + (f1 - f0) * i / 600.0;

		vu_point(f, r, &x, &y);
		x6100_lcd_fill(lcd, x - width / 2, y - width / 2, width, width, rgb);
	}
}

static void vu_tick(x6100_lcd_t *lcd, double f, double r0, double r1, uint32_t rgb)
{
	int x0, y0, x1, y1;

	vu_point(f, r0, &x0, &y0);
	vu_point(f, r1, &x1, &y1);
	x6100_lcd_line(lcd, x0, y0, x1, y1, 2, rgb);
}

static void vu_label(x6100_lcd_t *lcd, double f, double r, int scale, const char *s, uint32_t rgb)
{
	int x, y;

	vu_point(f, r, &x, &y);
	x6100_lcd_text(lcd, x - x6100_lcd_text_width(s, scale) / 2, y - 7 * scale / 2, scale, s, rgb);
}

static double vu_scale_radius(vu_scale_t scale);

static void draw_vu(x6100_lcd_t *lcd, double needle, double dbm, double marker, vu_scale_t scale)
{
	static const struct { double f; const char *s; } po[] = {
		{ 0.2, "2" }, { 0.4, "4" }, { 0.6, "6" }, { 0.8, "8" }, { 0.97, "10W" },
	};
	static const struct { double f; const char *s; } swr[] = {
		{ 0.0, "1" }, { 0.22, "1.5" }, { 0.38, "2" }, { 0.56, "3" }, { 0.76, "5" }, { 0.97, "INF" },
	};
	static const struct { double f; const char *s; } comp[] = {
		{ 0.10, "5" }, { 0.20, "10" }, { 0.32, "20" }, { 0.44, "30dB" },
	};
	const uint32_t bg = 0x000000, white = 0xF0F0F0, grey = 0x9A9A9A;
	const uint32_t red = 0xE02828, blue = 0x2E5FD0, pivot = 0xFFFFFF, mark = 0x58B8F0;
	char s[32];
	int x0, y0, x1, y1;

	x6100_lcd_fill(lcd, VU_X, VU_Y, VU_W, VU_H, bg);

	//
	// S scale: white to S9, heavy red above; Po labels under it
	//
	vu_arc(lcd, 0.0, 0.6, VU_R_S, 2, white);
	vu_arc(lcd, 0.6, 1.0, VU_R_S + 2, 5, red);

	for (int su = 1; su <= 9; su++) {
		double f = su / 9.0 * 0.6;

		vu_tick(lcd, f, VU_R_S, VU_R_S + (su % 2 ? 9 : 5), white);

		if (su % 2) {
			snprintf(s, sizeof(s), "%d", su);
			vu_label(lcd, f, VU_R_S + 20, 2, s, white);
		}
	}

	for (int db = 20; db <= 60; db += 20) {
		snprintf(s, sizeof(s), "+%d", db);
		vu_label(lcd, 0.6 + db / 60.0 * 0.4, VU_R_S + 20, 2, s, red);
	}

	vu_label(lcd, 0.0, VU_R_S + 20, 2, "S", white);

	for (size_t i = 0; i < sizeof(po) / sizeof(po[0]); i++) {
		vu_tick(lcd, po[i].f, VU_R_S - 1, VU_R_S - 5, grey);
		vu_label(lcd, po[i].f, VU_R_S - 13, 1, po[i].s, grey);
	}

	//
	// SWR scale
	//
	vu_arc(lcd, 0.0, 1.0, VU_R_SWR, 2, grey);

	for (size_t i = 0; i < sizeof(swr) / sizeof(swr[0]); i++) {
		vu_tick(lcd, swr[i].f, VU_R_SWR, VU_R_SWR + 5, grey);
		vu_label(lcd, swr[i].f, VU_R_SWR + 13, 1, swr[i].s, grey);
	}

	//
	// COMP (blue) and ALC (red) partial arcs
	//
	vu_arc(lcd, 0.04, 0.46, VU_R_COMP, 3, blue);

	for (size_t i = 0; i < sizeof(comp) / sizeof(comp[0]); i++)
		vu_label(lcd, comp[i].f, VU_R_COMP + 11, 1, comp[i].s, blue);

	vu_arc(lcd, 0.0, 0.34, VU_R_ALC, 4, red);

	//
	// Scale names at the left
	//
	vu_label(lcd, -0.08, VU_R_S - 13, 1, "PO", grey);
	vu_label(lcd, -0.08, VU_R_SWR + 2, 1, "SWR", grey);
	vu_label(lcd, -0.05, VU_R_COMP + 2, 1, "COMP", blue);
	vu_label(lcd, 0.17, VU_R_ALC - 10, 1, "ALC", red);

	//
	// Reading
	//
	s_reading(dbm, s, sizeof(s));
	x6100_lcd_text(lcd, VU_X + VU_W - 8 - x6100_lcd_text_width(s, 2), VU_Y + VU_H - 38, 2, s, white);
	snprintf(s, sizeof(s), "%.0fdBm", dbm);
	x6100_lcd_text(lcd, VU_X + VU_W - 8 - x6100_lcd_text_width(s, 2), VU_Y + VU_H - 20, 2, s, grey);

	//
	// Needle from the bottom edge of the face, with pivot dot
	//
	double a = (-VU_ANGLE + 2.0 * VU_ANGLE * needle) * M_PI / 180.0;
	double r_bot = (double) (VU_CY - (VU_Y + VU_H - 3)) / cos(a);

	vu_point(needle, r_bot, &x0, &y0);
	vu_point(needle, VU_R_S + 8, &x1, &y1);
	x6100_lcd_line(lcd, x0, y0, x1, y1, 2, white);
	x6100_lcd_fill(lcd, x0 - 5, y0 - 5, 10, 10, pivot);

	//
	// Marker dot on the active scale's arc
	//
	vu_point(marker, vu_scale_radius(scale), &x0, &y0);
	x6100_lcd_fill(lcd, x0 - 4, y0 - 4, 9, 9, 0x000000);
	x6100_lcd_fill(lcd, x0 - 3, y0 - 3, 7, 7, mark);

	//
	// Keep the face clipped: clear anything drawn below it
	//
	x6100_lcd_fill(lcd, VU_X, VU_Y + VU_H, VU_W, TOP_H - VU_Y - VU_H, SCR_BG);
}

//
// Needle ballistics: fast attack, slow release
//

static double vu_scale_radius(vu_scale_t scale)
{
	switch (scale) {
	case VU_SCALE_PO:
		return VU_R_S - 3;
	case VU_SCALE_ALC:
		return VU_R_ALC;
	default:
		return VU_R_S;
	}
}

static void vu_update(screen_t *scr, double target, uint64_t t)
{
	double dt = scr->needle_ms ? (double) (t - scr->needle_ms) : 0.0;
	double tau = target > scr->needle ? VU_ATTACK_MS : VU_RELEASE_MS;
	double k = dt / tau;

	scr->needle += (target - scr->needle) * (k > 1.0 ? 1.0 : k);
	scr->needle_ms = t;

	//
	// Peak hold: jump up with the needle, hold, then fall slowly
	//
	if (scr->needle >= scr->vu_peak) {
		scr->vu_peak = scr->needle;
		scr->vu_peak_ms = t;
	} else if ((double) (t - scr->vu_peak_ms) > VU_PEAK_HOLD_MS) {
		scr->vu_peak -= VU_PEAK_FALL * dt;

		if (scr->vu_peak < scr->needle)
			scr->vu_peak = scr->needle;
	}
}

static void draw_screen(screen_t *scr, const rx_t *rx, bool new_line)
{
	x6100_lcd_t *lcd = &scr->lcd;
	int W = lcd->w;
	char s[64];

	//
	// Status line
	//
	x6100_lcd_fill(lcd, 0, 0, W, TOP_H, SCR_BG);
	snprintf(s, sizeof(s), "%4s %5s AGC %4s", rx->band >= 0 ? g_bands[rx->band].name : "GEN",
		mode_name(rx->mode), agc_name(rx->agc));
	x6100_lcd_text(lcd, 12, TOP_Y, 3, s, SCR_ACCENT);

	//
	// Indicators in fixed slots, two rows, so nothing shifts when one changes
	//
	{
		static const struct { int x, y; } slot[4] = {
			{ 372, 2 }, { 432, 2 }, { 372, 18 }, { 432, 18 },
		};
		const char *txt[4] = {
			"", rx->att ? "ATT" : "",
			rx->pre ? "PRE" : "", rx->lock ? "LOCK" : "",
		};

		for (int i = 0; i < 4; i++)
			x6100_lcd_text(lcd, slot[i].x, slot[i].y, 2, txt[i], SCR_WARN);
	}

	//
	// Frequency, MHz.kHz.Hz
	//
	snprintf(s, sizeof(s), "%u.%03u.%03u", rx->freq / 1000000, rx->freq / 1000 % 1000, rx->freq % 1000);
	x6100_lcd_text_aa(lcd, &x6100_font_freq, FREQ_X + (480 - x6100_lcd_text_aa_width(&x6100_font_freq, s)),
		FREQ_Y - 17, s, SCR_FREQ);

	//
	// Info line in fixed columns, so changing values never shift the other fields
	//
	snprintf(s, sizeof(s), "STEP %4d", rx->step);
	x6100_lcd_text(lcd, INFO_COL_STEP, INFO_Y, 2, s, SCR_DIM);
	snprintf(s, sizeof(s), "VOL %d", rx->vol);
	x6100_lcd_text(lcd, INFO_COL_VOL, INFO_Y, 2, s, rx->mute ? SCR_WARN : SCR_DIM);
	snprintf(s, sizeof(s), "RFG %d", rx->rfg);
	x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_DIM);
	snprintf(s, sizeof(s), "%.1fV", scr->vbat);
	x6100_lcd_text(lcd, INFO_COL_VBAT, INFO_Y, 2, s, SCR_DIM);

	//
	// Meters
	//
	draw_sbar(lcd, scr->sig_dbm, scr->vu_peak);
	vu_update(scr, meter_frac(scr->sig_dbm), now_ms());
	draw_vu(lcd, scr->needle, scr->sig_dbm, scr->vu_peak, VU_SCALE_S);

	if (!scr->spec_valid)
		return;

	//
	// Spectrum
	//
	x6100_lcd_fill(lcd, 0, SPEC_Y, W, SPEC_H, 0x05080C);

	int pb_lo, pb_hi;

	passband(rx, &pb_lo, &pb_hi);
	x6100_lcd_fill(lcd, hz_to_x(pb_lo, W), SPEC_Y, hz_to_x(pb_hi, W) - hz_to_x(pb_lo, W) + 1, SPEC_H, 0x183820);

	for (int hz = -GRID_HZ * 4; hz <= GRID_HZ * 4; hz += GRID_HZ) {
		char lbl[8];
		int gx = hz_to_x(hz, W);

		x6100_lcd_vline(lcd, gx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0x1A2430);

		if (hz == 0)
			continue;

		snprintf(lbl, sizeof(lbl), "%+d", hz / 1000);
		x6100_lcd_text(lcd, gx - x6100_lcd_text_width(lbl, 2) / 2, SPEC_Y + 4, 2, lbl, SCR_DIM);
	}

	for (int db = (int) SPEC_DB_MAX - GRID_DB; db > (int) SPEC_DB_MIN; db -= GRID_DB) {
		char lbl[8];
		int gy = SPEC_Y + SPEC_H - 1 - (int) (spec_norm((float) db) * (float) (SPEC_H - 1));

		(void) lbl;
		x6100_lcd_hline(lcd, 0, W - 1, gy, 0x1A2430);
	}

	int prev_y = -1;

	for (int x = 0; x < W; x++) {
		int y = spec_y(spec_at(scr, x, W));
		int py = spec_y(peak_at(scr, x, W));

		x6100_lcd_vline(lcd, x, y, SPEC_Y + SPEC_H - 1, SCOPE_FILL);
		x6100_lcd_fill(lcd, x, py, 1, 1, SCOPE_PEAK);
		x6100_lcd_vline(lcd, x, prev_y < 0 ? y : prev_y, y, SCOPE_LINE);
		prev_y = y;
	}

	//
	// dB labels in a left margin box, drawn over the trace
	//
	for (int db = (int) SPEC_DB_MAX - GRID_DB; db > (int) SPEC_DB_MIN; db -= GRID_DB) {
		char lbl[8];
		int gy = spec_y((float) db);

		if (gy <= SPEC_Y + 20)
			continue;

		snprintf(lbl, sizeof(lbl), "%d", db + (int) SMETER_CAL_DB);
		x6100_lcd_fill(lcd, 0, gy - 10, LBL_W, 9, 0x05080C);
		x6100_lcd_text(lcd, 2, gy - 9, 1, lbl, SCR_DIM);
	}

	x6100_lcd_vline(lcd, W / 2, SPEC_Y, SPEC_Y + SPEC_H - 1, SCR_WARN);

	//
	// Waterfall, newest line on top
	//
	if (new_line) {
		x6100_lcd_scroll_down(lcd, WF_Y, lcd->h - WF_Y);

		for (int x = 0; x < W; x++)
			x6100_lcd_fill(lcd, x, WF_Y, 1, 1, wf_colour((spec_at(scr, x, W) - (scr->floor_db - WF_BELOW_DB)) / WF_RANGE_DB));
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

//
// Console commands: frequency, mode and step tokens
//

static bool console_token(rx_t *rx, const char *t)
{
	static const struct { const char *name; x6100_mode_t mode; } modes[] = {
		{ "lsb", X6100_MODE_LSB }, { "usb", X6100_MODE_USB }, { "cw", X6100_MODE_CW },
		{ "cwr", X6100_MODE_CWR }, { "am", X6100_MODE_AM }, { "nfm", X6100_MODE_NFM },
	};
	char *end;

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcasecmp(t, modes[i].name) == 0) {
			rx->mode = modes[i].mode;
			return true;
		}
	}

	if (isdigit((unsigned char) t[0])) {
		double v = strtod(t, &end);

		if (*end == 'k' || *end == 'K')
			v *= 1e3;
		else if (*end == 'm' || *end == 'M' || strchr(t, '.') != NULL)
			v *= 1e6;
		else if (*end != '\0')
			return false;

		set_freq(rx, (int64_t) llround(v));
		return true;
	}

	return false;
}

static bool console_line(rx_t *rx, char *line)
{
	bool changed = false;
	bool want_step = false;

	for (char *t = strtok(line, " \t"); t != NULL; t = strtok(NULL, " \t")) {
		if (want_step) {
			int st = atoi(t);

			want_step = false;

			if (st == 1 || st == 10 || st == 100 || st == 1000 || st == 5000) {
				rx->step = st;
				changed = true;
			} else {
				fprintf(stderr, "\nstep must be 1, 10, 100, 1000 or 5000\n");
			}

			continue;
		}

		if (strcasecmp(t, "step") == 0) {
			want_step = true;
			continue;
		}

		if (console_token(rx, t))
			changed = true;
		else
			fprintf(stderr, "\nunknown: %s\n", t);
	}

	return changed;
}

static bool console_read(rx_t *rx, char *buf, size_t size, size_t *len)
{
	bool changed = false;
	ssize_t n = read(STDIN_FILENO, buf + *len, size - 1 - *len);

	if (n <= 0)
		return false;

	*len += (size_t) n;
	buf[*len] = '\0';

	char *nl;

	while ((nl = strchr(buf, '\n')) != NULL) {
		*nl = '\0';

		if (rx->lock)
			fprintf(stderr, "\nlocked\n");
		else
			changed |= console_line(rx, buf);

		size_t rest = *len - (size_t) (nl + 1 - buf);

		memmove(buf, nl + 1, rest + 1);
		*len = rest;
	}

	if (*len >= size - 1)
		*len = 0;

	return changed;
}

int main(int argc, char **argv)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	int fds[IN_COUNT];
	struct pollfd pfd[IN_COUNT + 1];
	int npfd = 0;
	input_t pmap[IN_COUNT + 1];
	char cmd[128];
	size_t cmd_len = 0;
	int stdin_slot = -1;
	rx_t rx = { .vol = 20, .rfg = 100, .step = 100, .agc = X6100_AGC_AUTO, .mode = X6100_MODE_USB };
	int rc;

	for (int i = 0; i < BAND_COUNT; i++)
		rx.band_freq[i] = g_bands[i].def;

	set_freq(&rx, 14074000);

	for (int i = 1; i < argc; i++) {
		if (!console_token(&rx, argv[i])) {
			fprintf(stderr, "usage: %s [freq] [usb|lsb|cw|cwr|am|nfm]\n", argv[0]);
			return 1;
		}
	}

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

	pfd[npfd].fd = STDIN_FILENO;
	pfd[npfd].events = POLLIN;
	pmap[npfd] = IN_COUNT;
	stdin_slot = npfd;
	npfd++;

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

	static screen_t scr;

	if ((rc = x6100_lcd_open(&scr.lcd, X6100_LCD_ROTATION)) < 0)
		fprintf(stderr, "warning: display not available (%s), console only\n", strerror(-rc));
	else
		scr.on = true;

	if (scr.on) {
		x6100_lcd_fill(&scr.lcd, 0, 0, scr.lcd.w, scr.lcd.h, SCR_BG);
		draw_screen(&scr, &rx, false);
		x6100_lcd_flush(&scr.lcd);
	}

	printf("x6100_rx: PWR or Ctrl-C to quit\n");
	show(&rx);

	uint64_t last_refresh = now_ms();
	uint64_t last_meter = last_refresh;
	uint64_t last_disp = last_refresh;
	double level = 0.0;
	int level_n = 0;
	bool new_frame = false;
	bool dirty = false;
	hold_t hold = { .key = -1 };

	while (!g_stop) {
		bool changed = false;

		if (poll(pfd, (nfds_t) npfd, POLL_MS) < 0 && errno != EINTR)
			break;

		for (int p = 0; p < npfd; p++) {
			struct input_event ev;

			if (p == stdin_slot && (pfd[p].revents & (POLLHUP | POLLERR | POLLNVAL))) {
				pfd[p].fd = -1;
				continue;
			}

			if (!(pfd[p].revents & POLLIN))
				continue;

			if (p == stdin_slot) {
				changed |= console_read(&rx, cmd, sizeof(cmd), &cmd_len);
				continue;
			}

			while (read(pfd[p].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
				if (pmap[p] == IN_POWER && ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
					g_stop = 1;
				else if (pmap[p] == IN_KEYPAD && ev.type == EV_KEY)
					changed |= on_keypad(&rx, &hold, (int) ev.code - X6100_KEY_BASE, ev.value);
				else if (ev.type == EV_REL)
					changed |= on_knob(&rx, pmap[p], ev.value);
			}
		}

		if (hold.key >= 0 && !hold.fired && now_ms() - hold.since >= LONG_PRESS_MS) {
			hold.fired = true;
			changed |= on_long_key(&rx, hold.key);
		}

		if (changed) {
			apply(&ctl, &cal, &rx);
			show(&rx);
			dirty = true;
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

			if (scr.on) {
				spectrum_update(&scr, &frame);
				scr.sig_dbm = passband_dbm(&scr, &rx);
				scr.vbat = frame.vbat * 0.1;
				new_frame = true;
			}
		}

		if (t - last_meter >= METER_MS && level_n > 0) {
			scr.meter_db = level / level_n;
			show(&rx);
			printf("iq %6.1f dB  vbat %.1f", scr.meter_db, frame.vbat * 0.1);
			fflush(stdout);
			level = 0.0;
			level_n = 0;
			last_meter = t;
			dirty = true;
		}

		if (scr.on && (new_frame || dirty) && t - last_disp >= DISP_MS) {
			draw_screen(&scr, &rx, new_frame);
			x6100_lcd_flush(&scr.lcd);
			new_frame = false;
			dirty = false;
			last_disp = t;
		}
	}

	if (scr.on) {
		x6100_lcd_fill(&scr.lcd, 0, 0, scr.lcd.w, scr.lcd.h, 0);
		x6100_lcd_flush(&scr.lcd);
		x6100_lcd_close(&scr.lcd);
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
