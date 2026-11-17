// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace raw {

constexpr uint32_t kSelectorBytes = 64;

struct PackSelectorIndex {
    uint32_t record, at;
};

struct PackSelectorTable {
    uint32_t configs, assume;
};

struct PackSelectorConfig {
    uint32_t id, program;
};

static_assert(sizeof(PackSelectorIndex) == 8);
static_assert(sizeof(PackSelectorTable) == 8);
static_assert(sizeof(PackSelectorConfig) == 8);

inline const PackSelectorConfig *selectorConfigs(const PackSelectorTable &table)
{
    return reinterpret_cast<const PackSelectorConfig *>(&table + 1);
}

struct SelectorConfig {
    uint32_t id;
    std::array<uint8_t, kSelectorBytes> value;
};

struct SelectorConfigs {
    uint8_t slot = 0, length = 0;
    uint64_t padding = 0;
    std::vector<SelectorConfig> values;
};

}  // namespace raw
