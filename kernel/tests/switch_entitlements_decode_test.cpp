#include <gtest/gtest.h>
extern "C" int switch_entitlements_decode_proof(unsigned scenario);

TEST(SwitchEntitlementsDecode, MissingLegacyCallbackUsesNativeDictionaryOn27)
{
    EXPECT_EQ(switch_entitlements_decode_proof(0), 0);
}

TEST(SwitchEntitlementsDecode, BothVersionsHandleMissingCallbacksAndInvalidInput)
{
    for (unsigned scenario = 1; scenario <= 10; ++scenario)
        EXPECT_EQ(switch_entitlements_decode_proof(scenario), 0) << scenario;
}
