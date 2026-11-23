// SPDX-License-Identifier: MIT
// A fresh texture's first contents on gm20b::Device (fresh_fill.h fillFresh), off the copy engine's methods the device
// submitted: every level of a mip chain is filled, level 0 (the texture itself, which chainLevelImage does not name)
// included, each at its own address with the pattern down to the levels whose shorter side is 16 and zeros below.
// Boots 19, 21 and 22's gpu.texture.fresh read bgra8_chain level 0 wrong alone (4096, 640, 4096 of 4096 texels): its fill
// skipped level 0, which kept the pool memory it was given.
#include "device_harness.h"
#include "../fresh_fill.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <map>
#include <vector>

namespace {

constexpr uint32_t kBGRA8 = 80;

struct Fill {
    uint64_t va = 0;
    uint32_t constA = 0;
};

class DeviceFreshFill : public harness::DeviceTest {
protected:
    // The copies the job after `before` launched: each one's destination and the remap constant it wrote
    static std::vector<Fill> fills(size_t before)
    {
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        std::vector<switch_gm20b_method> m(words.size() * 2 + 1);
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        std::vector<Fill> out;
        uint64_t va = 0;
        uint32_t constA = 0;
        for (long i = 0; i < n; i++) {
            if (m[i].subchannel != gm20b::SUBC_COPY)
                continue;
            if (m[i].method == NVB0B5_OFFSET_OUT_UPPER)
                va = uint64_t(m[i].value) << 32 | (va & 0xffffffffu);
            else if (m[i].method == NVB0B5_OFFSET_OUT_LOWER)
                va = (va & ~0xffffffffull) | m[i].value;
            else if (m[i].method == NVB0B5_SET_REMAP_CONST_A)
                constA = m[i].value;
            else if (m[i].method == NVB0B5_LAUNCH_DMA &&
                     (m[i].value & 3) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE && (m[i].value & (1u << 10)))
                out.push_back({va, constA});
        }
        return out;
    }
};

TEST_F(DeviceFreshFill, EveryLevelOfAChainLevelZeroIncludedIsFilled)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMipTexture(64, 64, kBGRA8, 7, t));
    ASSERT_NE(t.chain, nullptr);
    ASSERT_EQ(t.chain->layout.levels, 7u);
    gm20b::FreshTexel texel;
    gm20b::CopyFill pattern, zero;
    ASSERT_TRUE(gm20b::freshPatternTexel(kBGRA8, texel));
    ASSERT_TRUE(gm20b::copyFillOf(texel.bytes, texel.size, pattern));
    const uint8_t zeros[16] = {};
    ASSERT_TRUE(gm20b::copyFillOf(zeros, 4, zero));
    ASSERT_NE(pattern.constA, zero.constA);
    const uint32_t patterned = gm20b::patternLevels(0x1 | 0x4, true, 64, 64, 7);
    ASSERT_EQ(patterned, 3u);

    const size_t before = kernel_model::model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::BlitEncoder *b = cb->blit();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(gm20b::fillFresh(*b, t, patterned, pattern, zero),
              uint64_t(64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 16 + 4 + 1) * 4);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));

    std::map<uint64_t, uint32_t> byLevel;
    for (const Fill &f : fills(before))
        byLevel[f.va] = f.constA;
    for (uint32_t level = 0; level < 7; level++) {
        const uint64_t va = t.chain->va + t.chain->layout.level[level].offset;
        const auto found = byLevel.find(va);
        ASSERT_NE(found, byLevel.end()) << "level " << level << " at 0x" << std::hex << va << " is not filled";
        EXPECT_EQ(found->second, level < patterned ? pattern.constA : zero.constA) << "level " << level;
    }
    EXPECT_EQ(t.va, t.chain->va + t.chain->layout.level[0].offset) << "level 0 is the texture itself";
}

}  // namespace
