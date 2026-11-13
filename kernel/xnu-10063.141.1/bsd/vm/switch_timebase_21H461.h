#ifndef _VM_SWITCH_TIMEBASE_21H461_H_
#define _VM_SWITCH_TIMEBASE_21H461_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 21H461 Swift converts absolute nanoseconds using Apple's 24 MHz ratio.
 * Keep its clock-domain selection and x8=1e9 leeway multiplier intact while
 * adapting the conversion to X1's 19.2 MHz counter. */
#define SWITCH_TB_PAGE_VMADDR     0x192698000ULL
#define SWITCH_TB_PAGE_FILEOFF    0x4c00000ULL
#define SWITCH_TB_PAGE_SIZE       0x4000ULL
#define SWITCH_TB_CHUNK_OFFSET    0x0ULL
#define SWITCH_TB_CHUNK_SIZE      0x1000u
#define SWITCH_TB_INSN_OFFSET     0x5f8u
#define SWITCH_TB_UUID_OFFSET     0x58u
#define SWITCH_TB_PAGER_SLOT      1u

static const uint8_t switch_tb_uuid[16] = {
	0x6d, 0xd9, 0xbe, 0x0f, 0x02, 0x0a, 0x30, 0x2f, 0x8d, 0xaf, 0x66, 0xc9, 0xa6, 0x79, 0x64, 0x8d,
};
static const char switch_tb_magic[16] = "dyld_v1   arm64";

static const uint32_t switch_tb_stock[13] = {
	0xb200f3ea, 0xeb0a013f, 0x54000163, 0xd297ceea, 0xf2a353ea, 0xf2dba5ea, 0xf2e0c48a,
	0x9bca7d2a, 0xcb0a0129, 0x8b490549, 0xd346fd29, 0x8b090529, 0x1400000a,
};

/* x9 = floor(x9 * 12 / 625), with a quotient/remainder split to avoid overflow.
 * x10 and x11 are scratch; the final branch rejoins at 0x192698650. */
static const uint32_t switch_tb_native[13] = {
	0xd2804e2a, 0x9aca092b, 0x9b0aa569, 0xd280018a, 0x9b0a7d6b, 0x9b0a7d29, 0xd2804e2a,
	0x9aca0929, 0x8b090169, 0x1400000d, 0xd503201f, 0xd503201f, 0xd503201f,
};

static inline bool
switch_tb_eligible(unsigned slot, uint32_t numer, uint32_t denom)
{
	return slot == SWITCH_TB_PAGER_SLOT && numer && denom &&
	       (uint64_t)numer * 12 == (uint64_t)denom * 625;
}

static inline bool
switch_tb_mapping(uint64_t address, uint64_t size, uint64_t fileoff)
{
	if (address > SWITCH_TB_PAGE_VMADDR) {
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
