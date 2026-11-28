// SwitchUnnest runs inside vm_map_clip_unnest / vm_range_lock_pmap_unnest_and_clip with the task's vm_map lock held:
// its user frame walk must use lockless no-fault PA translation (never copyin/backtrace_user or lossy 16K atop()),
// strip PAC and Swift async FP bits, guard against invalid physical addresses and loops, and compile out in RELEASE.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Occurrences;
using switch_ios::test::SourceText;

namespace {

TEST(XnuUnnestSource, UsesNofaultPhysicalReadWithoutFaultingCopy)
{
    const std::string unnest = SourceText("../switch/diag/unnest.c");
    const CodeText read = Block(unnest, "static bool unnest_read_u64(");

    EXPECT_TRUE(read.has("pmap_find_pa_nofault(pmap, (addr64_t)va)"));
    EXPECT_FALSE(read.has("pmap_find_phys_nofault"));
    EXPECT_TRUE(read.has("(va & 7u) != 0"));
    EXPECT_TRUE(read.has("MACH_VM_MAX_ADDRESS - sizeof(*out)"));
    EXPECT_TRUE(read.before("pmap_valid_address(pa)", "phystokv(pa)"));
    EXPECT_TRUE(read.has("memcpy(&value,"));

    for (const char *forbidden : {"copyin", "copyinmap", "copyin_kern", "backtrace_user"}) {
        EXPECT_TRUE(Occurrences(unnest, forbidden).empty()) << forbidden;
    }
}

TEST(XnuUnnestSource, WalksUserFrameChainWithPacStripAndBounds)
{
    const std::string unnest = SourceText("../switch/diag/unnest.c");
    const CodeText stripFp = Block(unnest, "static uint64_t unnest_strip_fp(");

    EXPECT_TRUE(stripFp.has("UNNEST_SWIFT_ASYNC_FP_BIT"));
    EXPECT_TRUE(stripFp.has("ptrauth_strip("));
    EXPECT_TRUE(stripFp.has("ptrauth_key_frame_pointer"));

    const CodeText walk = Block(unnest, "static void unnest_walk_user(");
    EXPECT_TRUE(walk.has("is_saved_state64(user)"));
    EXPECT_TRUE(walk.has("map != current_task()->map"));
    EXPECT_TRUE(walk.has("pmap == kernel_pmap"));
    EXPECT_TRUE(walk.has("unnest_strip_fp(get_saved_state_fp(user))"));
    EXPECT_TRUE(walk.has("i < UNNEST_UFRAMES"));
    EXPECT_TRUE(walk.has("unnest_read_u64(pmap, fp, &next_fp)"));
    EXPECT_TRUE(walk.has("unnest_read_u64(pmap, fp + sizeof(uint64_t), &ret)"));
    EXPECT_TRUE(walk.has("ptrauth_key_return_address"));
    EXPECT_TRUE(walk.has("if (next_fp <= fp)"));

    const CodeText diag = Block(unnest, "void switch_diag_unnest(");
    EXPECT_TRUE(diag.has("unnest_walk_user(user, uframes, sizeof(uframes))"));
}

TEST(XnuUnnestSource, CompilesOutInReleaseBuilds)
{
    const std::string unnest = SourceText("../switch/diag/unnest.c");
    const std::string beginGuard = "#if DEVELOPMENT || DEBUG";
    const std::string endGuard = "#endif /* DEVELOPMENT || DEBUG */";

    const size_t beginPos = unnest.find(beginGuard);
    ASSERT_NE(beginPos, std::string::npos);
    EXPECT_EQ(unnest.substr(0, beginPos).find('{'), std::string::npos);

    const std::string debugOnly = Between(unnest, beginGuard, endGuard);
    EXPECT_NE(debugOnly.find("unnest_read_u64("), std::string::npos);
    EXPECT_NE(debugOnly.find("unnest_walk_user("), std::string::npos);
    EXPECT_NE(debugOnly.find("switch_diag_unnest("), std::string::npos);

    const size_t endPos = unnest.find(endGuard, beginPos);
    ASSERT_NE(endPos, std::string::npos);
    const std::string tail = unnest.substr(endPos + endGuard.size());
    EXPECT_EQ(tail.find('{'), std::string::npos);
    EXPECT_EQ(CodeText(tail).count(";"), 0u);
}

TEST(XnuUnnestSource, BothXnuTreesTranslateUserVaLocklesslyInPmapFindPaNofault)
{
    for (const char *path : {"../xnu-10063.141.1/osfmk/arm/pmap/pmap.c", "../xnu-12377.121.6/osfmk/arm/pmap/pmap.c"}) {
        const std::string pmap = SourceText(path);
        const CodeText nofault = Block(pmap, "pmap_find_pa_nofault(pmap_t pmap, addr64_t va)");
        EXPECT_TRUE(nofault.has("pmap == vm_map_pmap(current_thread()->map)")) << path;
        EXPECT_TRUE(nofault.has("pa = mmu_uvtop(va);")) << path;
        EXPECT_FALSE(nofault.has("pmap_lock")) << path;
        EXPECT_FALSE(nofault.has("pmap_vtop")) << path;
    }
}

}  // namespace
