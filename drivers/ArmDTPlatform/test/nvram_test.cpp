// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "drivers/ArmDTPlatform/SwitchNVRAM.h"

class SwitchNVRAMTest : public testing::Test {
protected:
    IOService root;
    std::shared_ptr<IORegistryEntry> chosen = std::make_shared<IORegistryEntry>();
    std::shared_ptr<IODTNVRAM> nvram = std::make_shared<IODTNVRAM>();
    std::shared_ptr<SwitchTestNVRAMDiags> diagnostics = std::make_shared<SwitchTestNVRAMDiags>();

    void SetUp() override
    {
        root.nodes[{gIODTPlane, "chosen"}] = chosen;
        root.nodes[{gIODTPlane, "options"}] = std::make_shared<IORegistryEntry>();
        chosen->properties["nvram-bank-size"] = std::make_shared<OSData>(std::vector<unsigned char>{0, 32, 0, 0});
        chosen->properties["nvram-proxy-data"] = std::make_shared<OSData>(std::vector<unsigned char>(8192));
    }

    void consume()
    {
        chosen->properties.erase("nvram-proxy-data");
        root.nodes[{gIODTPlane, "options"}] = nvram;
        nvram->provider = &root;
        diagnostics->provider = nvram.get();
        diagnostics->properties["Version"] = std::make_shared<OSNumber>(1);
        nvram->children.push_back(diagnostics);
    }
};

TEST_F(SwitchNVRAMTest, ValidSourceIsAcceptedBeforeTheNativeServiceExists)
{
    EXPECT_TRUE(switch_platform_nvram_ready(&root));
}

TEST_F(SwitchNVRAMTest, ConsumedSourceRequiresTheInitializedNativeFormat)
{
    consume();
    EXPECT_TRUE(switch_platform_nvram_ready(&root));
    EXPECT_EQ(chosen->releases, 1);
    EXPECT_EQ(nvram->releases, 1);
    EXPECT_EQ(diagnostics->properties["Version"]->releases, 1);
    EXPECT_EQ(chosen->getProperty("nvram-proxy-data"), nullptr);
    diagnostics->properties["Version"] = std::make_shared<OSNumber>(0);
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    diagnostics->properties.erase("Version");
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    diagnostics->properties["Version"] = std::make_shared<OSString>();
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
}

TEST_F(SwitchNVRAMTest, GenuinelyAbsentSourceAndServiceAreRejected)
{
    chosen->properties.erase("nvram-proxy-data");
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    root.nodes[{gIODTPlane, "options"}] = nvram;
    nvram->provider = &root;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
}

TEST_F(SwitchNVRAMTest, RetainedMalformedInputsDoNotFallBackToNativeState)
{
    consume();
    chosen->properties["nvram-proxy-data"] = std::make_shared<OSString>();
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    chosen->properties["nvram-proxy-data"] = std::make_shared<OSData>(std::vector<unsigned char>{});
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    chosen->properties.erase("nvram-proxy-data");
    chosen->properties["nvram-bank-size"] = std::make_shared<OSData>(std::vector<unsigned char>{0, 32});
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    chosen->properties["nvram-bank-size"] = std::make_shared<OSString>();
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    chosen->properties.erase("nvram-bank-size");
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    root.nodes.erase({gIODTPlane, "chosen"});
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
}

TEST_F(SwitchNVRAMTest, InactiveAndUnrelatedServicesAreRejected)
{
    consume();
    nvram->inactive = true;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    nvram->inactive = false;
    IOService unrelated;
    nvram->provider = &unrelated;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    nvram->provider = &root;
    diagnostics->inactive = true;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    diagnostics->inactive = false;
    diagnostics->provider = &unrelated;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
    diagnostics->provider = nvram.get();
    nvram->fail_iterator = true;
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
}

TEST_F(SwitchNVRAMTest, AVersionPropertyOnAnUnrelatedChildDoesNotProveInitialization)
{
    consume();
    auto other = std::make_shared<IOService>();
    other->provider = nvram.get();
    other->properties["Version"] = std::make_shared<OSNumber>(1);
    nvram->children = {other};
    EXPECT_FALSE(switch_platform_nvram_ready(&root));
}
