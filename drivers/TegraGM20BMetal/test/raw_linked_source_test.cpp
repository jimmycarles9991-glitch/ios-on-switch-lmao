// SPDX-License-Identifier: MIT
// Guards on a render pipeline whose functions call linked functions by name (iOS 27's RenderBox: each icon effect is made with
// MTLFunctionDescriptor.specializedName custom_fn or custom_blend and linked through fragmentLinkedFunctions.privateFunctions),
// which no host model runs (m2v's link_tests run its linking):
//  - the frontend hands each stage's vertexLinkedFunctions/fragmentLinkedFunctions to the bridge as it hands a compute
//    descriptor's linkedFunctions, and the bridge gives them to m2v's function descriptors;
//  - a function keeps the specialized name it was made under: Metal's name answers it, the bridge links the function by it,
//    and the journal, the pack key and the AIR lookups name the library function it was made from;
//  - the C and Rust forms of the descriptor list the same fields in the same order;
//  - the raw path's twin links the twins of those functions, and its key names each, so the pack holds such a state.
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

TEST(RawLinkedSource, TheFrontendHandsEachStagesLinkedFunctionsToTheBridge)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    const std::string render = Body(frontend, "static bool renderDescriptor(MTLRenderPipelineDescriptor *d,");
    EXPECT_THAT(render, HasSubstr("stageLinked(d.vertexLinkedFunctions, linked,"));
    EXPECT_THAT(render, HasSubstr("stageLinked(d.fragmentLinkedFunctions, linked,"));
    for (const char *field :
         {"p->vertex_linked =", "p->vertex_private_linked =", "p->fragment_linked =", "p->fragment_private_linked ="})
        EXPECT_THAT(render, HasSubstr(field));
    const std::string stage = Body(frontend, "static bool stageLinked(MTLLinkedFunctions *functions,");
    EXPECT_THAT(stage, HasSubstr("linkedHandles(functions.functions, linked)"));
    EXPECT_THAT(stage, HasSubstr("linkedHandles(functions.privateFunctions, linked)"));
    // the async path makes the state on a worker only when every function, linked ones too, is this device's
    EXPECT_THAT(Body(frontend, "static bool asyncRenderPipelineWanted("),
                HasSubstr("d.vertexLinkedFunctions, d.fragmentLinkedFunctions"));

    const std::string bridge = SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs");
    const std::string convert = Body(bridge, "fn render_descriptor<F: DescriptorFunction>(");
    EXPECT_THAT(convert, HasSubstr("stage_function().with_linked(vertex_linked)"));
    EXPECT_THAT(convert, HasSubstr("stage_function().with_linked(fragment_linked)"));
}

TEST(RawLinkedSource, AFunctionKeepsTheSpecializedNameItWasMadeUnder)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    const std::string described = Body(
        frontend, "- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error");
    EXPECT_THAT(described, HasSubstr("specializedName:d.specializedName"));
    const std::string made = Body(frontend, "                       specializedName:(NSString *)specialized");
    EXPECT_THAT(made, HasSubstr("nmb_function_new_specialized(_handle, name.UTF8String, specialized.UTF8String,"));
    EXPECT_THAT(made, HasSubstr("function.name = specialized ?: name;"));
    EXPECT_THAT(made, HasSubstr("function.libraryName = name;"));
    // the journal names the library function, and the specialized name beside it
    EXPECT_THAT(Body(frontend, "static std::string stageText(NBFunction *f)"), HasSubstr("\" as=\""));
    EXPECT_THAT(Body(frontend, "static std::string renderPipelineLine("), HasSubstr("line += linkedText(d);"));

    const std::string bridge = SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs");
    EXPECT_THAT(Body(bridge, "impl DescriptorFunction for NmbFunction {"),
                HasSubstr("self.0.linked_function().with_link_name(self.1.clone())"));
}

std::vector<std::string> fields(const std::string &body, const std::regex &field)
{
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), field); it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1]);
    return out;
}

TEST(RawLinkedSource, TheDescriptorsCAndRustFormsAgree)
{
    const std::string c = Between(SourceText("../../NativeMetal/bridge/NativeMetalObjects.h"),
                                  "#define NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS(Function)", "\n\n");
    const std::string rust = Between(SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs"),
                                     "pub struct NmbRenderPipelineDescriptorOf<F> {", "\n}");
    const std::vector<std::string> namesC = fields(c, std::regex(R"((\w+_(?:linked|count));)"));
    EXPECT_EQ(namesC, fields(rust, std::regex(R"(pub (\w+_(?:linked|count)):)")));
    EXPECT_EQ(namesC.size(), 8u);
}

TEST(RawLinkedSource, TheRawRecordIsTheLinkedFunctionsToo)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    const std::string twin = Body(states, "MTLRenderPipelineDescriptor *twinDescriptor(");
    EXPECT_THAT(twin, HasSubstr("t.vertexLinkedFunctions = twinLinked(d.vertexLinkedFunctions);"));
    EXPECT_THAT(twin, HasSubstr("t.fragmentLinkedFunctions = twinLinked(d.fragmentLinkedFunctions);"));
    EXPECT_THAT(Body(states, "RMPipeline *rmPipelineShell("), HasSubstr("raw_metal_linked_packable(d)"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalLibrary.mm"), "NSString *raw_metal_pipeline_key("),
                HasSubstr("raw::pipelineKey(d, vf ? &vertex : nullptr, ff ? &fragment : nullptr, linkedKey(d),"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawPipelineKey.h"), "inline std::string pipelineKey("),
                HasSubstr("key += linked;"));

    EXPECT_THAT(Body(SourceText("../raw/runtime/RawComplete.mm"), "bool Completion::keyOf(Ask &a, CacheKey &k)"),
                HasSubstr("renderCacheKey(a.facade ?: a.descriptor, a.key, a.own, stack_, k, a.facade ? "
                          "rmFunctionSource : nullptr)"));
    const std::string key = SourceText("../raw/runtime/RawRenderKey.mm");
    EXPECT_THAT(key, HasSubstr("descriptor.vertexLinkedFunctions"));
    EXPECT_THAT(key, HasSubstr("descriptor.fragmentLinkedFunctions"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_render_key.h"), "inline CacheKey renderCacheKey("),
                HasSubstr("key += \" | linked\";"));
    // a specialized function's AIR is its library function's entry
    EXPECT_THAT(key, HasSubstr("native_bridge_metal_function_library_name(function)"));
}

// The pack generator files a state under the key the runtime computes of the descriptor it made, never under its corpus
// line's text, so a line written otherwise than the device journals it (a constant's width, a "spec" mark) still hits.
TEST(RawLinkedSource, ThePacksKeysAreTheRuntimes)
{
    const std::string parse = Body(SourceText("../raw/pack/probe_states.h"), "inline bool parseState(");
    EXPECT_THAT(parse,
                HasSubstr("s.key = raw::pipelineKey(d, &vertex, d.fragmentFunction ? &fragment : nullptr, linked,"));
    EXPECT_THAT(Body(SourceText("../raw/pack/probe_states.h"), "inline id<MTLFunction> function("),
                HasSubstr("key.constants = raw::constantsText(values);"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalLibrary.mm"), "- (RMFunction *)wrap:"),
                HasSubstr("f->_constants = raw::constantsText(values);"));
}

// A state the pack holds is this device's archive hit: every way the facade asks m2v for its twin (at once, with
// reflection; a completion handler's creation finishes as the synchronous one does) drops the creation's archives and
// FailOnBinaryArchiveMiss, so a command buffer that falls back never meets a twin m2v refused (device boot 10: "foreign
// render pipeline", 833 failed frames).
TEST(RawLinkedSource, APackedStatesTwinNeverFailsOnTheArchive)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Body(states, "static MTLRenderPipelineDescriptor *packedTwinDescriptor("),
                HasSubstr("t.binaryArchives = nil;"));
    EXPECT_THAT(Body(states, "static MTLPipelineOption packedTwinOptions("),
                HasSubstr("o & ~MTLPipelineOptionFailOnBinaryArchiveMiss"));
    EXPECT_THAT(Body(states, "static id<MTLRenderPipelineState> pipelineTwin("),
                HasSubstr("packedTwinOptions(o, true)"));
    const std::string finish = Body(states, "id<MTLRenderPipelineState> rmFinishState(");
    EXPECT_THAT(finish, HasSubstr("packedTwinOptions(o, packed)"));
    EXPECT_THAT(finish, HasSubstr("pipelineTwin(d, o, packed, error)"));
    EXPECT_THAT(Body(states, "static id<MTLRenderPipelineState> makeState("), HasSubstr("rmFinishState(p, d, o"));
    const std::string later = SourceText("../raw/runtime/RawMetalStatesLater.mm");
    EXPECT_THAT(Body(later, "static void finishState("), HasSubstr("rmFinishState(p, d, o"));
    for (const char *handlerType : {"(MTLNewRenderPipelineStateCompletionHandler)handler",
                                    "(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler"}) {
        const std::string handler = Between(later, std::string("completionHandler:") + handlerType, "\n}\n");
        EXPECT_THAT(handler, HasSubstr("createLater("));
        EXPECT_THAT(handler, HasSubstr("self, [d copy]"));
    }
}

}  // namespace
