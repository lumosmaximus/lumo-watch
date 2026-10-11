# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_ES8311 codec_es8311.c)
zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_ES7210 codec_es7210.c)
