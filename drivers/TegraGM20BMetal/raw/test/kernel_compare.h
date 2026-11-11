// SPDX-License-Identifier: MIT
// A kernel record made directly (runtime/raw_direct_compute.h) against the pack's of its key, byte for byte: its PackKernel but
// the program index and key string offset (each pack's own), its sampler entries by the header each names, and its program by
// its code, shader data and constant buffer types.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "../runtime/raw_compute_record.h"

namespace kernelcompare {

inline const uint32_t *packSampler(const raw::Pack &pack, uint8_t slot)
{
    const uint32_t index = slot & 0x7F;
    return index < pack.header()->samplers ? pack.section<uint32_t>(raw::PackSamplers) + 8 * index : nullptr;
}

inline bool sameEntry(const raw::Pack &pack, const raw::PackKernelEntry &p, const raw::ComputeRecord &r,
                      const raw::PackKernelEntry &d)
{
    if (p.offset != d.offset || p.kind != d.kind || p.window != d.window)
        return false;
    if (p.kind != raw::KernelSampler)
        return p.slot == d.slot;
    const uint32_t *header = packSampler(pack, p.slot);
    const size_t own = d.slot & 0x7F;
    return header && (p.slot & 0x80) && (d.slot & 0x80) && own < r.samplers.size() &&
           !memcmp(header, r.samplers[own].data(), 32);
}

// Whether `bytes` holds `want` and then zeros
inline bool padded(const uint8_t *bytes, size_t length, const std::vector<uint8_t> &want)
{
    if (want.size() > length || memcmp(bytes, want.data(), want.size()))
        return false;
    for (size_t i = want.size(); i < length; i++)
        if (bytes[i])
            return false;
    return true;
}

inline std::string programDifferences(const raw::Pack &pack, const raw::PackKernel &p, const raw::ComputeRecord &r)
{
    const raw::PackProgram &pp = pack.section<raw::PackProgram>(raw::PackPrograms)[p.program];
    const uint8_t *slot = pack.section<uint8_t>(raw::PackHeap) + pp.heapOffset;
    std::string out;
    if (!padded(slot, pp.dataBytes ? pp.dataOffset : pp.bytes, r.code))
        out += " code(" + std::to_string(r.code.size()) + "B)";
    if (pp.dataBytes != r.data.size() || (pp.dataBytes && memcmp(slot + pp.dataOffset, r.data.data(), pp.dataBytes)))
        out += " data";
    if (pp.cbufs != r.cbufs || memcmp(pp.cbufType, r.cbufType, std::min<size_t>(pp.cbufs, sizeof r.cbufType)))
        out += " cbufs";
    return out;
}

// " [<field> ...]" naming each part of `r` that is not the pack's `p`; empty: the same
inline std::string differences(const raw::Pack &pack, const raw::PackKernel &p, const raw::ComputeRecord &r)
{
    const raw::PackKernel &d = r.kernel;
    std::string out;
    for (int i = 0; i < 64; i++)
        if (p.qmd[i] != d.qmd[i]) {
            char w[48];
            snprintf(w, sizeof w, " qmd[%d]=%x/%x", i, p.qmd[i], d.qmd[i]);
            out += w;
        }
    if (p.regionWord != d.regionWord)
        out += " region=" + std::to_string(p.regionWord) + "/" + std::to_string(d.regionWord);
    if (p.setBytes != d.setBytes)
        out += " set=" + std::to_string(p.setBytes) + "/" + std::to_string(d.setBytes);
    if (p.pushMask != d.pushMask)
        out += " pushMask";
    for (int i = 0; i < 32; i++)
        if (p.pushMask >> i & 1 && p.pushWords[i] != d.pushWords[i])
            out += " push[" + std::to_string(i) + "]";
    if (p.entries != d.entries)
        out += " entries=" + std::to_string(p.entries) + "/" + std::to_string(d.entries);
    for (int i = 0; i < std::min(p.entries, d.entries); i++)
        if (!sameEntry(pack, p.entry[i], r, d.entry[i])) {
            char w[96];
            snprintf(w, sizeof w, " entry[%d]=%u:%u:%u:%x/%u:%u:%u:%x", i, p.entry[i].offset, p.entry[i].kind,
                     p.entry[i].slot, p.entry[i].window, d.entry[i].offset, d.entry[i].kind, d.entry[i].slot,
                     d.entry[i].window);
            out += w;
        }
    out += programDifferences(pack, p, r);
    return out.empty() ? out : " [" + out.substr(1) + "]";
}

}  // namespace kernelcompare
