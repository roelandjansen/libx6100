/*
 * x6100_lcd.h - Xiegu X6100 display via DRM/KMS
 *
 * Panel: Jinglitai JLT4013A, native 480x800 portrait, mounted landscape.
 * Verified on hardware: rotation 270 gives an upright 800x480 landscape canvas.
 *
 * Drawing goes to a landscape back buffer in RAM; x6100_lcd_flush() rotates it
 * into the DRM dumb buffer. Colours are 0xRRGGBB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <drm/drm_mode.h>

#define X6100_LCD_ROTATION	270
#define X6100_LCD_FONT_W	5
#define X6100_LCD_FONT_H	7

//
// Anti-aliased bitmap font: 8-bit alpha per glyph, y offset from the line top
//

typedef struct
{
	char c;
	uint8_t w;
	uint8_t h;
	int8_t xoff;
	int8_t yoff;
	uint8_t adv;
	const uint8_t *alpha;
} x6100_aa_glyph_t;

typedef struct
{
	int height;
	int count;
	const x6100_aa_glyph_t *glyphs;
} x6100_aa_font_t;

extern const x6100_aa_font_t x6100_font_freq;

typedef struct
{
	//
	// DRM state
	//
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
	uint32_t *scanout;
	struct drm_mode_modeinfo mode;

	//
	// Logical landscape canvas
	//
	int rot;
	int w;
	int h;
	uint32_t *pix;
} x6100_lcd_t;

//
// Setup
//

int x6100_lcd_open(x6100_lcd_t *lcd, int rotation);
void x6100_lcd_close(x6100_lcd_t *lcd);
int x6100_lcd_flush(x6100_lcd_t *lcd);

//
// Drawing on the logical canvas
//

void x6100_lcd_fill(x6100_lcd_t *lcd, int x, int y, int w, int h, uint32_t rgb);
void x6100_lcd_vline(x6100_lcd_t *lcd, int x, int y0, int y1, uint32_t rgb);
void x6100_lcd_hline(x6100_lcd_t *lcd, int x0, int x1, int y, uint32_t rgb);
void x6100_lcd_line(x6100_lcd_t *lcd, int x0, int y0, int x1, int y1, int width, uint32_t rgb);
int x6100_lcd_text(x6100_lcd_t *lcd, int x, int y, int scale, const char *s, uint32_t rgb);
int x6100_lcd_text_width(const char *s, int scale);
void x6100_lcd_scroll_down(x6100_lcd_t *lcd, int y, int h);

//
// Anti-aliased text; the 5x7 functions above stay available as the fallback font
//

int x6100_lcd_text_aa(x6100_lcd_t *lcd, const x6100_aa_font_t *font, int x, int y, const char *s, uint32_t rgb);
int x6100_lcd_text_aa_width(const x6100_aa_font_t *font, const char *s);
