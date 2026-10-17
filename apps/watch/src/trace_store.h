/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../common/trace.h"

extern volatile bool trace_dumping;
void trace_rec(uint8_t ev, uint8_t a, uint16_t b);   /* a core 0 event */
void trace_batch(const volatile void *msg, int len);  /* MSG_TRACE from core 1 (ISR) */
void trace_start(void);                               /* T1: clear, tell core 1 */
void trace_stop_dump(void);                           /* T0: tell core 1, print */
