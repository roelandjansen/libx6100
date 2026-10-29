/*
 * x6100_mixer - list and set the R16 codec's mixer controls (card 0), without alsa-utils
 *
 *   x6100_mixer list              every control: number, name, type, range, values
 *                                 (enumerated: the item names, the selected one marked *)
 *   x6100_mixer set <name> <n>    set a control by name (all channels; enumerated: item index)
 *
 * For finding the codec inputs besides Mic1 (Mic2, Line In, the mixer), where the hand mic
 * might arrive. Settings stay until something sets them again (x6100_trx sets the RX path
 * when it needs the capture).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "x6100_pcm.h"
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CARD	0

//
// One control: its info line and current values
//

static void show(int fd, const struct snd_ctl_elem_id *id)
{
	struct snd_ctl_elem_info info;
	struct snd_ctl_elem_value val;
	static const char *types[] = { "none", "bool", "int", "enum", "bytes", "iec958", "int64" };

	memset(&info, 0, sizeof(info));
	info.id = *id;

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
		printf("%4u  %-40s  (info: %s)\n", id->numid, (const char *) id->name, strerror(errno));
		return;
	}

	memset(&val, 0, sizeof(val));
	val.id = info.id;

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0)
		memset(&val.value, 0, sizeof(val.value));

	printf("%4u  %-40s  %-5s x%u", info.id.numid, (const char *) info.id.name,
		(unsigned) info.type < sizeof(types) / sizeof(types[0]) ? types[info.type] : "?", info.count);

	if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
		printf("  %ld..%ld", info.value.integer.min, info.value.integer.max);

	printf("  =");

	for (unsigned i = 0; i < info.count && i < 8; i++) {
		if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
			printf(" %u", val.value.enumerated.item[i]);
		else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN || info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
			printf(" %ld", val.value.integer.value[i]);
	}

	printf("\n");

	// Enumerated: the item names, the selected one (first channel) marked
	if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned items = info.value.enumerated.items;

		for (unsigned i = 0; i < items; i++) {
			struct snd_ctl_elem_info item = info;

			item.value.enumerated.item = i;

			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &item) == 0)
				printf("        %c %u %s\n", i == val.value.enumerated.item[0] ? '*' : ' ', i,
					item.value.enumerated.name);
		}
	}
}

//
// All controls of the card
//

static int list(void)
{
	struct snd_ctl_elem_list lst;
	struct snd_ctl_elem_id *ids;
	char path[32];
	int fd;

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", CARD);

	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	memset(&lst, 0, sizeof(lst));

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &lst) < 0 || (ids = calloc(lst.count, sizeof(*ids))) == NULL) {
		fprintf(stderr, "list: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	lst.space = lst.count;
	lst.pids = ids;

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &lst) < 0) {
		fprintf(stderr, "list: %s\n", strerror(errno));
		free(ids);
		close(fd);
		return 1;
	}

	printf("card %d: %u controls\n", CARD, lst.used);

	for (unsigned i = 0; i < lst.used; i++)
		show(fd, &ids[i]);

	free(ids);
	close(fd);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "list") == 0)
		return list();

	if (argc == 4 && strcmp(argv[1], "set") == 0) {
		int rc = x6100_mixer_set(CARD, argv[2], strtol(argv[3], NULL, 10));

		if (rc < 0) {
			fprintf(stderr, "%s: %s\n", argv[2], strerror(-rc));
			return 1;
		}

		return 0;
	}

	fprintf(stderr, "usage: %s list | set <name> <value>\n", argv[0]);

	return 1;
}
