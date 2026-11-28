// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bench::compiler {
struct Attachment {
    size_t index;
    bool depth;
    uint32_t width, height, samples;
};

std::string difference(const Attachment &plane, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                       const char *labelA, const char *labelB);
}  // namespace bench::compiler
