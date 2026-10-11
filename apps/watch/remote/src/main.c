/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watch, core 1 side: a GMT watch face with its own small compositor (no
 * LVGL), built for a gliding second hand at the panel's refresh rate.
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

#define N      WATCH_SIZE
#define CX     233.0f   /* centre, in pixel-edge coordinates */
#define BAND   8        /* rows per dirty band */
#define NBANDS ((N + BAND - 1) / BAND)
#define TE_PIN 13       /* panel tearing-effect output (core 0 makes it an input) */

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* one band, big-endian RGB565, internal RAM (sent straight by DMA) */
static uint16_t buf[BAND * N] __aligned(4);

/* ---- inputs from core 0 (written in the IPM interrupt) ------------------------- */

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
	}
}

/* ---- scene ---------------------------------------------------------------------- */

/* An image hand: ARGB8888 drawn pointing up, rotated about its pivot. */
struct img_hand {
	const uint8_t *px;
	int w, h;
	float pvx, pvy;
	int32_t ang;      /* 0.1 degree, clockwise from 12 */
	int32_t shown;    /* angle currently on screen (-1: never drawn) */
	float sn, cs;
};

#define HAND(name, NAME) {.px = name, .w = NAME##_W, .h = NAME##_H, \
			  .pvx = NAME##_PIVOT_X, .pvy = NAME##_PIVOT_Y, .shown = -1}

static struct img_hand gmt = HAND(watch_gmt, WATCH_GMT);
static struct img_hand hour = HAND(watch_hour, WATCH_HOUR);
static struct img_hand minute = HAND(watch_minute, WATCH_MINUTE);
static struct img_hand *const img_hands[] = {&gmt, &hour, &minute}; /* back to front */

/* second hand: distances along the hand from the centre (pixels) */
#define SEC_TIP   196.0f
#define SEC_TAIL  (-40.0f)
#define SEC_HALFW 1.3f
#define SEC_DOT   130.0f
#define SEC_CW    (-32.0f)
#define SEC_R     17      /* dirty-band radius: dot + shadow + blur */
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

/* Bilinear sample of an ARGB8888 hand at texel coords (u, v), blended onto d. */
static ALWAYS_INLINE void hand_px(const struct img_hand *hd, float u, float v, struct rgb *d)
{
	u -= 0.5f;
	v -= 0.5f;
	if (u < -1 || v < -1 || u >= hd->w || v >= hd->h) {
		return;
	}
	int x0 = ifloor(u), y0 = ifloor(v);
	int fx = (int)((u - x0) * 256), fy = (int)((v - y0) * 256);
	int wts[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
	/* a: weight x alpha, 0..65280 in total; colours accumulate as c x a */
	int pr = 0, pg = 0, pb = 0, pa = 0;

	for (int k = 0; k < 4; k++) {
		int x = x0 + (k & 1), y = y0 + (k >> 1);

		if (x < 0 || y < 0 || x >= hd->w || y >= hd->h || !wts[k]) {
			continue;
		}
		const uint8_t *p = hd->px + (y * hd->w + x) * 4; /* B G R A */
		int a = (p[3] * wts[k]) >> 8;

		pa += a;
		pb += p[0] * a;
		pg += p[1] * a;
		pr += p[2] * a;
	}
	if (pa == 0) {
		return;
	}
	/* premultiplied over: d = c + d * (1 - alpha), all over 65536 */
	int ia = 65536 - pa;

	d->r = (pr + d->r * ia) >> 16;
	d->g = (pg + d->g * ia) >> 16;
	d->b = (pb + d->b * ia) >> 16;
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

	if (s >= SEC_TAIL && s <= SEC_TIP) {
		w = clamp01(0.5f - (fabsf(p) - SEC_HALFW));
	} else {
		/* round end: disc of the shaft's half width at the end point */
		float e = s > SEC_TIP ? s - SEC_TIP : SEC_TAIL - s;

		w = disc(e * e + p * p, SEC_HALFW, 1.0f / (2 * SEC_HALFW + 1));
	}
	*a_gold = *a_lume = 0;
	if (fabsf(s - SEC_CW) < 7.5f) { /* counterweight */
		w = fmx(w, disc((s - SEC_CW) * (s - SEC_CW) + p * p, 6.0f, 1.0f / 13.0f));
	}
	if (fabsf(s) < 8.0f && fabsf(p) < 8.0f) { /* centre cap */
		w = fmx(w, disc(s * s + p * p, 6.5f, 1.0f / 14.0f));
	}
	if (fabsf(s - SEC_DOT) < 9.5f && fabsf(p) < 9.5f) { /* lume dot + surround */
		float d2 = (s - SEC_DOT) * (s - SEC_DOT) + p * p;

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
struct vhand {
	float tail, tip, reach; /* extent along, and widest half-width */
};

/* gmt, hour, minute (same order as img_hands) */
static const struct vhand vhands[3] = {
	{.tail = -10, .tip = 176, .reach = 14},
	{.tail = -16, .tip = 110, .reach = 18},
	{.tail = -18, .tip = 174, .reach = 9},
};

/* Footprint regions per hand, for the per-row span solver: along-hand range
 * and half-width (including the shadow offset). A row is only shaded where
 * it crosses one of these.
 */
struct region {
	float s0, s1, reach;
};

static const struct region sec_regions[] = {
	{-40, 196, 6}, {118, 142, 13.5f}, {-40, -24, 11.5f}, {-8, 8, 11.5f},
};
static const struct region vh_regions[3][4] = {
	{{-10, 150, 6}, {144, 175, 18}, {-9, 9, 13}, {0, 0, -1}},           /* GMT */
	{{-16, 56, 9}, {53, 91, 22}, {85, 109, 14}, {-10, 10, 14}},         /* hour */
	{{-18, 173, 12}, {-10, 10, 14}, {0, 0, -1}, {0, 0, -1}},            /* minute */
};

static void lin_range(float a, float b, float t, float *lo, float *hi);

/* Per-row x interval [lo, hi] covering the given regions of a hand at (sn, cs). */
static void region_span(const struct region *rg, int n, float sn, float cs, float ry, int x0,
			int x1, int *a, int *b);

#define GOLD_R 205
#define GOLD_G 200
#define GOLD_B 190
#define LUME_R 236
#define LUME_G 240
#define LUME_B 222

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
{
	float body = 0, lume = 0, red = 0, sh;
	/* shadow: same outline 2.5 px left, 3 px up in screen space, i.e. a fixed
	 * offset; approximated in hand space by shifting s,p a little
	 */
	float ss = s - 1.5f, pp = p - 1.5f;

	switch (k) {
	case 0: /* GMT: thin shaft, arrow with red insert, hub */
		body = fmx(bar(s, p, vhands[0].tail, 150, 1.5f), tri(s, p, 146, 174, 13));
		red = tri(s, p, 148.5f, 167, 9);
		body = fmx(body, disc(s * s + p * p, 8, 1.0f / 17));
		sh = fmx(bar(ss, pp, vhands[0].tail, 150, 1.5f), tri(ss, pp, 146, 174, 13));
		break;
	case 1: { /* hour: shaft, Mercedes circle (ring, lume, 3 spokes), tip */
		float dc2 = (s - 72) * (s - 72) + p * p;
		float ring = disc(dc2, 17, 1.0f / 35);

		body = fmx(bar(s, p, vhands[1].tail, 56, 4), ring);
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
		sh = fmx(bar(ss, pp, vhands[1].tail, 56, 4), disc((ss - 72) * (ss - 72) + pp * pp, 17, 1.0f / 35));
		break;
	}
	default: /* minute: sword (shaft + tip), lume stripe, hub */
		body = fmx(bar(s, p, vhands[2].tail, 150, 7), tri(s, p, 150, 172, 7));
		body = fmx(body, disc(s * s + p * p, 9, 1.0f / 19));
		lume = fmx(bar(s, p, 30, 148, 3.5f), tri(s, p, 148, 162, 3.5f));
		sh = fmx(bar(ss, pp, vhands[2].tail, 150, 7), tri(ss, pp, 150, 172, 7));
		break;
	}
	if (sh <= 0 && body <= 0) {
		return false;
	}
	if (sh > 0) {
		over(c, 0, 0, 0, (int)(sh * 120));
	}
	if (body > 0) {
		over(c, GOLD_R, GOLD_G, GOLD_B, (int)(body * 255));
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
static int16_t hb_x0[3], hb_y0[3], hb_x1[3], hb_y1[3];

static void hand_bbox(int k)
{
	const struct img_hand *hd = img_hands[k];
	const struct vhand *vh = &vhands[k];
	float r = vh->reach + 6;
	float ex[2] = {vh->tail, vh->tip};
	float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;

	for (int i = 0; i < 2; i++) {
		float X = ex[i] * hd->sn, Y = -ex[i] * hd->cs;

		mnx = fmn(mnx, X);
		mxx = fmx(mxx, X);
		mny = fmn(mny, Y);
		mxy = fmx(mxy, Y);
	}
	hb_x0[k] = (int16_t)(CX + mnx - r);
	hb_x1[k] = (int16_t)(CX + mxx + r);
	hb_y0[k] = (int16_t)(CX + mny - r);
	hb_y1[k] = (int16_t)(CX + mxy + r);
}

static void region_span(const struct region *rg, int n, float sn, float cs, float ry, int x0,
			int x1, int *a, int *b)
{
	float lo_all = 1e9f, hi_all = -1e9f;

	for (int i = 0; i < n; i++) {
		if (rg[i].reach < 0) {
			continue;
		}
		float lo = x0, hi = x1;
		float mid = (rg[i].s0 + rg[i].s1) / 2, half = (rg[i].s1 - rg[i].s0) / 2 + 2;

		lin_range(cs, ry * sn, rg[i].reach, &lo, &hi);
		lin_range(sn, -ry * cs - mid, half, &lo, &hi);
		if (hi >= lo) {
			lo_all = fmn(lo_all, lo);
			hi_all = fmx(hi_all, hi);
		}
	}
	*a = MAX(x0, (int)floorf(lo_all));
	*b = MIN(x1, (int)ceilf(hi_all));
}

/* x range (pixel indices, inclusive) where |a*X + b| < t, X = x + 0.5 - CX */
static void lin_range(float a, float b, float t, float *lo, float *hi)
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
	*lo = fmx(*lo, r0 + CX - 0.5f);
	*hi = fmn(*hi, r1 + CX - 0.5f);
}

static inline uint16_t to565(const struct rgb *c)
{
	return ((c->r & 0xf8) << 8) | ((c->g & 0xfc) << 3) | (c->b >> 3);
}

static void IRAM_ATTR render_box(int x0, int y0, int x1, int y1)
{
	uint32_t pt0 = k_cycle_get_32();
	const uint16_t *dial = (const uint16_t *)watch_dial;
	const int w = x1 - x0 + 1, h = y1 - y0 + 1;
	const int dx0 = WATCH_DATE_CX - WATCH_DATE_W / 2, dy0 = WATCH_DATE_CY - WATCH_DATE_H / 2;
	const uint16_t *date = date_day ? (const uint16_t *)watch_dates +
					  (date_day - 1) * WATCH_DATE_W * WATCH_DATE_H : NULL;
	bool hand_on[3];
	/* does this box reach the bezel ring at all? (corners vs radii) */
	float fxm = fmx(fabsf(x0 - CX), fabsf(x1 + 1 - CX)), fym = fmx(fabsf(y0 - CX), fabsf(y1 + 1 - CX));
	bool ring = fxm * fxm + fym * fym > (WATCH_BEZEL_IN - 1) * (WATCH_BEZEL_IN - 1);

	for (int k = 0; k < 3; k++) {
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
		/* 3. hour, minute and GMT hands, drawn as vector shapes (see vhand_px) */
		for (int k = 0; k < 3; k++) {
			const struct vhand *vh = &vhands[k];
			const struct img_hand *hd = img_hands[k];

			if (!hand_on[k] || y < hb_y0[k] || y > hb_y1[k]) {
				continue;
			}
			/* where this row crosses the hand's footprint (plus shadow):
			 * p = X cs + ry sn within +-reach, s = X sn - ry cs within the length
			 */
			int a, b;

			ARG_UNUSED(vh);
			region_span(vh_regions[k], 4, hd->sn, hd->cs, ry, x0, x1, &a, &b);
			if (b < a) {
				continue;
			}
			float rx = a + 0.5f - CX;
			float hs = rx * hd->sn - ry * hd->cs, hp = rx * hd->cs + ry * hd->sn;

			for (int x = a; x <= b; x++) {
				struct rgb c = from565(o[x - x0]);

				if (vhand_px(k, hs, hp, &c)) {
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

		region_span(sec_regions, ARRAY_SIZE(sec_regions), sec_sn, sec_cs, ry, x0, x1, &a, &b);
		float rx = a + 0.5f - CX;
		float ss = rx * sec_sn - ry * sec_cs, sp = rx * sec_cs + ry * sec_sn;
		/* shadow sample point: 2.5 px left, 3 px up */
		const float dss = -2.5f * sec_sn + 3.0f * sec_cs, dsp = -2.5f * sec_cs - 3.0f * sec_sn;

		for (int x = a; x <= b; x++, ss += sec_sn, sp += sec_cs) {
			float hs = ss + dss, hp = sp + dsp;
			/* plain shaft (away from dot, counterweight, cap): cheap path */
			bool plain = (ss > 12 && ss < SEC_DOT - 12 && hs > 12 && hs < SEC_DOT - 12) ||
				     (ss > SEC_DOT + 12 && ss < SEC_TIP - 2 && hs > SEC_DOT + 12 &&
				      hs < SEC_TIP - 2);
			float aw, ag = 0, al = 0, sh;

			if (plain) {
				if (fabsf(hp) > 3.0f && fabsf(sp) > 2.0f) {
					continue; /* neither hand nor shadow here */
				}
				sh = clamp01(0.5f - (fabsf(hp) - SEC_HALFW));
				aw = clamp01(0.5f - (fabsf(sp) - SEC_HALFW));
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

	display_write(disp, x0, y0, &desc, buf);
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
static void mark_hand(float sn, float cs, float tail, float tip, int r)
{
	for (float s = tail; s <= tip; s += 3.0f) {
		mark_point(CX + s * sn, CX - s * cs, r);
	}
	mark_point(CX + tip * sn, CX - tip * cs, r);
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

static void set_hand(struct img_hand *hd, int32_t a)
{
	hd->ang = a;
}

int main(void)
{
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);
	if (!device_is_ready(disp)) {
		return 0;
	}

	/* first frame: everything */
	for (int k = 0; k < 3; k++) {
		angle_sc(0, &img_hands[k]->sn, &img_hands[k]->cs);
		hand_bbox(k);
	}
	angle_sc(0, &bez_sn, &bez_cs);
	sec_sn = 0;
	sec_cs = 1;
	for (int y = 0; y < N; y += BAND) {
		render_box(0, y, N - 1, MIN(y + BAND - 1, N - 1));
	}
	display_blanking_off(disp);
	bezel_shown = 0;

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
	uint32_t frames = 0, busy_us = 0, last_stats = k_uptime_get_32(), max_us = 0;

	while (1) {
		wait_te(have_te);
		uint32_t t0 = k_cycle_get_32();

		bezel_touch();
		bands_clear();

		if (base_ms >= 0) {
			int64_t ms = k_uptime_get() - base_ms;
			int64_t day_ms = ((int64_t)base.hour * 3600 + base.min * 60 + base.sec) * 1000 +
					 ms;

			day_ms %= 24LL * 3600 * 1000;
			int32_t sec_a = (int32_t)((day_ms % 60000) * 3600 / 60000);

			set_hand(&minute, (int32_t)((day_ms % 3600000) * 3600 / 3600000));
			set_hand(&hour, (int32_t)((day_ms % 43200000) * 3600 / 43200000));
			set_hand(&gmt, (int32_t)(day_ms * 3600 / 86400000));
			date_day = base.day;

			/* second hand: every frame, from the clock */
			if (sec_a != sec_shown) {
				if (sec_shown >= 0) {
					mark_hand(sec_sn, sec_cs, SEC_TAIL, SEC_TIP, SEC_R);
				}
				float r = sec_a * (3.14159265f / 1800.0f);

				sec_sn = sinf(r);
				sec_cs = cosf(r);
				mark_hand(sec_sn, sec_cs, SEC_TAIL, SEC_TIP, SEC_R);
				sec_shown = sec_a;
			}
		}
		/* the other hands only when they actually move */
		for (int k = 0; k < 3; k++) {
			struct img_hand *hd = img_hands[k];
			int r = hd->w / 2 + 4;
			float tip = hd->pvy + 2, tail = -(hd->h - hd->pvy) - 2;

			if (hd->ang == hd->shown) {
				continue;
			}
			if (hd->shown >= 0) {
				mark_hand(hd->sn, hd->cs, tail, tip, r);
			} else {
				mark_hand(0, 1, tail, tip, r); /* the first frame drew it at 12 */
			}
			angle_sc(hd->ang, &hd->sn, &hd->cs);
			hand_bbox(k);
			mark_hand(hd->sn, hd->cs, tail, tip, r);
			hd->shown = hd->ang;
		}
		if (date_day != date_shown) {
			mark_rect(WATCH_DATE_CX - WATCH_DATE_W / 2, WATCH_DATE_CY - WATCH_DATE_H / 2,
				  WATCH_DATE_CX + WATCH_DATE_W / 2, WATCH_DATE_CY + WATCH_DATE_H / 2);
			date_shown = date_day;
		}
		bool ring = bezel_rot != bezel_shown;

		if (ring) {
			angle_sc(bezel_rot, &bez_sn, &bez_cs);
			bezel_shown = bezel_rot;
		}

		render_bands();
		if (ring) {
			render_ring();
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
					       .sec_px = st_secpx / f};

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
