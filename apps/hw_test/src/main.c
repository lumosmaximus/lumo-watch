/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware test, core 0 side. Checks every peripheral once at boot, then
 * prints a live status line every second:
 *
 *   I2C scan, AXP2101 power/battery, PCF85063 RTC, QMI8658 IMU, TF card,
 *   ES7210 microphones (level per mic), BOOT and PWR buttons, CST9217 touch,
 *   and core 1 alive (IPM ping).
 *
 * Speaker output is not tested (no speaker connected); the ES8311 is set up
 * with its amp off.
 */

#include <math.h>
#include <string.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/fs.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>

#include "../common/amp_msg.h"

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));
static const struct device *const exio = DEVICE_DT_GET(DT_NODELABEL(exio));
static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s_audio));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(es7210));
static const struct device *const dac = DEVICE_DT_GET(DT_NODELABEL(es8311));

#define PASS(ok) ((ok) ? "OK  " : "FAIL")

/* ---- input: BOOT button and touch --------------------------------------- */

static atomic_t boot_presses, touches;
static atomic_t touch_x = ATOMIC_INIT(-1), touch_y = ATOMIC_INIT(-1), touch_down;

static void on_input(struct input_event *evt, void *user_data)
{
	switch (evt->code) {
	case INPUT_KEY_0:
		if (evt->value) {
			atomic_inc(&boot_presses);
			printk("  [BOOT pressed]\n");
		}
		break;
	case INPUT_ABS_X:
		atomic_set(&touch_x, evt->value);
		break;
	case INPUT_ABS_Y:
		atomic_set(&touch_y, evt->value);
		break;
	case INPUT_BTN_TOUCH:
		if (evt->value && !atomic_get(&touch_down)) {
			atomic_inc(&touches);
		}
		atomic_set(&touch_down, evt->value);
		if (evt->sync) {
			struct amp_touch t = {.x = atomic_get(&touch_x), .y = atomic_get(&touch_y),
					      .down = evt->value};

			/* core 1 draws a dot there: proves touch and display agree */
			ipm_send(ipm, -1, AMP_MSG_TOUCH, &t, sizeof(t));
			if (!evt->value) {
				printk("  [touch up at %u,%u]\n", t.x, t.y);
			}
		}
		break;
	default:
		break;
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* ---- core 1 ----------------------------------------------------------------- */

static struct amp_pong pong;
static K_SEM_DEFINE(pong_sem, 0, 1);

static void on_ipm(const struct device *dev, void *user_data, uint32_t id, volatile void *data)
{
	memcpy(&pong, (const void *)data, sizeof(pong));
	k_sem_give(&pong_sem);
}

static bool core1_alive(void)
{
	static uint32_t seq;
	struct amp_ping ping = {.magic = AMP_MAGIC, .seq = ++seq, .core0_ms = k_uptime_get_32()};

	k_sem_reset(&pong_sem);
	ipm_send(ipm, -1, AMP_MSG_PING, &ping, sizeof(ping));
	/* core 1 may still be drawing/benchmarking; replies can lag */
	return k_sem_take(&pong_sem, K_MSEC(1500)) == 0 && pong.core_id == 1;
}

/* ---- AXP2101 (direct registers, see apps/amp_hello for the map) ----------- */

static int axp_rd(uint8_t reg, uint8_t *v)
{
	return i2c_reg_read_byte(i2c, 0x34, reg, v);
}

static void axp_status(char *out, size_t len)
{
	uint8_t s0 = 0, s1 = 0, hi = 0, lo = 0, pct = 0;

	axp_rd(0x00, &s0);
	axp_rd(0x01, &s1);
	bool vbus = s0 & BIT(5), bat = s0 & BIT(3);
	static const char *const dir[] = {"idle", "charging", "discharging", "?"};

	if (bat && axp_rd(0x34, &hi) == 0 && axp_rd(0x35, &lo) == 0 && axp_rd(0xa4, &pct) == 0) {
		snprintf(out, len, "USB %s, battery %u mV %u%% %s", vbus ? "yes" : "no",
			 ((hi & 0x3f) << 8) | lo, pct, dir[(s1 >> 5) & 3]);
	} else {
		snprintf(out, len, "USB %s, no battery", vbus ? "yes" : "no");
	}
}

/* ---- TF card ------------------------------------------------------------------ */

static FATFS fat;
static struct fs_mount_t sd_mnt = {.type = FS_FATFS, .fs_data = &fat, .mnt_point = "/SD:"};

static bool sd_test(void)
{
	uint32_t sectors = 0, sector_size = 0;
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int n = 0;

	if (disk_access_init("SD") != 0) {
		printk("  TF card:   FAIL  no card (or init failed)\n");
		return false;
	}
	disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
	disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	if (fs_mount(&sd_mnt) != 0) {
		printk("  TF card:   FAIL  %u MB card, but no FAT filesystem\n",
		       (unsigned int)((uint64_t)sectors * sector_size >> 20));
		return false;
	}
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, "/SD:") == 0) {
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] && n < 5) {
			printk("               %s %s\n", ent.type == FS_DIR_ENTRY_DIR ? "[dir]" : "     ",
			       ent.name);
			n++;
		}
		fs_closedir(&dir);
	}
	printk("  TF card:   OK    %u MB (SDMMC 1-bit), first %d entries above\n",
	       (unsigned int)((uint64_t)sectors * sector_size >> 20), n);
	return true;
}

/* ---- microphones -------------------------------------------------------------- */

#define RATE        16000
#define FRAMES      512
#define BLOCK_BYTES (FRAMES * 2 * 2)
#define BLOCKS      (CONFIG_I2S_ESP32_RX_BLOCK_COUNT + CONFIG_I2S_ESP32_TX_BLOCK_COUNT + 2)
K_MEM_SLAB_DEFINE_STATIC(audio_slab, BLOCK_BYTES, BLOCKS, 4);

static bool mic_ok;
static float mic_db[2] = {-99, -99};

static bool audio_start(void)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = RATE,
		.mem_slab = &audio_slab,
		.block_size = BLOCK_BYTES,
		.timeout = 1000,
	};
	struct audio_codec_cfg ccfg = {.mclk_freq = RATE * 256, .dai_type = AUDIO_DAI_TYPE_I2S,
				       .dai_cfg.i2s = cfg};
	void *blk;

	if (!device_is_ready(i2s) || !device_is_ready(adc) || !device_is_ready(dac) ||
	    i2s_configure(i2s, I2S_DIR_BOTH, &cfg) != 0) {
		return false;
	}
	ccfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	if (audio_codec_configure(dac, &ccfg) != 0) {
		printk("  ES8311 configure failed\n");
	}
	ccfg.dai_route = AUDIO_ROUTE_CAPTURE;
	if (audio_codec_configure(adc, &ccfg) != 0) {
		return false;
	}
	for (int i = 0; i < 2; i++) { /* silence out, so the TX side has data */
		if (k_mem_slab_alloc(&audio_slab, &blk, K_NO_WAIT) == 0) {
			memset(blk, 0, BLOCK_BYTES);
			i2s_write(i2s, blk, BLOCK_BYTES);
		}
	}
	if (i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START) != 0) {
		return false;
	}
	return audio_codec_start(adc, AUDIO_DAI_DIR_RX) == 0;
}

/* Reads mic blocks forever, keeps the latest level of MIC1 (left) and MIC2 (right). */
static void mic_thread(void *a, void *b, void *c)
{
	if (!audio_start()) {
		printk("  mics:      FAIL  I2S/ES7210 start\n");
		return;
	}
	mic_ok = true;
	while (1) {
		void *rx, *tx;
		size_t size;
		double sum[2] = {0};

		if (i2s_read(i2s, &rx, &size) != 0) {
			continue;
		}
		const int16_t *s = rx;

		for (int i = 0; i < FRAMES; i++) {
			sum[0] += (double)s[2 * i] * s[2 * i];
			sum[1] += (double)s[2 * i + 1] * s[2 * i + 1];
		}
		k_mem_slab_free(&audio_slab, rx);
		for (int c = 0; c < 2; c++) {
			double rms = sqrt(sum[c] / FRAMES) / 32768.0;

			mic_db[c] = rms > 1e-5 ? 20.0f * log10f((float)rms) : -99.0f;
		}
		if (k_mem_slab_alloc(&audio_slab, &tx, K_MSEC(50)) == 0) {
			memset(tx, 0, BLOCK_BYTES);
			if (i2s_write(i2s, tx, BLOCK_BYTES) != 0) {
				k_mem_slab_free(&audio_slab, tx);
			}
		}
	}
}
K_THREAD_DEFINE(mic_tid, 4096, mic_thread, NULL, NULL, NULL, 2, K_FP_REGS, 1500);

/* ---- main --------------------------------------------------------------------- */

int main(void)
{
	struct rtc_time t;
	int found = 0;

	printk("\n*** hw_test: ESP32-S3-Touch-AMOLED-1.75, core 0 ***\n");

	for (uint8_t a = 0x08; a < 0x78; a++) {
		uint8_t b;

		found += i2c_read(i2c, &b, 1, a) == 0;
	}
	printk("  I2C bus:   %s  %d devices (expect 7)\n", PASS(found == 7), found);

	char pw[64];

	axp_status(pw, sizeof(pw));
	printk("  AXP2101:   OK    %s\n", pw);

	int rret = device_is_ready(rtc) ? rtc_get_time(rtc, &t) : -ENODEV;

	if (rret == 0) {
		printk("  RTC:       OK    %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900,
		       t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	} else if (rret == -ENODATA) {
		/* The chip answered, but its oscillator-stop flag says the time was never
		 * set since power-up. Set it now so the next boot reads a real time.
		 */
		struct rtc_time set = {.tm_year = 2026 - 1900, .tm_mon = 9, .tm_mday = 10,
				       .tm_hour = 12, .tm_wday = 6};

		printk("  RTC:       OK    responds, time not set yet -> setting %s\n",
		       rtc_set_time(rtc, &set) == 0 ? "ok" : "FAILED");
	} else {
		printk("  RTC:       FAIL  (%d)\n", rret);
	}

	bool imu_ok = device_is_ready(imu) && sensor_sample_fetch(imu) == 0;

	printk("  IMU:       %s\n", PASS(imu_ok));
	printk("  expander:  %s\n", PASS(device_is_ready(exio)));
	printk("  touch:     %s  (touch the screen to test)\n",
	       PASS(device_is_ready(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)))));
	sd_test();

	ipm_register_callback(ipm, on_ipm, NULL);
	ipm_set_enabled(ipm, 1);
	k_msleep(50);
	bool c1 = core1_alive();

	printk("  core 1:    %s\n", PASS(c1));
	if (c1) {
		printk("  display:   %s  (core 1: CO5300 test pattern, err %d, %u ms)\n",
		       PASS(pong.disp_err == 0), pong.disp_err, pong.draw_ms);
		k_msleep(2500); /* core 1 runs its benchmark after drawing */
		core1_alive();
		printk("  display:   full 466x466 frame sent in %u us = %u fps max (bus only)\n",
		       pong.bench_us, pong.bench_us ? 1000000 / pong.bench_us : 0);
	}
	gpio_pin_configure(exio, 4, GPIO_INPUT); /* SYS_OUT: PWR key, high while pressed */
	k_msleep(1500);
	printk("  mics:      %s  levels below (talk or tap near the mics)\n\n", PASS(mic_ok));

	while (1) {
		struct sensor_value acc[3], gyr[3];
		int pwr = gpio_pin_get(exio, 4);

		if (imu_ok && sensor_sample_fetch(imu) == 0) {
			sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, acc);
			sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyr);
		}
		axp_status(pw, sizeof(pw));
		printk("mic1 %5.1f mic2 %5.1f dBFS | accel %5.2f %5.2f %5.2f | gyro %5.2f %5.2f %5.2f"
		       " | BOOT x%ld PWR %s | touches %ld | core1 %s | %s\n",
		       (double)mic_db[0], (double)mic_db[1],
		       sensor_value_to_double(&acc[0]), sensor_value_to_double(&acc[1]),
		       sensor_value_to_double(&acc[2]), sensor_value_to_double(&gyr[0]),
		       sensor_value_to_double(&gyr[1]), sensor_value_to_double(&gyr[2]),
		       atomic_get(&boot_presses), pwr > 0 ? "DOWN" : "up", atomic_get(&touches),
		       core1_alive() ? "ok" : "--", pw);
		printk("    core1 uptime %u ms, frame %u us\n", pong.core1_ms, pong.bench_us);
		k_msleep(1000);
	}
	return 0;
}
