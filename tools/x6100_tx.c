/*
 * x6100_tx - controlled TX tests: antenna tuner and single-tone transmit
 *
 *   x6100_tx [-g] <freq_hz> ...
 *       -g: sniffed IU1WXY GUI register state instead of library defaults (TX audio debugging)
 *
 *   x6100_tx <freq_hz> tune
 *       ATU tune cycle as in the stock UI; prints the resulting L/C network
 *       and writes it to register 17
 *
 *   x6100_tx <freq_hz> codec <seconds> [same options as tone]
 *       USB-D tone keyed with register 12 bit 0x4000 (codec as TX audio, as x6100_gui
 *       does for FT8 and voice messages) instead of PTT
 *
 *   x6100_tx <freq_hz> mictone <seconds> [same options as tone]
 *       as tone, but plain USB with internal mic gain 30: tests codec playback into the mic input
 *
 *   x6100_tx <freq_hz> carrier <seconds> [watts]
 *       NFM without audio: plain carrier, checks the RF chain and power telemetry
 *
 *   x6100_tx <freq_hz> tone <seconds> [watts] [tone_hz] [level_dbfs] [line_in] [micsel] [pttmode]
 *       USB-D, codec playback of a sine tone as modulation, PTT on for <seconds>;
 *       level_dbfs default -30, line_in (register 20 [7:0], 0-36) default 10;
 *       micsel / pttmode: register 21 [1:0] / [3:2], 0-3, default 2 / 0 (TX audio source tests);
 *       vrec 1: hold the register 12 voice-recorder flag during TX (codec playback routing test)
 *
 * Safety:
 *   - power 0.1 .. 10 W, default 1.0 W; tone duration 1 .. 30 s
 *   - 3 s countdown before keying, Ctrl-C aborts
 *   - TX stops on any front panel key or the power key, Ctrl-C, timeout,
 *     or SWR above 3.0 for 300 ms (tone mode)
 *   - PTT and tune are cleared on every exit path
 *   - use a dummy load
 *
 * Telemetry every 100 ms: TX flag, power, SWR, ALC, supply voltage, ATU flag.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "x6100_pcm.h"

#define MAX_INPUTS	8
#define SWR_LIMIT	3.0
#define SWR_TRIP_MS	300
#define TUNE_TIMEOUT_MS	20000
#define REPORT_MS	100
#define RATE		48000
#define PERIOD		512
#define PERIODS		4

typedef struct
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	bool have_frame;
	int inputs[MAX_INPUTS];
	int ninputs;
	uint32_t txflags;	/* extra register 12 flags held during tone TX */
} tx_t;

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
// Any front panel key or the power key aborts TX
//

static void open_inputs(tx_t *tx)
{
	tx->ninputs = 0;

	for (int n = 0; n < 16 && tx->ninputs < MAX_INPUTS; n++) {
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", n);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (fd >= 0)
			tx->inputs[tx->ninputs++] = fd;
	}
}

static bool key_pressed(tx_t *tx)
{
	bool hit = false;

	for (int i = 0; i < tx->ninputs; i++) {
		struct input_event ev;

		while (read(tx->inputs[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
			if (ev.type == EV_KEY && ev.value == 1)
				hit = true;
		}
	}

	return hit;
}

//
// Drop PTT and tune, write immediately; safe to call repeatedly
//

static void tx_off(tx_t *tx)
{
	x6100_set_ptt(&tx->ctl, false);
	x6100_set_codec_tx(&tx->ctl, false);
	x6100_ctl_update(&tx->ctl, X6100_REG_FLAGS, X6100_FLAG_VOICE_REC, 0);
	x6100_atu_tune(&tx->ctl, false);
	x6100_ctl_sync(&tx->ctl);
	x6100_ctl_refresh(&tx->ctl);
}

static void poll_flow(tx_t *tx)
{
	while (x6100_flow_read(&tx->flow, &tx->frame) > 0)
		tx->have_frame = true;
}

static void report(tx_t *tx, uint64_t t0)
{
	const x6100_flow_frame_t *f = &tx->frame;

	if (!tx->have_frame)
		return;

	printf("t=%5.1fs  tx=%u  pwr %4.1f W  swr %3.1f  alc %4.1f  vext %4.1f V  atu=%u  atu_params %06X\n",
		(now_ms() - t0) / 1000.0, !!(f->flags & X6100_FLOW_TX), f->tx_power * 0.1, f->vswr * 0.1,
		f->alc_level * 0.1, f->vext * 0.1, !!(f->flags & X6100_FLOW_ATU), f->atu_params & 0xFFFFFF);

	//
	// Raw telemetry: flags word, then the 32 bytes up to and including hkey
	//
	{
		const uint8_t *p = (const uint8_t *) &f->flags;
		size_t i;

		printf("          raw");
		for (i = 0; i < 36; i++)
			printf("%s%02X", (i % 4) ? "" : " ", p[i]);
		printf("\n");
	}
	fflush(stdout);
}

static bool countdown(tx_t *tx, const char *what)
{
	for (int i = 3; i > 0 && !g_stop; i--) {
		printf("%s in %d ... (Ctrl-C or any key aborts)\n", what, i);
		fflush(stdout);

		for (int k = 0; k < 10 && !g_stop; k++) {
			usleep(100000);
			x6100_ctl_sync(&tx->ctl);
			poll_flow(tx);

			if (key_pressed(tx))
				g_stop = 1;
		}
	}

	return !g_stop;
}

//
// ATU tune cycle
//

static int do_tune(tx_t *tx)
{
	uint64_t t0, last = 0;
	bool seen_tx = false, done = false;
	x6100_atu_t atu;

	if (!countdown(tx, "ATU tune"))
		return 1;

	x6100_atu_enable(&tx->ctl, true);
	x6100_atu_tune(&tx->ctl, true);
	x6100_ctl_sync(&tx->ctl);
	t0 = now_ms();

	while (!g_stop) {
		uint64_t t = now_ms();

		poll_flow(tx);

		if (tx->have_frame) {
			bool txf = tx->frame.flags & X6100_FLOW_TX;
			bool atuf = tx->frame.flags & X6100_FLOW_ATU;

			seen_tx |= txf;

			if (atuf && (seen_tx || t - t0 > 500)) {
				done = true;
				break;
			}
		}

		if (t - last >= REPORT_MS) {
			report(tx, t0);
			last = t;
		}

		if (t - t0 > TUNE_TIMEOUT_MS) {
			fprintf(stderr, "ATU tune timeout\n");
			break;
		}

		if (key_pressed(tx)) {
			fprintf(stderr, "aborted by key\n");
			break;
		}

		x6100_ctl_sync(&tx->ctl);
		usleep(5000);
	}

	tx_off(tx);
	report(tx, t0);

	if (!done)
		return 1;

	x6100_atu_from_frame(&tx->frame, &atu);
	x6100_atu_set_network(&tx->ctl, &atu);
	x6100_ctl_sync(&tx->ctl);

	printf("ATU done in %.1f s: L=%u C=%u S=%u (register 17 = %06X)\n", (now_ms() - t0) / 1000.0,
		atu.l, atu.c, atu.s, (unsigned) (atu.l | atu.c << 8 | atu.s << 16));
	return 0;
}

//
// Single tone via codec playback in USB-D
//

static int do_tone(tx_t *tx, int seconds, double tone_hz, double level_dbfs, bool audio, bool codec)
{
	const double amp = pow(10.0, level_dbfs / 20.0);
	static int16_t buf[PERIOD * 2];
	x6100_pcm_t pcm = { 0 };
	double phase = 0.0, dphi = 2.0 * M_PI * tone_hz / RATE;
	uint64_t t0, last = 0, swr_since = 0;
	int rc, result = 0;

	if (audio && (rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, false, RATE, 2, PERIOD, PERIODS)) < 0) {
		fprintf(stderr, "pcm playback open: %s\n", strerror(-rc));
		return 1;
	}

	if (audio && (rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0)
		fprintf(stderr, "warning: mixer setup: %s\n", strerror(-rc));

	if (!countdown(tx, "TX")) {
		if (audio)
			x6100_pcm_close(&pcm);
		return 1;
	}

	if (codec)
		x6100_set_codec_tx(&tx->ctl, true);
	else
		x6100_set_ptt(&tx->ctl, true);
	x6100_ctl_update(&tx->ctl, X6100_REG_FLAGS, tx->txflags, tx->txflags);
	x6100_ctl_sync(&tx->ctl);
	t0 = now_ms();

	while (!g_stop) {
		uint64_t t = now_ms();

		for (int i = 0; i < PERIOD; i++) {
			int16_t v = (int16_t) lrint(sin(phase) * amp * 32767.0);

			buf[2 * i] = v;
			buf[2 * i + 1] = v;
			phase += dphi;
		}

		phase = fmod(phase, 2.0 * M_PI);

		if (!audio)
			usleep(5000);
		else if (x6100_pcm_write(&pcm, buf, PERIOD) < 0) {
			fprintf(stderr, "pcm write failed\n");
			result = 1;
			break;
		}

		poll_flow(tx);

		if (tx->have_frame && (tx->frame.flags & X6100_FLOW_TX) && tx->frame.vswr * 0.1 > SWR_LIMIT) {
			if (swr_since == 0)
				swr_since = t;
			else if (t - swr_since >= SWR_TRIP_MS) {
				fprintf(stderr, "ABORT: SWR %.1f above %.1f\n", tx->frame.vswr * 0.1, SWR_LIMIT);
				result = 1;
				break;
			}
		} else {
			swr_since = 0;
		}

		if (t - last >= REPORT_MS) {
			report(tx, t0);

			if (audio)
				printf("          pcm xruns %u\n", pcm.xruns);
			last = t;
		}

		if (t - t0 >= (uint64_t) seconds * 1000)
			break;

		if (key_pressed(tx)) {
			fprintf(stderr, "aborted by key\n");
			break;
		}

		if (codec)
			x6100_set_codec_tx(&tx->ctl, true);
		else
			x6100_set_ptt(&tx->ctl, true);
		x6100_ctl_update(&tx->ctl, X6100_REG_FLAGS, tx->txflags, tx->txflags);
		x6100_ctl_sync(&tx->ctl);
	}

	tx_off(tx);
	if (audio)
		x6100_pcm_close(&pcm);
	report(tx, t0);
	printf("TX off\n");
	return result;
}

//
// Register state of the IU1WXY GUI while it transmits SSB from the mic (sniffed),
// minus frequency, mode, PTT and TX power. Applied with -g[mask] to compare against
// ours; the mask (hex, default all) selects groups:
//   01 VFO/band/ATT/PRE/AGC select and active VFO (0-11, 13)
//   02 RX volume, FFT decimation (14, 15 [23:16])
//   04 AGC 27/28
//   08 LO/center 35
//   10 bias 41
//   20 RX filters 44-47
//   40 calibration offsets 48-51 zeroed
//   80 audio, VOX, NR, keyer, 36, 38, 53, 55
//

#define G_VFO	0x01
#define G_FFT	0x02
#define G_AGC	0x04
#define G_LO	0x08
#define G_BIAS	0x10
#define G_FILT	0x20
#define G_CAL	0x40
#define G_MISC	0x80

static const struct { uint8_t grp; uint8_t reg; uint32_t val; } C_GUI_STATE[] = {
	{ G_VFO, 0, 0x00000001 }, { G_VFO, 2, 0 }, { G_VFO, 3, 0 }, { G_VFO, 5, 0x00000003 },
	{ G_VFO, 6, 0x00000001 }, { G_VFO, 8, 0 }, { G_VFO, 9, 0 }, { G_VFO, 11, 0x00000003 },
	{ G_FFT, 14, 0x00000016 },
	{ G_MISC, 20, 0x141E0A0A }, { G_MISC, 21, 0x00000012 }, { G_MISC, 22, 0x01F40000 },
	{ G_MISC, 23, 0x000A0A00 }, { G_MISC, 24, 0x000323E8 }, { G_MISC, 25, 0x00000002 },
	{ G_AGC, 27, 0x000006C4 }, { G_AGC, 28, 0x000001F4 },
	{ G_MISC, 29, 0x0000001E }, { G_MISC, 33, 0x052BC00F }, { G_MISC, 34, 0x001E0064 },
	{ G_LO, 35, 0x01000000 },
	{ G_MISC, 36, 0x00004A02 }, { G_MISC, 38, 0x0BB800A0 },
	{ G_BIAS, 41, 0x028A01C2 },
	{ G_FILT, 44, 0x00000064 }, { G_FILT, 45, 0x00000B54 }, { G_FILT, 46, 0x0000003C },
	{ G_FILT, 47, 0x00000B7C },
	{ G_CAL, 48, 0 }, { G_CAL, 49, 0 }, { G_CAL, 50, 0 }, { G_CAL, 51, 0 },
	{ G_MISC, 53, 0x001E8480 }, { G_MISC, 55, 0x00100001 },
};

static void apply_gui_state(x6100_ctl_t *ctl, unsigned mask)
{
	for (size_t i = 0; i < sizeof(C_GUI_STATE) / sizeof(C_GUI_STATE[0]); i++) {
		if (C_GUI_STATE[i].grp & mask)
			x6100_ctl_set(ctl, (x6100_reg_t) C_GUI_STATE[i].reg, C_GUI_STATE[i].val);
	}

	// active VFO B as in the GUI, keep our band index; FFT decimation 1
	if (mask & G_VFO)
		x6100_ctl_update(ctl, X6100_REG_VI_BAND_VM, 0xFF, 1);

	if (mask & G_FFT)
		x6100_ctl_update(ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF00FF, 0x01003F);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-g[mask]] <freq_hz> tune\n", argv0);
	fprintf(stderr, "       -g[mask]: sniffed IU1WXY GUI register groups (hex, default FF), see source\n");
	fprintf(stderr, "       %s <freq_hz> carrier <seconds 1-30> [watts 0.1-10, default 1]\n", argv0);
	fprintf(stderr, "       %s <freq_hz> mictone <seconds> ... (as tone, plain USB)\n", argv0);
	fprintf(stderr, "       %s <freq_hz> codec <seconds> ... (as tone, keyed with the codec-TX flag, not PTT)\n", argv0);
	fprintf(stderr, "       %s <freq_hz> tone <seconds 1-30> [watts 0.1-10, default 1] [tone_hz, default 1000]\n"
		"           [level_dbfs -60..-3, default -30] [line_in 0-36, default 10] [micsel 0-3, default 2]\n"
		"           [pttmode 0-3, default 0] [vrec 0-1, default 0: register 12 voice recorder flag]\n", argv0);
}

int main(int argc, char **argv)
{
	static tx_t tx;
	x6100_cal_t cal;
	uint32_t freq;
	float watts = 1.0f;
	int seconds = 0;
	double tone_hz = 1000.0;
	double level_dbfs = -30.0;
	int line_in = 10;
	int micsel = 2, pttmode = 0, vrec = 0;
	bool tune, carrier = false, mictone = false, codec = false;
	unsigned gui = 0;
	int rc;

	if (argc > 1 && strncmp(argv[1], "-g", 2) == 0) {
		gui = argv[1][2] ? (unsigned) strtoul(argv[1] + 2, NULL, 16) : 0xFFu;
		argv[1] = argv[0];
		argv++;
		argc--;
	}

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	freq = (uint32_t) strtoul(argv[1], NULL, 10);
	tune = strcmp(argv[2], "tune") == 0;

	if (!tune) {
		carrier = strcmp(argv[2], "carrier") == 0;
		mictone = strcmp(argv[2], "mictone") == 0;
		codec = strcmp(argv[2], "codec") == 0;

		if ((!carrier && !mictone && !codec && strcmp(argv[2], "tone") != 0) || argc < 4) {
			usage(argv[0]);
			return 1;
		}

		seconds = atoi(argv[3]);
		watts = argc > 4 ? strtof(argv[4], NULL) : 1.0f;
		tone_hz = argc > 5 ? atof(argv[5]) : 1000.0;
		level_dbfs = argc > 6 ? atof(argv[6]) : -30.0;
		line_in = argc > 7 ? atoi(argv[7]) : 10;
		micsel = argc > 8 ? atoi(argv[8]) : 2;
		pttmode = argc > 9 ? atoi(argv[9]) : 0;
		vrec = argc > 10 ? atoi(argv[10]) : 0;
	}

	if (freq < 1800000 || freq > 54000000 || (!tune && (seconds < 1 || seconds > 30 || watts < 0.1f
		|| watts > 10.0f || tone_hz < 200.0 || tone_hz > 2800.0 || level_dbfs < -60.0 || level_dbfs > -3.0
		|| line_in < 0 || line_in > 36 || micsel < 0 || micsel > 3 || pttmode < 0 || pttmode > 3
		|| vrec < 0 || vrec > 1))) {
		usage(argv[0]);
		return 1;
	}

	if (!(x6100_band_index(freq) & 1)) {
		fprintf(stderr, "%u Hz is outside the amateur bands; refusing to transmit\n", freq);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if ((rc = x6100_i2c_open(&tx.i2c, NULL)) < 0 || (rc = x6100_flow_open(&tx.flow, NULL)) < 0) {
		fprintf(stderr, "open: %s\n", strerror(-rc));
		return 1;
	}

	open_inputs(&tx);
	x6100_ctl_init(&tx.ctl, &tx.i2c);

	if (x6100_cal_read(&tx.i2c, &cal) == 0)
		x6100_cal_apply(&tx.ctl, &cal, x6100_band_index(freq));
	else
		fprintf(stderr, "warning: calibration invalid, using defaults\n");

	x6100_set_freq(&tx.ctl, 0, freq);
	x6100_set_freq(&tx.ctl, 1, freq);
	x6100_set_mode(&tx.ctl, 0, carrier ? X6100_MODE_NFM : mictone ? X6100_MODE_USB : X6100_MODE_USB_DIG);
	x6100_set_mode(&tx.ctl, 1, carrier ? X6100_MODE_NFM : mictone ? X6100_MODE_USB : X6100_MODE_USB_DIG);
	x6100_set_txpwr(&tx.ctl, watts);
	x6100_ctl_update(&tx.ctl, X6100_REG_AUDIO_GAIN, 0xFF, (uint32_t) line_in);

	if (mictone)
		x6100_ctl_update(&tx.ctl, X6100_REG_AUDIO_GAIN, 0xFF0000, 30u << 16);
	x6100_ctl_update(&tx.ctl, X6100_REG_MIC_PTT_SQL, 0xF, (uint32_t) (micsel | pttmode << 2));
	x6100_ctl_set(&tx.ctl, X6100_REG_FLAGS, 0);
	tx.txflags = vrec ? X6100_FLAG_VOICE_REC : 0;

	if (gui) {
		apply_gui_state(&tx.ctl, gui);
		printf("register state: IU1WXY GUI groups %02X\n", gui);
	}

	if ((rc = x6100_ctl_refresh(&tx.ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	usleep(200000);
	poll_flow(&tx);
	key_pressed(&tx);

	if (carrier)
		printf("%u Hz NFM carrier, power %.1f W\n", freq, watts);
	else if (tune)
		printf("%u Hz USB-D, power %.1f W, ATU tune\n", freq, watts);
	else
		printf("%u Hz %s, power %.1f W, tone %.0f Hz at %.0f dBFS, line-in %d, micsel %d, pttmode %d\n", freq,
			mictone ? "USB" : "USB-D",
			watts, tone_hz,
			level_dbfs, line_in, micsel, pttmode);

	rc = tune ? do_tune(&tx) : do_tone(&tx, seconds, tone_hz, level_dbfs, !carrier, codec);

	tx_off(&tx);
	x6100_flow_close(&tx.flow);
	x6100_i2c_close(&tx.i2c);

	for (int i = 0; i < tx.ninputs; i++)
		close(tx.inputs[i]);

	return rc;
}
