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
 *   F1             scope CENTER <-> SCROLL-C, or FIX <-> SCROLL-F when in FIX
 *   F1 long        scope CENTER family <-> FIX family (fixed window; SCROLL-F slides at the edges)
 *   F2             scope span +-48 / 24 / 12 / 6 / 3 / 1.5 kHz
 *   F3             free
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
 *   x6100_trx --no-limit ...          tune 10 kHz - 100 MHz instead of 0.5 - 56 MHz (baseband probe)
 *   type a line + Enter while running:
 *     7074000 | 7.074 | 7074k         frequency (Hz, MHz with '.', or kHz with 'k')
 *     usb lsb cw cwr am nfm           mode
 *     off slow fast auto              AGC
 *     att noatt pre nopre             attenuator / preamp
 *     center scrollc fix scrollf      spectrum scope mode (also F1 / F1 long)
 *     fil1 fil2 fil3                  receive filter
 *     step 1|10|100|1000|5000         tuning step
 *     pwr 0.1-10                      TX power in W
 *     vol 0-100                       volume (also unmutes)
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

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "x6100_civ.h"
#include "x6100_keys.h"
#include "x6100_lcd.h"
#include "x6100_pcm.h"

#ifndef X6100_REV
#define X6100_REV	"dev"	/* build.sh passes the git revision */
#endif

static bool g_no_limit;	/* --no-limit: tune 10 kHz - 100 MHz (test what the baseband accepts) */
static bool g_probe;	/* --probe: print flow flags and peak IQ (overflow hunt) */
static bool g_cat_log;	/* --cat-log: print CI-V frames in and out */
#define FREQ_MIN	(g_no_limit ? 10000u : 500000u)	/* --no-limit: probe the baseband */
#define FREQ_MAX	(g_no_limit ? 100000000u : 56000000u)
#define POLL_MS		5
#define LONG_PRESS_MS	600
#define PWR_QUIT_MS	1500	/* PWR: shorter = display on / off; 1.5-4 s = quit on release */
#define PWR_OFF_MS	4000	/* PWR held this long: shut the radio down (as the stock UI) */
#define REFRESH_MS	1000
#define METER_MS	500
#define TX_TIMEOUT_MS	180000
#define TX_TAP_MS	400
#define SWR_LIMIT	3.0
#define SWR_TRIP_MS	300
#define TUNE_TIMEOUT_MS	20000
#define TUNE_SHOW_MS	4000	/* TUNED result stays on screen */
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

	//
	// Baseband noise reduction and noise blanker (register 23)
	//
	bool nr;
	bool nb;
	int nr_lvl;		/* 0-60 */
	int nb_lvl;		/* 0-100 */
	int nb_w;		/* 0-100 */
	bool dnf;		/* manual notch (register 24) */
	int dnf_f;		/* 100-3000 Hz */
	int dnf_w;		/* 10-100 Hz */
	bool dedit;		/* DSP panel open (DFL long) */
	int dedit_par;		/* selected value, index into g_dsp */
	uint64_t dedit_since;	/* last panel activity, for the idle close */
	uint32_t band_freq[11];

	//
	// VFO B: the other VFO (A/B key swaps, A/B long = A=B); on_b: B is the one in use
	//
	uint32_t b_freq;
	x6100_mode_t b_mode;
	int b_fil;
	bool on_b;
	bool bb_tx;		/* baseband reports TX (any PTT, hand mic included) */
	bool split;		/* RX on the VFO in use, TX on the other (V/M long) */
	int rit_hz;		/* RIT / XIT offset, Hz (Icom: one offset, RIT and XIT on / off) */
	bool rit;
	bool xit;
	bool tedit;		/* RIT / XIT panel (AGC long) */
	int tedit_par;		/* 0 offset, 1 RIT, 2 XIT */
	uint64_t tedit_since;

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
	uint64_t tune_done;	/* tune finished at (ms), TUNED shown for TUNE_SHOW_MS */
	uint32_t tune_ms;	/* how long the last tune took */
	double tune_pwr;	/* last PO and SWR seen while tuning */
	double tune_swr;
	x6100_atu_t atu_net[11];	/* tuned network per band */
	bool atu_valid[11];
	const char *warn;	/* last TX refusal / trip reason, shown until next TX */

	//
	// Spectrum scope
	//
	scope_mode_t scope;
	int64_t fix_lo;		/* left edge of the fixed window, Hz */
	int span;		/* window width, Hz */
	bool ascope;		/* F3: audio scope instead of the RF scope */
	bool asplit;		/* F3 long: RF scope left, audio scope right */
	int arange;		/* audio scope range: 0 = 0-3 kHz, 1 = 0-6 kHz */
	int rref;		/* RF scope reference level, dB (+ = trace up) */
	int aref;		/* audio scope reference level, dB */
	bool redit;		/* F2 long: MFK sets the scope reference level */
	bool redit_af;		/* reference edit acts on the audio scope */
	uint64_t redit_since;	/* last reference edit activity, for the idle close */
	bool gedit;		/* GEN: settings panel */
	int gedit_par;		/* selected row, index into g_gen */
	uint64_t gedit_since;
	bool ginfo;		/* GEN long: status page */
	uint64_t ginfo_since;

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

//
// Transmit allocations per region (Hz). The TX check and the OOB marker use the
// selected region (conf token region); g_bands above stays the band memory and
// BAND key range. Sources: ITU RR / IARU band plans; NL: Regeling gebruik van
// frequentieruimte zonder vergunning en met meldingsplicht (stcrt 2015-3772,
// 2021-31799) for F registration, 60 m per WRC-15.
//

typedef struct
{
	uint32_t low;
	uint32_t high;
} seg_t;

typedef struct
{
	const char *name;
	seg_t seg[12];
} region_t;

static const region_t g_regions[] = {
	{ "r1", {
		{ 1810000, 2000000 }, { 3500000, 3800000 }, { 5351500, 5366500 }, { 7000000, 7200000 },
		{ 10100000, 10150000 }, { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 },
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 52000000 } } },
	{ "r2", {
		{ 1800000, 2000000 }, { 3500000, 4000000 }, { 5351500, 5366500 }, { 7000000, 7300000 },
		{ 10100000, 10150000 }, { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 },
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 54000000 } } },
	{ "r3", {
		{ 1800000, 2000000 }, { 3500000, 3900000 }, { 5351500, 5366500 }, { 7000000, 7300000 },
		{ 10100000, 10150000 }, { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 },
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 54000000 } } },
	{ "nl", {
		{ 1810000, 1880000 }, { 3500000, 3800000 }, { 5351500, 5366500 }, { 7000000, 7200000 },
		{ 10100000, 10150000 }, { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 },
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 52000000 } } },
};

#define REGION_COUNT	((int) (sizeof(g_regions) / sizeof(g_regions[0])))

static int g_region;	/* index into g_regions; conf token region, default r1 */

// Inside a transmit allocation of the selected region
static bool in_allocation(uint32_t hz)
{
	const region_t *r = &g_regions[g_region];

	for (int i = 0; i < 12 && r->seg[i].high != 0; i++) {
		if (hz >= r->seg[i].low && hz <= r->seg[i].high)
			return true;
	}

	return false;
}

static const char *g_input_names[IN_COUNT] = {
	X6100_INPUT_KEYPAD, X6100_INPUT_VFO, X6100_INPUT_VOL, X6100_INPUT_MFK, X6100_INPUT_POWER,
};

static volatile sig_atomic_t g_stop;
static bool g_poweroff;	/* PWR held 4 s: power the radio off after the clean exit */
static int g_cat_baud;		/* CI-V baud, 0 = no CAT: conf token cat, or --cat */
static char g_ntp_server[64];	/* conf token ntp <server> | off; empty = ntp.conf or pool.ntp.org */
static int g_bright;		/* backlight level, % (10-100); 0 = leave as the OS set it */
static unsigned g_conf_set;	/* GEN rows that x6100_trx.conf fixes (bit = row): shown CONF, locked */
static bool g_conf_reading;	/* conf_load of CONF_PATH in progress */
static char g_ntp_msg[64] = "not run";	/* last NTP result, for the status page */
static char g_toast[24];		/* short confirmation over the scope (VFO A / B, A=B) */
static uint64_t g_toast_until;	/* ms; shown while now_ms() is before this */

enum { GEN_BRIGHT, GEN_CAT, GEN_NTP, GEN_WIFI, GEN_WNET, GEN_BT, GEN_BTPAIR, GEN_BTDEV, GEN_ROWS };	/* GEN rows */

static void gen_adjust(int row, int dir, bool def);

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

// Short confirmation over the scope for TOAST_MS (VFO A / B, A=B)
#define TOAST_MS	1500

static void toast(const char *msg)
{
	snprintf(g_toast, sizeof(g_toast), "%s", msg);
	g_toast_until = now_ms() + TOAST_MS;
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
#define FIL_MAX_HZ	6000	/* baseband audio runs at 12 kHz; x6100_gui allows 6000 too */

static const char *g_fg_name[FG_COUNT] = { "ssb", "ssbd", "cw", "am", "fm" };

static const uint32_t g_fil_def[FG_COUNT][3][2] = {
	{ { 100, 3000 }, { 300, 2700 }, { 300, 2100 } },	/* SSB */
	{ { 100, 3000 }, { 300, 2700 }, { 300, 2100 } },	/* SSB-D */
	{ { 100, 1300 }, { 450, 950 }, { 575, 825 } },		/* CW, centred on 700 Hz */
	{ { 0, 3000 }, { 0, 2500 }, { 0, 2000 } },		/* AM: +-3 kHz like x6100_gui (2900); less 12 kHz image sizzle */
	{ { 0, 3000 }, { 0, 2500 }, { 0, 2000 } },		/* FM */
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
// RX and TX frequency: RX = the VFO in use + RIT; TX = the other VFO with split,
// else the VFO in use, + XIT (one offset for both, as on Icom radios)
//

static uint32_t rx_hz(const rx_t *rx)
{
	return (uint32_t) ((int64_t) rx->freq + (rx->rit ? rx->rit_hz : 0));
}

static uint32_t tx_hz(const rx_t *rx)
{
	return (uint32_t) ((int64_t) (rx->split ? rx->b_freq : rx->freq) + (rx->xit ? rx->rit_hz : 0));
}

static x6100_mode_t tx_mode(const rx_t *rx)
{
	return rx->split ? rx->b_mode : rx->mode;
}

//
// Push the receiver state into the register shadow. While keyed (or while the
// baseband has not yet seen the PTT drop: the register sync writes in index order,
// frequency before flags) both VFO registers carry the TX frequency and mode;
// otherwise the RX ones. Called on every pass of the main loop.
//

static void apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, const rx_t *rx)
{
	uint32_t low, high;
	bool keyed = rx->ptt || rx->tuning || rx->bb_tx || (ctl->sent[X6100_REG_FLAGS] & (X6100_FLAG_IPTT | X6100_FLAG_ATU_TUNE));
	uint32_t hz = keyed ? tx_hz(rx) : rx_hz(rx);
	x6100_mode_t mode = keyed ? tx_mode(rx) : rx->mode;

	mode_filter(mode, keyed && rx->split ? rx->b_fil : rx->fil, &low, &high);

	x6100_set_freq(ctl, 0, hz);
	x6100_set_freq(ctl, 1, hz);

	//
	// Registers 0 / 6: TX enable per VFO ("ham band"), normally set from the band
	// plan; --oob sets it everywhere, as the stock UI does with fullband-tx=enable.
	// The band index in register 13 stays the real one (filters, calibration).
	//
	if (g_allow_oob) {
		x6100_ctl_set(ctl, X6100_REG_VFOA_BAND, 1);
		x6100_ctl_set(ctl, X6100_REG_VFOB_BAND, 1);
	}
	x6100_set_mode(ctl, 0, mode);
	x6100_set_mode(ctl, 1, mode);
	x6100_ctl_set(ctl, X6100_REG_VFOA_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOB_AGC, rx->agc);
	x6100_ctl_set(ctl, X6100_REG_VFOA_ATT, rx->att);
	x6100_ctl_set(ctl, X6100_REG_VFOB_ATT, rx->att);
	x6100_ctl_set(ctl, X6100_REG_VFOA_PRE, rx->pre);
	x6100_ctl_set(ctl, X6100_REG_VFOB_PRE, rx->pre);
	x6100_ctl_set(ctl, X6100_REG_NR_NB, X6100_R23(rx->nr_lvl, rx->nb_w, rx->nb_lvl, rx->nr, rx->nb));
	x6100_ctl_set(ctl, X6100_REG_DNF, X6100_R24(rx->dnf_f, rx->dnf_w, rx->dnf));
	x6100_set_filter(ctl, mode, low, high);
	x6100_ctl_set(ctl, X6100_REG_RXVOL, rx->mute ? 0 : (uint32_t) rx->vol);
	x6100_ctl_update(ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF, (uint32_t) rx->rfg);
	x6100_set_txpwr(ctl, rx->txpwr / 10.0f);
	x6100_cal_apply(ctl, cal, x6100_band_index(hz));

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

	// Selected region's allocation, and a band the baseband transmits in
	if (!in_allocation(tx_hz(rx)) || rx->band < 0 || !(x6100_band_index(tx_hz(rx)) & 1)) {
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

//
// VFO A / B: the VFO in use is rx->freq / mode / fil, the other one is kept in
// rx->b_*; swapping exchanges them (A/B key, CI-V 07 00 / 01 / B0)
//

static void vfo_swap(rx_t *rx)
{
	uint32_t f = rx->freq;
	x6100_mode_t m = rx->mode;
	int fil = rx->fil;

	set_freq(rx, rx->b_freq);
	rx->mode = rx->b_mode;
	rx->fil = rx->b_fil;
	rx->b_freq = f;
	rx->b_mode = m;
	rx->b_fil = fil;
	rx->on_b = !rx->on_b;
	toast(rx->on_b ? "VFO B" : "VFO A");
}

// The other VFO takes over the one in use (A/B long, CI-V 07 A0)
static void vfo_equal(rx_t *rx)
{
	rx->b_freq = rx->freq;
	rx->b_mode = rx->mode;
	rx->b_fil = rx->fil;
	toast(rx->on_b ? "B COPIED TO A" : "A COPIED TO B");	/* the font has no = */
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
//
// AM broadcast on LW / MW: the 9 / 10 kHz channel steps are offered only there.
// The VFO steps on multiples of the step from 0 Hz, which is the channel raster
// (LW 153 kHz = 17 x 9, MW 531 = 59 x 9 kHz; US MW 530 = 53 x 10 kHz).
//

static bool am_broadcast(const rx_t *rx)
{
	return rx->mode == X6100_MODE_AM && ((rx->freq >= 148500 && rx->freq <= 283500)
		|| (rx->freq >= 520000 && rx->freq <= 1710000));
}

//
// 60 m channels: on 60 m, FST also offers CH, and the VFO knob then steps through
// the region's channel list (dial frequencies, USB).
//   r2: the US channels (FCC; national, outside the WRC-15 segment the TX check
//       of r2 refuses them unless oob).
//   r1 / r3 / nl: no official channels in the WRC-15 segment (5351.5-5366.5 kHz):
//       the segment edge for CW, the FT8 frequency and a 3 kHz raster for USB,
//       as a convenience. The band plan, not this list, says what goes where.
//

#define STEP_CH		3000000	/* FST "CH": sentinel step, never used as a raster */

static const uint32_t g_ch60_r2[] = { 5330500, 5346500, 5357000, 5371500, 5403500 };
static const uint32_t g_ch60_wrc[] = { 5351500, 5354000, 5357000, 5360000, 5363000 };

static bool on_60m(const rx_t *rx)
{
	return rx->freq >= 5250000 && rx->freq <= 5450000;
}

static void ch60_step(rx_t *rx, int dir)
{
	bool r2 = strcmp(g_regions[g_region].name, "r2") == 0;
	const uint32_t *ch = r2 ? g_ch60_r2 : g_ch60_wrc;
	int n = r2 ? (int) (sizeof(g_ch60_r2) / sizeof(g_ch60_r2[0])) : (int) (sizeof(g_ch60_wrc) / sizeof(g_ch60_wrc[0]));
	int i = 0;

	// Nearest channel at or above (dir > 0: strictly above) the current frequency
	if (dir > 0) {
		while (i < n && ch[i] <= rx->freq)
			i++;

		i = i < n ? i : 0;
	} else {
		i = n - 1;

		while (i >= 0 && ch[i] >= rx->freq)
			i--;

		i = i >= 0 ? i : n - 1;
	}

	set_freq(rx, ch[i]);
}


//
// Baseband DSP values edited in the DSP panel (DFL long): NR level, NB level and
// width (register 23), manual notch frequency and width (register 24). Ranges as
// x6100_gui; defaults NB 10 / 10 and notch 1000 / 50 Hz as the stock start values,
// NR 20 of 60.
//

typedef struct
{
	const char *name;
	int min;
	int max;
	int step;
	int def;
	const char *unit;
} dsp_par_t;

static const dsp_par_t g_dsp[] = {
	{ "NR LEVEL", 0, 60, 1, 20, "" },
	{ "NB LEVEL", 0, 100, 1, 10, "" },
	{ "NB WIDTH", 0, 100, 1, 10, "" },
	{ "NOTCH", 100, 3000, 10, 1000, "HZ" },
	{ "NOTCH WIDTH", 10, 100, 5, 50, "HZ" },
};

#define DSP_PARS	((int) (sizeof(g_dsp) / sizeof(g_dsp[0])))

static int *dsp_value(rx_t *rx, int i)
{
	switch (i) {
	case 0:
		return &rx->nr_lvl;
	case 1:
		return &rx->nb_lvl;
	case 2:
		return &rx->nb_w;
	case 3:
		return &rx->dnf_f;
	default:
		return &rx->dnf_w;
	}
}

#define REF_MAX_DB	30		/* scope reference level range, +- dB (F2 long + MFK) */
#define RIT_MAX_HZ	9990		/* RIT / XIT offset range, +- Hz (Icom: 9.99 kHz) */

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
	// Reference level edit open (F2 long): MFK push switches RF <-> audio scope;
	// any other key closes it and then does its normal job
	//
	if (rx->redit) {
		rx->redit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			rx->redit_af = !rx->redit_af;
			return true;
		}

		rx->redit = false;
	}

	//
	// GEN settings panel open: MFK push selects the next row; GEN closes it (below),
	// any other key closes it and does its normal job. The status page closes on any key.
	//
	if (rx->gedit) {
		rx->gedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			rx->gedit_par = (rx->gedit_par + 1) % GEN_ROWS;
			return true;
		}

		if (key != X6100_KEY_GEN)
			rx->gedit = false;
	}

	//
	// RIT / XIT panel open: MFK push selects offset / RIT / XIT; any other key
	// closes it and does its normal job (AGC long closes it too)
	//
	if (rx->tedit) {
		rx->tedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			rx->tedit_par = (rx->tedit_par + 1) % 3;
			return true;
		}

		rx->tedit = false;
	}

	if (rx->ginfo) {
		rx->ginfo = false;

		if (key == X6100_KEY_GEN)
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

	//
	// DSP panel open: MFK push selects the next value; DFN / DFL keep switching
	// NR, NB and notch; any other key closes the panel and does its normal job
	//
	if (rx->dedit) {
		rx->dedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			rx->dedit_par = (rx->dedit_par + 1) % DSP_PARS;
			return true;
		}

		if (key != X6100_KEY_DFN && key != X6100_KEY_DFL)
			rx->dedit = false;
	}

	if (rx->lock && key != X6100_KEY_LOCK)
		return false;

	switch (key) {
	case X6100_KEY_LOCK:
		rx->lock = !rx->lock;
		break;
	case X6100_KEY_AB:	/* short: VFO A <-> B; long: A=B (the other VFO takes this one) */
		vfo_swap(rx);
		break;
	case X6100_KEY_GEN:	/* short: settings panel; long: status page */
		rx->gedit = !rx->gedit;
		rx->gedit_since = now_ms();
		break;
	case X6100_KEY_DFL:	/* short: notch on / off; long: DSP panel */
		rx->dnf = !rx->dnf;
		break;
	case X6100_KEY_DFN:	/* short: NR on / off; long: NB on / off */
		rx->nr = !rx->nr;
		break;
	case X6100_KEY_FST:
		if (am_broadcast(rx))	/* LW / MW in AM: channel steps 9 kHz (EU) and 10 kHz (US) */
			rx->step = rx->step == 100 ? 1000 : rx->step == 1000 ? 5000 : rx->step == 5000 ? 9000
				: rx->step == 9000 ? 10000 : 100;
		else if (on_60m(rx))	/* 60 m: CH steps through the channel list */
			rx->step = rx->step == 10 ? 100 : rx->step == 100 ? 1000 : rx->step == 1000 ? 5000
				: rx->step == 5000 ? STEP_CH : 10;
		else
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
		// Short: toggle scroll within the current family (long press switches family)
		switch (rx->scope) {
		case SCOPE_CENTER:
			rx->scope = SCOPE_CENTER_S;
			break;
		case SCOPE_CENTER_S:
			rx->scope = SCOPE_CENTER;
			break;
		case SCOPE_FIX:
			rx->scope = SCOPE_FIX_S;
			break;
		case SCOPE_FIX_S:
			rx->scope = SCOPE_FIX;
			break;
		}

		break;
	case X6100_KEY_F3:	/* short: audio scope on / off; long: RF + audio side by side */
		rx->ascope = rx->asplit ? false : !rx->ascope;
		rx->asplit = false;
		break;
	case X6100_KEY_F2:
		if (rx->ascope)
			rx->arange = !rx->arange;	/* audio scope: 0-3 / 0-6 kHz */
		else
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
		|| key == X6100_KEY_F1 || key == X6100_KEY_F2 || key == X6100_KEY_F3 || key == X6100_KEY_F5 || key == X6100_KEY_DFN || key == X6100_KEY_DFL
		|| key == X6100_KEY_GEN || key == X6100_KEY_AB
		|| key == X6100_KEY_VM || key == X6100_KEY_AGC;
}

static bool on_long_key(rx_t *rx, int key)
{
	if (rx->lock)
		return false;

	switch (key) {
	case X6100_KEY_PRE:
		rx->att = !rx->att;
		return true;
	case X6100_KEY_F1:
		// Long: CENTER family <-> FIX family
		if (rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S) {
			rx->scope = SCOPE_CENTER;
		} else {
			scope_fix_start(rx);
			rx->scope = SCOPE_FIX;
		}

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
		if (rx->tedit) {
			rx->rit_hz = 0;	/* clear the offset, as Icom's CLEAR */
			rx->tedit_since = now_ms();
			return true;
		}

		if (rx->gedit) {
			gen_adjust(rx->gedit_par, 0, true);
			rx->gedit_since = now_ms();
			return true;
		}

		if (rx->dedit) {
			*dsp_value(rx, rx->dedit_par) = g_dsp[rx->dedit_par].def;
			rx->dedit_since = now_ms();
			return true;
		}

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
	case X6100_KEY_F2:
		// Long: reference level edit; starts on the scope that is shown (split: RF)
		rx->redit = !rx->redit;
		rx->redit_af = rx->ascope;
		rx->redit_since = now_ms();
		return true;
	case X6100_KEY_F3:
		rx->asplit = !rx->asplit;
		rx->ascope = false;
		return true;
	case X6100_KEY_DFL:
		rx->dedit = !rx->dedit;
		rx->dedit_since = now_ms();
		return true;
	case X6100_KEY_VM:	/* long: split on / off */
		rx->split = !rx->split;
		toast(rx->split ? "SPLIT ON" : "SPLIT OFF");
		return true;
	case X6100_KEY_AGC:	/* long: RIT / XIT panel */
		rx->tedit = !rx->tedit;
		rx->tedit_since = now_ms();
		return true;
	case X6100_KEY_AB:
		vfo_equal(rx);
		return true;
	case X6100_KEY_GEN:
		rx->ginfo = !rx->ginfo;
		rx->ginfo_since = now_ms();
		rx->gedit = false;
		return true;
	case X6100_KEY_DFN:
		rx->nb = !rx->nb;
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
		if (rx->step == STEP_CH)
			set_freq(rx, (int64_t) rx->freq - rx->freq % 100 + (int64_t) value * X6100_DIR_VFO * 100);
		else
			set_freq(rx, (int64_t) rx->freq - rx->freq % (uint32_t) rx->step + (int64_t) value * X6100_DIR_VFO * rx->step);
		return true;
	case IN_VOL:
		rx->vol = clampi(rx->vol + value * X6100_DIR_VOL, 0, 100);
		return true;
	case IN_MFK:
		if (rx->tedit) {
			int dir = value * X6100_DIR_MFK > 0 ? 1 : -1;

			if (rx->tedit_par == 0)
				rx->rit_hz = clampi(rx->rit_hz + value * X6100_DIR_MFK * 10, -RIT_MAX_HZ, RIT_MAX_HZ);
			else if (rx->tedit_par == 1)
				rx->rit = dir > 0;
			else
				rx->xit = dir > 0;

			rx->tedit_since = now_ms();
			return true;
		}

		if (rx->gedit) {
			gen_adjust(rx->gedit_par, value * X6100_DIR_MFK > 0 ? 1 : -1, false);
			rx->gedit_since = now_ms();
			return true;
		}

		if (rx->redit) {
			int *p = rx->redit_af ? &rx->aref : &rx->rref;

			*p = clampi(*p + value * X6100_DIR_MFK, -REF_MAX_DB, REF_MAX_DB);
			rx->redit_since = now_ms();
			return true;
		}

		if (rx->dedit) {
			int *p = dsp_value(rx, rx->dedit_par);
			const dsp_par_t *d = &g_dsp[rx->dedit_par];

			*p = clampi(*p + value * X6100_DIR_MFK * d->step, d->min, d->max);
			rx->dedit_since = now_ms();
			return true;
		}

		// 60 m channel mode: MFK picks the channel, the VFO knob fine-tunes (100 Hz)
		if (rx->step == STEP_CH && !rx->fedit) {
			ch60_step(rx, value * X6100_DIR_MFK > 0 ? 1 : -1);
			return true;
		}

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
#define OVF_POWER	0.178f	/* |IQ|^2 at -7.5 dB: flow IQ clips at about -6.3 dB */
#define VU_W		296
#define VU_H		168
#define AS_N		2048		/* audio scope FFT: 23.4 Hz bins at 48 kHz */
#define AS_RATE		48000
#define AS_DB_MIN	-110.0f	/* dBFS */
#define AS_DB_MAX	-10.0f
#define AS_RANGE_DB	80.0f		/* normalised window height */
#define AS_FLOOR_DB	10.0f		/* window bottom under the audio floor */
#define SPEC_Y		180
#define SPEC_H		150
#define WF_Y		(SPEC_Y + SPEC_H + 4)
#define TAG_H		20		/* F1-F5 tags along the bottom edge */
#define WF_H(lcd)	((lcd)->h - WF_Y - TAG_H)

// Tag centre x above F1-F5 (screen 800 px); F3 slightly left of centre, F1 / F5
// further out than a 160 px grid
static const int g_tag_x[5] = { 52, 224, 393, 567, 742 };	/* fitted to photos of the keys */

#define TAG_W		104		/* key cap width, about the key on screen */
#define TAG_FILL	0x1C2633
#define TAG_EDGE	0x4A5A70
#define TAG_EDGE_OFF	0x232B36
#define DISP_MS		70
#define SPEC_DB_MIN	-130.0f
#define SPEC_DB_MAX	-30.0f
#define SPAN_HZ		96000
#define GRID_HZ		10000
#define GRID_DB		10

//
// Meter calibration: dBm = passband power in dB + SMETER_CAL_DB.
// Provisional: -15 dB from an FT8 comparison with an IC-7851, then 6 dB lower as the
// meter read high in use (2026-10-24). Refine with a signal generator.
//
#define SMETER_CAL_DB	-21.0
#define VU_ATTACK_MS	80.0
#define VU_RELEASE_MS	900.0	/* arc needle only; the bar follows the reading directly */
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
	uint64_t ovf_until;	/* OVF shown until (ms), flow IQ at its clip level */
	bool was_tx;
	int was_meter;

	//
	// Waterfall alignment state
	//
	scope_mode_t wf_scope;
	int wf_span;

	//
	// Audio scope (F3): FFT of the codec capture
	//
	float aspec[AS_N / 2];	/* dB, smoothed */
	float apeak[AS_N / 2];
	float afloor;
	int apb_lo, apb_hi;	/* receive passband in FFT bins, for the floor; 0 = not known yet */
	bool aspec_valid;
	int wf_view;		/* waterfall holds: 0 RF, 1 AF, 2 RF + AF (split) */
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

static float g_rf_ref;	/* RF scope reference level, dB, from rx->rref */

static float spec_norm(float db);

static int spec_y(float db)
{
	float v = spec_norm(db);

	v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	return SPEC_Y + SPEC_H - 1 - (int) (v * (float) (SPEC_H - 1));
}

static float spec_norm(float db)
{
	return (db - SPEC_DB_MIN + g_rf_ref) / (SPEC_DB_MAX - SPEC_DB_MIN);
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

		snprintf(buf, len, "S%-4d", su < 0 ? 0 : su);	/* fixed 5 chars: "S" does not move */
	} else {
		int over = (int) (dbm + 73.0 + 0.5);

		// 10 dB steps, rounded: +0..5 = S9, +6..15 = S9+10, .. S9+60
		over = (over + 4) / 10 * 10;
		over = over > 60 ? 60 : over;

		if (over < 10)
			snprintf(buf, len, "S9   ");
		else
			snprintf(buf, len, "S9+%d", over);
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
// DSP panel (DFL long): the baseband's noise reduction, noise blanker and notch
// on one sheet. Teal, to tell it from the filter (blue) and tune (amber) panels.
// One row per value with a slider; the selected row is highlighted, MFK turns it,
// MFK push moves to the next row, MFK long sets it back to its default. The ON /
// OFF state of each function is shown in its group and switched with DFN / DFL.
//

//
// GEN: settings panel (short) and status page (long). Rows are kept in the
// state file; a row that x6100_trx.conf sets is shown CONF and cannot be
// changed here (the conf always wins at start). Violet, to tell it from the
// filter (blue), DSP (teal) and tune (amber) panels.
//

static const char *g_gen_name[GEN_ROWS] = {
	"BRIGHTNESS", "CAT CI-V", "NTP SYNC", "WIFI", "WIFI NETWORK", "BLUETOOTH", "BT PAIRING", "BT DEVICE",
};
static const int g_cat_rates[] = { 0, 4800, 9600, 19200, 38400, 57600, 115200 };

#define CAT_RATES	((int) (sizeof(g_cat_rates) / sizeof(g_cat_rates[0])))

static bool backlight_level(int pct);

//
// WiFi and Bluetooth as the image runs them: NetworkManager (no nmcli: D-Bus with
// dbus-send / gdbus, saved profiles in NM_CONN_DIR) and bluez (bluetoothctl).
// Read while the GEN panel is open, every SYS_POLL_MS; actions run detached.
//

#define NM_CONN_DIR	"/etc/NetworkManager/system-connections"
#define SYS_POLL_MS	3000
#define SYS_MAX		8
#define BT_VISIBLE_S	120

typedef struct
{
	bool wifi_on;
	char ssid[40];
	int signal;		/* dBm, 0 = not connected */
	char ip[INET_ADDRSTRLEN];
	int nnet;
	char net_id[SYS_MAX][40];
	char net_uuid[SYS_MAX][40];
	int net_sel;
	bool bt_on;
	bool bt_visible;
	uint64_t bt_visible_until;
	int ndev;
	char dev_mac[SYS_MAX][18];
	char dev_name[SYS_MAX][32];
	bool dev_conn[SYS_MAX];
	int dev_sel;
	uint64_t at;		/* last refresh, ms */
} sys_t;

static sys_t g_sys;

// Run a command (argv, no shell) and read its output; false when it cannot run
static bool sys_read(char *const argv[], char *buf, size_t size)
{
	int fd[2];
	pid_t pid;
	size_t len = 0;
	ssize_t r;

	buf[0] = '\0';

	if (pipe(fd) < 0)
		return false;

	if ((pid = fork()) == 0) {
		int nul = open("/dev/null", O_RDWR);

		dup2(fd[1], STDOUT_FILENO);
		if (nul >= 0) {
			dup2(nul, STDIN_FILENO);
			dup2(nul, STDERR_FILENO);
		}

		close(fd[0]);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fd[1]);

	if (pid < 0) {
		close(fd[0]);
		return false;
	}

	while (len + 1 < size && (r = read(fd[0], buf + len, size - 1 - len)) > 0)
		len += (size_t) r;

	buf[len] = '\0';
	close(fd[0]);
	waitpid(pid, NULL, 0);
	return true;
}

// Run a command detached (double fork: no zombie, no wait); for slow actions
static void sys_spawn(char *const argv[])
{
	pid_t pid = fork();

	if (pid == 0) {
		if (fork() == 0) {
			int nul = open("/dev/null", O_RDWR);

			if (nul >= 0) {
				dup2(nul, STDIN_FILENO);
				dup2(nul, STDOUT_FILENO);
				dup2(nul, STDERR_FILENO);
			}

			setsid();
			execvp(argv[0], argv);
		}

		_exit(0);
	}

	if (pid > 0)
		waitpid(pid, NULL, 0);
}

// Value after "key" on a line of text, up to the end of that line
static bool sys_field(const char *text, const char *key, char *out, size_t n)
{
	const char *p = strstr(text, key);
	size_t i = 0;

	if (p == NULL)
		return false;

	p += strlen(key);

	while (*p == ' ' || *p == '\t')
		p++;

	while (p[i] != '\0' && p[i] != '\n' && i + 1 < n) {
		out[i] = p[i];
		i++;
	}

	out[i] = '\0';
	return true;
}

static void sys_refresh(void)
{
	static char buf[4096];
	char v[64];
	DIR *d;
	struct ifaddrs *ifa;

	// WiFi switch (NetworkManager), link and address
	{
		char *get[] = { "dbus-send", "--system", "--print-reply", "--dest=org.freedesktop.NetworkManager",
			"/org/freedesktop/NetworkManager", "org.freedesktop.DBus.Properties.Get",
			"string:org.freedesktop.NetworkManager", "string:WirelessEnabled", NULL };
		char *link[] = { "iw", "dev", "wlan0", "link", NULL };

		g_sys.wifi_on = sys_read(get, buf, sizeof(buf)) && strstr(buf, "boolean true") != NULL;
		g_sys.ssid[0] = '\0';
		g_sys.signal = 0;

		if (sys_read(link, buf, sizeof(buf)) && sys_field(buf, "SSID:", g_sys.ssid, sizeof(g_sys.ssid))
			&& sys_field(buf, "signal:", v, sizeof(v)))
			g_sys.signal = atoi(v);
	}

	g_sys.ip[0] = '\0';

	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *a = ifa; a != NULL; a = a->ifa_next) {
			if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, "wlan0") == 0)
				inet_ntop(AF_INET, &((struct sockaddr_in *) a->ifa_addr)->sin_addr, g_sys.ip, sizeof(g_sys.ip));
		}

		freeifaddrs(ifa);
	}

	// Saved WiFi profiles (keyfiles: id=, uuid=, type=wifi)
	g_sys.nnet = 0;

	if ((d = opendir(NM_CONN_DIR)) != NULL) {
		struct dirent *e;

		while (g_sys.nnet < SYS_MAX && (e = readdir(d)) != NULL) {
			char path[300];
			FILE *fp;
			size_t n;

			if (e->d_name[0] == '.')
				continue;

			snprintf(path, sizeof(path), "%s/%s", NM_CONN_DIR, e->d_name);

			if ((fp = fopen(path, "r")) == NULL)
				continue;

			n = fread(buf, 1, sizeof(buf) - 1, fp);
			buf[n] = '\0';
			fclose(fp);

			if (strstr(buf, "\ntype=wifi") == NULL && strstr(buf, "\ntype=802-11-wireless") == NULL)
				continue;

			if (sys_field(buf, "\nid=", g_sys.net_id[g_sys.nnet], sizeof(g_sys.net_id[0]))
				&& sys_field(buf, "\nuuid=", g_sys.net_uuid[g_sys.nnet], sizeof(g_sys.net_uuid[0])))
				g_sys.nnet++;
		}

		closedir(d);
	}

	g_sys.net_sel = clampi(g_sys.net_sel, 0, g_sys.nnet > 0 ? g_sys.nnet - 1 : 0);

	// Bluetooth adapter and paired devices
	{
		char *show[] = { "bluetoothctl", "show", NULL };
		char *paired[] = { "bluetoothctl", "devices", "Paired", NULL };

		g_sys.bt_on = sys_read(show, buf, sizeof(buf)) && strstr(buf, "Powered: yes") != NULL;
		g_sys.bt_visible = strstr(buf, "Discoverable: yes") != NULL;
		g_sys.ndev = 0;

		if (sys_read(paired, buf, sizeof(buf))) {
			for (char *l = strtok(buf, "\n"); l != NULL && g_sys.ndev < SYS_MAX; l = strtok(NULL, "\n")) {
				char mac[18];
				int off = 0;

				if (sscanf(l, "Device %17s %n", mac, &off) != 1 || off == 0)
					continue;

				snprintf(g_sys.dev_mac[g_sys.ndev], sizeof(g_sys.dev_mac[0]), "%s", mac);
				snprintf(g_sys.dev_name[g_sys.ndev], sizeof(g_sys.dev_name[0]), "%s", l + off);
				g_sys.ndev++;
			}
		}

		for (int i = 0; i < g_sys.ndev; i++) {
			char *info[] = { "bluetoothctl", "info", g_sys.dev_mac[i], NULL };

			g_sys.dev_conn[i] = sys_read(info, buf, sizeof(buf)) && strstr(buf, "Connected: yes") != NULL;
		}
	}

	g_sys.dev_sel = clampi(g_sys.dev_sel, 0, g_sys.ndev > 0 ? g_sys.ndev - 1 : 0);
	g_sys.at = now_ms();
}

static void wifi_set(bool on)
{
	char *set[] = { "dbus-send", "--system", "--print-reply", "--dest=org.freedesktop.NetworkManager",
		"/org/freedesktop/NetworkManager", "org.freedesktop.DBus.Properties.Set",
		"string:org.freedesktop.NetworkManager", "string:WirelessEnabled",
		on ? "variant:boolean:true" : "variant:boolean:false", NULL };
	char out[256];

	sys_read(set, out, sizeof(out));
	g_sys.at = 0;	/* read back on the next draw */
}

// Activate a saved profile: settings path by uuid, then ActivateConnection (gdbus, detached)
static void wifi_connect(const char *uuid)
{
	char *sh[] = { "sh", "-c",
		"P=$(gdbus call --system --dest org.freedesktop.NetworkManager"
		" --object-path /org/freedesktop/NetworkManager/Settings"
		" --method org.freedesktop.NetworkManager.Settings.GetConnectionByUuid \"$1\""
		" | sed -n \"s/.*'\\(.*\\)'.*/\\1/p\");"
		" [ -n \"$P\" ] && gdbus call --system --dest org.freedesktop.NetworkManager"
		" --object-path /org/freedesktop/NetworkManager"
		" --method org.freedesktop.NetworkManager.ActivateConnection \"$P\" / /",
		"wifi_connect", (char *) uuid, NULL };

	sys_spawn(sh);
	g_sys.at = 0;
}

static void bt_cmd(char *a1, char *a2)
{
	char *cmd[] = { "bluetoothctl", a1, a2, NULL };
	char out[512];

	sys_read(cmd, out, sizeof(out));
	g_sys.at = 0;
}

static void gen_value(int row, char *s, size_t n)
{
	switch (row) {
	case GEN_BRIGHT:
		if (g_bright > 0)
			snprintf(s, n, "%d %%", g_bright);
		else
			snprintf(s, n, "AS OS");
		break;
	case GEN_CAT:
		if (g_cat_baud > 0)
			snprintf(s, n, "%d", g_cat_baud);
		else
			snprintf(s, n, "OFF");
		break;
	case GEN_NTP:
		snprintf(s, n, "%s", strcmp(g_ntp_server, "off") == 0 ? "OFF" : "AT START");
		break;
	case GEN_WIFI:
		snprintf(s, n, "%s", g_sys.wifi_on ? "ON" : "OFF");
		break;
	case GEN_WNET:
		snprintf(s, n, "%s", g_sys.nnet > 0 ? g_sys.net_id[g_sys.net_sel] : "NONE SAVED");
		break;
	case GEN_BT:
		snprintf(s, n, "%s", g_sys.bt_on ? "ON" : "OFF");
		break;
	case GEN_BTPAIR:
		if (g_sys.bt_visible && now_ms() < g_sys.bt_visible_until)
			snprintf(s, n, "VISIBLE %ds", (int) ((g_sys.bt_visible_until - now_ms()) / 1000));
		else
			snprintf(s, n, "%s", g_sys.bt_visible ? "VISIBLE" : "OFF");
		break;
	default:
		snprintf(s, n, "%s", g_sys.ndev > 0 ? g_sys.dev_name[g_sys.dev_sel] : "NONE PAIRED");
		break;
	}
}

// Second column: what the row does or what is going on
static void gen_hint(int row, char *s, size_t n)
{
	bool cur = g_sys.nnet > 0 && strcmp(g_sys.ssid, g_sys.net_id[g_sys.net_sel]) == 0;

	switch (row) {
	case GEN_CAT:
	case GEN_NTP:
		snprintf(s, n, "APPLIES AT NEXT START");
		break;
	case GEN_WIFI:
		if (g_sys.ssid[0] != '\0')
			snprintf(s, n, "%s  %s  %d DBM", g_sys.ssid, g_sys.ip[0] ? g_sys.ip : "NO IP", g_sys.signal);
		else
			snprintf(s, n, "NOT CONNECTED");
		break;
	case GEN_WNET:
		snprintf(s, n, "%s", g_sys.nnet == 0 ? "ADD WITH NMTUI OVER SSH" : cur ? "CONNECTED" : "HOLD MFK: CONNECT");
		break;
	case GEN_BTPAIR:
		snprintf(s, n, "TURN: VISIBLE %d S AS X6100", BT_VISIBLE_S);
		break;
	case GEN_BTDEV:
		if (g_sys.ndev > 0)
			snprintf(s, n, "%s", g_sys.dev_conn[g_sys.dev_sel] ? "CONNECTED, HOLD: DISCONNECT" : "HOLD MFK: CONNECT");
		else
			s[0] = '\0';
		break;
	default:
		s[0] = '\0';
		break;
	}
}

// MFK turn on a row (dir +-1); def: MFK long (default, or connect on the WiFi / BT rows)
static void gen_adjust(int row, int dir, bool def)
{
	if (g_conf_set & (1u << row))
		return;

	switch (row) {
	case GEN_BRIGHT:
		g_bright = def ? 100 : clampi((g_bright > 0 ? g_bright : 100) + dir * 5, 5, 100);
		backlight_level(g_bright);
		break;
	case GEN_CAT: {
		int i = 0;

		while (i < CAT_RATES - 1 && g_cat_rates[i] != g_cat_baud)
			i++;

		g_cat_baud = def ? 19200 : g_cat_rates[clampi(i + dir, 0, CAT_RATES - 1)];
		break;
	}
	case GEN_NTP:
		snprintf(g_ntp_server, sizeof(g_ntp_server), "%s", def || strcmp(g_ntp_server, "off") == 0 ? "" : "off");
		break;
	case GEN_WIFI:
		if (!def)
			wifi_set(dir > 0);
		break;
	case GEN_WNET:
		if (def && g_sys.nnet > 0)
			wifi_connect(g_sys.net_uuid[g_sys.net_sel]);
		else if (g_sys.nnet > 0)
			g_sys.net_sel = (g_sys.net_sel + dir + g_sys.nnet) % g_sys.nnet;
		break;
	case GEN_BT:
		if (!def)
			bt_cmd("power", dir > 0 ? "on" : "off");
		break;
	case GEN_BTPAIR:
		if (def)
			break;

		if (dir > 0) {
			char t[8];

			snprintf(t, sizeof(t), "%d", BT_VISIBLE_S);
			bt_cmd("discoverable-timeout", t);
			bt_cmd("pairable", "on");
			bt_cmd("discoverable", "on");
			g_sys.bt_visible_until = now_ms() + BT_VISIBLE_S * 1000u;
		} else {
			bt_cmd("discoverable", "off");
			bt_cmd("pairable", "off");
			g_sys.bt_visible_until = 0;
		}
		break;
	default:
		if (g_sys.ndev == 0)
			break;

		if (def) {
			char *cmd[] = { "bluetoothctl", g_sys.dev_conn[g_sys.dev_sel] ? "disconnect" : "connect",
				g_sys.dev_mac[g_sys.dev_sel], NULL };

			sys_spawn(cmd);	/* connecting takes seconds */
			g_sys.at = now_ms() - SYS_POLL_MS + 1500;
		} else {
			g_sys.dev_sel = (g_sys.dev_sel + dir + g_sys.ndev) % g_sys.ndev;
		}
		break;
	}
}

static void gen_frame(x6100_lcd_t *lcd, int px, int py, int pw, int ph, const char *title, const char *hint)
{
	const uint32_t bg = 0x140F1E, violet = 0xB080FF, edge = 0x3A2C55, dim = 0x8A82A0;

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, violet);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, edge);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, edge);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, edge);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, title, violet);
	x6100_lcd_text(lcd, px + 14 + x6100_lcd_text_width(title, 2) + 16, py + 11, 1, hint, dim);
}

static void upcase(char *s)
{
	for (; *s != '\0'; s++)
		*s = (char) toupper((unsigned char) *s);	/* the panel font is drawn in capitals */
}

static void draw_gen_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	// Five rows in view, scrolling with the selection: the panel stays inside the
	// spectrum, so the waterfall never scrolls panel pixels into its history
	const int vis = 5, top = clampi(rx->gedit_par - 2, 0, GEN_ROWS - vis);
	const int px = LBL_W + 8, py = SPEC_Y + 14, pw = 560, ph = 30 + vis * 21;
	const uint32_t sel = 0x2E2348, dim = 0x8A82A0, white = 0xF0F0F0, violet = 0xB080FF, amber = 0xFFB020;

	if (now_ms() - g_sys.at >= SYS_POLL_MS)
		sys_refresh();

	gen_frame(lcd, px, py, pw, ph, "SETTINGS", "MFK: ADJUST  PUSH: NEXT  HOLD: DEFAULT / CONNECT  GEN: CLOSE");

	for (int i = top; i < top + vis; i++) {
		int y = py + 30 + (i - top) * 21;
		bool locked = g_conf_set & (1u << i);
		char v[40], h[64];

		if (i == rx->gedit_par)
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

		x6100_lcd_text(lcd, px + 14, y, 2, g_gen_name[i], i == rx->gedit_par ? white : dim);

		gen_hint(i, h, sizeof(h));
		upcase(h);
		x6100_lcd_text(lcd, px + 196, y + 4, 1, h, dim);

		gen_value(i, v, sizeof(v));
		upcase(v);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v, 2), y, 2, v, locked ? dim : white);

		if (locked)
			x6100_lcd_text(lcd, px + pw - 22 - x6100_lcd_text_width(v, 2) - x6100_lcd_text_width("CONF", 2), y, 2, "CONF", amber);
	}

	// Position in the list, top right: the panel shows five of the rows
	{
		char pos[12];

		snprintf(pos, sizeof(pos), "%d/%d", rx->gedit_par + 1, GEN_ROWS);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(pos, 1), py + 11, 1, pos, violet);
	}
}

static void draw_gen_info(x6100_lcd_t *lcd, const screen_t *scr)
{
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 560, ph = 136;	/* as wide as the settings panel */
	const uint32_t dim = 0x8A82A0, white = 0xF0F0F0;
	struct ifaddrs *ifa = NULL;
	char lines[6][64];
	int n = 0;
	double up = 0.0;
	FILE *fp;

	gen_frame(lcd, px, py, pw, ph, "STATUS", "GEN LONG OR ANY KEY: CLOSE");

	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *a = ifa; a != NULL && n < 2; a = a->ifa_next) {
			char ip[INET_ADDRSTRLEN];

			if (a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET || strcmp(a->ifa_name, "lo") == 0)
				continue;

			inet_ntop(AF_INET, &((struct sockaddr_in *) a->ifa_addr)->sin_addr, ip, sizeof(ip));
			snprintf(lines[n++], sizeof(lines[0]), "IP      %s %s", a->ifa_name, ip);
		}

		freeifaddrs(ifa);
	}

	if (n == 0)
		snprintf(lines[n++], sizeof(lines[0]), "IP      no network");

	if ((fp = fopen("/proc/uptime", "r")) != NULL) {
		if (fscanf(fp, "%lf", &up) != 1)
			up = 0.0;

		fclose(fp);
	}

	snprintf(lines[n++], sizeof(lines[0]), "UPTIME  %dh%02dm   BATTERY %.1f V", (int) up / 3600, (int) up / 60 % 60, scr->vbat);

	if (g_cat_baud > 0)
		snprintf(lines[n++], sizeof(lines[0]), "CAT     %d BAUD, IC-705, ADDRESS A4", g_cat_baud);
	else
		snprintf(lines[n++], sizeof(lines[0]), "CAT     OFF");

	snprintf(lines[n++], sizeof(lines[0]), "NTP     %s", g_ntp_msg);
	snprintf(lines[n++], sizeof(lines[0]), "REGION  %s   BUILD %s", g_regions[g_region].name, X6100_REV);

	for (int i = 0; i < n; i++) {
		for (char *c = lines[i]; *c != '\0'; c++)
			*c = (char) toupper((unsigned char) *c);	/* the panel font has capitals only */

		x6100_lcd_text(lcd, px + 14, py + 30 + i * 17, 2, lines[i], i == n - 1 ? dim : white);
	}
}

//
// RIT / XIT panel (AGC long): one offset for both, as on Icom radios, and RIT /
// XIT on or off. Green. MFK turn: offset in 10 Hz steps, or on / off on the
// RIT and XIT rows; push: next row; hold: offset 0 (CLEAR).
//

static void draw_rit_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 420, ph = 96;
	const uint32_t bg = 0x0C180C, green = 0x60D060, sel = 0x1C3A1C, dim = 0x80A080, white = 0xF0F0F0;
	const uint32_t on = 0x3CC83C, off = 0x5A6464;
	char v[3][24];

	snprintf(v[0], sizeof(v[0]), "%+d HZ", rx->rit_hz);
	snprintf(v[1], sizeof(v[1]), "%s", rx->rit ? "ON" : "OFF");
	snprintf(v[2], sizeof(v[2]), "%s", rx->xit ? "ON" : "OFF");

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, green);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x2A4A2A);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x2A4A2A);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x2A4A2A);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "RIT / XIT", green);
	x6100_lcd_text(lcd, px + 140, py + 11, 1, "MFK: ADJUST  PUSH: NEXT  HOLD: CLEAR", dim);

	for (int i = 0; i < 3; i++) {
		static const char *names[3] = { "OFFSET", "RIT  RX", "XIT  TX" };
		int y = py + 30 + i * 21;
		uint32_t c = i == 0 ? white : (i == 1 ? rx->rit : rx->xit) ? on : off;

		if (i == rx->tedit_par)
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

		x6100_lcd_text(lcd, px + 14, y, 2, names[i], i == rx->tedit_par ? white : dim);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v[i], 2), y, 2, v[i], c);
	}
}

static void draw_dsp_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 500, ph = 132;
	const int sx = px + 196, sw = 170;	/* slider */
	const uint32_t bg = 0x0A1A1A, teal = 0x30D0B0, sel = 0x15403C, dim = 0x7A9490, white = 0xF0F0F0;
	const uint32_t on = 0x3CC83C, off = 0x5A6464;
	const bool state[DSP_PARS] = { rx->nr, rx->nb, rx->nb, rx->dnf, rx->dnf };
	char s[32];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, teal);	/* teal rule on the left */
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x1E4A44);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x1E4A44);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x1E4A44);

	x6100_lcd_text(lcd, px + 14, py + 7, 2, "DSP", teal);
	x6100_lcd_text(lcd, px + 70, py + 11, 1, "MFK: ADJUST   PUSH: NEXT   HOLD: DEFAULT   DFN: NR / NB   DFL: NOTCH", dim);

	for (int i = 0; i < DSP_PARS; i++) {
		const dsp_par_t *d = &g_dsp[i];
		int v = *dsp_value((rx_t *) rx, i);
		int y = py + 28 + i * 20;
		int fill = (v - d->min) * sw / (d->max - d->min);

		if (i == rx->dedit_par)
			x6100_lcd_fill(lcd, px + 6, y - 2, pw - 12, 19, sel);

		// ON / OFF once per function (NR row, first NB row, first notch row)
		if (i == 0 || i == 1 || i == 3)
			x6100_lcd_text(lcd, px + 14, y, 2, state[i] ? "ON " : "OFF", state[i] ? on : off);

		x6100_lcd_text(lcd, px + 58, y, 2, d->name, i == rx->dedit_par ? white : dim);

		x6100_lcd_fill(lcd, sx, y + 4, sw, 6, 0x1C2C2C);
		x6100_lcd_fill(lcd, sx, y + 4, fill, 6, state[i] ? teal : off);
		x6100_lcd_fill(lcd, sx + fill - 1, y + 1, 3, 12, white);

		snprintf(s, sizeof(s), "%d%s%s", v, d->unit[0] ? " " : "", d->unit);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 2), y, 2, s, white);
	}
}

//
// Tune panel: shown over the scope while the tuner searches (ATU long press or
// CAT 1C 01 02). Amber, to tell it apart from the blue filter panel: what is
// happening, for how long, and the two numbers that matter while tuning,
// forward power and SWR, as segmented bars with SWR zones (green to 1.5,
// amber to 2.5, red above).
//

static void tune_bar(x6100_lcd_t *lcd, int x, int y, double frac, bool swr)
{
	const int segs = 20, sw = 14, gap = 3, sh = 14;
	const uint32_t off = 0x23303F, po = 0x40C0FF;
	const uint32_t ok = 0x3CC83C, warn = 0xFFB020, bad = 0xFF4030;

	for (int i = 0; i < segs; i++) {
		double f = (i + 1.0) / segs;
		uint32_t c = po;

		if (swr)
			c = f <= swr_frac(1.5) + 0.01 ? ok : f <= swr_frac(2.5) + 0.01 ? warn : bad;

		x6100_lcd_fill(lcd, x + i * (sw + gap), y, sw, sh, f <= frac + 1e-9 ? c : off);
	}
}

static void draw_tune_panel(x6100_lcd_t *lcd, const rx_t *rx, const screen_t *scr)
{
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 500, ph = 132;	/* narrower than the filter panel: the clock stays clear */
	const int bx = px + 64, bar_w = 20 * 17 - 3;
	const uint32_t bg = 0x161208, amber = 0xFFB020, green = 0x3CC83C, dim = 0x8A8070, white = 0xF0F0F0;
	bool done = !rx->tuning;
	uint32_t accent = done ? green : amber;
	uint64_t ms = done ? rx->tune_ms : now_ms() - rx->tune_since;
	double pwr = done ? rx->tune_pwr : scr->tx ? scr->pwr : 0.0;
	double swr = done ? rx->tune_swr : scr->tx ? scr->swr : 0.0;
	bool have = done || scr->tx;
	char s[64];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, pw, 3, accent);	/* top rule: amber searching, green tuned */
	x6100_lcd_vline(lcd, px, py, py + ph - 1, 0x4A3A18);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x4A3A18);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x4A3A18);

	//
	// Title, band / frequency, elapsed (or total) time
	//
	x6100_lcd_text(lcd, px + 12, py + 12, 2, done ? "TUNED" : "TUNING", accent);
	snprintf(s, sizeof(s), "%s  %u.%03u.%03u", rx->band >= 0 ? g_bands[rx->band].name : "-",
		rx->freq / 1000000, rx->freq / 1000 % 1000, rx->freq % 1000);
	x6100_lcd_text(lcd, px + 120, py + 12, 2, s, white);
	snprintf(s, sizeof(s), "%2u.%u S", (unsigned) (ms / 1000), (unsigned) (ms / 100 % 10));
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 2), py + 12, 2, s, white);

	//
	// PO and SWR bars (live while searching, the last reading once tuned)
	//
	x6100_lcd_text(lcd, px + 14, py + 42, 2, "PO", dim);
	tune_bar(lcd, bx, py + 42, have ? pwr / 10.0 : 0.0, false);
	snprintf(s, sizeof(s), "%4.1f W", have ? pwr : 0.0);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 2), py + 42, 2, s, white);

	x6100_lcd_text(lcd, px + 14, py + 70, 2, "SWR", dim);
	tune_bar(lcd, bx, py + 70, have ? swr_frac(swr) : 0.0, true);

	if (have)
		snprintf(s, sizeof(s), "%4.1f", swr);
	else
		snprintf(s, sizeof(s), " -.-");

	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 2), py + 70, 2, s, white);

	// SWR scale marks under the bar
	{
		static const struct { double swr; const char *s; } mk[] = {
			{ 1.0, "1" }, { 1.5, "1.5" }, { 2.0, "2" }, { 3.0, "3" }, { 5.0, "5" },
		};

		for (size_t i = 0; i < sizeof(mk) / sizeof(mk[0]); i++) {
			int x = bx + (int) (swr_frac(mk[i].swr) * bar_w);

			x6100_lcd_text(lcd, x - x6100_lcd_text_width(mk[i].s, 1) / 2, py + 88, 1, mk[i].s, dim);
		}
	}

	//
	// Status line. Searching: animated dots. Tuned: the network the tuner
	// settled on, as numbers and as relay bit patterns (L and C are relay
	// banks, S the side the capacitor sits on), and in hex.
	//
	if (!done) {
		static const char *dots[4] = { "", ".", "..", "..." };

		snprintf(s, sizeof(s), "SEARCHING%s", dots[ms / 400 % 4]);
		x6100_lcd_text(lcd, px + 14, py + ph - 24, 2, s, amber);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width("ANY KEY: ABORT", 1), py + ph - 20, 1,
			"ANY KEY: ABORT", dim);
	} else if (rx->band >= 0 && rx->atu_valid[rx->band]) {
		const x6100_atu_t *n = &rx->atu_net[rx->band];
		char lb[9], cb[9];

		for (int i = 0; i < 8; i++) {
			lb[i] = (char) ('0' + (n->l >> (7 - i) & 1));
			cb[i] = (char) ('0' + (n->c >> (7 - i) & 1));
		}

		lb[8] = cb[8] = '\0';
		snprintf(s, sizeof(s), "L %3u  C %3u  S %u", n->l, n->c, n->s);
		x6100_lcd_text(lcd, px + 14, py + ph - 26, 2, s, green);
		snprintf(s, sizeof(s), "L %s  C %s  S %u", lb, cb, n->s);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 1), py + ph - 24, 1, s, dim);
		snprintf(s, sizeof(s), "HEX %02X %02X %02X", n->l, n->c, n->s);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(s, 1), py + ph - 13, 1, s, dim);
	}
}

//
// Audio scope (F3): the demodulated RX audio from the codec capture (48 kHz,
// left channel), FFT over AS_N samples every AS_N / 2 new ones (Hann window,
// 50 % overlap), smoothed like the RF spectrum.
//

static void audio_feed(screen_t *scr, const int16_t *buf, unsigned frames, unsigned channels)
{
	static float ring[AS_N];
	static unsigned pos, fresh;
	static float v[AS_N][2];

	for (unsigned i = 0; i < frames; i++) {
		ring[pos] = buf[i * channels] / 32768.0f;
		pos = (pos + 1) % AS_N;

		if (++fresh < AS_N / 2)
			continue;

		fresh = 0;

		for (int k = 0; k < AS_N; k++) {
			float w = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (float) k / (float) (AS_N - 1));

			v[k][0] = ring[(pos + k) % AS_N] * w;
			v[k][1] = 0.0f;
		}

		fft(v, AS_N);

		for (int k = 0; k < AS_N / 2; k++) {
			// dBFS: a full-scale sine (Hann, coherent gain 0.5) reads 0 dB
			float p = (v[k][0] * v[k][0] + v[k][1] * v[k][1]) * 16.0f / ((float) AS_N * (float) AS_N);
			float db = p > 1e-20f ? 10.0f * log10f(p) : -200.0f;

			scr->aspec[k] = scr->aspec_valid ? scr->aspec[k] * (1.0f - SCOPE_AVG) + db * SCOPE_AVG : db;
			scr->apeak[k] = scr->aspec_valid && scr->apeak[k] - PEAK_DECAY_DB > scr->aspec[k]
				? scr->apeak[k] - PEAK_DECAY_DB : scr->aspec[k];
		}

		// Floor: 20th percentile of the bins inside the receive passband (outside it the
		// filter leaves near silence, which would put the floor far under the noise), smoothed
		{
			static float s[AS_N / 2];
			const int b0 = scr->apb_hi > scr->apb_lo + 8 ? scr->apb_lo : 0;
			const int n = scr->apb_hi > scr->apb_lo + 8 ? scr->apb_hi - scr->apb_lo : 6000 * AS_N / AS_RATE;

			memcpy(s, scr->aspec + b0, sizeof(float) * (size_t) n);

			for (int i = 1; i < n; i++) {
				float x = s[i];
				int j = i - 1;

				while (j >= 0 && s[j] > x) {
					s[j + 1] = s[j];
					j--;
				}

				s[j + 1] = x;
			}

			scr->afloor = scr->aspec_valid ? scr->afloor * 0.9f + s[n / 5] * 0.1f : s[n / 5];
		}

		scr->aspec_valid = true;
	}
}

static float audio_db_at(const float *a, int x, int w, int range_hz)
{
	double bin = (double) x * range_hz / w * AS_N / AS_RATE;
	int i = (int) bin;
	double t = bin - i;

	if (i + 1 >= AS_N / 2)
		return a[AS_N / 2 - 1];

	return (float) (a[i] * (1.0 - t) + a[i + 1] * t);
}

// Audio scope vertical scale, normalised: the bottom follows the audio noise
// floor (AS_FLOOR_DB under it) and the window is AS_RANGE_DB high, so the
// picture stays put whatever the volume or band noise; labels stay in dBFS
static float g_as_bottom = AS_DB_MIN;

static int audio_y(float db)
{
	float n = (db - g_as_bottom) / AS_RANGE_DB;

	n = n < 0.0f ? 0.0f : n > 1.0f ? 1.0f : n;
	return SPEC_Y + SPEC_H - 1 - (int) (n * (float) (SPEC_H - 1));
}

//
// Audio scope picture in columns x0 .. x0 + w - 1: 0-3 or 0-6 kHz (F2), the
// receive passband shaded, the notch marked when on. Full width (F3) or the
// right half next to the RF scope (F3 long).
//

static void draw_audio_spec(screen_t *scr, const rx_t *rx, int x0, int w)
{
	x6100_lcd_t *lcd = &scr->lcd;
	const int range = rx->arange ? 6000 : 3000;
	uint32_t lo, hi;
	int prev_y = -1;

	g_as_bottom = (scr->aspec_valid ? scr->afloor - AS_FLOOR_DB : AS_DB_MIN) - (float) rx->aref;

	x6100_lcd_fill(lcd, x0, SPEC_Y, w, SPEC_H, 0x05080C);

	// Passband in audio Hz: the filter edges (AM / FM: 0 .. high)
	mode_filter(rx->mode, rx->fil, &lo, &hi);
	scr->apb_lo = (int) ((long) lo * AS_N / AS_RATE);
	scr->apb_hi = (int) ((long) (hi > 6000 ? 6000 : hi) * AS_N / AS_RATE);

	{
		int p0 = (int) ((long) lo * w / range), p1 = (int) ((long) hi * w / range);

		p1 = p1 >= w ? w - 1 : p1;

		if (p1 > p0)
			x6100_lcd_fill(lcd, x0 + p0, SPEC_Y, p1 - p0 + 1, SPEC_H, 0x183820);
	}

	// Grid every 500 Hz (labels every kHz) and every 10 dB
	for (int hz = 500; hz < range; hz += 500) {
		int gx = x0 + hz * w / range;

		x6100_lcd_vline(lcd, gx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0x1A2430);

		if (hz % 1000 == 0) {
			char lbl[8];

			snprintf(lbl, sizeof(lbl), "%dk", hz / 1000);
			x6100_lcd_text(lcd, gx - x6100_lcd_text_width(lbl, 2) / 2, SPEC_Y + 4, 2, lbl, SCR_DIM);
		}
	}

	for (int db = (int) ceilf(g_as_bottom / GRID_DB) * GRID_DB; db < g_as_bottom + AS_RANGE_DB; db += GRID_DB)
		x6100_lcd_hline(lcd, x0, x0 + w - 1, audio_y((float) db), 0x1A2430);

	if (scr->aspec_valid) {
		for (int x = 0; x < w; x++) {
			int y = audio_y(audio_db_at(scr->aspec, x, w, range));
			int py = audio_y(audio_db_at(scr->apeak, x, w, range));

			x6100_lcd_vline(lcd, x0 + x, y, SPEC_Y + SPEC_H - 1, SCOPE_FILL);
			x6100_lcd_fill(lcd, x0 + x, py, 1, 1, SCOPE_PEAK);
			x6100_lcd_vline(lcd, x0 + x, prev_y < 0 ? y : prev_y, y, SCOPE_LINE);
			prev_y = y;
		}
	} else {
		x6100_lcd_text(lcd, x0 + LBL_W + 8, SPEC_Y + SPEC_H / 2 - 7, 2, "NO AUDIO", SCR_WARN);
	}

	// dBFS labels in the left margin of this part, over the trace
	for (int db = (int) ceilf(g_as_bottom / GRID_DB) * GRID_DB; db < g_as_bottom + AS_RANGE_DB; db += GRID_DB) {
		char lbl[8];
		int gy = audio_y((float) db);

		if (gy <= SPEC_Y + 20)
			continue;

		snprintf(lbl, sizeof(lbl), "%d", db);
		x6100_lcd_fill(lcd, x0, gy - 10, LBL_W, 9, 0x05080C);
		x6100_lcd_text(lcd, x0 + 2, gy - 9, 1, lbl, SCR_DIM);
	}

	// Notch frequency when the notch is on
	if (rx->dnf && rx->dnf_f < range) {
		int nx = x0 + rx->dnf_f * w / range;

		x6100_lcd_vline(lcd, nx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0x30D0B0);
		x6100_lcd_text(lcd, nx + 4, SPEC_Y + SPEC_H - 18, 2, "MN", 0x30D0B0);
	}
}

// One audio waterfall line (row WF_Y) in columns x0 .. x0 + w - 1; the caller scrolls
static void draw_audio_wf_line(screen_t *scr, const rx_t *rx, int x0, int w)
{
	const int range = rx->arange ? 6000 : 3000;

	for (int x = 0; x < w; x++) {
		uint32_t c = !scr->aspec_valid ? 0x000000
			: wf_colour((audio_db_at(scr->aspec, x, w, range) - (scr->afloor - WF_BELOW_DB - (float) rx->aref)) / WF_RANGE_DB);

		x6100_lcd_fill(&scr->lcd, x0 + x, WF_Y, 1, 1, c);
	}
}

//
// Shift a band of rows horizontally by dx pixels (positive = right), fill the gap black
//

static void wf_shift(x6100_lcd_t *lcd, int y0, int h, int dx, int w)
{
	int stride = lcd->w;	/* shift only the first w pixels of each row */

	if (dx >= w || dx <= -w) {
		x6100_lcd_fill(lcd, 0, y0, w, h, 0x000000);
		return;
	}

	for (int y = y0; y < y0 + h; y++) {
		uint32_t *row = lcd->pix + (size_t) y * (size_t) stride;

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

	// VFO in use: A or B right of the TX box
	x6100_lcd_text(lcd, 72, FREQ_Y + 12, 3, rx->on_b ? "B" : "A", SCR_ACCENT);

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
	if (rx->step == STEP_CH)
		snprintf(s, sizeof(s), "STEP   CH");
	else if (rx->step >= 1000 && rx->step % 1000 == 0)
		snprintf(s, sizeof(s), "STEP %3dk", rx->step / 1000);	/* 1k .. 10k in four places */
	else
		snprintf(s, sizeof(s), "STEP %4d", rx->step);
	x6100_lcd_text(lcd, INFO_COL_STEP, INFO_Y, 2, s, SCR_DIM);
	snprintf(s, sizeof(s), "VOL %3d", rx->vol);
	x6100_lcd_text(lcd, INFO_COL_VOL, INFO_Y, 2, s, rx->mute ? SCR_WARN : SCR_DIM);

	if (rx->mfk_pwr) {
		snprintf(s, sizeof(s), "PWR %2d.%dW", rx->txpwr / 10, rx->txpwr % 10);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_ACCENT);
	} else {
		snprintf(s, sizeof(s), "RFG %3d", rx->rfg);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_DIM);
	}

	snprintf(s, sizeof(s), "%4.1fV", scr->vbat);
	x6100_lcd_text(lcd, INFO_COL_VBAT, INFO_Y, 2, s, SCR_DIM);
	snprintf(s, sizeof(s), "FIL%d", rx->fil + 1);
	x6100_lcd_text(lcd, INFO_COL_FIL, INFO_Y, 2, s, SCR_DIM);

	//
	// F1-F5 tags along the bottom: a key cap above each key (about the key's
	// width on screen, rounded corners), label centred. Positions in g_tag_x[].
	//
	{
		static const char *tag[5] = { "SCOPE", "SPAN", "AUDIO", "METER", "FIL" };
		const int y = lcd->h - TAG_H, bw = TAG_W, bh = TAG_H - 2;

		x6100_lcd_fill(lcd, 0, y, W, TAG_H, SCR_BG);

		for (int i = 0; i < 5; i++) {
			int x = g_tag_x[i] - bw / 2, by = y + 1;
			bool used = tag[i][0] != '\0';
			uint32_t edge = used ? TAG_EDGE : TAG_EDGE_OFF;

			// Body, then a 1 px edge with the corner pixels left out (rounded look)
			if (used)
				x6100_lcd_fill(lcd, x + 1, by + 1, bw - 2, bh - 2, TAG_FILL);

			x6100_lcd_fill(lcd, x + 2, by, bw - 4, 1, edge);
			x6100_lcd_fill(lcd, x + 2, by + bh - 1, bw - 4, 1, edge);
			x6100_lcd_fill(lcd, x, by + 2, 1, bh - 4, edge);
			x6100_lcd_fill(lcd, x + bw - 1, by + 2, 1, bh - 4, edge);
			x6100_lcd_fill(lcd, x + 1, by + 1, 1, 1, edge);
			x6100_lcd_fill(lcd, x + bw - 2, by + 1, 1, 1, edge);
			x6100_lcd_fill(lcd, x + 1, by + bh - 2, 1, 1, edge);
			x6100_lcd_fill(lcd, x + bw - 2, by + bh - 2, 1, 1, edge);

			if (used)
				x6100_lcd_text(lcd, g_tag_x[i] - x6100_lcd_text_width(tag[i], 2) / 2 + 1, by + 2, 2,
					tag[i], SCR_FG);
		}
	}

	//
	// Meters: S in RX; Po (needle, bar) and SWR in TX, marker on the ALC arc
	// while the ALC is working, else on Po
	//
	if (scr->tx != scr->was_tx || rx->meter != scr->was_meter) {
		// no needle reset: it glides to the new reading (release time), no flick to 0
		scr->vu_peak = 0.0;
		scr->was_tx = scr->tx;
		scr->was_meter = rx->meter;
	}

	static const char *meter_names[] = { "PO", "SWR", "ALC" };

	if (scr->tx) {
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
		x6100_lcd_text(lcd, VU_X + 8, VU_Y + VU_H - 20, 2, meter_names[rx->meter], SCR_ACCENT);
	} else {
		char l1[16], l2[16];

		vu_update(scr, meter_frac(scr->sig_dbm), now_ms());
		draw_sbar(lcd, meter_frac(scr->sig_dbm), scr->vu_peak, false, 1.0, 0);
		s_reading(scr->sig_dbm, l1, sizeof(l1));
		snprintf(l2, sizeof(l2), "%4.0fdBm", scr->sig_dbm);
		draw_vu(lcd, scr->needle, scr->vu_peak, VU_SCALE_S, l1, l2);

		// Overload (flow IQ at its ceiling), top left of the dial
		if (now_ms() < scr->ovf_until)
			x6100_lcd_text(lcd, VU_X + 8, VU_Y + 6, 2, "OVF", SCR_WARN);

		// Band check off (--oob / conf oob) and tuned outside the region's allocations
		if (g_allow_oob && !in_allocation(tx_hz(rx)))
			x6100_lcd_text(lcd, VU_X + 8, VU_Y + 24, 2, "OOB", SCR_WARN);

		// Baseband noise blanker / reduction on, top right of the dial
		{
			// Fixed slots: NB left, NR right, so neither moves when the other changes
			char dsp[12];

			snprintf(dsp, sizeof(dsp), "%s %s %s", rx->nb ? "NB" : "  ", rx->nr ? "NR" : "  ", rx->dnf ? "MN" : "  ");

			x6100_lcd_text(lcd, VU_X + VU_W - 8 - x6100_lcd_text_width(dsp, 2), VU_Y + 6, 2, dsp, SCR_ACCENT);
		}
		// TX meter selection (F4) shown dimmed, so RX already tells what TX will show
		x6100_lcd_text(lcd, VU_X + 8, VU_Y + VU_H - 20, 2, meter_names[rx->meter], SCR_DIM);
	}

	if (!scr->spec_valid)
		return;

	//
	// Scope view: RF (full width), AF (F3, full width) or both (F3 long: RF left,
	// AF right). A change of view starts a fresh waterfall.
	//
	int view = rx->asplit ? 2 : rx->ascope ? 1 : 0;

	g_rf_ref = (float) rx->rref;
	const int SW = view == 2 ? W * 5 / 8 : W;	/* RF scope width; the audio part gets 3 / 8 */

	if (view != scr->wf_view) {
		x6100_lcd_fill(lcd, 0, WF_Y, W, WF_H(lcd), 0x000000);
		scr->wf_view = view;
		scr->wf_scope = (scope_mode_t) -1;	/* RF part re-aligns too */
	}

	if (view == 1) {
		draw_audio_spec(scr, rx, 0, W);

		if (new_line) {
			x6100_lcd_scroll_down(lcd, WF_Y, WF_H(lcd));
			draw_audio_wf_line(scr, rx, 0, W);
		}

		goto overlays;
	}

	//
	// Spectrum in the scope window
	//
	view_t v = scope_view(rx, SW);
	bool fixed = rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S;

	x6100_lcd_fill(lcd, 0, SPEC_Y, SW, SPEC_H, 0x05080C);

	//
	// Waterfall alignment: a mode change clears it; in the scrolling modes a moved
	// window shifts it by the same number of pixels, so it stays frequency aligned
	//
	if (rx->scope != scr->wf_scope || rx->span != scr->wf_span) {
		x6100_lcd_fill(lcd, 0, WF_Y, SW, WF_H(lcd), 0x000000);
		scr->wf_scope = rx->scope;
		scr->wf_span = rx->span;
		scr->wf_ref = (double) v.lo;
	} else if (rx->scope == SCOPE_CENTER_S || rx->scope == SCOPE_FIX_S) {
		int dx = (int) lround((scr->wf_ref - (double) v.lo) * SW / v.span);

		if (dx != 0) {
			wf_shift(lcd, WF_Y, WF_H(lcd), dx, SW);
			scr->wf_ref -= (double) dx * v.span / SW;
		}
	} else {
		scr->wf_ref = (double) v.lo;
	}

	int pb_lo, pb_hi;

	passband(rx, &pb_lo, &pb_hi);

	{
		int x0 = f_to_x(&v, (int64_t) rx_hz(rx) + pb_lo);	/* receive passband, RIT included */
		int x1 = f_to_x(&v, (int64_t) rx_hz(rx) + pb_hi);

		x0 = x0 < 0 ? 0 : x0;
		x1 = x1 >= SW ? SW - 1 : x1;

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

			if (hz == 0 || gx < LBL_W + 12 || gx > SW - 24)
				continue;

			if (step >= 1000)
				snprintf(lbl, sizeof(lbl), "%+d", hz / 1000);
			else
				snprintf(lbl, sizeof(lbl), "%+.1f", hz / 1000.0);

			x6100_lcd_text(lcd, gx - x6100_lcd_text_width(lbl, 2) / 2, SPEC_Y + 4, 2, lbl, SCR_DIM);
		}
	}

	for (int db = (int) floorf((SPEC_DB_MAX - g_rf_ref) / GRID_DB) * GRID_DB; db > SPEC_DB_MIN - g_rf_ref; db -= GRID_DB) {
		int gy = SPEC_Y + SPEC_H - 1 - (int) (spec_norm((float) db) * (float) (SPEC_H - 1));

		x6100_lcd_hline(lcd, 0, SW - 1, gy, 0x1A2430);
	}

	int prev_y = -1;

	for (int x = 0; x < SW; x++) {
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
	for (int db = (int) floorf((SPEC_DB_MAX - g_rf_ref) / GRID_DB) * GRID_DB; db > SPEC_DB_MIN - g_rf_ref; db -= GRID_DB) {
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
	// With split / RIT / XIT: green line at the RX frequency, red at TX (when in view)
	if (rx->split || rx->rit || rx->xit) {
		int rxx = f_to_x(&v, rx_hz(rx)), txx = f_to_x(&v, tx_hz(rx));

		if (rxx >= 0 && rxx < SW)
			x6100_lcd_fill(lcd, rxx, SPEC_Y, 2, SPEC_H, 0x3CC83C);

		if (txx >= 0 && txx < SW) {
			x6100_lcd_fill(lcd, txx, SPEC_Y, 2, SPEC_H, SCR_WARN);
			x6100_lcd_fill(lcd, txx + 3, SPEC_Y + 44, x6100_lcd_text_width("TX", 2) + 4, 17, 0x000000);
			x6100_lcd_text(lcd, txx + 5, SPEC_Y + 46, 2, "TX", SCR_WARN);
		}
	} else {
		int cx = f_to_x(&v, rx->freq);

		if (cx >= 0 && cx < SW)
			x6100_lcd_vline(lcd, cx, SPEC_Y, SPEC_Y + SPEC_H - 1, SCR_WARN);
		else if (cx < 0)
			x6100_lcd_text(lcd, LBL_W + 2, SPEC_Y + SPEC_H / 2 - 7, 2, "<", SCR_WARN);
		else
			x6100_lcd_text(lcd, SW - 14, SPEC_Y + SPEC_H / 2 - 7, 2, ">", SCR_WARN);
	}

	//
	// F3 long: the audio scope in the right half, a divider between the two
	//
	if (view == 2) {
		static const char *ranges[] = { "AF 0-3K", "AF 0-6K" };

		draw_audio_spec(scr, rx, SW, W - SW);
		x6100_lcd_vline(lcd, SW, SPEC_Y, lcd->h - TAG_H - 1, 0x40C0FF);
		x6100_lcd_fill(lcd, SW + LBL_W + 4, SPEC_Y + 22, x6100_lcd_text_width(ranges[rx->arange], 2) + 8, 18, 0x000000);
		x6100_lcd_text(lcd, SW + LBL_W + 8, SPEC_Y + 24, 2, ranges[rx->arange], SCR_ACCENT);
	}

overlays:
	//
	// Split / RIT / XIT in use: top left of the spectrum, amber
	//
	if (rx->split || rx->rit || rx->xit) {
		static const char *mn[] = { [X6100_MODE_LSB] = "LSB", [X6100_MODE_USB] = "USB", [X6100_MODE_CW] = "CW",
			[X6100_MODE_CWR] = "CWR", [X6100_MODE_AM] = "AM", [X6100_MODE_NFM] = "NFM" };
		char m[48] = "";
		int n = 0;
		uint32_t f = tx_hz(rx);

		if (rx->split)
			n += snprintf(m + n, sizeof(m) - (size_t) n, "SPLIT TX %u.%03u.%02u %s  ", f / 1000000, f / 1000 % 1000,
				f % 1000 / 10, mn[tx_mode(rx)] ? mn[tx_mode(rx)] : "");

		if (rx->rit || rx->xit)
			snprintf(m + n, sizeof(m) - (size_t) n, "%s%s%s %+d", rx->rit ? "RIT" : "", rx->rit && rx->xit ? "/" : "",
				rx->xit ? "XIT" : "", rx->rit_hz);

		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 22, x6100_lcd_text_width(m, 2) + 8, 18, 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 24, 2, m, 0xFFB020);
	}

	//
	// Toast: a short confirmation in the middle of the spectrum (VFO A / B, A=B)
	//
	if (g_toast[0] != '\0' && now_ms() < g_toast_until) {
		int tw = x6100_lcd_text_width(g_toast, 3);
		int tx = (W - tw) / 2, ty = SPEC_Y + SPEC_H / 2 - 16;

		x6100_lcd_fill(lcd, tx - 14, ty - 8, tw + 28, 38, 0x000000);
		x6100_lcd_hline(lcd, tx - 14, tx + tw + 13, ty - 8, SCR_ACCENT);
		x6100_lcd_hline(lcd, tx - 14, tx + tw + 13, ty + 29, SCR_ACCENT);
		x6100_lcd_text(lcd, tx, ty, 3, g_toast, SCR_ACCENT);
	}

	//
	// Reference level edit (F2 long): which scope and its level, bottom left of
	// that scope part; amber while editing
	//
	if (rx->redit) {
		char m[24];
		int x0 = view == 2 && rx->redit_af ? SW : 0;
		int tw;

		snprintf(m, sizeof(m), "REF %s %+ddB", rx->redit_af ? "AF" : "RF", rx->redit_af ? rx->aref : rx->rref);
		tw = x6100_lcd_text_width(m, 2);
		x6100_lcd_fill(lcd, x0 + LBL_W + 4, SPEC_Y + SPEC_H - 24, tw + 8, 20, 0x000000);
		x6100_lcd_text(lcd, x0 + LBL_W + 8, SPEC_Y + SPEC_H - 22, 2, m, 0xFFB020);
	}

	//
	// Scope mode, top right of the spectrum
	//
	{
		static const char *names[] = { "CENTER", "SCROLL-C", "FIX", "SCROLL-F" };
		static const char *ranges[] = { "0-3K", "0-6K" };
		char m[24];
		int tw;

		if (rx->ascope)
			snprintf(m, sizeof(m), "AUDIO %s", ranges[rx->arange]);
		else if (rx->span % 2000 == 0)
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

	//
	// Clock under the filter label: system time (kept by the OS / network for
	// now), UTC, ISO 8601. Drawn before the panels, which cover it while open.
	//
	{
		time_t now = time(NULL);
		struct tm tm;
		char m[32];
		int tw;

		if (gmtime_r(&now, &tm) != NULL && strftime(m, sizeof(m), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0) {
			tw = x6100_lcd_text_width(m, 2);
			x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 60, tw + 8, 18, 0x000000);
			x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 62, 2, m, SCR_DIM);
		}
	}

	if (rx->fedit)
		draw_filter_panel(lcd, rx);

	if (rx->dedit)
		draw_dsp_panel(lcd, rx);

	if (rx->tedit)
		draw_rit_panel(lcd, rx);

	if (rx->gedit)
		draw_gen_panel(lcd, rx);

	if (rx->ginfo)
		draw_gen_info(lcd, scr);

	if (rx->tuning || (rx->tune_done && now_ms() - rx->tune_done < TUNE_SHOW_MS))
		draw_tune_panel(lcd, rx, scr);

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
	if (new_line && view != 1) {
		x6100_lcd_scroll_down(lcd, WF_Y, WF_H(lcd));

		for (int x = 0; x < SW; x++) {
			uint32_t c = spec_index(&v, x) < 0 ? 0x000000
				: wf_colour((spec_at(scr, &v, x) - (scr->floor_db - WF_BELOW_DB)) / WF_RANGE_DB);

			x6100_lcd_fill(lcd, x, WF_Y, 1, 1, c);
		}

		if (view == 2)
			draw_audio_wf_line(scr, rx, SW, W - SW);
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

	if (strcasecmp(t, "nr") == 0 || strcasecmp(t, "nonr") == 0) {
		rx->nr = strcasecmp(t, "nr") == 0;
		return true;
	}

	if (strcasecmp(t, "notch") == 0 || strcasecmp(t, "nonotch") == 0) {
		rx->dnf = strcasecmp(t, "notch") == 0;
		return true;
	}

	if (strcasecmp(t, "nb") == 0 || strcasecmp(t, "nonb") == 0) {
		rx->nb = strcasecmp(t, "nb") == 0;
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

	// Split (TX on the other VFO), RIT / XIT on / off
	if (strcasecmp(t, "txsplit") == 0 || strcasecmp(t, "notxsplit") == 0) {
		rx->split = strcasecmp(t, "txsplit") == 0;
		return true;
	}

	if (strcasecmp(t, "rit") == 0 || strcasecmp(t, "norit") == 0) {
		rx->rit = strcasecmp(t, "rit") == 0;
		return true;
	}

	if (strcasecmp(t, "xit") == 0 || strcasecmp(t, "noxit") == 0) {
		rx->xit = strcasecmp(t, "xit") == 0;
		return true;
	}

	// VFO A <-> B, A=B (as the A/B key short / long)
	if (strcasecmp(t, "ab") == 0) {
		vfo_swap(rx);
		return true;
	}

	if (strcasecmp(t, "a=b") == 0) {
		vfo_equal(rx);
		return true;
	}

	// Safety checks off, as --oob / --no-limit (the operator's responsibility)
	if (strcasecmp(t, "oob") == 0) {
		g_allow_oob = true;
		return true;
	}

	if (strcasecmp(t, "nolimit") == 0) {
		g_no_limit = true;
		return true;
	}

	if (strcasecmp(t, "mute") == 0 || strcasecmp(t, "nomute") == 0) {
		rx->mute = strcasecmp(t, "mute") == 0;
		return true;
	}

	if (strcasecmp(t, "atu") == 0 || strcasecmp(t, "noatu") == 0) {
		rx->atu_on = strcasecmp(t, "atu") == 0;
		return true;
	}

	// Scope view: RF, audio, or both side by side; audio range
	if (strcasecmp(t, "rf") == 0 || strcasecmp(t, "af") == 0 || strcasecmp(t, "split") == 0) {
		rx->ascope = strcasecmp(t, "af") == 0;
		rx->asplit = strcasecmp(t, "split") == 0;
		return true;
	}

	if (strcasecmp(t, "af3") == 0 || strcasecmp(t, "af6") == 0) {
		rx->arange = strcasecmp(t, "af6") == 0;
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

//
// Volume 0-100 from a token; also unmutes
//

static bool vol_parse(rx_t *rx, const char *t)
{
	char *end;
	long v = strtol(t, &end, 10);

	if (*t == '\0' || *end != '\0' || v < 0 || v > 100) {
		fprintf(stderr, "\nvol must be 0 to 100\n");
		return false;
	}

	rx->vol = (int) v;
	rx->mute = false;
	return true;
}

static bool console_line(rx_t *rx, char *line)
{
	bool changed = false;
	bool want_step = false;
	bool want_pwr = false;
	bool want_vol = false;
	int *want_lvl = NULL;	/* nrl / nbl / nbw: the level the next token sets */
	int lvl_min = 0, lvl_max = 0;
	int want_band = -1;	/* band <name> <Hz>: -2 = name next, >= 0 = that band's Hz next */
	bool want_span = false;

	for (char *t = strtok(line, " \t\r\n"); t != NULL; t = strtok(NULL, " \t\r\n")) {
		if (want_step) {
			int st = atoi(t);

			want_step = false;

			st = strcasecmp(t, "ch") == 0 ? STEP_CH : st;

			if (st == 1 || st == 10 || st == 100 || st == 1000 || st == 5000 || st == 9000 || st == 10000 || st == STEP_CH) {
				rx->step = st;
				changed = true;
			} else {
				fprintf(stderr, "\nstep must be 1, 10, 100, 1000, 5000 (9000, 10000 on LW / MW AM)\n");
			}

			continue;
		}

		if (want_lvl != NULL) {
			char *end;
			long v = strtol(t, &end, 10);

			if (*end == '\0' && v >= lvl_min && v <= lvl_max) {
				*want_lvl = (int) v;
				changed = true;
			} else {
				fprintf(stderr, "\nmust be %d to %d\n", lvl_min, lvl_max);
			}

			want_lvl = NULL;
			continue;
		}

		if (want_vol) {
			want_vol = false;

			if (vol_parse(rx, t))
				changed = true;

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

		if (strcasecmp(t, "nrl") == 0 || strcasecmp(t, "nbl") == 0 || strcasecmp(t, "nbw") == 0
			|| strcasecmp(t, "ntf") == 0 || strcasecmp(t, "ntw") == 0) {
			int i = strcasecmp(t, "nrl") == 0 ? 0 : strcasecmp(t, "nbl") == 0 ? 1 : strcasecmp(t, "nbw") == 0 ? 2
				: strcasecmp(t, "ntf") == 0 ? 3 : 4;

			want_lvl = dsp_value(rx, i);
			lvl_min = g_dsp[i].min;
			lvl_max = g_dsp[i].max;
			continue;
		}

		if (want_band >= 0) {
			uint32_t f = (uint32_t) strtoul(t, NULL, 10);

			if (f >= g_bands[want_band].low && f <= g_bands[want_band].high) {
				rx->band_freq[want_band] = f;
				changed = true;
			} else {
				fprintf(stderr, "\nband %s: %s outside the band\n", g_bands[want_band].name, t);
			}

			want_band = -1;
			continue;
		}

		if (want_band == -2) {
			want_band = -1;

			for (int i = 0; i < BAND_COUNT; i++) {
				if (strcasecmp(t, g_bands[i].name) == 0)
					want_band = i;
			}

			if (want_band < 0) {
				fprintf(stderr, "\nband: unknown band %s\n", t);
				want_band = -3;	/* skip its frequency */
			}

			continue;
		}

		if (want_band == -3) {
			want_band = -1;
			continue;
		}

		if (want_span) {
			int v = atoi(t);

			want_span = false;

			for (int i = 0; i < SPAN_COUNT; i++) {
				if (g_spans[i] == v) {
					rx->span = v;
					changed = true;
				}
			}

			if (rx->span != v)
				fprintf(stderr, "\nspan must be 96000, 48000, 24000, 12000, 6000 or 3000\n");

			continue;
		}

		if (strcasecmp(t, "ref") == 0 || strcasecmp(t, "aref") == 0) {
			want_lvl = strcasecmp(t, "ref") == 0 ? &rx->rref : &rx->aref;
			lvl_min = -REF_MAX_DB;
			lvl_max = REF_MAX_DB;
			continue;
		}

		// cat 19200 | cat off: CI-V on ttyS2 (read at start only)
		if (strcasecmp(t, "cat") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int b = v == NULL ? -1 : strcasecmp(v, "off") == 0 ? 0 : atoi(v);

			if (b == 0 || b == 4800 || b == 9600 || b == 19200 || b == 38400 || b == 57600 || b == 115200) {
				g_cat_baud = b;
				g_conf_set |= g_conf_reading ? 1u << GEN_CAT : 0;
			} else {
				fprintf(stderr, "\ncat: off, 4800, 9600, 19200, 38400, 57600 or 115200\n");
			}

			continue;
		}

		// ntp <server> | ntp off: the one sync at start (read at start only)
		if (strcasecmp(t, "ntp") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL) {
				snprintf(g_ntp_server, sizeof(g_ntp_server), "%s", strcasecmp(v, "on") == 0 ? "" : v);
				g_conf_set |= g_conf_reading ? 1u << GEN_NTP : 0;
			} else {
				fprintf(stderr, "\nntp: <server>, on or off\n");
			}

			continue;
		}

		// vfob <Hz> <mode> fil<n>: the VFO not in use; vfo a|b: which one is in use (label)
		if (strcasecmp(t, "vfob") == 0) {
			const char *fs = strtok(NULL, " \t\r\n"), *ms = strtok(NULL, " \t\r\n"), *ls = strtok(NULL, " \t\r\n");
			uint32_t hz = fs == NULL ? 0 : (uint32_t) strtoul(fs, NULL, 10);
			static rx_t tmp;

			tmp = *rx;

			if (hz < FREQ_MIN || hz > FREQ_MAX || ms == NULL || !console_token(&tmp, ms) || ls == NULL
				|| strncasecmp(ls, "fil", 3) != 0 || ls[3] < '1' || ls[3] > '3') {
				fprintf(stderr, "\nvfob: <Hz> <mode> fil1|fil2|fil3\n");
				continue;
			}

			rx->b_freq = hz;
			rx->b_mode = tmp.mode;
			rx->b_fil = ls[3] - '1';
			changed = true;
			continue;
		}

		if (strcasecmp(t, "vfo") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "a") == 0 || strcasecmp(v, "b") == 0))
				rx->on_b = strcasecmp(v, "b") == 0;
			else
				fprintf(stderr, "\nvfo: a or b\n");

			continue;
		}

		// region r1|r2|r3|nl: transmit allocations for the TX check and OOB marker
		if (strcasecmp(t, "region") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int r = -1;

			for (int i = 0; v != NULL && i < REGION_COUNT; i++)
				r = strcasecmp(v, g_regions[i].name) == 0 ? i : r;

			if (r >= 0) {
				g_region = r;
				changed = true;
			} else {
				fprintf(stderr, "\nregion: r1, r2, r3 or nl\n");
			}

			continue;
		}

		// bright 5-100: display backlight in % (GEN)
		if (strcasecmp(t, "bright") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int b = v == NULL ? 0 : atoi(v);

			if (b >= 5 && b <= 100) {
				g_bright = b;
				g_conf_set |= g_conf_reading ? 1u << GEN_BRIGHT : 0;
				backlight_level(b);	/* nothing yet at start: applied again when the display opens */
			} else {
				fprintf(stderr, "\nbright: 5 to 100\n");
			}

			continue;
		}

		// agc ssb|ssbd|cw|am|fm off|slow|fast|auto: AGC of a mode group
		if (strcasecmp(t, "agc") == 0) {
			static const char *agcs[] = { [X6100_AGC_OFF] = "off", [X6100_AGC_SLOW] = "slow",
				[X6100_AGC_FAST] = "fast", [X6100_AGC_AUTO] = "auto" };
			const char *gs = strtok(NULL, " \t\r\n"), *as = strtok(NULL, " \t\r\n");
			int g = -1, a = -1;

			for (int i = 0; gs != NULL && i < FG_COUNT; i++)
				g = strcasecmp(gs, g_fg_name[i]) == 0 ? i : g;

			for (int i = 0; as != NULL && i < 4; i++)
				a = strcasecmp(as, agcs[i]) == 0 ? i : a;

			if (g < 0 || a < 0) {
				fprintf(stderr, "\nagc: ssb|ssbd|cw|am|fm off|slow|fast|auto\n");
				continue;
			}

			rx->agc_group[g] = (x6100_agc_t) a;

			if (g == (int) mode_group(rx->mode))
				rx->agc = (x6100_agc_t) a;

			changed = true;
			continue;
		}

		// atunet 40m L C S: tuned network of a band (as the tuner found it)
		if (strcasecmp(t, "atunet") == 0) {
			const char *bs = strtok(NULL, " \t\r\n");
			const char *ls = strtok(NULL, " \t\r\n"), *cs = strtok(NULL, " \t\r\n"), *ss = strtok(NULL, " \t\r\n");
			int b = -1;

			for (int i = 0; bs != NULL && i < BAND_COUNT; i++)
				b = strcasecmp(bs, g_bands[i].name) == 0 ? i : b;

			if (b < 0 || ls == NULL || cs == NULL || ss == NULL) {
				fprintf(stderr, "\natunet: <band> <L> <C> <S>\n");
				continue;
			}

			rx->atu_net[b].l = (uint8_t) atoi(ls);
			rx->atu_net[b].c = (uint8_t) atoi(cs);
			rx->atu_net[b].s = (uint8_t) atoi(ss);
			rx->atu_valid[b] = true;
			changed = true;
			continue;
		}

		if (strcasecmp(t, "ritofs") == 0) {
			want_lvl = &rx->rit_hz;
			lvl_min = -RIT_MAX_HZ;
			lvl_max = RIT_MAX_HZ;
			continue;
		}

		if (strcasecmp(t, "rfg") == 0) {
			want_lvl = &rx->rfg;
			lvl_min = 0;
			lvl_max = 100;
			continue;
		}

		if (strcasecmp(t, "band") == 0) {
			want_band = -2;
			continue;
		}

		if (strcasecmp(t, "span") == 0) {
			want_span = true;
			continue;
		}

		if (strcasecmp(t, "vol") == 0) {
			want_vol = true;
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

//
// Settings files, both in console tokens (one or more per line, # comment):
//   STATE_PATH  last state, written by x6100_trx at exit
//   CONF_PATH   the user's own settings, never written by x6100_trx
// Read at start in that order, then the command line: later wins.
//

#define CONF_PATH	"/etc/x6100/x6100_trx.conf"
#define STATE_PATH	"/etc/x6100/x6100_trx.state"

static void conf_load(rx_t *rx, const char *path, bool quiet)
{
	FILE *fp;
	char line[256];

	if ((fp = fopen(path, "r")) == NULL)
		return;

	g_conf_reading = strcmp(path, CONF_PATH) == 0;


	while (fgets(line, sizeof(line), fp) != NULL) {
		char *hash = strchr(line, '#');

		if (hash != NULL)
			*hash = '\0';

		console_line(rx, line);
	}

	fclose(fp);
	g_conf_reading = false;

	if (!quiet)
		fprintf(stderr, "settings: %s\n", path);
}

static void state_save(const rx_t *rx)
{
	static const char *modes[] = { [X6100_MODE_LSB] = "lsb", [X6100_MODE_USB] = "usb", [X6100_MODE_CW] = "cw",
		[X6100_MODE_CWR] = "cwr", [X6100_MODE_AM] = "am", [X6100_MODE_NFM] = "nfm" };
	static const char *agcs[] = { [X6100_AGC_OFF] = "off", [X6100_AGC_SLOW] = "slow",
		[X6100_AGC_FAST] = "fast", [X6100_AGC_AUTO] = "auto" };
	static const char *scopes[] = { [SCOPE_CENTER] = "center", [SCOPE_CENTER_S] = "scrollc",
		[SCOPE_FIX] = "fix", [SCOPE_FIX_S] = "scrollf" };
	char tmp[] = STATE_PATH ".tmp";
	FILE *fp;

	if ((fp = fopen(tmp, "w")) == NULL) {
		fprintf(stderr, "\nstate: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}

	fprintf(fp, "# x6100_trx last state, written at exit. Do not edit: put your own\n"
		"# settings in x6100_trx.conf, which is read after this file.\n\n");

	for (int i = 0; i < BAND_COUNT; i++)
		fprintf(fp, "band %s %u\n", g_bands[i].name, (unsigned) rx->band_freq[i]);

	for (int i = 0; i < BAND_COUNT; i++) {
		if (rx->atu_valid[i])
			fprintf(fp, "atunet %s %u %u %u\n", g_bands[i].name, rx->atu_net[i].l, rx->atu_net[i].c, rx->atu_net[i].s);
	}

	fprintf(fp, "\n");

	for (int g = 0; g < FG_COUNT; g++)
		fprintf(fp, "agc %s %s\n", g_fg_name[g], agcs[rx->agc_group[g]]);

	fprintf(fp, "\n%u %s %s fil%d step %d\n", (unsigned) rx->freq, modes[rx->mode], agcs[rx->agc], rx->fil + 1, rx->step);
	fprintf(fp, "vfob %u %s fil%d vfo %s\n", (unsigned) rx->b_freq, modes[rx->b_mode], rx->b_fil + 1, rx->on_b ? "b" : "a");
	fprintf(fp, "%s %s %s ritofs %d\n", rx->split ? "txsplit" : "notxsplit", rx->rit ? "rit" : "norit", rx->xit ? "xit" : "noxit", rx->rit_hz);
	fprintf(fp, "vol %d %s rfg %d pwr %.1f\n", rx->vol, rx->mute ? "mute" : "nomute", rx->rfg, rx->txpwr / 10.0);
	fprintf(fp, "%s %s %s\n", rx->att ? "att" : "noatt", rx->pre ? "pre" : "nopre", rx->atu_on ? "atu" : "noatu");
	fprintf(fp, "%s %s %s\n", rx->nr ? "nr" : "nonr", rx->nb ? "nb" : "nonb", rx->dnf ? "notch" : "nonotch");
	fprintf(fp, "nrl %d nbl %d nbw %d ntf %d ntw %d\n", rx->nr_lvl, rx->nb_lvl, rx->nb_w, rx->dnf_f, rx->dnf_w);
	// GEN settings (a server from the conf is not repeated here: the conf sets it again)
	if (g_cat_baud > 0)
		fprintf(fp, "cat %d ", g_cat_baud);
	else
		fprintf(fp, "cat off ");

	fprintf(fp, "ntp %s", strcmp(g_ntp_server, "off") == 0 ? "off" : "on");

	if (g_bright > 0)
		fprintf(fp, " bright %d", g_bright);

	fprintf(fp, "\n");
	fprintf(fp, "%s span %d %s %s ref %d aref %d\n", scopes[rx->scope], rx->span,
		rx->asplit ? "split" : rx->ascope ? "af" : "rf", rx->arange ? "af6" : "af3", rx->rref, rx->aref);

	if (fclose(fp) != 0 || rename(tmp, STATE_PATH) != 0)
		fprintf(stderr, "\nstate: cannot save %s: %s\n", STATE_PATH, strerror(errno));
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

//
// CAT: CI-V over the USB serial port (IC-705 identity, see docs/x6100_civ.md).
// The engine (x6100_civ.c) calls back into the receiver state; a change sets
// cat.changed, and the main loop applies it like a key press.
//

#define CAT_DEV		"/dev/ttyS2"	/* "DEV" USB-C: CH342 port 2 (PC ttyACM1); port 1 is the console */
#define CAT_BAUD_DEF	19200	/* stock / IU1WXY default; verified 2026-10-23 */

typedef struct
{
	rx_t *rx;
	const screen_t *scr;
	int fd;
	x6100_civ_t civ;
	x6100_civ_framer_t framer;
	bool changed;

	//
	// VFO B: shadow only, the baseband runs one VFO; 07 / B0 swaps it in
	//

	//
	// Last state announced (transceive 00 / 01)
	//
	uint32_t ann_freq;
	int ann_mode;
	int ann_fil;

	//
	// Scope data (27): client settings and pacing
	//
	bool scope_on;
	bool scope_out;
	bool scope_hold;
	bool scope_vbw;
	int scope_edge;
	int scope_ref;		/* 0.1 dB */
	int scope_speed;	/* 0 fast, 1 mid, 2 slow */
	uint64_t scope_ms;
} cat_t;

// IC-705 band code (1A 01) to our band index; 60 m has no code
static int civ_band(int code)
{
	static const int map[11] = { -1, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10 };

	return code >= 1 && code <= 10 ? map[code] : -1;
}

// Our scope modes to the IC-705 order (0 CENTER, 1 FIX, 2 SCROLL-C, 3 SCROLL-F)
static int civ_scope_mode(scope_mode_t m)
{
	return m == SCOPE_FIX ? 1 : m == SCOPE_CENTER_S ? 2 : m == SCOPE_FIX_S ? 3 : 0;
}

static int civ_scale(double v, const double (*pts)[2], int n)
{
	if (v <= pts[0][0])
		return (int) pts[0][1];

	for (int i = 1; i < n; i++) {
		if (v <= pts[i][0])
			return (int) lround(pts[i - 1][1] + (v - pts[i - 1][0]) * (pts[i][1] - pts[i - 1][1])
				/ (pts[i][0] - pts[i - 1][0]));
	}

	return (int) pts[n - 1][1];
}

// IC-705 meter scales (CI-V reference, command 15)
static const double g_s_pts[][2] = { { -127, 0 }, { -73, 120 }, { -13, 241 }, { 0, 255 } };
static const double g_po_pts[][2] = { { 0, 0 }, { 5, 143 }, { 10, 213 }, { 12, 255 } };
static const double g_swr_pts[][2] = { { 1.0, 0 }, { 1.5, 48 }, { 2.0, 80 }, { 3.0, 120 }, { 6.0, 255 } };
static const double g_volt_pts[][2] = { { 0, 0 }, { 5, 75 }, { 16, 241 }, { 17, 255 } };

#define PTS(a)	a, (int) (sizeof(a) / sizeof((a)[0]))

static int fil_width(const rx_t *rx)
{
	fgroup_t g = mode_group(rx->mode);
	uint32_t lo = g_fil[g][rx->fil][0], hi = g_fil[g][rx->fil][1];

	return g == FG_AM || g == FG_FM ? (int) (2 * hi) : (int) (hi - lo);
}

// Width set by CI-V: BW steps around the current centre, saved like the panel
static void fil_set_width(rx_t *rx, int hz)
{
	fgroup_t g = mode_group(rx->mode);
	int step = g == FG_CW ? 50 : 100;
	int cur = fil_width(rx);

	if (g == FG_AM || g == FG_FM)
		fil_bw(g, rx->fil, (hz - cur) / 2 / step);
	else
		fil_bw(g, rx->fil, (hz - cur) / step);

	fil_save();
}

static bool cat_get(void *ctx, x6100_civ_item_t item, int vfo, int32_t *v)
{
	cat_t *cat = ctx;
	const rx_t *rx = cat->rx;
	const screen_t *scr = cat->scr;

	switch (item) {
	case X6100_CIV_FREQ:
		*v = (int32_t) (vfo ? rx->b_freq : rx->freq);
		return true;
	case X6100_CIV_MODE:
		*v = vfo ? rx->b_mode : rx->mode;
		return true;
	case X6100_CIV_FIL:
		*v = (vfo ? rx->b_fil : rx->fil) + 1;
		return true;
	case X6100_CIV_FIL_WIDTH:
		*v = fil_width(rx);
		return true;
	case X6100_CIV_SPLIT:
		*v = rx->split;
		return true;
	case X6100_CIV_RIT:
		*v = rx->rit;
		return true;
	case X6100_CIV_XIT:
		*v = rx->xit;
		return true;
	case X6100_CIV_RIT_HZ:
		*v = rx->rit_hz;
		return true;
	case X6100_CIV_STEP:
		*v = rx->step == STEP_CH ? 1000 : rx->step;	/* 60 m CH: report 1 kHz */
		return true;
	case X6100_CIV_ATT:
		*v = rx->att;
		return true;
	case X6100_CIV_PRE:
		*v = rx->pre;
		return true;
	case X6100_CIV_AGC:
		// AUTO reads as MID, OFF as 0 (outside 01-03: the IC-705 has no AGC off here)
		*v = rx->agc == X6100_AGC_FAST ? 1 : rx->agc == X6100_AGC_SLOW ? 3 : rx->agc == X6100_AGC_AUTO ? 2 : 0;
		return true;
	case X6100_CIV_LOCK:
		*v = rx->lock;
		return true;
	case X6100_CIV_LVL_AF:
		*v = rx->vol * 255 / 100;
		return true;
	case X6100_CIV_LVL_RF:
		*v = rx->rfg * 255 / 100;
		return true;
	case X6100_CIV_LVL_PWR:
		*v = rx->txpwr * 255 / TXPWR_MAX;
		return true;
	case X6100_CIV_MTR_S:
		*v = civ_scale(scr->sig_dbm, PTS(g_s_pts));
		return true;
	case X6100_CIV_MTR_PWR:
		*v = scr->tx ? civ_scale(scr->pwr, PTS(g_po_pts)) : 0;
		return true;
	case X6100_CIV_MTR_SWR:
		*v = scr->tx ? civ_scale(scr->swr, PTS(g_swr_pts)) : 0;
		return true;
	case X6100_CIV_MTR_ALC:
		*v = scr->tx && scr->alc < 100 ? scr->alc * 120 / 100 : 0;
		return true;
	case X6100_CIV_MTR_VOLT:
		*v = civ_scale(scr->vbat, PTS(g_volt_pts));
		return true;
	case X6100_CIV_OVF:
		*v = now_ms() < scr->ovf_until;
		return true;
	case X6100_CIV_SQL_OPEN:
		*v = 1;		/* no squelch */
		return true;
	case X6100_CIV_TX:
		*v = scr->tx || rx->ptt;
		return true;
	case X6100_CIV_SCOPE_ON:
		*v = cat->scope_on;
		return true;
	case X6100_CIV_SCOPE_OUT:
		*v = cat->scope_out;
		return true;
	case X6100_CIV_SCOPE_MODE:
		*v = civ_scope_mode(rx->scope);
		return true;
	case X6100_CIV_SCOPE_SPAN:
		*v = rx->span / 2;
		return true;
	case X6100_CIV_SCOPE_EDGE:
		*v = cat->scope_edge;
		return true;
	case X6100_CIV_SCOPE_HOLD:
		*v = cat->scope_hold;
		return true;
	case X6100_CIV_SCOPE_REF:
		*v = cat->scope_ref;
		return true;
	case X6100_CIV_SCOPE_SPEED:
		*v = cat->scope_speed;
		return true;
	case X6100_CIV_SCOPE_VBW:
		*v = cat->scope_vbw;
		return true;
	case X6100_CIV_BAND_FREQ:
	case X6100_CIV_BAND_MODE:
	case X6100_CIV_BAND_FIL: {
		int b = civ_band(vfo);

		if (b < 0)
			return false;

		if (item == X6100_CIV_BAND_FREQ)
			*v = (int32_t) rx->band_freq[b];
		else if (item == X6100_CIV_BAND_MODE)
			*v = b == rx->band ? rx->mode : rx->band_freq[b] < 10000000 ? X6100_MODE_LSB : X6100_MODE_USB;
		else
			*v = rx->fil + 1;

		return true;
	}
	case X6100_CIV_NR:
		*v = rx->nr;
		return true;
	case X6100_CIV_NB:
		*v = rx->nb;
		return true;
	case X6100_CIV_LVL_NR:
		*v = rx->nr_lvl * 255 / 60;
		return true;
	case X6100_CIV_LVL_NB:
		*v = rx->nb_lvl * 255 / 100;
		return true;
	case X6100_CIV_DNF:
		*v = rx->dnf;
		return true;
	case X6100_CIV_LVL_DNF:
		*v = (rx->dnf_f - 100) * 255 / 2900;	/* 0000-0255 over 100-3000 Hz */
		return true;
	case X6100_CIV_ATU:
		*v = rx->tuning ? 2 : rx->atu_on;
		return true;
	default:
		return false;	/* NB, NR, DNF, COMP, VOX, MONI, RIT, other levels */
	}
}

static bool cat_set(void *ctx, x6100_civ_item_t item, int vfo, int32_t v)
{
	cat_t *cat = ctx;
	rx_t *rx = cat->rx;

	switch (item) {
	case X6100_CIV_FREQ:
		if (v < (int32_t) FREQ_MIN || v > (int32_t) FREQ_MAX)
			return false;

		if (vfo)
			rx->b_freq = (uint32_t) v;
		else
			set_freq(rx, v);

		break;
	case X6100_CIV_MODE:
		if (vfo)
			rx->b_mode = (x6100_mode_t) v;
		else
			rx->mode = (x6100_mode_t) v;

		break;
	case X6100_CIV_FIL:
		if (v < 1 || v > 3)
			return false;

		if (vfo)
			rx->b_fil = v - 1;
		else
			rx->fil = v - 1;

		break;
	case X6100_CIV_FIL_WIDTH:
		fil_set_width(rx, v);
		break;
	case X6100_CIV_VFO:
		if ((v == 1) != rx->on_b)
			vfo_swap(cat->rx);

		break;
	case X6100_CIV_VFO_SWAP:
		vfo_swap(cat->rx);
		break;
	case X6100_CIV_VFO_EQUAL:
		vfo_equal(rx);
		break;
	case X6100_CIV_SPLIT:
		rx->split = v != 0;
		break;
	case X6100_CIV_RIT:
		rx->rit = v != 0;
		break;
	case X6100_CIV_XIT:
		rx->xit = v != 0;
		break;
	case X6100_CIV_RIT_HZ:
		if (v < -RIT_MAX_HZ || v > RIT_MAX_HZ)
			return false;

		rx->rit_hz = v;
		break;
	case X6100_CIV_STEP:
		if (v < 1 || v > 100000)
			return false;

		rx->step = v;
		break;
	case X6100_CIV_ATT:
		rx->att = v != 0;
		break;
	case X6100_CIV_PRE:
		rx->pre = v != 0;
		break;
	case X6100_CIV_AGC:
		rx->agc = v == 1 ? X6100_AGC_FAST : v == 3 ? X6100_AGC_SLOW : X6100_AGC_AUTO;
		rx->agc_group[mode_group(rx->mode)] = rx->agc;
		break;
	case X6100_CIV_LOCK:
		rx->lock = v != 0;
		break;
	case X6100_CIV_LVL_AF:
		rx->vol = v * 100 / 255;
		break;
	case X6100_CIV_LVL_RF:
		rx->rfg = v * 100 / 255;
		break;
	case X6100_CIV_LVL_PWR:
		rx->txpwr = clampi((v * TXPWR_MAX + 127) / 255, 1, TXPWR_MAX);
		break;
	case X6100_CIV_PTT:
		// CAT PTT latches like a PTT tap; same band check, timeout and SWR trip
		if (v) {
			// During a tune: abort it, as the PTT key does (no TX from this command)
			if (rx->tuning) {
				tx_drop(rx, "TUNE ABORTED");
				break;
			}

			if (rx->ptt)
				return true;

			if (!tx_allowed(rx))
				return false;

			rx->ptt = true;
			rx->ptt_latch = true;
			rx->ptt_since = now_ms();
			rx->warn = NULL;
		} else {
			tx_drop(rx, NULL);
		}

		break;
	case X6100_CIV_SCOPE_ON:
		cat->scope_on = v != 0;
		break;
	case X6100_CIV_SCOPE_OUT:
		cat->scope_out = v != 0;
		break;
	case X6100_CIV_SCOPE_MODE: {
		if (v < 0 || v > 3)
			return false;

		static const scope_mode_t ours[4] = { SCOPE_CENTER, SCOPE_FIX, SCOPE_CENTER_S, SCOPE_FIX_S };
		bool was_fix = rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S;

		rx->scope = ours[v];

		if (!was_fix && (rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S))
			scope_fix_start(rx);

		break;
	}
	case X6100_CIV_SCOPE_SPAN: {
		// Nearest of our spans that covers the request (IC-705 spans are wider)
		int span = g_spans[0];
		int64_t centre = rx->fix_lo + rx->span / 2;

		for (int i = 0; i < SPAN_COUNT; i++) {
			if (g_spans[i] / 2 >= v)
				span = g_spans[i];
		}

		rx->span = span;

		if (rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S)
			rx->fix_lo = centre - rx->span / 2;

		break;
	}
	case X6100_CIV_SCOPE_EDGE:
		if (v < 1 || v > 4)
			return false;

		cat->scope_edge = v;
		break;
	case X6100_CIV_SCOPE_HOLD:
		cat->scope_hold = v != 0;
		break;
	case X6100_CIV_SCOPE_REF:
		if (v < -200 || v > 200)
			return false;

		cat->scope_ref = v;
		break;
	case X6100_CIV_SCOPE_SPEED:
		if (v < 0 || v > 2)
			return false;

		cat->scope_speed = v;
		break;
	case X6100_CIV_SCOPE_VBW:
		cat->scope_vbw = v != 0;
		break;
	case X6100_CIV_BAND_FREQ: {
		int b = civ_band(vfo);

		if (b < 0 || (uint32_t) v < g_bands[b].low || (uint32_t) v > g_bands[b].high)
			return false;

		rx->band_freq[b] = (uint32_t) v;
		return true;	/* stored only, the current frequency does not change */
	}
	case X6100_CIV_BAND_MODE:
	case X6100_CIV_BAND_FIL:
		return civ_band(vfo) >= 0;	/* no per-band mode / filter memory */
	case X6100_CIV_NR:
		rx->nr = v != 0;
		break;
	case X6100_CIV_NB:
		rx->nb = v != 0;
		break;
	case X6100_CIV_LVL_NR:
		rx->nr_lvl = (v * 60 + 127) / 255;
		break;
	case X6100_CIV_LVL_NB:
		rx->nb_lvl = (v * 100 + 127) / 255;
		break;
	case X6100_CIV_DNF:
		rx->dnf = v != 0;
		break;
	case X6100_CIV_LVL_DNF:
		rx->dnf_f = 100 + (v * 2900 + 127) / 255 / 10 * 10;
		break;
	case X6100_CIV_ATU:
		if (v == 2) {
			if (rx->ptt || rx->tuning || !tx_allowed(rx))
				return false;

			rx->atu_on = true;
			rx->tuning = true;
			rx->tune_since = now_ms();
			rx->tune_seen_tx = false;
			rx->warn = NULL;
		} else {
			rx->atu_on = v != 0;
		}

		break;
	default:
		return false;
	}

	cat->changed = true;

	return true;
}

static speed_t cat_speed(int baud)
{
	switch (baud) {
	case 4800:
		return B4800;
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	default:
		return B115200;
	}
}

static int cat_open(cat_t *cat, rx_t *rx, const screen_t *scr, int baud)
{
	struct termios tio;
	x6100_civ_ops_t ops = { .ctx = cat, .get = cat_get, .set = cat_set };

	memset(cat, 0, sizeof(*cat));
	cat->rx = rx;
	cat->scr = scr;
	cat->ann_mode = -1;
	cat->scope_edge = 1;
	x6100_civ_init(&cat->civ, &ops);
	x6100_civ_framer_reset(&cat->framer);

	cat->fd = open(CAT_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (cat->fd < 0)
		return -errno;

	if (tcgetattr(cat->fd, &tio) < 0) {
		int e = errno;

		close(cat->fd);
		cat->fd = -1;
		return -e;
	}

	cfmakeraw(&tio);
	cfsetispeed(&tio, cat_speed(baud));
	cfsetospeed(&tio, cat_speed(baud));
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	//
	// Apply and read back: the speed must be what the PC uses
	//
	if (tcsetattr(cat->fd, TCSANOW, &tio) < 0 || tcgetattr(cat->fd, &tio) < 0
		|| cfgetospeed(&tio) != cat_speed(baud)) {
		int e = errno ? errno : EINVAL;

		fprintf(stderr, "CAT: %s refused %d baud\n", CAT_DEV, baud);
		close(cat->fd);
		cat->fd = -1;
		return -e;
	}

	tcflush(cat->fd, TCIOFLUSH);

	return 0;
}

static void cat_write(cat_t *cat, const uint8_t *buf, size_t n)
{
	if (n > 0 && write(cat->fd, buf, n) != (ssize_t) n) {
		// non-blocking port: a full buffer drops the frame; the client retries
	}
}

// --cat-log: one line per frame, "> " from the client, "< " our reply
static void cat_log(const char *dir, const uint8_t *b, size_t n)
{
	if (n == 0)
		return;

	fprintf(stderr, "\ncat %s", dir);

	for (size_t i = 0; i < n; i++)
		fprintf(stderr, " %02X", b[i]);

	fputc('\n', stderr);
}

// Read what is pending, answer each frame; true when the radio state changed
static bool cat_poll(cat_t *cat)
{
	uint8_t buf[256], frame[X6100_CIV_FRAME_MAX], reply[X6100_CIV_FRAME_MAX];
	ssize_t n;

	cat->changed = false;

	while ((n = read(cat->fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			size_t len = x6100_civ_framer_feed(&cat->framer, buf[i], frame, sizeof(frame));

			if (len == 0)
				continue;

			size_t rn = x6100_civ_process(&cat->civ, frame, len, reply, sizeof(reply));

			// Log commands and refusals only (reply OK / NG); the steady stream of
			// polls with data replies would drown them
			if (g_cat_log && (rn == 0 || (rn == 6 && (reply[4] == 0xFB || reply[4] == 0xFA)))) {
				cat_log(">", frame, len);
				cat_log("<", reply, rn);
			}

			cat_write(cat, reply, rn);
		}
	}

	return cat->changed;
}

// Transceive: announce frequency and mode changes, whatever made them
static void cat_announce(cat_t *cat)
{
	uint8_t out[X6100_CIV_FRAME_MAX];
	const rx_t *rx = cat->rx;

	if (rx->freq != cat->ann_freq) {
		cat->ann_freq = rx->freq;
		cat_write(cat, out, x6100_civ_announce_freq(&cat->civ, rx->freq, out, sizeof(out)));
	}

	if ((int) rx->mode != cat->ann_mode || rx->fil != cat->ann_fil) {
		cat->ann_mode = rx->mode;
		cat->ann_fil = rx->fil;
		cat_write(cat, out, x6100_civ_announce_mode(&cat->civ, rx->mode, rx->fil + 1, out, sizeof(out)));
	}
}

//
// Scope data for CAT (27 00), when the client turned scope and wave output on.
// Paced by the sweep speed and by the serial port: a sweep goes out only when
// the previous one has left (about 600 bytes: ~0.3 s at 19200 baud).
//

static void cat_scope(cat_t *cat, uint64_t t)
{
	static const int pace_ms[3] = { 100, 250, 500 };	/* FAST, MID, SLOW */
	const rx_t *rx = cat->rx;
	const screen_t *scr = cat->scr;
	uint8_t wave[X6100_CIV_SCOPE_POINTS];
	static uint8_t out[1024];
	int queued = 0;

	if (!cat->scope_on || !cat->scope_out || cat->scope_hold || !scr->spec_valid)
		return;

	if (t - cat->scope_ms < (uint64_t) pace_ms[cat->scope_speed])
		return;

	if (ioctl(cat->fd, TIOCOUTQ, &queued) == 0 && queued > 0)
		return;

	view_t v = scope_view(rx, X6100_CIV_SCOPE_POINTS);

	for (int x = 0; x < X6100_CIV_SCOPE_POINTS; x++) {
		float n = spec_norm(spec_at(scr, &v, x) - cat->scope_ref / 10.0f);

		n = n < 0.0f || n != n ? 0.0f : n > 1.0f ? 1.0f : n;
		wave[x] = (uint8_t) lroundf(n * 160.0f);
	}

	x6100_civ_scope_t s = {
		.mode = civ_scope_mode(rx->scope),
		.centre = rx->freq,
		.half_span = (uint32_t) rx->span / 2,
		.lo = (uint32_t) v.lo,
		.hi = (uint32_t) (v.lo + rx->span),
		.wave = wave,
		.len = X6100_CIV_SCOPE_POINTS,
	};

	cat_write(cat, out, x6100_civ_scope_frames(&cat->civ, &s, out, sizeof(out)));
	cat->scope_ms = t;
}

//
// Display backlight on / off (PWR short press), through the first backlight in
// /sys/class/backlight: bl_power 0 = on, 4 = off (FB_BLANK_POWERDOWN)
//

static bool backlight_set(bool on)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	bool ok = false;

	if (d == NULL)
		return false;

	while (!ok && (e = readdir(d)) != NULL) {
		char path[300];
		FILE *fp;

		if (e->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power", e->d_name);

		if ((fp = fopen(path, "w")) != NULL) {
			ok = fprintf(fp, "%d\n", on ? 0 : 4) > 0;
			ok = fclose(fp) == 0 && ok;
		}
	}

	closedir(d);
	return ok;
}

//
// Backlight level in % of max_brightness (GEN: BRIGHTNESS), first backlight found
//

static bool backlight_level(int pct)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	bool ok = false;

	if (d == NULL)
		return false;

	while (!ok && (e = readdir(d)) != NULL) {
		char path[300];
		FILE *fp;
		int max = 0;

		if (e->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", e->d_name);

		if ((fp = fopen(path, "r")) == NULL)
			continue;

		if (fscanf(fp, "%d", &max) != 1)
			max = 0;

		fclose(fp);

		if (max <= 0)
			continue;

		snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", e->d_name);

		if ((fp = fopen(path, "w")) != NULL) {
			// This panel's backlight is inverted (1 = full, max_brightness = darkest) and
			// 0 switches it off: 100 % writes 1
			int v = 1 + ((100 - pct) * (max - 1) + 50) / 100;

			ok = fprintf(fp, "%d\n", v) > 0;
			ok = fclose(fp) == 0 && ok;
		}
	}

	closedir(d);
	return ok;
}

//
// One NTP sync at start with the system's busybox ntpd (-q: set once, exit).
// Skipped when the OS already runs a time daemon (it would hold port 123 and
// the one-shot fails). Servers: conf token ntp <server>, else /etc/ntp.conf,
// else pool.ntp.org (busybox ntpd has no peers without -p or ntp.conf).
// Runs detached; the result is reported when it exits (ntp_poll).
//

static pid_t g_ntp_pid = -1;

static bool proc_running(const char *const *names)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	bool found = false;

	if (d == NULL)
		return false;

	while (!found && (e = readdir(d)) != NULL) {
		char path[300], comm[32] = "";
		FILE *fp;

		if (!isdigit((unsigned char) e->d_name[0]) || atoi(e->d_name) == getpid())
			continue;

		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);

		if ((fp = fopen(path, "r")) == NULL)
			continue;

		if (fgets(comm, sizeof(comm), fp) != NULL)
			comm[strcspn(comm, "\n")] = '\0';

		fclose(fp);

		for (int i = 0; names[i] != NULL; i++)
			found |= strcmp(comm, names[i]) == 0;
	}

	closedir(d);
	return found;
}

static void ntp_start(void)
{
	static const char *const daemons[] = { "ntpd", "chronyd", "systemd-timesyn", "openntpd", NULL };
	const char *server = g_ntp_server[0] != '\0' ? g_ntp_server
		: access("/etc/ntp.conf", R_OK) == 0 ? NULL : "pool.ntp.org";
	pid_t pid;

	if (strcmp(g_ntp_server, "off") == 0) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "off");
		return;
	}

	// The IU1WXY image syncs at boot itself (S48sntp, S49ntp): leave the clock to it
	if (access("/etc/init.d/S48sntp", X_OK) == 0 || access("/etc/init.d/S49ntp", X_OK) == 0) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "by the OS at boot");
		printf("ntp: %s\n", g_ntp_msg);
		return;
	}

	if (proc_running(daemons)) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "by the OS time daemon");
		printf("ntp: the OS runs a time daemon; clock as the OS has it\n");
		return;
	}

	if ((pid = fork()) == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}

		setsid();

		if (server != NULL)
			execlp("ntpd", "ntpd", "-q", "-n", "-p", server, (char *) NULL);
		else
			execlp("ntpd", "ntpd", "-q", "-n", (char *) NULL);

		_exit(127);
	}

	if (pid > 0) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "syncing with %s", server != NULL ? server : "ntp.conf servers");
		printf("ntp: %s\n", g_ntp_msg);
	}

	g_ntp_pid = pid;
}

static void ntp_poll(void)
{
	int st;

	if (g_ntp_pid <= 0 || waitpid(g_ntp_pid, &st, WNOHANG) != g_ntp_pid)
		return;

	g_ntp_pid = -1;

	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "synced");
	else if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "no ntpd on this image");
	else
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "failed (status %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);

	printf(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "\nntp: %s\n" : "\nwarning: ntp %s; clock as the OS has it\n", g_ntp_msg);
}

int main(int argc, char **argv)
{
	x6100_i2c_t i2c;
	x6100_ctl_t ctl;
	x6100_cal_t cal;
	x6100_flow_t flow;
	x6100_flow_frame_t frame;
	int fds[IN_COUNT];
	struct pollfd pfd[IN_COUNT + 3];
	int npfd = 0;
	input_t pmap[IN_COUNT + 3];
	char cmd[128];
	size_t cmd_len = 0;
	int stdin_slot = -1;
	int cat_slot = -1;
	int apcm_slot = -1;	/* audio scope capture, fd -1 while closed */
	x6100_pcm_t apcm;
	bool apcm_open = false;
	static cat_t cat = { .fd = -1 };
	rx_t rx = { .vol = 20, .rfg = 100, .step = 100, .agc = (x6100_agc_t) -1, .mode = X6100_MODE_USB,
		.txpwr = TXPWR_DEF, .scope = SCOPE_CENTER, .span = SPAN_HZ,
		.nr_lvl = 20, .nb_lvl = 10, .nb_w = 10, .dnf_f = 1000, .dnf_w = 50 };	/* defaults: g_dsp[] */
	int rc;

	fil_load();

	for (int i = 0; i < BAND_COUNT; i++)
		rx.band_freq[i] = g_bands[i].def;

	for (int g = 0; g < FG_COUNT; g++)
		rx.agc_group[g] = g == FG_AM || g == FG_FM ? X6100_AGC_SLOW : X6100_AGC_AUTO;

	set_freq(&rx, 14074000);

	// Last state, then the user's settings; the command line below goes last
	conf_load(&rx, CONF_PATH, true);	/* first for oob / nolimit, so the state frequency fits */
	conf_load(&rx, STATE_PATH, false);
	conf_load(&rx, CONF_PATH, false);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-limit") == 0) {
			g_no_limit = true;
			continue;
		}

		if (strcmp(argv[i], "--cat") == 0 || strncmp(argv[i], "--cat=", 6) == 0) {
			g_cat_baud = argv[i][5] == '=' ? atoi(argv[i] + 6) : CAT_BAUD_DEF;

			if (g_cat_baud != 4800 && g_cat_baud != 9600 && g_cat_baud != 19200 && g_cat_baud != 38400
				&& g_cat_baud != 57600 && g_cat_baud != 115200) {
				fprintf(stderr, "%s: baud 4800, 9600, 19200, 38400, 57600 or 115200\n", argv[i]);
				return 1;
			}
			continue;
		}

		if (strcmp(argv[i], "--cat-log") == 0) {
			g_cat_log = true;
			g_cat_baud = g_cat_baud ? g_cat_baud : CAT_BAUD_DEF;
			continue;
		}

		if (strcmp(argv[i], "--probe") == 0) {
			g_probe = true;
			continue;
		}

		if (strcmp(argv[i], "--oob") == 0) {
			g_allow_oob = true;
			continue;
		}

		if (strcasecmp(argv[i], "vol") == 0 && i + 1 < argc) {
			if (!vol_parse(&rx, argv[++i]))
				return 1;

			continue;
		}

		if (!console_token(&rx, argv[i])) {
			fprintf(stderr, "usage: %s [--oob] [--no-limit] [--cat[=baud]] [--cat-log] [--probe] [freq] [usb|lsb|cw|cwr|am|nfm] [off|slow|fast|auto] [att|noatt] [pre|nopre] [vol 0-100]\n", argv[0]);
			return 1;
		}
	}

	// Safety checks off: said at every start (and OOB on the dial); the operator decides
	if (g_no_limit)
		fprintf(stderr, "warning: no-limit, tuning 10 kHz - 100 MHz\n");

	if (g_allow_oob)
		fprintf(stderr, "warning: oob, band check off: you are responsible for where you transmit\n");

	// VFO B starts as a copy of A when no state gave it
	if (rx.b_freq == 0)
		vfo_equal(&rx);

	// AGC given with a mode (state, conf, command line) belongs to the start mode's group
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

	// Audio scope capture slot, unused (fd -1) until F3 opens it
	pfd[npfd].fd = -1;
	pfd[npfd].events = POLLIN;
	pmap[npfd] = IN_COUNT;
	apcm_slot = npfd;
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

	//
	// One NTP sync with the system's own client (runs in the background)
	//
	ntp_start();

	// Brightness from the settings (GEN)
	if (g_bright > 0)
		backlight_level(g_bright);

	//
	// CAT on the USB serial port (--cat)
	//
	if (g_cat_baud > 0) {
		if ((rc = cat_open(&cat, &rx, &scr, g_cat_baud)) < 0) {
			fprintf(stderr, "warning: CAT %s: %s\n", CAT_DEV, strerror(-rc));
		} else {
			printf("CAT: CI-V on %s, %d baud, address %02X (IC-705)\n", CAT_DEV, g_cat_baud, cat.civ.addr);
			pfd[npfd].fd = cat.fd;
			pfd[npfd].events = POLLIN;
			pmap[npfd] = IN_COUNT;
			cat_slot = npfd;
			npfd++;
		}
	}

#ifdef X6100_RX_ONLY
	printf("x6100_rx (receive only): PWR or Ctrl-C to quit\n");
#else
	printf("x6100_trx: PWR or Ctrl-C to quit\n");
#endif
	show(&rx);

	uint64_t i2c_fail_since = 0;	/* first failed I2C write of a run, 0 = none */
	unsigned i2c_fails = 0;
	uint64_t last_refresh = now_ms();
	uint64_t last_meter = last_refresh;
	uint64_t last_disp = last_refresh;
	double level = 0.0;
	int level_n = 0;
	bool new_frame = false;
	bool dirty = false;
	hold_t hold = { .key = -1 };
	uint64_t pwr_since = 0;	/* PWR pressed at (ms), 0 = not held */
	const char *pwr_msg = NULL;	/* the PWR hint we put in rx.warn */
	bool lcd_on = true;
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

			if (p == apcm_slot) {
				static int16_t abuf[1024 * 2];
				ssize_t got = x6100_pcm_read(&apcm, abuf, 1024);

				if (got > 0)
					audio_feed(&scr, abuf, (unsigned) got, 2);

				continue;
			}

			if (p == cat_slot) {
				changed |= cat_poll(&cat);
				continue;
			}

			while (read(pfd[p].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
				if (pmap[p] == IN_POWER && ev.type == EV_KEY && ev.code == KEY_POWER) {
					//
					// PWR: release before 1.5 s = display on / off, release 1.5-4 s =
					// quit, held 4 s = power off (acted on below while still held)
					//
					if (ev.value == 1) {
						pwr_since = now_ms();
					} else if (ev.value == 0 && pwr_since != 0) {
						uint64_t held = now_ms() - pwr_since;

						if (held < PWR_QUIT_MS) {
							lcd_on = !lcd_on;

							if (!backlight_set(lcd_on))
								fprintf(stderr, "\nwarning: no backlight control found\n");
						} else if (held < PWR_OFF_MS) {
							g_stop = 1;
						}

						pwr_since = 0;
					}
				} else if (pmap[p] == IN_KEYPAD && ev.type == EV_KEY)
					changed |= on_keypad(&rx, &hold, (int) ev.code - X6100_KEY_BASE, ev.value);
				else if (ev.type == EV_REL)
					changed |= on_knob(&rx, pmap[p], ev.value);
			}
		}

		//
		// Audio scope capture: open while F3 shows it, closed otherwise (the codec
		// is left alone when the audio scope is not used)
		//
		if ((rx.ascope || rx.asplit) && !apcm_open) {
			int boost = rx.mode == X6100_MODE_AM || rx.mode == X6100_MODE_NFM ? 0 : 1;	/* as x6100_audio */

			if (x6100_mixer_rx_setup(X6100_PCM_CARD, boost) < 0
				|| x6100_pcm_open(&apcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, AS_RATE, 2, 1024, 4) < 0) {
				fprintf(stderr, "\naudio scope: codec capture not available\n");
				rx.ascope = rx.asplit = false;
			} else {
				apcm_open = true;
				pfd[apcm_slot].fd = apcm.fd;
			}
		} else if (!rx.ascope && !rx.asplit && apcm_open) {
			x6100_pcm_close(&apcm);
			apcm_open = false;
			pfd[apcm_slot].fd = -1;
			scr.aspec_valid = false;
		}

		//
		// PWR held: from 1.5 s the screen says what releasing will do; at 4 s the
		// radio powers off without waiting for the release
		//
		if (pwr_since != 0) {
			uint64_t held = now_ms() - pwr_since;
			const char *msg = held >= PWR_OFF_MS ? "POWER OFF" : held >= PWR_QUIT_MS ? "RELEASE: QUIT, HOLD: OFF" : NULL;

			if (msg != rx.warn && (msg != NULL || rx.warn == pwr_msg)) {
				rx.warn = msg;
				pwr_msg = msg;
				dirty = true;
			}

			if (held >= PWR_OFF_MS) {
				g_poweroff = true;
				g_stop = 1;
			}
		}

		ntp_poll();

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

			// Channel steps only where they make sense
			if ((rx.step == 9000 || rx.step == 10000) && !am_broadcast(&rx))
				rx.step = 1000;

			if (rx.step == STEP_CH && !on_60m(&rx))
				rx.step = 1000;

			scope_follow(&rx);
			apply(&ctl, &cal, &rx);
			show(&rx);
			dirty = true;

			if (cat.fd >= 0)
				cat_announce(&cat);
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


		// Reference level edit closes after 10 s without activity
		if (rx.redit && t - rx.redit_since >= 10000) {
			rx.redit = false;
			dirty = true;
		}

		// GEN panel after 10 s, status page after 30 s
		if ((rx.gedit && t - rx.gedit_since >= 10000) || (rx.ginfo && t - rx.ginfo_since >= 30000)) {
			rx.gedit = rx.ginfo = false;
			dirty = true;
		}

		// RIT / XIT panel after 10 s
		if (rx.tedit && t - rx.tedit_since >= 10000) {
			rx.tedit = false;
			dirty = true;
		}

		// DSP panel closes after 10 s without activity
		if (rx.dedit && t - rx.dedit_since >= 10000) {
			rx.dedit = false;
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

		// Every pass: RX / TX frequency follows PTT (split, RIT, XIT)
		apply(&ctl, &cal, &rx);
		apply_flags(&ctl, &rx);

		if (cat.fd >= 0)
			cat_scope(&cat, t);

		//
		// Full refresh resends register 17 too: not while the tuner is searching
		//
		if (t - last_refresh >= REFRESH_MS && !rx.tuning) {
			rc = x6100_ctl_refresh(&ctl);
			last_refresh = t;
		} else {
			rc = x6100_ctl_sync(&ctl);
		}

		//
		// A single NAK happens (seen on a band change); the registers stay pending
		// and go out on the next pass. Give up only after 2 s of failures.
		//
		if (rc < 0) {
			if (i2c_fails++ == 0)
				i2c_fail_since = t;

			if (t - i2c_fail_since >= 2000) {
				fprintf(stderr, "\ni2c: %s, %u failures in 2 s, giving up\n", strerror(-rc), i2c_fails);
				break;
			}
		} else if (i2c_fails > 0) {
			fprintf(stderr, "\ni2c: recovered after %u failed write(s)\n", i2c_fails);
			i2c_fails = 0;
		}

		while ((rc = x6100_flow_read(&flow, &frame)) > 0) {
			bool tx = (frame.flags & X6100_FLOW_TX) != 0;
			double swr = frame.vswr * 0.1;

			if (tx != scr.tx)
				dirty = true;

			scr.tx = tx;
			rx.bb_tx = tx;	/* hand mic PTT too: apply() then moves to the TX side */
			scr.pwr = frame.tx_power * 0.1;
			scr.swr = swr < 1.0 ? 1.0 : swr;
			scr.alc = frame.alc_level;
			scr.vbat = frame.vbat * 0.1;

			//
			// Overload: the flow IQ clips hard at about -6.3 dB (|IQ| ~0.48); no
			// overflow flag exists. Any sample within ~1 dB of that holds OVF 500 ms.
			//
			for (int i = 0; i < X6100_FLOW_SAMPLES; i++) {
				if (frame.iq[i][0] * frame.iq[i][0] + frame.iq[i][1] * frame.iq[i][1] >= OVF_POWER) {
					if (scr.ovf_until < t)
						dirty = true;

					scr.ovf_until = t + 500;
					break;
				}
			}

			//
			// --probe: full flag word on change, and once a second the peak IQ
			// magnitude and every flag bit seen (looking for an overflow bit)
			//
			if (g_probe) {
				static uint32_t last_flags = 0xFFFFFFFFu, flags_or;
				static float peak;
				static uint64_t probe_t;

				for (int i = 0; i < X6100_FLOW_SAMPLES; i++) {
					float m = frame.iq[i][0] * frame.iq[i][0] + frame.iq[i][1] * frame.iq[i][1];

					peak = m > peak ? m : peak;
				}

				flags_or |= frame.flags;

				if (frame.flags != last_flags) {
					printf("\nprobe: flags %08X -> %08X\n", last_flags, frame.flags);
					last_flags = frame.flags;
				}

				if (t - probe_t >= 1000) {
					printf("\nprobe: IQ peak %.1f dB  flags seen %08X\n",
						10.0 * log10(peak > 1e-20f ? peak : 1e-20f), flags_or);
					peak = 0.0f;
					flags_or = 0;
					probe_t = t;
				}
			}

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

				if (tx) {
					rx.tune_pwr = scr.pwr;
					rx.tune_swr = scr.swr;
				}

				if ((frame.flags & X6100_FLOW_ATU) && (rx.tune_seen_tx || t - rx.tune_since > 500)) {
					rx.tuning = false;
					rx.tune_done = t;
					rx.tune_ms = (uint32_t) (t - rx.tune_since);

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
				printf("iq %6.1f dB  vbat %.1fV", scr.meter_db, frame.vbat * 0.1);
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
	state_save(&rx);

	if (!lcd_on)
		backlight_set(true);	/* leave the display lit for whatever runs next */

	if (apcm_open)
		x6100_pcm_close(&apcm);

	if (g_ntp_pid > 0)
		kill(g_ntp_pid, SIGTERM);

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

	//
	// PWR held 4 s: power the radio off, as the stock UI does (filters are
	// saved and TX dropped above). The kernel's poweroff switches the PMIC off.
	//
	if (g_poweroff) {
		printf("powering off\n");
		sync();
		execlp("poweroff", "poweroff", (char *) NULL);
		perror("poweroff");
		return 1;
	}

	return 0;
}
