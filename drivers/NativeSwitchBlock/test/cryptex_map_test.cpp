// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tests SwitchCryptexMap.h's extent merging and request splitting against a brute-force model on random maps.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "../SwitchCryptexMap.h"

static unsigned long failures;
static void report(const char *file, int line, const char *format, ...) __attribute__((format(printf, 3, 4)));

static void report(const char *file, int line, const char *format, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    ADD_FAILURE_AT(file, line) << text;
}

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            if (failures++ < 20)                         \
                report(__FILE__, __LINE__, __VA_ARGS__); \
        }                                                \
    } while (0)

static std::mt19937_64 rng(17);

TEST(CryptexMap, RequestsSplitIntoPiecesOfOneExtent)
{
    failures = 0;
    const uint64_t kBlock = 4096;
    unsigned long reads = 0, pieces = 0;

    for (int round = 0; round < 3000; ++round) {
        const uint32_t parts = 1 + rng() % 40;
        std::vector<SwitchExtent> map(parts + 1);
        std::vector<uint64_t> diskOf;
        uint32_t count = 0;
        uint64_t file = 0, disk = (rng() % 1000) * 512;
        for (uint32_t i = 0; i < parts; ++i) {
            const uint64_t length = (1 + rng() % 600) * kBlock;
            if (rng() % 7 == 0) {
                CHECK(switchAppendExtent(map.data(), &count, parts, file, kSwitchHole, length), "append refused\n");
                for (uint64_t b = 0; b < length / kBlock; ++b)
                    diskOf.push_back(kSwitchHole);
                file += length;
                continue;
            }
            if (rng() % 3)
                disk += (1 + rng() % 5000) * 512;
            CHECK(switchAppendExtent(map.data(), &count, parts, file, disk, length), "append refused\n");
            for (uint64_t b = 0; b < length / kBlock; ++b)
                diskOf.push_back(disk + b * kBlock);
            file += length;
            disk += length;
        }
        CHECK(switchExtentsCover(map.data(), count, file), "map does not cover\n");
        CHECK(!switchExtentsCover(map.data(), count, file + kBlock), "cover accepts a short map\n");
        for (uint32_t i = 1; i < count; ++i) {
            const bool holes = map[i - 1].disk == kSwitchHole && map[i].disk == kSwitchHole;
            const bool joined = map[i - 1].disk != kSwitchHole && map[i].disk != kSwitchHole &&
                                map[i - 1].disk + map[i - 1].length == map[i].disk;
            CHECK(!holes && !joined, "adjacent extents not merged\n");
        }
        const uint64_t blocks = file / kBlock;
        for (int q = 0; q < 50; ++q) {
            const uint64_t start = rng() % blocks, n = 1 + rng() % std::min<uint64_t>(blocks - start, 300);
            const uint64_t maxPiece = (rng() % 2 ? 2048 : 512) * 512ull;
            uint32_t at = switchExtentAt(map.data(), count, start * kBlock);
            uint64_t done = 0;
            while (done < n * kBlock) {
                const SwitchPiece piece =
                    switchNextPiece(map.data(), count, &at, start * kBlock + done, n * kBlock - done, maxPiece);
                CHECK(piece.length && piece.length <= maxPiece && piece.length % kBlock == 0, "bad piece length\n");
                if (!piece.length)
                    break;
                for (uint64_t b = 0; b < piece.length / kBlock; ++b) {
                    const uint64_t want = diskOf[start + done / kBlock + b];
                    CHECK(piece.disk == kSwitchHole ? want == kSwitchHole : piece.disk + b * kBlock == want,
                          "block mapped wrong\n");
                }
                const uint32_t e = switchExtentAt(map.data(), count, start * kBlock + done);
                CHECK(start * kBlock + done + piece.length <= map[e].file + map[e].length, "piece crosses extents\n");
                done += piece.length;
                ++pieces;
            }
            CHECK(done == n * kBlock, "request not tiled\n");
            ++reads;
        }
    }

    EXPECT_EQ(failures, 0u) << "checks failed (the first 20 reported above)";
    printf("cryptex map: %lu reads in %lu pieces\n", reads, pieces);
}
