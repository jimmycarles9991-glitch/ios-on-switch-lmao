#include <gtest/gtest.h>
extern "C" int switch_app_code_blob_proof(unsigned scenario);

TEST(SwitchAppCodeBlob, MetadataLifetimeAndNativeEntitlementSlots)
{
    EXPECT_EQ(switch_app_code_blob_proof(0), 0);
    EXPECT_EQ(switch_app_code_blob_proof(6), 0);
}

TEST(SwitchAppCodeBlob, MissingMalformedAndFailedMetadataNeverAttach)
{
    for (unsigned scenario = 1; scenario <= 5; ++scenario)
        EXPECT_EQ(switch_app_code_blob_proof(scenario), 0) << scenario;
}
