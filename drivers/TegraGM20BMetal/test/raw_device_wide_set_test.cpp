// SPDX-License-Identifier: MIT
// A set 0 of more than kPackEntries entries and past the encoder's 512 bytes (RenderEncoder::prepare's wide path,
// raw_descriptors.h WideDescriptors), on raw::Device: each entry's descriptor names its own buffer in the set the draw
// uploads, a draw whose bindings moved uploads a set naming the new ones, one whose bindings did not keeps the set, and a
// record of more entries than its far block holds is refused as the driver's error.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kRootSetWord = 0x38, kEntries = 40, kSlots = kEntries / 2;

struct Method {
    uint32_t method, value;
};

std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (op == 4) {
            if (subc == 0)
                out.push_back({method, count});
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            continue;
        for (uint32_t k = 0; k < count && i < words.size(); k++, i++)
            if (subc == 0)
                out.push_back({method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]});
    }
    return out;
}

// Each set-0 address the submitted root tables took, in order
std::vector<uint64_t> setAddresses()
{
    std::vector<uint64_t> out;
    uint32_t at = 0, low = 0;
    for (const kernel_model::Job &job : kernel_model::model.jobs)
        for (const Method &x : methods(job.words))
            if (x.method == NVB197_LOAD_CONSTANT_BUFFER_OFFSET) {
                at = x.value;
            } else if (x.method >= NVB197_LOAD_CONSTANT_BUFFER(0) && x.method <= NVB197_LOAD_CONSTANT_BUFFER(15)) {
                if (at == kRootSetWord)
                    low = x.value;
                else if (at == kRootSetWord + 4)
                    out.push_back(uint64_t(x.value) << 32 | low);
                at += 4;
            }
    return out;
}

// Entry k: a storage buffer of stage 1 (k even) or 2 at slot k / 2, its descriptor at set-0 byte 16k
raw::RuntimeRecord wideRecord(uint64_t key)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 wide " + std::to_string(key);
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
    }
    raw::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.flags = raw::PipelinePushMetadata;
    pp.vertexSlot = 0xff;
    pp.setBytes = 16 * kEntries;
    for (raw::PackUniformRange &u : pp.uniform)
        u.entry = raw::kNoUniform;
    raw::setEntryCount(pp, kEntries);
    r.far.assign(raw::farWords(kEntries), 0);
    for (uint32_t k = 0; k < kEntries; k++)
        raw::entryAt(pp, r.far, k) = {uint16_t(16 * k), raw::EntryUbo, uint8_t(k % 2 ? 2 : 1), uint8_t(k / 2), 1, 0, 0};
    pp.memoryEntries = ~0u;
    r.far[raw::kFarEntryWords * raw::farEntries(kEntries) + raw::farMaskWords(kEntries)] = 0xFF;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceWideSet : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer buffer;
    raw::CommandBuffer *cb = nullptr;
    raw::RenderEncoder *e = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(kEntries * 256 + 256, buffer));
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

    std::string draw()
    {
        const char *why = nullptr;
        return e->draw(kTriangle, 0, 3, 1, 0, &why) ? "" : why ? why : "refused";
    }

    // The buffer address each entry's descriptor in the set at `va` names
    std::vector<uint64_t> named(uint64_t va)
    {
        const uint8_t *words = raw::DeviceHarness::upload(dev, va, 16 * kEntries);
        std::vector<uint64_t> out;
        for (uint32_t k = 0; words && k < kEntries; k++) {
            uint64_t address = 0;
            memcpy(&address, words + 16 * k, 8);
            out.push_back(address);
        }
        return out;
    }
};

TEST_F(DeviceWideSet, EachEntryNamesItsOwnBufferAndOnlyAMovedBindingUploadsTheSetAgain)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(wideRecord(0x3a1), false, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(pl->entries.wide());
    begin(pl);
    for (uint32_t s = 0; s < kSlots; s++) {
        e->setBuffer(1, s, &buffer, 256 * (2 * s));
        e->setBuffer(2, s, &buffer, 256 * (2 * s + 1));
    }
    EXPECT_EQ(draw(), "");
    EXPECT_EQ(draw(), "") << "nothing moved";
    e->setBuffer(2, kSlots - 1, &buffer, 256 * kEntries);
    EXPECT_EQ(draw(), "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint64_t> sets = setAddresses();
    ASSERT_EQ(sets.size(), 2u) << "the second draw kept the first's set";
    std::vector<uint64_t> want;
    for (uint32_t k = 0; k < kEntries; k++)
        want.push_back(buffer.va + 256 * k);
    EXPECT_EQ(named(sets[0]), want);
    want[kEntries - 1] = buffer.va + 256 * kEntries;
    EXPECT_EQ(named(sets[1]), want);
}

// A binding that moved before a narrow state's draw is still in the wide state's next set: the narrow draw took the moves
TEST_F(DeviceWideSet, ABindingMovedBeforeANarrowStatesDrawIsInTheWideStatesNextSet)
{
    raw::RuntimeRecord narrow = wideRecord(0x3a4);
    raw::setEntryCount(narrow.pipeline, 2);
    narrow.pipeline.setBytes = 32;
    narrow.far.clear();
    std::string why;
    raw::Pipeline *wide = dev.addRecord(wideRecord(0x3a5), false, why);
    ASSERT_NE(wide, nullptr) << why;
    raw::Pipeline *other = dev.addRecord(narrow, false, why);
    ASSERT_NE(other, nullptr) << why;
    ASSERT_FALSE(other->entries.wide());
    begin(wide);
    for (uint32_t s = 0; s < kSlots; s++) {
        e->setBuffer(1, s, &buffer, 256 * (2 * s));
        e->setBuffer(2, s, &buffer, 256 * (2 * s + 1));
    }
    EXPECT_EQ(draw(), "");
    e->setBuffer(2, kSlots - 1, &buffer, 256 * kEntries);
    e->setPipeline(other);
    EXPECT_EQ(draw(), "");
    e->setPipeline(wide);
    EXPECT_EQ(draw(), "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint64_t> sets = setAddresses();
    ASSERT_EQ(sets.size(), 3u);
    EXPECT_EQ(named(sets[2])[kEntries - 1], buffer.va + 256 * kEntries);
}

TEST_F(DeviceWideSet, ARecordOfMoreEntriesThanItsFarBlockHoldsIsTheDriversError)
{
    raw::RuntimeRecord r = wideRecord(0x3a2);
    std::string why;
    raw::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    pl->entries = raw::SetEntries(*pl->rec, nullptr);
    begin(pl);
    EXPECT_EQ(draw(), "a record of more set-0 entries than its far block holds");
    EXPECT_EQ(cb->refusal.id, raw::RefSetBytes);
    EXPECT_EQ(cb->refusal.a, kEntries);
    e->end();
    cb->abandon();
}

TEST_F(DeviceWideSet, ARuntimeRecordWhoseFarBlockIsNotItsEntriesIsNotAdded)
{
    raw::RuntimeRecord r = wideRecord(0x3a3);
    r.far.pop_back();
    std::string why;
    EXPECT_EQ(dev.addRecord(r, false, why), nullptr);
    EXPECT_EQ(why, "a record whose far entries are not its entries'");
}

}  // namespace
