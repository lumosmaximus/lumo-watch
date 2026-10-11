/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everest ES8311, DAC (speaker) path only, I2S target with MCLK input.
 *
 * Register sequences follow Espressif's esp_codec_dev es8311.c
 * (Apache-2.0, (c) 2023 Espressif Systems), trimmed to what this board uses.
 */

#define DT_DRV_COMPAT everest_es8311

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(es8311, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#define ES8311_RESET      0x00
#define ES8311_CLK_MGR1   0x01
#define ES8311_CLK_MGR2   0x02
#define ES8311_CLK_MGR3   0x03
#define ES8311_CLK_MGR4   0x04
#define ES8311_CLK_MGR5   0x05
#define ES8311_CLK_MGR6   0x06
#define ES8311_CLK_MGR7   0x07
#define ES8311_CLK_MGR8   0x08
#define ES8311_SDPIN      0x09
#define ES8311_SDPOUT     0x0A
#define ES8311_SYSTEM_0B  0x0B
#define ES8311_SYSTEM_0C  0x0C
#define ES8311_SYSTEM_0D  0x0D
#define ES8311_SYSTEM_0E  0x0E
#define ES8311_SYSTEM_10  0x10
#define ES8311_SYSTEM_11  0x11
#define ES8311_SYSTEM_12  0x12
#define ES8311_SYSTEM_13  0x13
#define ES8311_SYSTEM_14  0x14
#define ES8311_ADC_15     0x15
#define ES8311_ADC_16     0x16
#define ES8311_ADC_17     0x17
#define ES8311_ADC_1B     0x1B
#define ES8311_ADC_1C     0x1C
#define ES8311_DAC_31     0x31
#define ES8311_DAC_VOL    0x32
#define ES8311_DAC_37     0x37
#define ES8311_GPIO_44    0x44
#define ES8311_GP_45      0x45
#define ES8311_CHIP_ID1   0xFD
#define ES8311_CHIP_ID2   0xFE

#define ES8311_DAC_MUTE_BITS 0x60
/* DAC volume register: 0x00 = -95.5 dB, 0xBF = 0 dB, 0.5 dB per step. */
#define ES8311_VOL_0DB       0xBF
#define ES8311_DEFAULT_VOL_DB (-10)

struct es8311_coeff {
	uint32_t mclk;
	uint32_t rate;
	uint8_t pre_div;
	uint8_t pre_multi;
	uint8_t adc_div;
	uint8_t dac_div;
	uint8_t fs_mode;
	uint8_t lrck_h;
	uint8_t lrck_l;
	uint8_t bclk_div;
	uint8_t adc_osr;
	uint8_t dac_osr;
};

/* Row copied from esp_codec_dev for MCLK = 256 x fs, the ESP32 I2S default.
 * Add rows from the same table when other rates are needed.
 */
static const struct es8311_coeff es8311_coeffs[] = {
	{4096000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
};

struct es8311_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec pa;
};

static int es8311_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct es8311_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int es8311_update(const struct device *dev, uint8_t reg, uint8_t keep_mask, uint8_t set)
{
	const struct es8311_config *cfg = dev->config;
	uint8_t val;
	int ret = i2c_reg_read_byte_dt(&cfg->i2c, reg, &val);

	if (ret) {
		return ret;
	}
	return i2c_reg_write_byte_dt(&cfg->i2c, reg, (val & keep_mask) | set);
}

static int es8311_set_volume_db(const struct device *dev, int db)
{
	int reg = ES8311_VOL_0DB + 2 * db;

	return es8311_write(dev, ES8311_DAC_VOL, CLAMP(reg, 0, 0xFF));
}

static int es8311_set_mute(const struct device *dev, bool mute)
{
	return es8311_update(dev, ES8311_DAC_31, (uint8_t)~ES8311_DAC_MUTE_BITS,
			     mute ? ES8311_DAC_MUTE_BITS : 0);
}

static int es8311_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	const struct i2s_config *i2s = &cfg->dai_cfg.i2s;
	const struct es8311_coeff *c = NULL;
	int ret = 0;

	if (cfg->dai_type != AUDIO_DAI_TYPE_I2S || i2s->word_size != 16) {
		LOG_ERR("only 16-bit I2S supported");
		return -ENOTSUP;
	}
	for (size_t i = 0; i < ARRAY_SIZE(es8311_coeffs); i++) {
		if (es8311_coeffs[i].mclk == cfg->mclk_freq &&
		    es8311_coeffs[i].rate == i2s->frame_clk_freq) {
			c = &es8311_coeffs[i];
		}
	}
	if (c == NULL) {
		LOG_ERR("no clock coefficients for %u Hz at MCLK %u Hz", i2s->frame_clk_freq,
			cfg->mclk_freq);
		return -ENOTSUP;
	}

	/* Open: defaults, target mode, clocks from the MCLK pin. */
	ret |= es8311_write(dev, ES8311_GPIO_44, 0x08); /* I2C noise immunity, */
	ret |= es8311_write(dev, ES8311_GPIO_44, 0x08); /* twice: first write can fail */
	ret |= es8311_write(dev, ES8311_CLK_MGR1, 0x30);
	ret |= es8311_write(dev, ES8311_CLK_MGR2, 0x00);
	ret |= es8311_write(dev, ES8311_CLK_MGR3, 0x10);
	ret |= es8311_write(dev, ES8311_ADC_16, 0x24);
	ret |= es8311_write(dev, ES8311_CLK_MGR4, 0x10);
	ret |= es8311_write(dev, ES8311_CLK_MGR5, 0x00);
	ret |= es8311_write(dev, ES8311_SYSTEM_0B, 0x00);
	ret |= es8311_write(dev, ES8311_SYSTEM_0C, 0x00);
	ret |= es8311_write(dev, ES8311_SYSTEM_10, 0x1F);
	ret |= es8311_write(dev, ES8311_SYSTEM_11, 0x7F);
	ret |= es8311_write(dev, ES8311_RESET, 0x80);     /* power on, target mode */
	ret |= es8311_write(dev, ES8311_CLK_MGR1, 0x3F);  /* MCLK from pin, not inverted */
	ret |= es8311_update(dev, ES8311_CLK_MGR6, (uint8_t)~0x20, 0); /* SCLK not inverted */
	ret |= es8311_write(dev, ES8311_SYSTEM_13, 0x10);
	ret |= es8311_write(dev, ES8311_ADC_1B, 0x0A);
	ret |= es8311_write(dev, ES8311_ADC_1C, 0x6A);
	ret |= es8311_write(dev, ES8311_GPIO_44, 0x08);   /* no DAC loopback reference */

	/* Format: standard I2S, 16 bit. */
	ret |= es8311_update(dev, ES8311_SDPIN, 0xFC, 0x0C);
	ret |= es8311_update(dev, ES8311_SDPOUT, 0xFC, 0x0C);

	/* Sample-rate clock tree. */
	ret |= es8311_update(dev, ES8311_CLK_MGR2, 0x07,
			     ((c->pre_div - 1) << 5) | (LOG2(c->pre_multi) << 3));
	ret |= es8311_write(dev, ES8311_CLK_MGR5, ((c->adc_div - 1) << 4) | (c->dac_div - 1));
	ret |= es8311_update(dev, ES8311_CLK_MGR3, 0x80, (c->fs_mode << 6) | c->adc_osr);
	ret |= es8311_update(dev, ES8311_CLK_MGR4, 0x80, c->dac_osr);
	ret |= es8311_update(dev, ES8311_CLK_MGR7, 0xC0, c->lrck_h);
	ret |= es8311_write(dev, ES8311_CLK_MGR8, c->lrck_l);
	ret |= es8311_update(dev, ES8311_CLK_MGR6, 0xE0, c->bclk_div - 1);

	/* Start, DAC only: serial input on, serial output (ADC) off. */
	ret |= es8311_write(dev, ES8311_RESET, 0x80);
	ret |= es8311_write(dev, ES8311_CLK_MGR1, 0x3F);
	ret |= es8311_update(dev, ES8311_SDPIN, (uint8_t)~BIT(6), 0);
	ret |= es8311_update(dev, ES8311_SDPOUT, 0xFF, BIT(6));
	ret |= es8311_write(dev, ES8311_ADC_17, 0xBF);
	ret |= es8311_write(dev, ES8311_SYSTEM_0E, 0x02);
	ret |= es8311_write(dev, ES8311_SYSTEM_12, 0x00);
	ret |= es8311_write(dev, ES8311_SYSTEM_14, 0x1A); /* analog mic, not PDM */
	ret |= es8311_write(dev, ES8311_SYSTEM_0D, 0x01);
	ret |= es8311_write(dev, ES8311_ADC_15, 0x40);
	ret |= es8311_write(dev, ES8311_DAC_37, 0x08);
	ret |= es8311_write(dev, ES8311_GP_45, 0x00);

	ret |= es8311_set_volume_db(dev, ES8311_DEFAULT_VOL_DB);
	ret |= es8311_set_mute(dev, true);

	if (ret) {
		LOG_ERR("configure failed");
		return -EIO;
	}
	LOG_INF("configured: %u Hz, MCLK %u Hz", i2s->frame_clk_freq, cfg->mclk_freq);
	return 0;
}

/* Call once the I2S TX stream is running so the amp never sees a bare edge. */
static void es8311_start_output(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;

	es8311_set_mute(dev, false);
	if (cfg->pa.port) {
		k_msleep(10);
		gpio_pin_set_dt(&cfg->pa, 1);
	}
}

static void es8311_stop_output(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;

	if (cfg->pa.port) {
		gpio_pin_set_dt(&cfg->pa, 0);
	}
	es8311_set_mute(dev, true);
}

/* OUTPUT_VOLUME takes dB (-95..+32); OUTPUT_MUTE takes a bool. */
static int es8311_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	ARG_UNUSED(channel);

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		return es8311_set_volume_db(dev, val.vol);
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		return es8311_set_mute(dev, val.mute);
	default:
		return -ENOTSUP;
	}
}

static int es8311_apply_properties(const struct device *dev)
{
	/* Properties are written immediately. */
	return 0;
}

static int es8311_init(const struct device *dev)
{
	const struct es8311_config *cfg = dev->config;
	uint8_t id1 = 0, id2 = 0;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		return -ENODEV;
	}
	if (cfg->pa.port) {
		if (!gpio_is_ready_dt(&cfg->pa)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->pa, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}

	ret = i2c_reg_read_byte_dt(&cfg->i2c, ES8311_CHIP_ID1, &id1);
	ret |= i2c_reg_read_byte_dt(&cfg->i2c, ES8311_CHIP_ID2, &id2);
	if (ret) {
		LOG_ERR("no response at 0x%02x", cfg->i2c.addr);
		return -EIO;
	}
	LOG_INF("chip id %02x%02x", id1, id2);
	return 0;
}

static DEVICE_API(audio_codec, es8311_api) = {
	.configure = es8311_configure,
	.start_output = es8311_start_output,
	.stop_output = es8311_stop_output,
	.set_property = es8311_set_property,
	.apply_properties = es8311_apply_properties,
};

#define ES8311_INIT(inst)                                                                          \
	static const struct es8311_config es8311_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.pa = GPIO_DT_SPEC_INST_GET_OR(inst, pa_gpios, {0}),                               \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, es8311_init, NULL, NULL, &es8311_config_##inst, POST_KERNEL,   \
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &es8311_api);

DT_INST_FOREACH_STATUS_OKAY(ES8311_INIT)
