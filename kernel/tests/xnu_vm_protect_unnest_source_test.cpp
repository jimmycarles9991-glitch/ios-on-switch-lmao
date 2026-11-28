// Both XNU trees skip clipping and unnesting a nested submap entry in vm_map_protect when the requested protection is
// already in effect.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;

namespace {

TEST(XnuVmProtectUnnestSource, Xnu12377SkipsPrepareAndCommitForSubmapNoops)
{
    const std::string src = SourceText("../xnu-12377.121.6/osfmk/vm/vm_map.c");
    const std::string preflight = Between(src, "\nvm_map_protect_preflight(", "\n/*\n *\tvm_map_protect:");
    const size_t disallow = preflight.find("map_disallow_new_exec");
    const size_t check = preflight.find("switch_vm_protect_submap_noop(", disallow);
    const size_t skip = preflight.find("return VMRL_ERR_SKIP_PREPARE;", check);

    ASSERT_NE(disallow, std::string::npos);
    ASSERT_NE(check, std::string::npos);
    ASSERT_NE(skip, std::string::npos);
    EXPECT_LT(disallow, check);
    EXPECT_LT(check, skip);

    const std::string protect = Between(src, "\nvm_map_protect(\n", "\nstatic __attribute__((always_inline");
    const size_t loopCheck = protect.find("switch_vm_protect_submap_noop(");
    const size_t loopContinue = protect.find("continue;", loopCheck);
    const size_t submapAssert = protect.find("assert(!entry->use_pmap);", loopContinue);

    ASSERT_NE(loopCheck, std::string::npos);
    ASSERT_NE(loopContinue, std::string::npos);
    ASSERT_NE(submapAssert, std::string::npos);
    EXPECT_LT(loopCheck, loopContinue);
    EXPECT_LT(loopContinue, submapAssert);
}

TEST(XnuVmProtectUnnestSource, Xnu10063SkipsClipBeforeUnnestForSubmapNoops)
{
    const std::string src = SourceText("../xnu-10063.141.1/osfmk/vm/vm_map.c");
    const std::string protect = Between(src, "\nvm_map_protect(\n", "\n/*\n *\tvm_map_inherit:");
    const size_t pass2 = protect.find("current = entry;");
    const size_t check = protect.find("switch_vm_protect_submap_noop(", pass2);
    const size_t clipStart = protect.find("vm_map_clip_start(", pass2);
    const size_t clipEnd = protect.find("vm_map_clip_end(", pass2);
    const size_t submapAssert = protect.find("assert(!current->use_pmap);", pass2);

    ASSERT_NE(pass2, std::string::npos);
    ASSERT_NE(check, std::string::npos);
    ASSERT_NE(clipStart, std::string::npos);
    ASSERT_NE(clipEnd, std::string::npos);
    ASSERT_NE(submapAssert, std::string::npos);
    EXPECT_LT(check, clipStart);
    EXPECT_LT(clipStart, clipEnd);
    EXPECT_LT(clipEnd, submapAssert);
}

}  // namespace
