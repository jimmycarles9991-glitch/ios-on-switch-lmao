// SPDX-License-Identifier: MIT
// What a record's making guarantees, checked again at its draw on gm20b::Device: a program reading a uniform buffer no
// entry names, argument tables the record has no words of, a vertex program choosing among
// viewports with no viewport count word, a vertex stream of no Metal buffer. assembleRecord and the bridge make no such
// record; a record that is one anyway is refused as its own kind (draw_refusal.h), which fails the command buffer
// with MTLCommandBufferErrorInternal (TegraGM20BMetalDrawErrors.mm) and never reaches m2v. And the indirect draws Metal's
// validation stops, refused as their own kinds too: arguments off 4 bytes or past their buffer, an index buffer offset off
// its index size, commands past the indirect command buffer (the recording takes each first: enc.idxrange, gm20b:icb.range).
// A turned draw of a state whose record holds no turned fragment program is its own kind too.
#include "device_harness.h"
#include "../gm20b_cache.h"
#include "../record.h"
#include <array>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

gm20b::RuntimeRecord plainRecord(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.pipeline.vertexSlot = 0xff;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceRecordGuards : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer buffer;
    gm20b::Refusal refused, skipped;
    uint32_t skips = 0;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
    }

    // One draw of `r` under `viewports` viewports (`call`: the draw call itself): "" drawn, else why not (its refusal in
    // `refused`)
    std::string draw(const gm20b::RuntimeRecord &r, uint32_t viewports = 1,
                     const std::function<bool(gm20b::RenderEncoder &, const char **)> &call = {})
    {
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        if (!pl)
            return "no state: " + why;
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        double views[gm20b::kMaxViewports][6] = {};
        for (uint32_t j = 0; j < viewports; j++) {
            views[j][2] = views[j][3] = 64;
            views[j][5] = 1;
        }
        if (viewports > 1)
            e->setViewports(views, viewports);
        const char *refusal = nullptr;
        const bool ok = call ? call(*e, &refusal) : e->draw(kTriangle, 0, 3, 1, 0, &refusal);
        refused = cb->refusal;
        skipped = cb->skipped;
        skips = cb->skips;
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return refusal ? refusal : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceRecordGuards, AProgramReadingAUniformBufferNoEntryNamesIsTheDriversError)
{
    gm20b::RuntimeRecord r = plainRecord(0x96a1);
    r.programs[1].cbufTypes = {gm20b::CbufRoot, uint8_t(gm20b::CbufUbo | (32 / 16) << 3)};
    EXPECT_EQ(draw(r), "a program reads a set-0 uniform buffer its record does not name");
    EXPECT_EQ(refused.id, gm20b::RefUnnamedUniform);
}

TEST_F(DeviceRecordGuards, ARecordWithoutItsArgumentTableWordsIsTheDriversError)
{
    gm20b::RuntimeRecord r = plainRecord(0x96a2);
    r.pipeline.flags = gm20b::PipelinePushMetadata;
    gm20b::setEntryCount(r.pipeline, 1);
    r.pipeline.entry[0] = {0, gm20b::EntrySpillTable, 2, 0, 0, 0, 0};
    r.pipeline.setBytes = 16;
    r.pipeline.spillEntry[1] = 1;
    r.pipeline.spillWords[1] = 4;
    EXPECT_EQ(draw(r), "an argument table whose words its record does not hold");
    EXPECT_EQ(refused.id, gm20b::RefSpillTable);
    EXPECT_EQ(refused.a, 2u);
}

TEST_F(DeviceRecordGuards, ASetPast512BytesDraws)
{
    gm20b::RuntimeRecord r = plainRecord(0x96a3);
    r.pipeline.setBytes = 600;
    EXPECT_EQ(draw(r), "");
}

// The vertex program's header writes ViewportIndex (OMAP_SYSTEM_VALUES_A bit 0x068 / 4, viewports.h)
TEST_F(DeviceRecordGuards, AViewportChoosingProgramOfNoCountWordIsTheDriversError)
{
    gm20b::RuntimeRecord r = plainRecord(0x96a4);
    constexpr uint32_t bit = 400 + 0x068 / 4;
    r.programs[0].image[bit / 8] |= uint8_t(1u << bit % 8);
    EXPECT_EQ(draw(r, 1), "");
    EXPECT_EQ(draw(r, 2), "a vertex function choosing among several viewports: its record draws viewport 0");
    EXPECT_EQ(refused.id, gm20b::RefViewports);
    EXPECT_EQ(refused.a, 2u);
    r.pipeline.keyHash = 0x96a5;
    r.pipeline.viewportCountWord = 3;
    r.pipeline.pushMask = 1u << 2;
    EXPECT_EQ(draw(r, 2), "");
}

TEST_F(DeviceRecordGuards, AVertexStreamOfNoMetalBufferIsTheDriversError)
{
    gm20b::RuntimeRecord r = plainRecord(0x96a6);
    r.pipeline.vertexStreams[0] = 32;
    EXPECT_EQ(draw(r), "a vertex stream of no Metal buffer");
    EXPECT_EQ(refused.id, gm20b::RefStreamSlot);
    EXPECT_EQ(refused.a, 31u);
}

// What an M-series Mac's driver does with API validation off (client_errors.h): misaligned indirect arguments or index
// offsets skip the draw, arguments past their buffer read as zeros
TEST_F(DeviceRecordGuards, IndirectDrawsApplesDriverSkipsOrReadsAsZeros)
{
    const gm20b::Buffer *b = &buffer;
    EXPECT_EQ(draw(plainRecord(0x96a7), 1,
                   [b](gm20b::RenderEncoder &e, const char **why) {
                       return e.drawIndirect(kTriangle, b, 2, why);
                   }),
              "");
    EXPECT_EQ(skips, 1u);
    EXPECT_EQ(skipped.id, gm20b::RefIndexedRange);
    EXPECT_EQ(draw(plainRecord(0x96a7), 1,
                   [b](gm20b::RenderEncoder &e, const char **why) {
                       return e.drawIndirect(kTriangle, b, 4092, why);
                   }),
              "");
    EXPECT_EQ(skips, 0u) << "arguments past their buffer read as zeros: drawn";
    EXPECT_EQ(draw(plainRecord(0x96a7), 1,
                   [b](gm20b::RenderEncoder &e, const char **why) {
                       return e.drawIndexedIndirect(kTriangle, 4, b, 2, b, 0, why);
                   }),
              "");
    EXPECT_EQ(skips, 1u);
    EXPECT_EQ(skipped.id, gm20b::RefIndexedRange);
    EXPECT_EQ(draw(plainRecord(0x96a7), 1,
                   [b](gm20b::RenderEncoder &e, const char **why) {
                       const gm20b::RenderEncoder::IcbExecute x = {b,         1000,  4,     16,      nullptr, 0,
                                                                   kTriangle, false, false, nullptr, 0};
                       return e.executeSlots(x, why);
                   }),
              "indirect command buffer commands outside its memory");
    EXPECT_EQ(refused.id, gm20b::RefCommandsRange);
    EXPECT_EQ(refused.a, 1000u);
    EXPECT_EQ(refused.c, 64u);
}

// The recording runs no execute whose range buffer is off 4 bytes or past its end (TegraGM20BMetalIndirect.mm, gm20b:icb.range): a
// draw that reaches its check anyway is the driver's error
TEST_F(DeviceRecordGuards, AnExecuteRangeReadOff4BytesIsTheDriversError)
{
    const gm20b::Buffer *b = &buffer;
    EXPECT_EQ(draw(plainRecord(0x96a9), 1,
                   [b](gm20b::RenderEncoder &e, const char **why) {
                       const gm20b::RenderEncoder::IcbExecute x = {b,         0,     4,     16,      b, 2,
                                                                   kTriangle, false, false, nullptr, 0};
                       return e.executeSlots(x, why);
                   }),
              "an indirect command buffer's execution range outside its buffer or not four-byte aligned");
    EXPECT_EQ(refused.id, gm20b::RefCommandsRangeBuffer);
    EXPECT_EQ(refused.a, 2u);
}

// The 3D class values the device's jobs since `first` wrote to `method` and the three methods after it, as one
// incrementing write each
std::vector<std::array<uint32_t, 4>> quads(size_t first, uint32_t method)
{
    std::vector<std::array<uint32_t, 4>> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++)
        for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
            const auto *w = reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va));
            for (uint32_t i = 0; w && i < push.dwords;) {
                const uint32_t hdr = w[i++], op = hdr >> 29, subc = hdr >> 13 & 7, m = (hdr & 0xfff) << 2,
                               n = hdr >> 16 & 0x1fff;
                if (!hdr || op == 4)
                    continue;
                if (op == 7)
                    break;
                if (!subc && op == 1 && m == method && n == 4 && i + 4 <= push.dwords)
                    out.push_back({w[i], w[i + 1], w[i + 2], w[i + 3]});
                i += n;
            }
        }
    return out;
}

// An index buffer offset at or past its end, which Metal's validation takes (an M4 Max draws from whatever memory follows
// the buffer): index 0 at every position, the zero page bound with its limit at its end (past it, 3D-B11), as NVK binds an
// empty range
TEST_F(DeviceRecordGuards, AnIndexOffsetAtOrPastItsBuffersEndReadsIndexZero)
{
    constexpr uint32_t kIndexBufferA = 0x17c8;
    const gm20b::Bo &zero = gm20b::DeviceHarness::zeroPage(dev);
    const std::array<uint32_t, 4> empty = {uint32_t(zero.va >> 32), uint32_t(zero.va),
                                           uint32_t((zero.va + zero.bytes - 1) >> 32),
                                           uint32_t(zero.va + zero.bytes - 1)};
    gm20b::Buffer odd;
    ASSERT_TRUE(dev.createBuffer(4098, odd));
    for (const auto &[indices, indexSize, indexOffset] :
         {std::tuple{&buffer, 4u, 4096ull}, std::tuple{&buffer, 4u, 8192ull}, std::tuple{&buffer, 2u, 4096ull},
          std::tuple{&odd, 4u, 4096ull}}) {
        const size_t first = kernel_model::model.jobs.size();
        const gm20b::Buffer *ib = indices, *arguments = &buffer;
        const uint32_t size = indexSize;
        const uint64_t offset = indexOffset;
        EXPECT_EQ(draw(plainRecord(0x96a8), 1,
                       [&](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawIndexedIndirect(kTriangle, size, ib, offset, arguments, 0, why);
                       }),
                  "")
            << size << " " << offset;
        const auto bound = quads(first, kIndexBufferA);
        ASSERT_EQ(bound.size(), 1u) << offset;
        EXPECT_EQ(bound[0], empty) << size << " " << offset;
    }
    const size_t first = kernel_model::model.jobs.size();
    const gm20b::Buffer *ib = &buffer;
    EXPECT_EQ(draw(plainRecord(0x96a8), 1,
                   [ib](gm20b::RenderEncoder &e, const char **why) {
                       return e.drawIndexedIndirect(kTriangle, 4, ib, 4092, ib, 0, why);
                   }),
              "");
    const uint64_t last = buffer.va + buffer.length - 1, at = buffer.va + 4092;
    const auto bound = quads(first, kIndexBufferA);
    ASSERT_EQ(bound.size(), 1u);
    EXPECT_EQ(bound[0],
              (std::array<uint32_t, 4>{uint32_t(at >> 32), uint32_t(at), uint32_t(last >> 32), uint32_t(last)}));
}

// The record as the record cache reads it back (decodeEntry, the reader of the compiler's records)
TEST_F(DeviceRecordGuards, ATurnedDrawOfAStateWithNoTurnedProgramIsItsOwnKind)
{
    gm20b::RuntimeRecord made = plainRecord(0x5eed000096aaull);
    made.pipeline.flags = gm20b::PipelineTurnRefused;
    gm20b::CacheKey k;
    k.stack = 1;
    k.key = "a state with no turned fragment program";
    gm20b::RuntimeRecord r;
    std::string refusal;
    ASSERT_EQ(gm20b::decodeEntry(gm20b::encodeEntry(k, &made, ""), k, r, refusal), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(draw(r), "");
    target.rotation = 1;
    EXPECT_EQ(draw(r), "a state drawn into a turned plane whose record holds no turned fragment program "
                       "(PipelineTurnRefused)");
    EXPECT_EQ(refused.id, gm20b::RefTurnRefused);
    EXPECT_EQ(uint64_t(refused.b) << 32 | refused.a, 0x5eed000096aaull);
    EXPECT_EQ(refused.c, 1u);
}

}  // namespace
