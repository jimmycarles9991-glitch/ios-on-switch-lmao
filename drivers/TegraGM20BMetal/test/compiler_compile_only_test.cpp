// SPDX-License-Identifier: MIT
// The system shader compiler's compile context (compiler_engine.h over bridge/NativeMetalCompile.h) against the m2v device
// it replaced, each in a process of its own over hostsim's mock GM20B and NVK's host twin: the context's enable set and
// compile devices are a runtime device's, and each record the engine makes of a sample of the corpus's states, render and
// compute, is byte for byte the one made of the m2v device's libraries by its direct compile on its own Vulkan device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalCompile.h"
#include "TegraGM20BCompilerABI.h"
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "../compiler/compiler_engine.h"
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_pack.h"

#include <gtest/gtest.h>
#include <fstream>
#include <regex>

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string loader()
{
    char path[PATH_MAX];
    const char *nvk = getenv("SWITCH_NVK");
    return nvk && realpath(nvk, path) ? path : "";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

void environment()
{
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
}

// The sample: every corpus state linking functions, a few of each other kind, a stride through the rest, a RenderBox
// state's cubic and test-class variants as the device asked for them, multisampled VectorKit ground states, kernels at
// an edge of their grid, Core Animation states that may fetch their attachment over a depth attachment and multisampled
// (their storage forms), and a state of the function the build's libraries refuse (SWITCH_REFUSED_FRAGMENT_FUNCTION).
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
        samplers(R"( smp=\S)"), quartzKernel("^D .*QuartzCore"), atOrigin("edge=0,0,0 threads=0");
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
    }
    return out;
}

std::vector<raw::CompileRequest> requests()
{
    compiler_test::Requests made(root());
    return made.of(sample());
}

// Each request's result and entry, `compile` making them, as `<name>.bin`: per request a u32 result and the entry
template <typename Compile> void writeEntries(const char *name, Compile compile)
{
    switch_compiler::SystemLibraries system(root());
    std::ofstream out(dir() + "/" + name + ".bin", std::ios::binary);
    for (const raw::CompileRequest &r : requests()) {
        switch_compiler::Prepared p;
        std::string entry, why;
        int32_t result = -1;
        if (switch_compiler::prepare(raw::encodeRequest(r), 0, system, p, why))
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

void viaTheDevice()
{
    environment();
    id<MTLDevice> device = nil;
    @autoreleasepool {
        device = native_bridge_metal_open(loader().c_str(), nullptr, 0, nullptr);
    }
    ASSERT_TRUE(device != nil) << nmb_last_error();
    raw::Pack pack;
    std::string why;
    ASSERT_TRUE(pack.load(compiler_test::emptyPack(dir()).c_str(), 0, why)) << why;
    raw::Direct direct(pack);
    const switch_compiler::Libraries libraries = compiler_test::deviceLibraries(device, root());
    writeEntries("device", [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
        return switch_compiler::makeEntry(p, libraries, direct, 0, entry, failed);
    });
}

void viaTheEngine()
{
    environment();
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), loader(), root()}, why)) << why;
    writeEntries("engine", [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
        return engine.compile(p, entry, failed);
    });
}

}  // namespace

TEST(CompilerCompileOnly, TheContextCompilesAsTheRuntimeDevice)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        environment();
        NmbDevice *device = nmb_device_open(loader().c_str(), nullptr);
        ASSERT_NE(device, nullptr) << nmb_last_error();
        NmbCompileContext *context = nmb_compile_context_open(loader().c_str());
        ASSERT_NE(context, nullptr) << nmb_last_error();
        EXPECT_TRUE(nmb_compile_context_matches(context, device)) << nmb_last_error();
    });
}

TEST(CompilerCompileOnly, ItsRecordsAreTheDevicesByteForByte)
{
    ASSERT_FALSE(root().empty());
    switch_ios::test::ExpectPassesInFreshProcess(viaTheDevice);
    switch_ios::test::ExpectPassesInFreshProcess(viaTheEngine);
    const std::vector<raw::CompileRequest> asked = requests();
    const std::vector<Made> device = readEntries("device"), engine = readEntries("engine");
    ASSERT_EQ(device.size(), asked.size());
    ASSERT_EQ(engine.size(), asked.size());
    switch_compiler::SystemLibraries system(root());
    std::map<std::string, size_t> covered;
    for (size_t i = 0; i < asked.size(); ++i) {
        const raw::CompileRequest &r = asked[i];
        EXPECT_EQ(engine[i].result, device[i].result) << r.key.substr(0, 300);
        EXPECT_TRUE(engine[i].entry == device[i].entry) << "entry " << i << " differs: " << r.key.substr(0, 300);
        if (engine[i].result == TegraGM20BCompileRefused)
            covered["refused"]++;
        if (engine[i].result != TegraGM20BCompileMade)
            continue;
        switch_compiler::Prepared p;
        std::string why;
        ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), 0, system, p, why)) << why;
        if (r.kind == raw::RequestKind::Kernel) {
            covered["kernel"]++;
            covered["kernel with textures and samplers"] += !r.kernel.textures.empty() && !r.kernel.samplers.empty();
            covered["region kernel"] += r.kernel.regionMode;
            continue;
        }
        raw::RuntimeRecord record;
        ASSERT_EQ(raw::decodeEntry(engine[i].entry, p.key, record, why), raw::RecordCache::Found::Record) << why;
        covered["render"]++;
        covered["constexpr samplers"] += !record.samplers.empty();
        covered["storage form"] += (record.pipeline.flags & raw::PipelineStorageMetadata) != 0;
        covered["cubic"] += !r.cubic.empty();
        covered["test class"] += r.fragmentTests != 0;
        covered["multisampled"] += r.fields.samples > 1;
        covered["specialized"] += std::any_of(r.stages.begin(), r.stages.end(), [](const raw::RequestStage &s) {
            return !s.constants.empty();
        });
        covered["private linked"] += std::any_of(r.stages.begin(), r.stages.end(), [](const raw::RequestStage &s) {
            return s.tag == "fp" || s.tag == "vp";
        });
    }
    for (const char *kind : {"render", "constexpr samplers", "storage form", "cubic", "test class", "multisampled",
                             "specialized", "kernel", "kernel with textures and samplers", "region kernel"})
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
