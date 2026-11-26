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
 * allocates.
 *
 * -ENOSPC may be reported while a lfbm_free() racing with this call is still
 * in flight, never after it has returned.
 */
long lfbm_alloc(struct lfbm *bm);

/* Takes the given bit: false if it was taken already. */
bool lfbm_claim(struct lfbm *bm, unsigned long bit);

/* The bit has to be taken, freeing a free one is warned about. */
void lfbm_free(struct lfbm *bm, unsigned long bit);

bool lfbm_test(const struct lfbm *bm, unsigned long bit);

/* How many bits are taken. Not a snapshot while others allocate or free. */
unsigned long lfbm_weight(const struct lfbm *bm);

#endif
