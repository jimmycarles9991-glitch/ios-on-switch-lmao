// SPDX-License-Identifier: MIT
// pack/fetch_free.h: the fetch-free form's R line a corpus line's " free=" tag makes.
#include <string>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/pack/fetch_free.h"

namespace {

const std::string kLib = "lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib";

TEST(PackFetchFree, AFragmentEditReplacesTheFragmentsConstantAndLeavesTheVertexStagesOfTheSameName)
{
    const std::string head = "R ok=1 label=primitive_image samples=1 topo=0 ds=260/260 c0=115:15:0:1:0:0:1:0:0";
    const std::string tag = " free=3:0:mpy;f/#0:36:470090040000190073f800001c000000";
    const std::string vertex = " | v primitive_image_vertex " + kLib + " #0:36:470090040000190073f800003d000000";
    const std::string fragment = " | f primitive_image_fragment " + kLib + " #0:36:";
    probe::FetchFree form;
    ASSERT_TRUE(probe::fetchFree(head + tag + vertex + fragment + "470090040000190073f800003d000000 | ", form));
    EXPECT_EQ(form.line, head + vertex + fragment + "470090040000190073f800001c000000 | ");
    EXPECT_EQ(form.rung, 3);
    EXPECT_EQ(form.program, "mpy");
}

}  // namespace
