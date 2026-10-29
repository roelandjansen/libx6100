/*
 * rade_rx_mt.c - RADE WAV demodulator in two threads: OFDM demodulator and RADE decoder in
 * one, the FARGAN vocoder in the other, with a queue of feature frames between them. On
 * the R16 each half takes about 0.55 of real time on one core; together, in one thread,
 * they miss it (contrib/rade/README.md).
 *
 * Usage: rade_rx_mt [--v2] <input.wav> <output.wav>
 *        (input 8 kHz 16-bit mono modem audio, output 16 kHz 16-bit mono speech)
 *
 * Derived from rade_c's src/rade_rx_wav.c, which carries this notice:
 *
 *   Copyright (C) 2024 David Rowe
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *   - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_api.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"

#define QUEUE_FRAMES	64	/* feature frames between the threads (0.64 s) */

//
// Feature frames from the decoder thread to the vocoder thread
//

typedef struct
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	float frame[QUEUE_FRAMES][RADE_NB_TOTAL_FEATURES];
	int head;		/* next to read */
	int count;		/* frames queued */
	int done;		/* the decoder has finished */
} queue_t;

typedef struct
{
	queue_t *q;
	FILE *out;
	uint32_t bytes;		/* PCM bytes written */
} vocoder_t;

//
// Queue a frame; waits while the queue is full
//

static void queue_put(queue_t *q, const float *feat)
{
	pthread_mutex_lock(&q->lock);

	while (q->count == QUEUE_FRAMES)
		pthread_cond_wait(&q->cond, &q->lock);

	memcpy(q->frame[(q->head + q->count) % QUEUE_FRAMES], feat, sizeof(q->frame[0]));
	q->count++;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);
}

//
// Take a frame; waits while the queue is empty. 0 when the decoder is done and all is read
//

static int queue_get(queue_t *q, float *feat)
{
	pthread_mutex_lock(&q->lock);

	while (q->count == 0 && !q->done)
		pthread_cond_wait(&q->cond, &q->lock);

	if (q->count == 0) {
		pthread_mutex_unlock(&q->lock);
		return 0;
	}

	memcpy(feat, q->frame[q->head], sizeof(q->frame[0]));
	q->head = (q->head + 1) % QUEUE_FRAMES;
	q->count--;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);

	return 1;
}

static void queue_close(queue_t *q)
{
	pthread_mutex_lock(&q->lock);
	q->done = 1;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);
}

//
// 16-bit mono WAV: header (data size patched at the end) and reading
//

static void wav_header(FILE *f, int rate, uint32_t bytes)
{
	uint32_t riff = 36 + bytes, fmt_size = 16, sr = (uint32_t) rate, byte_rate = (uint32_t) rate * 2;
	uint16_t fmt = 1, nch = 1, align = 2, bps = 16;

	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_size, 4, 1, f);
	fwrite(&fmt, 2, 1, f);
	fwrite(&nch, 2, 1, f);
	fwrite(&sr, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bps, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
}

// Samples of a 16-bit mono WAV at `rate` as IQ (imaginary 0), as rade_rx_wav does
static RADE_COMP *wav_read_iq(const char *path, int rate, long *n)
{
	FILE *f = fopen(path, "rb");
	char id[4];
	uint32_t size = 0, sr = 0;
	uint16_t nch = 0, bps = 0;
	RADE_COMP *iq = NULL;

	if (f == NULL || fseek(f, 12, SEEK_SET) != 0)
		goto out;

	// Chunks up to "data": the format from "fmt ", the rest skipped
	while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
		if (memcmp(id, "fmt ", 4) == 0) {
			uint8_t b[16];

			if (size < 16 || fread(b, 1, 16, f) != 16)
				goto out;

			memcpy(&nch, b + 2, 2);
			memcpy(&sr, b + 4, 4);
			memcpy(&bps, b + 14, 2);
			fseek(f, (long) (size - 16), SEEK_CUR);
		} else if (memcmp(id, "data", 4) == 0) {
			break;
		} else {
			fseek(f, (long) ((size + 1) & ~1u), SEEK_CUR);
		}
	}

	if (sr != (uint32_t) rate || nch != 1 || bps != 16) {
		fprintf(stderr, "rade_rx_mt: %s: need %d Hz 16-bit mono\n", path, rate);
		goto out;
	}

	*n = (long) (size / 2);

	if ((iq = malloc((size_t) *n * sizeof(*iq))) == NULL)
		goto out;

	for (long i = 0; i < *n; i++) {
		int16_t s = 0;

		if (fread(&s, 2, 1, f) != 1)
			s = 0;	/* short file: pad */

		iq[i].real = s * (2.0f / RADE_INT16_SCALE);
		iq[i].imag = 0.0f;
	}

out:
	if (f != NULL)
		fclose(f);

	return iq;
}

//
// Vocoder thread: FARGAN primed with the first 5 frames, then one 10 ms frame each
//

static void *vocoder_run(void *arg)
{
	vocoder_t *v = arg;
	FARGANState fargan;
	float feat[RADE_NB_TOTAL_FEATURES], prime[5 * NB_FEATURES], fpcm[LPCNET_FRAME_SIZE];
	int16_t pcm[LPCNET_FRAME_SIZE];
	int primed = 0;

	fargan_init(&fargan);

	while (queue_get(v->q, feat)) {
		if (primed < 5) {
			memcpy(&prime[primed * NB_FEATURES], feat, NB_FEATURES * sizeof(float));

			if (++primed == 5) {
				float zeros[FARGAN_CONT_SAMPLES] = { 0 };

				fargan_cont(&fargan, zeros, prime);
			}

			continue;
		}

		fargan_synthesize(&fargan, fpcm, feat);

		for (int s = 0; s < LPCNET_FRAME_SIZE; s++) {
			float x = fpcm[s] * 32768.0f;

			x = x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x;
			pcm[s] = (int16_t) floor(0.5 + (double) x);
		}

		fwrite(pcm, sizeof(pcm[0]), LPCNET_FRAME_SIZE, v->out);
		v->bytes += sizeof(pcm);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int v2 = argc > 1 && strcmp(argv[1], "--v2") == 0;
	long n = 0, pos = 0;
	RADE_COMP *iq, *buf;
	float *feat;
	struct rade *r;
	queue_t q = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
	vocoder_t voc = { .q = &q };
	pthread_t tid;
	int eoo;

	if (argc - v2 != 3) {
		fprintf(stderr, "usage: rade_rx_mt [--v2] <input.wav 8 kHz> <output.wav 16 kHz>\n");
		return 1;
	}

	if ((iq = wav_read_iq(argv[1 + v2], RADE_FS, &n)) == NULL)
		return 1;

	if ((voc.out = fopen(argv[2 + v2], "wb")) == NULL) {
		fprintf(stderr, "rade_rx_mt: cannot write %s\n", argv[2 + v2]);
		return 1;
	}

	wav_header(voc.out, RADE_FS_SPEECH, 0);

	rade_initialize();

	if ((r = rade_open("(built-in weights)", RADE_VERBOSE_0 | (v2 ? RADE_MODE_V2 : 0))) == NULL) {
		fprintf(stderr, "rade_rx_mt: rade_open failed\n");
		return 1;
	}

	buf = calloc((size_t) rade_nin_max(r), sizeof(*buf));
	feat = calloc((size_t) rade_n_features_in_out(r), sizeof(*feat));
	float *eoo_bits = calloc((size_t) rade_n_eoo_bits(r) + 1, sizeof(float));

	if (buf == NULL || feat == NULL || eoo_bits == NULL || pthread_create(&tid, NULL, vocoder_run, &voc) != 0) {
		fprintf(stderr, "rade_rx_mt: out of memory or no thread\n");
		return 1;
	}

	// Decoder thread (this one): demodulate, decode, queue the feature frames
	while (pos < n) {
		int nin = rade_nin(r), got;

		memset(buf, 0, (size_t) nin * sizeof(*buf));
		memcpy(buf, &iq[pos], (size_t) (n - pos < nin ? n - pos : nin) * sizeof(*buf));
		pos += nin;

		got = rade_rx(r, feat, &eoo, eoo_bits, buf);

		for (int i = 0; i + RADE_NB_TOTAL_FEATURES <= got; i += RADE_NB_TOTAL_FEATURES)
			queue_put(&q, &feat[i]);
	}

	queue_close(&q);
	pthread_join(tid, NULL);

	fseek(voc.out, 0, SEEK_SET);
	wav_header(voc.out, RADE_FS_SPEECH, voc.bytes);
	fclose(voc.out);

	free(iq);
	free(buf);
	free(feat);
	free(eoo_bits);
	rade_close(r);
	rade_finalize();

	return 0;
}
