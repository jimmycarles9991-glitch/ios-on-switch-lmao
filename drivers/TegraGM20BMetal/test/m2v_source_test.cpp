// SPDX-License-Identifier: MIT
// What the raw path relies on in m2v and NVK while they ship, which goes with them: tagged m2v, so the deletion's dry run
// (.bazelrc nom2v) leaves it out.
//  - the frontend's plain event answers a shared event's value, set and notifications (m2v's Event::host_view through
//    the bridge), which raw_events_source_test's RawMetal asks for;
//  - NVK's root table opens with cs.base_group then cs.group_count (nvk_cmd_buffer.h) and every compute shader is compiled
//    with its workgroup id based there (nvk_shader.c has_base_workgroup_id, nvk_nir_lower_descriptors.c), where
//    raw_dispatch_bands_test's launch writes a piece's base;
//  - a range that inline signals joined splits again where m2v takes it (raw_inline_signals_test's other half).
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

}  // namespace
