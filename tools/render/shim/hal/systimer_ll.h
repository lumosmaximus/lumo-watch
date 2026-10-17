#pragma once
#include <soc/systimer_struct.h>
extern int64_t host_time_ns;
static inline void systimer_ll_counter_snapshot(systimer_dev_t *d, uint32_t c) { (void)d; (void)c; }
static inline uint32_t systimer_ll_get_counter_value_low(systimer_dev_t *d, uint32_t c) { (void)d; (void)c; return (uint32_t)(host_time_ns / 62); }
