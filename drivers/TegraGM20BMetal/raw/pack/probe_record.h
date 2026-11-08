// SPDX-License-Identifier: MIT
// One probe draw as nvdec reads it (Decoder::probeRecord): what its NVDEC_PROBE "P" lines print, which pack/probelog.py reads off
// the log, and what the runtime's record maker (raw/runtime/raw_record.h) takes.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nvdec {

struct ProbeRecord {
    std::map<uint32_t, uint32_t> regs;  // the 3D registers that are not 0 (P regs)

    struct Bind {
        int group, slot;
        uint64_t addr;
        uint32_t bytes;
    };

    std::vector<Bind> binds;  // the bound constant buffers

    struct Words {
        uint64_t va = 0;
        std::vector<uint32_t> words;
    };

    Words root;                                            // the root table's first 0x780 bytes
    std::vector<std::pair<uint32_t, uint32_t>> pushWords;  // the root-table words [0x700, 0x780) the submission wrote
    Words sets[2];                                         // descriptor sets 0 and 1 (empty: none)
    std::map<uint32_t, Words> setPointers[2];  // by byte offset in the set: the 64 words a mapped pointer there reaches
    std::map<uint32_t, Words> pushPointers;    // likewise, by byte offset in the push words

    struct Header {
        std::string name;  // the persistent resource the header's address names (nvdec's names)
        std::array<uint32_t, 8> w{};
    };

    std::map<uint32_t, Header> tic;
    std::map<uint32_t, std::array<uint32_t, 8>> tsc;

    struct Stream {
        uint32_t format = 0;
        uint64_t location = 0, limit = 0;
        std::vector<uint32_t> words;  // 64 words at the location
    };

    std::map<uint32_t, Stream> streams;  // the enabled vertex streams

    struct Program {
        std::string id;  // nvdec's name for it
        uint32_t offset = 0;
        bool known = false;
    };

    std::map<int, Program> programs;           // the bound stages (1 vertex .. 5 pixel)
    uint32_t first = 0, count = 0, begin = 0;  // the draw
    std::map<std::string, uint32_t> targets;   // its live color targets by name
};

// A probe's submission: a record per draw, what else it ran, and its draws' render targets by name
struct ProbeSubmission {
    std::vector<ProbeRecord> records;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    std::map<std::string, uint32_t> targets;
};

}  // namespace nvdec
