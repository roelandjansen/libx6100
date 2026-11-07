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
 *   x6100_trx --no-limit ...          tune 10 kHz - 100 MHz instead of freqmin - freqmax (baseband probe)
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
#include <linux/gpio.h>
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timex.h>
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

//
// Tuning range: freqmin / freqmax (conf, console, state), within FREQ_LIM_LO - FREQ_LIM_HI
// while the limits the baseband takes are found out (2026-11-04). Default 148.5 kHz, the
// LW broadcast band edge (below it MW bleeds through: 77.5 kHz tried, 2026-11-04), to
// 75 MHz (RX works up to there, 4 m included, 2026-11-04). --no-limit: 10 kHz - 100 MHz,
// to probe the baseband
//

#define FREQ_LIM_LO	5000u
#define FREQ_LIM_HI	75000000u
#define FREQ_MIN_DEF	148500u
#define FREQ_MAX_DEF	75000000u

static uint32_t g_freq_min = FREQ_MIN_DEF;
static uint32_t g_freq_max = FREQ_MAX_DEF;

// Portrait layout and its auto lock (see draw_portrait); rotation 180 puts the radio's
// left side up (the landscape canvas is rotation 270: its top is the panel's left column)
static bool g_layout_portrait;	/* conf / state layout landscape | portrait; GEN row LAYOUT */
static int g_layout_rot = 180;	/* conf portraitrot 0 | 180: 180 = the left side up */
static int g_autolock_s = 20;	/* conf autolock <s>, 0 off: portrait only, the VFO / MFK lock */
static uint64_t g_last_turn_ms;	/* last knob turn or key: the auto lock counts from it */
static bool g_lock_auto;		/* the lock in force is the auto lock: released when leaving portrait */

// Faster tuning when the VFO knob turns fast (2026-11-07; as Icom's auto tuning step):
// the dial speed (detents within the last 250 ms) picks a multiplier of the step for
// steps up to 1 kHz. A pause or a turn back starts at x1 again. GEN row VFO SPEED,
// conf / console / state vfofast off|on (default on)
static bool g_vfo_fast = true;
static uint64_t g_vfo_t[64];	/* times of the last VFO detents (ring), for the dial speed */
static unsigned g_vfo_n;	/* detents put in the ring */
static int g_vfo_dir;		/* direction of the last VFO detent */
#define VFO_FAST_WIN_MS	250	/* the speed: detents within this window */
#define VFO_FAST_X2	12	/* detents per second for x2, x5, x10 */
#define VFO_FAST_X5	25
#define VFO_FAST_X10	45
#define VFO_FAST_STEP	1000	/* larger steps are never multiplied */

// VOL knob function (VOL long, as the VOL / SQL / RFG print on the radio): 0 volume,
// 1 squelch, 2 RF gain; back to the volume VOL_FN_MS after the last turn (2026-11-05)
#define VOL_FN_MS	10000
static int g_vol_fn;
static uint64_t g_vol_fn_ms;

// Squelch (2026-11-07): x6100_trx's own, on the S-meter bar's scale, in every mode (as an
// Icom S-meter squelch). SQL 1-100 is the position on the bar (40 about S6, 60 S9, 100
// S9+60); the audio is muted while the passband level stays below it. Opens at once,
// closes after SQL_HANG_MS below, so speech is not chopped. The baseband's own threshold
// (register 21 [15:8]) did nothing in NFM and is kept at 0. A marker on the bar shows it.
#define SQL_HANG_MS	300
static bool g_sql_open = true;	/* the squelch lets the audio through */
static uint64_t g_sql_below;	/* below the threshold since (ms), 0 = not below */

//
// Baseband reset line (2026-11-05): GPIO 204 on gpiochip1, high = the STM32 in reset, as
// X6100Control (LGPL-2.1, X6100_PIN_BB_RESET; its firmware update pulses it). Only that
// line is touched: the update also pulls the CW key line (203) low, which is taken to
// start the bootloader, so 203 is left alone. Once used, the line is held low (out of
// reset) while x6100_trx runs, as X6100Control holds it, not left floating.
//

#define BB_RESET_CHIP	"/dev/gpiochip1"
#define BB_RESET_LINE	204
#define BB_RESET_MS	100		/* in reset */
#define BB_BOOT_MS	1000		/* then the baseband starts before it answers */

static int g_bb_reset_fd = -1;		/* line handle, held once requested */
static bool g_bb_reset_req;		/* console bbreset: done in the main loop */
#define CMD_FIFO	"/tmp/x6100_trx.cmd"	/* command pipe: console lines without a console */
static bool g_shot_req;			/* screenshot asked (shot, LOCK long): the main loop takes it */
static bool g_bb_dead;			/* no flow frames for BB_DEAD_MS: shown on screen */

#define BB_DEAD_MS	5000		/* flow frames come every 35 ms */

// Pulse the reset line; 0 or -errno
static int bb_reset_pulse(void)
{
	struct gpiohandle_data d = { 0 };

	if (g_bb_reset_fd < 0) {
		struct gpiohandle_request req = { 0 };
		int fd = open(BB_RESET_CHIP, O_RDWR | O_CLOEXEC);

		if (fd < 0)
			return -errno;

		req.lineoffsets[0] = BB_RESET_LINE;
		req.lines = 1;
		req.flags = GPIOHANDLE_REQUEST_OUTPUT;
		req.default_values[0] = 0;
		snprintf(req.consumer_label, sizeof(req.consumer_label), "x6100_trx bb reset");

		if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
			int e = errno;

			close(fd);
			return -e;
		}

		close(fd);
		g_bb_reset_fd = req.fd;
	}

	d.values[0] = 1;

	if (ioctl(g_bb_reset_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d) < 0)
		return -errno;

	usleep(BB_RESET_MS * 1000);
	d.values[0] = 0;

	if (ioctl(g_bb_reset_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d) < 0)
		return -errno;

	usleep(BB_BOOT_MS * 1000);
	return 0;
}

#define FREQ_MIN	(g_no_limit ? 10000u : g_freq_min)
#define FREQ_MAX	(g_no_limit ? 100000000u : g_freq_max)

#define POLL_MS		5
#define LONG_PRESS_MS	600
#define PWR_QUIT_MS	1500	/* PWR: shorter = display on / off; 1.5-4 s = quit on release */
#define PWR_OFF_MS	4000	/* PWR held this long: shut the radio down (as the stock UI) */
#define REFRESH_MS	1000
#define METER_MS	500
static const int g_tot_min[4] = { 0, 1, 3, 5 };	/* TX timeout (TX panel TOT): off, 1, 3, 5 minutes */
#define TX_TAP_MS	400
#define SWR_LIMIT	3.0
#define SWR_TRIP_MS	300
#define START_TRIES	10		/* baseband I2C at start: calibration read, first refresh */
#define START_WAIT_US	500000		/* between the tries: 5 s in all */
#define TUNE_TIMEOUT_MS	20000
#define TUNE_SHOW_MS	4000	/* TUNED result stays on screen */
#define TXPWR_DEF	50	/* 0.1 W units */
#define WARN_MS		10000	/* a TX refusal / trip note stays this long (longer while a power cut lasts) */
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

enum { SEND_NONE, SEND_MSG, SEND_SCAN };	/* rx_t.send: our TX without the PTT key */

#define BAND_MAX	12	/* entries of g_bands: 160m .. 6m, 4m */

// The VFO as it was before memory mode (V/M, CI-V 08): back on V/M or CI-V 07
typedef struct
{
	uint32_t freq;
	uint32_t b_freq;
	x6100_mode_t mode;
	x6100_mode_t b_mode;
	int fil;
	int b_fil;
	int dup;
	int dup_khz;
	bool split;
} vfo_keep_t;

typedef struct
{
	uint32_t freq;
	x6100_mode_t mode;
	x6100_agc_t agc;
	int vol;
	int rfg;
	int step;
	int step_pre_am;		/* step before the AMFM key put us in AM, 0 = none (am_mode) */
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
	uint32_t band_freq[BAND_MAX];	/* last frequency per band, index into g_bands */
	uint32_t gen_freq;	/* GEN band (outside the ham bands): last frequency there */
	x6100_mode_t gen_mode;	/* and its mode */

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
	int pbt_in;		/* twin PBT inner / outer: shift of each filter copy, Hz (0 = centre; CI-V 14 07 / 08) */
	int pbt_out;
	bool rit;
	bool xit;
	int dup;		/* repeater duplex: DUP_AUTO, DUP_OFF, DUP_MINUS, DUP_PLUS */
	int dup_khz;		/* duplex shift for DUP_MINUS / DUP_PLUS, kHz */
	bool eedit;		/* fixed edge panel (F2 long in FIX / SCROLL-F) */
	int eedit_par;		/* 0 lower, 1 upper */
	uint64_t eedit_since;
	bool tedit;		/* RIT / XIT / duplex panel (AGC long) */
	int tedit_par;		/* 0 offset, 1 RIT, 2 XIT, 3 duplex, 4 shift, 5 AM offset */
	uint64_t tedit_since;
	bool medit;		/* memory panel (V/M short) */
	int medit_ch;		/* selected channel 1-99 */
	uint64_t medit_since;
	uint64_t mclear_at;	/* MFK push asked to clear: a second push within 3 s clears */
	bool mem_mode;		/* memory mode: a channel is in use, the VFO kept in mem_vfo */
	vfo_keep_t mem_vfo;

	//
	// CW keyer (registers 33 / 34): KEY short = key type, KEY long = keyer panel
	//
	int wpm;		/* 5-50 */
	int keymode;		/* 0 straight key, 1 paddle (dot left), 2 paddle reversed; register 1 / 2 swapped */
	int iambic;		/* 0 A, 1 B */
	int cwtone;		/* sidetone / CW pitch, 300-900 Hz (IC-705 range) */
	int cwvol;		/* sidetone volume 0-100 %, register 0-63 */
	int qsk;		/* break-in delay, ms */
	int ratio;		/* dash / dot ratio x10 */
	bool kedit;
	int kedit_par;
	uint64_t kedit_since;

	//
	// TX audio (registers 20-22, 25, 29): F4 long = TX panel
	//
	int micsel;		/* 0 internal, 1 hand mic, 2 auto */
	int imicg;		/* internal mic gain */
	int hmicg;		/* hand mic gain */
	int line_in;		/* LINE IN level (register 20 [7:0]: codec audio into the baseband TX) */
	int line_out;		/* LINE OUT level (register 20 [15:8]) */
	int cmp;		/* compressor on / off */
	int cmp_lvl;		/* 1 = 1:2, 2 = 1:4, 3 = 1:8 */
	int vox;		/* VOX on / off */
	int voxg;		/* VOX gain */
	int voxag;		/* anti-VOX */
	int voxdly;		/* VOX delay, ms */
	int moni;		/* TX monitor on / off */
	int moni_lvl;		/* monitor level */
	int sql;		/* squelch threshold, 0 = open */
	int tot;		/* TX timeout, index into g_tot_min (0 = off) */
	bool tot_cut;		/* power cut: minimum power and TX enable off (TOT on hand mic / VOX, stuck TX) */
	bool tot_lock;		/* TOT on hand mic / VOX: the cut stays until a key (the mic PTT is unseen) */
	uint64_t bb_tx_since;	/* baseband in TX since (ms), 0 = in RX */
	//
	// Sending without the PTT key: a message (MSG) or the SWR scan (APP)
	//
	int send;		/* SEND_NONE, SEND_MSG, SEND_SCAN */
	uint64_t send_since;
	bool codec_tx;		/* keyed with the codec as TX audio (register 12 0x4000) */
	uint32_t tx_hz_ovr;	/* 0, else the TX frequency (CW message: tone below the carrier; scan step) */
	int tx_mode_ovr;	/* -1, else the TX mode (CW message: USB; SWR scan: NFM) */
	int tx_pwr_ovr;		/* -1, else the TX power in 0.1 W (SWR scan) */
	bool msedit;		/* message panel (MSG) */
	int msedit_par;
	uint64_t msedit_since;
	bool apedit;		/* app panel (APP) */
	int apedit_par;
	uint64_t apedit_since;
	bool sedit;		/* scope panel (F1 long) */
	int sedit_par;
	uint64_t sedit_since;
	bool xedit;
	int xedit_par;
	uint64_t xedit_since;

	//
	// TX
	//
	int txpwr;		/* 0.1 W units */
	bool mfk_pwr;		/* MFK adjusts TX power instead of RF gain */
	bool ptt;		/* PTT requested (key held or latched) */
	bool ptt_cat;		/* that PTT came from CAT (wfview, wfserver, hamlib): DATA (OFF) MOD LAN sends the codec audio */
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
	x6100_atu_t atu_last;	/* network of the last tune (tune panel); memories: g_atum */
	bool atu_last_valid;
	const char *warn;	/* last TX refusal / trip reason, on the scope for WARN_MS or until the next TX */

	//
	// Spectrum scope
	//
	scope_mode_t scope;
	int64_t fix_lo;		/* left edge of the fixed window, Hz */
	int fix_w;		/* its width: the edge in use (FIX / SCROLL-F) */
	int edge;		/* fixed edge 1-4 (IC-705 EDGE) */
	int fix_range;		/* edge range and edge the window was last set from */
	int fix_edge;
	int span;		/* CENTER / SCROLL-C window width, Hz */
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
	int fedit_par;		/* 0 BW, 1 SFT, 2 PBT */
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
	{ "4m",   70000000, 70500000, 70200000 },	/* 2026-11-04: RX works up to 75 MHz; TX: the band plan (region) decides */
};

#define BAND_COUNT	((int) (sizeof(g_bands) / sizeof(g_bands[0])))
_Static_assert(sizeof(g_bands) / sizeof(g_bands[0]) == BAND_MAX, "band_freq[] in rx_t holds BAND_MAX bands");

//
// Transmit allocations per region (Hz). The TX check and the OOB marker use the
// selected region (conf token region); g_bands above stays the band memory and
// BAND key range. Sources: ITU RR / IARU band plans; NL: Regeling gebruik van
// frequentieruimte zonder vergunning en met meldingsplicht (stcrt 2015-3772,
// 2021-31799) for F registration, 60 m per WRC-15, 4 m 70.0-70.5 MHz. TX above 54 MHz
// is past the X6100's low-pass filters: check the output on a spectrum analyser before
// going on air there.
//

typedef struct
{
	uint32_t low;
	uint32_t high;
} seg_t;

#define SEG_MAX		24	/* TX segments per region (uk: 11 on 60 m alone) */

typedef struct
{
	const char *name;
	seg_t seg[SEG_MAX];
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
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 52000000 },
		{ 70000000, 70500000 } } },	/* 4 m (2026-11-04); no ITU allocation, so not in r1-r3 */
	{ "uk", {	/* 60 m: the 11 UK segments (Ofcom, Full licence; 2026-11-07), not WRC-15 */
		{ 1810000, 2000000 }, { 3500000, 3800000 },
		{ 5258500, 5264000 }, { 5276000, 5284000 }, { 5288500, 5292000 }, { 5298000, 5307000 },
		{ 5313000, 5323000 }, { 5333000, 5338000 }, { 5354000, 5358000 }, { 5362000, 5374500 },
		{ 5378000, 5382000 }, { 5395000, 5401500 }, { 5403500, 5406500 }, { 7000000, 7200000 },
		{ 10100000, 10150000 }, { 14000000, 14350000 }, { 18068000, 18168000 }, { 21000000, 21450000 },
		{ 24890000, 24990000 }, { 28000000, 29700000 }, { 50000000, 52000000 },
		{ 70000000, 70500000 } } },	/* 4 m */
};

#define REGION_COUNT	((int) (sizeof(g_regions) / sizeof(g_regions[0])))

static int g_region;	/* index into g_regions; conf token region, default r1 */

// Inside a transmit allocation of the selected region
static bool in_allocation(uint32_t hz)
{
	const region_t *r = &g_regions[g_region];

	for (int i = 0; i < SEG_MAX && r->seg[i].high != 0; i++) {
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
static const char *g_cmd_view;	/* console line being typed, shown at the end of the status line */
static double g_iq_db, g_vbat;	/* receive level and battery for the status line (0: not yet) */
static bool g_tx_show;		/* status line shows TX readings instead of the RX level */
static double g_tx_pwr, g_tx_swr;
static int g_tx_alc;
static int g_batcap;		/* battery %, and external supply present, for the status line */
static bool g_ext;
static int g_cat_baud;		/* CI-V on the DEV port: baud, 0 = off; conf token cat, or --cat */
static bool g_cat_pty = true;	/* CI-V on the pty (CAT_PTY_LINK) as well: conf token catpty */

//
// Two CI-V endpoints at once, each with its own engine state (scope, transceive): the
// DEV port for a PC on the USB cable, the pty for a client on the radio (wfserver).
// Before 2026-10-31 it was one or the other, so wfserver needed CAT on PTY.
//

#define CAT_PTY		(-1)		/* cat_open: the pty instead of the DEV port */

enum { CAT_EP_DEV, CAT_EP_PTY, CAT_N };

static bool g_i2c_down;		/* 2 s of failed I2C writes: TX dropped and refused, still trying */
static int g_civ_addr = X6100_CIV_ADDR_IC705;	/* conf token civaddr: a4 (IC-705) or 61 (X6100 rig file) */
static bool g_cat_mem_rw;	/* CAT may write / clear memories (09, 0B, 1A 00): conf token catmem rw; default ro */
enum { MODIN_MIC, MODIN_USB, MODIN_LAN };	/* LAN: CAT PTT the codec (network audio), the PTT key / hand mic the mic */
static int g_modin[2] = { MODIN_MIC, MODIN_USB };	/* TX audio input, DATA OFF MOD / DATA MOD (1A 05 0118 / 0119) */
static const char *const g_modin_word[3] = { "mic", "usb", "lan" };	/* console / state; CI-V 0 / 1 / 3 (IC-705 WLAN) */
static char g_ntp_server[64];	/* conf token ntp <server> | off; empty = ntp.conf or pool.ntp.org */
static int g_bright;		/* backlight level, % (10-100); 0 = leave as the OS set it */
static unsigned g_conf_set;	/* GEN rows that x6100_trx.conf fixes (bit = row): shown CONF, locked */
static bool g_conf_reading;	/* conf_load of CONF_PATH in progress */
static bool g_conf_loading;	/* any conf_load in progress (mem lines do not save then) */
static char g_ntp_msg[64] = "not run";	/* last NTP result, for the status page */
static char g_toast[24];		/* short confirmation over the scope (VFO A / B, A=B) */
static uint64_t g_toast_until;	/* ms; shown while now_ms() is before this */

//
// Where the clock comes from: unknown until an NTP sync succeeds, the OS syncs it (the
// IU1WXY image at boot, a time daemon) or it is set by hand (console time). The clock
// under the FIL label is amber while unknown, or when it reads before TIME_MIN (a
// clock never set: no network in the field). FT8 and logging need it right.
//

enum { TS_UNKNOWN, TS_NTP, TS_OS, TS_HAND, TS_CAT };

#define TIME_MIN	1767225600	/* 2026-01-01T00:00:00Z */

static int g_time_src = TS_UNKNOWN;

static bool time_ok(void)
{
	return g_time_src != TS_UNKNOWN && time(NULL) >= TIME_MIN;
}

// Console time <UTC>: 2026-10-31T10:15:00Z (the Z may be left out) or 10:15[:00]
// today; sets the system clock (x6100_trx runs as root). False when it does not parse
static bool time_set(const char *v)
{
	time_t now = time(NULL);
	struct tm tm;
	struct timespec ts = { 0 };
	int y, mo, d, h, mi, s = 0, n;

	if (v == NULL || gmtime_r(&now, &tm) == NULL)
		return false;

	if ((n = sscanf(v, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s)) >= 5) {
		tm.tm_year = y - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = d;
	} else if ((n = sscanf(v, "%2d:%2d:%2d", &h, &mi, &s)) < 2) {
		return false;
	}

	if (tm.tm_year < 126 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31
		|| h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
		return false;

	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	ts.tv_sec = timegm(&tm);

	if (ts.tv_sec < TIME_MIN || clock_settime(CLOCK_REALTIME, &ts) != 0)
		return false;

	g_time_src = TS_HAND;
	snprintf(g_ntp_msg, sizeof(g_ntp_msg), "set by hand");
	return true;
}

//
// The clock from CAT (1A 05 0165 date, 0166 time, 0170 UTC offset: wfview Set Clock or
// set radio time on connect). The client sends local time with its UTC offset; the
// system clock stays UTC. cattime off: refused; unsynced (default): only while no NTP
// keeps the clock (unknown, set by hand or by CAT, the OS without a kernel sync);
// on: always. A refused set answers NG.
//

enum { CT_OFF, CT_UNSYNCED, CT_ON };

static int g_cat_time = CT_UNSYNCED;	/* conf token cattime off | unsynced | on */
static int g_cat_utc_ofs;		/* minutes, local = UTC + offset; the last one a client sent */

// The clock kept by NTP: our own sync, or the OS's with the kernel flag synced
static bool clock_synced(void)
{
	struct timex tx = { 0 };
	int st;

	if (time(NULL) < TIME_MIN)
		return false;

	if (g_time_src == TS_NTP)
		return true;

	if (g_time_src != TS_OS)
		return false;

	st = adjtimex(&tx);
	return st >= 0 && st != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

static bool cat_time_allowed(void)
{
	return g_cat_time == CT_ON || (g_cat_time == CT_UNSYNCED && !clock_synced());
}

// Local time now as broken-down fields (UTC + the client's offset)
static void cat_time_local(struct tm *tm)
{
	time_t t = time(NULL) + (time_t) g_cat_utc_ofs * 60;

	gmtime_r(&t, tm);
}

// Set the local date (YYYYMMDD, time of day kept) or time (HHMM, second 00, date kept)
static bool cat_time_set(bool date, int32_t v)
{
	struct timespec now, ts = { 0 };
	struct tm tm;

	if (!cat_time_allowed() || clock_gettime(CLOCK_REALTIME, &now) != 0)
		return false;

	cat_time_local(&tm);

	if (date) {
		tm.tm_year = (int) (v / 10000) - 1900;
		tm.tm_mon = (int) (v / 100 % 100) - 1;
		tm.tm_mday = (int) (v % 100);
		ts.tv_nsec = now.tv_nsec;
	} else {
		tm.tm_hour = (int) (v / 100);
		tm.tm_min = (int) (v % 100);
		tm.tm_sec = 0;
	}

	ts.tv_sec = timegm(&tm) - (time_t) g_cat_utc_ofs * 60;

	// A time before TIME_MIN is set too: wfview sends the time before the date, so on a
	// clock never set the time of day lands first and the date then puts it right
	if ((date && ts.tv_sec < TIME_MIN) || clock_settime(CLOCK_REALTIME, &ts) != 0)
		return false;

	if (ts.tv_sec >= TIME_MIN) {
		g_time_src = TS_CAT;
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "set by CAT");
	}

	return true;
}

enum { GEN_BRIGHT, GEN_LAYOUT, GEN_VFOFAST, GEN_CAT, GEN_CIVADDR, GEN_CATMEM, GEN_CATTIME, GEN_MODOFF, GEN_MODDATA, GEN_WFS, GEN_NTP, GEN_WIFI, GEN_WNET, GEN_BT, GEN_BTPAIR,
	GEN_BTDEV, GEN_BSCAN, GEN_BMARK, GEN_BNOTCH, GEN_ROWS };	/* GEN rows */

static void gen_adjust(int row, int dir, bool def);
static uint64_t g_gen_save_at;	/* GEN setting changed: the state goes to disk at this time (ms), 0 none */
#define GEN_SAVE_MS	2000		/* after the last GEN change: one write for a run of MFK steps */

// SIGHUP: the ssh session (the console) went away, e.g. with the WiFi; x6100_trx goes on
static volatile sig_atomic_t g_hup;

static void on_hup(int sig)
{
	(void) sig;
	g_hup = 1;
}

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

// What the CAT address makes us: the IC-705 (wfview IC-705 rig) or ourselves (X6100.rig)
static const char *civ_ident(void)
{
	return g_civ_addr == X6100_CIV_ADDR_IC705 ? "IC-705" : g_civ_addr == X6100_CIV_ADDR_X6100 ? "X6100" : "CI-V";
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
// The receive filter with twin PBT, Icom style: each PBT shifts its own copy of the
// filter and the passband is their overlap, so both the same way shift it and opposite
// ways narrow it. AM: the baseband needs a symmetric filter, so the shift is a receive
// offset and the filter narrows symmetrically (am_pass). Not in FM (Icom has none there)
//

#define PBT_MAX_HZ	1250		/* +- Hz over the CI-V range 0000-0255 (hamlib's Icom range) */
#define PBT_MIN_WIDTH	50		/* narrowest passband left, Hz */
#define PBT_STEP_HZ	50		/* filter panel PBT box: Hz per MFK step */

// a / b rounded to the nearest, halves away from zero (b > 0): the PBT level <-> Hz
// conversion then reads back every CI-V value it was set to
static int div_round(int a, int b)
{
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

static void am_pass(const rx_t *rx, int *centre, int *half);

// PBT shifts set and the mode one where they act (not FM): the FIL label shows PBT
static bool pbt_active(const rx_t *rx)
{
	return mode_group(rx->mode) != FG_FM && (rx->pbt_in != 0 || rx->pbt_out != 0);
}

static void rx_filter(const rx_t *rx, uint32_t *low, uint32_t *high)
{
	int lo, hi;

	mode_filter(rx->mode, rx->fil, low, high);

	if (!pbt_active(rx))
		return;

	// AM: shown as Icom's half-width, narrowed by |in - out| / 2; the baseband filter
	// and the receive offset are am_pass
	if (mode_group(rx->mode) == FG_AM) {
		int d = rx->pbt_in > rx->pbt_out ? rx->pbt_in - rx->pbt_out : rx->pbt_out - rx->pbt_in;

		hi = (int) *high - d / 2;
		*high = (uint32_t) (hi < PBT_MIN_WIDTH ? PBT_MIN_WIDTH : hi);
		return;
	}

	lo = (int) *low + (rx->pbt_in > rx->pbt_out ? rx->pbt_in : rx->pbt_out);
	hi = (int) *high + (rx->pbt_in < rx->pbt_out ? rx->pbt_in : rx->pbt_out);
	lo = lo < 0 ? 0 : lo;
	hi = hi < lo + PBT_MIN_WIDTH ? lo + PBT_MIN_WIDTH : hi;
	*low = (uint32_t) lo;
	*high = (uint32_t) hi;
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
// Repeater duplex: TX = RX - / + the shift. AUTO (the default) gives -100 kHz in
// NFM on the 10 m repeater outputs (29.620-29.680 MHz, IARU and US band plans),
// simplex elsewhere; OFF is always simplex. Split takes precedence: its TX VFO is
// the TX frequency as set. No CTCSS: the baseband has no tone encoder that is
// known, and the microphone audio does not pass through Linux.
//

enum { DUP_AUTO, DUP_OFF, DUP_MINUS, DUP_PLUS, DUP_COUNT };

#define DUP_KHZ_DEF	100
#define DUP_KHZ_MAX	10000
#define RPT10_LO	29615000u	/* 10 m repeater outputs, with a few kHz off-tune */
#define RPT10_HI	29685000u
#define RPT10_SHIFT	(-100000)

static const char *g_dup_tok[DUP_COUNT] = { "dupauto", "dupoff", "dup-", "dup+" };	/* console / state */

static int dup_shift(const rx_t *rx)
{
	if (rx->split)
		return 0;

	switch (rx->dup) {
	case DUP_MINUS:
		return -rx->dup_khz * 1000;
	case DUP_PLUS:
		return rx->dup_khz * 1000;
	case DUP_AUTO:
		return rx->mode == X6100_MODE_NFM && rx->freq >= RPT10_LO && rx->freq <= RPT10_HI ? RPT10_SHIFT : 0;
	default:
		return 0;
	}
}

//
// AM receive offset: the baseband is zero IF and notches DC, which in AM takes part
// of the carrier when tuned exactly on it (distorted audio). In AM the receiver sits
// g_am_ofs Hz above the dial, the filter set by am_pass, the dial and TX stay on the
// carrier. 20 Hz already helps, 500 Hz sounds clean (2026-10-26, MW), but a weak
// station then carries a tone at the offset (the leftover at the receive frequency
// beating with the carrier, 2026-10-29); 100 Hz is the compromise, 50 Hz still usable.
// Not during a birdie scan (it needs the IQ centre on the dial).
//

#define AM_OFS_DEF	100
#define AM_OFS_MAX	1000
#define TPARS		9	/* rows of the RIT / XIT / duplex panel (AGC long): ..., AM OFFSET, AGC SLOW / FAST / HANG */

static int g_am_ofs = AM_OFS_DEF;	/* conf / console token am_offset, 0 = off; AGC long panel */

//
// AGC time constants (register 28, units of 10 ms) and hang (register 27 bit 12), as the
// stock UI 1.2.0 has them (RADIO SETTING1: AGC SLOW 250, AGC FAST 5, AGC HANG OFF, the
// values x6100_ctl_init writes). The middle one stays at 50: AGC AUTO does not follow it
// (nor FAST): AUTO picks its time by mode in the baseband, as tried 2026-11-04 (MID 5 / 300
// and FAST 100 in AUTO: no difference; AUTO sounds as FAST in SSB).
// AGC long panel rows; state / conf tokens agcslow, agcfast, agchang.
//

#define AGC_SLOW_DEF	250
#define AGC_SLOW_MIN	20
#define AGC_SLOW_MAX	1000
#define AGC_FAST_DEF	5
#define AGC_FAST_MIN	1
#define AGC_FAST_MAX	100
#define AGC_MID		50

static int g_agc_slow = AGC_SLOW_DEF;
static int g_agc_fast = AGC_FAST_DEF;
static int g_agc_hang;

//
// IF shift (register 35, BASE 1.1.9; as OpenX6X00 and X6100Control use it): [23:0]
// the shift in Hz, [24] set = off (centre mode). The VFO register gets the receive
// frequency minus the shift, the baseband receives that much above its LO, so the
// DC / LO spike of the zero-IF receiver sits g_ifshift Hz away from the passband
// instead of in it. In AM that spike beat with the carrier (a tone off the carrier,
// 2026-10-30, gone with the shift); the AM offset is then not needed and stays 0. In
// SSB / CW it keeps the spike off the receive line. All modes since 2026-10-30, not
// during a birdie scan (it needs the IQ centre on the dial). While keyed the shift is
// off (TX on the real frequency) unless g_ifshift_tx: whether the baseband applies the
// shift in TX too (OpenX6X00 keeps it) is to be checked; if so it can stay on, and the
// CW keyer / hand mic / VOX no longer cause a retune at every key down.
//

#define IFSHIFT_DEF	10000
#define IFSHIFT_MAX	40000

static int g_ifshift = IFSHIFT_DEF;	/* conf / console token ifshift, Hz, 0 = off */
static int g_ifshift_tx = 1;	/* conf / console token ifshift_tx 0 / 1: keep the shift while keyed */

static bool bscan_running(void);

static int lo_shift(const rx_t *rx)
{
	// CW: the baseband keys every element by itself (QSK); without ifshift_tx that
	// would retune at every key down, so CW waits until the TX test is done
	if (!g_ifshift_tx && (rx->mode == X6100_MODE_CW || rx->mode == X6100_MODE_CWR))
		return 0;

	return !bscan_running() ? g_ifshift : 0;
}

static int am_ofs(const rx_t *rx)
{
	return rx->mode == X6100_MODE_AM && !bscan_running() && g_ifshift == 0 ? g_am_ofs : 0;
}

//
// AM passband, Icom style, with what the baseband offers (a filter symmetric around the
// receive frequency): an IF passband [L, H] around the carrier is the receiver tuned
// (L + H) / 2 off with a half-width of (H - L) / 2. Icom's twin PBT gives L = -hi +
// max(in, out), H = hi + min(in, out). The AM offset moves the receiver up; the upper
// edge stays at H (an adjacent station above is kept out, 2026-10-29: 4.5 kHz FIL with
// the filter opened by the offset on both sides let a carrier 5 kHz up whistle) and the
// lower edge comes 2 x offset inside L: the top of the audio comes from one sideband
// only, 6 dB down. The carrier stays at least 100 Hz inside the passband.
//

static void am_pass(const rx_t *rx, int *centre, int *half)
{
	uint32_t lo, hi;
	int ofs = am_ofs(rx), mx = 0, mn = 0, l, h;

	mode_filter(rx->mode, rx->fil, &lo, &hi);

	if (rx->mode != X6100_MODE_AM || bscan_running()) {
		*centre = 0;
		*half = (int) hi;
		return;
	}

	if (pbt_active(rx)) {
		mx = rx->pbt_in > rx->pbt_out ? rx->pbt_in : rx->pbt_out;
		mn = rx->pbt_in < rx->pbt_out ? rx->pbt_in : rx->pbt_out;
	}

	l = -(int) hi + mx + 2 * ofs;
	h = (int) hi + mn;
	l = l > -100 ? -100 : l;
	h = h < 100 ? 100 : h;
	*centre = (l + h) / 2;
	*half = (h - l) / 2;
}

//
// RX and TX frequency: RX = the VFO in use + RIT (+ the AM offset); TX = the other
// VFO with split, else the VFO in use + the duplex shift, + XIT (one offset for both,
// as on Icom radios)
//

static uint32_t rx_hz(const rx_t *rx)
{
	int centre, half;

	am_pass(rx, &centre, &half);	/* AM: the offset and PBT shift are a receive offset */

	return (uint32_t) ((int64_t) rx->freq + (rx->rit ? rx->rit_hz : 0) + centre);
}

// TX frequency while keyed (apply), else 0: the TX IQ is centred on it, the shift
// applied after it (2026-10-30: with the shift kept the scope showed TX 10 kHz low)
static uint32_t g_keyed_hz;

//
// The LO for receiving. Normally the receive frequency less the IF shift. In FIX /
// SCROLL-F the LO is put where the IQ (LO +- 48 kHz) covers the whole window, and the
// shift (register 35) makes up the difference to the receive frequency: at least the
// IF shift (the DC spike out of the passband), at most IFSHIFT_DYN_MAX, either way (a
// negative shift: LO above, for a VFO left in the window). Of the LOs allowed the one
// nearest the window centre: the window covered when that can be, else as much of it
// as can (a 96 kHz window is covered only with the LO right in its middle).
// 2026-10-31: SCROLL-F on 6.920-6.976 with the VFO at 6.933 left the right part black.
//

#define IQ_HALF_HZ	50000	/* half the IQ span (SPAN_HZ) */
#define IFSHIFT_DYN_MAX	40000	/* largest shift used (OpenX6X00's range) */

static bool scope_fixed(const rx_t *rx);

static int64_t rx_lo(const rx_t *rx)
{
	int64_t f = rx_hz(rx), s = lo_shift(rx), c, p, n;

	if (s <= 0 || !scope_fixed(rx) || rx->fix_w <= 0)
		return f - s >= 0 || s <= 0 ? f - s : f + s;	/* below the shift (freqmin): the LO above */

	c = rx->fix_lo + rx->fix_w / 2;

	// Nearest the centre with a positive shift (LO in [f - max, f - s]) and with a
	// negative one (LO in [f + s, f + max]); the positive one on a tie (verified).
	// Never an LO below 0 Hz (a receive frequency of a few kHz: freqmin)
	p = c < f - IFSHIFT_DYN_MAX ? f - IFSHIFT_DYN_MAX : c > f - s ? f - s : c;
	n = c < f + s ? f + s : c > f + IFSHIFT_DYN_MAX ? f + IFSHIFT_DYN_MAX : c;

	if (p < 0)
		return n;

	return llabs(p - c) <= llabs(n - c) ? p : n;
}

// Centre of the IQ (the baseband LO)
static uint32_t iq_hz(const rx_t *rx)
{
	if (g_keyed_hz != 0)
		return g_keyed_hz;

	return (uint32_t) rx_lo(rx);
}

// What passband() is relative to: the carrier in AM (rx_hz less the AM receive
// offset and PBT shift), else the receive frequency
static uint32_t pb_ref(const rx_t *rx)
{
	int centre, half;

	am_pass(rx, &centre, &half);
	return (uint32_t) ((int64_t) rx_hz(rx) - centre);
}

static uint32_t tx_hz(const rx_t *rx)
{
	return (uint32_t) ((int64_t) (rx->split ? rx->b_freq : rx->freq) + dup_shift(rx) + (rx->xit ? rx->rit_hz : 0));
}

// RX and TX apart: both get a line on the scope and a note
static bool tx_apart(const rx_t *rx)
{
	return rx->split || rx->rit || rx->xit || dup_shift(rx) != 0;
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

//
// Birdies: carriers from the radio's own oscillators, fixed lines on the scope and a
// whistle in SSB. A receive-only scan (antenna off or a dummy load; GEN row BIRDIE
// SCAN or the console) finds them, BIRDIE_PATH keeps them as birdie <Hz> <dB over
// the noise>. With BIRDIE MARKS a tick on the scope marks each one; with BIRDIE
// NOTCH the baseband notch (register 24, one notch) sits on the strongest birdie in
// the passband, in USB, LSB and AM, unless the notch is on by hand (DFL). Own design;
// the idea of a scan and an automatic notch comes from the IU1WXY release notes.
//

#define BIRDIE_PATH	"/etc/x6100/birdies.conf"
#define BIRDIE_MAX	1024	/* 2026-11-06: 256 filled up on 6 m, 6 m from 52.7 MHz and 4 m were lost */
#define BIRDIE_NOTCH_W	40		/* notch width, Hz */

typedef struct
{
	uint32_t hz;
	int snr;			/* dB over the noise floor */
} birdie_t;

static birdie_t g_birdie[BIRDIE_MAX];
static int g_nbirdie;
static int g_birdie_full;		/* birdies that found the list full in this scan */
static bool g_bmarks = true;		/* ticks on the scope */
static bool g_bnotch;			/* automatic notch; off until checked on the radio */
static int g_bscan_req;			/* GEN / console: 1 start a scan, -1 stop it (main loop) */
static bool g_bscan_quiet;		/* console birdiescan ... quiet: WiFi and Bluetooth off meanwhile */

static void passband(const rx_t *rx, int *lo, int *hi);

// Add a birdie; one within 150 Hz of it is the same one: the stronger reading stays
static void birdie_add(uint32_t hz, int snr)
{
	for (int i = 0; i < g_nbirdie; i++) {
		uint32_t d = g_birdie[i].hz > hz ? g_birdie[i].hz - hz : hz - g_birdie[i].hz;

		if (d <= 150) {
			if (snr > g_birdie[i].snr) {
				g_birdie[i].hz = hz;
				g_birdie[i].snr = snr;
			}

			return;
		}
	}

	if (g_nbirdie < BIRDIE_MAX) {
		g_birdie[g_nbirdie].hz = hz;
		g_birdie[g_nbirdie].snr = snr;
		g_nbirdie++;
		return;
	}

	// Full: the weakest goes when this one is stronger, and it is said, not dropped
	// unseen as before
	int w = 0;

	for (int i = 1; i < g_nbirdie; i++)
		w = g_birdie[i].snr < g_birdie[w].snr ? i : w;

	if (snr > g_birdie[w].snr) {
		g_birdie[w].hz = hz;
		g_birdie[w].snr = snr;
	}

	g_birdie_full++;
}

// Drop the birdies in low .. high (a band about to be scanned again)
static void birdie_clear(uint32_t low, uint32_t high)
{
	int n = 0;

	for (int i = 0; i < g_nbirdie; i++)
		if (g_birdie[i].hz < low || g_birdie[i].hz > high)
			g_birdie[n++] = g_birdie[i];

	g_nbirdie = n;
}

static int birdie_cmp(const void *a, const void *b)
{
	const birdie_t *x = a, *y = b;

	return x->hz < y->hz ? -1 : x->hz > y->hz;
}

static void birdie_save(void)
{
	char tmp[] = BIRDIE_PATH ".tmp";
	FILE *fp;

	qsort(g_birdie, (size_t) g_nbirdie, sizeof(g_birdie[0]), birdie_cmp);

	if ((fp = fopen(tmp, "w")) == NULL) {
		fprintf(stderr, "\nbirdies: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}

	fprintf(fp, "# x6100_trx birdies (receive-only scan): birdie <Hz> <dB over the noise>\n");

	for (int i = 0; i < g_nbirdie; i++)
		fprintf(fp, "birdie %u %d\n", (unsigned) g_birdie[i].hz, g_birdie[i].snr);

	if (fclose(fp) != 0 || rename(tmp, BIRDIE_PATH) != 0)
		fprintf(stderr, "\nbirdies: cannot save %s: %s\n", BIRDIE_PATH, strerror(errno));
}

// Notch frequency in the audio for the strongest birdie in the passband, 0 for none
static int birdie_notch(const rx_t *rx)
{
	int lo, hi, best = 0, best_snr = -1;

	if (!g_bnotch || rx->dnf
		|| (rx->mode != X6100_MODE_USB && rx->mode != X6100_MODE_LSB && rx->mode != X6100_MODE_AM))
		return 0;

	passband(rx, &lo, &hi);

	for (int i = 0; i < g_nbirdie; i++) {
		int64_t rel = (int64_t) g_birdie[i].hz - (int64_t) (rx->mode == X6100_MODE_AM ? rx->freq : rx_hz(rx));	/* AM: the beat against the carrier */
		int audio = (int) (rel < 0 ? -rel : rel);

		if (rel >= lo && rel <= hi && audio >= 100 && audio <= 3000 && g_birdie[i].snr > best_snr) {
			best = audio;
			best_snr = g_birdie[i].snr;
		}
	}

	return best;
}

//
// Tuner memories: every tune stores its network (L, C, S) with the frequency it was
// tuned on; the network for the TX frequency is the nearest memory within 0.5 % of
// it (9 kHz at 1.8 MHz, 35 kHz at 7 MHz, 140 kHz at 28 MHz), else straight through.
// A tune near an existing memory (within half that) replaces it. Kept in the state
// file as atumem <Hz> <L> <C> <S>.
//

#define ATU_MEMS	200

typedef struct
{
	uint32_t hz;
	x6100_atu_t net;
} atum_t;

static atum_t g_atum[ATU_MEMS];
static int g_natum;

static const atum_t *atu_find(uint32_t hz)
{
	const atum_t *best = NULL;
	uint32_t best_d = hz / 200 + 1;

	for (int i = 0; i < g_natum; i++) {
		uint32_t d = g_atum[i].hz > hz ? g_atum[i].hz - hz : hz - g_atum[i].hz;

		if (d < best_d) {
			best = &g_atum[i];
			best_d = d;
		}
	}

	return best;
}

static void atu_store(uint32_t hz, const x6100_atu_t *net)
{
	int i;

	for (i = 0; i < g_natum; i++) {
		uint32_t d = g_atum[i].hz > hz ? g_atum[i].hz - hz : hz - g_atum[i].hz;

		if (d <= hz / 400)
			break;
	}

	if (i == g_natum) {
		if (g_natum == ATU_MEMS) {	/* full: the oldest goes */
			memmove(&g_atum[0], &g_atum[1], sizeof(g_atum[0]) * (ATU_MEMS - 1));
			i = ATU_MEMS - 1;
		} else {
			g_natum++;
		}
	}

	g_atum[i].hz = hz;
	g_atum[i].net = *net;
}

static void apply(x6100_ctl_t *ctl, const x6100_cal_t *cal, const rx_t *rx)
{
	uint32_t low, high;
	bool keyed = rx->ptt || rx->tuning || rx->bb_tx || rx->send != SEND_NONE
		|| (ctl->sent[X6100_REG_FLAGS] & (X6100_FLAG_IPTT | X6100_FLAG_ATU_TUNE | X6100_FLAG_CODEC_TX | X6100_FLAG_SWRSCAN));
	uint32_t hz = keyed ? (rx->tx_hz_ovr != 0 ? rx->tx_hz_ovr : tx_hz(rx)) : rx_hz(rx);
	x6100_mode_t mode = keyed ? (rx->tx_mode_ovr >= 0 ? (x6100_mode_t) rx->tx_mode_ovr : tx_mode(rx)) : rx->mode;

	g_keyed_hz = keyed ? hz : 0;

	if (keyed) {
		mode_filter(mode, rx->split ? rx->b_fil : rx->fil, &low, &high);
	} else if (rx->mode == X6100_MODE_AM) {
		int centre, half;

		am_pass(rx, &centre, &half);	/* AM offset and twin PBT, the upper edge kept */
		low = 0;
		high = (uint32_t) half;
	} else {
		rx_filter(rx, &low, &high);	/* with twin PBT */
	}

	// IF shift: the LO below the receive frequency, the spike out of the passband
	{
		// RX: the LO rx_lo picks (FIX / SCROLL-F: the window covered); TX: the plain
		// shift, verified (a key-down in FIX / SCROLL-F moves the LO back to it)
		int shift = keyed ? (g_ifshift_tx ? lo_shift(rx) : 0) : (int) ((int64_t) hz - rx_lo(rx));

		x6100_set_freq(ctl, 0, (uint32_t) ((int64_t) hz - shift));
		x6100_set_freq(ctl, 1, (uint32_t) ((int64_t) hz - shift));
		x6100_ctl_set(ctl, X6100_REG_LO_CENTER, (uint32_t) shift & 0xFFFFFF);
	}

	//
	// Registers 0 / 6: TX enable per VFO ("ham band"), normally set from the band
	// plan; --oob sets it everywhere, as the stock UI does with fullband-tx=enable.
	// The band index in register 13 stays the real one (filters, calibration).
	//
	if (g_allow_oob) {
		x6100_ctl_set(ctl, X6100_REG_VFOA_BAND, 1);
		x6100_ctl_set(ctl, X6100_REG_VFOB_BAND, 1);
	}

	// Power cut (TOT on hand mic / VOX, stuck TX): TX enable off as well; the baseband
	// keeps the TX power it keyed with (2026-10-27: 3 W stayed after the cut to 0.1 W)
	if (rx->tot_cut || rx->tot_lock) {
		x6100_ctl_set(ctl, X6100_REG_VFOA_BAND, 0);
		x6100_ctl_set(ctl, X6100_REG_VFOB_BAND, 0);
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

	// Notch: by hand (DFL), else on the strongest birdie in the passband (BIRDIE NOTCH)
	int bn = birdie_notch(rx);

	if (bn > 0)
		x6100_ctl_set(ctl, X6100_REG_DNF, X6100_R24(bn, BIRDIE_NOTCH_W, 1));
	else
		x6100_ctl_set(ctl, X6100_REG_DNF, X6100_R24(rx->dnf_f, rx->dnf_w, rx->dnf));

	x6100_ctl_set(ctl, X6100_REG_KEYER, X6100_R33(rx->wpm, rx->keymode ? 3 - rx->keymode : 0, rx->iambic, rx->cwtone, (rx->cwvol * 63 + 50) / 100, 0));
	x6100_ctl_set(ctl, X6100_REG_QSK_KR, X6100_R34(rx->qsk, rx->ratio));

	// TX audio: line in / out levels and mic gains, mic select and squelch (charger,
	// speaker and aux IQ bits of register 21 kept), VOX, compressor, monitor
	x6100_ctl_set(ctl, X6100_REG_AUDIO_GAIN, X6100_R20(rx->line_in, rx->line_out, rx->imicg, rx->hmicg));
	x6100_ctl_update(ctl, X6100_REG_MIC_PTT_SQL, 0x0000FF03u, X6100_R21(rx->micsel, 0, 0, 0, 0, 0));	/* squelch: our own (g_sql_open) */
	x6100_ctl_set(ctl, X6100_REG_VOX, X6100_R22(rx->voxg, rx->voxag, rx->voxdly, rx->vox));
	x6100_ctl_set(ctl, X6100_REG_CMP, X6100_R25(rx->cmp_lvl, rx->cmp));
	x6100_ctl_set(ctl, X6100_REG_MONILEVEL, rx->moni ? (uint32_t) rx->moni_lvl : 0);

	// AGC: hang (knee -60, slope 6 as x6100_ctl_init) and the time constants
	x6100_ctl_set(ctl, X6100_REG_AGC, X6100_R27(-60, 6, g_agc_hang));
	x6100_ctl_set(ctl, X6100_REG_AGC_TC, X6100_R28(g_agc_slow, AGC_MID, g_agc_fast));

	x6100_set_filter(ctl, mode, low, high);
	x6100_ctl_set(ctl, X6100_REG_RXVOL, rx->mute || !g_sql_open ? 0 : (uint32_t) rx->vol);	/* squelch closed: muted */
	x6100_ctl_update(ctl, X6100_REG_RFG_TXPWR_FFTDEC, 0xFF, (uint32_t) rx->rfg);
	x6100_set_txpwr(ctl, rx->tot_cut ? 0.0f : (rx->tx_pwr_ovr >= 0 ? rx->tx_pwr_ovr : rx->txpwr) / 10.0f);	/* TOT cut, scan power */
	x6100_cal_apply(ctl, cal, x6100_band_index(hz));

	//
	// Tuner: the memory nearest the TX frequency, or straight through
	//
	const atum_t *am = rx->atu_on ? atu_find(tx_hz(rx)) : NULL;

	if (am != NULL) {
		x6100_atu_set_network(ctl, &am->net);
	} else {
		x6100_atu_t none = { 0, 0, 0 };

		x6100_atu_set_network(ctl, &none);
	}
}

//
// PTT (key, CAT) with the mod input on USB: the codec playback is the TX audio. Codec TX
// keys by itself (as for voice messages), so the PTT bit stays off then
//

static bool tx_usb_in(const rx_t *rx)
{
	x6100_mode_t mode = tx_mode(rx);
	bool data = mode == X6100_MODE_LSB_DIG || mode == X6100_MODE_USB_DIG;
	bool cw = mode == X6100_MODE_CW || mode == X6100_MODE_CWR;

	return rx->ptt && !rx->tuning && rx->send == SEND_NONE && !cw
		&& (g_modin[data] == MODIN_USB || (g_modin[data] == MODIN_LAN && rx->ptt_cat));	/* LAN: CAT keyed */
}

//
// Where the TX audio comes from, shown on the arc meter while transmitting (2026-11-07):
// MIC INT / HAND / AUTO (TX panel MIC), USB or LAN (DATA (OFF) MOD; LAN only when CAT
// keyed, else the mic), CW, MSG, TUNE. warn:
// the hand mic is selected, so the internal mic is not used
//

static const char *tx_source(const rx_t *rx, bool *warn)
{
	static const char *mic[3] = { "MIC INT", "MIC HAND", "MIC AUTO" };
	x6100_mode_t mode = tx_mode(rx);

	*warn = false;

	if (rx->tuning)
		return "TUNE";

	if (rx->send != SEND_NONE)
		return rx->send == SEND_SCAN ? "TUNE" : "MSG";

	if (mode == X6100_MODE_CW || mode == X6100_MODE_CWR)
		return "CW";

	if (tx_usb_in(rx))
		return g_modin[mode == X6100_MODE_LSB_DIG || mode == X6100_MODE_USB_DIG] == MODIN_LAN ? "LAN" : "USB";

	*warn = rx->micsel == 1;
	return mic[rx->micsel < 0 || rx->micsel > 2 ? 2 : rx->micsel];
}

//
// Register 12 from the TX state: PTT, tuner enable, tune
//

static void apply_flags(x6100_ctl_t *ctl, const rx_t *rx)
{
	bool usb_in = tx_usb_in(rx);	/* the codec is the TX audio: no PTT bit */

	x6100_ctl_set(ctl, X6100_REG_FLAGS, 0);
	x6100_set_ptt(ctl, rx->ptt && !usb_in);
	x6100_ctl_update(ctl, X6100_REG_FLAGS, X6100_FLAG_SWRSCAN, rx->send == SEND_SCAN ? X6100_FLAG_SWRSCAN : 0);
	x6100_set_codec_tx(ctl, rx->codec_tx || usb_in);
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

	if (bscan_running()) {
		rx->warn = "BIRDIE SCAN";
		return false;
	}

	if (g_i2c_down) {
		rx->warn = "I2C ERROR";
		return false;
	}

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
	rx->send = SEND_NONE;		/* the sender closes its audio on the next pass */
	rx->codec_tx = false;
	rx->tx_hz_ovr = 0;
	rx->tx_mode_ovr = -1;
	rx->tx_pwr_ovr = -1;

	if (why != NULL)
		rx->warn = why;
}

#define STEP_CH		3000000	/* FST "CH" on 60 m: sentinel step, never used as a raster */

static void show(const rx_t *rx)
{
	char st[8], sp[48] = "";

	if (rx->step == STEP_CH)
		snprintf(st, sizeof(st), "  CH");
	else
		snprintf(st, sizeof(st), "%4d", rx->step);

	// RX / TX apart (split, RIT, XIT, duplex): both frequencies, TX with its mode
	if (tx_apart(rx))
		snprintf(sp, sizeof(sp), "  RX %.3f TX %.3f %s%s", rx_hz(rx) / 1000.0, tx_hz(rx) / 1000.0,
			mode_name(tx_mode(rx)), rx->split ? " SPLIT" : dup_shift(rx) < 0 ? " DUP-" : dup_shift(rx) > 0 ? " DUP+" : "");

	char line[256];
	struct winsize ws;
	int cols = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;

	snprintf(line, sizeof(line), "%c %9.3f kHz %-5s %-4s step %s  vol %3d%s  rfg %3d  pre %s  agc %-4s%s%s",
		rx->mem_mode ? 'M' : rx->on_b ? 'B' : 'A', rx->freq / 1000.0, rx->band >= 0 ? g_bands[rx->band].name : "-",
		mode_name(rx->mode), st, rx->vol, rx->mute ? " (mute)" : "", rx->rfg, rx->pre ? "on " : "off",
		agc_name(rx->agc), sp, rx->lock ? "  LOCK" : "");

	// CW: keyer speed and key type
	if (rx->mode == X6100_MODE_CW || rx->mode == X6100_MODE_CWR)
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "  %d wpm %s", rx->wpm,
			rx->keymode == 0 ? "straight" : rx->keymode == 1 ? "paddle" : "paddle rev");

	if (g_vbat > 0.0 && g_tx_show)
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "  TX %4.1f W  swr %3.1f  alc %3d  bat %.1fV %d%%%s",
			g_tx_pwr, g_tx_swr, g_tx_alc, g_vbat, g_batcap, g_ext ? " ext" : "");
	else if (g_vbat > 0.0)
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "  iq %6.1f dB  bat %.1fV %d%%%s", g_iq_db, g_vbat,
			g_batcap, g_ext ? " ext" : "");

	//
	// One terminal row only: a line wider than the terminal wraps, and every redraw
	// (\r goes back to the start of the last row) then scrolls the console up, taking
	// output such as the memr list with it. Cut to the width, clear the rest of the row.
	//
	// A line being typed goes at the end and always stays visible; the status gives way
	{
		const char *in = g_cmd_view != NULL && g_cmd_view[0] != '\0' ? g_cmd_view : NULL;
		int room = cols - 1 - (in != NULL ? (int) strlen(in) + 4 : 0);

		room = room < 0 ? 0 : room;

		if (room < (int) sizeof(line))
			line[room] = '\0';

		printf("\r%s%s%s\033[K", line, in != NULL ? "  > " : "", in != NULL ? in : "");
	}
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
	else
		rx->gen_freq = rx->freq;	/* GEN band: the last frequency outside the ham bands */
}

//
// VFO A / B: the VFO in use is rx->freq / mode / fil, the other one is kept in
// rx->b_*; swapping exchanges them (A/B key, CI-V 07 00 / 01 / B0)
//

static void mem_mode_off(rx_t *rx);

static void vfo_swap(rx_t *rx)
{
	uint32_t f;
	x6100_mode_t m;
	int fil;

	mem_mode_off(rx);	/* the VFOs, not a memory channel */
	f = rx->freq;
	m = rx->mode;
	fil = rx->fil;

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
	mem_mode_off(rx);	/* the VFOs, not a memory channel */
	rx->b_freq = rx->freq;
	rx->b_mode = rx->mode;
	rx->b_fil = rx->fil;
	toast(rx->on_b ? "B COPIED TO A" : "A COPIED TO B");	/* the font has no = */
}

//
// Memory channels 1-99 (V/M short: memory panel). A channel holds frequency, mode,
// filter, split with its TX frequency and mode, duplex and a name. Kept in MEM_PATH
// as console tokens, one channel per line, written on every change:
//   mem 5 7074000 usb fil1 tx 7076000 usb name FT8 40m
//   mem 22 29660000 nfm fil1 dup- 100 name RPT 29.66
// (tx ... only for split channels; dupoff, or dup- / dup+ with the shift in kHz,
// only when not AUTO; name runs to the end of the line)
//

#define MEM_PATH	"/etc/x6100/memories.conf"
#define MEM_COUNT	99

typedef struct
{
	bool used;
	uint32_t hz;
	x6100_mode_t mode;
	int fil;
	bool split;
	uint32_t tx_hz;
	x6100_mode_t tx_mode;
	int dup;		/* duplex; DUP_AUTO when the line has none */
	int dup_khz;
	char name[16];
} mem_t;

static mem_t g_mem[MEM_COUNT + 1];	/* index = channel number; 0 unused */

static void mem_save(void)
{
	static const char *mn[] = { [X6100_MODE_LSB] = "lsb", [X6100_MODE_USB] = "usb", [X6100_MODE_CW] = "cw",
		[X6100_MODE_CWR] = "cwr", [X6100_MODE_AM] = "am", [X6100_MODE_NFM] = "nfm" };
	char tmp[] = MEM_PATH ".tmp";
	FILE *fp;

	if ((fp = fopen(tmp, "w")) == NULL) {
		fprintf(stderr, "\nmemories: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}

	fprintf(fp, "# x6100_trx memory channels: mem <n> <Hz> <mode> fil<n> [tx <Hz> <mode>] [dupoff | dup- <kHz> | dup+ <kHz>] [name <text>]\n");

	for (int i = 1; i <= MEM_COUNT; i++) {
		const mem_t *m = &g_mem[i];

		if (!m->used)
			continue;

		fprintf(fp, "mem %d %u %s fil%d", i, (unsigned) m->hz, mn[m->mode], m->fil + 1);

		if (m->split)
			fprintf(fp, " tx %u %s", (unsigned) m->tx_hz, mn[m->tx_mode]);

		if (m->dup == DUP_OFF)
			fprintf(fp, " dupoff");
		else if (m->dup == DUP_MINUS || m->dup == DUP_PLUS)
			fprintf(fp, " %s %d", g_dup_tok[m->dup], m->dup_khz);

		if (m->name[0] != '\0')
			fprintf(fp, " name %s", m->name);

		fprintf(fp, "\n");
	}

	if (fclose(fp) != 0 || rename(tmp, MEM_PATH) != 0)
		fprintf(stderr, "\nmemories: cannot save %s: %s\n", MEM_PATH, strerror(errno));
}

// Current VFO (split TX side, duplex) into channel n
static void mem_write(const rx_t *rx, int n)
{
	mem_t *m = &g_mem[n];
	char name[sizeof(m->name)];

	snprintf(name, sizeof(name), "%s", m->name);	/* a rewrite keeps the name */
	memset(m, 0, sizeof(*m));
	m->used = true;
	m->hz = rx->freq;
	m->mode = rx->mode;
	m->fil = rx->fil;
	m->split = rx->split;
	m->tx_hz = rx->b_freq;
	m->tx_mode = rx->b_mode;
	m->dup = rx->dup;
	m->dup_khz = rx->dup_khz;
	snprintf(m->name, sizeof(m->name), "%s", name);
	mem_save();
}

// Channel n into the VFO in use (with split: its TX side into the other VFO)
static bool mem_recall(rx_t *rx, int n)
{
	const mem_t *m = &g_mem[n];

	if (n < 1 || n > MEM_COUNT || !m->used)
		return false;

	set_freq(rx, m->hz);
	rx->mode = m->mode;
	rx->fil = m->fil;
	rx->split = m->split;
	rx->dup = m->dup;

	if (m->dup == DUP_MINUS || m->dup == DUP_PLUS)
		rx->dup_khz = m->dup_khz;

	if (m->split) {
		rx->b_freq = m->tx_hz;
		rx->b_mode = m->tx_mode;
		rx->b_fil = m->fil;
	}

	return true;
}

//
// Memory mode, as an Icom: V/M (or CI-V 08) keeps the VFO and tunes a channel; V/M
// again (or CI-V 07) puts the VFO back as it was. Tuning in memory mode changes the
// frequency only, not the channel (write it with 09 / the panel to keep it).
//

// The first stored channel from n on (wrapping), 0 when there is none
static int mem_used_from(int n)
{
	for (int i = 0; i < MEM_COUNT; i++) {
		int c = (n - 1 + i + MEM_COUNT) % MEM_COUNT + 1;

		if (g_mem[c].used)
			return c;
	}

	return 0;
}

// Into memory mode on channel n (an empty one: the next stored); false when no
// channel is stored
static bool mem_mode_on(rx_t *rx, int n)
{
	int c = mem_used_from(n >= 1 && n <= MEM_COUNT ? n : 1);
	vfo_keep_t *k = &rx->mem_vfo;

	if (c == 0)
		return false;

	if (!rx->mem_mode) {
		k->freq = rx->freq;
		k->b_freq = rx->b_freq;
		k->mode = rx->mode;
		k->b_mode = rx->b_mode;
		k->fil = rx->fil;
		k->b_fil = rx->b_fil;
		k->dup = rx->dup;
		k->dup_khz = rx->dup_khz;
		k->split = rx->split;
		rx->mem_mode = true;
	}

	rx->medit_ch = c;
	return mem_recall(rx, c);
}

// The VFO into channel n: in memory mode the one kept (where V/M came from)
static void mem_write_vfo(const rx_t *rx, int n)
{
	static rx_t t;
	const vfo_keep_t *k = &rx->mem_vfo;

	if (!rx->mem_mode) {
		mem_write(rx, n);
		return;
	}

	t = *rx;
	t.freq = k->freq;
	t.b_freq = k->b_freq;
	t.mode = k->mode;
	t.b_mode = k->b_mode;
	t.fil = k->fil;
	t.b_fil = k->b_fil;
	t.dup = k->dup;
	t.dup_khz = k->dup_khz;
	t.split = k->split;
	mem_write(&t, n);
}

// Back to the VFO as it was before memory mode
static void mem_mode_off(rx_t *rx)
{
	const vfo_keep_t *k = &rx->mem_vfo;

	if (!rx->mem_mode)
		return;

	set_freq(rx, k->freq);
	rx->b_freq = k->b_freq;
	rx->mode = k->mode;
	rx->b_mode = k->b_mode;
	rx->fil = k->fil;
	rx->b_fil = k->b_fil;
	rx->dup = k->dup;
	rx->dup_khz = k->dup_khz;
	rx->split = k->split;
	rx->mem_mode = false;
}

static void mem_clear(int n)
{
	memset(&g_mem[n], 0, sizeof(g_mem[n]));
	mem_save();
}

//
// CW keyer settings (registers 33 / 34), one table for the panel, the console and
// the state file. Key type: 0 straight key, 1 paddle (dot on the left paddle), 2 paddle
// reversed. The baseband has these the other way round (register 33 [9:8]: 1 gives
// dash left, 2 dot left, checked on the radio 2026-10-25; x6100_gui: auto left /
// auto right), so the register gets 3 - n for a paddle.
//

typedef struct
{
	const char *name;	/* panel row */
	const char *token;	/* console / state token */
	int min, max, step, def;
} kpar_t;

static const kpar_t g_kpar[] = {
	{ "KEY TYPE", "keytype", 0, 2, 1, 0 },
	{ "SPEED", "wpm", 5, 50, 1, 20 },
	{ "IAMBIC", "iambic", 0, 1, 1, 1 },
	{ "PITCH", "cwpitch", 300, 900, 10, 600 },
	{ "SIDETONE", "cwvol", 0, 100, 5, 20 },
	{ "BREAK-IN", "qsk", 0, 1000, 10, 100 },
	{ "RATIO", "ratio", 25, 45, 1, 30 },
};

#define KPARS	((int) (sizeof(g_kpar) / sizeof(g_kpar[0])))

static int *kpar_value(rx_t *rx, int i)
{
	int *v[] = { &rx->keymode, &rx->wpm, &rx->iambic, &rx->cwtone, &rx->cwvol, &rx->qsk, &rx->ratio };

	return v[i];
}

// Value as the panel and the console show it
static void kpar_text(const rx_t *rx, int i, char *s, size_t n)
{
	static const char *types[] = { "STRAIGHT", "PADDLE", "PADDLE REV" };
	int v = *kpar_value((rx_t *) rx, i);

	switch (i) {
	case 0:
		snprintf(s, n, "%s", types[v]);
		break;
	case 1:
		snprintf(s, n, "%d WPM", v);
		break;
	case 2:
		snprintf(s, n, "%s", v ? "B" : "A");
		break;
	case 3:
		snprintf(s, n, "%d HZ", v);
		break;
	case 4:
		snprintf(s, n, "%d %%", v);
		break;
	case 5:
		snprintf(s, n, "%d MS", v);
		break;
	case 6:
		snprintf(s, n, "%d.%d", v / 10, v % 10);
		break;
	default:
		snprintf(s, n, "%d", v);
		break;
	}
}

static void kpar_defaults(rx_t *rx)
{
	for (int i = 0; i < KPARS; i++)
		*kpar_value(rx, i) = g_kpar[i].def;
}

//
// TX audio settings (F4 long: TX panel), one table for the panel, the console and
// the state file, as the keyer. Defaults are the values written at start before
// (x6100_ctl_init, from the R1CBU GUI; VOX gain / delay are new, VOX stays off); ranges
// as in X6100Control / OpenX6X00 (the same R1CBU line). Squelch sits here too: register 21
// holds it with the mic select.
//

enum { XP_MIC, XP_IMICG, XP_HMICG, XP_CMP, XP_CMPLVL, XP_VOX, XP_VOXG, XP_ANTIVOX, XP_VOXDLY,
	XP_MONI, XP_MONILVL, XP_LINEIN, XP_LINEOUT, XP_SQL, XP_TOT, XPARS };

static const kpar_t g_xpar[XPARS] = {
	[XP_MIC] = { "MIC", "mic", 0, 2, 1, 2 },
	[XP_IMICG] = { "INT MIC GAIN", "imicg", 0, 50, 1, 30 },
	[XP_HMICG] = { "HAND MIC GAIN", "hmicg", 0, 50, 1, 20 },
	[XP_CMP] = { "COMPRESSOR", "comp", 0, 1, 1, 0 },
	[XP_CMPLVL] = { "COMP LEVEL", "complvl", 1, 3, 1, 2 },
	[XP_VOX] = { "VOX", "vox", 0, 1, 1, 0 },
	[XP_VOXG] = { "VOX GAIN", "voxg", 0, 100, 1, 50 },
	[XP_ANTIVOX] = { "ANTI-VOX", "antivox", 0, 100, 1, 0 },
	[XP_VOXDLY] = { "VOX DELAY", "voxdly", 100, 2000, 50, 500 },
	[XP_MONI] = { "MONITOR", "moni", 0, 1, 1, 1 },
	[XP_MONILVL] = { "MONI LEVEL", "monilvl", 0, 100, 1, 30 },
	[XP_LINEIN] = { "LINE IN", "linein", 0, 36, 1, 10 },	/* 0-36 as OpenX6X00; 10 as stock */
	[XP_LINEOUT] = { "LINE OUT", "lineout", 0, 36, 1, 10 },
	[XP_SQL] = { "SQUELCH", "sql", 0, 100, 1, 0 },
	[XP_TOT] = { "TX TIMEOUT", "tot", 0, 3, 1, 2 },
};

static const char *g_mic_tok[3] = { "int", "hand", "auto" };
static const char *g_cmp_tok[4] = { "", "1:2", "1:4", "1:8" };

static int *xpar_value(rx_t *rx, int i)
{
	int *v[XPARS] = { &rx->micsel, &rx->imicg, &rx->hmicg, &rx->cmp, &rx->cmp_lvl, &rx->vox, &rx->voxg,
		&rx->voxag, &rx->voxdly, &rx->moni, &rx->moni_lvl, &rx->line_in, &rx->line_out, &rx->sql, &rx->tot };

	return v[i];
}

static bool xpar_onoff(int i)
{
	return i == XP_CMP || i == XP_VOX || i == XP_MONI;
}

// Value as the console and the state file write it
static void xpar_token(const rx_t *rx, int i, char *s, size_t n)
{
	int v = *xpar_value((rx_t *) rx, i);

	if (i == XP_MIC)
		snprintf(s, n, "%s", g_mic_tok[v]);
	else if (i == XP_CMPLVL)
		snprintf(s, n, "%s", g_cmp_tok[v]);
	else if (i == XP_TOT)
		snprintf(s, n, "%s%.0d", v == 0 ? "off" : "", g_tot_min[v]);
	else if (xpar_onoff(i))
		snprintf(s, n, "%s", v ? "on" : "off");
	else
		snprintf(s, n, "%d", v);
}

// Value as the panel shows it
static void xpar_text(const rx_t *rx, int i, char *s, size_t n)
{
	xpar_token(rx, i, s, n);

	if (i == XP_VOX)	/* register 22 is written, but BASE 1.1.9 keys nothing (2026-10-30) */
		snprintf(s + strlen(s), n - strlen(s), " N/A");
	else if (i == XP_VOXDLY)
		snprintf(s + strlen(s), n - strlen(s), " MS");
	else if (i == XP_TOT && *xpar_value((rx_t *) rx, i) > 0)
		snprintf(s + strlen(s), n - strlen(s), " MIN");

	for (char *c = s; *c != '\0'; c++)	/* the panel font is upper case */
		*c = (char) toupper((unsigned char) *c);
}

// Console / state value for row i; -1 when it does not parse or is out of range
static int xpar_parse(int i, const char *vs)
{
	int v = -1;

	if (vs == NULL)
		return -1;

	if (i == XP_MIC) {
		for (int k = 0; k < 3; k++)
			v = strcasecmp(vs, g_mic_tok[k]) == 0 ? k : v;
	} else if (i == XP_CMPLVL) {
		for (int k = 1; k < 4; k++)
			v = strcasecmp(vs, g_cmp_tok[k]) == 0 ? k : v;
	} else if (i == XP_TOT) {
		for (int k = 0; k < 4; k++)
			v = strcasecmp(vs, "off") == 0 ? 0 : atoi(vs) == g_tot_min[k] && isdigit((unsigned char) vs[0]) ? k : v;

		return v;
	} else if (xpar_onoff(i)) {
		v = strcasecmp(vs, "on") == 0 ? 1 : strcasecmp(vs, "off") == 0 ? 0 : -1;
	}

	if (v < 0 && isdigit((unsigned char) vs[0]))
		v = atoi(vs);

	return v >= g_xpar[i].min && v <= g_xpar[i].max ? v : -1;
}

static void xpar_defaults(rx_t *rx)
{
	for (int i = 0; i < XPARS; i++)
		*xpar_value(rx, i) = g_xpar[i].def;
}

//
// Band keys: 160m .. 6m, 4m with GEN (everything outside the ham bands, as Icom's GENE)
// in between at its frequency, round and round. GEN keeps the last frequency used
// outside the bands and its mode: at 5450 kHz the order is .. 60m, GEN, 40m ..; at
// 500 kHz or 60 MHz GEN sits between 6m and 160m.
//

static void band_step(rx_t *rx, int dir)
{
	uint32_t gen = rx->band < 0 ? rx->freq : rx->gen_freq;
	int gpos = 0;		/* GEN slot: after the bands below it */
	int slot;

	rx->mem_mode = false;	/* a band key leaves memory mode on that band, as an Icom */

	for (int i = 0; i < BAND_COUNT; i++) {
		if (g_bands[i].high < gen)
			gpos = i + 1;
	}

	if (rx->band < 0) {
		rx->gen_mode = rx->mode;			/* leaving GEN: keep its mode */
		slot = gpos;
	} else {
		slot = rx->band < gpos ? rx->band : rx->band + 1;
	}

	slot = (slot + dir + BAND_COUNT + 1) % (BAND_COUNT + 1);

	if (slot == gpos) {
		set_freq(rx, rx->gen_freq);
		rx->mode = rx->gen_mode;
		return;
	}

	int band = slot < gpos ? slot : slot - 1;

	set_freq(rx, rx->band_freq[band]);
	rx->mode = rx->freq < 10000000 ? X6100_MODE_LSB : X6100_MODE_USB;
}

//
// Key handling; returns true when the state changed
//

//
// Fixed edges, IC-705 style: four windows (EDGE 1-4) per frequency range, used by FIX
// and SCROLL-F. The ranges are the IC-705's as wfview numbers them for 27 1E (from the
// Range values in its IC-705 rig file): 01 below 1.6 MHz, 02 1.6-2, 03 2-6, 04 6-8,
// 05 8-11, 06 11-15, 07 15-20, 08 20-22, 09 22-26, 10 26-30, 11 30-45, 12 45-60 MHz.
// An edge is at most 96 kHz wide (the IQ width), 75 kHz with the IF shift (edge_max);
// defaults: four windows that wide from the bottom of the ham band in the range (the last
// one at its top), or of the range itself.
//

#define EDGE_RANGES	12
#define EDGE_MAX_HZ	96000		/* the IQ width */
#define EDGE_IFS_MAX_HZ	75000		/* with the IF shift: (96 - w) / 2 >= 10 kHz, see rx_lo */

typedef struct
{
	uint32_t lo, hi;
} edge_t;

static const uint32_t g_edge_bound[EDGE_RANGES + 1] = {
	500000, 1600000, 2000000, 6000000, 8000000, 11000000, 15000000, 20000000, 22000000, 26000000,
	30000000, 45000000, 60000000,
};

static edge_t g_edge[EDGE_RANGES][4];

// The widest window the IQ covers whole: with the IF shift the LO cannot sit in the middle
// of the window while the VFO is there (the spike), so 75 kHz (2026-10-31; 96 left up
// to 10 kHz unpainted)
static uint32_t edge_max(void)
{
	return g_ifshift != 0 ? EDGE_IFS_MAX_HZ : EDGE_MAX_HZ;
}

// Width of edge e in range r as used: a wider one stored before is cut to edge_max
static int edge_w(int r, int e)
{
	uint32_t w = g_edge[r][e].hi - g_edge[r][e].lo;

	return (int) (w < edge_max() ? w : edge_max());
}

// Range index 0-11 of a frequency, -1 outside
static int edge_range(uint32_t hz)
{
	for (int r = 0; r < EDGE_RANGES; r++)
		if (hz < g_edge_bound[r + 1])
			return hz >= g_edge_bound[r] ? r : -1;

	return -1;
}

static void edge_default(int r, int e, edge_t *out)
{
	uint32_t lo = g_edge_bound[r], hi = g_edge_bound[r + 1] - 1;

	for (int b = 0; b < BAND_COUNT; b++) {
		if (g_bands[b].low >= lo && g_bands[b].low < hi) {
			lo = g_bands[b].low;
			hi = g_bands[b].high;
			break;
		}
	}

	uint32_t w = hi - lo < edge_max() ? hi - lo : edge_max();
	uint32_t from = e < 3 ? lo + (uint32_t) e * w : hi - w;

	out->lo = from + w > hi ? hi - w : from;
	out->hi = out->lo + w;
}

static void edge_defaults(void)
{
	for (int r = 0; r < EDGE_RANGES; r++)
		for (int e = 0; e < 4; e++)
			edge_default(r, e, &g_edge[r][e]);
}

//
// Scope window. CENTER / SCROLL-C: the VFO in the middle, rx->span wide. FIX / SCROLL-F:
// the selected edge of the VFO's range; SCROLL-F moves the window by whole windows when the VFO
// leaves it. Outside every range (below 0.5 MHz) the fixed window starts at the VFO.
//

static bool scope_fixed(const rx_t *rx)
{
	return rx->scope == SCOPE_FIX || rx->scope == SCOPE_FIX_S;
}

static int scope_width(const rx_t *rx)
{
	return scope_fixed(rx) ? rx->fix_w : rx->span;
}

// Put the fixed window on the edge in use (on entering FIX, a new range or edge)
static void scope_fix_start(rx_t *rx)
{
	int r = edge_range(rx->freq), e = clampi(rx->edge, 1, 4) - 1;

	rx->fix_range = r;
	rx->fix_edge = rx->edge;

	if (r < 0) {
		rx->fix_w = rx->span < (int) edge_max() ? rx->span : (int) edge_max();
		rx->fix_lo = ((int64_t) rx->freq - rx->fix_w / 2) / 1000 * 1000;
		return;
	}

	rx->fix_lo = g_edge[r][e].lo;
	rx->fix_w = edge_w(r, e);
}

// Every pass: a new range or edge moves the window, SCROLL-F follows the VFO
static void scope_follow(rx_t *rx)
{
	if (!scope_fixed(rx))
		return;

	int r = edge_range(rx->freq), e = clampi(rx->edge, 1, 4) - 1;

	if (r != rx->fix_range || rx->edge != rx->fix_edge
		|| (r >= 0 && rx->scope == SCOPE_FIX && (rx->fix_lo != (int64_t) g_edge[r][e].lo
		|| rx->fix_w != edge_w(r, e))))
		scope_fix_start(rx);

	if (rx->scope != SCOPE_FIX_S || rx->fix_w <= 0 || rx->eedit)	/* editing: the edge itself */
		return;

	// SCROLL-F, as an Icom: the VFO out of the window moves it by whole windows on the
	// grid of the edge (7.000-7.100, at 7.100 and up 7.100-7.200), not along with the VFO
	if ((int64_t) rx->freq < rx->fix_lo || (int64_t) rx->freq >= rx->fix_lo + rx->fix_w) {
		int64_t base = r >= 0 ? (int64_t) g_edge[r][e].lo : rx->fix_lo;
		int64_t d = (int64_t) rx->freq - base;
		int64_t k = d >= 0 ? d / rx->fix_w : -((-d + rx->fix_w - 1) / rx->fix_w);

		rx->fix_lo = base + k * rx->fix_w;
	}
}

// Edge panel (F2 long in FIX / SCROLL-F), as an Icom's fixed edge menu: the lower or upper
// end of the edge in use in 1 kHz steps, inside its range; the other end follows when
// the width would pass edge_max, at least 1 kHz apart. The window shows the edge.
static void edge_adjust(rx_t *rx, int steps)
{
	int r = edge_range(rx->freq), e = clampi(rx->edge, 1, 4) - 1;
	int64_t lo, hi, mx = edge_max(), blo, bhi;

	if (r < 0)
		return;

	lo = g_edge[r][e].lo;
	hi = g_edge[r][e].hi;
	blo = g_edge_bound[r];
	bhi = g_edge_bound[r + 1];
	hi = hi - lo > mx ? lo + mx : hi;	/* a wider one stored before: as it is used */

	if (rx->eedit_par == 0) {
		lo += (int64_t) steps * 1000;
		lo = lo < blo ? blo : lo > bhi - 1000 ? bhi - 1000 : lo;
		hi = hi < lo + 1000 ? lo + 1000 : hi > lo + mx ? lo + mx : hi;
	} else {
		hi += (int64_t) steps * 1000;
		hi = hi < blo + 1000 ? blo + 1000 : hi > bhi ? bhi : hi;
		lo = lo > hi - 1000 ? hi - 1000 : lo < hi - mx ? hi - mx : lo;
	}

	g_edge[r][e].lo = (uint32_t) lo;
	g_edge[r][e].hi = (uint32_t) hi;
	scope_fix_start(rx);
}

//
// Scope settings (F1 long: scope panel), as the IC-7851 scope set menu: mode, center
// type, VBW, averaging, waveform type, max hold, the colours of the trace (current =
// fill, line, max hold), the waterfall (on, speed, size, peak level) and the scope
// during TX. Defaults as set on the IC-7851. One table for the panel, the console and
// the state file; the mode row is rx->scope (kept on the scope line).
//

enum { SP_MODE, SP_CTYPE, SP_REF, SP_VBW, SP_AVG, SP_WAVE, SP_HOLD, SP_FILL_R, SP_FILL_G, SP_FILL_B,
	SP_LINE_R, SP_LINE_G, SP_LINE_B, SP_HOLD_R, SP_HOLD_G, SP_HOLD_B, SP_WF, SP_WF_SPEED, SP_WF_SIZE,
	SP_WF_LEVEL, SP_WF_3D, SP_TXSCOPE, SP_AF_REF, SP_AF_WAVE, SP_AF_AVG, SP_AF_HOLD, SP_AF_FILL_R, SP_AF_FILL_G,
	SP_AF_FILL_B, SP_AF_LINE_R, SP_AF_LINE_G, SP_AF_LINE_B, SP_AF_HOLD_R, SP_AF_HOLD_G, SP_AF_HOLD_B, SPARS };

enum { WAVE_FILL, WAVE_LINE, WAVE_FILL_LINE };
enum { HOLD_OFF, HOLD_DECAY, HOLD_10S, HOLD_ON };

static const kpar_t g_spar[SPARS] = {
	[SP_MODE] = { "MODE", "", 0, 3, 1, 0 },
	[SP_CTYPE] = { "CENTER TYPE", "ctype", 0, 1, 1, 1 },
	[SP_REF] = { "REF LEVEL", "", -30, 30, 1, 0 },		/* rx->rref: ref on the scope line */
	[SP_VBW] = { "VBW", "vbw", 0, 1, 1, 0 },
	[SP_AVG] = { "AVERAGING", "scavg", 1, 4, 1, 2 },
	[SP_WAVE] = { "WAVEFORM TYPE", "wave", 0, 2, 1, WAVE_FILL_LINE },
	[SP_HOLD] = { "MAX HOLD", "maxhold", 0, 3, 1, HOLD_DECAY },
	[SP_FILL_R] = { "CURRENT RED", "fillr", 0, 255, 1, 255 },
	[SP_FILL_G] = { "CURRENT GREEN", "fillg", 0, 255, 1, 63 },
	[SP_FILL_B] = { "CURRENT BLUE", "fillb", 0, 255, 1, 127 },
	[SP_LINE_R] = { "LINE RED", "liner", 0, 255, 1, 255 },
	[SP_LINE_G] = { "LINE GREEN", "lineg", 0, 255, 1, 255 },
	[SP_LINE_B] = { "LINE BLUE", "lineb", 0, 255, 1, 0 },
	[SP_HOLD_R] = { "MAX HOLD RED", "holdr", 0, 255, 1, 0 },
	[SP_HOLD_G] = { "MAX HOLD GREEN", "holdg", 0, 255, 1, 127 },
	[SP_HOLD_B] = { "MAX HOLD BLUE", "holdb", 0, 255, 1, 127 },
	[SP_WF] = { "WATERFALL", "wf", 0, 1, 1, 1 },
	[SP_WF_SPEED] = { "WATERFALL SPEED", "wfspeed", 0, 2, 1, 2 },
	[SP_WF_SIZE] = { "WATERFALL SIZE", "wfsize", 0, 2, 1, 1 },
	[SP_WF_LEVEL] = { "WF PEAK LEVEL", "wflevel", 30, 80, 10, 50 },
	[SP_WF_3D] = { "WATERFALL 3D", "wf3d", 0, 1, 1, 0 },
	[SP_TXSCOPE] = { "SCOPE DURING TX", "txscope", 0, 1, 1, 1 },
	[SP_AF_REF] = { "AF REF LEVEL", "", -30, 30, 1, 0 },	/* rx->aref: aref on the scope line */
	[SP_AF_WAVE] = { "AF WAVEFORM TYPE", "afwave", 0, 2, 1, WAVE_FILL_LINE },
	[SP_AF_AVG] = { "AF AVERAGING", "afavg", 1, 4, 1, 2 },
	[SP_AF_HOLD] = { "AF MAX HOLD", "afhold", 0, 3, 1, HOLD_DECAY },
	[SP_AF_FILL_R] = { "AF CURRENT RED", "affillr", 0, 255, 1, 255 },
	[SP_AF_FILL_G] = { "AF CURRENT GREEN", "affillg", 0, 255, 1, 63 },
	[SP_AF_FILL_B] = { "AF CURRENT BLUE", "affillb", 0, 255, 1, 127 },
	[SP_AF_LINE_R] = { "AF LINE RED", "afliner", 0, 255, 1, 255 },
	[SP_AF_LINE_G] = { "AF LINE GREEN", "aflineg", 0, 255, 1, 255 },
	[SP_AF_LINE_B] = { "AF LINE BLUE", "aflineb", 0, 255, 1, 0 },
	[SP_AF_HOLD_R] = { "AF MAX HOLD RED", "afholdr", 0, 255, 1, 0 },
	[SP_AF_HOLD_G] = { "AF MAX HOLD GREEN", "afholdg", 0, 255, 1, 127 },
	[SP_AF_HOLD_B] = { "AF MAX HOLD BLUE", "afholdb", 0, 255, 1, 127 },
};

static int g_sp[SPARS];
static uint32_t g_iq_hz;	/* IQ centre of the last spectrum (max hold starts again when it moves) */
static bool g_scope_hold;	/* scope HOLD (CI-V 27 17, console hold): scope and waterfall frozen, the meter not */
static bool g_dc_hide;		/* IF shift on: the DC / LO spike at the IQ centre is not drawn */

#define DC_HIDE_BINS	2	/* IQ centre +- 2 bins (+- 375 Hz) interpolated over on the scope */
static bool g_af_seen;		/* an AF scope row came from the state or settings file */

//
// The words of a row (console, state file, panel in upper case); NULL: a number
//

static const char *const g_sp_words[SPARS][4] = {
	[SP_MODE] = { "center", "scroll-c", "fix", "scroll-f" },
	[SP_CTYPE] = { "filter", "carrier" },
	[SP_VBW] = { "narrow", "wide" },
	[SP_WAVE] = { "fill", "line", "fill+line" },
	[SP_HOLD] = { "off", "decay", "10s", "on" },
	[SP_WF] = { "off", "on" },
	[SP_WF_SPEED] = { "slow", "mid", "fast" },
	[SP_WF_SIZE] = { "small", "mid", "large" },
	[SP_WF_3D] = { "off", "on" },
	[SP_TXSCOPE] = { "off", "on" },
	[SP_AF_WAVE] = { "fill", "line", "fill+line" },
	[SP_AF_HOLD] = { "off", "decay", "10s", "on" },
};

static bool spar_onoff(int i)
{
	return i == SP_WF || i == SP_WF_3D || i == SP_TXSCOPE;
}

static bool spar_colour(int i)
{
	return (i >= SP_FILL_R && i <= SP_HOLD_B) || (i >= SP_AF_FILL_R && i <= SP_AF_HOLD_B);
}

// The red row of the colour that row i belongs to
static int spar_colour_base(int i)
{
	int first = i >= SP_AF_FILL_R ? SP_AF_FILL_R : SP_FILL_R;

	return first + (i - first) / 3 * 3;
}

// Trace colours as 0xRRGGBB from the R, G, B rows
static uint32_t spar_rgb(int r)
{
	return (uint32_t) g_sp[r] << 16 | (uint32_t) g_sp[r + 1] << 8 | (uint32_t) g_sp[r + 2];
}

static int spar_get(const rx_t *rx, int i)
{
	return i == SP_MODE ? (int) rx->scope : i == SP_REF ? rx->rref : i == SP_AF_REF ? rx->aref : g_sp[i];
}

static void spar_set(rx_t *rx, int i, int v)
{
	if (i == SP_REF || i == SP_AF_REF) {
		*(i == SP_REF ? &rx->rref : &rx->aref) = v;	/* as F2 long and ref / aref */
		return;
	}

	if (i != SP_MODE) {
		g_sp[i] = v;
		return;
	}

	bool was_fix = scope_fixed(rx);

	rx->scope = (scope_mode_t) v;

	if (!was_fix && scope_fixed(rx))
		scope_fix_start(rx);
}

// Value as the console and the state file write it
static void spar_token(const rx_t *rx, int i, char *s, size_t n)
{
	int v = spar_get(rx, i);

	if (g_sp_words[i][0] != NULL)
		snprintf(s, n, "%s", g_sp_words[i][v]);
	else if (i == SP_REF || i == SP_AF_REF)
		snprintf(s, n, "%+d", v);
	else if (i == SP_AVG || i == SP_AF_AVG)
		snprintf(s, n, "%s%.0d", v == 1 ? "off" : "", v == 1 ? 0 : v);
	else
		snprintf(s, n, "%d", v);
}

// Console / state value for row i; -1 when it does not parse or is out of range
static int spar_parse(int i, const char *vs)
{
	int v = -1;

	if (vs == NULL)
		return -1;

	if (g_sp_words[i][0] != NULL) {
		for (int k = 0; k < 4 && g_sp_words[i][k] != NULL; k++)
			v = strcasecmp(vs, g_sp_words[i][k]) == 0 ? k : v;
	} else if ((i == SP_AVG || i == SP_AF_AVG) && strcasecmp(vs, "off") == 0) {
		v = 1;
	} else if (isdigit((unsigned char) vs[0])) {
		v = atoi(vs);
	}

	if (i == SP_WF_LEVEL && v % 10 != 0)
		return -1;

	return v >= g_spar[i].min && v <= g_spar[i].max ? v : -1;
}

// The words a row takes, for a console error ("narrow, wide"); empty for numbers
static void spar_words(int i, char *s, size_t n)
{
	s[0] = '\0';

	for (int k = 0; k < 4 && g_sp_words[i][k] != NULL; k++)
		snprintf(s + strlen(s), n - strlen(s), "%s%s", k > 0 ? ", " : "", g_sp_words[i][k]);
}

static void spar_defaults(void)
{
	for (int i = 1; i < SPARS; i++)
		g_sp[i] = g_spar[i].def;
}

// Audio scope rows from the RF ones (a state file from before the AF rows existed)
static void spar_af_from_rf(void)
{
	g_sp[SP_AF_WAVE] = g_sp[SP_WAVE];
	g_sp[SP_AF_AVG] = g_sp[SP_AVG];
	g_sp[SP_AF_HOLD] = g_sp[SP_HOLD];

	for (int i = 0; i < 9; i++)
		g_sp[SP_AF_FILL_R + i] = g_sp[SP_FILL_R + i];
}

//
// Messages (MSG): M1-M8 in MSG_PATH, CW text or a WAV file in VOICE_DIR, sent with
// the codec as TX audio (register 12 0x4000), the way FT8 programs do; no PTT. A voice
// message plays in the mode in use (LSB / USB / AM / NFM). A CW message is a keyed
// tone at the CW pitch in USB, the VFO the pitch below the dial, so the carrier lands
// on the dial as with the keyer; the keying edges are smoothed (about 5 ms). The radio's
// microphone never reaches Linux: voice messages are recorded elsewhere (48 kHz,
// 16 bit WAV, mono or stereo). MSG long repeats the last message every msgrepeat s.
//

#define MSG_COUNT	8
#define MSG_PATH	"/etc/x6100/messages.conf"
#define VOICE_DIR	"/etc/x6100/voice"
#define SEND_RATE	48000
#define SEND_PERIOD	512
#define SEND_PERIODS	4
#define SEND_PREROLL	4800	/* frames of silence first: 100 ms for the baseband to key */
#define SEND_LEVEL	0.3	/* CW tone level (about -10 dBFS); voice plays at the file level. ALC test: TODO */

enum { MSG_EMPTY, MSG_CW, MSG_VOICE };

typedef struct
{
	int type;
	char text[64];		/* CW text, or the WAV file name in VOICE_DIR */
} msg_t;

static msg_t g_msg[MSG_COUNT + 1];	/* index = message number; 0: the CI-V 17 text */
static int g_msg_repeat = 10;		/* s between repeats (MSG long) */

typedef struct
{
	x6100_pcm_t pcm;
	bool open;
	int msg;		/* message being sent */
	bool repeat;		/* MSG long: send again after g_msg_repeat s */
	uint64_t next_at;	/* repeat: next start (ms) */
	uint64_t start_ms;
	uint64_t written;	/* frames given to the codec */
	bool done;		/* all audio written */
	// CW: one flag per dot unit
	uint8_t units[1600];
	int nunits;
	double unit_frames;
	double phase, env;
	double tone;
	// Voice
	FILE *wav;
	uint32_t wav_left;	/* bytes of sample data left */
	int wav_ch;
} sender_t;

static sender_t g_send;

static void msg_save(void)
{
	char tmp[] = MSG_PATH ".tmp";
	FILE *fp;

	if (g_conf_loading)
		return;

	if ((fp = fopen(tmp, "w")) == NULL) {
		fprintf(stderr, "\nmessages: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}

	fprintf(fp, "# x6100_trx messages: msg <1-%d> cw <text> | msg <n> voice <file.wav in %s>\n", MSG_COUNT, VOICE_DIR);
	fprintf(fp, "msgrepeat %d\n", g_msg_repeat);

	for (int i = 1; i <= MSG_COUNT; i++)
		if (g_msg[i].type != MSG_EMPTY)
			fprintf(fp, "msg %d %s %s\n", i, g_msg[i].type == MSG_CW ? "cw" : "voice", g_msg[i].text);

	fclose(fp);

	if (rename(tmp, MSG_PATH) < 0)
		fprintf(stderr, "\nmessages: cannot write %s: %s\n", MSG_PATH, strerror(errno));
}

// Morse of one character as dots and dashes; NULL: not sendable
static const char *morse(char c)
{
	static const char *const az[26] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
		"-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
		"-.--", "--.." };
	static const char *const num[10] = { "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...",
		"---..", "----." };
	static const struct { char c; const char *m; } punct[] = {
		{ '/', "-..-." }, { '?', "..--.." }, { '.', ".-.-.-" }, { ',', "--..--" }, { '=', "-...-" },
		{ '+', ".-.-." }, { '-', "-....-" }, { ':', "---..." }, { '\'', ".----." }, { '(', "-.--." },
		{ ')', "-.--.-" }, { '"', ".-..-." }, { '@', ".--.-." },
	};

	c = (char) toupper((unsigned char) c);

	if (c >= 'A' && c <= 'Z')
		return az[c - 'A'];

	if (c >= '0' && c <= '9')
		return num[c - '0'];

	for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
		if (punct[i].c == c)
			return punct[i].m;

	return NULL;
}

// CW text into key units (1 = key down): dot 1, dash 3, gap 1, letter 3, word 7; ^ joins
// the next two letters into one sign (^AR, ^SK)
static int cw_units(const char *text, uint8_t *u, int max)
{
	int n = 0, join = 0;

	for (const char *p = text; *p != '\0'; p++) {
		const char *m = morse(*p);

		if (*p == '^') {
			join = 2;	/* the letter after this one follows without the letter gap */
			continue;
		}

		if (*p == ' ') {
			for (int k = 0; k < 4 && n < max; k++)	/* 3 after the letter + 4 = 7 */
				u[n++] = 0;
			continue;
		}

		if (m == NULL)
			continue;

		for (const char *e = m; *e != '\0'; e++) {
			for (int k = 0; k < (*e == '-' ? 3 : 1) && n < max; k++)
				u[n++] = 1;

			for (int k = 0; k < (e[1] != '\0' || join == 2 ? 1 : 3) && n < max; k++)
				u[n++] = 0;
		}

		join = join > 0 ? join - 1 : 0;
	}

	return n;
}

// WAV: the sample data of a 48 kHz, 16 bit PCM file; false when it is not one
static bool wav_open(sender_t *s, const char *name)
{
	char path[192];
	uint8_t h[12], ck[8], fmt[16];

	snprintf(path, sizeof(path), "%s/%s", VOICE_DIR, name);

	if ((s->wav = fopen(path, "rb")) == NULL)
		return false;

	if (fread(h, 1, 12, s->wav) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
		goto bad;

	s->wav_ch = 0;

	while (fread(ck, 1, 8, s->wav) == 8) {
		uint32_t len = (uint32_t) ck[4] | (uint32_t) ck[5] << 8 | (uint32_t) ck[6] << 16 | (uint32_t) ck[7] << 24;

		if (memcmp(ck, "fmt ", 4) == 0 && len >= 16) {
			if (fread(fmt, 1, 16, s->wav) != 16)
				goto bad;

			uint32_t rate = (uint32_t) fmt[4] | (uint32_t) fmt[5] << 8 | (uint32_t) fmt[6] << 16 | (uint32_t) fmt[7] << 24;

			if (fmt[0] != 1 || fmt[1] != 0 || fmt[14] != 16 || rate != SEND_RATE || (fmt[2] != 1 && fmt[2] != 2))
				goto bad;

			s->wav_ch = fmt[2];

			if (fseek(s->wav, (long) (len - 16 + (len & 1)), SEEK_CUR) < 0)
				goto bad;
		} else if (memcmp(ck, "data", 4) == 0 && s->wav_ch > 0) {
			s->wav_left = len;
			return true;
		} else if (fseek(s->wav, (long) (len + (len & 1)), SEEK_CUR) < 0) {
			goto bad;
		}
	}

bad:
	fclose(s->wav);
	s->wav = NULL;
	return false;
}

static void send_close(sender_t *s)
{
	if (s->open)
		x6100_pcm_close(&s->pcm);

	if (s->wav != NULL)
		fclose(s->wav);

	s->open = false;
	s->wav = NULL;
}

// Start message n; false (and rx->warn) when it cannot go out here
static bool msg_start(rx_t *rx, int n)
{
	sender_t *s = &g_send;
	const msg_t *m = n >= 0 && n <= MSG_COUNT ? &g_msg[n] : NULL;	/* 0: CI-V 17 text */
	bool cw_mode = rx->mode == X6100_MODE_CW || rx->mode == X6100_MODE_CWR;
	int rc;

	if (m == NULL || m->type == MSG_EMPTY) {
		toast("NO MESSAGE");
		return false;
	}

	if (rx->ptt || rx->tuning || rx->send != SEND_NONE || rx->bb_tx || !tx_allowed(rx))
		return false;

	if (m->type == MSG_CW && !cw_mode) {
		rx->warn = "CW MESSAGE: CW MODE";
		return false;
	}

	if (m->type == MSG_VOICE && cw_mode) {
		rx->warn = "VOICE MESSAGE: NOT IN CW";
		return false;
	}

	send_close(s);
	s->msg = n;
	s->written = 0;
	s->done = false;
	s->phase = 0.0;
	s->env = 0.0;

	if (m->type == MSG_CW) {
		s->nunits = cw_units(m->text, s->units, (int) sizeof(s->units));
		s->unit_frames = 1.2 / (double) rx->wpm * SEND_RATE;
		s->tone = (double) rx->cwtone;
	} else if (!wav_open(s, m->text)) {
		rx->warn = "WAV: 48 KHZ 16 BIT";
		return false;
	}

	if ((rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0)
		fprintf(stderr, "\nmessage: mixer setup: %s\n", strerror(-rc));

	if ((rc = x6100_pcm_open(&s->pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, false, SEND_RATE, 2, SEND_PERIOD,
		SEND_PERIODS)) < 0) {
		fprintf(stderr, "\nmessage: playback: %s\n", strerror(-rc));
		send_close(s);
		rx->warn = "NO AUDIO OUT";
		return false;
	}

	s->open = true;
	s->start_ms = now_ms();
	rx->send = SEND_MSG;
	rx->send_since = s->start_ms;
	rx->codec_tx = true;
	rx->tx_hz_ovr = m->type == MSG_CW ? tx_hz(rx) - (uint32_t) rx->cwtone : 0;
	rx->tx_mode_ovr = m->type == MSG_CW ? X6100_MODE_USB : -1;
	rx->warn = NULL;

	return true;
}

static bool send_stop(rx_t *rx);

//
// CI-V 17 (send CW): the text goes out as message 0, as a CW message. Text that comes
// while it is still sending is added to it, as an Icom does with its CW buffer; NULL
// (17 FF) stops it. False (NG): not in CW mode, cannot transmit here, buffer full.
//

static bool cw_send_text(rx_t *rx, const char *text)
{
	sender_t *s = &g_send;

	if (text == NULL) {
		if (rx->send == SEND_MSG && s->msg == 0)
			send_stop(rx);

		return true;
	}

	if (rx->send == SEND_MSG && s->msg == 0) {
		// Key units already gone by stay gone: the new text starts at the unit in play
		int u_now = s->written > SEND_PREROLL ? (int) ((double) (s->written - SEND_PREROLL) / s->unit_frames) + 1 : 0;
		int max = (int) sizeof(s->units);

		while (s->nunits < u_now && s->nunits < max)
			s->units[s->nunits++] = 0;

		int add = cw_units(text, s->units + s->nunits, max - s->nunits);

		if (add == 0 || s->nunits + add >= max)
			return false;

		s->nunits += add;
		s->done = false;
		return true;
	}

	g_msg[0].type = MSG_CW;
	snprintf(g_msg[0].text, sizeof(g_msg[0].text), "%s", text);

	return msg_start(rx, 0);
}

// One period of message audio (stereo); false when the message has ended
static bool msg_fill(sender_t *s, int16_t *buf)
{
	bool more = false;

	for (int i = 0; i < SEND_PERIOD; i++) {
		double v = 0.0;
		uint64_t f = s->written + (uint64_t) i;

		if (f < SEND_PREROLL) {
			more = true;
		} else if (g_msg[s->msg].type == MSG_CW) {
			int u = (int) ((double) (f - SEND_PREROLL) / s->unit_frames);
			double target = u < s->nunits ? s->units[u] : 0.0;

			// Smoothed keying: the envelope follows the key, about 5 ms from 0 to 95 %
			s->env += (target - s->env) * (1.0 - exp(-1.0 / (0.005 * SEND_RATE / 3.0)));
			v = sin(s->phase) * s->env * SEND_LEVEL;
			s->phase = fmod(s->phase + 2.0 * M_PI * s->tone / SEND_RATE, 2.0 * M_PI);
			more |= u < s->nunits || s->env > 0.001;
		} else if (s->wav != NULL && s->wav_left >= (uint32_t) (2 * s->wav_ch)) {
			int16_t smp[2] = { 0, 0 };

			if (fread(smp, 2, (size_t) s->wav_ch, s->wav) == (size_t) s->wav_ch) {
				v = (s->wav_ch == 2 ? (smp[0] + smp[1]) / 2.0 : smp[0]) / 32768.0;	/* the file's own level */
				s->wav_left -= (uint32_t) (2 * s->wav_ch);
				more = true;
			} else {
				s->wav_left = 0;
			}
		}

		int16_t o = (int16_t) lrint(v * 32767.0);

		buf[2 * i] = o;
		buf[2 * i + 1] = o;
	}

	return more;
}

//
// SWR scan (APP): the baseband's own scan carrier (register 12 swrscan 0x10, in AM, no
// PTT and no microphone), as R1CBU's x6100_gui keys it, stepped over the allocation the
// VFO is in (inside its band): SCAN_STEPS points, SCAN_STEP_MS each, the SWR read in the
// second half of each step. The curve stays on the scope until a key is pressed.
//

#define SCAN_STEPS	41
#define SCAN_STEP_MS	300
#define SCAN_SETTLE_MS	150
#define SCAN_PWR	50	/* 0.1 W units: 5 W, as x6100_gui sets it */

typedef struct
{
	bool shown;		/* curve on the scope */
	uint32_t lo, hi;
	float swr[SCAN_STEPS];
	int step;		/* step running; SCAN_STEPS when done */
	uint64_t step_at;	/* step started (ms) */
	double acc;
	int acc_n;
} scan_t;

static scan_t g_scan;

static uint32_t scan_hz(int i)
{
	return g_scan.lo + (uint32_t) ((uint64_t) (g_scan.hi - g_scan.lo) * (uint64_t) i / (SCAN_STEPS - 1));
}

static bool scan_start(rx_t *rx)
{
	uint32_t f = tx_hz(rx);

	if (rx->ptt || rx->tuning || rx->send != SEND_NONE || rx->bb_tx || rx->band < 0 || !tx_allowed(rx))
		return false;

	// The allocation around the VFO, in 1 kHz steps, inside the band
	g_scan.lo = g_scan.hi = f;

	while (g_scan.lo - 1000 >= g_bands[rx->band].low && in_allocation(g_scan.lo - 1000))
		g_scan.lo -= 1000;

	while (g_scan.hi + 1000 <= g_bands[rx->band].high && in_allocation(g_scan.hi + 1000))
		g_scan.hi += 1000;

	if (g_scan.hi - g_scan.lo < 10000) {
		rx->warn = "SWR SCAN: SEGMENT < 10 KHZ";
		return false;
	}

	for (int i = 0; i < SCAN_STEPS; i++)
		g_scan.swr[i] = 0.0f;

	g_scan.step = 0;
	g_scan.step_at = now_ms();
	g_scan.acc = 0.0;
	g_scan.acc_n = 0;
	g_scan.shown = true;
	rx->send = SEND_SCAN;
	rx->send_since = g_scan.step_at;
	rx->tx_hz_ovr = scan_hz(0);
	rx->tx_mode_ovr = X6100_MODE_AM;	/* the baseband's scan carrier, as x6100_gui sets it */
	rx->tx_pwr_ovr = SCAN_PWR;
	rx->warn = NULL;

	return true;
}

//
// Every pass of the main loop: feed the message audio, end it, repeat it; step the
// SWR scan. swr: the SWR from the last flow frame.
// Returns true when the screen changes.
//

static bool send_poll(rx_t *rx, uint64_t t, double swr)
{
	sender_t *s = &g_send;
	static int16_t buf[SEND_PERIOD * 2];

	// Stopped from outside (key, TOT, SWR, band edge): close the audio, no repeat
	if (rx->send != SEND_MSG && s->open) {
		s->repeat = false;
		send_close(s);
		return true;
	}

	if (rx->send == SEND_NONE && s->repeat && t >= s->next_at) {
		if (!msg_start(rx, s->msg))
			s->repeat = false;

		return true;
	}

	if (rx->send == SEND_MSG) {
		uint64_t played = t > s->start_ms ? (t - s->start_ms) * SEND_RATE / 1000 : 0;

		while (!s->done && s->written < played + 3 * SEND_PERIOD) {
			s->done = !msg_fill(s, buf);

			if (x6100_pcm_write(&s->pcm, buf, SEND_PERIOD) < 0) {
				tx_drop(rx, "AUDIO OUT FAILED");
				send_close(s);
				s->repeat = false;
				return true;
			}

			s->written += SEND_PERIOD;
		}

		// All of it played (and 50 ms more): un-key
		if (s->done && t >= s->start_ms + s->written * 1000 / SEND_RATE + 50) {
			tx_drop(rx, NULL);
			send_close(s);

			if (s->repeat)
				s->next_at = t + (uint64_t) g_msg_repeat * 1000;

			return true;
		}
	}

	if (rx->send == SEND_SCAN) {
		if (t - g_scan.step_at >= SCAN_SETTLE_MS) {	/* x6100_gui reads every packet, TX flag or not */
			g_scan.acc += swr;
			g_scan.acc_n++;
		}

		if (t - g_scan.step_at >= SCAN_STEP_MS) {
			g_scan.swr[g_scan.step] = g_scan.acc_n > 0 ? (float) (g_scan.acc / g_scan.acc_n) : 0.0f;
			g_scan.acc = 0.0;
			g_scan.acc_n = 0;
			g_scan.step_at = t;

			if (++g_scan.step >= SCAN_STEPS) {
				tx_drop(rx, NULL);
				return true;
			}

			rx->tx_hz_ovr = scan_hz(g_scan.step);
			return true;
		}
	}

	return false;
}

// Any key: stop what is being sent or repeated; true when something was stopped
static bool send_stop(rx_t *rx)
{
	if (rx->send == SEND_NONE && !g_send.repeat)
		return false;

	if (rx->send == SEND_SCAN)
		g_scan.step = SCAN_STEPS;	/* the curve so far stays */

	g_send.repeat = false;
	tx_drop(rx, rx->send != SEND_NONE ? "STOPPED" : NULL);
	return true;
}

//
// Span (F2 in CENTER / SCROLL-C): full IQ width down to 3 kHz; FIX / SCROLL-F use the edges
//

static const int g_spans[] = { 96000, 48000, 24000, 12000, 6000, 3000 };

#define SPAN_COUNT	((int) (sizeof(g_spans) / sizeof(g_spans[0])))

static void scope_span_next(rx_t *rx)
{
	int i = 0;

	while (i < SPAN_COUNT && g_spans[i] != rx->span)
		i++;

	rx->span = g_spans[(i + 1) % SPAN_COUNT];
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

// On the LW or MW broadcast band, in any mode: the FST channel steps 9 / 10 kHz there
// (2026-11-05: in USB on 1400 kHz 9 kHz was missing; before, AM only)
static bool lwmw(const rx_t *rx)
{
	return (rx->freq >= 148500 && rx->freq <= 283500) || (rx->freq >= 520000 && rx->freq <= 1710000);
}

//
// AM snaps to the broadcast raster: off the carrier the baseband AM detector gives a
// tone (2026-10-30, v0.13.0 too), so the mode keys put AM on the channel raster: the
// step becomes the raster (LW / MW 9 kHz, US MW 10 kHz in r2, else 5 kHz) and the dial
// the nearest channel; the VFO then stays on it (it steps on multiples of the step).
// FST still offers the fine steps for a station off the raster. Leaving AM by a mode
// key gives back the step from before, unless FST changed it meanwhile.
//

static int am_raster(const rx_t *rx)
{
	bool lw = rx->freq >= 148500 && rx->freq <= 283500;
	bool mw = rx->freq >= 520000 && rx->freq <= 1710000;

	if (mw && strcmp(g_regions[g_region].name, "r2") == 0)
		return 10000;

	return lw || mw ? 9000 : 5000;
}

static void am_mode(rx_t *rx, x6100_mode_t mode)
{
	bool was_am = rx->mode == X6100_MODE_AM;

	rx->mode = mode;

	if (!was_am && mode == X6100_MODE_AM) {
		int r = am_raster(rx);

		rx->step_pre_am = rx->step;
		rx->step = r;
		set_freq(rx, ((int64_t) rx->freq + r / 2) / r * r);
	} else if (was_am && mode != X6100_MODE_AM && rx->step_pre_am != 0) {
		if (rx->step == am_raster(rx))
			rx->step = rx->step_pre_am;

		rx->step_pre_am = 0;
	}
}

//
// 60 m channels: on 60 m, FST also offers CH, and the VFO knob then steps through
// the region's channel list (dial frequencies, USB).
//   r2: the US channels (FCC; national, outside the WRC-15 segment the TX check
//       of r2 refuses them unless oob).
//   r1 / r3 / nl: no official channels in the WRC-15 segment (5351.5-5366.5 kHz):
//       the segment edge for CW, the FT8 frequency and a 3 kHz raster for USB,
//       as a convenience. The band plan, not this list, says what goes where.
//   uk: the lower edge of each of the 11 UK segments (Ofcom; 2026-11-07).
//

static const uint32_t g_ch60_r2[] = { 5330500, 5346500, 5357000, 5371500, 5403500 };
static const uint32_t g_ch60_wrc[] = { 5351500, 5354000, 5357000, 5360000, 5363000 };
static const uint32_t g_ch60_uk[] = { 5258500, 5276000, 5288500, 5298000, 5313000, 5333000, 5354000, 5362000,
	5378000, 5395000, 5403500 };

static bool on_60m(const rx_t *rx)
{
	return rx->freq >= 5250000 && rx->freq <= 5450000;
}

static void ch60_step(rx_t *rx, int dir)
{
	const char *rn = g_regions[g_region].name;
	bool r2 = strcmp(rn, "r2") == 0, uk = strcmp(rn, "uk") == 0;
	const uint32_t *ch = r2 ? g_ch60_r2 : uk ? g_ch60_uk : g_ch60_wrc;
	int n = r2 ? (int) (sizeof(g_ch60_r2) / sizeof(g_ch60_r2[0])) : uk ? (int) (sizeof(g_ch60_uk) / sizeof(g_ch60_uk[0]))
		: (int) (sizeof(g_ch60_wrc) / sizeof(g_ch60_wrc[0]));
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

//
// Panels with rows (GEN, KEY, scope, TX, RIT / AGC, DSP, filter, edge; 2026-11-07, as a
// radio menu): turning MFK selects the row; push switches an on / off row at once, on
// any other row it starts editing (an amber frame: MFK changes the value) and the next
// push ends that. HOLD sets the default as before. g_row_edit points at the edited
// panel's row index; the edit ends when that panel closes or another one opens.
//

static int *g_row_edit;
static bool gen_toggle(int row);
static int sql_from_noise(void);	/* SQL push: the squelch just above the noise */

// The open panel with rows: its row index, the number of rows, its idle timer; NULL none
static int *row_sel(rx_t *rx, int *n, uint64_t **since)
{
	uint64_t *dummy;
	uint64_t **s = since != NULL ? since : &dummy;

#define ROW_PANEL(open, par, tm, rows) \
	if (rx->open) { \
		*n = (rows); \
		*s = &rx->tm; \
		return &rx->par; \
	}

	ROW_PANEL(gedit, gedit_par, gedit_since, GEN_ROWS)
	ROW_PANEL(kedit, kedit_par, kedit_since, KPARS)
	ROW_PANEL(sedit, sedit_par, sedit_since, SPARS)
	ROW_PANEL(xedit, xedit_par, xedit_since, XPARS)
	ROW_PANEL(tedit, tedit_par, tedit_since, TPARS)
	ROW_PANEL(eedit, eedit_par, eedit_since, 2)
	ROW_PANEL(fedit, fedit_par, fedit_since, 3)
	ROW_PANEL(dedit, dedit_par, dedit_since, DSP_PARS)
#undef ROW_PANEL

	return NULL;
}

// Is the selected row being edited? Forgets an edit whose panel has closed.
static bool row_editing(const rx_t *rx)
{
	int n;
	int *sel = row_sel((rx_t *) rx, &n, NULL);

	if (g_row_edit != NULL && g_row_edit != sel)
		g_row_edit = NULL;

	return g_row_edit != NULL;
}

// Can row i of the open panel be selected (the filter panel: AM has no SFT, FM BW only)
static bool row_valid(const rx_t *rx, int i)
{
	if (!rx->fedit || rx->gedit || rx->kedit || rx->sedit || rx->xedit || rx->tedit || rx->eedit)
		return true;

	fgroup_t g = mode_group(rx->mode);

	return g == FG_FM ? i == 0 : g == FG_AM ? i != 1 : true;
}

// MFK turn while not editing: the next / previous row (round)
static void row_move(rx_t *rx, int *sel, int n, int dir)
{
	for (int k = 0; k < n; k++) {
		*sel = (*sel + dir + n) % n;

		if (row_valid(rx, *sel))
			break;
	}
}

// A row of two values (min, max one step apart): the other one
static bool kpar_flip(const kpar_t *k, int *p)
{
	if (k->max - k->min != k->step)
		return false;

	*p = *p == k->max ? k->min : k->max;
	return true;
}

// MFK push on an on / off row: switch it; false when the row has more values
static bool row_toggle(rx_t *rx)
{
	if (rx->gedit)
		return gen_toggle(rx->gedit_par);

	if (rx->kedit)
		return kpar_flip(&g_kpar[rx->kedit_par], kpar_value(rx, rx->kedit_par));

	if (rx->xedit)
		return kpar_flip(&g_xpar[rx->xedit_par], xpar_value(rx, rx->xedit_par));

	if (rx->sedit) {
		const kpar_t *k = &g_spar[rx->sedit_par];
		int v = spar_get(rx, rx->sedit_par);

		if (!kpar_flip(k, &v))
			return false;

		spar_set(rx, rx->sedit_par, v);
		return true;
	}

	if (rx->tedit) {
		if (rx->tedit_par == 1)
			rx->rit = !rx->rit;
		else if (rx->tedit_par == 2)
			rx->xit = !rx->xit;
		else if (rx->tedit_par == 8)
			g_agc_hang = !g_agc_hang;
		else
			return false;

		return true;
	}

	return false;
}

// MFK push in a panel with rows: end the edit, switch an on / off row, or start editing
static bool row_push(rx_t *rx)
{
	int n;
	int *sel = row_sel(rx, &n, NULL);

	if (sel == NULL)
		return false;

	if (row_editing(rx))
		g_row_edit = NULL;
	else if (!row_toggle(rx))
		g_row_edit = sel;

	return true;
}

// The edit frame around a selected row being edited
static void row_mark(x6100_lcd_t *lcd, int x, int y, int w, int h)
{
	const uint32_t amber = 0xFFB020;

	x6100_lcd_hline(lcd, x, x + w - 1, y, amber);
	x6100_lcd_hline(lcd, x, x + w - 1, y + h - 1, amber);
	x6100_lcd_vline(lcd, x, y, y + h - 1, amber);
	x6100_lcd_vline(lcd, x + w - 1, y, y + h - 1, amber);
	x6100_lcd_hline(lcd, x + 1, x + w - 2, y + 1, amber);
	x6100_lcd_hline(lcd, x + 1, x + w - 2, y + h - 2, amber);
}

static bool on_key(rx_t *rx, int key)
{
	// Baseband not responding: MFK push resets it (2026-11-07; started at boot there is
	// no console for bbreset), locked or not
	if (g_bb_dead && key == X6100_KEY_MFK_PUSH) {
		g_bb_reset_req = true;
		return true;
	}

	if (bscan_running()) {
		g_bscan_req = -1;	/* any key stops a birdie scan, and does nothing else */
		return true;
	}

	// After a TX timeout on hand mic / VOX: a key ends the TX block, and does nothing else
	if (rx->tot_lock) {
		rx->tot_lock = false;
		rx->warn = NULL;
		return true;
	}

	// Any key stops a message (and its repeat) or the SWR scan, then clears the scan
	// curve; it does nothing else
	if (send_stop(rx))
		return true;

	if (g_scan.shown) {
		g_scan.shown = false;
		return true;
	}

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
	// GEN settings panel open: MFK push switches or edits the row (row_push); GEN closes it,
	// any other key closes it and does its normal job. The status page closes on any key.
	//
	if (rx->gedit) {
		rx->gedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			return row_push(rx);
		}

		if (key != X6100_KEY_GEN)
			rx->gedit = false;
	}

	//
	// Keyer panel open: MFK push switches or edits the row (row_push); any other key closes it
	// and does its normal job (KEY long closes it too)
	//
	if (rx->kedit) {
		rx->kedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			return row_push(rx);
		}

		rx->kedit = false;
	}

	//
	// Message panel open (MSG): MFK push sends the selected message; MSG closes it, any
	// other key closes it and does its normal job
	//
	if (rx->msedit) {
		rx->msedit = false;

		if (key == X6100_KEY_MFK_PUSH) {
			msg_start(rx, rx->msedit_par + 1);
			return true;
		}

		if (key == X6100_KEY_MSG)
			return true;
	}

	//
	// App panel open (APP): MFK push starts the selected app; APP closes it, any other key
	// closes it and does its normal job
	//
	if (rx->apedit) {
		rx->apedit = false;

		if (key == X6100_KEY_MFK_PUSH) {
			scan_start(rx);		/* the only app so far */
			return true;
		}

		if (key == X6100_KEY_APP)
			return true;
	}

	//
	// Scope panel open: MFK push switches or edits the row (row_push); any other key closes it and
	// does its normal job (F1 long closes it too)
	//
	if (rx->sedit) {
		rx->sedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			return row_push(rx);
		}

		rx->sedit = false;
	}

	//
	// TX panel open: MFK push switches or edits the row (row_push); any other key closes it and
	// does its normal job (F4 long closes it too)
	//
	if (rx->xedit) {
		rx->xedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			return row_push(rx);
		}

		rx->xedit = false;
	}

	//
	// Memory panel open: MFK push clears the channel (asks first: a second push within
	// 3 s); V/M closes it (below), any other key closes it and does its normal job
	//
	if (rx->medit) {
		rx->medit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			if (!g_mem[rx->medit_ch].used) {
				return true;
			} else if (rx->mclear_at != 0 && now_ms() - rx->mclear_at < 3000) {
				mem_clear(rx->medit_ch);
				rx->mclear_at = 0;
				toast("CHANNEL CLEARED");
			} else {
				rx->mclear_at = now_ms();
				toast("PUSH AGAIN: CLEAR");
			}

			return true;
		}

		if (key != X6100_KEY_VM)
			rx->medit = false;
	}

	//
	// Edge panel open: MFK push edits the lower or upper edge (row_push), F2 the next
	// edge (the panel stays); any other key closes it and does its normal job
	//
	if (rx->eedit) {
		rx->eedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH)
			return row_push(rx);

		if (key != X6100_KEY_F2)
			rx->eedit = false;
	}

	//
	// RIT / XIT panel open: MFK push switches or edits the row (row_push); any other key
	// closes it and does its normal job (AGC long closes it too)
	//
	if (rx->tedit) {
		rx->tedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH)
			return row_push(rx);

		rx->tedit = false;
	}

	if (rx->ginfo) {
		rx->ginfo = false;

		if (key == X6100_KEY_GEN)
			return true;
	}

	//
	// Filter panel open: F5 cycles FIL1-3, MFK turn picks BW / SFT / PBT (AM no SFT, FM
	// BW only: row_valid), push edits it (row_push); any other key closes the panel
	// (edits are kept) and then does its normal job
	//
	if (rx->fedit) {
		rx->fedit_since = now_ms();

		if (key == X6100_KEY_F5) {
			rx->fil = (rx->fil + 1) % 3;
			return true;
		}

		if (key == X6100_KEY_MFK_PUSH)
			return row_push(rx);

		rx->fedit = false;
		fil_save();
	}

	//
	// DSP panel open: MFK push switches or edits the row (row_push); DFN / DFL keep switching
	// NR, NB and notch; any other key closes the panel and does its normal job
	//
	if (rx->dedit) {
		rx->dedit_since = now_ms();

		if (key == X6100_KEY_MFK_PUSH) {
			return row_push(rx);
		}

		if (key != X6100_KEY_DFN && key != X6100_KEY_DFL)
			rx->dedit = false;
	}

	if (rx->lock && key != X6100_KEY_LOCK && key != X6100_KEY_VOL_PUSH)	/* mute works locked, as the volume */
		return false;

	switch (key) {
	case X6100_KEY_LOCK:
		rx->lock = !rx->lock;
		g_lock_auto = false;	/* a lock by hand stays, also in landscape */
		break;
	case X6100_KEY_KEY:	/* short: key type straight / paddle / paddle reversed; long: keyer panel */
		rx->keymode = (rx->keymode + 1) % 3;
		toast(rx->keymode == 0 ? "STRAIGHT KEY" : rx->keymode == 1 ? "PADDLE" : "PADDLE REVERSED");
		break;
	case X6100_KEY_VM:	/* short: memory mode with its panel / back to the VFO; long: split */
		rx->medit_ch = rx->medit_ch < 1 ? 1 : rx->medit_ch;
		rx->medit_since = now_ms();
		rx->mclear_at = 0;

		if (rx->mem_mode) {
			mem_mode_off(rx);
			rx->medit = false;
			toast("VFO");
		} else {
			rx->medit = true;	/* with no channel stored: the panel, to write one */

			if (!mem_mode_on(rx, rx->medit_ch))
				toast("NO MEMORIES");
		}
		break;
	case X6100_KEY_AB:	/* short: VFO A <-> B; long: A=B (the other VFO takes this one) */
		vfo_swap(rx);
		break;
	case X6100_KEY_MSG:	/* short: message panel; long: repeat the selected message */
		rx->msedit = !rx->msedit;
		rx->msedit_since = now_ms();
		break;
	case X6100_KEY_APP:	/* short: app panel; long: SWR scan */
		rx->apedit = !rx->apedit;
		rx->apedit_since = now_ms();
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
		// The last step everywhere 100 kHz, to move across a band fast (2026-11-04)
		if (lwmw(rx))	/* LW / MW, any mode: channel steps 9 kHz (EU) and 10 kHz (US) */
			rx->step = rx->step == 100 ? 1000 : rx->step == 1000 ? 5000 : rx->step == 5000 ? 9000
				: rx->step == 9000 ? 10000 : rx->step == 10000 ? 100000 : 100;
		else if (on_60m(rx))	/* 60 m: CH steps through the channel list */
			rx->step = rx->step == 10 ? 100 : rx->step == 100 ? 1000 : rx->step == 1000 ? 5000
				: rx->step == 5000 ? STEP_CH : rx->step == STEP_CH ? 10000 : rx->step == 10000 ? 100000 : 10;
		else
			rx->step = rx->step == 10 ? 100 : rx->step == 100 ? 1000 : rx->step == 1000 ? 5000
				: rx->step == 5000 ? 10000 : rx->step == 10000 ? 100000 : 10;
		break;
	case X6100_KEY_BAND_UP:
		band_step(rx, +1);
		break;
	case X6100_KEY_BAND_DOWN:
		band_step(rx, -1);
		break;
	case X6100_KEY_SSB:
		am_mode(rx, rx->mode == X6100_MODE_USB ? X6100_MODE_LSB : X6100_MODE_USB);
		break;
	case X6100_KEY_CW:
		am_mode(rx, rx->mode == X6100_MODE_CW ? X6100_MODE_CWR : X6100_MODE_CW);
		break;
	case X6100_KEY_AMFM:
		am_mode(rx, rx->mode == X6100_MODE_AM ? X6100_MODE_NFM : X6100_MODE_AM);
		break;
	case X6100_KEY_PRE:
		rx->att = !rx->att;	/* short: ATT, the one used more (2026-11-04; was PRE) */
		break;
	case X6100_KEY_AGC:
		rx->agc = (x6100_agc_t) ((rx->agc + 1) % 4);
		break;
	case X6100_KEY_VOL_PUSH:
		// Knob on SQL (VOL long): the squelch just above the noise now (2026-11-07), and
		// the knob stays on SQL to fine-tune; else mute
		if (g_vol_fn == 1) {
			char msg[24];	/* toast() copies it */

			rx->sql = sql_from_noise();
			g_vol_fn_ms = now_ms();
			snprintf(msg, sizeof(msg), "SQL %d", rx->sql);
			toast(msg);
		} else {
			rx->mute = !rx->mute;
		}
		break;
	case X6100_KEY_ATU:
		rx->atu_on = !rx->atu_on;

		// Which tuner memory goes in (apply: the one nearest the TX frequency)
		if (rx->atu_on) {
			const atum_t *am = atu_find(tx_hz(rx));
			char s[40];

			if (am != NULL) {
				snprintf(s, sizeof(s), "ATU %u L %u C %u S %u", (unsigned) (am->hz / 1000),
					am->net.l, am->net.c, am->net.s);
				toast(s);
			} else {
				toast("ATU ON: NO MEMORY");
			}
		}
		break;
	case X6100_KEY_F1:
		// Short: the next scope mode, CENTER -> SCROLL-C -> FIX -> SCROLL-F -> CENTER (long:
		// scope panel); into FIX the window goes to the edge in use
		spar_set(rx, SP_MODE, ((int) rx->scope + 1) % 4);
		break;
	case X6100_KEY_F3:	/* short: audio scope on / off; long: RF + audio side by side */
		rx->ascope = rx->asplit ? false : !rx->ascope;
		rx->asplit = false;
		break;
	case X6100_KEY_F2:
		if (rx->ascope) {
			rx->arange = !rx->arange;	/* audio scope: 0-3 / 0-6 kHz */
		} else if (scope_fixed(rx)) {		/* FIX / SCROLL-F: the next edge, EDGE 1-4 */
			char s[24];

			rx->edge = rx->edge % 4 + 1;
			scope_fix_start(rx);
			snprintf(s, sizeof(s), "EDGE %d %u-%u", rx->edge, (unsigned) (rx->fix_lo / 1000) % 100000u,
				(unsigned) ((rx->fix_lo + rx->fix_w) / 1000) % 100000u);
			toast(s);
		} else {
			scope_span_next(rx);
		}
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
		|| key == X6100_KEY_F1 || key == X6100_KEY_F2 || key == X6100_KEY_F3 || key == X6100_KEY_F4 || key == X6100_KEY_F5 || key == X6100_KEY_DFN || key == X6100_KEY_DFL
		|| key == X6100_KEY_GEN || key == X6100_KEY_AB
		|| key == X6100_KEY_VM || key == X6100_KEY_AGC || key == X6100_KEY_KEY || key == X6100_KEY_MSG
		|| key == X6100_KEY_APP || key == X6100_KEY_VOL_PUSH || key == X6100_KEY_LOCK;	/* LOCK long: screenshot */
}

static bool on_long_key(rx_t *rx, int key)
{
	if (bscan_running()) {
		g_bscan_req = -1;	/* any key stops a birdie scan */
		return true;
	}

	if (rx->tot_lock) {	/* TX timeout on hand mic / VOX: the key ends the TX block */
		rx->tot_lock = false;
		rx->warn = NULL;
		return true;
	}

	if (send_stop(rx))	/* a message or the SWR scan */
		return true;

	// LOCK long: a screenshot (2026-11-07), locked or not; LOCK short still locks
	if (key == X6100_KEY_LOCK) {
		g_shot_req = true;
		return true;
	}

	// VOL long: the VOL knob to VOL / SQL / RFG (short: mute). Locked, VOL / SQL only: the
	// squelch stays free with the volume (2026-11-05), the RF gain stays locked
	if (key == X6100_KEY_VOL_PUSH) {
		g_vol_fn = rx->lock ? (g_vol_fn == 1 ? 0 : 1) : (g_vol_fn + 1) % 3;
		g_vol_fn_ms = now_ms();
		toast(g_vol_fn == 0 ? "KNOB: VOLUME" : g_vol_fn == 1 ? "KNOB: SQUELCH" : "KNOB: RF GAIN");
		return true;
	}

	if (rx->lock)
		return false;

	switch (key) {
	case X6100_KEY_PRE:
		rx->pre = !rx->pre;	/* long: PRE */
		return true;
	case X6100_KEY_F1:
		// Long: scope panel (mode, VBW, averaging, waveform, colours, waterfall)
		rx->sedit = !rx->sedit;
		rx->sedit_since = now_ms();
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
		if (rx->kedit) {
			*kpar_value(rx, rx->kedit_par) = g_kpar[rx->kedit_par].def;
			rx->kedit_since = now_ms();
			return true;
		}

		if (rx->xedit) {
			*xpar_value(rx, rx->xedit_par) = g_xpar[rx->xedit_par].def;
			rx->xedit_since = now_ms();
			return true;
		}

		if (rx->sedit) {
			spar_set(rx, rx->sedit_par, g_spar[rx->sedit_par].def);
			rx->sedit_since = now_ms();
			return true;
		}

		if (rx->medit) {
			mem_write_vfo(rx, rx->medit_ch);	/* in memory mode: the VFO it came from */
			mem_mode_on(rx, rx->medit_ch);		/* and on it, as written */
			toast("WRITTEN TO MEMORY");
			rx->medit_since = now_ms();
			return true;
		}

		if (rx->eedit) {	/* the edge back to its default */
			int r = edge_range(rx->freq), e = clampi(rx->edge, 1, 4) - 1;

			if (r >= 0) {
				edge_default(r, e, &g_edge[r][e]);
				scope_fix_start(rx);
			}

			rx->eedit_since = now_ms();
			return true;
		}

		if (rx->tedit) {
			if (rx->tedit_par == 3)
				rx->dup = DUP_AUTO;
			else if (rx->tedit_par == 4)
				rx->dup_khz = DUP_KHZ_DEF;
			else if (rx->tedit_par == 6)
				g_agc_slow = AGC_SLOW_DEF;
			else if (rx->tedit_par == 7)
				g_agc_fast = AGC_FAST_DEF;
			else if (rx->tedit_par == 8)
				g_agc_hang = 0;
			else if (rx->tedit_par == 5)
				g_am_ofs = AM_OFS_DEF;
			else
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
			if (rx->fedit_par == 2) {	/* PBT box: PBT CLR, as holding an Icom's PBT knob */
				rx->pbt_in = 0;
				rx->pbt_out = 0;
			} else {
				fil_default(mode_group(rx->mode), rx->fil);
			}

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
		// Long in FIX / SCROLL-F (RF scope): the edge panel, lower / upper of the edge in
		// use (F2 long again closes it); otherwise the reference level edit
		if (!rx->redit && !rx->ascope && scope_fixed(rx)) {
			if (edge_range(rx->freq) < 0) {
				toast("NO EDGE HERE");
				return true;
			}

			rx->eedit = !rx->eedit;
			rx->eedit_par = 0;
			rx->eedit_since = now_ms();
			scope_fix_start(rx);	/* the edge itself, not a SCROLL-F page */
			return true;
		}

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
	case X6100_KEY_KEY:
		rx->kedit = !rx->kedit;
		rx->kedit_since = now_ms();
		return true;
	case X6100_KEY_F4:	/* long: TX panel (mic, compressor, VOX, monitor, squelch) */
		rx->xedit = !rx->xedit;
		rx->xedit_since = now_ms();
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
	case X6100_KEY_MSG:	/* long: send the message selected in the panel, again every msgrepeat s */
		rx->msedit = false;
		g_send.repeat = msg_start(rx, rx->msedit_par + 1);
		return true;
	case X6100_KEY_APP:	/* long: SWR scan */
		rx->apedit = false;
		scan_start(rx);
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
		if (rx->tot_lock) {	/* the PTT key ends the TX block of a hand mic / VOX timeout */
			rx->tot_lock = false;
			rx->warn = NULL;
			return true;
		}

		if (send_stop(rx))	/* the PTT key stops a message or the SWR scan */
			return true;

		if (rx->ptt_latch || rx->tuning) {
			tx_drop(rx, NULL);
			return true;
		}

		if (!tx_allowed(rx))
			return true;

		rx->ptt = true;
		rx->ptt_cat = false;	/* the PTT key: LAN takes the mic */
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
	g_last_turn_ms = now_ms();	/* the portrait auto lock counts from the last key or turn */

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

//
// The step multiplier for a VFO turn of value detents: 1, or 2, 5, 10 by the dial speed
// (detents within the last VFO_FAST_WIN_MS, this one included). A turn back clears it.
//

static int vfo_mult(int value, uint32_t step)
{
	uint64_t t = now_ms();
	int dir = value > 0 ? 1 : -1, n = value > 0 ? value : -value, in = 0, rate;

	if (dir != g_vfo_dir)
		g_vfo_n = 0;

	g_vfo_dir = dir;

	for (int i = 0; i < n && i < 64; i++)
		g_vfo_t[g_vfo_n++ % 64] = t;

	for (unsigned i = 0; i < g_vfo_n && i < 64; i++)
		in += t - g_vfo_t[(g_vfo_n - 1 - i) % 64] < VFO_FAST_WIN_MS ? 1 : 0;

	rate = in * 1000 / VFO_FAST_WIN_MS;

	if (!g_vfo_fast || step > VFO_FAST_STEP)
		return 1;

	return rate >= VFO_FAST_X10 ? 10 : rate >= VFO_FAST_X5 ? 5 : rate >= VFO_FAST_X2 ? 2 : 1;
}

static bool on_knob(rx_t *rx, input_t in, int value)
{
	// The lock holds the VFO and MFK; the volume always works (2026-11-05, for the
	// portrait auto lock; before, LOCK held the volume too)
	if (rx->lock && in != IN_VOL)
		return false;

	if (!rx->lock)
		g_last_turn_ms = now_ms();

	if (bscan_running() && in != IN_VOL)
		return true;	/* the scan owns the VFO; volume still works */

	switch (in) {
	case IN_VFO: {
		int mult = vfo_mult(value, rx->step == STEP_CH ? STEP_CH : (uint32_t) rx->step);	/* faster when turned fast */
		int64_t f = rx->step == STEP_CH ? (int64_t) rx->freq - rx->freq % 100 + (int64_t) value * X6100_DIR_VFO * 100
			: (int64_t) rx->freq - rx->freq % (uint32_t) rx->step + (int64_t) value * mult * X6100_DIR_VFO * rx->step;

		// Past an end of the tuning range the dial goes round to the other end (2026-11-05;
		// it stopped there). Only from the end itself: a turn that overshoots lands on it
		if (f > (int64_t) FREQ_MAX)
			f = rx->freq >= FREQ_MAX ? (int64_t) FREQ_MIN : (int64_t) FREQ_MAX;
		else if (f < (int64_t) FREQ_MIN)
			f = rx->freq <= FREQ_MIN ? (int64_t) FREQ_MAX : (int64_t) FREQ_MIN;

		set_freq(rx, f);
		return true;
	}
	case IN_VOL:
		// VOL long chose squelch or RF gain; locked, volume and squelch only (RF gain locked)
		if (g_vol_fn == 1)
			rx->sql = clampi(rx->sql + value * X6100_DIR_VOL, 0, 100);
		else if (g_vol_fn == 2 && !rx->lock)
			rx->rfg = clampi(rx->rfg + value * X6100_DIR_VOL * 2, 0, 100);
		else
			rx->vol = clampi(rx->vol + value * X6100_DIR_VOL, 0, 100);

		g_vol_fn_ms = now_ms();
		return true;
	case IN_MFK:
		// A panel with rows, not editing: MFK selects the row (row_push starts the edit)
		{
			int n;
			uint64_t *since;
			int *sel = row_sel(rx, &n, &since);

			if (sel != NULL && !row_editing(rx)) {
				row_move(rx, sel, n, value * X6100_DIR_MFK > 0 ? 1 : -1);
				*since = now_ms();
				return true;
			}
		}

		if (rx->kedit) {
			const kpar_t *k = &g_kpar[rx->kedit_par];
			int *p = kpar_value(rx, rx->kedit_par);

			*p = clampi(*p + value * X6100_DIR_MFK * k->step, k->min, k->max);
			rx->kedit_since = now_ms();
			return true;
		}

		if (rx->xedit) {
			const kpar_t *k = &g_xpar[rx->xedit_par];
			int *p = xpar_value(rx, rx->xedit_par);

			*p = clampi(*p + value * X6100_DIR_MFK * k->step, k->min, k->max);
			rx->xedit_since = now_ms();
			return true;
		}

		if (rx->sedit) {
			const kpar_t *k = &g_spar[rx->sedit_par];

			spar_set(rx, rx->sedit_par,
				clampi(spar_get(rx, rx->sedit_par) + value * X6100_DIR_MFK * k->step, k->min, k->max));
			rx->sedit_since = now_ms();
			return true;
		}

		if (rx->msedit) {
			rx->msedit_par = (rx->msedit_par + (value * X6100_DIR_MFK > 0 ? 1 : MSG_COUNT - 1)) % MSG_COUNT;
			rx->msedit_since = now_ms();
			return true;
		}

		if (rx->apedit) {
			rx->apedit_since = now_ms();
			return true;
		}

		if (rx->medit) {
			rx->medit_ch = (rx->medit_ch - 1 + (value * X6100_DIR_MFK > 0 ? 1 : MEM_COUNT - 1)) % MEM_COUNT + 1;

			if (g_mem[rx->medit_ch].used)	/* listen while scrolling; an empty channel is passed over */
				mem_mode_on(rx, rx->medit_ch);
			rx->medit_since = now_ms();
			rx->mclear_at = 0;
			return true;
		}

		if (rx->eedit) {
			edge_adjust(rx, value * X6100_DIR_MFK);
			rx->eedit_since = now_ms();
			return true;
		}

		if (rx->tedit) {
			int dir = value * X6100_DIR_MFK > 0 ? 1 : -1;

			if (rx->tedit_par == 0)
				rx->rit_hz = clampi(rx->rit_hz + value * X6100_DIR_MFK * 10, -RIT_MAX_HZ, RIT_MAX_HZ);
			else if (rx->tedit_par == 1)
				rx->rit = dir > 0;
			else if (rx->tedit_par == 2)
				rx->xit = dir > 0;
			else if (rx->tedit_par == 3)
				rx->dup = (rx->dup + dir + DUP_COUNT) % DUP_COUNT;
			else if (rx->tedit_par == 4)
				rx->dup_khz = clampi(rx->dup_khz + value * X6100_DIR_MFK * 10, 10, DUP_KHZ_MAX);
			else if (rx->tedit_par == 5)
				g_am_ofs = clampi(g_am_ofs + value * X6100_DIR_MFK * 10, 0, AM_OFS_MAX);
			else if (rx->tedit_par == 6)		/* 10 ms steps, 50 ms from 1 s */
				g_agc_slow = clampi(g_agc_slow + value * X6100_DIR_MFK * (g_agc_slow >= 100 ? 5 : 1), AGC_SLOW_MIN,
					AGC_SLOW_MAX);
			else if (rx->tedit_par == 7)
				g_agc_fast = clampi(g_agc_fast + value * X6100_DIR_MFK, AGC_FAST_MIN, AGC_FAST_MAX);
			else
				g_agc_hang = dir > 0;

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

			if (rx->fedit_par == 0) {
				fil_bw(g, rx->fil, value * X6100_DIR_MFK);
			} else if (rx->fedit_par == 1) {
				fil_sft(g, rx->fil, value * X6100_DIR_MFK);
			} else if (g != FG_FM) {
				// PBT box: both together, 50 Hz a step (the passband shifts, the
				// difference between inner and outer stays)
				int d = value * X6100_DIR_MFK * PBT_STEP_HZ;

				rx->pbt_in = clampi(rx->pbt_in + d, -PBT_MAX_HZ, PBT_MAX_HZ);
				rx->pbt_out = clampi(rx->pbt_out + d, -PBT_MAX_HZ, PBT_MAX_HZ);
			}

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
#define BAND_OUT	0xD03030	/* scope: outside the TX allocations, band edges */

//
// Scope: max hold decay and waterfall scale (colours, averaging: scope panel)
//
#define PEAK_DECAY_DB	0.3f
#define WF_RANGE_DB	((float) g_sp[SP_WF_LEVEL])	/* scope panel WF PEAK LEVEL */
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
static int g_vu_x = 500, g_vu_y = 4;	/* arc meter origin: landscape top right, portrait top centre */
#define VU_X		g_vu_x
#define VU_Y		g_vu_y
#define OVF_POWER	0.178f	/* |IQ|^2 at -7.5 dB: flow IQ clips at about -6.3 dB */
#define VU_W		296
#define VU_H		168
#define AS_N		2048		/* audio scope FFT: 23.4 Hz bins at 48 kHz */
#define AS_RATE		48000
#define AS_DB_MIN	-110.0f	/* dBFS */
#define AS_DB_MAX	-10.0f
#define AS_RANGE_DB	80.0f		/* normalised window height */
#define AS_FLOOR_DB	10.0f		/* window bottom under the audio floor */
static int g_spec_y = 180;	/* RF / audio scope top: under the landscape top, lower in portrait */
#define SPEC_Y		g_spec_y
static int g_spec_h = 150;	/* RF / audio scope height: the scope panel WATERFALL SIZE */
#define SPEC_H		g_spec_h
#define WF_Y		(SPEC_Y + SPEC_H + 4)
static int g_tag_h = 20;	/* F1-F5 tags along the bottom edge; in portrait the footer */
#define TAG_H		g_tag_h
#define WF_H(lcd)	((lcd)->h - WF_Y - TAG_H)

//
// Portrait layout (GEN row LAYOUT, conf token layout landscape | portrait; the radio
// held upright, its left side up, as a handheld with a telescopic antenna): a red TX
// bar, the arc meter, the frequency, mode / filter / step, a small scope and waterfall,
// battery, the lock. Panels open in landscape: while one is open the screen turns back.
// The panel is natively portrait (480 x 800), so this is only another rotation.
//

#define P_BAR_H		34		/* status line, red while transmitting */
#define P_VU_Y		40		/* arc meter */
#define P_FREQ_Y	222		/* frequency, the 73 px font */
#define P_INFO_Y	306		/* mode, filter, step */
#define P_NOTE_Y	342		/* AGC, ATT / PRE, NB / NR, split / duplex */
#define P_SPEC_Y	372
#define P_SPEC_H	110
#define P_FOOT_H	40		/* lock and volume */

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
#define SPAN_HZ		100000	/* the IQ width: 512 bins of 195.3 Hz (2026-11-03; was 96000, 4 % off: with the
				   IF shift a carrier was drawn shift x 4 % short of the dial; stock: FFT SPAN 100k) */
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
	double vext;		/* external supply, V */
	int batcap;		/* battery charge, % */
	bool ext;		/* external supply present (flow flag b3): charging when below 100 % */

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

	static float dbs[X6100_FLOW_SAMPLES];

	for (int i = 0; i < n; i++) {
		int k = (i + n / 2) % n;
		float p = buf[k][0] * buf[k][0] + buf[k][1] * buf[k][1];

		dbs[i] = p > 1e-20f ? 10.0f * log10f(p / (float) n) : -200.0f;
		scr->pow[i] = p / (float) n;
	}

	if (g_scope_hold)
		return;	/* HOLD: the picture stays, the S-meter (pow) goes on */

	// IF shift on: the receiver's own DC / LO spike sits at the IQ centre, away from the
	// passband; drawn as the line between its neighbours (the scope only, not the meter)
	if (g_dc_hide) {
		const int a = n / 2 - DC_HIDE_BINS - 1, b = n / 2 + DC_HIDE_BINS + 1;

		for (int i = a + 1; i < b; i++)
			dbs[i] = dbs[a] + (dbs[b] - dbs[a]) * (float) (i - a) / (float) (b - a);
	}

	//
	// Scope panel: VBW narrow smooths over neighbouring bins (1-2-1), averaging over
	// frames (OFF = 1: none, 2-4: each frame weighs 1/n)
	//
	const float avg = 1.0f / (float) g_sp[SP_AVG];

	//
	// Max hold: DECAY falls slowly from each peak; 10 S holds the maximum and starts again
	// every 10 s, ON holds it; both start again when the IQ centre moves (tuning)
	//
	static uint64_t hold_t;
	static uint32_t hold_hz;
	static int hold_mode = -1;
	uint64_t t = now_ms();
	bool restart = !scr->spec_valid || g_sp[SP_HOLD] != hold_mode || g_iq_hz != hold_hz
		|| (g_sp[SP_HOLD] == HOLD_10S && t - hold_t >= 10000);

	if (restart) {
		hold_t = t;
		hold_hz = g_iq_hz;
		hold_mode = g_sp[SP_HOLD];
	}

	for (int i = 0; i < n; i++) {
		float db = dbs[i];

		if (g_sp[SP_VBW] == 0 && i > 0 && i < n - 1)
			db = (dbs[i - 1] + 2.0f * dbs[i] + dbs[i + 1]) / 4.0f;

		scr->spec[i] = scr->spec_valid ? scr->spec[i] * (1.0f - avg) + db * avg : db;

		if (restart)
			scr->peak[i] = scr->spec[i];
		else if (g_sp[SP_HOLD] == HOLD_DECAY)
			scr->peak[i] = scr->peak[i] - PEAK_DECAY_DB > scr->spec[i] ? scr->peak[i] - PEAK_DECAY_DB : scr->spec[i];
		else
			scr->peak[i] = scr->peak[i] > scr->spec[i] ? scr->peak[i] : scr->spec[i];
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

//
// Birdie scan: windows BS_STEP_HZ apart over each band, so every frequency is in two
// of them and the centre bins (DC, left out) of one are covered by the next. Per
// window: BS_SETTLE_MS after the VFO moves, then BS_FRAMES flow frames averaged in
// power; a bin is a birdie when it is a local peak at least BS_SNR_DB over the
// median of the window and narrow (BS_WIDE_BINS away it is 6 dB down); its frequency
// is interpolated between the bins (parabola on dB, Hann window: ~10 Hz at 20 dB).
// TX is refused while it runs; the tuner is out (straight through), and the VFO,
// band, RIT and tuner come back at the end.
//

#define BS_STEP_HZ	36000
#define BS_EDGE_BINS	200		/* usable: centre +- 200 bins, +- 39 kHz (195.3 Hz bins) */
#define BS_DC_BINS	4		/* centre +- 4 bins left out */
#define BS_SETTLE_MS	300
#define BS_FRAMES	32
#define BS_SNR_DB	16		/* 2026-11-07: two scans agree on all from 16 dB; 10-15 dB came and went (noise) */
#define BS_WIDE_BINS	3

typedef struct
{
	bool on;
	bool all;
	int band;			/* band being scanned */
	int last;			/* last band to scan */
	uint32_t hz;			/* window centre */
	uint64_t settle_until;
	int frames;
	int found;			/* birdies found in this scan */
	int steps, step;		/* windows in this scan, done so far */
	double acc[X6100_FLOW_SAMPLES];	/* summed power, frequency order (low to high) */
	uint32_t ret_freq;
	int ret_band;
	bool ret_rit;
	bool ret_atu;
} bscan_t;

static bscan_t g_bs;

static bool bscan_running(void)
{
	return g_bs.on;
}
static bool g_bscan_all;		/* GEN BIRDIE SCAN: this band or all bands */

static int bscan_windows(int band)
{
	return (int) ((g_bands[band].high - g_bands[band].low + BS_STEP_HZ - 1) / BS_STEP_HZ) + 1;	/* low .. first one >= high */
}

static void bscan_tune(rx_t *rx, uint32_t hz)
{
	g_bs.hz = hz;
	rx->freq = hz;			/* not set_freq: the band memories stay as they are */
	rx->band = band_of(hz);
	g_bs.frames = 0;
	memset(g_bs.acc, 0, sizeof(g_bs.acc));
	g_bs.settle_until = now_ms() + BS_SETTLE_MS;
}

static bool bscan_start(rx_t *rx, bool all)
{
	if (g_bs.on || rx->ptt || rx->tuning || rx->bb_tx)
		return false;

	if (!all && rx->band < 0) {
		toast("NOT ON A BAND");
		return false;
	}

	memset(&g_bs, 0, sizeof(g_bs));
	g_bs.on = true;
	g_bs.all = all;
	g_bs.band = all ? 0 : rx->band;
	g_bs.last = all ? BAND_COUNT - 1 : rx->band;
	g_bs.ret_freq = rx->freq;
	g_bs.ret_band = rx->band;
	g_bs.ret_rit = rx->rit;
	g_bs.ret_atu = rx->atu_on;
	rx->rit = false;
	rx->atu_on = false;	/* straight through: no relay switching between windows */

	for (int b = g_bs.band; b <= g_bs.last; b++) {
		birdie_clear(g_bands[b].low, g_bands[b].high);
		g_bs.steps += bscan_windows(b);
	}

	g_birdie_full = 0;
	printf("\nbirdie scan: %s, antenna off or a dummy load; any key stops\n", all ? "all bands" : g_bands[g_bs.band].name);
	bscan_tune(rx, g_bands[g_bs.band].low);

	return true;
}

static void bscan_stop(rx_t *rx, bool done)
{
	char s[24];	/* the toast takes 23 characters */

	if (!g_bs.on)
		return;

	g_bs.on = false;
	rx->freq = g_bs.ret_freq;
	rx->band = g_bs.ret_band;
	rx->rit = g_bs.ret_rit;
	rx->atu_on = g_bs.ret_atu;
	birdie_save();

	snprintf(s, sizeof(s), "%s: %u FOUND", done ? "BIRDIES" : "SCAN STOPPED", (unsigned) g_bs.found % 1000u);
	toast(s);
	printf("\nbirdie scan %s: %d found, %d stored in %s\n", done ? "done" : "stopped", g_bs.found, g_nbirdie, BIRDIE_PATH);

	if (g_birdie_full > 0)
		printf("birdie scan: the list (%d) was full %d times: the weakest gave way to stronger ones\n", BIRDIE_MAX,
			g_birdie_full);
}

// One averaged window: peaks into the birdie list
static void bscan_window(void)
{
	const int n = X6100_FLOW_SAMPLES, c = n / 2;
	static float db[X6100_FLOW_SAMPLES], sorted[2 * BS_EDGE_BINS];
	int m = 0;

	for (int i = 0; i < n; i++)
		db[i] = g_bs.acc[i] > 1e-30 ? (float) (10.0 * log10(g_bs.acc[i] / g_bs.frames)) : -300.0f;

	for (int i = c - BS_EDGE_BINS; i <= c + BS_EDGE_BINS; i++)
		if (abs(i - c) > BS_DC_BINS && m < (int) (sizeof(sorted) / sizeof(sorted[0])))
			sorted[m++] = db[i];

	for (int i = 1; i < m; i++) {
		float v = sorted[i];
		int j = i - 1;

		while (j >= 0 && sorted[j] > v) {
			sorted[j + 1] = sorted[j];
			j--;
		}

		sorted[j + 1] = v;
	}

	float noise = sorted[m / 2];
	const band_t *b = &g_bands[g_bs.band];

	for (int i = c - BS_EDGE_BINS + BS_WIDE_BINS; i <= c + BS_EDGE_BINS - BS_WIDE_BINS; i++) {
		if (abs(i - c) <= BS_DC_BINS + 1 || db[i] <= db[i - 1] || db[i] < db[i + 1] || db[i] - noise < BS_SNR_DB
			|| db[i] - db[i - BS_WIDE_BINS] < 6.0f || db[i] - db[i + BS_WIDE_BINS] < 6.0f)
			continue;

		float den = db[i - 1] - 2.0f * db[i] + db[i + 1];
		float d = den < 0.0f ? 0.5f * (db[i - 1] - db[i + 1]) / den : 0.0f;
		int64_t hz = (int64_t) g_bs.hz + (int64_t) lround((i + (d < -0.5f ? -0.5f : d > 0.5f ? 0.5f : d) - c)
			* (double) SPAN_HZ / n);
		int snr = (int) lround(db[i] - noise);

		if (hz < b->low || hz > b->high)
			continue;

		int before = g_nbirdie;

		birdie_add((uint32_t) hz, snr);

		if (g_nbirdie > before) {
			g_bs.found++;
			printf("\nbirdie %.3f kHz, %d dB over the noise\n", hz / 1000.0, snr);
		}
	}
}

// Every flow frame while a scan runs
static void bscan_feed(rx_t *rx, const x6100_flow_frame_t *f)
{
	static float buf[X6100_FLOW_SAMPLES][2];
	const int n = X6100_FLOW_SAMPLES;

	if (!g_bs.on || now_ms() < g_bs.settle_until)
		return;

	for (int i = 0; i < n; i++) {
		float w = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (float) i / (float) (n - 1));

		buf[i][0] = f->iq[i][0] * w;
		buf[i][1] = f->iq[i][1] * w;
	}

	fft(buf, n);

	for (int i = 0; i < n; i++) {
		int k = (i + n / 2) % n;	/* centred, as the scope */
		int pos = X6100_FLOW_IQ_MIRRORED ? (n - i) % n : i;	/* mirrored: offset n / 2 - i bins */

		g_bs.acc[pos] += (double) buf[k][0] * buf[k][0] + (double) buf[k][1] * buf[k][1];
	}

	if (++g_bs.frames < BS_FRAMES)
		return;

	bscan_window();
	g_bs.step++;

	// Next window, next band, or done
	if (g_bs.hz < g_bands[g_bs.band].high) {
		bscan_tune(rx, g_bs.hz + BS_STEP_HZ);
	} else if (g_bs.band < g_bs.last) {
		g_bs.band++;
		bscan_tune(rx, g_bands[g_bs.band].low);
	} else {
		bscan_stop(rx, true);
	}
}

// Progress for the scope and the GEN row
static void bscan_text(char *s, size_t n)
{
	snprintf(s, n, "BIRDIE SCAN %s %d %%  %d FOUND", g_bands[g_bs.band].name,
		g_bs.steps > 0 ? g_bs.step * 100 / g_bs.steps : 0, g_bs.found);
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
	view_t v = { .span = scope_width(rx), .vfo = iq_hz(rx), .w = w };	/* vfo: the IQ centre (RIT, AM offset, IF shift) */

	// CENTER / SCROLL-C: the carrier in the middle, or the middle of the receive
	// filter (scope panel CENTER TYPE)
	int lo = 0, hi = 0;

	if (g_sp[SP_CTYPE] == 0)
		passband(rx, &lo, &hi);

	v.lo = scope_fixed(rx) ? rx->fix_lo : (int64_t) rx->freq + (lo + hi) / 2 - rx->span / 2;
	return v;
}

static int spec_index(const view_t *v, int x)
{
	const int n = X6100_FLOW_SAMPLES;
	int64_t f = v->lo + (int64_t) x * v->span / v->w;
	int i = n / 2 + (int) floor((double) (f - (int64_t) v->vfo) * n / SPAN_HZ);

	if (i < 0 || i >= n)
		return -1;

	return X6100_FLOW_IQ_MIRRORED ? (n - i) % n : i;	/* mirrored: RF +f at IQ -f, bin n - i */
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
		return i0 >= 0 && i0 < n ? arr[X6100_FLOW_IQ_MIRRORED ? (n - i0) % n : i0] : SPEC_DB_MIN - 20.0f;

	if (X6100_FLOW_IQ_MIRRORED) {
		i0 = (n - i0) % n;	/* as spec_index: bin n - i, not n - 1 - i (one bin, 195.3 Hz) */
		i1 = (n - i1) % n;
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

	rx_filter(rx, &low, &high);

	switch (rx->mode) {
	case X6100_MODE_LSB:
	case X6100_MODE_LSB_DIG:
	case X6100_MODE_CWR:
		*lo = -(int) high;
		*hi = -(int) low;
		break;
	case X6100_MODE_AM: {
		// Around the carrier: centred on the receive offset, the half-width apply sets
		int centre, half;

		am_pass(rx, &centre, &half);
		*lo = centre - half;
		*hi = centre + half;
		break;
	}
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

	int64_t ref = (int64_t) pb_ref(rx) - iq_hz(rx);	/* the passband's reference from the IQ centre */

	passband(rx, &lo, &hi);

	for (int hz = lo; hz <= hi; hz += (int) bw) {
		int off = (int) lround((hz + ref) / bw);
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

//
// Squelch: open or closed for the passband level dbm (see g_sql_open); always open with
// SQL 0 and while transmitting
//

static void sql_update(const rx_t *rx, double dbm, bool tx)
{
	if (rx->sql <= 0 || tx || meter_frac(dbm) >= rx->sql / 100.0 - 1e-9) {	/* S6 on SQL 40: open */
		g_sql_open = true;
		g_sql_below = 0;
		return;
	}

	if (g_sql_below == 0)
		g_sql_below = now_ms();
	else if (now_ms() - g_sql_below >= SQL_HANG_MS)
		g_sql_open = false;
}

//
// The noise for SQL push (2026-11-07): with the VOL knob on SQL a push sets the squelch
// just above what comes in now. The highest bar position (meter_frac) of the last second
// to a full second ago, so a noise peak does not open it again at once.
//

#define SQL_SET_MARGIN	2	/* SQL above that peak: about a third of an S unit */

static double g_noise_max[2];	/* the highest bar position: this second, the one before */
static uint64_t g_noise_at;	/* when this second began (ms) */

static void sql_noise_track(double dbm)
{
	uint64_t t = now_ms();
	double fr = meter_frac(dbm);

	if (t - g_noise_at >= 1000) {
		g_noise_max[1] = g_noise_max[0];
		g_noise_max[0] = 0.0;
		g_noise_at = t;
	}

	g_noise_max[0] = fr > g_noise_max[0] ? fr : g_noise_max[0];
}

// SQL just above the noise of the last second or two
static int sql_from_noise(void)
{
	double n = g_noise_max[0] > g_noise_max[1] ? g_noise_max[0] : g_noise_max[1];

	return clampi((int) ceil(n * 100.0) + SQL_SET_MARGIN, 1, 100);
}

//
// The squelch for the audio to Linux too (2026-11-07): the RX audio reaches the codec on
// Mic1 before the RX volume, so wfserver (wfview) heard the noise of a closed squelch.
// The Mic1 switch alone did not do it (an analog mixer route can reach the ADC too), so
// the digital switch after the ADC, which every capture to Linux passes:
// closed: AIF1 Data Digital ADC capture off, open: on. Set only on a change; g_sql_cap -1
// (unknown) after anything else set the capture path, so it is set again.
//

static int g_sql_cap = -1;	/* that capture switch as last set: 1 on, 0 off, -1 unknown */

static void sql_capture(void)
{
	int want = g_sql_open ? 1 : 0;

	if (want == g_sql_cap)
		return;

	static bool warned;	/* said once */

	if (x6100_mixer_set(X6100_PCM_CARD, "AIF1 Data Digital ADC Capture Switch", want) < 0 && !warned) {
		fprintf(stderr, "\nsquelch: codec capture switch (AIF1 Data Digital ADC) not set\n");
		warned = true;
	}

	g_sql_cap = want;
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
// with an SWR bar in TX. frac and peak are 0..1 of the bar width. sql (RX, 1-100): the
// squelch marker, a small triangle over the scale and a line through the bar at the same
// fraction; amber while closed, green while open.
//

static void draw_sbar(x6100_lcd_t *lcd, double frac, double peak, bool tx, double swr, int meter, int sql)
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
	// Squelch marker: where the squelch opens (SQL = the position on the bar)
	//
	if (!tx && sql > 0) {
		const uint32_t c = g_sql_open ? 0x3CC83C : 0xFFB020;
		int x = x0 + (int) (sql / 100.0 * SBAR_W);

		for (int r = 0; r < 6; r++)	/* pointing down onto the scale line */
			x6100_lcd_hline(lcd, x - 5 + r, x + 5 - r, SBAR_SCALE_Y - 7 + r, c);

		x6100_lcd_vline(lcd, x, SBAR_BAR_Y - 1, SBAR_BAR_Y + SBAR_BAR_H, c);
	}

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
	const char *line2, int sql)
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
	// Squelch marker (RX, SQL 1-100 on the S scale, as on the bar): a small triangle just
	// outside the S arc pointing at it, a short line across the arc; amber closed, green open
	//
	if (scale == VU_SCALE_S && sql > 0) {
		const uint32_t c = g_sql_open ? 0x3CC83C : 0xFFB020;
		const double f = sql / 100.0, d = 0.014;
		int ax, ay, bx, by;

		vu_point(f, VU_R_S + 3, &ax, &ay);

		for (int k = 0; k <= 8; k++) {
			vu_point(f - d + 2.0 * d * k / 8.0, VU_R_S + 12, &bx, &by);
			x6100_lcd_line(lcd, ax, ay, bx, by, 1, c);
		}

		vu_point(f, VU_R_S - 6, &ax, &ay);
		vu_point(f, VU_R_S + 3, &bx, &by);
		x6100_lcd_line(lcd, ax, ay, bx, by, 2, c);
	}

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
	uint32_t lo, hi, elo, ehi;
	char s[48], w[12];

	mode_filter(rx->mode, rx->fil, &lo, &hi);
	rx_filter(rx, &elo, &ehi);	/* the passband as PBT leaves it */

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

	// PBT: centre --, one shift when inner and outer agree, else both (inner / outer)
	if (g == FG_FM || (rx->pbt_in == 0 && rx->pbt_out == 0))
		snprintf(s, sizeof(s), "PBT --");
	else if (rx->pbt_in == rx->pbt_out)
		snprintf(s, sizeof(s), "PBT %+d", rx->pbt_in);
	else
		snprintf(s, sizeof(s), "PBT %+d/%+d", rx->pbt_in, rx->pbt_out);

	x6100_lcd_fill(lcd, px + 350, py + 32, 190, 22, rx->fedit_par == 2 ? act : bg);
	x6100_lcd_text(lcd, px + 356, py + 36, 2, s, rx->fedit_par == 2 ? white : pbt_active(rx) ? SCR_WARN : dim);

	// The box being edited: the amber frame (MFK push: row_push)
	if (row_editing(rx)) {
		static const int bx[3] = { 10, 180, 350 }, bw[3] = { 160, 160, 190 };
		int b = rx->fedit_par < 0 || rx->fedit_par > 2 ? 0 : rx->fedit_par;

		row_mark(lcd, px + bx[b], py + 32, bw[b], 22);
	}

	//
	// Passband (after PBT) on a 0 .. max axis with a trapezoid, edges and centre labelled
	//
	{
		const int ax0 = px + 360, ax1 = px + pw - 16, ay = py + 92;
		const uint32_t span = g == FG_CW ? 2000 : FIL_MAX_HZ;
		int x0 = ax0 + (int) ((int64_t) elo * (ax1 - ax0) / span);
		int x1 = ax0 + (int) ((int64_t) ehi * (ax1 - ax0) / span);

		x0 = x0 > ax1 ? ax1 : x0;
		x1 = x1 > ax1 ? ax1 : x1;
		x6100_lcd_hline(lcd, ax0, ax1, ay, dim);

		for (int r = 0; r < 28; r++) {
			int inset = (28 - r) * 6 / 28;
			int a = x0 + inset, b = x1 - inset;

			if (b > a)
				x6100_lcd_hline(lcd, a, b, ay - 28 + r, green);
		}

		snprintf(s, sizeof(s), "%u", elo);
		x6100_lcd_text(lcd, x0 - x6100_lcd_text_width(s, 1) / 2, ay + 6, 1, s, white);
		snprintf(s, sizeof(s), "%u", (elo + ehi) / 2);
		x6100_lcd_text(lcd, (x0 + x1) / 2 - x6100_lcd_text_width(s, 1) / 2, ay + 16, 1, s, dim);
		snprintf(s, sizeof(s), "%u", ehi);
		x6100_lcd_text(lcd, x1 - x6100_lcd_text_width(s, 1) / 2, ay + 6, 1, s, white);
	}

	//
	// Edges as text (amber while PBT moves them), and the controls
	//
	snprintf(s, sizeof(s), "%u - %u Hz", elo, ehi);
	x6100_lcd_text(lcd, px + 16, py + 66, 2, s, pbt_active(rx) ? SCR_WARN : white);
	x6100_lcd_text(lcd, px + 16, py + 90, 1, g == FG_FM ? (row_editing(rx) ? "MFK: BW    PUSH MFK: DONE" : "PUSH MFK: EDIT BW")
		: g == FG_AM ? (row_editing(rx) ? "MFK: ADJUST    PUSH MFK: DONE" : "MFK: BW/PBT    PUSH MFK: EDIT")
		: row_editing(rx) ? "MFK: ADJUST    PUSH MFK: DONE" : "MFK: BW/SFT/PBT    PUSH MFK: EDIT", dim);
	x6100_lcd_text(lcd, px + 16, py + 102, 1, g == FG_FM ? "HOLD MFK: DEF" : "HOLD MFK: DEF / PBT CLR", dim);
	x6100_lcd_text(lcd, px + 16, py + 114, 1, "F5: FIL1-3    HOLD F5: CLOSE", dim);
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
	"BRIGHTNESS", "LAYOUT", "VFO SPEED", "CAT CI-V", "CI-V ADDRESS", "CAT MEMORIES", "CAT CLOCK", "DATA OFF MOD", "DATA MOD",
	"WFSERVER", "NTP SYNC", "WIFI", "WIFI NETWORK", "BLUETOOTH", "BT PAIRING", "BT DEVICE", "BIRDIE SCAN",
	"BIRDIE MARKS", "BIRDIE NOTCH",
};
static const int g_cat_rates[] = { 0, 4800, 9600, 19200 };	/* the DEV port carries no more than 19200 */

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

//
// Bluetooth headset autoconnect: BlueZ does not connect a paired headset again by
// itself (after Bluetooth off / on, the headset off / on, at start). The last one
// connected from the GEN row (or seen connected) is remembered (state token btauto)
// and, while not connected, tried every BT_AUTO_MS by a detached check-then-connect:
// x6100_trx never waits on bluetoothctl. Disconnecting it on the GEN row forgets it.
//

#define BT_AUTO_MS	20000

static char g_btauto[18];		/* MAC, empty = off */
static int g_bt_power = -1;		/* Bluetooth as last set on the GEN row: 1 on, 0 off, -1 as the image has it (state bt) */
static uint64_t g_btauto_at;		/* last try, ms */

static void sys_spawn(char *const argv[]);

static void bt_autoconnect(uint64_t t)
{
	static const char script[] = "bluetoothctl show | grep -q 'Powered: yes' || exit 0;"
		"bluetoothctl info \"$1\" | grep -q 'Connected: yes' || bluetoothctl connect \"$1\"";
	char sh[] = "sh", c[] = "-c", arg0[] = "btauto", sc[sizeof(script)];
	char *cmd[] = { sh, c, sc, arg0, g_btauto, NULL };

	if (g_btauto[0] == '\0' || t - g_btauto_at < BT_AUTO_MS)
		return;

	g_btauto_at = t;
	memcpy(sc, script, sizeof(script));
	sys_spawn(cmd);	/* the MAC as $1, never in the script text */
}

//
// Bluetooth power as last set on the GEN row: the image switches it on at every boot,
// and on the shared chip (rtl8723bu, one antenna) it costs WiFi airtime: wfview over
// WiFi had audio dropouts with Bluetooth on (2026-11-02). Applied at start, detached:
// bluetoothd may still be starting (up to 10 s waited for)
//

static void bt_power_apply(void)
{
	static const char script[] = "for i in 1 2 3 4 5 6 7 8 9 10; do bluetoothctl show >/dev/null 2>&1 && break;"
		" sleep 1; done; bluetoothctl power \"$1\"";
	char sh[] = "sh", c[] = "-c", arg0[] = "btpower", on[] = "on", off[] = "off", sc[sizeof(script)];
	char *cmd[] = { sh, c, sc, arg0, g_bt_power ? on : off, NULL };

	if (g_bt_power < 0)
		return;

	memcpy(sc, script, sizeof(script));
	sys_spawn(cmd);
}

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

			// Connected some other way (bluetoothctl, the headset itself): remember it
			if (g_sys.dev_conn[i] && g_btauto[0] == '\0' && strstr(buf, "Icon: audio-") != NULL)
				snprintf(g_btauto, sizeof(g_btauto), "%s", g_sys.dev_mac[i]);
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

//
// wfserver (contrib/wfserver) is run by x6100_ui.sh: a watcher starts it while CAT is on
// the pty; /etc/x6100/wfserver.off switches the autostart off. The GEN row WFSERVER
// calls x6100_ui.sh wfserver on / off, as on the shell.
//

#define WFS_SH		"/opt/wfserver/wfserver.sh"
#define WFS_OFF		"/etc/x6100/wfserver.off"
#define WFS_PID		"/var/run/x6100_wfserver.pid"	/* the watcher */
#define UI_SH		"/root/x6100_ui.sh"		/* where build.sh deploy puts it */

static bool wfs_installed(void)
{
	return access(WFS_SH, X_OK) == 0;
}

static bool wfs_watching(void)
{
	FILE *fp = fopen(WFS_PID, "r");
	int pid = 0;

	if (fp != NULL) {
		if (fscanf(fp, "%d", &pid) != 1)
			pid = 0;

		fclose(fp);
	}

	return pid > 0 && kill(pid, 0) == 0;
}

//
// LAN and CAT indicators (under the clock on the scope). LAN: a wfview client is connected
// through wfserver. wfserver pings each client on its three UDP ports every 100 ms and the
// client answers, so while one is connected the system takes in some 30 UDP datagrams a
// second (/proc/net/udp counts no traffic per socket; /proc/net/snmp Udp InDatagrams
// counts all of it: NTP, DNS and the like stay far below the threshold). CAT: a CI-V
// frame came in on the DEV port or the pty in the last CAT_SEEN_MS.
//

#define LAN_DGRAMS_S	10	/* UDP datagrams in a second that mean a client */
#define LAN_HOLD_S	3	/* seconds below that before LAN goes off */
#define CAT_SEEN_MS	3000

static bool g_lan;
static bool g_wfs_run;	/* wfserver running, as lan_poll last saw it */
static uint64_t g_cat_rx_ms[CAT_N];	/* last CI-V frame in, per endpoint (CAT_EP_DEV, CAT_EP_PTY) */

static long long udp_in_dgrams(void)
{
	FILE *fp = fopen("/proc/net/snmp", "r");
	char line[512];
	long long v = -1;
	bool head = false;

	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "Udp: ", 5) != 0)
			continue;

		if (!head) {		/* the first Udp: line names the fields, InDatagrams first */
			head = true;
			continue;
		}

		v = strtoll(line + 5, NULL, 10);
		break;
	}

	fclose(fp);
	return v;
}

// Once a second: LAN on while wfserver runs and the UDP rate says a client is there
static void lan_poll(uint64_t t)
{
	static uint64_t last;
	static long long prev = -1;
	static int low;
	long long now_in;

	if (t - last < 1000)
		return;

	last = t;
	now_in = udp_in_dgrams();
	g_wfs_run = wfs_watching();

	if (now_in < 0 || prev < 0 || !g_wfs_run) {
		g_lan = false;
		low = LAN_HOLD_S;
	} else if (now_in - prev >= LAN_DGRAMS_S) {
		g_lan = true;
		low = 0;
	} else if (++low >= LAN_HOLD_S) {
		g_lan = false;
	}

	prev = now_in;
}

//
// WiFi indicator (2026-11-07): three arcs, filled by the signal of wlan0. Read every 5 s
// from the kernel, no program started: /proc/net/wireless (the level in dBm) and
// /sys/class/net/wlan0/operstate. g_wifi_bars: -1 not connected (no symbol), 0-3 arcs
// lit (-80 / -70 / -60 dBm), 4 connected with the level unknown (all arcs, dim)
//

static int g_wifi_bars = -1;

static void wifi_poll(uint64_t t)
{
	static uint64_t last;
	char line[256], state[16] = "";
	FILE *fp;
	int dbm = 0;

	if (last != 0 && t - last < 5000)
		return;

	last = t;

	if ((fp = fopen("/sys/class/net/wlan0/operstate", "r")) != NULL) {
		if (fgets(state, sizeof(state), fp) == NULL)
			state[0] = '\0';

		fclose(fp);
	}

	if (strncmp(state, "up", 2) != 0) {
		g_wifi_bars = -1;
		return;
	}

	if ((fp = fopen("/proc/net/wireless", "r")) != NULL) {
		while (fgets(line, sizeof(line), fp) != NULL) {
			char *p = strstr(line, "wlan0:");
			float link, level;

			if (p != NULL && sscanf(p + 6, "%*x %f %f", &link, &level) == 2)
				dbm = (int) level;
		}

		fclose(fp);
	}

	g_wifi_bars = dbm >= 0 ? 4 : dbm >= -60 ? 3 : dbm >= -70 ? 2 : dbm >= -80 ? 1 : 0;
}

// The symbol, its dot at (x, y): three arcs above it, 9, 15 and 21 px
static void draw_wifi(x6100_lcd_t *lcd, int x, int y, int bars)
{
	if (bars < 0)
		return;

	x6100_lcd_fill(lcd, x - 1, y - 2, 3, 3, bars == 0 ? SCR_WARN : 0x3CC83C);

	for (int r = 0; r < 3; r++) {
		int rad = 7 + r * 6;
		uint32_t c = bars == 4 ? SCR_DIM : r < bars ? 0x3CC83C : 0x303840;
		int px = x - (int) lround(rad * 0.7071), py = y - (int) lround(rad * 0.7071);

		for (int a = -36; a <= 45; a += 9) {	/* -45 .. +45 degrees from straight up */
			double ra = a * M_PI / 180.0;
			int qx = x + (int) lround(rad * sin(ra)), qy = y - (int) lround(rad * cos(ra));

			x6100_lcd_line(lcd, px, py, qx, qy, 2, c);
			px = qx;
			py = qy;
		}
	}
}

//
// What a network client needs (2026-11-06): the radio's IPv4 addresses and the wfserver
// UDP ports (wfview: control, CI-V, audio; from wfserver.ini, the defaults 50001-50003),
// said at the start next to the CAT lines and again when an address changes (WiFi up
// later, another network)
//

#define WFS_INI		"/etc/x6100/wfserver.ini"

static int g_wfs_port[3] = { 50001, 50002, 50003 };	/* control, CI-V, audio */

static void wfs_ports_read(void)
{
	static const char *key[3] = { "ServerControlPort=", "ServerCivPort=", "ServerAudioPort=" };
	FILE *fp = fopen(WFS_INI, "r");
	char line[256];

	if (fp == NULL)
		return;

	while (fgets(line, sizeof(line), fp) != NULL) {
		for (int i = 0; i < 3; i++) {
			size_t kl = strlen(key[i]);
			int p;

			if (strncmp(line, key[i], kl) == 0 && (p = atoi(line + kl)) > 0 && p < 65536)
				g_wfs_port[i] = p;
		}
	}

	fclose(fp);
}

// "wlan0 192.168.1.20, usb0 10.0.0.1" (IPv4, not loopback); empty when there is none
static void net_addrs(char *out, size_t n)
{
	struct ifaddrs *ifa;
	size_t len = 0;

	out[0] = '\0';

	if (getifaddrs(&ifa) != 0)
		return;

	for (struct ifaddrs *a = ifa; a != NULL; a = a->ifa_next) {
		char ip[INET_ADDRSTRLEN];

		if (a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET || strcmp(a->ifa_name, "lo") == 0)
			continue;

		inet_ntop(AF_INET, &((struct sockaddr_in *) a->ifa_addr)->sin_addr, ip, sizeof(ip));
		len += (size_t) snprintf(out + len, len < n ? n - len : 0, "%s%s %s", len ? ", " : "", a->ifa_name, ip);
	}

	freeifaddrs(ifa);
}

// At the start (always) and every 5 s from the main loop (only when the addresses changed)
static void net_announce(bool always)
{
	static char last[400] = "-";
	char now[256], line[400];

	net_addrs(now, sizeof(now));
	wfs_ports_read();
	snprintf(line, sizeof(line), "network: %s; wfview (wfserver %s): UDP %d control, %d CI-V, %d audio",
		now[0] != '\0' ? now : "no IPv4 address yet", !wfs_installed() ? "not installed"
		: access(WFS_OFF, F_OK) == 0 ? "off" : wfs_watching() ? "running" : "not running",
		g_wfs_port[0], g_wfs_port[1], g_wfs_port[2]);

	// Again when an address or the wfserver state changed (x6100_ui.sh starts it later)
	if (!always && strcmp(line, last) == 0)
		return;

	snprintf(last, sizeof(last), "%s", line);
	printf("%s%s\n", always ? "" : "\n", line);
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
	case GEN_LAYOUT:
		snprintf(s, n, "%s", g_layout_portrait ? "PORTRAIT" : "LANDSCAPE");
		break;
	case GEN_VFOFAST:
		snprintf(s, n, "%s", g_vfo_fast ? "ON" : "OFF");
		break;
	case GEN_CAT:
		if (g_cat_baud > 0)
			snprintf(s, n, "%d", g_cat_baud);
		else
			snprintf(s, n, "OFF");
		break;
	case GEN_CIVADDR:
		snprintf(s, n, "%s %02XH", civ_ident(), g_civ_addr);
		break;
	case GEN_CATMEM:
		snprintf(s, n, "%s", g_cat_mem_rw ? "READ / WRITE" : "READ ONLY");
		break;
	case GEN_CATTIME:
		snprintf(s, n, "%s", g_cat_time == CT_OFF ? "OFF" : g_cat_time == CT_ON ? "ON" : "IF NOT SYNCED");
		break;
	case GEN_MODOFF:
	case GEN_MODDATA:
		snprintf(s, n, "%s", g_modin[row == GEN_MODDATA] == MODIN_LAN ? "LAN" : g_modin[row == GEN_MODDATA] == MODIN_USB ? "USB" : "MIC");
		break;
	case GEN_WFS:
		snprintf(s, n, "%s", !wfs_installed() ? "N/A" : access(WFS_OFF, F_OK) == 0 ? "OFF" : "ON");
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
	case GEN_BSCAN:
		if (bscan_running())
			snprintf(s, n, "%s %d %%", g_bands[g_bs.band].name, g_bs.steps > 0 ? g_bs.step * 100 / g_bs.steps : 0);
		else
			snprintf(s, n, "%s", g_bscan_all ? "ALL BANDS" : "THIS BAND");
		break;
	case GEN_BMARK:
		snprintf(s, n, "%s", g_bmarks ? "ON" : "OFF");
		break;
	case GEN_BNOTCH:
		snprintf(s, n, "%s", g_bnotch ? "ON" : "OFF");
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
	case GEN_LAYOUT:
		if (g_autolock_s > 0)
			snprintf(s, n, "PORTRAIT: LEFT SIDE UP, AUTO LOCK %d S", g_autolock_s);
		else
			snprintf(s, n, "PORTRAIT: LEFT SIDE UP, NO AUTO LOCK");
		break;
	case GEN_VFOFAST:
		snprintf(s, n, "TURNED FAST: STEP X2 X5 X10 (UP TO 1 KHZ)");
		break;
	case GEN_CAT:
		snprintf(s, n, "DEV PORT, NEXT START; PTY %s", g_cat_pty ? "ALWAYS ON" : "OFF");
		break;
	case GEN_NTP:
		snprintf(s, n, "APPLIES AT NEXT START");
		break;
	case GEN_CIVADDR:
		snprintf(s, n, "WFVIEW: %s RIG FILE", g_civ_addr == X6100_CIV_ADDR_X6100 ? "X6100" : "IC-705");
		break;
	case GEN_CATMEM:
		snprintf(s, n, "%s", g_cat_mem_rw ? "CAT MAY WRITE AND CLEAR CHANNELS" : "CAT WRITE / CLEAR: NG");
		break;
	case GEN_CATTIME:
		snprintf(s, n, "%s", g_cat_time == CT_OFF ? "CAT CLOCK SET: NG" : g_cat_time == CT_ON
			? "CAT MAY SET THE CLOCK" : clock_synced() ? "NTP KEEPS IT: CAT SET NG" : "CAT MAY SET IT NOW");
		break;
	case GEN_MODOFF:	/* the TX audio a PTT key or CAT PTT uses; the hand mic PTT is always MIC */
	case GEN_MODDATA:
		snprintf(s, n, "%s  USB: LINUX (WFSERVER)  LAN: CAT PTT USB, KEY MIC", row == GEN_MODDATA ? "USB-D LSB-D" : "SSB AM FM");
		break;
	case GEN_WFS:
		if (!wfs_installed())
			snprintf(s, n, "NOT INSTALLED: BUILD.SH DEPLOY");
		else if (access(WFS_OFF, F_OK) == 0)
			snprintf(s, n, "%s", wfs_watching() ? "STOPPING" : "TURN: ON");
		else if (!g_cat_pty)
			snprintf(s, n, "NEEDS THE CI-V PTY: CATPTY OFF IN THE CONF");
		else if (wfs_watching())
			snprintf(s, n, "RUNNING, WFVIEW PORT %d", g_wfs_port[0]);
		else
			snprintf(s, n, "NOT RUNNING, HOLD MFK: START");
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
	case GEN_BSCAN:
		snprintf(s, n, "%s", bscan_running() ? "ANY KEY STOPS" : "ANTENNA OFF, HOLD MFK: START");
		break;
	case GEN_BMARK:
		snprintf(s, n, "%d STORED, TICKS ON THE SCOPE", g_nbirdie);
		break;
	case GEN_BNOTCH:
		snprintf(s, n, "USB / LSB / AM, NOT WITH DFL ON");
		break;
	default:
		s[0] = '\0';
		break;
	}
}

// MFK turn on a row (dir +-1); def: MFK long (default, or connect on the WiFi / BT rows,
// start on BIRDIE SCAN)
static void gen_adjust(int row, int dir, bool def)
{
	if (g_conf_set & (1u << row))
		return;

	g_gen_save_at = now_ms() + GEN_SAVE_MS;	/* on disk soon, not only at exit */

	switch (row) {
	case GEN_BRIGHT:
		g_bright = def ? 100 : clampi((g_bright > 0 ? g_bright : 100) + dir * 5, 5, 100);
		backlight_level(g_bright);
		break;
	case GEN_LAYOUT:	/* landscape <-> portrait; default landscape; shown once the panel closes */
		g_layout_portrait = def ? false : !g_layout_portrait;
		break;
	case GEN_VFOFAST:	/* on <-> off; default on */
		g_vfo_fast = def ? true : !g_vfo_fast;
		break;
	case GEN_CAT: {
		int i = 0;

		while (i < CAT_RATES - 1 && g_cat_rates[i] != g_cat_baud)
			i++;

		g_cat_baud = def ? 19200 : g_cat_rates[clampi(i + dir, 0, CAT_RATES - 1)];
		break;
	}
	case GEN_CIVADDR:	/* IC-705 (A4) <-> X6100 (61); the main loop gives it to the engine at once */
		g_civ_addr = def || g_civ_addr != X6100_CIV_ADDR_IC705 ? X6100_CIV_ADDR_IC705 : X6100_CIV_ADDR_X6100;
		break;
	case GEN_CATMEM:	/* read only <-> read / write; default read only */
		g_cat_mem_rw = def ? false : !g_cat_mem_rw;
		break;
	case GEN_CATTIME:	/* off / if not synced / on; default if not synced */
		g_cat_time = def ? CT_UNSYNCED : clampi(g_cat_time + dir, CT_OFF, CT_ON);
		break;
	case GEN_MODOFF:	/* MIC <-> USB; default as an Icom: MIC, data USB */
	case GEN_MODDATA:
		g_modin[row == GEN_MODDATA] = def ? (row == GEN_MODDATA ? MODIN_USB : MODIN_MIC)
			: clampi(g_modin[row == GEN_MODDATA] + dir, MODIN_MIC, MODIN_LAN);
		break;
	case GEN_WFS: {	/* turn: on / off; hold: on (the default) */
		char on[] = "on", off[] = "off", sh[] = "sh", ui[] = UI_SH, wfs[] = "wfserver";
		char *cmd[] = { sh, ui, wfs, def || dir > 0 ? on : off, NULL };

		if (wfs_installed() && access(UI_SH, R_OK) == 0)
			sys_spawn(cmd);	/* off waits up to 10 s for wfserver to end */
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

		if (!def)
			g_bt_power = dir > 0;	/* kept: applied at the next start */

		if (!def && dir > 0)
			g_btauto_at = now_ms() - BT_AUTO_MS + 2000;	/* the headset again in 2 s */
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
	case GEN_BSCAN:
		if (def)
			g_bscan_req = 1;	/* started by the main loop */
		else
			g_bscan_all = dir > 0;
		break;
	case GEN_BMARK:
		g_bmarks = def ? true : dir > 0;
		break;
	case GEN_BNOTCH:
		g_bnotch = def ? false : dir > 0;
		break;
	default:
		if (g_sys.ndev == 0)
			break;

		if (def) {
			char *cmd[] = { "bluetoothctl", g_sys.dev_conn[g_sys.dev_sel] ? "disconnect" : "connect",
				g_sys.dev_mac[g_sys.dev_sel], NULL };

			sys_spawn(cmd);	/* connecting takes seconds */
			g_sys.at = now_ms() - SYS_POLL_MS + 1500;

			// Autoconnect follows the choice: connect remembers it, disconnect forgets it
			if (g_sys.dev_conn[g_sys.dev_sel])
				g_btauto[0] = '\0';
			else
				snprintf(g_btauto, sizeof(g_btauto), "%s", g_sys.dev_mac[g_sys.dev_sel]);
		} else {
			g_sys.dev_sel = (g_sys.dev_sel + dir + g_sys.ndev) % g_sys.ndev;
		}
		break;
	}
}

// MFK push on a GEN row of two values: the other one (row_toggle); false for a row with
// more values (brightness, CAT speed, CAT clock, mod inputs, WiFi network, BT device): those edit
static bool gen_toggle(int row)
{
	bool on;

	switch (row) {
	case GEN_LAYOUT:
	case GEN_VFOFAST:
	case GEN_CIVADDR:
	case GEN_CATMEM:
	case GEN_NTP:
		gen_adjust(row, 1, false);	/* these flip whatever the direction */
		return true;
	case GEN_WFS:
		on = wfs_installed() && access(WFS_OFF, F_OK) != 0;
		break;
	case GEN_WIFI:
		on = g_sys.wifi_on;
		break;
	case GEN_BT:
		on = g_sys.bt_on;
		break;
	case GEN_BTPAIR:
		on = g_sys.bt_visible && now_ms() < g_sys.bt_visible_until;
		break;
	case GEN_BSCAN:
		on = g_bscan_all;
		break;
	case GEN_BMARK:
		on = g_bmarks;
		break;
	case GEN_BNOTCH:
		on = g_bnotch;
		break;
	default:
		return false;
	}

	gen_adjust(row, on ? -1 : 1, false);
	return true;
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

	gen_frame(lcd, px, py, pw, ph, "SETTINGS", row_editing(rx)
		? "MFK: ADJUST  PUSH: DONE  HOLD: DEFAULT / CONNECT  GEN: CLOSE" : "MFK: SELECT  PUSH: ON-OFF / EDIT  HOLD: DEFAULT / CONNECT");

	for (int i = top; i < top + vis; i++) {
		int y = py + 30 + (i - top) * 21;
		bool locked = g_conf_set & (1u << i);
		char v[40], h[64];

		if (i == rx->gedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

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
		char pos[24];

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

	if (scr->ext)
		snprintf(lines[n++], sizeof(lines[0]), "BATTERY %.1f V %d %%  %s, EXT %.1f V", scr->vbat, scr->batcap,
			scr->batcap < 100 ? "CHARGING" : "FULL", scr->vext);	/* bit 4 stays 0 while charging (2026-11-03) */
	else
		snprintf(lines[n++], sizeof(lines[0]), "BATTERY %.1f V %d %%", scr->vbat, scr->batcap);

	{
		char dev[16] = "OFF";

		if (g_cat_baud > 0)
			snprintf(dev, sizeof(dev), "%d BAUD", g_cat_baud);

		snprintf(lines[n++], sizeof(lines[0]), "CAT     DEV %s, PTY %s, %s %02XH", dev, g_cat_pty ? "ON" : "OFF",
			civ_ident(), g_civ_addr);	/* pty: CAT_PTY_LINK */
	}

	snprintf(lines[n++], sizeof(lines[0]), "NTP     %s", g_ntp_msg);
	snprintf(lines[n++], sizeof(lines[0]), "REGION  %s   UPTIME %dh%02dm   BUILD %s", g_regions[g_region].name,
		(int) up / 3600, (int) up / 60 % 60, X6100_REV);

	for (int i = 0; i < n; i++) {
		for (char *c = lines[i]; *c != '\0'; c++)
			*c = (char) toupper((unsigned char) *c);	/* the panel font has capitals only */

		x6100_lcd_text(lcd, px + 14, py + 30 + i * 17, 2, lines[i], i == n - 1 ? dim : white);
	}
}

//
// Keyer panel (KEY long): five of the keyer values around the selected one.
// Orange. MFK turn: adjust; push: next value; hold: default; KEY long, another key
// or 10 s idle closes.
//

static void draw_key_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 14, pw = 500, ph = 30 + vis * 21;
	const uint32_t bg = 0x1A1008, orange = 0xFF9040, sel = 0x40281A, dim = 0xA08C78, white = 0xF0F0F0;
	int top = clampi(rx->kedit_par - 2, 0, KPARS - vis);
	char pos[12];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, orange);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x4A3220);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x4A3220);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x4A3220);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "KEYER", orange);
	x6100_lcd_text(lcd, px + 110, py + 11, 1, row_editing(rx) ? "MFK: ADJUST  PUSH: DONE  HOLD: DEFAULT"
		: "MFK: SELECT  PUSH: ON-OFF / EDIT  HOLD: DEFAULT", dim);
	snprintf(pos, sizeof(pos), "%d/%d", rx->kedit_par + 1, KPARS);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(pos, 1), py + 11, 1, pos, orange);

	for (int i = top; i < top + vis; i++) {
		int y = py + 30 + (i - top) * 21;
		char v[24];

		if (i == rx->kedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

		x6100_lcd_text(lcd, px + 14, y, 2, g_kpar[i].name, i == rx->kedit_par ? white : dim);
		kpar_text(rx, i, v, sizeof(v));
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v, 2), y, 2, v, white);
	}
}

//
// TX panel (F4 long): five of the TX audio values around the selected one. Red.
// MFK turn: adjust; push: next value; hold: default; F4 long, another key or 10 s
// idle closes. On / off rows are green when on.
//

static void draw_tx_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 14, pw = 500, ph = 30 + vis * 21;
	const uint32_t bg = 0x1C0A0A, red = 0xFF5050, sel = 0x441A1A, dim = 0xA08080, white = 0xF0F0F0;
	const uint32_t on = 0x3CC83C, off = 0x5A6464;
	int top = clampi(rx->xedit_par - 2, 0, XPARS - vis);
	char pos[24];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, red);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x4A2020);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x4A2020);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x4A2020);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "TX", red);
	x6100_lcd_text(lcd, px + 110, py + 11, 1, row_editing(rx) ? "MFK: ADJUST  PUSH: DONE  HOLD: DEFAULT"
		: "MFK: SELECT  PUSH: ON-OFF / EDIT  HOLD: DEFAULT", dim);
	snprintf(pos, sizeof(pos), "%d/%d", rx->xedit_par + 1, XPARS);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(pos, 1), py + 11, 1, pos, red);

	for (int i = top; i < top + vis; i++) {
		int y = py + 30 + (i - top) * 21;
		uint32_t c = !xpar_onoff(i) ? white : *xpar_value((rx_t *) rx, i) ? on : off;
		char v[24];

		if (i == rx->xedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

		x6100_lcd_text(lcd, px + 14, y, 2, g_xpar[i].name, i == rx->xedit_par ? white : dim);
		xpar_text(rx, i, v, sizeof(v));
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v, 2), y, 2, v, c);
	}
}

//
// Scope panel (F1 long): five of the scope settings around the selected one. Blue.
// MFK turn: adjust; push: next value; hold: default; F1 long, another key or 10 s idle
// closes. On / off rows are green when on; a colour row shows the colour it sets.
//

static void draw_scope_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 14, pw = 500, ph = 30 + vis * 21;
	const uint32_t bg = 0x0A1220, blue = 0x50A0FF, sel = 0x1A2A44, dim = 0x8090A8, white = 0xF0F0F0;
	const uint32_t on = 0x3CC83C, off = 0x5A6464;
	int top = clampi(rx->sedit_par - 2, 0, SPARS - vis);
	char pos[24];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, blue);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x203A5A);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x203A5A);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x203A5A);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "SCOPE", blue);
	x6100_lcd_text(lcd, px + 110, py + 11, 1, row_editing(rx) ? "MFK: ADJUST  PUSH: DONE  HOLD: DEFAULT"
		: "MFK: SELECT  PUSH: ON-OFF / EDIT  HOLD: DEFAULT", dim);
	snprintf(pos, sizeof(pos), "%d/%d", rx->sedit_par + 1, SPARS);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(pos, 1), py + 11, 1, pos, blue);

	for (int i = top; i < top + vis; i++) {
		int y = py + 30 + (i - top) * 21;
		uint32_t c = !spar_onoff(i) ? white : spar_get(rx, i) ? on : off;
		char v[24];
		int vw;

		if (i == rx->sedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

		x6100_lcd_text(lcd, px + 14, y, 2, g_spar[i].name, i == rx->sedit_par ? white : dim);
		spar_token(rx, i, v, sizeof(v));

		for (char *p = v; *p != '\0'; p++)	/* the panel font is upper case */
			*p = (char) toupper((unsigned char) *p);

		if (i == SP_WF_LEVEL || i == SP_REF || i == SP_AF_REF)
			snprintf(v + strlen(v), sizeof(v) - strlen(v), " DB");

		vw = x6100_lcd_text_width(v, 2);
		x6100_lcd_text(lcd, px + pw - 12 - vw, y, 2, v, c);

		if (spar_colour(i))
			x6100_lcd_fill(lcd, px + pw - 12 - x6100_lcd_text_width("255", 2) - 44, y - 1, 34, 15,
				spar_rgb(spar_colour_base(i)));
	}
}

//
// Message panel (MSG): five of M1-M8 around the selected one. Yellow. MFK turn:
// select; push: send it; MSG long: send it again every msgrepeat s; MSG, another key
// or 10 s idle closes. Messages are set from the console (msg) or in messages.conf.
//

static void draw_msg_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 14, pw = 500, ph = 30 + vis * 21;
	const uint32_t bg = 0x1C1A08, yel = 0xF0D040, sel = 0x3A3410, dim = 0xA8A080, white = 0xF0F0F0;
	int top = clampi(rx->msedit_par - 2, 0, MSG_COUNT - vis);

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, yel);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x4A4420);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x4A4420);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x4A4420);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "MSG", yel);
	x6100_lcd_text(lcd, px + 110, py + 11, 1, "MFK: SELECT  PUSH: SEND  MSG LONG: REPEAT", dim);

	for (int i = top; i < top + vis; i++) {
		const msg_t *m = &g_msg[i + 1];
		int y = py + 30 + (i - top) * 21;
		char s[48];

		if (i == rx->msedit_par)
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

		snprintf(s, sizeof(s), "M%d %s %.30s", i + 1, m->type == MSG_CW ? "CW   " : m->type == MSG_VOICE ? "VOICE"
			: "-", m->text);

		for (char *c = s; *c != '\0'; c++)	/* the panel font is upper case */
			*c = (char) toupper((unsigned char) *c);

		x6100_lcd_text(lcd, px + 14, y, 2, s, i == rx->msedit_par ? white : dim);
	}
}

//
// App panel (APP): the apps; MFK push starts the selected one; APP long starts the
// SWR scan; APP, another key or 10 s idle closes. Green.
//

static void draw_app_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int px = LBL_W + 8, py = SPEC_Y + 14, pw = 500, ph = 30 + 2 * 21;
	const uint32_t bg = 0x0A1C0E, grn = 0x50D070, sel = 0x163A20, dim = 0x80A088, white = 0xF0F0F0;
	char v[40];

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, grn);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x204A2A);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x204A2A);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x204A2A);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "APP", grn);
	x6100_lcd_text(lcd, px + 110, py + 11, 1, "PUSH: START  ANY KEY: STOP", dim);

	int y = py + 30;

	x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);
	x6100_lcd_text(lcd, px + 14, y, 2, "SWR SCAN", white);
	snprintf(v, sizeof(v), "%d W AM, %d POINTS", SCAN_PWR / 10, SCAN_STEPS);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v, 2), y, 2, v, dim);
	x6100_lcd_text(lcd, px + 14, y + 21, 1, "THE BAND SEGMENT THE VFO IS IN; THE CURVE STAYS UNTIL A KEY", dim);
	(void) rx;
}

//
// Memory panel (V/M short): five channels around the selected one. Cyan. MFK turn:
// channel (a used one is tuned at once); MFK long: write the VFO into it; MFK push
// twice: clear it; V/M, another key or 10 s idle closes.
//

static void draw_mem_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 14, pw = 560, ph = 30 + vis * 21;
	const uint32_t bg = 0x08141C, cyan = 0x40C0FF, sel = 0x163044, dim = 0x7A8C98, white = 0xF0F0F0;
	int top = clampi(rx->medit_ch - 2, 1, MEM_COUNT - vis + 1);

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, cyan);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x1E3A4A);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x1E3A4A);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x1E3A4A);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "MEMORY", cyan);
	x6100_lcd_text(lcd, px + 120, py + 11, 1, "MFK: CHANNEL  HOLD: WRITE  PUSH TWICE: CLEAR  V/M: CLOSE", dim);

	for (int i = 0; i < vis; i++) {
		int ch = top + i, y = py + 30 + i * 21;
		const mem_t *m = &g_mem[ch];
		char s[64];

		if (ch == rx->medit_ch)
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

		if (m->used) {
			int n = snprintf(s, sizeof(s), "%02d  %2u.%03u.%02u %-4s", ch, m->hz / 1000000, m->hz / 1000 % 1000,
				m->hz % 1000 / 10, mode_name(m->mode));

			if (m->split)
				n += snprintf(s + n, sizeof(s) - (size_t) n, " TX %u.%03u", m->tx_hz / 1000000, m->tx_hz / 1000 % 1000);
			else if (m->dup == DUP_MINUS || m->dup == DUP_PLUS)
				n += snprintf(s + n, sizeof(s) - (size_t) n, " DUP%c %d", m->dup == DUP_MINUS ? '-' : '+', m->dup_khz);

			snprintf(s + n, sizeof(s) - (size_t) n, "  %s", m->name);
		} else {
			snprintf(s, sizeof(s), "%02d  ---", ch);
		}

		for (char *c = s; *c != '\0'; c++)
			*c = (char) toupper((unsigned char) *c);

		x6100_lcd_text(lcd, px + 14, y, 2, s, ch == rx->medit_ch ? white : m->used ? 0xC0D0DC : dim);
	}
}

//
// RIT / XIT / duplex panel (AGC long): one offset for both, as on Icom radios,
// RIT / XIT on or off, repeater duplex and its shift, the AM receive offset, the AGC
// SLOW / FAST time constants and hang. Green.
// Five rows in view, scrolling with the selection. MFK turn: offset in 10 Hz steps,
// on / off on the RIT and XIT rows, AUTO / OFF / DUP- / DUP+, shift in 10 kHz steps,
// AM offset in 10 Hz steps, AGC times in 10 ms (SLOW 50 ms from 1 s), hang on / off;
// push: next row; hold: offset 0 (CLEAR), duplex AUTO,
// shift 100 kHz, AM offset 100 Hz, AGC SLOW 250 / FAST 5 (x10 ms), hang off.
//

static void draw_rit_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const char *hint = row_editing(rx) ? "MFK: ADJUST  PUSH: DONE  HOLD: CLEAR" : "MFK: SELECT  PUSH: ON-OFF / EDIT  HOLD: CLEAR";
	static const char *dups[DUP_COUNT] = { "AUTO", "OFF", "DUP-", "DUP+" };
	static const char *names[TPARS] = { "OFFSET", "RIT  RX", "XIT  TX", "DUPLEX", "SHIFT", "AM OFFSET", "AGC SLOW",
		"AGC FAST", "AGC HANG" };
	const int vis = 5, px = LBL_W + 8, py = SPEC_Y + 16, pw = 480, ph = 134;
	const int top = clampi(rx->tedit_par - 2, 0, TPARS - vis);
	const uint32_t bg = 0x0C180C, green = 0x60D060, sel = 0x1C3A1C, dim = 0x80A080, white = 0xF0F0F0;
	const uint32_t on = 0x3CC83C, off = 0x5A6464;
	const int shift = dup_shift(rx);
	char v[TPARS][24], pos[24];

	snprintf(v[0], sizeof(v[0]), "%+d HZ", rx->rit_hz);
	snprintf(v[1], sizeof(v[1]), "%s", rx->rit ? "ON" : "OFF");
	snprintf(v[2], sizeof(v[2]), "%s", rx->xit ? "ON" : "OFF");

	if (rx->dup == DUP_AUTO && shift != 0)
		snprintf(v[3], sizeof(v[3]), "AUTO %+d KHZ", shift / 1000);
	else
		snprintf(v[3], sizeof(v[3]), "%s", dups[rx->dup]);

	snprintf(v[4], sizeof(v[4]), "%d KHZ", rx->dup_khz);

	if (g_am_ofs > 0)
		snprintf(v[5], sizeof(v[5]), "%d HZ", g_am_ofs);
	else
		snprintf(v[5], sizeof(v[5]), "OFF");

	snprintf(v[6], sizeof(v[6]), "%d X10 MS", g_agc_slow);
	snprintf(v[7], sizeof(v[7]), "%d X10 MS", g_agc_fast);
	snprintf(v[8], sizeof(v[8]), "%s", g_agc_hang ? "ON" : "OFF");

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, green);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x2A4A2A);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x2A4A2A);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x2A4A2A);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, "RIT / XIT / DUP", green);
	x6100_lcd_text(lcd, px + 14 + x6100_lcd_text_width("RIT / XIT / DUP", 2) + 16, py + 11, 1, hint, dim);
	snprintf(pos, sizeof(pos), "%d/%d", rx->tedit_par + 1, TPARS);
	x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(pos, 1), py + 11, 1, pos, green);

	for (int i = top; i < top + vis; i++) {
		const bool lit[TPARS] = { true, rx->rit, rx->xit, shift != 0, rx->dup == DUP_MINUS || rx->dup == DUP_PLUS,
			g_am_ofs > 0 && rx->mode == X6100_MODE_AM, rx->agc == X6100_AGC_SLOW, rx->agc == X6100_AGC_FAST,
			g_agc_hang != 0 };
		int y = py + 30 + (i - top) * 21;
		uint32_t c = i == 0 ? white : lit[i] ? on : off;

		if (i == rx->tedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

		x6100_lcd_text(lcd, px + 14, y, 2, names[i], i == rx->tedit_par ? white : dim);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v[i], 2), y, 2, v[i], c);
	}
}

//
// Edge panel (F2 long in FIX / SCROLL-F): lower and upper of the edge in use, its range
// and width. Blue, as the scope label it belongs to.
//

static void draw_edge_panel(x6100_lcd_t *lcd, const rx_t *rx)
{
	const char *hint = row_editing(rx) ? "MFK: 1 KHZ  PUSH: DONE  HOLD: DEFAULT  F2: NEXT EDGE"
		: "MFK: LOWER / UPPER  PUSH: EDIT  HOLD: DEFAULT  F2: NEXT EDGE";
	const int px = LBL_W + 8, py = SPEC_Y + 16, pw = 560, ph = 96;
	const uint32_t bg = 0x0C1622, blue = 0x40C0FF, sel = 0x1E4A7A, dim = 0x7A8494, white = 0xF0F0F0;
	int r = edge_range(rx->freq), e = clampi(rx->edge, 1, 4) - 1;
	char title[16], v[2][24], info[48];

	if (r < 0)
		return;

	uint32_t lo = g_edge[r][e].lo, hi = lo + (uint32_t) edge_w(r, e);

	snprintf(title, sizeof(title), "EDGE %d", e + 1);
	snprintf(v[0], sizeof(v[0]), "%u.%03u MHZ", lo / 1000000u, lo / 1000u % 1000u);
	snprintf(v[1], sizeof(v[1]), "%u.%03u MHZ", hi / 1000000u, hi / 1000u % 1000u);
	snprintf(info, sizeof(info), "RANGE %d %u-%u MHZ   WIDTH %u OF AT MOST %u KHZ", r + 1, g_edge_bound[r] / 1000000u,
		g_edge_bound[r + 1] / 1000000u, (hi - lo) / 1000u, edge_max() / 1000u);

	x6100_lcd_fill(lcd, px, py, pw, ph, bg);
	x6100_lcd_fill(lcd, px, py, 3, ph, blue);
	x6100_lcd_hline(lcd, px, px + pw - 1, py, 0x204060);
	x6100_lcd_hline(lcd, px, px + pw - 1, py + ph - 1, 0x204060);
	x6100_lcd_vline(lcd, px + pw - 1, py, py + ph - 1, 0x204060);
	x6100_lcd_text(lcd, px + 14, py + 7, 2, title, blue);
	x6100_lcd_text(lcd, px + 14 + x6100_lcd_text_width(title, 2) + 16, py + 11, 1, hint, dim);

	for (int i = 0; i < 2; i++) {
		int y = py + 30 + i * 21;

		if (i == rx->eedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 3, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 3, pw - 12, 19);
		}

		x6100_lcd_text(lcd, px + 14, y, 2, i == 0 ? "LOWER" : "UPPER", i == rx->eedit_par ? white : dim);
		x6100_lcd_text(lcd, px + pw - 12 - x6100_lcd_text_width(v[i], 2), y, 2, v[i], white);
	}

	x6100_lcd_text(lcd, px + 14, py + 78, 1, info, dim);
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
	x6100_lcd_text(lcd, px + 70, py + 11, 1, row_editing(rx) ? "MFK: ADJUST   PUSH: DONE   HOLD: DEFAULT   DFN: NR / NB   DFL: NOTCH"
		: "MFK: SELECT   PUSH: EDIT   HOLD: DEFAULT   DFN: NR / NB   DFL: NOTCH", dim);

	for (int i = 0; i < DSP_PARS; i++) {
		const dsp_par_t *d = &g_dsp[i];
		int v = *dsp_value((rx_t *) rx, i);
		int y = py + 28 + i * 20;
		int fill = (v - d->min) * sw / (d->max - d->min);

		if (i == rx->dedit_par) {
			x6100_lcd_fill(lcd, px + 6, y - 2, pw - 12, 19, sel);

			if (row_editing(rx))
				row_mark(lcd, px + 6, y - 2, pw - 12, 19);
		}

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
	} else if (rx->atu_last_valid) {
		const x6100_atu_t *n = &rx->atu_last;
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

		// Scope panel AF rows: averaging; max hold as on the RF scope (10 S and ON start
		// again when the setting changes, 10 S every 10 s)
		const float avg = 1.0f / (float) g_sp[SP_AF_AVG];
		static uint64_t hold_t;
		static int hold_mode = -1;
		uint64_t now = now_ms();
		bool restart = !scr->aspec_valid || g_sp[SP_AF_HOLD] != hold_mode
			|| (g_sp[SP_AF_HOLD] == HOLD_10S && now - hold_t >= 10000);

		if (restart) {
			hold_t = now;
			hold_mode = g_sp[SP_AF_HOLD];
		}

		for (int k = 0; k < AS_N / 2; k++) {
			// dBFS: a full-scale sine (Hann, coherent gain 0.5) reads 0 dB
			float p = (v[k][0] * v[k][0] + v[k][1] * v[k][1]) * 16.0f / ((float) AS_N * (float) AS_N);
			float db = p > 1e-20f ? 10.0f * log10f(p) : -200.0f;

			scr->aspec[k] = scr->aspec_valid ? scr->aspec[k] * (1.0f - avg) + db * avg : db;

			if (restart)
				scr->apeak[k] = scr->aspec[k];
			else if (g_sp[SP_AF_HOLD] == HOLD_DECAY)
				scr->apeak[k] = scr->apeak[k] - PEAK_DECAY_DB > scr->aspec[k] ? scr->apeak[k] - PEAK_DECAY_DB : scr->aspec[k];
			else
				scr->apeak[k] = scr->apeak[k] > scr->aspec[k] ? scr->apeak[k] : scr->aspec[k];
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
// One trace column at x, as the scope panel sets it for the RF or the audio (af)
// scope: fill under the trace (CURRENT colour), the max hold dot at py, the line from
// the last column
//

static void trace_col(x6100_lcd_t *lcd, int x, int y, int py, int prev_y, bool af)
{
	int wave = g_sp[af ? SP_AF_WAVE : SP_WAVE];

	if (wave != WAVE_LINE)
		x6100_lcd_vline(lcd, x, y, SPEC_Y + SPEC_H - 1, spar_rgb(af ? SP_AF_FILL_R : SP_FILL_R));

	if (g_sp[af ? SP_AF_HOLD : SP_HOLD] != HOLD_OFF)
		x6100_lcd_fill(lcd, x, py, 1, 1, spar_rgb(af ? SP_AF_HOLD_R : SP_HOLD_R));

	if (wave != WAVE_FILL)
		x6100_lcd_vline(lcd, x, prev_y < 0 ? y : prev_y, y, spar_rgb(af ? SP_AF_LINE_R : SP_LINE_R));
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

	// Passband in audio Hz: the filter edges with twin PBT (AM / FM: 0 .. high)
	rx_filter(rx, &lo, &hi);
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

			trace_col(lcd, x0 + x, y, py, prev_y, true);
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

	// Automatic notch on a birdie (BIRDIE NOTCH), yellow as the birdie ticks
	int bn = birdie_notch(rx);

	if (bn > 0 && bn < range) {
		int nx = x0 + bn * w / range;

		x6100_lcd_vline(lcd, nx, SPEC_Y, SPEC_Y + SPEC_H - 1, 0xE0E040);
		x6100_lcd_text(lcd, nx + 4, SPEC_Y + SPEC_H - 18, 2, "BN", 0xE0E040);
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

// Scroll columns x0 .. x0 + w - 1 of rows y0 .. y0 + h - 1 down by one row (the audio
// waterfall next to the 3D one, which is not scrolled)
static void wf_scroll_cols(x6100_lcd_t *lcd, int y0, int h, int x0, int w)
{
	for (int y = y0 + h - 1; y > y0; y--)
		memcpy(lcd->pix + (size_t) y * (size_t) lcd->w + (size_t) x0,
			lcd->pix + (size_t) (y - 1) * (size_t) lcd->w + (size_t) x0, (size_t) w * sizeof(uint32_t));
}

//
// 3D waterfall (scope panel WATERFALL 3D, wf3d on|off; 2026-11-07, after the one in the
// OpenX6X00 GUI): the last WF3D_N waterfall lines as spectra in perspective, the newest
// in front, older ones further back, narrower and dimmer. Kept per screen column, as
// the flat waterfall is: cleared and shifted at the same moments, so it stays aligned.
// 255 = no data in that column.
//

#define WF3D_N		32
#define WF3D_W		800
static uint8_t g_wf3d[WF3D_N][WF3D_W];
static int g_wf3d_head;		/* row of the newest line */
static int g_wf3d_n;		/* lines held */
static bool g_wf3d_dirty = true;	/* cleared or shifted: draw it again */

static void wf3d_clear(void)
{
	g_wf3d_n = 0;
	g_wf3d_dirty = true;
}

static void wf3d_shift(int dx, int w)
{
	w = w > WF3D_W ? WF3D_W : w;
	g_wf3d_dirty = true;

	for (int k = 0; k < WF3D_N; k++) {
		uint8_t *row = g_wf3d[k];

		if (dx >= w || dx <= -w)
			memset(row, 255, (size_t) w);
		else if (dx > 0) {
			memmove(row + dx, row, (size_t) (w - dx));
			memset(row, 255, (size_t) dx);
		} else if (dx < 0) {
			memmove(row, row - dx, (size_t) (w + dx));
			memset(row + w + dx, 255, (size_t) -dx);
		}
	}
}

// A new line: the level t (0-1, as the waterfall colour) per column, < 0 no data
static void wf3d_push(const float *t, int w)
{
	w = w > WF3D_W ? WF3D_W : w;
	g_wf3d_head = (g_wf3d_head + 1) % WF3D_N;
	g_wf3d_n = g_wf3d_n < WF3D_N ? g_wf3d_n + 1 : WF3D_N;

	for (int x = 0; x < w; x++)
		g_wf3d[g_wf3d_head][x] = t[x] < 0.0f ? 255 : (uint8_t) lroundf((t[x] > 1.0f ? 1.0f : t[x]) * 254.0f);
}

// Colour c at brightness f (0-1)
static uint32_t rgb_dim(uint32_t c, float f)
{
	return (uint32_t) ((float) (c >> 16) * f) << 16 | (uint32_t) ((float) (c >> 8 & 0xFF) * f) << 8
		| (uint32_t) ((float) (c & 0xFF) * f);
}

//
// Draw it in columns 0 .. w - 1 of the waterfall area (y0, h); cursor_x: the VFO column
// on the front line (outside 0 .. w - 1: none); tune: also the tune line across the floor
//

static void wf3d_draw(x6100_lcd_t *lcd, int y0, int h, int w, int cursor_x, bool tune)
{
	const float zstep = 0.022f, sb = 1.0f / (1.0f + zstep * (WF3D_N - 1));	/* the back line's scale */
	const float yf = (float) (y0 + h - 3), yb = (float) y0 + (float) h * 0.22f;	/* front, back baselines */
	const float yh = (yb - yf * sb) / (1.0f - sb);	/* horizon: where the baselines meet */
	const float amp = (float) h * 0.5f, cx = (float) w / 2.0f;
	const uint32_t grid = 0x1A2430;

	w = w > WF3D_W ? WF3D_W : w;
	x6100_lcd_fill(lcd, 0, y0, w, h, 0x000000);

	//
	// The floor: front and back edge, the sides and three lines going back
	//
	int bl = (int) lroundf(cx - cx * sb), br = (int) lroundf(cx + cx * sb), ybi = (int) lroundf(yb), yfi = (int) lroundf(yf);

	x6100_lcd_hline(lcd, 0, w - 1, yfi, grid);
	x6100_lcd_hline(lcd, bl, br, ybi, grid);

	for (int i = 0; i <= 4; i++) {
		float xf = (float) (w - 1) * (float) i / 4.0f;

		x6100_lcd_line(lcd, (int) xf, yfi, (int) lroundf(cx + (xf - cx) * sb), ybi, 1, grid);
	}

	if (cursor_x >= 0 && cursor_x < w)
		x6100_lcd_line(lcd, cursor_x, yfi, (int) lroundf(cx + ((float) cursor_x - cx) * sb), ybi, 1, 0x602020);

	//
	// The lines, back to front, each a ridge that hides what lies behind it: per screen
	// column the highest point of the line, black below it down to its baseline, the
	// ridge in the waterfall colour (dimmer further back), joined to the next column
	//
	static int ytop[WF3D_W];
	static float tcol[WF3D_W];

	for (int k = g_wf3d_n - 1; k >= 0; k--) {
		const uint8_t *row = g_wf3d[(g_wf3d_head - k + WF3D_N) % WF3D_N];
		float s = 1.0f / (1.0f + zstep * (float) k);
		float base = yh + (yf - yh) * s;
		float fade = 1.0f - 0.65f * (float) k / (float) (WF3D_N - 1);
		int bi = (int) lroundf(base), prev = -1;

		for (int x = 0; x < w; x++)
			ytop[x] = -1;

		for (int x = 0; x < w; x++) {
			if (row[x] == 255)
				continue;

			float t = (float) row[x] / 254.0f;
			int xs = (int) lroundf(cx + ((float) x - cx) * s);
			int top = (int) lroundf(base - t * amp * s);

			top = top < y0 ? y0 : top;

			if (xs >= 0 && xs < w && (ytop[xs] < 0 || top < ytop[xs])) {
				ytop[xs] = top;
				tcol[xs] = t;
			}
		}

		for (int x = 0; x < w; x++) {
			if (ytop[x] < 0) {
				prev = -1;
				continue;
			}

			int a = prev < 0 ? ytop[x] : prev < ytop[x] ? prev : ytop[x];
			int b = prev < 0 ? ytop[x] : prev > ytop[x] ? prev : ytop[x];

			if (ytop[x] < bi)
				x6100_lcd_vline(lcd, x, ytop[x] + 1, bi, 0x000000);

			x6100_lcd_vline(lcd, x, a, b, rgb_dim(wf_colour(tcol[x]), fade));
			prev = ytop[x];
		}
	}

	// While tuning (and TUNE_LINE_MS after): the VFO from the front to the back of the floor,
	// in front of the ridges, as the tune line of the flat waterfall (2026-11-07)
	// and on, along the same direction, up to the top of the waterfall area: through the
	// back of the floor towards the scope, as asked (2026-11-07: it stopped at the back)
	if (tune && cursor_x >= 0 && cursor_x < w) {
		float xb = cx + ((float) cursor_x - cx) * sb;
		float xt = (float) cursor_x + (xb - (float) cursor_x) * (yf - (float) y0) / (yf - yb);

		x6100_lcd_line(lcd, cursor_x, yfi, (int) lroundf(xt), y0, 2, SCR_WARN);
	}

	if (cursor_x >= 0 && cursor_x < w)	/* the VFO on the front line */
		x6100_lcd_vline(lcd, cursor_x, yfi - 6, yfi, SCR_WARN);
}

//
// Band check off (--oob / conf oob) and the TX frequency outside the region's allocations:
// OOB in red under the top left of the arc meter, in RX and TX, landscape and portrait
// (2026-11-07: before RX only, landscape only)
//

static void draw_oob(x6100_lcd_t *lcd, const rx_t *rx)
{
	if (g_allow_oob && !in_allocation(tx_hz(rx)))
		x6100_lcd_text(lcd, VU_X + 8, VU_Y + 24, 2, "OOB", SCR_WARN);
}

//
// Scope markers as on the IC-7851 (2026-11-07): M (red) the VFO in use (RIT included), S
// (green) the other VFO, T (yellow) the TX frequency. Each a dashed line (solid colour;
// half-transparent was too faint on the radio) with a
// lettered tag under the frequency labels; lines on the same column dashed in their
// colours (M and T together: red / yellow, the usual case; the tag then T only, as the
// IC-7851), a single line dashed with gaps. A marker outside the view: an
// arrow at that edge in its colour (the IC-7851's << for the sub VFO). Columns 0 .. sw - 1.
//

#define MARK_M		0xE03030
#define MARK_S		0x3CC83C
#define MARK_T		0xF0D020

static void draw_markers(x6100_lcd_t *lcd, const rx_t *rx, const view_t *v, int sw)
{
	const uint32_t col[3] = { MARK_M, MARK_S, MARK_T };
	const char *const name[3] = { "M", "S", "T" };
	static const int prio[3] = { 2, 0, 1 };	/* the tag a shared column shows: T, then M, then S */
	const int64_t hz[3] = { (int64_t) rx->freq + (rx->rit ? rx->rit_hz : 0), rx->b_freq, tx_hz(rx) };
	const int tag_w = 16, tag_h = 17, row_y = SPEC_Y + 20;
	int x[3], tag_x[3], ntag = 0;

	for (int i = 0; i < 3; i++)
		x[i] = f_to_x(v, hz[i]);

	//
	// Lines and tags, in tag order: the markers on one column (within a pixel) are one
	// group with one line, dashed 6 px a colour (a single marker: 6 px on, 3 off), and
	// one tag; a tag that would cover one already drawn drops a row
	//
	for (int p = 0; p < 3; p++) {
		int i = prio[p], n = 0, row = 0;
		uint32_t on[3];
		bool lead = true;

		if (x[i] < LBL_W || x[i] >= sw)
			continue;

		for (int q = 0; q < 3; q++) {
			int k = prio[q];

			if (abs(x[k] - x[i]) > 1)
				continue;

			lead = lead && q >= p;	/* the group is drawn by its first in tag order */
			on[n++] = col[k];
		}

		if (!lead)
			continue;

		for (int y = SPEC_Y; y < SPEC_Y + SPEC_H; y += n == 1 ? 9 : 6) {
			int y1 = y + 5 < SPEC_Y + SPEC_H - 1 ? y + 5 : SPEC_Y + SPEC_H - 1;

			x6100_lcd_vline(lcd, x[i], y, y1, on[(y - SPEC_Y) / 6 % n]);	/* solid dashes: the gaps show the signal */
		}

		int tx = x[i] - tag_w / 2;

		tx = tx < LBL_W ? LBL_W : tx + tag_w > sw ? sw - tag_w : tx;

		for (int k = 0; k < ntag; k++)
			row += abs(tag_x[k] - tx) < tag_w + 2 ? 1 : 0;

		tag_x[ntag++] = tx;
		x6100_lcd_fill(lcd, tx, row_y + row * (tag_h + 2), tag_w, tag_h, col[i]);
		x6100_lcd_text(lcd, tx + (tag_w - x6100_lcd_text_width(name[i], 2)) / 2, row_y + row * (tag_h + 2) + 2, 2,
			name[i], 0x000000);
	}

	//
	// Out of view: an arrow at that edge in the marker's colour, one slot down per marker
	//
	for (int i = 0; i < 3; i++) {
		if (x[i] >= LBL_W && x[i] < sw)
			continue;

		const char *a = x[i] < LBL_W ? "<<" : ">>";
		int ay = SPEC_Y + SPEC_H / 2 - 16 + i * 16, ax = x[i] < LBL_W ? LBL_W + 2 : sw - 4 - x6100_lcd_text_width(a, 2);

		x6100_lcd_fill(lcd, ax - 1, ay - 1, x6100_lcd_text_width(a, 2) + 2, 16, 0x000000);
		x6100_lcd_text(lcd, ax, ay, 2, a, col[i]);
	}
}


//
// Screenshot (2026-11-07): console / pipe `shot` or LOCK long. The canvas as drawn (the
// screen as seen, portrait upright) as a PNG in /tmp. Uncompressed (deflate stored
// blocks): no zlib needed, about 1.1 MB for 800 x 480, opens anywhere.
//


static uint32_t png_crc(uint32_t crc, const uint8_t *b, size_t n)
{
	static uint32_t table[256];

	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;

			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;

			table[i] = c;
		}
	}

	crc = ~crc;

	for (size_t i = 0; i < n; i++)
		crc = table[(crc ^ b[i]) & 0xFF] ^ crc >> 8;

	return ~crc;
}

static void png_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

// One chunk: length, type, data, CRC over type and data
static bool png_chunk(FILE *fp, const char *type, const uint8_t *d, size_t n)
{
	uint8_t h[8], c[4];
	uint32_t crc = png_crc(0, (const uint8_t *) type, 4);

	png_be32(h, (uint32_t) n);
	memcpy(h + 4, type, 4);
	crc = png_crc(crc, d, n);
	png_be32(c, crc);

	return fwrite(h, 1, 8, fp) == 8 && (n == 0 || fwrite(d, 1, n, fp) == n) && fwrite(c, 1, 4, fp) == 4;
}

// The canvas as an RGB PNG at path; false on an error
static bool png_write(const x6100_lcd_t *lcd, const char *path)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	const size_t row = (size_t) lcd->w * 3 + 1, raw = row * (size_t) lcd->h;
	const size_t nblk = (raw + 65534) / 65535, zlen = 2 + raw + nblk * 5 + 4;
	uint8_t ihdr[13] = { 0 }, *z = malloc(zlen), *r = malloc(row);
	uint32_t a = 1, b = 0;	/* Adler-32 of the raw rows */
	size_t o = 0, left = raw, y = 0, x = row;
	FILE *fp;
	bool ok;

	if (z == NULL || r == NULL || (fp = fopen(path, "wb")) == NULL) {
		free(z);
		free(r);
		return false;
	}

	png_be32(ihdr, (uint32_t) lcd->w);
	png_be32(ihdr + 4, (uint32_t) lcd->h);
	ihdr[8] = 8;	/* bit depth */
	ihdr[9] = 2;	/* RGB */

	z[o++] = 0x78;	/* zlib header: deflate, no dictionary */
	z[o++] = 0x01;

	// Stored blocks of at most 65535 bytes, filled from the rows (filter 0 + RGB)
	while (left > 0) {
		size_t n = left > 65535 ? 65535 : left;

		z[o++] = left == n ? 1 : 0;
		z[o++] = (uint8_t) n;
		z[o++] = (uint8_t) (n >> 8);
		z[o++] = (uint8_t) ~n;
		z[o++] = (uint8_t) (~n >> 8);

		for (size_t i = 0; i < n; i++) {
			if (x == row) {
				const uint32_t *p = lcd->pix + y * (size_t) lcd->w;

				r[0] = 0;

				for (int k = 0; k < lcd->w; k++) {
					r[1 + k * 3] = (uint8_t) (p[k] >> 16);
					r[2 + k * 3] = (uint8_t) (p[k] >> 8);
					r[3 + k * 3] = (uint8_t) p[k];
				}

				x = 0;
				y++;
			}

			z[o] = r[x++];
			a = (a + z[o]) % 65521;
			b = (b + a) % 65521;
			o++;
		}

		left -= n;
	}

	png_be32(z + o, b << 16 | a);
	o += 4;

	ok = fwrite(sig, 1, 8, fp) == 8 && png_chunk(fp, "IHDR", ihdr, 13) && png_chunk(fp, "IDAT", z, o)
		&& png_chunk(fp, "IEND", NULL, 0);
	ok = fclose(fp) == 0 && ok;
	free(z);
	free(r);
	return ok;
}

// Take the screenshot: /tmp/x6100_shot_<UTC>.png; says where on the console / log
static void shot_take(const x6100_lcd_t *lcd)
{
	char path[64];
	time_t now = time(NULL);
	struct tm tm;

	gmtime_r(&now, &tm);
	strftime(path, sizeof(path), "/tmp/x6100_shot_%Y%m%dT%H%M%SZ.png", &tm);

	if (png_write(lcd, path)) {
		printf("\nscreenshot: %s\n", path);
		toast("SCREENSHOT");
	} else {
		fprintf(stderr, "\nscreenshot: %s not written\n", path);
		toast("SCREENSHOT FAILED");
	}
}
// A panel or page open: they are drawn for landscape, so portrait turns back meanwhile
static bool panel_open(const rx_t *rx)
{
	return rx->fedit || rx->dedit || rx->tedit || rx->eedit || rx->medit || rx->kedit || rx->xedit || rx->sedit
		|| rx->msedit || rx->apedit || rx->gedit || rx->ginfo || rx->redit;
}

//
// Portrait layout: everything above the scope and the footer under the waterfall (the
// scope and waterfall are the landscape code's, at P_SPEC_Y). See g_layout_portrait.
//

static void draw_portrait(screen_t *scr, const rx_t *rx)
{
	x6100_lcd_t *lcd = &scr->lcd;
	const int W = lcd->w, red = 0xE02020;
	bool on = scr->tx || rx->tuning;
	char s[64];

	x6100_lcd_fill(lcd, 0, 0, W, SPEC_Y, SCR_BG);
	x6100_lcd_fill(lcd, 0, lcd->h - TAG_H, W, TAG_H, SCR_BG);

	//
	// Status line: the full width red while transmitting (TUNE while the tuner searches),
	// RX in green otherwise; band, VFO; battery on the right
	//
	if (on)
		x6100_lcd_fill(lcd, 0, 0, W, P_BAR_H, red);

	x6100_lcd_text(lcd, 12, 6, 3, rx->tuning ? "TUNE" : scr->tx ? "TX" : "RX", on ? 0xFFFFFF : 0x3CC83C);
	snprintf(s, sizeof(s), "%s  %s", rx->band >= 0 ? g_bands[rx->band].name : "GEN",
		rx->mem_mode ? "M" : rx->on_b ? "B" : "A");
	x6100_lcd_text(lcd, 110, 10, 2, s, on ? 0xFFFFFF : SCR_DIM);

	{
		uint32_t c = on ? 0xFFFFFF : scr->ext ? 0x3CC83C : scr->batcap > 0 && scr->batcap < 10 ? SCR_WARN
			: scr->batcap > 0 && scr->batcap < 20 ? 0xFFB020 : SCR_DIM;

		snprintf(s, sizeof(s), "%.1fV %d%%", scr->vbat, scr->batcap);
		x6100_lcd_text(lcd, W - 12 - x6100_lcd_text_width(s, 2), 10, 2, s, c);

		// WiFi left of the battery, BT left of that while Bluetooth is on (2026-11-07)
		{
			int wx = W - 12 - x6100_lcd_text_width(s, 2) - 22;

			draw_wifi(lcd, wx, 25, g_wifi_bars);

			if (g_bt_power == 1)
				x6100_lcd_text(lcd, wx - 22 - x6100_lcd_text_width("BT", 2), 10, 2, "BT", on ? 0xFFFFFF : SCR_ACCENT);
		}
	}

	//
	// Arc meter, the landscape one moved to the top centre: S in RX, the F4 meter in TX
	//
	g_vu_x = (W - VU_W) / 2;
	g_vu_y = P_VU_Y;

	if (scr->tx != scr->was_tx || rx->meter != scr->was_meter) {
		scr->vu_peak = 0.0;
		scr->was_tx = scr->tx;
		scr->was_meter = rx->meter;
	}

	if (scr->tx) {
		char l1[16], l2[24];
		int alc = scr->alc >= 100 ? 0 : scr->alc;
		double target = rx->meter == 1 ? swr_frac(scr->swr) : rx->meter == 2 ? alc / 100.0 * 0.34 : scr->pwr / 10.0 * 0.97;
		vu_scale_t scale = rx->meter == 1 ? VU_SCALE_SWR : rx->meter == 2 ? VU_SCALE_ALC : VU_SCALE_PO;

		snprintf(l1, sizeof(l1), rx->meter == 1 ? "SWR %.1f" : "%.1fW", rx->meter == 1 ? scr->swr : scr->pwr);
		snprintf(l2, sizeof(l2), rx->meter == 1 ? "%.1fW" : "SWR %.1f", rx->meter == 1 ? scr->pwr : scr->swr);
		vu_update(scr, target, now_ms());
		draw_vu(lcd, scr->needle, scr->vu_peak, scale, l1, l2, 0);

		bool src_warn;	/* the TX audio source, as in landscape */
		const char *src = tx_source(rx, &src_warn);

		x6100_lcd_text(lcd, VU_X + 8, VU_Y + 6, 2, src, src_warn ? SCR_WARN : SCR_ACCENT);
		draw_oob(lcd, rx);
	} else {
		char l1[16], l2[16];

		vu_update(scr, meter_frac(scr->sig_dbm), now_ms());
		s_reading(scr->sig_dbm, l1, sizeof(l1));
		snprintf(l2, sizeof(l2), "%4.0fdBm", scr->sig_dbm);
		draw_vu(lcd, scr->needle, scr->vu_peak, VU_SCALE_S, l1, l2, rx->sql);

		if (now_ms() < scr->ovf_until)
			x6100_lcd_text(lcd, VU_X + 8, VU_Y + 6, 2, "OVF", SCR_WARN);

		draw_oob(lcd, rx);
	}

	//
	// Frequency (the TX frequency while keyed with split or a duplex shift), centred
	//
	{
		uint32_t f = rx->send == SEND_SCAN && rx->tx_hz_ovr != 0 ? rx->tx_hz_ovr
			: !on ? rx->freq : rx->split ? rx->b_freq : (uint32_t) ((int64_t) rx->freq + dup_shift(rx));

		// Always three digits after the kHz, so the width does not change with the step: the
		// Hz digit with the 1 Hz step (FST long), else a 0 there (2026-11-07; before, two
		// digits and the line jumped when the 1 Hz step came on)
		snprintf(s, sizeof(s), "%u.%03u.%03u", f / 1000000, f / 1000 % 1000,
			rx->step < 10 ? f % 1000 : f % 1000 / 10 * 10);
		x6100_lcd_text_aa(lcd, &x6100_font_freq, (W - x6100_lcd_text_aa_width(&x6100_font_freq, s)) / 2,
			P_FREQ_Y, s, SCR_FREQ);
	}

	//
	// Mode, filter, step; then AGC and the switches that are on, and split / duplex
	//
	{
		char st[16];

		if (rx->step == STEP_CH)
			snprintf(st, sizeof(st), "CH");
		else if (rx->step >= 1000 && rx->step % 1000 == 0)
			snprintf(st, sizeof(st), "%dK", rx->step / 1000);
		else
			snprintf(st, sizeof(st), "%d", rx->step);

		// Fixed widths (the font is monospaced), so a new mode or step does not move the
		// line: centred on a width that stays the same (2026-11-05)
		snprintf(s, sizeof(s), "%-5s  FIL%d  STEP %-4s", mode_name(rx->mode), rx->fil + 1, st);
		x6100_lcd_text(lcd, (W - x6100_lcd_text_width(s, 3)) / 2, P_INFO_Y, 3, s, SCR_ACCENT);
	}

	{
		int n = snprintf(s, sizeof(s), "AGC %s", agc_name(rx->agc));

		n += snprintf(s + n, sizeof(s) - (size_t) n, "%s%s%s%s%s", rx->att ? "  ATT" : "", rx->pre ? "  PRE" : "",
			rx->nb ? "  NB" : "", rx->nr ? "  NR" : "", rx->atu_on ? "  ATU" : "");

		if (rx->split)
			snprintf(s + n, sizeof(s) - (size_t) n, "  SPLIT");
		else if (dup_shift(rx) != 0)
			snprintf(s + n, sizeof(s) - (size_t) n, "  DUP%c %dK", dup_shift(rx) < 0 ? '-' : '+', abs(dup_shift(rx)) / 1000);

		x6100_lcd_text(lcd, (W - x6100_lcd_text_width(s, 2)) / 2, P_NOTE_Y, 2, s, SCR_DIM);
	}

	//
	// Footer in fixed columns, so nothing moves (2026-11-05): the lock (or the auto lock
	// count-down), then RF gain, squelch and volume, each always there. The one the VOL
	// knob turns now (VOL long) in accent, the volume amber when muted, RF gain brighter
	// below 100
	//
	{
		static const int col[3] = { 156, 264, 372 };	/* RFG, SQL, VOL: 7 characters at 12 px */
		const int y = lcd->h - TAG_H + 12;
		const int val[3] = { rx->rfg, rx->sql, rx->vol };
		static const char *name[3] = { "RFG", "SQL", "VOL" };

		if (rx->lock) {
			x6100_lcd_text(lcd, 12, y, 2, "LOCK", SCR_WARN);
		} else if (g_autolock_s > 0) {
			uint64_t idle = now_ms() - g_last_turn_ms;
			int left = idle >= (uint64_t) g_autolock_s * 1000u ? 0 : g_autolock_s - (int) (idle / 1000u);

			snprintf(s, sizeof(s), "LOCK IN %d", left);
			x6100_lcd_text(lcd, 12, y, 2, s, SCR_DIM);
		}

		for (int i = 0; i < 3; i++) {
			int knob = i == 0 ? 2 : i == 1 ? 1 : 0;		/* g_vol_fn of this field */
			uint32_t c = g_vol_fn == knob ? (knob == 0 && rx->mute ? SCR_WARN : g_vol_fn != 0 ? SCR_ACCENT : SCR_FG)
				: i == 2 && rx->mute ? SCR_WARN : i == 0 && rx->rfg < 100 ? SCR_FG : SCR_DIM;

			snprintf(s, sizeof(s), "%s %3d", name[i], val[i]);
			x6100_lcd_text(lcd, col[i], y, 2, s, c);
		}
	}
}

//
// Tune line (2026-11-07): while tuning, the VFO (RX) frequency as a line through the
// flat waterfall, as the cursor on the scope; gone TUNE_LINE_MS after the last change of
// the frequency. The waterfall lives in the frame buffer and scrolls in place, so the
// line is drawn last and the pixels under it are put back first thing in the next frame.
//

#define TUNE_LINE_MS	10000

static struct
{
	int x, y0, h, w;	/* where it was drawn (x -1: not drawn), the canvas width then */
	uint32_t px[800];	/* the waterfall pixels under it */
} g_tl = { .x = -1 };

static uint64_t g_tl_until;	/* shown until (ms) */
static uint32_t g_tl_freq;	/* frequency seen last; 0 at start: the first is not a tune */

static void tune_line_restore(x6100_lcd_t *lcd)
{
	if (g_tl.x >= 0 && lcd->w == g_tl.w)
		for (int y = 0; y < g_tl.h; y++)
			lcd->pix[(size_t) (g_tl.y0 + y) * (size_t) lcd->w + (size_t) g_tl.x] = g_tl.px[y];

	g_tl.x = -1;
}

static void tune_line_draw(x6100_lcd_t *lcd, int x, int y0, int h)
{
	h = h > 800 ? 800 : h;

	if (x < 0 || x >= lcd->w || h <= 0)
		return;

	for (int y = 0; y < h; y++)
		g_tl.px[y] = lcd->pix[(size_t) (y0 + y) * (size_t) lcd->w + (size_t) x];

	g_tl.x = x;
	g_tl.y0 = y0;
	g_tl.h = h;
	g_tl.w = lcd->w;
	x6100_lcd_vline(lcd, x, y0, y0 + h - 1, SCR_WARN);
}

static void draw_screen(screen_t *scr, const rx_t *rx, bool new_line)
{
	x6100_lcd_t *lcd = &scr->lcd;
	int W = lcd->w;
	char s[64];
	const bool portrait = lcd->h > lcd->w;

	tune_line_restore(lcd);	/* the waterfall under last frame's tune line, before anything moves it */

	// A changed frequency shows the tune line for TUNE_LINE_MS (flat and 3D waterfall)
	if (rx->freq != g_tl_freq) {
		g_tl_until = g_tl_freq != 0 ? now_ms() + TUNE_LINE_MS : 0;
		g_tl_freq = rx->freq;
	}

	//
	// Layout geometry: the scope under the landscape top, or lower in portrait
	//
	g_spec_y = portrait ? P_SPEC_Y : 180;
	g_tag_h = portrait ? P_FOOT_H : 20;
	g_vu_x = 500;
	g_vu_y = 4;

	//
	// Scope panel: waterfall off (cleared once, no new lines) or slower (a line on
	// every 4th / 2nd new frame: SLOW / MID)
	//
	static const int wf_every[3] = { 4, 2, 1 };
	static unsigned wf_frames;
	static bool wf_shown = true;

	if (!g_sp[SP_WF]) {
		if (wf_shown)
			x6100_lcd_fill(lcd, 0, WF_Y, W, WF_H(lcd), 0x000000);

		new_line = false;
	} else if (new_line) {
		new_line = wf_frames++ % (unsigned) wf_every[g_sp[SP_WF_SPEED]] == 0;
	}

	wf_shown = g_sp[SP_WF] != 0;

	// 3D on or off: a fresh waterfall area either way
	static bool wf3d_was;
	const bool wf3d = g_sp[SP_WF] && g_sp[SP_WF_3D];

	if (wf3d != wf3d_was) {
		x6100_lcd_fill(lcd, 0, WF_Y, W, WF_H(lcd), 0x000000);
		wf3d_clear();
		wf3d_was = wf3d;
	}

	// Waterfall size: the scope above gives or takes the rows (small waterfall = tall scope)
	static const int spec_h[3] = { 190, 150, 110 };
	int want_h = portrait ? P_SPEC_H : spec_h[g_sp[SP_WF_SIZE]];	/* portrait: a small scope */

	if (g_spec_h != want_h) {
		g_spec_h = want_h;
		x6100_lcd_fill(lcd, 0, SPEC_Y, W, lcd->h - TAG_H - SPEC_Y, 0x000000);
		scr->wf_scope = (scope_mode_t) -1;	/* waterfall re-aligns */
		wf3d_clear();
	}

	if (portrait) {
		draw_portrait(scr, rx);
		goto scope;
	}

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

	// VFO in use: A or B right of the TX box; in memory mode M and the channel (amber)
	if (rx->mem_mode) {
		char mc[8];

		snprintf(mc, sizeof(mc), "M%d", rx->medit_ch);
		x6100_lcd_text(lcd, 72, FREQ_Y + 12, 3, "M", SCR_WARN);
		x6100_lcd_text(lcd, 72 + x6100_lcd_text_width("M", 3) + 2, FREQ_Y + 24, 2, mc + 1, SCR_WARN);
	} else {
		x6100_lcd_text(lcd, 72, FREQ_Y + 12, 3, rx->on_b ? "B" : "A", SCR_ACCENT);
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
	// Frequency, MHz.kHz.Hz; while transmitting with split or a duplex shift the TX
	// frequency (without XIT, as on Icom radios), the scope stays on RX; during the SWR
	// scan the frequency it steps over (2026-11-01: the dial stood still)
	//
	{
		bool on = scr->tx || rx->tuning;
		uint32_t f = rx->send == SEND_SCAN && rx->tx_hz_ovr != 0 ? rx->tx_hz_ovr
			: !on ? rx->freq : rx->split ? rx->b_freq : (uint32_t) ((int64_t) rx->freq + dup_shift(rx));

		snprintf(s, sizeof(s), "%u.%03u.%03u", f / 1000000, f / 1000 % 1000, f % 1000);
	}
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
	// The VOL column shows what the VOL knob turns now (VOL long: SQL / RFG, accent)
	if (g_vol_fn == 1)
		snprintf(s, sizeof(s), "SQL %3d", rx->sql);
	else if (g_vol_fn == 2)
		snprintf(s, sizeof(s), "RFG*%3d", rx->rfg);
	else
		snprintf(s, sizeof(s), "VOL %3d", rx->vol);

	x6100_lcd_text(lcd, INFO_COL_VOL, INFO_Y, 2, s, g_vol_fn != 0 ? SCR_ACCENT : rx->mute ? SCR_WARN : SCR_DIM);

	if (rx->mfk_pwr) {
		snprintf(s, sizeof(s), "PWR %2d.%dW", rx->txpwr / 10, rx->txpwr % 10);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_ACCENT);
	} else {
		snprintf(s, sizeof(s), "RFG %3d", rx->rfg);
		x6100_lcd_text(lcd, INFO_COL_RFG, INFO_Y, 2, s, SCR_DIM);
	}

	//
	// Battery: voltage, coloured by charge: green on external power (charging below
	// 100 %), amber under 20 %, red under 10 %; status page (GEN long) has the details
	//
	{
		uint32_t c = scr->ext ? 0x3CC83C : scr->batcap > 0 && scr->batcap < 10 ? SCR_WARN
			: scr->batcap > 0 && scr->batcap < 20 ? 0xFFB020 : SCR_DIM;

		snprintf(s, sizeof(s), "%4.1fV", scr->vbat);
		x6100_lcd_text(lcd, INFO_COL_VBAT, INFO_Y, 2, s, c);
		draw_wifi(lcd, INFO_COL_VBAT + 74, INFO_Y + 13, g_wifi_bars);	/* WiFi right of the battery */
	}
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
			rx->meter == 2 ? scr->vu_peak / 0.34 : scr->vu_peak, true, scr->swr, rx->meter, 0);
		draw_vu(lcd, scr->needle, marker, scale, l1, l2, 0);
		x6100_lcd_text(lcd, VU_X + 8, VU_Y + VU_H - 20, 2, meter_names[rx->meter], SCR_ACCENT);

		// Where the TX audio comes from, top left of the dial (MIC HAND in red)
		bool src_warn;
		const char *src = tx_source(rx, &src_warn);

		x6100_lcd_text(lcd, VU_X + 8, VU_Y + 6, 2, src, src_warn ? SCR_WARN : SCR_ACCENT);
		draw_oob(lcd, rx);
	} else {
		char l1[16], l2[16];

		vu_update(scr, meter_frac(scr->sig_dbm), now_ms());
		draw_sbar(lcd, meter_frac(scr->sig_dbm), scr->vu_peak, false, 1.0, 0, rx->sql);
		s_reading(scr->sig_dbm, l1, sizeof(l1));
		snprintf(l2, sizeof(l2), "%4.0fdBm", scr->sig_dbm);
		draw_vu(lcd, scr->needle, scr->vu_peak, VU_SCALE_S, l1, l2, rx->sql);

		// Overload (flow IQ at its ceiling), top left of the dial
		if (now_ms() < scr->ovf_until)
			x6100_lcd_text(lcd, VU_X + 8, VU_Y + 6, 2, "OVF", SCR_WARN);

		draw_oob(lcd, rx);

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

scope:
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
		wf3d_clear();
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
	if (rx->scope != scr->wf_scope || scope_width(rx) != scr->wf_span
		|| (rx->scope == SCOPE_FIX && (double) v.lo != scr->wf_ref)) {	/* FIX: another edge */
		x6100_lcd_fill(lcd, 0, WF_Y, SW, WF_H(lcd), 0x000000);
		scr->wf_scope = rx->scope;
		wf3d_clear();
		scr->wf_span = scope_width(rx);
		scr->wf_ref = (double) v.lo;
	} else if (rx->scope == SCOPE_CENTER_S || rx->scope == SCOPE_FIX_S) {
		int dx = (int) lround((scr->wf_ref - (double) v.lo) * SW / v.span);

		if (dx != 0) {
			wf_shift(lcd, WF_Y, WF_H(lcd), dx, SW);
			wf3d_shift(dx, SW);
			scr->wf_ref -= (double) dx * v.span / SW;
		}
	} else {
		scr->wf_ref = (double) v.lo;
	}

	int pb_lo, pb_hi;

	passband(rx, &pb_lo, &pb_hi);

	{
		int x0 = f_to_x(&v, (int64_t) pb_ref(rx) + pb_lo);	/* receive passband, RIT included */
		int x1 = f_to_x(&v, (int64_t) pb_ref(rx) + pb_hi);

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
		trace_col(lcd, x, y, py, prev_y, false);
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
	// Band edges of the selected region (the TX allocations of the TX check): a red bar
	// along the top over what lies outside them, a dashed red line at each edge in view
	//
	{
		const region_t *rg = &g_regions[g_region];

		for (int x = LBL_W; x < SW; x++) {
			int64_t f = v.lo + (int64_t) x * v.span / v.w;

			if (f > 0 && !in_allocation((uint32_t) f))
				x6100_lcd_vline(lcd, x, SPEC_Y, SPEC_Y + 2, BAND_OUT);
		}

		for (int i = 0; i < SEG_MAX && rg->seg[i].high != 0; i++) {
			const uint32_t edge[2] = { rg->seg[i].low, rg->seg[i].high };

			for (int k = 0; k < 2; k++) {
				int ex = f_to_x(&v, edge[k]);

				if (ex <= LBL_W || ex >= SW - 1)
					continue;

				for (int y = SPEC_Y; y < SPEC_Y + SPEC_H; y += 6)
					x6100_lcd_vline(lcd, ex, y, y + 2 < SPEC_Y + SPEC_H ? y + 2 : SPEC_Y + SPEC_H - 1, BAND_OUT);
			}
		}
	}

	//
	// M (the VFO in use), S (the other VFO) and T (TX) markers, as the IC-7851; out of
	// view an arrow at the edge
	//
	draw_markers(lcd, rx, &v, SW);

	//
	// Birdies in view (BIRDIE MARKS): a short yellow tick at the top of the RF scope
	//
	if (g_bmarks) {
		for (int i = 0; i < g_nbirdie; i++) {
			int bx = f_to_x(&v, g_birdie[i].hz);

			if (bx > LBL_W && bx < SW - 1)
				x6100_lcd_fill(lcd, bx, SPEC_Y, 2, 10, 0xE0E040);
		}
	}

	//
	// 3D waterfall: a new line into the history, then all of it in perspective; drawn
	// before the panels, which may lie over the waterfall area. Only redrawn for a new
	// line, a shift or clear, a moved VFO cursor, a panel opened or closed (it costs CPU)
	//
	static int wf3d_cx = -1;
	static bool wf3d_panel;
	const int cur_x = f_to_x(&v, rx->freq);
	const bool pnl = panel_open(rx);

	static bool wf3d_tune;
	const bool tune = now_ms() < g_tl_until;

	if (wf3d && (new_line || g_wf3d_dirty || cur_x != wf3d_cx || pnl != wf3d_panel || tune != wf3d_tune)) {
		wf3d_tune = tune;
		g_wf3d_dirty = false;
		wf3d_cx = cur_x;
		wf3d_panel = pnl;

		if (new_line) {
			float t[WF3D_W];
			int n = SW > WF3D_W ? WF3D_W : SW;

			for (int x = 0; x < n; x++)
				t[x] = spec_index(&v, x) < 0 ? -1.0f
					: (spec_at(scr, &v, x) - (scr->floor_db - WF_BELOW_DB)) / WF_RANGE_DB;

			wf3d_push(t, n);
		}

		wf3d_draw(lcd, WF_Y, WF_H(lcd), SW, cur_x, tune);
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
	// Split / duplex / RIT / XIT in use: top left of the spectrum, amber
	//
	if (tx_apart(rx)) {
		static const char *mn[] = { [X6100_MODE_LSB] = "LSB", [X6100_MODE_USB] = "USB", [X6100_MODE_CW] = "CW",
			[X6100_MODE_CWR] = "CWR", [X6100_MODE_AM] = "AM", [X6100_MODE_NFM] = "NFM" };
		char m[48] = "";
		int n = 0;
		uint32_t f = tx_hz(rx);

		if (rx->split)
			n += snprintf(m + n, sizeof(m) - (size_t) n, "SPLIT TX %u.%03u.%02u %s  ", f / 1000000, f / 1000 % 1000,
				f % 1000 / 10, mn[tx_mode(rx)] ? mn[tx_mode(rx)] : "");
		else if (dup_shift(rx) != 0)
			n += snprintf(m + n, sizeof(m) - (size_t) n, "DUP%c TX %u.%03u.%02u  ", dup_shift(rx) < 0 ? '-' : '+',
				f / 1000000, f / 1000 % 1000, f % 1000 / 10);

		if (rx->rit || rx->xit)
			snprintf(m + n, sizeof(m) - (size_t) n, "%s%s%s %+d", rx->rit ? "RIT" : "", rx->rit && rx->xit ? "/" : "",
				rx->xit ? "XIT" : "", rx->rit_hz);

		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 22, x6100_lcd_text_width(m, 2) + 8, 18, 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 24, 2, m, 0xFFB020);

	//
	// Birdie scan running: progress under the split note, cyan
	//
	if (bscan_running()) {
		char m[48];

		bscan_text(m, sizeof(m));
		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 44, x6100_lcd_text_width(m, 2) + 8, 18, 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 46, 2, m, 0x40C0FF);
	}
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
		char m[40];
		int tw;

		if (rx->ascope)
			snprintf(m, sizeof(m), "AUDIO %s", ranges[rx->arange]);
		else if (scope_fixed(rx))	/* FIX / SCROLL-F: the edge and its window, kHz */
			snprintf(m, sizeof(m), "%s E%d %u-%u", names[rx->scope], rx->edge, (unsigned) (rx->fix_lo / 1000),
				(unsigned) ((rx->fix_lo + rx->fix_w) / 1000));
		else if (rx->span % 2000 == 0)
			snprintf(m, sizeof(m), "%s +-%dk", names[rx->scope], rx->span / 2000);
		else
			snprintf(m, sizeof(m), "%s +-%d.%dk", names[rx->scope], rx->span / 2000, rx->span / 200 % 10);

		if (g_scope_hold && !rx->ascope)
			snprintf(m + strlen(m), sizeof(m) - strlen(m), " HOLD");

		tw = x6100_lcd_text_width(m, 2);

		x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 20, tw + 8, 18, 0x000000);
		x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 22, 2, m, SCR_ACCENT);
	}

	//
	// FIX with the receive frequency outside the window, as an Icom: SCOPE OUT OF RANGE,
	// the arrows on the side the VFO is (the window then has no IQ: the LO is near the
	// VFO). SCROLL-F moves the window along, so never there.
	//
	if (view != 1 && rx->scope == SCOPE_FIX && rx->fix_w > 0) {
		int64_t f = rx_hz(rx);

		if (f < rx->fix_lo || f >= rx->fix_lo + rx->fix_w) {
			const char *m = f < rx->fix_lo ? "<< SCOPE OUT OF RANGE" : "SCOPE OUT OF RANGE >>";
			int tw = x6100_lcd_text_width(m, 2), y = SPEC_Y + SPEC_H / 2 - 8;

			x6100_lcd_fill(lcd, (SW - tw) / 2 - 6, y - 4, tw + 12, 22, 0x000000);
			x6100_lcd_text(lcd, (SW - tw) / 2, y, 2, m, SCR_WARN);
		}
	}

	//
	// Filter label under the scope mode: FILn and its width
	//
	if (!portrait) {	/* portrait: in the info lines, or left out */
		uint32_t lo, hi;
		char m[32], w[12];
		int tw;

		rx_filter(rx, &lo, &hi);	/* the width with twin PBT */
		fil_width_str(hi - lo, w, sizeof(w));
		snprintf(m, sizeof(m), "FIL%d %s%s", rx->fil + 1, w, pbt_active(rx) ? " PBT" : "");
		tw = x6100_lcd_text_width(m, 2);
		x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 40, tw + 8, 18, 0x000000);
		x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 42, 2, m, rx->fedit ? SCR_WARN : SCR_ACCENT);
	}

	//
	// Clock under the filter label: system time, UTC, ISO 8601; amber while not known
	// to be right (time_ok). Drawn before the panels, which cover it while open.
	//
	if (!portrait) {	/* portrait: in the info lines, or left out */
		time_t now = time(NULL);
		struct tm tm;
		char m[32];
		int tw;

		if (gmtime_r(&now, &tm) != NULL && strftime(m, sizeof(m), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0) {
			tw = x6100_lcd_text_width(m, 2);
			x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 60, tw + 8, 18, 0x000000);
			x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 62, 2, m, time_ok() ? SCR_DIM : SCR_WARN);
		}
	}

	//
	// LAN (a wfview client through wfserver) and CAT (CI-V traffic in the last 3 s), right
	// aligned under the clock, as the LAN icon on an Icom
	//
	if (!portrait) {	/* portrait: in the info lines, or left out */
		// wfserver polls the radio over the pty with no client too (2026-11-03): pty
		// traffic counts only without it; a client through wfserver shows as LAN
		uint64_t ms = now_ms();
		bool dev = g_cat_rx_ms[CAT_EP_DEV] != 0 && ms - g_cat_rx_ms[CAT_EP_DEV] < CAT_SEEN_MS;
		bool pty = g_cat_rx_ms[CAT_EP_PTY] != 0 && ms - g_cat_rx_ms[CAT_EP_PTY] < CAT_SEEN_MS && !g_wfs_run;
		bool cat = dev || pty;
		const char *m = g_lan && cat ? "LAN  CAT" : g_lan ? "LAN" : cat ? "CAT" : NULL;

		if (m != NULL) {
			int tw = x6100_lcd_text_width(m, 2);

			x6100_lcd_fill(lcd, W - tw - 10, SPEC_Y + 80, tw + 8, 18, 0x000000);
			x6100_lcd_text(lcd, W - tw - 6, SPEC_Y + 82, 2, m, g_lan ? 0x3CC83C : SCR_ACCENT);
		}
	}

	//
	// Toast: a short confirmation in the middle of the spectrum (VFO A / B, A=B), drawn
	// after the labels on the right (they cut AUDIO SCOPE: CODEC BUSY short, 2026-11-01)
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
	// Baseband watchdog: no flow (and so no new scope lines), red in the middle of the
	// waterfall, with what to do; the screen is redrawn every second meanwhile
	//
	// The box sits in the waterfall area, so it is part of the waterfall: when the baseband
	// answers again the whole waterfall (stale since the hang) is cleared, else the box
	// scrolled down with the new lines (2026-11-07)
	static bool bb_was_dead;

	if (bb_was_dead && !g_bb_dead) {
		x6100_lcd_fill(lcd, 0, WF_Y, W, WF_H(lcd), 0x000000);
		wf3d_clear();
	}

	bb_was_dead = g_bb_dead;

	if (g_bb_dead) {
		static const char *l1 = "BASEBAND NOT RESPONDING", *l2 = "MFK PUSH: BBRESET, ELSE A POWER CYCLE";
		int w1 = x6100_lcd_text_width(l1, 3), w2 = x6100_lcd_text_width(l2, 2);
		int bw = (w1 > w2 ? w1 : w2) + 28, y = WF_Y + 20;

		x6100_lcd_fill(lcd, (W - bw) / 2, y, bw, 60, 0x400000);
		x6100_lcd_text(lcd, (W - w1) / 2, y + 10, 3, l1, 0xFF4040);
		x6100_lcd_text(lcd, (W - w2) / 2, y + 38, 2, l2, SCR_FG);
	}

	if (rx->fedit)
		draw_filter_panel(lcd, rx);

	if (rx->dedit)
		draw_dsp_panel(lcd, rx);

	if (rx->tedit)
		draw_rit_panel(lcd, rx);

	if (rx->eedit)
		draw_edge_panel(lcd, rx);

	if (rx->medit)
		draw_mem_panel(lcd, rx);

	if (rx->kedit)
		draw_key_panel(lcd, rx);

	if (rx->xedit)
		draw_tx_panel(lcd, rx);

	if (rx->sedit)
		draw_scope_panel(lcd, rx);

	if (rx->msedit)
		draw_msg_panel(lcd, rx);

	if (rx->apedit)
		draw_app_panel(lcd, rx);

	if (rx->gedit)
		draw_gen_panel(lcd, rx);

	if (rx->ginfo)
		draw_gen_info(lcd, scr);

	if (!portrait && (rx->tuning || (rx->tune_done && now_ms() - rx->tune_done < TUNE_SHOW_MS)))	/* portrait: TUNE in the bar */
		draw_tune_panel(lcd, rx, scr);

	//
	// SWR scan curve over the RF scope (APP): SWR 1 at the bottom, 4 at the top, grid
	// at 1.5, 2 and 3; the lowest point marked; until a key is pressed
	//
	if (g_scan.shown) {
		const int x0 = LBL_W + 4, x1 = SW - 6, y0 = SPEC_Y + 4, y1 = SPEC_Y + SPEC_H - 4;
		int best = -1, prev_x = -1, prev_y = -1;
		char m[64];

		x6100_lcd_fill(lcd, 0, SPEC_Y, SW, SPEC_H, 0x05080C);

		for (int g = 0; g < 3; g++) {
			static const float gs[3] = { 1.5f, 2.0f, 3.0f };
			int gy = y1 - (int) ((gs[g] - 1.0f) / 3.0f * (float) (y1 - y0));

			snprintf(m, sizeof(m), "%.1f", gs[g]);
			x6100_lcd_hline(lcd, x0, x1, gy, 0x1A2430);
			x6100_lcd_text(lcd, x1 - x6100_lcd_text_width(m, 1) - 2, gy - 9, 1, m, SCR_DIM);
		}

		for (int i = 0; i < SCAN_STEPS; i++) {
			float s = g_scan.swr[i];

			if (s < 1.0f)
				continue;

			int x = x0 + (x1 - x0) * i / (SCAN_STEPS - 1);
			int y = y1 - (int) ((s > 4.0f ? 3.0f : s - 1.0f) / 3.0f * (float) (y1 - y0));

			if (prev_x >= 0)
				for (int k = prev_x; k <= x; k++)
					x6100_lcd_fill(lcd, k, prev_y + (y - prev_y) * (k - prev_x) / (x - prev_x > 0 ? x - prev_x : 1), 2, 2,
						s > SWR_LIMIT ? SCR_WARN : 0x3CC83C);

			prev_x = x;
			prev_y = y;
			best = best < 0 || s < g_scan.swr[best] ? i : best;
		}

		if (best >= 0) {
			int bx = x0 + (x1 - x0) * best / (SCAN_STEPS - 1);

			x6100_lcd_vline(lcd, bx, y0, y1, SCR_ACCENT);
			snprintf(m, sizeof(m), "SWR %.1f AT %.3f", g_scan.swr[best], scan_hz(best) / 1000.0);
			x6100_lcd_text(lcd, x0 + 40, y0 + 2, 2, m, SCR_ACCENT);
		}

		snprintf(m, sizeof(m), "%.0f - %.0f KHZ%s", g_scan.lo / 1000.0, g_scan.hi / 1000.0,
			rx->send == SEND_SCAN ? "  SCANNING" : "");
		x6100_lcd_text(lcd, x1 - x6100_lcd_text_width(m, 2), y1 - 18, 2, m, SCR_FG);
	}

	//
	// Sending a message: its number (and the repeat countdown)
	//
	if (rx->send == SEND_MSG || g_send.repeat) {
		char m[32];
		uint64_t t = now_ms();

		if (rx->send == SEND_MSG)
			snprintf(m, sizeof(m), "%s%.0d", g_send.msg == 0 ? "CAT CW" : "MSG ", g_send.msg);
		else
			snprintf(m, sizeof(m), "MSG %d AGAIN IN %d S", g_send.msg,
				(int) ((g_send.next_at > t ? g_send.next_at - t : 0) + 999) / 1000);

		int tw = x6100_lcd_text_width(m, 2);

		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 46, tw + 8, 20, rx->send == SEND_MSG ? 0xC02020 : 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 49, 2, m, rx->send == SEND_MSG ? 0xFFFFFF : SCR_WARN);
	}

	//
	// Last TX refusal or trip, for WARN_MS or until the next TX
	//
	if (rx->warn != NULL) {
		int tw = x6100_lcd_text_width(rx->warn, 2);

		x6100_lcd_fill(lcd, LBL_W + 4, SPEC_Y + 22, tw + 8, 20, 0x000000);
		x6100_lcd_text(lcd, LBL_W + 8, SPEC_Y + 25, 2, rx->warn, SCR_WARN);
	}

	//
	// Waterfall, newest line on top; no data outside the IQ span stays black. In 3D the
	// RF part is drawn above (every frame); only the audio part of F3 long scrolls here.
	//
	if (new_line && view != 1) {
		if (!wf3d) {
			x6100_lcd_scroll_down(lcd, WF_Y, WF_H(lcd));

			for (int x = 0; x < SW; x++) {
				uint32_t c = spec_index(&v, x) < 0 ? 0x000000
					: wf_colour((spec_at(scr, &v, x) - (scr->floor_db - WF_BELOW_DB)) / WF_RANGE_DB);

				x6100_lcd_fill(lcd, x, WF_Y, 1, 1, c);
			}
		} else if (view == 2) {
			wf_scroll_cols(lcd, WF_Y, WF_H(lcd), SW, W - SW);
		}

		if (view == 2)
			draw_audio_wf_line(scr, rx, SW, W - SW);
	}

	// Tune line in the flat waterfall, not under a panel (3D: drawn by wf3d_draw)
	if (view != 1 && g_sp[SP_WF] && !wf3d && !panel_open(rx) && now_ms() < g_tl_until) {
		int cx = f_to_x(&v, tx_apart(rx) ? rx_hz(rx) : rx->freq);

		if (cx >= 0 && cx < SW)
			tune_line_draw(lcd, cx, WF_Y, WF_H(lcd));
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

//
// Help: the start options (x6100_trx --help / -h / help / ?) and the console commands
// (help / ? on the console). In short; all of it in docs/x6100_trx_quickref.md
//

static void help_options(FILE *fp, const char *argv0)
{
	fprintf(fp, "usage: %s [options] [freq] [mode] [agc] [att|noatt] [pre|nopre] [vol 0-100]\n"
		"       [layout portrait|landscape]\n\n"
		"options:\n"
		"  --oob             band check off (you are responsible for where you transmit)\n"
		"  --no-limit        tune 10 kHz - 100 MHz (baseband probe)\n"
		"  --cat[=baud|pty]  CI-V on the DEV port (4800-19200), or the pty only\n"
		"  --cat-log         print CI-V commands and refusals\n"
		"  --probe           print the flow flags and IQ peak (overflow hunt)\n"
		"  --bbreset         reset the baseband first (when it does not answer on I2C)\n"
		"  --help, -h, ?     this text\n\n"
		"freq 7074000 | 7.074 | 7074k   mode usb lsb cw cwr am nfm   agc off slow fast auto\n"
		"While running: type help on the console for its commands.\n", argv0);
}

static void help_console(void)
{
	printf("\nconsole commands (tokens can be combined: 14074000 usb):\n"
		"  tune      7074000 | 7.074 | 7074k   usb lsb cw cwr am nfm   fil1 fil2 fil3\n"
		"            step 1|10|100|1000|5000|9000|10000|100000|ch   ab   a=b   vfo a|b\n"
		"  receive   off slow fast auto   att noatt pre nopre   rfg 0-100   vol 0-100   mute nomute\n"
		"            nr nonr nb nonb notch nonotch   nrl nbl nbw ntf ntw <n>   pbtin pbtout <Hz>\n"
		"            agcslow agcfast <x10 ms>   agchang on|off   ifshift <Hz>   am_offset <Hz>\n"
		"  transmit  pwr 0.1-10   atu noatu   meter po|swr|alc   txsplit notxsplit\n"
		"            rit norit xit noxit ritofs <Hz>   dupauto dupoff dup- dup+ dupshift <kHz>\n"
		"            mic int|hand|auto imicg hmicg comp complvl vox voxg moni monilvl linein lineout sql tot\n"
		"  scope     center scrollc fix scrollf   span <Hz>   edge 1-4   ref aref <dB>   rf af split\n"
		"            af3 af6   hold on|off   fixedge <range> <edge> <lo> <hi>\n"
		"  memory    memr [n]   memw n   memc n   mem n <Hz> <mode> fil<n> [...] [name text]\n"
		"  messages  msg   msg n cw <text> | voice <file> | clear   msgsend n   msgstop   msgrepeat <s>\n"
		"  birdies   birdiescan [all] [quiet]   birdiestop   birdies   birdieclear   birdiemarks   birdienotch\n"
		"  cat       cat <baud>|off   catpty on|off   civaddr a4|61   catmem ro|rw   cattime off|unsynced|on\n"
		"            modin mic|usb mic|usb\n"
		"  system    layout portrait|landscape   portraitrot 0|180   autolock <s>   bright 5-100\n"
		"            region r1|r2|r3|nl|uk   freqmin freqmax <Hz>   time <UTC>   ntp on|off|<server>\n"
		"            vfofast on|off   bt on|off   btauto <MAC>|off   bbreset   oob   nolimit\n"
		"  help, ?   this list (all of it: docs/x6100_trx_quickref.md)\n");
}

static bool console_token(rx_t *rx, const char *t)
{
	if (strcasecmp(t, "help") == 0 || strcmp(t, "?") == 0) {
		help_console();
		return true;
	}

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

	// Birdies: ticks on the scope, automatic notch
	if (strcasecmp(t, "birdiemarks") == 0 || strcasecmp(t, "nobirdiemarks") == 0) {
		g_bmarks = strcasecmp(t, "birdiemarks") == 0;
		return true;
	}

	if (strcasecmp(t, "birdienotch") == 0 || strcasecmp(t, "nobirdienotch") == 0) {
		g_bnotch = strcasecmp(t, "birdienotch") == 0;
		return true;
	}

	// Repeater duplex: dupauto (10 m repeaters), dupoff, dup- / dup+ by dupshift
	for (int i = 0; i < DUP_COUNT; i++) {
		if (strcasecmp(t, g_dup_tok[i]) == 0) {
			rx->dup = i;
			return true;
		}
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

	// shot: a screenshot of the screen as it is, PNG in /tmp (also LOCK long)
	if (strcasecmp(t, "shot") == 0) {
		g_shot_req = true;
		return false;
	}

	// bbreset: reset the baseband (GPIO line) and send it everything again (main loop)
	if (strcasecmp(t, "bbreset") == 0) {
		g_bb_reset_req = true;
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

			if (st == 1 || st == 10 || st == 100 || st == 1000 || st == 5000 || st == 9000 || st == 10000
				|| st == 100000 || st == STEP_CH) {
				rx->step = st;
				changed = true;
			} else {
				fprintf(stderr, "\nstep must be 1, 10, 100, 1000, 5000, 10000, 100000 (9000 on LW / MW)\n");
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

		// cat 19200 | cat off: CI-V on ttyS2 (4800-19200: the most the DEV port carries;
		// read at start only). cat pty: the old one-or-the-other choice, now the DEV port off
		if (strcasecmp(t, "cat") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int b = v == NULL ? 1 : strcasecmp(v, "off") == 0 || strcasecmp(v, "pty") == 0 ? 0 : atoi(v);

			if (b == 0 || b == 4800 || b == 9600 || b == 19200) {
				g_cat_baud = b;
				g_conf_set |= g_conf_reading ? 1u << GEN_CAT : 0;
			} else {
				fprintf(stderr, "\ncat: off, 4800, 9600 or 19200 (the pty is catpty)\n");
			}

			continue;
		}

		// catpty on | off: CI-V on the pty as well (default on; read at start only)
		if (strcasecmp(t, "catpty") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0))
				g_cat_pty = strcasecmp(v, "on") == 0;
			else
				fprintf(stderr, "\ncatpty: on or off\n");

			continue;
		}

		// civaddr a4 | 61 (hex) | ic705 | x6100: CI-V address; 61 needs the wfview rig file
		// contrib/wfview/X6100.rig (GEN row CI-V ADDRESS too; in the conf it fixes that row)
		if (strcasecmp(t, "civaddr") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			char *end = NULL;
			long a = v == NULL ? -1 : strcasecmp(v, "ic705") == 0 ? X6100_CIV_ADDR_IC705
				: strcasecmp(v, "x6100") == 0 ? X6100_CIV_ADDR_X6100 : strtol(v, &end, 16);

			if (a > 0x00 && a < X6100_CIV_ADDR_CTRL && (end == NULL || *end == '\0')) {
				g_civ_addr = (int) a;
				g_conf_set |= g_conf_reading ? 1u << GEN_CIVADDR : 0;	/* the conf fixes the GEN row */
			} else {
				fprintf(stderr, "\ncivaddr: a4 (IC-705), 61 (X6100) or 01-DF hex\n");
			}

			continue;
		}


		// layout landscape | portrait (GEN row LAYOUT; the conf fixes it), portraitrot 0 |
		// 180 (which side is up; 180: the left side), autolock <s> (portrait: the VFO / MFK
		// lock after s seconds without a turn or key, 0 off; default 20)
		if (strcasecmp(t, "layout") == 0 || strcasecmp(t, "portraitrot") == 0 || strcasecmp(t, "autolock") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			char *end = NULL;
			long n = v != NULL ? strtol(v, &end, 10) : -1;

			if (strcasecmp(t, "layout") == 0 && v != NULL && (strcasecmp(v, "landscape") == 0 || strcasecmp(v, "portrait") == 0)) {
				g_layout_portrait = strcasecmp(v, "portrait") == 0;
				g_conf_set |= g_conf_reading ? 1u << GEN_LAYOUT : 0;	/* the conf fixes the GEN row */
			} else if (strcasecmp(t, "portraitrot") == 0 && v != NULL && *end == '\0' && (n == 0 || n == 180)) {
				g_layout_rot = (int) n;
			} else if (strcasecmp(t, "autolock") == 0 && v != NULL && *end == '\0' && n >= 0 && n <= 600) {
				g_autolock_s = (int) n;
			} else {
				fprintf(stderr, "\nlayout landscape|portrait, portraitrot 0|180, autolock 0-600 (s)\n");
			}

			changed = true;
			continue;
		}

		// vfofast on | off: faster tuning when the VFO knob turns fast (GEN row VFO SPEED;
		// the conf fixes it)
		if (strcasecmp(t, "vfofast") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0)) {
				g_vfo_fast = strcasecmp(v, "on") == 0;
				g_conf_set |= g_conf_reading ? 1u << GEN_VFOFAST : 0;	/* the conf fixes the GEN row */
			} else {
				fprintf(stderr, "\nvfofast on|off\n");
			}

			changed = true;
			continue;
		}

		// freqmin / freqmax <Hz>: the tuning range, within 5 kHz - 75 MHz while the limits
		// the baseband takes are found out; default 148.5 kHz - 75 MHz (conf, console, state)
		if (strcasecmp(t, "freqmin") == 0 || strcasecmp(t, "freqmax") == 0) {
			bool lo = strcasecmp(t, "freqmin") == 0;
			const char *v = strtok(NULL, " \t\r\n");
			char *end = NULL;
			unsigned long hz = v != NULL ? strtoul(v, &end, 10) : 0;

			if (v == NULL || *end != '\0' || hz < FREQ_LIM_LO || hz > FREQ_LIM_HI
				|| (lo ? hz >= g_freq_max : hz <= g_freq_min)) {
				fprintf(stderr, "\n%s: %u - %u Hz, freqmin below freqmax (now %u - %u)\n", t, FREQ_LIM_LO,
					FREQ_LIM_HI, (unsigned) g_freq_min, (unsigned) g_freq_max);
				continue;
			}

			if (lo)
				g_freq_min = (uint32_t) hz;
			else
				g_freq_max = (uint32_t) hz;

			if (!g_conf_loading)
				set_freq(rx, rx->freq);	/* console: the VFO into the new range */

			changed = true;
			continue;
		}

		// catmem ro | rw: may CAT (wfview, rigctl, any client on the DEV port or the pty)
		// write and clear memory channels (09, 0B, 1A 00 with content)? Default ro: reading
		// and using them (08, 0A, 1A 00) only. GEN row CAT MEMORIES too; the conf fixes it.
		if (strcasecmp(t, "catmem") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "ro") == 0 || strcasecmp(v, "rw") == 0)) {
				g_cat_mem_rw = strcasecmp(v, "rw") == 0;
				g_conf_set |= g_conf_reading ? 1u << GEN_CATMEM : 0;	/* the conf fixes the GEN row */
			} else {
				fprintf(stderr, "\ncatmem: ro or rw\n");
			}

			continue;
		}

		// cattime off | unsynced | on: may CAT set the clock (1A 05 0165 / 0166 / 0170,
		// wfview Set Clock)? Default unsynced: only while NTP does not keep it. GEN row CAT
		// CLOCK too; the conf fixes it.
		if (strcasecmp(t, "cattime") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int c = v == NULL ? -1 : strcasecmp(v, "off") == 0 ? CT_OFF : strcasecmp(v, "unsynced") == 0
				? CT_UNSYNCED : strcasecmp(v, "on") == 0 ? CT_ON : -1;

			if (c >= 0) {
				g_cat_time = c;
				g_conf_set |= g_conf_reading ? 1u << GEN_CATTIME : 0;	/* the conf fixes the GEN row */
			} else {
				fprintf(stderr, "\ncattime: off, unsynced or on\n");
			}

			continue;
		}

		// modin <mic|usb|lan> <mic|usb|lan>: TX audio input for DATA OFF MOD and DATA MOD (GEN
		// rows too; in the conf it fixes them). usb: the codec, played by a Linux program
		// (wfserver); lan: the codec when CAT keys (wfview), the mic for the PTT key
		if (strcasecmp(t, "modin") == 0) {
			const char *v[2] = { strtok(NULL, " \t\r\n"), NULL };
			int m[2] = { -1, -1 };

			v[1] = strtok(NULL, " \t\r\n");

			for (int i = 0; i < 2; i++)
				for (int k = 0; k < 3 && v[i] != NULL; k++)
					m[i] = strcasecmp(v[i], g_modin_word[k]) == 0 ? k : m[i];

			if (m[0] >= 0 && m[1] >= 0) {
				g_modin[0] = m[0];
				g_modin[1] = m[1];
				g_conf_set |= g_conf_reading ? 1u << GEN_MODOFF | 1u << GEN_MODDATA : 0;
			} else {
				fprintf(stderr, "\nmodin: <mic|usb|lan> <mic|usb|lan> (data off, data)\n");
			}

			continue;
		}

		// time <UTC>: set the clock by hand (console only, not from the conf or state)
		if (strcasecmp(t, "time") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (g_conf_loading)
				continue;

			if (time_set(v))
				fprintf(stderr, "\ntime: set\n");
			else
				fprintf(stderr, "\ntime: 2026-10-31T10:15:00Z or 10:15[:00] (UTC), from 2026 on\n");

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

		// Keyer: keytype straight|paddle|rev, wpm, iambic a|b, cwpitch, cwvol, qsk, ratio 3.0
		{
			int k = -1;

			for (int i = 0; i < KPARS; i++)
				k = strcasecmp(t, g_kpar[i].token) == 0 ? i : k;

			if (k >= 0) {
				const char *vs = strtok(NULL, " \t\r\n");
				int v = -1;

				if (vs == NULL)
					v = -1;
				else if (k == 0)
					v = strcasecmp(vs, "straight") == 0 ? 0 : strcasecmp(vs, "paddle") == 0 ? 1
						: strcasecmp(vs, "rev") == 0 ? 2 : atoi(vs);
				else if (k == 2)
					v = strcasecmp(vs, "a") == 0 ? 0 : strcasecmp(vs, "b") == 0 ? 1 : atoi(vs);
				else if (k == 6 && strchr(vs, '.') != NULL)
					v = (int) lround(atof(vs) * 10.0);
				else
					v = atoi(vs);

				if (vs == NULL || v < g_kpar[k].min || v > g_kpar[k].max)
					fprintf(stderr, "\n%s: %d to %d\n", t, g_kpar[k].min, g_kpar[k].max);
				else
					*kpar_value(rx, k) = v;

				changed = true;
				continue;
			}
		}

		// TX audio: mic int|hand|auto, imicg, hmicg, comp on|off, complvl 1:2|1:4|1:8,
		// vox on|off, voxg, antivox, voxdly, moni on|off, monilvl, sql
		{
			int k = -1;

			for (int i = 0; i < XPARS; i++)
				k = strcasecmp(t, g_xpar[i].token) == 0 ? i : k;

			if (k >= 0) {
				int v = xpar_parse(k, strtok(NULL, " \t\r\n"));

				if (v < 0 && k == XP_MIC)
					fprintf(stderr, "\nmic: int, hand or auto\n");
				else if (v < 0 && k == XP_CMPLVL)
					fprintf(stderr, "\ncomplvl: 1:2, 1:4 or 1:8\n");
				else if (v < 0 && k == XP_TOT)
					fprintf(stderr, "\ntot: off, 1, 3 or 5 (minutes)\n");
				else if (v < 0 && xpar_onoff(k))
					fprintf(stderr, "\n%s: on or off\n", t);
				else if (v < 0)
					fprintf(stderr, "\n%s: %d to %d\n", t, g_xpar[k].min, g_xpar[k].max);
				else
					*xpar_value(rx, k) = v;

				changed = true;
				continue;
			}
		}

		// Scope panel: ctype filter|carrier, vbw narrow|wide, scavg off|2|3|4, wave fill|line|fill+line,
		// maxhold off|decay|10s|on, fillr .. holdb 0-255, wf on|off, wfspeed slow|mid|fast,
		// wfsize small|mid|large, wflevel 30-80, txscope on|off
		{
			int k = -1;

			for (int i = 1; i < SPARS; i++)
				k = strcasecmp(t, g_spar[i].token) == 0 ? i : k;

			if (k >= 0) {
				int v = spar_parse(k, strtok(NULL, " \t\r\n"));
				char w[48];

				spar_words(k, w, sizeof(w));

				if (v < 0 && k == SP_AVG)
					fprintf(stderr, "\nscavg: off, 2, 3 or 4\n");
				else if (v < 0 && k == SP_WF_LEVEL)
					fprintf(stderr, "\nwflevel: 30 to 80 dB in steps of 10\n");
				else if (v < 0 && w[0] != '\0')
					fprintf(stderr, "\n%s: %s\n", t, w);
				else if (v < 0)
					fprintf(stderr, "\n%s: %d to %d\n", t, g_spar[k].min, g_spar[k].max);
				else
					spar_set(rx, k, v);

				g_af_seen |= v >= 0 && k >= SP_AF_WAVE;

				changed = true;
				continue;
			}
		}

		// GEN band memory: gen <Hz> <lsb|usb|cw|cwr|am|nfm>
		if (strcasecmp(t, "gen") == 0) {
			static const char *const names[] = { [X6100_MODE_LSB] = "lsb", [X6100_MODE_USB] = "usb",
				[X6100_MODE_CW] = "cw", [X6100_MODE_CWR] = "cwr", [X6100_MODE_AM] = "am", [X6100_MODE_NFM] = "nfm" };
			const char *fs = strtok(NULL, " \t\r\n"), *ms = fs == NULL ? NULL : strtok(NULL, " \t\r\n");
			uint32_t f = fs == NULL ? 0 : (uint32_t) strtoul(fs, NULL, 10);
			int m = -1;

			for (int i = 0; ms != NULL && i < (int) (sizeof(names) / sizeof(names[0])); i++)
				m = names[i] != NULL && strcasecmp(ms, names[i]) == 0 ? i : m;

			if (f >= FREQ_MIN && f <= FREQ_MAX && m >= 0) {
				rx->gen_freq = f;
				rx->gen_mode = (x6100_mode_t) m;
			} else {
				fprintf(stderr, "\ngen <Hz> <lsb|usb|cw|cwr|am|nfm>\n");
			}

			continue;
		}

		// TX meter on the dial (F4): meter po|swr|alc
		if (strcasecmp(t, "meter") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int m = v == NULL ? -1 : strcasecmp(v, "po") == 0 ? 0 : strcasecmp(v, "swr") == 0 ? 1
				: strcasecmp(v, "alc") == 0 ? 2 : -1;

			if (m >= 0)
				rx->meter = m;
			else
				fprintf(stderr, "\nmeter: po, swr or alc\n");

			changed = true;
			continue;
		}

		// Messages (messages.conf): msg (list), msg <n> cw <text>, msg <n> voice <file.wav>,
		// msg <n> clear, msgrepeat <s>, msgsend <n>, msgstop; swrscan
		if (strcasecmp(t, "msg") == 0) {
			const char *ns = strtok(NULL, " \t\r\n"), *ty = ns == NULL ? NULL : strtok(NULL, " \t\r\n");
			const char *text = ty == NULL ? NULL : strtok(NULL, "\r\n");
			int n = ns == NULL ? 0 : atoi(ns);

			if (ns == NULL) {
				for (int i = 1; i <= MSG_COUNT; i++)
					printf("\nmsg %d %s %s", i, g_msg[i].type == MSG_CW ? "cw" : g_msg[i].type == MSG_VOICE ? "voice"
						: "-", g_msg[i].text);

				printf("\nmsgrepeat %d\n", g_msg_repeat);
			} else if (n < 1 || n > MSG_COUNT || ty == NULL) {
				fprintf(stderr, "\nmsg <1-%d> cw <text> | voice <file.wav> | clear\n", MSG_COUNT);
			} else if (strcasecmp(ty, "clear") == 0) {
				g_msg[n].type = MSG_EMPTY;
				g_msg[n].text[0] = '\0';
				msg_save();
			} else if ((strcasecmp(ty, "cw") == 0 || strcasecmp(ty, "voice") == 0) && text != NULL) {
				while (*text == ' ' || *text == '\t')
					text++;

				g_msg[n].type = strcasecmp(ty, "cw") == 0 ? MSG_CW : MSG_VOICE;
				snprintf(g_msg[n].text, sizeof(g_msg[n].text), "%s", text);

				if (g_msg[n].type == MSG_VOICE && strchr(text, '/') != NULL)
					g_msg[n].type = MSG_EMPTY;	/* a file name in VOICE_DIR, no path */

				if (g_msg[n].type == MSG_EMPTY)
					fprintf(stderr, "\nmsg: voice files live in %s, give the name only\n", VOICE_DIR);

				msg_save();
			} else {
				fprintf(stderr, "\nmsg <1-%d> cw <text> | voice <file.wav> | clear\n", MSG_COUNT);
			}

			continue;
		}

		if (strcasecmp(t, "msgrepeat") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int s = v == NULL ? 0 : atoi(v);

			if (s >= 1 && s <= 600) {
				g_msg_repeat = s;
				msg_save();
			} else {
				fprintf(stderr, "\nmsgrepeat: 1 to 600 s\n");
			}

			continue;
		}

		if (strcasecmp(t, "msgsend") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (!g_conf_loading && v != NULL)
				msg_start(rx, atoi(v));

			changed = true;
			continue;
		}

		if (strcasecmp(t, "msgstop") == 0) {
			send_stop(rx);
			changed = true;
			continue;
		}

		if (strcasecmp(t, "swrscan") == 0) {
			if (!g_conf_loading)
				scan_start(rx);

			changed = true;
			continue;
		}

		// Birdies: birdie <Hz> <dB> (birdies.conf), birdies (list), birdiescan [all],
		// birdiestop, birdieclear (all stored ones)
		if (strcasecmp(t, "birdie") == 0) {
			const char *hs = strtok(NULL, " \t\r\n"), *ds = strtok(NULL, " \t\r\n");
			uint32_t hz = hs == NULL ? 0 : (uint32_t) strtoul(hs, NULL, 10);

			if (hz < FREQ_MIN || hz > FREQ_MAX || ds == NULL)
				fprintf(stderr, "\nbirdie: <Hz> <dB over the noise>\n");
			else if (atoi(ds) >= BS_SNR_DB)	/* weaker ones of older scans: noise, left out */
				birdie_add(hz, atoi(ds));

			continue;
		}

		if (strcasecmp(t, "birdies") == 0) {
			for (int i = 0; i < g_nbirdie; i++) {
				int b = band_of(g_birdie[i].hz);

				printf("%12.3f kHz  %3d dB  %s\n", g_birdie[i].hz / 1000.0, g_birdie[i].snr, b >= 0 ? g_bands[b].name : "");
			}

			printf("%d birdies stored; marks %s, notch %s\n\n", g_nbirdie, g_bmarks ? "on" : "off", g_bnotch ? "on" : "off");
			continue;
		}

		// birdiescan [all] [quiet]: quiet switches WiFi and Bluetooth off for the scan
		// (they could be the source), and WiFi back on when it is done
		if (strcasecmp(t, "birdiescan") == 0) {
			g_bscan_all = false;
			g_bscan_quiet = false;

			for (int k = 0; k < 2; k++) {
				const char *as = strtok(NULL, " \t\r\n");

				if (as == NULL)
					break;

				if (strcasecmp(as, "all") == 0)
					g_bscan_all = true;
				else if (strcasecmp(as, "quiet") == 0)
					g_bscan_quiet = true;
				else
					fprintf(stderr, "\nbirdiescan [all] [quiet]: %s?\n", as);
			}

			g_bscan_req = 1;
			continue;
		}

		if (strcasecmp(t, "birdiestop") == 0) {
			g_bscan_req = -1;
			continue;
		}

		if (strcasecmp(t, "birdieclear") == 0) {
			g_nbirdie = 0;
			birdie_save();
			printf("birdies cleared\n");
			continue;
		}

		// mem <n> <Hz> <mode> fil<n> [tx <Hz> <mode>] [name <text>]: memory channel
		// (memories.conf; typed in the console it is saved at once)
		if (strcasecmp(t, "mem") == 0) {
			const char *ns = strtok(NULL, " \t\r\n"), *fs = strtok(NULL, " \t\r\n");
			const char *ms = strtok(NULL, " \t\r\n"), *ls = strtok(NULL, " \t\r\n");
			int n = ns == NULL ? 0 : atoi(ns);
			uint32_t hz = fs == NULL ? 0 : (uint32_t) strtoul(fs, NULL, 10);
			static rx_t tmp;
			mem_t m = { .used = true };
			char *w;

			tmp = *rx;

			if (n < 1 || n > MEM_COUNT || hz < FREQ_MIN || hz > FREQ_MAX || ms == NULL || !console_token(&tmp, ms)
				|| ls == NULL || strncasecmp(ls, "fil", 3) != 0 || ls[3] < '1' || ls[3] > '3') {
				fprintf(stderr, "\nmem: <1-99> <Hz> <mode> fil1|2|3 [tx <Hz> <mode>] [dupoff | dup- <kHz> | dup+ <kHz>] [name <text>]\n");
				break;	/* the rest of the line belongs to this token */
			}

			m.hz = hz;
			m.mode = tmp.mode;
			m.fil = ls[3] - '1';

			while ((w = strtok(NULL, " \t\r\n")) != NULL) {
				if (strcasecmp(w, "tx") == 0) {
					const char *th = strtok(NULL, " \t\r\n"), *tm = strtok(NULL, " \t\r\n");

					if (th != NULL && tm != NULL && console_token(&tmp, tm)) {
						m.split = true;
						m.tx_hz = (uint32_t) strtoul(th, NULL, 10);
						m.tx_mode = tmp.mode;
					}
				} else if (strcasecmp(w, "dupoff") == 0) {
					m.dup = DUP_OFF;
				} else if (strcasecmp(w, "dup-") == 0 || strcasecmp(w, "dup+") == 0) {
					const char *ks = strtok(NULL, " \t\r\n");
					int khz = ks == NULL ? 0 : atoi(ks);

					if (khz >= 10 && khz <= DUP_KHZ_MAX) {
						m.dup = w[3] == '-' ? DUP_MINUS : DUP_PLUS;
						m.dup_khz = khz;
					}
				} else if (strcasecmp(w, "name") == 0) {
					const char *rest = strtok(NULL, "\r\n");

					snprintf(m.name, sizeof(m.name), "%s", rest != NULL ? rest : "");
					break;
				}
			}

			g_mem[n] = m;

			if (!g_conf_loading)
				mem_save();

			break;
		}

		// memr / memw / memc <n>: recall, write the VFO into, clear a channel
		if (strcasecmp(t, "memr") == 0 || strcasecmp(t, "memw") == 0 || strcasecmp(t, "memc") == 0) {
			const char *ns = strtok(NULL, " \t\r\n");
			int n = ns == NULL ? 0 : atoi(ns);

			// memr alone: list the used channels
			if (ns == NULL && strcasecmp(t, "memr") == 0) {
				int used = 0;

				for (int i = 1; i <= MEM_COUNT; i++) {
					const mem_t *m = &g_mem[i];

					if (!m->used)
						continue;

					printf("%2d  %10.3f kHz %-4s fil%d", i, m->hz / 1000.0, mode_name(m->mode), m->fil + 1);

					if (m->split)
						printf("  TX %.3f %s", m->tx_hz / 1000.0, mode_name(m->tx_mode));
					else if (m->dup == DUP_MINUS || m->dup == DUP_PLUS)
						printf("  %s %d kHz", m->dup == DUP_MINUS ? "DUP-" : "DUP+", m->dup_khz);
					else if (m->dup == DUP_OFF)
						printf("  simplex");

					printf("%s%s\n", m->name[0] ? "  " : "", m->name);
					used++;
				}

				printf("%d of %d channels used\n\n", used, MEM_COUNT);
				continue;
			}

			if (n < 1 || n > MEM_COUNT) {
				fprintf(stderr, "\n%s: 1 to %d\n", t, MEM_COUNT);
				continue;
			}

			if (strcasecmp(t, "memw") == 0)
				mem_write(rx, n);
			else if (strcasecmp(t, "memc") == 0)
				mem_clear(n);
			else if (!mem_recall(rx, n))
				fprintf(stderr, "\nmemr: channel %d is empty\n", n);

			changed = true;
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

		// region r1|r2|r3|nl|uk: transmit allocations for the TX check and OOB marker
		if (strcasecmp(t, "region") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			int r = -1;

			for (int i = 0; v != NULL && i < REGION_COUNT; i++)
				r = strcasecmp(v, g_regions[i].name) == 0 ? i : r;

			if (r >= 0) {
				g_region = r;
				changed = true;
			} else {
				fprintf(stderr, "\nregion: r1, r2, r3, nl or uk\n");
			}

			continue;
		}

		// bright 5-100: display backlight in % (GEN)
		// btauto <MAC> | off: the Bluetooth headset to connect again (state; GEN BT DEVICE)
		// bt on | off: Bluetooth as last set on the GEN row, applied at start (state)
		if (strcasecmp(t, "bt") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0))
				g_bt_power = strcasecmp(v, "on") == 0;
			else
				fprintf(stderr, "\nbt: on or off\n");

			continue;
		}

		if (strcasecmp(t, "btauto") == 0) {
			const char *v = strtok(NULL, " \t\r\n");
			unsigned b[6];
			char x;

			if (v != NULL && strcasecmp(v, "off") == 0)
				g_btauto[0] = '\0';
			else if (v != NULL && strlen(v) == 17 && sscanf(v, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &x) == 6)
				snprintf(g_btauto, sizeof(g_btauto), "%s", v);
			else
				fprintf(stderr, "\nbtauto: a MAC (70:26:05:C0:19:7B) or off\n");

			continue;
		}

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

		// atumem <Hz> L C S: a tuner memory; atunet <band> L C S (older state files):
		// the same, at the band's default frequency
		if (strcasecmp(t, "atumem") == 0 || strcasecmp(t, "atunet") == 0) {
			bool old = strcasecmp(t, "atunet") == 0;
			const char *fs = strtok(NULL, " \t\r\n");
			const char *ls = strtok(NULL, " \t\r\n"), *cs = strtok(NULL, " \t\r\n"), *ss = strtok(NULL, " \t\r\n");
			uint32_t hz = 0;
			x6100_atu_t net;

			for (int i = 0; old && fs != NULL && i < BAND_COUNT; i++)
				hz = strcasecmp(fs, g_bands[i].name) == 0 ? g_bands[i].def : hz;

			if (!old && fs != NULL)
				hz = (uint32_t) strtoul(fs, NULL, 10);

			if (hz == 0 || ls == NULL || cs == NULL || ss == NULL) {
				fprintf(stderr, "\n%s: <%s> <L> <C> <S>\n", t, old ? "band" : "Hz");
				continue;
			}

			net.l = (uint8_t) atoi(ls);
			net.c = (uint8_t) atoi(cs);
			net.s = (uint8_t) atoi(ss);
			atu_store(hz, &net);
			changed = true;
			continue;
		}

		if (strcasecmp(t, "ritofs") == 0) {
			want_lvl = &rx->rit_hz;
			lvl_min = -RIT_MAX_HZ;
			lvl_max = RIT_MAX_HZ;
			continue;
		}

		// pbtin / pbtout <Hz>: twin PBT shifts, +-1250 (0 = centre; CI-V 14 07 / 08)
		if (strcasecmp(t, "pbtin") == 0 || strcasecmp(t, "pbtout") == 0) {
			want_lvl = strcasecmp(t, "pbtin") == 0 ? &rx->pbt_in : &rx->pbt_out;
			lvl_min = -PBT_MAX_HZ;
			lvl_max = PBT_MAX_HZ;
			continue;
		}

		if (strcasecmp(t, "edge") == 0) {	/* fixed edge in use, 1-4 */
			want_lvl = &rx->edge;
			lvl_min = 1;
			lvl_max = 4;
			continue;
		}

		// fixedge <range 1-12> <edge 1-4> <low Hz> <high Hz>: set a fixed edge (state file,
		// console); fixedge alone: list the edges of the VFO's range
		if (strcasecmp(t, "fixedge") == 0) {
			const char *s[4];

			for (int i = 0; i < 4; i++)
				s[i] = strtok(NULL, " \t\r\n");

			if (s[0] == NULL) {
				int r = edge_range(rx->freq);

				for (int e = 0; r >= 0 && e < 4; e++)
					printf("range %d edge %d: %.3f-%.3f kHz%s\n", r + 1, e + 1, g_edge[r][e].lo / 1000.0,
						g_edge[r][e].hi / 1000.0, e + 1 == rx->edge ? "  (in use)" : "");

				if (r < 0)
					printf("no edge range below %.1f MHz\n", g_edge_bound[0] / 1e6);

				continue;
			}

			int r = atoi(s[0]) - 1, e = s[1] ? atoi(s[1]) - 1 : -1;
			uint32_t lo = s[2] ? (uint32_t) strtoul(s[2], NULL, 10) : 0, hi = s[3] ? (uint32_t) strtoul(s[3], NULL, 10) : 0;

			if (r < 0 || r >= EDGE_RANGES || e < 0 || e > 3 || hi <= lo || hi - lo > EDGE_MAX_HZ) {
				fprintf(stderr, "\nfixedge: <range 1-%d> <edge 1-4> <low Hz> <high Hz>, at most %d kHz wide\n",
					EDGE_RANGES, EDGE_MAX_HZ / 1000);
				continue;
			}

			g_edge[r][e].lo = lo;
			g_edge[r][e].hi = hi;
			changed = true;
			continue;
		}

		if (strcasecmp(t, "dupshift") == 0) {
			want_lvl = &rx->dup_khz;
			lvl_min = 10;
			lvl_max = DUP_KHZ_MAX;
			continue;
		}

		if (strcasecmp(t, "am_offset") == 0 || strcasecmp(t, "amofs") == 0) {	/* amofs: first spelling, 2026-10-26 */
			want_lvl = &g_am_ofs;
			lvl_min = 0;
			lvl_max = AM_OFS_MAX;
			continue;
		}

		// agcslow / agcfast <x10 ms>, agchang on | off: AGC time constants and hang (AGC long)
		if (strcasecmp(t, "agcslow") == 0 || strcasecmp(t, "agcfast") == 0) {
			bool slow = strcasecmp(t, "agcslow") == 0;

			want_lvl = slow ? &g_agc_slow : &g_agc_fast;
			lvl_min = slow ? AGC_SLOW_MIN : AGC_FAST_MIN;
			lvl_max = slow ? AGC_SLOW_MAX : AGC_FAST_MAX;
			continue;
		}

		// agcmid <n>: in state files of 2026-11-04 (the MID / AUTO row, gone): read and dropped
		if (strcasecmp(t, "agcmid") == 0) {
			strtok(NULL, " \t\r\n");
			continue;
		}

		if (strcasecmp(t, "agchang") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0)) {
				g_agc_hang = strcasecmp(v, "on") == 0;
				changed = true;
			} else {
				fprintf(stderr, "\nagchang: on or off\n");
			}

			continue;
		}

		// hold on | off: scope HOLD, as CI-V 27 17 (console; not kept)
		if (strcasecmp(t, "hold") == 0) {
			const char *v = strtok(NULL, " \t\r\n");

			if (v != NULL && (strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0)) {
				g_scope_hold = strcasecmp(v, "on") == 0;
				changed = true;
			} else {
				fprintf(stderr, "\nhold: on or off\n");
			}

			continue;
		}

		if (strcasecmp(t, "ifshift_tx") == 0) {	/* 1: keep the IF shift while keyed (to test) */
			want_lvl = &g_ifshift_tx;
			lvl_min = 0;
			lvl_max = 1;
			continue;
		}

		if (strcasecmp(t, "ifshift") == 0) {	/* IF shift, Hz, 0 = off (register 35) */
			want_lvl = &g_ifshift;
			lvl_min = -IFSHIFT_MAX;
			lvl_max = IFSHIFT_MAX;
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
//   STATE_PATH  last state, written by x6100_trx at exit and after every tune
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
	g_conf_loading = true;


	while (fgets(line, sizeof(line), fp) != NULL) {
		char *hash = strchr(line, '#');

		if (hash != NULL)
			*hash = '\0';

		console_line(rx, line);
	}

	fclose(fp);
	g_conf_reading = false;
	g_conf_loading = false;

	if (!quiet)
		fprintf(stderr, "settings: %s\n", path);
}

static void state_write(const rx_t *rx)
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

	// The tuning range first: the frequencies below are checked against it as they are read
	fprintf(fp, "freqmin %u freqmax %u\n", (unsigned) g_freq_min, (unsigned) g_freq_max);

	for (int i = 0; i < BAND_COUNT; i++)
		fprintf(fp, "band %s %u\n", g_bands[i].name, (unsigned) rx->band_freq[i]);

	fprintf(fp, "gen %u %s\n", (unsigned) rx->gen_freq, modes[rx->band < 0 ? rx->mode : rx->gen_mode]);	/* GEN band */

	for (int i = 0; i < g_natum; i++)
		fprintf(fp, "atumem %u %u %u %u\n", (unsigned) g_atum[i].hz, g_atum[i].net.l, g_atum[i].net.c, g_atum[i].net.s);

	fprintf(fp, "\n");

	for (int g = 0; g < FG_COUNT; g++)
		fprintf(fp, "agc %s %s\n", g_fg_name[g], agcs[rx->agc_group[g]]);

	fprintf(fp, "\n%u %s %s fil%d step %d\n", (unsigned) rx->freq, modes[rx->mode], agcs[rx->agc], rx->fil + 1, rx->step);
	fprintf(fp, "vfob %u %s fil%d vfo %s\n", (unsigned) rx->b_freq, modes[rx->b_mode], rx->b_fil + 1, rx->on_b ? "b" : "a");
	fprintf(fp, "%s %s %s ritofs %d pbtin %d pbtout %d\n", rx->split ? "txsplit" : "notxsplit", rx->rit ? "rit" : "norit",
		rx->xit ? "xit" : "noxit", rx->rit_hz, rx->pbt_in, rx->pbt_out);
	fprintf(fp, "%s dupshift %d am_offset %d ifshift %d ifshift_tx %d agcslow %d agcfast %d agchang %s\n",
		g_dup_tok[rx->dup], rx->dup_khz, g_am_ofs, g_ifshift, g_ifshift_tx, g_agc_slow, g_agc_fast,
		g_agc_hang ? "on" : "off");
	fprintf(fp, "vol %d %s rfg %d pwr %.1f\n", rx->vol, rx->mute ? "mute" : "nomute", rx->rfg, rx->txpwr / 10.0);
	fprintf(fp, "keytype %d wpm %d iambic %d cwpitch %d cwvol %d qsk %d ratio %d.%d\n", rx->keymode, rx->wpm, rx->iambic,
		rx->cwtone, rx->cwvol, rx->qsk, rx->ratio / 10, rx->ratio % 10);

	for (int i = 0; i < XPARS; i++) {
		char v[16];

		xpar_token(rx, i, v, sizeof(v));
		fprintf(fp, "%s %s%s", g_xpar[i].token, v, i == XPARS - 1 ? "\n" : " ");
	}

	// Scope panel, one line each: RF trace, RF colours, waterfall and TX, AF trace, AF colours
	for (int i = 1; i < SPARS; i++) {
		bool eol = i == SP_HOLD || i == SP_HOLD_B || i == SP_TXSCOPE || i == SP_AF_HOLD || i == SPARS - 1;
		char v[16];

		if (g_spar[i].token[0] == '\0')
			continue;	/* REF rows: ref / aref on the scope line */

		spar_token(rx, i, v, sizeof(v));
		fprintf(fp, "%s %s%s", g_spar[i].token, v, eol ? "\n" : " ");
	}

	fprintf(fp, "%s %s\n", g_bmarks ? "birdiemarks" : "nobirdiemarks", g_bnotch ? "birdienotch" : "nobirdienotch");
	fprintf(fp, "%s %s %s\n", rx->att ? "att" : "noatt", rx->pre ? "pre" : "nopre", rx->atu_on ? "atu" : "noatu");
	fprintf(fp, "%s %s %s\n", rx->nr ? "nr" : "nonr", rx->nb ? "nb" : "nonb", rx->dnf ? "notch" : "nonotch");
	fprintf(fp, "nrl %d nbl %d nbw %d ntf %d ntw %d\n", rx->nr_lvl, rx->nb_lvl, rx->nb_w, rx->dnf_f, rx->dnf_w);
	// GEN settings (a server from the conf is not repeated here: the conf sets it again)
	if (g_cat_baud > 0)
		fprintf(fp, "cat %d ", g_cat_baud);
	else
		fprintf(fp, "cat off ");

	fprintf(fp, "civaddr %02x ", g_civ_addr);
	fprintf(fp, "catmem %s ", g_cat_mem_rw ? "rw" : "ro");
	fprintf(fp, "layout %s ", g_layout_portrait ? "portrait" : "landscape");
	fprintf(fp, "vfofast %s ", g_vfo_fast ? "on" : "off");
	fprintf(fp, "cattime %s ", g_cat_time == CT_OFF ? "off" : g_cat_time == CT_ON ? "on" : "unsynced");
	fprintf(fp, "modin %s %s ", g_modin_word[g_modin[0]], g_modin_word[g_modin[1]]);
	fprintf(fp, "ntp %s", strcmp(g_ntp_server, "off") == 0 ? "off" : "on");

	if (g_bright > 0)
		fprintf(fp, " bright %d", g_bright);

	if (g_bt_power >= 0)
		fprintf(fp, " bt %s", g_bt_power ? "on" : "off");

	fprintf(fp, " btauto %s\n", g_btauto[0] != '\0' ? g_btauto : "off");
	fprintf(fp, "%s span %d edge %d %s %s ref %d aref %d\n", scopes[rx->scope], rx->span, rx->edge,
		rx->asplit ? "split" : rx->ascope ? "af" : "rf", rx->arange ? "af6" : "af3", rx->rref, rx->aref);
	fprintf(fp, "meter %s\n", rx->meter == 1 ? "swr" : rx->meter == 2 ? "alc" : "po");	/* F4 */

	// Fixed edges that differ from their default
	for (int r = 0; r < EDGE_RANGES; r++) {
		for (int e = 0; e < 4; e++) {
			edge_t d;

			edge_default(r, e, &d);

			if (g_edge[r][e].lo != d.lo || g_edge[r][e].hi != d.hi)
				fprintf(fp, "fixedge %d %d %u %u\n", r + 1, e + 1, (unsigned) g_edge[r][e].lo, (unsigned) g_edge[r][e].hi);
		}
	}

	// fsync before the rename: also written after a tune, while the power may still go
	bool ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;

	if (fclose(fp) != 0 || !ok || rename(tmp, STATE_PATH) != 0)
		fprintf(stderr, "\nstate: cannot save %s: %s\n", STATE_PATH, strerror(errno));
}

// In memory mode the state keeps the VFO it came from: a restart comes back in VFO
// mode, where V/M left off
static void state_save(const rx_t *rx)
{
	static rx_t t;

	if (!rx->mem_mode) {
		state_write(rx);
		return;
	}

	t = *rx;
	mem_mode_off(&t);
	state_write(&t);
}

static bool console_read(rx_t *rx, int fd, char *buf, size_t size, size_t *len)
{
	bool changed = false;
	char in[64];
	ssize_t n = read(fd, in, sizeof(in));	/* stdin or the command pipe (CMD_FIFO) */

	if (n <= 0)
		return false;

	//
	// With the terminal in character mode (console_raw) we echo the line ourselves,
	// at the end of the status line, so its redraws never wipe what is being typed;
	// piped input arrives in lines and works the same
	//
	for (ssize_t i = 0; i < n; i++) {
		char c = in[i];

		if (c == '\r' || c == '\n') {
			buf[*len] = '\0';
			printf("\r> %s\033[K\n", buf);	/* the typed line stays visible above the output */

			// Locked: the console waits too, except for layout (portrait locks itself) and help
			if (rx->lock && strncasecmp(buf, "layout ", 7) != 0 && strcasecmp(buf, "help") != 0 && strcmp(buf, "?") != 0
				&& strcasecmp(buf, "bbreset") != 0)	/* a hung baseband is reset locked too */
				fprintf(stderr, "locked\n");
			else if (*len > 0)
				changed |= console_line(rx, buf);

			*len = 0;
			buf[0] = '\0';
		} else if (c == 0x7F || c == 0x08) {
			if (*len > 0)
				buf[--*len] = '\0';
		} else if (c == 0x15) {	/* ^U: clear the line */
			*len = 0;
			buf[0] = '\0';
		} else if (c >= 0x20 && c < 0x7F && *len < size - 1) {
			buf[(*len)++] = c;
			buf[*len] = '\0';
		}
	}

	g_cmd_view = buf;
	show(rx);
	return changed;
}

//
// Console in character mode without echo (ISIG kept: ^C still quits); restored at exit
//

static struct termios g_tty_saved;
static bool g_tty_raw;

static void console_restore(void)
{
	if (g_tty_raw)
		tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);

	g_tty_raw = false;
}

static void console_raw(void)
{
	struct termios t;

	if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_tty_saved) < 0)
		return;

	t = g_tty_saved;
	t.c_lflag &= ~(tcflag_t) (ICANON | ECHO);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
		g_tty_raw = true;
		atexit(console_restore);
	}
}

//
// CAT: CI-V over the USB serial port (IC-705 identity, see docs/x6100_civ.md).
// The engine (x6100_civ.c) calls back into the receiver state; a change sets
// cat.changed, and the main loop applies it like a key press.
//

#define CAT_DEV		"/dev/ttyS2"	/* "DEV" USB-C: CH342 port 2 (PC ttyACM1); port 1 is the console */
#define CAT_BAUD_DEF	19200	/* stock / IU1WXY default and the most the DEV port carries */
#define CAT_PTY_LINK	"/tmp/x6100-civ"	/* link to the pty for clients (wfserver, rigctld, socat) */

//
// Transceive beyond frequency and mode, as an Icom: these levels and switches are
// reported unasked (to 00) when they change, on the radio or from the other client
// (2026-10-31: wfview did not follow NR / NB set on the radio). A client's own change is
// not echoed back to it (cat_levels after its poll), so a dragged slider does not jump.
//

static const struct
{
	uint8_t cmd[3];
	uint8_t n;		/* bytes of cmd: 1 no subcommand (11 ATT, 0F split), 3 scope (27 xx 00) */
} g_cat_ann[] = {
	{ { 0x14, 0x01 }, 2 }, { { 0x14, 0x02 }, 2 }, { { 0x14, 0x03 }, 2 }, { { 0x14, 0x06 }, 2 },
	{ { 0x14, 0x07 }, 2 }, { { 0x14, 0x08 }, 2 }, { { 0x14, 0x09 }, 2 }, { { 0x14, 0x0A }, 2 },
	{ { 0x14, 0x0B }, 2 }, { { 0x14, 0x0C }, 2 }, { { 0x14, 0x0E }, 2 }, { { 0x14, 0x12 }, 2 },
	{ { 0x14, 0x15 }, 2 }, { { 0x16, 0x02 }, 2 }, { { 0x16, 0x12 }, 2 }, { { 0x16, 0x22 }, 2 },
	{ { 0x16, 0x40 }, 2 }, { { 0x16, 0x41 }, 2 }, { { 0x16, 0x44 }, 2 }, { { 0x16, 0x45 }, 2 },
	{ { 0x11 }, 1 }, { { 0x0F }, 1 }, { { 0x27, 0x17, 0x00 }, 3 },
};

#define CAT_ANN_N	((int) (sizeof(g_cat_ann) / sizeof(g_cat_ann[0])))
#define CAT_ANN_MAX	24	/* bytes of one report */

typedef struct
{
	rx_t *rx;
	const screen_t *scr;
	int fd;			/* the serial port, or the pty master */
	int pty_slave;		/* pty: the client side, held open by us (raw; no hangup between clients) */
	char pty_name[64];
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
	bool no_transceive;	/* 1A 05 0131 set to 00 by the client: no unasked 00 / 01 */
	uint8_t ann_val[CAT_ANN_N][CAT_ANN_MAX];	/* the last report of each g_cat_ann item */
	uint8_t ann_len[CAT_ANN_N];
	bool ann_ready;		/* the first pass only records */

	//
	// Scope data (27): client settings and pacing
	//
	bool scope_on;
	bool scope_out;
	uint32_t fedge_lo;	/* 27 1E: LO arrives first, HI stores the pair */
	int scope_ref;		/* 0.1 dB */
	int scope_speed;	/* 0 fast, 1 mid, 2 slow */
	uint64_t scope_ms;
} cat_t;

// IC-705 band code (1A 01) to our band index; 60 m has no code
static int civ_band(int code)
{
	static const int map[11] = { -1, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10 };

	if (code == 15)
		return BAND_COUNT;	/* IC-705 GEN: our GEN band (outside the ham bands) */

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
	case X6100_CIV_DUPLEX:
		*v = dup_shift(rx) < 0 ? 1 : dup_shift(rx) > 0 ? 2 : 0;
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
		*v = g_sql_open;	/* x6100_trx's own squelch */
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
		*v = rx->edge;
		return true;
	case X6100_CIV_SCOPE_FEDGE_LO:
	case X6100_CIV_SCOPE_FEDGE_HI:
		if (vfo < 0 || vfo >= EDGE_RANGES * 4)
			return false;

		*v = (int32_t) (item == X6100_CIV_SCOPE_FEDGE_LO ? g_edge[vfo / 4][vfo % 4].lo
			: g_edge[vfo / 4][vfo % 4].hi);
		return true;
	case X6100_CIV_SCOPE_HOLD:
		*v = g_scope_hold;
		return true;
	case X6100_CIV_SCOPE_REF:
		*v = cat->scope_ref;
		return true;
	case X6100_CIV_SCOPE_SPEED:
		*v = cat->scope_speed;
		return true;
	case X6100_CIV_SCOPE_VBW:
		*v = g_sp[SP_VBW];	/* 0 narrow, 1 wide: as the scope panel */
		return true;
	case X6100_CIV_SCOPE_TX:
		*v = g_sp[SP_TXSCOPE];
		return true;
	case X6100_CIV_SCOPE_CTYPE:	/* 0 filter centre, 1 carrier point */
		*v = g_sp[SP_CTYPE];
		return true;
	case X6100_CIV_BAND_FREQ:
	case X6100_CIV_BAND_MODE:
	case X6100_CIV_BAND_FIL: {
		int b = civ_band(vfo);

		if (b < 0)
			return false;

		// GEN: its last frequency and mode (the current ones while on GEN)
		if (b == BAND_COUNT) {
			if (item == X6100_CIV_BAND_FREQ)
				*v = (int32_t) (rx->band < 0 ? rx->freq : rx->gen_freq);
			else if (item == X6100_CIV_BAND_MODE)
				*v = rx->band < 0 ? rx->mode : rx->gen_mode;
			else
				*v = rx->fil + 1;

			return true;
		}

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
	case X6100_CIV_LVL_SQL:		/* TX audio: the panel ranges over 0000-0255 */
		*v = rx->sql * 255 / 100;
		return true;
	case X6100_CIV_LVL_MIC:		/* the mic in use: internal, else the hand mic */
		*v = (rx->micsel == 0 ? rx->imicg : rx->hmicg) * 255 / 50;
		return true;
	case X6100_CIV_LVL_COMP:	/* 1:2 / 1:4 / 1:8 as 0000 / 0127 / 0255 */
		*v = (rx->cmp_lvl - 1) * 255 / 2;
		return true;
	case X6100_CIV_LVL_MONI:
		*v = rx->moni_lvl * 255 / 100;
		return true;
	case X6100_CIV_LVL_VOX:
		*v = rx->voxg * 255 / 100;
		return true;
	case X6100_CIV_LVL_ANTIVOX:
		*v = rx->voxag * 255 / 100;
		return true;
	case X6100_CIV_COMP:
		*v = rx->cmp;
		return true;
	case X6100_CIV_VOX:
		*v = rx->vox;
		return true;
	case X6100_CIV_MONI:
		*v = rx->moni;
		return true;
	case X6100_CIV_LVL_KEY_SPEED:	/* 0000-0255 = 6-48 WPM (IC-705) */
		*v = clampi((rx->wpm - 6) * 255 / 42, 0, 255);
		return true;
	case X6100_CIV_LVL_CW_PITCH:	/* 0000-0255 = 300-900 Hz */
		*v = clampi((rx->cwtone - 300) * 255 / 600, 0, 255);
		return true;
	case X6100_CIV_LVL_QSK:		/* break-in delay; here 0000-0255 = 0-1000 ms */
		*v = rx->qsk * 255 / 1000;
		return true;
	case X6100_CIV_ATU:
		*v = rx->tuning ? 2 : rx->atu_on;
		return true;
	case X6100_CIV_MEM_CH:		/* the channel the memory panel shows */
		*v = rx->medit_ch >= 1 && rx->medit_ch <= MEM_COUNT ? rx->medit_ch : 1;
		return true;
	case X6100_CIV_TRANSCEIVE:
		*v = !cat->no_transceive;
		return true;
	case X6100_CIV_LVL_PBT1:	/* 14 07 / 08: 0128 centre, +-1250 Hz over 0000-0255 */
	case X6100_CIV_LVL_PBT2: {
		int hz = item == X6100_CIV_LVL_PBT1 ? rx->pbt_in : rx->pbt_out;
		int lv = 128 + div_round(hz * 128, PBT_MAX_HZ);

		*v = lv < 0 ? 0 : lv > 255 ? 255 : lv;
		return true;
	}
	case X6100_CIV_BKIN:		/* 16 47: the X6100 keys CW with semi break-in (QSK delay in the keyer) */
		*v = 1;
		return true;
	case X6100_CIV_DASH_RATIO:	/* 1A 05 0252: the keyer RATIO, x10 */
		*v = rx->ratio;
		return true;
	case X6100_CIV_VOICE_TX:	/* 28 00: the message going out (CI-V 17 text is 0: none) */
		*v = rx->send == SEND_MSG ? g_send.msg : 0;
		return true;
	case X6100_CIV_DATE:		/* 1A 05 0165 / 0166: local time, UTC + the client's offset */
	case X6100_CIV_TIME: {
		struct tm tm;

		cat_time_local(&tm);
		*v = item == X6100_CIV_DATE ? (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday
			: tm.tm_hour * 100 + tm.tm_min;
		return true;
	}
	case X6100_CIV_UTC_OFS:
		*v = g_cat_utc_ofs;
		return true;
	case X6100_CIV_MOD_IN:		/* vfo 0 DATA OFF MOD, 1 DATA MOD */
		*v = g_modin[vfo != 0] == MODIN_LAN ? 3 : g_modin[vfo != 0];	/* LAN as the IC-705 WLAN */
		return true;
	default:
		return false;	/* NB, NR, DNF, RIT, other levels */
	}
}

static bool cat_set(void *ctx, x6100_civ_item_t item, int vfo, int32_t v)
{
	cat_t *cat = ctx;
	rx_t *rx = cat->rx;

	switch (item) {
	case X6100_CIV_MEM_CH:		/* 08 0001-0099: select (the engine then recalls it) */
		if (v < 1 || v > MEM_COUNT)
			return false;

		rx->medit_ch = v;
		break;
	case X6100_CIV_MEM_WRITE:	/* 09: the VFO (in memory mode the one kept), as the panel */
		if (!g_cat_mem_rw)
			return false;	/* catmem ro: NG */

		mem_write_vfo(rx, v);
		break;
	case X6100_CIV_MEM_MODE:	/* 08: memory mode on channel v (empty: the next stored) */
		if (!mem_mode_on(rx, v))
			return false;
		break;
	case X6100_CIV_BKIN:		/* semi only: off and full break-in are not on the X6100 */
		if (v != 1)
			return false;
		break;
	case X6100_CIV_DASH_RATIO:	/* 28-45, the keyer panel takes 25-45 */
		rx->ratio = clampi(v, 25, 45);
		break;
	case X6100_CIV_UTC_OFS:		/* 1A 05 0170: wfview sends it first, then time and date */
		if (!cat_time_allowed())
			return false;

		g_cat_utc_ofs = v;
		break;
	case X6100_CIV_DATE:		/* 1A 05 0165 / 0166: NG unless cattime allows it now */
	case X6100_CIV_TIME:
		if (!cat_time_set(item == X6100_CIV_DATE, v))
			return false;
		break;
	case X6100_CIV_VOICE_TX:	/* 28 00 01-08: message M1-M8 as the MSG key (voice: not in CW;
				   CW: CW mode only); 00 stops a message going out */
		if (v == 0) {
			if (rx->send == SEND_MSG && g_send.msg > 0)
				send_stop(rx);
			break;
		}

		if (!msg_start(rx, v))
			return false;
		break;
	case X6100_CIV_VFO_MODE:	/* 07: back to the VFO as it was */
		mem_mode_off(rx);
		break;
	case X6100_CIV_MEM_TO_VFO:	/* 0A: the channel into the VFO, VFO mode (the kept VFO dropped) */
		if (!mem_recall(rx, v))
			return false;

		rx->mem_mode = false;
		break;
	case X6100_CIV_MEM_CLEAR:	/* 0B */
		if (!g_cat_mem_rw)
			return false;	/* catmem ro: NG */

		mem_clear(v);
		break;
	case X6100_CIV_TRANSCEIVE:	/* 1A 05 0131 */
		cat->no_transceive = v == 0;
		break;
	case X6100_CIV_LVL_PBT1:	/* 14 07 / 08: twin PBT, 9.8 Hz a step */
	case X6100_CIV_LVL_PBT2: {
		int hz = div_round((v - 128) * PBT_MAX_HZ, 128);

		hz = hz < -PBT_MAX_HZ ? -PBT_MAX_HZ : hz > PBT_MAX_HZ ? PBT_MAX_HZ : hz;

		if (item == X6100_CIV_LVL_PBT1)
			rx->pbt_in = hz;
		else
			rx->pbt_out = hz;

		break;
	}
	case X6100_CIV_MOD_IN:		/* 1A 05 0118 / 0119; the conf may fix it */
		if (g_conf_set & (1u << GEN_MODOFF))
			return false;

		if (v < 0 || v > 3)
			return false;

		g_modin[vfo != 0] = v == 0 ? MODIN_MIC : v == 1 ? MODIN_USB : MODIN_LAN;	/* 2 (MIC,USB), 3 (WLAN): LAN */
		break;
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
	case X6100_CIV_DUPLEX:	/* 0 simplex, 1 DUP-, 2 DUP+ with the panel shift; a duplex ends split */
		if (v < 0 || v > 2)
			return false;

		rx->split = v != 0 ? false : rx->split;

		if (v == 0 && dup_shift(rx) != 0)
			rx->dup = DUP_OFF;	/* simplex where AUTO would shift; elsewhere AUTO stays */
		else if (v == 1 && dup_shift(rx) >= 0)
			rx->dup = DUP_MINUS;	/* already DUP- (also AUTO): kept */
		else if (v == 2 && dup_shift(rx) <= 0)
			rx->dup = DUP_PLUS;

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
		g_lock_auto = false;	/* set by CAT: stays, as LOCK by hand */
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
			rx->ptt_cat = true;	/* CAT PTT: LAN takes the codec (network audio) */
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

		rx->edge = v;		/* scope_follow moves the window */
		break;
	case X6100_CIV_SCOPE_FEDGE_LO:
		if (vfo < 0 || vfo >= EDGE_RANGES * 4 || v <= 0)
			return false;

		cat->fedge_lo = (uint32_t) v;
		break;
	case X6100_CIV_SCOPE_FEDGE_HI: {
		// Wider than edge_max (96 kHz, 75 with the IF shift): cut from the lower end
		if (vfo < 0 || vfo >= EDGE_RANGES * 4 || (uint32_t) v <= cat->fedge_lo)
			return false;

		edge_t *ed = &g_edge[vfo / 4][vfo % 4];

		ed->lo = cat->fedge_lo;
		ed->hi = (uint32_t) v - cat->fedge_lo > edge_max() ? cat->fedge_lo + edge_max() : (uint32_t) v;
		break;
	}
	case X6100_CIV_SCOPE_HOLD:
		g_scope_hold = v != 0;	/* the radio's scope, as an Icom's HOLD, not only the CAT data */
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
		g_sp[SP_VBW] = v != 0;
		break;
	case X6100_CIV_SCOPE_TX:
		g_sp[SP_TXSCOPE] = v != 0;
		break;
	case X6100_CIV_SCOPE_CTYPE:
		if (v < 0 || v > 2)
			return false;

		g_sp[SP_CTYPE] = v != 0;	/* 2 (carrier, absolute frequency): carrier point */
		break;
	case X6100_CIV_BAND_FREQ: {
		int b = civ_band(vfo);

		// GEN: any frequency the radio tunes; stored as GEN's last frequency
		if (b == BAND_COUNT) {
			if (v < (int32_t) FREQ_MIN || v > (int32_t) FREQ_MAX)
				return false;

			rx->gen_freq = (uint32_t) v;
			return true;
		}

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
	case X6100_CIV_LVL_SQL:		/* TX audio: 0000-0255 over the panel ranges */
		rx->sql = (v * 100 + 127) / 255;
		break;
	case X6100_CIV_LVL_MIC:		/* the mic in use: internal, else the hand mic */
		*(rx->micsel == 0 ? &rx->imicg : &rx->hmicg) = (v * 50 + 127) / 255;
		break;
	case X6100_CIV_LVL_COMP:
		rx->cmp_lvl = 1 + (v * 2 + 127) / 255;
		break;
	case X6100_CIV_LVL_MONI:
		rx->moni_lvl = (v * 100 + 127) / 255;
		break;
	case X6100_CIV_LVL_VOX:
		rx->voxg = (v * 100 + 127) / 255;
		break;
	case X6100_CIV_LVL_ANTIVOX:
		rx->voxag = (v * 100 + 127) / 255;
		break;
	case X6100_CIV_COMP:
		rx->cmp = v != 0;
		break;
	case X6100_CIV_VOX:
		rx->vox = v != 0;
		break;
	case X6100_CIV_MONI:
		rx->moni = v != 0;
		break;
	case X6100_CIV_LVL_KEY_SPEED:
		rx->wpm = clampi(6 + (v * 42 + 127) / 255, 5, 50);
		break;
	case X6100_CIV_LVL_CW_PITCH:
		rx->cwtone = clampi(300 + (v * 600 + 127) / 255 / 10 * 10, 300, 900);
		break;
	case X6100_CIV_LVL_QSK:
		rx->qsk = (v * 1000 + 127) / 255 / 10 * 10;
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

//
// Memory channels over CI-V (1A 00), IC-705 layout: the B side is the TX side of a split
// channel (frequency, mode; one filter for both here, the A one);
// duplex 00 is AUTO here (simplex outside the 10 m repeater window). Data modes are
// refused: memories.conf has no names for them.
//

static bool cat_mem_get(void *ctx, int ch, x6100_civ_mem_t *m)
{
	const mem_t *c = &g_mem[ch];

	(void) ctx;
	memset(m, 0, sizeof(*m));
	m->used = c->used;
	m->hz = c->hz;
	m->mode = c->mode;
	m->fil = c->fil + 1;
	m->split = c->split;
	m->tx_hz = c->split ? c->tx_hz : c->hz;
	m->tx_mode = c->split ? c->tx_mode : c->mode;
	m->tx_fil = m->fil;
	m->dup = c->dup == DUP_MINUS ? 1 : c->dup == DUP_PLUS ? 2 : 0;
	m->dup_hz = m->dup ? (uint32_t) c->dup_khz * 1000u : 0;
	snprintf(m->name, sizeof(m->name), "%s", c->name);

	return true;
}

static bool cat_mem_set(void *ctx, int ch, const x6100_civ_mem_t *m)
{
	mem_t n = g_mem[ch];

	(void) ctx;

	if (!g_cat_mem_rw)
		return false;	/* catmem ro: NG */

	if (!m->used) {
		mem_clear(ch);
		return true;
	}

	if (m->hz < FREQ_MIN || m->hz > FREQ_MAX || m->mode == X6100_MODE_LSB_DIG || m->mode == X6100_MODE_USB_DIG)
		return false;

	n.used = true;
	n.hz = m->hz;
	n.mode = (x6100_mode_t) m->mode;
	n.fil = clampi(m->fil - 1, 0, 2);
	n.split = m->split && m->tx_hz >= FREQ_MIN && m->tx_hz <= FREQ_MAX
		&& m->tx_mode != X6100_MODE_LSB_DIG && m->tx_mode != X6100_MODE_USB_DIG;
	n.tx_hz = n.split ? m->tx_hz : 0;
	n.tx_mode = n.split ? (x6100_mode_t) m->tx_mode : n.mode;
	n.dup = m->dup == 1 ? DUP_MINUS : m->dup == 2 ? DUP_PLUS : DUP_AUTO;
	n.dup_khz = clampi((int) ((m->dup_hz + 500u) / 1000u), 10, DUP_KHZ_MAX);
	snprintf(n.name, sizeof(n.name), "%.15s", m->name);	/* CI-V gives 16, a memory name holds 15 */

	for (char *c = n.name; *c != '\0'; c++)
		*c = *c == '#' || (unsigned char) *c < ' ' || (unsigned char) *c > '~' ? ' ' : *c;	/* one line in memories.conf */

	g_mem[ch] = n;
	mem_save();

	return true;
}

// CI-V 17: send CW (message 0), 17 FF stops
static bool cat_send_cw(void *ctx, const char *text)
{
	cat_t *cat = ctx;

	cat->changed = true;
	return cw_send_text(cat->rx, text);
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

//
// ttyS2 is R_UART (0x01F02800, 24 MHz, DesignWare). On the image's kernel (6.1.82)
// a speed set through termios never reaches its divisor: it stays at 78 (19200)
// whatever stty reads back (2026-10-25, read from the registers). The hardware takes
// a divisor written directly, so x6100_trx sets it itself: DLAB in LCR, DLL / DLH,
// LCR back, then reads it back. Registers are 32 bits apart.
//

#define RUART_BASE	0x01F02800u
#define RUART_CLK	24000000u
#define RUART_DLL	0		/* with LCR.DLAB */
#define RUART_DLH	1
#define RUART_LCR	3
#define RUART_USR	31		/* 0x7C: bit 0 busy */

static int cat_divisor(int baud)
{
	const off_t page = RUART_BASE & ~0xFFFu;
	uint32_t div = (RUART_CLK / 16 + (uint32_t) baud / 2) / (uint32_t) baud;
	int fd = open("/dev/mem", O_RDWR | O_SYNC), ok;
	void *map;

	if (fd < 0)
		return -errno;

	map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
	close(fd);

	if (map == MAP_FAILED)
		return -errno;

	volatile uint32_t *r = (volatile uint32_t *) ((uint8_t *) map + (RUART_BASE - page));
	uint32_t lcr = r[RUART_LCR];

	// The divisor latch only opens while the UART is idle (busy: LCR writes are dropped)
	for (int i = 0; i < 100000 && (r[RUART_USR] & 1u); i++)
		;

	r[RUART_LCR] = lcr | 0x80u;
	r[RUART_DLL] = div & 0xFFu;
	r[RUART_DLH] = (div >> 8) & 0xFFu;
	ok = (r[RUART_LCR] & 0x80u) && r[RUART_DLL] == (div & 0xFFu) && r[RUART_DLH] == ((div >> 8) & 0xFFu);
	r[RUART_LCR] = lcr & ~0x80u;
	munmap(map, 4096);

	return ok ? (int) div : -EIO;
}

//
// CI-V on a pseudo-terminal (cat pty): no speed limit, for a client on the radio
// itself (wfserver, rigctld, socat to TCP). We hold the client side open too, set
// raw: its settings stay, and a client closing it does not hang up the master.
// CAT_PTY_LINK points at it.
//

static int cat_open_pty(cat_t *cat)
{
	struct termios tio;
	const char *name;

	cat->fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (cat->fd < 0)
		return -errno;

	if (grantpt(cat->fd) == 0 && unlockpt(cat->fd) == 0 && (name = ptsname(cat->fd)) != NULL
		&& (cat->pty_slave = open(name, O_RDWR | O_NOCTTY)) >= 0 && tcgetattr(cat->pty_slave, &tio) == 0) {
		cfmakeraw(&tio);

		if (tcsetattr(cat->pty_slave, TCSANOW, &tio) == 0) {
			snprintf(cat->pty_name, sizeof(cat->pty_name), "%s", name);
			unlink(CAT_PTY_LINK);

			if (symlink(cat->pty_name, CAT_PTY_LINK) < 0)
				fprintf(stderr, "CAT: cannot link %s to %s: %s\n", CAT_PTY_LINK, cat->pty_name, strerror(errno));

			return 0;
		}
	}

	int e = errno ? errno : EIO;

	if (cat->pty_slave >= 0)
		close(cat->pty_slave);

	close(cat->fd);
	cat->fd = cat->pty_slave = -1;
	return -e;
}

static int cat_open(cat_t *cat, rx_t *rx, const screen_t *scr, int baud)
{
	struct termios tio;
	x6100_civ_ops_t ops = { .ctx = cat, .get = cat_get, .set = cat_set, .mem_get = cat_mem_get, .mem_set = cat_mem_set,
		.send_cw = cat_send_cw };

	memset(cat, 0, sizeof(*cat));
	cat->rx = rx;
	cat->scr = scr;
	cat->ann_mode = -1;
	x6100_civ_init(&cat->civ, &ops);
	cat->civ.addr = (uint8_t) g_civ_addr;
	x6100_civ_framer_reset(&cat->framer);
	cat->pty_slave = -1;

	if (baud == CAT_PTY)
		return cat_open_pty(cat);

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

	// The kernel leaves the R_UART divisor alone: set it here (see cat_divisor)
	tcdrain(cat->fd);
	int div = cat_divisor(baud);

	if (div < 0) {
		fprintf(stderr, "CAT: %s divisor for %d baud not set: %s\n", CAT_DEV, baud, strerror(-div));
		close(cat->fd);
		cat->fd = -1;
		return div;
	}

	fprintf(stderr, "CAT: %s divisor %d (%d baud, R_UART set directly)\n", CAT_DEV, div, baud);

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

static void cat_levels(cat_t *cat, bool send);

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

			g_cat_rx_ms[cat->pty_name[0] != '\0' ? CAT_EP_PTY : CAT_EP_DEV] = now_ms();	/* the CAT indicator */

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

	if (cat->changed)
		cat_levels(cat, false);	/* its own change: recorded, not echoed back to it */

	return cat->changed;
}

// The g_cat_ann levels and switches as the engine reads them now: a changed one is
// recorded, and with send (and transceive on, past the first pass) reported to all
static void cat_levels(cat_t *cat, bool send)
{
	uint8_t out[X6100_CIV_FRAME_MAX];

	for (int i = 0; i < CAT_ANN_N; i++) {
		size_t n = x6100_civ_announce_read(&cat->civ, g_cat_ann[i].cmd, g_cat_ann[i].n, out, sizeof(out));

		if (n == 0 || n > CAT_ANN_MAX || (n == cat->ann_len[i] && memcmp(out, cat->ann_val[i], n) == 0))
			continue;

		memcpy(cat->ann_val[i], out, n);
		cat->ann_len[i] = (uint8_t) n;

		if (send && cat->ann_ready && !cat->no_transceive)
			cat_write(cat, out, n);
	}

	cat->ann_ready = true;
}

// Transceive: announce frequency and mode changes, whatever made them; with
// transceive off (1A 05 0131 00) only the state is tracked, nothing goes out
static void cat_announce(cat_t *cat)
{
	uint8_t out[X6100_CIV_FRAME_MAX];
	const rx_t *rx = cat->rx;

	if (rx->freq != cat->ann_freq) {
		cat->ann_freq = rx->freq;

		if (!cat->no_transceive)
			cat_write(cat, out, x6100_civ_announce_freq(&cat->civ, rx->freq, out, sizeof(out)));
	}

	if ((int) rx->mode != cat->ann_mode || rx->fil != cat->ann_fil) {
		cat->ann_mode = rx->mode;
		cat->ann_fil = rx->fil;

		if (!cat->no_transceive)
			cat_write(cat, out, x6100_civ_announce_mode(&cat->civ, rx->mode, rx->fil + 1, out, sizeof(out)));
	}

	cat_levels(cat, true);
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

	if (!cat->scope_on || !cat->scope_out || g_scope_hold || !scr->spec_valid)
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
		.centre = (uint32_t) (v.lo + v.span / 2),	/* the carrier, or the filter centre (CENTER TYPE) */
		.half_span = (uint32_t) rx->span / 2,
		.lo = (uint32_t) v.lo,
		.hi = (uint32_t) (v.lo + v.span),
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
		g_time_src = TS_OS;
		printf("ntp: %s\n", g_ntp_msg);
		return;
	}

	if (proc_running(daemons)) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "by the OS time daemon");
		g_time_src = TS_OS;
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

	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
		snprintf(g_ntp_msg, sizeof(g_ntp_msg), "synced");
		g_time_src = TS_NTP;
	} else if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
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
	struct pollfd pfd[IN_COUNT + 5];	/* inputs, stdin, the command pipe, audio scope, 2 CAT */
	int npfd = 0;
	input_t pmap[IN_COUNT + 5];
	char cmd[128];
	size_t cmd_len = 0;
	char fcmd[128];		/* a line from the command pipe (CMD_FIFO) */
	size_t fcmd_len = 0;
	int fifo_slot = -1;
	int stdin_slot = -1;
	int cat_slot[CAT_N] = { -1, -1 };
	int apcm_slot = -1;	/* audio scope capture, fd -1 while closed */
	x6100_pcm_t apcm;
	bool apcm_open = false;
	bool codec_routed = false;	/* codec mixer set up for a mod input on USB */
	static cat_t cats[CAT_N] = { { .fd = -1 }, { .fd = -1 } };	/* CAT_EP_DEV, CAT_EP_PTY */
	rx_t rx = { .vol = 20, .rfg = 100, .step = 100, .agc = (x6100_agc_t) -1, .mode = X6100_MODE_USB,
		.txpwr = TXPWR_DEF, .scope = SCOPE_CENTER, .span = 96000,	/* the widest span (g_spans) */
		.nr_lvl = 20, .nb_lvl = 10, .nb_w = 10, .dnf_f = 1000, .dnf_w = 50 };	/* defaults: g_dsp[] */
	int rc;

	// --help / -h / help / ?: the options, before anything is read or opened
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 || strcasecmp(argv[i], "help") == 0
			|| strcmp(argv[i], "?") == 0) {
			help_options(stdout, argv[0]);
			return 0;
		}
	}

	fil_load();

	for (int i = 0; i < BAND_COUNT; i++)
		rx.band_freq[i] = g_bands[i].def;

	rx.gen_freq = 10000000;		/* GEN band out of the box: 10 MHz time signal, AM */
	rx.gen_mode = X6100_MODE_AM;

	kpar_defaults(&rx);
	xpar_defaults(&rx);
	spar_defaults();
	rx.dup = DUP_AUTO;
	rx.dup_khz = DUP_KHZ_DEF;
	rx.tx_mode_ovr = -1;	/* no TX overrides: nothing being sent */
	rx.tx_pwr_ovr = -1;
	rx.edge = 1;
	edge_defaults();

	for (int g = 0; g < FG_COUNT; g++)
		rx.agc_group[g] = g == FG_AM || g == FG_FM ? X6100_AGC_SLOW : X6100_AGC_AUTO;

	set_freq(&rx, 14074000);

	// Last state, then the user's settings; the command line below goes last
	conf_load(&rx, CONF_PATH, true);	/* first for oob / nolimit, so the state frequency fits */
	conf_load(&rx, MEM_PATH, false);
	conf_load(&rx, BIRDIE_PATH, false);
	conf_load(&rx, MSG_PATH, false);
	conf_load(&rx, STATE_PATH, false);
	conf_load(&rx, CONF_PATH, false);

	// The fixed window as the settings left it: the edge in use and, in SCROLL-F, the page
	// with the VFO on it (else the scope stayed empty until the first change, 2026-10-31)
	if (scope_fixed(&rx))
		scope_fix_start(&rx);

	scope_follow(&rx);
	bt_power_apply();	/* Bluetooth as last set on the GEN row */

	if (!g_af_seen)
		spar_af_from_rf();	/* first start with the AF rows: as the RF scope looked */

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-limit") == 0) {
			g_no_limit = true;
			continue;
		}

		if (strcmp(argv[i], "--cat") == 0 || strncmp(argv[i], "--cat=", 6) == 0) {
			// --cat=pty: the pty only (it is always on), the DEV port off
			g_cat_baud = argv[i][5] != '=' ? CAT_BAUD_DEF : strcmp(argv[i] + 6, "pty") == 0 ? 0 : atoi(argv[i] + 6);

			if (g_cat_baud != 0 && g_cat_baud != 4800 && g_cat_baud != 9600 && g_cat_baud != 19200) {
				fprintf(stderr, "%s: 4800, 9600, 19200 (the most the DEV port carries) or pty\n", argv[i]);
				return 1;
			}
			continue;
		}

		if (strcmp(argv[i], "--cat-log") == 0) {
			g_cat_log = true;
			g_cat_baud = g_cat_baud ? g_cat_baud : CAT_BAUD_DEF;
			continue;
		}

		if (strcmp(argv[i], "--bbreset") == 0) {	/* reset the baseband first (GPIO) */
			g_bb_reset_req = true;
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

		// layout portrait | landscape on the command line (as the console token)
		if (strcasecmp(argv[i], "layout") == 0 && i + 1 < argc) {
			if (strcasecmp(argv[i + 1], "portrait") != 0 && strcasecmp(argv[i + 1], "landscape") != 0) {
				fprintf(stderr, "layout: portrait or landscape\n");
				return 1;
			}

			g_layout_portrait = strcasecmp(argv[++i], "portrait") == 0;
			continue;
		}

		if (!console_token(&rx, argv[i])) {
			help_options(stderr, argv[0]);
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

	console_raw();	/* character mode: our own echo after the status line */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_hup);		/* the console gone: go on without it (2026-11-07) */
	signal(SIGPIPE, SIG_IGN);	/* output to a pipe whose reader went away */

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

	// The command pipe: console lines for an x6100_trx without a console (started at boot,
	// stdin /dev/null): printf 'bbreset\n' > /tmp/x6100_trx.cmd. Owner only; opened read /
	// write so it never reports end of file when a writer closes; replies in the log
	pfd[npfd].fd = -1;

	struct stat fst;

	if ((mkfifo(CMD_FIFO, 0600) == 0 || errno == EEXIST) && stat(CMD_FIFO, &fst) == 0 && S_ISFIFO(fst.st_mode))
		pfd[npfd].fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (pfd[npfd].fd < 0)
		fprintf(stderr, "warning: no command pipe %s\n", CMD_FIFO);

	pfd[npfd].events = POLLIN;
	pmap[npfd] = IN_COUNT;
	fifo_slot = npfd;
	npfd++;

	// Audio scope capture slot, unused (fd -1) until F3 opens it
	pfd[npfd].fd = -1;
	pfd[npfd].events = POLLIN;
	pmap[npfd] = IN_COUNT;
	apcm_slot = npfd;
	npfd++;

	// Another program on the baseband (a GUI not stopped yet, a second x6100_trx): two
	// writers on the same registers garble each other, the calibration read too. The
	// names as /proc comm has them (15 characters)
	{
		static const char *const others[] = { "x6100-iu1wxy-gu", "x6100_gui", "x6100_trx", "x6100_rx", NULL };

		if (proc_running(others)) {
			fprintf(stderr, "error: another program drives the baseband (IU1WXY / R1CBU GUI or x6100_trx):"
				" x6100_ui.sh stop first\n");
			return 1;
		}
	}

	if ((rc = x6100_i2c_open(&i2c, NULL)) < 0 || (rc = x6100_flow_open(&flow, NULL)) < 0) {
		fprintf(stderr, "open: %s\n", strerror(-rc));
		return 1;
	}

	// --bbreset: a baseband that hangs (no answer on I2C, no flow) reset before the start
	if (g_bb_reset_req) {
		g_bb_reset_req = false;
		rc = bb_reset_pulse();
		fprintf(stderr, "bbreset: %s\n", rc < 0 ? strerror(-rc) : "baseband reset");
	}

	//
	// The baseband right after another program let it go (the IU1WXY GUI, an x6100_trx
	// that ended badly) may not answer on I2C for a while: the calibration read gave
	// garbage and the first refresh failed, x6100_trx did not start until a reboot
	// (2026-11-01). Both are tried again for a few seconds first.
	//
	for (int n = 1; (rc = x6100_cal_read(&i2c, &cal)) < 0 && n < START_TRIES; n++) {
		fprintf(stderr, "calibration read: %s, again (%d/%d)\n", strerror(-rc), n, START_TRIES);
		usleep(START_WAIT_US);
	}

	if (rc < 0) {
		fprintf(stderr, "warning: calibration invalid (%s), using defaults\n", strerror(-rc));
		memset(&cal, 0, sizeof(cal));
		cal.bias_drive = X6100_CAL_BIAS_DRIVE_DEF;
		cal.bias_final = X6100_CAL_BIAS_FINAL_DEF;
	}

	x6100_ctl_init(&ctl, &i2c);
	apply(&ctl, &cal, &rx);
	x6100_ctl_set(&ctl, X6100_REG_FLAGS, 0);

	for (int n = 1; (rc = x6100_ctl_refresh(&ctl)) < 0 && n < START_TRIES; n++) {
		fprintf(stderr, "refresh: %s, again (%d/%d)\n", strerror(-rc), n, START_TRIES);
		usleep(START_WAIT_US);
	}

	if (rc < 0) {
		fprintf(stderr, "refresh: %s: the baseband does not answer on I2C; try x6100_trx --bbreset, else a power cycle\n",
			strerror(-rc));
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
	// CAT: CI-V on the DEV port (cat, --cat) and on the pty (catpty), both at once
	//
	for (int e = 0; e < CAT_N; e++) {
		int baud = e == CAT_EP_PTY ? (g_cat_pty ? CAT_PTY : 0) : g_cat_baud;

		if (baud == 0)
			continue;

		if ((rc = cat_open(&cats[e], &rx, &scr, baud)) < 0) {
			fprintf(stderr, "warning: CAT %s: %s\n", baud == CAT_PTY ? "pty" : CAT_DEV, strerror(-rc));
			continue;
		}

		if (baud == CAT_PTY)
			printf("CAT: CI-V on %s -> %s (pseudo-terminal, any speed), address 0x%02X (%s)\n", CAT_PTY_LINK,
				cats[e].pty_name, cats[e].civ.addr, civ_ident());
		else
			printf("CAT: CI-V on %s, %d baud, address 0x%02X (%s)\n", CAT_DEV, baud, cats[e].civ.addr, civ_ident());

		pfd[npfd].fd = cats[e].fd;
		pfd[npfd].events = POLLIN;
		pmap[npfd] = IN_COUNT;
		cat_slot[e] = npfd;
		npfd++;
	}

#ifdef X6100_RX_ONLY
	printf("x6100_rx (receive only): PWR or Ctrl-C to quit\n");
#else
	net_announce(true);	/* IP addresses, wfserver UDP ports */
	printf("x6100_trx: PWR or Ctrl-C to quit\n");
#endif
	show(&rx);

	uint64_t i2c_fail_since = 0;	/* first failed I2C write of a run, 0 = none */
	unsigned i2c_fails = 0;
	bool was_keyed = false;		/* our PTT or tune on the last pass */
	bool force_refresh = false;	/* whole register block on the next pass */
	uint64_t watch_from = 0;	/* stuck-TX watch armed from .. until (ms) */
	uint64_t watch_until = 0;
	bool watch_err = false;		/* the watch was armed by a write error */
	uint64_t stuck_since = 0;	/* baseband in TX inside the watch since (ms) */
	uint64_t last_force = 0;
	uint64_t last_refresh = now_ms();
	uint64_t last_meter = last_refresh;
	uint64_t last_disp = last_refresh;

	g_last_turn_ms = now_ms();	/* the portrait auto lock counts from the start */
	double level = 0.0;
	int level_n = 0;
	bool new_frame = false;
	bool dirty = false;
	uint64_t last_flow_ms = now_ms();	/* last flow frame: the baseband watchdog */
	bool quiet_on = false;		/* a quiet birdie scan switched WiFi and Bluetooth off */
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

			if (p == fifo_slot) {
				changed |= console_read(&rx, pfd[p].fd, fcmd, sizeof(fcmd), &fcmd_len);
				g_cmd_view = cmd;	/* the status line keeps what is typed on the console */
				continue;
			}

			if (p == stdin_slot) {
				changed |= console_read(&rx, STDIN_FILENO, cmd, sizeof(cmd), &cmd_len);
				continue;
			}

			if (p == apcm_slot) {
				static int16_t abuf[1024 * 2];
				ssize_t got = x6100_pcm_read(&apcm, abuf, 1024);

				if (got > 0)
					audio_feed(&scr, abuf, (unsigned) got, 2);

				continue;
			}

			if (p == cat_slot[CAT_EP_DEV] || p == cat_slot[CAT_EP_PTY]) {
				changed |= cat_poll(&cats[p == cat_slot[CAT_EP_PTY]]);
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

			g_sql_cap = -1;	/* rx_setup turns the capture on: the squelch sets it again */
			int prc = x6100_mixer_rx_setup(X6100_PCM_CARD, boost) < 0 ? -EIO
				: x6100_pcm_open(&apcm, X6100_PCM_CARD, X6100_PCM_DEVICE, true, AS_RATE, 2, 1024, 4);

			if (prc < 0) {
				// Busy: another program has the codec capture (wfserver, pulseaudio's
				// Bluetooth loopback); the open no longer waits for it
				fprintf(stderr, "\naudio scope: codec capture not available: %s\n", strerror(-prc));
				toast(prc == -EBUSY ? "AUDIO SCOPE: CODEC BUSY" : "AUDIO SCOPE: NO CAPTURE");
				rx.ascope = rx.asplit = false;
				changed = true;
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
		bt_autoconnect(now_ms());

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

			// The 9 kHz channel step only where it makes sense (10 kHz is a normal step
			// since 2026-11-04: it was set back to 1 kHz here, so FST went 5k, 10k, 1k)
			if (rx.step == 9000 && !lwmw(&rx))
				rx.step = 1000;

			if (rx.step == STEP_CH && !on_60m(&rx))
				rx.step = 1000;

			scope_follow(&rx);
			apply(&ctl, &cal, &rx);
			show(&rx);
			dirty = true;

			for (int e = 0; e < CAT_N; e++) {	/* both endpoints: one client's change reaches the other */
				if (cats[e].fd >= 0)
					cat_announce(&cats[e]);
			}
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

		lan_poll(t);	/* the LAN indicator */
		wifi_poll(t);	/* the WiFi symbol */

		{
			static uint64_t net_ms;

			if (t - net_ms >= 5000) {
				net_ms = t;
				net_announce(false);	/* an address changed: say it again */
			}
		}

		// A GEN setting changed: the state on disk (a kill or power loss keeps it)
		if (g_gen_save_at != 0 && t >= g_gen_save_at) {
			g_gen_save_at = 0;
			state_save(&rx);
		}

		// Birdie scan asked for (GEN row, console): start / stop here, with rx at hand
		if (g_bscan_req != 0) {
			if (g_bscan_req > 0 && bscan_start(&rx, g_bscan_all) && g_bscan_quiet) {
				// birdiescan ... quiet: WiFi and Bluetooth off for the scan, back on after
				// it (WiFi always, Bluetooth if it was on); the ssh session goes with the
				// WiFi, x6100_trx goes on without it (2026-11-07)
				printf("\nbirdie scan: WiFi and Bluetooth off until it is done\n");
				fflush(stdout);
				bt_cmd("power", "off");
				wifi_set(false);
				quiet_on = true;
			} else if (g_bscan_req < 0) {
				bscan_stop(&rx, false);
			}

			g_bscan_req = 0;
			g_bscan_quiet = false;
			dirty = true;
		}

		if (quiet_on && !bscan_running()) {
			quiet_on = false;
			wifi_set(true);

			if (g_bt_power == 1)
				bt_cmd("power", "on");

			toast("WIFI BACK ON");
			dirty = true;
		}

		// Keyer panel after 10 s
		if (rx.kedit && t - rx.kedit_since >= 10000) {
			rx.kedit = false;
			dirty = true;
		}

		// TX refusal / trip note: gone WARN_MS after it was set, or after the power cut it
		// reports (TOT on hand mic / VOX, stuck TX) has ended
		static const char *warn_seen;
		static uint64_t warn_at;

		if (rx.warn != warn_seen || rx.tot_cut) {
			warn_seen = rx.warn;
			warn_at = t;
		} else if (rx.warn != NULL && t - warn_at >= WARN_MS) {
			rx.warn = NULL;
			warn_seen = NULL;
			dirty = true;
		}

		// CI-V address changed on the GEN panel: the engine answers on it from now on
		for (int e = 0; e < CAT_N; e++) {
			if (cats[e].fd >= 0 && cats[e].civ.addr != (uint8_t) g_civ_addr) {
				cats[e].civ.addr = (uint8_t) g_civ_addr;
				printf("\nCAT: %s address 0x%02X (%s)\n", e == CAT_EP_PTY ? "pty" : "DEV", cats[e].civ.addr, civ_ident());
			}
		}

		// A mod input on USB: route the codec once for a Linux audio program (wfserver):
		// capture carries the RX audio, playback goes to the modulator
		if ((g_modin[0] || g_modin[1]) && !codec_routed) {
			int boost = rx.mode == X6100_MODE_AM || rx.mode == X6100_MODE_NFM ? 0 : 1;	/* as x6100_audio */

			g_sql_cap = -1;	/* rx_setup turns the capture on: the squelch sets it again */

			if (x6100_mixer_rx_setup(X6100_PCM_CARD, boost) < 0 || x6100_mixer_tx_setup(X6100_PCM_CARD) < 0)
				fprintf(stderr, "\nmod input USB: codec mixer setup failed\n");

			codec_routed = true;
		}

		// Message and app panels after 10 s
		if ((rx.msedit && t - rx.msedit_since >= 10000) || (rx.apedit && t - rx.apedit_since >= 10000)) {
			rx.msedit = false;
			rx.apedit = false;
			dirty = true;
		}

		// Messages and the SWR scan: audio, end, repeat, scan steps
		if (send_poll(&rx, t, scr.swr))
			changed = true;

		// Scope panel after 10 s
		if (rx.sedit && t - rx.sedit_since >= 10000) {
			rx.sedit = false;
			dirty = true;
		}

		// TX panel after 10 s
		if (rx.xedit && t - rx.xedit_since >= 10000) {
			rx.xedit = false;
			dirty = true;
		}

		// Memory panel after 10 s
		if (rx.medit && t - rx.medit_since >= 10000) {
			rx.medit = false;
			dirty = true;
		}

		// RIT / XIT panel after 10 s
		if (rx.eedit && t - rx.eedit_since >= 10000) {
			rx.eedit = false;
			dirty = true;
		}

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

		//
		// TX timeout (TX panel TOT): our PTT drops; a TX keyed outside Linux (hand mic,
		// VOX) cannot be dropped, its power goes to the minimum until the baseband is in
		// RX again
		//
		uint64_t tot_ms = (uint64_t) g_tot_min[rx.tot] * 60000u;

		// A start can be a ms after t (a repeated message starts in send_poll): only
		// a start before t counts, so t - since cannot wrap
		bool tot_ptt = rx.ptt && t > rx.ptt_since && t - rx.ptt_since >= tot_ms;
		bool tot_send = rx.send != SEND_NONE && t > rx.send_since && t - rx.send_since >= tot_ms;

		if ((tot_ptt || tot_send) && tot_ms > 0) {
			g_send.repeat = false;
			tx_drop(&rx, "TX TIMEOUT");
			changed = true;
		}

		if (rx.bb_tx && rx.bb_tx_since == 0)
			rx.bb_tx_since = t;
		else if (!rx.bb_tx)
			rx.bb_tx_since = 0;

		if (!rx.tot_cut && tot_ms > 0 && rx.bb_tx && !rx.ptt && !rx.tuning && rx.send == SEND_NONE
			&& t - rx.bb_tx_since >= tot_ms) {
			rx.tot_cut = true;
			rx.tot_lock = true;	/* until a key: the mic PTT may still be held */
			rx.warn = "TX TIMEOUT: ANY KEY";
			fprintf(stderr, "\nTX timeout: baseband keyed from outside (hand mic, VOX): TX enable off, power to"
				" minimum until a key\n");
			changed = true;
		} else if (rx.tot_cut && !rx.bb_tx && !rx.tot_lock) {
			rx.tot_cut = false;
			changed = true;
		}

		//
		// Lost I2C writes (OpenX6X00 base_guard.h): the library resends a failed write
		// at once; on top, the whole register block goes out again after every un-key
		// and after a write error, and while the baseband stays in TX within 2 s of an
		// un-key (10 s after an error), every 250 ms
		//
		bool keyed = rx.ptt || rx.tuning || rx.send != SEND_NONE;

		if (was_keyed && !keyed) {
			force_refresh = true;
			watch_from = t + 300;	/* the baseband takes a moment to drop TX */
			watch_until = t + 2000;
			watch_err = false;
		}

		was_keyed = keyed;

		//
		// Still in TX 2 s into a watch armed by a write error (OpenX6X00 tx_watch_power):
		// the power goes to the minimum until the baseband is in RX again (as the TX
		// timeout does for a TX we cannot drop)
		//
		if (rx.bb_tx && !keyed && t >= watch_from && t < watch_until) {
			if (stuck_since == 0)
				stuck_since = t;

			if (t - last_force >= 250) {
				if (!force_refresh)
					fprintf(stderr, "\ni2c: baseband still in TX after un-key, registers sent again\n");

				force_refresh = true;
			}

			if (watch_err && !rx.tot_cut && t - stuck_since >= 2000) {
				rx.tot_cut = true;
				rx.warn = "TX STUCK: POWER MIN";
				fprintf(stderr, "\ni2c: baseband in TX for 2 s after a write error, power to minimum\n");
				changed = true;
			}
		} else {
			stuck_since = 0;
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

		for (int e = 0; e < CAT_N; e++) {
			if (cats[e].fd >= 0)
				cat_scope(&cats[e], t);
		}

		//
		// Console bbreset: the baseband reset over its GPIO line, the calibration read
		// again (kept when that fails), then every register sent again
		//
		if (g_bb_reset_req) {
			x6100_cal_t c2;
			int cr = -1;

			g_bb_reset_req = false;
			tx_drop(&rx, "BASEBAND RESET");

			if ((rc = bb_reset_pulse()) < 0) {
				fprintf(stderr, "\nbbreset: %s\n", strerror(-rc));
				toast("BASEBAND RESET FAILED");
			} else {
				for (int n = 1; (cr = x6100_cal_read(&i2c, &c2)) < 0 && n < START_TRIES; n++)
					usleep(START_WAIT_US);

				if (cr == 0)
					cal = c2;

				printf("\nbbreset: baseband reset, calibration %s\n", cr == 0 ? "read again" : "kept (no answer)");
				toast("BASEBAND RESET");
				force_refresh = true;
				last_flow_ms = now_ms();	/* the flow has a few seconds to come back */
			}

			t = now_ms();
			dirty = true;
		}

		//
		// Full refresh resends register 17 too: not while the tuner is searching
		//
		if ((t - last_refresh >= REFRESH_MS || force_refresh) && !rx.tuning) {
			rc = x6100_ctl_refresh(&ctl);
			last_refresh = t;

			if (force_refresh)
				last_force = t;

			force_refresh = false;
		} else {
			rc = x6100_ctl_sync(&ctl);
		}

		if (ctl.resent > 0 || ctl.lost > 0) {
			// Once, then a summary a minute: a hung baseband gave this line every few
			// seconds for hours (2026-11-04 night)
			static uint64_t last_said;
			static unsigned quiet;

			if (last_said == 0 || t - last_said >= 60000) {
				fprintf(stderr, "\ni2c: %u write(s) went through on a resend, %u still pending; all registers again%s\n",
					ctl.resent, ctl.lost, quiet > 0 ? " (and the same before, in the last minute)" : "");
				last_said = t;
				quiet = 0;
			} else {
				quiet++;
			}

			ctl.resent = 0;
			ctl.lost = 0;
			force_refresh = true;
			watch_from = t;
			watch_until = t + 10000;
			watch_err = true;
		}

		//
		// A single NAK happens (seen on a band change); the registers stay pending
		// and go out on the next pass. After 2 s of failures: our TX drops (PTT off is
		// then pending too) and x6100_trx keeps trying, it does not quit: quitting
		// while keyed would leave nobody to send the PTT off
		//
		if (rc < 0) {
			if (i2c_fails++ == 0)
				i2c_fail_since = t;

			if (!g_i2c_down && t - i2c_fail_since >= 2000) {
				fprintf(stderr, "\ni2c: %s, %u failures in 2 s: TX dropped, still trying\n", strerror(-rc), i2c_fails);
				tx_drop(&rx, "I2C ERROR");
				g_send.repeat = false;
				g_i2c_down = true;
				changed = true;
			}
		} else if (i2c_fails > 0) {
			fprintf(stderr, "\ni2c: recovered after %u failed write(s) in %.1f s\n", i2c_fails,
				(double) (t - i2c_fail_since) / 1000.0);
			i2c_fails = 0;
			g_i2c_down = false;
			force_refresh = true;	/* everything again */
		}

		while ((rc = x6100_flow_read(&flow, &frame)) > 0) {
			bool tx = (frame.flags & X6100_FLOW_TX) != 0;
			double swr = frame.vswr * 0.1;

			last_flow_ms = t;

			if (tx != scr.tx)
				dirty = true;

			scr.tx = tx;
			rx.bb_tx = tx;	/* hand mic PTT too: apply() then moves to the TX side */
			scr.pwr = frame.tx_power * 0.1;
			scr.swr = swr < 1.0 ? 1.0 : swr;
			scr.alc = frame.alc_level;
			scr.vbat = frame.vbat * 0.1;
			scr.vext = frame.vext * 0.1;
			// The gauge tops out at 99: the stock UI and IU1WXY show that as 100 %
			scr.batcap = frame.batcap >= 99 ? 100 : frame.batcap;
			scr.ext = (frame.flags & X6100_FLOW_VEXT) != 0;

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
			if (tx && (rx.ptt || rx.send == SEND_MSG) && swr > SWR_LIMIT) {
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

					// The network becomes the tuner memory for the TX frequency
					x6100_atu_from_frame(&frame, &rx.atu_last);
					rx.atu_last_valid = true;
					atu_store(tx_hz(&rx), &rx.atu_last);
					printf("\nATU tuned at %.3f kHz: L=%u C=%u S=%u (%d tuner memories)\n", tx_hz(&rx) / 1000.0,
						rx.atu_last.l, rx.atu_last.c, rx.atu_last.s, g_natum);

					rx.atu_on = true;
					state_save(&rx);	/* the new tuner memory on disk at once, not only at exit */
					apply(&ctl, &cal, &rx);
					apply_flags(&ctl, &rx);
					dirty = true;
				}
			}

			if (bscan_running()) {
				bscan_feed(&rx, &frame);
				dirty = true;
			}

			level += iq_level_db(&frame);
			level_n++;

			// SCOPE DURING TX off: the scope and the waterfall keep the last RX picture.
			// Without a display only for the squelch (it needs the passband level)
			if ((scr.on || rx.sql > 0) && (g_sp[SP_TXSCOPE] || !scr.tx)) {
				g_iq_hz = iq_hz(&rx);	/* max hold starts again when the IQ centre moves */
				g_dc_hide = lo_shift(&rx) != 0 && g_keyed_hz == 0;	/* TX: the carrier is there */
				spectrum_update(&scr, &frame);
				scr.sig_dbm = passband_dbm(&scr, &rx);
				new_frame = true;	/* meters redrawn every frame, also on HOLD */
			}

			sql_update(&rx, scr.sig_dbm, scr.tx || rx.ptt);
			sql_capture();	/* the audio to Linux (wfserver) as well */

			if (!scr.tx)
				sql_noise_track(scr.sig_dbm);	/* for SQL push */
		}

		if (t - last_meter >= METER_MS && level_n > 0) {
			scr.meter_db = level / level_n;
			// Readings go on the status line, before a line being typed: RX level, or
			// while transmitting power, SWR and ALC
			g_tx_show = scr.tx;
			g_tx_pwr = scr.pwr;
			g_tx_swr = scr.swr;
			g_tx_alc = scr.alc;
			g_iq_db = scr.meter_db;
			g_vbat = frame.vbat * 0.1;
			g_batcap = scr.batcap;
			g_ext = scr.ext;

			// Low battery: a note at 20 % and at 10 %, once each on the way down
			{
				static int warned = 100;

				if (!scr.ext && scr.batcap > 0 && scr.batcap <= 10 && warned > 10) {
					toast("BATTERY 10 %");
					warned = 10;
				} else if (!scr.ext && scr.batcap > 10 && scr.batcap <= 20 && warned > 20) {
					toast("BATTERY 20 %");
					warned = 20;
				} else if (scr.ext || scr.batcap > 25) {
					warned = 100;
				}
			}
			show(&rx);
			fflush(stdout);
			level = 0.0;
			level_n = 0;
			last_meter = t;
			dirty = true;
		}

		// The console went away (SIGHUP: the ssh session closed, the WiFi gone): x6100_trx
		// goes on without it, a birdie scan, CAT and TX included (before, SIGHUP killed it:
		// no state saved). Its input is dropped, its output goes to /dev/null
		if (g_hup) {
			g_hup = 0;
			pfd[stdin_slot].fd = -1;
			(void) !freopen("/dev/null", "w", stdout);
			(void) !freopen("/dev/null", "w", stderr);
			toast("CONSOLE GONE");
			dirty = true;
		}

		// Baseband watchdog: no flow frames for BB_DEAD_MS is a hung baseband (2026-11-04
		// night: no flow and no I2C for 6.8 h, the screen stood still). Said once, shown on
		// the screen, which is redrawn every second meanwhile; bbreset resets it
		{
			bool dead = t - last_flow_ms >= BB_DEAD_MS;

			if (dead != g_bb_dead) {
				if (dead)
					fprintf(stderr, "\nbaseband: no flow for %d s, no answer? bbreset on the console resets it\n",
						BB_DEAD_MS / 1000);
				else
					fprintf(stderr, "\nbaseband: flow again\n");

				g_bb_dead = dead;
				dirty = true;
			}

			if (dead && t - last_disp >= 1000)
				dirty = true;
		}

		// Layout: portrait while no panel is open (they are landscape); a turn clears the
		// canvas and starts the scope and waterfall again
		{
			int rot = g_layout_portrait && !panel_open(&rx) ? g_layout_rot : X6100_LCD_ROTATION;

			if (scr.lcd.rot != rot && x6100_lcd_rotate(&scr.lcd, rot) == 0) {
				x6100_lcd_fill(&scr.lcd, 0, 0, scr.lcd.w, scr.lcd.h, 0x000000);
				g_spec_h = 0;			/* re-laid out on the next draw */
				scr.wf_view = -1;
				dirty = true;
			}
		}

		// Auto lock in portrait: the VFO and MFK lock after g_autolock_s without a turn or
		// a key; LOCK unlocks (PTT and volume always work)
		if (g_layout_portrait && g_autolock_s > 0 && !rx.lock && !panel_open(&rx) && !rx.ptt
			&& t - g_last_turn_ms >= (uint64_t) g_autolock_s * 1000u) {
			rx.lock = true;
			g_lock_auto = true;
			dirty = true;
		}

		// VOL knob back to the volume VOL_FN_MS after its last turn as SQL / RFG
		if (g_vol_fn != 0 && t - g_vol_fn_ms >= VOL_FN_MS) {
			g_vol_fn = 0;
			dirty = true;
		}

		// Back to landscape: the auto lock is portrait's, so it goes (2026-11-05: a portrait
		// lock came along into landscape and caught the operator off guard); a lock set by
		// hand (LOCK, CAT) stays
		if (!g_layout_portrait && g_lock_auto) {
			if (rx.lock)
				rx.lock = false;

			g_lock_auto = false;
			dirty = true;
		}

		// Screenshot (shot, LOCK long): the canvas as last drawn
		if (g_shot_req) {
			if (scr.on)
				shot_take(&scr.lcd);

			g_shot_req = false;
			dirty = true;	/* the toast */
		}

		if (scr.on && (new_frame || dirty) && t - last_disp >= DISP_MS) {
			draw_screen(&scr, &rx, new_frame && !g_scope_hold);	/* HOLD: no new waterfall line */
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
	bscan_stop(&rx, false);	/* quit during a scan: the VFO comes back first */

	if (cats[CAT_EP_PTY].pty_name[0] != '\0')
		unlink(CAT_PTY_LINK);	/* the pty goes with us */

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
	if (g_sql_cap == 0)	/* a closed squelch left the capture off: on for the GUI / wfserver after us */
		x6100_mixer_set(X6100_PCM_CARD, "AIF1 Data Digital ADC Capture Switch", 1);

	if (fifo_slot >= 0 && pfd[fifo_slot].fd >= 0) {	/* the command pipe goes with us */
		close(pfd[fifo_slot].fd);
		unlink(CMD_FIFO);
	}

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
		FILE *mk = fopen("/tmp/x6100_trx.poweroff", "w");	/* the boot wrapper: no GUI */

		if (mk != NULL)
			fclose(mk);

		printf("powering off\n");
		sync();
		execlp("poweroff", "poweroff", (char *) NULL);
		perror("poweroff");
		return 1;
	}

	return 0;
}
