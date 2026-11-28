// Both kernels' boot setup runs kernel/switch/boot/icon_cache.c once, before launchd, on the record the GPU stager
// writes (drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py), with one error line in release; only xnu-12377 renames
// 24B119's salt folders aside and sweeps them on System Boot Complete with zero timers.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::CodeText;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
constexpr const char *k10063 = "../xnu-10063.141.1/bsd/kern/kern_exec.c";
constexpr const char *k12377 = "../xnu-12377.121.6/bsd/kern/kern_exec.c";
constexpr const char *kPm10063 = "../xnu-10063.141.1/iokit/Kernel/IOPMrootDomain.cpp";
constexpr const char *kPm12377 = "../xnu-12377.121.6/iokit/Kernel/IOPMrootDomain.cpp";
constexpr const char *kSweep = "../switch/boot/icon_cache_sweep.c";
constexpr const char *kCore = "../switch/boot/icon_cache.c";
constexpr const char *kHook = "native_switch_icon_cache(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kSetup = "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kError = "printf(\"SwitchRawBoot: icon cache for GPU build %s NOT dropped: %d\\n\"";
}  // namespace

TEST(XnuIconCacheSource, TheKernelReadsTheRecordTheStagerWrites)
{
    const std::string stager = SourceText("../../drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py");
    const std::string record = Between(stager, "\nICON_CACHE_RECORD = '", "'");
    ASSERT_FALSE(record.empty());
    EXPECT_THAT(SourceText(kCore), HasSubstr("switch_icon_cache_request[] = \"" + record + "\";"));
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

TEST(XnuIconCacheSource, OnlyXnu12377RenamesAsideAndSweepsOnBootComplete)
{
    const std::string salted = SourceText(k12377), top = SourceText(k10063);
    const std::string saltedHook = Body(salted, kHook), topHook = Body(top, kHook);
    EXPECT_THAT(saltedHook, HasSubstr(".folders = native_switch_icon_cache_folders,"));
    EXPECT_THAT(saltedHook, HasSubstr(".mkdir = native_switch_icon_cache_mkdir,"));
    EXPECT_THAT(saltedHook, HasSubstr(".rename = native_switch_icon_cache_rename,"));
    EXPECT_THAT(saltedHook, HasSubstr(".exists = native_switch_icon_cache_exists,"));
    EXPECT_THAT(saltedHook, HasSubstr(".session = bootsessionuuid_string,"));
    EXPECT_THAT(saltedHook, HasSubstr("switch_icon_cache_sweep_arm(data_mount, result.trash_pending);"));
    EXPECT_THAT(saltedHook, Not(HasSubstr("native_switch_icon_cache_unlink")));
    EXPECT_THAT(salted, HasSubstr("VNOP_READDIR(folder, uio, 0, &eof, &count, vfs)"));
    EXPECT_THAT(Body(salted, "\nnative_switch_icon_cache_rename("), HasSubstr("RENAME_NOFOLLOW_ANY"));

    // Folders closes the directory vnode before calling visit() so rename never mutates an open readdir.
    const std::string folders = Body(salted, "\nnative_switch_icon_cache_folders(");
    const size_t closed = folders.find("vnode_close(folder, FREAD, vfs)"), visited = folders.find("visit(walk, name)");
    ASSERT_NE(closed, std::string::npos);
    ASSERT_NE(visited, std::string::npos);
    EXPECT_LT(closed, visited);

    // xnu-10063 stays untouched.
    EXPECT_THAT(topHook, HasSubstr(".unlink = native_switch_icon_cache_unlink,"));
    EXPECT_THAT(topHook, Not(HasSubstr(".folders")));
    EXPECT_THAT(topHook, Not(HasSubstr(".rename")));
    EXPECT_THAT(topHook, Not(HasSubstr("switch_icon_cache_sweep_arm")));
    EXPECT_THAT(top, Not(HasSubstr("native_switch_icon_cache_folders")));
    EXPECT_THAT(top, Not(HasSubstr("native_switch_icon_cache_rename")));

    // Only xnu-12377's IOPMrootDomain triggers the post-boot trash sweep on kPowerEventSystemBootCompleted.
    const std::string pm12377 = SourceText(kPm12377), pm10063 = SourceText(kPm10063);
    const std::string bootComplete = Between(pm12377, "case kPowerEventSystemBootCompleted:", "\n\t\tbreak;");
    EXPECT_THAT(bootComplete, HasSubstr("switch_icon_cache_boot_complete();"));
    EXPECT_EQ(Count(pm12377, "switch_icon_cache_boot_complete();"), 1u);
    EXPECT_THAT(pm10063, Not(HasSubstr("switch_icon_cache_boot_complete")));
}

TEST(XnuIconCacheSource, SweepUsesZeroTimersAndKeepsFunctionalWorkInRelease)
{
    const std::string sweep = SourceText(kSweep);
    const CodeText sweepCode(sweep);
    const CodeText coreCode(SourceText(kCore));

    for (const char *forbidden : {"thread_call", "clock_interval_to_deadline", "timeout(", "IOSleep", "IODelay",
                                  "delay(", "nanosleep", "timer_call"}) {
        EXPECT_FALSE(sweepCode.has(forbidden)) << "sweep must be strictly event-driven: " << forbidden;
        EXPECT_FALSE(coreCode.has(forbidden)) << "core must be strictly event-driven: " << forbidden;
    }

    EXPECT_TRUE(sweepCode.has("MAXPRI_THROTTLE"));
    EXPECT_TRUE(sweepCode.has("IOPOL_THROTTLE"));
    EXPECT_TRUE(sweepCode.has("vfs_context_kernel()"));
    EXPECT_TRUE(sweepCode.has("O_NOFOLLOW_ANY"));
    EXPECT_TRUE(sweepCode.has("NAMEI_NOFOLLOW_ANY"));
    EXPECT_TRUE(sweepCode.has("vnode_mount("));
    EXPECT_TRUE(sweepCode.has("switch_icon_cache_sweep(&ops, &result);"));

    // Completion timing and info log compile only under DEVELOPMENT || DEBUG; functional sweep stays in release.
    const std::string worker = Body(sweep, "static void icon_cache_sweep_worker(");
    const size_t doneLog = worker.find("trash sweep done");
    const size_t debugGate = worker.rfind("#if DEVELOPMENT || DEBUG", doneLog);
    ASSERT_NE(doneLog, std::string::npos);
    ASSERT_NE(debugGate, std::string::npos);
    EXPECT_LT(debugGate, doneLog);
    const size_t startTime = worker.find("mach_absolute_time()");
    const size_t firstDebug = worker.find("#if DEVELOPMENT || DEBUG");
    ASSERT_NE(startTime, std::string::npos);
    ASSERT_NE(firstDebug, std::string::npos);
    EXPECT_LT(firstDebug, startTime);
}
