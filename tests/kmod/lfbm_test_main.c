// SPDX-License-Identifier: GPL-2.0-only

/*
 * Loading the module runs the checks: it fails to load, with -EIO, when one of
 * them does not hold. The result is in dmesg.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "lfbm.h"

#define LFBM_T_THREADS 8
#define LFBM_T_MAX_HOLD 64

static atomic_t lfbm_t_failed = ATOMIC_INIT(0);

#define LFBM_T_CHECK(cond)							\
	do {									\
		if (!(cond)) {							\
			pr_err("lfbm_test: %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
			atomic_set(&lfbm_t_failed, 1);				\
			return;							\
		}								\
	} while (0)

static void lfbm_t_order(unsigned long nbits)
{
	struct lfbm bm;
	unsigned long i;

	pr_info("lfbm_test: order, %lu bits\n", nbits);
	LFBM_T_CHECK(!lfbm_init(&bm, nbits));

	for (i = 0; i < nbits; i++)
		LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == (long)i);
	LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);
	LFBM_T_CHECK(lfbm_weight(&bm) == nbits);

	if (nbits < 3) {
		/* Too small for three distinct holes. */
		lfbm_test_and_clear_bit(&bm, nbits - 1);
		LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == (long)(nbits - 1));
	} else {
		/* Holes come back lowest first. */
		lfbm_test_and_clear_bit(&bm, nbits - 1);
		lfbm_test_and_clear_bit(&bm, nbits / 2);
		lfbm_test_and_clear_bit(&bm, 0);
		LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == 0);
		LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == (long)(nbits / 2));
		LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == (long)(nbits - 1));
	}
	LFBM_T_CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);

	/* The kernel semantics: the old value comes back. */
	LFBM_T_CHECK(lfbm_test_and_clear_bit(&bm, 0));
	LFBM_T_CHECK(!lfbm_test_and_clear_bit(&bm, 0));
	LFBM_T_CHECK(!lfbm_test_and_set_bit(&bm, 0));
	LFBM_T_CHECK(lfbm_test_and_set_bit(&bm, 0));
	LFBM_T_CHECK(lfbm_test_bit(&bm, 0));

	lfbm_destroy(&bm);
}

struct lfbm_t_stress {
	struct lfbm *bm;
	atomic_t *owner;
	struct completion *done;
	atomic_t *running;
	unsigned int id;
	unsigned int hold;
	unsigned long iters;
	atomic_long_t *nospc;
};

static int lfbm_t_thread(void *arg)
{
	struct lfbm_t_stress *st = arg;
	unsigned long held[LFBM_T_MAX_HOLD];
	unsigned int n = 0;
	u32 seed = 2463534242U * (st->id + 1);
	unsigned long it;

	for (it = 0; it < st->iters && !atomic_read(&lfbm_t_failed); it++) {
		bool take;

		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		take = n < st->hold && (n == 0 || (seed & 1));

		if (take) {
			long bit = lfbm_find_and_set_bit(st->bm);

			if (bit == -ENOSPC) {
				atomic_long_inc(st->nospc);
				continue;
			}
			if (bit < 0 || atomic_xchg(&st->owner[bit], st->id + 1)) {
				pr_err("lfbm_test: bit %ld handed out twice\n", bit);
				atomic_set(&lfbm_t_failed, 1);
				break;
			}
			held[n++] = bit;
		} else if (n) {
			unsigned int k = seed % n;
			unsigned long bit = held[k];

			held[k] = held[--n];
			if (atomic_xchg(&st->owner[bit], 0) != st->id + 1) {
				pr_err("lfbm_test: bit %lu lost its owner\n", bit);
				atomic_set(&lfbm_t_failed, 1);
				break;
			}
			lfbm_test_and_clear_bit(st->bm, bit);
		}

		if (!(it & 0xfff))
			cond_resched();
	}

	while (n) {
		unsigned long bit = held[--n];

		atomic_set(&st->owner[bit], 0);
		lfbm_test_and_clear_bit(st->bm, bit);
	}

	if (atomic_dec_and_test(st->running))
		complete(st->done);

	return 0;
}

static unsigned long lfbm_t_stress_run(unsigned long nbits, unsigned int hold, unsigned long iters)
{
	struct lfbm_t_stress st[LFBM_T_THREADS];
	DECLARE_COMPLETION_ONSTACK(done);
	atomic_t running = ATOMIC_INIT(LFBM_T_THREADS);
	atomic_long_t nospc = ATOMIC_LONG_INIT(0);
	struct lfbm bm;
	atomic_t *owner;
	unsigned long i;
	unsigned int t;

	owner = vzalloc(array_size(nbits, sizeof(*owner)));
	if (!owner) {
		atomic_set(&lfbm_t_failed, 1);
		return 0;
	}

	if (lfbm_init(&bm, nbits)) {
		atomic_set(&lfbm_t_failed, 1);
		vfree(owner);
		return 0;
	}

	for (t = 0; t < LFBM_T_THREADS; t++) {
		st[t] = (struct lfbm_t_stress){ &bm, owner, &done, &running, t, hold, iters, &nospc };
		kthread_run(lfbm_t_thread, &st[t], "lfbm_t/%u", t);
	}
	wait_for_completion(&done);

	/* Nothing is held any more: the hints have to be exact. */
	if (lfbm_weight(&bm)) {
		pr_err("lfbm_test: %lu bits left taken\n", lfbm_weight(&bm));
		atomic_set(&lfbm_t_failed, 1);
	}
	for (i = 0; i < nbits && !atomic_read(&lfbm_t_failed); i++) {
		if (lfbm_find_and_set_bit(&bm) != (long)i) {
			pr_err("lfbm_test: after the run the bit %lu did not come out in order\n", i);
			atomic_set(&lfbm_t_failed, 1);
		}
	}

	lfbm_destroy(&bm);
	vfree(owner);

	return atomic_long_read(&nospc);
}

static int __init lfbm_t_init(void)
{
	static const unsigned long sizes[] = { 1, 2, 63, 64, 65, 4096, 4097, 262144 + 7, (1UL << 20) + 13 };
	unsigned long nospc = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++)
		lfbm_t_order(sizes[i]);

	if (!atomic_read(&lfbm_t_failed)) {
		/* Plenty of room: -ENOSPC would be a bug. */
		if (lfbm_t_stress_run(4096 + 37, 16, 300000) || lfbm_t_stress_run(1UL << 20, 32, 300000)) {
			pr_err("lfbm_test: -ENOSPC with room to spare\n");
			atomic_set(&lfbm_t_failed, 1);
		}
	}

	if (!atomic_read(&lfbm_t_failed)) {
		nospc += lfbm_t_stress_run(64, 16, 300000);
		nospc += lfbm_t_stress_run(65, 16, 300000);
		nospc += lfbm_t_stress_run(130, 32, 300000);
	}

	if (atomic_read(&lfbm_t_failed)) {
		pr_err("lfbm_test: FAILED\n");
		return -EIO;
	}

	pr_info("lfbm_test: passed (%lu -ENOSPC while full)\n", nospc);
	return 0;
}

static void __exit lfbm_t_exit(void)
{
}

module_init(lfbm_t_init);
module_exit(lfbm_t_exit);
MODULE_DESCRIPTION("lfbm test");
MODULE_LICENSE("GPL");
