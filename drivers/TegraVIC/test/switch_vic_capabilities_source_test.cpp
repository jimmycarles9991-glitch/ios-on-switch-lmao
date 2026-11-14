// SPDX-License-Identifier: BSD-2-Clause-Patent
// The capability dictionary answers what the job builder does and the probes judge, not less.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(TegraVICCapabilities, FractionalRectanglesAndPackedYccArePublished)
{
    const std::string caps =
        Body(SourceText("../TegraVICUserClient.cpp"), "OSDictionary *capabilities(const TegraVICTraits *t)\n");
    EXPECT_THAT(caps, HasSubstr("putNumber(d, \"IOSurfaceAcceleratorCapabilitiesFractionalXYOffsetsSupported\", 1);"));
    EXPECT_THAT(caps, HasSubstr("putNumber(d, \"IOSurfaceAcceleratorCapabilitiesFractionalDimensions\", 1);"));
    EXPECT_THAT(caps, HasSubstr("{\"Interleaved422\", t->packedYcc}"));
}
