// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// XNU reports the memory a task owns through ledgers (tagged or non-volatile purgeable objects, mapped or not) as
// one region after the last map entry, with user tag -1. Every other user tag is below 256.
#define SWITCH_VM_TAG_OWNED 256u
#define SWITCH_VM_TAG_SLOTS 257u

struct switch_vm_region {
    uint32_t tag;
    uint32_t pages_resident;
    uint32_t pages_dirtied;
    uint32_t pages_swapped_out;
    uint64_t size;
};

struct switch_vm_tag_row {
    uint32_t tag;
    uint32_t regions;
    uint64_t dirty_bytes;
    uint64_t swapped_bytes;
    uint64_t resident_bytes;
    uint64_t virtual_bytes;
};

struct switch_vm_snapshot {
    const char *name;
    int pid;
    double uptime_s;
    uint64_t footprint_bytes;
    uint64_t peak_bytes;
};

struct switch_vm_tags {
    struct switch_vm_tag_row rows[SWITCH_VM_TAG_SLOTS];
    size_t kept;
    struct switch_vm_tag_row total;
};

bool switch_memory_footprint_mode(uint64_t *page_size);
bool switch_memory_walk(int pid, uint64_t page_size, struct switch_vm_tags *tags, struct switch_vm_snapshot *snap);
int switch_memory_report(const char *name, int pid, unsigned count, unsigned long interval_ms, size_t limit);

static inline const char *switch_vm_tag_name(uint32_t tag)
{
    static const struct {
        uint32_t tag;
        const char *name;
    } names[] = {
        {0, "untagged"},
        {1, "MALLOC"},
        {2, "MALLOC_SMALL"},
        {3, "MALLOC_LARGE"},
        {4, "MALLOC_HUGE"},
        {5, "SBRK"},
        {6, "REALLOC"},
        {7, "MALLOC_TINY"},
        {8, "MALLOC_LARGE_REUSABLE"},
        {9, "MALLOC_LARGE_REUSED"},
        {10, "ANALYSIS_TOOL"},
        {11, "MALLOC_NANO"},
        {12, "MALLOC_MEDIUM"},
        {13, "MALLOC_PROB_GUARD"},
        {20, "MACH_MSG"},
        {21, "IOKIT"},
        {22, "VM_RECLAIM"},
        {30, "STACK"},
        {31, "GUARD"},
        {32, "SHARED_PMAP"},
        {33, "DYLIB"},
        {34, "OBJC_DISPATCHERS"},
        {35, "UNSHARED_PMAP"},
        {36, "LIBCHANNEL"},
        {40, "APPKIT"},
        {41, "FOUNDATION"},
        {42, "COREGRAPHICS"},
        {43, "CORESERVICES"},
        {45, "COREDATA"},
        {46, "COREDATA_OBJECTIDS"},
        {50, "ATS"},
        {51, "LAYERKIT"},
        {52, "CGIMAGE"},
        {53, "TCMALLOC"},
        {54, "COREGRAPHICS_DATA"},
        {55, "COREGRAPHICS_SHARED"},
        {56, "COREGRAPHICS_FRAMEBUFFERS"},
        {57, "COREGRAPHICS_BACKINGSTORES"},
        {58, "COREGRAPHICS_XALLOC"},
        {60, "DYLD"},
        {61, "DYLD_MALLOC"},
        {62, "SQLITE"},
        {63, "JAVASCRIPT_CORE"},
        {64, "JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR"},
        {65, "JAVASCRIPT_JIT_REGISTER_FILE"},
        {66, "GLSL"},
        {67, "OPENCL"},
        {68, "COREIMAGE"},
        {69, "WEBCORE_PURGEABLE_BUFFERS"},
        {70, "IMAGEIO"},
        {71, "COREPROFILE"},
        {72, "ASSETSD"},
        {73, "OS_ALLOC_ONCE"},
        {74, "LIBDISPATCH"},
        {75, "ACCELERATE"},
        {76, "COREUI"},
        {77, "COREUIFILE"},
        {78, "GENEALOGY"},
        {79, "RAWCAMERA"},
        {80, "CORPSEINFO"},
        {81, "ASL"},
        {82, "SWIFT_RUNTIME"},
        {83, "SWIFT_METADATA"},
        {84, "DHMM"},
        {85, "DFR"},
        {86, "SCENEKIT"},
        {87, "SKYWALK"},
        {88, "IOSURFACE"},
        {89, "LIBNETWORK"},
        {90, "AUDIO"},
        {91, "VIDEOBITSTREAM"},
        {92, "CM_XPC"},
        {93, "CM_RPC"},
        {94, "CM_MEMORYPOOL"},
        {95, "CM_READCACHE"},
        {96, "CM_CRABS"},
        {97, "QUICKLOOK_THUMBNAILS"},
        {98, "ACCOUNTS"},
        {99, "SANITIZER"},
        {100, "IOACCELERATOR"},
        {101, "CM_REGWARP"},
        {102, "EAR_DECODER"},
        {103, "COREUI_CACHED_IMAGE_DATA"},
        {104, "COLORSYNC"},
        {105, "BTINFO"},
        {106, "CM_HLS"},
        {107, "COMPOSITOR_SERVICES"},
        {SWITCH_VM_TAG_OWNED, "owned (ledger)"},
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (names[i].tag == tag)
            return names[i].name;
    }
    if (tag >= 240 && tag <= 255)
        return "APPLICATION_SPECIFIC";
    return "-";
}

static inline int switch_vm_tag_row_cmp(const void *lhs, const void *rhs)
{
    const struct switch_vm_tag_row *a = (const struct switch_vm_tag_row *)lhs;
    const struct switch_vm_tag_row *b = (const struct switch_vm_tag_row *)rhs;
    const uint64_t fa = a->dirty_bytes + a->swapped_bytes;
    const uint64_t fb = b->dirty_bytes + b->swapped_bytes;
    if (fa != fb)
        return fa > fb ? -1 : 1;
    if (a->resident_bytes != b->resident_bytes)
        return a->resident_bytes > b->resident_bytes ? -1 : 1;
    if (a->tag != b->tag)
        return a->tag < b->tag ? -1 : 1;
    return 0;
}

// Sums regions walked in footprint mode by tag into `rows` (SWITCH_VM_TAG_SLOTS entries), drops tags with nothing
// resident, dirty or compressed, and sorts the rest by footprint. Returns the number of rows kept.
static inline size_t switch_vm_tags_sum(const struct switch_vm_region *regions, size_t count, uint64_t page_size,
                                        struct switch_vm_tag_row *rows, struct switch_vm_tag_row *total)
{
    memset(rows, 0, SWITCH_VM_TAG_SLOTS * sizeof(*rows));
    memset(total, 0, sizeof(*total));
    for (uint32_t i = 0; i < SWITCH_VM_TAG_SLOTS; ++i)
        rows[i].tag = i;
    for (size_t i = 0; i < count; ++i) {
        const struct switch_vm_region *r = &regions[i];
        struct switch_vm_tag_row *row = &rows[r->tag < 256 ? r->tag : SWITCH_VM_TAG_OWNED];
        row->regions++;
        row->dirty_bytes += (uint64_t)r->pages_dirtied * page_size;
        row->swapped_bytes += (uint64_t)r->pages_swapped_out * page_size;
        row->resident_bytes += (uint64_t)r->pages_resident * page_size;
        row->virtual_bytes += r->size;
    }
    size_t kept = 0;
    for (uint32_t i = 0; i < SWITCH_VM_TAG_SLOTS; ++i) {
        const struct switch_vm_tag_row row = rows[i];
        total->regions += row.regions;
        total->dirty_bytes += row.dirty_bytes;
        total->swapped_bytes += row.swapped_bytes;
        total->resident_bytes += row.resident_bytes;
        total->virtual_bytes += row.virtual_bytes;
        if (row.dirty_bytes || row.swapped_bytes || row.resident_bytes)
            rows[kept++] = row;
    }
    if (kept > 1)
        qsort(rows, kept, sizeof(*rows), switch_vm_tag_row_cmp);
    return kept;
}

// Page tables are charged to phys_footprint but belong to no region, so the walk comes out short by about their size.
static inline void switch_vm_tags_format(FILE *stream, const struct switch_vm_snapshot *snap,
                                         const struct switch_vm_tag_row *rows, size_t count,
                                         const struct switch_vm_tag_row *total, size_t limit)
{
    const size_t shown = (limit && limit < count) ? limit : count;
    const uint64_t walked = total->dirty_bytes + total->swapped_bytes;
    fprintf(stream,
            "t=%.1fs %s [%d] footprint %llu KB, lifetime peak %llu KB; regions dirty %llu KB + compressed %llu KB = "
            "%llu KB; not in a region %lld KB\n",
            snap->uptime_s, snap->name, snap->pid, (unsigned long long)(snap->footprint_bytes / 1024),
            (unsigned long long)(snap->peak_bytes / 1024), (unsigned long long)(total->dirty_bytes / 1024),
            (unsigned long long)(total->swapped_bytes / 1024), (unsigned long long)(walked / 1024),
            ((long long)snap->footprint_bytes - (long long)walked) / 1024);
    fprintf(stream, "%-4s %-36s %9s %9s %9s %10s %7s\n", "TAG", "NAME", "DIRTY_KB", "COMPR_KB", "RESID_KB", "VIRT_KB",
            "REGIONS");
    for (size_t i = 0; i < shown; ++i) {
        const struct switch_vm_tag_row *r = &rows[i];
        char tag[8] = "-1";
        if (r->tag != SWITCH_VM_TAG_OWNED)
            snprintf(tag, sizeof(tag), "%u", r->tag);
        fprintf(stream, "%-4s %-36s %9llu %9llu %9llu %10llu %7u\n", tag, switch_vm_tag_name(r->tag),
                (unsigned long long)(r->dirty_bytes / 1024), (unsigned long long)(r->swapped_bytes / 1024),
                (unsigned long long)(r->resident_bytes / 1024), (unsigned long long)(r->virtual_bytes / 1024),
                r->regions);
    }
}

#ifdef __cplusplus
}
#endif
