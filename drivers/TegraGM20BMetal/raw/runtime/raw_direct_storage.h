// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstring>
#include "bridge/NativeMetalDirect.h"
#include "raw_storage.h"

namespace raw {

inline bool storageDepthQualified(const char *key, bool earlyTests = false, uint32_t tests = 0)
{
    const char *field = strstr(key, "ds=");
    unsigned depth = 0, stencil = 0;
    return field && sscanf(field, "ds=%u/%u", &depth, &stencil) == 2 &&
           ((!depth && !stencil) || earlyTests || tests == 1);
}

template <class Header> const char *qualifyStorage(const NmbDirectState &state, Header header, PackPipeline &out)
{
    const NmbDirectStorage &s = state.storage;
    if (state.storageForm != bool(s.mask) || (s.mask & ~255u) || (s.writes & ~s.mask) || (s.blendMask & ~s.writes) ||
        (s.blendModel & ~(kRopBlendAssumptions >> kRopBlendShift)))
        return "storage form: inconsistent attachment masks";
    if (!s.mask)
        return nullptr;
    if (!nativeSamples(s.samples) || s.samplePlanes)
        return "storage form: sample planes or multisampled storage require a qualified native view";
    if (s.fragmentTests > 4 || s.testPlanes ||
        (s.depthFormat && (s.depthFormat != 100 || s.testSamples != s.samples)) ||
        (s.stencilFormat && (s.stencilFormat != 13 || !s.depthFormat)))
        return "storage form: unsupported depth/stencil storage view";
    constexpr int32_t kViewType2D = 1, kViewType2DArray = 5;
    for (const NmbDirectDraw &draw : state.draws) {
        uint32_t bound = 0;
        uint32_t tests = 0;
        // a slice-choosing state's layered form (raw_pack.h layeredRecord) binds its attachments as arrays of the pass's
        // layers, every other record as one 2D layer
        const int32_t attachmentView = layeredRecord(uint8_t(draw.arrayLengthWord)) ? kViewType2DArray : kViewType2D;
        for (uint32_t i = 0; i < draw.writeCount; i++) {
            const NmbDirectWrite &w = draw.writes[i];
            if (w.source == NmbDirectDepthStandIn || w.source == NmbDirectStencilStandIn) {
                if (w.stage != 2 || w.set || w.type != 3 || w.viewType != kViewType2D)
                    return "storage form: invalid depth/stencil descriptor";
                tests |= w.source == NmbDirectDepthStandIn ? 1u : 2u;
                continue;
            }
            if (w.source != NmbDirectStorageAttachment)
                continue;
            if (w.slot >= 8 || w.stage != 2 || w.set != 0 || w.type != 3 || !(s.mask & (1u << w.slot)))
                return "storage form: attachment descriptor does not match its storage view";
            if (w.viewType != attachmentView)
                return w.viewType == kViewType2DArray
                           ? "storage form: a layered storage view in a record not of the layered form"
                           : "storage form: a record of the layered form whose storage view is one 2D layer";
            bound |= 1u << w.slot;
        }
        if (bound != s.mask)
            return "storage form: probe variant does not bind every storage attachment";
        if (tests != (uint32_t(bool(s.depthFormat)) | uint32_t(bool(s.stencilFormat)) << 1))
            return "storage form: depth/stencil bindings do not match the shader contract";
    }
    uint32_t words[8] = {};
    PackShaderTests tests = {};
    if (s.depthFormat) {
        if (!header(s.depthFormat, tests.depthTic) || !tests.depthTic)
            return "storage form: native depth storage view unavailable";
        tests.flags = 1;
    }
    if (s.stencilFormat) {
        if (!header(s.stencilFormat, tests.stencilTic) || !tests.stencilTic)
            return "storage form: native stencil storage view unavailable";
        tests.flags |= 2;
    }
    for (uint32_t i = 0; i < 8; i++)
        if ((s.mask >> i & 1) && (!s.formats[i] || !header(s.formats[i], words[i]) || !words[i]))
            return "storage form: NVK cannot represent the attachment storage format";
    out.flags |= PipelineStorageMetadata;
    out.storageMask = uint8_t(s.mask);
    out.storageWriteMask = uint8_t(s.writes);
    out.storageBlendMask = uint8_t(s.blendMask);
    out.storageBlendModel = uint8_t(s.blendModel);
    out.storageSamples = uint8_t(s.samples);
    out.storageTests = uint8_t(s.fragmentTests);
    out.shaderTests = tests;
    for (uint32_t i = 0; i < 8; i++)
        out.storageTic[i] = words[i];
    return nullptr;
}

}  // namespace raw
