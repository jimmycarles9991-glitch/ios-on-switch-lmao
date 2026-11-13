// SPDX-License-Identifier: MIT
// A Core Animation state's fetch-free form (runtime/raw_pack.h PipelineFetchFreeForm) as its corpus line names it:
// " free=<rung>:<colour>:<program>;<edit>,<edit>..." after the label (ca/fetchfree.py tag). Each edit replaces a fragment
// constant (name:type:value) or a colour attachment's field (c<n>=...) of the line; the edited line is the form's state.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace probe {

struct FetchFree {
    int rung = 0;
    unsigned colour = 0;
    std::string program;  // the iterated blend program (iterated/table.py) of a rung-3 form
    std::string line;     // the form's R line
};

inline void applyEdit(std::string &line, const std::string &edit)
{
    const size_t bar = line.find(" | ");
    if (edit.size() > 2 && edit[0] == 'c' && isdigit(edit[1])) {
        const std::string field = " " + edit.substr(0, edit.find('=') + 1);
        const size_t at = line.find(field);
        if (at == std::string::npos || at > bar)
            return;
        const size_t end = line.find(' ', at + 1);
        line.replace(at + 1, end - at - 1, edit);
        return;
    }
    const size_t colon = edit.find(':');
    const std::string name = " " + edit.substr(0, edit.find(':', colon + 1) + 1);
    const size_t at = line.find(name, bar);
    if (at == std::string::npos)
        return;
    const size_t end = line.find(' ', at + 1);
    line.replace(at + 1, (end == std::string::npos ? line.size() : end) - at - 1, edit);
}

// False: the line names no fetch-free form
inline bool fetchFree(const std::string &line, FetchFree &out)
{
    const size_t at = line.find(" free=");
    if (at == std::string::npos)
        return false;
    const size_t end = line.find(' ', at + 1);
    const std::string tag = line.substr(at + 6, end - at - 6);
    const size_t semicolon = tag.find(';');
    char *rest = nullptr;
    out.rung = int(strtol(tag.c_str(), &rest, 10));
    if (*rest != ':' || semicolon == std::string::npos)
        return false;
    out.colour = unsigned(strtoul(rest + 1, &rest, 10));
    const size_t program = size_t(rest - tag.c_str()) + 1;
    if (*rest != ':' || program > semicolon)
        return false;
    out.program = tag.substr(program, semicolon - program);
    out.line = line.substr(0, at) + line.substr(end);
    for (size_t from = semicolon + 1; from <= tag.size();) {
        const size_t comma = std::min(tag.find(',', from), tag.size());
        applyEdit(out.line, tag.substr(from, comma - from));
        from = comma + 1;
    }
    return out.rung == 2 || out.rung == 3;
}

}  // namespace probe
