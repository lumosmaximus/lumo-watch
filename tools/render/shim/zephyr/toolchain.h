/* host render build: stand-in for Zephyr's toolchain.h */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define BIT(n) (1UL << (n))
#define IS_ENABLED(x) (x)
