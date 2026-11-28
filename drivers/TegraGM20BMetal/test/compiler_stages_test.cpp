// SPDX-License-Identifier: MIT
// The shader compiler's kept translations and stages (compiler_stages.h, metal2vulkan's kept stages): states that
// differ in one descriptor field each, made once cold (nothing kept) and once after the base state with everything
// kept. A state made from kept stages is byte for byte the state made cold; a stage whose compile reads the same inputs
// is compiled once; and the table of which inputs each field reaches, per stage, is the one stated here.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
// ahead of the compiler's headers, which name a Sampler of their own
#include "../resources.h"
#include "compiler_stage_cases.h"
#include "../compiler/compiler_facts.h"
#include "../compiler/compiler_libraries.h"
#include "../compiler/compiler_stages.h"
#include "../TegraGM20BMetalOffline.h"
#include "../gm20b_cache.h"
#include "../pack.h"
#include "../shader_tests.h"
#include "../view.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace {

// The base states: the render state's and the fetching state's; each case after one changes one field of it
constexpr size_t kRenderBase = 0, kFetchBase = 17;

size_t baseOf(size_t i)
{
    return i < kFetchBase ? kRenderBase : kFetchBase;
}

struct Made {
    int32_t result = -1;
    gm20b::CacheKey key;
    std::string entry, why;
    std::vector<switch_compiler::StageInputs> stages;  // each stage compile asked, in order
    uint64_t stagesMade = 0, translationsMade = 0;
};

// The engine's compile of a prepared request (compiler_engine.mm): its libraries, its record by the direct compile
class Compiler {
public:
    explicit Compiler(bool keep) : libraries_(std::make_shared<switch_compiler::LibraryCache>(root()))
    {
        std::string why;
        EXPECT_TRUE(pack_.load(compiler_test::emptyPack(dir()).c_str(), 0, why)) << why;
        direct_ = std::make_unique<gm20b::Direct>(gm20b::PackRules::of(pack_), false, offline::translator());
        if (keep)
            direct_->keepStages(&recording_);
    }

    Made render(const std::string &line)
    {
        compiler_test::Requests made(root(), {{kLibrary, library()}});
        gm20b::CompileRequest r;
        std::string why;
        EXPECT_TRUE(made.render(line, r, why)) << why;
        return compile(r);
    }

    void reread()
    {
        libraries_ = std::make_shared<switch_compiler::LibraryCache>(root());
    }

    void registered(uint64_t client, uint64_t id)
    {
        struct Owned {
            std::string bytes;
            switch_compiler::MappedClientLibrary mapped;
        };

        auto owned = std::make_shared<Owned>();
        owned->bytes = fileBytes(library());
        owned->mapped.client = client;
        owned->mapped.id = id;
        owned->mapped.data = reinterpret_cast<const uint8_t *>(owned->bytes.data());
        owned->mapped.size = owned->bytes.size();
        mapped_ = std::shared_ptr<const switch_compiler::MappedClientLibrary>(owned, &owned->mapped);
    }

    Made compile(const gm20b::CompileRequest &r)
    {
        Made out;
        switch_compiler::SystemLibraries system(root());
        switch_compiler::Prepared p;
        if (!switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, out.why)) {
            ADD_FAILURE() << "not prepared: " << out.why;
            return out;
        }
        out.key = p.content;
        recording_.asked.clear();
        const uint64_t stages = kept_.kept().entries;
        uint64_t before[2] = {}, after[2] = {};
        nmb_kept_translations(before);
        switch_compiler::LibraryCache &cache = *libraries_;
        const std::shared_ptr<const switch_compiler::MappedClientLibrary> mapped = mapped_;
        const switch_compiler::Libraries libraries = [&cache, mapped](const gm20b::RequestLibrary &l,
                                                                      std::string &why) {
            return cache.library(l, why, l.system ? nullptr : mapped);
        };
        out.result = p.request.kind == gm20b::RequestKind::Facts
                         ? switch_compiler::makeFacts(p, libraries, out.entry, out.why)
                         : switch_compiler::makeEntry(p, libraries, *direct_, 0, out.entry, out.why);
        nmb_kept_translations(after);
        out.stages = recording_.asked;
        out.stagesMade = kept_.kept().entries - stages;
        out.translationsMade = after[0] - before[0];
        return out;
    }

private:
    // The kept stages, each compile's inputs recorded as it is asked
    struct Recording final : public gm20b::StageMemo {
        switch_compiler::CompiledStages &kept;
        std::vector<switch_compiler::StageInputs> asked;

        explicit Recording(switch_compiler::CompiledStages &k) : kept(k)
        {}

        int32_t compile(const gm20b::StageCompile &c, const gm20b::NvkStageInfo &info, const gm20b::NakLayout &layout,
                        gm20b::NvkStage &out, std::shared_ptr<const void> &held) override
        {
            asked.push_back(switch_compiler::stageInputs(c, info, layout));
            return kept.compile(c, info, layout, out, held);
        }
    };

    gm20b::Pack pack_;
    std::shared_ptr<const switch_compiler::MappedClientLibrary> mapped_;
    std::shared_ptr<switch_compiler::LibraryCache> libraries_;
    switch_compiler::CompiledStages kept_{kKept};
    Recording recording_{kept_};
    std::unique_ptr<gm20b::Direct> direct_;
};

// The parts of each stage's inputs (vertex 0, fragment 1 of the primary form) that differ from `base`'s
std::string differing(const Made &base, const Made &made, size_t stage)
{
    if (stage >= base.stages.size() || stage >= made.stages.size())
        return "(no stage)";
    std::string out;
    for (int part = 0; part < switch_compiler::StageInputs::kParts; part++)
        if (base.stages[stage].parts[part] != made.stages[stage].parts[part])
            out += std::string(out.empty() ? "" : ",") + switch_compiler::StageInputs::name(part);
    return out.empty() ? "-" : out;
}

class CompilerStages : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (root().empty() || library().empty())
            GTEST_SKIP() << "no crawl or no test library";
    }

    void TearDown() override
    {
        nmb_keep_translations(0);
    }
};

}  // namespace

TEST_F(CompilerStages, AStateOfKeptStagesIsTheStateMadeColdAndEachFieldReachesTheStagesStated)
{
    const std::vector<Case> cases = renderCases();
    std::vector<Made> cold;
    {
        nmb_keep_translations(0);
        for (const Case &c : cases) {
            Compiler compiler(false);
            cold.push_back(compiler.render(c.line));
        }
    }
    nmb_keep_translations(kKept);
    Compiler warm(true);
    std::vector<Made> made;
    for (const Case &c : cases)
        made.push_back(warm.render(c.line));
    std::string table = "\nfield | vertex inputs | fragment inputs | stages made | translations made\n";
    for (size_t i = 0; i < cases.size(); i++) {
        const Made &base = made[baseOf(i)];
        table += std::string(cases[i].name) + " | " + differing(base, made[i], 0) + " | " +
                 differing(base, made[i], 1) + " | " + std::to_string(made[i].stagesMade) + " | " +
                 std::to_string(made[i].translationsMade) + "\n";
    }
    printf("%s", table.c_str());
    for (size_t i = 0; i < cases.size(); i++) {
        const Case &c = cases[i];
        ASSERT_EQ(made[i].result, TegraGM20BCompileMade) << c.name << ": " << made[i].why;
        EXPECT_EQ(made[i].entry, cold[i].entry)
            << c.name << ": a state of kept stages differs from the state made cold";
        if (i == baseOf(i))
            continue;
        const Made &base = made[baseOf(i)];
        if (!c.vertex)
            EXPECT_EQ(differing(base, made[i], 0), "-") << c.name << ": the vertex stage's inputs" << table;
        if (!c.fragment)
            EXPECT_EQ(differing(base, made[i], 1), "-") << c.name << ": the fragment stage's inputs" << table;
        if (!c.vertex && !c.fragment)
            EXPECT_EQ(made[i].stagesMade, 0u) << c.name << ": its stages were compiled again";
        if (!c.translated)
            EXPECT_EQ(made[i].translationsMade, 0u) << c.name << ": its functions were translated again";
    }
}

TEST_F(CompilerStages, AnInlineLibraryReadAgainTranslatesAgain)
{
    const std::string line = renderCases()[kRenderBase].line;
    nmb_keep_translations(kKept);
    Compiler compiler(true);
    const Made first = compiler.render(line);
    ASSERT_EQ(first.result, TegraGM20BCompileMade) << first.why;
    EXPECT_GT(first.translationsMade, 0u);
    compiler.reread();
    const Made reread = compiler.render(line);
    ASSERT_EQ(reread.result, TegraGM20BCompileMade) << reread.why;
    EXPECT_EQ(reread.entry, first.entry);
    EXPECT_EQ(reread.translationsMade, first.translationsMade);
}

TEST_F(CompilerStages, AClientsLibraryRegisteredAgainFindsItsTranslationsAndAnotherClientsDoesNot)
{
    const std::string line = renderCases()[kRenderBase].line;
    nmb_keep_translations(kKept);
    Compiler compiler(true);
    compiler.registered(7, 1);
    const Made first = compiler.render(line);
    ASSERT_EQ(first.result, TegraGM20BCompileMade) << first.why;
    EXPECT_EQ(first.stagesMade, first.stages.size());
    EXPECT_GT(first.translationsMade, 0u);
    compiler.registered(7, 2);
    const Made again = compiler.render(line);
    ASSERT_EQ(again.result, TegraGM20BCompileMade) << again.why;
    EXPECT_EQ(again.entry, first.entry);
    EXPECT_EQ(again.stagesMade, 0u) << "the client's own library, registered again, compiled its stages again";
    EXPECT_EQ(again.translationsMade, 0u) << "and translated its functions again";
    compiler.registered(8, 1);
    const Made other = compiler.render(line);
    ASSERT_EQ(other.result, TegraGM20BCompileMade) << other.why;
    EXPECT_EQ(other.entry, first.entry);
    EXPECT_EQ(other.translationsMade, first.translationsMade)
        << "another client's library took a translation of this client's";

    const std::string bytes = fileBytes(library());
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    EXPECT_EQ([[GM20BOfflineDevice new] newLibraryWithData:data scope:0 error:&error], nil);
    EXPECT_NE(error, nil);
}

TEST_F(CompilerStages, AKernelIsTranslatedOnceForItsFactsAndEveryDispatch)
{
    const std::string bytes = fileBytes(library());
    const uint32_t shapes[][3] = {{8, 8, 1}, {8, 8, 1}, {2, 8, 1}, {16, 16, 1}};
    const uint64_t formats[] = {80, 70, 70, 115};
    std::vector<Made> cold;
    nmb_keep_translations(0);
    for (size_t i = 0; i < 4; i++) {
        Compiler compiler(false);
        cold.push_back(compiler.compile(kernelRequest(bytes, gm20b::RequestKind::Kernel, shapes[i], formats[i])));
    }
    nmb_keep_translations(kKept);
    Compiler warm(true);
    const Made facts = warm.compile(kernelRequest(bytes, gm20b::RequestKind::Facts, shapes[0], 0));
    ASSERT_EQ(facts.result, TegraGM20BCompileMade) << facts.why;
    EXPECT_EQ(facts.translationsMade, 1u);
    for (size_t i = 0; i < 4; i++) {
        const Made k = warm.compile(kernelRequest(bytes, gm20b::RequestKind::Kernel, shapes[i], formats[i]));
        ASSERT_EQ(k.result, TegraGM20BCompileMade) << k.why;
        EXPECT_EQ(k.entry, cold[i].entry) << "dispatch " << i << ": a kernel of a kept translation differs";
        EXPECT_EQ(k.translationsMade, 0u) << "dispatch " << i << " translated its kernel again";
    }
    // a kept translation is sized by what its compile left charged (the native program and its typed module), so the
    // bound holds: never by its bookkeeping alone
    uint64_t kept[2] = {};
    nmb_kept_translations(kept);
    printf("kept translations: %llu, %llu bytes\n", (unsigned long long)kept[0], (unsigned long long)kept[1]);
    EXPECT_GT(kept[1], kept[0] * 4096);
}

TEST_F(CompilerStages, ASystemLibraryIsReadOncePerProcess)
{
    auto cache = std::make_shared<switch_compiler::LibraryCache>(root());
    gm20b::RequestLibrary system;
    system.path = compiler_test::kQuartzCore;
    system.system = true;
    gm20b::RequestLibrary data;
    data.path = kLibrary;
    data.bytes = fileBytes(library());
    std::string why;
    cache->began();
    id<MTLLibrary> first = cache->library(system, why), firstData = cache->library(data, why);
    ASSERT_TRUE(first && firstData) << why;
    EXPECT_NE(offline::libraryHandle(first), nullptr);
    EXPECT_EQ(cache->library(system, why), first);
    cache->ended();
    // past the time a library of a client's data is kept once no compile runs
    std::this_thread::sleep_for(std::chrono::seconds(6));
    EXPECT_EQ(cache->library(system, why), first) << "a system library let go";
    EXPECT_NE(cache->library(data, why), firstData) << "a client's data library kept at rest";
}

TEST_F(CompilerStages, SpecializedComputeFactsPreserveConstants)
{
    const std::string bytes = fileBytes(library());
    ASSERT_FALSE(bytes.empty());
    std::map<std::string, gm20b::AirHash> hashes;
    gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes);
    ASSERT_TRUE(hashes.count("stages_specialized_kernel"));

    // Constant #0 = -1, #1 = 2, #2 = -1, #3 = 0, #4 = true, #5 = 1 (encoded as type:hex-le-bytes).
    // Any UAF of newIndexedConstantArray's wrappers corrupts #0/#1 into a large index and trips SlotPastTable.
    const std::string constants = "#0:29:ffffffff #1:29:02000000 #2:29:ffffffff #3:29:00000000 #4:53:01 #5:29:01000000";

    nmb_keep_translations(0);
    Compiler compiler(false);
    gm20b::CompileRequest r;
    r.kind = gm20b::RequestKind::Facts;
    gm20b::RequestLibrary l;
    l.path = kLibrary;
    l.bytes = bytes;
    r.libraries.push_back(l);
    gm20b::RequestStage s;
    s.tag = "k";
    s.function = s.name = "stages_specialized_kernel";
    s.specialized = true;
    s.constants = constants;
    r.stages.push_back(s);
    r.key =
        gm20b::factsCacheKey(0, gm20b::computeStateKey(hashes["stages_specialized_kernel"], constants, 0, false)).key;
    for (int round = 0; round < 8; ++round) {
        std::vector<std::vector<uint8_t>> churn(64, std::vector<uint8_t>(16, 0x22));
        (void)churn;
        const Made made = compiler.compile(r);
        ASSERT_EQ(made.result, TegraGM20BCompileMade) << "round " << round << ": " << made.why;
    }
}

namespace {

gm20b::CompileRequest textureKernelRequest(const std::string &bytes, const char *fn, uint64_t format,
                                           MTLTextureType type, MTLTextureUsage usage, uint8_t addressMap = 0)
{
    std::map<std::string, gm20b::AirHash> hashes;
    gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes);
    gm20b::CompileRequest r;
    r.kind = gm20b::RequestKind::Kernel;
    gm20b::RequestLibrary l;
    l.path = kLibrary;
    l.bytes = bytes;
    r.libraries.push_back(l);
    gm20b::RequestStage s;
    s.tag = "k";
    s.function = s.name = fn;
    r.stages.push_back(s);
    const uint32_t threads[3] = {8, type == MTLTextureTypeTextureBuffer ? 1u : 8u, 1};
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = MTLPixelFormat(format);
    d.textureType = type;
    d.usage = usage;
    gm20b::KernelDispatch dispatch(threads, {});
    dispatch.texture(0, d);
    const NmbDirectDispatch &nd = dispatch.dispatch();
    memcpy(r.kernel.threads, nd.threads, sizeof r.kernel.threads);
    r.kernel.textures.assign(nd.textures, nd.textures + nd.textureCount);
    std::string tex = "0:" + std::to_string(format) + ":" + std::to_string(uint64_t(type)) + ":1:1:02030405";
    if (addressMap)
        tex += ":am" + std::to_string(unsigned(addressMap));
    r.key = "kernel | " + gm20b::computeKey(hashes[fn], "", 0, false, threads, "", tex, "");
    return r;
}

uint32_t slotTextureWindow(const Made &made, uint8_t slot)
{
    gm20b::ComputeRecord record;
    std::string refusal;
    EXPECT_EQ(gm20b::decodeKernelEntry(made.entry, made.key, record, refusal), gm20b::RecordCache::Found::Record)
        << refusal;
    for (uint32_t i = 0; i < record.kernel.entries; ++i)
        if (record.kernel.entry[i].kind == gm20b::KernelTexture && record.kernel.entry[i].slot == slot)
            return record.kernel.entry[i].window;
    return 0;
}

}  // namespace

// A kernel's float read of each stand-in format binds, by the window its record names, the header the dispatch takes
// under the format's own row (compute.cpp, view.h slotView): BGRA4Unorm and ARGB8Unorm their own; R8Unorm_sRGB its own
// sRGB header, which decodes as a draw's fetch of it does; ARGB8Unorm_sRGB its codes through a UNORM view, which m2v
// decodes in the shader (a device decodes the first three channels before the swizzle: Metal's alpha, red and green);
// the extended-range sRGB twins their codes, which the kernel reads at float precision, not the half stand-in a draw
// samples
TEST_F(CompilerStages, StandInFormatKernelReadsBindTheirOwnHeadersOrCodes)
{
    const std::string bytes = fileBytes(library());
    nmb_keep_translations(0);
    Compiler compiler(false);

    const struct {
        uint64_t format;
        uint32_t window;
        gm20b::SlotView view;
    } cases[] = {
        {44, 0x54e24912u, gm20b::SlotView::Own},                             // BGRA4Unorm
        {84, 0x2b1a4908u, gm20b::SlotView::Own},                             // ARGB8Unorm
        {85, 0x2b1a4908u, gm20b::SlotView::View},                            // ARGB8Unorm_sRGB: its codes
        {11, 0x7012491du | gm20b::kKernelWindowSrgb, gm20b::SlotView::Own},  // R8Unorm_sRGB
        {553, 0x58d49203u, gm20b::SlotView::Own},                            // BGRA10_XR_sRGB
        {555, 0x6014920fu, gm20b::SlotView::Own},                            // BGR10_XR_sRGB
    };

    for (const auto &c : cases) {
        const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(c.format));
        ASSERT_NE(f, nullptr) << c.format;
        const Made made = compiler.compile(
            textureKernelRequest(bytes, "stages_kernel", c.format, MTLTextureType2D, MTLTextureUsageShaderRead));
        ASSERT_EQ(made.result, TegraGM20BCompileMade) << c.format << ": " << made.why;
        const uint32_t window = slotTextureWindow(made, 0);
        EXPECT_EQ(window, c.window) << c.format;
        EXPECT_EQ(gm20b::slotView(window, f->tic0, f->tic0, f->bpp, false, f->srgb), c.view) << c.format;
    }
}

TEST_F(CompilerStages, ExtendedRangeAndYcbcrTexelBuffersCompileDirect)
{
    const std::string bytes = fileBytes(library());
    nmb_keep_translations(0);
    Compiler compiler(false);

    const struct {
        const char *fn;
        uint64_t format;
        MTLTextureUsage usage;
        uint8_t addressMap;
        uint32_t tic0;
        uint32_t bpp;
    } cases[] = {
        {"stages_texel_write", 552, MTLTextureUsageShaderWrite, 0, 0x58d49203u, 8},
        {"stages_texel_write", 553, MTLTextureUsageShaderWrite, 0, 0x58d49203u, 8},
        {"stages_texel_write", 554, MTLTextureUsageShaderWrite, 0, 0x6014920fu, 4},
        {"stages_texel_write", 555, MTLTextureUsageShaderWrite, 0, 0x6014920fu, 4},
        {"stages_texel_read", 552, MTLTextureUsageShaderRead, 0, 0x58d49203u, 8},
        {"stages_texel_read", 553, MTLTextureUsageShaderRead, 0, 0x58d49203u, 8},
        {"stages_texel_read", 554, MTLTextureUsageShaderRead, 0, 0x6014920fu, 4},
        {"stages_texel_read", 555, MTLTextureUsageShaderRead, 0, 0x6014920fu, 4},
        {"stages_texel_read", 500, MTLTextureUsageShaderRead, 1, 0x58d7ff83u, 8},
        {"stages_texel_read", 500, MTLTextureUsageShaderRead, 3, 0x58d24908u, 4},
        {"stages_texel_read", 503, MTLTextureUsageShaderRead, 0, 0x58d7ff83u, 8},
        {"stages_texel_read", 540, MTLTextureUsageShaderRead, 3, 0x58d24908u, 4},
        {"stages_texel_read", 588, MTLTextureUsageShaderRead, 0, 0x58d24909u, 4},
        {"stages_texel_read", 589, MTLTextureUsageShaderRead, 0, 0x58d24909u, 4},
    };

    for (const auto &c : cases) {
        const Made made = compiler.compile(
            textureKernelRequest(bytes, c.fn, c.format, MTLTextureTypeTextureBuffer, c.usage, c.addressMap));
        ASSERT_EQ(made.result, TegraGM20BCompileMade) << c.fn << " fmt " << c.format << ": " << made.why;
        const uint32_t window = slotTextureWindow(made, 0);
        EXPECT_EQ(window, c.tic0) << c.fn << " fmt " << c.format;
        EXPECT_EQ(gm20b::slotView(window, c.tic0, c.tic0, c.bpp, false), gm20b::SlotView::Own)
            << c.fn << " fmt " << c.format;
    }
}

TEST_F(CompilerStages, FetchDepthOnlyStencilWriteCompilesWithShaderStencilTest)
{
    nmb_keep_translations(0);
    Compiler compiler(false);
    const std::string fetchDiscard = " | f stages_fetch_discard_fragment lib=" + std::string(kLibrary);
    for (const char *ds : {"ds=252/0", "ds=250/0"}) {
        const std::string line =
            state(std::string("samples=1 topo=0 ") + ds, "c0=80:15:0:1:0:0:1:0:0", kAttributes, fetchDiscard) +
            " | tests=4";
        const Made made = compiler.render(line);
        ASSERT_EQ(made.result, TegraGM20BCompileMade) << ds << ": " << made.why;
        gm20b::RuntimeRecord record;
        std::string refusal;
        ASSERT_EQ(gm20b::decodeEntry(made.entry, made.key, record, refusal), gm20b::RecordCache::Found::Record)
            << ds << ": " << refusal;
        EXPECT_EQ(record.pipeline.shaderTests.flags, gm20b::kShaderTestDepth | gm20b::kShaderTestStencil) << ds;
        EXPECT_EQ(record.pipeline.storageTests, 4u) << ds;
    }
}

// Fetching states of vertex amplification in Weather's VFX particles' colour layouts (maxVertexAmplificationCount 2, a
// blended RGBA16Float, RGBA8Unorm or RG11B10Float colour 0, a Depth32Float_Stencil8 attachment), drawn one amplification
// at a time, each into its layer: a function no test can drop a write of takes the storage form under early tests, and
// one that discards the storage form of each class of tests a draw asks
TEST_F(CompilerStages, AnAmplifiedFetchStateTakesTheStorageForm)
{
    nmb_keep_translations(0);
    Compiler compiler(false);
    const std::string fetchDiscard = " | f stages_fetch_discard_fragment lib=" + std::string(kLibrary);
    const std::string head = "samples=1 topo=3 ds=260/260 amp=2";
    for (const char *colours :
         {"c0=115:15:1:1:5:0:1:5:0", "c0=70:15:1:1:5:0:1:5:0",
          "c0=92:15:1:1:5:0:1:5:0 c1=115:15:0:1:0:0:1:0:0 c2=70:15:0:1:0:0:1:0:0 c3=115:15:0:1:0:0:1:0:0"}) {
        for (uint32_t tests = 0; tests <= 4; tests++) {
            const std::string line =
                tests ? state(head, colours, kAttributes, fetchDiscard) + " | tests=" + std::to_string(tests)
                      : state(head, colours, kAttributes, kFetch);
            const Made made = compiler.render(line);
            ASSERT_EQ(made.result, TegraGM20BCompileMade) << colours << " tests=" << tests << ": " << made.why;
            gm20b::RuntimeRecord record;
            std::string refusal;
            ASSERT_EQ(gm20b::decodeEntry(made.entry, made.key, record, refusal), gm20b::RecordCache::Found::Record)
                << colours << " tests=" << tests << ": " << refusal;
            EXPECT_EQ(record.pipeline.storageMask, 1u) << colours << " tests=" << tests;
            EXPECT_EQ(record.pipeline.storageTests, tests) << colours;
            EXPECT_EQ(record.pipeline.amplificationMax, 2u) << colours << " tests=" << tests;
            EXPECT_TRUE(gm20b::layeredRecord(uint8_t(record.pipeline.arrayLengthWord)))
                << colours << " tests=" << tests;
        }
    }
}

// A multisampled fetching state of vertex amplification: its draws' layered form has no multisampled storage, so it
// fetches through its input attachment, beside a depth and stencil attachment or none, and its record still draws each
// amplification into its layer
TEST_F(CompilerStages, AMultisampledAmplifiedFetchStateReadsItsInputAttachment)
{
    nmb_keep_translations(0);
    Compiler compiler(false);
    for (const char *head : {"samples=4 topo=3 ds=0/0 amp=2", "samples=4 topo=3 ds=260/260 amp=2"}) {
        const Made made = compiler.render(state(head, "c0=70:15:1:1:5:0:1:5:0", kAttributes, kFetch));
        ASSERT_EQ(made.result, TegraGM20BCompileMade) << head << ": " << made.why;
        gm20b::RuntimeRecord record;
        std::string refusal;
        ASSERT_EQ(gm20b::decodeEntry(made.entry, made.key, record, refusal), gm20b::RecordCache::Found::Record)
            << head << ": " << refusal;
        EXPECT_TRUE(record.pipeline.flags & gm20b::PipelineFetches) << head;
        EXPECT_EQ(record.pipeline.storageMask, 0u) << head;
        EXPECT_EQ(record.pipeline.amplificationMax, 2u) << head;
        EXPECT_TRUE(gm20b::layeredRecord(uint8_t(record.pipeline.arrayLengthWord))) << head;
    }
}
