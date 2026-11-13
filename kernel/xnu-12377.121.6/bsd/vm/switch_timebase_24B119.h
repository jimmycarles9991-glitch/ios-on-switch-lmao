#ifndef _VM_SWITCH_TIMEBASE_24B119_H_
#define _VM_SWITCH_TIMEBASE_24B119_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 24B119 libswift_Concurrency converts absolute nanoseconds with Apple's 24 MHz
 * ratio in _swift_dispatchEnqueueWithDeadline, bypassing mach_timebase_info.
 * Its clock_gettime input uses X1's real 19.2 MHz timebase. Keep the surrounding
 * overflow, clock-domain, tolerance and cancellation handling unchanged. */
#define SWITCH_TB_PAGE_VMADDR     0x181bec000ULL
#define SWITCH_TB_PAGE_FILEOFF    0x17ec000ULL
#define SWITCH_TB_PAGE_SIZE       0x4000ULL
#define SWITCH_TB_CHUNK_OFFSET    0x2000ULL
#define SWITCH_TB_CHUNK_SIZE      0x1000u
#define SWITCH_TB_INSN_OFFSET     0xc9cu
#define SWITCH_TB_UUID_OFFSET     0x58u
#define SWITCH_TB_PAGER_SLOT      1u

static const uint8_t switch_tb_uuid[16] = {
	0xe7, 0xc1, 0xb4, 0x11, 0xdb, 0x39, 0x30, 0x4d, 0x91, 0x4a, 0x72, 0xe3, 0x1f, 0xa3, 0x1b, 0x86,
};
static const char switch_tb_magic[16] = "dyld_v1  arm64e";

static const uint32_t switch_tb_stock[13] = {
	0xb200f3e9, 0xeb09011f, 0x540006a3, 0xd297cee9, 0xf2a353e9, 0xf2dba5e9, 0xf2e0c489,
	0x9bc97d09, 0xcb090108, 0x8b480528, 0xd346fd08, 0x8b080515, 0x17ffffb7,
};

/* x21 = floor(x8 * 12 / 625), without overflowing before division.
 * mov x9,625; udiv x10,x8,x9; msub x8,x10,x9,x8; mov x9,12;
 * mul x10,x10,x9; mul x8,x8,x9; mov x9,625; udiv x8,x8,x9;
 * add x21,x10,x8; b 0x181beeba8; nop; nop; nop. */
static const uint32_t switch_tb_native[13] = {
	0xd2804e29, 0x9ac9090a, 0x9b09a148, 0xd2800189, 0x9b097d4a, 0x9b097d08, 0xd2804e29,
	0x9ac90908, 0x8b080155, 0x17ffffba, 0xd503201f, 0xd503201f, 0xd503201f,
};

static inline bool
switch_tb_eligible(uint32_t numer, uint32_t denom)
{
	return numer && denom && (uint64_t)numer * 12 == (uint64_t)denom * 625;
}

static inline bool
switch_tb_mapping(uint64_t address, uint64_t size, uint64_t fileoff, uint64_t object_base)
{
	if (address > SWITCH_TB_PAGE_VMADDR || object_base > SWITCH_TB_PAGE_FILEOFF) {
		return false;
	}
	const uint64_t offset = SWITCH_TB_PAGE_VMADDR - address;
	return offset <= size && SWITCH_TB_PAGE_SIZE <= size - offset &&
	       fileoff <= SWITCH_TB_PAGE_FILEOFF && offset == SWITCH_TB_PAGE_FILEOFF - fileoff;
}

static inline bool
switch_tb_header(const uint8_t *header, size_t size)
{
	return size >= SWITCH_TB_UUID_OFFSET + sizeof(switch_tb_uuid) &&
	       memcmp(header, switch_tb_magic, sizeof(switch_tb_magic)) == 0 &&
	       memcmp(header + SWITCH_TB_UUID_OFFSET, switch_tb_uuid, sizeof(switch_tb_uuid)) == 0;
}

static inline bool
switch_tb_patch(void *chunk, size_t size, uint64_t fileoff)
{
	uint8_t *bytes = (uint8_t *)chunk;
	if (size != SWITCH_TB_CHUNK_SIZE || fileoff != SWITCH_TB_PAGE_FILEOFF + SWITCH_TB_CHUNK_OFFSET ||
	    memcmp(bytes + SWITCH_TB_INSN_OFFSET, switch_tb_stock, sizeof(switch_tb_stock)) != 0) {
		return false;
	}
	memcpy(bytes + SWITCH_TB_INSN_OFFSET, switch_tb_native, sizeof(switch_tb_native));
	return true;
}

#endif
