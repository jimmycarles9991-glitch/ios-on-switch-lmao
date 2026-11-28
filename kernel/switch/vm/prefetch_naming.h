// SPDX-License-Identifier: GPL-2.0-only
// The save's pure half: one boot's record log, grouped per vnode, named and turned into list ranges (host-tested).
#ifndef SWITCH_PREFETCH_NAMING_H
#define SWITCH_PREFETCH_NAMING_H

#include <stdbool.h>
#include <stdint.h>
#include <switch/prefetch_vm.h>
#include "prefetch_list.h"
#include "prefetch_pace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The log entries of one (vnode, vid, volume): log[first, end) after switch_prefetch_compare_log_entries' sort.
 * file_id 0 means not named yet. */
struct switch_log_group {
    const void *vp;
    uint32_t vid;
    uint32_t first;
    uint32_t end;
    uint64_t file_id;
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint8_t volume;
};

/* A file the replay opened by its id, under the (vnode, vid) it had then; only compared, never dereferenced. */
struct switch_named_file {
    const void *vp;
    uint32_t vid;
    uint8_t volume;
    uint64_t file_id;
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

static inline int switch_prefetch_compare_log_entries(const struct switch_record_entry *x,
                                                      const struct switch_record_entry *y)
{
    if (x->vp != y->vp) {
        return (uintptr_t)x->vp < (uintptr_t)y->vp ? -1 : 1;
    }
    if (x->vid != y->vid) {
        return x->vid < y->vid ? -1 : 1;
    }
    return x->index < y->index ? -1 : (int)(x->index > y->index);
}

static inline int switch_prefetch_compare_named_files(const struct switch_named_file *x,
                                                      const struct switch_named_file *y)
{
    if (x->vp != y->vp) {
        return (uintptr_t)x->vp < (uintptr_t)y->vp ? -1 : 1;
    }
    if (x->vid != y->vid) {
        return x->vid < y->vid ? -1 : 1;
    }
    return x->volume < y->volume ? -1 : (int)(x->volume > y->volume);
}

/* log sorted; returns how many groups switch_prefetch_group_log will emit. */
static inline uint32_t switch_prefetch_count_groups(const struct switch_record_entry *log, uint32_t entries,
                                                    uint32_t volumes)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < entries;) {
        uint32_t j = i + 1u;
        while (j < entries && log[j].vp == log[i].vp && log[j].vid == log[i].vid && log[j].index == log[i].index) {
            ++j;
        }
        if (log[i].index < volumes && log[i].index < SWITCH_PREFETCH_VOLUME_COUNT) {
            ++count;
        }
        i = j;
    }
    return count;
}

/* log sorted; groups holds switch_prefetch_count_groups slots. Entries of volumes at or past `volumes` are left out. */
static inline uint32_t switch_prefetch_group_log(const struct switch_record_entry *log, uint32_t entries,
                                                 uint32_t volumes, struct switch_log_group *groups)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < entries;) {
        uint32_t j = i + 1u;
        while (j < entries && log[j].vp == log[i].vp && log[j].vid == log[i].vid && log[j].index == log[i].index) {
            ++j;
        }
        if (log[i].index < volumes && log[i].index < SWITCH_PREFETCH_VOLUME_COUNT) {
            const struct switch_log_group group = {log[i].vp, log[i].vid, i, j, 0, 0, 0, 0, (uint8_t)log[i].index};
            groups[count++] = group;
        }
        i = j;
    }
    return count;
}

/* Names the unnamed groups the replay's files (sorted) cover; Data's only when `data_too`, since a Data file can
 * change after the replay read its size and mtime. Returns how many it named. */
static inline uint32_t switch_prefetch_name_from_files(struct switch_log_group *groups, uint32_t count,
                                                       const struct switch_named_file *files, uint32_t file_count,
                                                       bool data_too)
{
    uint32_t named = 0;
    for (uint32_t g = 0; g < count; ++g) {
        struct switch_log_group *group = &groups[g];
        if (group->file_id != 0u || (!data_too && group->volume == SWITCH_PREFETCH_VOL_DATA)) {
            continue;
        }
        const struct switch_named_file key = {group->vp, group->vid, group->volume, 0, 0, 0, 0};
        uint32_t lo = 0;
        uint32_t hi = file_count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2u;
            if (switch_prefetch_compare_named_files(&files[mid], &key) < 0) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo < file_count && switch_prefetch_compare_named_files(&files[lo], &key) == 0) {
            group->file_id = files[lo].file_id;
            group->file_size = files[lo].file_size;
            group->mtime_sec = files[lo].mtime_sec;
            group->mtime_nsec = files[lo].mtime_nsec;
            ++named;
        }
    }
    return named;
}

/* One range per log entry of a named group, in list pages; unnamed groups' bytes go to *unnamed_bytes. */
static inline uint32_t switch_prefetch_log_ranges(const struct switch_record_entry *log,
                                                  const struct switch_log_group *groups, uint32_t count,
                                                  struct switch_prefetch_range *out, uint64_t *pages,
                                                  uint64_t *unnamed_bytes)
{
    uint32_t n = 0;
    for (uint32_t g = 0; g < count; ++g) {
        for (uint32_t i = groups[g].first; i < groups[g].end; ++i) {
            const struct switch_record_entry *e = &log[i];
            if (e->when == 0u || e->length == 0u || e->offset > UINT64_MAX - e->length - SWITCH_PREFETCH_LIST_PAGE) {
                continue;
            }
            if (groups[g].file_id == 0u) {
                *unnamed_bytes += e->length;
                continue;
            }
            const uint64_t first = e->offset / SWITCH_PREFETCH_LIST_PAGE;
            const uint64_t end = (e->offset + e->length + SWITCH_PREFETCH_LIST_PAGE - 1u) / SWITCH_PREFETCH_LIST_PAGE;
            if (end <= first || end > UINT32_MAX) {
                continue;
            }
            const struct switch_prefetch_range range = {
                groups[g].file_id,
                groups[g].file_size,
                groups[g].mtime_sec,
                groups[g].mtime_nsec,
                (uint32_t)first,
                (uint32_t)end,
                e->when,
                groups[g].volume,
            };
            out[n++] = range;
            *pages += end - first;
        }
    }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif
