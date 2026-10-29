/*
 * x6100_tx - controlled TX tests: antenna tuner, carrier, codec audio
 *
 *   x6100_tx <freq_hz> tune
 *       ATU tune cycle as in the stock UI; prints the resulting L/C network
 *       and writes it to register 17
 *
 *   x6100_tx <freq_hz> carrier <seconds> [watts]
 *       NFM without audio: plain carrier; checks the RF chain and the power,
 *       SWR and ALC telemetry
 *
 *   x6100_tx <freq_hz> codec <seconds> [watts] [tone_hz] [level_dbfs]
 *       USB, sine tone played into the R16 codec, TX keyed with register 12
 *       bit 0x4000 (codec as TX audio, not the microphone);
 *       tone default 1000 Hz, level default -30 dBFS
 *
 *   x6100_tx <freq_hz> codec ... --play-mode
 *       the same with register 12 bit 0x8 (voice_rec, "play mode") set as well while
 *       keyed. Test E2 (RADE TX): does the codec tone still go out as the TX signal
 *       (listen on a second receiver, watch power and ALC), or does play mode send the
 *       codec to the speaker instead? 0x8 is cleared with the TX flags on every exit
 *
 *   x6100_tx <freq_hz> micrec <seconds> <out.wav> [watts] [--rf] [--monilvl 0-100]
 *       USB, keyed with PTT (the mic as TX source) and the monitor at its level (default
 *       70); TX enable (registers 0 / 6 bit 0) off unless --rf, so no RF. Records the codec
 *       capture to a WAV and prints its level with tx / power / ALC once a second. Test E4
 *       (RADE): with the monitor the mic reaches the capture while keyed (E3, 2026-10-29);
 *       does it without RF too? --monilvl 0: is it the monitor at all?
 *
 * Safety:
 *   - power 0.1 .. 10 W, default 1.0 W; duration 1 .. 30 s
 *   - amateur bands only; 3 s countdown before keying
 *   - TX stops on any front panel key or the power key, Ctrl-C, timeout,
 *     or SWR above 3.0 for 300 ms (carrier and codec)
 *   - PTT, codec TX and tune are cleared on every exit path, and the mode is
 *     set back to the voice mode of the band (LSB below 10 MHz, else USB)
 *   - use a dummy load
 *
 * Telemetry every 100 ms: TX flag, power, SWR, ALC (100 = no audio), supply
 * voltage, ATU flag.
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

typedef enum
{
	CMD_TUNE = 0,
	CMD_CARRIER,
	CMD_CODEC,
	CMD_MICREC
} cmd_t;

typedef struct
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	bool have_frame;
	int inputs[MAX_INPUTS];
	int ninputs;
} tx_t;

static bool g_play;		/* --play-mode: register 12 bit 0x8 with codec TX */
static bool g_rf;		/* micrec --rf: TX enable on (RF out); off by default: keyed, no RF */
static int g_monilvl = 70;	/* micrec --monilvl: monitor level, register 29 */
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
// Drop every TX source and tune, write immediately; safe to call repeatedly
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

	printf("t=%5.1fs  tx=%u  pwr %4.1f W  swr %4.1f  alc %3u  vext %4.1f V  atu=%u  atu_params %06X\n",
		(now_ms() - t0) / 1000.0, !!(f->flags & X6100_FLOW_TX), f->tx_power * 0.1, f->vswr * 0.1,
		f->alc_level, f->vext * 0.1, !!(f->flags & X6100_FLOW_ATU), f->atu_params & 0xFFFFFF);
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

			//
			// The done flag is stale from the previous tune until the baseband starts
			//
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
// Keyed transmission: NFM carrier via PTT, or a codec tone via codec TX
//

static void key(tx_t *tx, cmd_t cmd)
{
	if (cmd == CMD_CODEC) {
		x6100_set_codec_tx(&tx->ctl, true);
		x6100_ctl_update(&tx->ctl, X6100_REG_FLAGS, X6100_FLAG_VOICE_REC, g_play ? X6100_FLAG_VOICE_REC : 0);
	} else {
		x6100_set_ptt(&tx->ctl, true);
	}
}

static int do_tx(tx_t *tx, cmd_t cmd, int seconds, double tone_hz, double level_dbfs)
{
	const bool audio = cmd == CMD_CODEC;
	const double amp = pow(10.0, level_dbfs / 20.0);
	static int16_t buf[PERIOD * 2];
	x6100_pcm_t pcm = { 0 };
	double phase = 0.0, dphi = 2.0 * M_PI * tone_hz / RATE;
	uint64_t t0, last = 0, swr_since = 0;
	int rc, result = 0;

	if (audio) {
		if ((rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0)
			fprintf(stderr, "warning: mixer setup: %s\n", strerror(-rc));

		if ((rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, false, RATE, 2, PERIOD, PERIODS)) < 0) {
			fprintf(stderr, "pcm playback open: %s\n", strerror(-rc));
			return 1;
		}
	}

	if (!countdown(tx, "TX")) {
		if (audio)
			x6100_pcm_close(&pcm);

		return 1;
	}

	key(tx, cmd);
	x6100_ctl_sync(&tx->ctl);
	t0 = now_ms();

	while (!g_stop) {
		uint64_t t = now_ms();

		if (audio) {
			for (int i = 0; i < PERIOD; i++) {
				int16_t v = (int16_t) lrint(sin(phase) * amp * 32767.0);

				buf[2 * i] = v;
				buf[2 * i + 1] = v;
				phase += dphi;
			}

			phase = fmod(phase, 2.0 * M_PI);

			if (x6100_pcm_write(&pcm, buf, PERIOD) < 0) {
				fprintf(stderr, "pcm write failed\n");
				result = 1;
				break;
			}
		} else {
			usleep(5000);
		}

		poll_flow(tx);

		if (tx->have_frame && (tx->frame.flags & X6100_FLOW_TX) && tx->frame.vswr * 0.1 > SWR_LIMIT) {
			if (swr_since == 0) {
				swr_since = t;
			} else if (t - swr_since >= SWR_TRIP_MS) {
				fprintf(stderr, "ABORT: SWR %.1f above %.1f\n", tx->frame.vswr * 0.1, SWR_LIMIT);
				result = 1;
				break;
			}
		} else {
			swr_since = 0;
		}

		if (t - last >= REPORT_MS) {
			report(tx, t0);
			last = t;
		}

		if (t - t0 >= (uint64_t) seconds * 1000)
			break;

		if (key_pressed(tx)) {
			fprintf(stderr, "aborted by key\n");
			break;
		}

		key(tx, cmd);
		x6100_ctl_sync(&tx->ctl);
	}

	tx_off(tx);
	report(tx, t0);

	if (audio) {
		if (pcm.xruns)
			printf("pcm xruns: %u\n", pcm.xruns);

		x6100_pcm_close(&pcm);
	}

	printf("TX off\n");
	return result;
}

//
// 16-bit PCM WAV header (data size patched at the end)
//

static void wav_header(FILE *f, unsigned rate, unsigned ch, uint32_t bytes)
{
	uint32_t riff = 36 + bytes, fmt_size = 16, byte_rate = rate * ch * 2;
	uint16_t fmt = 1, nch = (uint16_t) ch, align = (uint16_t) (ch * 2), bps = 16;

	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_size, 4, 1, f);
	fwrite(&fmt, 2, 1, f);
	fwrite(&nch, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bps, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
}

//
// micrec: keyed with PTT (the mic as TX source), TX enable off unless --rf; the codec
// capture (the baseband's audio, with the monitor while keyed) to a WAV, its level and
// the TX telemetry once a second: does the mic reach Linux without RF?
//

static int do_micrec(tx_t *tx, int seconds, const char *path)
{
	static int16_t buf[PERIOD * 2];
	x6100_pcm_t pcm = { 0 };
	FILE *wav;
	uint64_t t0, last;
	uint32_t bytes = 0;
	double sum = 0.0;
	int peak = 0, n = 0, rc, result = 0;

	if ((rc = x6100_mixer_rx_setup(X6100_PCM_CARD, 1)) < 0)
		fprintf(stderr, "warning: mixer setup: %s\n", strerror(-rc));

	if ((rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, RATE, 2, PERIOD, PERIODS)) < 0) {
		fprintf(stderr, "pcm capture open: %s\n", strerror(-rc));
		return 1;
	}

	if ((wav = fopen(path, "wb")) == NULL) {
		perror(path);
		x6100_pcm_close(&pcm);
		return 1;
	}

	wav_header(wav, RATE, 2, 0);

	if (!countdown(tx, "PTT (mic)")) {
		fclose(wav);
		x6100_pcm_close(&pcm);
		return 1;
	}

	x6100_set_ptt(&tx->ctl, true);
	x6100_ctl_sync(&tx->ctl);
	t0 = last = now_ms();

	while (!g_stop) {
		uint64_t t = now_ms();
		ssize_t got = x6100_pcm_read(&pcm, buf, PERIOD);

		if (got < 0) {
			fprintf(stderr, "pcm read failed\n");
			result = 1;
			break;
		}

		fwrite(buf, 4, (size_t) got, wav);
		bytes += (uint32_t) got * 4;

		for (ssize_t i = 0; i < got * 2; i++) {
			int v = abs(buf[i]);

			sum += (double) buf[i] * buf[i];
			peak = v > peak ? v : peak;
		}

		n += (int) got * 2;
		poll_flow(tx);

		// With --rf the SWR trip applies as for the other TX tests
		if (g_rf && tx->have_frame && (tx->frame.flags & X6100_FLOW_TX) && tx->frame.vswr * 0.1 > SWR_LIMIT) {
			fprintf(stderr, "ABORT: SWR %.1f above %.1f\n", tx->frame.vswr * 0.1, SWR_LIMIT);
			result = 1;
			break;
		}

		if (t - last >= 1000 && n > 0) {
			const x6100_flow_frame_t *f = &tx->frame;

			printf("t=%2.0fs  capture rms %6.1f pk %6.1f dBFS   tx=%u  pwr %4.1f W  alc %3u\n",
				(t - t0) / 1000.0, 10.0 * log10(sum / n / (32768.0 * 32768.0) + 1e-12),
				20.0 * log10(peak / 32768.0 + 1e-9), tx->have_frame && (f->flags & X6100_FLOW_TX),
				tx->have_frame ? f->tx_power * 0.1 : 0.0, tx->have_frame ? f->alc_level : 0);
			fflush(stdout);
			sum = 0.0;
			peak = 0;
			n = 0;
			last = t;
		}

		if (t - t0 >= (uint64_t) seconds * 1000)
			break;

		if (key_pressed(tx)) {
			fprintf(stderr, "aborted by key\n");
			break;
		}

		x6100_set_ptt(&tx->ctl, true);
		x6100_ctl_sync(&tx->ctl);
	}

	tx_off(tx);
	fseek(wav, 0, SEEK_SET);
	wav_header(wav, RATE, 2, bytes);
	fclose(wav);
	x6100_pcm_close(&pcm);
	printf("PTT off; wrote %s (%u Hz, 2 ch, %.1f s)\n", path, RATE, bytes / (4.0 * RATE));

	return result;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <freq_hz> tune\n"
		"       %s <freq_hz> carrier <seconds 1-30> [watts 0.1-10, default 1]\n"
		"       %s <freq_hz> codec <seconds 1-30> [watts 0.1-10, default 1] [tone_hz 200-2800, default 1000]\n"
		"           [level_dbfs -60..-3, default -30] [--play-mode]\n"
		"       %s <freq_hz> micrec <seconds 1-30> <out.wav> [watts 0.1-10, default 1] [--rf] [--monilvl 0-100]\n",
		argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	static tx_t tx;
	x6100_cal_t cal;
	uint32_t freq;
	cmd_t cmd;
	float watts = 1.0f;
	int seconds = 0;
	double tone_hz = 1000.0;
	double level_dbfs = -30.0;
	x6100_mode_t mode;
	const char *mode_name;
	int rc;

	// --play-mode, --rf, --monilvl anywhere: taken out, the rest stays positional
	{
		int n = 1;

		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--play-mode") == 0)
				g_play = true;
			else if (strcmp(argv[i], "--rf") == 0)
				g_rf = true;
			else if (strcmp(argv[i], "--monilvl") == 0 && i + 1 < argc)
				g_monilvl = atoi(argv[++i]);
			else
				argv[n++] = argv[i];
		}

		argc = n;
	}

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	freq = (uint32_t) strtoul(argv[1], NULL, 10);

	if (strcmp(argv[2], "tune") == 0) {
		cmd = CMD_TUNE;
	} else if (strcmp(argv[2], "carrier") == 0 && argc >= 4) {
		cmd = CMD_CARRIER;
	} else if (strcmp(argv[2], "codec") == 0 && argc >= 4) {
		cmd = CMD_CODEC;
	} else if (strcmp(argv[2], "micrec") == 0 && argc >= 5) {
		cmd = CMD_MICREC;
	} else {
		usage(argv[0]);
		return 1;
	}

	if (g_play && cmd != CMD_CODEC) {
		fprintf(stderr, "--play-mode goes with codec only\n");
		return 1;
	}

	if (g_rf && cmd != CMD_MICREC) {
		fprintf(stderr, "--rf goes with micrec only\n");
		return 1;
	}

	if (cmd != CMD_TUNE) {
		int w = cmd == CMD_MICREC ? 5 : 4;	/* micrec: the WAV file comes first */

		seconds = atoi(argv[3]);
		watts = argc > w ? strtof(argv[w], NULL) : 1.0f;
	}

	if (cmd == CMD_CODEC) {
		tone_hz = argc > 5 ? atof(argv[5]) : 1000.0;
		level_dbfs = argc > 6 ? atof(argv[6]) : -30.0;
	}

	if (freq < 1800000 || freq > 54000000 || seconds < 0 || seconds > 30 || (cmd != CMD_TUNE && seconds < 1)
		|| watts < 0.1f || watts > 10.0f || tone_hz < 200.0 || tone_hz > 2800.0 || level_dbfs < -60.0
		|| level_dbfs > -3.0 || g_monilvl < 0 || g_monilvl > 100) {
		usage(argv[0]);
		return 1;
	}

	if (g_play)
		printf("play mode: register 12 bit 0x8 set with codec TX while keyed (test E2)\n");

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

	switch (cmd) {
	case CMD_CARRIER:
		mode = X6100_MODE_NFM;
		mode_name = "NFM carrier";
		break;
	case CMD_CODEC:
		mode = X6100_MODE_USB;
		mode_name = "USB codec TX";
		break;
	case CMD_MICREC:
		mode = X6100_MODE_USB;
		mode_name = g_rf ? "USB mic (PTT), RF on" : "USB mic (PTT), TX enable off: no RF";
		break;
	default:
		mode = X6100_MODE_USB;
		mode_name = "ATU tune";
		break;
	}

	x6100_set_freq(&tx.ctl, 0, freq);
	x6100_set_freq(&tx.ctl, 1, freq);
	x6100_set_mode(&tx.ctl, 0, mode);
	x6100_set_mode(&tx.ctl, 1, mode);
	x6100_set_txpwr(&tx.ctl, watts);
	x6100_ctl_set(&tx.ctl, X6100_REG_FLAGS, 0);

	// micrec: the monitor at its level; TX enable (registers 0 / 6 bit 0) off unless --rf,
	// as x6100_trx's TOT lock does: keyed, the RF stays off
	if (cmd == CMD_MICREC) {
		x6100_ctl_set(&tx.ctl, X6100_REG_MONILEVEL, (uint32_t) g_monilvl);

		if (!g_rf) {
			x6100_ctl_update(&tx.ctl, X6100_REG_VFOA_BAND, 1, 0);
			x6100_ctl_update(&tx.ctl, X6100_REG_VFOB_BAND, 1, 0);
		}
	}

	if ((rc = x6100_ctl_refresh(&tx.ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	usleep(200000);
	poll_flow(&tx);
	key_pressed(&tx);

	if (cmd == CMD_CODEC)
		printf("%u Hz %s, power %.1f W, tone %.0f Hz at %.0f dBFS\n", freq, mode_name, watts, tone_hz, level_dbfs);
	else if (cmd == CMD_CARRIER)
		printf("%u Hz %s, power %.1f W\n", freq, mode_name, watts);
	else if (cmd == CMD_MICREC)
		printf("%u Hz %s, power %.1f W, monitor level %d; speak while keyed\n", freq, mode_name, watts, g_monilvl);
	else
		printf("%u Hz %s\n", freq, mode_name);

	if (cmd == CMD_TUNE)
		rc = do_tune(&tx);
	else if (cmd == CMD_MICREC)
		rc = do_micrec(&tx, seconds, argv[4]);
	else
		rc = do_tx(&tx, cmd, seconds, tone_hz, level_dbfs);

	tx_off(&tx);

	// TX enable back as the band gives it (micrec cleared it)
	x6100_set_freq(&tx.ctl, 0, freq);
	x6100_set_freq(&tx.ctl, 1, freq);

	//
	// Back to the voice mode for the band: a digital mode left set makes PTT ignore the mic
	//
	x6100_set_mode(&tx.ctl, 0, freq < 10000000 ? X6100_MODE_LSB : X6100_MODE_USB);
	x6100_set_mode(&tx.ctl, 1, freq < 10000000 ? X6100_MODE_LSB : X6100_MODE_USB);
	x6100_ctl_refresh(&tx.ctl);

	x6100_flow_close(&tx.flow);
	x6100_i2c_close(&tx.i2c);

	for (int i = 0; i < tx.ninputs; i++)
		close(tx.inputs[i]);

	return rc;
}
