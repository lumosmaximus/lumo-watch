/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watch, core 0 side: owns the RTC, touch and buttons, tells core 1 the time
 * and the button presses, and prints core 1's frame rate.
 *
 * Time: the RTC has no battery, so it loses the time when unplugged (then the
 * build time is used). Send the real time over USB with tools/settime.py: a
 * line "T YYYY-MM-DD HH:MM:SS" on the console sets the RTC. Core 1 says hello
 * when it is up; the time goes to it then (and every minute after).
 *
 * Buttons: BOOT (GPIO0, a gpio-keys input) short press, or held 3 s (face
 * picker); PWR read from the I/O expander (EXIO4, high while pressed), whose
 * interrupt line is not wired, so it is polled.
 *
 * The RTC only has 1 s resolution, so we wait for its seconds to change and
 * send the time right then: core 1 counts milliseconds from that edge, which
 * puts the gliding second hand within a few ms of the real second.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include "../common/watch_msg.h"

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));

static struct msg_stats stats;
static K_SEM_DEFINE(stats_sem, 0, 1);
static K_SEM_DEFINE(time_req, 0, 1); /* core 1 is up, or the RTC was just set */

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	if (id == MSG_BENCH) {
		struct msg_bench mb;

		memcpy(&mb, (const void *)data, sizeof(mb));
		if (mb.ring_full_us) {
			printk("core 1 bench: full bezel repaint %u us (%u px): bg %u ring %u hands %u"
			       " sweep %u swap %u write %u us\n", mb.ring_full_us, mb.ring_px,
			       mb.seq_read, mb.band_read, mb.compute, mb.internal_read,
			       mb.box_plain, mb.box_hands);
			return;
		}
		printk("core 1 bench (CPU cycles per pixel): flash sequential %u.%u, flash band "
		       "pattern %u.%u, internal RAM %u.%u, second-hand maths %u.%u\n",
		       mb.seq_read / 10, mb.seq_read % 10, mb.band_read / 10, mb.band_read % 10,
		       mb.internal_read / 10, mb.internal_read % 10, mb.compute / 10,
		       mb.compute % 10);
		printk("core 1 bench: render_box dial only %u.%u, with hands %u.%u cycles/px\n",
		       mb.box_plain / 10, mb.box_plain % 10, mb.box_hands / 10, mb.box_hands % 10);
		return;
	}
	if (id == MSG_HELLO) {
		k_sem_give(&time_req);
		return;
	}
	if (id == MSG_STATS) {
		memcpy(&stats, (const void *)data, sizeof(stats));
		k_sem_give(&stats_sem);
	}
}

static void send_button(uint8_t btn)
{
	struct msg_button mb = {.btn = btn};

	ipm_send(ipm, -1, MSG_BUTTON, &mb, sizeof(mb));
}

/* BOOT held for 3 s: fires once while still held */
static bool boot_long_sent;

static void boot_long(struct k_work *w)
{
	boot_long_sent = true;
	send_button(BTN_BOOT_LONG);
}
static K_WORK_DELAYABLE_DEFINE(boot_long_work, boot_long);

static void on_input(struct input_event *evt, void *user_data)
{
	static struct msg_touch t;

	if (evt->code == INPUT_KEY_0) { /* BOOT */
		if (evt->value) {
			boot_long_sent = false;
			k_work_reschedule(&boot_long_work, K_MSEC(3000));
		} else {
			k_work_cancel_delayable(&boot_long_work);
			if (!boot_long_sent) {
				send_button(BTN_BOOT);
			}
		}
		return;
	}

	if (evt->code == INPUT_ABS_X) {
		t.x = evt->value;
	} else if (evt->code == INPUT_ABS_Y) {
		t.y = evt->value;
	} else if (evt->code == INPUT_BTN_TOUCH) {
		t.down = evt->value;
	}
	if (evt->sync) {
		ipm_send(ipm, -1, MSG_TOUCH, &t, sizeof(t));
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* No battery: the RTC loses its time when unplugged. Fall back to the time
 * this firmware was built, so the watch is close after flashing.
 */
static void rtc_seed_from_build(void)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4];
	int d, y, h, m, s;
	struct rtc_time t = {0};

	sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
	sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
	t.tm_year = y - 1900;
	t.tm_mon = (strstr(months, mon) - months) / 3;
	t.tm_mday = d;
	t.tm_hour = h;
	t.tm_min = m;
	t.tm_sec = s;
	rtc_set_time(rtc, &t);
	printk("RTC had no time: set from build time %s %s\n", __DATE__, __TIME__);
}

static void send_time_on_edge(void)
{
	struct rtc_time a, b;

	if (rtc_get_time(rtc, &a) != 0) {
		return;
	}
	do { /* poll until the seconds roll over */
		k_msleep(2);
		if (rtc_get_time(rtc, &b) != 0) {
			return;
		}
	} while (b.tm_sec == a.tm_sec);

	struct msg_time mt = {.year = b.tm_year + 1900, .month = b.tm_mon + 1, .day = b.tm_mday,
			      .hour = b.tm_hour, .min = b.tm_min, .sec = b.tm_sec,
			      .wday = b.tm_wday};

	ipm_send(ipm, -1, MSG_TIME, &mt, sizeof(mt));
	printk("time -> core 1: %04u-%02u-%02u %02u:%02u:%02u\n", mt.year, mt.month, mt.day,
	       mt.hour, mt.min, mt.sec);
}

/* Day of week (0 = Sunday) for the RTC, Sakamoto's method. */
static int weekday(int y, int m, int d)
{
	static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

	y -= m < 3;
	return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* Console input: "T YYYY-MM-DD HH:MM:SS" sets the RTC (tools/settime.py). */
static void console_rx(void *a, void *b, void *c)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	char line[40];
	size_t n = 0;

	while (1) {
		unsigned char ch;

		if (uart_poll_in(con, &ch) != 0) {
			k_msleep(10);
			continue;
		}
		if (ch != '\n' && ch != '\r') {
			if (n < sizeof(line) - 1) {
				line[n++] = ch;
			}
			continue;
		}
		line[n] = 0;
		n = 0;
		int y, mo, d, h, mi, s;

		if (sscanf(line, "T %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
			continue;
		}
		struct rtc_time t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
				     .tm_hour = h, .tm_min = mi, .tm_sec = s,
				     .tm_wday = weekday(y, mo, d)};
		int ret = rtc_set_time(rtc, &t);

		printk("time set from USB: %04d-%02d-%02d %02d:%02d:%02d (%d)\n", y, mo, d, h, mi, s,
		       ret);
		k_sem_give(&time_req);
	}
}
K_THREAD_DEFINE(console_tid, 1536, console_rx, NULL, NULL, NULL, 6, 0, 0);

/* PWR: poll the expander every 20 ms, report presses */
static void pwr_poll(void *a, void *b, void *c)
{
	const struct device *exio = DEVICE_DT_GET(DT_NODELABEL(exio));
	bool was = false;

	if (!device_is_ready(exio) || gpio_pin_configure(exio, 4, GPIO_INPUT) != 0) {
		printk("PWR key: expander not ready\n");
		return;
	}
	while (1) {
		bool down = gpio_pin_get(exio, 4) > 0;

		if (down && !was) {
			send_button(BTN_PWR);
		}
		was = down;
		k_msleep(20);
	}
}
K_THREAD_DEFINE(pwr_tid, 1024, pwr_poll, NULL, NULL, NULL, 5, 0, 500);

int main(void)
{
	struct rtc_time t;

	printk("\n*** watch: core 0 ***\n");
	/* the panel's tearing-effect pin; core 1 polls it to pace its frames */
	gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio0)), 13, GPIO_INPUT);
	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);

	if (rtc_get_time(rtc, &t) == -ENODATA) {
		rtc_seed_from_build();
	}
	k_msleep(300);
	send_time_on_edge(); /* core 1 may not be listening yet: it says hello */

	for (int n = 1;; n++) {
		if (k_sem_take(&stats_sem, K_MSEC(2000)) == 0) {
			/* frames = frames in the last second */
			printk("core 1: %u fps, %u us/frame: %u px in %u boxes, composite %u us,"
			       " write %u us\n", stats.frames, stats.render_us, stats.px, stats.boxes,
			       stats.comp_us, stats.write_us);
			printk("   stages us: bg %u ring %u hands %u sec %u (%u px) swap %u\n",
			       stats.st_bg, stats.st_ring, stats.st_hands, stats.st_sec, stats.sec_px,
			       stats.st_swap);
			printk("   spi: %u underruns, overlap %u\n", stats.underruns, stats.overlap);
		} else {
			printk("core 1: no stats\n");
		}
		if (k_sem_take(&time_req, K_NO_WAIT) == 0 || n % 60 == 0) {
			send_time_on_edge(); /* core 1 asked, the RTC was set, or every minute */
		}
	}
	return 0;
}
