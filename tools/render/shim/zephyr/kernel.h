/* host render build: a tiny single-threaded stand-in for the Zephyr kernel.
 * Time is simulated: host_time_ns advances when the compositor waits (TE,
 * sleeps), so renders are deterministic.
 */
#pragma once
#include <zephyr/toolchain.h>
#include <string.h>

extern int64_t host_time_ns;
void host_advance_ns(int64_t ns);
void host_on_wait(void); /* the compositor is waiting outside the frame loop (picker) */

static inline int64_t k_uptime_get(void) { return host_time_ns / 1000000; }
static inline uint32_t k_uptime_get_32(void) { return (uint32_t)(host_time_ns / 1000000); }
static inline uint32_t k_cycle_get_32(void) { return (uint32_t)(host_time_ns * 240 / 1000); }
static inline uint32_t k_cyc_to_us_floor32(uint32_t c) { return c / 240; }
static inline void k_msleep(int ms) { host_advance_ns((int64_t)ms * 1000000); }
static inline void k_busy_wait(uint32_t us) { host_advance_ns((int64_t)us * 1000); }
static inline uint32_t irq_lock(void) { return 0; }
static inline void irq_unlock(uint32_t k) { (void)k; }

typedef struct { int ms; } k_timeout_t;
#define K_NO_WAIT ((k_timeout_t){0})
#define K_FOREVER ((k_timeout_t){-1})
#define K_MSEC(m) ((k_timeout_t){(m)})

struct k_mutex { int dummy; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t) { (void)m; (void)t; return 0; }
static inline void k_mutex_unlock(struct k_mutex *m) { (void)m; }

/* message queue: a fixed ring */
struct k_msgq {
	char *buf; size_t msg_size; uint32_t max, head, tail, count;
};
#define K_MSGQ_DEFINE(name, size, n, align) \
	static char name##_buf[(size) * (n)]; \
	struct k_msgq name = {name##_buf, (size), (n), 0, 0, 0}
static inline int k_msgq_put(struct k_msgq *q, const void *d, k_timeout_t t) {
	(void)t; if (q->count == q->max) return -1;
	memcpy(q->buf + q->tail * q->msg_size, d, q->msg_size); q->tail = (q->tail + 1) % q->max; q->count++; return 0; }
static inline int k_msgq_get(struct k_msgq *q, void *d, k_timeout_t t) {
	if (q->count == 0) { if (t.ms > 0) { host_advance_ns((int64_t)t.ms * 1000000); host_on_wait(); } return -1; }
	memcpy(d, q->buf + q->head * q->msg_size, q->msg_size); q->head = (q->head + 1) % q->max; q->count--; return 0; }
