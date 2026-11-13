// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_variants.h"
#include "SwitchGM20BContext.h"

namespace {

// A pack of one pipeline whose fragment stage has `variants` variants, its header saying `version`
std::string writePack(uint32_t version, uint32_t variants, const char *name)
{
    std::vector<uint8_t> file(0x600, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = version;
    h.pipelines = 1;
    h.section[raw::PackPipelines] = {0x200, sizeof(raw::PackPipeline)};
    h.section[raw::PackVariants] = {0x500, variants * sizeof(raw::PackVariant)};
    h.chipFacts = raw::AssumeTexsF16;
    memcpy(file.data(), &h, sizeof h);
    raw::PackPipeline pp = {};
    pp.keyHash = 7;
    pp.variantCount[1] = uint8_t(variants);
    memcpy(file.data() + 0x200, &pp, sizeof pp);
    for (uint32_t i = 0; i < variants; i++) {
        const raw::PackVariant v = {raw::AssumeUniformAligned << i, 70000 + i};
        memcpy(file.data() + 0x500 + i * sizeof v, &v, sizeof v);
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

// A pack of a state's own record and two of its variants' (" | tests=2" and a cubic one), under one key
std::string writeVariantPack(const char *name)
{
    std::vector<uint8_t> file(0x2000, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.pipelines = 3;
    h.section[raw::PackPipelines] = {0x200, 3 * sizeof(raw::PackPipeline)};
    const char strings[] = "\0 | tests=2\0 | cubic s0:0000n t0:80";
    h.section[raw::PackStrings] = {0xc00, sizeof strings};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0xc00, strings, sizeof strings);
    raw::PackPipeline pp[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        pp[i].keyHash = 7;
        pp[i].vs = 70000 + i;
        pp[i].flags = i ? raw::PipelineVariantRecord : 0;
    }
    pp[1].variantName = 1;
    pp[2].variantName = 12;
    memcpy(file.data() + 0x200, pp, sizeof pp);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

// A pack holding only the channel's context init (the kit's, gen/SwitchGM20BContext.h), each relocated method's word
// `fill(i)` where the kit holds 0
template <typename Fill> std::string writeInitPack(const char *name, Fill fill)
{
    std::vector<raw::PackInitMethod> init;
    for (uint32_t i = 0; i < SWITCH_GM20B_CONTEXT_METHODS; i++) {
        const switch_gm20b_context_method &m = switch_gm20b_context[i];
        init.push_back({m.subc, m.mthd, m.reloc ? fill(i) : m.data, m.reloc});
    }
    std::vector<uint8_t> file(0x200 + init.size() * sizeof(raw::PackInitMethod), 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.section[raw::PackInit] = {0x200, init.size() * sizeof(raw::PackInitMethod)};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0x200, init.data(), init.size() * sizeof(raw::PackInitMethod));
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

// The context init as Device::emitContext pushes it: each method with its word relocated to `here`
std::vector<uint32_t> pushedInit(const raw::Pack &pack, const raw::InitAddresses &here)
{
    std::vector<uint32_t> out;
    const raw::PackInitMethod *im = pack.section<raw::PackInitMethod>(raw::PackInit);
    for (uint64_t i = 0; i < pack.bytes(raw::PackInit) / sizeof(raw::PackInitMethod); i++) {
        out.push_back(uint32_t(im[i].subc) << 16 | im[i].mthd);
        out.push_back(raw::relocated(im[i], here));
    }
    return out;
}

// The words a pack's init holds in its relocated methods (the probe's addresses once, 0 from the kit) never reach the
// GPU: each takes this process's address or limit, the interlock's ticket buffer among them
TEST(RawPack, TheContextInitPushesThisProcesssAddressesWhateverThePackHolds)
{
    raw::Pack zero, garbage;
    std::string err;
    ASSERT_TRUE(zero.load(writeInitPack("init-zero.bin",
                                        [](uint32_t) {
                                            return 0u;
                                        })
                              .c_str(),
                          0, err))
        << err;
    ASSERT_TRUE(garbage.load(writeInitPack("init-garbage.bin",
                                           [](uint32_t i) {
                                               return 0xdead0000u | i;
                                           })
                                 .c_str(),
                             0, err))
        << err;
    const raw::InitAddresses here = {0x10'0000'1000, 0x20'0000'2000, 0x30'0000'3000, 0x40'0000'4000,
                                     0x50'0000'5000, 8191,           1023,           0x60'0000'6000};
    const std::vector<uint32_t> pushed = pushedInit(zero, here);
    EXPECT_EQ(pushed, pushedInit(garbage, here));
    ASSERT_EQ(pushed.size(), 2u * SWITCH_GM20B_CONTEXT_METHODS);
    const std::map<uint32_t, uint32_t> expected = {
        {raw::RelocHeapHi, 0x10},      {raw::RelocHeapLo, 0x1000}, {raw::RelocZeroHi, 0x20},
        {raw::RelocZeroLo, 0x2000},    {raw::RelocRootHi, 0x30},   {raw::RelocRootLo, 0x3000},
        {raw::RelocTicHi, 0x40},       {raw::RelocTicLo, 0x4000},  {raw::RelocTicMax, 8191},
        {raw::RelocTscHi, 0x50},       {raw::RelocTscLo, 0x5000},  {raw::RelocTscMax, 1023},
        {raw::RelocTicketsLo, 0x6000}, {raw::RelocTicketsHi, 0x60}};
    uint32_t relocatedWords = 0;
    std::set<uint32_t> kinds;
    for (uint32_t i = 0; i < SWITCH_GM20B_CONTEXT_METHODS; i++) {
        const uint32_t reloc = switch_gm20b_context[i].reloc;
        if (!reloc)
            continue;
        relocatedWords++;
        kinds.insert(reloc);
        EXPECT_EQ(pushed[2 * i + 1], expected.at(reloc)) << "method " << i << " relocation " << reloc;
    }
    EXPECT_EQ(relocatedWords, 24u);
    EXPECT_EQ(kinds.size(), expected.size());
}

TEST(RawPack, AStatesVariantRecordsAreFoundByNameNeverAsTheState)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("variants.bin").c_str(), 0, err)) << err;
    ASSERT_NE(pack.find(7), nullptr);
    EXPECT_EQ(pack.find(7)->vs, 70000u);
    EXPECT_EQ(pack.find(7, raw::PackForm::Barrier), nullptr);
    EXPECT_EQ(pack.variantRecords(7).size(), 2u);
    ASSERT_NE(pack.variantRecord(7, " | tests=2"), nullptr);
    EXPECT_EQ(pack.variantRecord(7, " | tests=2")->vs, 70001u);
    EXPECT_EQ(pack.variantRecord(7, " | cubic s0:0000n t0:80")->vs, 70002u);
    EXPECT_EQ(pack.variantRecord(7, " | tests=3"), nullptr);
    EXPECT_EQ(pack.variantRecord(8, " | tests=2"), nullptr);
    const std::string key = "c0=80:15:0:1:0:0:1:0:0 | v a | f b | l RenderBox | cubic s0:0000n t0:80 | tests=4";
    EXPECT_EQ(key.substr(raw::variantNameAt(key)), " | cubic s0:0000n t0:80 | tests=4");
    EXPECT_EQ(raw::variantNameAt("c0=80 | v a | f b"), 17u);
}

// A state's records under one key: its storage form, its barrier twin, and its fetch-free form that reads another colour
// behind a barrier (a barrier form's flags too)
std::string writeFormsPack(const char *name)
{
    std::vector<uint8_t> file(0x1000, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.pipelines = 3;
    h.section[raw::PackPipelines] = {0x200, 3 * sizeof(raw::PackPipeline)};
    memcpy(file.data(), &h, sizeof h);
    raw::PackPipeline pp[3] = {};
    const uint32_t flags[3] = {raw::PipelineFetches | raw::PipelineStorageMetadata,
                               raw::PipelineFetches | raw::PipelineBarrierForm,
                               raw::PipelineFetches | raw::PipelineBarrierForm | raw::PipelineFetchFreeForm};
    for (uint32_t i = 0; i < 3; i++) {
        pp[i].keyHash = 9;
        pp[i].vs = 80000 + i;
        pp[i].flags = flags[i];
    }
    memcpy(file.data() + 0x200, pp, sizeof pp);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(RawPack, AFetchFreeFormThatReadsBehindABarrierIsFoundOnlyAsTheFetchFreeForm)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeFormsPack("forms.bin").c_str(), 0, err)) << err;
    ASSERT_NE(pack.find(9), nullptr);
    EXPECT_EQ(pack.find(9)->vs, 80000u);
    ASSERT_NE(pack.find(9, raw::PackForm::Barrier), nullptr);
    EXPECT_EQ(pack.find(9, raw::PackForm::Barrier)->vs, 80001u);
    ASSERT_NE(pack.find(9, raw::PackForm::FetchFree), nullptr);
    EXPECT_EQ(pack.find(9, raw::PackForm::FetchFree)->vs, 80002u);
    EXPECT_EQ(raw::recordForm(*pack.find(9, raw::PackForm::FetchFree)), raw::PackForm::FetchFree);
}

// T-v1 iconservicesagent[322]: it made primitive_image's state (17:55:29.836) and its cubic variant (17:55:30.681), none of
// them in the pack; a later pipeline object of that state got the made record from Device::pipeline, was taken as packed,
// and its variant was refused as one the pack lacks (17:55:44.477), though the process had made it 14 s before.
TEST(RawPack, ARecordAProcessMadeIsNotThePacks)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("holds.bin").c_str(), 0, err)) << err;
    EXPECT_TRUE(raw::packHolds(pack, pack.find(7)));
    EXPECT_TRUE(raw::packHolds(pack, pack.variantRecord(7, " | tests=2")));
    const raw::PackPipeline made = *pack.find(7);
    EXPECT_FALSE(raw::packHolds(pack, &made));
    EXPECT_FALSE(raw::packHolds(pack, nullptr));
    EXPECT_FALSE(raw::packHolds(pack, pack.find(7) + 3));
}

TEST(RawPack, RefusesAPackOfAnotherVersion)
{
    raw::Pack pack;
    std::string err;
    EXPECT_FALSE(pack.load(writePack(22, 0, "v22.bin").c_str(), 0, err));
    EXPECT_EQ(err, "a version-22 pack: this runtime reads version " + std::to_string(raw::kPackVersion) + " only");
}

TEST(RawPack, ReadsAStagesVariantsInOrder)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(raw::kPackVersion, 2, "current.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.header()->chipFacts, raw::AssumeTexsF16);
    const raw::PackPipeline *pp = pack.find(7);
    ASSERT_NE(pp, nullptr);
    uint32_t count = 0;
    EXPECT_EQ(pack.variants(*pp, 0, count), nullptr);
    EXPECT_EQ(count, 0u);
    const raw::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(v[0].program, 70000u);
    EXPECT_EQ(v[1].assume, raw::AssumeUniformAligned << 1);
}

TEST(RawPack, RefusesVariantsPastTheirSection)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(raw::kPackVersion, 0, "short.bin").c_str(), 0, err)) << err;
    raw::PackPipeline pp = *pack.find(7);
    pp.variantCount[1] = 1;
    uint32_t count = 1;
    EXPECT_EQ(pack.variants(pp, 1, count), nullptr);
    EXPECT_EQ(count, 0u);
}

TEST(RawVariants, TheFirstVariantWhoseAssumptionsHoldIsDrawn)
{
    const raw::PackVariant v[2] = {{raw::AssumeUniformAligned | raw::AssumeTexsF16, 1}, {raw::AssumeUniformAligned, 2}};
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned | raw::AssumeTexsF16), 0u);
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned | raw::AssumeUniformAligned << 1), 1u);
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned << 1), 2u);
    EXPECT_EQ(raw::chooseVariant(nullptr, 0, ~0u), 0u);
}

TEST(RawVariants, FloatAndExtendedRangeTexelsAreNotFixedPoint)
{
    EXPECT_TRUE(raw::fixedPointTexels(80));    // BGRA8Unorm
    EXPECT_TRUE(raw::fixedPointTexels(71));    // RGBA8Unorm_sRGB
    EXPECT_FALSE(raw::fixedPointTexels(115));  // RGBA16Float
    EXPECT_FALSE(raw::fixedPointTexels(552));  // BGRA10_XR
    EXPECT_FALSE(raw::fixedPointTexels(0));
}

// A pack listing two archives (keys {3, 9} and {5}) and the compute states it holds kernels of ({11})
std::string writeArchivePack(const char *name)
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    const uint32_t counts[2] = {3, 4};
    const raw::PackArchive archives[3] = {{raw::kPackHeldKernels, 0, 1}, {0x20, 1, 2}, {0x40, 3, 1}};
    const uint64_t keys[4] = {11, 3, 9, 5};
    h.section[raw::PackArchives] = {0x200, sizeof counts + sizeof archives + sizeof keys};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0x200, counts, sizeof counts);
    memcpy(file.data() + 0x200 + sizeof counts, archives, sizeof archives);
    memcpy(file.data() + 0x200 + sizeof counts + sizeof archives, keys, sizeof keys);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(RawPack, AnArchiveDeclaresItsOwnKeysAlone)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeArchivePack("archives.bin").c_str(), 0, err)) << err;
    const raw::PackArchive *first = pack.archive(0x20), *second = pack.archive(0x40);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(pack.archive(0x30), nullptr) << "an archive the pack does not list";
    EXPECT_TRUE(pack.declares(*first, 3));
    EXPECT_TRUE(pack.declares(*first, 9));
    EXPECT_FALSE(pack.declares(*first, 5)) << "the next archive's key";
    EXPECT_FALSE(pack.declares(*first, 11)) << "a held kernel's key";
    EXPECT_TRUE(pack.declares(*second, 5));
    const raw::PackArchive *held = pack.archive(raw::kPackHeldKernels);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(pack.declares(*held, 11));
    EXPECT_FALSE(pack.declares(*held, 3));
}

// wide_index_pack.py's pack, written by the pack writer's codec: every program index past 65,535 reads back whole
TEST(RawPack, ProgramIndicesPast16BitsReadBackAsThePackWriterWroteThem)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load("drivers/TegraGM20BMetal/test/wide_index.bin", 0, err)) << err;
    const std::string key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v wide_vertex  | f wide_fragment";
    const raw::PackPipeline *pp = pack.find(raw::Pack::fnv(key.data(), key.size()));
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->vs, 70000u);
    EXPECT_EQ(pp->fs, 70001u);
    uint32_t count = 0;
    const raw::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(v[0].program, 70002u);
    const std::string kernel = "kernel | wide_kernel";
    const raw::PackKernel *k = pack.findKernel(raw::Pack::fnv(kernel.data(), kernel.size()));
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->program, 70003u);
    EXPECT_EQ(k->regionWord, -1);
    const raw::PackSplit *split = pack.split(70001);
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->fs, 70004u);
    EXPECT_EQ(split->prologue, 70005u);
    EXPECT_EQ(split->frontierBytes, 16u);
    EXPECT_EQ(split->frontierCbuf, 2u);
    EXPECT_EQ(pack.split(70001 & 0xFFFF), nullptr);
}

// wide_index_pack.py's second program: the root table and 11 uniform buffers, then its registers and bound textures' slot
TEST(RawPack, AProgramOfTwelveConstantBuffersReadsBackWhole)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load("drivers/TegraGM20BMetal/test/wide_index.bin", 0, err)) << err;
    ASSERT_EQ(pack.bytes(raw::PackPrograms), 2 * sizeof(raw::PackProgram));
    const raw::PackProgram &p = pack.section<raw::PackProgram>(raw::PackPrograms)[1];
    ASSERT_EQ(p.cbufs, 12u);
    EXPECT_EQ(p.cbufType[0], raw::CbufRoot);
    for (uint32_t c = 1; c < 12; c++) {
        EXPECT_EQ(raw::cbufKind(p.cbufType[c]), raw::CbufUbo);
        EXPECT_EQ(raw::cbufUboOffset(p.cbufType[c]), 16 * (c - 1));
    }
    EXPECT_EQ(p.cbufType[12], 0u);
    EXPECT_EQ(p.gprs, 40u);
    EXPECT_EQ(p.boundTextures, 2u);
}

TEST(RawPack, APackWithoutArchiveListsListsNone)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("no-archives.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.archive(raw::kPackHeldKernels), nullptr);
    EXPECT_EQ(pack.archive(0x20), nullptr);
}

}  // namespace
