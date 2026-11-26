/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * The few kernel primitives lfbm.c uses, on top of the GCC atomics, so that the
 * algorithm can be run and sanitized in userspace. Not a part of the kernel build.
 */

#ifndef LFBM_COMPAT_H
#define LFBM_COMPAT_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITS_PER_LONG ((unsigned int)(sizeof(unsigned long) * 8))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define GFP_KERNEL 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

#define try_cmpxchg(ptr, oldp, new) \
	__atomic_compare_exchange_n(ptr, oldp, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* set_bit() is sequentially consistent here already, the fence only mirrors the kernel call. */
#define smp_mb__after_atomic() __atomic_thread_fence(__ATOMIC_SEQ_CST)

#define WARN_ON_ONCE(cond) ({					\
	int __warn = !!(cond);					\
	if (__warn)						\
		fprintf(stderr, "WARN at %s:%d\n", __FILE__, __LINE__);	\
	__warn;							\
})

/* As in the kernel, the bit number may run past the first word. */
static inline void set_bit(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_or(&addr[nr / BITS_PER_LONG], 1UL << (nr % BITS_PER_LONG), __ATOMIC_SEQ_CST);
}

static inline void clear_bit(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_and(&addr[nr / BITS_PER_LONG], ~(1UL << (nr % BITS_PER_LONG)), __ATOMIC_SEQ_CST);
}

static inline bool test_bit(unsigned long nr, const unsigned long *addr)
{
	return (__atomic_load_n(&addr[nr / BITS_PER_LONG], __ATOMIC_RELAXED) >> (nr % BITS_PER_LONG)) & 1;
}

static inline bool test_and_set_bit(unsigned long nr, unsigned long *addr)
{
	unsigned long mask = 1UL << (nr % BITS_PER_LONG);

	return __atomic_fetch_or(&addr[nr / BITS_PER_LONG], mask, __ATOMIC_SEQ_CST) & mask;
}

static inline bool test_and_clear_bit(unsigned long nr, unsigned long *addr)
{
	unsigned long mask = 1UL << (nr % BITS_PER_LONG);

	return __atomic_fetch_and(&addr[nr / BITS_PER_LONG], ~mask, __ATOMIC_SEQ_CST) & mask;
}

/* The lowest zero bit, undefined for a full word. */
static inline unsigned long ffz(unsigned long word)
{
	return __builtin_ctzl(~word);
}

static inline unsigned long hweight_long(unsigned long word)
{
	return __builtin_popcountl(word);
}

static inline void *kvcalloc(size_t n, size_t size, int flags)
{
	return calloc(n, size);
}

static inline void kvfree(void *ptr)
{
	free(ptr);
}

#endif
