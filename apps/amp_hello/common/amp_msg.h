/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Messages between core 0 and core 1 (shared by both images).
 */

#ifndef AMP_MSG_H_
#define AMP_MSG_H_

#include <stdint.h>

#define AMP_MAGIC     0x4c554d4fu /* "LUMO" */
#define AMP_MSG_PING  1
#define AMP_MSG_PONG  2

struct amp_ping {
	uint32_t magic;
	uint32_t seq;
	uint32_t core0_ms;
};

struct amp_pong {
	uint32_t seq;
	uint32_t core1_ms;
	uint32_t core_id;  /* from the PRID register: 0 = PROCPU, 1 = APPCPU */
};

#endif
