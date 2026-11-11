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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x6100_pcm.h"

#include "core/audio_decode.h"
#include "core/session.h"
#include "model/radio_state.h"

#define C_RATE		48000
#define C_PERIOD	480		/* 10 ms */
#define C_PERIODS	4
#define C_ECHO_MS	400		/* rig reports this soon after our own change: our echo */
#define C_ATT_DB	12		/* ATT key on: the Icom attenuator step used (0 = off) */

//
// Icom CI-V mode numbers (wfview .rig Modes\N\Num)
//

#define C_ICOM_LSB	0
#define C_ICOM_USB	1
#define C_ICOM_AM	2
#define C_ICOM_CW	3
#define C_ICOM_FM	5
#define C_ICOM_CWR	7

struct x6100_remote
{
	//
	// Link
	//
	struct session *s;

	//
	// What we last sent (valid after the first apply)
	//
	bool sent_valid;
	x6100_remote_rx_t sent;
	int64_t freq_sent_ms;
	int64_t mode_sent_ms;

	//
	// What the rig last reported
	//
	uint64_t rig_freq;
	int rig_mode_num;
	int rig_mode_data;
	int rig_filter;
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

		if (x6100_pcm_write(&r->pcm, out, C_PERIOD) < 0)
			fprintf(stderr, "\nremote: playback write failed\n");	/* the pcm layer recovers xruns */
	}

	return NULL;
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
	atomic_init(&r->stop, false);

	memset(&p, 0, sizeof(p));
	snprintf(p.host, sizeof(p.host), "%s", cfg->host);
	p.port_control = cfg->port;
	snprintf(p.username, sizeof(p.username), "%s", cfg->user);
	snprintf(p.password, sizeof(p.password), "%s", cfg->pass);
	snprintf(p.client_name, sizeof(p.client_name), "%s", "x6100trx");
	snprintf(p.rig_dir, sizeof(p.rig_dir), "%s", cfg->rig_dir);

	//
	// RX: LPCM 16 bit stereo 48 kHz (no decode cost on the R16), 150 ms as wfview.
	// TX: none (no capture rate, PTT disabled)
	//
	p.rx_codec = AUDIO_CODEC_PCM16_2;
	p.rx_sample = C_RATE;
	p.rx_latency = 150;
	p.dev_rate = C_RATE;
	p.cap_rate = 0;
	p.enable_ptt = false;

	memset(&h, 0, sizeof(h));

	if ((rc = x6100_mixer_tx_setup(X6100_PCM_CARD)) < 0
		|| (rc = x6100_pcm_open(&r->pcm, X6100_PCM_CARD, X6100_PCM_DEVICE, false, C_RATE, 2, C_PERIOD,
			C_PERIODS)) < 0) {
		fprintf(stderr, "remote: playback: %s\n", strerror(-rc));
		free(r);
		return rc;
	}

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

	r->audio_running = true;
	*out = r;
	return 0;
}

void x6100_remote_close(x6100_remote_t *r)
{
	if (r == NULL)
		return;

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
	}

	if ((all || rx->agc != o->agc) && agc_to_icom(rx->agc) > 0)
		session_set_value(r->s, "AGC Status", 0, agc_to_icom(rx->agc));

	if (all || rx->att != o->att)
		session_set_value(r->s, "Attenuator Status", 0, rx->att ? C_ATT_DB : 0);

	if (all || rx->pre != o->pre)
		session_set_value(r->s, "Preamp Status", 0, rx->pre ? 1 : 0);

	if (all || rx->rfg != o->rfg)
		session_set_value(r->s, "RF Gain", 0, (rx->rfg * 255 + 50) / 100);

	if (all || rx->nr != o->nr)
		session_set_value(r->s, "Noise Reduction", 0, rx->nr);

	if (all || rx->nb != o->nb)
		session_set_value(r->s, "Noise Blanker", 0, rx->nb);

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

	//
	// Link down: everything is pushed again once it is back (a rig restart loses
	// nothing of ours), no meter meanwhile
	//
	if (!st->connected) {
		r->sent_valid = false;
		r->smeter_valid = false;
		return;
	}

	session_get_state(r->s, &rs);

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
