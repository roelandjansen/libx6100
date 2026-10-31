/*
 * x6100_lcd.c - Xiegu X6100 display via DRM/KMS (raw ioctls, no libdrm)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "x6100_lcd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>

#define MAX_CONN	8
#define MAX_MODES	16
#define MAX_ENC		8
#define MAX_CRTC	4
#define MAX_FB		4

typedef struct
{
	char c;
	uint8_t rows[X6100_LCD_FONT_H];
} glyph_t;

//
// 5x7 font, bit 4 = left column; lower case without own glyph is drawn as upper case
//

static const glyph_t g_font[] = {
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
	{ '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
	{ '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
	{ 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
	{ 'd', { 0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F } },
	{ 'k', { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 } },
	{ 'm', { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 } },
	{ 'z', { 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
	{ ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
	{ '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
	{ '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
	{ '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
	{ '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
	{ '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int rc;

	do {
		rc = ioctl(fd, req, arg);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));

	return rc;
}

static const uint8_t *glyph_exact(char c)
{
	for (size_t i = 0; i < sizeof(g_font) / sizeof(g_font[0]); i++) {
		if (g_font[i].c == c)
			return g_font[i].rows;
	}

	return NULL;
}

static const uint8_t *glyph(char c)
{
	const uint8_t *g = glyph_exact(c);

	return g != NULL ? g : glyph_exact((char) toupper((unsigned char) c));
}

//
// Find the first connected connector with a mode and a usable CRTC
//

static int probe_card(x6100_lcd_t *lcd)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t conn_ids[MAX_CONN], enc_ids[MAX_ENC], crtc_ids[MAX_CRTC], fb_ids[MAX_FB];

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;

	if (res.count_connectors > MAX_CONN || res.count_encoders > MAX_ENC
		|| res.count_crtcs > MAX_CRTC || res.count_fbs > MAX_FB)
		return -1;

	res.connector_id_ptr = (uintptr_t) conn_ids;
	res.encoder_id_ptr = (uintptr_t) enc_ids;
	res.crtc_id_ptr = (uintptr_t) crtc_ids;
	res.fb_id_ptr = (uintptr_t) fb_ids;

	if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;

	for (uint32_t c = 0; c < res.count_connectors; c++) {
		struct drm_mode_get_connector conn = { .connector_id = conn_ids[c] };
		struct drm_mode_modeinfo modes[MAX_MODES];
		uint32_t encs[MAX_ENC];

		if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;

		if (conn.connection != 1 || conn.count_modes == 0
			|| conn.count_modes > MAX_MODES || conn.count_encoders > MAX_ENC)
			continue;

		conn.modes_ptr = (uintptr_t) modes;
		conn.encoders_ptr = (uintptr_t) encs;
		conn.count_props = 0;

		if (xioctl(lcd->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;

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
		return 0;
	}

	return -1;
}

static int create_scanout(x6100_lcd_t *lcd)
{
	struct drm_mode_create_dumb cd = { .width = lcd->mode.hdisplay, .height = lcd->mode.vdisplay, .bpp = 32 };
	struct drm_mode_fb_cmd fb = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	uint32_t conn = lcd->conn_id;

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

	lcd->scanout = mmap(NULL, (size_t) cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, lcd->fd, (off_t) md.offset);

	if (lcd->scanout == MAP_FAILED) {
		lcd->scanout = NULL;
		return -errno;
	}

	memset(lcd->scanout, 0, (size_t) cd.size);

	crtc.crtc_id = lcd->crtc_id;
	crtc.fb_id = lcd->fb_id;
	crtc.set_connectors_ptr = (uintptr_t) &conn;
	crtc.count_connectors = 1;
	crtc.mode = lcd->mode;
	crtc.mode_valid = 1;

	return xioctl(lcd->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0 ? -errno : 0;
}

int x6100_lcd_open(x6100_lcd_t *lcd, int rotation)
{
	int rc = -ENODEV;

	memset(lcd, 0, sizeof(*lcd));
	lcd->fd = -1;
	lcd->rot = rotation;

	if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
		return -EINVAL;

	for (int n = 0; n < 4 && lcd->conn_id == 0; n++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/card%d", n);
		lcd->fd = open(path, O_RDWR | O_CLOEXEC);

		if (lcd->fd < 0)
			continue;

		if (probe_card(lcd) == 0)
			break;

		close(lcd->fd);
		lcd->fd = -1;
	}

	if (lcd->fd < 0)
		return -ENODEV;

	xioctl(lcd->fd, DRM_IOCTL_SET_MASTER, NULL);

	if ((rc = create_scanout(lcd)) < 0) {
		x6100_lcd_close(lcd);
		return rc;
	}

	lcd->w = rotation % 180 ? lcd->mode.vdisplay : lcd->mode.hdisplay;
	lcd->h = rotation % 180 ? lcd->mode.hdisplay : lcd->mode.vdisplay;
	lcd->pix = calloc((size_t) lcd->w * (size_t) lcd->h, sizeof(uint32_t));

	if (lcd->pix == NULL) {
		x6100_lcd_close(lcd);
		return -ENOMEM;
	}

	return 0;
}

void x6100_lcd_close(x6100_lcd_t *lcd)
{
	free(lcd->pix);
	lcd->pix = NULL;

	if (lcd->scanout != NULL)
		munmap(lcd->scanout, (size_t) lcd->size);

	lcd->scanout = NULL;

	if (lcd->fd >= 0)
		close(lcd->fd);

	lcd->fd = -1;
}

//
// Copy the logical canvas into the scanout buffer with rotation
//

int x6100_lcd_flush(x6100_lcd_t *lcd)
{
	int PW = lcd->mode.hdisplay, PH = lcd->mode.vdisplay;

	if (lcd->scanout == NULL)
		return -ENODEV;

	for (int py = 0; py < PH; py++) {
		uint32_t *dst = (uint32_t *) ((uint8_t *) lcd->scanout + (size_t) py * lcd->pitch);

		for (int px = 0; px < PW; px++) {
			int x, y;

			switch (lcd->rot) {
			case 90:
				x = py;
				y = PW - 1 - px;
				break;
			case 180:
				x = PW - 1 - px;
				y = PH - 1 - py;
				break;
			case 270:
				x = PH - 1 - py;
				y = px;
				break;
			default:
				x = px;
				y = py;
				break;
			}

			dst[px] = lcd->pix[(size_t) y * (size_t) lcd->w + (size_t) x];
		}
	}

	return 0;
}

//
// Drawing
//

void x6100_lcd_fill(x6100_lcd_t *lcd, int x, int y, int w, int h, uint32_t rgb)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > lcd->w ? lcd->w : x + w;
	int y1 = y + h > lcd->h ? lcd->h : y + h;

	for (int j = y0; j < y1; j++) {
		uint32_t *row = lcd->pix + (size_t) j * (size_t) lcd->w;

		for (int i = x0; i < x1; i++)
			row[i] = rgb;
	}
}

void x6100_lcd_vline(x6100_lcd_t *lcd, int x, int y0, int y1, uint32_t rgb)
{
	if (y0 > y1) {
		int t = y0;

		y0 = y1;
		y1 = t;
	}

	x6100_lcd_fill(lcd, x, y0, 1, y1 - y0 + 1, rgb);
}

void x6100_lcd_hline(x6100_lcd_t *lcd, int x0, int x1, int y, uint32_t rgb)
{
	if (x0 > x1) {
		int t = x0;

		x0 = x1;
		x1 = t;
	}

	x6100_lcd_fill(lcd, x0, y, x1 - x0 + 1, 1, rgb);
}

//
// Bresenham line; width > 1 draws a square pen of that size
//

void x6100_lcd_line(x6100_lcd_t *lcd, int x0, int y0, int x1, int y1, int width, uint32_t rgb)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int off = width / 2;

	if (width < 1)
		width = 1;

	for (;;) {
		x6100_lcd_fill(lcd, x0 - off, y0 - off, width, width, rgb);

		if (x0 == x1 && y0 == y1)
			break;

		int e2 = 2 * err;

		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}

		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

int x6100_lcd_text_width(const char *s, int scale)
{
	return (int) strlen(s) * (X6100_LCD_FONT_W + 1) * scale;
}

int x6100_lcd_text(x6100_lcd_t *lcd, int x, int y, int scale, const char *s, uint32_t rgb)
{
	int x0 = x;

	for (; *s; s++, x += (X6100_LCD_FONT_W + 1) * scale) {
		const uint8_t *g = glyph(*s);

		if (g == NULL)
			continue;

		for (int r = 0; r < X6100_LCD_FONT_H; r++) {
			for (int c = 0; c < X6100_LCD_FONT_W; c++) {
				if (g[r] & (0x10 >> c))
					x6100_lcd_fill(lcd, x + c * scale, y + r * scale, scale, scale, rgb);
			}
		}
	}

	return x - x0;
}

//
// Move rows [y, y+h-2] down by one; row y is left for the caller to fill
//

void x6100_lcd_scroll_down(x6100_lcd_t *lcd, int y, int h)
{
	if (h < 2 || y < 0 || y + h > lcd->h)
		return;

	memmove(lcd->pix + (size_t) (y + 1) * (size_t) lcd->w, lcd->pix + (size_t) y * (size_t) lcd->w,
		(size_t) (h - 1) * (size_t) lcd->w * sizeof(uint32_t));
}

//
// Anti-aliased text, alpha blended over the canvas
//

static const x6100_aa_glyph_t *aa_glyph(const x6100_aa_font_t *font, char c)
{
	for (int i = 0; i < font->count; i++) {
		if (font->glyphs[i].c == c)
			return &font->glyphs[i];
	}

	return NULL;
}

int x6100_lcd_text_aa_width(const x6100_aa_font_t *font, const char *s)
{
	int w = 0;

	for (; *s; s++) {
		const x6100_aa_glyph_t *g = aa_glyph(font, *s);

		w += g != NULL ? g->adv : font->height / 3;
	}

	return w;
}

static uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
	uint32_t r = ((src >> 16 & 0xFF) * a + (dst >> 16 & 0xFF) * (255 - a)) / 255;
	uint32_t g = ((src >> 8 & 0xFF) * a + (dst >> 8 & 0xFF) * (255 - a)) / 255;
	uint32_t b = ((src & 0xFF) * a + (dst & 0xFF) * (255 - a)) / 255;

	return r << 16 | g << 8 | b;
}

int x6100_lcd_text_aa(x6100_lcd_t *lcd, const x6100_aa_font_t *font, int x, int y, const char *s, uint32_t rgb)
{
	int x0 = x;

	for (; *s; s++) {
		const x6100_aa_glyph_t *g = aa_glyph(font, *s);

		if (g == NULL) {
			x += font->height / 3;
			continue;
		}

		for (int j = 0; j < g->h; j++) {
			int py = y + g->yoff + j;

			if (py < 0 || py >= lcd->h)
				continue;

			uint32_t *row = lcd->pix + (size_t) py * (size_t) lcd->w;

			for (int i = 0; i < g->w; i++) {
				int px = x + g->xoff + i;
				uint32_t a = g->alpha[j * g->w + i];

				if (a == 0 || px < 0 || px >= lcd->w)
					continue;

				row[px] = a == 255 ? rgb : blend(row[px], rgb, a);
			}
		}

		x += g->adv;
	}

	return x - x0;
}
