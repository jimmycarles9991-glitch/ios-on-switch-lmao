// SPDX-License-Identifier: MIT
// The record cache's and compile table's checks (runtime/raw_cache.h, raw_compiler.h), over any records: raw/test/cachecheck's, a bucket's real
// ones, and //drivers/TegraGM20BMetal/test:raw_cache_test's made up ones.
//  - every record and a refusal come back from an entry as they went in, and an entry of another key, stack or AIR, a truncated
//    one and one with a byte changed come back as none;
//  - a second cache over the same directory (the next boot) finds every entry; another stack's directory and a dead writer's
//    temporary are gone once it opens; four processes storing the same entries while this one reads never show a torn entry;
//  - a capped cache stays under its cap and keeps the latest entry;
//  - eight threads asking for one state make it once; a refusal sticks; a probe that failed is tried again by later asks,
//    Compiler::kRetries times; the next process's table takes the state (and the refusal) from the disk, and finds no entry
//    for the probe that failed.
#pragma once
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../runtime/raw_compiler.h"

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
        return x.id == y.id && x.image == y.image && x.data == y.data && x.cbufTypes == y.cbufTypes &&
               x.gprs == y.gprs && x.boundTextures == y.boundTextures;
    };
    bool variants = a.variants.size() == b.variants.size();
    for (size_t i = 0; variants && i < a.variants.size(); i++)
        variants =
            a.variants[i].assume == b.variants[i].assume && program(a.variants[i].program, b.variants[i].program);
    const bool split = a.split == b.split && a.splitTried == b.splitTried &&
                       (!a.split || (!memcmp(&a.splitRecord, &b.splitRecord, sizeof a.splitRecord) &&
                                     program(a.splitPrograms[0], b.splitPrograms[0]) &&
                                     program(a.splitPrograms[1], b.splitPrograms[1])));
    const bool twins = !a.strideTwin == !b.strideTwin && (!a.strideTwin || same(*a.strideTwin, *b.strideTwin));
    return !memcmp(&a.pipeline, &b.pipeline, sizeof a.pipeline) && a.keyString == b.keyString &&
           program(a.programs[0], b.programs[0]) && program(a.programs[1], b.programs[1]) &&
           program(a.tessPrograms[0], b.tessPrograms[0]) && program(a.tessPrograms[1], b.tessPrograms[1]) &&
           a.blob == b.blob && a.extras == b.extras && a.restore == b.restore && a.samplers == b.samplers &&
           a.spill == b.spill && split && variants && twins;
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
        const auto readAll = [&] {
            for (const auto &[key, rec] : records) {
                raw::RuntimeRecord back;
                std::string why;
                const raw::CacheKey k = keyOf(stack, key, 7);
                const auto found = cache.load(k, back, why);
                seen += found == raw::RecordCache::Found::Record;
                torn += found == raw::RecordCache::Found::Record && !same(rec, back);
            }
        };
        // the reader reads for as long as any writer runs, and once after: a fixed number of rounds can end before the
        // writer processes are scheduled at all
        std::vector<int> statuses(writers.size(), -1);
        size_t running = writers.size();
        for (int round = 0; getpid() == me && (running || round < 200); round++) {
            readAll();
            for (size_t w = 0; w < writers.size(); w++) {
                int status = 0;
                if (statuses[w] < 0 && waitpid(writers[w], &status, WNOHANG) == writers[w]) {
                    statuses[w] = status;
                    running--;
                }
            }
        }
        readAll();
        for (int status : statuses)
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a writer process exits");
        check(seen > 0, "the reader sees the writers' entries while four processes write");
        check(torn == 0, "no reader sees a torn entry while four processes write");
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
        uint64_t largest = 0;
        for (const auto &[key, rec] : records)
            largest = std::max<uint64_t>(largest, raw::encodeEntry(keyOf(stack, key, 7), &rec, "").size());
        const uint64_t cap = largest * 3;
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
            raw::Compiler compiler(&cache, make, 0);
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
            check(makes == 3 && dones == 8, "eight threads asking one state make it once and each hears of it");
            check(compiler.ask(keyOf(stack, refusedKey, 7), nullptr) == raw::Compiler::State::Refused,
                  "a refusal sticks");
            uint64_t wait = 1;
            check(compiler.retryAfter(keyOf(stack, "failed", 7), wait) && wait == 0,
                  "a failed probe may be tried again");
            check(!compiler.retryAfter(keyOf(stack, refusedKey, 7), wait), "a refused state is not tried again");
            for (uint32_t retry = 0; retry <= raw::Compiler::kRetries; retry++) {
                compiler.ask(keyOf(stack, "failed", 7), nullptr);
                compiler.drain();
            }
            check(compiler.ask(keyOf(stack, "failed", 7), nullptr) == raw::Compiler::State::Failed &&
                      makes == 3 + raw::Compiler::kRetries && !compiler.retryAfter(keyOf(stack, "failed", 7), wait),
                  "a failed probe is tried again, kRetries times");
            const raw::Compiler::Stats s = compiler.stats();
            check(s.hits == 0 && s.refusals == 1 && s.failures == 1 + raw::Compiler::kRetries,
                  "first process: every make a miss");
        }
        {
            raw::RecordCache again;
            again.open(tableDir, stack, 0);
            raw::Compiler compiler(&again, make, 0);
            compiler.ask(k, nullptr);
            compiler.ask(keyOf(stack, refusedKey, 7), nullptr);
            compiler.ask(keyOf(stack, "failed", 7), nullptr);
            compiler.drain();
            const raw::Compiler::Stats s = compiler.stats();
            check(makes == 4 + raw::Compiler::kRetries && s.hits == 2 && s.misses == 1,
                  "the next process takes the record and refusal from disk");
            check(compiler.ask(keyOf(stack, refusedKey, 7), nullptr) == raw::Compiler::State::Refused,
                  "a refusal from disk stays refused");
        }
        system(("rm -rf '" + tableDir + "'").c_str());
    }
    system(("rm -rf '" + root + "'").c_str());
}

}  // namespace cachechecks
