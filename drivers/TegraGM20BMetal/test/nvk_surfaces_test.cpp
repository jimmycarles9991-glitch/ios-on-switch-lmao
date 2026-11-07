// SPDX-License-Identifier: MIT
// nvkmd_darwin's ExecSurfaces record and submit hint (third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_surfaces.h)
// on the host: what NVK's submissions name to the kernel. Each BO once, its accesses merged; past the record's room, the
// all-imports flag (no GPU waits, every import a writer for CPU locks); a length that ends after the uses. The hint is the
// next vkQueueSubmit's on its queue alone, and its copy reaches a submission Mesa's submit thread runs.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <thread>

#include "../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_surfaces.h"

namespace {

TEST(NvkSurfaces, EachBoOnceWrittenIfAnyUseWritesIt)
{
    TegraGM20BAbiExecSurfaces s = {};
    darwin_surfaces_clear(&s);
    darwin_surfaces_add(&s, 7, TegraGM20BSurfaceRead);
    darwin_surfaces_add(&s, 9, TegraGM20BSurfaceRead);
    darwin_surfaces_add(&s, 7, TegraGM20BSurfaceWrite);
    ASSERT_EQ(s.count, 2u);
    EXPECT_EQ(s.flags, 0u);
    EXPECT_EQ(s.uses[0].handle, 7u);
    EXPECT_EQ(s.uses[0].access, uint32_t(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    EXPECT_EQ(s.uses[1].handle, 9u);
    EXPECT_EQ(s.uses[1].access, uint32_t(TegraGM20BSurfaceRead));
    EXPECT_EQ(darwin_surfaces_bytes(&s), offsetof(TegraGM20BAbiExecSurfaces, uses) + 2 * sizeof(s.uses[0]));
}

TEST(NvkSurfaces, AnAccessItDoesNotKnowCountsAsBoth)
{
    TegraGM20BAbiExecSurfaces s = {};
    darwin_surfaces_clear(&s);
    darwin_surfaces_add(&s, 3, 0);
    darwin_surfaces_add(&s, 4, 0x10);
    for (uint32_t i = 0; i < 2; i++)
        EXPECT_EQ(s.uses[i].access, uint32_t(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
}

TEST(NvkSurfaces, MoreThanTheRecordHoldsTouchesEveryImport)
{
    TegraGM20BAbiExecSurfaces s = {};
    darwin_surfaces_clear(&s);
    for (uint64_t h = 1; h <= TegraGM20BMaxExecSurfaces; h++)
        darwin_surfaces_add(&s, h, TegraGM20BSurfaceRead);
    EXPECT_EQ(s.flags, 0u);
    darwin_surfaces_add(&s, 1, TegraGM20BSurfaceWrite);  // one it has: merged, still room for nothing more
    EXPECT_EQ(s.flags, 0u);
    darwin_surfaces_add(&s, 1000, TegraGM20BSurfaceRead);
    EXPECT_EQ(s.count, uint32_t(TegraGM20BMaxExecSurfaces));
    EXPECT_EQ(s.flags, uint32_t(TegraGM20BExecSurfacesAll));
    EXPECT_EQ(darwin_surfaces_bytes(&s), sizeof(TegraGM20BAbiExecSurfaces));
    darwin_surfaces_clear(&s);
    EXPECT_EQ(s.count, 0u);
    EXPECT_EQ(s.flags, 0u);
    EXPECT_EQ(darwin_surfaces_bytes(&s), offsetof(TegraGM20BAbiExecSurfaces, uses));
}

// Thread-local as nvkmd_darwin.c keeps them: what switch_nvk_submit_surfaces made, and what the call took.
thread_local darwin_submit_hint tHint, tTaken;

const int kQueue = 1, kOther = 2;  // two queues: only their addresses matter

TEST(NvkSurfaces, TheCallTakesTheHintAndItsSubmissionCarriesItToTheSubmitThread)
{
    darwin_hint_begin(&tHint, &kQueue);
    darwin_surfaces_add(&tHint.list, 11, TegraGM20BSurfaceWrite);
    darwin_surfaces_add(&tHint.list, 12, TegraGM20BSurfaceRead);
    darwin_hint_take(&tHint, &tTaken, &kQueue);
    EXPECT_EQ(tHint.queue, nullptr);
    TegraGM20BAbiExecSurfaces *data = darwin_hint_copy(&tTaken, &kQueue);  // the submission's driver_data
    ASSERT_NE(data, nullptr);
    tTaken.queue = nullptr;  // the call returned
    TegraGM20BAbiExecSurfaces seen = {};
    bool threadHasHint = true;
    std::thread submitThread([&] {
        threadHasHint = darwin_hint_copy(&tTaken, &kQueue) != nullptr;
        seen = *data;
    });
    submitThread.join();
    free(data);
    EXPECT_FALSE(threadHasHint);
    ASSERT_EQ(seen.count, 2u);
    EXPECT_EQ(seen.flags, 0u);
    EXPECT_EQ(seen.uses[0].handle, 11u);
    EXPECT_EQ(seen.uses[0].access, uint32_t(TegraGM20BSurfaceWrite));
    EXPECT_EQ(seen.uses[1].handle, 12u);
    EXPECT_EQ(seen.uses[1].access, uint32_t(TegraGM20BSurfaceRead));
}

TEST(NvkSurfaces, AHintIsTheNextCallsOnItsQueueOnly)
{
    darwin_hint_begin(&tHint, &kQueue);
    darwin_surfaces_add(&tHint.list, 21, TegraGM20BSurfaceWrite);
    darwin_hint_take(&tHint, &tTaken, &kOther);  // a call on another queue drops it
    EXPECT_EQ(darwin_hint_copy(&tTaken, &kOther), nullptr);
    darwin_hint_take(&tHint, &tTaken, &kQueue);
    EXPECT_EQ(darwin_hint_copy(&tTaken, &kQueue), nullptr);
    darwin_hint_begin(&tHint, &kQueue);  // an empty list is a list: the submission touches no import
    darwin_hint_take(&tHint, &tTaken, &kQueue);
    TegraGM20BAbiExecSurfaces *data = darwin_hint_copy(&tTaken, &kQueue);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->count, 0u);
    free(data);
    darwin_hint_take(&tHint, &tTaken, &kQueue);  // the next call made none
    EXPECT_EQ(darwin_hint_copy(&tTaken, &kQueue), nullptr);
}

}  // namespace
