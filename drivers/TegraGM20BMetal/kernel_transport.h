// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// A TegraGM20B connection and the form its calls take: direct, or tunnelled through the one selector a sandbox admits
struct KernelTransport {
    uint32_t connection = 0;
    bool tunnel = false;
    uint32_t selector = 0;
};

}  // namespace gm20b
