#include "raw_apfs.h"
#include <string.h>

#define RAW_MAGIC UINT64_C(0x5357434852415750)
static const uint8_t apfs_type[16] = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                      0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int nonzero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i])
            return 1;
    }
    return 0;
}

static int fail(struct switch_raw_apfs *b, int error)
{
    b->online = 0;
    b->io.offline(b->io.context);
    if (b->bound) {
        b->io.unbind(b->io.context, b);
        b->bound = 0;
    }
    return error;
}

static int healthy(struct switch_raw_apfs *b)
{
    if (!b || b->magic != RAW_MAGIC || !b->online)
        return 0;
    if (b->io.ready(b->io.context)) {
        fail(b, SWITCH_BLOCK_NOT_READY);
        return 0;
    }
    return 1;
}

static int permit(void *context, uint64_t lba, uint32_t count)
{
    struct switch_raw_apfs *b = context;
    return healthy(b) && count && count <= 128 && lba >= b->first_sector &&
           lba - b->first_sector < b->partition_sectors && count <= b->partition_sectors - (lba - b->first_sector);
}

static int finish(struct switch_raw_apfs *b, int error)
{
    int end = b->io.end(b->io.context);
    return end || error ? fail(b, end ? end : error) : SWITCH_BLOCK_OK;
}

/* Apple File System Reference, obj_phys_t and nx_superblock_t. Exclude the
 * checksum words; the two stored words complete the Fletcher-64 sums. */
static uint64_t checksum(const uint8_t *p, uint32_t size)
{
    const uint64_t mod = UINT64_C(0xffffffff);
    uint64_t a = 0;
    uint64_t b = 0;
    for (uint32_t at = 8; at < size; at += 4) {
        a += le32(p + at);
        b += a;
    }
    a %= mod;
    b %= mod;
    uint64_t x = mod - (a + b) % mod;
    uint64_t y = mod - (a + x) % mod;
    return x | (y << 32);
}

static int area(uint64_t base, uint32_t raw_count, uint64_t blocks)
{
    uint32_t count = raw_count & 0x7fffffff;
    if (!count || !base || base >= blocks || count > blocks)
        return 0;
    /* Fragmented checkpoint areas name a B-tree root, not a contiguous run. */
    return (raw_count & 0x80000000) || count <= blocks - base;
}

int switch_raw_apfs_validate_nxsb(const void *block, uint32_t available_bytes, uint64_t partition_sector_count)
{
    if (!block || available_bytes < 4096 || !partition_sector_count)
        return SWITCH_BLOCK_INVALID;
    const uint8_t *p = block;
    uint32_t size = le32(p + 36);
    if (size < 4096 || size > 65536 || (size & (size - 1)) || size > available_bytes)
        return SWITCH_BLOCK_CORRUPT;
    uint64_t blocks = le64(p + 40);
    uint32_t type = le32(p + 24);
    if (le64(p) != checksum(p, size) || le64(p + 8) != 1 || !le64(p + 16) || (type & 0xffff) != 1 ||
        (type & 0x30000000) || le32(p + 28) || le32(p + 32) != 0x4253584e || le32(p + 36) != size || !blocks ||
        blocks > partition_sector_count / (size / 512) || !nonzero(p + 72, 16) || (le64(p + 64) & UINT64_C(0x100)) ||
        nonzero(p + 1280, 16) || !area(le64(p + 112), le32(p + 104), blocks) ||
        !area(le64(p + 120), le32(p + 108), blocks) || !le64(p + 160) || le64(p + 160) >= blocks || !le32(p + 180) ||
        le32(p + 180) > 100)
        return SWITCH_BLOCK_CORRUPT;
    if (!(le32(p + 104) & 0x80000000) && !(le32(p + 108) & 0x80000000)) {
        uint64_t a = le64(p + 112);
        uint64_t z = le64(p + 120);
        if (a < z + le32(p + 108) && z < a + le32(p + 104))
            return SWITCH_BLOCK_CORRUPT;
    }
    return SWITCH_BLOCK_OK;
}

int switch_raw_apfs_open(struct switch_raw_apfs *b, const struct switch_raw_apfs_io *io, uint64_t capacity_sectors,
                         const struct switch_raw_apfs_partition *parts, uint32_t count,
                         const uint8_t expected_container_uuid[16], void *private_scratch, uint32_t scratch_bytes)
{
    if (!b || b->magic == RAW_MAGIC || !io || !io->ready || !io->begin || !io->end || !io->offline || !io->read ||
        !io->qualify || !io->unbind || !io->write || !io->sync || (!parts && count) ||
        count > SWITCH_RAW_APFS_MAX_PARTITIONS || !capacity_sectors || capacity_sectors > UINT64_MAX / 512 ||
        !expected_container_uuid || !nonzero(expected_container_uuid, 16) || !private_scratch ||
        scratch_bytes < SWITCH_RAW_APFS_SCRATCH_BYTES)
        return SWITCH_BLOCK_INVALID;
    memset(b, 0, sizeof(*b));
    b->magic = RAW_MAGIC;
    b->io = *io;
    const struct switch_raw_apfs_partition *selected = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct switch_raw_apfs_partition *p = &parts[i];
        if (!p->first_sector || !p->sector_count || p->first_sector >= capacity_sectors ||
            p->sector_count > capacity_sectors - p->first_sector || !nonzero(p->type_guid, 16))
            return fail(b, SWITCH_BLOCK_CORRUPT);
        for (uint32_t j = 0; j < i; j++) {
            if (p->first_sector < parts[j].first_sector + parts[j].sector_count &&
                parts[j].first_sector < p->first_sector + p->sector_count)
                return fail(b, SWITCH_BLOCK_CORRUPT);
        }
        if (!memcmp(p->type_guid, apfs_type, 16)) {
            if (selected)
                return fail(b, SWITCH_BLOCK_CORRUPT);
            selected = p;
        }
    }
    if (!selected)
        return SWITCH_BLOCK_NOT_READY;
    b->first_sector = selected->first_sector;
    b->partition_sectors = selected->sector_count;
    if (b->partition_sectors < 8)
        return fail(b, SWITCH_BLOCK_CORRUPT);
    int status = io->ready(io->context);
    if (status)
        return fail(b, status);
    status = io->begin(io->context, 129, 5);
    if (status)
        return fail(b, status);
    uint8_t *bytes = private_scratch;
    status = io->read(io->context, b->first_sector, 1, bytes);
    if (!status) {
        uint32_t size = le32(bytes + 36);
        if (le32(bytes + 32) != 0x4253584e || size < 4096 || size > 65536 || (size & (size - 1)) ||
            size / 512 > b->partition_sectors) {
            status = SWITCH_BLOCK_CORRUPT;
        } else {
            status = io->read(io->context, b->first_sector, size / 512, bytes);
            if (!status)
                status = switch_raw_apfs_validate_nxsb(bytes, size, b->partition_sectors);
            if (!status && memcmp(bytes + 72, expected_container_uuid, 16))
                status = SWITCH_BLOCK_CORRUPT;
            if (!status) {
                b->block_size = size;
                b->container_sectors = le64(bytes + 40) * (size / 512);
                memcpy(b->container_uuid, bytes + 72, 16);
            }
        }
    }
    status = finish(b, status);
    if (status)
        return status;
    b->online = 1;
    b->bound = 1;
    status = io->qualify(io->context, permit, b);
    return status ? fail(b, status) : SWITCH_BLOCK_OK;
}

int switch_raw_apfs_get_info(struct switch_raw_apfs *b, struct switch_block_info *info)
{
    if (!info)
        return SWITCH_BLOCK_INVALID;
    memset(info, 0, sizeof(*info));
    if (!healthy(b))
        return SWITCH_BLOCK_NOT_READY;
    info->size_bytes = b->partition_sectors * 512;
    memcpy(info->label, "APFS", 5);
    return SWITCH_BLOCK_OK;
}

static int request(struct switch_raw_apfs *b, uint64_t offset, const void *bytes, uint32_t n)
{
    if (!bytes || !n || n > SWITCH_BLOCK_MAX_TRANSFER_BYTES || ((offset | n) & 511))
        return SWITCH_BLOCK_INVALID;
    if (!healthy(b))
        return SWITCH_BLOCK_NOT_READY;
    uint64_t size = b->partition_sectors * 512;
    return offset > size || n > size - offset ? SWITCH_BLOCK_INVALID : SWITCH_BLOCK_OK;
}

int switch_raw_apfs_read(struct switch_raw_apfs *b, uint64_t offset, void *bytes, uint32_t length)
{
    int status = request(b, offset, bytes, length);
    if (status)
        return status;
    status = b->io.begin(b->io.context, length / 512, 5);
    if (status)
        return fail(b, status);
    return finish(b, b->io.read(b->io.context, b->first_sector + offset / 512, length / 512, bytes));
}

int switch_raw_apfs_write(struct switch_raw_apfs *b, uint64_t offset, const void *bytes, uint32_t length)
{
    int status = request(b, offset, bytes, length);
    if (status)
        return status;
    uint64_t lba = b->first_sector + offset / 512;
    if (!permit(b, lba, length / 512))
        return fail(b, SWITCH_BLOCK_CORRUPT);
    status = b->io.begin(b->io.context, length / 512, 30);
    if (status)
        return fail(b, status);
    return finish(b, b->io.write(b->io.context, lba, length / 512, bytes));
}

int switch_raw_apfs_sync(struct switch_raw_apfs *b)
{
    if (!healthy(b))
        return SWITCH_BLOCK_NOT_READY;
    int status = b->io.begin(b->io.context, 1, 30);
    if (status)
        return fail(b, status);
    return finish(b, b->io.sync(b->io.context));
}

void switch_raw_apfs_close(struct switch_raw_apfs *b)
{
    if (!b || b->magic != RAW_MAGIC)
        return;
    b->online = 0;
    if (b->bound)
        b->io.unbind(b->io.context, b);
    memset(b, 0, sizeof(*b));
}
