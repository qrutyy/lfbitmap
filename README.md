# lfbitmap

A lock-free bitmap for the Linux kernel that hands out the **lowest free bit**,
with a 64-bit index (no `INT_MAX` limit of `ida`).

Level 0 holds the bits. A bit of level `k + 1` says that the word with the same
number on level `k` is full, up to one word on top. Allocation walks the hints
down from the top, one `ffz()` per level, so the cost is `O(levels)` (6 levels
for 2^36 bits) instead of a scan over the full words in front of the first free
one. Taking a bit is a `try_cmpxchg()` on one word, freeing is a `test_and_clear_bit()`.

```c
struct lfbm bm;

lfbm_init(&bm, nbits);        /* may sleep, allocates once */
bit = lfbm_find_and_set_bit(&bm);   /* lowest free bit, or -ENOSPC; no locks, no allocation */
lfbm_test_and_set_bit(&bm, 7);      /* like the kernel: old value, true if it was taken already */
lfbm_test_and_clear_bit(&bm, bit);  /* old value, true if it was taken and is free now */
lfbm_test_bit(&bm, bit);
lfbm_destroy(&bm);
```

## Semantics

- `lfbm_find_and_set_bit()` returns the lowest free bit **as seen at the time**: with
  concurrent frees it may return a higher one, never one that is taken.
- A bit is never handed out twice.
- `lfbm_test_and_set_bit()` and `lfbm_test_and_clear_bit()` follow the kernel:
  they return the old value. A bit past the end warns; set reports it as taken,
  clear returns false. Clearing a free bit is not warned about.
- `-ENOSPC` (the kernel's `find_and_set_bit()` returns `nbits` instead) may be
  false while a parallel `lfbm_test_and_clear_bit()`, or a parallel search that
  has just filled a word, is still setting or clearing the hints: a late hint
  may be set before the word is re-read, and a search on a nearly full map then
  sees the top word full. On a map nobody else touches the result is exact.
- Hints are only hints. A hint that is wrong in either direction is fixed by
  the next allocation that walks into it, and when nothing is held any more
  they are exact (the tests check that).
- `lfbm_weight()` is not a snapshot while others allocate or free.
- The size is fixed at `lfbm_init()`: one bit per cell, `nbits / 8` bytes plus
  about 1.6% for the hints.

## Using it as a submodule

```make
# Kbuild
lsv-objs += utils/lfbitmap/lfbm.o
ccflags-y += -I$(src)/utils/lfbitmap
```

```c
#include "lfbm.h"
```

`compat/` and `tests/` are not part of the kernel build.

## Tests

The algorithm runs in userspace on top of `compat/compat.h`, which maps the few
kernel primitives to the GCC atomics:

```
make test            # order, edges of the sizes, stress with 8 threads
make tsan            # the same under ThreadSanitizer
make asan            # AddressSanitizer + UBSan
make test SCALE=10   # longer runs
```

The kernel side is checked by a module that builds `lfbm.c` against a real
kernel and runs the same stress in kthreads. Loading it fails with `-EIO` if a
check does not hold:

```
make kmod            # against /lib/modules/$(uname -r)/build, or KDIR=...
make kmod-test       # insmod, print the result from dmesg, rmmod (needs sudo)
```

Tested on 7.2.9 (aarch64).

## License

GPL-2.0-only.
