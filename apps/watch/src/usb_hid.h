/*
 * Copyright (c) 2026 Adi Nandanamudi
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

int usb_start(void);                /* CDC-ACM console + HID media keys */
void usb_key(uint8_t key);          /* queue a KEY_* press for the computer */
void usb_reboot_to_download(void);  /* for flashing: ROM download mode */
