// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

// The IOSurfaceAccelerator line for the unified log, one per 5 s window with work. Requests are submitted on the
// engine thread and finish on VIC's retire thread, so every count is atomic. ops: VIC operations submitted; bounced:
// requests whose surfaces went through the copy engine's scratch; engine: submission to completion per finished
// request; cpu: the engine thread's submission work per submitted request; mapped: bytes of the surfaces VIC
// addressed per submitted request.
struct SwitchVICStats {
    static constexpr uint64_t kWindowUs = 5000000;
    uint64_t windowStart, jobs, ops, refused, pitch, failed, submitted, bounced, engineUs, cpuUs, bytes;
    uint64_t presents, presentWaits;  // decoded pictures VIC placed, and those a CPU thread waited for
    uint64_t maps;                    // SMMU mappings made: a cached surface costs none (LIMITS V11)
    bool failureLogged;

    void present(bool waited)
    {
        add(presents, 1);
        add(presentWaits, waited ? 1 : 0);
    }

    void add(uint64_t &counter, uint64_t amount)
    {
        __atomic_add_fetch(&counter, amount, __ATOMIC_RELAXED);
    }

    void submission(uint32_t operations, bool bounce, uint64_t cpu, uint64_t addressed)
    {
        add(submitted, 1);
        add(ops, operations);
        add(bounced, bounce ? 1 : 0);
        add(cpuUs, cpu);
        add(bytes, addressed);
    }

    void finished(int status, uint64_t elapsedUs)
    {
        add(jobs, 1);
        add(failed, status ? 1 : 0);
        add(engineUs, elapsedUs);
    }

    // The first job a boot fails after its call accepted it, which otherwise shows only as failed=: the stage that
    // failed, its status, the surfaces' formats (none for a fill's source) and the caller's pid. A job cancelled with
    // its caller (-125) is not a failure.
    bool firstFailure(const char *stage, int status, uint32_t source, uint32_t destination, int pid, char *out,
                      size_t size)
    {
        if (!status || status == -125 || __atomic_exchange_n(&failureLogged, true, __ATOMIC_RELAXED))
            return false;
        char from[5], to[5];
        snprintf(out, size, "TegraVIC: first failed job this boot: %s, status %d, '%s' to '%s', pid %d", stage, status,
                 fourcc(source, from), fourcc(destination, to), pid);
        return true;
    }

    // The line once the window has run its 5 s, written by one caller only; false otherwise.
    bool line(uint64_t now, char *out, size_t size)
    {
        uint64_t start = __atomic_load_n(&windowStart, __ATOMIC_ACQUIRE);
        if (!start || now - start < kWindowUs) {
            if (!start)
                __atomic_compare_exchange_n(&windowStart, &start, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            return false;
        }
        if (!__atomic_compare_exchange_n(&windowStart, &start, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return false;
        const uint64_t j = take(jobs), s = take(submitted);
        const uint64_t o = take(ops), r = take(refused), p = take(pitch), f = take(failed), b = take(bounced);
        const uint64_t e = take(engineUs), c = take(cpuUs), m = take(bytes);
        const uint64_t pr = take(presents), pw = take(presentWaits), mp = take(maps);
        snprintf(out, size,
                 "TegraVIC: %llus jobs=%llu ops=%llu refused=%llu pitch=%llu failed=%llu engine=%lluus/job "
                 "cpu=%lluus/job mapped=%lluKB/job bounced=%llu presents=%llu present_waits=%llu maps=%llu",
                 ull((now - start) / 1000000), ull(j), ull(o), ull(r), ull(p), ull(f), ull(j ? e / j : 0),
                 ull(s ? c / s : 0), ull(s ? (m >> 10) / s : 0), ull(b), ull(pr), ull(pw), ull(mp));
        return true;
    }

private:
    static const char *fourcc(uint32_t code, char out[5])
    {
        if (!code)
            return "none";
        for (unsigned i = 0; i < 4; i++) {
            const char c = static_cast<char>(code >> (24 - 8 * i));
            out[i] = c >= ' ' && c <= '~' ? c : '?';
        }
        out[4] = 0;
        return out;
    }

    static uint64_t take(uint64_t &counter)
    {
        return __atomic_exchange_n(&counter, uint64_t(0), __ATOMIC_RELAXED);
    }

    static unsigned long long ull(uint64_t value)
    {
        return static_cast<unsigned long long>(value);
    }
};
