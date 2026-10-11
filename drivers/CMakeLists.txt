# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AUDIO_CODEC audio)
add_subdirectory_ifdef(CONFIG_INPUT input)
add_subdirectory_ifdef(CONFIG_DISPLAY display)
