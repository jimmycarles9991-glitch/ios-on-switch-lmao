#include "split_disk.h"

static int validate_parts(const struct switch_fat_volume *v, const struct switch_split_part *parts, uint32_t count,
                          uint64_t total)
{
    if (!v || !parts || !count || count > SWITCH_SPLIT_MAX_PARTS || !total || (total & 511))
        return SWITCH_FAT_ARGUMENT;
    /* Validate all descriptor arithmetic before doing I/O. */
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!parts[i].path || !parts[i].size || (parts[i].size & 511) || parts[i].offset != cursor || cursor > total ||
            parts[i].size > total - cursor)
            return SWITCH_FAT_RANGE;
        cursor += parts[i].size;
    }
    if (cursor != total)
        return SWITCH_FAT_RANGE;
    return SWITCH_FAT_OK;
}

int switch_split_open(struct switch_split_disk *disk, struct switch_fat_volume *v,
                      const struct switch_split_part *parts, uint32_t count, uint64_t total)
{
    if (!disk)
        return SWITCH_FAT_ARGUMENT;
    disk->count = 0;
    disk->volume = 0;
    disk->size = 0;
    int rc = validate_parts(v, parts, count, total);
    if (rc)
        return rc;
    for (uint32_t i = 0; i < count; i++) {
        int rc = switch_fat_open(v, parts[i].path, &disk->files[i]);
        if (rc)
            return rc;
        if (disk->files[i].size != parts[i].size)
            return SWITCH_FAT_RANGE;
        disk->offsets[i] = parts[i].offset;
    }
    disk->volume = v;
    disk->size = total;
    disk->count = count;
    return SWITCH_FAT_OK;
}

int switch_split_read(struct switch_split_disk *disk, uint64_t offset, void *output, uint32_t length)
{
    if (!disk || !disk->volume || !disk->count || disk->count > SWITCH_SPLIT_MAX_PARTS || (!output && length))
        return SWITCH_FAT_ARGUMENT;
    if (offset > disk->size || length > disk->size - offset)
        return SWITCH_FAT_RANGE;
    /* Also validate remount lifetime on zero-length reads. */
    int rc = switch_fat_read(disk->volume, &disk->files[0], 0, 0, 0);
    if (rc || !length)
        return rc;
    uint8_t *dest = output;
    for (uint32_t i = 0; i < disk->count && length; i++) {
        uint64_t start = disk->offsets[i];
        uint64_t size = disk->files[i].size;
        if (offset >= start && offset - start < size) {
            uint64_t local = offset - start;
            uint64_t available = size - local;
            uint32_t n = available < length ? (uint32_t)available : length;
            rc = switch_fat_read(disk->volume, &disk->files[i], local, dest, n);
            if (rc)
                return rc;
            offset += n;
            dest += n;
            length -= n;
        }
    }
    return length ? SWITCH_FAT_RANGE : SWITCH_FAT_OK;
}

int switch_split_index_count(const struct switch_fat_volume *v, const struct switch_split_part *parts, uint32_t count,
                             uint64_t total, uint32_t *entries)
{
    if (!entries)
        return SWITCH_FAT_ARGUMENT;
    *entries = 0;
    int rc = validate_parts(v, parts, count, total);
    if (rc)
        return rc;
    uint32_t total_entries = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t needed = 0;
        rc = switch_fat_index_count(v, parts[i].size, &needed);
        if (rc)
            return rc;
        if (needed > UINT32_MAX - total_entries)
            return SWITCH_FAT_LIMIT;
        total_entries += needed;
    }
    *entries = total_entries;
    return SWITCH_FAT_OK;
}

int switch_split_open_indexed(struct switch_split_indexed_disk *disk, struct switch_fat_volume *v,
                              const struct switch_split_part *parts, uint32_t count, uint64_t total, uint32_t *map,
                              uint32_t capacity)
{
    if (!disk)
        return SWITCH_FAT_ARGUMENT;
    disk->count = 0;
    disk->volume = 0;
    disk->size = 0;
    if (!map && capacity)
        return SWITCH_FAT_ARGUMENT;
    uint32_t needed = 0;
    int rc = switch_split_index_count(v, parts, count, total, &needed);
    if (rc)
        return rc;
    if (needed > capacity)
        return SWITCH_FAT_LIMIT;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t part_entries = 0;
        rc = switch_fat_index_count(v, parts[i].size, &part_entries);
        if (rc)
            return rc;
        uint32_t *entries = map;
        if (entries)
            entries += at;
        rc = switch_fat_open_indexed(v, parts[i].path, &disk->files[i], entries, part_entries);
        if (rc)
            return rc;
        if (disk->files[i].file.size != parts[i].size)
            return SWITCH_FAT_RANGE;
        disk->offsets[i] = parts[i].offset;
        at += part_entries;
    }
    disk->volume = v;
    disk->size = total;
    disk->count = count;
    return SWITCH_FAT_OK;
}

int switch_split_read_indexed(struct switch_split_indexed_disk *disk, uint64_t offset, void *output, uint32_t length)
{
    if (!disk || !disk->volume || !disk->count || disk->count > SWITCH_SPLIT_MAX_PARTS || (!output && length))
        return SWITCH_FAT_ARGUMENT;
    if (offset > disk->size || length > disk->size - offset)
        return SWITCH_FAT_RANGE;
    int rc = switch_fat_read_indexed(disk->volume, &disk->files[0], 0, 0, 0);
    if (rc || !length)
        return rc;
    uint8_t *dest = output;
    for (uint32_t i = 0; i < disk->count && length; i++) {
        uint64_t start = disk->offsets[i];
        uint64_t size = disk->files[i].file.size;
        if (offset >= start && offset - start < size) {
            uint64_t local = offset - start;
            uint64_t available = size - local;
            uint32_t n = available < length ? (uint32_t)available : length;
            rc = switch_fat_read_indexed(disk->volume, &disk->files[i], local, dest, n);
            if (rc)
                return rc;
            offset += n;
            dest += n;
            length -= n;
        }
    }
    return length ? SWITCH_FAT_RANGE : SWITCH_FAT_OK;
}
