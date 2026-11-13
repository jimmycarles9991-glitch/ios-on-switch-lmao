// SPDX-License-Identifier: MIT
// Guards on the facade's binary archives, which no host model runs: they are the device's own (RawMetalBinaryArchive.mm),
// Metal adds the device's own functions to them, and m2v, which takes only its own functions and archives
// (NativeBridgeMetalFrontend.mm renderDescriptor and computeDescriptor refuse a foreign function), is never handed one.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(RawArchiveAddSource, M2VsArchiveTakesOnlyItsOwnFunctions)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    EXPECT_THAT(Body(frontend, "static bool renderDescriptor(MTLRenderPipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
    EXPECT_THAT(Body(frontend, "static bool computeDescriptor(MTLComputePipelineDescriptor *d,"),
                HasSubstr("isKindOfClass:[NBFunction class]"));
}

TEST(RawArchiveAddSource, TheFacadesArchiveIsTheDevicesOwnAndTakesItsFunctions)
{
    const std::string archive = SourceText("../raw/runtime/RawMetalArchive.mm");
    const std::string opened =
        Body(archive, "- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d");
    EXPECT_THAT(opened, HasSubstr("rmNewBinaryArchive("));
    const std::string own = SourceText("../raw/runtime/RawMetalBinaryArchive.mm");
    EXPECT_THAT(Body(own, "- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d"),
                HasSubstr("own(d.vertexFunction)"));
    EXPECT_THAT(Body(own, "- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d"),
                HasSubstr("own(d.computeFunction)"));
}

TEST(RawArchiveAddSource, M2vsTwinsAreMadeWithoutTheDevicesArchives)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Body(states, "MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)"),
                HasSubstr("t.binaryArchives = nil;"));
    const std::string compute = SourceText("../raw/runtime/RawMetalComputeState.mm");
    EXPECT_THAT(Body(compute, "static bool archivesAnswer("), HasSubstr("s->_direct.binaryArchives = nil;"));
}

}  // namespace
