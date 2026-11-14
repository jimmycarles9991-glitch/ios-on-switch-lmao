// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstdint>
#include <map>

#include "../SwitchWindowAperture.h"

namespace {
using namespace switch_window_aperture;

constexpr uint32_t kStateControl = 0x041;

// DC A's register file for the words the check touches: with `aliased`, window A's direct apertures are the
// WINDOW_HEADER copy's assembly registers while window A is selected (TRM 24.10-24.11); otherwise registers of their own.
struct Dc {
    bool aliased = true;
    std::map<uint32_t, uint32_t> words;
    unsigned stateControlWrites = 0;

    uint32_t &slot(uint32_t index)
    {
        if (aliased && words[kWindowHeader] == kWindowASelect && !(words[kStateAccess] & kMuxActive)) {
            for (const Word &w : kWords)
                if (index == w.aperture)
                    return words[w.header];
        }
        return words[index];
    }

    struct Ref {
        Dc &dc;
        uint32_t index;

        operator uint32_t() const
        {
            return dc.slot(index);
        }

        Ref &operator=(uint32_t value)
        {
            if (index == kStateControl)
                ++dc.stateControlWrites;
            dc.slot(index) = value;
            return *this;
        }
    };

    Ref operator[](uint32_t index)
    {
        return {*this, index};
    }
};

Dc loaded(bool aliased)
{
    Dc dc;
    dc.aliased = aliased;
    dc.words[kStateAccess] = 1u << 0;   // READ_MUX ACTIVE, as a gate read leaves it
    dc.words[kWindowHeader] = 1u << 5;  // window B selected
    dc.words[0x800] = 0xc0000000;
    dc.words[0x806] = 0x40;
    dc.words[0x80b] = 0x12;
    dc.words[0x70a] = 0x0b40;
    if (!aliased)
        for (const Word &w : kWords)
            dc.words[w.aperture] = dc.words[w.header] ^ 0x3;
    return dc;
}

TEST(SwitchWindowAperture, AliasedApertureReadsBackEveryWordBothWaysAndPutsItBack)
{
    Dc dc = loaded(true);
    const Dc before = dc;
    const Result result = check(dc);
    EXPECT_EQ(result.aliased, kAllWords);
    EXPECT_EQ(result.restored, kAllWords);
    EXPECT_EQ(dc.words, before.words) << "every word, STATE_ACCESS and WINDOW_HEADER back as they were";
    EXPECT_EQ(dc.stateControlWrites, 0u) << "no UPDATE or ACT_REQ: the test values never leave the assembly copy";
}

TEST(SwitchWindowAperture, SeparateRegistersAreNoAliasAndArePutBackToo)
{
    Dc dc = loaded(false);
    const Dc before = dc;
    const Result result = check(dc);
    EXPECT_EQ(result.aliased, 0u);
    EXPECT_EQ(result.restored, kAllWords);
    EXPECT_EQ(dc.words, before.words);
}

TEST(SwitchWindowAperture, TheFlippedBitsStayInsideEachWordsField)
{
    const uint32_t fields[] = {0xffffffffu, 0xffffffffu, 0x70u, 0xffffu};  // TRM 24.11.1, 24.11.4, 24.11.7, 24.10.10
    for (unsigned i = 0; i < kWordCount; ++i) {
        EXPECT_EQ(kWords[i].first & ~fields[i], 0u) << i;
        EXPECT_EQ(kWords[i].second & ~fields[i], 0u) << i;
        EXPECT_NE(kWords[i].first, kWords[i].second) << i;
    }
}
}  // namespace
