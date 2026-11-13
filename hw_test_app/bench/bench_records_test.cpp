// bench_records.cpp over a pack holding the template state the Switch's pack gave the bench (iOS 27, 1b1ae4ebb482e4d1
// in pack v14 19bc973b66d63deb and v15 c7fe25c8f16ed0bf): colour 0 of BGR10_XR, which its pixel program packs into one
// word, so its register words write R alone. Since v15 a program's register count is its own (PackProgram::gprs), not
// a blob word.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>
#include "hw_test_app/bench/bench_known.h"
#include "hw_test_app/bench/bench_records.h"

namespace {

constexpr uint64_t kTemplateKey = 0x1b1ae4ebb482e4d1ull;
constexpr char kTemplateKeyString[] =
    "samples=1 topo=0 ds=0/0 c0=554:15:0:1:0:0:1:0:0 c1=115:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 "
    "a2=29:24:1:48:1:1 a3=27:32:1:48:1:1";
// Its register words in the device's pack: SET_CT_WRITE(0) (0x1a00) R alone
constexpr uint32_t kTemplateBlob[] = {
    0x0210, 0,          0x0754, 1,          0x0f90, 0,      0x1160, 0x38200000, 0x1164, 0x38601000, 0x1168, 0x38200040,
    0x116c, 0x38200040, 0x1170, 0x38200040, 0x1224, 0,      0x1360, 0,          0x1364, 0,          0x193c, 0x181d,
    0x1a00, 1,          0x1a04, 0,          0x1c00, 0x1030, 0x1c10, 0,          0x1e04, 0x8006,     0x1e08, 0x4000,
    0x1e0c, 0x4000,     0x1e10, 0x8006,     0x1e14, 0x4000, 0x1e18, 0x4000,     0x1e24, 0x8006,     0x1e28, 0x4000,
    0x1e2c, 0x4000,     0x1e30, 0x8006,     0x1e34, 0x4000, 0x1e38, 0x4000,
};
// The template's programs' register counts and the vertex program's bound-texture slot
constexpr uint8_t kTemplateGprs[2] = {0x20, 0x18}, kVertexBoundTextures = 3;
constexpr uint32_t kSetCtWrite0 = 0x1a00, kSetPixelRegisterCount = 0x214c;
constexpr uint64_t kTexturedKey = 0x2b2ae4ebb482e4d1ull;
constexpr uint16_t kTextureOffset = 0x20;
constexpr uint32_t kRgbaFloat32 = 125;

template <class T>
void put(std::vector<uint8_t> &out, raw::PackHeader &h, raw::PackSectionId s, const T *items, size_t n)
{
    out.resize((out.size() + 0xff) & ~size_t(0xff));
    h.section[s] = {out.size(), n * sizeof(T)};
    const auto *bytes = reinterpret_cast<const uint8_t *>(items);
    out.insert(out.end(), bytes, bytes + n * sizeof(T));
}

class BenchRecords : public testing::Test {
protected:
    void SetUp() override
    {
        raw::PackHeader h = {};
        memcpy(h.magic, "GM20BPK1", 8);
        h.version = raw::kPackVersion;
        h.programs = 2;
        h.pipelines = 2;
        raw::PackProgram programs[2] = {};
        for (uint32_t i = 0; i < 2; i++) {
            programs[i].heapOffset = i * 0x100;
            programs[i].program = i * 0x100 + 0x30;
            programs[i].bytes = 0x100;
            programs[i].cbufs = 1;
            programs[i].cbufType[0] = raw::CbufRoot;
            programs[i].gprs = kTemplateGprs[i];
        }
        programs[0].boundTextures = kVertexBoundTextures;
        raw::PackPipeline pipeline = {};
        pipeline.keyHash = kTemplateKey;
        pipeline.vs = 0;
        pipeline.fs = 1;
        pipeline.blobDwords = sizeof kTemplateBlob / 4;
        pipeline.vertexSlot = 1;
        pipeline.vertexStride = 48;
        // the same state with a sampled texture: the template of a program that samples
        raw::PackPipeline textured = pipeline;
        textured.keyHash = kTexturedKey;
        textured.entries = 1;
        textured.entry[0] = {kTextureOffset, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
        const raw::PackPipeline both[2] = {pipeline, textured};
        std::vector<uint8_t> heap(0x200, 0x5a);
        std::vector<uint8_t> out(sizeof h);
        put(out, h, raw::PackPrograms, programs, 2);
        put(out, h, raw::PackPipelines, both, 2);
        put(out, h, raw::PackBlob, kTemplateBlob, sizeof kTemplateBlob / 4);
        put(out, h, raw::PackStrings, kTemplateKeyString, sizeof kTemplateKeyString);
        put(out, h, raw::PackHeap, heap.data(), heap.size());
        memcpy(out.data(), &h, sizeof h);
        const char *dir = getenv("TEST_TMPDIR");
        path_ = std::string(dir ? dir : "/tmp") + "/bench_records_test.pack";
        FILE *f = fopen(path_.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fwrite(out.data(), 1, out.size(), f), out.size());
        fclose(f);
        std::string err;
        ASSERT_TRUE(pack_.load(path_.c_str(), 0, err)) << err;
    }

    void TearDown() override
    {
        unlink(path_.c_str());
    }

    static bench::Program known()
    {
        bench::Program p;
        const auto *sph = reinterpret_cast<const uint8_t *>(bench::kKnownSph);
        const auto *code = reinterpret_cast<const uint8_t *>(bench::kKnownCode);
        p.sph.assign(sph, sph + sizeof bench::kKnownSph);
        p.code.assign(code, code + sizeof bench::kKnownCode);
        p.gprs = bench::kKnownGprs;
        return p;
    }

    // TEX.LZ R0, (R4, R5), bound texture <placeholder>, 2D, RGBA: a sampling program's fetch before the app places it
    static bench::Program sampling()
    {
        bench::Program p = known();
        const uint64_t tex = 0xc038ull << 48 | 7ull << 16 | 4ull << 8 | 255ull << 20 | 2ull << 28 | 0xfull << 31 |
                             uint64_t(bench::kTextureIndexPlaceholder) << 36 | 1ull << 55;
        memcpy(p.code.data() + 8, &tex, sizeof tex);
        return p;
    }

    static std::vector<uint32_t> words(const raw::RuntimeRecord &r, uint32_t method)
    {
        std::vector<uint32_t> found;
        for (size_t k = 0; k + 1 < r.blob.size(); k += 2)
            if (r.blob[k] == method)
                found.push_back(r.blob[k + 1]);
        return found;
    }

    raw::Pack pack_;
    std::string path_;
};

TEST_F(BenchRecords, TheDevicesTemplateIsTaken)
{
    const raw::PackPipeline *tpl = bench::fragmentTemplate(pack_);
    ASSERT_NE(tpl, nullptr);
    EXPECT_EQ(tpl->keyHash, kTemplateKey);
}

TEST_F(BenchRecords, ColourZeroIsWrittenWholeWhateverTheTemplateWrote)
{
    raw::RuntimeRecord r;
    std::string why;
    ASSERT_TRUE(bench::fragmentRecord(pack_, *bench::fragmentTemplate(pack_), known(), kRgbaFloat32,
                                      0xbe7c000000000001ull, {}, {}, r, why))
        << why;
    EXPECT_EQ(words(r, kSetCtWrite0), std::vector<uint32_t>{0x1111});
    EXPECT_TRUE(words(r, kSetPixelRegisterCount).empty());
    EXPECT_EQ(r.pipeline.blobDwords, r.blob.size());
    EXPECT_NE(r.keyString.find(" c0=125:15:"), std::string::npos) << r.keyString;
}

// The runtime writes SET_PIPELINE_REGISTER_COUNT from the PackProgram a record's program is placed as (programSlot); a count
// of 0 is ILLEGAL_SHADER_REGISTER_COUNT at the draw (class error 0xb5 for the vertex program)
TEST_F(BenchRecords, BothProgramsCarryTheirRegisterCounts)
{
    raw::RuntimeRecord r;
    std::string why;
    ASSERT_TRUE(bench::fragmentRecord(pack_, *bench::fragmentTemplate(pack_), known(), kRgbaFloat32,
                                      0xbe7c000000000003ull, {}, {}, r, why))
        << why;
    raw::PackProgram vertex, pixel;
    raw::programSlot(r.programs[0], vertex);
    raw::programSlot(r.programs[1], pixel);
    EXPECT_EQ(vertex.gprs, kTemplateGprs[0]);
    EXPECT_EQ(vertex.boundTextures, kVertexBoundTextures);
    EXPECT_EQ(pixel.gprs, bench::kKnownGprs);
    EXPECT_NE(pixel.gprs, 0);
    EXPECT_EQ(pixel.boundTextures, 0);
}

TEST_F(BenchRecords, ASamplingProgramReadsItsTemplatesTextureFromSetZero)
{
    EXPECT_FALSE(bench::samplesTexture(known()));
    ASSERT_TRUE(bench::samplesTexture(sampling()));
    const raw::PackPipeline *tpl = bench::textureTemplate(pack_);
    ASSERT_NE(tpl, nullptr);
    EXPECT_EQ(tpl->keyHash, kTexturedKey);
    EXPECT_EQ(bench::fragmentTemplate(pack_)->keyHash, kTemplateKey);
    raw::RuntimeRecord r;
    std::string why;
    ASSERT_TRUE(
        bench::fragmentRecord(pack_, *tpl, sampling(), kRgbaFloat32, 0xbe7c000000000004ull, {}, {}, r, why, true))
        << why;
    uint64_t placed;
    memcpy(&placed, r.programs[1].image.data() + 80 + 8, sizeof placed);
    EXPECT_EQ(placed >> 36 & 0x1fff, kTextureOffset / 4);
    EXPECT_EQ(placed & ~(0x1fffull << 36),
              0xc038ull << 48 | 7ull << 16 | 4ull << 8 | 255ull << 20 | 2ull << 28 | 0xfull << 31 | 1ull << 55);
    EXPECT_EQ(r.programs[1].cbufTypes.back(), raw::CbufSet);
    EXPECT_EQ(r.programs[1].boundTextures, r.programs[1].cbufTypes.size());
    EXPECT_FALSE(bench::fragmentRecord(pack_, *bench::fragmentTemplate(pack_), sampling(), kRgbaFloat32,
                                       0xbe7c000000000005ull, {}, {}, r, why));
}

TEST_F(BenchRecords, ATemplateIsTakenByItsKeyOnlyWhenPlain)
{
    EXPECT_EQ(bench::fragmentTemplate(pack_, kTemplateKey), bench::fragmentTemplate(pack_));
    EXPECT_EQ(bench::fragmentTemplate(pack_, 0x0123456789abcdefull), nullptr);
}

TEST_F(BenchRecords, FirstColourOnlyDropsTheOtherColours)
{
    EXPECT_EQ(
        bench::withoutOtherColours("samples=1 topo=0 ds=0/0 c0=554:15:0:1:0:0:1:0:0 c1=115:0:0:1:0:0:1:0:0 a0=31"),
        "samples=1 topo=0 ds=0/0 c0=554:15:0:1:0:0:1:0:0 a0=31");
    EXPECT_EQ(bench::withoutOtherColours("c0=80:15:0 c1=115:0:0 c2=55:0:0"), "c0=80:15:0");
    raw::RuntimeRecord r;
    std::string why;
    ASSERT_TRUE(bench::fragmentRecord(pack_, *bench::fragmentTemplate(pack_), known(), kRgbaFloat32,
                                      0xbe7c000000000002ull, {}, {}, r, why, true))
        << why;
    EXPECT_EQ(r.keyString.find(" c1="), std::string::npos) << r.keyString;
    EXPECT_NE(r.keyString.find(" c0=125:15:"), std::string::npos) << r.keyString;
}

}  // namespace
