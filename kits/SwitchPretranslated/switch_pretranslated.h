/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * The decisions behind iOS 27's pretranslated System files (kernel/switch/vfs/pretranslated.c): the tree's index
 * (tools/install/overlay_index.py writes it), the tables the kernel builds from it, the changes to the tree it
 * refuses and the executables that keep the System volume's launch status. No kernel state here, so the host tests
 * run the same code.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_PT_MAGIC           0x564f5753u
#define SWITCH_PT_VERSION         2u
#define SWITCH_PT_HEADER          24u
#define SWITCH_PT_RECORD          44u
#define SWITCH_PT_CDHASH          20u
#define SWITCH_PT_MAX_PATH        1024u
#define SWITCH_PT_MAX_FILES       (1u << 20)
#define SWITCH_PT_MAX_DIRECTORIES (1u << 20)
#define SWITCH_PT_MAX_INDEX       ((size_t)64 << 20)

enum switch_pt_error {
    SWITCH_PT_OK = 0,
    SWITCH_PT_SHORT,
    SWITCH_PT_HEADER_BAD,
    SWITCH_PT_SIZE_BAD,
    SWITCH_PT_RECORD_BAD,
    SWITCH_PT_PATH_BAD,
    SWITCH_PT_ORDER_BAD,
    SWITCH_PT_DIRECTORY_BAD,
    SWITCH_PT_DUPLICATE_ID,
    SWITCH_PT_STORAGE_SHORT,
};

struct switch_pt_index {
    const uint8_t *records;
    const uint8_t *directories;
    const char *strings;
    uint32_t count;
    uint32_t dirs;
    uint32_t string_bytes;
};

struct switch_pt_file {
    uint64_t system_id;
    uint64_t data_id;
    const char *path;
    const uint8_t *cdhash;
    uint32_t length;
};

static inline uint32_t switch_pt_u32(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t switch_pt_u64(const uint8_t *p)
{
    uint64_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline bool switch_pt_path_ok(const char *path, uint32_t length)
{
    if (length < 2 || length >= SWITCH_PT_MAX_PATH || path[0] != '/' || path[length - 1] == '/') {
        return false;
    }
    if (length == 7 && __builtin_memcmp(path, "/.index", 7) == 0) {
        return false;
    }
    for (uint32_t at = 1; at <= length;) {
        uint32_t end = at;
        while (end < length && path[end] != '/') {
            ++end;
        }
        const uint32_t part = end - at;
        if (part == 0 || (part == 1 && path[at] == '.') || (part == 2 && path[at] == '.' && path[at + 1] == '.')) {
            return false;
        }
        at = end + 1;
    }
    return true;
}

static inline int switch_pt_bytes_cmp(const char *a, uint32_t alen, const char *b, uint32_t blen)
{
    const int order = __builtin_memcmp(a, b, alen < blen ? alen : blen);
    if (order) {
        return order;
    }
    return alen < blen ? -1 : alen > blen;
}

static inline struct switch_pt_file switch_pt_file_at(const struct switch_pt_index *index, uint32_t i)
{
    const uint8_t *record = index->records + (size_t)SWITCH_PT_RECORD * i;
    struct switch_pt_file file;
    file.system_id = switch_pt_u64(record);
    file.data_id = switch_pt_u64(record + 8);
    file.path = index->strings + switch_pt_u32(record + 16);
    file.cdhash = record + 24;
    file.length = (uint32_t)__builtin_strlen(file.path);
    return file;
}

static inline uint64_t switch_pt_directory_at(const struct switch_pt_index *index, uint32_t i)
{
    return switch_pt_u64(index->directories + (size_t)8 * i);
}

/* Every count, offset and path is checked before anything is read through it; ids are checked by the tables. */
static inline enum switch_pt_error switch_pt_index_parse(const uint8_t *raw, size_t size, struct switch_pt_index *out)
{
    if (size < SWITCH_PT_HEADER) {
        return SWITCH_PT_SHORT;
    }
    const uint32_t count = switch_pt_u32(raw + 8), dirs = switch_pt_u32(raw + 12), strings = switch_pt_u32(raw + 16);
    if (switch_pt_u32(raw) != SWITCH_PT_MAGIC || switch_pt_u32(raw + 4) != SWITCH_PT_VERSION ||
        switch_pt_u32(raw + 20) != 0 || count > SWITCH_PT_MAX_FILES || dirs > SWITCH_PT_MAX_DIRECTORIES ||
        size > SWITCH_PT_MAX_INDEX) {
        return SWITCH_PT_HEADER_BAD;
    }
    const uint64_t want =
        (uint64_t)SWITCH_PT_HEADER + (uint64_t)SWITCH_PT_RECORD * count + (uint64_t)8 * dirs + strings;
    if (want != size) {
        return SWITCH_PT_SIZE_BAD;
    }
    out->records = raw + SWITCH_PT_HEADER;
    out->directories = out->records + (size_t)SWITCH_PT_RECORD * count;
    out->strings = (const char *)(out->directories + (size_t)8 * dirs);
    out->count = count;
    out->dirs = dirs;
    out->string_bytes = strings;
    uint32_t end = 0;
    const char *previous = NULL;
    uint32_t previous_length = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *record = out->records + (size_t)SWITCH_PT_RECORD * i;
        if (switch_pt_u64(record) == 0 || switch_pt_u64(record + 8) == 0 || switch_pt_u32(record + 20) != 0 ||
            switch_pt_u32(record + 16) != end) {
            return SWITCH_PT_RECORD_BAD;
        }
        const char *path = out->strings + end;
        uint32_t length = 0;
        while (end + length < strings && path[length] != '\0') {
            ++length;
        }
        if (end + length >= strings || !switch_pt_path_ok(path, length)) {
            return SWITCH_PT_PATH_BAD;
        }
        if (previous && switch_pt_bytes_cmp(previous, previous_length, path, length) >= 0) {
            return SWITCH_PT_ORDER_BAD;
        }
        previous = path;
        previous_length = length;
        end += length + 1;
    }
    if (end != strings) {
        return SWITCH_PT_SIZE_BAD;
    }
    uint64_t last = 0;
    for (uint32_t i = 0; i < dirs; ++i) {
        const uint64_t id = switch_pt_directory_at(out, i);
        if (id <= last) {
            return SWITCH_PT_DIRECTORY_BAD;
        }
        last = id;
    }
    return SWITCH_PT_OK;
}

/* Open addressing over nonzero 64-bit ids; capacity is a power of two at least twice the entries. */
struct switch_pt_map {
    uint64_t *keys;
    uint32_t *values;
    uint32_t mask;
};

#define SWITCH_PT_DIRECTORY UINT32_MAX
#define SWITCH_PT_PROTECTED (UINT32_MAX - 1)

static inline uint32_t switch_pt_slot(uint64_t key, uint32_t mask)
{
    return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 32) & mask;
}

static inline bool switch_pt_map_put(struct switch_pt_map *map, uint64_t key, uint32_t value)
{
    for (uint32_t at = switch_pt_slot(key, map->mask);; at = (at + 1) & map->mask) {
        if (map->keys[at] == key) {
            return false;
        }
        if (map->keys[at] == 0) {
            map->keys[at] = key;
            map->values[at] = value;
            return true;
        }
    }
}

static inline bool switch_pt_map_get(const struct switch_pt_map *map, uint64_t key, uint32_t *value)
{
    if (key == 0 || map->mask == 0) {
        return false;
    }
    for (uint32_t at = switch_pt_slot(key, map->mask);; at = (at + 1) & map->mask) {
        if (map->keys[at] == key) {
            if (value) {
                *value = map->values[at];
            }
            return true;
        }
        if (map->keys[at] == 0) {
            return false;
        }
    }
}

/* Basenames by the namecache's hash of a component (cn_hash), each slot a file index + 1. */
struct switch_pt_names {
    uint32_t *slots;
    uint32_t *hashes;
    uint32_t mask;
};

struct switch_pt_tables {
    struct switch_pt_index index;
    struct switch_pt_map system;
    struct switch_pt_map data;
    struct switch_pt_names names;
};

typedef uint32_t (*switch_pt_hash_fn)(const char *name, uint32_t length);

static inline uint32_t switch_pt_capacity(uint32_t entries)
{
    uint32_t capacity = 16;
    while (capacity < 2 * entries) {
        capacity <<= 1;
    }
    return capacity;
}

static inline size_t switch_pt_tables_bytes(const struct switch_pt_index *index, uint32_t extra_data_ids)
{
    const size_t files = switch_pt_capacity(index->count);
    const size_t data = switch_pt_capacity(index->count + index->dirs + extra_data_ids);
    return files * (8 + 4) + data * (8 + 4) + files * 4 + (size_t)index->count * 4;
}

static inline const char *switch_pt_basename(const struct switch_pt_file *file, uint32_t *length)
{
    uint32_t at = file->length;
    while (at && file->path[at - 1] != '/') {
        --at;
    }
    *length = file->length - at;
    return file->path + at;
}

/* storage: switch_pt_tables_bytes() bytes, 8-aligned and zeroed. A System id on several records is one file's hard
 * links, so they must share its Data id and CDHash; any other id twice is refused. */
static inline enum switch_pt_error switch_pt_tables_build(struct switch_pt_tables *tables,
                                                          const struct switch_pt_index *index, uint32_t extra_data_ids,
                                                          void *storage, size_t bytes, switch_pt_hash_fn hash)
{
    if (bytes < switch_pt_tables_bytes(index, extra_data_ids)) {
        return SWITCH_PT_STORAGE_SHORT;
    }
    const uint32_t files = switch_pt_capacity(index->count);
    const uint32_t data = switch_pt_capacity(index->count + index->dirs + extra_data_ids);
    uint8_t *at = (uint8_t *)storage;
    tables->index = *index;
    tables->system.keys = (uint64_t *)(void *)at;
    at += (size_t)files * 8;
    tables->data.keys = (uint64_t *)(void *)at;
    at += (size_t)data * 8;
    tables->system.values = (uint32_t *)(void *)at;
    at += (size_t)files * 4;
    tables->data.values = (uint32_t *)(void *)at;
    at += (size_t)data * 4;
    tables->names.slots = (uint32_t *)(void *)at;
    at += (size_t)files * 4;
    tables->names.hashes = (uint32_t *)(void *)at;
    tables->system.mask = files - 1;
    tables->data.mask = data - 1;
    tables->names.mask = files - 1;
    for (uint32_t i = 0; i < index->count; ++i) {
        const struct switch_pt_file file = switch_pt_file_at(index, i);
        uint32_t first;
        if (switch_pt_map_get(&tables->system, file.system_id, &first)) {
            const struct switch_pt_file link = switch_pt_file_at(index, first);
            if (link.data_id != file.data_id || __builtin_memcmp(link.cdhash, file.cdhash, SWITCH_PT_CDHASH) != 0) {
                return SWITCH_PT_DUPLICATE_ID;
            }
        } else if (!switch_pt_map_put(&tables->system, file.system_id, i) ||
                   !switch_pt_map_put(&tables->data, file.data_id, i)) {
            return SWITCH_PT_DUPLICATE_ID;
        }
        uint32_t length;
        const char *name = switch_pt_basename(&file, &length);
        const uint32_t h = hash(name, length);
        tables->names.hashes[i] = h;
        uint32_t slot = h & tables->names.mask;
        while (tables->names.slots[slot]) {
            slot = (slot + 1) & tables->names.mask;
        }
        tables->names.slots[slot] = i + 1;
    }
    for (uint32_t i = 0; i < index->dirs; ++i) {
        if (!switch_pt_map_put(&tables->data, switch_pt_directory_at(index, i), SWITCH_PT_DIRECTORY)) {
            return SWITCH_PT_DUPLICATE_ID;
        }
    }
    return SWITCH_PT_OK;
}

static inline bool switch_pt_name_listed(const struct switch_pt_tables *tables, uint32_t hash, const char *name,
                                         uint32_t length)
{
    for (uint32_t slot = hash & tables->names.mask; tables->names.slots[slot]; slot = (slot + 1) & tables->names.mask) {
        const uint32_t i = tables->names.slots[slot] - 1;
        if (tables->names.hashes[i] != hash) {
            continue;
        }
        const struct switch_pt_file file = switch_pt_file_at(&tables->index, i);
        uint32_t listed_length;
        const char *listed = switch_pt_basename(&file, &listed_length);
        if (listed_length == length && __builtin_memcmp(listed, name, length) == 0) {
            return true;
        }
    }
    return false;
}

/* The System file with this id, if listed under this name (hash: the name's, as switch_pt_name_listed takes it): the
 * file index, or -1. A hard link named differently from the id's first record is found among the name's records. */
static inline int64_t switch_pt_system_file(const struct switch_pt_tables *tables, uint64_t system_id, uint32_t hash,
                                            const char *name, uint32_t length)
{
    uint32_t i;
    if (!switch_pt_map_get(&tables->system, system_id, &i)) {
        return -1;
    }
    struct switch_pt_file file = switch_pt_file_at(&tables->index, i);
    uint32_t listed_length;
    const char *listed = switch_pt_basename(&file, &listed_length);
    if (listed_length == length && __builtin_memcmp(listed, name, length) == 0) {
        return i;
    }
    for (uint32_t slot = hash & tables->names.mask; tables->names.slots[slot]; slot = (slot + 1) & tables->names.mask) {
        i = tables->names.slots[slot] - 1;
        if (tables->names.hashes[i] != hash) {
            continue;
        }
        file = switch_pt_file_at(&tables->index, i);
        listed = switch_pt_basename(&file, &listed_length);
        if (file.system_id == system_id && listed_length == length && __builtin_memcmp(listed, name, length) == 0) {
            return i;
        }
    }
    return -1;
}

enum switch_pt_op {
    SWITCH_PT_OPEN,
    SWITCH_PT_TRUNCATE,
    SWITCH_PT_SETATTR,
    SWITCH_PT_SETEXTATTR,
    SWITCH_PT_DELETEEXTATTR,
    SWITCH_PT_UNLINK,
    SWITCH_PT_RENAME,
    SWITCH_PT_LINK,
    SWITCH_PT_CREATE,
    SWITCH_PT_EXCHANGEDATA,
    SWITCH_PT_CLONE,
    SWITCH_PT_COPYFILE,
    SWITCH_PT_FSCTL,
    SWITCH_PT_MMAP,
    SWITCH_PT_MOUNT,
    SWITCH_PT_GRAFT,
    SWITCH_PT_OPS,
};

/* in_tree: bit n set when the hook's n-th vnode, in the order each case below names them, is in the tree. */
#define SWITCH_PT_VNODE(n)  (1u << (n))
#define SWITCH_PT_ANY_VNODE 0xfu
/* OPEN: for writing, truncating or appending. MMAP: a shared mapping that can write. */
#define SWITCH_PT_WRITES 1u

/* Nothing in the tree changes on the device, root included: these are the vnodes whose being in it refuses each op. */
static inline bool switch_pt_denied(enum switch_pt_op op, uint32_t in_tree, uint32_t intent)
{
    switch (op) {
    case SWITCH_PT_OPEN:  /* vp */
    case SWITCH_PT_MMAP:  /* vp */
    case SWITCH_PT_FSCTL: /* vp: an fsctl or ioctl that writes or changes metadata; a read-only query does not */
        return (in_tree & SWITCH_PT_VNODE(0)) && (intent & SWITCH_PT_WRITES);
    case SWITCH_PT_TRUNCATE:      /* vp */
    case SWITCH_PT_SETATTR:       /* vp: mode, owner, flags, times, ACL, attribute lists */
    case SWITCH_PT_SETEXTATTR:    /* vp */
    case SWITCH_PT_DELETEEXTATTR: /* vp */
    case SWITCH_PT_CREATE:        /* dvp: files, directories, symlinks, nodes */
    case SWITCH_PT_MOUNT:         /* the covered directory */
    case SWITCH_PT_GRAFT:         /* the graft directory */
        return in_tree & SWITCH_PT_VNODE(0);
    case SWITCH_PT_UNLINK:       /* dvp, vp */
    case SWITCH_PT_LINK:         /* destination dvp, the file linked (its source) */
    case SWITCH_PT_EXCHANGEDATA: /* v1, v2 */
    case SWITCH_PT_CLONE:        /* destination dvp, source vp: a copy either way carries the tree's trusted bytes */
    case SWITCH_PT_COPYFILE:     /* destination dvp, source vp: a copy either way carries the tree's trusted bytes */
        return in_tree & (SWITCH_PT_VNODE(0) | SWITCH_PT_VNODE(1));
    case SWITCH_PT_RENAME: /* fdvp, fvp, tdvp, tvp: out of, into, over or of the tree, its root included */
        return in_tree & SWITCH_PT_ANY_VNODE;
    case SWITCH_PT_OPS:
        break;
    }
    return true;
}

struct switch_pt_trust_facts {
    bool listed;
    bool regular;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    bool cdhash_pinned;
    bool cdhash_matches;
};

/* A file of the tree counts as authenticated, as a sealed volume's file does, only if it is listed, a root:wheel
 * regular file nobody else can write, its CDHash both in the trust module pinned for this tree and equal to the one
 * its own index entry carries, so no trusted binary stands in at another listed path. */
static inline bool switch_pt_trusted(const struct switch_pt_trust_facts *facts)
{
    return facts->listed && facts->regular && facts->uid == 0 && facts->gid == 0 && (facts->mode & 022u) == 0 &&
           facts->cdhash_pinned && facts->cdhash_matches;
}
