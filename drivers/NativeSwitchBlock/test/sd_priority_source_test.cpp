// Guards on IOStorage's Priority feature: both block devices publish it and take doSetPriority, and both XNU trees
// schedule a mount on such a device by tier while keeping the software throttle on it (bsd/vfs/vfs_subr.c).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SdPrioritySource, BothDevicesPublishPriorityAndTakeRaises)
{
    for (const char *path : {"../NativeSwitchBlock.cpp", "../SwitchCryptex.cpp"}) {
        const std::string source = SourceText(path);
        EXPECT_THAT(source, HasSubstr("features->setObject(kIOStorageFeaturePriority, kOSBooleanTrue);")) << path;
        EXPECT_THAT(source, HasSubstr("::doSetPriority(IOBlockStorageDeviceExtent *")) << path;
    }
}

TEST(SdPrioritySource, BothTreesKeepTheThrottleOnAPriorityDevice)
{
    for (const char *path :
         {"../../../kernel/xnu-10063.141.1/bsd/vfs/vfs_subr.c", "../../../kernel/xnu-12377.121.6/bsd/vfs/vfs_subr.c"}) {
        const std::string branch =
            Between(SourceText(path), "if (iosched_enabled && (features & DK_FEATURE_PRIORITY)) {",
                    "#endif /* CONFIG_IOSCHED */");
        ASSERT_FALSE(branch.empty()) << path;
        EXPECT_THAT(branch, HasSubstr("mp->mnt_ioflags |= MNT_IOFLAGS_IOSCHED_SUPPORTED;")) << path;
        EXPECT_THAT(branch, Not(HasSubstr("throttle_info_disable_throttle"))) << path;
    }
}
