// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <deque>
#include "../SwitchNVDECAccess.h"
#include "../SwitchNVDECQueue.h"

TEST(SwitchNVDECQueue, ConfigurationsStayOwnedUntilTheirCompletion)
{
    SwitchNVDECQueue queue;
    ASSERT_TRUE(queue.rebase(40));
    for (uint32_t i = 0; i < queue.kSlots; ++i)
        ASSERT_TRUE(queue.publish(i));
    EXPECT_TRUE(queue.full());
    EXPECT_FALSE(queue.publish(9));
    EXPECT_FALSE(queue.retire(40));
    EXPECT_FALSE(queue.rebase(100));
    ASSERT_TRUE(queue.retire(41));
    ASSERT_TRUE(queue.publish(8));
    EXPECT_EQ(queue.jobs[0].sequence, 49u);
    while (queue.count)
        ASSERT_TRUE(queue.retire(static_cast<uint32_t>(queue.jobs[queue.head].sequence)));
    EXPECT_TRUE(queue.rebase(51));
    EXPECT_EQ(queue.completed, 51u);
    EXPECT_FALSE(queue.rebase(50));
}

TEST(SwitchNVDECQueue, LongLivedTokensRemainCompletedAcrossHardwareWrap)
{
    SwitchNVDECQueue queue;
    queue.submitted = queue.completed = 0xfffffffeu;
    ASSERT_TRUE(queue.publish(1));
    ASSERT_TRUE(queue.publish(2));
    EXPECT_EQ(queue.submitted, 0x100000000ULL);
    uint64_t sequence = 0;
    ASSERT_TRUE(queue.legacySequence(0xffffffffu, &sequence));
    EXPECT_EQ(sequence, 0xffffffffULL);
    ASSERT_TRUE(queue.legacySequence(0, &sequence));
    EXPECT_EQ(sequence, 0x100000000ULL);
    EXPECT_FALSE(queue.legacySequence(1, &sequence));
    EXPECT_FALSE(queue.retire(0xfffffffeu));
    ASSERT_TRUE(queue.retire(0xffffffffu));
    ASSERT_TRUE(queue.retire(0));
    ASSERT_TRUE(queue.rebase(2));
    EXPECT_EQ(queue.completed, 0x100000002ULL);
    EXPECT_LT(1u, queue.completed);
}

TEST(SwitchNVDECQueue, MixedAdmissionAndRetirementPreserveEveryLiveConfiguration)
{
    SwitchNVDECQueue queue;
    queue.submitted = queue.completed = 0xffffff00u;
    std::deque<uint64_t> expected;
    uint32_t random = 0xc3b56u;
    for (uint32_t i = 0; i < 100000; ++i) {
        random = random * 1664525u + 1013904223u;
        if ((random & 3) && !queue.full()) {
            const uint64_t next = queue.next();
            ASSERT_TRUE(queue.publish(i));
            expected.push_back(next);
        } else if (queue.count) {
            ASSERT_EQ(queue.jobs[queue.head].sequence, expected.front());
            ASSERT_TRUE(queue.retire(static_cast<uint32_t>(expected.front())));
            expected.pop_front();
        }
        ASSERT_EQ(queue.count, expected.size());
        for (uint32_t at = 0; at < queue.count; ++at)
            ASSERT_EQ(queue.jobs[(queue.head + at) % queue.kSlots].sequence, expected[at]);
    }
}

TEST(SwitchNVDECAccess, EveryCodecSeparatesCurrentPictureWritesFromReferenceReads)
{
    TegraNVDECSubmit submit = {};
    submit.entry_count = 1;
    submit.entries[0].kind = TEGRA_NVDEC_ENTRY_RELOC;
    const uint32_t codecs[] = {TEGRA_NVDEC_CODEC_H264, TEGRA_NVDEC_CODEC_HEVC, TEGRA_NVDEC_CODEC_VP9,
                               TEGRA_NVDEC_CODEC_MPEG4, TEGRA_NVDEC_CODEC_MPEG12};
    for (uint32_t codec : codecs) {
        submit.codec = codec;
        uint32_t slots = 4, current = 3;
        if (codec == TEGRA_NVDEC_CODEC_H264) {
            submit.setup_bytes = sizeof(nvdec_h264_pic_s);
            slots = 17;
        } else if (codec == TEGRA_NVDEC_CODEC_HEVC) {
            submit.setup_bytes = sizeof(nvdec_hevc_pic_s);
            slots = 16;
        } else if (codec == TEGRA_NVDEC_CODEC_MPEG4 || codec == TEGRA_NVDEC_CODEC_MPEG12) {
            submit.setup_bytes =
                codec == TEGRA_NVDEC_CODEC_MPEG4 ? sizeof(nvdec_mpeg4_pic_s) : sizeof(nvdec_mpeg2_pic_s);
            slots = 3;
            current = 0;
        } else {
            submit.setup_bytes = sizeof(nvdec_vp9_pic_s);
        }
        const uint32_t planes[] = {NVC5B0_SET_PICTURE_LUMA_OFFSET0, NVC5B0_SET_PICTURE_CHROMA_OFFSET0};
        for (uint32_t output = 0; output < slots; ++output) {
            if (codec == TEGRA_NVDEC_CODEC_H264)
                reinterpret_cast<nvdec_h264_pic_s *>(submit.setup)->CurrPicIdx = current = output;
            if (codec == TEGRA_NVDEC_CODEC_HEVC)
                reinterpret_cast<nvdec_hevc_pic_s *>(submit.setup)->curr_pic_idx = current = output;
            for (uint32_t plane : planes) {
                for (uint32_t picture = 0; picture < slots; ++picture) {
                    submit.entries[0].method = plane + 4 * picture;
                    EXPECT_EQ(switch_nvdec_entry_access(submit, 0),
                              picture == current ? SWITCH_NVDEC_WRITE : SWITCH_NVDEC_READ);
                }
            }
        }
    }
}

TEST(SwitchNVDECAccess, InputsStatusAndAuxiliaryHistoryHaveDifferentOwnership)
{
    TegraNVDECSubmit submit = {};
    submit.codec = TEGRA_NVDEC_CODEC_H264;
    submit.setup_bytes = sizeof(nvdec_h264_pic_s);
    submit.entry_count = 1;
    submit.entries[0].kind = TEGRA_NVDEC_ENTRY_RELOC;
    submit.entries[0].method = NVC5B0_SET_IN_BUF_BASE_OFFSET;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), SWITCH_NVDEC_READ);
    submit.entries[0].method = NVC5B0_SET_NVDEC_STATUS_OFFSET;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), SWITCH_NVDEC_WRITE);
    submit.entries[0].method = NVC5B0_SET_COLOC_DATA_OFFSET;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), SWITCH_NVDEC_READ | SWITCH_NVDEC_WRITE);
    submit.entries[0].method = NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), SWITCH_NVDEC_READ | SWITCH_NVDEC_WRITE);
    submit.entries[0].method = 0xffff;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), 0u);
    EXPECT_EQ(switch_nvdec_entry_access(submit, 1), 0u);
    --submit.setup_bytes;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), 0u);
    submit.entry_count = TEGRA_NVDEC_MAX_ENTRIES + 1;
    EXPECT_EQ(switch_nvdec_entry_access(submit, 0), 0u);
}
