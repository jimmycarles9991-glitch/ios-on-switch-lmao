// SPDX-License-Identifier: BSD-2-Clause-Patent
// A CPU's busy time as its context-switch callout keeps it (SwitchPerfGovernor.cpp) and another CPU's tick reads it.
// Each slot has one writer, its CPU's callout, and no fence: `mark` is when the CPU last switched, with bit 0 set while
// it runs something other than its idle thread, so a reader never pairs one switch's state with another's time. A
// reader racing a switch may still pair `busy_total` from one side of it with `mark` from the other; its sum is then off
// by that switch's interval, in either direction, and the next read is right again. The governor uses only differences
// of these sums, one per 8 ms window, so one window is off by at most that interval and the next corrects it.
// test/switch_cpu_load_test.cpp checks that bound with the reader racing the writer.
#ifndef SWITCH_CPU_LOAD_H
#define SWITCH_CPU_LOAD_H

#include <stdint.h>

struct switch_cpu_load {
    uint64_t mark;        // the last switch's time (even), | 1 while busy
    uint64_t busy_total;  // busy time before the last switch
} __attribute__((aligned(64)));

// The CPU switched at `timestamp` to its idle thread (busy 0) or to anything else (busy 1). Its own callout only.
static inline void switch_cpu_load_switch(struct switch_cpu_load *load, uint64_t timestamp, int busy)
{
    const uint64_t mark = __atomic_load_n(&load->mark, __ATOMIC_RELAXED);
    const uint64_t since = mark & ~(uint64_t)1;
    const uint64_t at = timestamp > since ? timestamp & ~(uint64_t)1 : since;
    if (mark & 1u) {
        __atomic_store_n(&load->busy_total, __atomic_load_n(&load->busy_total, __ATOMIC_RELAXED) + (at - since),
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&load->mark, at | (busy ? 1u : 0u), __ATOMIC_RELAXED);
}

// The CPU's busy time up to `now`, from any CPU.
static inline uint64_t switch_cpu_load_busy(const struct switch_cpu_load *load, uint64_t now)
{
    const uint64_t total = __atomic_load_n(&load->busy_total, __ATOMIC_RELAXED);
    const uint64_t mark = __atomic_load_n(&load->mark, __ATOMIC_RELAXED);
    const uint64_t since = mark & ~(uint64_t)1;
    return total + ((mark & 1u) && now > since ? now - since : 0);
}

#endif
