// Our in-kernel drivers register their classes through switch_kernel_classes (libkern/c++/OSKext.cpp, both trees),
// which holds OSKext's sKextLock across OSMetaClass::preModLoad, the static initializers and postModLoad, the order
// OSKext::start keeps. A driver calling preModLoad/postModLoad itself holds sStalledClassesLock while postModLoad waits
// for sKextLock; a kext starting on another thread holds them the other way round, and the two threads, then every
// IOKit matching thread, wait for good: the root never mounts.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
std::vector<std::filesystem::path> sources()
{
    std::vector<std::filesystem::path> found;
    for (const char *root : {"drivers", "kits", "diagnostics", "kernel/switch"}) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::follow_directory_symlink)) {
            const std::string ext = entry.path().extension().string();
            if (entry.is_regular_file() && (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".mm"))
                found.push_back(entry.path());
        }
    }
    return found;
}

std::string text(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The offsets of `steps` in `body`, in order; npos for a step missing or out of order.
std::vector<size_t> inOrder(const std::string &body, std::initializer_list<const char *> steps)
{
    std::vector<size_t> at;
    size_t from = 0;
    for (const char *step : steps) {
        const size_t found = body.find(step, from);
        at.push_back(found);
        from = found == std::string::npos ? body.size() : found + 1;
    }
    return at;
}
}  // namespace

TEST(XnuKernelClasses, BothTreesRegisterUnderTheKextLock)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        const std::string source = SourceText(std::string("../") + tree + "/libkern/c++/OSKext.cpp");
        const std::string body = Body(source, "\nswitch_kernel_classes(void (*initialize)(void))\n{");
        for (const size_t at :
             inOrder(body, {"IORecursiveLockLock(sKextLock);", "OSMetaClass::preModLoad(", "initialize();",
                            "OSMetaClass::postModLoad(handle)", "IORecursiveLockUnlock(sKextLock);", "return result;"}))
            EXPECT_NE(at, std::string::npos) << tree;
        EXPECT_EQ(Count(body, "return"), 1u) << tree << ": a return before the unlock keeps sKextLock";
        EXPECT_EQ(Count(body, "IORecursiveLock"), 2u) << tree;
    }
}

TEST(XnuKernelClasses, NoDriverRegistersClassesItself)
{
    const std::vector<std::filesystem::path> files = sources();
    ASSERT_GT(files.size(), 100u);
    unsigned registering = 0;
    for (const auto &path : files) {
        const std::string source = text(path);
        EXPECT_THAT(source, Not(HasSubstr("OSMetaClass::preModLoad("))) << path;
        EXPECT_THAT(source, Not(HasSubstr("OSMetaClass::postModLoad("))) << path;
        if (source.find("switch_kernel_classes(") != std::string::npos)
            ++registering;
    }
    EXPECT_GE(registering, 12u);
}
