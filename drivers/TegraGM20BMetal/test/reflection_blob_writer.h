// SPDX-License-Identifier: MIT
// A reflection blob (reflection_blob.h) of an NmbReflectionNode table, as the translator's
// Reflection::blob lays out a state's (translate/NativeMetalReflection.rs): each stage's bindings a range of the edges.
// `of` takes a table as m2v's compute artifact held it (Reflection::of: node 0 the root, its edges the bindings): no
// root in the blob, every other node one index down, the root's bindings stage 0's.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "../reflection_blob.h"
#include "NativeMetalComputeArtifact.h"

namespace reflection_blob {

inline void word(std::string &out, uint32_t w)
{
    out.append(reinterpret_cast<const char *>(&w), sizeof w);
}

// `nodes` (children and members before their parents) and `edges`, `stages` each stage's bindings
inline std::string blob(const std::vector<NmbReflectionNode> &nodes, const std::vector<uint32_t> &edges,
                        const std::vector<gm20b::ReflectionRange> &stages)
{
    std::string names;
    std::vector<uint32_t> nameAt;
    for (const NmbReflectionNode &n : nodes) {
        nameAt.push_back(n.name ? uint32_t(names.size()) + 1 : 0);
        if (n.name)
            names.append(n.name, strlen(n.name) + 1);
    }
    std::string out(gm20b::kReflectionMagic, sizeof gm20b::kReflectionMagic);
    for (const uint32_t w : {gm20b::kReflectionVersion, uint32_t(stages.size()), uint32_t(nodes.size()),
                             uint32_t(edges.size()), uint32_t(names.size())})
        word(out, w);
    for (const gm20b::ReflectionRange &s : stages) {
        word(out, s.first);
        word(out, s.count);
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        const NmbReflectionNode &n = nodes[i];
        for (const uint32_t w : {n.kind, n.flags, nameAt[i], n.first, n.count})
            word(out, w);
        for (const uint32_t child : n.children)
            word(out, child);
        word(out, 0);
        out.append(reinterpret_cast<const char *>(n.values), sizeof n.values);
    }
    for (const uint32_t e : edges)
        word(out, e);
    return out + names;
}

// `nodes` and `edges` as m2v's table; `stages`: 1 (compute) or 5 (render, the root's bindings the vertex stage's)
inline std::string of(const std::vector<NmbReflectionNode> &nodes, const std::vector<uint32_t> &edges, uint32_t stages)
{
    const auto down = [](uint32_t i) {
        return i == gm20b::kReflectionAbsent ? i : i - 1;
    };
    std::vector<NmbReflectionNode> rest(nodes.begin() + 1, nodes.end());
    for (NmbReflectionNode &n : rest)
        std::transform(std::begin(n.children), std::end(n.children), std::begin(n.children), down);
    std::vector<uint32_t> lowered(edges.size());
    std::transform(edges.begin(), edges.end(), lowered.begin(), down);
    std::vector<gm20b::ReflectionRange> ranges(stages, gm20b::ReflectionRange{0, 0});
    ranges[0] = {nodes[0].first, nodes[0].count};
    return blob(rest, lowered, ranges);
}

struct Entry {
    uint64_t keyHash;
    bool compute;
    std::string blob;
};

// The PackReflections section of `entries` as pack/packfile.py reflection_bytes lays it out (each blob its own here)
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
