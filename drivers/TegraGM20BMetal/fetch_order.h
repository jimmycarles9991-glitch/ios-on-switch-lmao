// SPDX-License-Identifier: MIT
// A draw that fetches a color it writes takes its state's storage form, inside the pixel interlock, whole. Only a state
// m2v gives no storage form, or a draw whose attachments its storage form cannot view (storage_form.h), fetches
// through the texture unit: it keeps Metal's raster order by drawing one primitive at a time behind the fetch barrier, on
// the GPU's macro engine (RenderEncoder::fetchDraw), whatever its shape but patches. fetchRefusal names the shapes that
// cost more than a barrier a triangle of a short list or strip, which rule fetch.twin (twin_rule.h) keeps in the
// storage form.
//
// Patches: the tessellator makes all of a patch's triangles inside its one BEGIN, so PIXEL_SHADER_BARRIER, a method
// between BEGINs, cannot order them, though Metal does (each fragment reads every earlier fragment of its pixel, a
// patch's triangles in the tessellator's order: an M4 Max, c38-audit/m2v-tess-fetch/tess_probe.mm). The pixel interlock
// orders them only for a program that reads and writes the colour as storage inside its ordering ticket: the program
// waits for and releases the ticket (SPH READS_INTERLOCK_TICKET), SET_PIXEL_SHADER_INTERLOCK_CONTROL only sets how
// tickets are handed out, and the ROP's write lands after the program has released it. So a patch draw is the storage
// form's at any count, and one of a form that fetches through the texture unit is refused (RefGeometry).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "key_tally.h"
#include "storage_form.h"

namespace gm20b {

enum class FetchRefusal : uint8_t {
    None,
    Indirect,     // its primitive count is in GPU memory
    Topology,     // points or lines
    Instances,    // more than one instance
    Restart,      // an indexed strip whose faces decide its pixels, which past a restart only its indices say
    Long,         // more triangles than a draw is split into (kSequentialTriangles)
    PrimitiveId,  // its fragments read the primitive ID, which each triangle's draw would restart
    Geometry,     // patches, whose triangles the barrier form cannot split
    Count
};

// The driver's patch primitive (icb.h icb::kPatchPrimitive)
constexpr uint32_t kFetchPatch = 5;

constexpr size_t kFetchRefusals = size_t(FetchRefusal::Count);
constexpr const char *kFetchRefusalNames[kFetchRefusals] = {"-",       "indirect", "topology",    "instances",
                                                            "restart", "long",     "primitiveid", "geometry"};

// The triangles a draw is split into at most, one draw and one fetch barrier each
constexpr uint32_t kSequentialTriangles = 96;

// Fetching draws by how they kept raster order since the device opened, counted in debug builds only: drawn a primitive
// at a time (the fetch macro) and those primitives; fetched inside the pixel interlock, and of those the draws whose
// shader computes the fixed-function blend there; refused at encoding, by reason
struct FetchCounts {
    std::atomic<uint64_t> sequential{0}, sequentialTriangles{0}, interlocked{0}, interlockedBlends{0};
    // drawn by their state's fetch-free form instead: through the ROP's blend (rung 2), its iterated blend (rung 3)
    std::atomic<uint64_t> fetchFree[2]{};
    // drawn by a storage form's barrier twin where a debug build's probe forces it (fetch_rungs.h), and where rule
    // fetch.twin took it (twin_rule.h)
    std::atomic<uint64_t> blendTwin{0}, twinRule{0};
    std::atomic<uint64_t> refusedAtEncode[kFetchRefusals]{};
    std::atomic<uint64_t> storageFallback[kStorageFallbacks]{};
    // the pipeline key hashes behind refusedAtEncode and storageFallback; and those of every draw in the pixel interlock
    // and of every generic merge state draw, the glass frame's slowest kinds
    KeyTally encodeKeys, fallbackKeys, interlockKeys, genericKeys;
};

struct FetchDraw {
    uint32_t primitive = 3;  // MTLPrimitiveType
    uint32_t count = 0;      // vertices or indices
    uint32_t instances = 1;
    bool indirect = false;
    bool indexed = false;
    // which face a triangle shows decides its pixels (culling, or stencil faces that differ); unknown as a draw is recorded
    bool facing = false;
};

constexpr bool fetchesOwnColor(uint8_t fetchBits, uint8_t writeBits, uint8_t storageBits)
{
    return fetchBits & writeBits & ~storageBits;
}

constexpr bool singlePrimitive(const FetchDraw &d)
{
    if (!d.count || !d.instances)
        return true;
    if (d.primitive == kFetchPatch)
        return false;
    const uint32_t vertices = d.primitive == 0 ? 1 : d.primitive <= 2 ? 2 : 3;
    return d.count <= vertices && d.instances == 1;
}

constexpr uint32_t triangles(const FetchDraw &d)
{
    return d.primitive == 4 ? (d.count > 2 ? d.count - 2 : 0) : d.count / 3;
}

// The primitives `count` vertices or indices of MTLPrimitiveType `primitive` make, none restarted
constexpr uint32_t primitives(uint32_t primitive, uint32_t count)
{
    switch (primitive) {
    case 0:
        return count;
    case 1:
        return count / 2;
    case 2:
        return count > 1 ? count - 1 : 0;
    case 3:
        return count / 3;
    default:
        return count > 2 ? count - 2 : 0;
    }
}

// What keeps a list or strip of one instance from being drawn a triangle at a time
constexpr FetchRefusal sequentialRefusal(bool primitiveId, const FetchDraw &d)
{
    if (d.indexed && d.primitive == 4 && d.facing)
        return FetchRefusal::Restart;
    if (triangles(d) > kSequentialTriangles)
        return FetchRefusal::Long;
    return primitiveId ? FetchRefusal::PrimitiveId : FetchRefusal::None;
}

constexpr FetchRefusal fetchRefusal(bool fetchesOwn, bool primitiveId, const FetchDraw &d)
{
    if (!fetchesOwn)
        return FetchRefusal::None;
    if (d.primitive == kFetchPatch && (d.indirect || !singlePrimitive(d)))
        return FetchRefusal::Geometry;
    if (d.indirect)
        return FetchRefusal::Indirect;
    if (singlePrimitive(d))
        return FetchRefusal::None;
    if (d.primitive != 3 && d.primitive != 4)
        return FetchRefusal::Topology;
    if (d.instances != 1)
        return FetchRefusal::Instances;
    return sequentialRefusal(primitiveId, d);
}

constexpr const char *fetchRefusalWhy(FetchRefusal r)
{
    return r == FetchRefusal::Indirect
               ? "indirect framebuffer-fetch draws require an interlocked program"
               : "framebuffer-fetch ordering requires an interlocked program or available bounded geometry";
}

}  // namespace gm20b
