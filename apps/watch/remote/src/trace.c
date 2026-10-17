/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include "trace.h"
#include "../../common/watch_msg.h"

#ifdef CONFIG_WATCH_TRACE

/* Two batches: one filling while the other is sent. Events are added from
 * the main loop and from the IPM interrupt, so the index is guarded.
 */
static struct msg_trace batch[2];
static int cur, dropped;
volatile bool trace_on;

static const struct device *const ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));

void trace_put(uint8_t ev, uint8_t a, uint16_t b)
{
	uint32_t key = irq_lock();
	struct msg_trace *m = &batch[cur];

	if (m->n < TRACE_BATCH) {
		m->ev[m->n++] = (struct trace_ev){.ts = trace_now(), .ev = ev, .a = a, .b = b};
	} else {
		dropped++; /* the main loop has not flushed yet */
	}
	irq_unlock(key);
}

void trace_flush_room(int room)
{
	if (batch[cur].n + room <= TRACE_BATCH && trace_on) {
		return; /* enough room left (when stopping, send what there is) */
	}
	uint32_t key = irq_lock();
	int send = cur;

	cur ^= 1;
	batch[cur].n = 0;
	irq_unlock(key);
	if (batch[send].n) {
		msg_send(ipm_dev, MSG_TRACE, &batch[send],
			 1 + batch[send].n * sizeof(struct trace_ev));
		batch[send].n = 0;
	}
}

void trace_set(bool on)
{
	uint32_t key = irq_lock();

	if (on && !trace_on) {
		batch[0].n = batch[1].n = 0;
		cur = 0;
		dropped = 0;
	}
	trace_on = on;
	irq_unlock(key);
}

#endif
