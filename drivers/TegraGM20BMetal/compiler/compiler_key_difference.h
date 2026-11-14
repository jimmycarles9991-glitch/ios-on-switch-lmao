// SPDX-License-Identifier: MIT
// Where a request's own key and the key the compiler made of its fields part (compiler_request.mm): the first field of
// either that the other does not have at the same place, then each key from shortly before it. An os_log message is cut
// near 1 KiB, so the difference comes first and each key's excerpt is bounded.
#pragma once
#include <string>
#include <vector>

namespace switch_compiler {

namespace key_difference {

constexpr size_t kBefore = 100, kExcerpt = 300;

struct Field {
    size_t at;
    std::string text;
};

inline size_t wordEnd(const std::string &key, size_t at)
{
    const size_t end = key.find(' ', at);
    return end == std::string::npos ? key.size() : end;
}

// A key's fields: its blank-separated words, a "|" joined to the word after it ("| v", "| xr2p") unless that is a "|"
// too (an empty segment, such as a kernel's constants where it has none)
inline std::vector<Field> fields(const std::string &key)
{
    std::vector<Field> out;
    for (size_t at = key.find_first_not_of(' '); at != std::string::npos; at = key.find_first_not_of(' ', at)) {
        size_t end = wordEnd(key, at);
        const size_t next = key.find_first_not_of(' ', end);
        const bool pipe = end - at == 1 && key[at] == '|';
        if (pipe && next != std::string::npos && key.compare(next, wordEnd(key, next) - next, "|"))
            end = wordEnd(key, next);
        out.push_back({at, key.substr(at, end - at)});
        at = end;
    }
    return out;
}

inline std::string excerpt(const std::string &key, size_t at)
{
    const size_t from = at > kBefore ? at - kBefore : 0;
    return (from ? "..." : "") + key.substr(from, kExcerpt) + (key.size() > from + kExcerpt ? "..." : "");
}

}  // namespace key_difference

inline std::string keyDifference(const std::string &requested, const std::string &made)
{
    using namespace key_difference;
    const std::vector<Field> a = fields(requested), b = fields(made);
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i].text == b[i].text)
        ++i;
    const std::string mine = i < a.size() ? a[i].text : "(end)", theirs = i < b.size() ? b[i].text : "(end)";
    const size_t atA = i < a.size() ? a[i].at : requested.size(), atB = i < b.size() ? b[i].at : made.size();
    return "first differing field \"" + mine + "\" in its key, \"" + theirs + "\" in the compiler's; its key " +
           excerpt(requested, atA) + "; the compiler's " + excerpt(made, atB);
}

}  // namespace switch_compiler
