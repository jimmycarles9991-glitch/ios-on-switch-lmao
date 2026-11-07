// SPDX-License-Identifier: MIT
// The record cache's and compile table's checks (runtime/raw_cache.h), over any records: raw/test/cachecheck's, a bucket's real
// ones, and //drivers/TegraGM20BMetal/test:raw_cache_test's made up ones.
//  - every record and a refusal come back from an entry as they went in, and an entry of another key, stack or AIR, a truncated
//    one and one with a byte changed come back as none;
//  - a second cache over the same directory (the next boot) finds every entry; another stack's directory and a dead writer's
//    temporary are gone once it opens; four processes storing the same entries while this one reads never show a torn entry;
//  - a capped cache stays under its cap and keeps the latest entry;
//  - eight threads asking for one state make it once; the next process's table takes it (and a refusal) from the disk, and
//    keeps no entry for a probe that failed.
#pragma once
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../runtime/raw_cache.h"

namespace cachechecks {

inline int failures = 0;

inline void check(bool ok, const char *what)
{
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

inline bool same(const raw::RuntimeRecord &a, const raw::RuntimeRecord &b)
{
    auto program = [](const raw::CapturedProgram &x, const raw::CapturedProgram &y) {
        return x.id == y.id && x.image == y.image && x.data == y.data && x.cbufTypes == y.cbufTypes;
    };
    return !memcmp(&a.pipeline, &b.pipeline, sizeof a.pipeline) && a.keyString == b.keyString &&
           program(a.programs[0], b.programs[0]) && program(a.programs[1], b.programs[1]) && a.blob == b.blob &&
           a.extras == b.extras && a.restore == b.restore && a.samplers == b.samplers;
}

inline raw::CacheKey keyOf(uint64_t stack, const std::string &key, uint8_t air)
{
    raw::CacheKey k;
    k.stack = stack;
    k.key = key;
    memset(k.vertexAir, air, sizeof k.vertexAir);
    memset(k.fragmentAir, air + 1, sizeof k.fragmentAir);
    return k;
}

inline void setOld(const std::string &path)
{
    struct timeval old[2] = {{time(nullptr) - 3600, 0}, {time(nullptr) - 3600, 0}};
    utimes(path.c_str(), old);
}

// The cache's checks over `records` (state key, record) and a refused state's key and reason, in directories under `scratch`
inline void run(const std::vector<std::pair<std::string, raw::RuntimeRecord>> &records, const std::string &refusedKey,
                const std::string &refusal, const std::string &scratch)
{
    const uint64_t stack = 0x5717c4e5;

    // entries
    uint32_t roundTrips = 0;
    for (const auto &[key, rec] : records) {
        const raw::CacheKey k = keyOf(stack, key, 7);
        const std::string bytes = raw::encodeEntry(k, &rec, "");
        raw::RuntimeRecord back;
        std::string why;
        roundTrips += raw::decodeEntry(bytes, k, back, why) == raw::RecordCache::Found::Record && same(rec, back);
        const raw::CacheKey others[3] = {keyOf(stack + 1, key, 7), keyOf(stack, key, 9), keyOf(stack, key + " ", 7)};
        for (const raw::CacheKey &o : others)
            check(raw::decodeEntry(bytes, o, back, why) == raw::RecordCache::Found::None, "another key's entry missed");
        std::string bent = bytes;
        bent[bent.size() / 2] ^= 1;
        check(raw::decodeEntry(bent, k, back, why) == raw::RecordCache::Found::None, "a changed byte missed");
        check(raw::decodeEntry(bytes.substr(0, bytes.size() - 1), k, back, why) == raw::RecordCache::Found::None,
              "a truncated entry missed");
    }
    check(roundTrips == records.size(), "every record round-trips");
    {
        const raw::CacheKey k = keyOf(stack, refusedKey, 7);
        raw::RuntimeRecord back;
        std::string why;
        check(raw::decodeEntry(raw::encodeEntry(k, nullptr, refusal), k, back, why) ==
                      raw::RecordCache::Found::Refusal &&
                  why == refusal,
              "a refusal round-trips");
    }

    // the cache across processes and boots
    const std::string root = scratch + "/cachecheck-disk";
    system(("rm -rf '" + root + "'").c_str());
    mkdir(root.c_str(), 0700);
    const std::string stale = root + "/raw-00000000deadbeef";
    mkdir(stale.c_str(), 0700);
    fclose(fopen((stale + "/0.rec").c_str(), "w"));
    {
        raw::RecordCache cache;
        check(cache.open(root, stack, 0), "the cache opens");
        check(access(stale.c_str(), F_OK) != 0, "another stack's directory removed");
        const pid_t me = getpid();
        std::atomic<uint32_t> torn{0}, seen{0};
        std::vector<pid_t> writers;
        for (int w = 0; w < 4; w++)
            if (const pid_t pid = fork(); pid == 0) {
                raw::RecordCache mine;
                mine.open(root, stack, 0);
                for (int round = 0; round < 20; round++)
                    for (const auto &[key, rec] : records)
                        mine.store(keyOf(stack, key, 7), &rec, "");
                _exit(0);
            } else
                writers.push_back(pid);
        for (int round = 0; round < 200 && getpid() == me; round++)
            for (const auto &[key, rec] : records) {
                raw::RuntimeRecord back;
                std::string why;
                const raw::CacheKey k = keyOf(stack, key, 7);
                const auto found = cache.load(k, back, why);
                seen += found == raw::RecordCache::Found::Record;
                torn += found == raw::RecordCache::Found::Record && !same(rec, back);
            }
        for (pid_t pid : writers) {
            int status = 0;
            waitpid(pid, &status, 0);
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a writer process exits");
        }
        check(torn == 0 && seen > 0, "no reader sees a torn entry while four processes write");
        check(cache.store(keyOf(stack, refusedKey, 7), nullptr, refusal), "a refusal stored");
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/raw-%016llx/99-0.tmp", (unsigned long long)stack);
    fclose(fopen((root + tmp).c_str(), "w"));
    setOld(root + tmp);
    uint32_t found = 0;
    {
        raw::RecordCache next;
        check(next.open(root, stack, 0), "the next boot's cache opens");
        check(access((root + tmp).c_str(), F_OK) != 0, "a dead writer's temporary removed");
        for (const auto &[key, rec] : records) {
            raw::RuntimeRecord back;
            std::string why;
            found += next.load(keyOf(stack, key, 7), back, why) == raw::RecordCache::Found::Record && same(rec, back);
        }
        raw::RuntimeRecord back;
        std::string why;
        check(next.load(keyOf(stack, refusedKey, 7), back, why) == raw::RecordCache::Found::Refusal && why == refusal,
              "the next boot finds the refusal");
    }
    check(found == records.size(), "the next boot finds every record");

    // the cap
    {
        const std::string capped = scratch + "/cachecheck-cap";
        system(("rm -rf '" + capped + "'").c_str());
        mkdir(capped.c_str(), 0700);
        const uint64_t one = raw::encodeEntry(keyOf(stack, records[0].first, 7), &records[0].second, "").size();
        const uint64_t cap = one * 3;
        raw::RecordCache cache;
        cache.open(capped, stack, cap);
        for (const auto &[key, rec] : records)
            cache.store(keyOf(stack, key, 7), &rec, "");
        raw::RuntimeRecord back;
        std::string why;
        check(cache.bytes() <= cap, "a capped cache stays under its cap");
        check(cache.load(keyOf(stack, records.back().first, 7), back, why) == raw::RecordCache::Found::Record,
              "a capped cache keeps the latest entry");
        system(("rm -rf '" + capped + "'").c_str());
    }

    // the compile table
    {
        const std::string tableDir = scratch + "/cachecheck-table";
        system(("rm -rf '" + tableDir + "'").c_str());
        mkdir(tableDir.c_str(), 0700);
        raw::RecordCache cache;
        cache.open(tableDir, stack, 0);
        std::atomic<uint32_t> makes{0}, dones{0};
        const raw::RuntimeRecord &model = records[0].second;
        auto make = [&](const raw::CacheKey &k, raw::RuntimeRecord &rec, std::string &why) {
            makes++;
            usleep(20000);
            if (k.key == refusedKey) {
                why = refusal;
                return raw::Compiler::Outcome::Refused;
            }
            if (k.key == "failed") {
                why = "no capture";
                return raw::Compiler::Outcome::Failed;
            }
            rec = model;
            return raw::Compiler::Outcome::Made;
        };
        const raw::CacheKey k = keyOf(stack, records[0].first, 7);
        {
            raw::Compiler compiler(&cache, make);
            std::vector<std::thread> askers;
            for (int t = 0; t < 8; t++)
                askers.emplace_back([&] {
                    compiler.ask(k, [&](const raw::RuntimeRecord *, const std::string &) {
                        dones++;
                    });
                });
            for (std::thread &t : askers)
                t.join();
            compiler.ask(keyOf(stack, refusedKey, 7), nullptr);
            compiler.ask(keyOf(stack, "failed", 7), nullptr);
            compiler.drain();
            const raw::RuntimeRecord *rec = nullptr;
            check(compiler.ask(k, nullptr, &rec) == raw::Compiler::State::Ready && rec && same(*rec, model),
                  "an asked state is ready");
            check(makes == 3 && dones == 1, "eight threads asking one state make it once");
            check(compiler.ask(keyOf(stack, "failed", 7), nullptr) == raw::Compiler::State::Refused,
                  "a failed probe is not tried again in the process");
            const raw::Compiler::Stats s = compiler.stats();
            check(s.misses == 3 && s.hits == 0 && s.refusals == 1 && s.failures == 1, "first process: three misses");
        }
        {
            raw::RecordCache again;
            again.open(tableDir, stack, 0);
            raw::Compiler compiler(&again, make);
            compiler.ask(k, nullptr);
            compiler.ask(keyOf(stack, refusedKey, 7), nullptr);
            compiler.ask(keyOf(stack, "failed", 7), nullptr);
            compiler.drain();
            const raw::Compiler::Stats s = compiler.stats();
            check(makes == 4 && s.hits == 2 && s.misses == 1,
                  "the next process takes the record and refusal from disk");
            check(compiler.ask(keyOf(stack, refusedKey, 7), nullptr) == raw::Compiler::State::Refused,
                  "a refusal from disk stays refused");
        }
        system(("rm -rf '" + tableDir + "'").c_str());
    }
    system(("rm -rf '" + root + "'").c_str());
}

}  // namespace cachechecks
