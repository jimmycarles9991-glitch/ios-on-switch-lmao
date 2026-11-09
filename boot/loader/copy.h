#ifndef SWITCH_LOADER_COPY_H
#define SWITCH_LOADER_COPY_H

#define LOADER_COPY_TABLE_PAGES   8
#define LOADER_COPY_TABLE_ENTRIES 512
#define LOADER_COPY_TCR           0x80a03520
#define LOADER_COPY_MAIR          0xff00

#ifndef __ASSEMBLER__
#include <stdint.h>

/* The most the loader decodes into the kernel window: the copy tables map the window from the L2 table of
 * 0x80000000-0xBFFFFFFF, which this leaves room in (0xB2000000 + 128 MiB), and the LZ4 stream is staged separately in
 * bl33.bin below 0xB0000000 (stage.h). tools/package/loader_abi.py WINDOW_BYTES_MAX. */
#define LOADER_WINDOW_BYTES_MAX UINT64_C(0x8000000)

/* The physical table address is explicit so the page-table builder can be
 * checked on the host without executing privileged instructions. */
struct loader_copy_bounds {
    uint64_t source, destination, bytes;
    uint64_t rx_end, ro_end, image_end;
    uint64_t source_bytes; /* the staged file at source (4 KiB multiple); bytes is the window at destination */
};

int loader_copy_build_tables(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES],
                             uint64_t table_address, const struct loader_copy_bounds *bounds);
int loader_copy_prepare(void);
uint64_t loader_copy_run(void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started);
/* Bit 63 of a block's source length: its bytes are stored as they are, not LZ4-compressed. */
#define LOADER_BLOCK_STORED (UINT64_C(1) << 63)
/* destination[0, bytes) = the window: blocks[i] = {offset, length, source offset, source length} in ascending order,
 * each one LZ4 block (lz4.org block format) of the staged file, or stored bytes, read back to back from source until
 * exactly source_bytes are used; zeros elsewhere. Offsets and lengths are 4 KiB multiples inside bytes. 0, or 1 for
 * a table that is not or a block that does not decode to exactly its length (nothing is written outside the window). */
int loader_unpack(uint64_t destination, uint64_t bytes, uint64_t source, uint64_t source_bytes,
                  const uint64_t (*blocks)[4], unsigned count, void (*progress)(uint64_t, uint64_t, uint64_t),
                  uint64_t started);
int loader_copy_payload(void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started);
#endif
#endif
