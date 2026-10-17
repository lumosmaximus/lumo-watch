#pragma once
#include <zephyr/toolchain.h>
struct device { const char *name; };
extern const struct device host_dev;
#define DEVICE_DT_GET(x) (&host_dev)
#define DT_NODELABEL(x) x
#define DT_CHOSEN(x) x
static inline bool device_is_ready(const struct device *d) { (void)d; return true; }
