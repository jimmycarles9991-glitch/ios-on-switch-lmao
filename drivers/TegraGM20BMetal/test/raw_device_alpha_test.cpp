// SPDX-License-Identifier: MIT
// SET_ANTI_ALIAS_ALPHA_CONTROL on raw::Device over the kernel model (raw_alpha_control.h): a draw of a state whose Metal
// descriptor asks alpha to coverage or alpha to one writes it after the record's registers, a later draw of the same
// record for a Metal state that asks neither writes it back to 0, and draws that ask nothing new write nothing more, in
// a command buffer or the next one. Boot 21's gpu.raw.alpha_to_coverage read full coverage at every alpha
// (coverage_off_alpha=448): the register was never written.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

raw::RuntimeRecord plainRecord(uint64_t key)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceAlpha : public harness::DeviceTest {
protected:
    raw::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    // One command buffer of one pass drawing `controls.size()` triangles of `pl`, draw i bound for a Metal state asking
    // controls[i]: the values its words write SET_ANTI_ALIAS_ALPHA_CONTROL, in order
    std::vector<uint32_t> written(raw::Pipeline *pl, const std::vector<uint32_t> &controls)
    {
        const size_t before = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = raw::ColorAttachment::Load;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        EXPECT_NE(e, nullptr);
        for (uint32_t control : controls) {
            e->setPipeline(pl);
            e->setAlphaControl(control);
            const char *why = nullptr;
            EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        }
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        std::vector<switch_gm20b_method> m(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        std::vector<uint32_t> out;
        for (long i = 0; i < n; i++)
            if (m[i].method == raw::kAlphaControlMethod)
                out.push_back(m[i].value);
        return out;
    }
};

TEST_F(DeviceAlpha, AMetalStatesAlphaToCoverageAndAlphaToOneReachTheRegister)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(plainRecord(0xa2c1), false, why);
    ASSERT_NE(pl, nullptr) << why;
    EXPECT_EQ(written(pl, {raw::kAlphaToCoverage}), (std::vector<uint32_t>{raw::kAlphaToCoverage}));
    EXPECT_EQ(written(pl, {raw::alphaControl(true, true)}), (std::vector<uint32_t>{0x11}));
    EXPECT_EQ(written(pl, {0}), (std::vector<uint32_t>{0}));
    EXPECT_EQ(written(pl, {0}), (std::vector<uint32_t>{}));
}

TEST_F(DeviceAlpha, OneRecordDrawnForStatesThatDifferInThemWritesEachChange)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(plainRecord(0xa2c2), false, why);
    ASSERT_NE(pl, nullptr) << why;
    EXPECT_EQ(written(pl, {raw::kAlphaToCoverage, raw::kAlphaToCoverage, 0, 0, raw::kAlphaToOne}),
              (std::vector<uint32_t>{raw::kAlphaToCoverage, 0, raw::kAlphaToOne}));
}

}  // namespace
