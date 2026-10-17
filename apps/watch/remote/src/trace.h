/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core 1's side of the event trace: events go into a small staging buffer
 * (core 1 has little RAM) and are sent to core 0 in batches from the main
 * loop. Cheap when off: one load and a branch.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include "../../common/trace.h"

#ifdef CONFIG_WATCH_TRACE
extern volatile bool trace_on;
void trace_put(uint8_t ev, uint8_t a, uint16_t b);
void trace_set(bool on);  /* from MSG_TRACE_CTL */
void trace_flush(void);   /* main loop: send full batches to core 0 */

static inline void trace_ev(uint8_t ev, uint8_t a, uint16_t b)
{
	if (trace_on) {
		trace_put(ev, a, b);
	}
}
#else
static inline void trace_ev(uint8_t ev, uint8_t a, uint16_t b) {}
static inline void trace_set(bool on) {}
static inline void trace_flush(void) {}
#endif
