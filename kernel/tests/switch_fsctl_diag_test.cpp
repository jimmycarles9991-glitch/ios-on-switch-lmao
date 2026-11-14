// <switch/fsctl_diag.h>: what a refused APFS fsctl's file says in its first bytes (the reference is the snapshot an iOS 27
// simulator's SplashBoard wrote, which APFS marks purgeable), and that both kernels report every filesystem fsctl after
// the filesystem answered, with nothing in a release kernel.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch/fsctl_diag.h"

using testing::HasSubstr;

namespace {

// The first 0x48 bytes of an iOS 27 simulator SplashBoard snapshot (1668x2420 ASTC 4x4, LZFSE inside).
const uint8_t kSplashBoardSnapshot[0x48] = {
    0x41, 0x41, 0x50, 0x4c, 0x0d, 0x0a, 0x1a, 0x0a, 0x54, 0x00, 0x00, 0x00, 0x48, 0x45, 0x41, 0x44, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x93, 0x00, 0x00,
    0x08, 0x19, 0x00, 0x00, 0x84, 0x06, 0x00, 0x00, 0x74, 0x09, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

// The first 0x2c bytes of ImageIO's org.khronos.ktx for a 720x1280 sRGB image (RGBA16F).
const uint8_t kKtx[0x2c] = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a, 0x01, 0x02, 0x03,
    0x04, 0x0b, 0x14, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x19, 0x00, 0x00, 0x1a, 0x88,
    0x00, 0x00, 0x08, 0x19, 0x00, 0x00, 0xd0, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
};

std::string Describe(const uint8_t *b, size_t n, size_t size = 96)
{
    std::string out(size, '\0');
    const int written = switch_fsctl_describe_content(b, n, out.data(), out.size());
    EXPECT_GE(written, 0);
    out.resize(std::min(static_cast<size_t>(written), size - 1));
    return out;
}

TEST(SwitchFsctlDiag, AnAppleTextureGivesItsFormatSizeAndInternalLzfse)
{
    EXPECT_EQ(Describe(kSplashBoardSnapshot, sizeof(kSplashBoardSnapshot)),
              "AAPL texture format 0x93b0 1668x2420 lzfse 1");
}

TEST(SwitchFsctlDiag, AKtxGivesItsInternalFormatAndSize)
{
    EXPECT_EQ(Describe(kKtx, sizeof(kKtx)), "KTX 1 texture internal format 0x881a 720x1280");
}

TEST(SwitchFsctlDiag, AnythingElseGivesItsFirstBytes)
{
    const uint8_t other[12] = {0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 1, 2, 3, 4};
    EXPECT_EQ(Describe(other, sizeof(other)), "bytes 62706c6973743030");
    EXPECT_EQ(Describe(other, 3), "3 bytes");
    EXPECT_EQ(Describe(kSplashBoardSnapshot, 0x40), "bytes 4141504c0d0a1a0a");
}

TEST(SwitchFsctlDiag, TheTextStaysInsideItsBuffer)
{
    for (size_t size : {1u, 5u, 20u}) {
        std::string out(size + 4, 'Z');
        switch_fsctl_describe_content(kSplashBoardSnapshot, sizeof(kSplashBoardSnapshot), out.data(), size);
        EXPECT_EQ(out[size - 1], '\0');
        EXPECT_EQ(out[size], 'Z');
    }
}

TEST(SwitchFsctlDiag, ThePurgeableMarkHasItsOwnBudgetAndEachEndIsToldOnce)
{
    const unsigned long mark = 0xc0304a6f, other = 0x40084a47;
    for (uint32_t n = 0; n < SWITCH_FSCTL_MARK_LINES; n++)
        EXPECT_EQ(switch_fsctl_line_for(mark, n), SWITCH_FSCTL_FULL) << n;
    EXPECT_EQ(switch_fsctl_line_for(mark, SWITCH_FSCTL_MARK_LINES), SWITCH_FSCTL_LAST);
    EXPECT_EQ(switch_fsctl_line_for(mark, SWITCH_FSCTL_MARK_LINES + 1), SWITCH_FSCTL_NONE);
    for (uint32_t n = 0; n < SWITCH_FSCTL_OTHER_LINES; n++)
        EXPECT_EQ(switch_fsctl_line_for(other, n), SWITCH_FSCTL_BRIEF) << n;
    EXPECT_EQ(switch_fsctl_line_for(other, SWITCH_FSCTL_OTHER_LINES), SWITCH_FSCTL_LAST);
    EXPECT_EQ(switch_fsctl_line_for(other, 4000), SWITCH_FSCTL_NONE);
    const std::string source = switch_ios::test::SourceText("../switch/vfs/fsctl_diag.c");
    EXPECT_THAT(source, HasSubstr("? &fsctl_diag_marks"));
}

TEST(SwitchFsctlDiagSource, BothKernelsReportAFilesystemFsctlAfterItsAnswer)
{
    for (const char *tree : {"../xnu-10063.141.1", "../xnu-12377.121.6"}) {
        const std::string syscalls = switch_ios::test::SourceText(std::string(tree) + "/bsd/vfs/vfs_syscalls.c");
        const std::string fsctl = switch_ios::test::Body(syscalls, "\nfsctl_internal(");
        EXPECT_THAT(fsctl, HasSubstr("\t\terror = VNOP_IOCTL(vp, cmd, data, (int)options, ctx);\n"
                                     "\t\tswitch_fsctl_failed(vp, cmd, data, error, ctx);\n"))
            << tree;
        EXPECT_THAT(syscalls, HasSubstr("#include <switch/fsctl_diag.h>")) << tree;
    }
}

TEST(SwitchFsctlDiagSource, ReleaseKernelsCompileItToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/fsctl_diag.h");
    EXPECT_THAT(header, HasSubstr("#else\nstatic inline void switch_fsctl_failed("));
    const std::string source = switch_ios::test::SourceText("../switch/vfs/fsctl_diag.c");
    EXPECT_THAT(source, HasSubstr("\n#if DEVELOPMENT || DEBUG\n"));
    EXPECT_THAT(source, HasSubstr("\n#endif /* DEVELOPMENT || DEBUG */\n"));
}

}  // namespace
