/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hynitron CST9217 capacitive touch controller.
 *
 * Protocol (from the vendor's Arduino/ESP-IDF driver, SensorLib's
 * TouchDrvCST92xx, re-implemented here):
 *   - registers are 16-bit, sent big-endian; a read is "write the register,
 *     then read"
 *   - touch report: write 0xD000, read 15 bytes:
 *       [0..4]  finger 0: id<<4 | event, X[11:4], Y[11:4], X[3:0]<<4 | Y[3:0], pressure
 *       [5]     number of fingers (bit 7: key)
 *       [6]     0xAB when the report is valid
 *       [7..11] finger 1 (follows two extra bytes)
 *     event 0x06 = finger down; then write D0 00 AB to acknowledge
 *   - the INT line pulses on first contact, then only about once a second
 *     while the finger stays down, so after an interrupt we poll every
 *     CONFIG_INPUT_CST9217_POLL_MS until the finger lifts.
 *
 * Only the first finger is reported (this is a pointer, not multi-touch).
 */

#define DT_DRV_COMPAT hynitron_cst9217

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(cst9217, CONFIG_INPUT_LOG_LEVEL);

#define CST9217_REG_REPORT  0xD000
#define CST9217_REG_CHIP_ID 0xD204 /* after reset: project id (16 LE), chip type (16 LE) */
#define CST9217_ACK         0xAB
#define CST9217_EVT_DOWN    0x06
#define CST9217_REPORT_LEN  15

struct cst9217_config {
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec int_gpio;
	struct gpio_dt_spec reset_gpio;
};

INPUT_TOUCH_STRUCT_CHECK(struct cst9217_config);

struct cst9217_data {
	const struct device *dev;
	struct k_work_delayable work;
	struct gpio_callback int_cb;
	bool down;
};

static int cst9217_read_reg(const struct device *dev, uint16_t reg, uint8_t *buf, size_t len)
{
	const struct cst9217_config *cfg = dev->config;
	uint8_t addr[2];

	sys_put_be16(reg, addr);
	return i2c_write_read_dt(&cfg->i2c, addr, sizeof(addr), buf, len);
}

/* Returns true if a finger is down (and reports it). */
static bool cst9217_process(const struct device *dev)
{
	const struct cst9217_config *cfg = dev->config;
	struct cst9217_data *data = dev->data;
	uint8_t buf[CST9217_REPORT_LEN];
	const uint8_t ack[3] = {0xD0, 0x00, CST9217_ACK};
	bool down = false;

	if (cst9217_read_reg(dev, CST9217_REG_REPORT, buf, sizeof(buf)) != 0) {
		return data->down; /* bus hiccup: keep the previous state, poll again */
	}
	(void)i2c_write_dt(&cfg->i2c, ack, sizeof(ack));

	uint8_t fingers = buf[5] & 0x7f;

	if (buf[6] == CST9217_ACK && fingers > 0 && (buf[0] & 0x0f) == CST9217_EVT_DOWN) {
		uint16_t x = (buf[1] << 4) | (buf[3] >> 4);
		uint16_t y = (buf[2] << 4) | (buf[3] & 0x0f);

		input_touchscreen_report_pos(dev, x, y, K_FOREVER);
		input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
		down = true;
	} else if (data->down) {
		input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
	}
	data->down = down;
	return down;
}

static void cst9217_work_fn(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct cst9217_data *data = CONTAINER_OF(dw, struct cst9217_data, work);

	if (cst9217_process(data->dev)) {
		k_work_reschedule(dw, K_MSEC(CONFIG_INPUT_CST9217_POLL_MS));
	}
}

static void cst9217_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct cst9217_data *data = CONTAINER_OF(cb, struct cst9217_data, int_cb);

	k_work_reschedule(&data->work, K_NO_WAIT);
}

static int cst9217_init(const struct device *dev)
{
	const struct cst9217_config *cfg = dev->config;
	struct cst9217_data *data = dev->data;
	uint8_t id[4];
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c) || !gpio_is_ready_dt(&cfg->int_gpio) ||
	    !gpio_is_ready_dt(&cfg->reset_gpio)) {
		return -ENODEV;
	}
	data->dev = dev;
	k_work_init_delayable(&data->work, cst9217_work_fn);

	/* reset pulse, then give the firmware time to start */
	gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
	k_msleep(10);
	gpio_pin_set_dt(&cfg->reset_gpio, 0);
	k_msleep(50);

	/* Chip ID is readable right after reset (0xD101 enters debug mode). */
	const uint8_t debug[2] = {0xD1, 0x01};

	(void)i2c_write_dt(&cfg->i2c, debug, sizeof(debug));
	k_msleep(10);
	ret = cst9217_read_reg(dev, CST9217_REG_CHIP_ID, id, sizeof(id));
	if (ret == 0) {
		LOG_INF("chip type 0x%04x, project 0x%04x", sys_get_le16(&id[2]),
			sys_get_le16(&id[0]));
	} else {
		LOG_WRN("chip id read failed (%d)", ret);
	}
	/* back to normal reporting */
	const uint8_t normal[2] = {0xD1, 0x09};

	(void)i2c_write_dt(&cfg->i2c, normal, sizeof(normal));

	ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
	if (ret) {
		return ret;
	}
	gpio_init_callback(&data->int_cb, cst9217_isr, BIT(cfg->int_gpio.pin));
	ret = gpio_add_callback(cfg->int_gpio.port, &data->int_cb);
	if (ret) {
		return ret;
	}
	return gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
}

#define CST9217_DEFINE(inst)                                                                 \
	static const struct cst9217_config cst9217_config_##inst = {                       \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(inst),                    \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                         \
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                        \
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                    \
	};                                                                                 \
	static struct cst9217_data cst9217_data_##inst;                                    \
	DEVICE_DT_INST_DEFINE(inst, cst9217_init, NULL, &cst9217_data_##inst,              \
			      &cst9217_config_##inst, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(CST9217_DEFINE)
