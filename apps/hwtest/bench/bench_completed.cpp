// The bench library's completed.cpp: the runtime's own, compiled with usableStorageMetadata widened so a record
// with no storage form may set SET_PIXEL_SHADER_INTERLOCK_CONTROL (tools/benchloop interlock_ab: hand-written
// interlocked programs on the bench's own raw device), and held to every other storage rule. The widening lives here
// because the stack key hashes completed.cpp: the plugin's file, and every record it makes, stay as they are.
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20BMetal/gpu.h"
#include "drivers/TegraGM20BMetal/storage.h"

namespace gm20b {
namespace {

bool benchStorageMetadata(const PackPipeline &record, const uint32_t *blob, uint32_t dwords, const uint32_t *far)
{
    if (usableStorageMetadata(record, blob, dwords, far))
        return true;
#if SWITCH_METAL_DIAGNOSTICS
    std::vector<uint32_t> rest;
    bool interlock = false;
    for (uint32_t k = 0; k + 1 < dwords; k += 2) {
        if (blob[k] == 0x1224) {
            interlock = true;
            continue;
        }
        rest.insert(rest.end(), {blob[k], blob[k + 1]});
    }
    return interlock && !record.storageMask && usableStorageMetadata(record, rest.data(), uint32_t(rest.size()), far);
#else
    return false;
#endif
}

}  // namespace
}  // namespace gm20b

#define usableStorageMetadata benchStorageMetadata
#include "drivers/TegraGM20BMetal/completed.cpp"
#undef usableStorageMetadata
