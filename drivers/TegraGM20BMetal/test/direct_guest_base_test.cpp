// SPDX-License-Identifier: MIT
// A buffer slot whose Metal address a state's programs read (m2v's SlotGuestBase, Weather VFX's custom_mesh_vert reading
// the vertex buffer it binds with setVertexBytes): the record holds the push words each draw writes that address to, and
// those words are neither the compile's constants nor the record's push words, whatever the probe draws put there.
#include "../direct_heap.h"
#include "../direct_push.h"

#include <gtest/gtest.h>

namespace {

constexpr uint8_t kWord = 7;  // push words 6 and 7
constexpr uint8_t kVertexSlot5 = 5;

NmbDirectState guestBaseState()
{
    NmbDirectState state = {};
    for (NmbDirectDraw &draw : state.draws) {
        draw.pushMask = 0xffu;
        for (uint32_t i = 0; i < 8; i++)
            draw.push[i] = 0x100 + i;
        draw.guestBaseWord[0] = kWord;
        draw.guestBaseSlot[0] = kVertexSlot5;
    }
    return state;
}

TEST(GM20BDirectGuestBase, ItsWordsAreNoConstantWhenEveryProbeDrawAgrees)
{
    const NmbDirectState state = guestBaseState();
    EXPECT_EQ(gm20b::guestBasePushWords(state.draws[0]), 0xc0u);
    EXPECT_EQ(gm20b::constantPushWords(state), 0x3fu);
}

TEST(GM20BDirectGuestBase, TheRecordHoldsItsWordAndSlot)
{
    NmbDirectState state = guestBaseState();
    for (NmbDirectDraw &draw : state.draws) {
        draw.guestBaseWord[1] = 3;
        draw.guestBaseSlot[1] = gm20b::kGuestBaseFragment | 2;
    }
    gm20b::RecordHeap heap;
    ASSERT_EQ(gm20b::directHeap(state, heap), nullptr);
    EXPECT_EQ(heap.guestBaseWord[0], kWord);
    EXPECT_EQ(heap.guestBaseSlot[0], kVertexSlot5);
    EXPECT_EQ(heap.guestBaseWord[1], 3);
    EXPECT_EQ(heap.guestBaseSlot[1], gm20b::kGuestBaseFragment | 2);
    EXPECT_EQ(heap.guestBaseWord[2], 0);
}

TEST(GM20BDirectGuestBase, ProbeDrawsOfOtherGuestBasesAreRefused)
{
    NmbDirectState state = guestBaseState();
    state.draws[2].guestBaseSlot[0] = 6;
    gm20b::RecordHeap heap;
    EXPECT_NE(gm20b::directHeap(state, heap), nullptr);
}

TEST(GM20BDirectGuestBase, AStateOfNoneRecordsNone)
{
    NmbDirectState state = {};
    gm20b::RecordHeap heap;
    ASSERT_EQ(gm20b::directHeap(state, heap), nullptr);
    for (uint32_t k = 0; k < gm20b::kPackGuestBases; k++)
        EXPECT_EQ(heap.guestBaseWord[k], 0) << k;
}

}  // namespace
