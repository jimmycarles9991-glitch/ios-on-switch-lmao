// SPDX-License-Identifier: MIT
// A metallib function's public metadata as MTLFunction answers it, decoding no AIR (m2v's metal2vulkan reflection/function.rs
// metallib_properties and metallib_constants, measured on an M4 over its reflection fixtures). OFFT's first u64 places the
// function's record in the public metadata section (header offset 40): a u32 size, then tags up to ENDT. CNST lists the
// constants the function reads: a u16 count, then each one's name, MTLDataType byte, u16 index and a flag byte that is
// `required` (0 exactly for a constant the function asks is_function_constant_defined about). VATT lists the stage-input
// attributes in declaration order: a u16 count, then each one's name and a u16 holding the index in its low five bits, bit 15
// for isActive, bit 14 for control-point data and bit 13 for per-patch data. VATY gives each one's MTLDataType byte.
//
// The function list is walked here rather than through MetallibEntries.h, whose entry carries no public metadata offset: that
// header is one of the raw stack's sources, which the pack is keyed by.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm20b {

struct FunctionConstantTag {
    std::string name;
    uint8_t type = 0;
    uint16_t index = 0;
    bool required = false;
};

struct FunctionAttributeTag {
    std::string name;
    uint8_t index = 0, type = 0;
    bool active = false, patchData = false, controlPointData = false;
};

struct FunctionTags {
    // nullopt: a CNST entry Metal's answer was not measured for, which m2v answered as no constants
    std::optional<std::vector<FunctionConstantTag>> constants;
    // nullopt: unmeasured (the TYPE, the TESS byte, a VATT bit or a type byte), which m2v answered as nil; empty: no VATT, which
    // Metal answers as nil
    std::optional<std::vector<FunctionAttributeTag>> attributes;
};

// The MTLDataType values m2v's DataType names (metal2vulkan-air metal/generated.rs)
inline bool measuredDataType(uint8_t t)
{
    return t <= 56 || (t >= 58 && t <= 60) || (t >= 62 && t <= 88) || (t >= 115 && t <= 118) ||
           (t >= 121 && t <= 124) || t == 139 || t == 140;
}

// TYPE values whose MTLFunctionType m2v measured: vertex, fragment, kernel, visible, intersection, mesh, object
inline bool measuredFunctionType(uint8_t t)
{
    return t <= 2 || t == 4 || t == 6 || t == 7 || t == 8;
}

// A triangle or quad patch of 0 to 32 control points, or one that names none (63)
inline bool measuredTess(uint8_t t)
{
    const uint8_t kind = t & 3, count = t >> 2;
    return (kind == 1 || kind == 2) && (count <= 32 || count == 63);
}

class TagReader {
public:
    TagReader(const uint8_t *at, size_t size) : at_(at), end_(at + size)
    {}

    bool u8(uint8_t &v)
    {
        return take(&v, 1);
    }

    bool u16(uint16_t &v)
    {
        return take(&v, 2);
    }

    bool bytes(void *v, size_t n)
    {
        return take(v, n);
    }

    bool cstr(std::string &v)
    {
        const void *nul = memchr(at_, 0, size_t(end_ - at_));
        if (!nul)
            return false;
        v.assign(reinterpret_cast<const char *>(at_));
        at_ = static_cast<const uint8_t *>(nul) + 1;
        return true;
    }

    bool done() const
    {
        return at_ == end_;
    }

private:
    bool take(void *v, size_t n)
    {
        if (size_t(end_ - at_) < n)
            return false;
        memcpy(v, at_, n);
        at_ += n;
        return true;
    }

    const uint8_t *at_, *end_;
};

// Each tag (fourcc, u16 length, bytes) from `t` to ENDT; false where the list runs past `end` or names a tag twice
template <class Visit> bool forEachTag(const uint8_t *t, const uint8_t *end, Visit &&visit)
{
    std::vector<std::string_view> seen;
    for (;;) {
        if (end - t < 4)
            return false;
        const std::string_view tag(reinterpret_cast<const char *>(t), 4);
        if (tag == "ENDT")
            return true;
        uint16_t n = 0;
        if (end - t < 6 || (memcpy(&n, t + 4, 2), n > end - (t + 6)))
            return false;
        if (std::find(seen.begin(), seen.end(), tag) != seen.end())
            return false;
        seen.push_back(tag);
        visit(tag, t + 6, size_t(n));
        t += 6 + n;
    }
}

inline std::optional<std::vector<FunctionConstantTag>> decodeConstants(const uint8_t *v, size_t n)
{
    TagReader r(v, n);
    uint16_t count = 0;
    if (!r.u16(count))
        return std::nullopt;
    std::vector<FunctionConstantTag> out(count);
    for (FunctionConstantTag &c : out) {
        uint8_t flag = 0;
        if (!r.cstr(c.name) || !r.u8(c.type) || !r.u16(c.index) || !r.u8(flag) || flag > 1 || !measuredDataType(c.type))
            return std::nullopt;
        c.required = flag;
    }
    return r.done() ? std::optional(out) : std::nullopt;
}

inline bool decodeAttributeNames(const uint8_t *v, size_t n, std::vector<FunctionAttributeTag> &out)
{
    constexpr uint16_t kActive = 0x8000, kControlPoint = 0x4000, kPatch = 0x2000, kIndex = 0x1f;
    TagReader r(v, n);
    uint16_t count = 0;
    if (!r.u16(count))
        return false;
    out.resize(count);
    for (FunctionAttributeTag &a : out) {
        uint16_t packed = 0;
        if (!r.cstr(a.name) || !r.u16(packed) || (packed & ~(kActive | kControlPoint | kPatch | kIndex)))
            return false;
        a.index = uint8_t(packed & kIndex);
        a.active = packed & kActive;
        a.controlPointData = packed & kControlPoint;
        a.patchData = packed & kPatch;
    }
    return r.done();
}

inline bool decodeAttributeTypes(const uint8_t *v, size_t n, std::vector<uint8_t> &out)
{
    TagReader r(v, n);
    uint16_t count = 0;
    if (!r.u16(count))
        return false;
    out.resize(count);
    for (uint8_t &t : out)
        if (!r.u8(t) || !measuredDataType(t))
            return false;
    return r.done();
}

// `name`'s TYPE, TESS and public metadata offset from the function list (header offset 24)
struct ListedFunction {
    std::optional<uint8_t> type, tess;
    std::optional<uint64_t> publicAt;
};

inline bool listedFunction(const uint8_t *file, size_t length, std::string_view name, ListedFunction &out)
{
    constexpr size_t kHeaderBytes = 88;
    if (!file || length < kHeaderBytes || memcmp(file, "MTLB", 4))
        return false;
    uint64_t listAt = 0;
    memcpy(&listAt, file + 24, 8);
    if (listAt > length - 4)
        return false;
    uint32_t count = 0;
    memcpy(&count, file + listAt, 4);
    for (size_t at = listAt + 4; count--;) {
        uint32_t size = 0;
        if (at > length - 4 || (memcpy(&size, file + at, 4), size < 8 || size > length - at))
            return false;
        std::string entryName;
        ListedFunction entry;
        const bool whole =
            forEachTag(file + at + 4, file + at + size, [&](std::string_view tag, const uint8_t *v, size_t n) {
                if (tag == "NAME")
                    entryName.assign(reinterpret_cast<const char *>(v), strnlen(reinterpret_cast<const char *>(v), n));
                else if (tag == "TYPE" && n >= 1)
                    entry.type = v[0];
                else if (tag == "TESS" && n == 1)
                    entry.tess = v[0];
                else if (tag == "OFFT" && n >= 24) {
                    uint64_t publicAt = 0;
                    memcpy(&publicAt, v, 8);
                    entry.publicAt = publicAt;
                }
            });
        if (!whole)
            return false;
        if (entryName == name) {
            out = entry;
            return true;
        }
        at += size;
    }
    return false;
}

// What `name`'s list entry and public metadata say of it outside the bitcode its HASH tag names, raw: its TYPE and TESS
// bytes (0xff for none) and its public metadata record whole (its constants, attributes and their types among its tags).
// A library could pair one bitcode with any of them, so the store keys a stage by them beside its AIR (store_key.h). Empty
// where the library lists no such function.
inline std::string listedFacts(const uint8_t *file, size_t length, std::string_view name)
{
    ListedFunction f;
    if (!listedFunction(file, length, name, f))
        return std::string();
    std::string out = {char(f.type ? *f.type : 0xff), char(f.tess ? *f.tess : 0xff)};
    uint64_t sectionAt = 0, sectionBytes = 0;
    memcpy(&sectionAt, file + 40, 8);
    memcpy(&sectionBytes, file + 48, 8);
    if (!f.publicAt || sectionAt > length || sectionBytes > length - sectionAt || *f.publicAt > sectionBytes ||
        sectionBytes - *f.publicAt < 4)
        return out;
    const uint8_t *record = file + sectionAt + *f.publicAt;
    uint32_t size = 0;
    memcpy(&size, record, 4);
    out.append(reinterpret_cast<const char *>(record),
               std::min<uint64_t>(4 + uint64_t(size), sectionBytes - *f.publicAt));
    return out;
}

// `name`'s constants and attributes in a metallib (Metal's AIR slice of one); false where the library lists no such function or
// its records are malformed (m2v refused such a library whole)
inline bool functionTags(const uint8_t *file, size_t length, std::string_view name, FunctionTags &out)
{
    ListedFunction f;
    if (!listedFunction(file, length, name, f) || !f.publicAt)
        return false;
    uint64_t sectionAt = 0, sectionBytes = 0;
    memcpy(&sectionAt, file + 40, 8);
    memcpy(&sectionBytes, file + 48, 8);
    if (sectionAt > length || sectionBytes > length - sectionAt || *f.publicAt > sectionBytes ||
        sectionBytes - *f.publicAt < 4)
        return false;
    const uint8_t *record = file + sectionAt + *f.publicAt;
    uint32_t size = 0;
    memcpy(&size, record, 4);
    const uint8_t *end = record + std::min<uint64_t>(4 + uint64_t(size), sectionBytes - *f.publicAt);
    std::vector<FunctionAttributeTag> attributes;
    std::vector<uint8_t> types;
    bool attributesWhole = true;
    out.constants.emplace();
    const bool whole = forEachTag(record + 4, end, [&](std::string_view tag, const uint8_t *v, size_t n) {
        if (tag == "CNST")
            out.constants = decodeConstants(v, n);
        else if (tag == "VATT")
            attributesWhole &= decodeAttributeNames(v, n, attributes);
        else if (tag == "VATY")
            attributesWhole &= decodeAttributeTypes(v, n, types);
    });
    if (!whole)
        return false;
    out.attributes.reset();
    if (attributesWhole && f.type && measuredFunctionType(*f.type) && (!f.tess || measuredTess(*f.tess)) &&
        attributes.size() == types.size()) {
        for (size_t i = 0; i < attributes.size(); i++)
            attributes[i].type = types[i];
        out.attributes = std::move(attributes);
    }
    return true;
}

}  // namespace gm20b
