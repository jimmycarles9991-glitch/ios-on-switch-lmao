// SPDX-License-Identifier: MIT
// A reflection blob (raw/runtime/raw_reflection_blob.h) of an NmbReflectionNode table as m2v's compute artifact holds it
// (translate/NativeMetalReflection.rs Reflection::of: node 0 the root, its edges the bindings), laid out as the bridge's
// Reflection::blob lays the same bindings out: no root, every other node one index down, the root's bindings stage 0's.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "../raw/runtime/raw_reflection_blob.h"
#include "bridge/NativeMetalComputeArtifact.h"

namespace reflection_blob {

inline void word(std::string &out, uint32_t w)
{
    out.append(reinterpret_cast<const char *>(&w), sizeof w);
}

// `nodes` and `edges` as m2v's table; `stages`: 1 (compute) or 5 (render, the root's bindings the vertex stage's)
inline std::string of(const std::vector<NmbReflectionNode> &nodes, const std::vector<uint32_t> &edges, uint32_t stages)
{
    const auto down = [](uint32_t i) {
        return i == raw::kReflectionAbsent ? i : i - 1;
    };
    std::string names, body;
    std::vector<uint32_t> nameAt;
    for (size_t i = 1; i < nodes.size(); i++) {
        nameAt.push_back(nodes[i].name ? uint32_t(names.size()) + 1 : 0);
        if (nodes[i].name)
            names.append(nodes[i].name, strlen(nodes[i].name) + 1);
    }
    std::string out(raw::kReflectionMagic, sizeof raw::kReflectionMagic);
    for (const uint32_t w :
         {raw::kReflectionVersion, stages, uint32_t(nodes.size() - 1), uint32_t(edges.size()), uint32_t(names.size())})
        word(out, w);
    for (uint32_t s = 0; s < stages; s++) {
        word(out, s ? 0 : nodes[0].first);
        word(out, s ? 0 : nodes[0].count);
    }
    for (size_t i = 1; i < nodes.size(); i++) {
        const NmbReflectionNode &n = nodes[i];
        for (const uint32_t w : {n.kind, n.flags, nameAt[i - 1], n.first, n.count})
            word(out, w);
        for (const uint32_t child : n.children)
            word(out, down(child));
        word(out, 0);
        out.append(reinterpret_cast<const char *>(n.values), sizeof n.values);
    }
    for (const uint32_t e : edges)
        word(out, down(e));
    return out + names;
}

struct Entry {
    uint64_t keyHash;
    bool compute;
    std::string blob;
};

// The PackReflections section of `entries` as raw/pack/packfile.py reflection_bytes lays it out (each blob its own here)
inline std::string section(std::vector<Entry> entries)
{
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.compute != b.compute ? !a.compute : a.keyHash < b.keyHash;
    });
    uint32_t render = 0;
    for (const Entry &e : entries)
        render += !e.compute;
    std::string out;
    word(out, render);
    word(out, uint32_t(entries.size()) - render);
    std::string blobs;
    const size_t at = 8 + 16 * entries.size();
    for (const Entry &e : entries) {
        blobs.append((8 - (at + blobs.size()) % 8) % 8, '\0');
        const uint64_t hash = e.keyHash;
        out.append(reinterpret_cast<const char *>(&hash), sizeof hash);
        word(out, uint32_t(at + blobs.size()));
        word(out, uint32_t(e.blob.size()));
        blobs += e.blob;
    }
    return out + blobs;
}

}  // namespace reflection_blob
