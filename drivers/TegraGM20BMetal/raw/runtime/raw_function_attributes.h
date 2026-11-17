// SPDX-License-Identifier: MIT
// A function made with constant values that has stage-input attributes: which of them its constants gate off is in its AIR,
// not its metallib's tags (raw_function_tags.h), so the shader compiler answers them from the translator
// (RequestKind::Reflection of one stage tagged "a", raw_compile_request.h functionAttributes). Its key names the function's
// AIR, its constants and the name it was made under; its blob is vertexAttributes, then stageInputAttributes, each a u8
// that is 0 for nil, else 1 and a u16 count of (name, NUL, index, MTLDataType, flags: 1 active, 2 patch data, 4 control-point
// data).
#pragma once
#include "raw_function_tags.h"
#include "raw_metallib.h"
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace raw {

using AttributeList = std::optional<std::vector<FunctionAttributeTag>>;

inline std::string functionAttributesKey(const AirHash &air, const std::string &constants, const std::string &name)
{
    std::string key;
    char b[4];
    for (uint8_t x : air) {
        snprintf(b, sizeof b, "%02x", x);
        key += b;
    }
    return key + " | " + constants + " | as=" + name;
}

inline std::string encodeAttributeLists(const AttributeList &vertex, const AttributeList &stageInput)
{
    std::string out;
    for (const AttributeList *list : {&vertex, &stageInput}) {
        out.push_back(char(list->has_value()));
        if (!*list)
            continue;
        const uint16_t count = uint16_t((*list)->size());
        out.append(reinterpret_cast<const char *>(&count), sizeof count);
        for (const FunctionAttributeTag &a : **list) {
            out.append(a.name.c_str(), a.name.size() + 1);
            out.push_back(char(a.index));
            out.push_back(char(a.type));
            out.push_back(char((a.active ? 1 : 0) | (a.patchData ? 2 : 0) | (a.controlPointData ? 4 : 0)));
        }
    }
    return out;
}

inline bool decodeAttributeLists(const std::string &blob, AttributeList &vertex, AttributeList &stageInput)
{
    TagReader r(reinterpret_cast<const uint8_t *>(blob.data()), blob.size());
    for (AttributeList *list : {&vertex, &stageInput}) {
        uint8_t present = 0;
        if (!r.u8(present) || present > 1)
            return false;
        list->reset();
        if (!present)
            continue;
        uint16_t count = 0;
        if (!r.u16(count))
            return false;
        list->emplace(count);
        for (FunctionAttributeTag &a : **list) {
            uint8_t flags = 0;
            if (!r.cstr(a.name) || !r.u8(a.index) || !r.u8(a.type) || !r.u8(flags) || flags > 7)
                return false;
            a.active = flags & 1;
            a.patchData = flags & 2;
            a.controlPointData = flags & 4;
        }
    }
    return r.done();
}

}  // namespace raw
