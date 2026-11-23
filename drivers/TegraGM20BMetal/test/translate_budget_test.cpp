// SPDX-License-Identifier: MIT
// Every compile the translate crate starts runs under compile_budget(), which has no deadline: the translator's work-unit
// cap and memory ledger bound it, so the same compile never fails under load and passes later.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(TranslateBudget, EveryCompileRunsWithoutADeadline)
{
    const std::string translate = SourceText("../translate/NativeMetalTranslate.rs");
    EXPECT_THAT(Body(translate, "pub fn compile_budget() -> metal2vulkan::Budget"),
                HasSubstr("metal2vulkan::Budget::default()"));
    for (const char *file :
         {"../translate/NativeMetalDirect.rs", "../translate/NativeMetalDirectAbi.rs",
          "../translate/NativeMetalDirectCompute.rs", "../translate/NativeMetalDirectComputeAbi.rs",
          "../translate/NativeMetalDirectComputeBinds.rs", "../translate/NativeMetalDirectComputeWords.rs",
          "../translate/NativeMetalDirectComputeWrites.rs", "../translate/NativeMetalDirectDraw.rs",
          "../translate/NativeMetalDirectLayout.rs", "../translate/NativeMetalDirectProbe.rs",
          "../translate/NativeMetalDirectTess.rs", "../translate/NativeMetalDirectWrites.rs",
          "../translate/NativeMetalOffline.rs", "../translate/NativeMetalReflection.rs",
          "../translate/NativeMetalTranslate.rs"}) {
        const std::string text = SourceText(file);
        EXPECT_THAT(text, Not(HasSubstr("Budget::standard"))) << file;
        EXPECT_THAT(text, Not(HasSubstr("with_deadline"))) << file;
    }
}

}  // namespace
