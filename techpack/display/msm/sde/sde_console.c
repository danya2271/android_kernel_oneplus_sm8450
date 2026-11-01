// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <asm/cacheflush.h>

#include "../msm_kms.h"
#include "sde_kms.h"
#include "sde_plane.h"
#include "sde_console.h"

#define SDE_CONSOLE_FONT_WIDTH	8
#define SDE_CONSOLE_FONT_HEIGHT	11
#define SDE_CONSOLE_GLYPH_WIDTH	6
#define SDE_CONSOLE_GLYPH_HEIGHT	9

struct sde_console {
	struct drm_device *dev;
	struct console console;
	void *vaddr;
	void *map_base;

	struct work_struct work;
	spinlock_t lock;
	u8 *cells;
	u8 *snapshot;
	u32 width;
	u32 height;
	u32 pitch;
	u32 cols;
	u32 rows;
	u32 row;
	u32 col;
	u32 top;
	bool active;
	bool registered;
};

static DEFINE_MUTEX(sde_console_mutex);
static struct sde_console *sde_console;

static void sde_console_get_glyph(u8 ch, u8 glyph[5])
{
	static const u8 digits[10][5] = {
		{ 0x3e, 0x51, 0x49, 0x45, 0x3e },
		{ 0x00, 0x42, 0x7f, 0x40, 0x00 },
		{ 0x42, 0x61, 0x51, 0x49, 0x46 },
		{ 0x21, 0x41, 0x45, 0x4b, 0x31 },
		{ 0x18, 0x14, 0x12, 0x7f, 0x10 },
		{ 0x27, 0x45, 0x45, 0x45, 0x39 },
		{ 0x3c, 0x4a, 0x49, 0x49, 0x30 },
		{ 0x01, 0x71, 0x09, 0x05, 0x03 },
		{ 0x36, 0x49, 0x49, 0x49, 0x36 },
		{ 0x06, 0x49, 0x49, 0x29, 0x1e },
	};
	static const u8 letters[26][5] = {
		{ 0x7e, 0x11, 0x11, 0x11, 0x7e },
		{ 0x7f, 0x49, 0x49, 0x49, 0x36 },
		{ 0x3e, 0x41, 0x41, 0x41, 0x22 },
		{ 0x7f, 0x41, 0x41, 0x22, 0x1c },
		{ 0x7f, 0x49, 0x49, 0x49, 0x41 },
		{ 0x7f, 0x09, 0x09, 0x09, 0x01 },
		{ 0x3e, 0x41, 0x49, 0x49, 0x7a },
		{ 0x7f, 0x08, 0x08, 0x08, 0x7f },
		{ 0x00, 0x41, 0x7f, 0x41, 0x00 },
		{ 0x20, 0x40, 0x41, 0x3f, 0x01 },
		{ 0x7f, 0x08, 0x14, 0x22, 0x41 },
		{ 0x7f, 0x40, 0x40, 0x40, 0x40 },
		{ 0x7f, 0x02, 0x0c, 0x02, 0x7f },
		{ 0x7f, 0x04, 0x08, 0x10, 0x7f },
		{ 0x3e, 0x41, 0x41, 0x41, 0x3e },
		{ 0x7f, 0x09, 0x09, 0x09, 0x06 },
		{ 0x3e, 0x41, 0x51, 0x21, 0x5e },
		{ 0x7f, 0x09, 0x19, 0x29, 0x46 },
		{ 0x46, 0x49, 0x49, 0x49, 0x31 },
		{ 0x01, 0x01, 0x7f, 0x01, 0x01 },
		{ 0x3f, 0x40, 0x40, 0x40, 0x3f },
		{ 0x1f, 0x20, 0x40, 0x20, 0x1f },
		{ 0x3f, 0x40, 0x38, 0x40, 0x3f },
		{ 0x63, 0x14, 0x08, 0x14, 0x63 },
		{ 0x07, 0x08, 0x70, 0x08, 0x07 },
		{ 0x61, 0x51, 0x49, 0x45, 0x43 },
	};

	memset(glyph, 0, 5);

	if (ch >= 'a' && ch <= 'z')
		ch -= 'a' - 'A';

	if (ch >= '0' && ch <= '9') {
		memcpy(glyph, digits[ch - '0'], 5);
		return;
	}

	if (ch >= 'A' && ch <= 'Z') {
		memcpy(glyph, letters[ch - 'A'], 5);
		return;
	}

	switch (ch) {
	case '!':
		glyph[2] = 0x5f;
		break;
	case '"':
		glyph[1] = 0x03;
		glyph[3] = 0x03;
		break;
	case '#':
		glyph[0] = 0x14;
		glyph[1] = 0x7f;
		glyph[2] = 0x14;
		glyph[3] = 0x7f;
		glyph[4] = 0x14;
		break;
	case '$':
		glyph[0] = 0x24;
		glyph[1] = 0x2a;
		glyph[2] = 0x7f;
		glyph[3] = 0x2a;
		glyph[4] = 0x12;
		break;
	case '%':
		glyph[0] = 0x63;
		glyph[1] = 0x13;
		glyph[2] = 0x08;
		glyph[3] = 0x64;
		glyph[4] = 0x63;
		break;
	case '&':
		glyph[0] = 0x36;
		glyph[1] = 0x49;
		glyph[2] = 0x55;
		glyph[3] = 0x22;
		glyph[4] = 0x50;
		break;
	case '\'':
		glyph[2] = 0x03;
		break;
	case '(':
		glyph[1] = 0x1c;
		glyph[2] = 0x22;
		glyph[3] = 0x41;
		break;
	case ')':
		glyph[1] = 0x41;
		glyph[2] = 0x22;
		glyph[3] = 0x1c;
		break;
	case '*':
		glyph[1] = 0x14;
		glyph[2] = 0x08;
		glyph[3] = 0x14;
		break;
	case '+':
		glyph[1] = 0x08;
		glyph[2] = 0x3e;
		glyph[3] = 0x08;
		break;
	case ',':
		glyph[1] = 0x50;
		glyph[2] = 0x30;
		break;
	case '-':
		glyph[1] = 0x08;
		glyph[2] = 0x08;
		glyph[3] = 0x08;
		break;
	case '.':
		glyph[3] = 0x60;
		glyph[4] = 0x60;
		break;
	case '/':
		glyph[0] = 0x20;
		glyph[1] = 0x10;
		glyph[2] = 0x08;
		glyph[3] = 0x04;
		glyph[4] = 0x02;
		break;
	case ':':
		glyph[2] = 0x36;
		break;
	case ';':
		glyph[1] = 0x50;
		glyph[2] = 0x36;
		break;
	case '<':
		glyph[0] = 0x08;
		glyph[1] = 0x14;
		glyph[2] = 0x22;
		break;
	case '=':
		glyph[1] = 0x14;
		glyph[2] = 0x14;
		glyph[3] = 0x14;
		break;
	case '>':
		glyph[2] = 0x22;
		glyph[3] = 0x14;
		glyph[4] = 0x08;
		break;
	case '?':
		glyph[0] = 0x02;
		glyph[1] = 0x01;
		glyph[2] = 0x51;
		glyph[3] = 0x09;
		glyph[4] = 0x06;
		break;
	case '@':
		glyph[0] = 0x3e;
		glyph[1] = 0x41;
		glyph[2] = 0x5d;
		glyph[3] = 0x55;
		glyph[4] = 0x1e;
		break;
	case '[':
		glyph[1] = 0x7f;
		glyph[2] = 0x41;
		glyph[3] = 0x41;
		break;
	case '\\':
		glyph[0] = 0x02;
		glyph[1] = 0x04;
		glyph[2] = 0x08;
		glyph[3] = 0x10;
		glyph[4] = 0x20;
		break;
	case ']':
		glyph[1] = 0x41;
		glyph[2] = 0x41;
		glyph[3] = 0x7f;
		break;
	case '^':
		glyph[1] = 0x02;
		glyph[2] = 0x01;
		glyph[3] = 0x02;
		break;
	case '_':
		memset(glyph, 0x40, 5);
		break;
	case '`':
		glyph[2] = 0x01;
		glyph[3] = 0x02;
		break;
	case '{':
		glyph[1] = 0x08;
		glyph[2] = 0x36;
		glyph[3] = 0x41;
		break;
	case '|':
		glyph[2] = 0x7f;
		break;
	case '}':
		glyph[1] = 0x41;
		glyph[2] = 0x36;
		glyph[3] = 0x08;
		break;
	case '~':
		glyph[0] = 0x08;
		glyph[1] = 0x04;
		glyph[2] = 0x08;
		glyph[3] = 0x10;
		glyph[4] = 0x08;
		break;
	case ' ':
		break;
	default:
		glyph[0] = 0x7f;
		glyph[1] = 0x41;
		glyph[2] = 0x41;
		glyph[3] = 0x41;
		glyph[4] = 0x7f;
		break;
	}
}

static void sde_console_clear_row(struct sde_console *sc, u32 row)
{
	u32 physical_row = (sc->top + row) % sc->rows;

	memset(sc->cells + physical_row * sc->cols, ' ', sc->cols);
}

static void sde_console_newline(struct sde_console *sc)
{
	sc->col = 0;
	if (sc->row + 1 < sc->rows) {
		sc->row++;
		sde_console_clear_row(sc, sc->row);
		return;
	}

	sc->top = (sc->top + 1) % sc->rows;
	sde_console_clear_row(sc, sc->row);
}

static void sde_console_put_char(struct sde_console *sc, u8 ch)
{
	u32 physical_row;

	if (sc->col >= sc->cols)
		sde_console_newline(sc);

	physical_row = (sc->top + sc->row) % sc->rows;
	sc->cells[physical_row * sc->cols + sc->col++] = ch;
}

static void sde_console_write(struct console *console, const char *text,
		unsigned int count)
{
	struct sde_console *sc = container_of(console, struct sde_console, console);
	unsigned long flags;
	unsigned int i;

	if (!READ_ONCE(sc->active))
		return;

	spin_lock_irqsave(&sc->lock, flags);
	for (i = 0; i < count; i++) {
		u8 ch = text[i];

		switch (ch) {
		case '\n':
			sde_console_newline(sc);
			break;
		case '\r':
			sc->col = 0;
			break;
		case '\b':
			if (sc->col)
				sc->col--;
			break;
		case '\t':
			do {
				sde_console_put_char(sc, ' ');
			} while (sc->col & 7);
			break;
		case '\f':
			memset(sc->cells, ' ', sc->cols * sc->rows);
			sc->top = 0;
			sc->row = 0;
			sc->col = 0;
			break;
		case '\0':
		case '\a':
			break;
		default:
			if (ch < ' ' || ch > '~')
				ch = '?';
			sde_console_put_char(sc, ch);
			break;
		}
	}
	spin_unlock_irqrestore(&sc->lock, flags);

	queue_work(system_unbound_wq, &sc->work);
}

static void sde_console_render(struct work_struct *work)
{
	struct sde_console *sc = container_of(work, struct sde_console, work);
	unsigned long flags;
	u32 row, col;

	mutex_lock(&sde_console_mutex);
	if (!sc->active) {
		mutex_unlock(&sde_console_mutex);
		return;
	}

	spin_lock_irqsave(&sc->lock, flags);
	for (row = 0; row < sc->rows; row++) {
		u32 physical_row = (sc->top + row) % sc->rows;

		memcpy(sc->snapshot + row * sc->cols,
			sc->cells + physical_row * sc->cols, sc->cols);
	}
	spin_unlock_irqrestore(&sc->lock, flags);

	for (row = 0; row < sc->height; row++) {
		u32 *pixels = (u32 *)((u8 *)sc->vaddr + row * sc->pitch);

		memset(pixels, 0, sc->width * sizeof(*pixels));
	}

	for (row = 0; row < sc->rows; row++) {
		for (col = 0; col < sc->cols; col++) {
			u8 glyph[5];
			u32 glyph_col, glyph_row;

			sde_console_get_glyph(sc->snapshot[row * sc->cols + col],
					glyph);
			for (glyph_col = 0; glyph_col < SDE_CONSOLE_GLYPH_WIDTH;
					glyph_col++) {
				u32 *pixels = (u32 *)((u8 *)sc->vaddr +
					(row * SDE_CONSOLE_FONT_HEIGHT) * sc->pitch) +
					col * SDE_CONSOLE_FONT_WIDTH + glyph_col;
				u32 source_col = glyph_col * ARRAY_SIZE(glyph) /
					SDE_CONSOLE_GLYPH_WIDTH;

				for (glyph_row = 0;
						glyph_row < SDE_CONSOLE_GLYPH_HEIGHT;
						glyph_row++) {
					u32 source_row = glyph_row * 7 /
						SDE_CONSOLE_GLYPH_HEIGHT;

					if (glyph[source_col] & BIT(source_row))
						pixels[glyph_row * sc->pitch /
							sizeof(*pixels)] = 0xffd0d0d0;
				}
			}
		}
	}

	__clean_dcache_area_poc(sc->vaddr, sc->pitch * sc->height);
	mutex_unlock(&sde_console_mutex);
}

static void sde_console_register(struct sde_console *sc)
{
	sc->active = true;
	sde_console = sc;
	sc->registered = true;
	strscpy(sc->console.name, "sdecon", sizeof(sc->console.name));
	sc->console.write = sde_console_write;
	sc->console.index = 0;
	sc->console.flags = CON_ENABLED | CON_PRINTBUFFER;
	register_console(&sc->console);
}

static int sde_console_alloc_text(struct sde_console *sc, u32 width, u32 height)
{
	sc->width = width;
	sc->height = height;
	sc->cols = width / SDE_CONSOLE_FONT_WIDTH;
	sc->rows = height / SDE_CONSOLE_FONT_HEIGHT;
	if (!sc->cols || !sc->rows)
		return -EINVAL;

	sc->cells = kvzalloc(sc->cols * sc->rows, GFP_KERNEL);
	sc->snapshot = kvzalloc(sc->cols * sc->rows, GFP_KERNEL);
	if (!sc->cells || !sc->snapshot)
		return -ENOMEM;

	memset(sc->cells, ' ', sc->cols * sc->rows);
	memset(sc->snapshot, ' ', sc->cols * sc->rows);
	spin_lock_init(&sc->lock);
	INIT_WORK(&sc->work, sde_console_render);
	return 0;
}

int sde_console_init_splash(struct drm_device *dev)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct sde_splash_display *splash_display = NULL;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	struct sde_splash_mem *splash;
	struct sde_console *sc;
	u32 width = 0, height = 0, pitch = 0;
	u32 source_addr = 0, bytes_per_pixel = 0;
	size_t source_offset;
	void *map_base, *vaddr;
	int i, ret = -ENODEV;

	if (!dev || !dev->dev_private)
		return -EINVAL;

	drm_modeset_lock_all(dev);
	mutex_lock(&sde_console_mutex);
	if (sde_console) {
		mutex_unlock(&sde_console_mutex);
		drm_modeset_unlock_all(dev);
		return -EBUSY;
	}

	priv = dev->dev_private;
	if (!priv->kms || !priv->kms->funcs ||
			!priv->kms->funcs->check_for_splash ||
			!priv->kms->funcs->check_for_splash(priv->kms)) {
		goto unlock;
	}

	sde_kms = to_sde_kms(priv->kms);
	for (i = 0; i < MAX_DSI_DISPLAYS; i++) {
		if (sde_kms->splash_data.splash_display[i].cont_splash_enabled) {
			splash_display = &sde_kms->splash_data.splash_display[i];
			break;
		}
	}
	if (!splash_display || !splash_display->splash)
		goto unlock;

	splash = splash_display->splash;
	if (!splash->splash_buf_base || !splash->splash_buf_size)
		goto unlock;

	drm_for_each_crtc(crtc, dev) {
		if (!crtc->state || !crtc->state->active)
			continue;

		drm_for_each_plane(plane, dev) {
			if (plane->state->crtc != crtc ||
					sde_plane_validate_src_addr(plane,
						splash->splash_buf_base,
						splash->splash_buf_size))
				continue;

			width = crtc->state->mode.hdisplay;
			height = crtc->state->mode.vdisplay;
			source_addr = sde_plane_get_source_addr(plane);
			pitch = sde_plane_get_source_stride(plane);
			bytes_per_pixel = sde_plane_get_source_bpp(plane);
			break;
		}
		if (width && height && pitch && source_addr && bytes_per_pixel)
			break;
	}

	if (!width || !height || !pitch || bytes_per_pixel != 4 ||
			pitch < width * bytes_per_pixel ||
			source_addr < splash->splash_buf_base) {
		ret = -EINVAL;
		goto unlock;
	}

	source_offset = source_addr - splash->splash_buf_base;
	if (source_offset + (u64)pitch * height > splash->splash_buf_size) {
		ret = -EINVAL;
		goto unlock;
	}

	map_base = memremap(splash->splash_buf_base, splash->splash_buf_size,
			MEMREMAP_WB);
	if (IS_ERR_OR_NULL(map_base)) {
		ret = map_base ? PTR_ERR(map_base) : -ENOMEM;
		goto unlock;
	}
	vaddr = (u8 *)map_base + source_offset;

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc) {
		memunmap(map_base);
		ret = -ENOMEM;
		goto unlock;
	}

	sc->dev = dev;
	sc->vaddr = vaddr;
	sc->map_base = map_base;
	sc->pitch = pitch;
	ret = sde_console_alloc_text(sc, width, height);
	if (ret) {
		kvfree(sc->snapshot);
		kvfree(sc->cells);
		kfree(sc);
		memunmap(map_base);
		goto unlock;
	}

	sde_console_register(sc);
	mutex_unlock(&sde_console_mutex);
	drm_modeset_unlock_all(dev);
	return 0;

unlock:
	mutex_unlock(&sde_console_mutex);
	drm_modeset_unlock_all(dev);
	return ret;
}

void sde_console_user_open(struct drm_device *dev)
{
	sde_console_fini(dev);
}

static void sde_console_disable(struct drm_device *dev)
{
	struct sde_console *sc;

	mutex_lock(&sde_console_mutex);
	sc = sde_console;
	if (!sc) {
		mutex_unlock(&sde_console_mutex);
		return;
	}
	if (sc->dev != dev) {
		mutex_unlock(&sde_console_mutex);
		return;
	}

	WRITE_ONCE(sc->active, false);
	if (sc->registered) {
		unregister_console(&sc->console);
		sc->registered = false;
	}
	mutex_unlock(&sde_console_mutex);
}

void sde_console_fini(struct drm_device *dev)
{
	struct sde_console *sc;

	sde_console_disable(dev);

	mutex_lock(&sde_console_mutex);
	sc = sde_console;
	if (!sc) {
		mutex_unlock(&sde_console_mutex);
		return;
	}
	if (sc->dev != dev) {
		mutex_unlock(&sde_console_mutex);
		return;
	}

	mutex_unlock(&sde_console_mutex);
	cancel_work_sync(&sc->work);
	mutex_lock(&sde_console_mutex);

	if (sde_console != sc || sc->dev != dev) {
		mutex_unlock(&sde_console_mutex);
		return;
	}

	memunmap(sc->map_base);
	kvfree(sc->snapshot);
	kvfree(sc->cells);
	sde_console = NULL;
	kfree(sc);
	mutex_unlock(&sde_console_mutex);
}
