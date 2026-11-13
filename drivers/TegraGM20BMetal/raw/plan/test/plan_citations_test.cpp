// SPDX-License-Identifier: MIT
// The hazard table's NVK citations point at lines that exist, and the principal ones at the code they name.
#include "../plan_hazard.h"
#include "bazel/testing/source_text.h"
#include <gtest/gtest.h>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace raw::plan {
namespace {

const char *const kNvk = "../../../../../third_party/mesa/src/nouveau/vulkan/";

std::vector<std::string> lines(const std::string &file)
{
    std::istringstream in(switch_ios::test::SourceText(std::string(kNvk) + file));
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}

bool within(const std::vector<std::string> &text, uint32_t first, uint32_t last, const std::string &token)
{
    for (uint32_t i = first; i <= last && i <= text.size(); ++i)
        if (text[i - 1].find(token) != std::string::npos)
            return true;
    return false;
}

TEST(Citations, EveryNvkLineExists)
{
    const std::regex cite(R"((nvk_\w+\.[ch])?:(\d+)(?:-(\d+))?)");
    for (size_t c = 0; c < size_t(Clause::Count); ++c) {
        const std::string text = clauseInfo(Clause(c)).nvk;
        std::string file;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), cite); it != std::sregex_iterator(); ++it) {
            if ((*it)[1].matched)
                file = (*it)[1];
            ASSERT_FALSE(file.empty()) << text;
            const uint32_t last = uint32_t(std::stoul((*it)[3].matched ? (*it)[3].str() : (*it)[2].str()));
            EXPECT_LE(last, lines(file).size()) << file << ":" << last;
        }
    }
}

TEST(Citations, LinesSayWhatTheyAreCitedFor)
{
    const std::vector<std::string> buffer = lines("nvk_cmd_buffer.c");
    EXPECT_TRUE(within(buffer, 521, 523, "NVK_BARRIER_WFI"));
    EXPECT_TRUE(within(buffer, 525, 526, "SHADER_STORAGE_WRITE"));
    EXPECT_TRUE(within(buffer, 550, 558, "INDIRECT_COMMAND_READ"));
    EXPECT_TRUE(within(buffer, 560, 563, "UNIFORM_READ"));
    EXPECT_TRUE(within(buffer, 565, 567, "SHADER_SAMPLED_READ"));
    EXPECT_TRUE(within(buffer, 569, 570, "SHADER_STORAGE_READ"));
    EXPECT_TRUE(within(buffer, 663, 675, "flush_data"));
    EXPECT_TRUE(within(buffer, 678, 687, "implicitly"));
    EXPECT_TRUE(within(buffer, 700, 712, "NON_PIPELINED"));
    EXPECT_TRUE(within(buffer, 818, 829, "INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI"));
    EXPECT_TRUE(within(buffer, 880, 891, "SET_REFERENCE"));
    EXPECT_TRUE(within(buffer, 922, 1032, "PIXEL_SHADER_BARRIER"));
    EXPECT_TRUE(within(buffer, 431, 431, "nvk_cmd_flush_tiled_cache"));
    EXPECT_TRUE(within(lines("nvk_event.c"), 184, 184, "nvk_cmd_flush_tiled_cache"));
    EXPECT_TRUE(within(lines("nvk_tiled_cache.h"), 65, 72, "release"));
    EXPECT_TRUE(within(lines("nvk_physical_device.c"), 733, 734, "Interlock"));
}

}  // namespace
}  // namespace raw::plan
