/*
 * x6100_i2csniff.so - LD_PRELOAD logger for baseband register writes
 *
 * Preload into a GUI that drives the baseband (stock or R1CBU) to see exactly
 * which registers it writes, e.g. during an FT8 transmission:
 *
 *   scripts/x6100_ui.sh stop
 *   LD_PRELOAD=/root/x6100_i2csniff.so /usr/app_qt/x6100_gui &   (path as in x6100_daemon)
 *
 * Catches open/openat on /dev/i2c-*, then write() and ioctl(I2C_RDWR) on that fd.
 * The register file (address 0x0000-0x00DF, 4-byte aligned) is shadowed: only
 * registers whose value changes are logged, so the 1 ms resend loop stays quiet.
 * Everything else (command mailbox, reads' address phase) is logged raw.
 *
 * Log: /tmp/x6100_i2c.log, or the path in X6100_SNIFF_LOG.
 *   <ms> REG nn  old -> new
 *   <ms> RAW addr len: bytes
 *
 * Read-only towards the radio: every call is passed through unchanged.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NREGS		56
#define MAX_FDS		1024

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_ioctl)(int, unsigned long, ...);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_i2c_fd[MAX_FDS];
static uint32_t g_shadow[NREGS];
static bool g_known[NREGS];
static int g_log = -1;
static uint64_t g_t0;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

//
// Setup: resolve real symbols, open the log
//

__attribute__((constructor)) static void sniff_init(void)
{
	const char *path = getenv("X6100_SNIFF_LOG");

	real_open = dlsym(RTLD_NEXT, "open");
	real_open64 = dlsym(RTLD_NEXT, "open64");
	real_openat = dlsym(RTLD_NEXT, "openat");
	real_write = dlsym(RTLD_NEXT, "write");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	g_t0 = now_ms();
	g_log = real_open(path ? path : "/tmp/x6100_i2c.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

static void log_line(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;

	if (g_log < 0)
		return;

	n = snprintf(line, sizeof(line), "%8llu ", (unsigned long long) (now_ms() - g_t0));
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t) n, fmt, ap);
	va_end(ap);

	if (n > (int) sizeof(line) - 2)
		n = (int) sizeof(line) - 2;

	line[n++] = '\n';
	real_write(g_log, line, (size_t) n);
}

//
// Decode one write transaction: BE16 address, then data
//

static void decode(const uint8_t *buf, size_t len)
{
	uint16_t addr;
	size_t i;

	if (len < 2)
		return;

	addr = (uint16_t) (buf[0] << 8 | buf[1]);

	pthread_mutex_lock(&g_lock);

	if (addr % 4 == 0 && addr < NREGS * 4 && (len - 2) % 4 == 0 && len > 2) {
		for (i = 0; i + 2 < len; i += 4) {
			unsigned reg = addr / 4 + (unsigned) (i / 4);
			uint32_t v;

			if (reg >= NREGS)
				break;

			memcpy(&v, buf + 2 + i, 4);

			if (!g_known[reg] || g_shadow[reg] != v) {
				log_line("REG %2u  %08X -> %08X", reg, g_known[reg] ? g_shadow[reg] : 0u, v);
				g_shadow[reg] = v;
				g_known[reg] = true;
			}
		}
	} else if (len > 2) {
		char hex[3 * 64 + 1];
		size_t n = 0;

		for (i = 2; i < len && i < 2 + 64; i++)
			n += (size_t) snprintf(hex + n, sizeof(hex) - n, " %02X", buf[i]);

		hex[n] = 0;
		log_line("RAW %04X %zu:%s", addr, len - 2, hex);
	}

	pthread_mutex_unlock(&g_lock);
}

static void track(int fd, const char *path)
{
	if (fd >= 0 && fd < MAX_FDS)
		g_i2c_fd[fd] = path && strncmp(path, "/dev/i2c-", 9) == 0;
}

//
// Interposed calls
//

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;
	int fd;

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = (mode_t) va_arg(ap, int);
	va_end(ap);

	fd = real_open(path, flags, mode);
	track(fd, path);
	return fd;
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;
	int fd;

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = (mode_t) va_arg(ap, int);
	va_end(ap);

	fd = real_open64 ? real_open64(path, flags, mode) : real_open(path, flags, mode);
	track(fd, path);
	return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;
	int fd;

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = (mode_t) va_arg(ap, int);
	va_end(ap);

	fd = real_openat(dirfd, path, flags, mode);
	track(fd, path);
	return fd;
}

ssize_t write(int fd, const void *buf, size_t len)
{
	if (fd >= 0 && fd < MAX_FDS && g_i2c_fd[fd])
		decode(buf, len);

	return real_write(fd, buf, len);
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (req == I2C_RDWR && fd >= 0 && fd < MAX_FDS && g_i2c_fd[fd] && arg) {
		const struct i2c_rdwr_ioctl_data *d = arg;

		for (unsigned i = 0; i < d->nmsgs; i++) {
			if (!(d->msgs[i].flags & I2C_M_RD))
				decode(d->msgs[i].buf, d->msgs[i].len);
		}
	}

	return real_ioctl(fd, req, arg);
}
