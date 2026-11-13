// The bench library's raw_completed.cpp: the runtime's own, compiled with usableStorageMetadata widened so a record
// with no storage form may set SET_PIXEL_SHADER_INTERLOCK_CONTROL (tools/benchloop interlock_ab: hand-written
// interlocked programs on the bench's own raw device), and held to every other storage rule. The widening lives here
// because the raw stack key hashes raw_completed.cpp: the plugin's file, and every record it makes, stay as they are.
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_gpu.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage.h"

namespace raw {
namespace {

bool benchStorageMetadata(const PackPipeline &record, const uint32_t *blob, uint32_t dwords)
{
    if (usableStorageMetadata(record, blob, dwords))
        return true;
#if SWITCH_RAW_DIAGNOSTICS
    std::vector<uint32_t> rest;
    bool interlock = false;
    for (uint32_t k = 0; k + 1 < dwords; k += 2) {
        if (blob[k] == 0x1224) {
            interlock = true;
            continue;
        }
        rest.insert(rest.end(), {blob[k], blob[k + 1]});
    }
    return interlock && !record.storageMask && usableStorageMetadata(record, rest.data(), uint32_t(rest.size()));
#else
    return false;
#endif
}

}  // namespace
}  // namespace raw

#define usableStorageMetadata benchStorageMetadata
#include "drivers/TegraGM20BMetal/raw/runtime/raw_completed.cpp"
#undef usableStorageMetadata
