/*
 * x6100_audio - record RX audio from the codec while tuned, with level readout
 *
 * Tunes like x6100_tune (RX only, TX blocked), captures card 0 device 0 and
 * writes a WAV file. Every second it prints RMS and peak per channel and the
 * L/R correlation:
 *   corr ~ 1     both channels carry the same (mono) audio
 *   corr ~ 0     channels independent; with --iq this hints at I/Q in quadrature
 *
 * --iq sets the auxiqen bit (register 21 bit 6, "IQ OUT EN" in the stock UI)
 * to test whether the baseband then delivers IQ on the codec input.
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop). If the device is busy,
 * PulseAudio may hold it.
 *
 * --boost sets the codec Mic1 boost (0 = 0 dB, 1 = +24 dB, +3 dB per step); default 0
 * for AM / NFM (the stock +24 dB saturates the codec input on AM), 1 otherwise.
 *
 * --ft8 waits for the next 15 s UTC slot boundary, records one 15 s slot and
 * writes 12 kHz mono 16-bit (WSJT-X File > Open format); <seconds> is ignored.
 * The radio clock must be on UTC time within about a second (NTP).
 *
 * Usage: x6100_audio <freq_hz> <mode> <seconds> <out.wav> [--iq] [--rate N] [--ft8] [--boost 0-7]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "x6100_pcm.h"

#define DEF_RATE	48000
#define CHANNELS	2
#define PERIOD		1024
#define PERIODS		4
#define RXVOL		10

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void) sig;
	g_stop = 1;
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

//
// WAV header, sizes patched on close
//

static void put_le(uint8_t *p, uint32_t v, int n)
{
	for (int i = 0; i < n; i++)
		p[i] = (uint8_t) (v >> (8 * i));
}

static void wav_header(FILE *f, unsigned rate, unsigned ch, uint32_t data_bytes)
{
	uint8_t h[44];

	memcpy(h, "RIFF", 4);
	put_le(h + 4, 36 + data_bytes, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	put_le(h + 16, 16, 4);
	put_le(h + 20, 1, 2);
	put_le(h + 22, ch, 2);
	put_le(h + 24, rate, 4);
	put_le(h + 28, rate * ch * 2, 4);
	put_le(h + 32, ch * 2, 2);
	put_le(h + 34, 16, 2);
	memcpy(h + 36, "data", 4);
	put_le(h + 40, data_bytes, 4);

	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, sizeof(h), f);
}

static double dbfs(double v)
{
	return v > 0.0 ? 20.0 * log10(v / 32768.0) : -120.0;
}

//
// 48 kHz -> 12 kHz, 4:1, windowed-sinc low-pass (cut-off 5 kHz, 95 taps)
//

#define DEC		4
#define FIR_TAPS	95

static float g_fir[FIR_TAPS];
static float g_hist[FIR_TAPS];
static int g_hpos;
static int g_phase;

static void fir_init(double rate)
{
	double fc = 5000.0 / rate, sum = 0.0;

	for (int i = 0; i < FIR_TAPS; i++) {
		double n = i - (FIR_TAPS - 1) / 2.0;
		double sinc = n == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
		double win = 0.42 - 0.5 * cos(2.0 * M_PI * i / (FIR_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * i / (FIR_TAPS - 1));

		g_fir[i] = (float) (sinc * win);
		sum += g_fir[i];
	}

	for (int i = 0; i < FIR_TAPS; i++)
		g_fir[i] /= (float) sum;
}

//
// Push one mono sample; returns true and sets *out every DEC-th sample
//

static bool decimate(float in, int16_t *out)
{
	g_hist[g_hpos] = in;
	g_hpos = (g_hpos + 1) % FIR_TAPS;

	if (++g_phase < DEC)
		return false;

	g_phase = 0;

	float acc = 0.0f;

	for (int i = 0; i < FIR_TAPS; i++)
		acc += g_fir[i] * g_hist[(g_hpos + i) % FIR_TAPS];

	acc = acc > 32767.0f ? 32767.0f : acc < -32768.0f ? -32768.0f : acc;
	*out = (int16_t) lrintf(acc);
	return true;
}

//
// Sleep until the next 15 s UTC boundary
//

static void wait_slot(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	double t = (double) now.tv_sec + now.tv_nsec / 1e9;
	double next = (double) ((now.tv_sec / 15 + 1) * 15);
	double wait = next - t;
	struct timespec ts = { (time_t) wait, (long) ((wait - (time_t) wait) * 1e9) };

	printf("waiting %.1f s for slot start %02ld:%02ld:%02ld UTC\n", wait,
		(long) next % 86400 / 3600, (long) next % 3600 / 60, (long) next % 60);
	fflush(stdout);
	nanosleep(&ts, NULL);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <freq_hz> <usb|lsb|cw|cwr|am|nfm> <seconds> <out.wav> [--iq] [--rate N] [--ft8] [--boost 0-7]\n", argv0);
}

int main(int argc, char **argv)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	x6100_pcm_t pcm;
	x6100_mode_t mode;
	int boost = -1;
	static int16_t buf[PERIOD * CHANNELS];
	unsigned rate = DEF_RATE;
	bool iq = false;
	bool ft8 = false;
	uint32_t freq;
	int seconds;
	int rc;

	if (argc < 5 || parse_mode(argv[2], &mode) < 0) {
		usage(argv[0]);
		return 1;
	}

	freq = (uint32_t) strtoul(argv[1], NULL, 10);
	seconds = atoi(argv[3]);

	for (int i = 5; i < argc; i++) {
		if (strcmp(argv[i], "--iq") == 0)
			iq = true;
		else if (strcmp(argv[i], "--ft8") == 0)
			ft8 = true;
		else if (strcmp(argv[i], "--boost") == 0 && i + 1 < argc)
			boost = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
			rate = (unsigned) atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 1;
		}
	}

	if (freq < 100000 || freq > 60000000 || seconds < 1) {
		usage(argv[0]);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	//
	// Baseband: tune, RX only
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
	x6100_ctl_set(&ctl, X6100_REG_RXVOL, RXVOL);
	x6100_ctl_update(&ctl, X6100_REG_MIC_PTT_SQL, 1u << 6, iq ? 1u << 6 : 0);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

	if ((rc = x6100_ctl_refresh(&ctl)) < 0) {
		fprintf(stderr, "refresh: %s\n", strerror(-rc));
		return 1;
	}

	//
	// Codec capture: Mic1 boost 0 for AM / FM (saturates otherwise), +24 dB for the rest
	//
	if (boost < 0)
		boost = mode == X6100_MODE_AM || mode == X6100_MODE_NFM ? 0 : 1;

	if ((rc = x6100_mixer_rx_setup(X6100_PCM_CARD, boost)) < 0)
		fprintf(stderr, "warning: mixer setup: %s\n", strerror(-rc));

	printf("capture: Mic1 boost %d (%s)\n", boost, boost == 0 ? "0 dB" : boost == 1 ? "+24 dB" : "+27 dB or more");

	if ((rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, rate, CHANNELS, PERIOD, PERIODS)) < 0) {
		fprintf(stderr, "pcm open: %s%s\n", strerror(-rc), rc == -EBUSY ? " (PulseAudio or the GUI holds it?)" : "");
		return 1;
	}

	printf("tuned %u Hz %s, auxiqen %s; capture %u Hz %u ch, period %u x %u\n", freq, argv[2], iq ? "ON" : "off",
		pcm.rate, pcm.channels, pcm.period, pcm.periods);

	FILE *wav = fopen(argv[4], "wb");

	if (wav == NULL) {
		perror(argv[4]);
		return 1;
	}

	unsigned out_rate = ft8 ? pcm.rate / DEC : pcm.rate;
	unsigned out_ch = ft8 ? 1 : pcm.channels;

	if (ft8) {
		if (pcm.rate != 48000) {
			fprintf(stderr, "--ft8 needs a 48000 Hz capture rate\n");
			return 1;
		}

		seconds = 15;
		fir_init(pcm.rate);
	}

	wav_header(wav, out_rate, out_ch, 0);

	uint32_t data_bytes = 0;
	uint64_t total = 0, target = (uint64_t) seconds * pcm.rate;

	//
	// For FT8: flush the stale capture buffer, then start exactly on the slot
	//
	if (ft8) {
		wait_slot();
		x6100_pcm_close(&pcm);

		if ((rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, 48000, CHANNELS, PERIOD, PERIODS)) < 0) {
			fprintf(stderr, "pcm reopen: %s\n", strerror(-rc));
			return 1;
		}
	}
	double s2[2] = { 0 }, slr = 0.0;
	int pk[2] = { 0 };
	unsigned n = 0, sec = 0;

	while (!g_stop && total < target) {
		ssize_t got = x6100_pcm_read(&pcm, buf, PERIOD);

		if (got < 0) {
			fprintf(stderr, "pcm read: %s\n", strerror((int) -got));
			break;
		}

		if (ft8) {
			for (ssize_t i = 0; i < got; i++) {
				int16_t o;
				float m = ((float) buf[i * pcm.channels] + (float) buf[i * pcm.channels + 1]) * 0.5f;

				if (decimate(m, &o)) {
					fwrite(&o, sizeof(o), 1, wav);
					data_bytes += sizeof(o);
				}
			}
		} else {
			fwrite(buf, (size_t) got * pcm.channels * 2, 1, wav);
			data_bytes += (uint32_t) got * pcm.channels * 2;
		}
		total += (uint64_t) got;

		for (ssize_t i = 0; i < got; i++) {
			int l = buf[i * pcm.channels], r = pcm.channels > 1 ? buf[i * pcm.channels + 1] : l;

			s2[0] += (double) l * l;
			s2[1] += (double) r * r;
			slr += (double) l * r;
			pk[0] = abs(l) > pk[0] ? abs(l) : pk[0];
			pk[1] = abs(r) > pk[1] ? abs(r) : pk[1];
		}

		n += (unsigned) got;

		//
		// Keep the baseband registers alive, never TX
		//
		x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);
		x6100_ctl_sync(&ctl);

		while (x6100_flow_read(&flow, &frame) > 0) {
			if (frame.flags & X6100_FLOW_TX) {
				fprintf(stderr, "ABORT: baseband reports TX\n");
				g_stop = 1;
			}
		}

		if (n >= pcm.rate) {
			double corr = s2[0] > 0 && s2[1] > 0 ? slr / sqrt(s2[0] * s2[1]) : 0.0;

			printf("t=%2us  L rms %6.1f pk %6.1f dBFS   R rms %6.1f pk %6.1f dBFS   corr %+.2f   xruns %u\n",
				++sec, dbfs(sqrt(s2[0] / n)), dbfs(pk[0]), dbfs(sqrt(s2[1] / n)), dbfs(pk[1]), corr, pcm.xruns);
			fflush(stdout);
			s2[0] = s2[1] = slr = 0.0;
			pk[0] = pk[1] = 0;
			n = 0;
		}
	}

	wav_header(wav, out_rate, out_ch, data_bytes);
	fclose(wav);

	x6100_ctl_update(&ctl, X6100_REG_MIC_PTT_SQL, 1u << 6, 0);
	x6100_ctl_set(&ctl, X6100_REG_RXVOL, 0);
	x6100_ctl_sync(&ctl);

	printf("wrote %s: %u Hz %u ch, %u bytes, %.1f s\n", argv[4], out_rate, out_ch, data_bytes, (double) total / pcm.rate);
	x6100_pcm_close(&pcm);
	x6100_flow_close(&flow);
	x6100_i2c_close(&i2c);
	return 0;
}
