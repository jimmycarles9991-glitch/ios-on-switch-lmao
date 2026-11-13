// SPDX-License-Identifier: MIT
// A state's fragment textures its programs write as storage images, and the formats they were compiled for
// (PackPipeline::writtenFormat). m2v compiles a storage texture's stores for the format bound at the slot: a 565 code
// written to an R16Uint texture is stored whole, the same code written to an RGBA8Uint one saturated to 255. A draw that
// binds another format at such a slot than the state's record was made for draws the record of the state's " | wtex="
// variant, made for the formats bound (RawMetalCubic.mm renderVariant), never the record itself.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "raw_pack.h"

namespace raw {

struct WrittenTexture {
    uint8_t slot;
    uint32_t format;  // MTLPixelFormat

    bool operator==(const WrittenTexture &o) const
    {
        return slot == o.slot && format == o.format;
    }
};

// The variant's name after its state's key: " | wtex=<slot>:<MTLPixelFormat>,..." in slot order; "" for none
inline std::string writtenVariant(const std::vector<WrittenTexture> &written)
{
    std::string name;
    for (const WrittenTexture &w : written)
        name += (name.empty() ? " | wtex=" : ",") + std::to_string(w.slot) + ":" + std::to_string(w.format);
    return name;
}

// A corpus line's "wtex=<slot>:<MTLPixelFormat>,..." segment; false: not one, or slots out of order or past the record's
inline bool parseWritten(const std::string &segment, std::vector<WrittenTexture> &out)
{
    if (segment.rfind("wtex=", 0) != 0)
        return false;
    out.clear();
    const char *at = segment.c_str() + 5;
    for (;;) {
        char *end = nullptr;
        const unsigned long slot = strtoul(at, &end, 10);
        if (end == at || *end != ':' || slot >= kWrittenSlots || (!out.empty() && slot <= out.back().slot))
            return false;
        at = end + 1;
        const unsigned long format = strtoul(at, &end, 10);
        if (end == at || !format || format > UINT16_MAX)
            return false;
        out.push_back({uint8_t(slot), uint32_t(format)});
        if (*end == '\0')
            return true;
        if (*end != ',')
            return false;
        at = end + 1;
    }
}

// The fragment slots `rec` writes a texture at whose bound format (`formatAt(slot)`, 0: none bound) is not the one its
// programs were compiled for, with the bound format: the variant a draw of these bindings needs (empty: the record itself).
// An unbound slot matches any record: a store to it is dropped.
template <typename FormatAt> std::vector<WrittenTexture> reboundWritten(const PackPipeline &rec, FormatAt formatAt)
{
    std::vector<WrittenTexture> out;
    for (uint32_t slot = 0; slot < kWrittenSlots; slot++)
        if (const uint32_t compiled = rec.writtenFormat[slot]) {
            const uint32_t bound = formatAt(slot);
            if (bound && bound != compiled)
                out.push_back({uint8_t(slot), bound});
        }
    return out;
}

// The slots `rec` writes a texture at (bit k: slot k)
inline uint16_t writtenSlots(const PackPipeline &rec)
{
    uint16_t slots = 0;
    for (uint32_t slot = 0; slot < kWrittenSlots; slot++)
        if (rec.writtenFormat[slot])
            slots |= uint16_t(1u << slot);
    return slots;
}

// Whether `rec`'s programs were compiled for each of `written`'s formats at its slot
inline bool compiledFor(const PackPipeline &rec, const std::vector<WrittenTexture> &written)
{
    for (const WrittenTexture &w : written)
        if (w.slot >= kWrittenSlots || rec.writtenFormat[w.slot] != w.format)
            return false;
    return true;
}

}  // namespace raw
