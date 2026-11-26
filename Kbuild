# Test module: lfbm.c built for the kernel, run by kthreads (make kmod, make kmod-test).
obj-m := lfbm_test.o
lfbm_test-y := lfbm.o tests/kmod/lfbm_test_main.o

ccflags-y := -I$(src) -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
