# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_CO5300_QSPI display_co5300_qspi.c)
