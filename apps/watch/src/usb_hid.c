/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core 0: the USB device. One USB-C port, two interfaces:
 *   - CDC-ACM serial: the console (logs, tools/settime.py)
 *   - HID: media keys for the computer, sent when core 1 asks (MSG_KEY)
 *
 * Keys: volume up/down, mute and play/pause are standard Consumer Control
 * usages that Windows and macOS act on with no app; so are Consumer "Display
 * Brightness Increment/Decrement". (No keyboard report: a HID keyboard makes
 * macOS open its Keyboard Setup Assistant every time the watch is plugged in.)
 *
 * The chip's USB-Serial-JTAG (the usual flashing port) shares the PHY with
 * the OTG controller used here, so it is off while this runs. A console line
 * "B" reboots into the ROM's download mode (USB-Serial-JTAG again), so
 * west flash works without touching the buttons (tools/flash.sh).
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <soc/rtc_cntl_reg.h>

#include "../common/watch_msg.h"
#include "usb_hid.h"

#define REPORT_CONSUMER 1

static const uint8_t report_desc[] = {
	/* Consumer Control: one 16-bit usage */
	0x05, 0x0C,       /* Usage Page (Consumer) */
	0x09, 0x01,       /* Usage (Consumer Control) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, REPORT_CONSUMER,
	0x15, 0x00,       /*   Logical Minimum (0) */
	0x26, 0xFF, 0x03, /*   Logical Maximum (0x3FF) */
	0x19, 0x00,       /*   Usage Minimum (0) */
	0x2A, 0xFF, 0x03, /*   Usage Maximum (0x3FF) */
	0x75, 0x10,       /*   Report Size (16) */
	0x95, 0x01,       /*   Report Count (1) */
	0x81, 0x00,       /*   Input (Data, Array) */
	0xC0,             /* End Collection */

};

/* Consumer usages and keyboard keys per MSG_KEY code */
static const struct {
	uint16_t consumer;
} keymap[] = {
	[KEY_VOL_UP] = {0x00E9},
	[KEY_VOL_DOWN] = {0x00EA},
	[KEY_MUTE] = {0x00E2},
	[KEY_PLAY_PAUSE] = {0x00CD},
	[KEY_BRIGHT_UP] = {0x006F},
	[KEY_BRIGHT_DOWN] = {0x0070},
};

static const struct device *const hid_dev = DEVICE_DT_GET(DT_NODELABEL(hid_media));
static K_SEM_DEFINE(report_done, 1, 1);
static bool hid_ready;
K_MSGQ_DEFINE(key_q, sizeof(uint8_t), 16, 1);

static void iface_ready(const struct device *dev, const bool ready)
{
	hid_ready = ready;
}

static void input_report_done(const struct device *dev, const uint8_t *const report)
{
	k_sem_give(&report_done);
}

static int get_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, uint8_t *const buf)
{
	return 0;
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready,
	.get_report = get_report,
	.input_report_done = input_report_done,
};

/* one report at a time; the stack keeps the buffer until it is sent */
static uint8_t report[8] __aligned(4);

static void send(const uint8_t *r, size_t len)
{
	if (!hid_ready || k_sem_take(&report_done, K_MSEC(50)) != 0) {
		return;
	}
	memcpy(report, r, len);
	if (hid_device_submit_report(hid_dev, len, report) != 0) {
		k_sem_give(&report_done);
	}
}

static void key_thread(void *a, void *b, void *c)
{
	uint8_t k;

	while (1) {
		k_msgq_get(&key_q, &k, K_FOREVER);
		if (k == 0 || k >= ARRAY_SIZE(keymap)) {
			continue;
		}
		uint16_t u = keymap[k].consumer;
		const uint8_t press[] = {REPORT_CONSUMER, u & 0xff, u >> 8};
		const uint8_t release[] = {REPORT_CONSUMER, 0, 0};

		send(press, sizeof(press));
		k_msleep(12);
		send(release, sizeof(release));
		k_msleep(4);
	}
}
K_THREAD_DEFINE(key_tid, 1024, key_thread, NULL, NULL, NULL, 4, 0, 0);

void usb_key(uint8_t key)
{
	(void)k_msgq_put(&key_q, &key, K_NO_WAIT);
}

/* ---- device setup ---------------------------------------------------------------------- */

/* pid.codes test VID/PID: fine on your own desk, not for products */
USBD_DEVICE_DEFINE(watch_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x1209, 0x0001);
USBD_DESC_LANG_DEFINE(watch_lang);
USBD_DESC_MANUFACTURER_DEFINE(watch_mfr, "Zephyr watch");
USBD_DESC_PRODUCT_DEFINE(watch_product, "AMOLED watch remote");
USBD_DESC_CONFIG_DEFINE(watch_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(watch_config, 0, 250, &watch_cfg_desc);

int usb_start(void)
{
	int err;

	err = hid_device_register(hid_dev, report_desc, sizeof(report_desc), &hid_ops);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&watch_usbd, &watch_lang);
	err = err ?: usbd_add_descriptor(&watch_usbd, &watch_mfr);
	err = err ?: usbd_add_descriptor(&watch_usbd, &watch_product);
	err = err ?: usbd_add_configuration(&watch_usbd, USBD_SPEED_FS, &watch_config);
	err = err ?: usbd_register_all_classes(&watch_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}
	/* CDC-ACM has two interfaces: say so with the IAD class triple */
	usbd_device_set_code_triple(&watch_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	err = usbd_init(&watch_usbd);
	return err ?: usbd_enable(&watch_usbd);
}

/* Reboot into the ROM download mode on USB-Serial-JTAG (for flashing). */
void usb_reboot_to_download(void)
{
	usbd_disable(&watch_usbd);
	k_msleep(50);
	/* PHY back to USB-Serial-JTAG, and ask the ROM to stay in download mode */
	REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL);
	REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
	sys_reboot(SYS_REBOOT_COLD);
}
