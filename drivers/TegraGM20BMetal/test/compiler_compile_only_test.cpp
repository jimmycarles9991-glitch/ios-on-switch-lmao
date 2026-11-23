// SPDX-License-Identifier: MIT
// The system shader compiler's engine (compiler_engine.h, the translator alone: TegraGM20BMetalOffline.h), run in a
// process of its own: it makes a record of every kind a sample of the corpus's states asks for, render and compute,
// refusing only the build's refused function, and each sampled kernel's and render state's reflection is the one this
// Mac's Metal gives the same functions.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "TegraGM20BCompilerABI.h"
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "../compiler/compiler_engine.h"
#include "../compiler/compiler_record.h"
#include "../gm20b_cache.h"
#include "../compile_request.h"
#include "../pack.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../TegraGM20BMetalReflection.h"
#include "../reflection_blob.h"
#include "reflection_compare.h"

#include <gtest/gtest.h>
#include <fstream>
#include <regex>
#include <set>

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

// The sample: every corpus state linking functions, a few of each other kind, a stride through the rest, a RenderBox
// state's cubic and test-class variants as the device asked for them, the written-format variants the corpus names, Core
// Animation state into b3a8 amplified, states blending an extended-range colour, multisampled VectorKit ground states,
// kernels at an edge of their grid, Core Animation states that may fetch their attachment over a depth attachment and
// multisampled (their storage forms), and a state of the function the build's libraries refuse
// (SWITCH_REFUSED_FRAGMENT_FUNCTION).
// Each kind is drawn from lines both crawls' libraries hold, but for the private linked functions, which only iOS 27's
// libraries have.
std::vector<std::string> sample()
{
    const std::vector<std::string> lines = compiler_test::corpusLines(getenv("SWITCH_CORPUS"));
    std::vector<std::string> out;
    std::map<std::string, size_t> taken;
    const auto take = [&](const std::string &kind, const std::string &line, size_t most) {
        if (taken[kind] < most && std::find(out.begin(), out.end(), line) == out.end()) {
            out.push_back(line);
            taken[kind]++;
        }
    };
    const std::regex linked(R"( \| (fp|vp|fl|vl) )"), msaa("samples=[24] "), depth("ds=(252|260)/"),
        samplers(R"( smp=\S)"), quartzKernel("^D .*QuartzCore"), atOrigin("edge=0,0,0 threads=0"),
        blendedXr(" c0=55[0-5]:15:1:");
    const auto atAnEdge = [&](const std::string &l) {
        return std::regex_replace(l, atOrigin, "edge=7,3,0 threads=1");
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        const bool regionable = std::regex_search(l, atOrigin);
        if (std::regex_search(l, linked))
            take("linked", l, 8);
        if (l[0] == 'R' && std::regex_search(l, msaa))
            take("msaa", l, 4);
        if (l[0] == 'R' && std::regex_search(l, depth))
            take("depth", l, 6);
        if (l[0] == 'R' && l.find("samples=4 ") != std::string::npos &&
            l.find("| v DaVinci::ground_base_vertex ") != std::string::npos)
            take("VectorKit ground", l, 2);
#ifdef SWITCH_REFUSED_FRAGMENT_FUNCTION
        if (l[0] == 'R' && l.find("| f " SWITCH_REFUSED_FRAGMENT_FUNCTION " ") != std::string::npos)
            take("refused", l, 1);
#endif
        if (l[0] == 'D' && std::regex_search(l, samplers)) {
            take("samplers", l, 6);
            if (regionable && l.find(" lib=/System/") != std::string::npos)
                take("sampler kernel region", atAnEdge(l), 1);
        }
        if (std::regex_search(l, quartzKernel)) {
            take("kernel", l, 4);
            if (regionable)
                take("region", atAnEdge(l), 1);
        }
        if (i % 41 == 0)
            take("stride", l, 64);
        if (l.find("fc_framebuffer_fetch:53:01") != std::string::npos &&
            l.find("samples=1 topo=0 ds=0/0 ") != std::string::npos) {
            take("depth fetch", std::regex_replace(l, std::regex("ds=0/0 "), "ds=252/0 "), 3);
            take("multisampled fetch", std::regex_replace(l, std::regex("samples=1 "), "samples=4 "), 3);
        }
        if (l.find("| v primitive_image_vertex lib=/System/Library/PrivateFrameworks/RenderBox") != std::string::npos &&
            l.find("ds=260/260") != std::string::npos) {
            take("cubic", l + " | cubic s0:0000n t0:80", 1);
            take("tests", l + " | tests=3", 1);
        }
        if (l[0] == 'R' && l.find(" | wtex=") != std::string::npos)
            take("written", l, 2);
        if (l[0] == 'R' && std::regex_search(l, blendedXr))
            take("extended range", l, 8);
        if (l[0] == 'R' && l.find(" c0=550:") != std::string::npos && l.find("QuartzCore") != std::string::npos) {
            const std::string amplified =
                std::regex_replace(l, std::regex(" \\| v "), " amp=2 | v ", std::regex_constants::format_first_only);
            take("amplified", amplified, 1);
        }
    }
    return out;
}

std::vector<gm20b::CompileRequest> requests()
{
    compiler_test::Requests made(root());
    return made.of(sample());
}

// Each request's result and entry, `compile` making them, as `<name>.bin`: per request a u32 result and the entry
template <typename Compile> void writeEntries(const char *name, Compile compile)
{
    switch_compiler::SystemLibraries system(root());
    std::ofstream out(dir() + "/" + name + ".bin", std::ios::binary);
    for (const gm20b::CompileRequest &r : requests()) {
        switch_compiler::Prepared p;
        std::string entry, why;
        int32_t result = -1;
        if (switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, why))
            result = compile(p, entry, why);
        const uint32_t head[2] = {uint32_t(result), uint32_t(entry.size())};
        out.write(reinterpret_cast<const char *>(head), sizeof head);
        out.write(entry.data(), std::streamsize(entry.size()));
    }
}

struct Made {
    int32_t result;
    std::string entry;
};

std::vector<Made> readEntries(const char *name)
{
    std::vector<Made> out;
    std::ifstream in(dir() + "/" + name + ".bin", std::ios::binary);
    for (uint32_t head[2]; in.read(reinterpret_cast<char *>(head), sizeof head);) {
        Made m{int32_t(head[0]), std::string(head[1], '\0')};
        in.read(m.entry.data(), std::streamsize(head[1]));
        out.push_back(std::move(m));
    }
    return out;
}

void viaTheEngine()
{
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), root(), {}}, why)) << why;
    writeEntries("engine", [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
        return engine.compile(p, entry, failed);
    });
}

// Each sampled kernel's reflection as the shader compiler makes it (a Reflection request: the translator alone) and as
// this Mac's Metal gives it for the same function and pipeline descriptor, selector by selector
void reflectionsAsMetal()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), root(), {}}, why)) << why;
    const switch_compiler::Libraries libraries = compiler_test::deviceLibraries(device, root());
    switch_compiler::SystemLibraries system(root());
    std::set<std::string> states;
    for (gm20b::CompileRequest r : requests()) {
        switch_compiler::Prepared kernel;
        if (r.kind != gm20b::RequestKind::Kernel || !r.stages[0].constants.empty() ||
            !switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, kernel, why))
            continue;
        const std::string state = kernel.packKey.substr(0, kernel.packKey.find(" | tg="));
        if (!states.insert(state).second)
            continue;
        id<MTLLibrary> library = libraries(r.libraries[r.stages[0].library], why);
        ASSERT_TRUE(library != nil) << why;
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library newFunctionWithName:@(r.stages[0].function.c_str())];
        d.maxTotalThreadsPerThreadgroup = r.kernel.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = r.kernel.multipleOfWidth;
        MTLComputePipelineReflection *metal = nil;
        NSError *error = nil;
        ASSERT_TRUE([device newComputePipelineStateWithDescriptor:d
                                                          options:MTLPipelineOptionArgumentInfo
                                                       reflection:&metal
                                                            error:&error] != nil)
            << state << ": " << (error ? error.localizedDescription.UTF8String : "");
        r.kind = gm20b::RequestKind::Reflection;
        r.key = gm20b::computeReflectionKey(0, state).key;
        switch_compiler::Prepared p;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, why)) << why;
        std::string entry, blob;
        ASSERT_EQ(engine.compile(p, entry, why), TegraGM20BCompileMade) << state << ": " << why;
        bool made = false;
        ASSERT_TRUE(gm20b::decodeReflectionEntry(entry, p.key, made, blob) && made) << state << ": " << blob;
        id raw = gm20bReflectionOf(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), true);
        ASSERT_TRUE(raw != nil) << state;
        reflection_compare::expectSameList(metal.bindings, [raw bindings], state);
    }
    EXPECT_FALSE(states.empty()) << "the sample has no kernel of no constants";
    printf("%zu compute reflections compared\n", states.size());
}

// A render state as a difference names it: its functions and its key's hash
std::string stateName(const gm20b::CompileRequest &r)
{
    const size_t f = gm20b::fragmentStage(r);
    char hash[24];
    snprintf(hash, sizeof hash, " #%016llx", (unsigned long long)gm20b::Pack::fnv(r.key.data(), r.key.size()));
    return r.stages[0].function + "/" + (f == gm20b::kNoStage ? std::string("-") : r.stages[f].function) + hash;
}

// Each sampled render state's reflection as the shader compiler makes it (a Reflection request of the state's key) and
// as this Mac's Metal gives it for the same functions and descriptor fields, selector by selector: vertex, fragment and
// tile bindings, isUsed and isActive one way (reflection_compare.h Options). A state of linked functions is left out,
// and so is one this Mac's Metal does not make (a function or a field of iOS alone), counted. It prints the GPU and OS
// that answered for Metal, every state by name with its key's prefix, and the differences summarised.
void renderReflectionsAsMetal()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    printf("Metal of %s, %s\n", device.name.UTF8String,
           NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), root(), {}}, why)) << why;
    const switch_compiler::Libraries libraries = compiler_test::deviceLibraries(device, root());
    switch_compiler::SystemLibraries system(root());
    std::set<std::string> states;
    size_t unmade = 0;
    reflection_compare::Differences differences;
    const reflection_compare::Options usedOneWay{true};
    for (gm20b::CompileRequest r : requests()) {
        if (r.kind != gm20b::RequestKind::Render || gm20b::firstLinked(r) != r.stages.size() ||
            !states.insert(r.key).second)
            continue;
        std::vector<id<MTLFunction>> made;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        MTLRenderPipelineReflection *metal = nil;
        NSError *error = nil;
        if (switch_compiler::functions(r, libraries, made, why)) {
            gm20b::applyFields(r.fields, d);
            d.vertexFunction = made[0];
            if (const size_t f = gm20b::fragmentStage(r); f != gm20b::kNoStage)
                d.fragmentFunction = made[f];
        }
        const std::string state = stateName(r);
        if (!d.vertexFunction || ![device newRenderPipelineStateWithDescriptor:d
                                                                       options:MTLPipelineOptionArgumentInfo
                                                                    reflection:&metal
                                                                         error:&error]) {
            printf("not made by this Mac's Metal: %s %s: %s\n", state.c_str(), r.key.substr(0, 160).c_str(),
                   error ? error.localizedDescription.UTF8String : why.c_str());
            unmade++;
            states.erase(r.key);
            continue;
        }
        printf("compared %s %s\n", state.c_str(), r.key.substr(0, 160).c_str());
        r.kind = gm20b::RequestKind::Reflection;
        switch_compiler::Prepared p;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, why)) << state << ": " << why;
        std::string entry, blob;
        ASSERT_EQ(engine.compile(p, entry, why), TegraGM20BCompileMade) << state << ": " << why;
        bool reflected = false;
        ASSERT_TRUE(gm20b::decodeReflectionEntry(entry, p.key, reflected, blob) && reflected) << state << ": " << blob;
        id raw = gm20bReflectionOf(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), false);
        ASSERT_TRUE(raw != nil) << state;
        differences.state = state;
        reflection_compare::compareRender(metal, raw, usedOneWay, differences);
    }
    EXPECT_FALSE(states.empty()) << "this Mac's Metal made none of the sample's render states";
    printf("%zu render reflections compared, %zu states this Mac's Metal did not make, %zu differences; %zu bindings "
           "raw answers used and Metal's compiler unused (not judged):\n%s",
           states.size(), unmade, differences.found.size(), differences.usedOnlyByRaw.size(),
           reflection_compare::Differences::summary(differences.usedOnlyByRaw).c_str());
    EXPECT_TRUE(differences.found.empty()) << differences.found.size() << " differences\n"
                                           << reflection_compare::Differences::summary(differences.found);
}

}  // namespace

TEST(CompilerCompileOnly, AKernelsReflectionIsMetalsSelectorForSelector)
{
    ASSERT_FALSE(root().empty());
    switch_ios::test::ExpectPassesInFreshProcess(reflectionsAsMetal);
}

TEST(CompilerCompileOnly, ARenderStatesReflectionIsMetalsSelectorForSelector)
{
    ASSERT_FALSE(root().empty());
    switch_ios::test::ExpectPassesInFreshProcess(renderReflectionsAsMetal);
}

TEST(CompilerCompileOnly, ItMakesARecordOfEveryKindTheSampleAsks)
{
    ASSERT_FALSE(root().empty());
    switch_ios::test::ExpectPassesInFreshProcess(viaTheEngine);
    const std::vector<gm20b::CompileRequest> asked = requests();
    const std::vector<Made> engine = readEntries("engine");
    ASSERT_EQ(engine.size(), asked.size());
    switch_compiler::SystemLibraries system(root());
    std::map<std::string, size_t> covered;
    const std::regex blendedXr(" c0=55[0-5]:15:1:");
    for (size_t i = 0; i < asked.size(); ++i) {
        const gm20b::CompileRequest &r = asked[i];
        if (engine[i].result == TegraGM20BCompileRefused)
            covered["refused"]++;
        if (engine[i].result != TegraGM20BCompileMade)
            continue;
        switch_compiler::Prepared p;
        std::string why;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, why)) << why;
        if (r.kind == gm20b::RequestKind::Kernel) {
            covered["kernel"]++;
            covered["kernel with textures and samplers"] += !r.kernel.textures.empty() && !r.kernel.samplers.empty();
            covered["region kernel"] += r.kernel.regionMode;
            continue;
        }
        gm20b::RuntimeRecord record;
        ASSERT_EQ(gm20b::decodeEntry(engine[i].entry, p.key, record, why), gm20b::RecordCache::Found::Record) << why;
        covered["render"]++;
        covered["constexpr samplers"] += !record.samplers.empty();
        covered["storage form"] += (record.pipeline.flags & gm20b::PipelineStorageMetadata) != 0;
        covered["cubic"] += !r.variant.cubic.empty();
        covered["test class"] += r.variant.fragmentTests != 0;
        covered["written"] += !r.variant.written.empty();
        // its RGBA16Float stand-in, which the ROP blends: no storage form for the blend
        covered["extended range blended by the ROP"] +=
            std::regex_search(r.key, blendedXr) &&
            !(record.pipeline.flags & (gm20b::PipelineStorageMetadata | gm20b::PipelineBarrierForm));
        covered["amplified"] += r.fields.amplification > 1;
        covered["multisampled"] += r.fields.samples > 1;
        covered["specialized"] += std::any_of(r.stages.begin(), r.stages.end(), [](const gm20b::RequestStage &s) {
            return !s.constants.empty();
        });
        covered["private linked"] += std::any_of(r.stages.begin(), r.stages.end(), [](const gm20b::RequestStage &s) {
            return s.tag == "fp" || s.tag == "vp";
        });
    }
    for (const char *kind : {"render", "constexpr samplers", "storage form", "cubic", "test class", "written",
                             "extended range blended by the ROP", "multisampled", "specialized", "kernel",
                             "kernel with textures and samplers", "region kernel"})
        EXPECT_GT(covered[kind], 0u) << "the sample made no " << kind << " record";
#if SWITCH_LIBRARIES_LINK_PRIVATE_FUNCTIONS
    EXPECT_GT(covered["private linked"], 0u) << "the sample made no private linked record";
#endif
#ifdef SWITCH_REFUSED_FRAGMENT_FUNCTION
    EXPECT_GT(covered["refused"], 0u) << "the sample made no refused record";
#else
    EXPECT_EQ(covered["refused"], 0u) << "a request was refused on a build that names no refused state: give the build "
                                         "its SWITCH_REFUSED_FRAGMENT_FUNCTION (test/BUILD.bazel)";
#endif
    std::string counts;
    for (const auto &[kind, n] : covered)
        counts += " " + kind + "=" + std::to_string(n);
    printf("%zu requests:%s\n", asked.size(), counts.c_str());
}
