/*
 * x6100_pcm.h - minimal ALSA PCM access via kernel ioctls (no libasound)
 *
 * X6100 audio, from the stock image: card 0 "sun8i-a33-audio" (A33 internal codec).
 *   capture   Mic1/ADC: demodulated RX audio from the baseband (default routing)
 *   playback  DAC/headphone: TX modulation audio to the baseband
 * Stock mixer setup (S51amixer): Mic1 capture on, ADC gain 3, DAC on, headphone 63.
 * The R1CBU GUI leaves "DAC Playback" off/0: x6100_mixer_tx_setup() restores the TX path.
 *
 * Interleaved S16_LE only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define X6100_PCM_CARD		0
#define X6100_PCM_DEVICE	0

typedef struct
{
	int fd;
	bool capture;
	unsigned rate;
	unsigned channels;
	unsigned period;
	unsigned periods;
	unsigned xruns;
} x6100_pcm_t;

//
// Open, transfer, close
//

int x6100_pcm_open(x6100_pcm_t *pcm, int card, int device, bool capture, unsigned rate, unsigned channels,
	unsigned period, unsigned periods);
ssize_t x6100_pcm_read(x6100_pcm_t *pcm, int16_t *buf, unsigned frames);
ssize_t x6100_pcm_write(x6100_pcm_t *pcm, const int16_t *buf, unsigned frames);
void x6100_pcm_close(x6100_pcm_t *pcm);

//
// Mixer (control device); returns 0 or -errno
//

int x6100_mixer_set(int card, const char *name, long value);
int x6100_mixer_tx_setup(int card);
