/*
 * acq_test.c - equivalence and timing test for rade_acq_fft.patch: rade_c's original
 * rade_acq_detect_pilots() (built from the unpatched rade_acq.c with its functions renamed,
 * -Drade_acq_detect_pilots=o_detect and so on) against the patched one, on noise with a
 * pilot at a known time and frequency. See contrib/rade/README.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rade_acq.h"

int o_detect(rade_acq *acq, const RADE_COMP *rx, int *tmax, float *fmax);


#define CALLS	20

/* rade_dsp.c's complex dot product, for rade_acq_init (the test links rade_acq only) */
RADE_COMP rade_cdot(const RADE_COMP *a, const RADE_COMP *b, int n)
{
	RADE_COMP z = { 0.0f, 0.0f };

	for (int i = 0; i < n; i++) {
		z.real += a[i].real * b[i].real - a[i].imag * b[i].imag;
		z.imag += a[i].real * b[i].imag + a[i].imag * b[i].real;
	}

	return z;
}

static rade_ofdm g_ofdm;
static rade_acq g_a, g_b;
static RADE_COMP g_rx[2 * RADE_NMF + RADE_M + RADE_NCP];

/* Milliseconds per call of a detect function, over CALLS calls on g_rx */
static double ms_per_call(int (*detect)(rade_acq *, const RADE_COMP *, int *, float *), rade_acq *acq)
{
	struct timespec t0, t1;
	int t;
	float f;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (int i = 0; i < CALLS; i++)
		detect(acq, g_rx, &t, &f);

	clock_gettime(CLOCK_MONOTONIC, &t1);

	return ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / CALLS;
}

/* Parts of the function on their own, on g_b's grid; the sums keep them from being optimised away */
static volatile float g_sink;

static void part_clear(void)
{
	memset(g_b.Dt1, 0, sizeof(g_b.Dt1));
	memset(g_b.Dt2, 0, sizeof(g_b.Dt2));
	g_sink += g_b.Dt1[RADE_NMF / 2][3].real;
}

static void part_scan(void)
{
	float sum = 0.0f;

	for (int t = 0; t < RADE_NMF; t++)
		for (int f = 0; f < RADE_ACQ_NFREQ; f++)
			sum += rade_cabs(g_b.Dt1[t][f]) + rade_cabs(g_b.Dt2[t][f]);

	g_sink += sum;
}

static void part_sqrt(void)
{
	float sum = 0.0f, x = g_sink + 2.0f;

	for (int i = 0; i < 2 * RADE_NMF * RADE_ACQ_NFREQ; i++) {
		sum += sqrtf(x);
		x += 0.001f;
	}

	g_sink += sum;
}

static double ms_part(void (*part)(void))
{
	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (int i = 0; i < CALLS; i++)
		part();

	clock_gettime(CLOCK_MONOTONIC, &t1);

	return ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / CALLS;
}

static float frand(void)
{
	return (float) rand() / (float) RAND_MAX * 2.0f - 1.0f;
}

int main(void)
{
	int ta, tb, ra, rb, fails = 0;
	float fa, fb, err = 0.0f, mag = 0.0f;
	const int t0 = 333, f0 = 17;

	srand(1);

	/* A pilot of random phases, set up as the receiver does it (the patched function
	   needs rade_acq_init's FFT set-up; the original reads only p_w and the fields) */
	for (int n = 0; n < RADE_M; n++) {
		float ph = (float) M_PI * frand();

		g_ofdm.p[n].real = cosf(ph);
		g_ofdm.p[n].imag = sinf(ph);
		g_ofdm.pend[n] = g_ofdm.p[n];
	}

	rade_acq_init(&g_a, &g_ofdm, RADE_ACQ_FRANGE, RADE_ACQ_FSTEP);
	g_b = g_a;

	for (size_t i = 0; i < sizeof(g_rx) / sizeof(g_rx[0]); i++) {
		g_rx[i].real = 0.5f * frand();
		g_rx[i].imag = 0.5f * frand();
	}

	/* The pilot at t0 and t0 + Nmf, frequency f0: conj(rx) . p_w peaks where rx = p_w */
	for (int n = 0; n < RADE_M; n++) {
		g_rx[t0 + n].real += g_a.p_w[f0][n].real;
		g_rx[t0 + n].imag += g_a.p_w[f0][n].imag;
		g_rx[t0 + RADE_NMF + n].real += g_a.p_w[f0][n].real;
		g_rx[t0 + RADE_NMF + n].imag += g_a.p_w[f0][n].imag;
	}

	ra = o_detect(&g_a, g_rx, &ta, &fa);
	rb = rade_acq_detect_pilots(&g_b, g_rx, &tb, &fb);

	for (int t = 0; t < RADE_NMF; t++) {
		for (int f = 0; f < RADE_ACQ_NFREQ; f++) {
			RADE_COMP x[2] = { g_a.Dt1[t][f], g_a.Dt2[t][f] }, y[2] = { g_b.Dt1[t][f], g_b.Dt2[t][f] };

			for (int k = 0; k < 2; k++) {
				float e = fabsf(x[k].real - y[k].real) + fabsf(x[k].imag - y[k].imag);

				err = e > err ? e : err;
				mag = fabsf(x[k].real) > mag ? fabsf(x[k].real) : mag;
			}
		}
	}

	printf("orig: detect %d t %d f %.1f thresh %.4f max %.4f\n", ra, ta, fa, g_a.Dthresh, g_a.Dtmax12);
	printf("new:  detect %d t %d f %.1f thresh %.4f max %.4f\n", rb, tb, fb, g_b.Dthresh, g_b.Dtmax12);
	printf("grid: max abs difference %.3g (largest value %.3g)\n", err, mag);

	fails += ra != rb || ta != tb || fa != fb || ta != t0 || fabsf(fa - g_a.fcoarse_range[f0]) > 0.01f;
	fails += err > 1e-4f * mag || fabsf(g_a.Dthresh - g_b.Dthresh) > 1e-4f * g_a.Dthresh;
	printf("%s\n", fails ? "FAIL" : "PASS");

	/* Time per call (one call per 120 ms modem frame while out of sync) */
	printf("orig: %6.1f ms per call\n", ms_per_call(o_detect, &g_a));
	printf("new:  %6.1f ms per call   (budget 120 ms per frame)\n", ms_per_call(rade_acq_detect_pilots, &g_b));

	/* Where the time goes: the parts of the function other than the correlations */
	printf("parts, ms per call:\n");
	printf("  clear both grids (memset 2 x %zu kB)   %7.2f\n", sizeof(g_b.Dt1) / 1024, ms_part(part_clear));
	printf("  one |Dt1|+|Dt2| scan (%d rade_cabs)   %7.2f   (the function does 2)\n",
		2 * RADE_NMF * RADE_ACQ_NFREQ, ms_part(part_scan));
	printf("  %d bare sqrtf                        %7.2f\n", 2 * RADE_NMF * RADE_ACQ_NFREQ, ms_part(part_sqrt));

	return fails != 0;
}
