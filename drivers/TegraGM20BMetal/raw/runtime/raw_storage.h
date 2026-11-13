// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include "raw_sample_shading.h"

namespace raw {

inline bool orderedStorageControl(uint32_t control)
{
    return control == 1 || control == 2;
}

inline bool usableStorage(const PackPipeline &pipeline, uint32_t control)
{
    if (pipeline.entries > kPackEntries)
        return false;
    if (!(pipeline.flags & PipelineStorageMetadata)) {
        if (control || pipeline.storageMask || pipeline.storageWriteMask || pipeline.storageSamples ||
            pipeline.storageTests)
            return false;
        for (uint32_t tic : pipeline.storageTic)
            if (tic)
                return false;
        const auto &tests = pipeline.shaderTests;
        if (tests.depthWord || tests.stencilWord || tests.flags || tests.reserved || tests.depthTic ||
            tests.stencilTic || tests.reservedTail)
            return false;
        return true;
    }
    if (!orderedStorageControl(control) || !pipeline.storageMask || !nativeSamples(pipeline.storageSamples) ||
        pipeline.storageTests > 4 || (pipeline.storageWriteMask & ~pipeline.storageMask) ||
        (pipeline.flags & PipelineBarrierForm))
        return false;
    uint32_t bound = 0;
    uint8_t testBindings = 0;
    for (uint32_t i = 0; i < pipeline.entries; i++) {
        const PackEntry &e = pipeline.entry[i];
        if (e.kind == EntryFetch) {
            if (e.stage != 2 || e.slot >= 8)
                return false;
            bound |= 1u << e.slot;
        }
        if (e.kind == EntryDepthTest || e.kind == EntryStencilTest) {
            if (e.stage != 2 || e.slot || e.samplerKind)
                return false;
            testBindings |= e.kind == EntryDepthTest ? 1 : 2;
        }
    }
    if ((bound & pipeline.storageMask) != pipeline.storageMask)
        return false;
    const auto &tests = pipeline.shaderTests;
    if (tests.reserved || tests.reservedTail || (tests.flags & ~3u) || testBindings != tests.flags ||
        bool(tests.flags & 1) != bool(tests.depthTic) || bool(tests.flags & 2) != bool(tests.stencilTic) ||
        ((tests.flags & 2) && !(tests.flags & 1)) || (tests.flags && pipeline.storageTests < 2))
        return false;
    for (uint32_t i = 0; i < 8; i++)
        if (bool(pipeline.storageMask >> i & 1) != bool(pipeline.storageTic[i]))
            return false;
    return true;
}

inline bool usableStorageMetadata(const PackPipeline &pipeline, const uint32_t *blob, uint32_t dwords)
{
    if ((dwords & 1) || (dwords && !blob))
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
    if (!usableStorage(pipeline, control))
        return false;
    if (pipeline.storageSamples > 1 && sampleControl != sampleShadingControl(pipeline.storageSamples))
        return false;
    if (pipeline.storageTests >= 2 && !pipeline.shaderTests.flags && !earlyTests)
        return false;
    return !pipeline.storageMask || (explicitControl && explicitStorageWrites == pipeline.storageMask);
}

}  // namespace raw
