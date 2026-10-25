/*
 * x6100_liquid - link and run check for the static liquid-dsp build
 *
 * Prints the liquid-dsp version and runs a 63-tap Kaiser low-pass (cutoff 0.1 fs)
 * over an impulse and a stopband tone, so a build that links but computes nonsense
 * shows up too. Exit 0 when the gain and the stopband attenuation are as expected.
 *
 *   x6100_liquid
 */

#include <math.h>
#include <stdio.h>

#include <liquid/liquid.h>

#define TAPS	63

int main(void)
{
	firfilt_rrrf f = firfilt_rrrf_create_kaiser(TAPS, 0.1f, 60.0f, 0.0f);
	float dc = 0.0f, stop = 0.0f, y;
	float att;

	printf("liquid-dsp %s\n", liquid_libversion());

	// DC gain: the impulse response sums to the gain at 0 Hz
	for (int i = 0; i < TAPS; i++) {
		firfilt_rrrf_push(f, i == 0 ? 1.0f : 0.0f);
		firfilt_rrrf_execute(f, &y);
		dc += y;
	}

	// A tone at 0.3 fs, well in the stopband: peak output after the filter has filled
	firfilt_rrrf_reset(f);

	for (int i = 0; i < 2000; i++) {
		firfilt_rrrf_push(f, cosf(2.0f * (float) M_PI * 0.3f * (float) i));
		firfilt_rrrf_execute(f, &y);

		if (i > TAPS && fabsf(y) > stop)
			stop = fabsf(y);
	}

	firfilt_rrrf_destroy(f);

	att = 20.0f * log10f(stop / (dc > 0.0f ? dc : 1.0f));
	printf("low-pass %d taps: dc gain %.3f, stopband %.1f dB\n", TAPS, dc, att);

	return dc > 0.5f && att < -40.0f ? 0 : 1;
}
