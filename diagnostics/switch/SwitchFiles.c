/* Bounded FAT/split-file discovery after same-boot card and GPT validation.
 * Required file paths/geometry are immutable build inputs, not card identity. */
#include "SwitchFiles.h"
#include "SwitchMediaExpectations.h"
#include "storage/fat32.h"
#include "storage/split_disk.h"
#include "storage/partition.h"
#include "storage/raw_apfs.h"
#include "storage/block_backend_source.h"
#include "storage/data_backend_source.h"
#include "SwitchWritableData.h"
#include <libkern/crypto/sha2.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

enum {
    FILES_BUFFER = 131072,
    FILES_MAX_SECTORS = 131072,
    FILES_MAX_SECONDS = 120
};

volatile uint64_t switch_files_record[40];
uint8_t switch_files_digest[32], switch_files_expected_digest[32];
static uint8_t file_buffer[FILES_BUFFER];
static struct switch_fat_volume volume;
static struct switch_fat_file fixture;
static struct switch_split_disk disks[SWITCH_STORAGE_VOLUME_COUNT];
static struct switch_split_part parts[SWITCH_STORAGE_VOLUME_COUNT][SWITCH_SPLIT_MAX_PARTS];
static unsigned attempted;
static struct switch_gpt_workspace gpt_workspace;
static struct switch_raw_apfs_partition raw_partitions[SWITCH_GPT_MAX_ENTRIES];
static uint32_t raw_partition_count;
static const int storage_profile = SWITCH_STORAGE_RAW;

struct reader {
    struct switch_sd_host *host;
    uint64_t start, request_start;
    uint32_t request_sectors, sector_budget, seconds_budget;
    unsigned active, qualified, offline;
    int request_error;
};
static struct reader reader;

static int equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void update(struct reader *r)
{
    switch_files_record[25] = r->host->io.ticks(r->host->context) - r->start;
    switch_files_record[15] = r->host->blocked;
    if (!r->qualified)
        switch_diag_files_update();
}

static int fail(struct reader *r, unsigned code, unsigned fat)
{
    if (!switch_files_record[3])
        switch_files_record[3] = code;
    switch_files_record[4] = fat;
    update(r);
    return 0;
}

static int read_sector(void *context, uint64_t lba, uint8_t out[512])
{
    struct reader *r = context;
    struct switch_sd_host *h = r->host;
    switch_files_record[11] = lba;
    if (!r->active || r->offline || r->request_error)
        return -1;
    uint64_t elapsed = h->io.ticks(h->context) - r->request_start;
    if (r->request_sectors >= r->sector_budget || elapsed >= h->frequency * r->seconds_budget) {
        r->request_error = SWITCH_BLOCK_TIMEOUT;
        r->offline = 1;
        if (!r->qualified)
            switch_files_record[3] = 202;
        return -1;
    }
    struct switch_sd_result result = {0};
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    int error = switch_sd_read_sector(h, lba, out, &result);
    (void)ml_set_interrupts_enabled(previous);
    switch_files_record[10]++;
    r->request_sectors++;
    if (error) {
        r->request_error = error == SWITCH_SD_TIMEOUT ? SWITCH_BLOCK_TIMEOUT : SWITCH_BLOCK_IO;
        r->offline = 1;
    }
    switch_files_record[5] = (unsigned)error;
    switch_files_record[12] = result.status;
    switch_files_record[13] = result.stage;
    switch_files_record[14] = result.readiness_failures;
    switch_files_record[15] = h->blocked;
    if (!(switch_files_record[10] & 511u))
        update(r);
    return error;
}
#if !SWITCH_SD_MODEL
/* The mounted FAT reader and both block devices share one serialized owner.
 * SDHCI signals are masked by the transport; CPU interrupts can remain in
 * their caller's state while a batch is polled. Output is backend-private
 * scratch and is never copied to the I/O client after a partial/error read. */
static int read_sectors(void *context, uint64_t lba, uint32_t count, uint8_t *out)
{
    if (count == 1)
        return read_sector(context, lba, out);
    struct reader *r = context;
    struct switch_sd_host *h = r->host;
    if (!count || count > SWITCH_SD_MAX_READ_SECTORS || !out)
        return -1;
    switch_files_record[11] = lba;
    if (!r->active || r->offline || r->request_error)
        return -1;
    uint64_t elapsed = h->io.ticks(h->context) - r->request_start;
    if (r->request_sectors > r->sector_budget || count > r->sector_budget - r->request_sectors ||
        elapsed >= h->frequency * r->seconds_budget) {
        r->request_error = SWITCH_BLOCK_TIMEOUT;
        r->offline = 1;
        return -1;
    }
    struct switch_sd_result result = {0};
    int error = switch_sd_read_sectors(h, lba, count, out, &result);
    uint64_t prior_sectors = switch_files_record[10];
    switch_files_record[10] += count;
    r->request_sectors += count;
    if (error) {
        r->request_error = error == SWITCH_SD_TIMEOUT ? SWITCH_BLOCK_TIMEOUT : SWITCH_BLOCK_IO;
        r->offline = 1;
    }
    switch_files_record[5] = (unsigned)error;
    switch_files_record[12] = result.status;
    switch_files_record[13] = result.stage;
    switch_files_record[14] = result.readiness_failures;
    switch_files_record[15] = h->blocked;
    if ((prior_sectors >> 9) != (switch_files_record[10] >> 9))
        update(r);
    return error;
}
#endif
static void digest(const void *bytes, unsigned size, uint8_t out[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, bytes, size);
    SHA256_Final(out, &ctx);
}

static int sha_known_answers(void)
{
    static const uint8_t empty[32] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                      0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                      0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                    0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                    0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t out[32];
    digest("", 0, out);
    if (!equal(out, empty, 32))
        return 0;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, "a", 1);
    SHA256_Update(&ctx, "bc", 2);
    SHA256_Final(out, &ctx);
    return equal(out, abc, 32);
}

static int expected_manifest_valid(void)
{
    if (!SWITCH_STORAGE_VOLUME_COUNT || SWITCH_STORAGE_VOLUME_COUNT > 4 || !SWITCH_STORAGE_RANGE_COUNT ||
        SWITCH_STORAGE_RANGE_COUNT > 16 || !SWITCH_STORAGE_FIXTURE_SIZE || SWITCH_STORAGE_FIXTURE_SIZE > 1048576)
        return 0;
    for (unsigned v = 0; v < SWITCH_STORAGE_VOLUME_COUNT; v++) {
        const struct switch_expected_volume *e = &switch_storage_volumes[v];
        if (!e->part_count || e->part_count > SWITCH_SPLIT_MAX_PARTS || !e->size_bytes || (e->size_bytes & 511u))
            return 0;
        uint64_t size = 0;
        for (unsigned p = 0; p < e->part_count; p++) {
            uint64_t n = e->parts[p].size_bytes;
            if (!e->parts[p].path || !n || n > UINT32_MAX || (n & 511u) || size > e->size_bytes ||
                n > e->size_bytes - size)
                return 0;
            size += n;
        }
        if (size != e->size_bytes)
            return 0;
    }
    for (unsigned i = 0; i < SWITCH_STORAGE_RANGE_COUNT; i++) {
        const struct switch_expected_range *e = &switch_storage_ranges[i];
        if (e->volume_index >= SWITCH_STORAGE_VOLUME_COUNT || !e->length || e->length > FILES_BUFFER)
            return 0;
        uint64_t size = switch_storage_volumes[e->volume_index].size_bytes;
        if (e->offset > size || e->length > size - e->offset)
            return 0;
    }
    return 1;
}

/* Discover using directory entries only. Final file-chain validation and Data
 * versus OS/ancestor disjointness remain mandatory in the block backends.
 * All candidates share the reader's fixed sector/time budget; an unreadable or
 * exhausted candidate aborts selection, so it cannot hide an ambiguous copy. */
struct partition_probe {
    struct reader *reader;
    unsigned root_boot;
};

static int required_file(struct switch_fat_volume *candidate, const char *path, uint64_t size)
{
    uint32_t actual = 0;
    int error = switch_fat_lookup_size(candidate, path, &actual);
    if (error == SWITCH_FAT_IO || error == SWITCH_FAT_LIMIT)
        return -1;
    return !error && actual == size;
}

static int partition_matches(void *context, const struct switch_gpt_slice *slice)
{
    struct partition_probe *probe = context;
    struct reader *r = probe->reader;
    struct switch_fat_volume candidate = {0};
    int error = switch_fat_mount_slice(&candidate, read_sector, r, r->host->capacity_sectors, slice->first_sector,
                                       slice->sector_count);
    if (error == SWITCH_FAT_IO || error == SWITCH_FAT_LIMIT)
        return -1;
    if (error)
        return 0;
    for (unsigned v = 0; v < SWITCH_STORAGE_VOLUME_COUNT; v++) {
        const struct switch_expected_volume *e = &switch_storage_volumes[v];
        for (unsigned p = 0; p < e->part_count; p++) {
            int match = required_file(&candidate, e->parts[p].path, e->parts[p].size_bytes);
            if (match != 1)
                return match;
        }
    }
    if (probe->root_boot) {
        for (unsigned p = 0; p < SWITCH_WRITABLE_DATA_PART_COUNT; p++) {
            int match =
                required_file(&candidate, switch_writable_data_parts[p].path, switch_writable_data_parts[p].size);
            if (match != 1)
                return match;
        }
    }
    return 1;
}

void switch_files_probe(struct switch_sd_host *host)
{
    if (attempted)
        return;
    attempted = 1;
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
    const int root_boot = 1;
#else
    const int root_boot = 0;
#endif
    switch_files_record[0] = SWITCH_FILES_MAGIC;
    switch_files_record[1] = 1;
    switch_files_record[2] = 1;
    switch_files_record[20] = UINT64_MAX;
    switch_files_record[21] = UINT64_MAX;
    if (storage_profile == SWITCH_STORAGE_INVALID || (storage_profile == SWITCH_STORAGE_RAW && root_boot != 1) ||
        !host || host->blocked || host->block_addressing_verified != 1 || !host->io.ticks || host->frequency < 1000 ||
        host->frequency > 1000000000 || !host->capacity_sectors || host->capacity_sectors > UINT64_C(0x100000000) ||
        host->permitted_first_sector || host->permitted_sector_count != host->capacity_sectors) {
        switch_files_record[3] = 200;
        switch_diag_files_update();
        return;
    }
    reader = (struct reader){.host = host,
                             .start = host->io.ticks(host->context),
                             .sector_budget = FILES_MAX_SECTORS,
                             .seconds_budget = FILES_MAX_SECONDS,
                             .active = 1};
    reader.request_start = reader.start;
    switch_files_record[6] = host->capacity_sectors;
    switch_files_record[26] = host->frequency;
    switch_files_record[27] = FILES_MAX_SECTORS;
    switch_files_record[28] = FILES_MAX_SECONDS;
    switch_files_record[30] = root_boot == 1 ? 0 : SWITCH_STORAGE_RANGE_COUNT;
    switch_files_record[31] = SWITCH_STORAGE_VOLUME_COUNT;
    if (storage_profile == SWITCH_STORAGE_RAW) {
        /* Same strict GPT parser as FAT; no FAT access or file indexing. */
        int error = switch_gpt_scan(read_sector, &reader, host->capacity_sectors, &gpt_workspace, &raw_partition_count);
        switch_files_record[32] = (unsigned)error;
        switch_files_record[35] = gpt_workspace.headers[0].entry_count;
        switch_files_record[36] = gpt_workspace.headers[0].entry_size;
        if (error) {
            fail(&reader, 212, 0);
            return;
        }
        for (uint32_t i = 0; i < raw_partition_count; i++) {
            const struct switch_gpt_extent *e = &gpt_workspace.used[i];
            raw_partitions[i].first_sector = e->first;
            raw_partitions[i].sector_count = e->last - e->first + 1;
            for (unsigned j = 0; j < 16; j++)
                raw_partitions[i].type_guid[j] = e->type_guid[j];
        }
        switch_files_record[31] = 1;
        switch_files_record[2] = 9;
        reader.active = 0;
        reader.qualified = 1;
        update(&reader);
        return;
    }
    if (!expected_manifest_valid()) {
        fail(&reader, 201, 0);
        return;
    }
    switch_files_record[2] = 2;
    update(&reader);
    if (root_boot != 1) {
        if (!sha_known_answers()) {
            fail(&reader, 203, 0);
            return;
        }
        switch_files_record[29] = 1;
    }
    switch_files_record[2] = 3;
    update(&reader);
    struct switch_gpt_slice slice;
    struct partition_probe probe = {.reader = &reader, .root_boot = root_boot == 1};
    int error = switch_partition_find(read_sector, &reader, host->capacity_sectors, partition_matches, &probe,
                                      &gpt_workspace, &slice);
    switch_files_record[32] = (unsigned)error;
    switch_files_record[35] = gpt_workspace.headers[0].entry_count;
    switch_files_record[36] = gpt_workspace.headers[0].entry_size;
    if (error) {
        fail(&reader, 212, 0);
        return;
    }
    uint64_t first = slice.first_sector;
    uint64_t count = slice.sector_count;
    switch_files_record[7] = first;
    switch_files_record[8] = count;
    switch_files_record[33] = slice.partition_index;
    host->permitted_first_sector = first;
    host->permitted_sector_count = count;
    switch_files_record[2] = 4;
    update(&reader);
    error = switch_fat_mount_slice(&volume, read_sector, &reader, host->capacity_sectors, first, count);
    if (error) {
        fail(&reader, 204, (unsigned)error);
        return;
    }
#if !SWITCH_SD_MODEL
    if (root_boot == 1)
        volume.read_sectors = read_sectors;
#endif
    switch_files_record[9] = (uint64_t)volume.sectors_per_cluster * 512;
    /* BPB may name a shorter usable extent than the GPT allocation. */
    host->permitted_sector_count = volume.total_sectors;
    switch_files_record[34] = volume.total_sectors;
    switch_files_record[2] = 5;
    update(&reader);
    if (root_boot == 1) {
        /* Root startup needs the mounted FAT reader and part descriptors.
         * The real block backend opens and indexes the files once. Historical
         * SHA/fixture/sample qualification is not part of normal startup. */
        for (unsigned v = 0; v < SWITCH_STORAGE_VOLUME_COUNT; v++) {
            const struct switch_expected_volume *e = &switch_storage_volumes[v];
            uint64_t offset = 0;
            for (unsigned p = 0; p < e->part_count; p++) {
                parts[v][p] = (struct switch_split_part){e->parts[p].path, offset, (uint32_t)e->parts[p].size_bytes};
                offset += e->parts[p].size_bytes;
            }
        }
        /* Stage 9 is the source-ready contract. Leave test-result counters at
         * zero and suppress the historical "samples verified" HUD update. */
        switch_files_record[2] = 9;
        reader.active = 0;
        reader.qualified = 1;
        update(&reader);
        return;
    }
    error = switch_fat_open(&volume, SWITCH_STORAGE_FIXTURE_PATH, &fixture);
    if (error) {
        fail(&reader, 205, (unsigned)error);
        return;
    }
    if (fixture.size != SWITCH_STORAGE_FIXTURE_SIZE) {
        fail(&reader, 206, 0);
        return;
    }
    switch_files_record[2] = 6;
    update(&reader);
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (uint32_t offset = 0; offset < SWITCH_STORAGE_FIXTURE_SIZE;) {
        uint32_t n = SWITCH_STORAGE_FIXTURE_SIZE - offset;
        if (n > FILES_BUFFER)
            n = FILES_BUFFER;
        error = switch_fat_read(&volume, &fixture, offset, file_buffer, n);
        if (error) {
            fail(&reader, 207, (unsigned)error);
            return;
        }
        SHA256_Update(&hash, file_buffer, n);
        offset += n;
        switch_files_record[16] = offset;
        update(&reader);
    }
    SHA256_Final(switch_files_digest, &hash);
    for (unsigned i = 0; i < 32; i++)
        switch_files_expected_digest[i] = switch_storage_fixture_sha256[i];
    if (!equal(switch_files_digest, switch_files_expected_digest, 32)) {
        fail(&reader, 208, 0);
        return;
    }
    switch_files_record[17] = 1;
    switch_files_record[24] = SWITCH_STORAGE_FIXTURE_SIZE;
    switch_files_record[2] = 7;
    update(&reader);
    for (unsigned v = 0; v < SWITCH_STORAGE_VOLUME_COUNT; v++) {
        const struct switch_expected_volume *e = &switch_storage_volumes[v];
        uint64_t offset = 0;
        switch_files_record[21] = v;
        update(&reader);
        for (unsigned p = 0; p < e->part_count; p++) {
            parts[v][p] = (struct switch_split_part){e->parts[p].path, offset, (uint32_t)e->parts[p].size_bytes};
            offset += e->parts[p].size_bytes;
        }
        error = switch_split_open(&disks[v], &volume, parts[v], e->part_count, e->size_bytes);
        if (error) {
            fail(&reader, 209, (unsigned)error);
            return;
        }
        switch_files_record[18] = v + 1;
        update(&reader);
    }
    switch_files_record[2] = 8;
    update(&reader);
    for (unsigned i = 0; i < SWITCH_STORAGE_RANGE_COUNT; i++) {
        const struct switch_expected_range *e = &switch_storage_ranges[i];
        switch_files_record[20] = i;
        switch_files_record[21] = e->volume_index;
        switch_files_record[22] = e->offset;
        switch_files_record[23] = e->length;
        update(&reader);
        error = switch_split_read(&disks[e->volume_index], e->offset, file_buffer, e->length);
        if (error) {
            fail(&reader, 210, (unsigned)error);
            return;
        }
        digest(file_buffer, e->length, switch_files_digest);
        for (unsigned j = 0; j < 32; j++)
            switch_files_expected_digest[j] = e->sha256[j];
        if (!equal(switch_files_digest, switch_files_expected_digest, 32)) {
            fail(&reader, 211, 0);
            return;
        }
        switch_files_record[19] = i + 1;
        switch_files_record[24] += e->length;
        update(&reader);
    }
    switch_files_record[2] = 9;
    update(&reader);
    reader.active = 0;
    reader.qualified = 1;
}

static int source_ready(void *context)
{
    struct reader *r = context;
    if (r != &reader || !r->qualified || r->offline || !r->host || r->host->blocked || switch_files_record[2] != 9 ||
        switch_files_record[3] || (storage_profile != SWITCH_STORAGE_RAW && !volume.mounted))
        return SWITCH_BLOCK_NOT_READY;
    return SWITCH_BLOCK_OK;
}

static int source_begin(void *context, uint32_t sectors, uint32_t seconds)
{
    struct reader *r = context;
    if (source_ready(context) != SWITCH_BLOCK_OK)
        return SWITCH_BLOCK_NOT_READY;
    if (r->active || !sectors || sectors > FILES_MAX_SECTORS || !seconds || seconds > FILES_MAX_SECONDS)
        return SWITCH_BLOCK_INVALID;
    r->request_start = r->host->io.ticks(r->host->context);
    r->request_sectors = 0;
    r->sector_budget = sectors;
    r->seconds_budget = seconds;
    r->request_error = SWITCH_BLOCK_OK;
    r->active = 1;
    return SWITCH_BLOCK_OK;
}

static int source_end(void *context)
{
    struct reader *r = context;
    if (r != &reader || !r->active)
        return SWITCH_BLOCK_INVALID;
    if (!r->request_error &&
        r->host->io.ticks(r->host->context) - r->request_start >= r->host->frequency * r->seconds_budget)
        r->request_error = SWITCH_BLOCK_TIMEOUT;
    if (!r->request_error && r->host->blocked)
        r->request_error = SWITCH_BLOCK_IO;
    r->active = 0;
    if (r->request_error)
        r->offline = 1;
    return r->request_error;
}

int switch_files_block_source(struct switch_block_source *source)
{
    if (!source)
        return SWITCH_BLOCK_INVALID;
    *source = (struct switch_block_source){0};
    if (storage_profile != SWITCH_STORAGE_FAT)
        return SWITCH_BLOCK_INVALID;
    if (source_ready(&reader) != SWITCH_BLOCK_OK)
        return SWITCH_BLOCK_NOT_READY;
    if (SWITCH_STORAGE_VOLUME_COUNT != SWITCH_BLOCK_DISK_COUNT)
        return SWITCH_BLOCK_CORRUPT;
    source->volume = &volume;
    source->context = &reader;
    source->ready = source_ready;
    source->begin = source_begin;
    source->end = source_end;
    static const char *const labels[SWITCH_BLOCK_DISK_COUNT] = {"OS", "SystemOS"};
    for (unsigned v = 0; v < SWITCH_BLOCK_DISK_COUNT; v++) {
        source->disks[v] = (struct switch_block_source_disk){.size_bytes = switch_storage_volumes[v].size_bytes,
                                                             .part_count = switch_storage_volumes[v].part_count,
                                                             .parts = parts[v],
                                                             .label = labels[v]};
    }
    return SWITCH_BLOCK_OK;
}

#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
#include "SwitchRootPolicyLoad.h"
#endif

/* Persistent Data uses the same reader, card and failure latch as OS/SystemOS. */
static volatile uint64_t switch_data_record[16];

static int data_begin_metadata(void *context, uint32_t sectors, uint32_t seconds)
{
    return source_begin(context, sectors, seconds);
}

static void data_offline(void *context)
{
    struct reader *r = context;
    if (r != &reader)
        return;
    r->offline = 1;
    if (r->host)
        r->host->blocked = 1;
}

static int data_result(struct reader *r, int error, const struct switch_sd_result *result)
{
    switch_data_record[1] = (unsigned)error;
    switch_data_record[2] = result->stage;
    switch_data_record[3] = result->status;
    switch_data_record[4] = result->card_status;
    switch_data_record[5] = result->cache_evidence;
    switch_data_record[9] = result->scr_high;
    switch_data_record[10] = result->scr_low;
    if (error) {
        r->request_error = error == SWITCH_SD_TIMEOUT ? SWITCH_BLOCK_TIMEOUT : SWITCH_BLOCK_IO;
        data_offline(r);
    }
    return error ? r->request_error : SWITCH_BLOCK_OK;
}

static int data_qualify(void *context, int (*permit)(void *, uint64_t, uint32_t), void *owner)
{
    struct reader *r = context;
    if (r != &reader || !permit || !owner || !r->host || r->host->permit_write)
        return SWITCH_BLOCK_INVALID;
    int status = source_begin(context, 16, 30);
    if (status)
        return status;
    r->host->permit_write = permit;
    r->host->write_context = owner;
    struct switch_sd_result result = {0};
    int error = switch_sd_qualify_writes(r->host, &result);
    status = data_result(r, error, &result);
    int end = source_end(context);
    if (end)
        status = end;
    switch_data_record[0] = status ? 0 : 1;
    return status;
}

static void data_unbind(void *context, void *owner)
{
    struct reader *r = context;
    if (r == &reader && r->host && r->host->write_context == owner) {
        r->host->permit_write = 0;
        r->host->write_context = 0;
        r->host->write_cache_off_verified = 0;
    }
}

static int data_write(void *context, uint64_t lba, uint32_t count, const uint8_t *bytes)
{
    struct reader *r = context;
    if (r != &reader || !r->active || r->offline || r->request_error)
        return SWITCH_BLOCK_NOT_READY;
    if (!count || count > 128 || !bytes)
        return SWITCH_BLOCK_INVALID;
    if (r->request_sectors > r->sector_budget || count > r->sector_budget - r->request_sectors ||
        r->host->io.ticks(r->host->context) - r->request_start >= r->host->frequency * r->seconds_budget) {
        r->request_error = SWITCH_BLOCK_TIMEOUT;
        data_offline(r);
        return SWITCH_BLOCK_TIMEOUT;
    }
    struct switch_sd_result result = {0};
    int error = switch_sd_write_sectors(r->host, lba, count, bytes, &result);
    r->request_sectors += count;
    switch_data_record[6] = lba;
    switch_data_record[7] += (uint64_t)result.words_written * 4u;
    return data_result(r, error, &result);
}

static int data_sync(void *context)
{
    struct reader *r = context;
    if (r != &reader || !r->active || r->offline || r->request_error)
        return SWITCH_BLOCK_NOT_READY;
    struct switch_sd_result result = {0};
    int error = switch_sd_sync(r->host, &result);
    if (!error)
        switch_data_record[8]++;
    return data_result(r, error, &result);
}

int switch_files_data_source(struct switch_data_source *source)
{
    if (!source)
        return SWITCH_BLOCK_INVALID;
    *source = (struct switch_data_source){0};
    if (storage_profile != SWITCH_STORAGE_FAT)
        return SWITCH_BLOCK_INVALID;
    if (source_ready(&reader) != SWITCH_BLOCK_OK)
        return SWITCH_BLOCK_NOT_READY;
    *source = (struct switch_data_source){.volume = &volume,
                                          .context = &reader,
                                          .ready = source_ready,
                                          .begin = source_begin,
                                          .begin_metadata = data_begin_metadata,
                                          .end = source_end,
                                          .offline = data_offline,
                                          .qualify = data_qualify,
                                          .unbind = data_unbind,
                                          .write = data_write,
                                          .sync = data_sync};
    return SWITCH_BLOCK_OK;
}

/* Direct transport reuses the same serialized reader, failure latch, cache
 * qualification and exact write permit. No file-layer lock is held around I/O. */
static int raw_read(void *context, uint64_t lba, uint32_t count, uint8_t *bytes)
{
#if !SWITCH_SD_MODEL
    int error = read_sectors(context, lba, count, bytes);
#else
    int error = -1;
    (void)context;
    (void)lba;
    (void)count;
    (void)bytes;
#endif
    if (!error)
        return SWITCH_BLOCK_OK;
    return reader.request_error ? reader.request_error : SWITCH_BLOCK_IO;
}

int switch_files_raw_open(struct switch_raw_apfs *backend, void *scratch, uint32_t bytes)
{
    static const uint8_t container_uuid[16] = {0x50, 0xf8, 0xa6, 0x7f, 0xaf, 0xc1, 0x4a, 0x1b,
                                               0xbc, 0xbb, 0x82, 0x16, 0x26, 0x74, 0xc6, 0x7f};
    if (storage_profile != SWITCH_STORAGE_RAW)
        return SWITCH_BLOCK_INVALID;
    if (source_ready(&reader))
        return SWITCH_BLOCK_NOT_READY;
    struct switch_raw_apfs_io io = {.context = &reader,
                                    .ready = source_ready,
                                    .begin = source_begin,
                                    .end = source_end,
                                    .offline = data_offline,
                                    .read = raw_read,
                                    .qualify = data_qualify,
                                    .unbind = data_unbind,
                                    .write = data_write,
                                    .sync = data_sync};
    int result = switch_raw_apfs_open(backend, &io, reader.host->capacity_sectors, raw_partitions, raw_partition_count,
                                      container_uuid, scratch, bytes);
    if (result) {
        data_offline(&reader);
        return result;
    }
    /* UUID and NXSB already validated, exact write permit bound. Narrow the
     * host's physical read range before publishing the one container child. */
    reader.host->permitted_first_sector = backend->first_sector;
    reader.host->permitted_sector_count = backend->partition_sectors;
    switch_files_record[7] = backend->first_sector;
    switch_files_record[8] = backend->partition_sectors;
    return SWITCH_BLOCK_OK;
}
