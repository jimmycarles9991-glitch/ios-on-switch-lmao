// SPDX-License-Identifier: MIT
// A metallib's functions by name with their AIR hashes: each function entry's HASH tag, the SHA-256 of its bitcode (the layout
// m2v's metal2vulkan-air container/mtlb.rs reads, which checks the hash against the bitcode). Only the header and the function
// list are read, not the bitcode.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace raw {

using AirHash = std::array<uint8_t, 32>;

// false: not a metallib whose function list this reads (`out` is left empty then)
bool metallibAirHashes(const std::string &path, std::map<std::string, AirHash> &out);
bool metallibAirHashes(const uint8_t *file, size_t size, std::map<std::string, AirHash> &out);

}  // namespace raw
