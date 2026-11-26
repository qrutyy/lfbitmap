# Userspace tests of lfbm.c, built against compat/compat.h.
#
#   make test        plain build
#   make tsan        ThreadSanitizer build
#   make asan        AddressSanitizer and UBSan build
#   make test SCALE=10   longer stress runs

CC ?= gcc
SCALE ?= 1
CFLAGS := -std=gnu18 -O2 -g -Wall -Wextra -Wno-unused-parameter -Icompat -I.
LDLIBS := -lpthread
SRCS := lfbm.c tests/test_lfbm.c

all: test

build/test: $(SRCS) lfbm.h compat/compat.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDLIBS)

build/tsan: $(SRCS) lfbm.h compat/compat.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=thread $(SRCS) -o $@ $(LDLIBS)

build/asan: $(SRCS) lfbm.h compat/compat.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(SRCS) -o $@ $(LDLIBS)

test: build/test
	./build/test $(SCALE)

tsan: build/tsan
	./build/tsan $(SCALE)

asan: build/asan
	./build/asan $(SCALE)

# Kernel build of lfbm.c with a test module that runs the checks in kthreads.
#   make kmod        build lfbm_test.ko against the running kernel (or KDIR=...)
#   make kmod-test   load it (needs root), print the result from dmesg and unload it
KDIR ?= /lib/modules/$(shell uname -r)/build

kmod:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

kmod-test: kmod
	-sudo rmmod lfbm_test 2>/dev/null
	-sudo insmod lfbm_test.ko
	@sudo dmesg | grep lfbm_test | tail -5
	-sudo rmmod lfbm_test 2>/dev/null

clean:
	rm -rf build
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null

.PHONY: all test tsan asan kmod kmod-test clean
