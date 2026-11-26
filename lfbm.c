// SPDX-License-Identifier: GPL-2.0-only

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#else
#include "compat.h"
#endif

#include "lfbm.h"

#define LFBM_FULL (~0UL)

/* Bits of the level: the cells for level 0, the words of the level below above it. */
static unsigned long lfbm_level_bits(const struct lfbm *bm, unsigned int k)
{
	return k ? bm->nwords[k - 1] : bm->nbits;
}

int lfbm_init(struct lfbm *bm, unsigned long nbits)
{
	unsigned long total = 0;
	unsigned long n = nbits;
	unsigned int k;

	if (!nbits || nbits > LONG_MAX)
		return -EINVAL;

	memset(bm, 0, sizeof(*bm));
	bm->nbits = nbits;

	for (k = 0; k < LFBM_MAX_LEVELS; k++) {
		bm->nwords[k] = DIV_ROUND_UP(n, BITS_PER_LONG);
		total += bm->nwords[k];
		n = bm->nwords[k];

		if (n == 1) {
			k++;
			break;
		}
	}

	if (bm->nwords[k - 1] != 1)
		return -EINVAL;

	bm->levels = k;
	bm->mem = kvcalloc(total, sizeof(*bm->mem), GFP_KERNEL);
	if (!bm->mem)
		return -ENOMEM;

	n = 0;
	for (k = 0; k < bm->levels; k++) {
		unsigned long bits = lfbm_level_bits(bm, k);

		bm->w[k] = bm->mem + n;
		n += bm->nwords[k];

		/* The bits past the end are taken for good: nothing picks them. */
		if (bits % BITS_PER_LONG)
			bm->w[k][bm->nwords[k] - 1] |= LFBM_FULL << (bits % BITS_PER_LONG);
	}

	return 0;
}

void lfbm_destroy(struct lfbm *bm)
{
	kvfree(bm->mem);
	memset(bm, 0, sizeof(*bm));
}

/*
 * The word |i| of level |k| is full: say so one level up, and go on while that
 * fills a word as well.
 *
 * The early return is safe. Whoever fills a word last sees it FULL here and
 * climbs on, and whoever sees it not full returns because the one who fills it
 * will do the climbing.
 */
static void lfbm_hint_set(struct lfbm *bm, unsigned int k, unsigned long i)
{
	while (k + 1 < bm->levels) {
		unsigned long *parent = bm->w[k + 1];

		set_bit(i, parent);
		/* The hint is visible before the word is read again below, pairs with the full barrier of the clear. */
		smp_mb__after_atomic();

		/*
		 * A clear may have emptied the word after it was seen full and cleared
		 * the hint before it was set, then nobody takes the hint back but us.
		 */
		if (READ_ONCE(bm->w[k][i]) != LFBM_FULL) {
			clear_bit(i, parent);
			return;
		}

		i /= BITS_PER_LONG;
		k++;

		if (READ_ONCE(bm->w[k][i]) != LFBM_FULL)
			return;
	}
}

/* The bit |bit| of level 0 was freed: the words above it have a free bit now. */
static void lfbm_hint_clear(struct lfbm *bm, unsigned long bit)
{
	unsigned int k;

	for (k = 1; k < bm->levels; k++) {
		bit /= BITS_PER_LONG;

		if (test_bit(bit, bm->w[k]))
			clear_bit(bit, bm->w[k]);
	}
}

/* The lowest free bit of the leaf word |i|, or -1 when it is full. */
static long lfbm_word_find_and_set(struct lfbm *bm, unsigned long i)
{
	unsigned long *word = &bm->w[0][i];
	unsigned long old = READ_ONCE(*word);
	unsigned long new;
	unsigned long bit;

	do {
		if (old == LFBM_FULL)
			return -1;

		bit = ffz(old);
		new = old | (1UL << bit);
	} while (!try_cmpxchg(word, &old, new));

	if (new == LFBM_FULL)
		lfbm_hint_set(bm, 0, i);

	return i * BITS_PER_LONG + bit;
}

/*
 * Walks the hints from the top to the leaf word they point to: its number,
 * -ENOSPC when the top word is full, or -EAGAIN after a word that was full
 * in spite of its hint has been fixed.
 */
static long lfbm_find_first_zero_word(struct lfbm *bm)
{
	unsigned int k = bm->levels - 1;
	unsigned long i = 0;

	while (k) {
		unsigned long v = READ_ONCE(bm->w[k][i]);

		if (v == LFBM_FULL) {
			if (k == bm->levels - 1)
				return -ENOSPC;

			lfbm_hint_set(bm, k, i);
			return -EAGAIN;
		}

		i = i * BITS_PER_LONG + ffz(v);
		k--;
	}

	return i;
}

long lfbm_find_and_set_bit(struct lfbm *bm)
{
	for (;;) {
		long i = lfbm_find_first_zero_word(bm);
		long bit;

		if (i == -EAGAIN)
			continue;
		if (i < 0)
			return i;

		bit = lfbm_word_find_and_set(bm, i);
		if (bit >= 0)
			return bit;

		/* Somebody took the rest of the word first. */
		if (bm->levels == 1)
			return -ENOSPC;

		lfbm_hint_set(bm, 0, i);
	}
}

bool lfbm_test_and_set_bit(struct lfbm *bm, unsigned long bit)
{
	unsigned long i = bit / BITS_PER_LONG;

	/* Not ours to own: report it as taken. */
	if (WARN_ON_ONCE(bit >= bm->nbits))
		return true;

	if (test_and_set_bit(bit, bm->w[0]))
		return true;

	if (bm->levels > 1 && READ_ONCE(bm->w[0][i]) == LFBM_FULL)
		lfbm_hint_set(bm, 0, i);

	return false;
}

bool lfbm_test_and_clear_bit(struct lfbm *bm, unsigned long bit)
{
	if (WARN_ON_ONCE(bit >= bm->nbits))
		return false;

	/* Ordered before the hints below are touched. */
	if (!test_and_clear_bit(bit, bm->w[0]))
		return false;

	lfbm_hint_clear(bm, bit);

	return true;
}

bool lfbm_test_bit(const struct lfbm *bm, unsigned long bit)
{
	if (bit >= bm->nbits)
		return false;

	return test_bit(bit, bm->w[0]);
}

unsigned long lfbm_weight(const struct lfbm *bm)
{
	unsigned long weight = 0;
	unsigned long i;

	for (i = 0; i < bm->nwords[0]; i++)
		weight += hweight_long(READ_ONCE(bm->w[0][i]));

	return weight - (bm->nwords[0] * BITS_PER_LONG - bm->nbits);
}
