// SPDX-License-Identifier: MIT
// A set 0 of more than kPackEntries entries and past the encoder's 512 bytes (RenderEncoder::prepare's wide path,
// descriptors.h WideDescriptors), on gm20b::Device: each entry's descriptor names its own buffer in the set the draw
// uploads, a draw whose bindings moved uploads a set naming the new ones, one whose bindings did not keeps the set, a
// runtime record of more entries than its far block holds is not added, a pack record whose entries no draw can write
// is not placed, and the entry of a buffer slot bound to nothing names the zero page.
#include "device_harness.h"
#include "record_pack.h"
#include "../record.h"
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
gm20b::RuntimeRecord wideRecord(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 wide " + std::to_string(key);
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.flags = gm20b::PipelinePushMetadata;
    pp.vertexSlot = 0xff;
    pp.setBytes = 16 * kEntries;
    for (gm20b::PackUniformRange &u : pp.uniform)
        u.entry = gm20b::kNoUniform;
    gm20b::setEntryCount(pp, kEntries);
    r.far.assign(gm20b::farWords(kEntries), 0);
    for (uint32_t k = 0; k < kEntries; k++)
        gm20b::entryAt(pp, r.far,
                       k) = {uint16_t(16 * k), gm20b::EntryUbo, uint8_t(k % 2 ? 2 : 1), uint8_t(k / 2), 1, 0, 0};
    pp.memoryEntries = ~0u;
    r.far[gm20b::kFarEntryWords * gm20b::farEntries(kEntries) + gm20b::farMaskWords(kEntries)] = 0xFF;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceWideSet : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer buffer;
    gm20b::CommandBuffer *cb = nullptr;
    gm20b::RenderEncoder *e = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(kEntries * 256 + 256, buffer));
    }

    void begin(gm20b::Pipeline *pl)
    {
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc pass;
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
        const uint8_t *words = gm20b::DeviceHarness::upload(dev, va, 16 * kEntries);
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
    gm20b::Pipeline *pl = dev.addRecord(wideRecord(0x3a1), false, why);
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
    gm20b::RuntimeRecord narrow = wideRecord(0x3a4);
    gm20b::setEntryCount(narrow.pipeline, 2);
    narrow.pipeline.setBytes = 32;
    narrow.far.clear();
    std::string why;
    gm20b::Pipeline *wide = dev.addRecord(wideRecord(0x3a5), false, why);
    ASSERT_NE(wide, nullptr) << why;
    gm20b::Pipeline *other = dev.addRecord(narrow, false, why);
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

TEST_F(DeviceWideSet, ARuntimeRecordWhoseFarBlockIsNotItsEntriesIsNotAdded)
{
    gm20b::RuntimeRecord r = wideRecord(0x3a3);
    r.far.pop_back();
    std::string why;
    EXPECT_EQ(dev.addRecord(r, false, why), nullptr);
    EXPECT_EQ(why, "a record whose far entries are not its entries'");
}

// Two buffer entries at set-0 bytes 0 and 16 of a 32-byte set
gm20b::RuntimeRecord narrowRecord(uint64_t key)
{
    gm20b::RuntimeRecord r = wideRecord(key);
    gm20b::setEntryCount(r.pipeline, 2);
    r.pipeline.setBytes = 32;
    r.far.clear();
    return r;
}

// A buffer slot bound to nothing reads the zero page with range 0 (no buffer load is bounds-checked, nak_target.h),
// where a bound slot's descriptor names its buffer
TEST_F(DeviceWideSet, AnEntryOfASlotBoundToNothingNamesTheZeroPage)
{
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(narrowRecord(0x3a6), false, why);
    ASSERT_NE(pl, nullptr) << why;
    begin(pl);
    e->setBuffer(1, 0, &buffer, 256);
    EXPECT_EQ(draw(), "");
    uint32_t words[8] = {};
    if (const uint8_t *set = gm20b::DeviceHarness::upload(dev, gm20b::DeviceHarness::preparedSet(*e), sizeof words))
        memcpy(words, set, sizeof words);
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(uint64_t(words[1]) << 32 | words[0], buffer.va + 256);
    EXPECT_NE(words[2], 0u);
    EXPECT_EQ(uint64_t(words[5]) << 32 | words[4], gm20b::DeviceHarness::zeroPage(dev).va);
    EXPECT_EQ(words[6], 0u);
}

// A pack is read where its records' draws index by their entries: a record of an entry no draw can write is refused
// where it is placed, and its state is no state of the device's
TEST(DevicePackEntries, ARecordOfAnEntryNoDrawCanWriteIsNotPlaced)
{
    gm20b::RuntimeRecord slot = narrowRecord(0x3c1), kind = narrowRecord(0x3c2), stage = narrowRecord(0x3c3),
                         offset = narrowRecord(0x3c4);
    slot.pipeline.entry[1].slot = gm20b::kStageBufferArguments;
    kind.pipeline.entry[1].kind = uint8_t(gm20b::EntryTessBuffer + 1);
    stage.pipeline.entry[1].stage = 3;
    offset.pipeline.entry[1].offset = 256;
    const std::vector<gm20b::RuntimeRecord> records = {narrowRecord(0x3c0), slot, kind, stage, offset};
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::recordPack(records, "pack-entries.bin").c_str(), 0, err, gm20b::Device::Role::Client))
        << err;
    EXPECT_NE(dev.pipeline(0x3c0), nullptr);
    for (const gm20b::RuntimeRecord *bad : {&slot, &kind, &stage, &offset})
        EXPECT_EQ(dev.pipeline(bad->pipeline.keyHash), nullptr) << std::hex << bad->pipeline.keyHash;
}

}  // namespace
