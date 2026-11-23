// SPDX-License-Identifier: MIT
// An IPSW kind's selector table (precompile/ca-selectors/<build>.txt, ca_selectors.py's format) as the pack generator reads
// it: its layout line, "layout slot=<n> length=<n> padding=<byte or first-last>,...", and each "<key hash> <config hex>"
// line, the configs a state of that key hash gets selector variants of (gm20b::Direct::selectorConfigs).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include "../pack_selectors.h"

namespace pack {

inline bool selectorPadding(const std::string &list, uint64_t &padding)
{
    padding = 0;
    std::stringstream parts(list);
    std::string part;
    while (std::getline(parts, part, ',')) {
        unsigned first = 0, last = 0;
        const int read = sscanf(part.c_str(), "%u-%u", &first, &last);
        if (read == 1)
            last = first;
        if (read < 1 || first > last || last >= 64)
            return false;
        for (unsigned i = first; i <= last; i++)
            padding |= 1ull << i;
    }
    return true;
}

// False, with `why`: a line neither a layout nor a config, no layout, or a config of another length or of padding not 0
inline bool readSelectorConfigs(const std::string &text, std::map<uint64_t, gm20b::SelectorConfigs> &out,
                                std::string &why)
{
    gm20b::SelectorConfigs layout;
    bool laidOut = false;
    std::map<uint64_t, std::vector<std::array<uint8_t, gm20b::kSelectorBytes>>> configs;
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        unsigned slot = 0, length = 0;
        char padding[256] = {};
        if (sscanf(line.c_str() + start, "layout slot=%u length=%u padding=%255s", &slot, &length, padding) >= 2) {
            laidOut =
                slot < 31 && length && length <= gm20b::kSelectorBytes && selectorPadding(padding, layout.padding);
            layout.slot = uint8_t(slot);
            layout.length = uint8_t(length);
            if (!laidOut) {
                why = "a layout line of no usable slot, length or padding: " + line;
                return false;
            }
            continue;
        }
        char key[17] = {}, hex[2 * gm20b::kSelectorBytes + 2] = {};
        if (sscanf(line.c_str() + start, "%16[0-9a-f] %129[0-9a-f]", key, hex) != 2 || strlen(key) != 16 || !laidOut ||
            strlen(hex) != 2u * layout.length) {
            why = "not a layout line or a config of the layout's length after it: " + line;
            return false;
        }
        std::array<uint8_t, gm20b::kSelectorBytes> value = {};
        for (uint32_t i = 0; i < layout.length; i++) {
            const char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
            value[i] = uint8_t(strtoul(byte, nullptr, 16));
            if (value[i] && (layout.padding >> i & 1)) {
                why = "a config of padding bytes not 0: " + line;
                return false;
            }
        }
        configs[strtoull(key, nullptr, 16)].push_back(value);
    }
    if (!laidOut) {
        why = "no layout line";
        return false;
    }
    std::map<std::array<uint8_t, gm20b::kSelectorBytes>, uint32_t> ids;
    for (const auto &[key, values] : configs)
        for (const auto &value : values)
            ids.emplace(value, 0);
    uint32_t id = 0;
    for (auto &[value, number] : ids)
        number = ++id;
    for (auto &[key, values] : configs) {
        gm20b::SelectorConfigs &c = out[key];
        c = layout;
        for (const auto &value : values)
            c.values.push_back({ids.at(value), value});
    }
    return true;
}

}  // namespace pack
