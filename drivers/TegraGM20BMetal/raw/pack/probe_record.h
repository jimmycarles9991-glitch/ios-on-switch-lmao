// SPDX-License-Identifier: MIT
// One probe draw as nvdec reads it in NVDEC_PROBE mode: its "P" lines, which pack/probelog.py reads off the log, as a struct the
// runtime's record maker (raw/runtime/raw_record.h) takes.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nvdec {

struct ProbeRecord {
    std::map<uint32_t, uint32_t> regs;                     // the 3D registers that are not 0 (P regs)
    std::vector<uint32_t> root;                            // the root table's first 0x780 bytes
    std::vector<std::pair<uint32_t, uint32_t>> pushWords;  // the root-table words [0x700, 0x780) the submission wrote
    std::vector<uint32_t> set0;                            // descriptor set 0's words (empty: none)
    // by byte offset in set 0: the 64 words a mapped pointer there reaches
    std::map<uint32_t, std::vector<uint32_t>> set0Pointers;

    struct Header {
        std::string name;  // the persistent resource the header's address names (nvdec's names)
        std::array<uint32_t, 8> w{};
    };

    std::map<uint32_t, Header> tic;
    std::map<uint32_t, std::array<uint32_t, 8>> tsc;

    struct Stream {
        uint32_t format = 0;
        std::vector<uint32_t> words;  // 64 words at the stream's location
    };

    std::map<uint32_t, Stream> streams;  // the enabled vertex streams

    struct Program {
        std::string id;  // nvdec's name for it
        bool known = false;
    };

    std::map<int, Program> programs;  // the bound stages (1 vertex .. 5 pixel)
};

// A probe's submission: a record per draw, what else it ran, and its render targets by name (the canonical "rt<i>=<name>" lines)
struct ProbeSubmission {
    std::vector<ProbeRecord> records;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    std::map<std::string, uint32_t> targets;
};

}  // namespace nvdec
