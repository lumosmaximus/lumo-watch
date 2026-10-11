/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

/* Start sending a w x h RGB565 (big-endian) window to the CO5300 at (x, y).
 * Returns once the pixel DMA is running; buf must stay untouched until the
 * next qf_write() or qf_wait() returns.
 */
void qf_write(int x, int y, int w, int h, const void *buf);

/* Wait for the last transfer to finish. */
void qf_wait(void);
