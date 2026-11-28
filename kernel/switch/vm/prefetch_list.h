// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_LIST_H
#define SWITCH_PREFETCH_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_PREFETCH_CRYPTEX_COUNT 2u
#define SWITCH_PREFETCH_VOLUME_COUNT  4u
#define SWITCH_PREFETCH_READY_ALL     ((1u << SWITCH_PREFETCH_VOLUME_COUNT) - 1u)
#define SWITCH_PREFETCH_LIST_V2_MAGIC "SWCHPF02"
#define SWITCH_PREFETCH_LIST_V3_MAGIC "SWCHPF03"
#define SWITCH_PREFETCH_LIST_VERSION  3u
#define SWITCH_PREFETCH_MAX_RUN_LIMIT 128u

struct switch_prefetch_ready {
    void *mounts[SWITCH_PREFETCH_VOLUME_COUNT];
    uint32_t mask;
};

struct switch_image_key {
    uint64_t bytes;
    uint64_t file_id;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

/* Frozen SWCHPF02 layout kept so older saved lists are recognized and refused with "another format". */
struct switch_list_header_v2 {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    struct switch_image_key images[SWITCH_PREFETCH_CRYPTEX_COUNT];
    uint32_t runs;
    uint32_t files;
    uint64_t checksum;
};

struct switch_list_run_v2 {
    uint64_t file_id;
    uint32_t page;
    uint16_t pages;
    uint8_t when;
    uint8_t index;
};

struct switch_prefetch_header {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    struct switch_image_key images[SWITCH_PREFETCH_CRYPTEX_COUNT];
    uint8_t root_uuid[16];
    uint8_t data_uuid[16];
    uint32_t runs;
    uint32_t files;
    uint64_t reserved; /* 0: a list is synced before it is renamed in, so nothing checks its runs again */
};

struct switch_prefetch_run {
    uint64_t file_id;
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint32_t page;
    uint16_t pages;
    uint8_t when;
    uint8_t volume;
};

struct switch_prefetch_range {
    uint64_t file_id;
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint32_t first;
    uint32_t end;
    uint16_t when;
    uint8_t volume;
};

struct switch_prefetch_pagein {
    uint64_t offset;
    uint64_t length;
};

struct switch_prefetch_file_ops {
    int (*write_file)(void *ctx, const char *path, const void *header, size_t header_len, const void *runs,
                      size_t runs_len);
    int (*rename_file)(void *ctx, const char *from, const char *to);
    void (*unlink_file)(void *ctx, const char *path);
};

#ifndef __cplusplus
_Static_assert(sizeof(struct switch_image_key) == 32, "SWCHPF image key");
_Static_assert(sizeof(struct switch_list_header_v2) == 96, "SWCHPF02 header");
_Static_assert(sizeof(struct switch_list_run_v2) == 16, "SWCHPF02 run");
_Static_assert(sizeof(struct switch_prefetch_header) == 128, "SWCHPF03 header");
_Static_assert(sizeof(struct switch_prefetch_run) == 40, "SWCHPF03 run");
#else
static_assert(sizeof(struct switch_image_key) == 32, "SWCHPF image key");
static_assert(sizeof(struct switch_list_header_v2) == 96, "SWCHPF02 header");
static_assert(sizeof(struct switch_list_run_v2) == 16, "SWCHPF02 run");
static_assert(sizeof(struct switch_prefetch_header) == 128, "SWCHPF03 header");
static_assert(sizeof(struct switch_prefetch_run) == 40, "SWCHPF03 run");
#endif

static inline int switch_prefetch_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static inline bool switch_prefetch_parse_uuid(const char *str, uint8_t out[16])
{
    if (str == NULL || out == NULL) {
        return false;
    }
    unsigned byte_index = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (str[i] != '-') {
                return false;
            }
            continue;
        }
        const int hi = switch_prefetch_hex_nibble(str[i]);
        const int lo = switch_prefetch_hex_nibble(str[i + 1]);
        if (hi < 0 || lo < 0 || byte_index >= 16) {
            return false;
        }
        out[byte_index++] = (uint8_t)((hi << 4) | lo);
        ++i;
    }
    return str[36] == '\0' && byte_index == 16;
}

static inline bool switch_prefetch_uuid_is_zero(const uint8_t uuid[16])
{
    uint8_t acc = 0;
    for (unsigned i = 0; i < 16u; ++i) {
        acc |= uuid[i];
    }
    return acc == 0u;
}

static inline void switch_prefetch_ready_init(struct switch_prefetch_ready *r)
{
    for (unsigned i = 0; i < SWITCH_PREFETCH_VOLUME_COUNT; ++i) {
        __atomic_store_n(&r->mounts[i], NULL, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&r->mask, 0u, __ATOMIC_RELAXED);
}

static inline bool switch_prefetch_ready_has_mount(const struct switch_prefetch_ready *r, void *mount)
{
    if (mount == NULL) {
        return false;
    }
    for (unsigned i = 0; i < SWITCH_PREFETCH_VOLUME_COUNT; ++i) {
        if (__atomic_load_n(&r->mounts[i], __ATOMIC_ACQUIRE) == mount) {
            return true;
        }
    }
    return false;
}

static inline bool switch_prefetch_ready_add(struct switch_prefetch_ready *r, unsigned index, void *mount)
{
    if (index >= SWITCH_PREFETCH_VOLUME_COUNT || mount == NULL) {
        return false;
    }
    const uint32_t bit = 1u << index;
    if ((__atomic_load_n(&r->mask, __ATOMIC_ACQUIRE) & bit) != 0u) {
        return false;
    }
    if (switch_prefetch_ready_has_mount(r, mount)) {
        return false;
    }
    void *expected = NULL;
    if (!__atomic_compare_exchange_n(&r->mounts[index], &expected, mount, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        return false;
    }
    const uint32_t prev = __atomic_fetch_or(&r->mask, bit, __ATOMIC_ACQ_REL);
    return (prev & bit) == 0u && (prev | bit) == SWITCH_PREFETCH_READY_ALL;
}

static inline bool switch_prefetch_vnode_eligible(bool is_reg, bool is_namedstream, bool is_system, bool is_swap,
                                                  bool is_recycled, bool mount_matches, uint64_t file_id,
                                                  uint64_t expected_file_id, uint64_t file_size, bool nlink_supported,
                                                  uint64_t nlink)
{
    if (!is_reg || is_namedstream || is_system || is_swap || is_recycled || !mount_matches) {
        return false;
    }
    if (file_id == 0u || file_size == 0u) {
        return false;
    }
    if (expected_file_id != 0u && file_id != expected_file_id) {
        return false;
    }
    if (nlink_supported && nlink == 0u) {
        return false;
    }
    return true;
}

static inline bool switch_prefetch_run_matches_file(const struct switch_prefetch_run *run, uint64_t file_size,
                                                    int64_t mtime_sec, int64_t mtime_nsec)
{
    return run->file_size == file_size && run->mtime_sec == mtime_sec && run->mtime_nsec == mtime_nsec;
}

static inline bool switch_prefetch_run_pagein(const struct switch_prefetch_run *run, uint64_t list_page,
                                              uint64_t vm_page, uint64_t file_bytes, struct switch_prefetch_pagein *out)
{
    const uint64_t first = (uint64_t)run->page * list_page;
    const uint64_t mask = vm_page - 1u;
    if (first >= file_bytes) {
        return false;
    }
    const uint64_t eof = (file_bytes + mask) & ~mask;
    const uint64_t end = (first + (uint64_t)run->pages * list_page + mask) & ~mask;
    out->offset = first & ~mask;
    out->length = (end < eof ? end : eof) - out->offset;
    return out->length != 0u;
}

static inline uint64_t switch_prefetch_run_capacity(uint32_t n, uint64_t pages, uint32_t max_run, uint32_t max_gap)
{
    return (uint64_t)n + (pages + (uint64_t)n * max_gap) / max_run + 1u;
}

static inline int switch_prefetch_compare_ranges(const struct switch_prefetch_range *x,
                                                 const struct switch_prefetch_range *y)
{
    if (x->volume != y->volume) {
        return x->volume < y->volume ? -1 : 1;
    }
    if (x->file_id != y->file_id) {
        return x->file_id < y->file_id ? -1 : 1;
    }
    if (x->first != y->first) {
        return x->first < y->first ? -1 : 1;
    }
    return x->end < y->end ? -1 : (int)(x->end > y->end);
}

static inline int switch_prefetch_compare_runs(const struct switch_prefetch_run *x, const struct switch_prefetch_run *y)
{
    if (x->when != y->when) {
        return x->when < y->when ? -1 : 1;
    }
    if (x->volume != y->volume) {
        return x->volume < y->volume ? -1 : 1;
    }
    if (x->file_id != y->file_id) {
        return x->file_id < y->file_id ? -1 : 1;
    }
    return x->page < y->page ? -1 : (int)(x->page > y->page);
}

static inline uint32_t switch_prefetch_merge_runs(const struct switch_prefetch_range *ranges, uint32_t n,
                                                  struct switch_prefetch_run *runs, uint64_t capacity, uint32_t max_run,
                                                  uint32_t max_gap, uint32_t *files)
{
    uint32_t count = 0;
    *files = 0;
    for (uint32_t i = 0; i < n;) {
        const struct switch_prefetch_range *r = &ranges[i];
        uint32_t end = r->end;
        uint16_t when = r->when;
        if (i == 0u || r->file_id != ranges[i - 1u].file_id || r->volume != ranges[i - 1u].volume) {
            ++*files;
        }
        for (++i; i < n && ranges[i].file_id == r->file_id && ranges[i].volume == r->volume &&
                  (uint64_t)ranges[i].first <= (uint64_t)end + max_gap;
             ++i) {
            if (ranges[i].end > end) {
                end = ranges[i].end;
            }
            if (ranges[i].when < when) {
                when = ranges[i].when;
            }
        }
        for (uint64_t page = r->first; page < (uint64_t)end && count < capacity; page += max_run) {
            const uint64_t remain = (uint64_t)end - page;
            const uint32_t pages = remain < max_run ? (uint32_t)remain : max_run;
            runs[count].file_id = r->file_id;
            runs[count].file_size = r->file_size;
            runs[count].mtime_sec = r->mtime_sec;
            runs[count].mtime_nsec = r->mtime_nsec;
            runs[count].page = (uint32_t)page;
            runs[count].pages = (uint16_t)pages;
            runs[count].when = (uint8_t)(when < 255u ? when : 255u);
            runs[count].volume = r->volume;
            ++count;
        }
    }
    return count;
}

static inline uint32_t switch_prefetch_cap_runs(const struct switch_prefetch_run *runs, uint32_t count,
                                                uint64_t cap_pages, uint32_t max_runs)
{
    const uint32_t limit = count < max_runs ? count : max_runs;
    uint64_t pages = 0;
    for (uint32_t i = 0; i < limit; ++i) {
        if (pages + runs[i].pages > cap_pages) {
            return i;
        }
        pages += runs[i].pages;
    }
    return limit;
}

static inline const char *switch_prefetch_header_refusal(const struct switch_prefetch_header *saved,
                                                         const struct switch_prefetch_header *expected,
                                                         uint32_t max_runs)
{
    if (memcmp(saved->magic, SWITCH_PREFETCH_LIST_V2_MAGIC, 8) == 0) {
        return "another format";
    }
    if (memcmp(saved->magic, SWITCH_PREFETCH_LIST_V3_MAGIC, 8) != 0) {
        return "not an SWCHPF03 list";
    }
    if (saved->version != SWITCH_PREFETCH_LIST_VERSION ||
        saved->header_bytes != sizeof(struct switch_prefetch_header)) {
        return "an SWCHPF03 list of another version";
    }
    for (unsigned i = 0; i < SWITCH_PREFETCH_CRYPTEX_COUNT; ++i) {
        if (saved->images[i].bytes != expected->images[i].bytes ||
            saved->images[i].file_id != expected->images[i].file_id ||
            saved->images[i].mtime_sec != expected->images[i].mtime_sec ||
            saved->images[i].mtime_nsec != expected->images[i].mtime_nsec) {
            return "saved for other cryptex images";
        }
    }
    if (switch_prefetch_uuid_is_zero(saved->root_uuid) || switch_prefetch_uuid_is_zero(expected->root_uuid) ||
        memcmp(saved->root_uuid, expected->root_uuid, sizeof(saved->root_uuid)) != 0) {
        return "saved for another root volume";
    }
    if (switch_prefetch_uuid_is_zero(saved->data_uuid) || switch_prefetch_uuid_is_zero(expected->data_uuid) ||
        memcmp(saved->data_uuid, expected->data_uuid, sizeof(saved->data_uuid)) != 0) {
        return "saved for another Data volume";
    }
    if (saved->runs == 0u || saved->runs > max_runs) {
        return "no runs, or too many";
    }
    return NULL;
}

static inline const char *switch_prefetch_validate_runs(const struct switch_prefetch_header *header,
                                                        const struct switch_prefetch_run *runs, size_t byte_len)
{
    const size_t want = (size_t)header->runs * sizeof(struct switch_prefetch_run);
    if (byte_len != want) {
        return "torn (short list)";
    }
    for (uint32_t i = 0; i < header->runs; ++i) {
        if (runs[i].volume >= SWITCH_PREFETCH_VOLUME_COUNT || runs[i].file_id == 0u || runs[i].file_size == 0u ||
            runs[i].pages == 0u || runs[i].pages > SWITCH_PREFETCH_MAX_RUN_LIMIT) {
            return "invalid run";
        }
    }
    return NULL;
}

static inline const char *switch_prefetch_validate_file(const uint8_t *bytes, size_t len,
                                                        const struct switch_prefetch_header *expected,
                                                        uint32_t max_runs, struct switch_prefetch_header *header_out)
{
    if (len >= 8u && memcmp(bytes, SWITCH_PREFETCH_LIST_V2_MAGIC, 8) == 0) {
        return "another format";
    }
    if (len < sizeof(struct switch_prefetch_header)) {
        return "too short";
    }
    struct switch_prefetch_header header;
    memcpy(&header, bytes, sizeof(header));
    const char *why = switch_prefetch_header_refusal(&header, expected, max_runs);
    if (why != NULL) {
        return why;
    }
    const size_t body_len = len - sizeof(header);
    const size_t want = (size_t)header.runs * sizeof(struct switch_prefetch_run);
    if (body_len != want) {
        return "torn (short list)";
    }
    for (uint32_t i = 0; i < header.runs; ++i) {
        struct switch_prefetch_run run;
        memcpy(&run, bytes + sizeof(header) + (size_t)i * sizeof(run), sizeof(run));
        if (run.volume >= SWITCH_PREFETCH_VOLUME_COUNT || run.file_id == 0u || run.file_size == 0u || run.pages == 0u ||
            run.pages > SWITCH_PREFETCH_MAX_RUN_LIMIT) {
            return "invalid run";
        }
    }
    if (header_out != NULL) {
        *header_out = header;
    }
    return NULL;
}

static inline int switch_prefetch_save_atomic(const char *path, const char *boot_uuid,
                                              const struct switch_prefetch_header *header,
                                              const struct switch_prefetch_run *runs, uint32_t run_count,
                                              const struct switch_prefetch_file_ops *ops, void *ctx)
{
    if (path == NULL || boot_uuid == NULL || header == NULL || ops == NULL || ops->write_file == NULL ||
        ops->rename_file == NULL || run_count == 0u) {
        return 22;
    }
    const size_t path_len = strlen(path);
    const size_t uuid_len = strlen(boot_uuid);
    static const char tag[] = ".Switch-";
    const size_t tag_len = sizeof(tag) - 1u;
    char temp[256];
    if (path_len + tag_len + uuid_len + 1u > sizeof(temp)) {
        return 63;
    }
    memcpy(temp, path, path_len);
    memcpy(temp + path_len, tag, tag_len);
    memcpy(temp + path_len + tag_len, boot_uuid, uuid_len + 1u);

    const size_t runs_len = (size_t)run_count * sizeof(struct switch_prefetch_run);
    int error = ops->write_file(ctx, temp, header, sizeof(*header), runs, runs_len);
    if (error == 0) {
        error = ops->rename_file(ctx, temp, path);
    }
    if (error != 0 && ops->unlink_file != NULL) {
        ops->unlink_file(ctx, temp);
    }
    return error;
}

#ifdef __cplusplus
}
#endif

#endif
