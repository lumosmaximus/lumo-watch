/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watch, core 1 side: watch faces with their own small compositor (no LVGL),
 * built for a gliding sweep hand at the panel's refresh rate. Faces: a GMT
 * (rotating bezel, date) and a chronograph in black or white (fixed
 * tachymeter, subdials; PWR starts/stops, BOOT resets). Holding BOOT 3 s
 * opens a face picker.
 *
 * Per frame:
 *   1. wait for the panel's tearing-effect pulse (TE, GPIO13), so a frame is
 *      sent right as the panel starts a refresh: no tearing, steady pacing
 *   2. work out what moved: the second hand always, the other hands when
 *      their angle changes, the bezel when it was turned
 *   3. mark dirty "bands" (8 rows tall, only as wide as needed) covering the
 *      old and new positions, and redraw just those
 *
 * Each pixel of a band is composited from back to front: dial (RGB565 in
 * flash) or the date window, the rotated bezel ring, the GMT/hour/minute hand
 * images (rotated, bilinear), then the second hand, which is computed
 * analytically (anti-aliased line and circles at sub-pixel precision, plus a
 * soft shadow) so it moves without hopping between pixels.
 *
 * Time: core 0 sends the wall clock right as a second begins; we add
 * milliseconds from our own uptime. Touch: core 0 forwards it; dragging on
 * the bezel turns it in 6 degree clicks (the dial's minute track).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>
#include <esp_attr.h>
#include <soc/soc.h>
#include <soc/gpio_reg.h>

#include "../../common/watch_msg.h"
#include "watch_img.h"
#include "speed_img.h"
#include "knob_img.h"
#include "qspi_fast.h"

#define N      WATCH_SIZE
#define CX     233.0f   /* centre, in pixel-edge coordinates */
#define BAND   8        /* rows per dirty band */
#define NBANDS ((N + BAND - 1) / BAND)
#define TE_PIN 13       /* panel tearing-effect output (core 0 makes it an input) */

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* Two band buffers (big-endian RGB565, internal RAM, sent straight by DMA):
 * one is composed while the other goes out over SPI (see qspi_fast.c).
 */
static uint16_t bufs[2][BAND * N] __aligned(4);
static uint16_t *buf = bufs[0];
static int cur;
static bool fast_spi; /* after setup: our own register-level writes */
static bool qf_overlap = true; /* boxes go out while the next is composed */

/* ---- inputs from core 0 (written in the IPM interrupt) ------------------------- */

extern struct k_msgq buttons;
static struct msg_time base;
static volatile int64_t base_ms = -1;
static volatile struct msg_touch touch;
static volatile bool touch_new;

/* the computer's media state, from tools/agent through core 0 */
static volatile struct msg_state host = {-1, -1, -1, -1};
static volatile int64_t host_ms = -100000;
static volatile bool host_new;
/* now-playing text (title, artist) drawn by the helper app: 2-bit alpha */
static uint8_t np_img[TEXT_BYTES];
static volatile int64_t np_ms = -100000;
static volatile bool np_new;

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	if (id == MSG_TIME) {
		memcpy(&base, (const void *)data, sizeof(base));
		base_ms = k_uptime_get();
	} else if (id == MSG_TOUCH) {
		memcpy((void *)&touch, (const void *)data, sizeof(touch));
		touch_new = true;
	} else if (id == MSG_TEXT) {
		memcpy(np_img, (const void *)data, sizeof(np_img));
		np_ms = k_uptime_get();
		np_new = true;
	} else if (id == MSG_STATE) {
		memcpy((void *)&host, (const void *)data, sizeof(host));
		host_ms = k_uptime_get();
		host_new = true;
	} else if (id == MSG_BUTTON) {
		uint8_t b = ((volatile struct msg_button *)data)->btn;

		(void)k_msgq_put(&buttons, &b, K_NO_WAIT);
	}
}

/* ---- scene ---------------------------------------------------------------------- */

/* Faces: the GMT (Rolex style, rotating bezel, date) and the chronograph
 * (moon-watch style, fixed tachymeter, three subdials).
 */
enum { FACE_GMT, FACE_CHRONO, FACE_CHRONO_WHITE, FACE_REMOTE, NFACES };
/* FACE_REMOTE is the media remote: the bezel is a volume (or brightness) knob */
#define BOOT_FACE FACE_CHRONO_WHITE /* not remembered across resets yet */
static int face = FACE_GMT;

/* The sweeping hand (GMT: seconds; chronograph: chrono seconds), distances
 * along it from its pivot in pixels. dot = 0: no lume dot.
 */
struct sweep {
	float tip, tail, halfw, dot, cw, cwr, cwk, cap, capk;
	float loz;     /* centre of an open lozenge along the hand (0: none) */
	float redtip;  /* length of a red tip (0: none) */
	uint8_t r, g, b; /* hand colour */
	int mr; /* dirty-band radius: widest part + shadow + blur */
};
static const struct sweep sweep_gmt = {
	.tip = 196, .tail = -40, .halfw = 1.3f, .dot = 130, .cw = -32, .cwr = 6,
	.cwk = 1.0f / 13, .cap = 6.5f, .capk = 1.0f / 14, .r = 240, .g = 240, .b = 242, .mr = 17,
};
static const struct sweep sweep_chrono = {
	.tip = 192, .tail = -42, .halfw = 1.0f, .dot = 0, .cw = -30, .cwr = 4.5f,
	.cwk = 1.0f / 10, .cap = 5.5f, .capk = 1.0f / 12, .r = 240, .g = 240, .b = 242, .mr = 12,
};
static const struct sweep sweep_chrono_white = {
	.tip = 192, .tail = -42, .halfw = 1.0f, .dot = 0, .cw = -30, .cwr = 4.5f,
	.cwk = 1.0f / 10, .cap = 5.5f, .capk = 1.0f / 12, .loz = 150, .redtip = 26,
	.r = 22, .g = 22, .b = 24, .mr = 12,
};
static struct sweep sw = sweep_gmt;
static float sec_sn, sec_cs;
static int32_t sec_shown; /* the first frame draws it at 12 */

#define BEZEL_STEP 60     /* 6 degrees = one minute on the dial */
static int32_t bezel_rot, bezel_shown = -1;
/* bezel turn as an offset into the ring strip: angle steps x WATCH_RING_RAD */
static int32_t ring_shift;
/* the ring strip (palette indices) and palette, copied to internal RAM at boot:
 * a bezel click reads the strip all over, and random flash reads miss the cache
 */
static uint8_t ring_q[WATCH_RING_ANG * WATCH_RING_RAD];
static uint16_t ring_pal[256];
static const uint16_t *ring_palp = ring_pal;
/* sweep hand's screen bounding box at its current angle */
static int16_t swb_x0, swb_y0, swb_x1, swb_y1;

static int date_day, date_shown;

static void angle_sc(int32_t a, float *sn, float *cs)
{
	float r = a * (3.14159265f / 1800.0f);

	*sn = sinf(r);
	*cs = cosf(r);
}

/* ---- pixel helpers --------------------------------------------------------------- */

struct rgb {
	int r, g, b;
};

static ALWAYS_INLINE struct rgb from565(uint16_t v)
{
	return (struct rgb){(v >> 8) & 0xf8, (v >> 3) & 0xfc, (v << 3) & 0xf8};
}

static ALWAYS_INLINE void over(struct rgb *d, int r, int g, int b, int a)
{
	d->r += ((r - d->r) * a) >> 8;
	d->g += ((g - d->g) * a) >> 8;
	d->b += ((b - d->b) * a) >> 8;
}

/* floor for the small positive/negative values used here, without a libm call */
static ALWAYS_INLINE int ifloor(float v)
{
	return (int)(v + 1024.0f) - 1024;
}

static ALWAYS_INLINE float clamp01(float v)
{
	return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* inline min/max: fmaxf/fminf are library calls on this toolchain */
static ALWAYS_INLINE float fmx(float a, float b)
{
	return a > b ? a : b;
}

static ALWAYS_INLINE float fmn(float a, float b)
{
	return a < b ? a : b;
}

/* Anti-aliased disc coverage from the squared distance, without sqrt:
 * near the edge d^2 ~ r^2 + 2r(d - r), so coverage = 0.5 - (d - r) becomes
 * (r^2 + r + 0.25 - d^2) / (2r + 1), i.e. a linear ramp in d^2. k = 1/(2r+1).
 */
static ALWAYS_INLINE float disc(float d2, float r, float k)
{
	return clamp01((r * r + r + 0.25f - d2) * k);
}

/* Second hand coverage at (s along the hand, p across it). Returns alpha of
 * the shaft/counterweight (white), dot surround (gold) and lume.
 */
static ALWAYS_INLINE void sec_cover(float s, float p, float *a_white, float *a_gold, float *a_lume)
{
	float w;

	if (s >= sw.tail && s <= sw.tip) {
		w = clamp01(0.5f - (fabsf(p) - sw.halfw));
	} else {
		/* round end: disc of the shaft's half width at the end point */
		float e = s > sw.tip ? s - sw.tip : sw.tail - s;

		w = disc(e * e + p * p, sw.halfw, 1.0f / (2 * sw.halfw + 1));
	}
	*a_gold = *a_lume = 0;
	if (fabsf(s - sw.cw) < sw.cwr + 1.5f) { /* counterweight */
		w = fmx(w, disc((s - sw.cw) * (s - sw.cw) + p * p, sw.cwr, sw.cwk));
	}
	if (fabsf(s) < sw.cap + 1.5f && fabsf(p) < sw.cap + 1.5f) { /* centre cap */
		w = fmx(w, disc(s * s + p * p, sw.cap, sw.capk));
	}
	if (sw.dot && fabsf(s - sw.dot) < 9.5f && fabsf(p) < 9.5f) { /* lume dot + surround */
		float d2 = (s - sw.dot) * (s - sw.dot) + p * p;

		*a_gold = disc(d2, 8.0f, 1.0f / 17.0f);
		*a_lume = disc(d2, 5.5f, 1.0f / 12.0f);
	}
	if (sw.loz && fabsf(s - sw.loz) < 11 && fabsf(p) < 5) {
		/* open lozenge: outer diamond (half-lengths 10 x 4) minus inner (7 x 2.2);
		 * distance to a diamond edge ~ (|ds|/a + |p|/b - 1) * ab / sqrt(a^2 + b^2)
		 */
		float ds = fabsf(s - sw.loz), ap = fabsf(p);
		float outer = clamp01(0.5f - (ds / 10 + ap / 4 - 1) * 3.71f);
		float inner = clamp01(0.5f - (ds / 7 + ap / 2.2f - 1) * 2.10f);

		w = fmx(w, outer - inner);
	}
	*a_white = w;
}

/* ---- rendering one dirty box --------------------------------------------------- */

static uint32_t prof_px, prof_comp, prof_write, prof_boxes;

/* ---- vector hands ------------------------------------------------------------
 * Hand space: s along the hand from the centre (pixels), p across it. Shapes
 * match tools/watchface/watch.py: white-gold (GOLD) bodies, lume fills.
 */

/* Footprint regions per hand, for the per-row span solver: along-hand range
 * and half-width (including the shadow offset). A row is only shaded where
 * it crosses one of these.
 */
struct region {
	float s0, s1, reach;
};

static const struct region sec_regions[4] = {
	{-40, 196, 6}, {118, 142, 13.5f}, {-40, -24, 11.5f}, {-8, 8, 11.5f},
};
static const struct region chrono_regions[4] = {
	{-42, 192, 5}, {-36, -24, 9}, {-8, 8, 9.5f}, {0, 0, -1},
};
static const struct region chrono_white_regions[4] = {
	{-42, 192, 5}, {-36, -24, 9}, {-8, 8, 9.5f}, {138, 162, 8.5f},
};
static const struct region *sweep_rg = sec_regions;

/* hand shapes (see vhand_px) */
enum { SH_GMT, SH_HOUR, SH_MIN, SH_CH_HOUR, SH_CH_MIN, SH_CH_SUB };

static const struct region shape_regions[][4] = {
	[SH_GMT] = {{-10, 150, 6}, {144, 175, 18}, {-9, 9, 13}, {0, 0, -1}},
	[SH_HOUR] = {{-16, 56, 9}, {53, 91, 22}, {85, 109, 14}, {-10, 10, 14}},
	[SH_MIN] = {{-18, 173, 12}, {-10, 10, 14}, {0, 0, -1}, {0, 0, -1}},
	[SH_CH_HOUR] = {{-18, 104, 8}, {-9, 9, 10}, {0, 0, -1}, {0, 0, -1}},
	[SH_CH_MIN] = {{-20, 166, 7}, {-9, 9, 10}, {0, 0, -1}, {0, 0, -1}},
	[SH_CH_SUB] = {{-8, 42, 5}, {-6, 6, 6.5f}, {0, 0, -1}, {0, 0, -1}},
};

/* One hand: pivot, shape, extent, and the area marked dirty when it moves. */
struct hand {
	float px, py;           /* pivot, pixel-edge coordinates */
	uint8_t shape;
	float tail, tip, reach; /* bounding box */
	float mtail, mtip;      /* marked range along the hand ... */
	int mr;                 /* ... and radius around it */
	int32_t ang, shown;     /* 0.1 degree clockwise from 12; -1: never drawn */
	float sn, cs;
};

#define MAXH 5
static struct hand hands[MAXH]; /* back to front */
static int nhands;

static void lin_range(float a, float b, float t, float *lo, float *hi, float cx);

/* Per-row x interval [lo, hi] covering the given regions of a hand at (sn, cs)
 * pivoting at x = cx; ry is the row's y relative to the pivot.
 */
static void region_span(const struct region *rg, int n, float sn, float cs, float ry, int x0,
			int x1, int *a, int *b, float cx);

#define GOLD_R 205
#define GOLD_G 200
#define GOLD_B 190
#define LUME_R 236
#define LUME_G 240
#define LUME_B 222
/* chronograph hand colours (set per face) */
static struct rgb ch_body = {226, 228, 232}, ch_lume = {236, 240, 222};

/* coverage of a band |p| <= hw for s in [s0, s1] (flat ends) */
static ALWAYS_INLINE float bar(float s, float p, float s0, float s1, float hw)
{
	float d = fmx(fabsf(p) - hw, fmx(s0 - s, s - s1));

	return clamp01(0.5f - d);
}

/* coverage of a triangle pointing along +s: base half-width bw at s0, apex at s1 */
static ALWAYS_INLINE float tri(float s, float p, float s0, float s1, float bw)
{
	if (s < s0 - 1 || s > s1 + 1) {
		return 0;
	}
	float hw = bw * (s1 - s) / (s1 - s0);
	/* side distance, roughly perpendicular for these slender triangles */
	float d = fmx((fabsf(p) - hw) * 0.95f, s0 - s);

	return clamp01(0.5f - d);
}

/* Shade one pixel of vector hand k at (s, p); returns true if it changed. */
static ALWAYS_INLINE bool vhand_px(int k, float s, float p, struct rgb *c)
/* k: shape (SH_*) */
{
	float body = 0, lume = 0, red = 0, sh;
	bool steel = false;
	/* shadow: same outline 2.5 px left, 3 px up in screen space, i.e. a fixed
	 * offset; approximated in hand space by shifting s,p a little
	 */
	float ss = s - 1.5f, pp = p - 1.5f;

	switch (k) {
	case SH_GMT: /* GMT: thin shaft, arrow with red insert, hub */
		body = fmx(bar(s, p, -10, 150, 1.5f), tri(s, p, 146, 174, 13));
		red = tri(s, p, 148.5f, 167, 9);
		body = fmx(body, disc(s * s + p * p, 8, 1.0f / 17));
		sh = fmx(bar(ss, pp, -10, 150, 1.5f), tri(ss, pp, 146, 174, 13));
		break;
	case SH_HOUR: { /* hour: shaft, Mercedes circle (ring, lume, 3 spokes), tip */
		float dc2 = (s - 72) * (s - 72) + p * p;
		float ring = disc(dc2, 17, 1.0f / 35);

		body = fmx(bar(s, p, -16, 56, 4), ring);
		body = fmx(body, tri(s, p, 86, 108, 9));
		body = fmx(body, disc(s * s + p * p, 9, 1.0f / 19));
		lume = disc(dc2, 13, 1.0f / 27);
		/* spokes cut through the lume: angles 90, 210, 330 deg in hand space */
		if (lume > 0) {
			float ds = s - 72;
			float sp1 = clamp01(0.5f - (fabsf(p) - 1.5f));          /* along s */
			float sp2 = clamp01(0.5f - (fabsf(-0.866f * p - 0.5f * ds) - 1.5f));
			float sp3 = clamp01(0.5f - (fabsf(0.866f * p - 0.5f * ds) - 1.5f));
			float spokes = fmx(ds < 0 ? sp1 : 0, fmx(ds > 0 ? fmx(sp2, sp3) : 0, 0));

			lume *= 1 - spokes;
		}
		lume = fmx(lume, tri(s, p, 89, 101, 5.5f));
		sh = fmx(bar(ss, pp, -16, 56, 4), disc((ss - 72) * (ss - 72) + pp * pp, 17, 1.0f / 35));
		break;
	}
	case SH_MIN: /* minute: sword (shaft + tip), lume stripe, hub */
		body = fmx(bar(s, p, -18, 150, 7), tri(s, p, 150, 172, 7));
		body = fmx(body, disc(s * s + p * p, 9, 1.0f / 19));
		lume = fmx(bar(s, p, 30, 148, 3.5f), tri(s, p, 148, 162, 3.5f));
		sh = fmx(bar(ss, pp, -18, 150, 7), tri(ss, pp, 150, 172, 7));
		break;
	case SH_CH_HOUR: /* chronograph hour: steel baton, lume stripe, hub */
		steel = true;
		body = fmx(bar(s, p, -18, 104, 4.5f), disc(s * s + p * p, 7, 1.0f / 15));
		lume = bar(s, p, 28, 100, 2.2f);
		sh = bar(ss, pp, -18, 104, 4.5f);
		break;
	case SH_CH_MIN: /* chronograph minute: longer, slimmer baton */
		steel = true;
		body = fmx(bar(s, p, -20, 166, 3.5f), disc(s * s + p * p, 7, 1.0f / 15));
		lume = bar(s, p, 28, 162, 1.6f);
		sh = bar(ss, pp, -20, 166, 3.5f);
		break;
	default: /* SH_CH_SUB: subdial needle */
		steel = true;
		body = fmx(bar(s, p, -8, 42, 1.2f), disc(s * s + p * p, 3.5f, 1.0f / 8));
		sh = bar(ss, pp, -8, 42, 1.2f);
		break;
	}
	if (sh <= 0 && body <= 0) {
		return false;
	}
	if (sh > 0) {
		over(c, 0, 0, 0, (int)(sh * 120));
	}
	if (body > 0) {
		if (steel) {
			over(c, ch_body.r, ch_body.g, ch_body.b, (int)(body * 255));
		} else {
			over(c, GOLD_R, GOLD_G, GOLD_B, (int)(body * 255));
		}
	}
	if (lume > 0) {
		if (steel) {
			over(c, ch_lume.r, ch_lume.g, ch_lume.b, (int)(lume * 255));
		} else {
			over(c, LUME_R, LUME_G, LUME_B, (int)(lume * 255));
		}
	}
	if (red > 0) {
		over(c, 214, 36, 44, (int)(red * 255));
	}
	return true;
}
static uint32_t st_c[5], st_secpx;
#define ST(i, t) (st_c[i] += k_cycle_get_32() - (t), (t) = k_cycle_get_32())

/* Screen-space bounding boxes of the image hands at their current angle. */
static int16_t hb_x0[MAXH], hb_y0[MAXH], hb_x1[MAXH], hb_y1[MAXH];

/* Sweep hand's bounding box (pivot at the centre), widest part plus shadow. */
static void sweep_bbox(void)
{
	float r = sw.mr + 2;
	float xa = sw.tail * sec_sn, xb = sw.tip * sec_sn;
	float ya = -sw.tail * sec_cs, yb = -sw.tip * sec_cs;

	swb_x0 = (int16_t)(CX + fmn(xa, xb) - r);
	swb_x1 = (int16_t)(CX + fmx(xa, xb) + r);
	swb_y0 = (int16_t)(CX + fmn(ya, yb) - r);
	swb_y1 = (int16_t)(CX + fmx(ya, yb) + r);
}

static void hand_bbox(int k)
{
	const struct hand *hd = &hands[k];
	float r = hd->reach + 6;
	float ex[2] = {hd->tail, hd->tip};
	float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;

	for (int i = 0; i < 2; i++) {
		float X = ex[i] * hd->sn, Y = -ex[i] * hd->cs;

		mnx = fmn(mnx, X);
		mxx = fmx(mxx, X);
		mny = fmn(mny, Y);
		mxy = fmx(mxy, Y);
	}
	hb_x0[k] = (int16_t)(hd->px + mnx - r);
	hb_x1[k] = (int16_t)(hd->px + mxx + r);
	hb_y0[k] = (int16_t)(hd->py + mny - r);
	hb_y1[k] = (int16_t)(hd->py + mxy + r);
}

static void region_span(const struct region *rg, int n, float sn, float cs, float ry, int x0,
			int x1, int *a, int *b, float cx)
{
	float lo_all = 1e9f, hi_all = -1e9f;

	for (int i = 0; i < n; i++) {
		if (rg[i].reach < 0) {
			continue;
		}
		float lo = x0, hi = x1;
		float mid = (rg[i].s0 + rg[i].s1) / 2, half = (rg[i].s1 - rg[i].s0) / 2 + 2;

		lin_range(cs, ry * sn, rg[i].reach, &lo, &hi, cx);
		lin_range(sn, -ry * cs - mid, half, &lo, &hi, cx);
		if (hi >= lo) {
			lo_all = fmn(lo_all, lo);
			hi_all = fmx(hi_all, hi);
		}
	}
	*a = MAX(x0, (int)floorf(lo_all));
	*b = MIN(x1, (int)ceilf(hi_all));
}

/* x range (pixel indices, inclusive) where |a*X + b| < t, X = x + 0.5 - cx */
static void lin_range(float a, float b, float t, float *lo, float *hi, float cx)
{
	if (fabsf(a) < 1e-5f) {
		if (fabsf(b) >= t) {
			*lo = 1e9f;
			*hi = -1e9f;
		}
		return; /* leaves [lo, hi] as is */
	}
	float r0 = (-t - b) / a, r1 = (t - b) / a;

	if (r0 > r1) {
		float tt = r0;

		r0 = r1;
		r1 = tt;
	}
	*lo = fmx(*lo, r0 + cx - 0.5f);
	*hi = fmn(*hi, r1 + cx - 0.5f);
}

static inline uint16_t to565(const struct rgb *c)
{
	return ((c->r & 0xf8) << 8) | ((c->g & 0xfc) << 3) | (c->b >> 3);
}

static void remote_row(uint16_t *o, int x0, int x1, int y);

static void IRAM_ATTR render_box(int x0, int y0, int x1, int y1)
{
	uint32_t pt0 = k_cycle_get_32();
	const bool gmt_face = face == FACE_GMT;
	const uint16_t *dial = (const uint16_t *)(gmt_face ? watch_dial :
						  face == FACE_CHRONO ? speed_dial : speed_white_dial);
	const int w = x1 - x0 + 1, h = y1 - y0 + 1;
	const int dx0 = WATCH_DATE_CX - WATCH_DATE_W / 2, dy0 = WATCH_DATE_CY - WATCH_DATE_H / 2;
	const uint16_t *date = gmt_face && date_day ? (const uint16_t *)watch_dates +
					  (date_day - 1) * WATCH_DATE_W * WATCH_DATE_H : NULL;
	bool hand_on[MAXH];
	/* does this box reach the bezel ring at all? (corners vs radii) */
	float fxm = fmx(fabsf(x0 - CX), fabsf(x1 + 1 - CX)), fym = fmx(fabsf(y0 - CX), fabsf(y1 + 1 - CX));
	const bool remote = face == FACE_REMOTE;
	bool ring = (gmt_face || remote) &&
		    fxm * fxm + fym * fym > (WATCH_BEZEL_IN - 1) * (WATCH_BEZEL_IN - 1);
	const bool sweep_on = !remote && swb_x1 >= x0 && swb_x0 <= x1 && swb_y1 >= y0 &&
			      swb_y0 <= y1;
	/* plain-shaft stretch of the sweep hand ends at its lume dot, if any */
	const float d0 = sw.dot ? sw.dot : (sw.loz ? sw.loz : sw.tip + 10);

	for (int k = 0; k < nhands; k++) {
		hand_on[k] = hb_x1[k] >= x0 && hb_x0[k] <= x1 && hb_y1[k] >= y0 && hb_y0[k] <= y1;
	}

	for (int y = y0; y <= y1; y++) {
		uint16_t *o = &buf[(y - y0) * w];
		const uint16_t *drow = &dial[y * N];
		const float ry = y + 0.5f - CX;
		uint32_t tt = k_cycle_get_32();

		/* 1. background: the dial (or the date window), straight copy; not
		 *    under the bezel ring, which covers it completely
		 */
		if (ring) {
			/* outside the outermost span is off the round panel: black */
			const int16_t *sp = (const int16_t *)watch_ring_spans + y * 4;
			int nsp = (sp[1] >= sp[0]) + (sp[3] >= sp[2]);
			int lo = nsp ? sp[0] : N, hi = nsp ? sp[nsp * 2 - 1] : -1;
			int x = x0;

			if (x < lo) {
				int e = MIN(x1, lo - 1);

				memset(&o[0], 0, (e - x0 + 1) * 2);
				x = e + 1;
			}
			if (nsp == 2 && x <= x1) { /* between the spans: the dial */
				int a = MAX(x, sp[1] + 1), b = MIN(x1, sp[2] - 1);

				if (b >= a) {
					if (remote) {
						remote_row(&o[a - x0], a, b, y);
					} else {
						memcpy(&o[a - x0], &drow[a], (b - a + 1) * 2);
					}
				}
			}
			if (hi < x1) {
				int a = MAX(x0, hi + 1);

				memset(&o[a - x0], 0, (x1 - a + 1) * 2);
			}
		} else {
			if (remote) {
				remote_row(o, x0, x1, y);
			} else {
				memcpy(o, &drow[x0], w * 2);
			}
		}
		if (date && y >= dy0 && y < dy0 + WATCH_DATE_H && x1 >= dx0 &&
		    x0 < dx0 + WATCH_DATE_W) {
			int a = MAX(x0, dx0), b = MIN(x1, dx0 + WATCH_DATE_W - 1);

			memcpy(&o[a - x0], &date[(y - dy0) * WATCH_DATE_W + (a - dx0)], (b - a + 1) * 2);
		}

		ST(0, tt);
		/* 2. bezel ring via the polar lookup: each ring pixel has a precomputed
		 *    index into the unwrapped ring strip (angle-major, so neighbours
		 *    on a row read neighbouring entries); turning the bezel just
		 *    offsets the index
		 */
		if (ring) {
			const int16_t *sp = (const int16_t *)watch_ring_spans + y * 4;
			uint32_t idx = ((const uint32_t *)watch_ring_row_off)[y];
			const uint16_t *lut = (const uint16_t *)watch_ring_idx;
			const int32_t total = WATCH_RING_ANG * WATCH_RING_RAD;

			for (int k = 0; k < 2; k++, sp += 2) {
				int sa = sp[0], sb = sp[1];

				if (sb < sa) {
					continue;
				}
				int a = MAX(x0, sa), b = MIN(x1, sb);
				const uint16_t *li = &lut[idx + (a - sa)];
				uint16_t *op = &o[a - x0];

				/* entries are pre-blended onto what is beneath: plain copy */
				for (int x = a; x <= b; x++) {
					int32_t t = (int32_t)*li++ - ring_shift;

					if (t < 0) {
						t += total;
					}
					*op++ = ring_palp[ring_q[t]];
				}
				idx += sb - sa + 1;
			}
		}

		ST(1, tt);
		/* 3. the other hands, drawn as vector shapes (see vhand_px) */
		for (int k = 0; k < nhands; k++) {
			const struct hand *hd = &hands[k];

			if (!hand_on[k] || y < hb_y0[k] || y > hb_y1[k]) {
				continue;
			}
			/* where this row crosses the hand's footprint (plus shadow):
			 * p = X cs + ry sn within +-reach, s = X sn - ry cs within the length
			 */
			int a, b;
			const float hy = y + 0.5f - hd->py; /* row, relative to the pivot */

			region_span(shape_regions[hd->shape], 4, hd->sn, hd->cs, hy, x0, x1, &a, &b,
				    hd->px);
			if (b < a) {
				continue;
			}
			float rx = a + 0.5f - hd->px;
			float hs = rx * hd->sn - hy * hd->cs, hp = rx * hd->cs + hy * hd->sn;

			for (int x = a; x <= b; x++) {
				struct rgb c = from565(o[x - x0]);

				if (vhand_px(hd->shape, hs, hp, &c)) {
					o[x - x0] = to565(&c);
				}
				hs += hd->sn;
				hp += hd->cs;
			}
		}
		ST(2, tt);
		/* 4. second hand + shadow: solve where they cross this row
		 *    p (across) = X cs + ry sn, s (along) = X sn - ry cs, X = x + 0.5 - CX
		 */
		int a = 1, b = 0;

		if (sweep_on && y >= swb_y0 && y <= swb_y1) {
			region_span(sweep_rg, 4, sec_sn, sec_cs, ry, x0, x1, &a, &b, CX);
		}
		float rx = a + 0.5f - CX;
		float ss = rx * sec_sn - ry * sec_cs, sp = rx * sec_cs + ry * sec_sn;
		/* shadow sample point: 2.5 px left, 3 px up */
		const float dss = -2.5f * sec_sn + 3.0f * sec_cs, dsp = -2.5f * sec_cs - 3.0f * sec_sn;

		for (int x = a; x <= b; x++, ss += sec_sn, sp += sec_cs) {
			float hs = ss + dss, hp = sp + dsp;
			/* plain shaft (away from dot, counterweight, cap): cheap path */
			bool plain = (ss > 12 && ss < d0 - 12 && hs > 12 && hs < d0 - 12) ||
				     (ss > d0 + 12 && ss < sw.tip - 2 && hs > d0 + 12 &&
				      hs < sw.tip - 2);
			float aw, ag = 0, al = 0, sh;

			if (plain) {
				if (fabsf(hp) > 3.0f && fabsf(sp) > 2.0f) {
					continue; /* neither hand nor shadow here */
				}
				sh = clamp01(0.5f - (fabsf(hp) - sw.halfw));
				aw = clamp01(0.5f - (fabsf(sp) - sw.halfw));
			} else {
				float g2, l2;

				sec_cover(hs, hp, &sh, &g2, &l2);
				sh = fmx(sh, g2);
				sec_cover(ss, sp, &aw, &ag, &al);
			}
			if (sh <= 0 && aw <= 0 && ag <= 0) {
				continue;
			}
			struct rgb c = from565(o[x - x0]);

			if (sh > 0) {
				over(&c, 0, 0, 0, (int)(sh * 110));
			}
			if (ag > 0) {
				over(&c, 205, 200, 190, (int)(ag * 255));
			}
			if (al > 0) {
				over(&c, 236, 240, 222, (int)(al * 255));
			}
			if (aw > 0) {
				if (sw.redtip && ss > sw.tip - sw.redtip) {
					over(&c, 200, 30, 36, (int)(aw * 255));
				} else {
					over(&c, sw.r, sw.g, sw.b, (int)(aw * 255));
				}
			}
			o[x - x0] = to565(&c);
		}
		st_secpx += b >= a ? b - a + 1 : 0;
		ST(3, tt);
		/* big-endian on the wire */
		for (int x = 0; x < w; x++) {
			o[x] = (o[x] >> 8) | (o[x] << 8);
		}
		ST(4, tt);
	}
	struct display_buffer_descriptor desc = {
		.buf_size = w * h * 2, .width = w, .height = h, .pitch = w,
	};
	uint32_t pt1 = k_cycle_get_32();

	if (fast_spi) {
		qf_write(x0, y0, w, h, buf); /* returns as soon as the DMA runs */
		if (!qf_overlap) {
			qf_wait();
		}
		cur ^= 1;
		buf = bufs[cur]; /* its previous transfer finished in qf_write */
	} else {
		display_write(disp, x0, y0, &desc, buf);
	}
	prof_comp += pt1 - pt0;
	prof_write += k_cycle_get_32() - pt1;
	prof_px += w * h;
	prof_boxes++;
}

/* ---- dirty bands ---------------------------------------------------------------- */

static int16_t band_x0[NBANDS], band_x1[NBANDS];

static void bands_clear(void)
{
	for (int i = 0; i < NBANDS; i++) {
		band_x0[i] = N;
		band_x1[i] = -1;
	}
}

static void mark_point(float x, float y, int r)
{
	int b0 = MAX(0, (int)(y - r) / BAND), b1 = MIN(NBANDS - 1, (int)(y + r) / BAND);

	for (int b = b0; b <= b1; b++) {
		band_x0[b] = MIN(band_x0[b], (int16_t)MAX(0, (int)(x - r)));
		band_x1[b] = MAX(band_x1[b], (int16_t)MIN(N - 1, (int)(x + r)));
	}
}

/* Mark the area a hand covers at angle (sn, cs) from tail to tip, radius r. */
static void mark_hand(float px, float py, float sn, float cs, float tail, float tip, int r)
{
	for (float s = tail; s <= tip; s += 3.0f) {
		mark_point(px + s * sn, py - s * cs, r);
	}
	mark_point(px + tip * sn, py - tip * cs, r);
}

static void mark_rect(int x0, int y0, int x1, int y1)
{
	for (int b = y0 / BAND; b <= y1 / BAND && b < NBANDS; b++) {
		band_x0[b] = MIN(band_x0[b], x0);
		band_x1[b] = MAX(band_x1[b], x1);
	}
}

static int render_bands(void)
{
	int n = 0;

	for (int b = 0; b < NBANDS; b++) {
		if (band_x1[b] < band_x0[b]) {
			continue;
		}
		/* the CO5300 wants windows starting on an even pixel, ending on odd */
		int x0 = band_x0[b] & ~1, x1 = band_x1[b] | 1;

		render_box(x0, b * BAND, MIN(x1, N - 1), MIN(b * BAND + BAND - 1, N - 1));
		n++;
	}
	return n;
}

/* The bezel ring, band by band: two spans per band where the dial is inside. */
static void render_ring(void)
{
	for (int b = 0; b < NBANDS; b++) {
		int y0 = b * BAND, y1 = MIN(y0 + BAND - 1, N - 1);
		/* widest outer-circle span and narrowest inner-circle span in this band */
		float ny = fmn(fabsf(y0 - CX), fabsf(y1 + 1 - CX));
		float fy = fmx(fabsf(y0 - CX), fabsf(y1 + 1 - CX));

		if (y0 <= CX && y1 + 1 >= CX) {
			ny = 0;
		}
		float ro = WATCH_BEZEL_OUT, ri = WATCH_BEZEL_IN - 1;

		if (ny >= ro) {
			continue;
		}
		int ho = (int)ceilf(sqrtf(ro * ro - ny * ny));

		if (fy < ri) {
			int hi = (int)floorf(sqrtf(ri * ri - fy * fy));

			render_box(MAX(((int)CX - ho - 1) & ~1, 0), y0, ((int)CX - hi + 1) | 1, y1);
			render_box(((int)CX + hi - 1) & ~1, y0, MIN(((int)CX + ho + 1) | 1, N - 1), y1);
		} else {
			render_box(MAX(((int)CX - ho - 1) & ~1, 0), y0,
				   MIN(((int)CX + ho + 1) | 1, N - 1), y1);
		}
	}
}

/* ---- touch: turning the bezel ----------------------------------------------------- */

static void bezel_touch(void)
{
	static bool dragging;
	static int32_t grab_angle, grab_rot;

	if (!touch_new) {
		return;
	}
	touch_new = false;
	float dx = touch.x + 0.5f - CX, dy = touch.y + 0.5f - CX;
	int32_t ang = (int32_t)(atan2f(dx, -dy) * (1800.0f / 3.14159265f)); /* clock angle */

	if (!touch.down) {
		dragging = false;
		return;
	}
	if (!dragging) {
		if (dx * dx + dy * dy < 165.0f * 165.0f) {
			return; /* grabs only on the ring */
		}
		dragging = true;
		grab_angle = ang;
		grab_rot = bezel_rot;
		return;
	}
	int32_t delta = ang - grab_angle;

	if (delta > 1800) {
		delta -= 3600;
	} else if (delta < -1800) {
		delta += 3600;
	}
	int32_t rot = grab_rot + delta;

	/* snap to the nearest minute click */
	rot = (int32_t)lroundf((float)rot / BEZEL_STEP) * BEZEL_STEP;
	bezel_rot = ((rot % 3600) + 3600) % 3600;
}

/* ---- frame pacing ------------------------------------------------------------------ */

static inline bool te_high(void)
{
	return REG_READ(GPIO_IN_REG) & BIT(TE_PIN);
}

/* Wait for the start of the next panel refresh (TE rising edge). */
static void wait_te(bool have_te)
{
	if (!have_te) {
		k_msleep(16);
		return;
	}
	uint32_t t0 = k_uptime_get_32();

	while (te_high() && k_uptime_get_32() - t0 < 25) {
	}
	while (!te_high() && k_uptime_get_32() - t0 < 25) {
	}
}

static bool detect_te(void)
{
	int edges = 0;
	bool last = te_high();
	uint32_t t0 = k_uptime_get_32();

	while (k_uptime_get_32() - t0 < 100) {
		bool now = te_high();

		edges += now && !last;
		last = now;
	}
	return edges >= 3; /* ~6 at 60 Hz */
}

/* ---- main ---------------------------------------------------------------------------- */

/* ---- faces ------------------------------------------------------------------------- */

#define SUB_OFF SPEED_SUB_OFF

static void face_setup(int f)
{
	face = f;
	memset(hands, 0, sizeof(hands));
	if (f == FACE_REMOTE) {
		nhands = 0; /* no hands: the bezel knob and buttons only */
		return;
	}
	if (f == FACE_GMT) {
		nhands = 3;
		/* the marked range comes from the hand images these shapes replaced */
		hands[0] = (struct hand){.px = CX, .py = CX, .shape = SH_GMT, .tail = -10,
					 .tip = 176, .reach = 14, .mtail = -26, .mtip = 182,
					 .mr = 26};
		hands[1] = (struct hand){.px = CX, .py = CX, .shape = SH_HOUR, .tail = -16,
					 .tip = 110, .reach = 18, .mtail = -26, .mtip = 122,
					 .mr = 30};
		hands[2] = (struct hand){.px = CX, .py = CX, .shape = SH_MIN, .tail = -18,
					 .tip = 174, .reach = 9, .mtail = -26, .mtip = 182,
					 .mr = 23};
		sw = sweep_gmt;
		sweep_rg = sec_regions;
	} else {
		const struct hand sub = {.shape = SH_CH_SUB, .tail = -8, .tip = 42, .reach = 6,
					 .mtail = -12, .mtip = 46, .mr = 11};

		nhands = 5;
		hands[0] = sub; /* running seconds, at 9 */
		hands[0].px = CX - SUB_OFF;
		hands[0].py = CX;
		hands[1] = sub; /* 30-minute counter, at 3 */
		hands[1].px = CX + SUB_OFF;
		hands[1].py = CX;
		hands[2] = sub; /* 12-hour counter, at 6 */
		hands[2].px = CX;
		hands[2].py = CX + SUB_OFF;
		hands[3] = (struct hand){.px = CX, .py = CX, .shape = SH_CH_HOUR, .tail = -18,
					 .tip = 104, .reach = 10, .mtail = -22, .mtip = 108,
					 .mr = 16};
		hands[4] = (struct hand){.px = CX, .py = CX, .shape = SH_CH_MIN, .tail = -20,
					 .tip = 166, .reach = 10, .mtail = -24, .mtip = 170,
					 .mr = 15};
		if (f == FACE_CHRONO) {
			sw = sweep_chrono;
			sweep_rg = chrono_regions;
			ch_body = (struct rgb){226, 228, 232};
			ch_lume = (struct rgb){236, 240, 222};
		} else { /* white dial: black hands with cream lume, red-tipped chrono */
			sw = sweep_chrono_white;
			sweep_rg = chrono_white_regions;
			ch_body = (struct rgb){22, 22, 24};
			ch_lume = (struct rgb){238, 240, 226};
		}
	}
}

/* chronograph: PWR starts/stops, BOOT (short) resets */
static bool chrono_run;
static int64_t chrono_t0, chrono_acc;

static int64_t chrono_ms(int64_t now)
{
	return chrono_acc + (chrono_run ? now - chrono_t0 : 0);
}

/* Angles (0.1 degree) of every hand and of the sweep hand, for this face. */
static int32_t face_angles(int64_t day_ms, int64_t now)
{
	int32_t hour_a = (int32_t)((day_ms % 43200000) * 3600 / 43200000);
	int32_t min_a = (int32_t)((day_ms % 3600000) * 3600 / 3600000);

	if (face == FACE_REMOTE) {
		return 0;
	}
	if (face == FACE_GMT) {
		hands[0].ang = (int32_t)(day_ms * 3600 / 86400000);
		hands[1].ang = hour_a;
		hands[2].ang = min_a;
		return (int32_t)((day_ms % 60000) * 3600 / 60000);
	}
	int64_t el = chrono_ms(now);

	hands[0].ang = (int32_t)((day_ms % 60000) * 3600 / 60000);
	hands[1].ang = (int32_t)((el / 60000) % 30 * 120); /* jumps each minute */
	hands[2].ang = (int32_t)((el % 43200000) * 3600 / 43200000);
	hands[3].ang = hour_a;
	hands[4].ang = min_a;
	return (int32_t)((el % 60000) * 3600 / 60000);
}

static int64_t day_ms_now(void)
{
	if (base_ms < 0) {
		return 0;
	}
	int64_t ms = k_uptime_get() - base_ms;

	return (((int64_t)base.hour * 3600 + base.min * 60 + base.sec) * 1000 + ms) %
	       (24LL * 3600 * 1000);
}

static int32_t rm_steps(void);

/* Redraw the whole screen with every hand at its current angle. */
static void draw_full(void)
{
	int64_t now = k_uptime_get();
	int32_t sa = face_angles(day_ms_now(), now);

	for (int k = 0; k < nhands; k++) {
		angle_sc(hands[k].ang, &hands[k].sn, &hands[k].cs);
		hand_bbox(k);
		hands[k].shown = hands[k].ang;
	}
	angle_sc(sa, &sec_sn, &sec_cs);
	sweep_bbox();
	sec_shown = sa;
	if (face == FACE_REMOTE) {
		ring_shift = rm_steps() * WATCH_RING_RAD;
	} else {
		ring_shift = bezel_rot * WATCH_RING_ANG / 3600 * WATCH_RING_RAD;
	}
	bezel_shown = bezel_rot;
	date_day = base_ms >= 0 ? base.day : 0;
	date_shown = date_day;
	for (int y = 0; y < N; y += BAND) {
		render_box(0, y, N - 1, MIN(y + BAND - 1, N - 1));
	}
	qf_wait();
}

/* ---- the media remote --------------------------------------------------------------
 * A knob ring (tools/watchface/knob.py, drawn with the bezel's lookup) follows
 * the finger in 0.25 degree steps and sends one key per 15 degrees (its long
 * ticks), so like an iPod wheel it takes a couple of turns from silent to full. Clockwise is up. Buttons:
 * mute (top), previous / next (left / right), play/pause (centre) and a sun
 * (bottom) that switches the knob to brightness (amber) until pressed again or
 * 30 s without touch. Open/close: hold two fingers (or one, still, in the
 * middle). PWR plays/pauses, BOOT switches the mode.
 */

#define RM_IDLE_MS  30000

enum { RB_PLAY, RB_MUTE, RB_PREV, RB_NEXT, RB_SUN, RB_N };
static const struct {
	int16_t x, y, r;
	uint8_t key;
	uint8_t cr, cg, cb; /* pastel colour */
} rm_btn[RB_N] = {
	[RB_PLAY] = {0, 0, 50, KEY_PLAY_PAUSE, 255, 150, 190},  /* pink */
	[RB_MUTE] = {0, -104, 28, KEY_MUTE, 130, 225, 195},     /* mint */
	[RB_PREV] = {-104, 0, 28, KEY_PREV, 178, 160, 255},     /* lavender */
	[RB_NEXT] = {104, 0, 28, KEY_NEXT, 178, 160, 255},
	[RB_SUN] = {0, 148, 22, 0, 255, 212, 110},              /* sunny */
};
static struct rgb rm_vis_col[40]; /* a rainbow, one colour per visualiser bar */
/* the now-playing text, below the play button (from the middle) */
#define RM_TEXT_X0 (-TEXT_W / 2)
#define RM_TEXT_Y0 80
static bool rm_text_on;

static uint16_t knob_pal[256], knob_pal_amber[256];
static bool rm_bright, rm_active, rm_drag, rm_moved;
static bool rm_muted; /* our own idea: mute toggles it, a volume key clears it */
static bool rm_playing; /* likewise: play/pause toggles it (starts as paused) */
/* level arc (just inside the knob): the computer's volume or brightness,
 * 0..100, or -1 when no helper app is reporting it
 */
#define RM_ARC_R  186.5f
static int rm_level = -1;
static int rm_level_now(void);
/* A visualiser around the play button: RM_VIS_N bars from radius RM_VIS_R0,
 * up to RM_VIS_H long, bouncing while playing and fading away when paused.
 * (Made up on the watch for now; a companion app could send real levels.)
 */
#define RM_VIS_N  40
#define RM_VIS_R0 57.0f
#define RM_VIS_H  17.0f
static float rm_vis[RM_VIS_N]; /* bar lengths, px */
static float rm_pos;  /* knob angle, 0.1 degree, unwrapped */
static int rm_pressed = -1;
static int64_t rm_last_touch, rm_t0, rm_press_until;
static int32_t rm_last_ang, rm_saved_bezel;
static float rm_acc;
static int rm_x0, rm_y0, rm_prev_face = BOOT_FACE;

/* icon coverage (white) for button b, at (X, Y) relative to its centre */
static float rm_icon(int b, float X, float Y)
{
	switch (b) {
	case RB_PLAY: /* shows what a tap does: pause while playing, play while paused */
		if (rm_playing) {
			return fmx(bar(X, Y, -11, -3.5f, 15), bar(X, Y, 3.5f, 11, 15));
		}
		return tri(X, Y, -10, 16, 17);
	case RB_PREV: /* |<< */
		return fmx(bar(X, Y, -12, -9.5f, 8), fmx(tri(-X, Y, -1, 9, 8), tri(-X, Y, -11, -1, 8)));
	case RB_NEXT: /* >>| */
		return fmx(bar(X, Y, 9.5f, 12, 8), fmx(tri(X, Y, -1, 9, 8), tri(X, Y, -11, -1, 8)));
	case RB_MUTE: { /* speaker with a cross */
		float body = bar(X, Y, -13, -7, 4.5f);
		float hw = 4.5f + (X + 7) * 0.9f; /* cone widens to the right */
		float cone = (X > -7.5f && X < 1.5f) ? clamp01(0.5f - (fabsf(Y) - hw)) : 0;
		float spk = fmx(body, cone);

		if (rm_muted) {
			float u = (X - 9) * 0.7071f, v = Y * 0.7071f;

			return fmx(spk, fmx(bar(u + v, u - v, -6, 6, 1.3f), bar(u - v, u + v, -6, 6, 1.3f)));
		}
		float wx = X + 2, wr = sqrtf(wx * wx + Y * Y);

		if (wx > fabsf(Y) * 0.9f) { /* two sound waves */
			spk = fmx(spk, clamp01(0.5f - (fabsf(wr - 8) - 1.1f)));
			spk = fmx(spk, clamp01(0.5f - (fabsf(wr - 13) - 1.1f)));
		}
		return spk;
	}
	default: { /* sun: disc and 8 rays */
		float s2 = X * X + Y * Y, sun = disc(s2, 6.5f, 1.0f / 14);

		if (s2 > 9.5f * 9.5f && s2 < 16 * 16) {
			static const float dc[8] = {1, 0.7071f, 0, -0.7071f, -1, -0.7071f, 0, 0.7071f};

			for (int k = 0; k < 8; k++) {
				float cx = dc[k], cy = dc[(k + 2) % 8];

				if (X * cx + Y * cy > 0) {
					sun = fmx(sun, clamp01(0.5f - (fabsf(X * cy - Y * cx) - 1.2f)));
				}
			}
		}
		return sun;
	}
	}
}

/* Curved arrows inside the knob: clockwise on the right ("+"), anticlockwise
 * on the left ("-"), each an arc from 30 to 56 degrees with a head to 62.
 */
static float rm_arrows(float X, float Y, float d2)
{
	if (d2 < 160 * 160 || d2 > 182 * 182) {
		return 0;
	}
	const float deg = 3.14159265f / 180;
	float d = sqrtf(d2), th = atan2f(X, -Y) / deg; /* clock angle */
	float a = 0;

	for (int side = -1; side <= 1; side += 2) {
		float t = th * side;

		if (t > 30 && t < 57) {
			a = fmx(a, clamp01(0.5f - (fabsf(d - 171) - 0.7f)));
		}
		float bx = sinf(side * 56 * deg) * 171, by = -cosf(side * 56 * deg) * 171;
		float tx = sinf(side * 64 * deg) * 171, ty = -cosf(side * 64 * deg) * 171;
		float ux = tx - bx, uy = ty - by, ul = sqrtf(ux * ux + uy * uy);

		ux /= ul;
		uy /= ul;
		float sl = (X - bx) * ux + (Y - by) * uy, pl = -(X - bx) * uy + (Y - by) * ux;

		a = fmx(a, tri(sl, pl, 0, ul, 4.5f));
	}
	return a;
}

static float rm_signs(float X, float Y)
{
	const float sx = 0.7193f * 150, sy = -0.6947f * 150; /* 46 degrees, r 150 */
	float plus = fmx(bar(X - sx, Y - sy, -6, 6, 1.4f), bar(Y - sy, X - sx, -6, 6, 1.4f));
	float minus = bar(X + sx, Y - sy, -6, 6, 1.4f);

	return fmx(plus, minus);
}

/* Clock angle of (X, Y) in turns (0..1, 0 at 12, clockwise), approximated
 * (to ~0.1 degree), which is plenty for bars and avoids atan2f per pixel.
 */
static ALWAYS_INLINE float fast_turns(float X, float Y)
{
	float ax = fabsf(X), ay = fabsf(Y), mn = fmn(ax, ay), mx = fmx(ax, ay);
	float r = mx > 0 ? mn / mx : 0, s2 = r * r;
	/* atan(r) for r in [0, 1] (polynomial, ~0.0001 rad) */
	float a = ((-0.0464964749f * s2 + 0.15931422f) * s2 - 0.327622764f) * s2 * r + r;
	float phi = ay > ax ? 1.57079633f - a : a; /* from the X axis, 0..pi/2 */
	float th;                                  /* clock angle (Y points down) */

	if (X >= 0) {
		th = Y <= 0 ? 1.57079633f - phi : 1.57079633f + phi;
	} else {
		th = Y > 0 ? 4.71238898f - phi : 4.71238898f + phi;
	}
	return th * (1.0f / 6.28318531f);
}

/* One pixel of the remote inside the knob (pixel centre, relative to the middle). */
static uint16_t remote_px(float X, float Y)
{
	float d2 = X * X + Y * Y;
	/* one flat dark plum (the knob strip's inside colour) */
	struct rgb c = {30, 18, 40};

	if (rm_muted) { /* "MUTED" curved over the mute button */
		int lx = (int)(X + CX) - (int)(CX + MUTED_X0), ly = (int)(Y + CX) - (int)(CX + MUTED_Y0);

		if (lx >= 0 && lx < MUTED_W && ly >= 0 && ly < MUTED_H && muted_a[ly * MUTED_W + lx]) {
			over(&c, 255, 96, 110, muted_a[ly * MUTED_W + lx]);
			return to565(&c);
		}
	}
	for (int b = 0; b < RB_N; b++) {
		float bx = X - rm_btn[b].x, by = Y - rm_btn[b].y, b2 = bx * bx + by * by;
		float r = rm_btn[b].r;

		if (b2 > (r + 2) * (r + 2)) {
			continue;
		}
		float a = disc(b2, r, 1.0f / (2 * r + 1));
		bool lit = b == rm_pressed || (b == RB_SUN && rm_bright);

		struct rgb bc = {rm_btn[b].cr, rm_btn[b].cg, rm_btn[b].cb};

		if (b == RB_MUTE && rm_muted) {
			bc = (struct rgb){255, 92, 106};
		}
		/* minimal: icons in the button's colour; a faint disc only when
		 * pressed or active, a thin ring round play/pause
		 */
		if (lit || (b == RB_MUTE && rm_muted)) {
			over(&c, bc.r, bc.g, bc.b, (int)(a * 50));
		}
		if (b == RB_PLAY) {
			float ring = a - disc(b2, r - 1.4f, 1.0f / (2 * r - 1.8f));

			over(&c, bc.r, bc.g, bc.b, (int)(clamp01(ring) * 110));
		}
		float ic = rm_icon(b, bx, by);

		over(&c, bc.r, bc.g, bc.b, (int)(ic * 255));
		return to565(&c);
	}
	/* now playing: title (white) and artist (pink) */
	if (rm_text_on && Y >= RM_TEXT_Y0 && Y < RM_TEXT_Y0 + TEXT_H && X >= RM_TEXT_X0 &&
	    X < RM_TEXT_X0 + TEXT_W) {
		int tx = (int)(X - RM_TEXT_X0), ty = (int)(Y - RM_TEXT_Y0);
		int i = ty * TEXT_W + tx;
		int a = (np_img[i >> 2] >> (6 - 2 * (i & 3))) & 3;

		if (a) {
			if (ty < 22) {
				over(&c, 255, 255, 255, a * 85);
			} else {
				over(&c, 255, 176, 214, a * 85);
			}
		}
		return to565(&c);
	}
	/* the level meter: 60 small bars from 12 o'clock clockwise, lit up to the
	 * level (volume, or brightness in brightness mode), dim beyond it
	 */
	if (rm_level >= 0 && d2 > 181 * 181 && d2 < 192 * 192) {
		float d = sqrtf(d2);
		float u = fast_turns(X, Y) * 60;
		int seg = (int)u;
		float f = u - seg, arc = d * (6.28318531f / 60);
		float across = clamp01(0.5f - (fabsf(f - 0.5f) - 0.30f) * arc);
		float along = clamp01(0.5f - (fabsf(d - RM_ARC_R) - 3.4f));
		int a = (int)(across * along * 255);

		if (a > 0) {
			if (seg >= rm_level * 60 / 100) {
				over(&c, 255, 255, 255, a / 7);
			} else if (rm_bright) {
				over(&c, 255, 212, 110, a);
			} else {
				over(&c, 255, 150, 190, a);
			}
		}
		return to565(&c);
	}
	/* the visualiser ring */
	if (d2 > (RM_VIS_R0 - 1) * (RM_VIS_R0 - 1) &&
	    d2 < (RM_VIS_R0 + RM_VIS_H + 1) * (RM_VIS_R0 + RM_VIS_H + 1)) {
		float d = sqrtf(d2);
		float u = fast_turns(X, Y) * RM_VIS_N; /* position around, in bars */
		int k = (int)u % RM_VIS_N;
		float f = u - (int)u, arc = d * (2 * 3.14159265f / RM_VIS_N);
		float across = clamp01(0.5f - (fabsf(f - 0.5f) - 0.28f) * arc);
		float along = clamp01(0.5f - (d - (RM_VIS_R0 + rm_vis[k]))) *
			      clamp01(0.5f + (d - RM_VIS_R0));
		float t = clamp01((d - RM_VIS_R0) / RM_VIS_H);
		int a = (int)(across * along * 230);

		if (a > 0) {
			const struct rgb *vc = &rm_vis_col[k];

			ARG_UNUSED(t);
			over(&c, vc->r, vc->g, vc->b, a);
		}
		return to565(&c);
	}
	float ar = fmx(rm_arrows(X, Y, d2), rm_signs(X, Y));

	if (ar > 0) {
		if (rm_bright) {
			over(&c, 255, 212, 110, (int)(ar * 120));
		} else {
			over(&c, 255, 176, 214, (int)(ar * 120));
		}
	}
	return to565(&c);
}

static void remote_row(uint16_t *o, int x0, int x1, int y)
{
	const float Y = y + 0.5f - CX;

	for (int x = x0; x <= x1; x++) {
		float X = x + 0.5f - CX;

		*o++ = X * X + Y * Y > (WATCH_BEZEL_IN + 2) * (WATCH_BEZEL_IN + 2) ? 0 : remote_px(X, Y);
	}
}

/* Redraw a rectangle (any size: split into strips that fit the band buffer). */
static void render_rect(int x0, int y0, int x1, int y1)
{
	x0 = MAX(0, x0 & ~1);
	x1 = MIN(N - 1, x1 | 1);
	int w = x1 - x0 + 1, rows = (BAND * N) / w;

	for (int y = MAX(0, y0); y <= MIN(N - 1, y1); y += rows) {
		render_box(x0, y, x1, MIN(MIN(N - 1, y1), y + rows - 1));
	}
	qf_wait();
}

/* Redraw the ring between radii ri and ro (two boxes per band, like the bezel). */
static void render_annulus(float ri, float ro)
{
	for (int y0 = (int)(CX - ro) & ~(BAND - 1); y0 <= CX + ro; y0 += BAND) {
		int y1 = MIN(y0 + BAND - 1, N - 1);
		float ny = fmn(fabsf(y0 - CX), fabsf(y1 + 1 - CX));
		float fy = fmx(fabsf(y0 - CX), fabsf(y1 + 1 - CX));

		if (y0 <= CX && y1 + 1 >= CX) {
			ny = 0;
		}
		if (ny >= ro) {
			continue;
		}
		int ho = (int)ceilf(sqrtf(ro * ro - ny * ny));

		if (fy < ri) {
			int hi = (int)floorf(sqrtf(ri * ri - fy * fy));

			render_box(((int)CX - ho - 1) & ~1, y0, ((int)CX - hi + 1) | 1, y1);
			render_box(((int)CX + hi - 1) & ~1, y0, ((int)CX + ho + 1) | 1, y1);
		} else {
			render_box(((int)CX - ho - 1) & ~1, y0, ((int)CX + ho + 1) | 1, y1);
		}
	}
	qf_wait();
}

static void render_button(int b)
{
	int r = rm_btn[b].r + 3;

	render_rect(CX + rm_btn[b].x - r, CX + rm_btn[b].y - r, CX + rm_btn[b].x + r,
		    CX + rm_btn[b].y + r);
}

static void remote_key(uint8_t key)
{
	struct msg_key mk = {.key = key};

	msg_send(ipm, MSG_KEY, &mk, sizeof(mk));
}

/* knob angle in lookup steps (0.25 degree): the ring follows the finger */
static int32_t rm_steps(void)
{
	int32_t st = (int32_t)lroundf(rm_pos * (WATCH_RING_ANG / 3600.0f)) % WATCH_RING_ANG;

	return st < 0 ? st + WATCH_RING_ANG : st;
}

static void remote_knob_redraw(void)
{
	ring_shift = rm_steps() * WATCH_RING_RAD;
	render_ring();
	qf_wait();
}

static void remote_set_mode(bool bright)
{
	rm_bright = bright;
	ring_palp = bright ? knob_pal_amber : knob_pal;
	rm_last_touch = k_uptime_get();
	remote_knob_redraw();
	/* the arrows change colour too: redraw the band they are in */
	render_rect(CX - 185, CX - 185, CX + 185, CX - 60);
	render_button(RB_SUN);
}

static void remote_enter(void)
{
	if (face != FACE_REMOTE) {
		rm_prev_face = face;
		rm_saved_bezel = bezel_rot;
	}
	memcpy(ring_q, knob_q, sizeof(ring_q)); /* the knob replaces the bezel strip */
	face_setup(FACE_REMOTE);
	rm_bright = rm_active = false;
	rm_pressed = -1;
	ring_palp = knob_pal;
	draw_full(); /* (with the knob where it was left: draw_full keeps ring_shift) */
}

static void remote_leave_ring(void)
{
	memcpy(ring_q, watch_ring_q, sizeof(ring_q)); /* the GMT bezel again */
	ring_palp = ring_pal;
	bezel_rot = rm_saved_bezel;
}

static void remote_exit(void)
{
	remote_leave_ring();
	face_setup(rm_prev_face);
	draw_full();
}

static int rm_level_now(void)
{
	if (k_uptime_get() - host_ms > 5000) {
		return -1; /* no helper app (any more) */
	}
	return rm_bright ? host.brightness : host.volume;
}

static void remote_frame(void)
{
	int64_t now = k_uptime_get();

	/* follow the computer when the helper app reports its state */
	if (host_new) {
		host_new = false;
		if (host.muted >= 0 && (host.muted != 0) != rm_muted) {
			rm_muted = host.muted;
			render_button(RB_MUTE);
			render_rect(CX + MUTED_X0, CX + MUTED_Y0, CX + MUTED_X0 + MUTED_W,
				    CX + MUTED_Y0 + MUTED_H);
		}
		if (host.playing >= 0 && (host.playing != 0) != rm_playing) {
			rm_playing = host.playing;
			render_button(RB_PLAY);
		}
	}
	/* now playing: show the text while the helper keeps sending it */
	bool text_on = now - np_ms < 12000;

	if (np_new || text_on != rm_text_on) {
		np_new = false;
		rm_text_on = text_on;
		render_rect(CX + RM_TEXT_X0, CX + RM_TEXT_Y0, CX + RM_TEXT_X0 + TEXT_W - 1,
			    CX + RM_TEXT_Y0 + TEXT_H - 1);
	}
	int lv = rm_level_now();

	if (lv != rm_level) { /* the arc follows the level (or goes away) */
		rm_level = lv;
		render_annulus(180, 193);
	}

	if (touch_new) {
		touch_new = false;
		float dx = touch.x + 0.5f - CX, dy = touch.y + 0.5f - CX, r2 = dx * dx + dy * dy;
		int32_t ang = (int32_t)(atan2f(dx, -dy) * (1800.0f / 3.14159265f));

		if (touch.down) {
			rm_last_touch = now;
			if (!rm_active) {
				rm_active = true;
				rm_t0 = now;
				rm_x0 = touch.x;
				rm_y0 = touch.y;
				/* the knob: anywhere from 120 px out (the arrows too), except
				 * on a button
				 */
				rm_drag = r2 > 120.0f * 120.0f;
				for (int b = 0; b < RB_N && rm_drag; b++) {
					float bx = dx - rm_btn[b].x, by = dy - rm_btn[b].y;
					float r = rm_btn[b].r + 6;

					rm_drag = bx * bx + by * by > r * r;
				}
				rm_moved = false;
				rm_last_ang = ang;
				rm_acc = 0;
			} else {
				int mx = touch.x - rm_x0, my = touch.y - rm_y0;

				rm_moved |= mx * mx + my * my > 12 * 12;
				if (rm_drag) {
					int32_t d = ang - rm_last_ang;

					d += d > 1800 ? -3600 : (d < -1800 ? 3600 : 0);
					rm_last_ang = ang;
					int32_t before = rm_steps();

					rm_pos += d;
					/* one key each time the knob crosses a key tick */
					int32_t k_old = (int32_t)floorf((rm_pos - d) / (KNOB_KEY_DEG * 10));
					int32_t k_new = (int32_t)floorf(rm_pos / (KNOB_KEY_DEG * 10));

					for (int n = 0; k_new != k_old && n < 8; n++) {
						bool up = k_new > k_old;

						remote_key(rm_bright ? (up ? KEY_BRIGHT_UP : KEY_BRIGHT_DOWN)
								     : (up ? KEY_VOL_UP : KEY_VOL_DOWN));
						k_old += up ? 1 : -1;
						if (!rm_bright && rm_muted) {
							rm_muted = false; /* a volume key unmutes */
							render_button(RB_MUTE);
							render_rect(CX + MUTED_X0, CX + MUTED_Y0,
								    CX + MUTED_X0 + MUTED_W,
								    CX + MUTED_Y0 + MUTED_H);
						}
					}
					if (rm_steps() != before) {
						remote_knob_redraw();
					}
				}
			}
		} else if (rm_active) {
			rm_active = false;
			if (!rm_drag && !rm_moved && now - rm_t0 < 600) { /* a tap */
				for (int b = 0; b < RB_N; b++) {
					float bx = dx - rm_btn[b].x, by = dy - rm_btn[b].y;
					float r = rm_btn[b].r + 8;

					if (bx * bx + by * by > r * r) {
						continue;
					}
					if (b == RB_SUN) {
						remote_set_mode(!rm_bright);
					} else if (b == RB_MUTE) {
						remote_key(KEY_MUTE);
						rm_muted = !rm_muted;
						render_button(b);
						render_rect(CX + MUTED_X0, CX + MUTED_Y0, CX + MUTED_X0 + MUTED_W,
							    CX + MUTED_Y0 + MUTED_H);
					} else {
						remote_key(rm_btn[b].key);
						if (b == RB_PLAY) {
							rm_playing = !rm_playing;
						}
						rm_pressed = b;
						rm_press_until = now + 150;
						render_button(b);
					}
					break;
				}
			}
		}
	}
	/* the visualiser: bounce while playing, fade away when paused (30 fps) */
	static int64_t vis_last;
	static bool vis_on;

	if ((rm_playing || vis_on) && now - vis_last >= 33) {
		float tsec = now * 0.001f, total = 0;

		vis_last = now;
		for (int k = 0; k < RM_VIS_N; k++) {
			float target = 0;

			if (rm_playing) { /* a few drifting "bands", a bit of jitter */
				float b = 0.5f + 0.5f * sinf(tsec * 2.3f + k * 0.55f) *
						    sinf(tsec * 1.7f - k * 0.31f);
				float beat = 0.5f + 0.5f * sinf(tsec * 7.5f);

				target = RM_VIS_H * (0.15f + 0.85f * b * (0.6f + 0.4f * beat));
			}
			float v = rm_vis[k];

			/* rise fast, fall slowly: the bars fade away */
			v += (target - v) * (target > v ? 0.5f : 0.12f);
			rm_vis[k] = v < 0.3f ? 0 : v;
			total += rm_vis[k];
		}
		vis_on = total > 0;
		render_annulus(RM_VIS_R0 - 2, RM_VIS_R0 + RM_VIS_H + 2);
	}
	if (rm_pressed >= 0 && now > rm_press_until) {
		int b = rm_pressed;

		rm_pressed = -1;
		render_button(b);
	}
	if (rm_bright && !rm_active && now - rm_last_touch > RM_IDLE_MS) {
		remote_set_mode(false); /* back to volume */
	}
}

/* ---- buttons and the face picker ------------------------------------------------------ */

K_MSGQ_DEFINE(buttons, sizeof(uint8_t), 8, 1);

#define PICK_R  80 /* thumbnail radius */
static const int pick_cx[NFACES] = {147, 319, 147, 319};
static const int pick_cy[NFACES] = {147, 147, 319, 319};

/* Bezel colour at (x, y) at the current turn, or bg if not on the ring. */
static uint16_t ring_at(int x, int y, uint16_t bg)
{
	const int16_t *sp = (const int16_t *)watch_ring_spans + y * 4;
	uint32_t idx = ((const uint32_t *)watch_ring_row_off)[y];

	for (int k = 0; k < 2; k++, sp += 2) {
		if (x >= sp[0] && x <= sp[1]) {
			int32_t t = (int32_t)((const uint16_t *)watch_ring_idx)[idx + x - sp[0]] -
				    ring_shift;

			if (t < 0) {
				t += WATCH_RING_ANG * WATCH_RING_RAD;
			}
			return ring_palp[ring_q[t]];
		}
		if (sp[1] >= sp[0]) {
			idx += sp[1] - sp[0] + 1;
		}
	}
	return bg;
}

/* The knob (at rest) at (x, y), or bg if not on the ring: for the picker. */
static uint16_t knob_at(int x, int y, uint16_t bg)
{
	const int16_t *sp = (const int16_t *)watch_ring_spans + y * 4;
	uint32_t idx = ((const uint32_t *)watch_ring_row_off)[y];

	for (int k = 0; k < 2; k++, sp += 2) {
		if (x >= sp[0] && x <= sp[1]) {
			return knob_pal[knob_q[((const uint16_t *)watch_ring_idx)[idx + x - sp[0]]]];
		}
		if (sp[1] >= sp[0]) {
			idx += sp[1] - sp[0] + 1;
		}
	}
	return bg;
}

/* The faces as round thumbnails, the selected one ringed. */
static void draw_picker(int sel)
{
	const uint16_t *dials[NFACES] = {(const uint16_t *)watch_dial,
					 (const uint16_t *)speed_dial,
					 (const uint16_t *)speed_white_dial, NULL};
	const int k = (int)(CX * 256 / PICK_R); /* dial pixels per thumbnail pixel, x256 */

	for (int y0 = 0; y0 < N; y0 += BAND) {
		int h = MIN(BAND, N - y0);

		for (int y = y0; y < y0 + h; y++) {
			uint16_t *o = &buf[(y - y0) * N];
			for (int x = 0; x < N; x++) {
				uint16_t v = 0;

				for (int f = 0; f < NFACES; f++) {
					int dx = x - pick_cx[f], dy = y - pick_cy[f];
					int d2 = dx * dx + dy * dy;

					if (d2 < PICK_R * PICK_R) {
						int sx = (int)CX + dx * k / 256, sy = (int)CX + dy * k / 256;

						if (dials[f]) {
							v = dials[f][sy * N + sx];
						} else {
							remote_row(&v, sx, sx, sy);
						}
						if (f == FACE_GMT) {
							v = ring_at(sx, sy, v);
						} else if (f == FACE_REMOTE) {
							v = knob_at(sx, sy, v);
						}
					} else if (f == sel && d2 < (PICK_R + 6) * (PICK_R + 6) &&
						   d2 >= (PICK_R + 2) * (PICK_R + 2)) {
						v = 0xffff;
					}
				}
				o[x] = (v >> 8) | (v << 8);
			}
		}
		qf_write(0, y0, N, h, buf);
		cur ^= 1;
		buf = bufs[cur];
	}
	qf_wait();
}

/* Runs the picker until a face is chosen (or 15 s pass); returns it. */
static int pick_face(void)
{
	int sel = face;
	int64_t t0 = k_uptime_get();
	uint8_t btn;

	draw_picker(sel);
	while (k_uptime_get() - t0 < 15000) {
		if (k_msgq_get(&buttons, &btn, K_MSEC(20)) == 0) {
			t0 = k_uptime_get();
			if (btn == BTN_BOOT) {
				sel = (sel + 1) % NFACES;
				draw_picker(sel);
			} else {
				return sel; /* PWR or BOOT held: take it */
			}
		}
		if (touch_new) {
			touch_new = false;
			if (touch.down) { /* the nearest thumbnail */
				int best = 0, bd = INT32_MAX;

				for (int f = 0; f < NFACES; f++) {
					int dx = touch.x - pick_cx[f], dy = touch.y - pick_cy[f];

					if (dx * dx + dy * dy < bd) {
						bd = dx * dx + dy * dy;
						best = f;
					}
				}
				return best;
			}
		}
	}
	return face;
}

static void handle_buttons(void)
{
	uint8_t btn;

	while (k_msgq_get(&buttons, &btn, K_NO_WAIT) == 0) {
		int64_t now = k_uptime_get();

		if (btn == BTN_BOOT_LONG) {
			int f = pick_face();

			if (f == FACE_REMOTE) {
				remote_enter();
			} else {
				if (face == FACE_REMOTE) {
					remote_leave_ring();
				}
				face_setup(f);
				draw_full();
			}
			continue;
		}
		if (btn == BTN_TWO_HOLD) {
			if (face == FACE_REMOTE) {
				remote_exit();
			} else {
				remote_enter();
			}
			continue;
		}
		if (face == FACE_REMOTE) {
			if (btn == BTN_PWR) {
				remote_key(KEY_PLAY_PAUSE);
				rm_playing = !rm_playing;
				render_button(RB_PLAY);
			} else if (btn == BTN_BOOT) {
				remote_set_mode(!rm_bright);
			}
			continue;
		}
		if (face == FACE_GMT) {
			continue;
		}
		if (btn == BTN_PWR) {
			if (chrono_run) {
				chrono_acc += now - chrono_t0;
			} else {
				chrono_t0 = now;
			}
			chrono_run = !chrono_run;
		} else if (btn == BTN_BOOT) {
			chrono_acc = 0;
			chrono_t0 = now;
		}
	}
}

/* ---- main ---------------------------------------------------------------------------- */

int main(void)
{
	memcpy(ring_q, watch_ring_q, sizeof(ring_q));
	memcpy(ring_pal, watch_ring_pal, sizeof(ring_pal));
	memcpy(knob_pal, knob_pal_flash, sizeof(knob_pal));
	for (int k = 0; k < RM_VIS_N; k++) { /* pastel rainbow for the bars */
		float h = k * 6.0f / RM_VIS_N;
		int i = (int)h;
		float f = h - i, v = 255, lo = 255 * 0.45f;
		float rise = lo + (v - lo) * f, fall = v - (v - lo) * f;
		float rgb[6][3] = {{v, rise, lo}, {fall, v, lo}, {lo, v, rise},
				   {lo, fall, v}, {rise, lo, v}, {v, lo, fall}};

		rm_vis_col[k] = (struct rgb){(int)rgb[i][0], (int)rgb[i][1], (int)rgb[i][2]};
	}
	for (int i = 0; i < 256; i++) { /* the knob's brightness-mode colours */
		struct rgb c = from565(knob_pal[i]);
		int l = 30 + ((c.r * 77 + c.g * 150 + c.b * 29) >> 8) * 225 / 255;
		struct rgb a = {l, l * 72 / 100, l * 24 / 100};

		knob_pal_amber[i] = to565(&a);
	}
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);
	if (!device_is_ready(disp)) {
		return 0;
	}

	/* first frame: everything (hands at 12 until the time arrives) */
	face_setup(BOOT_FACE);
	draw_full();
	display_blanking_off(disp);

	/* micro-benchmark: where does a composited pixel's time go? */
	{
		const uint16_t *dial = (const uint16_t *)watch_dial;
		struct msg_bench mb = {0};
		volatile uint32_t sink = 0;
		uint32_t t0, acc = 0;

		t0 = k_cycle_get_32(); /* 20000 px read in long runs (rows 100..142) */
		for (int i = 0; i < 20000; i++) {
			acc += dial[100 * N + i];
		}
		mb.seq_read = (k_cycle_get_32() - t0) * 10 / 20000;
		t0 = k_cycle_get_32(); /* band pattern: 8 rows x 70 px, many places */
		for (int b = 0; b < 36; b++) {
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 70; x++) {
					acc += dial[(b * 12 + y) * N + 150 + (b * 7) % 200 + x];
				}
			}
		}
		mb.band_read = (k_cycle_get_32() - t0) * 10 / (36 * 8 * 70);
		t0 = k_cycle_get_32(); /* pure maths: second-hand coverage, no memory */
		float fs = 0, fp = -12;
		for (int i = 0; i < 20000; i++) {
			float aw, ag, al;

			sec_cover(fs, fp, &aw, &ag, &al);
			acc += (int)(aw * 255 + ag + al);
			fs += 0.01f;
			fp += 0.001f;
		}
		mb.compute = (k_cycle_get_32() - t0) * 10 / 20000;
		t0 = k_cycle_get_32(); /* internal RAM read for comparison */
		for (int i = 0; i < 20000; i++) {
			acc += buf[i % (BAND * N)];
		}
		mb.internal_read = (k_cycle_get_32() - t0) * 10 / 20000;
		/* render_box itself (includes display_write): a box with only dial,
		 * and a box where all four hands overlap (they all point at 12 now)
		 */
		uint32_t c0 = prof_comp;

		render_box(100, 380, 169, 387);
		mb.box_plain = (prof_comp - c0) * 10 / (70 * 8);
		c0 = prof_comp;
		render_box(200, 100, 269, 107);
		mb.box_hands = (prof_comp - c0) * 10 / (70 * 8);
		sink = acc;
		(void)sink;
		k_msleep(500);
		msg_send(ipm, MSG_BENCH, &mb, sizeof(mb));
	}

	bool have_te = detect_te();
	int64_t hello_ms = 0;

	fast_spi = true; /* the Zephyr driver has set SPI2 + GDMA up; take over */
	/* the cost of one bezel click: the whole ring, as the loop does it */
	{
		struct msg_bench rb = {0};

		for (int i = 1; i <= 3; i++) {
			uint32_t px0 = prof_px, c0 = k_cycle_get_32(), w0 = prof_write;
			uint32_t s0[5];

			memcpy(s0, st_c, sizeof(s0));
			ring_shift = i * (WATCH_RING_ANG / 60) * WATCH_RING_RAD;
			render_ring();
			qf_wait();
			rb.ring_full_us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
			rb.ring_px = prof_px - px0;
			/* stage split, us: bg, ring, hands, sweep, swap; then write */
			rb.seq_read = k_cyc_to_us_floor32(st_c[0] - s0[0]);
			rb.band_read = k_cyc_to_us_floor32(st_c[1] - s0[1]);
			rb.compute = k_cyc_to_us_floor32(st_c[2] - s0[2]);
			rb.internal_read = k_cyc_to_us_floor32(st_c[3] - s0[3]);
			rb.box_plain = k_cyc_to_us_floor32(st_c[4] - s0[4]);
			rb.box_hands = k_cyc_to_us_floor32(prof_write - w0);
		}
		ring_shift = 0;
		render_ring();
		qf_wait();
		k_msleep(600);
		msg_send(ipm, MSG_BENCH, &rb, sizeof(rb));
	}
	uint32_t frames = 0, busy_us = 0, last_stats = k_uptime_get_32(), max_us = 0;

	while (1) {
		wait_te(have_te);
		uint32_t t0 = k_cycle_get_32();

		handle_buttons();
		if (face == FACE_REMOTE) {
			remote_frame();
			continue;
		}
		if (base_ms < 0 && k_uptime_get() - hello_ms > 1000) {
			hello_ms = k_uptime_get(); /* no time yet: ask core 0 (again) */
			msg_send(ipm, MSG_HELLO, NULL, 0);
		}
		if (face == FACE_GMT) {
			bezel_touch();
		} else {
			touch_new = false;
		}
		bands_clear();

		int32_t sec_a = face_angles(day_ms_now(), k_uptime_get());

		date_day = base_ms >= 0 ? base.day : 0;
		/* the sweep hand: every frame it moves */
		if (sec_a != sec_shown) {
			mark_hand(CX, CX, sec_sn, sec_cs, sw.tail, sw.tip, sw.mr);
			angle_sc(sec_a, &sec_sn, &sec_cs);
			sweep_bbox();
			mark_hand(CX, CX, sec_sn, sec_cs, sw.tail, sw.tip, sw.mr);
			sec_shown = sec_a;
		}
		/* the other hands only when they actually move */
		for (int k = 0; k < nhands; k++) {
			struct hand *hd = &hands[k];

			if (hd->ang == hd->shown) {
				continue;
			}
			mark_hand(hd->px, hd->py, hd->sn, hd->cs, hd->mtail, hd->mtip, hd->mr);
			angle_sc(hd->ang, &hd->sn, &hd->cs);
			hand_bbox(k);
			mark_hand(hd->px, hd->py, hd->sn, hd->cs, hd->mtail, hd->mtip, hd->mr);
			hd->shown = hd->ang;
		}
		if (face == FACE_GMT && date_day != date_shown) {
			mark_rect(WATCH_DATE_CX - WATCH_DATE_W / 2, WATCH_DATE_CY - WATCH_DATE_H / 2,
				  WATCH_DATE_CX + WATCH_DATE_W / 2, WATCH_DATE_CY + WATCH_DATE_H / 2);
			date_shown = date_day;
		}
		bool ring = face == FACE_GMT && bezel_rot != bezel_shown;

		if (ring) {
			ring_shift = bezel_rot * WATCH_RING_ANG / 3600 * WATCH_RING_RAD;
			bezel_shown = bezel_rot;
		}

		render_bands();
		qf_wait();
		if (ring) {
			render_ring();
			qf_wait();
		}

		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

		busy_us += us;
		max_us = MAX(max_us, us);
		frames++;
		if (k_uptime_get_32() - last_stats >= 1000) {
			uint32_t f = MAX(frames, 1);
			struct msg_stats st = {.frames = frames, .uptime_ms = k_uptime_get_32(),
					       .render_us = busy_us / f, .px = prof_px / f,
					       .comp_us = k_cyc_to_us_floor32(prof_comp / f),
					       .write_us = k_cyc_to_us_floor32(prof_write / f),
					       .boxes = prof_boxes / f,
					       .st_bg = k_cyc_to_us_floor32(st_c[0] / f),
					       .st_ring = k_cyc_to_us_floor32(st_c[1] / f),
					       .st_hands = k_cyc_to_us_floor32(st_c[2] / f),
					       .st_sec = k_cyc_to_us_floor32(st_c[3] / f),
					       .st_swap = k_cyc_to_us_floor32(st_c[4] / f),
					       .sec_px = st_secpx / f,
					       .underruns = qf_underruns, .overlap = qf_overlap};

			qf_underruns = 0;

			qf_underruns = 0;

			msg_send(ipm, MSG_STATS, &st, sizeof(st));
			frames = busy_us = max_us = 0;
			prof_px = prof_comp = prof_write = prof_boxes = 0;
			memset(st_c, 0, sizeof(st_c));
			st_secpx = 0;
			last_stats = k_uptime_get_32();
		}
	}
	return 0;
}
