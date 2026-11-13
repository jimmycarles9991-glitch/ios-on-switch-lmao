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

#include <cctype>
#include <cstddef>
#include <fstream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

namespace source_text_detail {

// `raw` with every whitespace character and backslash-newline removed, then every `""`: what a formatter may change
// (line breaks, indentation, alignment, a long literal split in two) is gone. `origin` maps each character kept back to
// its index in `raw` (left empty unless asked for).
struct Squeezed {
    std::string text;
    std::vector<size_t> origin;
};

inline Squeezed Squeeze(std::string_view raw, bool withOrigin)
{
    Squeezed out;
    out.text.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (std::isspace(static_cast<unsigned char>(c)))
            continue;
        if (c == '\\' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            ++i;
            continue;
        }
        if (c == '"' && !out.text.empty() && out.text.back() == '"') {
            out.text.pop_back();
            if (withOrigin)
                out.origin.pop_back();
            continue;
        }
        out.text += c;
        if (withOrigin)
            out.origin.push_back(i);
    }
    return out;
}

// Which characters of a C or C++ source are code: not in a comment or a string or character literal.
inline std::vector<bool> CodeMask(std::string_view raw)
{
    enum {
        kCode,
        kLineComment,
        kBlockComment,
        kString,
        kChar
    } state = kCode;

    std::vector<bool> code(raw.size(), false);
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i], next = i + 1 < raw.size() ? raw[i + 1] : '\0';
        switch (state) {
        case kCode:
            if (c == '/' && (next == '/' || next == '*')) {
                state = next == '/' ? kLineComment : kBlockComment;
                ++i;
            } else if (c == '"' || c == '\'') {
                state = c == '"' ? kString : kChar;
            } else {
                code[i] = true;
            }
            break;
        case kLineComment:
            if (c == '\n') {
                state = kCode;
                code[i] = true;
            }
            break;
        case kBlockComment:
            if (c == '*' && next == '/') {
                state = kCode;
                ++i;
            }
            break;
        case kString:
        case kChar:
            if (c == '\\')
                ++i;
            else if (c == (state == kString ? '"' : '\'') || c == '\n')
                state = kCode;
            break;
        }
    }
    return code;
}

// A source squeezed and masked once: Block and Occurrences are asked of the same few large sources hundreds of times.
struct Prepared {
    std::string source;
    Squeezed squeezed;
    std::vector<bool> code;
};

inline const Prepared &Prepare(const std::string &source)
{
    static std::vector<std::unique_ptr<const Prepared>> cache;
    for (const auto &prepared : cache)
        if (prepared->source == source)
            return *prepared;
    if (cache.size() == 8)
        cache.erase(cache.begin());
    cache.push_back(std::make_unique<const Prepared>(Prepared{source, Squeeze(source, true), CodeMask(source)}));
    return *cache.back();
}

}  // namespace source_text_detail

// Code compared without its layout, so a check holds however the formatter breaks, joins, indents or aligns its lines or
// splits a long string literal. The text and every needle lose all whitespace and every `""` alike, so a needle may begin
// or end anywhere, in code, a comment or a literal.
//
//   const CodeText kick = switch_ios::test::Block(source, "void kickOnce()");  // its body, braces matched
//   EXPECT_TRUE(kick.Has("if (!pd.uiNew) {"));
//   EXPECT_FALSE(kick.Has("planeNextUnfenced"));
//   EXPECT_TRUE(kick.Before("planeDecide(request, pd);", "kickHeld(request,"));
//   EXPECT_TRUE(kick.StartsWith("if (presentNotDue(request))"));  // its first statement
class CodeText {
public:
    CodeText() = default;

    explicit CodeText(std::string_view text) : text_(Normal(text))
    {}

    [[nodiscard]] testing::AssertionResult Has(std::string_view needle) const
    {
        if (Find(needle) != std::string::npos)
            return testing::AssertionSuccess() << "has " << needle;
        return testing::AssertionFailure() << "no " << needle << " in " << Shown();
    }

    size_t Count(std::string_view needle) const
    {
        return switch_ios::test::Count(text_, Normal(needle));
    }

    // `first` and `second` both occur, the first `first` before the first `second`.
    [[nodiscard]] testing::AssertionResult Before(std::string_view first, std::string_view second) const
    {
        const size_t a = Find(first), b = Find(second);
        if (a == std::string::npos || b == std::string::npos)
            return testing::AssertionFailure()
                   << "no " << (a == std::string::npos ? first : second) << " in " << Shown();
        if (a >= b)
            return testing::AssertionFailure() << second << " comes before " << first << " in " << Shown();
        return testing::AssertionSuccess() << first << " comes before " << second;
    }

    [[nodiscard]] testing::AssertionResult StartsWith(std::string_view needle) const
    {
        const std::string start = Normal(needle);
        if (text_.compare(0, start.size(), start) == 0)
            return testing::AssertionSuccess() << "starts with " << needle;
        return testing::AssertionFailure() << "does not start with " << needle << ": " << Shown();
    }

    [[nodiscard]] testing::AssertionResult EndsWith(std::string_view needle) const
    {
        const std::string end = Normal(needle);
        if (end.size() <= text_.size() && text_.compare(text_.size() - end.size(), end.size(), end) == 0)
            return testing::AssertionSuccess() << "ends with " << needle;
        return testing::AssertionFailure() << "does not end with " << needle << ": " << Shown();
    }

    // The code after the first `start` up to the first `end` after it (to the end when there is none); a missing `start`
    // is a failure and gives nothing.
    CodeText Between(std::string_view start, std::string_view end) const
    {
        const size_t from = Find(start);
        if (from == std::string::npos) {
            ADD_FAILURE() << "not found: " << start;
            return {};
        }
        const size_t begin = from + Normal(start).size();
        const size_t to = text_.find(Normal(end), begin);
        return Normalized(text_.substr(begin, to == std::string::npos ? std::string::npos : to - begin));
    }

    // The code after the first `start` to the end; a missing `start` is a failure and gives nothing.
    CodeText After(std::string_view start) const
    {
        const size_t from = Find(start);
        if (from == std::string::npos) {
            ADD_FAILURE() << "not found: " << start;
            return {};
        }
        return Normalized(text_.substr(from + Normal(start).size()));
    }

    // The code after the last `start` to the end; a missing `start` is a failure and gives nothing.
    CodeText AfterLast(std::string_view start) const
    {
        const std::string normal = Normal(start);
        const size_t from = normal.empty() ? std::string::npos : text_.rfind(normal);
        if (from == std::string::npos) {
            ADD_FAILURE() << "not found: " << start;
            return {};
        }
        return Normalized(text_.substr(from + normal.size()));
    }

    CodeText operator+(const CodeText &more) const
    {
        return Normalized(text_ + more.text_);
    }

    friend std::ostream &operator<<(std::ostream &out, const CodeText &code)
    {
        return out << code.Shown();
    }

private:
    static std::string Normal(std::string_view text)
    {
        return source_text_detail::Squeeze(text, false).text;
    }

    static CodeText Normalized(std::string text)
    {
        CodeText code;
        code.text_ = std::move(text);
        return code;
    }

    size_t Find(std::string_view needle) const
    {
        const std::string normal = Normal(needle);
        return normal.empty() ? std::string::npos : text_.find(normal);
    }

    std::string Shown() const
    {
        if (text_.size() <= 1500)
            return text_;
        return text_.substr(0, 1500) + "... (" + std::to_string(text_.size()) + " characters)";
    }

    std::string text_;
};

// The inside of the braced block `head` opens in `source`: a function's body after its signature, a struct's after its
// name, an if's after its condition. Braces are matched in code only (not in comments or literals), and the block is the
// first `{` outside the head's parentheses (a default argument's `{}` is not it). A `head` found in a comment or a
// literal, or that a `;` follows first (a declaration, a call), is passed over for the next one.
inline CodeText Block(const std::string &source, std::string_view head)
{
    const source_text_detail::Prepared &prepared = source_text_detail::Prepare(source);
    const source_text_detail::Squeezed &squeezed = prepared.squeezed;
    const std::vector<bool> &code = prepared.code;
    const std::string needle = source_text_detail::Squeeze(head, false).text;
    for (size_t at = needle.empty() ? std::string::npos : squeezed.text.find(needle); at != std::string::npos;
         at = squeezed.text.find(needle, at + 1)) {
        if (!code[squeezed.origin[at]])
            continue;
        size_t open = squeezed.origin[at], parens = 0;
        for (; open < source.size(); ++open) {
            if (!code[open])
                continue;
            if (source[open] == '(')
                ++parens;
            else if (source[open] == ')' && parens > 0)
                --parens;
            else if (parens == 0 && (source[open] == '{' || source[open] == ';'))
                break;
        }
        if (open == source.size() || source[open] == ';')
            continue;
        size_t depth = 0;
        for (size_t close = open; close < source.size(); ++close) {
            if (!code[close] || (source[close] != '{' && source[close] != '}'))
                continue;
            if (source[close] == '{')
                ++depth;
            else if (--depth == 0)
                return CodeText(std::string_view(source).substr(open + 1, close - open - 1));
        }
        ADD_FAILURE() << "unbalanced braces after " << head;
        return {};
    }
    ADD_FAILURE() << "no block after " << head;
    return {};
}

// Where `needle` occurs in code (not in a comment or a literal): the head of the outermost block around it (a function's
// signature, what follows the declaration before it) and that block's code up to the occurrence.
struct Occurrence {
    CodeText head, before;
};

inline std::vector<Occurrence> Occurrences(const std::string &source, std::string_view needle)
{
    const source_text_detail::Prepared &prepared = source_text_detail::Prepare(source);
    const source_text_detail::Squeezed &squeezed = prepared.squeezed;
    const std::vector<bool> &code = prepared.code;
    const std::string squeezedNeedle = source_text_detail::Squeeze(needle, false).text;
    std::vector<size_t> sites;
    for (size_t at = squeezedNeedle.empty() ? std::string::npos : squeezed.text.find(squeezedNeedle);
         at != std::string::npos; at = squeezed.text.find(squeezedNeedle, at + squeezedNeedle.size()))
        if (code[squeezed.origin[at]])
            sites.push_back(squeezed.origin[at]);
    std::vector<Occurrence> found;
    size_t depth = 0, headStart = 0, open = 0, next = 0;
    for (size_t i = 0; i < source.size() && next < sites.size(); ++i) {
        if (i == sites[next]) {
            if (depth == 0)
                found.push_back({CodeText(), CodeText()});
            else
                found.push_back({CodeText(std::string_view(source).substr(headStart, open - headStart)),
                                 CodeText(std::string_view(source).substr(open + 1, i - open - 1))});
            ++next;
        }
        if (!code[i])
            continue;
        if (source[i] == '{' && depth++ == 0)
            open = i;
        else if (source[i] == '}' && depth > 0 && --depth == 0)
            headStart = i + 1;
        else if (source[i] == ';' && depth == 0)
            headStart = i + 1;
    }
    return found;
}

}  // namespace switch_ios::test

#endif  // SWITCH_BAZEL_TESTING_SOURCE_TEXT_H
