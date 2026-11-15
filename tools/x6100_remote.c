/*
 * x6100_remote.c - x6100_trx as a control head for a remote Icom (receive only)
 *
 * See x6100_remote.h. One session (libx6100_app core: its own network thread),
 * plus an audio thread here: session RX audio -> codec playback (48 kHz stereo),
 * which the baseband plays on the speaker in play mode.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100_remote.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "x6100_pcm.h"

#include "core/audio_decode.h"
#include "core/rig.h"		/* RIG_CONTROLLER_ADDR, for our own ID query */
#include "core/session.h"
#include "model/radio_state.h"

#define C_RATE		48000		/* codec playback */
#define C_NET_RATE	16000		/* the stream from the rig, resampled to C_RATE by the core */
#define C_PERIOD	480		/* 10 ms */
#define C_PERIODS	4
#define C_TAP		16384		/* the decoder tap: about 340 ms at C_RATE (a power of 2) */
#define C_ECHO_MS	400		/* rig reports this soon after our own change: our echo */
#define C_ATT_DB	12		/* ATT key on: the Icom attenuator step used (0 = off) */

// The controls, as indices (send times, last rig values) and their X6100_REMOTE_C_* bits
enum { C_AGC, C_ATT, C_PRE, C_RFG, C_NR, C_NB, C_ATU, C_CTLS };
#define C_SCOPE_HALF	50000		/* the rig scope, centre mode +- 50 kHz: the X6100 IQ width (SPAN_HZ) */
#define C_SCOPE_TRIES	5		/* scope mode / span sent so often after connecting at most, then left alone */
#define C_FIL_HOLD_MS	700		/* after a mode / filter change: the old Filter Width answer not shown so long */
#define C_TUNE_WATCH_MS	30000		/* a rig tune watched so long at most */

//
// Mode numbers as wfview has them (.rig Modes\N\Num: its own list, not the CI-V Reg;
// they part at CW-R: Num 6, Reg 7; Num 7 is RTTY-R, 2026-11-12: CW-R came out as RTTY-R)
//

#define C_ICOM_LSB	0
#define C_ICOM_USB	1
#define C_ICOM_AM	2
#define C_ICOM_CW	3
#define C_ICOM_FM	5
#define C_ICOM_CWR	6

struct x6100_remote
{
	//
	// Link
	//
	struct session *s;

	//
	// The rig's DATA OFF MOD (2026-11-14), TX via the dummy only: read when the rig file
	// is there, set to LAN, the old value back at the close (lanmod_*)
	//
	bool lan_mod;			/* cfg.tx_dummy */
	int lan_state;			/* LANMOD_* */
	int lan_reg;			/* the value of LAN (the rig file's Inputs) */
	int lan_old;			/* what the rig had */
	int lan_tries;
	int64_t lan_ask_ms;
	uint8_t lan_cmd[8];		/* the command bytes, to know its answer */
	size_t lan_cmd_n;
	atomic_int lan_seen;		/* its value from the rig (CI-V hook), -1 none yet */

	//
	// What we last sent (valid after the first apply)
	//
	bool sent_valid;
	x6100_remote_rx_t sent;
	int64_t freq_sent_ms;
	int64_t mode_sent_ms;
	int64_t ctl_sent_ms[C_CTLS];	/* per control (C_* below): last sent, for the echo guard */

	//
	// Logged on a change (stderr)
	//
	int log_state;
	int log_reason;
	int log_ctl;
	uint32_t log_rejected;
	int64_t log_audio_ms;
	uint32_t log_under;
	uint32_t log_over;
	uint32_t log_late;
	uint32_t log_decode;
	uint32_t log_xrun;
	atomic_uint xruns;		/* playback write failures (audio thread) */

	//
	// The received audio for the CW decoder (2026-11-12): the left channel at C_RATE, one
	// writer (the audio thread), one reader (the main loop); full: the newest are dropped
	//
	int16_t tap[C_TAP];
	atomic_size_t tap_w;		/* samples written, ever */
	atomic_size_t tap_r;		/* samples read, ever */

	//
	// What the rig last reported
	//
	uint64_t rig_freq;
	int rig_mode_num;
	int rig_mode_data;
	int rig_filter;
	int rig_ctl[C_CTLS];			/* the controls as the rig last reported them, -1 none yet */
	bool log_caps;			/* "rig file loaded" said */
	bool log_nocaps;		/* "no CI-V ID yet" said */
	bool log_idask;			/* "asking the ID directly" said */
	int64_t id_sent_ms;		/* our own ID query to the login address, last sent */
	int64_t ctl_poll_ms;		/* our own NR / NB poll, last sent */
	int ctl_poll_i;			/* next one of our poll: NR, NB, RF gain, CW pitch, RF power */
	int log_rfpwr;			/* RF power as last logged, -2 never */
	int64_t streams_ms;		/* when the streams opened (0: not yet), for those two */
	uint32_t scope_seq;
	bool scope_seen;
	int64_t scope_set_ms;		/* last scope mode / span sent */
	int scope_tries;		/* scope mode / span sends so far (C_SCOPE_TRIES, then left alone) */
	int64_t fil_hold_ms;		/* mode / filter changed then: the Filter Width before it not used for C_FIL_HOLD_MS */
	int64_t tune_ms;		/* our tune sent then, 0 none: its tuner watched (C_TUNE_WATCH_MS) */
	int64_t tune_ask_ms;		/* the tuner last asked (each second, each 0.5 s while tuning) */
	uint64_t row_width;		/* width of the last scope row, 0: none yet */
	bool smeter_valid;
	double smeter_dbm;

	//
	// Audio thread
	//
	pthread_t audio;
	bool audio_running;
	atomic_bool stop;
	x6100_pcm_t pcm;
};

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

//
// Mode mapping, X6100 <-> Icom
//

static void mode_to_icom(x6100_mode_t m, int *num, int *data)
{
	*data = 0;

	switch (m) {
	case X6100_MODE_LSB_DIG:
		*data = 1;
		/* fall through */
	case X6100_MODE_LSB:
		*num = C_ICOM_LSB;
		break;
	case X6100_MODE_USB_DIG:
		*data = 1;
		/* fall through */
	case X6100_MODE_USB:
		*num = C_ICOM_USB;
		break;
	case X6100_MODE_CW:
		*num = C_ICOM_CW;
		break;
	case X6100_MODE_CWR:
		*num = C_ICOM_CWR;
		break;
	case X6100_MODE_AM:
		*num = C_ICOM_AM;
		break;
	case X6100_MODE_NFM:
	default:
		*num = C_ICOM_FM;
		break;
	}
}

static bool mode_from_icom(int num, int data, x6100_mode_t *m)
{
	switch (num) {
	case C_ICOM_LSB:
		*m = data ? X6100_MODE_LSB_DIG : X6100_MODE_LSB;
		return true;
	case C_ICOM_USB:
		*m = data ? X6100_MODE_USB_DIG : X6100_MODE_USB;
		return true;
	case C_ICOM_AM:
		*m = X6100_MODE_AM;
		return true;
	case C_ICOM_CW:
		*m = X6100_MODE_CW;
		return true;
	case C_ICOM_CWR:
		*m = X6100_MODE_CWR;
		return true;
	case C_ICOM_FM:
		*m = X6100_MODE_NFM;
		return true;
	default:
		return false;	/* RTTY, PSK, DV ...: not on the X6100 UI, left alone */
	}
}

//
// X6100 AGC (OFF 0, SLOW 1, FAST 2, AUTO 3) -> Icom (FAST 1, MID 2, SLOW 3); OFF
// cannot be set over CI-V: -1
//

static int agc_to_icom(x6100_agc_t a)
{
	switch (a) {
	case X6100_AGC_FAST:
		return 1;
	case X6100_AGC_AUTO:
		return 2;
	case X6100_AGC_SLOW:
		return 3;
	default:
		return -1;
	}
}

// Icom AGC (1 FAST, 2 MID, 3 SLOW) as the X6100 has it: MID as AUTO, as agc_to_icom
static x6100_agc_t agc_from_icom(int v)
{
	return v == 1 ? X6100_AGC_FAST : v == 3 ? X6100_AGC_SLOW : X6100_AGC_AUTO;
}

//
// Audio thread: session RX audio (float stereo) -> codec playback (S16 stereo).
// x6100_pcm_write blocks: it paces the loop
//

static void *audio_main(void *arg)
{
	x6100_remote_t *r = arg;
	float in[C_PERIOD * 2];
	int16_t out[C_PERIOD * 2];

	while (!atomic_load(&r->stop)) {
		session_audio_read(r->s, in, C_PERIOD);

		for (size_t i = 0; i < C_PERIOD * 2; i++) {
			float v = in[i] * 32767.0f;

			out[i] = (int16_t) (v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
		}

		// The left channel into the decoder tap, while there is room
		{
			size_t w = atomic_load_explicit(&r->tap_w, memory_order_relaxed);
			size_t rd = atomic_load_explicit(&r->tap_r, memory_order_acquire);

			for (size_t i = 0; i < C_PERIOD && w - rd < C_TAP; i++, w++)
				r->tap[w % C_TAP] = out[i * 2];

			atomic_store_explicit(&r->tap_w, w, memory_order_release);
		}

		if (x6100_pcm_write(&r->pcm, out, C_PERIOD) < 0)
			atomic_fetch_add(&r->xruns, 1);	/* counted, logged from the main loop; the pcm layer recovers */
	}

	return NULL;
}

//
// DATA OFF MOD on the rig (2026-11-14): TX via the dummy needs the rig's voice input on
// LAN, so the rig's own mic was off for local use after every session. Now set on the
// way in and put back on the way out: when the rig file is loaded, its Data Off Mod
// Input is asked for (the answer is picked out of the CI-V stream by its command bytes,
// the core keeps no value for it), set to the rig file's LAN input when it is not
// already, and the old value is sent again at the close. Without that command or a LAN
// input in the rig file: set it on the rig by hand. A kill -9 leaves LAN on the rig
//

#define LANMOD_TYPE	"Data Off Mod Input"
#define LANMOD_TRIES	3

enum { LANMOD_IDLE, LANMOD_ASKED, LANMOD_SET, LANMOD_DONE };

// Network thread: an answer to our ask is FE FE E1 <rig> <command> <value> FD; a packet
// may hold more than one frame, so each is looked at
static void lanmod_hook(void *ctx, const uint8_t *civ, size_t len)
{
	x6100_remote_t *r = ctx;
	const size_t n = r->lan_cmd_n;

	if (n == 0)
		return;

	for (size_t a = 0; a + 1 < len; a++) {
		size_t e;

		if (civ[a] != 0xFE || civ[a + 1] != 0xFE)
			continue;

		for (e = a + 2; e < len && civ[e] != 0xFD; e++)
			;

		if (e >= len)
			return;

		const uint8_t *f = civ + a;
		const size_t fl = e - a + 1;

		if (fl == 6 + n && f[2] == RIG_CONTROLLER_ADDR && memcmp(f + 4, r->lan_cmd, n) == 0)
			atomic_store(&r->lan_seen, (f[4 + n] >> 4) * 10 + (f[4 + n] & 0x0F));	/* BCD */

		a = e;
	}
}

// The set as a CI-V frame of our own: the core encodes values only for the kinds it
// knows, not this one (RIG_ERR_UNSUPPORTED). One BCD byte, as the rigs take it
static int lanmod_send(x6100_remote_t *r, int value)
{
	struct session_info info;
	uint8_t f[16];
	size_t n = 0;

	session_get_info(r->s, &info);

	if (info.civ_addr == 0x00 || value < 0 || value > 99 || r->lan_cmd_n + 6 > sizeof(f))
		return -1;

	f[n++] = 0xFE;
	f[n++] = 0xFE;
	f[n++] = info.civ_addr;
	f[n++] = RIG_CONTROLLER_ADDR;
	memcpy(f + n, r->lan_cmd, r->lan_cmd_n);
	n += r->lan_cmd_n;
	f[n++] = (uint8_t) ((value / 10) << 4 | value % 10);
	f[n++] = 0xFD;
	return session_civ_send(r->s, f, n);
}

static const char *lanmod_name(const struct rig_def *def, int reg)
{
	for (size_t i = 0; i < def->n_inputs; i++)
		if (def->inputs[i].reg == reg)
			return def->inputs[i].name;

	return "?";
}

// From the poll, once the rig file is there
static void lanmod_poll(x6100_remote_t *r, int64_t t)
{
	const struct rig_def *def = session_rig_def(r->s);
	const struct rig_cmd *c;
	int v;

	if (!r->lan_mod || def == NULL || r->lan_state == LANMOD_SET || r->lan_state == LANMOD_DONE)
		return;

	if (r->lan_state == LANMOD_IDLE) {
		r->lan_reg = -1;

		for (size_t i = 0; i < def->n_inputs; i++)
			if (strcmp(def->inputs[i].name, "LAN") == 0)
				r->lan_reg = def->inputs[i].reg;

		c = rigfile_cmd(def, LANMOD_TYPE);

		if (c == NULL || !c->get || !c->set || r->lan_reg < 0 || c->n == 0 || c->n > sizeof(r->lan_cmd)) {
			fprintf(stderr, "\nremote: the rig file has no %s with LAN: set DATA OFF MOD to LAN on the rig by hand\n",
				LANMOD_TYPE);
			r->lan_state = LANMOD_DONE;
			return;
		}

		memcpy(r->lan_cmd, c->bytes, c->n);
		r->lan_cmd_n = c->n;
		atomic_store(&r->lan_seen, -1);
		r->lan_tries = 0;
		r->lan_state = LANMOD_ASKED;
		r->lan_ask_ms = 0;
	}

	if ((v = atomic_load(&r->lan_seen)) >= 0) {
		r->lan_old = v;

		if (v == r->lan_reg) {
			fprintf(stderr, "\nremote: DATA OFF MOD already LAN on the rig: left as it is\n");
			r->lan_state = LANMOD_DONE;
		} else if (lanmod_send(r, r->lan_reg) == 0) {
			fprintf(stderr, "\nremote: DATA OFF MOD was %s, now LAN; %s again when the head lets go\n",
				lanmod_name(def, v), lanmod_name(def, v));
			r->lan_state = LANMOD_SET;
		} else {
			fprintf(stderr, "\nremote: DATA OFF MOD could not be set: set it to LAN on the rig by hand\n");
			r->lan_state = LANMOD_DONE;
		}

		return;
	}

	if (t - r->lan_ask_ms >= 1500) {
		if (r->lan_tries++ >= LANMOD_TRIES) {
			fprintf(stderr, "\nremote: no answer for DATA OFF MOD: not changed, set it to LAN on the rig by hand\n");
			r->lan_state = LANMOD_DONE;
			return;
		}

		session_get_value(r->s, LANMOD_TYPE, 0);
		r->lan_ask_ms = t;
	}
}

// At the close: the rig's own value back, sent before the link goes
static void lanmod_restore(x6100_remote_t *r)
{
	const struct rig_def *def = session_rig_def(r->s);

	if (r->lan_state != LANMOD_SET)
		return;

	if (lanmod_send(r, r->lan_old) == 0) {
		fprintf(stderr, "\nremote: DATA OFF MOD back to %s\n", def != NULL ? lanmod_name(def, r->lan_old) : "as it was");
		usleep(300000);	/* on its way before the link goes */
	} else {
		fprintf(stderr, "\nremote: DATA OFF MOD could not be put back: LAN stays on the rig\n");
	}

	r->lan_state = LANMOD_DONE;
}

int x6100_remote_open(x6100_remote_t **out, const x6100_remote_cfg_t *cfg)
{
	struct profile p;
	struct session_hooks h;
	x6100_remote_t *r;
	int rc;

	*out = NULL;

	if ((r = calloc(1, sizeof(*r))) == NULL)
		return -ENOMEM;

	r->rig_mode_num = -1;

	for (int i = 0; i < C_CTLS; i++)
		r->rig_ctl[i] = -1;
	r->log_state = -1;
	r->log_rfpwr = -2;
	atomic_init(&r->stop, false);
	atomic_init(&r->xruns, 0);
	atomic_init(&r->tap_w, 0);
	atomic_init(&r->tap_r, 0);

	memset(&p, 0, sizeof(p));
	snprintf(p.host, sizeof(p.host), "%s", cfg->host);
	p.port_control = cfg->port;
	snprintf(p.username, sizeof(p.username), "%s", cfg->user);
	snprintf(p.password, sizeof(p.password), "%s", cfg->pass);
	snprintf(p.client_name, sizeof(p.client_name), "%s", "x6100trx");
	snprintf(p.rig_dir, sizeof(p.rig_dir), "%s", cfg->rig_dir);

	//
	// RX: LPCM 16 bit stereo at C_NET_RATE (no decode cost on the R16), 150 ms as wfview.
	// TX: the stream request still carries a TX codec, rate and buffer as wfview
	// sends them (a rig that announces TX ignores a request with zeros there: no
	// streams, no LAN on the 7851). Receive only: no capture rate, PTT disabled. With
	// tx_dummy: the capture at C_RATE (the X6100 monitor), PTT with a 500 ms heartbeat
	// and the rig's time-out at 180 s
	//
	p.rx_codec = AUDIO_CODEC_PCM16_2;
	p.rx_sample = C_NET_RATE;
	p.rx_latency = 150;
	p.tx_codec = AUDIO_CODEC_PCM16_1;
	p.tx_sample = C_NET_RATE;	/* 16 kHz as RX (2026-11-14: 48 kHz, about 770 kbit/s over the internet; resampled from the capture by the core) */
	p.tx_latency = 150;
	p.dev_rate = C_RATE;
	p.cap_rate = cfg->tx_dummy ? C_RATE : 0;
	p.enable_ptt = cfg->tx_dummy;
	p.ptt_heartbeat_ms = cfg->tx_dummy ? 500 : 0;
	p.mic_gain = 100;	/* the TX gain in % (2026-11-14: left at 0 by the memset, the rig got silence) */
	p.ptt_timeout_enabled = true;
	p.ptt_timeout_s = 180;

	memset(&h, 0, sizeof(h));
	h.civ_data = lanmod_hook;	/* the rig's answer to our DATA OFF MOD ask */
	h.ctx = r;
	r->lan_mod = cfg->tx_dummy;
	atomic_init(&r->lan_seen, -1);

	if ((rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0
		|| (rc = x6100_pcm_open(&r->pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, false, C_RATE, 2, C_PERIOD,
			C_PERIODS)) < 0) {
		fprintf(stderr, "remote: playback: %s\n", strerror(-rc));
		free(r);
		return rc;
	}

	fprintf(stderr, "remote: playback %u Hz, stream %u Hz\n", r->pcm.rate, (unsigned) C_NET_RATE);

	if (session_connect(&r->s, &p, &h) != 0) {
		x6100_pcm_close(&r->pcm);
		free(r);
		return -EIO;
	}

	if ((rc = pthread_create(&r->audio, NULL, audio_main, r)) != 0) {
		session_disconnect(r->s);
		x6100_pcm_close(&r->pcm);
		free(r);
		return -rc;
	}

	// Playback ahead of the UI: a busy main loop must not starve it (root on the
	// radio; without the right it runs at normal priority)
	{
		struct sched_param sp = { .sched_priority = 10 };

		if (pthread_setschedparam(r->audio, SCHED_FIFO, &sp) != 0)
			fprintf(stderr, "%s\n", "remote: audio thread at normal priority");
	}

	r->audio_running = true;
	*out = r;
	return 0;
}

void x6100_remote_close(x6100_remote_t *r)
{
	if (r == NULL)
		return;

	lanmod_restore(r);	/* the rig's own DATA OFF MOD, while the link is still up */
	atomic_store(&r->stop, true);

	if (r->audio_running)
		pthread_join(r->audio, NULL);

	session_disconnect(r->s);
	x6100_pcm_close(&r->pcm);
	free(r);
}

void x6100_remote_apply(x6100_remote_t *r, const x6100_remote_rx_t *rx)
{
	const x6100_remote_rx_t *o = &r->sent;
	bool all = !r->sent_valid;
	int64_t t = now_ms();
	struct session_info info;

	//
	// Nothing until the streams are open: the session would reject it. Then one full
	// push, after that only changes
	//
	session_get_info(r->s, &info);

	if (info.state != SESSION_CONNECTED || info.control_state != CTL_STATE_STREAMS_OPEN)
		return;

	if (all || rx->freq_hz != o->freq_hz) {
		if (session_set_freq(r->s, 0, rx->freq_hz) == 0)
			r->freq_sent_ms = t;
	}

	if (all || rx->mode != o->mode || rx->filter != o->filter) {
		int num, data;

		mode_to_icom(rx->mode, &num, &data);

		if (session_set_mode(r->s, 0, num, data, rx->filter + 1) == 0)
			r->mode_sent_ms = t;

		// The width of the new filter at once; the old answer not shown meanwhile
		r->fil_hold_ms = t;
		session_get_value(r->s, "Filter Width", 0);
	}

	// The controls; each send noted for the echo guard in poll
	if ((all || rx->agc != o->agc) && agc_to_icom(rx->agc) > 0) {
		session_set_value(r->s, "AGC Status", 0, agc_to_icom(rx->agc));
		r->ctl_sent_ms[C_AGC] = t;
	}

	if (all || rx->att != o->att) {
		session_set_value(r->s, "Attenuator Status", 0, rx->att ? C_ATT_DB : 0);
		r->ctl_sent_ms[C_ATT] = t;
	}

	if (all || rx->pre != o->pre) {
		session_set_value(r->s, "Preamp Status", 0, rx->pre ? 1 : 0);
		r->ctl_sent_ms[C_PRE] = t;
	}

	if (all || rx->rfg != o->rfg) {
		session_set_value(r->s, "RF Gain", 0, (rx->rfg * 255 + 50) / 100);
		r->ctl_sent_ms[C_RFG] = t;
	}

	if (all || rx->nr != o->nr) {
		session_set_value(r->s, "Noise Reduction", 0, rx->nr);
		r->ctl_sent_ms[C_NR] = t;
	}

	if (all || rx->nb != o->nb) {
		session_set_value(r->s, "Noise Blanker", 0, rx->nb);
		r->ctl_sent_ms[C_NB] = t;
	}

	// The tuner on / off (2026-11-14): only a change on the head goes out, not the first
	// push, so the rig keeps its own state at connect and the head takes it (poll)
	if (!all && rx->atu != o->atu) {
		session_set_value(r->s, "Tuner/ATU Status", 0, rx->atu ? 1 : 0);
		r->ctl_sent_ms[C_ATU] = t;
	}

	r->sent = *rx;
	r->sent_valid = true;
}

void x6100_remote_poll(x6100_remote_t *r, x6100_remote_status_t *st)
{
	static struct radio_state rs;	/* large: not on the stack; one caller (the main loop) */
	struct session_info info;
	int64_t t = now_ms();

	memset(st, 0, sizeof(*st));

	session_get_info(r->s, &info);
	st->connected = info.state == SESSION_CONNECTED;
	snprintf(st->state, sizeof(st->state), "%s", session_state_name(info.state));
	snprintf(st->radio, sizeof(st->radio), "%s", info.radio_name);

	if ((int) info.state != r->log_state || (int) info.reason != r->log_reason
		|| (int) info.control_state != r->log_ctl) {
		fprintf(stderr, "\nremote: %s, reason %s, control %s, radio '%s' civ 0x%02x: %s\n",
			session_state_name(info.state), session_reason_name(info.reason),
			control_state_name(info.control_state), info.radio_name, info.civ_addr, info.status);
		r->log_state = (int) info.state;
		r->log_reason = (int) info.reason;
		r->log_ctl = (int) info.control_state;
	}

	if (info.ctl_rejected != r->log_rejected) {
		fprintf(stderr, "\nremote: %u control(s) refused, last error %d\n", info.ctl_rejected, info.ctl_last_error);
		r->log_rejected = info.ctl_rejected;
	}

	//
	// Audio: the counters every 2 s while any of them moves (crackle hunt)
	//
	if (t - r->log_audio_ms >= 2000) {
		uint32_t xr = atomic_load(&r->xruns);

		if (info.audio_underruns != r->log_under || info.audio_overrun_frames != r->log_over
			|| info.audio_dropped_late != r->log_late || info.audio_decode_errors != r->log_decode
			|| xr != r->log_xrun) {
			fprintf(stderr, "\nremote audio: underruns %u (+%u), overrun frames %u (+%u), late %u (+%u), "
				"decode errors %u, playback xruns %u (+%u), latency %d ms, lateness %d ms\n",
				info.audio_underruns, info.audio_underruns - r->log_under,
				info.audio_overrun_frames, info.audio_overrun_frames - r->log_over,
				info.audio_dropped_late, info.audio_dropped_late - r->log_late, info.audio_decode_errors,
				xr, xr - r->log_xrun, info.audio_latency_ms, info.audio_lateness_ms);
			r->log_under = info.audio_underruns;
			r->log_over = info.audio_overrun_frames;
			r->log_late = info.audio_dropped_late;
			r->log_decode = info.audio_decode_errors;
			r->log_xrun = xr;
		}

		r->log_audio_ms = t;
	}

	//
	// Link down: everything is pushed again once it is back (a rig restart loses
	// nothing of ours), no meter meanwhile
	//
	if (!st->connected) {
		r->sent_valid = false;
		r->smeter_valid = false;
		r->streams_ms = 0;	/* the rig file notes again at the next connect */
		r->log_caps = false;
		r->log_nocaps = false;
		r->log_idask = false;
		return;
	}

	if (r->streams_ms == 0)
		r->streams_ms = t;

	session_get_state(r->s, &rs);

	// The core asks the ID at the broadcast address 00 until it knows the rig; the 7851
	// at times does not answer that while its CI-V stream does flow (2026-11-12: 138
	// frames in, no ID, no rig file). The login gave its address: ask there as well,
	// once a second, from the core's controller address, so the answer takes the usual
	// way (and loads the rig file)
	if (!rs.rig_valid && info.civ_addr != 0x00 && t - r->streams_ms >= 1500 && t - r->id_sent_ms >= 1000) {
		const uint8_t q[] = { 0xFE, 0xFE, info.civ_addr, RIG_CONTROLLER_ADDR, 0x19, 0x00, 0xFD };

		if (session_civ_send(r->s, q, sizeof(q)) == 0 && !r->log_idask) {
			fprintf(stderr, "\nremote: asking the CI-V ID at 0x%02x directly\n", info.civ_addr);
			r->log_idask = true;
		}

		r->id_sent_ms = t;
	}

	// NR, NB and RF gain of the main receiver, asked by us: the IC-785x file polls NR / NB
	// Medium Low for both receivers into one field (about 10 s, and the sub's answer undoes
	// the main's) and RF gain Medium (seconds; 2026-11-12: the number followed slowly).
	// One every 333 ms, by turns: each about every 1.7 s. CW Pitch too (not in the file's
	// poll): the CW decoder listens at the rig's pitch. RF Power too (2026-11-14: 0 % shown
	// with the 7851 at 200 W; the file polls it Medium): under the TX box
	if (rs.rig_valid && t - r->ctl_poll_ms >= 333) {
		// Filter Width too (2026-11-14: the head showed its own widths, 2.8 / 2.4 / 1.8 k,
		// with the 7851 on 3.0 / 2.6 / 1.8 k); each now about every 2 s
		static const char *const poll[6] = { "Noise Reduction", "Noise Blanker", "RF Gain", "CW Pitch", "RF Power",
			"Filter Width" };

		session_get_value(r->s, poll[r->ctl_poll_i], 0);
		r->ctl_poll_i = (r->ctl_poll_i + 1) % 6;
		r->ctl_poll_ms = t;
	}

	// The tuner (2026-11-14): once a second on its own (in the rotation it came every
	// 2.3 s, so the TUNER key on the rig showed late; Icom does not announce it), twice a
	// second while the rig tunes (ours or one started on the rig), at most C_TUNE_WATCH_MS
	{
		const bool watch = r->tune_ms != 0 && t - r->tune_ms < C_TUNE_WATCH_MS;

		if (rs.rig_valid && t - r->tune_ask_ms >= (watch ? 500 : 1000)) {
			session_get_value(r->s, "Tuner/ATU Status", 0);
			r->tune_ask_ms = t;
		}
	}

	st->rig_tuning = rs.tuner.valid && rs.tuner.value == 2;

	// A tune started on the rig (its TUNER key) watched the same way as ours, so TUNE
	// goes off when it is done, not at the next turn of our poll (about 2.3 s)
	if (st->rig_tuning && r->tune_ms == 0) {
		r->tune_ms = t;
		r->tune_ask_ms = t;
	}

	if (r->tune_ms != 0 && t - r->tune_ms >= 1500 && !st->rig_tuning)
		r->tune_ms = 0;	/* done (or never started): the watch ends */

	// The rig file: loaded once the rig has answered its CI-V ID (19 00); until then every
	// set is refused (RIG_ERR_NO_CAPS -1), so say when it is there, and when it is long
	// in coming (2026-11-12: 20 refused in 10 s)
	st->rig_ready = rs.rig_valid;

	if (rs.rig_valid)
		lanmod_poll(r, t);	/* DATA OFF MOD to LAN for TX via the dummy */
	st->cw_pitch = rs.cw_pitch.valid ? rs.cw_pitch.value : 0;
	st->rf_power = rs.rf_power.valid ? rs.rf_power.value : -1;
	st->filter_hz = rs.filter_width.valid && t - r->fil_hold_ms >= C_FIL_HOLD_MS ? rs.filter_width.value : 0;

	// RF power each change, for the log (2026-11-14: 0 % shown with the 7851 at 200 W)
	if (st->rf_power != r->log_rfpwr) {
		fprintf(stderr, "\nremote: rig RF power %d (0-255)\n", st->rf_power);
		r->log_rfpwr = st->rf_power;
	}

	if (rs.rig_valid && !r->log_caps) {
		fprintf(stderr, "\nremote: rig file for %s (CI-V 0x%02x) loaded, %lld ms after the streams\n", rs.model,
			rs.model_id, (long long) (t - r->streams_ms));
		r->log_caps = true;
	} else if (!rs.rig_valid && r->streams_ms != 0 && !r->log_nocaps && t - r->streams_ms >= 5000) {
		fprintf(stderr, "\nremote: no CI-V ID (19 00) from the rig after 5 s: no rig file yet, controls refused; "
			"CI-V frames in so far %u (0: the CI-V stream does not flow)\n", info.civ_frames);
		r->log_nocaps = true;
	}

	//
	// Frequency: a new report that differs from what we sent, and not just after
	// our own change (the echo of an older step while the knob still turns)
	//
	if (rs.rx[0].freq_valid[0] && rs.rx[0].freq_hz[0] != r->rig_freq) {
		r->rig_freq = rs.rx[0].freq_hz[0];

		if (r->sent_valid && r->rig_freq != r->sent.freq_hz && t - r->freq_sent_ms >= C_ECHO_MS) {
			st->freq_changed = true;
			st->freq_hz = (uint32_t) r->rig_freq;
			r->sent.freq_hz = st->freq_hz;	/* taken over by the UI: no resend */
		}
	}

	//
	// Mode and filter, the same way
	//
	if (rs.rx[0].mode[0].valid && (rs.rx[0].mode[0].num != r->rig_mode_num
		|| rs.rx[0].mode[0].data != r->rig_mode_data || rs.rx[0].mode[0].filter != r->rig_filter)) {
		x6100_mode_t m;

		r->rig_mode_num = rs.rx[0].mode[0].num;
		r->rig_mode_data = rs.rx[0].mode[0].data;
		r->rig_filter = rs.rx[0].mode[0].filter;

		// Its filter width anew (also when the change was ours: the rig confirms it)
		r->fil_hold_ms = t;
		session_get_value(r->s, "Filter Width", 0);

		if (r->sent_valid && t - r->mode_sent_ms >= C_ECHO_MS && mode_from_icom(r->rig_mode_num, r->rig_mode_data, &m)
			&& (m != r->sent.mode || r->rig_filter - 1 != r->sent.filter)) {
			st->mode_changed = true;
			st->mode = m;
			st->filter = r->rig_filter >= 1 && r->rig_filter <= 3 ? r->rig_filter - 1 : 0;
			r->sent.mode = m;
			r->sent.filter = st->filter;
		}
	}

	//
	// The controls, the same way (2026-11-12: AGC, ATT, PRE, RF gain, NR and NB only went
	// out; changed on the rig they did not show): a new report that differs from what we
	// sent, and not just after our own send of that control
	//
	{
		static const unsigned bit[C_CTLS] = { X6100_REMOTE_C_AGC, X6100_REMOTE_C_ATT, X6100_REMOTE_C_PRE,
			X6100_REMOTE_C_RFG, X6100_REMOTE_C_NR, X6100_REMOTE_C_NB, X6100_REMOTE_C_ATU };
		const struct radio_value *val[C_CTLS] = { &rs.agc, &rs.attenuator, &rs.preamp, &rs.rf_gain,
			&rs.noise_reduction, &rs.noise_blanker, &rs.tuner };
		static const char *const name[C_CTLS] = { "AGC", "ATT", "PRE", "RF gain", "NR", "NB", "ATU" };

		for (int i = 0; i < C_CTLS; i++) {
			if (!val[i]->valid || val[i]->value == r->rig_ctl[i])
				continue;

			r->rig_ctl[i] = val[i]->value;

			// Each report that changed, for the log (2026-11-12: RF gain, NR, NB from the 7851
			// did not show on the head; this says whether they come in at all)
			fprintf(stderr, "\nremote: rig %s %d%s\n", name[i], val[i]->value,
				!r->sent_valid ? " (before our first push)" : t - r->ctl_sent_ms[i] < C_ECHO_MS ? " (our echo)" : "");

			if (!r->sent_valid || t - r->ctl_sent_ms[i] < C_ECHO_MS)
				continue;

			int v = val[i]->value;
			x6100_remote_rx_t *s = &r->sent, *c = &st->ctl;

			switch (i) {
			case C_AGC:
				if (agc_from_icom(v) == s->agc)
					continue;
				c->agc = s->agc = agc_from_icom(v);
				break;
			case C_ATT:
				if ((v != 0) == s->att)
					continue;
				c->att = s->att = v != 0;
				break;
			case C_PRE:
				if ((v != 0) == s->pre)
					continue;
				c->pre = s->pre = v != 0;
				break;
			case C_RFG:
				if ((v * 100 + 127) / 255 == s->rfg)
					continue;
				c->rfg = s->rfg = (v * 100 + 127) / 255;
				break;
			case C_NR:
				if ((v != 0) == s->nr)
					continue;
				c->nr = s->nr = v != 0;
				break;
			case C_ATU:	/* 0 off, 1 on, 2 tuning (on too) */
				if ((v != 0) == s->atu)
					continue;
				c->atu = s->atu = v != 0;
				break;
			default:
				if ((v != 0) == s->nb)
					continue;
				c->nb = s->nb = v != 0;
				break;
			}

			st->ctl_changed |= bit[i];	/* taken over by the UI: no resend */
		}
	}
	//
	// Rig scope: centre mode, +- 50 kHz, set after connecting until a row of that width
	// comes (judged by the rows: the core learns the span only from a reply to its own
	// query), at most C_SCOPE_TRIES times 3 s apart once the rig file is loaded (before
	// it every set is refused, RIG_ERR_NO_CAPS), then left alone. Until 2026-11-14 it was sent
	// again once a second whenever the rows had another width, and each send took the
	// 7851 back to its scope screen: its menus could not be read. The rows are placed
	// by their own edges (remote_scope_feed), so any span the rig is set to shows right
	//
	{
		const uint64_t want = 2 * (uint64_t) C_SCOPE_HALF;
		const bool right = r->row_width != 0
			&& (r->row_width > want ? r->row_width - want : want - r->row_width) <= 500;

		if (right)
			r->scope_tries = C_SCOPE_TRIES;	/* done: the rig keeps what is set there from now */

		if (!right && rs.rig_valid && r->scope_tries < C_SCOPE_TRIES && t - r->scope_set_ms >= 3000) {
			session_set_value(r->s, "Scope Mode", 0, 0);
			session_set_span(r->s, 0, C_SCOPE_HALF);
			r->scope_set_ms = t;
			r->scope_tries++;
		}
	}

	//
	// S meter: the rig file calibration gives dB over S9
	//
	r->smeter_valid = rs.meter[RADIO_METER_S].valid;

	if (r->smeter_valid) {
		st->smeter_valid = true;
		st->smeter_dbm = rs.meter[RADIO_METER_S].cal - 73.0;
		r->smeter_dbm = st->smeter_dbm;
	}

	st->rig_tx = rs.transmit.valid && rs.transmit.value != 0;
}

bool x6100_remote_smeter(const x6100_remote_t *r, double *dbm)
{
	*dbm = r->smeter_dbm;
	return r->smeter_valid;
}

bool x6100_remote_scope(x6100_remote_t *r, x6100_remote_scope_t *out)
{
	static struct radio_scope_row row;	/* one caller (the main loop) */

	if (!session_get_scope_row(r->s, 0, &row) || (r->scope_seen && row.seq == r->scope_seq))
		return false;

	r->scope_seen = true;
	r->scope_seq = row.seq;

	if (row.oor || row.len == 0 || row.high_hz <= row.low_hz)
		return false;

	r->row_width = row.high_hz - row.low_hz;
	out->low_hz = row.low_hz;
	out->high_hz = row.high_hz;
	out->len = row.len;
	out->bins = row.bins;
	return true;
}

size_t x6100_remote_audio_tap(x6100_remote_t *r, int16_t *out, size_t max)
{
	size_t rd = atomic_load_explicit(&r->tap_r, memory_order_relaxed);
	size_t w = atomic_load_explicit(&r->tap_w, memory_order_acquire);
	size_t n = w - rd < max ? w - rd : max;

	for (size_t i = 0; i < n; i++)
		out[i] = r->tap[(rd + i) % C_TAP];

	atomic_store_explicit(&r->tap_r, rd + n, memory_order_release);
	return n;
}

int x6100_remote_ptt(x6100_remote_t *r, bool on)
{
	return tx_ptt(r->s, on ? 1 : 0);
}

void x6100_remote_ptt_alive(x6100_remote_t *r)
{
	tx_ptt_heartbeat(r->s);
}

void x6100_remote_tx_audio(x6100_remote_t *r, const int16_t *stereo, size_t frames)
{
	float buf[1024 * 2];

	while (frames > 0) {
		size_t n = frames < 1024 ? frames : 1024;

		for (size_t i = 0; i < n * 2; i++)
			buf[i] = (float) stereo[i] / 32768.0f;

		tx_capture_write(r->s, buf, n, 2);
		stereo += n * 2;
		frames -= n;
	}
}

void x6100_remote_tx_stats(x6100_remote_t *r, x6100_remote_tx_stats_t *out)
{
	struct session_info info;

	session_get_info(r->s, &info);
	out->packets = info.tx_packets;
	out->blocks = info.tx_blocks;
	out->overruns = info.tx_overrun_frames;
	out->peak = info.tx_peak;
	out->rms = info.tx_rms;
	out->available = info.tx_available;
	out->codec = info.tx_codec;
	out->rate = info.tx_sample;
}

void x6100_remote_tx_gain(x6100_remote_t *r, int percent)
{
	tx_set_mic_gain(r->s, percent);
}

//
// The rig's bands from its wfview rig file: those for IARU region `region` (1-3; the
// file has some per region, 40 m, 80 m, 2 m, 70 cm) and those for all, each name once,
// by frequency. 0 while the rig file is not loaded
//

static int band_cmp(const void *a, const void *b)
{
	const x6100_remote_band_t *x = a, *y = b;

	return x->low < y->low ? -1 : x->low > y->low ? 1 : 0;
}

size_t x6100_remote_bands(x6100_remote_t *r, int region, x6100_remote_band_t *out, size_t max)
{
	const struct rig_def *def = session_rig_def(r->s);
	size_t n = 0;

	if (def == NULL)
		return 0;

	for (size_t i = 0; i < def->n_bands && n < max; i++) {
		const struct rig_band *b = &def->bands[i];
		bool twice = false;

		if ((b->region[0] != '\0' && atoi(b->region) != region) || b->end > UINT32_MAX || b->end <= b->start)
			continue;

		for (size_t k = 0; k < n; k++)
			twice |= strcmp(out[k].name, b->name) == 0;

		if (twice)
			continue;

		out[n].low = (uint32_t) b->start;
		out[n].high = (uint32_t) b->end;
		snprintf(out[n].name, sizeof(out[n].name), "%s", b->name);
		n++;
	}

	qsort(out, n, sizeof(out[0]), band_cmp);
	return n;
}

//
// The rig's tuner tunes (2026-11-14: the ATU key long on the head): Tuner/ATU Status 2,
// as wfview's tune; the session refuses it for a receive-only rig (PTT disabled). TUNE
// shows while the rig reports 2 (rig_tuning)
//

int x6100_remote_tune(x6100_remote_t *r)
{
	int rc = tx_tune(r->s);

	if (rc == 0) {
		r->tune_ms = now_ms();
		r->tune_ask_ms = 0;
	}

	return rc;
}

//
// The rig on or off (2026-11-15): CI-V 18 01 / 18 00 to the address it gave at login,
// as a frame of our own, as wfview's powerOn / powerOff over the network (no FE
// preamble there): in standby the rig answers no CI-V ID, so no rig file is loaded and
// the core would refuse the command. Works only with the rig's LAN kept alive in
// standby (its network setting for power on over LAN)
//

int x6100_remote_power(x6100_remote_t *r, bool on)
{
	struct session_info info;
	uint8_t f[7];

	session_get_info(r->s, &info);

	if (info.civ_addr == 0x00)
		return -1;

	f[0] = 0xFE;
	f[1] = 0xFE;
	f[2] = info.civ_addr;
	f[3] = RIG_CONTROLLER_ADDR;
	f[4] = 0x18;
	f[5] = on ? 0x01 : 0x00;
	f[6] = 0xFD;
	return session_civ_send(r->s, f, sizeof(f));
}
