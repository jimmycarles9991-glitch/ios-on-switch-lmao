// XNU's kalloc_large() panics if Z_NOFAIL is passed with a size above KHEAP_MAX_SIZE (32 KiB on arm64). Our code in
// kernel/switch, drivers, kits, diagnostics, switch_kprof.c and switch_bootstrap_environment never passes Z_NOFAIL to
// kalloc; every fallible allocation uses Z_WAITOK and handles NULL cleanly.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

std::string stripComments(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < src.size() && src[i] != '\n')
                ++i;
            continue;
        }
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n')
                    out.push_back('\n');
                ++i;
            }
            if (i + 1 < src.size())
                i += 2;
            continue;
        }
        if (src[i] == '"' || src[i] == '\'') {
            const char quote = src[i];
            out.push_back(src[i++]);
            while (i < src.size()) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    out.push_back(src[i++]);
                    out.push_back(src[i++]);
                    continue;
                }
                const char c = src[i];
                out.push_back(src[i++]);
                if (c == quote)
                    break;
            }
            continue;
        }
        out.push_back(src[i++]);
    }
    return out;
}

bool isTestPath(const std::filesystem::path &path)
{
    for (const auto &part : path) {
        if (part == "test" || part == "tests")
            return true;
    }
    return false;
}

std::vector<std::filesystem::path> switchSources()
{
    std::vector<std::filesystem::path> found;
    for (const char *root : {"drivers", "kits", "diagnostics", "kernel/switch"}) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::follow_directory_symlink)) {
            if (!entry.is_regular_file() || isTestPath(entry.path()))
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".mm")
                found.push_back(entry.path());
        }
    }
    return found;
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST(XnuKallocNoFail, SwitchSourcesNeverPassZNoFail)
{
    const std::vector<std::filesystem::path> files = switchSources();
    ASSERT_GT(files.size(), 100u);
    for (const auto &path : files) {
        EXPECT_THAT(stripComments(readFile(path)), Not(HasSubstr("Z_NOFAIL"))) << path;
    }
}

TEST(XnuKallocNoFail, CustomXnuHooksNeverPassZNoFail)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        const std::string kprof = SourceText(std::string("../") + tree + "/osfmk/kern/switch_kprof.c");
        EXPECT_THAT(stripComments(kprof), Not(HasSubstr("Z_NOFAIL"))) << tree << " switch_kprof.c";

        const std::string exec = SourceText(std::string("../") + tree + "/bsd/kern/kern_exec.c");
        const std::string env = Body(exec, "\nswitch_bootstrap_environment(");
        ASSERT_FALSE(env.empty()) << tree;
        EXPECT_THAT(stripComments(env), Not(HasSubstr("Z_NOFAIL"))) << tree << " switch_bootstrap_environment";
    }
}
