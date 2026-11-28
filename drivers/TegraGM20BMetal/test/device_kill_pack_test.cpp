// SPDX-License-Identifier: MIT
// Every pre-built state of the built pack (//gpu/seed:pack, of the configured IPSW kind) pushes the zero-source kill the
// runtime's rule gives it, both ways: Device::build takes a pre-built state's kill from its segment's flag
// (kPackSegmentBlendKill) and counts each state whose rule says otherwise (Stats::blendKillPackDisagreements). An
// over-kill changes pixels; an under-kill costs the ROP work it saves. Each flag is its segment's own last
// SET_ITERATED_BLEND_OPTIMIZATION word, the GPU's, which no state's creation reads.
#include "device_harness.h"
#include "../pack.h"
#include "../blend_kill.h"
#include "../push.h"
#include "../pack_window.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> fileBytes(const char *path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    std::vector<uint8_t> bytes(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
    return bytes;
}

TEST(GM20BKillPack, EverySegmentsFlagIsItsOwnLastKillWord)
{
    const char *path = getenv("SWITCH_METAL_KILL_PACK");
    ASSERT_NE(path, nullptr);
    const std::vector<uint8_t> file = fileBytes(path);
    ASSERT_FALSE(file.empty()) << path;
    gm20b::PackHeader h = {};
    std::memcpy(&h, file.data(), sizeof h);
    const gm20b::PackSection segments = h.section[gm20b::PackSegments];
    ASSERT_GE(segments.bytes, sizeof(gm20b::PackSegmentTable));
    gm20b::PackSegmentTable table = {};
    std::memcpy(&table, file.data() + segments.offset, sizeof table);
    uint32_t flagged = 0, unflagged = 0;
    for (uint32_t i = 0; i < table.pipelineCount; i++) {
        gm20b::PackSegment segment = {};
        std::memcpy(&segment, file.data() + segments.offset + sizeof table + i * sizeof segment, sizeof segment);
        if (!segment.words)
            continue;
        const auto *words =
            reinterpret_cast<const uint32_t *>(file.data() + h.section[gm20b::PackHeap].offset + segment.heapOffset);
        const bool kills = gm20b::lastWritten(words, segment.words, gm20b::kIteratedBlendOptimization,
                                              gm20b::kNoopNever) == gm20b::kNoopSourceRgba0000;
        const bool flag = segment.flags & gm20b::kPackSegmentBlendKill;
        EXPECT_EQ(flag, kills) << "segment " << i;
        (flag ? flagged : unflagged)++;
    }
    EXPECT_GT(flagged, 0u);
    EXPECT_GT(unflagged, 0u);
}

TEST(GM20BKillPack, EveryPrebuiltStatesSegmentPushesItsRulesKill)
{
    const char *path = getenv("SWITCH_METAL_KILL_PACK");
    ASSERT_NE(path, nullptr);
    const std::vector<uint8_t> file = fileBytes(path);
    ASSERT_FALSE(file.empty()) << path;
    gm20b::PackHeader h = {};
    std::memcpy(&h, file.data(), sizeof h);
    kernel_model::model = {};
    kernel_model::model.vaEnd = uint64_t(1) << 39;
    kernel_model::model.packImage.enabled = true;
    kernel_model::model.packImage.status = TegraGM20BPackStatusReady;
    kernel_model::model.packImage.gpuVa = TGM_PACK_WINDOW_VA;
    kernel_model::model.packImage.tier0HeapBytes = h.tier0[gm20b::PackHeap];
    kernel_model::model.packImage.heapBytes = h.section[gm20b::PackHeap].bytes;
    kernel_model::model.packImage.stack = h.stack;
    kernel_model::model.packImage.hostData = file.data() + h.section[gm20b::PackHeap].offset;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(path, 0, err, gm20b::Device::Role::Client)) << err;
    const auto *records = dev.pack().section<gm20b::PackPipeline>(gm20b::PackPipelines);
    const uint64_t tier0 = h.tier0[gm20b::PackPipelines] / sizeof(gm20b::PackPipeline);
    size_t prebuilt = 0, killed = 0;
    for (uint64_t i = 0; i < tier0; i++) {
        const gm20b::Pipeline *pl = gm20b::DeviceHarness::placePacked(dev, records[i]);
        if (!pl || !pl->prebuiltWords)
            continue;
        prebuilt++;
        killed += pl->blendKill;
    }
    std::printf("%zu pre-built states, %zu of them killing zero-source pixels\n", prebuilt, killed);
    EXPECT_GT(killed, 0u) << "no segment of the pack kills: the rule is checked one way only";
    EXPECT_GT(prebuilt - killed, 0u) << "every segment of the pack kills: the rule is checked one way only";
    EXPECT_EQ(dev.stats.blendKillPackDisagreements.load(), 0u);
}

}  // namespace
