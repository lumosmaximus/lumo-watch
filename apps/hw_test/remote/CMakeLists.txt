# Copyright (c) 2026 Adi Nandanamudi
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hw_test_remote)

target_sources(app PRIVATE src/main.c)
