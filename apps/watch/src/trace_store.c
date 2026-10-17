/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core 0 keeps the whole trace (its RAM has room): its own events plus the
 * batches core 1 sends, in time order as they arrive. T0 prints it as
 * base64 lines of 60 events each (tools/trace/capture.py reads them).
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>

#include "../common/watch_msg.h"
#include "trace_store.h"

#define TRACE_MAX 12288 /* events: 96 KB, about 3 s of frames */

static struct trace_ev store[TRACE_MAX];
static volatile int n_ev;
static volatile bool on;
volatile bool trace_dumping; /* main.c pauses its stats lines meanwhile */
static int dropped;

static const struct device *const ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));

void trace_rec(uint8_t ev, uint8_t a, uint16_t b)
{
	if (!on) {
		return;
	}
	uint32_t key = irq_lock();

	if (n_ev < TRACE_MAX) {
		store[n_ev++] = (struct trace_ev){.ts = trace_now(), .ev = ev, .a = a, .b = b};
	} else {
		dropped++;
	}
	irq_unlock(key);
}

void trace_batch(const volatile void *msg, int len)
{
	const volatile struct msg_trace *m = msg;
	int n = m->n;

	if (!on || n > TRACE_BATCH || len < 1 + n * (int)sizeof(struct trace_ev)) {
		return;
	}
	uint32_t key = irq_lock();

	if (n_ev + n <= TRACE_MAX) {
		memcpy(&store[n_ev], (const void *)m->ev, n * sizeof(struct trace_ev));
		n_ev += n;
	} else {
		dropped += n;
	}
	irq_unlock(key);
}

static void ctl(bool enable)
{
	struct msg_trace_ctl c = {.on = enable};

	msg_send(ipm_dev, MSG_TRACE_CTL, &c, sizeof(c));
}

void trace_start(void)
{
	n_ev = 0;
	dropped = 0;
	on = true;
	trace_rec(TR_TRACE_ON, 0, 0);
	ctl(true);
	systimer_ll_counter_snapshot(&SYSTIMER, 1);
	printk("TR start now u0=%u u1=%u\n", trace_now(),
	       systimer_ll_get_counter_value_low(&SYSTIMER, 1));
}

/* straight to the console port: printk goes through the log buffer, which
 * drops lines when a dump floods it
 */
static void out(const char *str)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	for (; *str; str++) {
		uart_poll_out(con, *str);
	}
}

void trace_stop_dump(void)
{
	ctl(false);
	k_msleep(50); /* core 1 flushes its last batch between frames */
	on = false;
	trace_dumping = true;
	k_msleep(20); /* a stats line in progress finishes */
	int n = n_ev;
	static char line[TRACE_BATCH * sizeof(struct trace_ev) * 4 / 3 + 8];

	snprintk(line, sizeof(line), "TR begin %d %d %u\n", n, dropped, TRACE_HZ);
	out(line);
	for (int i = 0; i < n; i += TRACE_BATCH) {
		int k = MIN(TRACE_BATCH, n - i);
		size_t olen;

		base64_encode((uint8_t *)line, sizeof(line), &olen, (const uint8_t *)&store[i],
			      k * sizeof(struct trace_ev));
		line[olen] = '\n';
		line[olen + 1] = 0;
		out("TR ");
		out(line);
	}
	out("TR end\n");
	trace_dumping = false;
}
