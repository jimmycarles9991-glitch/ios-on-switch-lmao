// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../pack.h"
#include "../variants.h"
#include "SwitchGM20BContext.h"

namespace {

// A pack of one pipeline whose fragment stage has `variants` variants, its header saying `version`
std::string writePack(uint32_t version, uint32_t variants, const char *name)
{
    std::vector<uint8_t> file(0x600, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = version;
    h.pipelines = 1;
    h.section[gm20b::PackPipelines] = {0x200, sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackVariants] = {0x500, variants * sizeof(gm20b::PackVariant)};
    h.chipFacts = gm20b::AssumeTexsF16;
    memcpy(file.data(), &h, sizeof h);
    gm20b::PackPipeline pp = {};
    pp.keyHash = 7;
    pp.variantCount[1] = uint8_t(variants);
    memcpy(file.data() + 0x200, &pp, sizeof pp);
    for (uint32_t i = 0; i < variants; i++) {
        const gm20b::PackVariant v = {gm20b::AssumeUniformAligned << i, 70000 + i};
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
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = 3;
    h.section[gm20b::PackPipelines] = {0x200, 3 * sizeof(gm20b::PackPipeline)};
    const char strings[] = "\0 | tests=2\0 | cubic s0:0000n t0:80";
    h.section[gm20b::PackStrings] = {0xc00, sizeof strings};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0xc00, strings, sizeof strings);
    gm20b::PackPipeline pp[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        pp[i].keyHash = 7;
        pp[i].vs = 70000 + i;
        pp[i].flags = i ? gm20b::PipelineVariantRecord : 0;
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
    std::vector<gm20b::PackInitMethod> init;
    for (uint32_t i = 0; i < SWITCH_GM20B_CONTEXT_METHODS; i++) {
        const switch_gm20b_context_method &m = switch_gm20b_context[i];
        init.push_back({m.subc, m.mthd, m.reloc ? fill(i) : m.data, m.reloc});
    }
    std::vector<uint8_t> file(0x200 + init.size() * sizeof(gm20b::PackInitMethod), 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.section[gm20b::PackInit] = {0x200, init.size() * sizeof(gm20b::PackInitMethod)};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0x200, init.data(), init.size() * sizeof(gm20b::PackInitMethod));
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

// The context init as Device::emitContext pushes it: each method with its word relocated to `here`
std::vector<uint32_t> pushedInit(const gm20b::Pack &pack, const gm20b::InitAddresses &here)
{
    std::vector<uint32_t> out;
    const gm20b::PackInitMethod *im = pack.section<gm20b::PackInitMethod>(gm20b::PackInit);
    for (uint64_t i = 0; i < pack.bytes(gm20b::PackInit) / sizeof(gm20b::PackInitMethod); i++) {
        out.push_back(uint32_t(im[i].subc) << 16 | im[i].mthd);
        out.push_back(gm20b::relocated(im[i], here));
    }
    return out;
}

// The words a pack's init holds in its relocated methods (the probe's addresses once, 0 from the kit) never reach the
// GPU: each takes this process's address or limit, the interlock's ticket buffer among them
TEST(GM20BPack, TheContextInitPushesThisProcesssAddressesWhateverThePackHolds)
{
    gm20b::Pack zero, garbage;
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
    const gm20b::InitAddresses here = {0x10'0000'1000, 0x20'0000'2000, 0x30'0000'3000, 0x40'0000'4000,
                                       0x50'0000'5000, 8191,           1023,           0x60'0000'6000};
    const std::vector<uint32_t> pushed = pushedInit(zero, here);
    EXPECT_EQ(pushed, pushedInit(garbage, here));
    ASSERT_EQ(pushed.size(), 2u * SWITCH_GM20B_CONTEXT_METHODS);
    const std::map<uint32_t, uint32_t> expected = {
        {gm20b::RelocHeapHi, 0x10},      {gm20b::RelocHeapLo, 0x1000}, {gm20b::RelocZeroHi, 0x20},
        {gm20b::RelocZeroLo, 0x2000},    {gm20b::RelocRootHi, 0x30},   {gm20b::RelocRootLo, 0x3000},
        {gm20b::RelocTicHi, 0x40},       {gm20b::RelocTicLo, 0x4000},  {gm20b::RelocTicMax, 8191},
        {gm20b::RelocTscHi, 0x50},       {gm20b::RelocTscLo, 0x5000},  {gm20b::RelocTscMax, 1023},
        {gm20b::RelocTicketsLo, 0x6000}, {gm20b::RelocTicketsHi, 0x60}};
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

TEST(GM20BPack, AStatesVariantRecordsAreFoundByNameNeverAsTheState)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("variants.bin").c_str(), 0, err)) << err;
    ASSERT_NE(pack.find(7), nullptr);
    EXPECT_EQ(pack.find(7)->vs, 70000u);
    EXPECT_EQ(pack.find(7, gm20b::PackForm::Barrier), nullptr);
    EXPECT_EQ(pack.variantRecords(7).size(), 2u);
    ASSERT_NE(pack.variantRecord(7, " | tests=2"), nullptr);
    EXPECT_EQ(pack.variantRecord(7, " | tests=2")->vs, 70001u);
    EXPECT_EQ(pack.variantRecord(7, " | cubic s0:0000n t0:80")->vs, 70002u);
    EXPECT_EQ(pack.variantRecord(7, " | tests=3"), nullptr);
    EXPECT_EQ(pack.variantRecord(8, " | tests=2"), nullptr);
    const std::string key = "c0=80:15:0:1:0:0:1:0:0 | v a | f b | l RenderBox | cubic s0:0000n t0:80 | tests=4";
    EXPECT_EQ(key.substr(gm20b::variantNameAt(key)), " | cubic s0:0000n t0:80 | tests=4");
    EXPECT_EQ(gm20b::variantNameAt("c0=80 | v a | f b"), 17u);
}

// A state's records under one key: its storage form, its barrier twin, and its fetch-free form that reads another colour
// behind a barrier (a barrier form's flags too)
std::string writeFormsPack(const char *name)
{
    std::vector<uint8_t> file(0x1000, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = 3;
    h.section[gm20b::PackPipelines] = {0x200, 3 * sizeof(gm20b::PackPipeline)};
    memcpy(file.data(), &h, sizeof h);
    gm20b::PackPipeline pp[3] = {};
    const uint32_t flags[3] = {gm20b::PipelineFetches | gm20b::PipelineStorageMetadata,
                               gm20b::PipelineFetches | gm20b::PipelineBarrierForm,
                               gm20b::PipelineFetches | gm20b::PipelineBarrierForm | gm20b::PipelineFetchFreeForm};
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

TEST(GM20BPack, AFetchFreeFormThatReadsBehindABarrierIsFoundOnlyAsTheFetchFreeForm)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeFormsPack("forms.bin").c_str(), 0, err)) << err;
    ASSERT_NE(pack.find(9), nullptr);
    EXPECT_EQ(pack.find(9)->vs, 80000u);
    ASSERT_NE(pack.find(9, gm20b::PackForm::Barrier), nullptr);
    EXPECT_EQ(pack.find(9, gm20b::PackForm::Barrier)->vs, 80001u);
    ASSERT_NE(pack.find(9, gm20b::PackForm::FetchFree), nullptr);
    EXPECT_EQ(pack.find(9, gm20b::PackForm::FetchFree)->vs, 80002u);
    EXPECT_EQ(gm20b::recordForm(*pack.find(9, gm20b::PackForm::FetchFree)), gm20b::PackForm::FetchFree);
}

// T-v1 iconservicesagent[322]: it made primitive_image's state (17:55:29.836) and its cubic variant (17:55:30.681), none of
// them in the pack; a later pipeline object of that state got the made record from Device::pipeline, was taken as packed,
// and its variant was refused as one the pack lacks (17:55:44.477), though the process had made it 14 s before.
TEST(GM20BPack, ARecordAProcessMadeIsNotThePacks)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("holds.bin").c_str(), 0, err)) << err;
    EXPECT_TRUE(gm20b::packHolds(pack, pack.find(7)));
    EXPECT_TRUE(gm20b::packHolds(pack, pack.variantRecord(7, " | tests=2")));
    const gm20b::PackPipeline made = *pack.find(7);
    EXPECT_FALSE(gm20b::packHolds(pack, &made));
    EXPECT_FALSE(gm20b::packHolds(pack, nullptr));
    EXPECT_FALSE(gm20b::packHolds(pack, pack.find(7) + 3));
}

// A pack of records of `keys` in that order, and `fast` as its PackFastKeys section
std::string writeKeys(const std::vector<uint64_t> &keys, const std::vector<gm20b::PackFastKey> &fast, const char *name)
{
    std::vector<uint8_t> file(0x1000, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = uint32_t(keys.size());
    h.section[gm20b::PackPipelines] = {0x200, keys.size() * sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackFastKeys] = {0xF00, fast.size() * sizeof(gm20b::PackFastKey)};
    memcpy(file.data(), &h, sizeof h);
    for (size_t i = 0; i < keys.size(); i++) {
        gm20b::PackPipeline pp = {};
        pp.keyHash = keys[i];
        pp.fs = gm20b::kNoProgram;
        memcpy(file.data() + 0x200 + i * sizeof pp, &pp, sizeof pp);
    }
    memcpy(file.data() + 0xF00, fast.data(), fast.size() * sizeof(gm20b::PackFastKey));
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(PackFastKeys, AFastKeyFindsItsStatesRecordsWithNoSearch)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeKeys({3, 7, 7, 9}, {{11, 0, 1}, {12, 1, 2}, {13, 3, 1}}, "fast.pack").c_str(), 0, err))
        << err;
    const auto state = [&pack](uint64_t fast) {
        const gm20b::PackRun run = pack.fastRun(fast);
        return std::make_pair(run.records ? run.first->keyHash : 0, run.records);
    };
    EXPECT_EQ(state(11), std::make_pair(uint64_t(3), 1u));
    EXPECT_EQ(state(12), std::make_pair(uint64_t(7), 2u));
    EXPECT_EQ(state(13), std::make_pair(uint64_t(9), 1u));
    EXPECT_EQ(state(10), std::make_pair(uint64_t(0), 0u));
    EXPECT_EQ(state(14), std::make_pair(uint64_t(0), 0u));
    EXPECT_EQ(pack.run(7).first, pack.fastRun(12).first);
    EXPECT_EQ(pack.run(8).records, 0u);
}

TEST(GM20BPack, RefusesAPackOfAnotherVersion)
{
    gm20b::Pack pack;
    std::string err;
    EXPECT_FALSE(pack.load(writePack(22, 0, "v22.bin").c_str(), 0, err));
    EXPECT_EQ(err, "a version-22 pack: this runtime reads version " + std::to_string(gm20b::kPackVersion) + " only");
}

TEST(GM20BPack, ReadsAStagesVariantsInOrder)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(gm20b::kPackVersion, 2, "current.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.header()->chipFacts, gm20b::AssumeTexsF16);
    const gm20b::PackPipeline *pp = pack.find(7);
    ASSERT_NE(pp, nullptr);
    uint32_t count = 0;
    EXPECT_EQ(pack.variants(*pp, 0, count), nullptr);
    EXPECT_EQ(count, 0u);
    const gm20b::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(v[0].program, 70000u);
    EXPECT_EQ(v[1].assume, gm20b::AssumeUniformAligned << 1);
}

TEST(GM20BPack, RefusesVariantsPastTheirSection)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(gm20b::kPackVersion, 0, "short.bin").c_str(), 0, err)) << err;
    gm20b::PackPipeline pp = *pack.find(7);
    pp.variantCount[1] = 1;
    uint32_t count = 1;
    EXPECT_EQ(pack.variants(pp, 1, count), nullptr);
    EXPECT_EQ(count, 0u);
}

TEST(GM20BVariants, TheFirstVariantWhoseAssumptionsHoldIsDrawn)
{
    const gm20b::PackVariant v[2] = {{gm20b::AssumeUniformAligned | gm20b::AssumeTexsF16, 1},
                                     {gm20b::AssumeUniformAligned, 2}};
    EXPECT_EQ(gm20b::chooseVariant(v, 2, gm20b::AssumeUniformAligned | gm20b::AssumeTexsF16), 0u);
    EXPECT_EQ(gm20b::chooseVariant(v, 2, gm20b::AssumeUniformAligned | gm20b::AssumeUniformAligned << 1), 1u);
    EXPECT_EQ(gm20b::chooseVariant(v, 2, gm20b::AssumeUniformAligned << 1), 2u);
    EXPECT_EQ(gm20b::chooseVariant(nullptr, 0, ~0u), 0u);
}

TEST(GM20BVariants, FloatAndExtendedRangeTexelsAreNotFixedPoint)
{
    EXPECT_TRUE(gm20b::fixedPointTexels(80));    // BGRA8Unorm
    EXPECT_TRUE(gm20b::fixedPointTexels(71));    // RGBA8Unorm_sRGB
    EXPECT_FALSE(gm20b::fixedPointTexels(115));  // RGBA16Float
    EXPECT_FALSE(gm20b::fixedPointTexels(552));  // BGRA10_XR
    EXPECT_FALSE(gm20b::fixedPointTexels(0));
}

// A pack listing two archives (keys {3, 9} and {5}) and the compute states it holds kernels of ({11})
std::string writeArchivePack(const char *name)
{
    std::vector<uint8_t> file(0x400, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    const uint32_t counts[2] = {3, 4};
    const gm20b::PackArchive archives[3] = {{gm20b::kPackHeldKernels, 0, 1}, {0x20, 1, 2}, {0x40, 3, 1}};
    const uint64_t keys[4] = {11, 3, 9, 5};
    h.section[gm20b::PackArchives] = {0x200, sizeof counts + sizeof archives + sizeof keys};
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

TEST(GM20BPack, AnArchiveDeclaresItsOwnKeysAlone)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeArchivePack("archives.bin").c_str(), 0, err)) << err;
    const gm20b::PackArchive *first = pack.archive(0x20), *second = pack.archive(0x40);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(pack.archive(0x30), nullptr) << "an archive the pack does not list";
    EXPECT_TRUE(pack.declares(*first, 3));
    EXPECT_TRUE(pack.declares(*first, 9));
    EXPECT_FALSE(pack.declares(*first, 5)) << "the next archive's key";
    EXPECT_FALSE(pack.declares(*first, 11)) << "a held kernel's key";
    EXPECT_TRUE(pack.declares(*second, 5));
    const gm20b::PackArchive *held = pack.archive(gm20b::kPackHeldKernels);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(pack.declares(*held, 11));
    EXPECT_FALSE(pack.declares(*held, 3));
}

// wide_index_pack.py's pack, written by the pack writer's codec: every program index past 65,535 reads back whole
TEST(GM20BPack, ProgramIndicesPast16BitsReadBackAsThePackWriterWroteThem)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load("drivers/TegraGM20BMetal/test/wide_index.bin", 0, err)) << err;
    const std::string key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v wide_vertex  | f wide_fragment";
    const gm20b::PackPipeline *pp = pack.find(gm20b::Pack::fnv(key.data(), key.size()));
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->vs, 70000u);
    EXPECT_EQ(pp->fs, 70001u);
    uint32_t count = 0;
    const gm20b::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(v[0].program, 70002u);
    const std::string kernel = "kernel | wide_kernel";
    const gm20b::PackKernel *k = pack.findKernel(gm20b::Pack::fnv(kernel.data(), kernel.size()));
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->program, 70003u);
    EXPECT_EQ(k->regionWord, -1);
    const gm20b::PackSplit *split = pack.split(70001);
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->fs, 70004u);
    EXPECT_EQ(split->prologue, 70005u);
    EXPECT_EQ(split->frontierBytes, 16u);
    EXPECT_EQ(split->frontierCbuf, 2u);
    EXPECT_EQ(pack.split(70001 & 0xFFFF), nullptr);
}

// wide_index_pack.py's second program: the root table and 11 uniform buffers, then its registers and bound textures' slot
TEST(GM20BPack, AProgramOfTwelveConstantBuffersReadsBackWhole)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load("drivers/TegraGM20BMetal/test/wide_index.bin", 0, err)) << err;
    ASSERT_EQ(pack.bytes(gm20b::PackPrograms), 2 * sizeof(gm20b::PackProgram));
    const gm20b::PackProgram &p = pack.section<gm20b::PackProgram>(gm20b::PackPrograms)[1];
    ASSERT_EQ(p.cbufs, 12u);
    EXPECT_EQ(p.cbufType[0], gm20b::CbufRoot);
    for (uint32_t c = 1; c < 12; c++) {
        EXPECT_EQ(gm20b::cbufKind(p.cbufType[c]), gm20b::CbufUbo);
        EXPECT_EQ(gm20b::cbufUboOffset(p.cbufType, nullptr, c), 16 * (c - 1));
    }
    EXPECT_EQ(p.cbufType[12], 0u);
    EXPECT_EQ(p.gprs, 40u);
    EXPECT_EQ(p.boundTextures, 2u);
}

TEST(GM20BPack, APackWithoutArchiveListsListsNone)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("no-archives.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.archive(gm20b::kPackHeldKernels), nullptr);
    EXPECT_EQ(pack.archive(0x20), nullptr);
}

}  // namespace
