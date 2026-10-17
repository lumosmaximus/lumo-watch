#pragma once
#include <zephyr/device.h>
typedef void (*ipm_callback_t)(const struct device *dev, void *user_data, uint32_t id, volatile void *data);
int ipm_send(const struct device *d, int wait, uint32_t id, const void *data, int size);
void ipm_register_callback(const struct device *d, ipm_callback_t cb, void *user);
static inline int ipm_set_enabled(const struct device *d, int e) { (void)d; (void)e; return 0; }
