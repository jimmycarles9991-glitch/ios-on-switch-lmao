// SPDX-License-Identifier: MIT
// What the checks the store, the request log, the record entries and the compile replies no longer run cost a byte on
// the host: the byte-wise FNV-1a 64 of an entry's tail and the prefetch list, the FNV-1a 32 of a store slot and a logged
// request, and CommonCrypto's SHA-256 of a reply, a client library or a request. Each is timed over the sizes they read
// (a record entry, a logged request, a metallib) and the fastest of the rounds kept, since a shared executor's other
// work only slows a round. It always passes: run it for its lines.
//   bazel test --config=agent //kits/SwitchSeedPack/test:hash_cost_bench_test --test_output=all
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint32_t fnv32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

uint64_t sha(const uint8_t *p, size_t n)
{
    uint8_t out[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p, CC_LONG(n), out);
    uint64_t first = 0;
    memcpy(&first, out, sizeof first);
    return first;
}

template <class F> double fastestNsPerByte(const std::vector<uint8_t> &bytes, F f)
{
    volatile uint64_t sink = 0;
    double best = 1e9;
    for (int round = 0; round < 25; ++round) {
        const auto begin = std::chrono::steady_clock::now();
        sink = sink + f(bytes.data(), bytes.size());
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
        best = std::min(best, ns / double(bytes.size()));
    }
    return best;
}

TEST(HashCost, TheRemovedChecksNsPerByte)
{
    for (const size_t size : {size_t(32) << 10, size_t(1) << 20, size_t(8) << 20}) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i)
            bytes[i] = uint8_t(i * 131 + (i >> 7));
        printf("HASHCOST bytes=%zu fnv64=%.3f fnv32=%.3f sha256=%.3f ns/byte\n", size, fastestNsPerByte(bytes, fnv64),
               fastestNsPerByte(bytes,
                                [](const uint8_t *p, size_t n) {
                                    return uint64_t(fnv32(p, n));
                                }),
               fastestNsPerByte(bytes, sha));
    }
}

}  // namespace
