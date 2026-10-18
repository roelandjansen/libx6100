/*
 * x6100_rodump - read-only dump of the X6100 baseband status and calibration areas
 *
 * Protocol (from stock APP 1.2.0 / BASE 1.1.9, x6100_ui_v100):
 *   bus /dev/i2c-0, slave 0x72
 *   read = write 2-byte big-endian address (separate transaction),
 *          then plain read of N bytes
 *   0x0000  128 bytes  read-only status block, starts with NUL-terminated BASE version
 *   0xF000  816 bytes  calibration block (factory mode)
 *
 * Only the 2-byte address pointer is ever written. Stop the radio UI first:
 * it pushes registers on the same bus every millisecond.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define X6100_I2C_DEV		"/dev/i2c-0"
#define X6100_I2C_ADDR		0x72

#define X6100_RORAM_ADDR	0x0000
#define X6100_RORAM_LEN		128

#define X6100_CAL_ADDR		0xF000
#define X6100_CAL_LEN		816

//
// Set the 16-bit read pointer, then read len bytes
//

static int x6100_read(int fd, uint16_t addr, uint8_t *buf, uint16_t len)
{
	uint8_t ptr[2] = { addr >> 8, addr & 0xFF };
	struct i2c_msg msg;
	struct i2c_rdwr_ioctl_data xfer = { &msg, 1 };

	msg.addr = X6100_I2C_ADDR;
	msg.flags = 0;
	msg.len = sizeof(ptr);
	msg.buf = ptr;

	if (ioctl(fd, I2C_RDWR, &xfer) < 0)
		return -errno;

	msg.flags = I2C_M_RD;
	msg.len = len;
	msg.buf = buf;

	if (ioctl(fd, I2C_RDWR, &xfer) < 0)
		return -errno;

	return 0;
}

//
// Hexdump with offsets relative to the area base address
//

static void hexdump(uint16_t base, const uint8_t *buf, uint16_t len)
{
	for (uint16_t i = 0; i < len; i += 16) {
		printf("%04X ", base + i);

		for (uint16_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf(" %02X", buf[i + j]);
			else
				printf("   ");
		}

		printf("  ");

		for (uint16_t j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = buf[i + j];
			putchar(c >= 0x20 && c < 0x7F ? c : '.');
		}

		putchar('\n');
	}
}

static int dump_area(int fd, const char *name, uint16_t addr, uint16_t len, const char *out)
{
	uint8_t *buf = calloc(1, len);
	int rc;

	if (buf == NULL)
		return -ENOMEM;

	rc = x6100_read(fd, addr, buf, len);
	if (rc < 0) {
		fprintf(stderr, "%s: read 0x%04X failed: %s\n", name, addr, strerror(-rc));
		free(buf);
		return rc;
	}

	printf("== %s @ 0x%04X, %u bytes\n", name, addr, len);
	hexdump(addr, buf, len);

	if (addr == X6100_RORAM_ADDR)
		printf("version string: \"%.*s\"\n", (int) strnlen((char *) buf, len), buf);

	if (out != NULL) {
		FILE *f = fopen(out, "wb");

		if (f == NULL || fwrite(buf, 1, len, f) != len)
			fprintf(stderr, "%s: cannot write %s\n", name, out);
		else
			printf("saved to %s\n", out);

		if (f != NULL)
			fclose(f);
	}

	putchar('\n');
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	int with_cal = (argc > 1 && strcmp(argv[1], "-c") == 0);
	int fd = open(X6100_I2C_DEV, O_RDWR);
	int rc;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", X6100_I2C_DEV, strerror(errno));
		return 1;
	}

	rc = dump_area(fd, "roram", X6100_RORAM_ADDR, X6100_RORAM_LEN, "roram.bin");

	if (rc == 0 && with_cal)
		rc = dump_area(fd, "calibration", X6100_CAL_ADDR, X6100_CAL_LEN, "cal_f000.bin");

	close(fd);
	return rc == 0 ? 0 : 1;
}
