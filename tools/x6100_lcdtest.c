/*
 * x6100_lcdtest - probe the X6100 display via DRM/KMS and draw a test screen
 *
 * Display path, from the stock device tree and Qt config:
 *   Jinglitai JLT4013A panel, RGB666 parallel from the A33/R16 TCON,
 *   panel init over a GPIO SPI bus, PWM backlight; 800x480 per qtkmsconfig.json.
 *
 * Raw DRM ioctls only (no libdrm). Finds the first connected connector on
 * /dev/dri/card0..3, prints connectors and modes, then shows a test pattern
 * for a few seconds:
 *   white 4 px border, red block top-left, green block top-right,
 *   colour bars at the bottom, "14.074.000" in large digits in the centre.
 *
 * Stop the GUI first (scripts/x6100_ui.sh stop): only one DRM master at a time.
 *
 * Usage: x6100_lcdtest [rotation 0|90|180|270] [seconds]
 *
 * The panel is native 480x800 portrait, mounted landscape: use 90 or 270.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#define MAX_CONN	8
#define MAX_MODES	32
#define MAX_ENC		8
#define MAX_CRTC	4

typedef struct
{
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	struct drm_mode_modeinfo mode;
	uint32_t fb_id;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
	uint32_t *pix;
} lcd_t;

//
// 5x7 digits and '.', one byte per row, bit 4 = left column
//

static const uint8_t g_font[11][7] = {
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
	{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
	{ 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
	{ 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
	{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
	{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int rc;

	do {
		rc = ioctl(fd, req, arg);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

	return rc;
}

static const char *conn_state(uint32_t s)
{
	return s == 1 ? "connected" : s == 2 ? "disconnected" : "unknown";
}

//
// Enumerate one card; pick the first connected connector with a mode
//

static int probe_card(lcd_t *lcd, const char *path)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t conn_ids[MAX_CONN], enc_ids[MAX_ENC], crtc_ids[MAX_CRTC], fb_ids[4];

	lcd->fd = open(path, O_RDWR | O_CLOEXEC);

	if (lcd->fd < 0)
		return -1;

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		printf("%s: not a KMS device (%s)\n", path, strerror(errno));
		close(lcd->fd);
		return -1;
	}

	if (res.count_connectors > MAX_CONN || res.count_encoders > MAX_ENC
		|| res.count_crtcs > MAX_CRTC || res.count_fbs > 4) {
		close(lcd->fd);
		return -1;
	}

	res.connector_id_ptr = (uintptr_t) conn_ids;
	res.encoder_id_ptr = (uintptr_t) enc_ids;
	res.crtc_id_ptr = (uintptr_t) crtc_ids;
	res.fb_id_ptr = (uintptr_t) fb_ids;

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		close(lcd->fd);
		return -1;
	}

	printf("%s: %u connectors, %u encoders, %u crtcs, %ux%u..%ux%u\n", path,
		res.count_connectors, res.count_encoders, res.count_crtcs,
		res.min_width, res.min_height, res.max_width, res.max_height);

	for (uint32_t c = 0; c < res.count_connectors; c++) {
		struct drm_mode_get_connector conn = { .connector_id = conn_ids[c] };
		struct drm_mode_modeinfo modes[MAX_MODES];
		uint32_t encs[MAX_ENC];
		uint32_t props[32];
		uint64_t prop_vals[32];

		if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;

		if (conn.count_modes > MAX_MODES || conn.count_encoders > MAX_ENC || conn.count_props > 32)
			continue;

		conn.modes_ptr = (uintptr_t) modes;
		conn.encoders_ptr = (uintptr_t) encs;
		conn.props_ptr = (uintptr_t) props;
		conn.prop_values_ptr = (uintptr_t) prop_vals;

		if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;

		printf("  connector %u type %u: %s, %ux%u mm, %u modes\n", conn.connector_id,
			conn.connector_type, conn_state(conn.connection), conn.mm_width, conn.mm_height,
			conn.count_modes);

		for (uint32_t m = 0; m < conn.count_modes; m++)
			printf("    mode %ux%u @ %u Hz, clock %u kHz%s\n", modes[m].hdisplay, modes[m].vdisplay,
				modes[m].vrefresh, modes[m].clock, m == 0 ? "  (used)" : "");

		if (lcd->conn_id != 0 || conn.connection != 1 || conn.count_modes == 0)
			continue;

		//
		// Resolve a CRTC: current encoder first, else any possible one
		//
		uint32_t enc_id = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? encs[0] : 0);
		struct drm_mode_get_encoder enc = { .encoder_id = enc_id };

		if (enc_id == 0 || xioctl(lcd->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
			continue;

		uint32_t crtc = enc.crtc_id;

		for (uint32_t i = 0; crtc == 0 && i < res.count_crtcs; i++) {
			if (enc.possible_crtcs & (1u << i))
				crtc = crtc_ids[i];
		}

		if (crtc == 0)
			continue;

		lcd->conn_id = conn.connector_id;
		lcd->crtc_id = crtc;
		lcd->mode = modes[0];
	}

	if (lcd->conn_id == 0) {
		close(lcd->fd);
		return -1;
	}

	return 0;
}

static int create_fb(lcd_t *lcd)
{
	struct drm_mode_create_dumb cd = { .width = lcd->mode.hdisplay, .height = lcd->mode.vdisplay, .bpp = 32 };
	struct drm_mode_fb_cmd fb = { 0 };
	struct drm_mode_map_dumb md = { 0 };

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return -errno;

	lcd->handle = cd.handle;
	lcd->pitch = cd.pitch;
	lcd->size = cd.size;

	fb.width = cd.width;
	fb.height = cd.height;
	fb.pitch = cd.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = cd.handle;

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0)
		return -errno;

	lcd->fb_id = fb.fb_id;
	md.handle = cd.handle;

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return -errno;

	lcd->pix = mmap(NULL, (size_t) cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, lcd->fd, (off_t) md.offset);

	return lcd->pix == MAP_FAILED ? -errno : 0;
}

static int show_fb(lcd_t *lcd)
{
	struct drm_mode_crtc crtc = { 0 };
	uint32_t conn = lcd->conn_id;

	crtc.crtc_id = lcd->crtc_id;
	crtc.fb_id = lcd->fb_id;
	crtc.set_connectors_ptr = (uintptr_t) &conn;
	crtc.count_connectors = 1;
	crtc.mode = lcd->mode;
	crtc.mode_valid = 1;

	return xioctl(lcd->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0 ? -errno : 0;
}

//
// Drawing, XRGB8888
//

//
// Logical canvas: landscape when rot is 90 or 270, mapped onto the
// physical portrait panel (480x800)
//

static int g_rot;

static int logical_w(const lcd_t *lcd)
{
	return g_rot % 180 ? lcd->mode.vdisplay : lcd->mode.hdisplay;
}

static int logical_h(const lcd_t *lcd)
{
	return g_rot % 180 ? lcd->mode.hdisplay : lcd->mode.vdisplay;
}

static void put(lcd_t *lcd, int x, int y, uint32_t rgb)
{
	int PW = lcd->mode.hdisplay, PH = lcd->mode.vdisplay;
	int px, py;

	switch (g_rot) {
	case 90:
		px = PW - 1 - y;
		py = x;
		break;
	case 180:
		px = PW - 1 - x;
		py = PH - 1 - y;
		break;
	case 270:
		px = y;
		py = PH - 1 - x;
		break;
	default:
		px = x;
		py = y;
		break;
	}

	if (px < 0 || px >= PW || py < 0 || py >= PH)
		return;

	((uint32_t *) ((uint8_t *) lcd->pix + (size_t) py * lcd->pitch))[px] = rgb;
}

static void fill(lcd_t *lcd, int x, int y, int w, int h, uint32_t rgb)
{
	for (int j = y; j < y + h; j++) {
		for (int i = x; i < x + w; i++)
			put(lcd, i, j, rgb);
	}
}

static void draw_text(lcd_t *lcd, int x, int y, int scale, const char *s, uint32_t rgb)
{
	for (; *s; s++, x += 6 * scale) {
		int g = *s == '.' ? 10 : *s - '0';

		if (g < 0 || g > 10)
			continue;

		for (int r = 0; r < 7; r++) {
			for (int c = 0; c < 5; c++) {
				if (g_font[g][r] & (0x10 >> c))
					fill(lcd, x + c * scale, y + r * scale, scale, scale, rgb);
			}
		}
	}
}

static void test_pattern(lcd_t *lcd)
{
	static const uint32_t bars[] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
	int W = logical_w(lcd), H = logical_h(lcd);
	int bw = W / 8;
	const char *txt = "14.074.000";
	int scale = W / 80 > 1 ? W / 80 : 1;
	int tw = (int) strlen(txt) * 6 * scale;

	fill(lcd, 0, 0, W, H, 0x101820);
	fill(lcd, 0, 0, W, 4, 0xFFFFFF);
	fill(lcd, 0, H - 4, W, 4, 0xFFFFFF);
	fill(lcd, 0, 0, 4, H, 0xFFFFFF);
	fill(lcd, W - 4, 0, 4, H, 0xFFFFFF);
	fill(lcd, 12, 12, 40, 40, 0xFF0000);
	fill(lcd, W - 52, 12, 40, 40, 0x00FF00);

	for (int i = 0; i < 8; i++)
		fill(lcd, 4 + i * bw, H - 4 - H / 6, bw, H / 6, bars[i]);

	draw_text(lcd, (W - tw) / 2, H / 2 - 7 * scale / 2 - H / 12, scale, txt, 0xFFD040);
}

int main(int argc, char **argv)
{
	lcd_t lcd = { .fd = -1 };
	int seconds = argc > 2 ? atoi(argv[2]) : 10;

	g_rot = argc > 1 ? atoi(argv[1]) : 270;

	if (g_rot != 0 && g_rot != 90 && g_rot != 180 && g_rot != 270) {
		fprintf(stderr, "usage: %s [0|90|180|270] [seconds]\n", argv[0]);
		return 1;
	}
	int rc;

	for (int n = 0; n < 4; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/card%d", n);

		if (probe_card(&lcd, path) == 0) {
			printf("using %s connector %u crtc %u, %ux%u\n", path, lcd.conn_id, lcd.crtc_id,
				lcd.mode.hdisplay, lcd.mode.vdisplay);
			break;
		}
	}

	if (lcd.conn_id == 0) {
		fprintf(stderr, "no connected DRM output found\n");
		return 1;
	}

	if ((rc = xioctl(lcd.fd, DRM_IOCTL_SET_MASTER, NULL)) < 0)
		fprintf(stderr, "warning: not DRM master (%s); is the GUI still running?\n", strerror(errno));

	if ((rc = create_fb(&lcd)) < 0) {
		fprintf(stderr, "framebuffer: %s\n", strerror(-rc));
		return 1;
	}

	printf("dumb buffer: pitch %u, size %llu\n", lcd.pitch, (unsigned long long) lcd.size);
	test_pattern(&lcd);

	if ((rc = show_fb(&lcd)) < 0) {
		fprintf(stderr, "setcrtc: %s\n", strerror(-rc));
		return 1;
	}

	printf("test pattern, rotation %d, logical %dx%d, shown for %d s\n", g_rot, logical_w(&lcd), logical_h(&lcd), seconds);
	sleep((unsigned) seconds);

	munmap(lcd.pix, (size_t) lcd.size);
	close(lcd.fd);
	return 0;
}
