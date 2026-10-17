#pragma once
#include <zephyr/device.h>
struct display_buffer_descriptor { uint32_t buf_size; uint16_t width, height, pitch; };
int display_write(const struct device *d, uint16_t x, uint16_t y,
		  const struct display_buffer_descriptor *desc, const void *buf);
static inline int display_blanking_off(const struct device *d) { (void)d; return 0; }
