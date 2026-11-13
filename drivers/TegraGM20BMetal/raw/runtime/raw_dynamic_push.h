// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include <cstdio>
#include <cstring>

namespace raw {

inline uint32_t pushFieldMask(uint32_t word, uint32_t count)
{
    return word && word <= 33 - count ? ((1u << count) - 1) << (word - 1) : 0;
}

// A dynamic push field of a record: its word + 1 (0: absent) and its words
struct DynamicField {
    uint32_t word, count;
};

// Whether every present field lies whole in the root push words `pushMask` names or in the fragment stage's table of
// `tableWords` words (raw_spill_table.h), no two overlapping
template <size_t N> bool dynamicFieldsPlaced(const DynamicField (&fields)[N], uint32_t pushMask, uint32_t tableWords)
{
    for (size_t i = 0; i < N; i++) {
        const DynamicField &f = fields[i];
        if (!f.word)
            continue;
        const uint64_t first = uint64_t(f.word) - 1, end = first + f.count;
        const bool root = end <= 32 && (pushFieldMask(f.word, f.count) & pushMask) == pushFieldMask(f.word, f.count);
        if (!root && !(first >= 32 && end <= 32 + uint64_t(tableWords)))
            return false;
        for (size_t j = 0; j < i; j++)
            if (fields[j].word && first < uint64_t(fields[j].word) - 1 + fields[j].count && fields[j].word - 1 < end)
                return false;
    }
    return true;
}

inline bool validDynamicPush(uint32_t blend, uint32_t half, uint32_t pushMask, uint32_t tableWords = 0)
{
    const DynamicField fields[] = {{blend, 8}, {half, 4}};
    return dynamicFieldsPlaced(fields, pushMask, tableWords);
}

// Where field `word` + 1 of `count` words lies in the words [first, first + words) at `at`; nullptr: outside them
inline uint32_t *fieldWords(uint32_t *at, uint32_t first, uint32_t words, uint32_t word, uint32_t count)
{
    return word && word - 1 >= first && uint64_t(word) - 1 + count <= uint64_t(first) + words ? at + (word - 1 - first)
                                                                                              : nullptr;
}

inline bool keyUsesBlendColor(const char *key)
{
    for (const char *at = key; (at = strstr(at, " c")); at += 2) {
        unsigned blend = 0, factors[4] = {};
        const int n =
            sscanf(at, " c%*u=%*u:%*u:%u:%u:%u:%*u:%u:%u", &blend, &factors[0], &factors[1], &factors[2], &factors[3]);
        for (int k = 0; blend && k + 1 < n; k++)
            if (factors[k] >= 11 && factors[k] <= 14)
                return true;
    }
    return false;
}

inline bool usableDynamicPush(const PackPipeline &p, const char *key)
{
    const auto &tests = p.shaderTests;
    const DynamicField fields[] = {
        {tests.depthWord, 4}, {tests.stencilWord, 4}, {p.blendColorWord, 8}, {p.halfBlendColorWord, 4}};
    if (tests.reserved || tests.reservedTail || (tests.flags & ~3u) || bool(tests.flags & 1) != bool(tests.depthWord) ||
        bool(tests.flags & 2) != bool(tests.stencilWord) ||
        !dynamicFieldsPlaced(fields, p.pushMask, p.spillEntry[1] ? p.spillWords[1] : 0))
        return false;
    if (!(p.flags & PipelinePushMetadata))
        return !p.blendColorWord && !p.halfBlendColorWord && !tests.flags && !keyUsesBlendColor(key);
    return true;
}

// The blend colour's fields among the words [first, first + words) of the record's word space at `at`: the root's push
// words from 0, the fragment stage's table from 32
inline void patchBlendPush(const PackPipeline &p, const uint32_t color[4], const uint32_t extended[8], uint32_t *at,
                           uint32_t first = 0, uint32_t words = 32)
{
    if (uint32_t *w = fieldWords(at, first, words, p.blendColorWord, 8))
        memcpy(w, extended, 8 * sizeof(uint32_t));
    if (uint32_t *w = fieldWords(at, first, words, p.halfBlendColorWord, 4))
        memcpy(w, color, 4 * sizeof(uint32_t));
}

}  // namespace raw
