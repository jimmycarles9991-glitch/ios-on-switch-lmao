#include "fat32.h"

/* Format source: Microsoft FAT specification 1.03, pp. 8-18, 23-25, 34-35:
 * https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf
 * Cross-check: Linux fs/fat/fatent.c fat32_ent_get masks to 28 bits. This
 * diagnostic reader additionally refuses bad clusters and divergent mirrors.
 */
struct budget {
    uint32_t io, links;
};

struct chain {
    uint32_t cluster, anchor, power, distance;
};

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

static void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        to[i] = from[i];
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static int cluster_valid(const struct switch_fat_volume *v, uint32_t c)
{
    return c >= 2 && c - 2 < v->cluster_count && c < 0x0ffffff0u;
}

static int sector(struct switch_fat_volume *v, uint64_t lba, uint8_t out[512], struct budget *b)
{
    if (lba >= v->capacity_sectors)
        return SWITCH_FAT_RANGE;
    if (v->mounted && (lba < v->partition_start || lba - v->partition_start >= v->total_sectors))
        return SWITCH_FAT_RANGE;
    if (!b->io)
        return SWITCH_FAT_LIMIT;
    b->io--;
    return v->read512(v->context, lba, out) ? SWITCH_FAT_IO : SWITCH_FAT_OK;
}

/* Only requested whole data sectors use this path. Metadata and partial
 * sectors retain the original single-sector staging and callback contract. */
static int sector_run(struct switch_fat_volume *v, uint64_t lba, uint32_t count, uint8_t *out, struct budget *b)
{
    if (!count || count > SWITCH_FAT_MAX_READ_SECTORS || !v->read_sectors)
        return SWITCH_FAT_ARGUMENT;
    if (lba >= v->capacity_sectors || count > v->capacity_sectors - lba)
        return SWITCH_FAT_RANGE;
    if (v->mounted && (lba < v->partition_start || lba - v->partition_start >= v->total_sectors ||
                       count > v->total_sectors - (lba - v->partition_start)))
        return SWITCH_FAT_RANGE;
    if (count > b->io)
        return SWITCH_FAT_LIMIT;
    b->io -= count;
    return v->read_sectors(v->context, lba, count, out) ? SWITCH_FAT_IO : SWITCH_FAT_OK;
}

static int fat_entry(struct switch_fat_volume *v, uint32_t cluster, uint32_t *value, struct budget *b)
{
    if (!cluster_valid(v, cluster))
        return SWITCH_FAT_CHAIN;
    if (!b->links)
        return SWITCH_FAT_LIMIT;
    b->links--;
    uint64_t index = (uint64_t)cluster * 4 / 512;
    uint32_t byte = (cluster * 4u) % 512;
    if (index >= v->fat_sectors)
        return SWITCH_FAT_RANGE;
    if (!v->cache_valid || v->cache_sector != index) {
        v->cache_valid = 0;
        int rc = sector(v, v->fat_start + (uint64_t)v->active_fat * v->fat_sectors + index, v->cache[0], b);
        if (rc)
            return rc;
        if (v->mirrored && v->fat_count == 2) {
            rc = sector(v, v->fat_start + v->fat_sectors + index, v->cache[1], b);
            if (rc)
                return rc;
        }
        v->cache_sector = index;
        v->cache_valid = 1;
    }
    uint32_t next = le32(v->cache[0] + byte) & 0x0fffffffu;
    if (v->mirrored && v->fat_count == 2 && next != (le32(v->cache[1] + byte) & 0x0fffffffu))
        return SWITCH_FAT_MIRROR;
    if (next >= 0x0ffffff8u) {
        *value = 0;
        return SWITCH_FAT_OK;
    }
    if (!cluster_valid(v, next))
        return SWITCH_FAT_CHAIN;
    *value = next;
    return SWITCH_FAT_OK;
}

static struct chain chain_start(uint32_t c)
{
    struct chain s = {c, c, 1, 0};
    return s;
}

/* Brent's cycle detector needs constant memory; budget also bounds every walk. */
static int chain_next(struct switch_fat_volume *v, struct chain *s, struct budget *b)
{
    uint32_t next = 0;
    int rc = fat_entry(v, s->cluster, &next, b);
    if (rc)
        return rc;
    if (!next) {
        s->cluster = 0;
        return SWITCH_FAT_OK;
    }
    s->distance++;
    if (next == s->anchor)
        return SWITCH_FAT_CHAIN;
    if (s->distance == s->power) {
        s->anchor = next;
        s->distance = 0;
        s->power *= 2;
    }
    s->cluster = next;
    return SWITCH_FAT_OK;
}

static uint64_t cluster_sector(const struct switch_fat_volume *v, uint32_t c)
{
    return v->data_start + (uint64_t)(c - 2) * v->sectors_per_cluster;
}

static int begin_mount(struct switch_fat_volume *v, switch_fat_read512 read512, void *context, uint64_t capacity)
{
    if (!v)
        return SWITCH_FAT_ARGUMENT;
    v->mounted = 0;
    v->cache_valid = 0;
    v->read_sectors = 0;
    v->generation++;
    if (!v->generation)
        v->generation = 1;
    if (!read512 || !capacity)
        return SWITCH_FAT_ARGUMENT;
    v->read512 = read512;
    v->context = context;
    v->capacity_sectors = capacity;
    return SWITCH_FAT_OK;
}

static int mount_bpb(struct switch_fat_volume *v, uint64_t first, uint64_t count, struct budget *b)
{
    if (!first || !count || first >= v->capacity_sectors || count > v->capacity_sectors - first)
        return SWITCH_FAT_RANGE;
    v->partition_start = first;
    v->partition_sectors = count;
    uint8_t buf[512];
    int rc = sector(v, first, buf, b);
    if (rc)
        return rc;
    uint32_t spc = buf[13];
    uint32_t reserved = le16(buf + 14);
    uint32_t fats = buf[16];
    uint32_t total = le32(buf + 32);
    uint32_t fat_size = le32(buf + 36);
    uint32_t flags = le16(buf + 40);
    uint32_t root = le32(buf + 44);
    if (buf[510] != 0x55 || buf[511] != 0xaa || le16(buf + 11) != 512 || !spc || spc > 128 || (spc & (spc - 1)) ||
        !reserved || !fats || fats > 2 || le16(buf + 17) || le16(buf + 19) || le16(buf + 22) || !fat_size || !total ||
        le16(buf + 42) || (flags & 0xff70u))
        return SWITCH_FAT_FORMAT;
    uint64_t overhead = (uint64_t)reserved + (uint64_t)fats * fat_size;
    if (total > count || overhead >= total)
        return SWITCH_FAT_RANGE;
    uint64_t clusters = ((uint64_t)total - overhead) / spc;
    if (clusters < 65525 || clusters > 0x0fffffeeu || (uint64_t)fat_size * 128 < clusters + 2)
        return SWITCH_FAT_FORMAT;
    uint32_t mirrored = (flags & 0x80u) == 0;
    uint32_t active = mirrored ? 0 : flags & 15;
    if (active >= fats)
        return SWITCH_FAT_FORMAT;
    v->fat_start = first + reserved;
    v->data_start = first + overhead;
    v->total_sectors = total;
    v->fat_sectors = fat_size;
    v->cluster_count = (uint32_t)clusters;
    v->root_cluster = root;
    v->sectors_per_cluster = spc;
    v->fat_count = fats;
    v->active_fat = active;
    v->mirrored = mirrored;
    if (!cluster_valid(v, root))
        return SWITCH_FAT_FORMAT;
    v->mounted = 1;
    return SWITCH_FAT_OK;
}

int switch_fat_mount(struct switch_fat_volume *v, switch_fat_read512 read512, void *context, uint64_t capacity,
                     uint32_t partition_index)
{
    int rc = begin_mount(v, read512, context, capacity);
    if (rc)
        return rc;
    if (partition_index >= 4)
        return SWITCH_FAT_ARGUMENT;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    uint8_t buf[512];
    rc = sector(v, 0, buf, &b);
    if (rc)
        return rc;
    if (buf[510] != 0x55 || buf[511] != 0xaa)
        return SWITCH_FAT_FORMAT;
    const uint8_t *p = buf + 446 + (uintptr_t)partition_index * 16;
    if ((p[0] != 0 && p[0] != 0x80) || (p[4] != 0x0b && p[4] != 0x0c))
        return SWITCH_FAT_FORMAT;
    return mount_bpb(v, le32(p + 8), le32(p + 12), &b);
}

int switch_fat_mount_slice(struct switch_fat_volume *v, switch_fat_read512 read512, void *context, uint64_t capacity,
                           uint64_t first, uint64_t count)
{
    int rc = begin_mount(v, read512, context, capacity);
    if (rc)
        return rc;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    return mount_bpb(v, first, count, &b);
}

static int name_char(uint8_t c)
{
    /* The packager's portable subset; no DOS aliases, separators or wildcards. */
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int parse_path(const char *path, uint8_t names[SWITCH_FAT_MAX_DEPTH][11], uint32_t *depth)
{
    if (!path)
        return SWITCH_FAT_ARGUMENT;
    uint32_t length = 0;
    while (length <= SWITCH_FAT_MAX_PATH && path[length])
        length++;
    if (!length || length > SWITCH_FAT_MAX_PATH)
        return SWITCH_FAT_ARGUMENT;
    uint32_t i = path[0] == '/' ? 1 : 0;
    uint32_t n = 0;
    while (i < length) {
        if (n == SWITCH_FAT_MAX_DEPTH)
            return SWITCH_FAT_LIMIT;
        for (uint32_t j = 0; j < 11; j++)
            names[n][j] = ' ';
        uint32_t base = 0;
        uint32_t ext = 0;
        uint32_t dot = 0;
        while (i < length && path[i] != '/') {
            uint8_t c = (uint8_t)path[i++];
            if (c == '.') {
                if (!base || dot)
                    return SWITCH_FAT_ARGUMENT;
                dot = 1;
                continue;
            }
            if (c >= 'a' && c <= 'z')
                c = (uint8_t)(c - 'a' + 'A');
            if (!name_char(c))
                return SWITCH_FAT_ARGUMENT;
            if (!dot) {
                if (base == 8)
                    return SWITCH_FAT_ARGUMENT;
                names[n][base++] = c;
            } else {
                if (ext == 3)
                    return SWITCH_FAT_ARGUMENT;
                names[n][8 + ext++] = c;
            }
        }
        if (!base || (dot && !ext))
            return SWITCH_FAT_ARGUMENT;
        n++;
        if (i < length) {
            ++i;
            if (i == length)
                return SWITCH_FAT_ARGUMENT;
        }
    }
    if (!n)
        return SWITCH_FAT_ARGUMENT;
    *depth = n;
    return SWITCH_FAT_OK;
}

static int lookup(struct switch_fat_volume *v, uint32_t first, const uint8_t name[11], uint8_t entry[32],
                  struct budget *b)
{
    if (!cluster_valid(v, first))
        return SWITCH_FAT_CHAIN;
    struct chain s = chain_start(first);
    uint32_t sectors = 0;
    uint8_t buf[512];
    while (s.cluster) {
        /* Validate this cluster's allocation even if its first entry matches. */
        struct chain next = s;
        int rc = chain_next(v, &next, b);
        if (rc)
            return rc;
        for (uint32_t n = 0; n < v->sectors_per_cluster; n++) {
            if (sectors++ == SWITCH_FAT_MAX_DIR_SECTORS)
                return SWITCH_FAT_LIMIT;
            rc = sector(v, cluster_sector(v, s.cluster) + n, buf, b);
            if (rc)
                return rc;
            for (uint32_t i = 0; i < 512; i += 32) {
                const uint8_t *p = buf + i;
                if (!p[0])
                    return SWITCH_FAT_NOT_FOUND;
                if (p[0] == 0xe5 || (p[11] & 0x3f) == 0x0f || (p[11] & 8))
                    continue;
                if (same(p, name, 11)) {
                    copy_bytes(entry, p, 32);
                    return SWITCH_FAT_OK;
                }
            }
        }
        s = next;
    }
    return SWITCH_FAT_NOT_FOUND;
}

static int cluster_in_sorted(const uint32_t *clusters, uint32_t count, uint32_t key)
{
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (clusters[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < count && clusters[lo] == key;
}

int switch_fat_lookup_size(struct switch_fat_volume *v, const char *path, uint32_t *size)
{
    if (!v || !v->mounted || !v->read512 || !size)
        return SWITCH_FAT_ARGUMENT;
    uint8_t names[SWITCH_FAT_MAX_DEPTH][11];
    uint8_t entry[32];
    uint32_t depth = 0;
    int rc = parse_path(path, names, &depth);
    if (rc)
        return rc;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    uint32_t first = v->root_cluster;
    uint32_t length = 0;
    for (uint32_t i = 0; i < depth; i++) {
        rc = lookup(v, first, names[i], entry, &b);
        if (rc)
            return rc;
        first = (le16(entry + 20) << 16) | le16(entry + 26);
        length = le32(entry + 28);
        if (i + 1 < depth) {
            if (!(entry[11] & 0x10) || length)
                return SWITCH_FAT_NOT_FILE;
            if (!cluster_valid(v, first))
                return SWITCH_FAT_CHAIN;
        } else if (entry[11] & 0x10) {
            return SWITCH_FAT_NOT_FILE;
        }
    }
    if ((!length && first) || (length && !cluster_valid(v, first)))
        return SWITCH_FAT_CHAIN;
    uint32_t entries = 0;
    rc = switch_fat_index_count(v, length, &entries);
    if (rc)
        return rc;
    *size = length;
    return SWITCH_FAT_OK;
}

static int directory_excludes(struct switch_fat_volume *v, uint32_t first, const uint32_t *clusters, uint32_t count,
                              struct budget *b)
{
    if (!cluster_valid(v, first))
        return SWITCH_FAT_CHAIN;
    struct chain s = chain_start(first);
    uint32_t sectors = 0;
    while (s.cluster) {
        if (v->sectors_per_cluster > SWITCH_FAT_MAX_DIR_SECTORS - sectors)
            return SWITCH_FAT_LIMIT;
        sectors += v->sectors_per_cluster;
        if (cluster_in_sorted(clusters, count, s.cluster))
            return SWITCH_FAT_CHAIN;
        int rc = chain_next(v, &s, b);
        if (rc)
            return rc;
    }
    return SWITCH_FAT_OK;
}

int switch_fat_path_directories_exclude(struct switch_fat_volume *v, const char *path, const uint32_t *clusters,
                                        uint32_t count)
{
    if (!v || !v->mounted || !v->read512 || (!clusters && count) || !v->sectors_per_cluster ||
        v->sectors_per_cluster > 128 || (v->sectors_per_cluster & (v->sectors_per_cluster - 1)) ||
        count > v->cluster_count)
        return SWITCH_FAT_ARGUMENT;
    for (uint32_t n = 0; n < count; n++) {
        if (!cluster_valid(v, clusters[n]) || (n && clusters[n - 1] >= clusters[n]))
            return SWITCH_FAT_ARGUMENT;
    }
    uint8_t names[SWITCH_FAT_MAX_DEPTH][11];
    uint8_t entry[32];
    uint32_t depth = 0;
    int rc = parse_path(path, names, &depth);
    if (rc)
        return rc;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    uint32_t first = v->root_cluster;
    for (uint32_t i = 0; i < depth; i++) {
        rc = directory_excludes(v, first, clusters, count, &b);
        if (rc)
            return rc;
        rc = lookup(v, first, names[i], entry, &b);
        if (rc)
            return rc;
        if (i + 1 < depth) {
            if (!(entry[11] & 0x10) || le32(entry + 28))
                return SWITCH_FAT_NOT_FILE;
            first = (le16(entry + 20) << 16) | le16(entry + 26);
        } else if (entry[11] & 0x10) {
            return SWITCH_FAT_NOT_FILE;
        }
    }
    return SWITCH_FAT_OK;
}

int switch_fat_index_count(const struct switch_fat_volume *v, uint32_t size, uint32_t *entries)
{
    if (!entries)
        return SWITCH_FAT_ARGUMENT;
    *entries = 0;
    if (!v || !v->mounted || !v->sectors_per_cluster || v->sectors_per_cluster > 128 ||
        (v->sectors_per_cluster & (v->sectors_per_cluster - 1)))
        return SWITCH_FAT_ARGUMENT;
    uint32_t bytes = v->sectors_per_cluster * 512u;
    uint32_t count = (uint32_t)(((uint64_t)size + bytes - 1) / bytes);
    if (count > v->cluster_count || count > SWITCH_FAT_MAX_CHAIN)
        return SWITCH_FAT_LIMIT;
    *entries = count;
    return SWITCH_FAT_OK;
}

static int open_file(struct switch_fat_volume *v, const char *path, struct switch_fat_file *file, uint32_t *map,
                     uint32_t capacity, int indexed)
{
    if (!file)
        return SWITCH_FAT_ARGUMENT;
    file->volume = 0;
    if (!v || !v->mounted)
        return SWITCH_FAT_ARGUMENT;
    if (indexed && !map && capacity)
        return SWITCH_FAT_ARGUMENT;
    uint8_t names[SWITCH_FAT_MAX_DEPTH][11];
    uint8_t entry[32];
    uint32_t depth = 0;
    int rc = parse_path(path, names, &depth);
    if (rc)
        return rc;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    uint32_t first = v->root_cluster;
    uint32_t size = 0;
    for (uint32_t i = 0; i < depth; i++) {
        rc = lookup(v, first, names[i], entry, &b);
        if (rc)
            return rc;
        first = (le16(entry + 20) << 16) | le16(entry + 26);
        size = le32(entry + 28);
        if (i + 1 < depth) {
            if (!(entry[11] & 0x10) || size)
                return SWITCH_FAT_NOT_FILE;
            if (!cluster_valid(v, first))
                return SWITCH_FAT_CHAIN;
        } else if (entry[11] & 0x10) {
            return SWITCH_FAT_NOT_FILE;
        }
    }
    uint32_t clusters = 0;
    rc = switch_fat_index_count(v, size, &clusters);
    if (rc)
        return rc;
    if (indexed && clusters > capacity)
        return SWITCH_FAT_LIMIT;
    if (!size) {
        if (first)
            return SWITCH_FAT_CHAIN;
    } else {
        if (!cluster_valid(v, first))
            return SWITCH_FAT_CHAIN;
        struct chain s = chain_start(first);
        for (uint32_t n = 0; n < clusters; n++) {
            if (!s.cluster)
                return SWITCH_FAT_CHAIN;
            if (indexed)
                map[n] = s.cluster;
            rc = chain_next(v, &s, &b);
            if (rc)
                return rc;
        }
        if (s.cluster)
            return SWITCH_FAT_CHAIN;
    }
    file->volume = v;
    file->generation = v->generation;
    file->first_cluster = first;
    file->size = size;
    file->clusters = clusters;
    return SWITCH_FAT_OK;
}

int switch_fat_open(struct switch_fat_volume *v, const char *path, struct switch_fat_file *file)
{
    return open_file(v, path, file, 0, 0, 0);
}

int switch_fat_open_indexed(struct switch_fat_volume *v, const char *path, struct switch_fat_indexed_file *file,
                            uint32_t *map, uint32_t capacity)
{
    if (!file)
        return SWITCH_FAT_ARGUMENT;
    *file = (struct switch_fat_indexed_file){0};
    int rc = open_file(v, path, &file->file, map, capacity, 1);
    if (rc)
        return rc;
    file->clusters = map;
    file->count = file->file.clusters;
    return SWITCH_FAT_OK;
}

int switch_fat_read(struct switch_fat_volume *v, const struct switch_fat_file *file, uint64_t offset, void *output,
                    uint32_t length)
{
    if (!v || !v->mounted || !file || file->volume != v || file->generation != v->generation || (!output && length))
        return SWITCH_FAT_ARGUMENT;
    if (offset > file->size || length > (uint64_t)file->size - offset)
        return SWITCH_FAT_RANGE;
    if (!length)
        return SWITCH_FAT_OK;
    if (!cluster_valid(v, file->first_cluster))
        return SWITCH_FAT_CHAIN;
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    struct chain s = chain_start(file->first_cluster);
    uint32_t bytes = v->sectors_per_cluster * 512u;
    uint32_t skip = (uint32_t)(offset / bytes);
    uint32_t within = (uint32_t)(offset % bytes);
    for (uint32_t n = 0; n < skip; n++) {
        int rc = chain_next(v, &s, &b);
        if (rc)
            return rc;
        if (!s.cluster)
            return SWITCH_FAT_CHAIN;
    }
    uint8_t buf[512];
    uint8_t *dest = output;
    while (length) {
        uint32_t sector_index = within / 512;
        uint32_t byte = within % 512;
        uint32_t n = 512 - byte;
        if (n > length)
            n = length;
        uint32_t count = length / 512;
        if (count > v->sectors_per_cluster - sector_index)
            count = v->sectors_per_cluster - sector_index;
        if (count > SWITCH_FAT_MAX_READ_SECTORS)
            count = SWITCH_FAT_MAX_READ_SECTORS;
        int rc = 0;
        if (!byte && count > 1 && v->read_sectors) {
            rc = sector_run(v, cluster_sector(v, s.cluster) + sector_index, count, dest, &b);
            n = count * 512;
        } else {
            rc = sector(v, cluster_sector(v, s.cluster) + sector_index, buf, &b);
            if (!rc)
                copy_bytes(dest, buf + byte, n);
        }
        if (rc)
            return rc;
        dest += n;
        length -= n;
        within += n;
        if (length && within == bytes) {
            rc = chain_next(v, &s, &b);
            if (rc)
                return rc;
            if (!s.cluster)
                return SWITCH_FAT_CHAIN;
            within = 0;
        }
    }
    return SWITCH_FAT_OK;
}

int switch_fat_read_indexed(struct switch_fat_volume *v, const struct switch_fat_indexed_file *indexed, uint64_t offset,
                            void *output, uint32_t length)
{
    if (!v || !v->mounted || !indexed || indexed->file.volume != v || indexed->file.generation != v->generation ||
        (!output && length) || indexed->count != indexed->file.clusters || (indexed->count && !indexed->clusters))
        return SWITCH_FAT_ARGUMENT;
    const struct switch_fat_file *file = &indexed->file;
    if (offset > file->size || length > (uint64_t)file->size - offset)
        return SWITCH_FAT_RANGE;
    if (!length)
        return SWITCH_FAT_OK;
    uint32_t bytes = v->sectors_per_cluster * 512u;
    uint32_t at = (uint32_t)(offset / bytes);
    uint32_t within = (uint32_t)(offset % bytes);
    struct budget b = {SWITCH_FAT_MAX_IO, SWITCH_FAT_MAX_CHAIN};
    uint8_t buf[512];
    uint8_t *dest = output;
    while (length) {
        if (at >= indexed->count || !cluster_valid(v, indexed->clusters[at]))
            return SWITCH_FAT_CHAIN;
        uint32_t sector_index = within / 512;
        uint32_t byte = within % 512;
        uint32_t n = 512 - byte;
        if (n > length)
            n = length;
        uint32_t count = 0;
        if (!byte && length >= 1024 && v->read_sectors) {
            uint32_t limit = length / 512;
            if (limit > SWITCH_FAT_MAX_READ_SECTORS)
                limit = SWITCH_FAT_MAX_READ_SECTORS;
            count = v->sectors_per_cluster - sector_index;
            if (count > limit)
                count = limit;
            uint32_t next = at + 1;
            uint32_t previous = indexed->clusters[at];
            while (count < limit && next < indexed->count && cluster_valid(v, indexed->clusters[next]) &&
                   indexed->clusters[next] == previous + 1) {
                uint32_t add = limit - count;
                if (add > v->sectors_per_cluster)
                    add = v->sectors_per_cluster;
                count += add;
                previous = indexed->clusters[next++];
            }
        }
        int rc = 0;
        if (count > 1) {
            rc = sector_run(v, cluster_sector(v, indexed->clusters[at]) + sector_index, count, dest, &b);
            n = count * 512;
        } else {
            rc = sector(v, cluster_sector(v, indexed->clusters[at]) + sector_index, buf, &b);
            if (!rc)
                copy_bytes(dest, buf + byte, n);
        }
        if (rc)
            return rc;
        dest += n;
        length -= n;
        within += n;
        at += within / bytes;
        within %= bytes;
    }
    return SWITCH_FAT_OK;
}
