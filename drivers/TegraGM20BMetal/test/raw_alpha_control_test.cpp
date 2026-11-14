// SPDX-License-Identifier: MIT
// raw/runtime/raw_alpha_control.h on the host: the register from the bound Metal state after the record's registers,
// written where it changes or the record's own registers set it; a state that owns it keeps its value; alpha to one turns
// off a zero-source pixel kill.
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "../raw/runtime/raw_alpha_control.h"

namespace {

struct Words {
    std::vector<std::pair<uint32_t, uint32_t>> m_;

    void m(uint32_t method, uint32_t value)
    {
        m_.emplace_back(method, value);
    }
};

using Writes = std::vector<std::pair<uint32_t, uint32_t>>;

TEST(AlphaControl, TheBoundStatesValueIsWrittenWhereItChanges)
{
    uint32_t held = raw::kAlphaControlUnknown;
    Words w;
    raw::writeAlphaControl(w, held, 0, true, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 0}}));
    w.m_.clear();
    raw::writeAlphaControl(w, held, 0, false, false, false, false);
    raw::writeAlphaControl(w, held, 0, true, false, false, false);
    EXPECT_TRUE(w.m_.empty());
    raw::writeAlphaControl(w, held, raw::alphaControl(true, false), false, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 1}}));
    EXPECT_EQ(held, 1u);
}

TEST(AlphaControl, ARecordThatNamesTheRegisterIsWrittenOverAndAnOwnerKeepsIt)
{
    uint32_t held = raw::kAlphaToCoverage;
    Words w;
    raw::writeAlphaControl(w, held, raw::kAlphaToCoverage, true, true, false, false);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 1}}));
    w.m_.clear();
    raw::writeAlphaControl(w, held, raw::kAlphaToCoverage, true, true, true, false);
    EXPECT_TRUE(w.m_.empty());
    EXPECT_EQ(held, raw::kAlphaControlUnknown);
    raw::writeAlphaControl(w, held, raw::kAlphaToCoverage, true, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 1}}));
}

TEST(AlphaControl, AlphaToOneTurnsOffAZeroSourceKill)
{
    uint32_t held = raw::kAlphaControlUnknown;
    const uint32_t toOne = raw::alphaControl(false, true);
    Words w;
    raw::writeAlphaControl(w, held, toOne, true, false, false, true);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 0x10}, {raw::kIteratedBlendOptimization, raw::kNoopNever}}));
    w.m_.clear();
    raw::writeAlphaControl(w, held, toOne, false, false, false, true);
    EXPECT_TRUE(w.m_.empty());
    raw::writeAlphaControl(w, held, toOne, true, false, false, true);
    EXPECT_EQ(w.m_, (Writes{{raw::kIteratedBlendOptimization, raw::kNoopNever}}));
    w.m_.clear();
    raw::writeAlphaControl(w, held, raw::kAlphaToCoverage, true, false, false, true);
    EXPECT_EQ(w.m_, (Writes{{raw::kAlphaControlMethod, 1}}));
}

}  // namespace
