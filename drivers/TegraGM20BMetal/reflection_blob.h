// SPDX-License-Identifier: MIT
// A pipeline state's reflection as the translator makes it with no m2v runtime (translate/NativeMetalReflection.rs blob):
// the pack holds one per packed render and compute state (pack.h PackReflections), the system shader compiler's
// store one per state it was asked for (RequestKind::Reflection). Little endian. A header, each stage's bindings as a
// range of the edge table (compute: its kernel's; render: vertex, fragment, tile, object, mesh), the nodes
// (NmbReflectionNode's kinds and values: 1 binding, 2 struct, 3 member, 4 array, 5 pointer, 6 texture reference), the
// edges (node indices: a stage's bindings, a struct's members) and the names, NUL-terminated. A node's children and
// members precede it.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "cache_io.h"
#include "pack.h"

namespace gm20b {

constexpr char kReflectionMagic[4] = {'S', 'W', 'R', 'F'};
constexpr uint32_t kReflectionVersion = 1;
constexpr uint32_t kReflectionComputeStages = 1, kReflectionRenderStages = 5;
constexpr uint32_t kReflectionAbsent = UINT32_MAX;
constexpr uint32_t kKindReflection = 9, kKindReflectionRefusal = 10;  // after the facts (5, 6) and library (7, 8) kinds

enum ReflectionKind : uint32_t {
    ReflectionBinding = 1,
    ReflectionStruct,
    ReflectionMember,
    ReflectionArray,
    ReflectionPointer,
    ReflectionTexture
};

#pragma pack(push, 1)

struct ReflectionHeader {
    char magic[4];
    uint32_t version, stages, nodes, edges, nameBytes;
};

struct ReflectionRange {
    uint32_t first, count;
};

struct ReflectionNode {
    uint32_t kind, flags;
    uint32_t name;  // its offset in the names + 1; 0: none
    uint32_t first, count;
    // struct, array, texture reference, pointer (a binding's: its struct and pointer); kReflectionAbsent: none
    uint32_t children[4];
    uint32_t reserved;
    // A binding's: 0 its MTLBindingType, 1 access, 2 index, 3 a buffer's data type, 5 and 6 a buffer's or threadgroup
    // memory's alignment and size or a texture's type and data type, 7 its array length (MTLArgument arrayLength, 1 but
    // for an array of textures)
    uint64_t values[8];
};

#pragma pack(pop)

static_assert(sizeof(ReflectionHeader) == 24 && sizeof(ReflectionNode) == 104,
              "translate/NativeMetalReflection.rs blob");

// A blob read in place: valid only while its bytes are
struct ReflectionView {
    const ReflectionHeader *header = nullptr;
    const ReflectionRange *stages = nullptr;
    const ReflectionNode *nodes = nullptr;
    const uint32_t *edges = nullptr;
    const char *names = nullptr;

    const char *name(const ReflectionNode &n) const
    {
        return n.name ? names + n.name - 1 : nullptr;
    }
};

// `bytes` as a blob of `stages` stages; false where it is not one whole (bounds, kinds, order or names)
inline bool readReflection(const void *bytes, size_t length, uint32_t stages, ReflectionView &out)
{
    const auto *at = static_cast<const uint8_t *>(bytes);
    if (!at || length < sizeof(ReflectionHeader))
        return false;
    const auto *h = reinterpret_cast<const ReflectionHeader *>(at);
    if (memcmp(h->magic, kReflectionMagic, sizeof h->magic) || h->version != kReflectionVersion ||
        h->stages != stages || h->nodes > length / sizeof(ReflectionNode) || h->edges > length / 4)
        return false;
    const uint64_t nodesAt = sizeof(ReflectionHeader) + uint64_t(h->stages) * sizeof(ReflectionRange);
    const uint64_t edgesAt = nodesAt + uint64_t(h->nodes) * sizeof(ReflectionNode);
    const uint64_t namesAt = edgesAt + uint64_t(h->edges) * 4;
    if (namesAt + h->nameBytes != length || (h->nameBytes && at[length - 1]))
        return false;
    ReflectionView v;
    v.header = h;
    v.stages = reinterpret_cast<const ReflectionRange *>(at + sizeof(ReflectionHeader));
    v.nodes = reinterpret_cast<const ReflectionNode *>(at + nodesAt);
    v.edges = reinterpret_cast<const uint32_t *>(at + edgesAt);
    v.names = reinterpret_cast<const char *>(at + namesAt);
    const auto range = [&](uint32_t first, uint32_t count) {
        return first <= h->edges && count <= h->edges - first;
    };
    for (uint32_t s = 0; s < h->stages; s++) {
        if (!range(v.stages[s].first, v.stages[s].count))
            return false;
        for (uint32_t e = 0; e < v.stages[s].count; e++)
            if (v.edges[v.stages[s].first + e] >= h->nodes ||
                v.nodes[v.edges[v.stages[s].first + e]].kind != ReflectionBinding)
                return false;
    }
    static constexpr uint32_t kChildKinds[4] = {ReflectionStruct, ReflectionArray, ReflectionTexture,
                                                ReflectionPointer};
    for (uint32_t i = 0; i < h->nodes; i++) {
        const ReflectionNode &n = v.nodes[i];
        if (n.kind < ReflectionBinding || n.kind > ReflectionTexture || (n.name && n.name > h->nameBytes) ||
            !range(n.first, n.count) || (n.count && n.kind != ReflectionStruct))
            return false;
        for (uint32_t e = 0; e < n.count; e++)
            if (v.edges[n.first + e] >= i || v.nodes[v.edges[n.first + e]].kind != ReflectionMember)
                return false;
        for (uint32_t c = 0; c < 4; c++) {
            const uint32_t child = n.children[c];
            if (child != kReflectionAbsent && (child >= i || v.nodes[child].kind != kChildKinds[c]))
                return false;
        }
    }
    out = v;
    return true;
}

// The store's entry of a reflection (gm20b_cache.h's entry head and tail), or of why it was refused
inline std::string encodeReflectionEntry(const CacheKey &k, const std::string *blob, const std::string &refusal)
{
    Writer w;
    writeEntryHead(w, k, blob ? kKindReflection : kKindReflectionRefusal);
    w.text(blob ? *blob : refusal);
    w.value(Pack::fnv(w.out.data(), w.out.size()));
    return w.out;
}

// False: no entry of `k`; otherwise `made` says whether `text` is its blob or its refusal
inline bool decodeReflectionEntry(const std::string &bytes, const CacheKey &k, bool &made, std::string &text)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind) || (kind != kKindReflection && kind != kKindReflectionRefusal))
        return false;
    text = r.text();
    made = kind == kKindReflection;
    return r.ok() && r.at() == bytes.size() - 8;
}

}  // namespace gm20b
