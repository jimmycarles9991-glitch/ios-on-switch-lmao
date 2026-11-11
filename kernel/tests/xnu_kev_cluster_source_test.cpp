// Kernel events above one mbuf: xnu-12377 (iOS 27) takes one cluster for them, so SwitchWLAN's 24B119 join report
// reaches IO80211's kernel-event reader; xnu-10063 (iOS 17) still refuses them as Apple ships it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
constexpr const char *kPost = "kev_post_msg_internal(struct kev_msg *event_msg, int wait)";
}  // namespace

TEST(XnuKevClusterSource, Xnu12377TakesOneCluster)
{
    const std::string post = Body(SourceText("../xnu-12377.121.6/bsd/kern/kern_event.c"), kPost);
    EXPECT_THAT(post, HasSubstr("if (total_size > MCLBYTES) {"));
    EXPECT_THAT(post, HasSubstr("m = total_size > MLEN ? m_getcl(wait, MT_DATA, 0) : m_get(wait, MT_DATA);"));
    EXPECT_THAT(post, Not(HasSubstr("if (total_size > MLEN) {")));
}

TEST(XnuKevClusterSource, Xnu10063Unchanged)
{
    const std::string post = Body(SourceText("../xnu-10063.141.1/bsd/kern/kern_event.c"), kPost);
    EXPECT_THAT(post, HasSubstr("if (total_size > MLEN) {"));
    EXPECT_THAT(post, HasSubstr("m = m_get(wait, MT_DATA);"));
    EXPECT_THAT(post, Not(HasSubstr("m_getcl")));
}
