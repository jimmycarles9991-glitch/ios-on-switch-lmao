// SPDX-License-Identifier: MIT
// Guards on the facade's binary archives, which no host model runs: they are the device's own (RawMetalBinaryArchive.mm)
// and Metal adds the device's own functions to them.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

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

}  // namespace
