/*
 * x6100_input - list front panel input devices and print decoded events
 *
 * Front panel, from the stock device tree (sun8i-r16-x6100.dtb):
 *   gpio-matrix-keypad   5 rows x 6 cols, codes BTN_TRIGGER_HAPPY1..30 (0x2C0..0x2DD),
 *                        key index = code - 0x2C0 = row * 6 + col
 *   rotary-encoder x3    relative axes REL_X (PB2/PB3), REL_Y (PB4/PB6), REL_Z (PB5/PB7)
 *
 * Usage: x6100_input        press every key and turn every knob to build the map
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEV		16
#define KEY_BASE	0x2C0
#define KEY_COUNT	30
#define KEY_COLS	6

typedef struct
{
	int fd;
	char path[32];
	char name[64];
} dev_t_;

static int open_devices(dev_t_ *devs)
{
	int n = 0;

	for (int i = 0; i < MAX_DEV && n < MAX_DEV; i++) {
		dev_t_ *d = &devs[n];

		snprintf(d->path, sizeof(d->path), "/dev/input/event%d", i);
		d->fd = open(d->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

		if (d->fd < 0)
			continue;

		if (ioctl(d->fd, EVIOCGNAME(sizeof(d->name)), d->name) < 0)
			snprintf(d->name, sizeof(d->name), "%s", "?");

		printf("%-18s %s\n", d->path, d->name);
		n++;
	}

	return n;
}

static void print_event(const dev_t_ *d, const struct input_event *ev)
{
	if (ev->type == EV_KEY) {
		int idx = (int) ev->code - KEY_BASE;
		const char *state = ev->value == 1 ? "press" : ev->value == 0 ? "release" : "repeat";

		if (idx >= 0 && idx < KEY_COUNT)
			printf("%-18s key K%02d (row %d col %d) %s\n", d->path, idx, idx / KEY_COLS, idx % KEY_COLS, state);
		else
			printf("%-18s key code 0x%03X %s\n", d->path, ev->code, state);
	} else if (ev->type == EV_REL) {
		static const char *axes[] = { "REL_X", "REL_Y", "REL_Z" };
		const char *axis = ev->code < 3 ? axes[ev->code] : "REL_?";

		printf("%-18s %s %+d  [%s]\n", d->path, axis, ev->value, d->name);
	} else if (ev->type != EV_SYN && ev->type != EV_MSC) {
		printf("%-18s type %u code %u value %d\n", d->path, ev->type, ev->code, ev->value);
	}

	fflush(stdout);
}

int main(void)
{
	dev_t_ devs[MAX_DEV];
	struct pollfd pfd[MAX_DEV];
	int n = open_devices(devs);

	if (n == 0) {
		fprintf(stderr, "no input devices found\n");
		return 1;
	}

	printf("-- press keys / turn knobs, Ctrl-C to stop --\n");

	for (int i = 0; i < n; i++) {
		pfd[i].fd = devs[i].fd;
		pfd[i].events = POLLIN;
	}

	for (;;) {
		if (poll(pfd, (nfds_t) n, -1) < 0) {
			if (errno == EINTR)
				continue;

			perror("poll");
			return 1;
		}

		for (int i = 0; i < n; i++) {
			struct input_event ev;

			if (!(pfd[i].revents & POLLIN))
				continue;

			while (read(devs[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev))
				print_event(&devs[i], &ev);
		}
	}
}
