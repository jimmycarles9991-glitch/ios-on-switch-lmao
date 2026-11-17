// SPDX-License-Identifier: MIT
// A compile request's bytes in layout SWCREQ03, written field by field as raw_compile_request.cpp wrote them at
// 2dd3e358f, for the reader of a later layout to refuse. `merged`: as that writer would have with the vertex stage's
// cubic bits (the sampler's flag bit 1, the texture slot's top bit) and the shadowed colours' byte after the test class
// put in under the same magic.
#pragma once
#include <string>
#include "../raw/runtime/raw_cache_io.h"
#include "../raw/runtime/raw_compile_request.h"

namespace layout03 {

inline void writeVariant(raw::Writer &w, const raw::RenderVariant &v, bool merged)
{
    w.value(uint32_t(v.cubic.samplers.size()));
    for (const raw::CubicBinds::Sampler &s : v.cubic.samplers) {
        const bool vertex = merged && s.stage == raw::kCubicVertex;
        const uint8_t packed[6] = {s.slot, s.s, s.t, s.r, s.border, uint8_t(s.normalized | (vertex ? 2 : 0))};
        w.bytes(packed, sizeof packed);
    }
    w.value(uint32_t(v.cubic.textures.size()));
    for (const raw::CubicBinds::Texture &t : v.cubic.textures) {
        const bool vertex = merged && t.stage == raw::kCubicVertex;
        w.value(uint8_t(t.slot | (vertex ? 0x80 : 0)));
        w.value(t.format);
    }
    w.value(v.fragmentTests);
    if (merged)
        w.value(v.xrShadows);
    w.value(uint8_t(v.twoPlane));
    w.value(v.xrSlots);
    w.value(uint32_t(v.written.size()));
    for (const raw::WrittenTexture &t : v.written) {
        w.value(t.slot);
        w.value(t.format);
    }
}

inline void writeKernel(raw::Writer &w, const raw::KernelRequest &k)
{
    w.value(k.maxThreads);
    w.value(uint8_t(k.multipleOfWidth));
    w.value(uint8_t(k.regionMode));
    w.bytes(k.threads, sizeof k.threads);
    w.bytes(k.memory, sizeof k.memory);
    w.list(k.textures);
    w.list(k.samplers);
    w.bytes(&k.stageIn, sizeof k.stageIn);
}

inline std::string encode(const raw::CompileRequest &r, bool merged = false)
{
    static constexpr char kMagic[8] = {'S', 'W', 'C', 'R', 'E', 'Q', '0', '3'};
    raw::Writer w;
    w.bytes(kMagic, sizeof kMagic);
    w.value(r.stack);
    w.value(uint8_t(r.kind));
    w.value(uint8_t(r.form));
    w.text(r.key);
    w.bytes(&r.fields, sizeof r.fields);
    w.value(uint32_t(r.stages.size()));
    for (const raw::RequestStage &s : r.stages) {
        w.text(s.tag);
        w.text(s.function);
        w.text(s.constants);
        w.text(s.name);
        w.value(uint8_t(s.specialized));
        w.value(s.library);
    }
    w.value(uint32_t(r.libraries.size()));
    for (const raw::RequestLibrary &l : r.libraries) {
        w.text(l.path);
        w.value(uint8_t(l.system));
        w.text(l.bytes);
    }
    writeVariant(w, r.variant, merged);
    writeKernel(w, r.kernel);
    return w.out;
}

}  // namespace layout03
