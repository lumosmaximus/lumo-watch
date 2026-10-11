# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0
#
# Two images: this app runs on core 0 (procpu); remote/ is built for core 1
# (appcpu) and flashed into the appcpu partition. Core 0 starts core 1 at boot.

string(REPLACE "procpu" "appcpu" REMOTE_CPU "${BOARD_QUALIFIERS}")
string(CONFIGURE "${BOARD}/${REMOTE_CPU}" REMOTE_BOARD)

if(${REMOTE_CPU} STREQUAL ${BOARD_QUALIFIERS})
  message(FATAL_ERROR "Build for the procpu target; the appcpu image is added automatically.")
endif()

ExternalZephyrProject_Add(
  APPLICATION watch_remote
  SOURCE_DIR ${APP_DIR}/remote
  BOARD ${REMOTE_BOARD}
)

# Core 0 needs the core-1 image's build output (load addresses), so build it first.
add_dependencies(watch watch_remote)
sysbuild_add_dependencies(CONFIGURE watch watch_remote)
