/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core 1 side: answer every ping with our uptime and the hardware core ID, so
 * core 0 can prove this code really runs on the second core.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <string.h>
#include <zephyr/kernel.h>

#include "../../common/amp_msg.h"

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static struct amp_ping ping;
static K_SEM_DEFINE(ping_sem, 0, 1);

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	memcpy(&ping, (const void *)data, sizeof(ping));
	k_sem_give(&ping_sem);
}

static uint32_t core_id(void)
{
	uint32_t prid;

	__asm__ volatile("rsr.prid %0" : "=a"(prid));
	return (prid & 0xffff) == 0xabab ? 1 : 0; /* ESP32-S3: 0xcdcd PROCPU, 0xabab APPCPU */
}

int main(void)
{
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);

	while (1) {
		k_sem_take(&ping_sem, K_FOREVER);
		if (ping.magic != AMP_MAGIC) {
			continue;
		}
		struct amp_pong pong = {.seq = ping.seq, .core1_ms = k_uptime_get_32(),
					.core_id = core_id()};

		ipm_send(ipm, -1, AMP_MSG_PONG, &pong, sizeof(pong));
	}
	return 0;
}
