/*
 * x6100_trx - transceiver front end driven by the X6100 front panel
 *
 * Receiver as x6100_rx, plus TX from the microphone and the antenna tuner.
 * Uses libx6100 for the baseband and the front panel input devices directly.
 *
 *   VFO knob       tune, step set by FST (10 / 100 / 1000 / 5000 Hz);
 *                  long press FST: 1 Hz step on/off
 *   VOL knob       speaker volume, push = mute
 *   MFK knob       RF gain or TX power; long press MFK: switch function,
 *                  short press: RF gain 100 / TX power 5.0 W
 *   BAND- / BAND+  previous / next band, last frequency per band kept
 *   SSB / CW / AM-FM   USB<->LSB / CW<->CWR / AM<->NFM
 *   PRE            preamp on/off; long press: attenuator on/off
 *   F1             scope CENTER <-> SCROLL-C (waterfall follows tuning)
 *   F2             scope FIX <-> SCROLL-F (fixed window; SCROLL-F slides at the edges)
 *   F3             scope span +-48 / 24 / 12 / 6 / 3 / 1.5 kHz
 *   F4             TX meter on dial and bar: PO / SWR / ALC
 *   F5             receive filter FIL1 / FIL2 / FIL3 (wide / mid / narrow per mode)
 *   F5 long        filter panel (IC-705 style): MFK turn = BW or SFT, MFK push = BW <-> SFT,
 *                  MFK long = DEF; F5 = next FIL; F5 long, another key or 10 s idle closes;
 *                  per mode group (SSB, SSB-D, CW, AM, FM), saved in /etc/x6100/filters.conf
 *                  (F1-F5 do not drop a latched TX)
 *   AGC            off / slow / fast / auto, kept per mode group (default AUTO; AM / FM SLOW)
 *   PTT            hold: transmit while held; tap (< 400 ms): latch TX on,
 *                  next press drops it
 *   ATU            tuner in/out; long press: tune (keys TX at baseband tune power)
 *   LOCK           lock knobs and keys (LOCK again to unlock; PTT still works)
 *   PWR            quit (as does Ctrl-C); drops TX first
 *
 * TX safety: amateur bands only, TX timeout, SWR above 3.0 for 300 ms drops TX,
 * any key except PTT drops a latched TX, tune aborts on any key.
 * The hand mic PTT keys the baseband directly; the display follows the TX flag.
 *
 * Console (ssh):
 *   x6100_trx [freq] [mode]           start values, e.g. x6100_trx 7074000 lsb
 *   x6100_trx --oob ...               no band check before TX (test use; legal limits are yours)
 *   type a line + Enter while running:
 *     7074000 | 7.074 | 7074k         frequency (Hz, MHz with '.', or kHz with 'k')
 *     usb lsb cw cwr am nfm           mode
 *     off slow fast auto              AGC
 *     att noatt pre nopre             attenuator / preamp
 *     center scrollc fix scrollf      spectrum scope mode (also F1 / F2)
 *     fil1 fil2 fil3                  receive filter
 *     step 1|10|100|1000|5000         tuning step
 *     pwr 0.1-10                      TX power in W
 *   tokens can be combined: "14074000 usb"
 *
 * Built twice: x6100_trx, and x6100_rx with -DX6100_RX_ONLY (PTT and tune refused,
 * the transmitter is never keyed; hand mic TX is only displayed).
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop). Use a dummy load or a tuned antenna.
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
#include <sys/stat.h>
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
#define TX_TIMEOUT_MS	180000
#define TX_TAP_MS	400
#define SWR_LIMIT	3.0
#define SWR_TRIP_MS	300
#define TUNE_TIMEOUT_MS	20000
#define TXPWR_DEF	50	/* 0.1 W units */
#define TXPWR_MAX	100

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

//
// Mode groups for filters and AGC: SSB, SSB-D, CW (+CWR), AM, FM
//

typedef enum
{
	FG_SSB = 0,
	FG_SSBD,
	FG_CW,
	FG_AM,
	FG_FM,
	FG_COUNT
} fgroup_t;

//
// Spectrum scope modes (Icom style)
//   CENTER     VFO in the middle, waterfall rows stay where they were drawn
//   CENTER_S   VFO in the middle, waterfall shifts with tuning (stays frequency aligned)
//   FIX        fixed frequency window, the VFO cursor moves across it
//   FIX_S      fixed window that slides along when the VFO reaches an edge
//

typedef enum
{
	SCOPE_CENTER = 0,
	SCOPE_CENTER_S,
	SCOPE_FIX,
	SCOPE_FIX_S
} scope_mode_t;

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

	//
	// TX
	//
	int txpwr;		/* 0.1 W units */
	bool mfk_pwr;		/* MFK adjusts TX power instead of RF gain */
	bool ptt;		/* PTT requested (key held or latched) */
	bool ptt_latch;
	uint64_t ptt_since;
	bool atu_on;
	bool tuning;
	uint64_t tune_since;
	bool tune_seen_tx;
	x6100_atu_t atu_net[11];	/* tuned network per band */
	bool atu_valid[11];
	const char *warn;	/* last TX refusal / trip reason, shown until next TX */

	//
	// Spectrum scope
	//
	scope_mode_t scope;
	int64_t fix_lo;		/* left edge of the fixed window, Hz */
	int span;		/* window width, Hz */

	//
	// Meter shown on the dial during TX (F4): 0 Po, 1 SWR, 2 ALC
	//
	int meter;

	//
	// Receive filter FIL1-3 (F5), 0-2
	//
	int fil;

	//
	// AGC per mode group, as on Icom: switching mode recalls that group's AGC;
	// defaults AUTO, AM / FM SLOW (AUTO squashes AM to ~4 dB dynamic range)
	//
	x6100_agc_t agc_group[FG_COUNT];
	int agc_last_group;
	bool fedit;		/* filter panel open (F5 long) */
	int fedit_par;		/* 0 BW, 1 SFT */
	uint64_t fedit_since;	/* last panel activity, for the idle close */
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

//
// Receive filters, IC-705 style: five mode groups x FIL1 / FIL2 / FIL3, each an
// audio passband low / high in Hz (registers 44-47). Edited with F5 long + MFK as
// bandwidth (BW) and shift (SFT); kept in FIL_PATH.
//

#define FIL_PATH	"/etc/x6100/filters.conf"
#define FIL_MAX_HZ	4500

static const char *g_fg_name[FG_COUNT] = { "ssb", "ssbd", "cw", "am", "fm" };

static const uint32_t g_fil_def[FG_COUNT][3][2] = {
	{ { 100, 3000 }, { 300, 2700 }, { 300, 2100 } },	/* SSB */
	{ { 100, 3000 }, { 300, 2700 }, { 300, 2100 } },	/* SSB-D */
	{ { 100, 1300 }, { 450, 950 }, { 575, 825 } },		/* CW, centred on 700 Hz */
	{ { 0, 4500 }, { 0, 3000 }, { 0, 2000 } },		/* AM */
	{ { 0, 4500 }, { 0, 3000 }, { 0, 2000 } },		/* FM */
};

static uint32_t g_fil[FG_COUNT][3][2];
static bool g_fil_dirty;

static fgroup_t mode_group(x6100_mode_t mode)
{
	switch (mode) {
	case X6100_MODE_LSB_DIG:
	case X6100_MODE_USB_DIG:
		return FG_SSBD;
	case X6100_MODE_CW:
	case X6100_MODE_CWR:
		return FG_CW;
	case X6100_MODE_AM:
		return FG_AM;
	case X6100_MODE_NFM:
		return FG_FM;
	default:
		return FG_SSB;
	}
}

static void mode_filter(x6100_mode_t mode, int fil, uint32_t *low, uint32_t *high)
{
	fgroup_t g = mode_group(mode);

	fil = fil < 0 || fil > 2 ? 0 : fil;
	*low = g_fil[g][fil][0];
	*high = g_fil[g][fil][1];
}

//
// Filter file: one line per filter, "<group> <1-3> <low> <high>"; missing or bad
// lines keep the default
//

static void fil_load(void)
{
	FILE *fp;
	char line[128];

	memcpy(g_fil, g_fil_def, sizeof(g_fil));

	if ((fp = fopen(FIL_PATH, "r")) == NULL)
		return;

	while (fgets(line, sizeof(line), fp) != NULL) {
		char name[16];
		int n;
		unsigned lo, hi;

		if (line[0] == '#' || sscanf(line, "%15s %d %u %u", name, &n, &lo, &hi) != 4)
			continue;

		for (int g = 0; g < FG_COUNT; g++) {
			if (strcmp(name, g_fg_name[g]) == 0 && n >= 1 && n <= 3 && lo < hi && hi <= FIL_MAX_HZ) {
				g_fil[g][n - 1][0] = lo;
				g_fil[g][n - 1][1] = hi;
			}
		}
	}

	fclose(fp);
}

static void fil_save(void)
{
	char tmp[] = FIL_PATH ".tmp";
	FILE *fp;

	if (!g_fil_dirty)
		return;

	mkdir("/etc/x6100", 0755);

	if ((fp = fopen(tmp, "w")) == NULL) {
		fprintf(stderr, "\nfilters: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}

	fprintf(fp, "# x6100_trx receive filters: <group> <fil 1-3> <low Hz> <high Hz>\n");

	for (int g = 0; g < FG_COUNT; g++) {
		for (int n = 0; n < 3; n++)
			fprintf(fp, "%s %d %u %u\n", g_fg_name[g], n + 1, g_fil[g][n][0], g_fil[g][n][1]);
	}

	if (fclose(fp) == 0 && rename(tmp, FIL_PATH) == 0)
		g_fil_dirty = false;
	else
		fprintf(stderr, "\nfilters: cannot save %s: %s\n", FIL_PATH, strerror(errno));
}

//
// BW / SFT edits. AM and FM have no shift: the passband stays at 0 Hz and BW
// moves the high edge. Shift is shown relative to the default centre.
//

static void fil_bw(fgroup_t g, int n, int steps)
{
	uint32_t *f = g_fil[g][n];
	int step = g == FG_CW ? 50 : 100;
	int min_bw = g == FG_CW ? 50 : 300;
	int lo = (int) f[0], hi = (int) f[1];

	if (g == FG_AM || g == FG_FM) {
		hi += steps * step;
		hi = hi < min_bw ? min_bw : hi > FIL_MAX_HZ ? FIL_MAX_HZ : hi;
		lo = 0;
	} else {
		int c2 = lo + hi;	/* twice the centre, keeps odd widths exact */
		int bw = hi - lo + steps * step;

		bw = bw < min_bw ? min_bw : bw > FIL_MAX_HZ ? FIL_MAX_HZ : bw;
		lo = (c2 - bw) / 2;
		hi = lo + bw;

		if (lo < 0) {
			hi -= lo;
			lo = 0;
		}

		if (hi > FIL_MAX_HZ) {
			lo -= hi - FIL_MAX_HZ;
			hi = FIL_MAX_HZ;
		}
	}

	f[0] = (uint32_t) lo;
	f[1] = (uint32_t) hi;
	g_fil_dirty = true;
}

static void fil_sft(fgroup_t g, int n, int steps)
{
	uint32_t *f = g_fil[g][n];
	int d = steps * (g == FG_CW ? 10 : 50);
	int lo = (int) f[0] + d, hi = (int) f[1] + d;

	if (g == FG_AM || g == FG_FM || lo < 0 || hi > FIL_MAX_HZ)
		return;

	f[0] = (uint32_t) lo;
	f[1] = (uint32_t) hi;
	g_fil_dirty = true;
}

static int fil_sft_hz(fgroup_t g, int n)
{
	return ((int) (g_fil[g][n][0] + g_fil[g][n][1]) - (int) (g_fil_def[g][n][0] + g_fil_def[g][n][1])) / 2;
}

static void fil_default(fgroup_t g, int n)
{
	g_fil[g][n][0] = g_fil_def[g][n][0];
	g_fil[g][n][1] = g_fil_def[g][n][1];
	g_fil_dirty = true;
}

static bool g_allow_oob;	/* --oob: skip the band check, enable TX in the baseband everywhere */

//
// Push the receiver state into the register shadow
//

static void apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, const rx_t *rx)
{
	uint32_t low, high;

	mode_filter(rx->mode, rx->fil, &low, &high);

	x6100_set_freq(ctl, 0, rx->freq);
	x6100_set_freq(ctl, 1, rx->freq);

	//
	// Registers 0 / 6: TX enable per VFO ("ham band"), normally set from the band
	// plan; --oob sets it everywhere, as the stock UI does with fullband-tx=enable.
	// The band index in register 13 stays the real one (filters, calibration).
	//
	if (g_allow_oob) {
		x6100_ctl_set(ctl, X6100_REG_VFOA_BAND, 1);
		x6100_ctl_set(ctl, X6100_REG_VFOB_BAND, 1);
	}
	x6100_set_mode(ctl, 0, rx->mode);
	x6100_set_mode(ctl, 1, rx->mode);
	x6100_ctl_set(ctl, X6100_REG_VFOA_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOB_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOA_ATT, rx->att);
	x6100_ctl_set(ctl, X6100_REG_VFOB_ATT, rx->att);
	x6100_ctl_set(ctl, X6100_REG_VFOA_PRE, rx->pre);
	x6100_ctl_set(ctl, X6100_REG_VFOB_PRE, rx->pre);
	x6100_set_filter(ctl, rx->mode, low, high);
	x6100_ctl_set(ctl, X6100_REG_RXVOL, rx->mute ? 0 : (uint32_t) rx->vol);
	x6100_ctl_update(ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF, (uint32_t) rx->rfg);
	x6100_set_txpwr(ctl, rx->txpwr / 10.0f);
	x6100_cal_apply(ctl, cal, x6100_band_index(rx->freq));

	//
	// Tuner: stored network for this band, or straight through
	//
	if (rx->atu_on && rx->band >= 0 && rx->atu_valid[rx->band]) {
		x6100_atu_set_network(ctl, &rx->atu_net[rx->band]);
	} else {
		x6100_atu_t none = { 0, 0, 0 };

		x6100_atu_set_network(ctl, &none);
	}
}

//
// Register 12 from the TX state: PTT, tuner enable, tune
//

static void apply_flags(x6100_ctl_t *ctl, const rx_t *rx)
{
	x6100_ctl_set(ctl, X6100_REG_FLAGS, 0);
	x6100_set_ptt(ctl, rx->ptt);
	x6100_atu_enable(ctl, rx->atu_on || rx->tuning);
	x6100_atu_tune(ctl, rx->tuning);
}

//
// TX allowed here? Sets rx->warn when not
//


static bool tx_allowed(rx_t *rx)
{
#ifdef X6100_RX_ONLY
	rx->warn = "RX ONLY";
	return false;
#endif

	if (g_allow_oob)
		return true;

	if (rx->band < 0 || !(x6100_band_index(rx->freq) & 1)) {
		rx->warn = "OUT OF BAND";
		return false;
	}

	return true;
}

static void tx_drop(rx_t *rx, const char *why)
{
	rx->ptt = false;
	rx->ptt_latch = false;
	rx->tuning = false;

	if (why != NULL)
		rx->warn = why;
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

//
// Scope window: fixed window starts centred on the VFO, on a 1 kHz boundary;
// the sliding window follows the VFO when it leaves the window
//

static void scope_fix_start(rx_t *rx)
{
	rx->fix_lo = ((int64_t) rx->freq - rx->span / 2) / 1000 * 1000;
}

static void scope_follow(rx_t *rx)
{
	if (rx->scope != SCOPE_FIX_S)
		return;

	if ((int64_t) rx->freq < rx->fix_lo)
		rx->fix_lo = rx->freq;
	else if ((int64_t) rx->freq > rx->fix_lo + rx->span)
		rx->fix_lo = (int64_t) rx->freq - rx->span;
}

//
// Span (F3): full IQ width down to 3 kHz; the fixed window keeps its centre
//

static const int g_spans[] = { 96000, 48000, 24000, 12000, 6000, 3000 };

#define SPAN_COUNT	((int) (sizeof(g_spans) / sizeof(g_spans[0])))

static void scope_span_next(rx_t *rx)
{
	int i = 0;
	int64_t centre = rx->fix_lo + rx->span / 2;

	while (i < SPAN_COUNT && g_spans[i] != rx->span)
		i++;

	rx->span = g_spans[(i + 1) % SPAN_COUNT];

	if (rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S)
		rx->fix_lo = centre - rx->span / 2;
}

//
// Grid step: 1-2-5 sequence, about 8 lines across the window
//

static int grid_step(int span)
{
	static const int steps[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (span / steps[i] <= 10)
			return steps[i];
	}

	return 20000;
}

static bool on_key(rx_t *rx, int key)
{
	//
	// Any other key drops a latched TX or aborts a tune, and does nothing else;
	// F1-F5 are exempt (scope, meter, receive filter)
	//
	bool display_key = key == X6100_KEY_F1 || key == X6100_KEY_F2 || key == X6100_KEY_F3 || key == X6100_KEY_F4
		|| key == X6100_KEY_F5;

	if ((rx->ptt_latch || rx->tuning) && !display_key) {
		tx_drop(rx, rx->tuning ? "TUNE ABORTED" : NULL);
		return true;
	}

	//
	// Filter panel open: F5 cycles FIL1-3, MFK push switches BW / SFT; any other key
	// closes the panel (edits are kept) and then does its normal job
	//
	if (rx->fedit) {
		fgroup_t g = mode_group(rx->mode);

		rx->fedit_since = now_ms();

		if (key == X6100_KEY_F5) {
			rx->fil = (rx->fil + 1) % 3;
			return true;
		}

		if (key == X6100_KEY_MFK_PUSH) {
			rx->fedit_par = g == FG_AM || g == FG_FM ? 0 : !rx->fedit_par;
			return true;
		}

		rx->fedit = false;
		fil_save();
	}

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
	case X6100_KEY_ATU:
		rx->atu_on = !rx->atu_on;
		break;
	case X6100_KEY_F1:
		rx->scope = rx->scope == SCOPE_CENTER ? SCOPE_CENTER_S : SCOPE_CENTER;
		break;
	case X6100_KEY_F2:
		if (rx->scope != SCOPE_FIX && rx->scope != SCOPE_FIX_S) {
			scope_fix_start(rx);
			rx->scope = SCOPE_FIX;
		} else {
			rx->scope = rx->scope == SCOPE_FIX ? SCOPE_FIX_S : SCOPE_FIX;
		}

		break;
	case X6100_KEY_F3:
		scope_span_next(rx);
		break;
	case X6100_KEY_F4:
		rx->meter = (rx->meter + 1) % 3;
		break;
	case X6100_KEY_F5:
		rx->fil = (rx->fil + 1) % 3;
		break;
	case X6100_KEY_MFK_PUSH:
		if (rx->mfk_pwr)
			rx->txpwr = TXPWR_DEF;
		else
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
	return key == X6100_KEY_PRE || key == X6100_KEY_FST || key == X6100_KEY_ATU || key == X6100_KEY_MFK_PUSH
		|| key == X6100_KEY_F5;
}

static bool on_long_key(rx_t *rx, int key)
{
	if (rx->lock)
		return false;

	switch (key) {
	case X6100_KEY_PRE:
		rx->att = !rx->att;
		return true;
	case X6100_KEY_F5:
		rx->fedit = !rx->fedit;
		rx->fedit_since = now_ms();

		if (rx->fedit)
			rx->fedit_par = 0;
		else
			fil_save();

		return true;
	case X6100_KEY_MFK_PUSH:
		if (rx->fedit) {
			fil_default(mode_group(rx->mode), rx->fil);
			rx->fedit_since = now_ms();
		} else {
			rx->mfk_pwr = !rx->mfk_pwr;
		}

		return true;
	case X6100_KEY_ATU:
		if (rx->ptt || rx->tuning || !tx_allowed(rx))
			return true;

		rx->tuning = true;
		rx->tune_since = now_ms();
		rx->tune_seen_tx = false;
		rx->warn = NULL;
		return true;
	case X6100_KEY_FST:
		rx->step = rx->step == 1 ? 10 : 1;
		return true;
	default:
		return false;
	}
}

//
// PTT key: hold to talk; a tap shorter than TX_TAP_MS latches TX, the next
// press drops it
//

static bool on_ptt(rx_t *rx, int value)
{
	uint64_t t = now_ms();

	if (value == 1) {
		if (rx->ptt_latch || rx->tuning) {
			tx_drop(rx, NULL);
			return true;
		}

		if (!tx_allowed(rx))
			return true;

		rx->ptt = true;
		rx->ptt_since = t;
		rx->warn = NULL;
		return true;
	}

	if (value == 0 && rx->ptt && !rx->ptt_latch) {
		if (t - rx->ptt_since < TX_TAP_MS)
			rx->ptt_latch = true;
		else
			rx->ptt = false;

		return true;
	}

	return false;
}

static bool on_keypad(rx_t *rx, hold_t *hold, int key, int value)
{
	if (key == X6100_KEY_PTT)
		return on_ptt(rx, value);

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
		if (rx->fedit) {
			fgroup_t g = mode_group(rx->mode);

			if (rx->fedit_par == 0)
				fil_bw(g, rx->fil, value * X6100_DIR_MFK);
			else
				fil_sft(g, rx->fil, value * X6100_DIR_MFK);

			rx->fedit_since = now_ms();
		} else if (rx->mfk_pwr) {
			rx->txpwr = clampi(rx->txpwr + value * X6100_DIR_MFK, 1, TXPWR_MAX);
		} else {
			rx->rfg = clampi(rx->rfg + value * X6100_DIR_MFK * 2, 0, 100);
		}

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
#define INFO_COL_VOL	132
#define INFO_COL_RFG	228
#define INFO_COL_FIL	348
#define INFO_COL_VBAT	408
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
	VU_SCALE_ALC,
	VU_SCALE_SWR
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

	//
	// TX telemetry from the flow frames
	//
	bool tx;
	double pwr;
	double swr;
	int alc;		/* 0-100; 100 = no audio arriving */
	bool was_tx;
	int was_meter;

	//
	// Waterfall alignment state
	//
	scope_mode_t wf_scope;
	int wf_span;
	double wf_ref;		/* window left edge the waterfall rows are aligned to, Hz */
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

//
// Scope view: frequency window on screen. The baseband IQ is centred on the VFO
// and spans SPAN_HZ; window pixels outside that have no data.
//

typedef struct
{
	int64_t lo;
	int span;
	uint32_t vfo;
	int w;
} view_t;

static view_t scope_view(const rx_t *rx, int w)
{
	view_t v = { .span = rx->span, .vfo = rx->freq, .w = w };

	v.lo = rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S ? rx->fix_lo : (int64_t) rx->freq - rx->span / 2;
	return v;
}

static int spec_index(const view_t *v, int x)
{
	const int n = X6100_FLOW_SAMPLES;
	int64_t f = v->lo + (int64_t) x * v->span / v->w;
	int i = n / 2 + (int) floor((double) (f - (int64_t) v->vfo) * n / SPAN_HZ);

	if (i < 0 || i >= n)
		return -1;

	return X6100_FLOW_IQ_MIRRORED ? n - 1 - i : i;
}

//
// Value at pixel x, linearly interpolated between FFT bins (narrow spans show
// fewer bins than pixels); outside the IQ span: below the scale
//

static float bin_at(const float *arr, const view_t *v, int x)
{
	const int n = X6100_FLOW_SAMPLES;
	int64_t f = v->lo + (int64_t) x * v->span / v->w;
	double pos = n / 2 + (double) (f - (int64_t) v->vfo) * n / SPAN_HZ;
	int i0 = (int) floor(pos);
	double t = pos - i0;
	int i1 = i0 + 1;

	if (i0 < 0 || i1 >= n)
		return i0 >= 0 && i0 < n ? arr[X6100_FLOW_IQ_MIRRORED ? n - 1 - i0 : i0] : SPEC_DB_MIN - 20.0f;

	if (X6100_FLOW_IQ_MIRRORED) {
		i0 = n - 1 - i0;
		i1 = n - 1 - i1;
	}

	return (float) (arr[i0] * (1.0 - t) + arr[i1] * t);
}

static float spec_at(const screen_t *scr, const view_t *v, int x)
{
	return bin_at(scr->spec, v, x);
}

static float peak_at(const screen_t *scr, const view_t *v, int x)
{
	return bin_at(scr->peak, v, x);
}

static int f_to_x(const view_t *v, int64_t f)
{
	return (int) ((f - v->lo) * v->w / v->span);
}

//
// Receive passband relative to the dial frequency, in Hz
//

static void passband(const rx_t *rx, int *lo, int *hi)
{
	uint32_t low, high;

	mode_filter(rx->mode, rx->fil, &low, &high);

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
// SWR to meter fraction, matching the scale marks (1 .. 3 on the first 60 %)
//

static double swr_frac(double swr)
{
	static const double pts[][2] = {
		{ 1.0, 0.0 }, { 1.5, 0.22 }, { 2.0, 0.38 }, { 2.5, 0.49 }, { 3.0, 0.6 }, { 5.0, 0.76 }, { 10.0, 0.97 },
	};

	if (swr <= 1.0)
		return 0.0;

	for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
		if (swr <= pts[i][0])
			return pts[i - 1][1] + (swr - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]) * (pts[i][1] - pts[i - 1][1]);
	}

	return 0.97;
}

//
// Bar meter under the frequency: S scale in RX; in TX the meter chosen with F4
// (Po, SWR or ALC) on the upper scale; SWR scale below,
// with an SWR bar in TX. frac and peak are 0..1 of the bar width.
//

static void draw_sbar(x6100_lcd_t *lcd, double frac, double peak, bool tx, double swr, int meter)
{
	static const struct { double frac; const char *lbl; } s_marks[] = {
		{ 1.0 / 9 * 0.6, "1" }, { 3.0 / 9 * 0.6, "3" }, { 5.0 / 9 * 0.6, "5" },
		{ 7.0 / 9 * 0.6, "7" }, { 0.6, "9" },
	};
	static const struct { double frac; const char *lbl; } db_marks[] = {
		{ 0.6 + 20.0 / 60 * 0.4, "+20" }, { 0.6 + 40.0 / 60 * 0.4, "+40" }, { 1.0, "+60dB" },
	};
	static const struct { double frac; const char *lbl; } po_marks[] = {
		{ 0.2, "2" }, { 0.4, "4" }, { 0.6, "6" }, { 0.8, "8" }, { 1.0, "10W" },
	};
	static const struct { double frac; const char *lbl; } swr_marks[] = {
		{ 0.22, "1.5" }, { 0.38, "2" }, { 0.49, "2.5" }, { 0.6, "3" }, { 0.97, "INF" },
	};
	const uint32_t white = 0xF0F0F0, red = 0xE02828, grey = 0x9A9A9A;
	const int x0 = SBAR_X, x9 = SBAR_X + (int) (0.6 * SBAR_W), x1 = SBAR_X + SBAR_W;
	int fill = (int) ((frac > 1.0 ? 1.0 : frac < 0.0 ? 0.0 : frac) * SBAR_W);
	int pk = SBAR_X + (int) (peak * SBAR_W);

	x6100_lcd_fill(lcd, 0, SBAR_LBL_Y - 2, LEFT_W, SBAR_SWR_LBL_Y + 10 - SBAR_LBL_Y, 0x000000);

	if (tx && meter == 1) {
		//
		// SWR on the upper scale: white to 3, red above
		//
		x6100_lcd_text(lcd, 0, SBAR_LBL_Y + 4, 2, "SWR", white);
		x6100_lcd_hline(lcd, x0, x9, SBAR_SCALE_Y, white);
		x6100_lcd_fill(lcd, x9, SBAR_SCALE_Y - 1, x1 - x9 + 1, 3, red);

		for (size_t i = 0; i < sizeof(swr_marks) / sizeof(swr_marks[0]); i++) {
			int x = x0 + (int) (swr_marks[i].frac * SBAR_W);
			int tw = x6100_lcd_text_width(swr_marks[i].lbl, 2);

			x6100_lcd_vline(lcd, x, SBAR_SCALE_Y - 6, SBAR_SCALE_Y, swr_marks[i].frac > 0.6 ? red : white);
			x = i + 1 == sizeof(swr_marks) / sizeof(swr_marks[0]) ? x - tw : x - tw / 2;
			x6100_lcd_text(lcd, x, SBAR_LBL_Y, 2, swr_marks[i].lbl, swr_marks[i].frac > 0.6 ? red : white);
		}
	} else if (tx && meter == 2) {
		//
		// ALC on the upper scale: 0-100, ticks every 10
		//
		x6100_lcd_text(lcd, 0, SBAR_LBL_Y + 4, 2, "ALC", white);
		x6100_lcd_hline(lcd, x0, x1, SBAR_SCALE_Y, white);

		for (int a = 1; a <= 10; a++)
			x6100_lcd_vline(lcd, x0 + a * SBAR_W / 10, SBAR_SCALE_Y - (a % 5 ? 3 : 6), SBAR_SCALE_Y, white);

		x6100_lcd_text(lcd, x0 + SBAR_W / 2 - 12, SBAR_LBL_Y, 2, "50", white);
		x6100_lcd_text(lcd, x1 - 36, SBAR_LBL_Y, 2, "100", white);
	} else if (tx) {
		//
		// Po scale: 0-10 W linear, ticks every watt
		//
		x6100_lcd_text(lcd, 6, SBAR_LBL_Y + 4, 2, "PO", white);
		x6100_lcd_hline(lcd, x0, x1, SBAR_SCALE_Y, white);

		for (int w = 1; w <= 10; w++)
			x6100_lcd_vline(lcd, x0 + w * SBAR_W / 10, SBAR_SCALE_Y - (w % 2 ? 3 : 6), SBAR_SCALE_Y, white);

		for (size_t i = 0; i < sizeof(po_marks) / sizeof(po_marks[0]); i++) {
			int x = x0 + (int) (po_marks[i].frac * SBAR_W);
			int tw = x6100_lcd_text_width(po_marks[i].lbl, 2);

			x = i + 1 == sizeof(po_marks) / sizeof(po_marks[0]) ? x - tw : x - tw / 2;
			x6100_lcd_text(lcd, x, SBAR_LBL_Y, 2, po_marks[i].lbl, white);
		}
	} else {
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
	}

	//
	// Level bar between the scales, with peak hold marker
	//
	x6100_lcd_fill(lcd, x0, SBAR_BAR_Y, fill, SBAR_BAR_H, white);
	x6100_lcd_fill(lcd, pk - 1, SBAR_BAR_Y - 1, 3, SBAR_BAR_H + 2, 0x58B8F0);
	x6100_lcd_vline(lcd, x0, SBAR_SCALE_Y - 8, SBAR_SWR_Y + 3, white);

	//
	// SWR scale below: white to 3, double line above; in TX a bar on the scale
	//
	x6100_lcd_text(lcd, 4, SBAR_SWR_Y - 2, 1, "SWR", grey);

	if (tx) {
		int sw = (int) (swr_frac(swr) * SBAR_W);

		x6100_lcd_fill(lcd, x0, SBAR_SWR_Y - 4, sw, 3, swr > SWR_LIMIT ? red : 0x58B8F0);
	}

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

static void draw_vu(x6100_lcd_t *lcd, double needle, double marker, vu_scale_t scale, const char *line1,
	const char *line2)
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
	// RX: S scale, white to S9, heavy red above, small Po labels under it.
	// TX (PO meter): the main arc becomes the Po scale, 0-10 W in white
	//
	if (scale == VU_SCALE_PO) {
		vu_arc(lcd, 0.0, 1.0, VU_R_S, 2, white);

		for (int w = 0; w <= 10; w++) {
			double f = w / 10.0 * 0.97;

			vu_tick(lcd, f, VU_R_S, VU_R_S + (w % 2 ? 5 : 9), white);

			if (w % 2 == 0) {
				snprintf(s, sizeof(s), w == 10 ? "10W" : "%d", w);
				vu_label(lcd, f, VU_R_S + 20, 2, s, white);
			}
		}

		vu_label(lcd, -0.11, VU_R_S + 12, 2, "PO", white);
	} else {
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
	x6100_lcd_text(lcd, VU_X + VU_W - 8 - x6100_lcd_text_width(line1, 2), VU_Y + VU_H - 38, 2, line1, white);
	x6100_lcd_text(lcd, VU_X + VU_W - 8 - x6100_lcd_text_width(line2, 2), VU_Y + VU_H - 20, 2, line2, grey);

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
	case VU_SCALE_SWR:
		return VU_R_SWR;
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

//
// Filter width as text: "500", "1.50k", "2.40k"
//

static void fil_width_str(uint32_t hz, char *buf, size_t len)
{
	if (hz < 1000)
		snprintf(buf, len, "%u", hz);
	else
		snprintf(buf, len, "%u.%02uk", hz / 1000, hz % 1000 / 10);
}

//
// Filter edit panel (F5 long), IC-705 style, drawn over the spectrum (which is
// redrawn every frame, so the panel never smears into the waterfall)
//

static void draw_filter_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	static const char *gname[FG_COUNT] = { "SSB", "SSB-D", "CW", "AM", "FM" };
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 560, ph = 132;
	const uint32_t bg = 0x0C1622, edge = 0x40C0FF, act = 0x1E4A7A, dim = 0x7A8494, white = 0xF0F0F0;
	const uint32_t green = 0x3CC83C;
	fgroup_t g = mode_group(rx->mode);
	bool no_sft = g == FG_AM || g == FG_FM;
	uint32_t lo, hi;
	char s[48], w[12];

	mode_filter(rx->mode, rx->fil, &lo, &hi);

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, edge);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, edge);
	x6100_lcd_vline(lcd, px, py, py + ph - 1, edge);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, edge);

	//
	// Title and FIL number
	//
	snprintf(s, sizeof(s), "FILTER  %s", gname[g]);
	x6100_lcd_text(lcd, px + 10, py + 8, 2, s, white);
	snprintf(s, sizeof(s), "FIL%d", rx->fil + 1);
	x6100_lcd_fill(lcd, px + pw - 70, py + 5, 60, 20, act);
	x6100_lcd_text(lcd, px + pw - 64, py + 8, 2, s, white);

	//
	// BW and SFT boxes; the one MFK adjusts is highlighted
	//
	fil_width_str(hi - lo, w, sizeof(w));
	snprintf(s, sizeof(s), "BW %s", w);
	x6100_lcd_fill(lcd, px + 10, py + 32, 160, 22, rx->fedit_par == 0 ? act : bg);
	x6100_lcd_text(lcd, px + 16, py + 36, 2, s, rx->fedit_par == 0 ? white : dim);

	if (no_sft)
		snprintf(s, sizeof(s), "SFT --");
	else
		snprintf(s, sizeof(s), "SFT %+d", fil_sft_hz(g, rx->fil));

	x6100_lcd_fill(lcd, px + 180, py + 32, 160, 22, rx->fedit_par == 1 ? act : bg);
	x6100_lcd_text(lcd, px + 186, py + 36, 2, s, rx->fedit_par == 1 ? white : dim);

	//
	// Passband on a 0 .. max axis with a trapezoid, edges and centre labelled
	//
	{
		const int ax0 = px + 360, ax1 = px + pw - 16, ay = py + 92;
		const uint32_t span = g == FG_CW ? 2000 : FIL_MAX_HZ;
		int x0 = ax0 + (int) ((int64_t) lo * (ax1 - ax0) / span);
		int x1 = ax0 + (int) ((int64_t) hi * (ax1 - ax0) / span);

		x1 = x1 > ax1 ? ax1 : x1;
		x6100_lcd_hline(lcd, ax0, ax1, ay, dim);

		for (int r = 0; r < 28; r++) {
			int inset = (28 - r) * 6 / 28;
			int a = x0 + inset, b = x1 - inset;

			if (b > a)
				x6100_lcd_hline(lcd, a, b, ay - 28 + r, green);
		}

		snprintf(s, sizeof(s), "%u", lo);
		x6100_lcd_text(lcd, x0 - x6100_lcd_text_width(s, 1) / 2, ay + 6, 1, s, white);
		snprintf(s, sizeof(s), "%u", (lo + hi) / 2);
		x6100_lcd_text(lcd, (x0 + x1) / 2 - x6100_lcd_text_width(s, 1) / 2, ay + 16, 1, s, dim);
		snprintf(s, sizeof(s), "%u", hi);
		x6100_lcd_text(lcd, x1 - x6100_lcd_text_width(s, 1) / 2, ay + 6, 1, s, white);
	}

	//
	// Edges as text, and the controls
	//
	snprintf(s, sizeof(s), "%u - %u Hz", lo, hi);
	x6100_lcd_text(lcd, px + 16, py + 66, 2, s, white);
	x6100_lcd_text(lcd, px + 16, py + 96, 1, no_sft ? "MFK: BW    HOLD MFK: DEF" : "MFK: ADJUST    PUSH MFK: BW/SFT    HOLD MFK: DEF",
		dim);
	x6100_lcd_text(lcd, px + 16, py + 110, 1, "F5: FIL1-3    HOLD F5: CLOSE", dim);
}

//
// Shift a band of rows horizontally by dx pixels (positive = right), fill the gap black
//

static void wf_shift(x6100_lcd_t *lcd, int y0, int h, int dx)
{
	int w = lcd->w;

	if (dx >= w || dx <= -w) {
		x6100_lcd_fill(lcd, 0, y0, w, h, 0x000000);
		return;
	}

	for (int y = y0; y < y0 + h; y++) {
		uint32_t *row = lcd->pix + (size_t) y * (size_t) w;

		if (dx > 0) {
			memmove(row + dx, row, (size_t) (w - dx) * sizeof(uint32_t));
			memset(row, 0, (size_t) dx * sizeof(uint32_t));
		} else {
			memmove(row, row - dx, (size_t) (w + dx) * sizeof(uint32_t));
			memset(row + w + dx, 0, (size_t) -dx * sizeof(uint32_t));
		}
	}
}

static void draw_screen(screen_t *scr, const rx_t *rx, bool new_line)
{
	x6100_lcd_t *lcd = &scr->lcd;
	int W = lcd->w;
	char s[64];

	//
	// Status line, TX indicator, indicators right (TUNE while the tuner searches)
	//
	x6100_lcd_fill(lcd, 0, 0, W, TOP_H, SCR_BG);
	snprintf(s, sizeof(s), "%4s %5s AGC %4s", rx->band >= 0 ? g_bands[rx->band].name : "GEN",
		mode_name(rx->mode), agc_name(rx->agc));
	x6100_lcd_text(lcd, 12, TOP_Y, 3, s, SCR_ACCENT);

	//
	// TX indicator left of the frequency, Icom style: RX = red outline, red text on black;
	// TX (or tune) = red box, white text
	//
	{
		const int bx = 12, by = FREQ_Y + 8, bw = 54, bh = 29;
		const uint32_t red = 0xE02020;
		bool on = scr->tx || rx->tuning;
		int tw = x6100_lcd_text_width("TX", 3);

		x6100_lcd_fill(lcd, bx, by, bw, bh, on ? red : 0x000000);

		if (!on) {
			x6100_lcd_hline(lcd, bx, bx + bw - 1, by, red);
			x6100_lcd_hline(lcd, bx, bx + bw - 1, by + 1, red);
			x6100_lcd_hline(lcd, bx, bx + bw - 1, by + bh - 1, red);
			x6100_lcd_hline(lcd, bx, bx + bw - 1, by + bh - 2, red);
			x6100_lcd_vline(lcd, bx, by, by + bh - 1, red);
			x6100_lcd_vline(lcd, bx + 1, by, by + bh - 1, red);
			x6100_lcd_vline(lcd, bx + bw - 1, by, by + bh - 1, red);
			x6100_lcd_vline(lcd, bx + bw - 2, by, by + bh - 1, red);
		}

		x6100_lcd_text(lcd, bx + (bw - tw) / 2, by + 4, 3, "TX", on ? 0xFFFFFF : red);

		//
		// TX power setting under the box
		//
		snprintf(s, sizeof(s), "%d.%dW", rx->txpwr / 10, rx->txpwr % 10);
		x6100_lcd_text(lcd, bx + (bw - x6100_lcd_text_width(s, 2)) / 2, by + bh + 6, 2, s, SCR_FG);
	}

	//
	// Indicators in fixed slots, two rows, so nothing shifts when one changes
	//
	{
		static const struct { int x, y; } slot[4] = {
			{ 372, 2 }, { 432, 2 }, { 372, 18 }, { 432, 18 },
		};
		const char *txt[4] = {
			rx->tuning ? "TUNE" : rx->atu_on ? "ATU" : "", rx->att ? "ATT" : "",
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

	if (rx->mfk_pwr) {
		snprintf(s, sizeof(s), "PWR %d.%dW", rx->txpwr / 10, rx->txpwr % 10);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_ACCENT);
	} else {
		snprintf(s, sizeof(s), "RFG %d", rx->rfg);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_DIM);
	}

	snprintf(s, sizeof(s), "%.1fV", scr->vbat);
	x6100_lcd_text(lcd, INFO_COL_VBAT, INFO_Y, 2, s, SCR_DIM);
	snprintf(s, sizeof(s), "FIL%d", rx->fil + 1);
	x6100_lcd_text(lcd, INFO_COL_FIL, INFO_Y, 2, s, SCR_DIM);

	//
	// Meters: S in RX; Po (needle, bar) and SWR in TX, marker on the ALC arc
	// while the ALC is working, else on Po
	//
	if (scr->tx != scr->was_tx || rx->meter != scr->was_meter) {
		scr->needle = 0.0;
		scr->vu_peak = 0.0;
		scr->was_tx = scr->tx;
		scr->was_meter = rx->meter;
	}

	if (scr->tx) {
		static const char *names[] = { "PO", "SWR", "ALC" };
		char l1[16], l2[24];
		double po = scr->pwr / 10.0;
		int alc = scr->alc >= 100 ? 0 : scr->alc;	/* 100 = no audio arriving */
		double target, marker;
		vu_scale_t scale;

		switch (rx->meter) {
		case 1:
			target = swr_frac(scr->swr);
			scale = VU_SCALE_SWR;
			snprintf(l1, sizeof(l1), "SWR %.1f", scr->swr);
			snprintf(l2, sizeof(l2), "%.1fW ALC %d", scr->pwr, alc);
			break;
		case 2:
			target = alc / 100.0 * 0.34;
			scale = VU_SCALE_ALC;
			snprintf(l1, sizeof(l1), "ALC %d", alc);
			snprintf(l2, sizeof(l2), "%.1fW SWR %.1f", scr->pwr, scr->swr);
			break;
		default:
			target = po * 0.97;	/* 10 W at the 10W mark */
			scale = VU_SCALE_PO;
			snprintf(l1, sizeof(l1), "%.1fW", scr->pwr);
			snprintf(l2, sizeof(l2), "SWR %.1f", scr->swr);
			break;
		}

		vu_update(scr, target, now_ms());
		marker = scr->vu_peak;
		draw_sbar(lcd, rx->meter == 0 ? po : rx->meter == 1 ? swr_frac(scr->swr) : alc / 100.0,
			rx->meter == 2 ? scr->vu_peak / 0.34 : scr->vu_peak, true, scr->swr, rx->meter);
		draw_vu(lcd, scr->needle, marker, scale, l1, l2);
		x6100_lcd_text(lcd, VU_X + 8, VU_Y + VU_H - 20, 2, names[rx->meter], SCR_ACCENT);
	} else {
		char l1[16], l2[16];

		vu_update(scr, meter_frac(scr->sig_dbm), now_ms());
		draw_sbar(lcd, meter_frac(scr->sig_dbm), scr->vu_peak, false, 1.0, 0);
		s_reading(scr->sig_dbm, l1, sizeof(l1));
		snprintf(l2, sizeof(l2), "%.0fdBm", scr->sig_dbm);
		draw_vu(lcd, scr->needle, scr->vu_peak, VU_SCALE_S, l1, l2);
	}

	if (!scr->spec_valid)
		return;

	//
	// Spectrum in the scope window
	//
	view_t v = scope_view(rx, W);
	bool fixed = rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S;

	x6100_lcd_fill(lcd, 0, SPEC_Y, W, SPEC_H, 0x05080C);

	//
	// Waterfall alignment: a mode change clears it; in the scrolling modes a moved
	// window shifts it by the same number of pixels, so it stays frequency aligned
	//
	if (rx->scope != scr->wf_scope || rx->span != scr->wf_span) {
		x6100_lcd_fill(lcd, 0, WF_Y, W, lcd->h - WF_Y, 0x000000);
		scr->wf_scope = rx->scope;
		scr->wf_span = rx->span;
		scr->wf_ref = (double) v.lo;
	} else if (rx->scope == SCOPE_CENTER_S || rx->scope == SCOPE_FIX_S) {
		int dx = (int) lround((scr->wf_ref - (double) v.lo) * W / v.span);

		if (dx != 0) {
			wf_shift(lcd, WF_Y, lcd->h - WF_Y, dx);
			scr->wf_ref -= (double) dx * v.span / W;
		}
	} else {
		scr->wf_ref = (double) v.lo;
	}

	int pb_lo, pb_hi;

	passband(rx, &pb_lo, &pb_hi);

	{
		int x0 = f_to_x(&v, (int64_t) rx->freq + pb_lo);
		int x1 = f_to_x(&v, (int64_t) rx->freq + pb_hi);

		x0 = x0 < 0 ? 0 : x0;
		x1 = x1 >= W ? W - 1 : x1;

		if (x1 >= x0)
			x6100_lcd_fill(lcd, x0, SPEC_Y, x1 - x0 + 1, SPEC_H, 0x183820);
	}

	//
	// Frequency grid: relative kHz around the VFO in the centre modes, absolute
	// MHz on round frequencies in the fixed modes
	//
	if (fixed) {
		int step = grid_step(v.span);

		for (int64_t g = (v.lo / step + 1) * step; g < v.lo + v.span; g += step) {
			char lbl[16];
			int gx = f_to_x(&v, g);

			x6100_lcd_vline(lcd, gx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0x1A2430);

			if (step >= 1000)
				snprintf(lbl, sizeof(lbl), "%d.%03d", (int) (g / 1000000), (int) (g / 1000 % 1000));
			else
				snprintf(lbl, sizeof(lbl), "%d.%04d", (int) (g / 1000000), (int) (g / 100 % 10000));

			x6100_lcd_text(lcd, gx - x6100_lcd_text_width(lbl, 1) / 2, SPEC_Y + 4, 1, lbl, SCR_DIM);
		}
	} else {
		int step = grid_step(v.span);

		for (int hz = -(v.span / 2) / step * step; hz <= v.span / 2; hz += step) {
			char lbl[12];
			int gx = f_to_x(&v, (int64_t) rx->freq + hz);

			x6100_lcd_vline(lcd, gx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0x1A2430);

			if (hz == 0 || gx < LBL_W + 12 || gx > W - 24)
				continue;

			if (step >= 1000)
				snprintf(lbl, sizeof(lbl), "%+d", hz / 1000);
			else
				snprintf(lbl, sizeof(lbl), "%+.1f", hz / 1000.0);

			x6100_lcd_text(lcd, gx - x6100_lcd_text_width(lbl, 2) / 2, SPEC_Y + 4, 2, lbl, SCR_DIM);
		}
	}

	for (int db = (int) SPEC_DB_MAX - GRID_DB; db > (int) SPEC_DB_MIN; db -= GRID_DB) {
		int gy = SPEC_Y + SPEC_H - 1 - (int) (spec_norm((float) db) * (float) (SPEC_H - 1));

		x6100_lcd_hline(lcd, 0, W - 1, gy, 0x1A2430);
	}

	int prev_y = -1;

	for (int x = 0; x < W; x++) {
		int y, py;

		if (spec_index(&v, x) < 0) {
			prev_y = -1;
			continue;
		}

		y = spec_y(spec_at(scr, &v, x));
		py = spec_y(peak_at(scr, &v, x));
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

	//
	// VFO cursor; in a fixed window an arrow at the edge when the VFO is outside it
	//
	{
		int cx = f_to_x(&v, rx->freq);

		if (cx >= 0 && cx < W)
			x6100_lcd_vline(lcd, cx, SPEC_Y, SPEC_Y + SPEC_H - 1, SCR_WARN);
		else if (cx < 0)
			x6100_lcd_text(lcd, LBL_W + 2, SPEC_Y + SPEC_H / 2 - 7, 2, "<", SCR_WARN);
		else
			x6100_lcd_text(lcd, W - 14, SPEC_Y + SPEC_H / 2 - 7, 2, ">", SCR_WARN);
	}

	//
	// Scope mode, top right of the spectrum
	//
	{
		static const char *names[] = { "CENTER", "SCROLL-C", "FIX", "SCROLL-F" };
		char m[24];
		int tw;

		if (rx->span % 2000 == 0)
			snprintf(m, sizeof(m), "%s +-%dk", names[rx->scope], rx->span / 2000);
		else
			snprintf(m, sizeof(m), "%s +-%d.%dk", names[rx->scope], rx->span / 2000, rx->span / 200 % 10);

		tw = x6100_lcd_text_width(m, 2);

		x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 20, tw + 8, 18, 0x000000);
		x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 22, 2, m, SCR_ACCENT);
	}

	//
	// Filter label under the scope mode: FILn and its width
	//
	{
		uint32_t lo, hi;
		char m[24], w[12];
		int tw;

		mode_filter(rx->mode, rx->fil, &lo, &hi);
		fil_width_str(hi - lo, w, sizeof(w));
		snprintf(m, sizeof(m), "FIL%d %s", rx->fil + 1, w);
		tw = x6100_lcd_text_width(m, 2);
		x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 40, tw + 8, 18, 0x000000);
		x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 42, 2, m, rx->fedit ? SCR_WARN : SCR_ACCENT);
	}

	if (rx->fedit)
		draw_filter_panel(lcd, rx);

	//
	// Last TX refusal or trip, until the next TX
	//
	if (rx->warn != NULL) {
		int tw = x6100_lcd_text_width(rx->warn, 2);

		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 22, tw + 8, 20, 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 25, 2, rx->warn, SCR_WARN);
	}

	//
	// Waterfall, newest line on top; no data outside the IQ span stays black
	//
	if (new_line) {
		x6100_lcd_scroll_down(lcd, WF_Y, lcd->h - WF_Y);

		for (int x = 0; x < W; x++) {
			uint32_t c = spec_index(&v, x) < 0 ? 0x000000
				: wf_colour((spec_at(scr, &v, x) - (scr->floor_db - WF_BELOW_DB)) / WF_RANGE_DB);

			x6100_lcd_fill(lcd, x, WF_Y, 1, 1, c);
		}
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
	static const struct { const char *name; x6100_agc_t agc; } agcs[] = {
		{ "off", X6100_AGC_OFF }, { "slow", X6100_AGC_SLOW }, { "fast", X6100_AGC_FAST }, { "auto", X6100_AGC_AUTO },
	};
	char *end;

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcasecmp(t, modes[i].name) == 0) {
			rx->mode = modes[i].mode;
			return true;
		}
	}

	for (size_t i = 0; i < sizeof(agcs) / sizeof(agcs[0]); i++) {
		if (strcasecmp(t, agcs[i].name) == 0) {
			rx->agc = agcs[i].agc;
			return true;
		}
	}

	if (strcasecmp(t, "center") == 0 || strcasecmp(t, "scrollc") == 0) {
		rx->scope = strcasecmp(t, "center") == 0 ? SCOPE_CENTER : SCOPE_CENTER_S;
		return true;
	}

	if (strcasecmp(t, "fix") == 0 || strcasecmp(t, "scrollf") == 0) {
		if (rx->scope != SCOPE_FIX && rx->scope != SCOPE_FIX_S)
			scope_fix_start(rx);

		rx->scope = strcasecmp(t, "fix") == 0 ? SCOPE_FIX : SCOPE_FIX_S;
		return true;
	}

	if (strncasecmp(t, "fil", 3) == 0 && t[3] >= '1' && t[3] <= '3' && t[4] == '\0') {
		rx->fil = t[3] - '1';
		return true;
	}

	if (strcasecmp(t, "att") == 0 || strcasecmp(t, "noatt") == 0) {
		rx->att = strcasecmp(t, "att") == 0;
		return true;
	}

	if (strcasecmp(t, "pre") == 0 || strcasecmp(t, "nopre") == 0) {
		rx->pre = strcasecmp(t, "pre") == 0;
		return true;
	}

	if (isdigit((unsigned char) t[0])) {
		double v = strtod(t, &end);

		if (*end == 'k' || *end == 'K')
			v *= 1e3;
		else if (*end == 'm' || *end == 'M' || strchr(t, '.') != NULL)
			v *= 1e6;
		else if (*end != '\0')
			return false;

		//
		// Outside the receiver range is a typo (e.g. a stray "20"), not a wish
		// for the band edge: refuse instead of clamping
		//
		if (v < FREQ_MIN || v > FREQ_MAX)
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
	bool want_pwr = false;

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

		if (want_pwr) {
			double w = atof(t);

			want_pwr = false;

			if (w >= 0.1 && w <= TXPWR_MAX / 10.0) {
				rx->txpwr = (int) lround(w * 10.0);
				changed = true;
			} else {
				fprintf(stderr, "\npwr must be 0.1 to %d W\n", TXPWR_MAX / 10);
			}

			continue;
		}

		if (strcasecmp(t, "pwr") == 0) {
			want_pwr = true;
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
	rx_t rx = { .vol = 20, .rfg = 100, .step = 100, .agc = (x6100_agc_t) -1, .mode = X6100_MODE_USB,
		.txpwr = TXPWR_DEF, .scope = SCOPE_CENTER, .span = SPAN_HZ };
	int rc;

	fil_load();

	for (int i = 0; i < BAND_COUNT; i++)
		rx.band_freq[i] = g_bands[i].def;

	set_freq(&rx, 14074000);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--oob") == 0) {
			g_allow_oob = true;
			fprintf(stderr, "warning: --oob, band check off: you are responsible for where you transmit\n");
			continue;
		}

		if (!console_token(&rx, argv[i])) {
			fprintf(stderr, "usage: %s [--oob] [freq] [usb|lsb|cw|cwr|am|nfm] [off|slow|fast|auto] [att|noatt] [pre|nopre]\n", argv[0]);
			return 1;
		}
	}

	//
	// AGC per mode group; an AGC given on the command line applies to the start mode
	//
	for (int g = 0; g < FG_COUNT; g++)
		rx.agc_group[g] = g == FG_AM || g == FG_FM ? X6100_AGC_SLOW : X6100_AGC_AUTO;

	rx.agc_last_group = mode_group(rx.mode);

	if ((int) rx.agc < 0)
		rx.agc = rx.agc_group[rx.agc_last_group];
	else
		rx.agc_group[rx.agc_last_group] = rx.agc;

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

#ifdef X6100_RX_ONLY
	printf("x6100_rx (receive only): PWR or Ctrl-C to quit\n");
#else
	printf("x6100_trx: PWR or Ctrl-C to quit\n");
#endif
	show(&rx);

	uint64_t last_refresh = now_ms();
	uint64_t last_meter = last_refresh;
	uint64_t last_disp = last_refresh;
	double level = 0.0;
	int level_n = 0;
	bool new_frame = false;
	bool dirty = false;
	hold_t hold = { .key = -1 };
	uint64_t swr_since = 0;

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
			//
			// Mode group changed: recall its AGC; otherwise remember the current one
			//
			{
				int g = mode_group(rx.mode);

				if (g != rx.agc_last_group) {
					rx.agc = rx.agc_group[g];
					rx.agc_last_group = g;
				} else {
					rx.agc_group[g] = rx.agc;
				}
			}

			scope_follow(&rx);
			apply(&ctl, &cal, &rx);
			show(&rx);
			dirty = true;
		}

		uint64_t t = now_ms();

		//
		// Filter panel closes after 10 s without activity
		//
		if (rx.fedit && t - rx.fedit_since >= 10000) {
			rx.fedit = false;
			fil_save();
			dirty = true;
		}

		//
		// TX supervision: band edge (VFO moved while keyed), timeout
		//
		if ((rx.ptt || rx.tuning) && !tx_allowed(&rx)) {
			tx_drop(&rx, NULL);
			changed = true;
		}

		if (rx.ptt && t - rx.ptt_since >= TX_TIMEOUT_MS) {
			tx_drop(&rx, "TX TIMEOUT");
			changed = true;
		}

		if (rx.tuning && t - rx.tune_since >= TUNE_TIMEOUT_MS) {
			tx_drop(&rx, "TUNE TIMEOUT");
			changed = true;
		}

		if (changed) {
			show(&rx);
			dirty = true;
		}

		apply_flags(&ctl, &rx);

		//
		// Full refresh resends register 17 too: not while the tuner is searching
		//
		if (t - last_refresh >= REFRESH_MS && !rx.tuning) {
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
			bool tx = (frame.flags & X6100_FLOW_TX) != 0;
			double swr = frame.vswr * 0.1;

			if (tx != scr.tx)
				dirty = true;

			scr.tx = tx;
			scr.pwr = frame.tx_power * 0.1;
			scr.swr = swr < 1.0 ? 1.0 : swr;
			scr.alc = frame.alc_level;
			scr.vbat = frame.vbat * 0.1;

			//
			// SWR trip on our own TX (not while tuning, where it sweeps)
			//
			if (tx && rx.ptt && swr > SWR_LIMIT) {
				if (swr_since == 0) {
					swr_since = t;
				} else if (t - swr_since >= SWR_TRIP_MS) {
					tx_drop(&rx, "SWR HIGH");
					apply_flags(&ctl, &rx);
					x6100_ctl_sync(&ctl);
					dirty = true;
				}
			} else {
				swr_since = 0;
			}

			//
			// Tune done: flow flag set after the tune actually ran (the flag is
			// stale from the previous tune until the baseband starts)
			//
			if (rx.tuning) {
				rx.tune_seen_tx |= tx;

				if ((frame.flags & X6100_FLOW_ATU) && (rx.tune_seen_tx || t - rx.tune_since > 500)) {
					rx.tuning = false;

					if (rx.band >= 0) {
						x6100_atu_from_frame(&frame, &rx.atu_net[rx.band]);
						rx.atu_valid[rx.band] = true;
						printf("\nATU tuned: L=%u C=%u S=%u\n", rx.atu_net[rx.band].l, rx.atu_net[rx.band].c,
							rx.atu_net[rx.band].s);
					}

					rx.atu_on = true;
					apply(&ctl, &cal, &rx);
					apply_flags(&ctl, &rx);
					dirty = true;
				}
			}

			level += iq_level_db(&frame);
			level_n++;

			if (scr.on) {
				spectrum_update(&scr, &frame);
				scr.sig_dbm = passband_dbm(&scr, &rx);
				new_frame = true;
			}
		}

		if (t - last_meter >= METER_MS && level_n > 0) {
			scr.meter_db = level / level_n;
			if (scr.tx) {
				//
				// TX: one line per update, so the readings stay on screen
				//
				printf("\nTX %4.1f W  swr %3.1f  alc %3d  (raw pwr %u)\n", scr.pwr, scr.swr, scr.alc,
					(unsigned) frame.tx_power);
			} else {
				show(&rx);
				printf("iq %6.1f dB  vbat %.1f", scr.meter_db, frame.vbat * 0.1);
			}
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

	//
	// Drop TX before anything else; keep filter edits
	//
	tx_drop(&rx, NULL);
	apply_flags(&ctl, &rx);
	x6100_ctl_sync(&ctl);
	fil_save();

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
