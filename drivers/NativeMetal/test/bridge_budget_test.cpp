// SPDX-License-Identifier: MIT
// Every compile the Metal bridge and the translate crate start runs under compile_budget(), which has no deadline: the
// translator's work-unit cap and memory ledger bound it, so the same compile never fails under load and passes later.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(BridgeBudget, EveryBridgeCompileRunsWithoutADeadline)
{
    const std::string translate = SourceText("../../TegraGM20BMetal/translate/NativeMetalTranslate.rs");
    EXPECT_THAT(Body(translate, "pub fn compile_budget() -> metal2vulkan::Budget"),
                HasSubstr("metal2vulkan::Budget::default()"));
    for (const char *file : {"../bridge/NativeMetalBridge.rs", "../bridge/NativeMetalCompile.rs",
                             "../bridge/NativeMetalComputeArtifact.rs", "../bridge/NativeMetalFunctions.rs",
                             "../bridge/NativeMetalVariants.rs", "../../TegraGM20BMetal/translate/NativeMetalDirect.rs",
                             "../../TegraGM20BMetal/translate/NativeMetalDirectCompute.rs",
                             "../../TegraGM20BMetal/translate/NativeMetalDirectTess.rs",
                             "../../TegraGM20BMetal/translate/NativeMetalOffline.rs",
                             "../../TegraGM20BMetal/translate/NativeMetalReflection.rs",
                             "../../TegraGM20BMetal/translate/NativeMetalTranslate.rs"}) {
        const std::string text = SourceText(file);
        EXPECT_THAT(text, Not(HasSubstr("Budget::standard"))) << file;
        EXPECT_THAT(text, Not(HasSubstr("with_deadline"))) << file;
    }
}

}  // namespace
