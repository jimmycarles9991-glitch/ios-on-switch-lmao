// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOMFBDisplayRefresh is the family's (IOMobileFramebufferLegacy::setDisplayRefreshProperties): the display's hardware
// start calls it once the boot framebuffer is the built-in panel's, under the work-loop gate, with the internal-display
// flag set first, as Apple's UnifiedPipeline::start_hardware_boot does; a failure is one error line. Both kinds' tables
// carry the publisher's address and the flag's offset.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace {

TEST(RefreshProperties, TheHardwareStartPublishesThemForTheBuiltInPanel)
{
    const CodeText start = Block(SourceText("../NativeLegacyDisplay.cpp"), "bool startHardware() override");
    EXPECT_TRUE(start.before("primary = true;", "publishRefreshProperties();"));
    EXPECT_TRUE(start.before("valid = native_legacy_gop_mapping_observed(", "publishRefreshProperties();"));
}

TEST(RefreshProperties, TheFlagIsSetBeforeTheFamilyPublishesUnderItsGate)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const CodeText publish = Block(display, "void publishRefreshProperties()");
    EXPECT_TRUE(publish.before("setInternalDisplay(true);", "loop->runAction(&publishRefreshGated, this)"));
    EXPECT_TRUE(publish.has("IOLog(\"SwitchDisplay: IOMobileFramebufferLegacy::setDisplayRefreshProperties failed"));
    EXPECT_TRUE(Block(display, "static IOReturn publishRefreshGated(")
                    .has("native_legacy_set_display_refresh_properties(owner)"));
}

TEST(RefreshProperties, BothKindsBindThePublisherAndTheFlag)
{
    for (const char *kind : {"21H461", "24B119"}) {
        const std::string table =
            SourceText(std::string("../../../kits/SwitchABI/kinds/") + kind + "/IOMobileFramebufferLegacy.json");
        EXPECT_NE(table.find("\"native_legacy_set_display_refresh_properties\": \"0x"), std::string::npos) << kind;
        EXPECT_NE(table.find("\"internalDisplay\": \"0x"), std::string::npos) << kind;
    }
    const std::string interface = SourceText("../../../kits/SwitchABI/interface/IOMobileFramebufferLegacy.json");
    EXPECT_NE(interface.find("bool native_legacy_set_display_refresh_properties(void *display)"), std::string::npos);
}

}  // namespace
