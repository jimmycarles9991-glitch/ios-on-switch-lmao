// --//kernel:recorder defines SWITCH_RECORDER only on the objects kernel/switch/objects.bzl's _RECORDER_OBJECTS names
// (with_recorder). An object that uses the recorder's hooks but is missing from that list builds without them, and
// nothing else would notice: every source in kernel/switch and in both trees' bsd and osfmk that names SWITCH_RECORDER
// must be listed, and every listed one must name it.
#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <set>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::PackagePath;
using switch_ios::test::SourceText;

namespace {

namespace fs = std::filesystem;

const char *const kTrees[] = {"xnu-10063.141.1", "xnu-12377.121.6"};

std::set<std::string> Listed()
{
    const std::string table = Between(SourceText("../switch/objects.bzl"), "_RECORDER_OBJECTS = {", "\n}");
    std::set<std::string> out;
    const std::regex entry("\"([^\"]+\\.(c|cpp))\"");
    for (std::sregex_iterator it(table.begin(), table.end(), entry), end; it != end; ++it) {
        out.insert((*it)[1]);
    }
    return out;
}

// Every C or C++ source below `root` that names SWITCH_RECORDER, as `prefix` + its path below `root`.
void Hooked(const std::string &root, const std::string &prefix, std::set<std::string> *out)
{
    const fs::path base = PackagePath(root);
    for (const auto &e : fs::recursive_directory_iterator(base, fs::directory_options::follow_directory_symlink)) {
        const std::string ext = e.path().extension().string();
        if (!e.is_regular_file() || (ext != ".c" && ext != ".cpp")) {
            continue;
        }
        const std::string rel = fs::relative(e.path(), base).generic_string();
        if (SourceText(root + "/" + rel).find("SWITCH_RECORDER") != std::string::npos) {
            out->insert(prefix + rel);
        }
    }
}

TEST(XnuRecorderHooksSource, EverySourceWithARecorderHookGetsTheDefine)
{
    const std::set<std::string> listed = Listed();
    std::set<std::string> hooked;

    ASSERT_FALSE(listed.empty()) << "no _RECORDER_OBJECTS table in kernel/switch/objects.bzl";
    Hooked("../switch", "switch:", &hooked);
    for (const char *tree : kTrees) {
        for (const char *component : {"bsd", "osfmk"}) {
            Hooked(std::string("../") + tree + "/" + component, std::string(component) + "/", &hooked);
        }
    }
    for (const std::string &file : hooked) {
        EXPECT_TRUE(listed.count(file)) << file << " uses the recorder's hooks but is not in _RECORDER_OBJECTS";
    }
    for (const std::string &file : listed) {
        EXPECT_TRUE(hooked.count(file)) << file << " is in _RECORDER_OBJECTS but names no recorder hook";
    }
}

TEST(XnuRecorderHooksSource, DumpTraceClearsInheritedEagerPreemptAroundKdbgDump)
{
    const std::string body =
        Between(SourceText("../switch/recorder/recorder.c"), "switch_recorder_trace(", "\nstatic uint64_t");
    const size_t clear = body.find("thread_clear_eager_preempt(");
    const size_t dump = body.find("kdbg_dump_trace_to_file(", clear);
    const size_t restore = body.find("thread_set_eager_preempt(", dump);

    ASSERT_NE(clear, std::string::npos);
    ASSERT_NE(dump, std::string::npos);
    ASSERT_NE(restore, std::string::npos);
    EXPECT_LT(clear, dump);
    EXPECT_LT(dump, restore);
}

}  // namespace
