# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_INPUT_CST9217 input_cst9217.c)
