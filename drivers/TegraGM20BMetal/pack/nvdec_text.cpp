// SPDX-License-Identifier: MIT
#include "nvdec.h"
#include "nvdec_methods.h"

#include <algorithm>
#include <cstring>

namespace nvdec {
namespace {

// a record's text without the run's resource names (R<n>): what goes into a hash (names are compared as text, by tools/eqcheck.py
// in order of first appearance, so the two runs' numbering may differ)
std::string nameless(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == 'R' && i + 1 < s.size() && isdigit(s[i + 1]) && (i == 0 || !isalnum(s[i - 1]))) {
            while (i + 1 < s.size() && isdigit(s[i + 1]))
                i++;
            out += 'R';
            continue;
        }
        out += s[i];
    }
    return out;
}
}  // namespace

std::map<uint64_t, int> &Decoder::names()
{
    static std::map<uint64_t, int> n;
    return n;
}

std::string Decoder::name(uint64_t va)
{
    if (!va)
        return "null";
    const uint8_t *host = map_ ? map_(va, 1) : nullptr;
    const uint64_t key = host ? uint64_t(uintptr_t(host)) : va;
    auto &n = names();
    auto it = n.find(key);
    if (it == n.end())
        it = n.emplace(key, int(n.size()) + 1).first;
    return "R" + std::to_string(it->second);
}

// The bytes of [va, va + bytes) with every GPU pointer replaced by the hash of what it points to (a descriptor's own size when
// it is an nvk_buffer_address {addr, size, 0}, else 64 bytes), and, in descriptor sets, every image/sampler handle by its
// texture/sampler header. Printed as a hash plus the decoded words.
std::string Decoder::contentHash(uint64_t va, uint64_t bytes, int depth, bool handles, bool childHandles)
{
    if (!bytes)
        return "empty";
    bytes = std::min<uint64_t>(bytes, 1 << 16);
    std::vector<uint8_t> b(bytes);
    if (!read(va, b.data(), bytes))
        return "unmapped:" + hex(bytes);
    std::string words;
    std::vector<bool> done(bytes / 4 + 1, false);
    if (handles && layout_) {  // a descriptor set: keep only the bytes its layout's entries cover
        std::vector<uint8_t> keep(bytes, 0);
        for (auto &r : layout_->ranges)
            for (uint32_t k = r.first; k < r.first + r.second && k < bytes; k++)
                keep[k] = 1;
        for (uint64_t k = 0; k < bytes; k++)
            if (!keep[k])
                b[k] = 0;
        if (getenv("NVDEC_SETWORDS")) {
            std::string l = "set:";
            for (uint64_t k = 0; k + 4 <= bytes && k < 96; k += 4) {
                uint32_t x;
                memcpy(&x, &b[k], 4);
                char s[12];
                snprintf(s, sizeof s, " %x", x);
                l += s;
            }
            words += " [" + l + "]";
        }
    }
    for (uint64_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t v;
        memcpy(&v, &b[i], 8);
        const uint64_t hi = v >> 32;
        if (hi < 1 || hi >= 0x80 || !map_ || !map_(v, 1) || depth >= 3)
            continue;
        uint32_t size = 64, z = 1;
        if (i % 16 == 0 && i + 16 <= bytes) {
            memcpy(&size, &b[i + 8], 4);
            memcpy(&z, &b[i + 12], 4);
            if (z != 0 || size == 0 || size > (1u << 16))
                size = 64;
        }
        const std::string inner = contentHash(v, size, depth + 1, childHandles, false);
        const std::string innerKey = nameless(inner);
        const uint64_t h = fnv(innerKey.data(), innerKey.size());
        memcpy(&b[i], &h, 8);
        done[i / 4] = done[i / 4 + 1] = true;
        words += " @" + std::to_string(i) + "=ptr{" + inner + "}";
        if (getenv("NVDEC_HEAD")) {
            uint32_t w[20] = {};
            read(v, w, 80);
            char t[96];
            snprintf(t, sizeof t, "[%08x %08x %08x %08x .. %08x]", w[0], w[1], w[2], w[3], w[16]);
            words += t;
        }
    }
    if (handles) {
        const uint32_t ticMax = r3(TIC_POOL_C), tscMax = r3(TSC_POOL_C);
        for (uint64_t i = 0; i + 4 <= bytes; i += 4) {
            if (done[i / 4])
                continue;
            uint32_t w;
            memcpy(&w, &b[i], 4);
            if (!w)
                continue;
            const uint32_t tic = w & 0xfffff, tsc = w >> 20;
            if (tic > ticMax || tsc > tscMax)
                continue;
            // NVK gives out sampler index 0 like any other: resolve it, unless the layout says the program reads no sampler there
            const bool noSampler = layout_ && layout_->textureOnly.count(uint32_t(i));
            const std::string t = tic ? ticEntry(tic) : "-", s = noSampler ? "-" : tscEntry(tsc);
            if (t == "zero" || s == "zero")
                continue;
            const std::string both = t + "/" + s, bothKey = nameless(both);
            const uint32_t h = uint32_t(fnv(bothKey.data(), bothKey.size()));
            memcpy(&b[i], &h, 4);
            words += " @" + std::to_string(i) + "=handle{" + both + "}";
        }
    }
    return hex(fnv(b.data(), b.size())) + ":" + hex(bytes) + words;
}

std::string Decoder::ticEntry(uint32_t index)
{
    uint32_t t[8];
    read(addr3(TIC_POOL_A, TIC_POOL_B) + uint64_t(index) * 32, t, 32);
    bool any = false;
    for (uint32_t w : t)
        any |= w != 0;
    if (!any)
        return "zero";
    const uint32_t version = (t[2] >> 21) & 7;
    const uint32_t mask = version == 3 || version == 4 ? 0xfffffe00u : version == 0 ? 0xffffffffu : 0xffffffe0u;
    const uint64_t address = (uint64_t(t[2] & 0xffff) << 32) | (t[1] & mask);
    // NVDEC_TEXELS: a pitch header's texels as a draw samples them, row by row (the image, wherever and however its rows lie:
    // a surface's planes and the pool image the copy engine fills from them hash the same)
    char texels[40] = "";
    static const bool wantTexels = getenv("NVDEC_TEXELS") != nullptr;
    if (wantTexels && version == 2) {
        uint32_t bpp = 0;
        switch (t[0] & 0x7f) {
        case 0x1d:
            bpp = 1;
            break;  // R8
        case 0x18:
            bpp = 2;
            break;  // G8R8
        case 0x08:
            bpp = 4;
            break;  // A8B8G8R8
        case 0x03:
            bpp = 8;
            break;  // R16G16B16A16
        case 0x01:
            bpp = 16;
            break;  // R32G32B32A32
        default:
            break;
        }
        const uint32_t pitch = (t[3] & 0xffff) << 5, w = (t[4] & 0xffff) + 1, h = (t[5] & 0xffff) + 1;
        if (bpp && pitch >= w * bpp && h <= 16384) {
            std::vector<uint8_t> row(size_t(w) * bpp);
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t y = 0; y < h; y++) {
                if (!read(address + uint64_t(y) * pitch, row.data(), row.size()))
                    break;
                hash = fnv(row.data(), row.size(), hash);
            }
            snprintf(texels, sizeof texels, " texels=%016llx", (unsigned long long)hash);
        }
    }
    t[1] &= ~mask;
    t[2] &= ~0xffffu;
    char b[200];
    snprintf(b, sizeof b, "tic[%s %08x %08x %08x %08x %08x %08x %08x %08x%s]", name(address).c_str(), t[0], t[1], t[2],
             t[3], t[4], t[5], t[6], t[7], texels);
    return b;
}

std::string Decoder::tscEntry(uint32_t index)
{
    uint32_t t[8];
    read(addr3(TSC_POOL_A, TSC_POOL_B) + uint64_t(index) * 32, t, 32);
    bool any = false;
    for (uint32_t w : t)
        any |= w != 0;
    if (!any)
        return "zero";
    char b[120];
    snprintf(b, sizeof b, "tsc[%08x %08x %08x %08x %08x %08x %08x %08x]", t[0], t[1], t[2], t[3], t[4], t[5], t[6],
             t[7]);
    return b;
}

}  // namespace nvdec
