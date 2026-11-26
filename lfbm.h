/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LFBM_H
#define LFBM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include "compat.h"
#endif

/* 64 ^ 11 > 2 ^ 63: enough levels for every size lfbm_init() takes. */
#define LFBM_MAX_LEVELS 11

/*
 * Lock-free bitmap of a fixed size, that hands out the lowest free bit.
 *
 * Level 0 holds the bits themselves. A bit of level k + 1 tells that the word
 * with the same number on level k is full, up to the one word on top. Such a
 * bit is a hint: allocation walks the hints down from the top, so it costs one
 * ffz() per level instead of a walk over the words in front of the first free
 * one.
 */
struct lfbm {
	unsigned long nbits;
	unsigned int levels;
	unsigned long nwords[LFBM_MAX_LEVELS];
	unsigned long *w[LFBM_MAX_LEVELS];
	unsigned long *mem;
};

/* -EINVAL for a size of zero or above LONG_MAX, -ENOMEM. May sleep. */
int lfbm_init(struct lfbm *bm, unsigned long nbits);
void lfbm_destroy(struct lfbm *bm);

/*
 * Takes the lowest free bit: its number, or -ENOSPC. Never sleeps, never
 * allocates. Unlike find_and_set_bit() of the kernel, which returns nbits when
 * there is none, this returns -ENOSPC on purpose.
 *
 * -ENOSPC may be false while a parallel lfbm_test_and_clear_bit(), or a
 * parallel search that has just filled a word, is still setting or clearing
 * the hints: a late lfbm_hint_set() may have set a hint that it has not yet
 * re-checked, and a search on a nearly full map sees the top word full. On a
 * map nobody else touches the result is exact.
 */
long lfbm_find_and_set_bit(struct lfbm *bm);

/*
 * Like test_and_set_bit(): returns the old value, true if the bit was taken
 * already, false if we took it now. A bit past the end warns and reads as
 * taken, so that the caller never thinks it owns it.
 */
bool lfbm_test_and_set_bit(struct lfbm *bm, unsigned long bit);

/*
 * Like test_and_clear_bit(): returns the old value, true if the bit was taken
 * and is free now, false if it was free already (not warned about, that is up
 * to the caller). A bit past the end warns and returns false.
 */
bool lfbm_test_and_clear_bit(struct lfbm *bm, unsigned long bit);

bool lfbm_test_bit(const struct lfbm *bm, unsigned long bit);

/* How many bits are taken. Not a snapshot while others allocate or free. */
unsigned long lfbm_weight(const struct lfbm *bm);

#endif
