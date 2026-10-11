/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watch, core 1 side: watch faces with their own small compositor (no LVGL),
 * built for a gliding sweep hand at the panel's refresh rate. Two faces: a
 * GMT (rotating bezel, date) and a chronograph (fixed tachymeter, subdials;
 * PWR starts/stops, BOOT resets). Holding BOOT 3 s opens a face picker.
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

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	if (id == MSG_TIME) {
		memcpy(&base, (const void *)data, sizeof(base));
		base_ms = k_uptime_get();
	} else if (id == MSG_TOUCH) {
		memcpy((void *)&touch, (const void *)data, sizeof(touch));
		touch_new = true;
	} else if (id == MSG_BUTTON) {
		uint8_t b = ((volatile struct msg_button *)data)->btn;

		(void)k_msgq_put(&buttons, &b, K_NO_WAIT);
	}
}

/* ---- scene ---------------------------------------------------------------------- */

/* Faces: the GMT (Rolex style, rotating bezel, date) and the chronograph
 * (moon-watch style, fixed tachymeter, three subdials).
 */
enum { FACE_GMT, FACE_CHRONO, NFACES };
static int face = FACE_GMT;

/* The sweeping hand (GMT: seconds; chronograph: chrono seconds), distances
 * along it from its pivot in pixels. dot = 0: no lume dot.
 */
struct sweep {
	float tip, tail, halfw, dot, cw, cwr, cwk, cap, capk;
	int mr; /* dirty-band radius: widest part + shadow + blur */
};
static const struct sweep sweep_gmt = {
	.tip = 196, .tail = -40, .halfw = 1.3f, .dot = 130, .cw = -32, .cwr = 6,
	.cwk = 1.0f / 13, .cap = 6.5f, .capk = 1.0f / 14, .mr = 17,
};
static const struct sweep sweep_chrono = {
	.tip = 192, .tail = -42, .halfw = 1.0f, .dot = 0, .cw = -30, .cwr = 4.5f,
	.cwk = 1.0f / 10, .cap = 5.5f, .capk = 1.0f / 12, .mr = 12,
};
static struct sweep sw = sweep_gmt;
static float sec_sn, sec_cs;
static int32_t sec_shown; /* the first frame draws it at 12 */

#define BEZEL_STEP 60     /* 6 degrees = one minute on the dial */
static int32_t bezel_rot, bezel_shown = -1;
static float bez_sn, bez_cs;

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

/* Bilinear sample of the bezel ring (RGB565 + A8 planes) at image coords. */
static ALWAYS_INLINE void bezel_px(float u, float v, struct rgb *d)
{
	const uint16_t *rgbp = (const uint16_t *)watch_bezel;
	const uint8_t *ap = watch_bezel + N * N * 2;

	u -= 0.5f;
	v -= 0.5f;
	int x0 = ifloor(u), y0 = ifloor(v);

	if (x0 < 0 || y0 < 0 || x0 >= N - 1 || y0 >= N - 1) {
		return;
	}
	int fx = (int)((u - x0) * 256), fy = (int)((v - y0) * 256);
	int wts[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
	int pr = 0, pg = 0, pb = 0, pa = 0;

	for (int k = 0; k < 4; k++) {
		int i = (y0 + (k >> 1)) * N + x0 + (k & 1);
		int a = (ap[i] * wts[k]) >> 8;

		if (!a) {
			continue;
		}
		struct rgb c = from565(rgbp[i]);

		pa += a;
		pr += c.r * a;
		pg += c.g * a;
		pb += c.b * a;
	}
	if (pa == 0) {
		return;
	}
	int ia = 65536 - pa;

	d->r = (pr + d->r * ia) >> 16;
	d->g = (pg + d->g * ia) >> 16;
	d->b = (pb + d->b * ia) >> 16;
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
#define STEEL_R 226
#define STEEL_G 228
#define STEEL_B 232

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
			over(c, STEEL_R, STEEL_G, STEEL_B, (int)(body * 255));
		} else {
			over(c, GOLD_R, GOLD_G, GOLD_B, (int)(body * 255));
		}
	}
	if (lume > 0) {
		over(c, LUME_R, LUME_G, LUME_B, (int)(lume * 255));
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

static void IRAM_ATTR render_box(int x0, int y0, int x1, int y1)
{
	uint32_t pt0 = k_cycle_get_32();
	const bool gmt_face = face == FACE_GMT;
	const uint16_t *dial = (const uint16_t *)(gmt_face ? watch_dial : speed_dial);
	const int w = x1 - x0 + 1, h = y1 - y0 + 1;
	const int dx0 = WATCH_DATE_CX - WATCH_DATE_W / 2, dy0 = WATCH_DATE_CY - WATCH_DATE_H / 2;
	const uint16_t *date = gmt_face && date_day ? (const uint16_t *)watch_dates +
					  (date_day - 1) * WATCH_DATE_W * WATCH_DATE_H : NULL;
	bool hand_on[MAXH];
	/* does this box reach the bezel ring at all? (corners vs radii) */
	float fxm = fmx(fabsf(x0 - CX), fabsf(x1 + 1 - CX)), fym = fmx(fabsf(y0 - CX), fabsf(y1 + 1 - CX));
	bool ring = gmt_face &&
		    fxm * fxm + fym * fym > (WATCH_BEZEL_IN - 1) * (WATCH_BEZEL_IN - 1);
	/* plain-shaft stretch of the sweep hand ends at its lume dot, if any */
	const float d0 = sw.dot ? sw.dot : sw.tip + 10;

	for (int k = 0; k < nhands; k++) {
		hand_on[k] = hb_x1[k] >= x0 && hb_x0[k] <= x1 && hb_y1[k] >= y0 && hb_y0[k] <= y1;
	}

	for (int y = y0; y <= y1; y++) {
		uint16_t *o = &buf[(y - y0) * w];
		const uint16_t *drow = &dial[y * N];
		const float ry = y + 0.5f - CX;
		uint32_t tt = k_cycle_get_32();

		/* 1. background: the dial (or the date window), straight copy */
		memcpy(o, &drow[x0], w * 2);
		if (date && y >= dy0 && y < dy0 + WATCH_DATE_H && x1 >= dx0 &&
		    x0 < dx0 + WATCH_DATE_W) {
			int a = MAX(x0, dx0), b = MIN(x1, dx0 + WATCH_DATE_W - 1);

			memcpy(&o[a - x0], &date[(y - dy0) * WATCH_DATE_W + (a - dx0)], (b - a + 1) * 2);
		}

		ST(0, tt);
		/* 2. bezel ring: only its spans in this row */
		const float ro = WATCH_BEZEL_OUT, ri = WATCH_BEZEL_IN - 1;

		if (ring && fabsf(ry) < ro) {
			float ho = sqrtf(ro * ro - ry * ry);
			float hi = fabsf(ry) < ri ? sqrtf(ri * ri - ry * ry) : 0;
			int spans[2][2] = {{(int)(CX - ho - 1), (int)(CX - hi + 1)},
					   {(int)(CX + hi - 1), (int)(CX + ho + 1)}};
			int nsp = hi > 0 ? 2 : 1;

			if (nsp == 1) {
				spans[0][1] = spans[1][1];
			}
			for (int sp = 0; sp < nsp; sp++) {
				int a = MAX(x0, spans[sp][0]), b = MIN(x1, spans[sp][1]);
				float rx = a + 0.5f - CX;
				float bu = CX + rx * bez_cs + ry * bez_sn;
				float bv = CX - rx * bez_sn + ry * bez_cs;

				for (int x = a; x <= b; x++) {
					struct rgb c = from565(o[x - x0]);

					bezel_px(bu, bv, &c);
					o[x - x0] = to565(&c);
					bu += bez_cs;
					bv -= bez_sn;
				}
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
		int a, b;

		region_span(sweep_rg, 4, sec_sn, sec_cs, ry, x0, x1, &a, &b, CX);
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
				over(&c, 240, 240, 242, (int)(aw * 255));
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
		sw = sweep_chrono;
		sweep_rg = chrono_regions;
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
	sec_shown = sa;
	angle_sc(bezel_rot, &bez_sn, &bez_cs);
	bezel_shown = bezel_rot;
	date_day = base_ms >= 0 ? base.day : 0;
	date_shown = date_day;
	for (int y = 0; y < N; y += BAND) {
		render_box(0, y, N - 1, MIN(y + BAND - 1, N - 1));
	}
	qf_wait();
}

/* ---- buttons and the face picker ------------------------------------------------------ */

K_MSGQ_DEFINE(buttons, sizeof(uint8_t), 8, 1);

#define PICK_R  104 /* thumbnail radius */
static const int pick_cx[NFACES] = {117, 349};

/* Both faces as round thumbnails side by side, the selected one ringed. */
static void draw_picker(int sel)
{
	const uint16_t *dials[NFACES] = {(const uint16_t *)watch_dial,
					 (const uint16_t *)speed_dial};
	const int k = (int)(CX * 256 / PICK_R); /* dial pixels per thumbnail pixel, x256 */

	for (int y0 = 0; y0 < N; y0 += BAND) {
		int h = MIN(BAND, N - y0);

		for (int y = y0; y < y0 + h; y++) {
			uint16_t *o = &buf[(y - y0) * N];
			int dy = y - (int)CX;

			for (int x = 0; x < N; x++) {
				uint16_t v = 0;

				for (int f = 0; f < NFACES; f++) {
					int dx = x - pick_cx[f], d2 = dx * dx + dy * dy;

					if (d2 < PICK_R * PICK_R) {
						int sx = (int)CX + dx * k / 256, sy = (int)CX + dy * k / 256;

						v = dials[f][sy * N + sx];
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
			if (touch.down) {
				return touch.x < CX ? 0 : 1;
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
			face_setup(pick_face());
			draw_full();
			continue;
		}
		if (face != FACE_CHRONO) {
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
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);
	if (!device_is_ready(disp)) {
		return 0;
	}

	/* first frame: everything (hands at 12 until the time arrives) */
	face_setup(FACE_GMT);
	draw_full();
	display_blanking_off(disp);

	/* micro-benchmark: where does a composited pixel's time go? */
	{
		const uint16_t *dial = (const uint16_t *)watch_dial;
		struct msg_bench mb;
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
		ipm_send(ipm, -1, MSG_BENCH, &mb, sizeof(mb));
	}

	bool have_te = detect_te();

	fast_spi = true; /* the Zephyr driver has set SPI2 + GDMA up; take over */
	uint32_t frames = 0, busy_us = 0, last_stats = k_uptime_get_32(), max_us = 0;

	while (1) {
		wait_te(have_te);
		uint32_t t0 = k_cycle_get_32();

		handle_buttons();
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
			angle_sc(bezel_rot, &bez_sn, &bez_cs);
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

			ipm_send(ipm, 0, MSG_STATS, &st, sizeof(st));
			frames = busy_us = max_us = 0;
			prof_px = prof_comp = prof_write = prof_boxes = 0;
			memset(st_c, 0, sizeof(st_c));
			st_secpx = 0;
			last_stats = k_uptime_get_32();
		}
	}
	return 0;
}
