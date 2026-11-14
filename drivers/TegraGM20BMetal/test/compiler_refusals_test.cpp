// SPDX-License-Identifier: MIT
// compiler/compiler_refusals.h: the refusals a compiler build made, kept beside the store per build: a refusal is this
// build's at its compile, and the warm-up makes another build's again once; a refusal is logged with its request so a
// later build can make it again.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

#include "../compiler/compiler_refusals.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_compiler::ConfirmedRefusals;
using testing::HasSubstr;

std::string directory()
{
    std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/refusals-XXXXXX";
    return mkdtemp(pattern.data()) ? pattern : std::string();
}

ConfirmedRefusals::Name name(uint8_t seed)
{
    ConfirmedRefusals::Name d{};
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = uint8_t(seed * 31 + i);
    return d;
}

TEST(CompilerRefusals, AConfirmedRefusalLastsForItsBuildAlone)
{
    const std::string dir = directory();
    ASSERT_FALSE(dir.empty());
    {
        ConfirmedRefusals build(dir, "aaaa");
        EXPECT_FALSE(build.confirmed(name(1)));
        build.confirm(name(1));
        build.confirm(name(1));
        EXPECT_TRUE(build.confirmed(name(1)));
    }
    ConfirmedRefusals again(dir, "aaaa");
    EXPECT_TRUE(again.confirmed(name(1))) << "the next boot of the same build";
    EXPECT_FALSE(again.confirmed(name(2)));
    ConfirmedRefusals other(dir, "bbbb");
    EXPECT_FALSE(other.confirmed(name(1))) << "another compiler build makes it again";
}

TEST(CompilerRefusals, TheImagesBuildIsItsUuid)
{
    const std::string build = switch_compiler::imageBuild(reinterpret_cast<const void *>(&directory));
    EXPECT_EQ(build.size(), 32u) << build;
    EXPECT_EQ(build.find_first_not_of("0123456789abcdef"), std::string::npos) << build;
}

TEST(CompilerRefusals, ARefusalIsThisBuildsAtItsCompileAndTheWarmUpMakesAnotherBuildsAgain)
{
    const std::string service = switch_ios::test::SourceText("../compiler/compiler_service.mm");
    const std::string warm = switch_ios::test::Body(service, "void Service::warm(uint64_t stack)");
    EXPECT_THAT(warm, HasSubstr("stored == TegraGM20BCompileRefused && !confirmed.confirmed(name)"));
    EXPECT_THAT(warm, HasSubstr("if (unconfirmed)\n                store_.retire(name);"));
    EXPECT_THAT(warm, testing::Not(HasSubstr("confirm(name)")));
    const std::string run = switch_ios::test::Body(service, "int32_t Service::run(Work &work)");
    EXPECT_THAT(run, HasSubstr("if (result == TegraGM20BCompileRefused)\n        refusals_->confirm(p.name);"));
    EXPECT_THAT(run, HasSubstr("(result == TegraGM20BCompileMade || result == TegraGM20BCompileRefused) && "
                               "!asked.request.empty()"));
}

}  // namespace
