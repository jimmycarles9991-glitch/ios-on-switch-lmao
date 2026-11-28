/* system.shared_cache_nested (switch_shared_cache_probe.c): the dyld shared cache's read-only data as this process's
 * map holds it. The headers come from the mapped cache itself (its base from TASK_DYLD_INFO). The mappings checked
 * are every subcache's DYLD_CACHE_MAPPING_CONST_DATA ones outside __TPRO_CONST: the ones dyld remaps VM_PROT_COPY in
 * every process when it has to store into the cache without TPRO pagers. Nested means each top-level map entry over
 * the range is the shared region's submap with its pmap still nested (share mode SM_TRUESHARED). */
#ifndef SWITCH_SHARED_CACHE_PROBE_H
#define SWITCH_SHARED_CACHE_PROBE_H

#include <stdint.h>

enum switch_shared_cache_probe_error {
    SWITCH_SHARED_CACHE_PROBE_OK,
    SWITCH_SHARED_CACHE_PROBE_DYLD_INFO, /* task_info(TASK_DYLD_INFO) failed */
    SWITCH_SHARED_CACHE_PROBE_NO_CACHE,  /* dyld_all_image_infos names no shared cache */
    SWITCH_SHARED_CACHE_PROBE_HEADER,    /* a cache header without dyld's magic */
};

struct switch_shared_cache_probe {
    int32_t error;    /* enum switch_shared_cache_probe_error */
    uint8_t uuid[16]; /* the main cache file's */
    uint64_t slide;
    uint32_t files;    /* the main file and the subcaches read */
    uint32_t mappings; /* read-only data mappings found */
    uint32_t nested;   /* of those, wholly nested */
    uint64_t bytes, nested_bytes;
    uint64_t first_unnested; /* unslid address of the first mapping that is not, 0 for none */
    int32_t first_mode;      /* there: the entry's share mode, or -1 where no submap entry covers it */
};

void switch_shared_cache_probe(struct switch_shared_cache_probe *out);

#endif
