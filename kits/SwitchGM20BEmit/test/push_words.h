// SPDX-License-Identifier: MIT
// Pushes compared word for word (Words) or as the methods they write (Methods), printed in hex when they differ. Either
// fails the test on a method its class lacks.
#pragma once

#include <cstdint>
#include <ios>
#include <ostream>
#include <vector>

#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "method_checks.h"

namespace switch_gm20b_test {

struct Hex {
    std::vector<uint32_t> words;
    bool operator==(const Hex &) const = default;
};

inline void PrintTo(const Hex &h, std::ostream *os)
{
    for (uint32_t w : h.words)
        *os << std::hex << "0x" << w << ' ';
}

inline Hex Words(const std::vector<uint32_t> &words)
{
    expectPushMethods(words.data(), words.size());
    return {words};
}

struct Method {
    uint32_t subchannel, method, value;
    bool operator==(const Method &) const = default;
};

inline void PrintTo(const Method &m, std::ostream *os)
{
    *os << std::hex << m.subchannel << ":0x" << m.method << "=0x" << m.value;
}

inline std::vector<Method> Methods(const std::vector<uint32_t> &words)
{
    std::vector<switch_gm20b_method> decoded(words.size() * 2 + 1);
    const long n = switch_gm20b_push_decode(words.data(), words.size(), decoded.data(), decoded.size());
    expectPushMethods(words.data(), words.size());
    std::vector<Method> out;
    for (long i = 0; i < n; i++)
        out.push_back({decoded[size_t(i)].subchannel, decoded[size_t(i)].method, decoded[size_t(i)].value});
    if (n < 0)
        out.push_back({~0u, ~0u, ~0u});
    return out;
}

}  // namespace switch_gm20b_test
