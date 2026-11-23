// SPDX-License-Identifier: MIT
// A made-up universal metallib (an Apple GPU slice, then an AIR64 slice whose function list names the given functions,
// function i's HASH filled with `seed + i`), as metallib.h reads a framework's
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace made_metallib {

inline void tag(std::vector<uint8_t> &out, const char *fourcc, const void *data, uint16_t length)
{
    out.insert(out.end(), fourcc, fourcc + 4);
    out.push_back(uint8_t(length));
    out.push_back(uint8_t(length >> 8));
    const auto *b = static_cast<const uint8_t *>(data);
    out.insert(out.end(), b, b + length);
}

template <class T> void put(std::vector<uint8_t> &out, size_t at, T v)
{
    memcpy(&out[at], &v, sizeof v);
}

inline void bigEndian(std::vector<uint8_t> &out, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

// `types`: each function's TYPE tag (0 vertex, 1 fragment, 2 kernel); fragment where it has none
inline std::string universal(const std::vector<std::string> &functions, uint8_t seed = 0xA0, uint8_t filler = 0xEE,
                             const std::vector<uint8_t> &types = {})
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4);
    put(list, 0, uint32_t(functions.size()));
    for (size_t i = 0; i < functions.size(); i++) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", functions[i].c_str(), uint16_t(functions[i].size() + 1));
        const uint8_t type = i < types.size() ? types[i] : 1;
        tag(e, "TYPE", &type, 1);
        uint8_t hash[32];
        memset(hash, seed + int(i), sizeof hash);
        tag(e, "HASH", hash, sizeof hash);
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    std::vector<uint8_t> file(0x100, 0);
    bigEndian(file, 0, 0xCAFEBABE);
    bigEndian(file, 4, 2);
    bigEndian(file, 8, 0x01000013);
    bigEndian(file, 16, 0x100);
    bigEndian(file, 20, 16);
    bigEndian(file, 28, 0x01000017);
    bigEndian(file, 36, 0x110);
    bigEndian(file, 40, uint32_t(slice.size()));
    file.resize(0x110, filler);
    file.insert(file.end(), slice.begin(), slice.end());
    return std::string(file.begin(), file.end());
}

// LLVM's bitcode wrapper magic, then `seed` filling `bytes` in all: a function's bitcode as a metallib places it
inline std::string wrappedBitcode(uint8_t seed, size_t bytes = 64)
{
    std::string b(bytes, char(seed));
    const char magic[4] = {char(0xde), char(0xc0), 0x17, 0x0b};
    b.replace(0, sizeof magic, magic, sizeof magic);
    return b;
}

// A thin metallib, as the system's Metal front end makes one: function i of TYPE types[i], its bitcode bitcodes[i] placed
// in the bitcode section (OFFT, MDSZ; the section's offset and size at 72 and 80), and its public metadata record's tags
// publics[i] (made with tag()) in the public metadata section (OFFT's first offset; the section's at 40 and 48)
inline std::string thin(const std::vector<std::string> &functions, const std::vector<uint8_t> &types,
                        const std::vector<std::string> &bitcodes, const std::vector<std::vector<uint8_t>> &publics = {})
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4), records;
    put(list, 0, uint32_t(functions.size()));
    uint64_t placed = 0;
    for (size_t i = 0; i < functions.size(); i++) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", functions[i].c_str(), uint16_t(functions[i].size() + 1));
        tag(e, "TYPE", &types[i], 1);
        uint8_t hash[32];
        memset(hash, 0xB0 + int(i), sizeof hash);
        tag(e, "HASH", hash, sizeof hash);
        const uint64_t offsets[3] = {records.size(), 0, placed}, size = bitcodes[i].size();
        tag(e, "OFFT", offsets, sizeof offsets);
        tag(e, "MDSZ", &size, sizeof size);
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
        placed += size;
        std::vector<uint8_t> record(4);
        if (i < publics.size())
            record.insert(record.end(), publics[i].begin(), publics[i].end());
        record.insert(record.end(), {'E', 'N', 'D', 'T'});
        put(record, 0, uint32_t(record.size() - 4));
        records.insert(records.end(), record.begin(), record.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    put(slice, 40, uint64_t(slice.size()));
    put(slice, 48, uint64_t(records.size()));
    slice.insert(slice.end(), records.begin(), records.end());
    put(slice, 72, uint64_t(slice.size()));
    put(slice, 80, placed);
    for (const std::string &b : bitcodes)
        slice.insert(slice.end(), b.begin(), b.end());
    return std::string(slice.begin(), slice.end());
}

}  // namespace made_metallib
