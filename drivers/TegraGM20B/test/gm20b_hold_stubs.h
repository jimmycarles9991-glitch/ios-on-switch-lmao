// SPDX-License-Identifier: MIT
// hold.c on the host, after gm20b_sched_stubs.h: Linux's bitmaps, and the read-modify-writes the shim makes SEQ_CST
// RMWs (shim/include/nvif/os.h), each one counted by the test.
#pragma once
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
#ifdef __cplusplus
extern "C" {
#endif

#define BITS_PER_LONG 64u
#define BITS_TO_LONGS(bits) (((bits) + 63u) / 64u)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline void __set_bit(unsigned long nr, unsigned long *addr)
{
	addr[nr / 64] |= 1UL << (nr % 64);
}

static inline void __clear_bit(unsigned long nr, unsigned long *addr)
{
	addr[nr / 64] &= ~(1UL << (nr % 64));
}

static inline int bitmap_empty(const unsigned long *addr, unsigned int bits)
{
	for (unsigned int i = 0; i < BITS_TO_LONGS(bits); i++) {
		if (addr[i])
			return 0;
	}
	return 1;
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int bits)
{
	memcpy(dst, src, BITS_TO_LONGS(bits) * sizeof(unsigned long));
}

static inline void bitmap_zero(unsigned long *dst, unsigned int bits)
{
	memset(dst, 0, BITS_TO_LONGS(bits) * sizeof(unsigned long));
}

static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
					  unsigned long offset)
{
	for (; offset < size; offset++) {
		if ((addr[offset / 64] >> (offset % 64)) & 1)
			return offset;
	}
	return size;
}

#define for_each_set_bit(bit, addr, size)                              \
	for ((bit) = find_next_bit((addr), (size), 0); (bit) < (size); \
	     (bit) = find_next_bit((addr), (size), (bit) + 1))

int test_and_clear_bit(unsigned long nr, unsigned long *addr);
int test_and_set_bit(unsigned long nr, unsigned long *addr);
void atomic_inc(atomic_t *v);
int atomic_dec_and_test(atomic_t *v);
int proc_selfpid(void);

#ifdef __cplusplus
}
#endif
