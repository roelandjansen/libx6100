/*
 * x6100_audio - record RX audio from the codec while tuned, with level readout
 *
 * Tunes like x6100_tune (RX only, TX blocked), captures card 0 device 0 and
 * writes a WAV file. Every second it prints RMS and peak per channel and the
 * L/R correlation:
 *   corr ~ 1     both channels carry the same (mono) audio
 *   corr ~ 0     channels independent; with --iq this hints at I/Q in quadrature
 *
 * --play-mode sets register 12 bit 0x8 (voice_rec): the speaker plays the codec and the
 * capture loses the RX audio (2026-10-21). Test E1 (RADE TX): does the capture then carry
 * the microphone? Speak into the active mic (register 21 mic select auto: the hand mic when
 * plugged in, or --mic int / hand) and watch the RMS per second. --micgain sets both mic gains
 * (register 20; the stock UI uses 0 when it records a voice memo). Cleared on exit.
 *
 * --input mic2|line|mixer captures one other codec input instead of Mic1 (where the
 * baseband delivers the RX audio): is the hand mic wired to one of them? x6100_mixer list
 * shows which switches the codec has. --input aif2 captures what arrives on AIF2, the
 * codec's second I2S interface (on A33 boards a modem or Bluetooth chip; here unknown,
 * muted in the image). Mic1 and the ADC again on exit.
 *
 * --iq sets the auxiqen bit (register 21 bit 6, "IQ OUT EN" in the stock UI)
 * to test whether the baseband then delivers IQ on the codec input.
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop). If the device is busy,
 * PulseAudio may hold it.
 *
 * --boost sets the codec Mic1 boost (0 = 0 dB, 1 = +24 dB, +3 dB per step); default 0
 * for AM / NFM (the stock +24 dB saturates the codec input on AM), 1 otherwise.
 * --agc and --rfg set the baseband AGC (default auto) and RF gain (default 100).
 * --lpf low-passes the recording at 5.5 kHz: removes the images of the baseband 12 kHz
 * audio (6-11 kHz) that sound as sizzle on AM.
 *
 * --ft8 waits for the next 15 s UTC slot boundary, records one 15 s slot and
 * writes 12 kHz mono 16-bit (WSJT-X File > Open format); <seconds> is ignored.
 * The radio clock must be on UTC time within about a second (NTP).
 *
 * Usage: x6100_audio <freq_hz> <mode> <seconds> <out.wav> [--iq] [--rate N] [--ft8] [--boost 0-7] [--agc off|slow|fast|auto] [--rfg 0-100] [--lpf] [--play-mode] [--mic int|hand|auto] [--micgain 0-100] [--input mic1|mic2|line|mixer|aif2]
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

// The --micgain value as text, for the start line
static const char *argv_gain(int gain)
{
	static char s[24];

	snprintf(s, sizeof(s), "%d (both)", gain);

	return s;
}

// --input: codec capture input; -1 when unknown
static int parse_input(const char *s)
{
	return strcmp(s, "mic1") == 0 ? 0 : strcmp(s, "mic2") == 0 ? 1 : strcmp(s, "line") == 0 ? 2
		: strcmp(s, "mixer") == 0 ? 3 : strcmp(s, "aif2") == 0 ? 4 : -1;
}

//
// Codec capture from one input only (R16 codec: one capture switch per input); a switch
// this codec does not have is reported, which is an answer too
//

static void set_input(int input, int boost)
{
	static const char *sw[] = { "Mic1 Capture Switch", "Mic2 Capture Switch", "Line In Capture Switch",
		"Mixer Capture Switch" };
	int analog = input < 4 ? input : 0;	/* aif2: the analog side stays on Mic1, unused */
	bool aif2 = input == 4;
	int rc;

	for (int i = 0; i < 4; i++) {
		if ((rc = x6100_mixer_set(X6100_PCM_CARD, sw[i], i == analog)) < 0)
			fprintf(stderr, "input: %s: %s\n", sw[i], strerror(-rc));
	}

	if (input == 1 && (rc = x6100_mixer_set(X6100_PCM_CARD, "Mic2 Boost Volume", boost)) < 0)
		fprintf(stderr, "input: Mic2 Boost Volume: %s\n", strerror(-rc));

	// The digital side of the capture (AIF1 AD0): the ADC (default) or what arrives on AIF2,
	// the codec's second I2S interface; its volume 160 = 0 dB, 0 = muted as the image has it
	{
		static const struct { const char *name; long on, off; } C_DIG[] = {
			{ "AIF1 Data Digital ADC Capture Switch", 0, 1 },
			{ "AIF2 Digital ADC Capture Switch", 1, 0 },
			{ "AIF2 DAC Playback Volume", 160, 0 },
		};

		for (size_t i = 0; i < sizeof(C_DIG) / sizeof(C_DIG[0]); i++) {
			if ((rc = x6100_mixer_set(X6100_PCM_CARD, C_DIG[i].name, aif2 ? C_DIG[i].on : C_DIG[i].off)) < 0)
				fprintf(stderr, "input: %s: %s\n", C_DIG[i].name, strerror(-rc));
		}
	}

	if (aif2)
		printf("capture input: AIF2 (second I2S interface) instead of the ADC\n");
	else if (input != 0)
		printf("capture input: %s only\n", sw[input]);
}

// --mic: register 21 mic select; -2 when unknown
static int parse_mic(const char *s)
{
	return strcmp(s, "int") == 0 ? 0 : strcmp(s, "hand") == 0 ? 1 : strcmp(s, "auto") == 0 ? 2 : -2;
}

static int parse_agc(const char *s)
{
	static const char *names[] = { "off", "slow", "fast", "auto" };

	for (int i = 0; i < 4; i++) {
		if (strcasecmp(s, names[i]) == 0)
			return i;
	}

	return -2;
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
// --lpf: 5.5 kHz low-pass at the capture rate (windowed sinc, Blackman, 255 taps,
// about 1 kHz transition): removes the images of the baseband's 12 kHz audio at
// 6-11 kHz ("sizzle" on AM) from the recording
//

#define LPF_TAPS	255
#define LPF_HZ		5500.0

static float g_lpf[LPF_TAPS];
static float g_lpf_hist[2][LPF_TAPS];
static int g_lpf_pos[2];

static void lpf_init(double rate)
{
	double fc = LPF_HZ / rate, sum = 0.0;

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

static int16_t lpf_run(int ch, int16_t in)
{
	float *h = g_lpf_hist[ch];
	float acc = 0.0f;

	h[g_lpf_pos[ch]] = in;
	g_lpf_pos[ch] = (g_lpf_pos[ch] + 1) % LPF_TAPS;

	for (int i = 0; i < LPF_TAPS; i++)
		acc += g_lpf[i] * h[(g_lpf_pos[ch] + i) % LPF_TAPS];

	acc = acc > 32767.0f ? 32767.0f : acc < -32768.0f ? -32768.0f : acc;
	return (int16_t) lrintf(acc);
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
	fprintf(stderr, "usage: %s <freq_hz> <usb|lsb|cw|cwr|am|nfm> <seconds> <out.wav> [--iq] [--rate N] [--ft8] [--boost 0-7] [--agc off|slow|fast|auto] [--rfg 0-100] [--lpf] [--play-mode] [--mic int|hand|auto] [--micgain 0-100] [--input mic1|mic2|line|mixer|aif2]\n", argv0);
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
	int agc = -1;
	int rfg = -1;
	int mic = -1;		/* register 21 [1:0]: 0 internal, 1 hand, 2 auto; -1 library default (auto) */
	int micgain = -1;	/* register 20 [23:16] and [31:24]; -1 library default (30 / 20) */
	int input = 0;		/* codec capture input: 0 Mic1 (the baseband RX audio), 1 Mic2, 2 Line In, 3 mixer */
	static int16_t buf[PERIOD * CHANNELS];
	unsigned rate = DEF_RATE;
	bool iq = false;
	bool ft8 = false;
	bool lpf = false;
	bool play = false;
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
		else if (strcmp(argv[i], "--play-mode") == 0)
			play = true;
		else if (strcmp(argv[i], "--lpf") == 0)
			lpf = true;
		else if (strcmp(argv[i], "--ft8") == 0)
			ft8 = true;
		else if (strcmp(argv[i], "--agc") == 0 && i + 1 < argc)
			agc = parse_agc(argv[++i]);
		else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
			input = parse_input(argv[++i]);
		else if (strcmp(argv[i], "--mic") == 0 && i + 1 < argc)
			mic = parse_mic(argv[++i]);
		else if (strcmp(argv[i], "--micgain") == 0 && i + 1 < argc)
			micgain = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rfg") == 0 && i + 1 < argc)
			rfg = atoi(argv[++i]);
		else if (strcmp(argv[i], "--boost") == 0 && i + 1 < argc)
			boost = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
			rate = (unsigned) atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 1;
		}
	}

	if (freq < 100000 || freq > 60000000 || seconds < 1 || agc == -2 || rfg > 100 || mic == -2 || micgain > 100 || input < 0) {
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

	//
	// Receive filter per mode (AM / FM symmetric, see x6100_set_filter)
	//
	if (mode == X6100_MODE_AM || mode == X6100_MODE_NFM)
		x6100_set_filter(&ctl, mode, 0, 4000);
	else if (mode == X6100_MODE_CW || mode == X6100_MODE_CWR)
		x6100_set_filter(&ctl, mode, 450, 950);
	else
		x6100_set_filter(&ctl, mode, 100, 3000);
	x6100_ctl_set(&ctl, X6100_REG_RXVOL, RXVOL);

	if (agc >= 0) {
		x6100_ctl_set(&ctl, X6100_REG_VFOA_AGC, (uint32_t) agc);
		x6100_ctl_set(&ctl, X6100_REG_VFOB_AGC, (uint32_t) agc);
	}

	if (rfg >= 0)
		x6100_ctl_update(&ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF, (uint32_t) rfg);
	x6100_ctl_update(&ctl, X6100_REG_MIC_PTT_SQL, 1u << 6, iq ? 1u << 6 : 0);

	// Microphone (play mode tests): which one, and the gain of both
	if (mic >= 0)
		x6100_ctl_update(&ctl, X6100_REG_MIC_PTT_SQL, 0x3, (uint32_t) mic);

	if (micgain >= 0)
		x6100_ctl_update(&ctl, X6100_REG_AUDIO_GAIN, 0xFFFF0000u, (uint32_t) micgain << 16 | (uint32_t) micgain << 24);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, play ? X6100_FLAG_VOICE_REC : 0);

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

	if (input != 0)
		set_input(input, boost);

	printf("capture: Mic1 boost %d (%s)\n", boost, boost == 0 ? "0 dB" : boost == 1 ? "+24 dB" : "+27 dB or more");

	if ((rc = x6100_pcm_open(&pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, rate, CHANNELS, PERIOD, PERIODS)) < 0) {
		fprintf(stderr, "pcm open: %s%s\n", strerror(-rc), rc == -EBUSY ? " (PulseAudio or the GUI holds it?)" : "");
		return 1;
	}

	printf("tuned %u Hz %s, auxiqen %s; capture %u Hz %u ch, period %u x %u\n", freq, argv[2], iq ? "ON" : "off",
		pcm.rate, pcm.channels, pcm.period, pcm.periods);

	if (play)
		printf("play mode ON (register 12 0x8): speaker plays the codec; speak into the mic now\n");

	printf("mic %s, mic gains %s\n", mic == 0 ? "internal" : mic == 1 ? "hand" : "auto (hand when plugged)",
		micgain >= 0 ? argv_gain(micgain) : "library default (int 30, hand 20)");

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

	if (lpf && !ft8) {
		lpf_init(pcm.rate);
		printf("low-pass %.1f kHz on\n", LPF_HZ / 1000.0);
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
			if (lpf) {
				for (ssize_t i = 0; i < got * (ssize_t) pcm.channels; i++)
					buf[i] = lpf_run((int) (i % pcm.channels) & 1, buf[i]);
			}

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
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);	/* play mode off again */

	if (input != 0)
		set_input(0, boost);	/* Mic1 again: x6100_trx and wfserver expect the RX audio there */
	x6100_ctl_sync(&ctl);

	printf("wrote %s: %u Hz %u ch, %u bytes, %.1f s\n", argv[4], out_rate, out_ch, data_bytes, (double) total / pcm.rate);
	x6100_pcm_close(&pcm);
	x6100_flow_close(&flow);
	x6100_i2c_close(&i2c);
	return 0;
}
