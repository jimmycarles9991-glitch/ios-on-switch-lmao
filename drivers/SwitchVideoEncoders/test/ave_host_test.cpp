// ave_host.c: AVE on this Mac's VideoToolbox over a lossless fake NVENC. It prints one "layout L chroma C: ..." line per
// input layout and chroma mode, then "PASS ave: ..." and returns 0, or "FAIL ..." lines and 1.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

extern "C" int ave_host_main(void);

TEST(SwitchVideoEncoders, AvePluginOnVideoToolbox)
{
    testing::internal::CaptureStdout();
    const int code = ave_host_main();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(code, 0) << out;
    EXPECT_NE(out.find("PASS ave"), std::string::npos) << out;
    // Each route that is to code every frame (three layouts the CPU copies into, VIC writing BL2 from on-grid and
    // bounced off-grid rows; the two kernels refusing surfaces fail those frames instead): every frame by the hardware encoder, decoded back, sync samples
    // where the encode check asks for them, exact (PSNR 99 dB is the check's "identical").
    EXPECT_EQ(switch_ios::test::Count(out, " ok 30/30 frames encoded (0 errors) by a hardware encoder, 30 decoded (0 "
                                           "errors), sync samples 0 10 15 25, PSNR min Y 99.0 U 99.0 V 99.0 dB"),
              5u)
        << out;
    EXPECT_NE(out.find("ok layout probe: 8 of 8 layouts, 8 decode back exactly"), std::string::npos) << out;
}
