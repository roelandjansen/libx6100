/*
 * x6100_remote.h - x6100_trx as a control head for a remote Icom (receive only)
 *
 * x6100_trx --remote <host> <port> <user> <password>
 *
 * The UI, the knobs and the keys stay as they are; the rig behind them is an Icom
 * on the RS-BA1 / wfview UDP protocol (libx6100_app core, third_party/libx6100_app),
 * reached through wfserver or the rig's own LAN port.
 *
 * The baseband still runs, as audio output only: the speaker and the headphone
 * hang off it and play the codec only in play mode (register 12 bit 0x8, STATUS.md
 * item 18). In remote mode x6100_trx writes a fixed head state: play mode, RX volume,
 * TX enable off on both VFOs, TX power 0, tuner straight through. PTT, codec TX and
 * the tuner are never set. The mics go to the baseband only: no TX.
 *
 * The UI pushes its whole state every pass (as apply() does); only changes go out
 * as CI-V. What the rig reports (its own VFO knob, another client) comes back
 * through x6100_remote_poll() and is taken over by the UI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x6100_regs.h"

typedef struct
{
	//
	// Server
	//
	const char *host;
	uint16_t port;
	const char *user;
	const char *pass;
	const char *rig_dir;		/* wfview .rig files */
} x6100_remote_cfg_t;

typedef struct
{
	//
	// Receiver, in x6100_trx terms
	//
	uint32_t freq_hz;
	x6100_mode_t mode;
	int filter;			/* FIL1-3, 0-based */
	x6100_agc_t agc;
	bool att;
	bool pre;
	int rfg;			/* 0-100 */

	//
	// DSP switches
	//
	bool nr;
	bool nb;
} x6100_remote_rx_t;

// The controls the rig reports back (bits of x6100_remote_status_t.ctl_changed)
enum { X6100_REMOTE_C_AGC = 1, X6100_REMOTE_C_ATT = 2, X6100_REMOTE_C_PRE = 4, X6100_REMOTE_C_RFG = 8,
	X6100_REMOTE_C_NR = 16, X6100_REMOTE_C_NB = 32 };

typedef struct
{
	//
	// Link
	//
	bool connected;
	char state[24];			/* session state name, for the status line */
	char radio[33];

	//
	// Changed at the rig (not by us): take over in the UI
	//
	bool freq_changed;
	uint32_t freq_hz;
	bool mode_changed;
	x6100_mode_t mode;
	int filter;

	// The controls, in the same way: X6100_REMOTE_C_* bits for those changed at the rig,
	// the new values in ctl (only those fields)
	unsigned ctl_changed;
	x6100_remote_rx_t ctl;

	//
	// Meter
	//
	bool smeter_valid;
	double smeter_dbm;
	bool rig_tx;			/* the rig is keyed (another client) */
	bool rig_ready;			/* the rig file is loaded (the rig answered its CI-V ID): controls work */
	int cw_pitch;			/* the rig's CW pitch in Hz, 0 not known yet (the decoder listens there) */
} x6100_remote_status_t;

typedef struct x6100_remote x6100_remote_t;

//
// Lifecycle
//

int x6100_remote_open(x6100_remote_t **out, const x6100_remote_cfg_t *cfg);
void x6100_remote_close(x6100_remote_t *r);

//
// UI -> rig: whole state, only changes are sent
//

void x6100_remote_apply(x6100_remote_t *r, const x6100_remote_rx_t *rx);

//
// Rig -> UI
//

void x6100_remote_poll(x6100_remote_t *r, x6100_remote_status_t *st);
bool x6100_remote_smeter(const x6100_remote_t *r, double *dbm);	/* as last polled, no side effects */

//
// Rig scope: one new row since the last call, else false. bins: len values 0-160
// (Icom amplitude), low_hz to high_hz; valid until the next call
//

typedef struct
{
	uint64_t low_hz;
	uint64_t high_hz;
	size_t len;
	const uint8_t *bins;
} x6100_remote_scope_t;

bool x6100_remote_scope(x6100_remote_t *r, x6100_remote_scope_t *out);

// The received audio (left channel, 48 kHz S16) since the last call, at most max
// samples; for the CW decoder (main loop)
size_t x6100_remote_audio_tap(x6100_remote_t *r, int16_t *out, size_t max);
