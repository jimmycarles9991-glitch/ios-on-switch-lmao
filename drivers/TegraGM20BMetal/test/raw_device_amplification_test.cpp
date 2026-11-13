// SPDX-License-Identifier: MIT
// raw_amplification.cpp on raw::Device: a state of vertex amplification drawn once per amplification by one amplify macro
// call over the scratch words the encoder writes, a count the state does not take refused, each amplification's view
// mapping offsets in the scratch maps and the state's offset words, and a dynamic vertex stride taken from the bind up to
// the stream's 12-bit field, past it by the state's stride twin alone.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

raw::RuntimeRecord record(uint64_t key, uint8_t amplificationMax, bool countRead, bool dynamicStride)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    r.pipeline.amplificationMax = amplificationMax;
    r.pipeline.amplificationIdWord = amplificationMax ? 3 : 0;
    r.pipeline.amplificationCountWord = countRead ? 4 : 0;
    r.pipeline.pushMask = (amplificationMax ? 1u << 2 : 0) | (countRead ? 1u << 3 : 0);
    if (dynamicStride) {
        r.pipeline.vertexSlot = 1;
        r.pipeline.vertexStrideDynamic = 1;
        r.pipeline.pulledStride[0][0] = 2;
        r.pipeline.pulledStride[0][1] = 5;
        r.pipeline.pushMask |= 1u << 4;
    } else {
        r.pipeline.vertexSlot = 0xff;
    }
    return r;
}

class DeviceAmplification : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer vertices;
    raw::Pipeline *amplified = nullptr, *counted = nullptr, *plain = nullptr, *strided = nullptr, *twinned = nullptr,
                  *offsetting = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, vertices));
        std::string why;
        amplified = dev.addRecord(record(0xa301, 2, false, false), false, why);
        ASSERT_NE(amplified, nullptr) << why;
        counted = dev.addRecord(record(0xa302, 2, true, false), false, why);
        ASSERT_NE(counted, nullptr) << why;
        plain = dev.addRecord(record(0xa303, 0, false, false), false, why);
        ASSERT_NE(plain, nullptr) << why;
        strided = dev.addRecord(record(0xa304, 0, false, true), false, why);
        ASSERT_NE(strided, nullptr) << why;
        raw::RuntimeRecord viewports = record(0xa307, 2, false, false);
        viewports.amplificationViewportWord = 6;
        viewports.pipeline.pushMask |= 1u << 5;
        offsetting = dev.addRecord(viewports, false, why);
        ASSERT_NE(offsetting, nullptr) << why;
        raw::RuntimeRecord withTwin = record(0xa305, 0, false, true);
        raw::RuntimeRecord twin = record(0xa306, 0, false, false);
        twin.pipeline.pulledStride[0][0] = 1;
        twin.pipeline.pulledStride[0][1] = 5;
        twin.pipeline.pushMask = 1u << 4;
        withTwin.strideTwin = std::make_shared<raw::RuntimeRecord>(twin);
        twinned = dev.addRecord(withTwin, false, why);
        ASSERT_NE(twinned, nullptr) << why;
    }

    struct Drawn {
        std::string why;
        uint32_t draws = 0;
        std::vector<uint32_t> words;  // what the device submitted for the draw's command buffer
    };

    // One draw of `pl` at amplification `count` under view mappings of viewport offsets `viewports`, the vertex buffer at
    // slot 1 bound at `stride`
    Drawn drawWith(raw::Pipeline *pl, uint32_t count, std::vector<uint32_t> viewports = {},
                   uint32_t stride = UINT32_MAX)
    {
        Drawn d;
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {"no command buffer", 0, {}};
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return {"no pass", 0, {}};
        }
        e->setPipeline(pl);
        e->setBuffer(1, 1, &vertices, 0);
        e->setVertexStride(1, stride);
        uint32_t offsets[raw::RenderEncoder::kAmplificationMaps] = {},
                 layers[raw::RenderEncoder::kAmplificationMaps] = {};
        std::copy_n(viewports.begin(), std::min<size_t>(viewports.size(), std::size(offsets)), offsets);
        e->setAmplification(count, offsets, layers);
        const char *why = nullptr;
        const size_t jobs = kernel_model::model.jobs.size();
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        e->end();
        d.draws = cb->draws;
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            d.why = why ? why : "not committed";
        }
        for (size_t j = jobs; j < kernel_model::model.jobs.size(); j++)
            d.words.insert(d.words.end(), kernel_model::model.jobs[j].words.begin(),
                           kernel_model::model.jobs[j].words.end());
        return d;
    }
};

TEST_F(DeviceAmplification, EachAmplificationIsADrawOfItsOwn)
{
    const Drawn two = drawWith(amplified, 2);
    EXPECT_EQ(two.why, "");
    EXPECT_EQ(two.draws, 2u);
    const Drawn one = drawWith(amplified, 1);
    EXPECT_EQ(one.why, "");
    EXPECT_EQ(one.draws, 1u);
}

// (method, value) of every method a pushed word stream writes (raw_push.h's encodings: 1 incrementing, 3 non-incrementing,
// 4 immediate, 5 the first word to the method and the rest to the next)
std::vector<std::pair<uint32_t, uint32_t>> methods(const std::vector<uint32_t> &words)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, method = (h & 0x1fff) << 2;
        if (op == 4) {
            out.emplace_back(method, count);
            continue;
        }
        for (uint32_t k = 0; k < count && i < words.size(); k++)
            out.emplace_back(op == 1 ? method + 4 * k : op == 5 && k ? method + 4 : method, words[i++]);
    }
    return out;
}

TEST_F(DeviceAmplification, AnAmplifiedDrawIsOneMacroCallOverTheScratchWords)
{
    const Drawn two = drawWith(amplified, 2);
    ASSERT_EQ(two.why, "");
    const uint32_t call = NVB197_CALL_MME_MACRO(SWITCH_GM20B_MACRO_AMPLIFY);
    const uint32_t scratch = NVB197_SET_MME_SHADOW_SCRATCH(0);
    std::vector<uint32_t> calls, data;
    std::map<uint32_t, uint32_t> words;
    for (const auto &[m, v] : methods(two.words)) {
        if (m == call)
            calls.push_back(v);
        else if (m == call + 4)
            data.push_back(v);
        else if (m >= scratch + 4 * SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD &&
                 m < scratch + 4 * SWITCH_GM20B_SCRATCH_AMP_END)
            words[(m - scratch) / 4] = v;
    }
    EXPECT_EQ(calls, std::vector<uint32_t>{3});  // its vertex count, then instances, first, the first instance
    EXPECT_EQ(data, (std::vector<uint32_t>{1, 0, 0}));
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_COUNT], 2u);
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_ID_WORD], 0x700u + 4 * 2);
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_BARRIER], 0u);
    for (const auto &[m, v] : methods(drawWith(amplified, 1).words))
        EXPECT_NE(m, call) << "one amplification draws as any draw";
}

// The amplify macro writes each amplification's id: no draw of a pass writes the id word from the CPU, the second of the
// same state and count none of the scratch words either
TEST_F(DeviceAmplification, TheIdWordIsTheMacrosAlone)
{
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassDesc pass;
    pass.color[0].texture = &target;
    pass.count = 1;
    raw::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    e->setPipeline(amplified);
    e->setBuffer(1, 1, &vertices, 0);
    const uint32_t none[raw::RenderEncoder::kAmplificationMaps] = {};
    e->setAmplification(2, none, none);
    const char *why = nullptr;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << why;
    ASSERT_TRUE(e->draw(kTriangle, 3, 3, 1, 0, &why)) << why;
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    std::vector<uint32_t> words;
    for (size_t j = jobs; j < kernel_model::model.jobs.size(); j++)
        words.insert(words.end(), kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end());
    const uint32_t call = NVB197_CALL_MME_MACRO(SWITCH_GM20B_MACRO_AMPLIFY);
    const uint32_t first = NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD);
    const uint32_t end = NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_END);
    uint32_t offset = 0, idWrites = 0, calls = 0, scratchAfterFirst = 0;
    for (const auto &[m, v] : methods(words)) {
        if (m == NVB197_LOAD_CONSTANT_BUFFER_OFFSET) {
            offset = v;
        } else if (m >= NVB197_LOAD_CONSTANT_BUFFER(0) && m < NVB197_LOAD_CONSTANT_BUFFER(16)) {
            idWrites += offset == 0x700 + 4 * 2;
            offset += 4;
        } else if (m == call) {
            calls++;
        } else if (m >= first && m < end) {
            scratchAfterFirst += calls > 0;
        }
    }
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(idWrites, 0u);
    EXPECT_EQ(scratchAfterFirst, 0u);
}

// A state reading [[amplification_count]] draws at any count up to its maximum: the plain draw's count word is 1, the
// amplify call's the count it writes (its scratch word names the word)
TEST_F(DeviceAmplification, ACountReadingStateTakesEachDrawsCount)
{
    const uint32_t root = 0x700 + 4 * 3;
    std::vector<uint32_t> one;
    uint32_t offset = 0;
    for (const auto &[m, v] : methods(drawWith(counted, 1).words)) {
        if (m == NVB197_LOAD_CONSTANT_BUFFER_OFFSET)
            offset = v;
        else if (m >= NVB197_LOAD_CONSTANT_BUFFER(0) && m < NVB197_LOAD_CONSTANT_BUFFER(16)) {
            if (offset == root)
                one.push_back(v);
            offset += 4;
        }
    }
    EXPECT_EQ(one, std::vector<uint32_t>{1});
    uint32_t countWord = 0;
    const uint32_t scratch = NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD);
    for (const auto &[m, v] : methods(drawWith(counted, 2).words))
        if (m == scratch)
            countWord = v;
    EXPECT_EQ(countWord, root);
}

// A mapping's offsets share one scratch word, 16 bits each
TEST_F(DeviceAmplification, AMappingOffsetPastHalfItsScratchWordIsRefused)
{
    EXPECT_NE(drawWith(offsetting, 2, {3, 0x10000}).why, "");
    EXPECT_EQ(drawWith(offsetting, 2, {3, 0xffff}).why, "");
}

TEST_F(DeviceAmplification, ACountTheStateDoesNotTakeIsRefused)
{
    EXPECT_NE(drawWith(amplified, 3).why, "");
    EXPECT_NE(drawWith(plain, 2).why, "");
    EXPECT_EQ(drawWith(plain, 1).why, "");
    EXPECT_EQ(drawWith(counted, 1).why, "");
    EXPECT_EQ(drawWith(counted, 2).why, "");
}

// View mappings that offset the viewport: the macro's map holds each amplification's offset and its viewport scratch word
// names the state's AmplificationViewportOffset word; one amplification draws its mapping's from the CPU
TEST_F(DeviceAmplification, EachAmplificationsViewportOffsetIsTheStatesWord)
{
    const uint32_t scratch = NVB197_SET_MME_SHADOW_SCRATCH(0);
    std::map<uint32_t, uint32_t> words;
    for (const auto &[m, v] : methods(drawWith(offsetting, 2, {3, 6}).words))
        if (m >= scratch + 4 * SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD && m < scratch + 4 * SWITCH_GM20B_SCRATCH_AMP_END)
            words[(m - scratch) / 4] = v;
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_VIEWPORT_WORD], 0x700u + 4 * 5);
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_MAPS] >> SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT, 3u);
    EXPECT_EQ(words[SWITCH_GM20B_SCRATCH_AMP_MAPS + 1] >> SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT, 6u);
    uint32_t offset = 0, word = ~0u;
    for (const auto &[m, v] : methods(drawWith(offsetting, 1, {4}).words)) {
        if (m == NVB197_LOAD_CONSTANT_BUFFER_OFFSET) {
            offset = v;
        } else if (m >= NVB197_LOAD_CONSTANT_BUFFER(0) && m < NVB197_LOAD_CONSTANT_BUFFER(16)) {
            if (offset == 0x700 + 4 * 5)
                word = v;
            offset += 4;
        }
    }
    EXPECT_EQ(word, 4u);
}

TEST_F(DeviceAmplification, ADynamicStrideIsTheBindsUpToTheStreamField)
{
    EXPECT_EQ(drawWith(strided, 1, {}, 16).why, "");
    EXPECT_EQ(drawWith(strided, 1, {}, 0xfff).why, "");
    EXPECT_EQ(drawWith(strided, 1).why, "");
    EXPECT_NE(drawWith(strided, 1, {}, 0x1000).why, "");
}

TEST_F(DeviceAmplification, AStream0BindStridePastTheFieldDrawsTheStrideTwin)
{
    ASSERT_NE(twinned->strideTwin, nullptr);
    EXPECT_EQ(twinned->strideTwin->rec->vertexSlot, 0xff);
    EXPECT_EQ(drawWith(twinned, 1, {}, 16).why, "");
    EXPECT_EQ(drawWith(twinned, 1, {}, 0x1000).why, "");
    EXPECT_EQ(drawWith(twinned, 1, {}, 0x10000).draws, 1u);
}

}  // namespace
