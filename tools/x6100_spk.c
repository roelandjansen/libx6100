/*
 * x6100_spk - speaker audio through Linux, with a clean low-pass (experiment)
 *
 * The baseband makes its audio at 12 kHz and drives the speaker without a proper
 * reconstruction filter: images of the audio at 6-11 kHz sound as sizzle, most of
 * all on AM. This tool reroutes the speaker:
 *
 *   baseband RX audio -> codec capture -> 5.5 kHz low-pass -> codec playback
 *   -> speaker, with register 12 bit 0x8 (voice_rec / "play" mode) set, so the
 *   baseband plays the codec audio instead of its own receiver audio
 *
 *   x6100_spk <freq_hz> <usb|lsb|cw|cwr|am|nfm> [--agc off|slow|fast|auto]
 *             [--vol 0-100] [--nolpf] [--gain dB]
 *
 *   Any front panel key toggles A/B: Linux path (filtered) <-> baseband direct.
 *   Ctrl-C or PWR quits; bit 0x8 is always cleared on exit.
 *
 * Receive only: PTT and codec TX are never set. Unknowns this answers: whether
 * capture still carries RX audio while 0x8 is set, the added latency, and whether
 * the speaker volume (register 14) still applies.
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "x6100_pcm.h"

#define RATE		48000
#define CHANNELS	2
#define PERIOD		256	/* 5.3 ms */
#define PERIODS		4
#define LPF_TAPS	255
#define LPF_HZ		5500.0
#define MAX_INPUTS	8
#define LEVEL_MS	1000

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
// 5.5 kHz low-pass, windowed sinc (Blackman), one history per channel
//

static float g_lpf[LPF_TAPS];
static float g_hist[CHANNELS][2 * LPF_TAPS];	/* doubled: no modulo in the inner loop */
static int g_pos[CHANNELS];

static void lpf_init(void)
{
	double fc = LPF_HZ / RATE, sum = 0.0;

	for (int i = 0; i < LPF_TAPS; i++) {
		double n = i - (LPF_TAPS - 1) / 2.0;
		double sinc = n == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
		double win = 0.42 - 0.5 * cos(2.0 * M_PI * i / (LPF_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * i / (LPF_TAPS - 1));

		g_lpf[i] = (float) (sinc * win);
		sum += g_lpf[i];
	}

	for (int i = 0; i < LPF_TAPS; i++)
		g_lpf[i] /= (float) sum;
}

static float lpf_run(int ch, float in)
{
	float *h = g_hist[ch];
	const float *x;
	float acc = 0.0f;

	//
	// Each sample is stored twice, LPF_TAPS apart, so the window is contiguous
	//
	h[g_pos[ch]] = in;
	h[g_pos[ch] + LPF_TAPS] = in;
	g_pos[ch] = (g_pos[ch] + 1) % LPF_TAPS;
	x = h + g_pos[ch];

	for (int i = 0; i < LPF_TAPS; i++)
		acc += g_lpf[i] * x[i];

	return acc;
}

//
// Front panel keys and the power key: A/B toggle, quit
//

static int g_inputs[MAX_INPUTS];
static int g_ninputs;

static void open_inputs(void)
{
	for (int n = 0; n < 16 && g_ninputs < MAX_INPUTS; n++) {
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", n);

		if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) >= 0)
			g_inputs[g_ninputs++] = fd;
	}
}

//
// Returns 1 for a key press, 2 for the power key, 0 for nothing
//

static int poll_keys(void)
{
	int hit = 0;

	for (int i = 0; i < g_ninputs; i++) {
		struct input_event ev;

		while (read(g_inputs[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
			if (ev.type == EV_KEY && ev.value == 1)
				hit = ev.code == KEY_POWER ? 2 : hit ? hit : 1;
		}
	}

	return hit;
}

static int parse_mode(const char *s, x6100_mode_t *mode)
{
	static const struct { const char *name; x6100_mode_t mode; } modes[] = {
		{ "lsb", X6100_MODE_LSB }, { "usb", X6100_MODE_USB }, { "cw", X6100_MODE_CW },
		{ "cwr", X6100_MODE_CWR }, { "am", X6100_MODE_AM }, { "nfm", X6100_MODE_NFM },
	};

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcasecmp(s, modes[i].name) == 0) {
			*mode = modes[i].mode;
			return 0;
		}
	}

	return -1;
}

static int parse_agc(const char *s)
{
	static const char *names[] = { "off", "slow", "fast", "auto" };

	for (int i = 0; i < 4; i++) {
		if (strcasecmp(s, names[i]) == 0)
			return i;
	}

	return -1;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <freq_hz> <usb|lsb|cw|cwr|am|nfm> [--agc off|slow|fast|auto] [--vol 0-100]\n"
		"          [--nolpf] [--gain dB]\n", argv0);
}

//
// Play mode on / off: register 12 bit 0x8, written immediately
//

static void play_mode(x6100_ctl_t *ctl, bool on)
{
	x6100_ctl_set(ctl, X6100_REG_FLAGS, on ? X6100_FLAG_VOICE_REC : 0);
	x6100_ctl_sync(ctl);
}

int main(int argc, char **argv)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	x6100_pcm_t cap = { .fd = -1 }, play = { .fd = -1 };
	x6100_mode_t mode;
	static int16_t in[PERIOD * CHANNELS], out[PERIOD * CHANNELS];
	uint32_t freq;
	int agc = -1, vol = 20, rc;
	bool lpf = true, linux_path = true;
	double gain = 1.0, sum2 = 0.0;
	int peak = 0;
	unsigned nsamp = 0;
	uint64_t last = 0;

	if (argc < 3 || parse_mode(argv[2], &mode) < 0) {
		usage(argv[0]);
		return 1;
	}

	freq = (uint32_t) strtoul(argv[1], NULL, 10);

	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--agc") == 0 && i + 1 < argc && (agc = parse_agc(argv[++i])) >= 0) {
			continue;
		} else if (strcmp(argv[i], "--vol") == 0 && i + 1 < argc) {
			vol = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--nolpf") == 0) {
			lpf = false;
		} else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
			gain = pow(10.0, atof(argv[++i]) / 20.0);
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (freq < 500000 || freq > 56000000 || vol < 0 || vol > 100) {
		usage(argv[0]);
		return 1;
	}

	if (agc < 0)
		agc = mode == X6100_MODE_AM || mode == X6100_MODE_NFM ? X6100_AGC_SLOW : X6100_AGC_AUTO;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	//
	// Baseband: tune, receive only
	//
	if ((rc = x6100_i2c_open(&i2c, NULL)) < 0 || (rc = x6100_flow_open(&flow, NULL)) < 0) {
		fprintf(stderr, "open: %s\n", strerror(-rc));
		return 1;
	}

	x6100_ctl_init(&ctl, &i2c);

	if (x6100_cal_read(&i2c, &cal) == 0)
		x6100_cal_apply(&ctl, &cal, x6100_band_index(freq));

	x6100_set_freq(&ctl, 0, freq);
	x6100_set_freq(&ctl, 1, freq);
	x6100_set_mode(&ctl, 0, mode);
	x6100_set_mode(&ctl, 1, mode);

	if (mode == X6100_MODE_AM || mode == X6100_MODE_NFM)
		x6100_set_filter(&ctl, mode, 0, 4500);
	else if (mode == X6100_MODE_CW || mode == X6100_MODE_CWR)
		x6100_set_filter(&ctl, mode, 450, 950);
	else
		x6100_set_filter(&ctl, mode, 100, 3000);

	x6100_ctl_set(&ctl, X6100_REG_VFOA_AGC, (uint32_t) agc);
	x6100_ctl_set(&ctl, X6100_REG_VFOB_AGC, (uint32_t) agc);
	x6100_ctl_set(&ctl, X6100_REG_RXVOL, (uint32_t) vol);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

	if ((rc = x6100_ctl_refresh(&ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	//
	// Codec: capture without the +24 dB boost on AM / FM, playback path on
	//
	if ((rc = x6100_mixer_rx_setup(X6100_PCM_CARD, mode == X6100_MODE_AM || mode == X6100_MODE_NFM ? 0 : 1)) < 0)
		fprintf(stderr, "warning: capture mixer: %s\n", strerror(-rc));

	if ((rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0)
		fprintf(stderr, "warning: playback mixer: %s\n", strerror(-rc));

	if ((rc = x6100_pcm_open(&cap, X6100_PCM_CARD, X6100_PCM_DEVICE, true, RATE, CHANNELS, PERIOD, PERIODS)) < 0
		|| (rc = x6100_pcm_open(&play, X6100_PCM_CARD, X6100_PCM_DEVICE, false, RATE, CHANNELS, PERIOD, PERIODS)) < 0) {
		fprintf(stderr, "pcm open: %s\n", strerror(-rc));
		x6100_pcm_close(&cap);
		return 1;
	}

	lpf_init();
	open_inputs();

	//
	// Prefill two periods of silence so playback never starts empty
	//
	memset(out, 0, sizeof(out));
	x6100_pcm_write(&play, out, PERIOD);
	x6100_pcm_write(&play, out, PERIOD);

	play_mode(&ctl, true);
	printf("%u Hz %s, AGC %s, vol %d, low-pass %s; any key: A/B, PWR / Ctrl-C: quit\n", freq, argv[2],
		(const char *[]) { "OFF", "SLOW", "FAST", "AUTO" }[agc], vol, lpf ? "5.5 kHz" : "off");
	printf("path: LINUX (filtered)\n");

	while (!g_stop) {
		ssize_t got = x6100_pcm_read(&cap, in, PERIOD);
		int k;

		if (got < 0) {
			fprintf(stderr, "capture: %s\n", strerror((int) -got));
			break;
		}

		//
		// L = R (mono from the baseband): filter the left channel once, copy to both
		//
		for (ssize_t i = 0; i < got; i++) {
			float v = lpf ? lpf_run(0, in[i * CHANNELS]) : in[i * CHANNELS];
			long s;

			v = (float) (v * gain);
			s = lrintf(v);
			s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
			out[i * CHANNELS] = (int16_t) s;
			out[i * CHANNELS + 1] = (int16_t) s;
			sum2 += (double) s * s;
			peak = abs((int) s) > peak ? abs((int) s) : peak;
		}

		nsamp += (unsigned) got;

		if (x6100_pcm_write(&play, out, (unsigned) got) < 0) {
			fprintf(stderr, "playback write failed\n");
			break;
		}

		if ((k = poll_keys()) == 2)
			break;

		if (k == 1) {
			linux_path = !linux_path;
			play_mode(&ctl, linux_path);
			printf("path: %s\n", linux_path ? "LINUX (filtered)" : "BASEBAND direct");
			fflush(stdout);
		}

		while (x6100_flow_read(&flow, &frame) > 0) {
			if (frame.flags & X6100_FLOW_TX) {
				fprintf(stderr, "baseband reports TX: stopping\n");
				g_stop = 1;
			}
		}

		x6100_ctl_sync(&ctl);

		if (now_ms() - last >= LEVEL_MS && nsamp > 0) {
			printf("out rms %6.1f dBFS  peak %6.1f dBFS  xruns cap %u play %u\n",
				10.0 * log10(sum2 / nsamp + 1e-9) - 90.31, 20.0 * log10(peak / 32768.0 + 1e-9), cap.xruns,
				play.xruns);
			fflush(stdout);
			sum2 = 0.0;
			peak = 0;
			nsamp = 0;
			last = now_ms();
		}
	}

	//
	// Always back to the baseband's own speaker audio
	//
	play_mode(&ctl, false);
	x6100_ctl_refresh(&ctl);
	x6100_pcm_close(&play);
	x6100_pcm_close(&cap);
	x6100_flow_close(&flow);
	x6100_i2c_close(&i2c);

	for (int i = 0; i < g_ninputs; i++)
		close(g_inputs[i]);

	printf("speaker back to the baseband\n");
	return 0;
}
