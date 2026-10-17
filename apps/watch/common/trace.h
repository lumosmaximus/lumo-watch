/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Event trace shared by both cores (tools/trace reads the dump).
 *
 * Every event is 8 bytes: a timestamp from the chip's system timer, which
 * both cores read (16 MHz, so 62.5 ns steps and one shared timeline), an
 * event id, and two small arguments. Core 1 collects its events in batches
 * and sends them to core 0 (MSG_TRACE), which keeps the whole trace in its
 * larger RAM and dumps it over the USB console on request:
 *
 *   T1   start recording (both cores)
 *   T0   stop and dump: lines "TR <base64 of 60 events>", then "TR end"
 */

#ifndef WATCH_TRACE_H_
#define WATCH_TRACE_H_

#include <stdint.h>
#include <soc/systimer_struct.h>
#include <hal/systimer_ll.h>

struct trace_ev {
	uint32_t ts; /* system timer ticks (16 MHz), wraps every 268 s */
	uint8_t ev;  /* TR_* below */
	uint8_t a;
	uint16_t b;
} __packed;

#define TRACE_HZ    16000000
#define TRACE_BATCH 60 /* events per MSG_TRACE message (480 bytes) */

struct msg_trace {
	uint8_t n;
	struct trace_ev ev[TRACE_BATCH];
} __packed;

struct msg_trace_ctl {
	uint8_t on;
};

enum {
	/* core 1 (display) */
	TR_TE = 1,         /* panel tearing-effect edge seen */
	TR_FRAME_BEGIN,    /* a = face, b = frame counter */
	TR_BOX_BEGIN,      /* a = band (y0 / 8), b = x0: composing a box */
	TR_BOX_END,        /* a = rows, b = width: composed, handed to the SPI */
	TR_SPI_WAIT,       /* CPU stalled waiting for the previous DMA transfer: b = ticks */
	TR_UNUSED6,
	TR_RING_BEGIN,     /* bezel ring repaint */
	TR_RING_END,
	TR_FRAME_END,      /* a = boxes, b = frame work in 10 us units */
	TR_MSG_RX,         /* a = message id from core 0 */
	TR_KEY_TX,         /* a = media key asked of core 0 */
	TR_TEXT_RX,        /* a now-playing text frame complete */
	TR_FACE,           /* a = face shown */
	/* core 0 (sensors, USB) */
	TR_TOUCH = 0x40,   /* a = down | fingers << 1, b = (x/4) << 8 | y/4 */
	TR_BUTTON,         /* a = BTN_* */
	TR_HID_KEY,        /* a = key sent to the computer, b = 1 if host ready */
	TR_HOST_STATE,     /* a = playing | muted << 1, b = volume */
	TR_TEXT_LINE,      /* a now-playing text line decoded (a = 1 ok) */
	TR_TIME_TX,        /* time sent to core 1 */
	TR_MSG_TX,         /* a = message id sent to core 1 */
	TR_TRACE_ON,       /* recording started (first event of a dump) */
};

static inline uint32_t trace_now(void)
{
	systimer_ll_counter_snapshot(&SYSTIMER, 0);
	return systimer_ll_get_counter_value_low(&SYSTIMER, 0);
}

#endif
