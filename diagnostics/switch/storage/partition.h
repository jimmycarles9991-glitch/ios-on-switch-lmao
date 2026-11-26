#ifndef SWITCH_PARTITION_H
#define SWITCH_PARTITION_H
#include "fat32.h"

#define SWITCH_GPT_MAX_ENTRIES 128u

enum switch_gpt_error {
    SWITCH_GPT_OK = 0,
    SWITCH_GPT_ARGUMENT = 1,
    SWITCH_GPT_IO = 2,
    SWITCH_GPT_MBR = 3,
    SWITCH_GPT_HEADER = 4,
    SWITCH_GPT_HEADER_CRC = 5,
    SWITCH_GPT_GEOMETRY = 6,
    SWITCH_GPT_ENTRIES = 7,
    SWITCH_GPT_ARRAY_CRC = 8,
    SWITCH_GPT_MISMATCH = 9,
    SWITCH_GPT_NOT_FOUND = 10,
    SWITCH_GPT_OVERLAP = 11,
    SWITCH_GPT_DUPLICATE = 12,
    SWITCH_GPT_PROBE = 13,
    SWITCH_GPT_AMBIGUOUS = 14
};

struct switch_gpt_slice {
    uint64_t first_sector, sector_count;
    uint32_t partition_index; /* Zero-based GPT array index. */
    uint8_t unique_guid[16];  /* Exact on-disk GUID byte order. */
};

struct switch_gpt_header {
    uint64_t first_usable, last_usable, entry_lba;
    uint32_t entry_count, entry_size, array_crc;
    uint8_t disk_guid[16];
};

struct switch_gpt_extent {
    uint64_t first, last;
    uint8_t guid[16], type_guid[16];
    uint32_t partition_index;
    uint8_t basic_data, writable, hos_data;
};

/* Caller-owned scratch, normally static for kernel callers. No initialization
 * needed, no heap, and no pointers into scratch are retained by the result. */
struct switch_gpt_workspace {
    uint8_t sectors[2][512];
    struct switch_gpt_header headers[2];
    struct switch_gpt_extent used[SWITCH_GPT_MAX_ENTRIES];
};

/* Strict GPT validation only, shared by FAT selection and raw APFS discovery.
 * Success fills used[0..*used_count) including exact type GUIDs. The same
 * CRC/copy/geometry/overlap/unique-GUID checks and <=259 reads apply. No MBR
 * fallback and no probes; *used_count is untouched on failure. */
int switch_gpt_scan(switch_fat_read512, void *, uint64_t capacity_sectors, struct switch_gpt_workspace *,
                    uint32_t *used_count);

/* Return 1 only if the slice contains this build's required image paths, 0 if
 * incompatible, or a negative value if bounded metadata probing failed.
 * Called only after the complete GPT validates, at most MAX_ENTRIES times.
 * The callback is read-only and must impose its own I/O/time limits. */
typedef int (*switch_gpt_probe_slice)(void *, const struct switch_gpt_slice *);

/* Read-only, strict GPT v1 profile: 512-byte sectors, 92-byte headers,
 * 1..128 entries of 128/256/512 bytes, at most 64KiB of declared entry data.
 * At least 16KiB must be reserved at each array location. Hekate may declare
 * only created entries; CRCs and copy comparisons cover exactly that count,
 * excluding final-sector padding and undeclared reserved entries. Validates both
 * headers at LBA1/capacity-1, both array CRCs and their exact covered bytes,
 * all used extents, unique GUIDs, and no overlaps. No repair or fallback.
 * MBR must have its signature and a protective type0xEE entry; additional
 * hybrid MBR entries are permitted and never used for partition selection.
 * Probes every Microsoft Basic Data partition after GPT validation and requires
 * exactly one matching slice. No GUID, card identity, partition order or disk
 * capacity is compiled into selection. Ambiguous matches are refused.
 * Capacity must be independently trusted. Serial immutable-media access and
 * read callback semantics are as in fat32.h. GPT validation performs at most
 * 259 sector reads, in addition to the caller's bounded per-slice probes.
 * Failure leaves *output entirely unchanged. */
int switch_gpt_find(switch_fat_read512, void *context, uint64_t capacity_sectors, switch_gpt_probe_slice,
                    void *probe_context, struct switch_gpt_workspace *, struct switch_gpt_slice *output);
/* Runtime partition discovery. A protective MBR selects the strict GPT path
 * above. Otherwise validate all four primary MBR entries and probe only FAT32
 * types 0x0B/0x0C. Require one compatible slice, valid bounds, no overlaps and
 * no extended partition types (0x05/0x0F/0x85). No superfloppy or logical MBR
 * partitions. Other bounded primary partition types are ignored as candidates.
 * GPT performs <=260 table reads including dispatch; MBR performs one.
 * MBR results have an all-zero unique_guid. The callback and untouched-output
 * failure contracts are identical to switch_gpt_find. */
int switch_partition_find(switch_fat_read512, void *context, uint64_t capacity_sectors, switch_gpt_probe_slice,
                          void *probe_context, struct switch_gpt_workspace *, struct switch_gpt_slice *output);
#endif
