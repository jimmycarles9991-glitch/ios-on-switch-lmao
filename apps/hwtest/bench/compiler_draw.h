// SPDX-License-Identifier: MIT
#pragma once
#include "apps/hwtest/bench/bench_device.h"
#include "drivers/TegraGM20BMetal/record.h"

namespace bench::compiler {
constexpr uint32_t width = 1280, height = 720;

struct Block {
    uint32_t stage, slot, offset;
    std::vector<uint8_t> bytes;
};

struct Seed {
    uint32_t number;
    std::vector<Block> blocks;
};

struct Input {
    std::string key;
    gm20b::RuntimeRecord nak;
    gm20b::CapturedProgram nvidia[2];
    std::vector<uint32_t> colors;
    uint32_t samples = 0, depth = 0;
    std::vector<uint8_t> kinds;
    std::vector<Seed> seeds;

    const gm20b::CapturedProgram &program(bool vendor, unsigned stage) const
    {
        return vendor ? nvidia[stage] : nak.programs[stage];
    }

    bool compositor() const
    {
        return key == "3ef3a99e4f382ce1";
    }
};

struct Result {
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<uint64_t> times;
    std::string clockBefore, clockAfter;
    uint64_t visible = 0;
    bool changed = false, varied = false;
};

struct Resources {
    gm20b::Buffer metadata, tickets, reports;
};

bool draw(gm20b::RenderEncoder &e, gm20b::Pipeline &pipeline, Resources &r, const Input &input, bool vendor,
          std::string &why);
bool run(const Input &input, const Seed &seed, bool vendor, unsigned ordinal, Result &result, std::string &why);
}  // namespace bench::compiler
