// SPDX-License-Identifier: MIT
// The functions a metallib lists, read by the facade itself (MetallibEntries.h over the AIR slice MetallibSlice.h picks), so
// a library and its functions need no m2v object until something only m2v answers is asked of them.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include "MetallibEntries.h"
#include "MetallibSlice.h"

namespace raw {

// Metal's MTLFunctionType for a metallib TYPE tag (metal2vulkan-air container/mod.rs FunctionStage: 0 vertex, 1 fragment,
// 2 kernel or tile, 4 visible, 6 intersection, 7 mesh, 8 object); 0 for a tag Metal makes no function of by name here
// (3 a dynamic library's export, 5 a Core Image extern) or one not observed
constexpr uint32_t metalFunctionType(uint8_t tag)
{
    switch (tag) {
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 3;
    case 4:
        return 5;
    case 6:
        return 6;
    case 7:
        return 7;
    case 8:
        return 8;
    default:
        return 0;
    }
}

struct MetallibFunctions {
    const uint8_t *slice = nullptr;
    size_t sliceBytes = 0;
    std::unordered_map<std::string, uint32_t> types;  // each listed function's metalFunctionType (0: m2v answers it)

    // False: no AIR slice Metal takes, or a function list this does not read
    bool read(const uint8_t *file, size_t length)
    {
        size_t at = 0, size = 0;
        types.clear();
        if (!switch_metal::airSlice(file, length, at, size))
            return false;
        const bool whole =
            switch_metal::forEachMetallibEntry(file + at, size, [&](const switch_metal::MetallibEntry &e) {
                types[e.name] = e.type ? metalFunctionType(*e.type) : 0;
                return true;
            });
        if (!whole || types.empty()) {
            types.clear();
            return false;
        }
        slice = file + at;
        sliceBytes = size;
        return true;
    }

    // The function's MTLFunctionType; 0: not listed, or one m2v makes
    uint32_t type(const std::string &name) const
    {
        const auto found = types.find(name);
        return found == types.end() ? 0 : found->second;
    }

    bool lists(const std::string &name) const
    {
        return types.count(name) != 0;
    }
};

}  // namespace raw
