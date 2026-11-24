// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: the SD driver's request accounting (NativeSwitchBlock.cpp) behind its 10 s "SwitchSD io:" line and
// kern.switch_sd. Per I/O class, the time a request waits in the queue (submit to the pick of the command that carries
// it) and its whole time (submit to its completion); per direction and size, a card command's time; the queue's depth
// over time; requests whose class the driver chose instead of the client; bytes the CPU copied. Free of kernel types,
// so test/sd_io_test.cpp runs this code.
//
// Every counter is a relaxed atomic: the worker and the submitters (under queueLock) write them, and kern.switch_sd
// reads them from any thread without the lock. Each counter is whole; a snapshot is not one instant across counters
// (a histogram's count may run one request ahead of its sum).
#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

constexpr unsigned kSwitchSdClasses = 4;  // IOStoragePriority >> 6: high (I/O tier 0), default, low, background
constexpr unsigned kSwitchSdSizes = 4;    // a command's bytes: <= 4 KiB, <= 32 KiB, <= 256 KiB, more
constexpr unsigned kSwitchSdRtPhases = 5;
constexpr unsigned kSwitchSdBuckets = 32;
constexpr uint64_t kSwitchSdFirstNs = 64000;     // bucket 0 holds everything below 64 us
constexpr uint64_t kSwitchSdFineFirstNs = 1000;  // round-trip split: bucket 0 holds everything below 1 us

enum SwitchSdRtPhase : unsigned {
    kSwitchSdRtSubmit = 0,  // idle submit() -> COMMAND write
    kSwitchSdRtPrep = 1,    // beginBatch() -> COMMAND write
    kSwitchSdRtWire = 2,    // COMMAND write -> transfer-complete primary IRQ
    kSwitchSdRtWake = 3,    // transfer-complete primary IRQ -> worker thread_block() return
    kSwitchSdRtGap = 4,     // transfer-complete primary IRQ -> next back-to-back COMMAND write
};

// Half-octave buckets: 0 below firstNs, then two per doubling up to the last, which holds everything from
// firstNs << 15 on. A bucket's lower bound (bucket >= 1):
inline uint64_t switchSdBucketLow(unsigned bucket, uint64_t firstNs = kSwitchSdFirstNs)
{
    const uint64_t octave = firstNs << ((bucket - 1) / 2);
    return (bucket - 1) % 2 ? octave * 362 / 256 : octave;  // x sqrt(2), to 0.02%
}

inline unsigned switchSdBucket(uint64_t ns, uint64_t firstNs = kSwitchSdFirstNs)
{
    unsigned bucket = 0;
    while (bucket + 1 < kSwitchSdBuckets && ns >= switchSdBucketLow(bucket + 1, firstNs))
        ++bucket;
    return bucket;
}

// What a percentile reports for a bucket: its upper bound (the open last bucket: its lower one).
inline uint64_t switchSdBucketTop(unsigned bucket, uint64_t firstNs = kSwitchSdFirstNs)
{
    return switchSdBucketLow(bucket + 1 < kSwitchSdBuckets ? bucket + 1 : bucket, firstNs);
}

inline void switchSdAdd(uint64_t *counter, uint64_t value)
{
    __atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

inline uint64_t switchSdLoad(const uint64_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

template <uint64_t FirstNs> struct SwitchSdBucketsHistogram {
    uint64_t n[kSwitchSdBuckets];
    uint64_t count, sumNs;

    void add(uint64_t ns)
    {
        switchSdAdd(&n[switchSdBucket(ns, FirstNs)], 1);
        switchSdAdd(&count, 1);
        switchSdAdd(&sumNs, ns);
    }

    // What was added since `before` (a copy of this one taken earlier).
    SwitchSdBucketsHistogram since(const SwitchSdBucketsHistogram &before) const
    {
        SwitchSdBucketsHistogram d = {};
        for (unsigned i = 0; i < kSwitchSdBuckets; ++i)
            d.n[i] = switchSdLoad(&n[i]) - before.n[i];
        d.count = switchSdLoad(&count) - before.count;
        d.sumNs = switchSdLoad(&sumNs) - before.sumNs;
        return d;
    }

    // The upper bound of the bucket that holds the `permille`-th of the values; 0 when empty.
    uint64_t percentileNs(unsigned permille) const
    {
        if (!count)
            return 0;
        const uint64_t rank = (count * permille + 999) / 1000;
        uint64_t seen = 0;
        for (unsigned i = 0; i < kSwitchSdBuckets; ++i) {
            seen += n[i];
            if (seen >= rank)
                return switchSdBucketTop(i, FirstNs);
        }
        return switchSdBucketTop(kSwitchSdBuckets - 1, FirstNs);
    }
};

using SwitchSdHistogram = SwitchSdBucketsHistogram<kSwitchSdFirstNs>;
using SwitchSdFineHistogram = SwitchSdBucketsHistogram<kSwitchSdFineFirstNs>;

inline unsigned switchSdSize(uint64_t bytes)
{
    if (bytes <= 4096)
        return 0;
    if (bytes <= 32768)
        return 1;
    return bytes <= 262144 ? 2 : 3;
}

struct SwitchSdIo {
    SwitchSdHistogram wait[kSwitchSdClasses], total[kSwitchSdClasses];
    SwitchSdHistogram command[2][kSwitchSdSizes];  // [write][size]
    SwitchSdFineHistogram roundTrip[kSwitchSdRtPhases];
    uint64_t asked[kSwitchSdClasses];  // requests by the class their client gave
    uint64_t chosen;                   // of them, queued in another class by the driver (the boot prefetch)
    uint64_t depthNs;                  // the queue's length integrated over time (requests x ns)
    uint64_t depthAtNs;                // when it last changed (under queueLock)
    uint64_t depthMax;                 // the longest since the last line
    uint64_t bounceBytes;              // copied by the CPU between the bounce buffer and requests

    // Every counter into `copy`, each read whole (the line's `before`; the driver's copy is a member, not on a stack).
    void copyTo(SwitchSdIo &copy) const
    {
        for (unsigned c = 0; c < kSwitchSdClasses; ++c) {
            copy.wait[c] = wait[c].since({});
            copy.total[c] = total[c].since({});
            copy.asked[c] = switchSdLoad(&asked[c]);
        }
        for (unsigned z = 0; z < kSwitchSdSizes; ++z) {
            copy.command[0][z] = command[0][z].since({});
            copy.command[1][z] = command[1][z].since({});
        }
        for (unsigned p = 0; p < kSwitchSdRtPhases; ++p)
            copy.roundTrip[p] = roundTrip[p].since({});
        copy.chosen = switchSdLoad(&chosen);
        copy.depthNs = switchSdLoad(&depthNs);
        copy.depthAtNs = switchSdLoad(&depthAtNs);
        copy.depthMax = switchSdLoad(&depthMax);
        copy.bounceBytes = switchSdLoad(&bounceBytes);
    }

    // Under queueLock, before the queue's length changes from `queued`. A submitter's clock read before it took the
    // lock may be older than the last change: that change's time stands.
    void depthChanging(uint64_t now, uint32_t queued)
    {
        const uint64_t at = switchSdLoad(&depthAtNs);
        if (now > at) {
            if (at)
                switchSdAdd(&depthNs, static_cast<uint64_t>(queued) * (now - at));
            __atomic_store_n(&depthAtNs, now, __ATOMIC_RELAXED);
        }
        if (queued > switchSdLoad(&depthMax))
            __atomic_store_n(&depthMax, queued, __ATOMIC_RELAXED);
    }
};

// The line's percentiles of one histogram: "n p50/p90/p99" in ms with one decimal ("-" when empty).
inline size_t switchSdFormatHistogram(const SwitchSdHistogram &h, char *out, size_t size)
{
    if (!h.count)
        return static_cast<size_t>(snprintf(out, size, " -"));
    const uint64_t p[3] = {h.percentileNs(500), h.percentileNs(900), h.percentileNs(990)};
    return static_cast<size_t>(
        snprintf(out, size, " %llu %llu.%llu/%llu.%llu/%llu.%llu", static_cast<unsigned long long>(h.count),
                 static_cast<unsigned long long>(p[0] / 1000000), static_cast<unsigned long long>(p[0] / 100000 % 10),
                 static_cast<unsigned long long>(p[1] / 1000000), static_cast<unsigned long long>(p[1] / 100000 % 10),
                 static_cast<unsigned long long>(p[2] / 1000000), static_cast<unsigned long long>(p[2] / 100000 % 10)));
}

inline size_t switchSdFormatHistogramUs(const SwitchSdFineHistogram &h, char *out, size_t size)
{
    if (!h.count)
        return static_cast<size_t>(snprintf(out, size, " -"));
    const uint64_t p[3] = {h.percentileNs(500) / 1000, h.percentileNs(900) / 1000, h.percentileNs(990) / 1000};
    return static_cast<size_t>(snprintf(out, size, " %llu/%llu/%llu", static_cast<unsigned long long>(p[0]),
                                        static_cast<unsigned long long>(p[1]), static_cast<unsigned long long>(p[2])));
}

// The 10 s line: what happened between `before` and `now` over `spanNs`. Per class (hi def lo bg) the queue wait and
// the whole request (count p50/p90/p99 ms), per direction and size (<=4K <=32K <=256K >256K) a command's time, the
// ADMA2 round-trip split (sub prep wire wake gap, p50/p90/p99 us), the mean and longest queue, requests the driver
// queued in another class, and bounce bytes.
inline size_t switchSdFormatIo(const SwitchSdIo &now, const SwitchSdIo &before, uint64_t spanNs, char *out, size_t size)
{
    static const char *const kClassNames[kSwitchSdClasses] = {"hi", "def", "lo", "bg"};
    size_t at = static_cast<size_t>(snprintf(out, size, "SwitchSD io:"));
    const auto room = [&] {
        return at < size ? size - at : 0;
    };
    for (unsigned c = 0; c < kSwitchSdClasses; ++c) {
        const SwitchSdHistogram wait = now.wait[c].since(before.wait[c]);
        const SwitchSdHistogram total = now.total[c].since(before.total[c]);
        if (!wait.count && !total.count)
            continue;
        at += static_cast<size_t>(snprintf(out + at, room(), " | %s wait", kClassNames[c]));
        at += switchSdFormatHistogram(wait, out + at, room());
        at += static_cast<size_t>(snprintf(out + at, room(), " all"));
        at += switchSdFormatHistogram(total, out + at, room());
    }
    for (unsigned w = 0; w < 2; ++w) {
        static const char *const kRows[2] = {"cmd rd", "cmd wr"};
        at += static_cast<size_t>(snprintf(out + at, room(), " | %s", kRows[w]));
        for (unsigned s = 0; s < kSwitchSdSizes; ++s)
            at += switchSdFormatHistogram(now.command[w][s].since(before.command[w][s]), out + at, room());
    }
    static const char *const kRtNames[kSwitchSdRtPhases] = {"sub", "prep", "wire", "wake", "gap"};
    bool anyRt = false;
    for (unsigned p = 0; p < kSwitchSdRtPhases; ++p)
        anyRt |= switchSdLoad(&now.roundTrip[p].count) != before.roundTrip[p].count;
    if (anyRt) {
        at += static_cast<size_t>(snprintf(out + at, room(), " | rt us"));
        for (unsigned p = 0; p < kSwitchSdRtPhases; ++p) {
            const SwitchSdFineHistogram rt = now.roundTrip[p].since(before.roundTrip[p]);
            at += static_cast<size_t>(snprintf(out + at, room(), " %s", kRtNames[p]));
            at += switchSdFormatHistogramUs(rt, out + at, room());
        }
    }
    const uint64_t depth = spanNs ? (switchSdLoad(&now.depthNs) - before.depthNs) * 10 / spanNs : 0;
    const uint64_t chosen = switchSdLoad(&now.chosen) - before.chosen;
    at += static_cast<size_t>(
        snprintf(out + at, room(), " | queue %llu.%llu max %llu | chosen %llu | bounce %lluK",
                 static_cast<unsigned long long>(depth / 10), static_cast<unsigned long long>(depth % 10),
                 static_cast<unsigned long long>(switchSdLoad(&now.depthMax)), static_cast<unsigned long long>(chosen),
                 static_cast<unsigned long long>((switchSdLoad(&now.bounceBytes) - before.bounceBytes) >> 10)));
    return at < size ? at : size - 1;
}

// kern.switch_sd's counters since boot, as "key=value" tokens; a histogram is "count,sum_us,b0,...,b31". A reader
// takes two and subtracts.
inline size_t switchSdFormatTotals(const SwitchSdIo &io, char *out, size_t size)
{
    size_t at = 0;
    const auto room = [&] {
        return at < size ? size - at : 0;
    };
    const auto histogram = [&](const char *name, unsigned index, const auto &h) {
        at += static_cast<size_t>(snprintf(out + at, room(), "%s%s%u=%llu,%llu", at ? " " : "", name, index,
                                           static_cast<unsigned long long>(switchSdLoad(&h.count)),
                                           static_cast<unsigned long long>(switchSdLoad(&h.sumNs) / 1000)));
        for (unsigned i = 0; i < kSwitchSdBuckets; ++i) {
            at += static_cast<size_t>(
                snprintf(out + at, room(), ",%llu", static_cast<unsigned long long>(switchSdLoad(&h.n[i]))));
        }
    };
    for (unsigned c = 0; c < kSwitchSdClasses; ++c) {
        histogram("wait", c, io.wait[c]);
        histogram("all", c, io.total[c]);
    }
    for (unsigned s = 0; s < kSwitchSdSizes; ++s) {
        histogram("rd", s, io.command[0][s]);
        histogram("wr", s, io.command[1][s]);
    }
    for (unsigned p = 0; p < kSwitchSdRtPhases; ++p)
        histogram("rt", p, io.roundTrip[p]);
    for (unsigned c = 0; c < kSwitchSdClasses; ++c) {
        at += static_cast<size_t>(snprintf(out + at, room(), " asked%u=%llu", c,
                                           static_cast<unsigned long long>(switchSdLoad(&io.asked[c]))));
    }
    at += static_cast<size_t>(snprintf(out + at, room(), " chosen=%llu depth_us=%llu bounce=%llu",
                                       static_cast<unsigned long long>(switchSdLoad(&io.chosen)),
                                       static_cast<unsigned long long>(switchSdLoad(&io.depthNs) / 1000),
                                       static_cast<unsigned long long>(switchSdLoad(&io.bounceBytes))));
    return at < size ? at : size - 1;
}
