// SPDX-License-Identifier: MIT
// A YCbCr surface's planes read past their ends on raw::Device, as Metal reads them (raw/test/ycbcroracle.mm): rows past a
// plane up to the end of the allocation's last page, which the import maps, and rows longer than the plane's pitch,
// running on into the next, which the copy engine gathers into the plane's pool image (Texture::sampled) as no texture
// header lays them out; and a one-plane 4:2:2 texture's plane, split by the copy engine into the luma and chroma images its
// conversion reads.
#include "raw_device_harness.h"
#include "predication_model.h"
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_ycbcr.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kR8Unorm = 10;

struct Surface {
    void *base = nullptr;
    uint64_t alloc = 0, mapped = 0;

    explicit Surface(uint64_t bytes, size_t page = size_t(getpagesize())) : alloc(bytes)
    {
        mapped = (bytes + page - 1) / page * page;
        base = aligned_alloc(page, mapped);
        memset(base, 0x5a, mapped);
    }

    ~Surface()
    {
        free(base);
    }
};

class DeviceSurfaceRows : public harness::DeviceTest {};

TEST_F(DeviceSurfaceRows, SurfaceBuffersMapTheirWholeRoundedMetalLength)
{
    constexpr uint64_t kMetalPage = 16384;
    kernel_model::model.unknownUsesRefused = true;
    for (const uint64_t bytes : {20480u, 24576u, 28672u}) {
        Surface s(bytes, kMetalPage);
        SCOPED_TRACE(bytes);
        ASSERT_EQ(s.mapped, 32768u);
        memset(static_cast<uint8_t *>(s.base) + s.alloc, 0xe7, s.mapped - s.alloc);
        raw::Texture image;
        ASSERT_TRUE(
            dev.createSurfaceTexture(1, s.base, s.alloc, nullptr, 0, 64, uint32_t(bytes / 256), 256, 80, image));
        raw::Buffer source, destination;
        bool tiled = false;
        ASSERT_TRUE(dev.createSurfaceBuffer(1, s.base, s.mapped, nullptr, source, tiled));
        ASSERT_FALSE(tiled);
        ASSERT_EQ(source.length, s.mapped);
        EXPECT_EQ(source.import, image.import);
        ASSERT_TRUE(dev.createBuffer(s.mapped, destination));
        for (const uint64_t copied : {uint64_t(4), s.mapped}) {
            SCOPED_TRACE(copied);
            memset(destination.cpu, 0, destination.length);
            raw::CommandBuffer *cb = dev.begin();
            ASSERT_NE(cb, nullptr);
            cb->use(source, false);
            cb->use(destination, true);
            const size_t first = kernel_model::model.jobs.size();
            raw::BlitEncoder *blit = cb->blit();
            ASSERT_NE(blit, nullptr);
            blit->copyBytes(source.va, destination.va, copied);
            blit->end();
            uint32_t seqno = 0;
            const bool committed = cb->commit(seqno);
            EXPECT_TRUE(committed) << dev.kernel().error();
            if (!committed) {
                cb->abandon();
                continue;
            }
            predication_model::Gpu gpu(0);
            gpu.map = [](uint64_t va) {
                return kernel_model::model.host(va);
            };
            for (size_t j = first; j < kernel_model::model.jobs.size(); ++j)
                for (const TegraGM20BAbiPush &push : kernel_model::model.jobs[j].pushes) {
                    const auto *words = reinterpret_cast<const uint32_t *>(
                        kernel_model::model.host(push.va, uint64_t(push.dwords) * 4));
                    ASSERT_NE(words, nullptr);
                    gpu.run(words, push.dwords, true);
                }
            EXPECT_EQ(memcmp(destination.cpu, s.base, copied), 0);
        }
        dev.destroyBuffer(destination);
        dev.destroyBuffer(source);
        dev.destroyTexture(image);
    }
}

TEST_F(DeviceSurfaceRows, RowsPastAPlaneAreReadWithinTheAllocationsLastPage)
{
    Surface s(1344);
    raw::Texture t, refused;
    EXPECT_FALSE(dev.createSurfaceTexture(0, s.base, s.alloc, nullptr, 1024, 16, 40, 64, kR8Unorm, refused))
        << "past the allocation, with no readable bytes named";
    ASSERT_TRUE(dev.createSurfaceTexture(0, s.base, s.alloc, nullptr, 1024, 16, 40, 64, kR8Unorm, t, false, s.mapped));
    EXPECT_EQ(t.storage.bytes, 64u * 40) << "its rows past the allocation are what the texture reads";
    EXPECT_EQ(t.sampled, nullptr);
    raw::Texture past;
    EXPECT_FALSE(
        dev.createSurfaceTexture(0, s.base, s.alloc, nullptr, 1024, 16, 400, 64, kR8Unorm, past, false, s.mapped))
        << "past the last page: nothing maps it";
}

TEST_F(DeviceSurfaceRows, RowsLongerThanThePitchAreGatheredIntoThePoolImage)
{
    Surface s(4096);
    raw::Texture t, refused;
    EXPECT_FALSE(dev.createSurfaceTexture(0, s.base, s.alloc, nullptr, 0, 80, 8, 64, kR8Unorm, refused));
    ASSERT_TRUE(dev.createSurfaceTexture(0, s.base, s.alloc, nullptr, 0, 80, 8, 64, kR8Unorm, t, false, s.mapped));
    ASSERT_NE(t.sampled, nullptr) << "no header lays out rows that overlap";
    EXPECT_EQ(t.tic, t.sampled->tic);
    EXPECT_EQ(t.sampled->width, 80u);
    EXPECT_EQ(t.storage.bytes, 64u * 7 + 80) << "the last row runs 16 bytes past its pitch";
}

TEST_F(DeviceSurfaceRows, AYcbcrTextureOverPlaneOneOfAFourTwoZeroSurfaceIsMade)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    const char *state = "raw.ycbcr.ycbcr_codes_frag";
    r.pipeline.keyHash = raw::Pack::fnv(state, strlen(state));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    Surface s(64 * 8 + 64 * 4);
    raw::YcbcrFacts f;
    f.pixelFormat = 540;
    f.fourcc = 0x34323076;  // '420v'
    f.plane = 1;
    f.planes = 2;
    f.width = f.surfaceWidth = 16;
    f.height = 16;
    f.surfaceHeight = 8;
    f.offset[1] = 64 * 8;
    f.pitch[0] = f.pitch[1] = 64;
    f.allocBytes = s.alloc;
    f.mappedBytes = s.mapped;
    f.simple = true;
    raw::YcbcrPlan plan;
    ASSERT_EQ(raw::planYcbcr(f, plan), nullptr);
    raw::Texture t;
    ASSERT_TRUE(dev.createYcbcrTexture(0, s.base, s.alloc, nullptr, plan, 16, 16, t));
    ASSERT_NE(t.luma, nullptr);
    EXPECT_EQ(t.luma->va, t.luma->import->bo.va + 64 * 8) << "plane 1 read as luma";
    EXPECT_EQ(t.luma->height, 16u) << "twelve rows past plane 1's four";
    EXPECT_EQ(t.chroma, nullptr);
    EXPECT_EQ(t.luma->nativePlane, ~0u) << "no native view holds rows past the plane";
}

// The copy engine's launches the command buffer submitted: (source, destination, SET_REMAP_COMPONENTS) each
struct Launch {
    uint64_t from, to;
    uint32_t remap;
};

std::vector<Launch> launches(size_t first)
{
    constexpr uint32_t kLaunch = 0x0300, kInUpper = 0x0400, kInLower = 0x0404, kOutUpper = 0x0408, kOutLower = 0x040c,
                       kRemap = 0x0708;
    std::vector<Launch> out;
    uint64_t in = 0, to = 0;
    uint32_t remap = 0;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        auto method = [&](uint32_t v) {
            if (subc != raw::SUBC_COPY)
                return;
            if (m == kInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kOutUpper)
                to = (to & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kOutLower)
                to = (to & ~0xffffffffull) | v;
            else if (m == kRemap)
                remap = v;
            else if (m == kLaunch)
                out.push_back({in, to, remap});
        };
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(w);
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                method(w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
    return out;
}

TEST_F(DeviceSurfaceRows, AOnePlaneTextureIsSplitIntoItsLumaAndChromaImages)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=115 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    const char *state = "raw.ycbcr.ycbcr_converted_frag";
    r.pipeline.keyHash = raw::Pack::fnv(state, strlen(state));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    Surface s(80 * 9);
    raw::YcbcrFacts f;
    f.pixelFormat = 501;
    f.fourcc = 0x32767579;  // '2vuy'
    f.width = f.surfaceWidth = 33;
    f.height = f.surfaceHeight = 9;
    f.pitch[0] = 80;
    f.allocBytes = s.alloc;
    f.mappedBytes = s.mapped;
    f.simple = true;
    raw::YcbcrPlan plan;
    ASSERT_EQ(raw::planYcbcr(f, plan), nullptr);
    raw::Texture t;
    ASSERT_TRUE(dev.createYcbcrTexture(0, s.base, s.alloc, nullptr, plan, 33, 9, t));
    ASSERT_TRUE(t.onePlane);
    ASSERT_NE(t.luma, nullptr);
    ASSERT_NE(t.chroma, nullptr);
    EXPECT_EQ(t.luma->width, 34u);
    EXPECT_EQ(t.chroma->width, 17u);
    EXPECT_FALSE(t.luma->blockLinear || t.chroma->blockLinear) << "the copy engine writes them pitch to pitch";
    EXPECT_EQ(t.planes.luma, t.import->bo.va);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    raw::BlitEncoder *b = cb->blit();
    b->splitPlane(t);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<Launch> l = launches(first);
    ASSERT_EQ(l.size(), 2u);
    // DST_X, DST_Y of one-byte components, four a source texel, two a destination's
    constexpr uint32_t kSizes = 0u << 16 | 3u << 20 | 1u << 24;
    EXPECT_EQ(l[0].from, t.planes.luma);
    EXPECT_EQ(l[0].to, t.luma->va);
    EXPECT_EQ(l[0].remap, (0u | 2u << 4) | kSizes) << "Y0 and Y1";
    EXPECT_EQ(l[1].from, t.planes.luma);
    EXPECT_EQ(l[1].to, t.chroma->va);
    EXPECT_EQ(l[1].remap, (1u | 3u << 4) | kSizes) << "Cb and Cr";
    dev.destroyTexture(t);
}

}  // namespace
