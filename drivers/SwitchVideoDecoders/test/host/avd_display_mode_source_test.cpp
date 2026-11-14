// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// An HEVC session picks its display buffer by hevc_display_mode_for (hevc_host tests the choice), and a mode-3 session
// tags its pictures Rec.709, which is what NVDEC's Rec.2020 -> Rec.709 path writes.
TEST(AVDDisplayMode, AnHEVCSessionPicksItsDisplayBufferByTheStreamsPrimaries)
{
    const std::string start = Body(
        SourceText("../../AVD/AVDDecoder.c"),
        "static OSStatus startHEVC(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)");
    EXPECT_THAT(start, HasSubstr("CFEqual(d->primaries, kCVImageBufferColorPrimaries_ITU_R_2020)"));
    EXPECT_THAT(start, HasSubstr("hevc_display_mode_for(sps->bit_depth, rec2020, hdr,"));
    EXPECT_THAT(start, HasSubstr("if (displayMode == 3)\n        retagRec709(d);"));
    EXPECT_THAT(Body(SourceText("../../AVD/HEVCSetup.c"), "void hevc_fill_setup("),
                HasSubstr("(hevc_display_buffer(d) ? d->display_mode : 1)"));
}
}  // namespace
