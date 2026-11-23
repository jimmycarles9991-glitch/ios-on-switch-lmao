// SPDX-License-Identifier: MIT
// Every constant buffer the driver binds is bound at whole 256-byte units (cbuf.h), the split frontier included,
// whose pack size (PackSplit::frontierBytes) is a multiple of 16 only.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/cbuf.h"

using switch_ios::test::SourceText;

TEST(GM20BCbuf, BindsAreWhole256ByteUnits)
{
    // the glass states' frontiers (Iscd 64, Ialp 16, TcimIalpCcl 32, Tgbg 480, Tgbh 672) and the other sizes the path binds
    for (const auto &[bytes, bound] : {std::pair{16u, 256u},
                                       {32u, 256u},
                                       {64u, 256u},
                                       {256u, 256u},
                                       {480u, 512u},
                                       {672u, 768u},
                                       {0x800u, 0x800u},
                                       {0xff10u, 0x10000u},
                                       {0x10000u, 0x10000u},
                                       {0x20000u, 0x10000u}})
        EXPECT_EQ(gm20b::cbufBindBytes(bytes), bound) << bytes;
    EXPECT_EQ(gm20b::cbufBindBytes(0), 0u);
}

// Each SET_CONSTANT_BUFFER_SELECTOR_A the driver emits names its size as cbufBindBytes(...) or a constant of whole units
TEST(GM20BCbuf, EveryBindSiteSizesThroughTheHelper)
{
    const std::regex bind(R"(CB_SELECTOR_A,\s*\{([^,]+),)");
    for (const char *file : {"../pipelines.cpp", "../draw_prepare.cpp", "../split.cpp"}) {
        const std::string text = SourceText(file);
        int sites = 0;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), bind); it != std::sregex_iterator(); ++it) {
            const std::string size = (*it)[1];
            sites++;
            EXPECT_TRUE(size.rfind("cbufBindBytes(", 0) == 0 || size == "kRootBytes" || size == "kSetBytes")
                << file << ": " << size;
        }
        EXPECT_GT(sites, 0) << file;
    }
}
