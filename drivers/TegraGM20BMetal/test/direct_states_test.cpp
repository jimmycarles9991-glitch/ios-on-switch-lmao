// SPDX-License-Identifier: MIT
// pack/direct_states.h eachState over this Mac's Metal: every corpus line either makes a state or reaches `drop` with what
// of it made none, which directpack.mm records as a refusal the link report lists.
#import <Metal/Metal.h>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include "../pack/direct_states.h"

namespace gm20b {

// A bundle library's states are keyed by their functions' AIR (TegraGM20BMetalRenderKey.mm, over the frontend); no line here is one
bool renderCacheKey(MTLRenderPipelineDescriptor *, const std::string &, bool, uint64_t, CacheKey &, FunctionSource)
{
    return false;
}

}  // namespace gm20b

namespace {

constexpr const char *kSource = "#include <metal_stdlib>\n"
                                "using namespace metal;\n"
                                "vertex float4 v_main(uint i [[vertex_id]]) { return float4(float(i)); }\n"
                                "fragment half4 f_main() { return half4(1.0h); }\n"
                                "[[patch(quad, 4)]] vertex float4 patch_main(float2 uv [[position_in_patch]]) {\n"
                                "    return float4(uv, 0, 1);\n"
                                "}\n";
const std::string kHead = "R ok=1 label=com.apple.coreanimation.T samples=1 topo=3 ds=0/0 c0=80:15:0:1:0:0:1:0:0";

struct Seen {
    std::vector<std::string> made, keys;
    std::vector<std::pair<std::string, std::string>> dropped;
};

Seen eachOf(const std::vector<std::string> &lines)
{
    Seen seen;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev)
        return seen;
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(kSource) options:nil error:&error];
    if (!library)
        return seen;
    probe::eachState(
        dev, library, nil, nil, lines,
        [&](const std::string &line, bool, probe::State &s) {
            seen.made.push_back(line);
            seen.keys.push_back(s.key);
        },
        [&](const std::string &line, const std::string &why) {
            seen.dropped.emplace_back(line, why);
        });
    return seen;
}

TEST(EachState, ALineOfItsLibrarysFunctionsMakesAState)
{
    const Seen seen = eachOf({kHead + " | v v_main | f f_main"});
    ASSERT_EQ(seen.made.size(), 1u);
    EXPECT_TRUE(seen.dropped.empty());
}

// TegraGM20BMetalPipelineKey.h keys a tessellation state's tessellator and an amplification count above 1: a line holding them makes
// the state they name, keyed as the runtime keys it
TEST(EachState, ALineOfATessellatorAndAnAmplificationCountMakesTheirState)
{
    const std::string head = kHead + " t=1:0:1:1:2:16:1 amp=2";
    const Seen seen = eachOf({head + " | v patch_main | f f_main"});
    ASSERT_EQ(seen.made.size(), 1u);
    EXPECT_NE(seen.keys[0].find(" t=1:0:1:1:2:16:1 amp=2 "), std::string::npos) << seen.keys[0];
    const Seen bad = eachOf({kHead + " t=1:0 | v patch_main | f f_main"});
    ASSERT_EQ(bad.dropped.size(), 1u);
    EXPECT_EQ(bad.dropped[0].second, "its tessellator field t=1:0 does not parse");
}

TEST(EachState, ALineWhoseFunctionItsLibraryLacksIsDroppedNamingIt)
{
    const Seen seen = eachOf({kHead + " | v fixed_vert_lph_gen | f f_main", kHead + " | v v_main | f no_frag"});
    EXPECT_TRUE(seen.made.empty());
    ASSERT_EQ(seen.dropped.size(), 2u);
    EXPECT_EQ(seen.dropped[0].second, "its library makes no vertex function fixed_vert_lph_gen");
    EXPECT_EQ(seen.dropped[1].second, "its library makes no fragment function no_frag");
    EXPECT_EQ(seen.dropped[1].first, kHead + " | v v_main | f no_frag");
}

TEST(EachState, ALineOfNoFunctionsOrAnUnreadSegmentIsDropped)
{
    const Seen seen = eachOf({kHead, kHead + " | v v_main | f f_main | xrs=0"});
    EXPECT_TRUE(seen.made.empty());
    ASSERT_EQ(seen.dropped.size(), 2u);
    EXPECT_EQ(seen.dropped[0].second, "no vertex or fragment segment");
    EXPECT_EQ(seen.dropped[1].second, "its segment \"xrs=0\" makes nothing");
}

TEST(EachState, AFrameworkLineWhoseLibraryDoesNotLoadIsDroppedNamingItsPath)
{
    const std::string path = "/System/Library/Frameworks/Absent.framework/default.metallib";
    const Seen seen = eachOf({"@L " + path + " " + kHead + " | v v_main | f f_main"});
    EXPECT_TRUE(seen.made.empty());
    ASSERT_EQ(seen.dropped.size(), 1u);
    EXPECT_EQ(seen.dropped[0].first, kHead + " | v v_main | f f_main");
    EXPECT_NE(seen.dropped[0].second.find(path), std::string::npos) << seen.dropped[0].second;
}

}  // namespace
