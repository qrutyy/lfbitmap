// SPDX-License-Identifier: GPL-2.0-only

/*
 * Userspace tests: the order of the allocation, the edges of the sizes, and a
 * stress run that checks no bit is ever handed to two threads at once and that
 * the hints are all right once everybody has freed what it took.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "lfbm.h"

#define CHECK(cond)								\
	do {									\
		if (!(cond)) {							\
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
			exit(1);						\
		}								\
	} while (0)

static int cmp_ul(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *)a;
	unsigned long y = *(const unsigned long *)b;

	return (x > y) - (x < y);
}

/* Everything taken in order, then the holes refilled lowest first. */
static void test_order(unsigned long nbits)
{
	unsigned long holes[] = { 0, 5, 63, 64, 65, 4095, 4096, nbits / 2, nbits - 1 };
	unsigned long sorted[ARRAY_SIZE(holes)];
	unsigned long n = 0;
	unsigned long i;
	struct lfbm bm;

	CHECK(!lfbm_init(&bm, nbits));

	for (i = 0; i < nbits; i++)
		CHECK(lfbm_find_and_set_bit(&bm) == (long)i);
	CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);
	CHECK(lfbm_weight(&bm) == nbits);

	for (i = 0; i < ARRAY_SIZE(holes); i++) {
		if (holes[i] >= nbits)
			continue;
		sorted[n++] = holes[i];
	}
	qsort(sorted, n, sizeof(sorted[0]), cmp_ul);

	/* Drop the duplicates, nbits / 2 may be one of the others. */
	for (i = 1; i < n; i++) {
		if (sorted[i] == sorted[i - 1]) {
			memmove(&sorted[i], &sorted[i + 1], (n - i - 1) * sizeof(sorted[0]));
			n--;
			i--;
		}
	}

	for (i = 0; i < n; i++)
		lfbm_test_and_clear_bit(&bm, sorted[i]);
	CHECK(lfbm_weight(&bm) == nbits - n);

	for (i = 0; i < n; i++)
		CHECK(lfbm_find_and_set_bit(&bm) == (long)sorted[i]);
	CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);

	for (i = nbits; i > 0; i--)
		lfbm_test_and_clear_bit(&bm, i - 1);
	CHECK(lfbm_weight(&bm) == 0);
	CHECK(lfbm_find_and_set_bit(&bm) == 0);

	lfbm_destroy(&bm);
}

static void test_test_and_set(void)
{
	struct lfbm bm;

	CHECK(!lfbm_init(&bm, 200));

	CHECK(!lfbm_test_and_set_bit(&bm, 7));
	CHECK(lfbm_test_and_set_bit(&bm, 7));
	CHECK(lfbm_test_bit(&bm, 7));
	CHECK(!lfbm_test_bit(&bm, 8));
	CHECK(!lfbm_test_bit(&bm, 200));

	/* The taken bit is skipped. */
	CHECK(lfbm_find_and_set_bit(&bm) == 0);
	CHECK(lfbm_find_and_set_bit(&bm) == 1);

	/* Claiming the whole first word makes the next one come out. */
	for (unsigned long i = 2; i < 64; i++)
		CHECK(i == 7 || !lfbm_test_and_set_bit(&bm, i));
	CHECK(lfbm_find_and_set_bit(&bm) == 64);

	CHECK(lfbm_test_and_clear_bit(&bm, 7));
	CHECK(!lfbm_test_and_clear_bit(&bm, 7));
	CHECK(lfbm_find_and_set_bit(&bm) == 7);

	/* Past the end: reads as taken, nothing to free. */
	CHECK(lfbm_test_and_set_bit(&bm, 200));
	CHECK(!lfbm_test_and_clear_bit(&bm, 200));

	lfbm_destroy(&bm);
}

static void test_init(void)
{
	struct lfbm bm;

	CHECK(lfbm_init(&bm, 0) == -EINVAL);
	CHECK(lfbm_init(&bm, (unsigned long)LONG_MAX + 1) == -EINVAL);

	CHECK(!lfbm_init(&bm, 1));
	CHECK(bm.levels == 1);
	CHECK(lfbm_find_and_set_bit(&bm) == 0);
	CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);
	lfbm_test_and_clear_bit(&bm, 0);
	CHECK(lfbm_find_and_set_bit(&bm) == 0);
	lfbm_destroy(&bm);

	CHECK(!lfbm_init(&bm, 64));
	CHECK(bm.levels == 1);
	lfbm_destroy(&bm);

	CHECK(!lfbm_init(&bm, 65));
	CHECK(bm.levels == 2);
	lfbm_destroy(&bm);

	CHECK(!lfbm_init(&bm, 64UL * 64));
	CHECK(bm.levels == 2);
	lfbm_destroy(&bm);

	CHECK(!lfbm_init(&bm, 64UL * 64 + 1));
	CHECK(bm.levels == 3);
	lfbm_destroy(&bm);
}

struct stress {
	struct lfbm *bm;
	atomic_ulong *owner; /* who holds the bit, 0 for nobody */
	unsigned int id;
	unsigned long iters;
	unsigned int hold;
	unsigned long nospc;
};

static uint64_t xorshift(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;

	return *s;
}

static void *stress_run(void *arg)
{
	struct stress *st = arg;
	unsigned long held[64];
	unsigned int n = 0;
	uint64_t seed = 0x9e3779b97f4a7c15ULL * (st->id + 1);
	unsigned long it;

	for (it = 0; it < st->iters; it++) {
		bool take = n < st->hold && (n == 0 || (xorshift(&seed) & 1));

		if (take) {
			long bit = lfbm_find_and_set_bit(st->bm);

			if (bit == -ENOSPC) {
				st->nospc++;
				continue;
			}

			CHECK(bit >= 0);
			/* Two owners of one bit is exactly what must never happen. */
			CHECK(atomic_exchange(&st->owner[bit], st->id + 1) == 0);
			held[n++] = bit;
		} else if (n) {
			unsigned int k = xorshift(&seed) % n;
			unsigned long bit = held[k];

			held[k] = held[--n];
			CHECK(atomic_exchange(&st->owner[bit], 0) == st->id + 1);
			lfbm_test_and_clear_bit(st->bm, bit);
		}
	}

	while (n) {
		unsigned long bit = held[--n];

		CHECK(atomic_exchange(&st->owner[bit], 0) == st->id + 1);
		lfbm_test_and_clear_bit(st->bm, bit);
	}

	return NULL;
}

static unsigned long test_stress(unsigned long nbits, unsigned int threads, unsigned int hold, unsigned long iters)
{
	pthread_t tid[64];
	struct stress st[64];
	atomic_ulong *owner = calloc(nbits, sizeof(*owner));
	unsigned long nospc = 0;
	unsigned int t;
	unsigned long i;
	struct lfbm bm;

	CHECK(owner);
	CHECK(threads <= 64 && hold <= 64);
	CHECK(!lfbm_init(&bm, nbits));

	for (t = 0; t < threads; t++) {
		st[t] = (struct stress){ &bm, owner, t, iters, hold, 0 };
		CHECK(!pthread_create(&tid[t], NULL, stress_run, &st[t]));
	}

	for (t = 0; t < threads; t++) {
		pthread_join(tid[t], NULL);
		nospc += st[t].nospc;
	}

	/* Nothing is held: the hints have to be exact, or this does not come out. */
	CHECK(lfbm_weight(&bm) == 0);
	for (i = 0; i < nbits; i++)
		CHECK(lfbm_find_and_set_bit(&bm) == (long)i);
	CHECK(lfbm_find_and_set_bit(&bm) == -ENOSPC);

	lfbm_destroy(&bm);
	free(owner);

	return nospc;
}

int main(int argc, char **argv)
{
	unsigned long scale = argc > 1 ? strtoul(argv[1], NULL, 10) : 1;
	unsigned long sizes[] = { 1, 2, 63, 64, 65, 127, 128, 4095, 4096, 4097, 262144 + 7, (1UL << 20) + 13 };
	unsigned long nospc;
	unsigned int i;

	test_init();
	test_test_and_set();

	for (i = 0; i < ARRAY_SIZE(sizes); i++)
		test_order(sizes[i]);
	printf("order, test_and_set, init: ok\n");

	/* Plenty of room: not a single -ENOSPC is allowed. */
	nospc = test_stress(4096 + 37, 8, 16, 300000 * scale);
	CHECK(nospc == 0);
	nospc = test_stress(1UL << 20, 8, 32, 300000 * scale);
	CHECK(nospc == 0);
	printf("stress, roomy: ok\n");

	/* Right at the limit, the words keep filling up and emptying. */
	nospc = test_stress(64, 8, 16, 300000 * scale);
	nospc += test_stress(65, 8, 16, 300000 * scale);
	nospc += test_stress(130, 8, 32, 300000 * scale);
	CHECK(nospc > 0);
	printf("stress, tight: ok (%lu -ENOSPC while full)\n", nospc);

	return 0;
}
