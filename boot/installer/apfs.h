// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_INSTALL_APFS_H
#define SWITCH_INSTALL_APFS_H

#include <stdint.h>

// The image's container is composed for SWITCH_APFS_NOMINAL_BYTES and fitted to the partition at install time:
// switch_apfs_fit does in place what `diskutil apfs resizeContainer` does when it shrinks, without moving anything.
#define SWITCH_APFS_BLOCK         4096u
#define SWITCH_APFS_NOMINAL_BYTES (UINT64_C(1) << 40)
#define SWITCH_APFS_EDIT_CIBS     4u
#define SWITCH_APFS_EDIT_BITMAPS  4u
#define SWITCH_APFS_WORK_BYTES    ((9u + SWITCH_APFS_EDIT_CIBS + SWITCH_APFS_EDIT_BITMAPS) * SWITCH_APFS_BLOCK + 1024u)

// APFS's object checksum: Fletcher-64 over 32-bit words, everything after the 8 checksum bytes.
uint64_t switch_apfs_checksum(const uint8_t *block, uint32_t bytes);

// One container block (SWITCH_APFS_BLOCK bytes) by its number in the container; 0 on success.
struct switch_apfs_io {
    void *context;
    int (*read)(void *context, uint64_t block, uint8_t *buffer);
    int (*write)(void *context, uint64_t block, const uint8_t *buffer);
};

// In blocks: the container's size, the smallest switch_apfs_fit takes, and the internal pool, which holds the
// bitmaps the fit edits.
struct switch_apfs_layout {
    uint64_t blocks;
    uint64_t minimum;
    uint64_t pool_base;
    uint64_t pool_blocks;
};

// 0, or nonzero for a container it cannot fit. `work` is SWITCH_APFS_WORK_BYTES.
int switch_apfs_measure(const struct switch_apfs_io *io, void *work, struct switch_apfs_layout *layout);

// The free queue node limits (sfq_tree_node_limit) the kernel requires, exactly, of a container of `blocks` (its main
// queue's) and of an internal pool of `pool_blocks` (the pool's queue's); fsck_apfs does not check them.
uint16_t switch_apfs_main_queue_limit(uint64_t blocks);
uint16_t switch_apfs_pool_queue_limit(uint64_t pool_blocks);

// Shrinks the container to `blocks` (from switch_apfs_measure's minimum up to its size; its size changes nothing).
// Every check runs before the first write, so a nonzero return has written nothing.
int switch_apfs_fit(const struct switch_apfs_io *io, void *work, uint64_t blocks);

#endif
