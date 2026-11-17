// Both kernels' boot setup runs kernel/switch/boot/icon_cache.c once, before launchd, on the record the deploy writes
// (tools/deploy/icon_cache.py), with one error line in release; only xnu-12377's walks 24B119's salt folders.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
constexpr const char *k10063 = "../xnu-10063.141.1/bsd/kern/kern_exec.c";
constexpr const char *k12377 = "../xnu-12377.121.6/bsd/kern/kern_exec.c";
constexpr const char *kHook = "native_switch_icon_cache(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kSetup = "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kError = "printf(\"SwitchRawBoot: icon cache for GPU build %s NOT dropped: %d\\n\"";
}  // namespace

TEST(XnuIconCacheSource, TheKernelReadsTheRecordTheDeployWrites)
{
    const std::string record = Between(SourceText("../../tools/deploy/icon_cache.py"), "\nRECORD = '", "'");
    ASSERT_FALSE(record.empty());
    EXPECT_THAT(SourceText("../switch/boot/icon_cache.c"),
                HasSubstr("switch_icon_cache_request[] = \"Switch/" + record + "\";"));
}

TEST(XnuIconCacheSource, EachTreeRunsItOnceBeforeLaunchdWithOneReleaseLine)
{
    for (const char *path : {k10063, k12377}) {
        const std::string source = SourceText(path);
        const std::string hook = Body(source, kHook);
        ASSERT_FALSE(hook.empty()) << path;
        EXPECT_EQ(Count(source, "native_switch_icon_cache(p, scratch, data_mount);"), 1u) << path;
        EXPECT_EQ(Count(Body(source, kSetup), "native_switch_icon_cache(p, scratch, data_mount);"), 1u) << path;
        EXPECT_EQ(Count(hook, "switch_icon_cache_drop(&ops, &result);"), 1u) << path;
        EXPECT_EQ(Count(hook, "printf("), 2u) << path;
        const size_t error = hook.find(kError), debug = hook.find("#if DEVELOPMENT || DEBUG");
        ASSERT_NE(error, std::string::npos) << path;
        EXPECT_LT(error, debug) << path << ": the error line is the one release keeps";
        EXPECT_LT(debug, hook.find("printf(", error + 1)) << path << ": every other line is a debug kernel's";
    }
}

TEST(XnuIconCacheSource, OnlyXnu12377WalksSaltFolders)
{
    const std::string salted = SourceText(k12377), top = SourceText(k10063);
    EXPECT_THAT(Body(salted, kHook), HasSubstr(".folders = native_switch_icon_cache_folders,"));
    EXPECT_THAT(salted, HasSubstr("VNOP_READDIR(folder, uio, 0, &eof, &count, vfs)"));
    EXPECT_THAT(Body(top, kHook), Not(HasSubstr(".folders")));
    EXPECT_THAT(top, Not(HasSubstr("native_switch_icon_cache_folders")));
}
