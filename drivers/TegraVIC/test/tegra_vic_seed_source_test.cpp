// A TransformSurface destination's seed moves when its job is done (r18-planes), as Apple's AppleM2ScalerCSCDriver moves
// it: TegraVIC's finishJob calls the IOSurface kext's device write end on the destination after the job and before
// the caller hears back, through the kits/SwitchABI IOSurface binding, whose tables pin the function by its words (the
// seed's own loads and stores among them).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
void ExpectBefore(const std::string &text, const std::string &a, const std::string &b)
{
    const size_t at = text.find(a), bt = text.find(b);
    ASSERT_NE(at, std::string::npos) << a;
    ASSERT_NE(bt, std::string::npos) << b;
    EXPECT_LT(at, bt) << a << " before " << b;
}
}  // namespace

TEST(TegraVICSeed, DestinationSeedMovesBeforeTheReply)
{
    const std::string source = SourceText("../TegraVICUserClient.cpp");
    const std::string engine = Between(source, "void engineThread(void *, wait_result_t)", "\n}\n");
    ExpectBefore(engine, "runJob(job, &status)", "finishJob(job, status);");
    // finishJob runs on the engine thread, or on a bounced job's copy-back completion
    const std::string finish = Between(source, "void finishJob(Job *job, IOReturn status)\n{", "\n}\n");
    ExpectBefore(finish, "tegra_vic_surface_written(&job->dst);", "tegra_vic_surface_release(&job->dst);");
    ExpectBefore(finish, "tegra_vic_surface_written(&job->dst);", "iouc_send_async_result64");
    ExpectBefore(finish, "tegra_vic_surface_written(&job->dst);", "job->done = true;");
    EXPECT_THAT(SourceText("../TegraVICSurface.cpp"), HasSubstr("tegra_vic_ios_device_unlock(s->surface, 0)"));
}

TEST(TegraVICSeed, BindingIsChecked)
{
    // Each kind's table pins the device write end by its words, the seed's load, increment and store among them.
    const std::string ios17 = SourceText("../../../kits/SwitchABI/kinds/21H461/IOSurface.json");
    const std::string ios27 = SourceText("../../../kits/SwitchABI/kinds/24B119/IOSurface.json");
    EXPECT_THAT(Between(ios17, "\"device_unlock\": {", "}"), HasSubstr("f940be68 b9400d09 11000529 b9000d09"));
    EXPECT_THAT(Between(ios27, "\"device_unlock\": {", "}"), HasSubstr("b9400e08 11000508 b9000e08"));
}
