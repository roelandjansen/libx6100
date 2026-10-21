/*
 * x6100_pcm.c - minimal ALSA PCM access via kernel ioctls (no libasound)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100_pcm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

//
// hw_params helpers: masks and intervals, as the kernel expects them
//

static struct snd_mask *param_mask(struct snd_pcm_hw_params *p, int n)
{
	return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *param_interval(struct snd_pcm_hw_params *p, int n)
{
	return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void param_init(struct snd_pcm_hw_params *p)
{
	memset(p, 0, sizeof(*p));

	for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
		struct snd_mask *m = param_mask(p, n);

		m->bits[0] = ~0u;
		m->bits[1] = ~0u;
	}

	for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		struct snd_interval *i = param_interval(p, n);

		i->min = 0;
		i->max = ~0u;
	}

	p->rmask = ~0u;
	p->cmask = 0;
	p->info = ~0u;
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
	struct snd_mask *m = param_mask(p, n);

	memset(m->bits, 0, sizeof(m->bits));
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned val)
{
	struct snd_interval *i = param_interval(p, n);

	i->min = val;
	i->max = val;
	i->integer = 1;
}

static void param_set_min(struct snd_pcm_hw_params *p, int n, unsigned val)
{
	param_interval(p, n)->min = val;
}

static int xrun_recover(x6100_pcm_t *pcm)
{
	pcm->xruns++;

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		return -errno;

	if (pcm->capture && ioctl(pcm->fd, SNDRV_PCM_IOCTL_START) < 0)
		return -errno;

	return 0;
}

int x6100_pcm_open(x6100_pcm_t *pcm, int card, int device, bool capture, unsigned rate, unsigned channels,
	unsigned period, unsigned periods)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	char path[64];
	int err;

	memset(pcm, 0, sizeof(*pcm));
	pcm->capture = capture;

	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", card, device, capture ? 'c' : 'p');
	pcm->fd = open(path, O_RDWR | O_CLOEXEC);

	if (pcm->fd < 0)
		return -errno;

	param_init(&hw);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	param_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	param_set_min(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * channels);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIODS, periods);
	param_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0)
		goto fail;

	pcm->rate = param_interval(&hw, SNDRV_PCM_HW_PARAM_RATE)->min;
	pcm->channels = param_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS)->min;
	pcm->period = param_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
	pcm->periods = param_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS)->min;

	unsigned long buffer = (unsigned long) pcm->period * pcm->periods;

	memset(&sw, 0, sizeof(sw));
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = pcm->period;
	sw.start_threshold = capture ? 1 : buffer;
	sw.stop_threshold = buffer;
	sw.boundary = buffer;

	while (sw.boundary * 2 <= (unsigned long) LONG_MAX - buffer)
		sw.boundary *= 2;

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0)
		goto fail;

	if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		goto fail;

	if (capture && ioctl(pcm->fd, SNDRV_PCM_IOCTL_START) < 0)
		goto fail;

	return 0;

fail:
	err = -errno;
	close(pcm->fd);
	pcm->fd = -1;
	return err;
}

ssize_t x6100_pcm_read(x6100_pcm_t *pcm, int16_t *buf, unsigned frames)
{
	struct snd_xferi x = { .buf = buf, .frames = frames };

	for (;;) {
		if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0)
			return (ssize_t) x.result;

		if (errno == EINTR)
			continue;

		if (errno != EPIPE)
			return -errno;

		int rc = xrun_recover(pcm);

		if (rc < 0)
			return rc;
	}
}

ssize_t x6100_pcm_write(x6100_pcm_t *pcm, const int16_t *buf, unsigned frames)
{
	struct snd_xferi x = { .buf = (void *) buf, .frames = frames };

	for (;;) {
		if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0)
			return (ssize_t) x.result;

		if (errno == EINTR)
			continue;

		if (errno != EPIPE)
			return -errno;

		int rc = xrun_recover(pcm);

		if (rc < 0)
			return rc;
	}
}

void x6100_pcm_close(x6100_pcm_t *pcm)
{
	if (pcm->fd >= 0) {
		ioctl(pcm->fd, SNDRV_PCM_IOCTL_DROP);
		close(pcm->fd);
	}

	pcm->fd = -1;
}

//
// Mixer control by name, same value on every channel (BOOLEAN, INTEGER or ENUMERATED item index)
//

int x6100_mixer_set(int card, const char *name, long value)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value val;
	char path[32];
	int fd, rc = 0;

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);

	if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0)
		return -errno;

	memset(&info, 0, sizeof(info));
	info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf((char *) info.id.name, sizeof(info.id.name), "%s", name);

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
		rc = -errno;
		goto out;
	}

	if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.type != SNDRV_CTL_ELEM_TYPE_INTEGER
		&& info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		rc = -EINVAL;
		goto out;
	}

	memset(&val, 0, sizeof(val));
	val.id = info.id;

	for (unsigned i = 0; i < info.count && i < 64; i++) {
		if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
			val.value.enumerated.item[i] = (unsigned) value;
		else
			val.value.integer.value[i] = value;
	}

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0)
		rc = -errno;
out:
	close(fd);
	return rc;
}

//
// Stock TX path: DAC unmuted at 0 dB, headphone from DAC at full scale
//

int x6100_mixer_tx_setup(int card)
{
	static const struct { const char *name; long value; } C_SET[] = {
		{ "AIF1 Slot 0 Digital DAC Playback Switch", 1 },
		{ "AIF1 DA0 Playback Volume", 160 },
		{ "DAC Playback Switch", 1 },
		{ "DAC Playback Volume", 160 },
		{ "Headphone Source Playback Route", 0 },
		{ "Headphone Playback Switch", 1 },
		{ "Headphone Playback Volume", 63 },
	};
	int rc = 0;

	for (size_t i = 0; i < sizeof(C_SET) / sizeof(C_SET[0]); i++) {
		int r = x6100_mixer_set(card, C_SET[i].name, C_SET[i].value);

		if (r < 0) {
			fprintf(stderr, "mixer: %s: %s\n", C_SET[i].name, strerror(-r));
			rc = r;
		}
	}

	return rc;
}

//
// Stock RX capture path (Mic1 = baseband audio). boost: Mic1 Boost Volume 0-7
// (0 = 0 dB, 1 = +24 dB, then +3 dB per step); use 0 for AM / FM, whose
// demodulated level saturates the codec input with the stock +24 dB
//

int x6100_mixer_rx_setup(int card, int boost)
{
	static const struct { const char *name; long value; } C_SET[] = {
		{ "Mic1 Capture Switch", 1 },
		{ "Mixer Capture Switch", 0 },
		{ "ADC Gain Capture Volume", 3 },
		{ "ADC Capture Volume", 160 },
		{ "AIF1 AD0 Capture Volume", 160 },
		{ "AIF1 AD0 Stereo Capture Route", 3 },
		{ "AIF1 Data Digital ADC Capture Switch", 1 },
	};
	int rc = 0, r;

	for (size_t i = 0; i < sizeof(C_SET) / sizeof(C_SET[0]); i++) {
		if ((r = x6100_mixer_set(card, C_SET[i].name, C_SET[i].value)) < 0) {
			fprintf(stderr, "mixer: %s: %s\n", C_SET[i].name, strerror(-r));
			rc = r;
		}
	}

	boost = boost < 0 ? 0 : boost > 7 ? 7 : boost;

	if ((r = x6100_mixer_set(card, "Mic1 Boost Volume", boost)) < 0) {
		fprintf(stderr, "mixer: Mic1 Boost Volume: %s\n", strerror(-r));
		rc = r;
	}

	return rc;
}
