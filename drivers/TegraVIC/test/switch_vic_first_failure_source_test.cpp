// SPDX-License-Identifier: BSD-2-Clause-Patent
// A job the transform call accepted and that then fails shows only in the 5 s line's failed= count; debug kernels name
// the first one a boot, from the engine thread's two exits before submission and from the job's completion.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraVICFirstFailure, EveryExitOfAnAcceptedJobNamesTheFirstFailureInDebugKernelsOnly)
{
    const std::string source = SourceText("../TegraVICUserClient.cpp");
    const std::string debug = Configured(source, "TEGRA_VIC_STATS", true);
    const std::string run = Body(debug, "bool runJob(Job *job, IOReturn *status)\n");
    EXPECT_THAT(run, HasSubstr("firstFailure(job, \"events\", prepared);"));
    EXPECT_THAT(run,
                HasSubstr("firstFailure(job, bounced ? \"bounce submission\" : \"direct submission\", submitted);"));
    EXPECT_THAT(Body(debug, "void transformed(void *context, int status)\n"),
                HasSubstr("firstFailure(job, \"completion\", status);"));
    EXPECT_THAT(Body(debug, "IOReturn transform(Conn *conn, const void *in, uint32_t size)\n"),
                HasSubstr("job->pid = proc_selfpid();"));
    EXPECT_THAT(Configured(source, "TEGRA_VIC_STATS", false), Not(HasSubstr("firstFailure")));
}
