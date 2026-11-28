// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bench::compiler {
class HandleWriter {
public:
    HandleWriter(uint32_t *table, const std::vector<uint8_t> &kinds, bool renderbox)
        : sampled_(table),
          images_(table + (renderbox ? 16 : std::count_if(kinds.begin(), kinds.end(), [](uint8_t kind) {
                      return kind == 1 || kind == 2;
                  })))
    {}

    void sampled(uint32_t handle)
    {
        *sampled_++ = handle;
    }

    void image(uint32_t handle)
    {
        *images_++ = handle;
    }

private:
    uint32_t *sampled_, *images_;
};
}  // namespace bench::compiler
