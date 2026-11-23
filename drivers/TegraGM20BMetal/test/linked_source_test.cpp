// SPDX-License-Identifier: MIT
// Guards on a render pipeline whose functions call linked functions by name (iOS 27's RenderBox: each icon effect is made with
// MTLFunctionDescriptor.specializedName custom_fn or custom_blend and linked through fragmentLinkedFunctions.privateFunctions),
// which no host model runs: the driver's key names each linked function, so the pack holds such a state, and the pack
// generator files a state under the key the runtime computes; the bridge hands the translator each stage's linked
// functions, and the descriptor's C and Rust forms agree.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

TEST(GM20BLinkedSource, TheRawRecordIsTheLinkedFunctionsToo)
{
    const std::string states = SourceText("../TegraGM20BMetalStates.mm");
    EXPECT_THAT(Body(states, "SwitchMetalPipeline *gm20bPipelineShell("), HasSubstr("metal_linked_packable(d)"));
    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalLibrary.mm"), "NSString *metal_pipeline_key("),
                HasSubstr("gm20b::pipelineKey(d, vf ? &vertex : nullptr, ff ? &fragment : nullptr, linkedKey(d),"));
    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalPipelineKey.h"), "inline std::string pipelineKey("),
                HasSubstr("key += linked;"));

    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalComplete.mm"), "bool Completion::keyOf(Ask &a, CacheKey &k)"),
                HasSubstr("renderCacheKey(a.facade, a.key, a.own, stack_, k, gm20bFunctionSource)"));
    const std::string key = SourceText("../TegraGM20BMetalRenderKey.mm");
    EXPECT_THAT(key, HasSubstr("descriptor.vertexLinkedFunctions"));
    EXPECT_THAT(key, HasSubstr("descriptor.fragmentLinkedFunctions"));
    EXPECT_THAT(Body(SourceText("../render_key.h"), "inline CacheKey renderCacheKey("),
                HasSubstr("key += \" | linked\";"));
}

// The pack generator files a state under the key the runtime computes of the descriptor it made, never under its corpus
// line's text, so a line written otherwise than the device journals it (a constant's width, a "spec" mark) still hits.
TEST(GM20BLinkedSource, ThePacksKeysAreTheRuntimes)
{
    const std::string parse = Body(SourceText("../pack/probe_states.h"), "inline bool parseState(");
    EXPECT_THAT(parse,
                HasSubstr("s.key = gm20b::pipelineKey(d, &vertex, d.fragmentFunction ? &fragment : nullptr, linked,"));
    EXPECT_THAT(Body(SourceText("../pack/probe_states.h"), "inline id<MTLFunction> function("),
                HasSubstr("key.constants = gm20b::constantsText(values);"));
    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalLibrary.mm"), "- (SwitchMetalFunction *)function:"),
                HasSubstr("f->_constants = gm20b::constantsText(values);"));
}

// The bridge gives each stage's linked functions to the translator's function descriptors, for the runtime's functions and
// the offline compile's alike (render_descriptor is generic over both).
TEST(GM20BLinkedSource, TheBridgeGivesEachStagesLinkedFunctionsToTheTranslator)
{
    const std::string descriptors = SourceText("../translate/NativeMetalDescriptors.rs");
    const std::string convert = Body(descriptors, "fn render_descriptor<F: DescriptorFunction>(");
    EXPECT_THAT(convert, HasSubstr("stage_function().with_linked(vertex_linked)"));
    EXPECT_THAT(convert, HasSubstr("stage_function().with_linked(fragment_linked)"));
}

std::vector<std::string> fields(const std::string &body, const std::regex &field)
{
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), field); it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1]);
    return out;
}

TEST(GM20BLinkedSource, TheDescriptorsCAndRustFormsAgree)
{
    const std::string c = Between(SourceText("../translate/NativeMetalTranslate.h"),
                                  "#define NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS(Function)", "\n\n");
    const std::string rust = Between(SourceText("../translate/NativeMetalDescriptors.rs"),
                                     "pub struct NmbRenderPipelineDescriptorOf<F> {", "\n}");
    const std::vector<std::string> namesC = fields(c, std::regex(R"((\w+_(?:linked|count));)"));
    EXPECT_EQ(namesC, fields(rust, std::regex(R"(pub (\w+_(?:linked|count)):)")));
    EXPECT_EQ(namesC.size(), 8u);
}

}  // namespace
