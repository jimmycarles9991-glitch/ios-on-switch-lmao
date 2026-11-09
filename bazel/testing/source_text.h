// Source guards: checks on the text of code under test, for properties a model cannot observe (no wait in an interrupt
// path, a register value as Linux programs it, an API other drivers rely on). Keep them few and meaningful: a value
// reachable through a header is a static_assert instead, and behaviour belongs in the test's scenarios.
//
//   const std::string source = switch_ios::test::SourceText("../SwitchTouch.cpp");   // relative to the test's package
//   const std::string action = switch_ios::test::Body(source, "void SwitchTouch::interruptOccurred(");
//   EXPECT_THAT(action, Not(HasSubstr("IOSleep")));
//
// The file must be in the test's `data`. Paths are relative to the test's own package (SWITCH_TEST_PACKAGE, set by
// host_test), so a driver directory that moves with its test/ package keeps its guards working.
#ifndef SWITCH_BAZEL_TESTING_SOURCE_TEXT_H
#define SWITCH_BAZEL_TESTING_SOURCE_TEXT_H

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SWITCH_TEST_PACKAGE
#error "SWITCH_TEST_PACKAGE is set by //bazel/testing:defs.bzl host_test"
#endif

namespace switch_ios::test {

// The workspace-relative path of `relative` (to the test's package), with "dir/../" folded.
inline std::string PackagePath(std::string_view relative)
{
    std::string path = SWITCH_TEST_PACKAGE;
    while (relative.substr(0, 3) == "../") {
        const size_t slash = path.rfind('/');
        path = slash == std::string::npos ? std::string() : path.substr(0, slash);
        relative.remove_prefix(3);
    }
    return path.empty() ? std::string(relative) : path + "/" + std::string(relative);
}

// The whole text of a file of the test's `data`, by its path relative to the test's package. A test runs in its
// runfiles' main-repository directory, where every data file sits at its workspace path.
inline std::string SourceText(std::string_view relative)
{
    const std::string path = PackagePath(relative);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot read " << path << " (is it in the test's data?)";
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// The text after the first `start` up to the first `end` after it (Python's s.split(start, 1)[1].split(end, 1)[0]); a
// missing `start` is a failure and gives "".
inline std::string Between(const std::string &text, std::string_view start, std::string_view end)
{
    const size_t from = text.find(start);
    if (from == std::string::npos) {
        ADD_FAILURE() << "not found: " << start;
        return {};
    }
    const size_t begin = from + start.size();
    const size_t to = text.find(end, begin);
    return text.substr(begin, to == std::string::npos ? std::string::npos : to - begin);
}

// A function's body: from its signature to the first line that is just "}" (house style closes a function there).
inline std::string Body(const std::string &source, std::string_view signature)
{
    return Between(source, signature, "\n}\n");
}

// How many times `needle` occurs in `text` (non-overlapping).
inline size_t Count(std::string_view text, std::string_view needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); !needle.empty() && at != std::string_view::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

// The lines a compiler sees with `macro` (a build-time version or capability flag such as XNU_12377_121_6) set or not:
// "#if macro" and "#if !macro" blocks, with their "#else", are resolved; every other conditional is kept whole.
inline std::string Configured(const std::string &source, std::string_view macro, bool set)
{
    enum Branch {
        kOther,
        kTaken,
        kSkipped
    };

    const std::string ifSet = "#if " + std::string(macro), ifUnset = "#if !" + std::string(macro);
    std::vector<Branch> open;
    std::istringstream lines(source);
    std::string out;
    for (std::string line; std::getline(lines, line);) {
        const auto starts = [&](std::string_view prefix) {
            return line.rfind(prefix, 0) == 0;
        };
        if (line == ifSet || line == ifUnset) {
            open.push_back((line == ifSet) == set ? kTaken : kSkipped);
            continue;
        }
        if (starts("#if")) {
            open.push_back(kOther);
        } else if (starts("#else") && !open.empty() && open.back() != kOther) {
            open.back() = open.back() == kTaken ? kSkipped : kTaken;
            continue;
        } else if (starts("#endif") && !open.empty()) {
            const bool ours = open.back() != kOther;
            open.pop_back();
            if (ours)
                continue;
        }
        bool skipping = false;
        for (const Branch branch : open)
            skipping = skipping || branch == kSkipped;
        if (!skipping)
            out += line + "\n";
    }
    EXPECT_TRUE(open.empty()) << "unbalanced conditionals";
    return out;
}

}  // namespace switch_ios::test

#endif  // SWITCH_BAZEL_TESTING_SOURCE_TEXT_H
