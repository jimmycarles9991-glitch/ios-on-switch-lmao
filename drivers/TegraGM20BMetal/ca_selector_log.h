// SPDX-License-Identifier: MIT
// DEVELOPMENT: the selector line's process-wide log (ca_selectors.h): the build's layout, which the facade sets at open
// (TegraGM20BMetal.mm, from ca_selector_layout.h), every encoder's generic merge state draws, each window logged as it closes
// and each config the first time the process draws it. A window closes at its end by a one-shot dispatch: its state may
// draw nothing more for minutes, so no later draw would close it.
#pragma once
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <time.h>
#include <cstdio>
#include <mutex>
#include "ca_selectors.h"

namespace gm20b {

// Set once by the facade's open, before any draw
inline CaSelectorLayout gCaSelectorLayout;

// Draws on every encoder's thread and the flushes on a dispatch queue share the log
inline std::mutex gCaSelectorsLock;
inline CaSelectorLog gCaSelectors;

inline uint64_t caSelectorUptimeUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

inline void logCaSelectorEvents(const CaSelectorLog::Events &events, size_t length)
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    for (const CaSelectorLog::Fresh &f : events.fresh) {
        char hex[2 * kCaSelectorMax + 1] = {};
        for (size_t i = 0; i < length; i++)
            snprintf(hex + 2 * i, 3, "%02x", f.selector.bound[i]);
        os_log(log, "SwitchMetal: ca selector key=%016llx config %zu %016llx: %s", (unsigned long long)f.keyHash,
               f.ordinal, (unsigned long long)f.selector.hash, hex);
    }
    for (const CaSelectorLog::Window &w : events.closed)
        os_log(log,
               "SwitchMetal: ca selectors key=%016llx in 5 s: %llu draws, %zu configs (%zu new; the most %llu draws, "
               "config "
               "%016llx), %llu draws of configs past the process's first %zu, %llu draws of a config the CPU does not "
               "map; the process: %zu configs, %llu draws",
               (unsigned long long)w.keyHash, (unsigned long long)w.draws, w.configs, w.fresh,
               (unsigned long long)w.most, (unsigned long long)w.mostConfig, (unsigned long long)w.beyond,
               CaSelectorLog::kConfigsPerState, (unsigned long long)w.unmapped, w.processConfigs,
               (unsigned long long)w.processDraws);
}

inline void flushCaSelectors(void *length)
{
    CaSelectorLog::Events events;
    {
        std::lock_guard<std::mutex> l(gCaSelectorsLock);
        gCaSelectors.flush(caSelectorUptimeUs(), events);
    }
    logCaSelectorEvents(events, reinterpret_cast<uintptr_t>(length));
}

// A draw of the generic merge state `keyHash` whose config's slot holds the `size` bytes at `cpu` (null: unmapped)
inline void logCaSelector(uint64_t keyHash, const uint8_t *cpu, size_t size)
{
    const CaSelectorLayout &layout = gCaSelectorLayout;
    if (!layout.length)
        return;
    CaSelector selector;
    const bool mapped = caSelector(layout, cpu, size, selector);
    CaSelectorLog::Events events;
    bool opened = false;
    {
        std::lock_guard<std::mutex> l(gCaSelectorsLock);
        opened = gCaSelectors.note(keyHash, mapped ? &selector : nullptr, caSelectorUptimeUs(), events);
    }
    logCaSelectorEvents(events, layout.length);
    if (opened)
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, int64_t(CaSelectorLog::kWindowUs + 1000) * 1000),
                         dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                         reinterpret_cast<void *>(uintptr_t(layout.length)), flushCaSelectors);
}

}  // namespace gm20b
