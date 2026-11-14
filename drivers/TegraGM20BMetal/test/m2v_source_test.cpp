// SPDX-License-Identifier: MIT
// What the raw path relies on in m2v and NVK while they ship, which goes with them: tagged m2v, so the deletion's dry run
// (.bazelrc nom2v) leaves it out.
//  - the frontend's plain event answers a shared event's value, set and notifications (m2v's Event::host_view through
//    the bridge), which raw_events_source_test's RawMetal asks for;
//  - NVK's root table opens with cs.base_group then cs.group_count (nvk_cmd_buffer.h) and every compute shader is compiled
//    with its workgroup id based there (nvk_shader.c has_base_workgroup_id, nvk_nir_lower_descriptors.c), where
//    raw_dispatch_bands_test's launch writes a piece's base;
//  - a range that inline signals joined splits again where m2v takes it (raw_inline_signals_test's other half);
//  - m2v takes only its own functions and archives, and the facade's twins (archives dropped, linked functions twinned,
//    fences replayed as their twins) never hand it the device's own objects;
//  - the frontend hands each stage's linked functions to the bridge under the specialized name it was made under.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

TEST(M2VSource, TheFrontendsPlainEventAnswersASharedEventsSelectors)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    const std::string plain = Between(frontend, "@implementation NBEvent\n", "@end");
    for (const char *method : {"- (uint64_t)signaledValue", "- (void)setSignaledValue:(uint64_t)value",
                               "- (void)notifyListener:(MTLSharedEventListener *)listener"})
        EXPECT_THAT(plain, HasSubstr(method));

    const std::string bridge = SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs");
    EXPECT_THAT(Between(bridge, "fn shared(event: *const NmbEvent)", "\n}\n"),
                HasSubstr("NmbEvent::Event(e) => Ok(e.host_view()),"));
    EXPECT_THAT(SourceText("../../../third_party/m2v/crates/metal2vulkan-runtime/src/event.rs"),
                HasSubstr("pub fn host_view(&self) -> SharedEvent {"));
}

TEST(M2VSource, NvksComputeShadersReadTheirWorkgroupBaseFromTheRootTable)
{
    const std::string root = SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_cmd_buffer.h");
    EXPECT_THAT(
        root, HasSubstr("struct {\n         uint32_t base_group[3];\n         uint32_t group_count[3];\n      } cs;"));
    EXPECT_THAT(SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_shader.c"),
                HasSubstr(".has_base_workgroup_id = mesa_shader_stage_is_compute(nir->info.stage),"));
    EXPECT_THAT(SourceText("../../../third_party/mesa/src/nouveau/vulkan/nvk_nir_lower_descriptors.c"),
                HasSubstr("return lower_sysval_to_root_table(b, intrin, cs.base_group, ctx);"));
}

TEST(M2VSource, AJoinedRangeSplitsAgainWhereM2VTakesIt)
{
    const std::string submit = SourceText("../raw/runtime/RawMetalSubmit.mm");
    EXPECT_THAT(Body(submit, "bool submitRange(RMCommandBuffer *cb, bool followed)"),
                HasSubstr("const std::vector<RMSegment> parts = compatibilityParts(cb, range);"));
}

TEST(M2VSource, M2VsArchiveTakesOnlyItsOwnFunctions)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    EXPECT_THAT(Body(frontend, "static bool renderDescriptor(MTLRenderPipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
    EXPECT_THAT(Body(frontend, "static bool computeDescriptor(MTLComputePipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
}

TEST(M2VSource, M2vsTwinsAreMadeWithoutTheDevicesArchives)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Body(states, "MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)"),
                HasSubstr("t.binaryArchives = nil;"));
    const std::string compute = SourceText("../raw/runtime/RawMetalComputeState.mm");
    EXPECT_THAT(Body(compute, "static bool archivesAnswer("), HasSubstr("s->_direct.binaryArchives = nil;"));
}

TEST(M2VSource, ReplayGetsTheFencesTwin)
{
    const std::string replay = SourceText("../raw/runtime/RawMetalReplay.mm");
    EXPECT_THAT(replay, HasSubstr("[enc updateFence:twinOf((__bridge id)c.obj)];"));
    EXPECT_THAT(replay, HasSubstr("[enc waitForFence:twinOf((__bridge id)c.obj)];"));
}

TEST(M2VSource, TheFrontendHandsEachStagesLinkedFunctionsToTheBridge)
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
}

TEST(M2VSource, AFunctionKeepsTheSpecializedNameItWasMadeUnder)
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

// A state the pack holds is this device's archive hit: every way the facade asks m2v for its twin (at once, with
// reflection; a completion handler's creation finishes as the synchronous one does) drops the creation's archives and
// FailOnBinaryArchiveMiss, so a command buffer that falls back never meets a twin m2v refused (device boot 10: "foreign
// render pipeline", 833 failed frames).
TEST(M2VSource, APackedStatesTwinNeverFailsOnTheArchive)
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

TEST(M2VSource, TheTwinLinksTheTwinsOfItsLinkedFunctions)
{
    const std::string twin =
        Body(SourceText("../raw/runtime/RawMetalStates.mm"), "MTLRenderPipelineDescriptor *twinDescriptor(");
    EXPECT_THAT(twin, HasSubstr("t.vertexLinkedFunctions = twinLinked(d.vertexLinkedFunctions);"));
    EXPECT_THAT(twin, HasSubstr("t.fragmentLinkedFunctions = twinLinked(d.fragmentLinkedFunctions);"));
    // a specialized function's AIR is its library function's entry, named by its m2v twin
    EXPECT_THAT(SourceText("../raw/runtime/RawRenderKey.mm"),
                HasSubstr("native_bridge_metal_function_library_name(function)"));
}

}  // namespace
