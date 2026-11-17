// SPDX-License-Identifier: MIT
// A render stage's texture slots on raw::Device are the identity's (raw_stage_textures.h): 24B119's A13 binds 128 a stage,
// 21H461's A10X 31. Each texture bound at a vertex or fragment slot lands as its own handle in the set word its entry
// names, slots 31, 64 and 127 on 24B119, in a set of kPackEntries or fewer entries and in a wider one; a rebind of the
// last slot alone uploads a set naming the new texture, and a draw of nothing moved keeps the set.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_stage_textures.h"
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

// The identity's last slots: past the A10X's 31 on 24B119, the A10X's own last on 21H461
std::vector<uint8_t> highSlots()
{
    static_assert(raw::kStageTextures == 31 || raw::kStageTextures == 128);
    return raw::kStageTextures == 128 ? std::vector<uint8_t>{31, 64, 127} : std::vector<uint8_t>{28, 29, 30};
}

using StageSlot = std::pair<uint8_t, uint8_t>;  // stage (1 vertex, 2 fragment), slot

// A record of one handle entry a set word at each of `slots`, sampled with no sampler, its entries past kPackEntries in
// its far block
raw::RuntimeRecord texturesRecord(uint64_t key, const std::vector<StageSlot> &slots)
{
    const uint32_t count = uint32_t(slots.size());
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 textures " + std::to_string(key);
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
    }
    raw::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.vertexSlot = 0xff;
    pp.setBytes = std::max<uint32_t>(32, (4 * count + 15) & ~15u);
    pp.memoryEntries = count >= 32 ? ~0u : (1u << count) - 1;
    for (raw::PackUniformRange &u : pp.uniform)
        u.entry = raw::kNoUniform;
    raw::setEntryCount(pp, count);
    r.far.assign(raw::farWords(count), 0);
    for (uint32_t k = 0; k < count; k++)
        raw::entryAt(pp, r.far,
                     k) = {uint16_t(4 * k), raw::EntryTexture, slots[k].first, slots[k].second, raw::SamplerNone, 0, 0};
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceWideTextures : public harness::DeviceTest {
protected:
    raw::Texture target;
    std::deque<raw::Texture> made;  // the textures bound, each at its own address
    std::map<StageSlot, const raw::Texture *> bound;
    raw::CommandBuffer *cb = nullptr;
    raw::RenderEncoder *e = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    raw::Pipeline *state(uint64_t key, const std::vector<StageSlot> &slots)
    {
        std::string why;
        raw::Pipeline *made = dev.addRecord(texturesRecord(key, slots), false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // A new texture of its own at `s`
    void bindNew(const StageSlot &s)
    {
        ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, made.emplace_back()));
        bound[s] = &made.back();
        e->setTexture(s.first, s.second, &made.back());
    }

    void begin(raw::Pipeline *pl)
    {
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        e->setPipeline(pl);
    }

    struct Drawn {
        uint64_t set = 0;             // where the draw's set lies
        std::vector<uint32_t> words;  // its handle words, one an entry
    };

    Drawn draw(uint32_t entries)
    {
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        Drawn out;
        out.set = raw::DeviceHarness::preparedSet(*e);
        out.words.assign(entries, 0);
        if (const uint8_t *set = raw::DeviceHarness::upload(dev, out.set, 4 * entries))
            memcpy(out.words.data(), set, 4 * entries);
        return out;
    }

    std::vector<uint32_t> handles(const std::vector<StageSlot> &slots)
    {
        std::vector<uint32_t> out;
        for (const StageSlot &s : slots)
            out.push_back(bound[s]->tic);
        return out;
    }

    void drawsEachHandle(const std::vector<StageSlot> &slots, const std::vector<StageSlot> &rebound);
};

std::vector<StageSlot> bothStages(const std::vector<uint8_t> &slots)
{
    std::vector<StageSlot> out;
    for (uint8_t stage : {1, 2})
        for (uint8_t slot : slots)
            out.push_back({stage, slot});
    return out;
}

// Each draw names every slot's own texture; a draw of nothing moved keeps the set, and each rebind of one of `rebound`
// uploads a set of its new texture
void DeviceWideTextures::drawsEachHandle(const std::vector<StageSlot> &slots, const std::vector<StageSlot> &rebound)
{
    for (const StageSlot &s : slots)
        bindNew(s);
    const uint32_t entries = uint32_t(slots.size());
    Drawn last = draw(entries);
    EXPECT_EQ(last.words, handles(slots));
    EXPECT_EQ(draw(entries).set, last.set) << "nothing moved: the set is kept";
    for (const StageSlot &s : rebound) {
        bindNew(s);
        const Drawn next = draw(entries);
        EXPECT_NE(next.set, last.set) << "stage " << int(s.first) << " slot " << int(s.second) << " moved";
        EXPECT_EQ(next.words, handles(slots)) << "stage " << int(s.first) << " slot " << int(s.second);
        last = next;
    }
    e->end();
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}

TEST_F(DeviceWideTextures, EachHighSlotsTextureIsItsOwnHandleInANarrowSet)
{
    const std::vector<StageSlot> slots = bothStages(highSlots());
    raw::Pipeline *pl = state(0x7e1, slots);
    ASSERT_NE(pl, nullptr);
    ASSERT_FALSE(pl->entries.wide());
    begin(pl);
    drawsEachHandle(slots, {slots.back()});
}

TEST_F(DeviceWideTextures, EachHighSlotsTextureIsItsOwnHandleInAWideSet)
{
    std::vector<uint8_t> perStage;
    for (uint8_t slot = 0; slot < 17; slot++)
        perStage.push_back(slot);
    for (uint8_t slot : highSlots())
        perStage.push_back(slot);
    const std::vector<StageSlot> slots = bothStages(perStage);
    raw::Pipeline *pl = state(0x7e2, slots);
    ASSERT_NE(pl, nullptr);
    ASSERT_TRUE(pl->entries.wide());
    begin(pl);
    drawsEachHandle(slots, {slots[slots.size() / 2 - 1], slots.back()});
}

}  // namespace
