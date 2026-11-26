// SPDX-License-Identifier: BSD-2-Clause-Patent
// setup() takes the root and the shared volume's status through SwitchSdRoot.h (sd_root_test.cpp and sd_layout_test.cpp
// test it on the host); what stays inline is pinned here: the root window it publishes, and the one line every build,
// release included, logs when the shared volume stays offline.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Index;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

// Conditionals open (#if, #ifdef, #ifndef not yet closed by #endif) where `text` ends.
int conditionalDepth(const std::string &text)
{
    int depth = 0;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash == std::string::npos || line[hash] != '#')
            continue;
        const size_t at = line.find_first_not_of(" \t", hash + 1);
        const std::string directive =
            at == std::string::npos ? "" : line.substr(at, line.find_first_of(" \t(", at) - at);
        if (directive == "if" || directive == "ifdef" || directive == "ifndef")
            ++depth;
        else if (directive == "endif")
            --depth;
    }
    return depth;
}
}  // namespace

TEST(SdRootSource, SetupTakesTheTestedRootRule)
{
    const std::string setup = Body(driver(), "bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "if (!switchSdGptArray(header, &array)) {"), Index(setup, "SwitchSD: invalid GPT header"));
    EXPECT_THAT(setup, HasSubstr("    const uint32_t entrySectors = switchSdGptArraySectors(array);\n"
                                 "    if (!entrySectors)\n"
                                 "        return false;\n"));
    EXPECT_THAT(setup, HasSubstr("    for (uint32_t done = 0; done < entrySectors;) {\n"));
    const std::string publish =
        "    const SwitchSdRoot root = switchSdFindRoot(entries, array, kApfsType, kPartitionName);\n"
        "    rootWindow = root.window;\n"
        "    if (!root.unique()) {\n";
    EXPECT_THAT(setup, HasSubstr(publish));
}

TEST(SdRootSource, SharedVolumeOnlineOnlyOnOkAndEveryBuildLogsWhyNot)
{
    const std::string &source = driver();
    const std::string decision =
        "    const int lookup = findFiles(&filesSlice);\n"
        "    const int files = switchSdFilesStatus(lookup, haveCid, cid, filesSlice.cid);\n"
        "    if (files == SWITCH_GPT_OK) {\n"
        "        if (!haveCid) {\n"
        "            memcpy(cid, filesSlice.cid, sizeof(cid));\n"
        "            haveCid = true;\n"
        "        }\n"
        "        filesWindow = {filesSlice.first_sector, filesSlice.sector_count};\n"
        "        __atomic_store_n(&filesOnline, true, __ATOMIC_RELEASE);\n"
        "    } else {\n"
        "        IOLog(\"SwitchSD: no shared SD volume (%s)\\n\", switchSdFilesReason(files));\n"
        "    }\n";
    EXPECT_THAT(Body(source, "bool NativeSwitchBlock::setup()"), HasSubstr(decision));
    const size_t at = source.find(decision);
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(conditionalDepth(source.substr(0, at)), 0);
    EXPECT_THAT(Body(source, "void NativeSwitchBlock::verifyFilesCard()"),
                HasSubstr("findFiles(&current) == SWITCH_GPT_OK"));
}
