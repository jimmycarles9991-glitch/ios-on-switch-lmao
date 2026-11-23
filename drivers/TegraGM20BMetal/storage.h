// SPDX-License-Identifier: MIT
#pragma once
#include "pack.h"
#include "set_entries.h"
#include "sample_shading.h"

namespace gm20b {

inline bool orderedStorageControl(uint32_t control)
{
    return control == 1 || control == 2;
}

// Whether a fragment program's SET_PIXEL_SHADER_INTERLOCK_CONTROL keeps the ordering its state needs: the ordered interlock
// around the colours it fetches in the storage form or the depth and stencil tests it runs on stand-ins (`ordered`), around
// a body of raster order groups with neither (PipelineRasterOrdered), or none at all
inline bool interlockMatchesStorage(uint32_t control, bool ordered)
{
    return (!ordered && !control) || orderedStorageControl(control);
}

// The stand-ins a record's entries bind: bit 0 depth, bit 1 stencil; 0xFF: one malformed
inline uint8_t testBindings(const SetEntries &entries)
{
    uint8_t bound = 0;
    for (uint32_t i = 0; i < entries.size(); i++) {
        const PackEntry &e = entries[i];
        if (e.kind != EntryDepthTest && e.kind != EntryStencilTest)
            continue;
        if (e.stage != 2 || e.slot || e.samplerKind)
            return 0xFF;
        bound |= e.kind == EntryDepthTest ? 1 : 2;
    }
    return bound;
}

// A shader-side stencil's record (shader_test_push.h shadowStencil): its program runs in the ordered interlock and binds
// one stencil stand-in, its view's header in a pass with a stencil (flags 2) and none in a pass without (flags 0)
inline bool usableShadowStencil(const PackPipeline &pipeline, const SetEntries &entries, uint32_t control)
{
    const PackShaderTests &t = pipeline.shaderTests;
    return orderedStorageControl(control) && !t.depthWord && !t.stencilWord && !t.depthTic && !t.reservedTail &&
           (t.flags == 0 || t.flags == 2) && bool(t.flags) == bool(t.stencilTic) && testBindings(entries) == 2;
}

// A record of no storage attachment whose program runs the depth and stencil tests on their stand-ins
// (direct_storage.h qualifyShaderTests): its draws' tests StencilWrite (fragment_tests.h)
inline bool usableShaderTests(const PackPipeline &pipeline, const SetEntries &entries, uint32_t control)
{
    const PackShaderTests &t = pipeline.shaderTests;
    return orderedStorageControl(control) && t.flags == 3 && t.depthWord && t.stencilWord && t.depthTic &&
           t.stencilTic && !t.reservedTail && pipeline.storageTests == 4 && testBindings(entries) == 3;
}

inline bool usableStorage(const PackPipeline &pipeline, const SetEntries &entries, uint32_t control)
{
    const auto &tests = pipeline.shaderTests;
    if (!(pipeline.flags & PipelineStorageMetadata)) {
        if (pipeline.storageMask || pipeline.storageWriteMask || pipeline.storageSamples)
            return false;
        for (uint32_t tic : pipeline.storageTic)
            if (tic)
                return false;
        if (tests.reserved)
            return !pipeline.storageTests && usableShadowStencil(pipeline, entries, control);
        if (tests.flags)
            return usableShaderTests(pipeline, entries, control);
        if (pipeline.storageTests)
            return false;
        const bool rasterOrdered = pipeline.flags & PipelineRasterOrdered;
        if ((rasterOrdered ? !orderedStorageControl(control) : control != 0) || tests.depthWord || tests.stencilWord ||
            tests.depthTic || tests.stencilTic || tests.reservedTail)
            return false;
        return true;
    }
    if (!orderedStorageControl(control) || !pipeline.storageMask || !nativeSamples(pipeline.storageSamples) ||
        pipeline.storageTests > 4 || (pipeline.storageWriteMask & ~pipeline.storageMask) ||
        (pipeline.flags & (PipelineBarrierForm | PipelineRasterOrdered)))
        return false;
    uint32_t bound = 0;
    for (uint32_t i = 0; i < entries.size(); i++) {
        const PackEntry &e = entries[i];
        if (e.kind == EntryFetch) {
            if (e.stage != 2 || e.slot >= 8)
                return false;
            bound |= 1u << e.slot;
        }
    }
    if ((bound & pipeline.storageMask) != pipeline.storageMask)
        return false;
    if (tests.reserved
            ? !usableShadowStencil(pipeline, entries, control)
            : tests.reservedTail || (tests.flags & ~3u) || testBindings(entries) != tests.flags ||
                  bool(tests.flags & 1) != bool(tests.depthTic) || bool(tests.flags & 2) != bool(tests.stencilTic) ||
                  ((tests.flags & 2) && !(tests.flags & 1)) || (tests.flags && pipeline.storageTests < 2))
        return false;
    for (uint32_t i = 0; i < 8; i++)
        if (bool(pipeline.storageMask >> i & 1) != bool(pipeline.storageTic[i]))
            return false;
    return true;
}

// `far`: the record's far block (set_entries.h), null where it has kPackEntries entries or fewer. Every record is
// qualified here once as it enters (usableRecord, addRecord, directRecord) and its draws read its entries unchecked, so
// one past kPackEntries with no far block is refused here
inline bool usableStorageMetadata(const PackPipeline &pipeline, const uint32_t *blob, uint32_t dwords,
                                  const uint32_t *far)
{
    if ((dwords & 1) || (dwords && !blob) || (entryCount(pipeline) > kPackEntries && !far))
        return false;
    uint32_t control = 0;
    uint8_t explicitStorageWrites = 0;
    bool explicitControl = false;
    uint32_t sampleControl = 0;
    bool earlyTests = false;
    for (uint32_t i = 0; i < dwords; i += 2) {
        const uint32_t method = blob[i], value = blob[i + 1];
        if (method == 0x1224) {
            control = value;
            explicitControl = true;
        }
        if (method == 0x0754)
            sampleControl = value;
        if (method == 0x0210)
            earlyTests = value != 0;
        if (method >= 0x1a00 && method < 0x1a20 && !(method & 3)) {
            const uint32_t bit = 1u << ((method - 0x1a00) / 4);
            if (pipeline.storageMask & bit) {
                if (value & 0x1111)
                    return false;
                explicitStorageWrites |= uint8_t(bit);
            }
        }
    }
    if (!usableStorage(pipeline, SetEntries(pipeline, far), control))
        return false;
    if (pipeline.storageSamples > 1 && sampleControl != sampleShadingControl(pipeline.storageSamples))
        return false;
    if (pipeline.storageTests >= 2 && !pipeline.shaderTests.flags && !earlyTests)
        return false;
    return !pipeline.storageMask || (explicitControl && explicitStorageWrites == pipeline.storageMask);
}

}  // namespace gm20b
