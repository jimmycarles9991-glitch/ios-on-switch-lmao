// SPDX-License-Identifier: MIT
// raw_slm.h: the shader local memory a program's header or a kernel's QMD asks of a warp, the area NVK would size for it on
// GM20B (two TPCs), and the methods that give it to the raw channel's 3D and compute classes.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_owned.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_slm.h"

namespace {
using namespace raw;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

constexpr uint32_t kTpcs = 2;

TEST(SlmNeed, AProgramWithoutLocalMemoryAsksNothing)
{
    const uint32_t sph[4] = {0x00001462, 0, 0, 0};
    EXPECT_EQ(slmPerWarpOfHeader(sph), 0u);
}

TEST(SlmNeed, AHarvestedPixelProgramsScratchPerWarp)
{
    // NAK's code for an Apple fragment function in the device's records: slm 96 bytes a thread, no CRS spill
    const uint32_t sph[4] = {0x00001462, 96, 0, 0};
    EXPECT_EQ(slmPerWarpOfHeader(sph), 96u * 32);
    EXPECT_EQ(slmPerWarpOfHeader(reinterpret_cast<const uint8_t *>(sph)), 96u * 32);
}

TEST(SlmNeed, ACrsSpillAloneAsksForItsBytes)
{
    const uint32_t sph[4] = {0x00001462, 0, 0, 1024};
    EXPECT_EQ(slmPerWarpOfHeader(sph), 1024u);
}

TEST(SlmNeed, ScratchAndSpillAddUpAndAlignTo512)
{
    EXPECT_EQ(slmPerWarp(16, 1024), 1536u);
    EXPECT_EQ(slmPerWarp(432, 1024), 14848u);
    EXPECT_EQ(slmPerWarp(1, 0), 512u);
}

TEST(SlmNeed, AKernelsQmdFields)
{
    uint32_t qmd[64] = {};
    qmd[45] = 128;   // SHADER_LOCAL_MEMORY_LOW_SIZE
    qmd[47] = 1024;  // SHADER_LOCAL_MEMORY_CRS_SIZE
    EXPECT_EQ(slmPerWarpOfQmd(qmd), 128u * 32 + 1024);
    qmd[45] = qmd[47] = 0;
    EXPECT_EQ(slmPerWarpOfQmd(qmd), 0u);
}

TEST(SlmNeed, AQmdAtAnEvenOffset)
{
    alignas(4) uint8_t record[2 + sizeof(uint32_t) * 64] = {};
    const uint32_t low = 128, crs = 1024;
    memcpy(record + 2 + 45 * 4, &low, sizeof low);
    memcpy(record + 2 + 47 * 4, &crs, sizeof crs);
    EXPECT_EQ(slmPerWarpOfQmd(record + 2), 128u * 32 + 1024);
}

TEST(SlmArea, NvksSizesOnTwoTpcs)
{
    const SlmArea crs = slmArea(1024, kTpcs);
    EXPECT_EQ(crs.perTpc, 64u * 1024);
    EXPECT_EQ(crs.bytes, 128u * 1024);
    const SlmArea scratch = slmArea(96 * 32, kTpcs);
    EXPECT_EQ(scratch.perTpc, 192u * 1024);
    EXPECT_EQ(scratch.bytes, 384u * 1024);
    const SlmArea small = slmArea(512, kTpcs);
    EXPECT_EQ(small.perTpc, 32u * 1024);
    EXPECT_EQ(small.bytes, 128u * 1024);
}

std::vector<uint8_t> header(uint32_t w0)
{
    std::vector<uint8_t> image(80 + 16);
    memcpy(image.data(), &w0, sizeof w0);
    return image;
}

TEST(SlmStage, ARecordsProgramsAreAVertexThenAPixelProgram)
{
    const uint32_t vertex = 1 | 3 << 5 | 1 << 10, pixel = 2 | 3 << 5 | 5 << 10;
    EXPECT_TRUE(headerIsStage(header(vertex), false));
    EXPECT_TRUE(headerIsStage(header(pixel), true));
    EXPECT_FALSE(headerIsStage(header(vertex), true));
    EXPECT_FALSE(headerIsStage(header(pixel), false));
    EXPECT_FALSE(headerIsStage(header(0), true));  // a capture off its program: the slot's zero padding
    EXPECT_FALSE(headerIsStage(std::vector<uint8_t>(16), true));
}

uint32_t method(uint32_t header)
{
    return (header & 0xfff) << 2;
}

uint32_t subchannel(uint32_t header)
{
    return (header >> 13) & 7;
}

TEST(SlmEmit, TheChannelGetsTheAreaOnBothClassesAfterAnIdle)
{
    uint32_t words[kSlmWords + 4] = {};
    Push p{words, words + kSlmWords};
    const uint64_t va = 0x13'2000'0000ull;
    const SlmArea a = slmArea(96 * 32, kTpcs);
    emitSlm(p, va, a);
    ASSERT_EQ(p.at - words, int(kSlmWords));
    EXPECT_EQ(method(words[0]), 0x0110u);  // WAIT_FOR_IDLE
    EXPECT_EQ(words[0] >> 29, 4u);
    EXPECT_EQ(method(words[1]), 0x0790u);  // SET_SHADER_LOCAL_MEMORY_A..E
    EXPECT_EQ(subchannel(words[1]), 0u);
    EXPECT_EQ(words[2], 0x13u);
    EXPECT_EQ(words[3], 0x20000000u);
    EXPECT_EQ(words[4], 0u);
    EXPECT_EQ(words[5], 384u * 1024);
    EXPECT_EQ(words[6], 96u * 32);
    EXPECT_EQ(method(words[7]), 0x0790u);  // compute SET_SHADER_LOCAL_MEMORY_A/B
    EXPECT_EQ(subchannel(words[7]), 1u);
    EXPECT_EQ(words[9], 0x20000000u);
    EXPECT_EQ(method(words[10]), 0x02e4u);  // NON_THROTTLED_A..C
    EXPECT_EQ(words[12], 192u * 1024);
    EXPECT_EQ(words[13], 0xffu);
    EXPECT_EQ(method(words[14]), 0x02f0u);  // THROTTLED_A..C
    EXPECT_EQ(words[16], 192u * 1024);
    EXPECT_EQ(words[17], 0xffu);
}

TEST(SlmOwned, TheAreaIsTheRuntimesNeverARecords)
{
    // iOS 27's pack carried its probe process's area in 396 records' blobs: 0x790=0x3e 0x794=0xfbf6c000 ...
    for (uint32_t m = 0x0790; m < 0x07A4; m += 4)
        EXPECT_TRUE(ownedMethod(m)) << std::hex << m;
    EXPECT_FALSE(ownedMethod(0x077C));
    EXPECT_FALSE(ownedMethod(0x07A4));
}

size_t at(const std::string &text, const std::string &what)
{
    const size_t found = text.find(what);
    EXPECT_NE(found, std::string::npos) << what;
    return found;
}

// Every program the raw path places asks for its area before its state can be bound, and a submission gives the channel
// the area ahead of its prologue; a pack made before the methods were the runtime's has them dropped at the bind.
TEST(SlmOwned, EveryPlacedProgramGetsTheAreaBeforeItsBind)
{
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    const std::string pack = Body(gpu, "Pipeline *Device::placeRecord(const PackPipeline &pp, bool caTransform)");
    EXPECT_LT(at(pack, "slmNeed(std::max(slmPerWarpOfHeader(vs.header), slmPerWarpOfHeader(fs.header)))"),
              at(pack, "build(pl, pp, vs, fs,"));
    const std::string completed = SourceText("../raw/runtime/raw_completed.cpp");
    EXPECT_THAT(Body(completed, "bool Device::placeCompleted(const CapturedProgram &p, PlacedProgram &out)"),
                HasSubstr("slmNeed(slmPerWarpOfHeader(p.image.data()))"));
    const std::string split =
        Body(SourceText("../raw/runtime/raw_split.cpp"), "SplitState *Device::makeSplit(Pipeline &pl)");
    EXPECT_LT(at(split, "slmNeed(std::max(slmPerWarpOfHeader(fs.header), slmPerWarpOfQmd(source->rec.qmd)))"),
              at(split, "build(s.pipeline,"));
    const std::string compute = SourceText("../raw/runtime/raw_compute.cpp");
    EXPECT_THAT(Body(compute, "const ComputeState *computeState(Device &dev, uint64_t keyHash)"),
                HasSubstr("dev.slmNeed(slmPerWarpOfQmd(k->qmd))"));
    EXPECT_THAT(Body(compute, "const ComputeState *placeCompute(Device &dev, const ComputeRecord &r)"),
                HasSubstr("dev.slmNeed(slmPerWarpOfQmd(r.kernel.qmd))"));
    const std::string commit = Body(gpu, "bool CommandBuffer::commit(uint32_t &seqno)");
    EXPECT_THAT(commit, HasSubstr("segments_.insert(0,"));
    EXPECT_LT(at(commit, "emitSlm(p, slmVa,"), at(commit, "d.job(d.setView_, d.channel_,"));
    const std::string build = Body(gpu, "void Device::build(Pipeline &pl, const PackPipeline &pp,");
    EXPECT_LT(at(build, "if (ownedMethod(m) || swizzleMethod(m))"), at(build, "w.m(m, v);"));
}
}  // namespace
