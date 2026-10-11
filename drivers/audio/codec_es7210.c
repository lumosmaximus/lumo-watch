/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everest ES7210 microphone ADC, I2S target with MCLK input.
 * MIC1/MIC2 only, standard I2S stereo (MIC1 left, MIC2 right). TDM, needed
 * for MIC3 (speaker loopback reference on this board), is not supported yet.
 *
 * Register sequences follow Espressif's esp_codec_dev es7210.c
 * (Apache-2.0, (c) 2023 Espressif Systems), trimmed to what this board uses.
 */

#define DT_DRV_COMPAT everest_es7210

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(es7210, CONFIG_AUDIO_CODEC_LOG_LEVEL);

#define ES7210_RESET        0x00
#define ES7210_CLOCK_OFF    0x01
#define ES7210_MAINCLK      0x02
#define ES7210_POWER_DOWN   0x06
#define ES7210_OSR          0x07
#define ES7210_MODE_CONFIG  0x08
#define ES7210_TIME_CTRL0   0x09
#define ES7210_TIME_CTRL1   0x0A
#define ES7210_SDP_IF1      0x11
#define ES7210_SDP_IF2      0x12
#define ES7210_ADC34_HPF2   0x20
#define ES7210_ADC34_HPF1   0x21
#define ES7210_ADC12_HPF1   0x22
#define ES7210_ADC12_HPF2   0x23
#define ES7210_CHIP_ID1     0x3D
#define ES7210_CHIP_ID2     0x3E
#define ES7210_ANALOG       0x40
#define ES7210_MIC12_BIAS   0x41
#define ES7210_MIC34_BIAS   0x42
#define ES7210_MIC1_GAIN    0x43
#define ES7210_MIC2_GAIN    0x44
#define ES7210_MIC3_GAIN    0x45
#define ES7210_MIC4_GAIN    0x46
#define ES7210_MIC1_POWER   0x47
#define ES7210_MIC2_POWER   0x48
#define ES7210_MIC3_POWER   0x49
#define ES7210_MIC4_POWER   0x4A
#define ES7210_MIC12_POWER  0x4B
#define ES7210_MIC34_POWER  0x4C

#define ES7210_GAIN_EN      0x10
#define ES7210_GAIN_MASK    0x0F
#define ES7210_GAIN_MAX_DB  33

struct es7210_config {
	struct i2c_dt_spec i2c;
	int gain_db;
};

struct es7210_data {
	uint8_t clock_off; /* REG01 value captured after mic selection */
};

static int es7210_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct es7210_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int es7210_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	const struct es7210_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->i2c, reg, mask, val);
}

/* 0..33 dB in 3 dB steps maps straight onto the register code. */
static int es7210_set_gain_db(const struct device *dev, int db)
{
	uint8_t code = CLAMP(db, 0, ES7210_GAIN_MAX_DB) / 3;
	int ret;

	ret = es7210_update(dev, ES7210_MIC1_GAIN, ES7210_GAIN_MASK, code);
	ret |= es7210_update(dev, ES7210_MIC2_GAIN, ES7210_GAIN_MASK, code);
	return ret ? -EIO : 0;
}

/* Power up MIC1/MIC2 (ADC1/2 clocks, bias, PGA), leave MIC3/MIC4 off. */
static int es7210_select_mic12(const struct device *dev)
{
	const struct es7210_config *cfg = dev->config;
	int ret = 0;

	for (uint8_t reg = ES7210_MIC1_GAIN; reg <= ES7210_MIC4_GAIN; reg++) {
		ret |= es7210_update(dev, reg, ES7210_GAIN_EN, 0);
	}
	ret |= es7210_write(dev, ES7210_MIC12_POWER, 0xFF);
	ret |= es7210_write(dev, ES7210_MIC34_POWER, 0xFF);

	ret |= es7210_update(dev, ES7210_CLOCK_OFF, 0x0B, 0x00);
	ret |= es7210_write(dev, ES7210_MIC12_POWER, 0x00);
	ret |= es7210_update(dev, ES7210_MIC1_GAIN, ES7210_GAIN_EN, ES7210_GAIN_EN);
	ret |= es7210_update(dev, ES7210_MIC2_GAIN, ES7210_GAIN_EN, ES7210_GAIN_EN);
	ret |= es7210_set_gain_db(dev, cfg->gain_db);

	ret |= es7210_write(dev, ES7210_SDP_IF2, 0x00); /* not TDM */
	return ret ? -EIO : 0;
}

static int es7210_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	const struct es7210_config *dcfg = dev->config;
	struct es7210_data *data = dev->data;
	const struct i2s_config *i2s = &cfg->dai_cfg.i2s;
	uint8_t reg01;
	int ret = 0;

	if (cfg->dai_type != AUDIO_DAI_TYPE_I2S || i2s->word_size != 16) {
		LOG_ERR("only 16-bit I2S supported");
		return -ENOTSUP;
	}

	/* Open: reset, timing, high-pass filters, target mode. */
	ret |= es7210_write(dev, ES7210_RESET, 0xFF);
	ret |= es7210_write(dev, ES7210_RESET, 0x41);
	ret |= es7210_write(dev, ES7210_CLOCK_OFF, 0x3F);
	ret |= es7210_write(dev, ES7210_TIME_CTRL0, 0x30);
	ret |= es7210_write(dev, ES7210_TIME_CTRL1, 0x30);
	ret |= es7210_write(dev, ES7210_ADC12_HPF2, 0x2A);
	ret |= es7210_write(dev, ES7210_ADC12_HPF1, 0x0A);
	ret |= es7210_write(dev, ES7210_ADC34_HPF2, 0x0A);
	ret |= es7210_write(dev, ES7210_ADC34_HPF1, 0x2A);
	ret |= es7210_update(dev, ES7210_MODE_CONFIG, 0x01, 0x00); /* target */
	ret |= es7210_write(dev, ES7210_ANALOG, 0x43);   /* VDDA 3.3 V, VMID 5k */
	ret |= es7210_write(dev, ES7210_MIC12_BIAS, 0x70); /* 2.87 V */
	ret |= es7210_write(dev, ES7210_MIC34_BIAS, 0x70);
	ret |= es7210_write(dev, ES7210_OSR, 0x20);
	ret |= es7210_write(dev, ES7210_MAINCLK, 0xC1);  /* DLL on, div 1; clears state */
	ret |= es7210_select_mic12(dev);

	/* Format: 16 bit, standard I2S. */
	ret |= es7210_update(dev, ES7210_SDP_IF1, 0xE0, 0x60);
	ret |= es7210_update(dev, ES7210_SDP_IF1, 0x03, 0x00);

	ret |= i2c_reg_read_byte_dt(&dcfg->i2c, ES7210_CLOCK_OFF, &reg01);
	if (ret) {
		LOG_ERR("configure failed");
		return -EIO;
	}
	data->clock_off = reg01;
	LOG_INF("configured: %u Hz, MIC1/MIC2, gain %d dB", i2s->frame_clk_freq, dcfg->gain_db);
	return 0;
}

static int es7210_start(const struct device *dev, audio_dai_dir_t dir)
{
	struct es7210_data *data = dev->data;
	int ret = 0;

	if (dir != AUDIO_DAI_DIR_RX) {
		return -ENOTSUP;
	}
	ret |= es7210_write(dev, ES7210_CLOCK_OFF, data->clock_off);
	ret |= es7210_write(dev, ES7210_POWER_DOWN, 0x00);
	ret |= es7210_write(dev, ES7210_ANALOG, 0x43);
	ret |= es7210_write(dev, ES7210_MIC1_POWER, 0x08);
	ret |= es7210_write(dev, ES7210_MIC2_POWER, 0x08);
	ret |= es7210_write(dev, ES7210_MIC3_POWER, 0x08);
	ret |= es7210_write(dev, ES7210_MIC4_POWER, 0x08);
	ret |= es7210_select_mic12(dev);
	ret |= es7210_write(dev, ES7210_ANALOG, 0x43);
	ret |= es7210_write(dev, ES7210_RESET, 0x71);
	ret |= es7210_write(dev, ES7210_RESET, 0x41);
	return ret ? -EIO : 0;
}

static int es7210_stop(const struct device *dev, audio_dai_dir_t dir)
{
	int ret = 0;

	if (dir != AUDIO_DAI_DIR_RX) {
		return -ENOTSUP;
	}
	for (uint8_t reg = ES7210_MIC1_POWER; reg <= ES7210_MIC34_POWER; reg++) {
		ret |= es7210_write(dev, reg, 0xFF);
	}
	ret |= es7210_write(dev, ES7210_ANALOG, 0xC0);
	ret |= es7210_write(dev, ES7210_CLOCK_OFF, 0x7F);
	ret |= es7210_write(dev, ES7210_POWER_DOWN, 0x07);
	return ret ? -EIO : 0;
}

/* INPUT_VOLUME takes PGA gain in dB (0..33). */
static int es7210_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	ARG_UNUSED(channel);

	if (property == AUDIO_PROPERTY_INPUT_VOLUME) {
		return es7210_set_gain_db(dev, val.vol);
	}
	return -ENOTSUP;
}

static int es7210_apply_properties(const struct device *dev)
{
	return 0;
}

static int es7210_init(const struct device *dev)
{
	const struct es7210_config *cfg = dev->config;
	uint8_t id1 = 0, id2 = 0;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		return -ENODEV;
	}
	ret = i2c_reg_read_byte_dt(&cfg->i2c, ES7210_CHIP_ID1, &id1);
	ret |= i2c_reg_read_byte_dt(&cfg->i2c, ES7210_CHIP_ID2, &id2);
	if (ret) {
		LOG_ERR("no response at 0x%02x", cfg->i2c.addr);
		return -EIO;
	}
	LOG_INF("chip id %02x%02x", id1, id2);
	return 0;
}

static DEVICE_API(audio_codec, es7210_api) = {
	.configure = es7210_configure,
	.start = es7210_start,
	.stop = es7210_stop,
	.set_property = es7210_set_property,
	.apply_properties = es7210_apply_properties,
};

#define ES7210_INIT(inst)                                                                          \
	static const struct es7210_config es7210_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.gain_db = DT_INST_PROP(inst, mic_gain_db),                                        \
	};                                                                                         \
	static struct es7210_data es7210_data_##inst;                                              \
	DEVICE_DT_INST_DEFINE(inst, es7210_init, NULL, &es7210_data_##inst,                        \
			      &es7210_config_##inst, POST_KERNEL,                                  \
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &es7210_api);

DT_INST_FOREACH_STATUS_OKAY(ES7210_INIT)
