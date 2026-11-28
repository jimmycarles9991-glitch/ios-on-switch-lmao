#include "switch_shared_cache_probe.h"

#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <string.h>

/* dyld_cache_header and its tables (dyld's dyld_cache_format.h): the fields read here. */
#define HEADER_MAPPING_OFFSET            0x10
#define HEADER_UUID                      0x58
#define HEADER_MAPPING_WITH_SLIDE_OFFSET 0x138
#define HEADER_MAPPING_WITH_SLIDE_COUNT  0x13c
#define HEADER_SUBCACHE_ARRAY_OFFSET     0x188
#define HEADER_SUBCACHE_ARRAY_COUNT      0x18c
#define HEADER_CACHE_SUBTYPE    0x1c8 /* subcache entries carry a file suffix when the header reaches past it */
#define SUBCACHE_ENTRY_V1       24    /* uuid, cacheVMOffset */
#define SUBCACHE_ENTRY          56    /* uuid, cacheVMOffset, fileSuffix[32] */
#define SUBCACHE_VM_OFFSET      16
#define MAPPING_AND_SLIDE       56 /* address, size, fileOffset, slideInfoFileOffset/Size, flags, maxProt, initProt */
#define MAPPING_FLAGS           40
#define MAPPING_INIT_PROT       52
#define MAPPING_CONST_DATA      0x4
#define MAPPING_CONST_TPRO_DATA 0x40

static uint32_t u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* Whether every top-level entry over [address, address + size) is the shared region's submap, still nested. */
static bool nested(uint64_t address, uint64_t size, int32_t *mode)
{
    for (uint64_t at = address; at < address + size;) {
        vm_address_t region = (vm_address_t)at;
        vm_size_t length = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &region, &length, &depth, (vm_region_recurse_info_t)&info, &count) !=
                KERN_SUCCESS ||
            region > at || !info.is_submap) {
            *mode = -1;
            return false;
        }
        if (info.share_mode != SM_TRUESHARED) {
            *mode = info.share_mode;
            return false;
        }
        at = region + length;
    }
    return true;
}

static bool scan(const uint8_t *header, uint64_t slide, struct switch_shared_cache_probe *out)
{
    if (memcmp(header, "dyld_v1", 7) != 0)
        return false;
    const uint8_t *mapping = header + u32(header + HEADER_MAPPING_WITH_SLIDE_OFFSET);
    for (uint32_t i = 0; i < u32(header + HEADER_MAPPING_WITH_SLIDE_COUNT); ++i, mapping += MAPPING_AND_SLIDE) {
        const uint64_t flags = u64(mapping + MAPPING_FLAGS);
        if (!(flags & MAPPING_CONST_DATA) || (flags & MAPPING_CONST_TPRO_DATA) ||
            (u32(mapping + MAPPING_INIT_PROT) & VM_PROT_WRITE)) {
            continue;
        }
        const uint64_t address = u64(mapping), size = u64(mapping + 8);
        int32_t mode = 0;
        out->mappings++;
        out->bytes += size;
        if (nested(address + slide, size, &mode)) {
            out->nested++;
            out->nested_bytes += size;
        } else if (out->first_unnested == 0) {
            out->first_unnested = address;
            out->first_mode = mode;
        }
    }
    out->files++;
    return true;
}

void switch_shared_cache_probe(struct switch_shared_cache_probe *out)
{
    memset(out, 0, sizeof(*out));
    struct task_dyld_info dyld;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&dyld, &count) != KERN_SUCCESS) {
        out->error = SWITCH_SHARED_CACHE_PROBE_DYLD_INFO;
        return;
    }
    const struct dyld_all_image_infos *infos = (const struct dyld_all_image_infos *)(uintptr_t)dyld.all_image_info_addr;
    if (infos == NULL || infos->sharedCacheBaseAddress == 0) {
        out->error = SWITCH_SHARED_CACHE_PROBE_NO_CACHE;
        return;
    }
    const uint8_t *main = (const uint8_t *)infos->sharedCacheBaseAddress;
    out->slide = infos->sharedCacheSlide;
    memcpy(out->uuid, main + HEADER_UUID, sizeof(out->uuid));
    if (!scan(main, out->slide, out)) {
        out->error = SWITCH_SHARED_CACHE_PROBE_HEADER;
        return;
    }
    const uint32_t entry =
        u32(main + HEADER_MAPPING_OFFSET) > HEADER_CACHE_SUBTYPE ? SUBCACHE_ENTRY : SUBCACHE_ENTRY_V1;
    const uint8_t *subcache = main + u32(main + HEADER_SUBCACHE_ARRAY_OFFSET);
    for (uint32_t i = 0; i < u32(main + HEADER_SUBCACHE_ARRAY_COUNT); ++i, subcache += entry) {
        if (!scan(main + u64(subcache + SUBCACHE_VM_OFFSET), out->slide, out)) {
            out->error = SWITCH_SHARED_CACHE_PROBE_HEADER;
            return;
        }
    }
}
