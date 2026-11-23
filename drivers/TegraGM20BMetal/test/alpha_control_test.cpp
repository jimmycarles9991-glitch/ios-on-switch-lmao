// SPDX-License-Identifier: MIT
// alpha_control.h on the host: the register from the bound Metal state after the record's registers,
// written where it changes or the record's own registers set it; a state that owns it keeps its value; alpha to one turns
// off a zero-source pixel kill.
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "../alpha_control.h"

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
    uint32_t held = gm20b::kAlphaControlUnknown;
    Words w;
    gm20b::writeAlphaControl(w, held, 0, true, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kAlphaControlMethod, 0}}));
    w.m_.clear();
    gm20b::writeAlphaControl(w, held, 0, false, false, false, false);
    gm20b::writeAlphaControl(w, held, 0, true, false, false, false);
    EXPECT_TRUE(w.m_.empty());
    gm20b::writeAlphaControl(w, held, gm20b::alphaControl(true, false), false, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kAlphaControlMethod, 1}}));
    EXPECT_EQ(held, 1u);
}

TEST(AlphaControl, ARecordThatNamesTheRegisterIsWrittenOverAndAnOwnerKeepsIt)
{
    uint32_t held = gm20b::kAlphaToCoverage;
    Words w;
    gm20b::writeAlphaControl(w, held, gm20b::kAlphaToCoverage, true, true, false, false);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kAlphaControlMethod, 1}}));
    w.m_.clear();
    gm20b::writeAlphaControl(w, held, gm20b::kAlphaToCoverage, true, true, true, false);
    EXPECT_TRUE(w.m_.empty());
    EXPECT_EQ(held, gm20b::kAlphaControlUnknown);
    gm20b::writeAlphaControl(w, held, gm20b::kAlphaToCoverage, true, false, false, false);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kAlphaControlMethod, 1}}));
}

TEST(AlphaControl, AlphaToOneTurnsOffAZeroSourceKill)
{
    uint32_t held = gm20b::kAlphaControlUnknown;
    const uint32_t toOne = gm20b::alphaControl(false, true);
    Words w;
    gm20b::writeAlphaControl(w, held, toOne, true, false, false, true);
    EXPECT_EQ(w.m_,
              (Writes{{gm20b::kAlphaControlMethod, 0x10}, {gm20b::kIteratedBlendOptimization, gm20b::kNoopNever}}));
    w.m_.clear();
    gm20b::writeAlphaControl(w, held, toOne, false, false, false, true);
    EXPECT_TRUE(w.m_.empty());
    gm20b::writeAlphaControl(w, held, toOne, true, false, false, true);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kIteratedBlendOptimization, gm20b::kNoopNever}}));
    w.m_.clear();
    gm20b::writeAlphaControl(w, held, gm20b::kAlphaToCoverage, true, false, false, true);
    EXPECT_EQ(w.m_, (Writes{{gm20b::kAlphaControlMethod, 1}}));
}

}  // namespace
