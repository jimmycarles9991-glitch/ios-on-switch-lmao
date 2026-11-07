// SPDX-License-Identifier: MIT
// cachecheck: the runtime's record cache and compile table (runtime/raw_cache.h; test/cache_checks.h's checks) on a bucket's
// records (raw::makeRecord over its probe runs), and the AIR hashes that key them (runtime/raw_metallib.h) on a real library: every
// function's HASH tag read is the SHA-256 of its bitcode.
// usage: cachecheck <bucket dir> <pack.bin> <default.metallib> <scratch dir>
#include <CommonCrypto/CommonDigest.h>
#include "../runtime/raw_metallib.h"
#include "cache_checks.h"
#include "probe_logs.h"

namespace {

// Every function's HASH against the SHA-256 of its bitcode (OFFT's third offset from the bitcode section, MDSZ bytes)
uint32_t checkAirHashes(const std::string &path, std::map<std::string, raw::AirHash> &hashes)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!raw::metallibAirHashes(path, hashes) || hashes.empty()) {
        cachechecks::check(false, "metallib function list read");
        return 0;
    }
    std::map<std::string, raw::AirHash> fromBytes;
    cachechecks::check(raw::metallibAirHashes(whole.data(), whole.size(), fromBytes) && fromBytes == hashes,
                       "the same read from the bytes");
    // the AIR64 slice of a universal file (big endian: magic, count, then cputype, subtype, offset, size, align)
    size_t base = 0;
    auto be = [&](size_t at) {
        return uint32_t(whole[at]) << 24 | uint32_t(whole[at + 1]) << 16 | uint32_t(whole[at + 2]) << 8 | whole[at + 3];
    };
    if (be(0) == 0xCAFEBABE)
        for (uint32_t i = 0; i < be(4); i++)
            if (be(8 + 20 * i) == 0x01000017)
                base = be(8 + 20 * i + 8);
    const uint8_t *f = whole.data() + base;
    uint64_t list, bitcode;
    memcpy(&list, &f[24], 8);
    memcpy(&bitcode, &f[72], 8);
    uint32_t count, verified = 0;
    memcpy(&count, &f[list], 4);
    size_t at = list + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bytes;
        memcpy(&bytes, &f[at], 4);
        std::string name;
        uint64_t size = 0, offsets[3] = {};
        for (size_t t = at + 4; memcmp(&f[t], "ENDT", 4);) {
            uint16_t n;
            memcpy(&n, &f[t + 4], 2);
            if (!memcmp(&f[t], "NAME", 4))
                name = reinterpret_cast<const char *>(&f[t + 6]);
            if (!memcmp(&f[t], "MDSZ", 4))
                memcpy(&size, &f[t + 6], 8);
            if (!memcmp(&f[t], "OFFT", 4))
                memcpy(offsets, &f[t + 6], 24);
            t += 6 + n;
        }
        raw::AirHash sha;
        CC_SHA256(&f[bitcode + offsets[2]], CC_LONG(size), sha.data());
        verified += hashes.count(name) && hashes[name] == sha;
        at += bytes;
    }
    cachechecks::check(verified == count && count == hashes.size(), "every HASH tag is its bitcode's SHA-256");
    return verified;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: cachecheck <bucket dir> <pack.bin> <default.metallib> <scratch dir>\n");
        return 2;
    }
    const std::string dir = argv[1], scratch = argv[4];
    probelogs::Run r256, r16, r4096;
    raw::Pack pack;
    std::string err;
    if (!probelogs::loadRun(dir, 256, r256) || !probelogs::loadRun(dir, 16, r16) ||
        !probelogs::loadRun(dir, 4096, r4096) || !pack.load(argv[2], 0, err)) {
        fprintf(stderr, "cachecheck: %s\n", err.empty() ? "probe runs unread" : err.c_str());
        return 1;
    }
    std::map<std::string, raw::AirHash> hashes;
    const uint32_t functions = checkAirHashes(argv[3], hashes);

    const auto programs = probelogs::loadPrograms(dir + "/programs.bin");
    const raw::PackRules rules = raw::PackRules::of(pack, r256.context);
    std::vector<std::pair<std::string, raw::RuntimeRecord>> records;
    std::string refusedKey, refusal;
    for (size_t i = 0; i < r256.probes.size(); i++) {
        const uint32_t index = r256.probes[i].first;
        const std::string &key = r256.probes[i].second;
        auto at = [&](const probelogs::Run &run) -> const nvdec::ProbeSubmission * {
            for (size_t j = 0; j < run.probes.size(); j++)
                if (run.probes[j].first == index)
                    return &run.submissions[j];
            return nullptr;
        };
        raw::RuntimeRecord rec;
        std::string why;
        if (raw::makeRecord(
                {&r256.submissions[i], at(r16), at(r4096), index, key}, rules,
                [&](const std::string &id) {
                    const auto it = programs.find(id);
                    return it == programs.end() ? nullptr : &it->second;
                },
                rec, why))
            records.push_back({key, rec});
        else if (refusedKey.empty())
            refusedKey = key, refusal = why;
    }
    cachechecks::check(!records.empty() && !refusedKey.empty(), "the bucket has records and a refusal");
    cachechecks::run(records, refusedKey, refusal, scratch);
    printf("cachecheck %s: %u functions' AIR hashes, %zu records and a refusal through entries, the disk and the "
           "table: %s\n",
           dir.c_str(), functions, records.size(), cachechecks::failures ? "FAILED" : "ok");
    return cachechecks::failures ? 1 : 0;
}
