#pragma once
#include <stdint.h>
uint32_t host_reg_read(uint32_t reg);
#define REG_READ(r) host_reg_read(r)
