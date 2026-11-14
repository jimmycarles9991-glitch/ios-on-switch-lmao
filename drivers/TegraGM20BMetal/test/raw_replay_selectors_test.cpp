// SPDX-License-Identifier: MIT
// What the raw path's replay onto m2v sends m2v's encoders: selectors its frontend (NativeBridgeMetalFrontend.mm)
// implements, as it answers any other zero (selector.zero) and draws without it. Boot 21: every replayed scissor
// (setScissorRects:count:) was answered zero in WidgetRenderer and iconservicesagent, and the stride forms' binds were
// refused. The selectors left are named with what puts them in a range m2v replays.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::SourceText;

bool word(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The selector of the method declaration starting at `at` ("- (type)name:(T)a label:(T)b {")
std::string declared(const std::string &text, size_t at)
{
    const size_t end = text.find_first_of("{;", at);
    std::string selector;
    int depth = 0;
    for (size_t i = text.find(')', at) + 1; i < end; ++i) {
        const char c = text[i];
        if (c == '(' || c == ')') {
            depth += c == '(' ? 1 : -1;
            continue;
        }
        if (depth || c != ':')
            continue;
        size_t from = i;
        while (from > at && word(text[from - 1]))
            --from;
        selector += text.substr(from, i - from) + ":";
    }
    if (selector.empty()) {
        size_t from = text.find(')', at) + 1, to = from;
        while (to < end && word(text[to]))
            ++to;
        selector = text.substr(from, to - from);
    }
    return selector;
}

// The selectors `classes`' @implementation blocks of `text` define
std::set<std::string> implemented(const std::string &text, const std::set<std::string> &classes)
{
    std::set<std::string> out;
    for (size_t at = text.find("@implementation "); at != std::string::npos; at = text.find("@implementation ", at)) {
        at += 16;
        size_t name = at;
        while (word(text[name]))
            ++name;
        const size_t end = text.find("\n@end", at);
        if (!classes.count(text.substr(at, name - at)))
            continue;
        for (size_t m = text.find("\n- (", at); m < end; m = text.find("\n- (", m + 1))
            out.insert(declared(text, m + 1));
    }
    return out;
}

// The selectors of the messages `text` sends `receiver` ("[receiver key:value key:value]")
std::set<std::string> sent(const std::string &text, const std::string &receiver)
{
    std::set<std::string> out;
    const std::string open = "[" + receiver + " ";
    for (size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
        if (at && word(text[at - 1]))
            continue;
        std::string selector, first;
        int depth = 1;
        for (size_t i = at + open.size(); i < text.size() && depth; ++i) {
            const char c = text[i];
            if (c == '[' || c == '(')
                ++depth;
            else if (c == ']' || c == ')')
                --depth;
            else if (depth == 1 && c == ':' && text[i + 1] != ':' && text[i - 1] != ':') {
                size_t from = i;
                while (word(text[from - 1]))
                    --from;
                selector += text.substr(from, i - from) + ":";
            } else if (depth == 1 && first.empty() && word(c)) {
                size_t to = i;
                while (word(text[to]))
                    ++to;
                first = text.substr(i, to - i);
            }
        }
        out.insert(selector.empty() ? first : selector);
    }
    return out;
}

TEST(ReplaySelectors, TheReplaySendsM2vOnlyWhatItsFrontendImplements)
{
    const std::set<std::string> frontend =
        implemented(SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm"),
                    {"NBEncoder", "NBRenderEncoder", "NBComputeEncoder", "NBBlitEncoder"});
    ASSERT_GT(frontend.size(), 60u);
    EXPECT_TRUE(frontend.count("setScissorRect:"));
    EXPECT_TRUE(frontend.count("setVertexBytes:length:atIndex:"));
    const std::string compute = SourceText("../raw/runtime/RawMetalCompute.mm");
    const size_t kernel = compute.find("void replayKernel(");
    ASSERT_NE(kernel, std::string::npos);

    const struct {
        std::string text;
        std::set<std::string> receivers;
    } replays[] = {
        {SourceText("../raw/runtime/RawMetalPlanReplay.mm"), {"encoder", "render", "compute"}},
        {SourceText("../raw/runtime/RawMetalReplay.mm"), {"enc", "e"}},
        {compute.substr(kernel, compute.find("\n}\n", kernel) - kernel), {"e"}},
    };

    // What m2v has no form of that a range it replays can still hold: an indirect draw of an encoder the raw path
    // refused for any reason (enc.geometry's Indirect among them: a fetching state with no storage form), and a render
    // encoder's memory barrier. The viewport and scissor arrays are sent only for more than one view, and a range that
    // holds such arrays never replays (RawMetalM2vGuard.mm, raw_m2v_views.h)
    const std::set<std::string> left = {
        "drawPrimitives:indirectBuffer:indirectBufferOffset:",
        "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
        "memoryBarrierWithScope:afterStages:beforeStages:",
        "setScissorRects:count:",
        "setViewports:count:",
    };
    std::set<std::string> unanswered;
    size_t messages = 0;
    for (const auto &replay : replays)
        for (const std::string &receiver : replay.receivers)
            for (const std::string &selector : sent(replay.text, receiver)) {
                ++messages;
                if (!frontend.count(selector))
                    unanswered.insert(selector);
            }
    EXPECT_GT(messages, 40u);
    EXPECT_EQ(unanswered, left);
}

}  // namespace
