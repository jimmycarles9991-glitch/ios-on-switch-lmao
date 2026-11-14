// SPDX-License-Identifier: MIT
// One state line (a corpus R or D line) through the system shader compiler's compile context over hostsim's mock GM20B and
// NVK's host twin, as the device's compiler made it: its result and why. SWITCH_REPRO_LINE names the state; a stage's
// lib=/repro.metallib reads SWITCH_REPRO_LIBRARY, a library a client made from source (the HW Test's). Manual: a probe
// of one device failure, run with --test_env=SWITCH_REPRO_LINE=... --test_env=SWITCH_REPRO_LIBRARY=<absolute path>.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalCompile.h"
#include "TegraGM20BCompilerABI.h"
#include "compiler_requests.h"
#include "../compiler/compiler_engine.h"

#include <gtest/gtest.h>
#include <cstdio>

namespace {

std::string env(const char *name)
{
    const char *v = getenv(name);
    return v ? v : "";
}

TEST(CompilerRepro, TheLinesStateThroughTheCompileContext)
{
    const std::string line = env("SWITCH_REPRO_LINE"), library = env("SWITCH_REPRO_LIBRARY");
    if (line.empty())
        GTEST_SKIP() << "no SWITCH_REPRO_LINE";
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
    char nvk[PATH_MAX];
    ASSERT_NE(realpath(env("SWITCH_NVK").c_str(), nvk), nullptr);
    const std::string root = compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
    compiler_test::Requests made(root, {{"/repro.metallib", library}});
    const std::vector<raw::CompileRequest> asked = made.of({line});
    ASSERT_EQ(asked.size(), 1u);
    const std::string dir = env("TEST_TMPDIR");
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir), nvk, root}, why)) << why;
    switch_compiler::SystemLibraries system(root);
    switch_compiler::Prepared p;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(asked[0]), 0, system, p, why)) << why;
    std::string entry;
    const int32_t result = engine.compile(p, entry, why);
    fprintf(stderr, "repro: result %d (%s), entry %zu bytes, why: %s\n", result,
            result == TegraGM20BCompileMade      ? "made"
            : result == TegraGM20BCompileRefused ? "refused"
                                                 : "failed",
            entry.size(), why.c_str());
    EXPECT_EQ(result, TegraGM20BCompileMade) << why;
}

}  // namespace
