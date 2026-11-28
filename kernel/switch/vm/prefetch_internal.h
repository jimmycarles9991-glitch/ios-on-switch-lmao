// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_INTERNAL_H
#define SWITCH_PREFETCH_INTERNAL_H

#include <stdint.h>
#include <sys/mount.h>
#include <switch/prefetch_vm.h>
#include "prefetch_list.h"

#define SWITCH_PREFETCH_LOG_ENTRIES (1u << 18) /* 262144 x 32 B = 8 MiB (4 GiB / 512) */
#define SWITCH_PREFETCH_MAX_RUNS    (1u << 18)
#define SWITCH_PREFETCH_CAP_PAGES   ((2ULL << 30) / 4096ULL)

struct switch_prefetch_file_meta {
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

void switch_prefetch_list_path(char *out, size_t size);
void switch_cryptex_mounted(unsigned index, mount_t mount);
void switch_prefetch_volumes_mounted(mount_t root_mount, mount_t data_mount);
void switch_boot_io_report(void);
void switch_boot_io_complete(void);
mount_t switch_prefetch_busy_mount(mount_t expected_mp, fsid_t fsid);
bool switch_prefetch_vnode_meta(vnode_t vp, mount_t expected_mount, uint64_t expected_file_id, vfs_context_t ctx,
                                uint64_t *file_id_out, struct switch_prefetch_file_meta *meta_out);
bool switch_prefetch_vget_regular(mount_t mp, fsid_t fsid, uint64_t file_id, vfs_context_t ctx, vnode_t *vp_out,
                                  struct switch_prefetch_file_meta *meta_out);
struct switch_named_file;
/* The next list is this boot's record log alone: what it read and what it used of the replay, in first-need order.
 * files: the replay's, for naming; sorted here. Takes ownership of *log_p and frees it once ranges are built. */
void switch_prefetch_save_next(struct switch_record_entry **log_p, size_t log_bytes, uint32_t appended,
                               struct switch_named_file *files, uint32_t file_count,
                               const struct switch_prefetch_header *expected_key, mount_t *mounts, const fsid_t *fsids,
                               uint32_t mount_count);

#endif
