/*
 * acq_test.c - equivalence test for rade_acq_neon.patch: rade_c's original
 * rade_acq_detect_pilots() (built from the unpatched rade_acq.c with its functions renamed,
 * -Drade_acq_detect_pilots=o_detect and so on) against the patched one, on noise with a
 * pilot at a known time and frequency. See contrib/rade/README.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_acq.h"

int o_detect(rade_acq *acq, const RADE_COMP *rx, int *tmax, float *fmax);

/* rade_acq_init (not called here) needs it */
RADE_COMP rade_cdot(const RADE_COMP *a, const RADE_COMP *b, int n)
{
	RADE_COMP z = { 0.0f, 0.0f };

	(void) a;
	(void) b;
	(void) n;

	return z;
}

static rade_acq g_a, g_b;
static RADE_COMP g_rx[2 * RADE_NMF + RADE_M + RADE_NCP];

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
	g_a.fs = RADE_FS;
	g_a.m = RADE_M;
	g_a.ncp = RADE_NCP;
	g_a.nmf = RADE_NMF;
	g_a.n_fcoarse = RADE_ACQ_NFREQ;
	g_a.Pacq_error1 = RADE_ACQ_PACQ_ERR1;

	for (int f = 0; f < RADE_ACQ_NFREQ; f++) {
		g_a.fcoarse_range[f] = -50.0f + 2.5f * f;

		for (int n = 0; n < RADE_M; n++) {
			float w = 2.0f * (float) M_PI * g_a.fcoarse_range[f] / RADE_FS * n;
			float ph = (float) M_PI * frand();	/* pilot: random phases */

			if (f == 0) {
				g_a.p[n].real = cosf(ph);
				g_a.p[n].imag = sinf(ph);
			}

			g_a.p_w[f][n].real = g_a.p[n].real * cosf(w) - g_a.p[n].imag * sinf(w);
			g_a.p_w[f][n].imag = g_a.p[n].real * sinf(w) + g_a.p[n].imag * cosf(w);
		}
	}

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

	return fails != 0;
}
