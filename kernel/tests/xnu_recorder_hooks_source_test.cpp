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

// The capture reads the ring into a kernel buffer through kdebug's own merge (switch_recorder_kdebug_read), whose copy
// to a reader's buffer is a copyout that would refuse a kernel address: on the capture thread alone, the copy is a
// memcpy. Each tree's read must route through that hook, and the capture must mark its thread around the read.
TEST(XnuRecorderHooksSource, TheCaptureReadsTheRingThroughKdebugsMergeIntoMemory)
{
    for (const char *tree : kTrees) {
        const std::string kdebug = SourceText(std::string("../") + tree + "/bsd/kern/kdebug.c");
        const std::string read =
            Between(kdebug, "switch_recorder_kdebug_read(void *buf", "#endif /* SWITCH_RECORDER */");

        EXPECT_NE(kdebug.find("= SWITCH_RECORDER_COPYOUT("), std::string::npos)
            << tree << ": the merged read's copy does not go through SWITCH_RECORDER_COPYOUT";
        EXPECT_NE(read.find("_read_merged_trace_events("), std::string::npos) << tree;
        EXPECT_NE(read.find("kdebug_enable = reenable;"), std::string::npos) << tree << ": tracing comes back on";
    }
    const std::string capture =
        Between(SourceText("../switch/recorder/capture.c"), "static void switch_recorder_ring(", "\nstatic const char");
    const size_t on = capture.find("switch_recorder_dumping_set(true);");
    const size_t read = capture.find("switch_recorder_kdebug_read(", on);
    const size_t off = capture.find("switch_recorder_dumping_set(false);", read);

    ASSERT_NE(on, std::string::npos);
    ASSERT_NE(read, std::string::npos);
    ASSERT_NE(off, std::string::npos);
}

// The capture thread runs at fixed priority 52, above SpringBoard's main thread: it compresses nothing (the store's
// low-priority call does, as it writes), and an automatic capture with no room for its stackshot or ring is dropped
// before the store ever holds it.
TEST(XnuRecorderHooksSource, TheCaptureThreadNeverCompressesAndDropsAnAutomaticCaptureWithNoRoom)
{
    const std::string capture = SourceText("../switch/recorder/capture.c");
    EXPECT_EQ(capture.find("deflate("), std::string::npos);
    EXPECT_EQ(capture.find("zlib"), std::string::npos);
    const std::string one = Between(capture, "static void switch_recorder_capture_one(", "\n}\n");
    const size_t abandon = one.find("switch_recorder_store_abandon(c);");
    ASSERT_NE(abandon, std::string::npos);
    EXPECT_LT(abandon, one.find("switch_recorder_store_push(c);"));
    EXPECT_NE(SourceText("../switch/recorder/store.c").find("deflate(&store.zs"), std::string::npos);
}

}  // namespace
