// What mock_iokit.c's GM20B holds for the process, split as iOS charges a BO: a cached BO's task mapping counts in the
// task's phys_footprint as iokit_mapped (an internal VM object mapped with IOKit's accounting); a coherent BO's mapping
// is a physical range from the shared pool and counts in no ledger.
#ifndef SWITCH_MOCK_IOKIT_H
#define SWITCH_MOCK_IOKIT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mock_memory_kind {
    uint64_t cached, cached_peak;
    uint64_t coherent, coherent_peak;
};

struct mock_memory {
    struct mock_memory_kind live;    // BOs allocated and not freed
    struct mock_memory_kind mapped;  // their task mappings: `mapped.cached` is the iokit_mapped charge
    uint64_t allocs[2];              // BoAllocs: cached, coherent
    uint64_t bind_refusals;          // VmBind ops the kernel's checks refuse
};

// The last ExecSurfaces record (method 17) and how many came
struct mock_exec_surfaces {
    uint64_t lists;
    uint32_t count, flags;

    struct {
        uint64_t handle;
        uint32_t access;
    } uses[64];
};

void mock_iokit_memory(struct mock_memory *out);
void mock_iokit_exec_surfaces(struct mock_exec_surfaces *out);
// The live BoImport of the pages at `address`, 0 if none
uint64_t mock_iokit_import_handle(const void *address);
void mock_iokit_report(void);

#ifdef __cplusplus
}
#endif
#endif
