// SPDX-License-Identifier: MIT
// Why a fetching state's draw takes its storage form (the pixel interlock) or its barrier form (a barrier a triangle,
// where its record has no storage form) rather than a cheaper route: its fetch-free form (fetch_free.h, rules
// fetch.blend and fetch.iterated) or, from the storage form, its barrier twin (twin_rule.h, rule fetch.twin). A debug
// build says so once per state (noteStorageRoute), so a state that never reaches a cheaper route names what stops it.
#pragma once
#include <atomic>
#include <cstdint>
#include "stats.h"
#if SWITCH_METAL_STATS
#include <os/log.h>
#endif
#include "fetch_free.h"
#include "storage_form.h"

namespace gm20b {

enum class FreeMiss : uint8_t {
    None,
    NoForm,    // no fetch-free form: the pack links one only to its own records, from the Core Animation and RenderBox
               // taggers (pack/ca/fetchfree.py, pack/rb/rbfree.py); a state compiled on the device has none
    NoTarget,  // the blended colour is not one of the pass's
    Rule,      // the form's rule refused this draw (`draw`)
};

enum class TwinMiss : uint8_t {
    None,
    NoTwin,  // the state has no barrier twin
    Probe,   // a debug build's probe forced the storage form
    Shape,   // rule fetch.twin: the draw's triangles or the pass's area are none it measured cheaper
};

struct StorageRoute {
    uint64_t key = 0;
    FreeMiss free = FreeMiss::None;
    uint8_t rung = FetchNone;  // the fetch-free form's
    FetchFreeDraw draw;        // what rule fetch.blend read, where `free` is Rule
    TwinMiss twin = TwinMiss::None;
    bool barrier = false;  // drawn by its barrier form, a triangle at a time
    uint32_t triangles = 0, areaPixels = 0;
};

constexpr const char *freeMissName(FreeMiss m)
{
    switch (m) {
    case FreeMiss::None:
        return "-";
    case FreeMiss::NoForm:
        return "no fetch-free form (the pack's are Core Animation's and RenderBox's records only)";
    case FreeMiss::NoTarget:
        return "its colour is not the pass's";
    case FreeMiss::Rule:
        return "its rule refused the draw";
    }
    return "?";
}

constexpr const char *twinMissName(TwinMiss m)
{
    switch (m) {
    case TwinMiss::None:
        return "-";
    case TwinMiss::NoTwin:
        return "no barrier twin";
    case TwinMiss::Probe:
        return "a probe forced the storage form";
    case TwinMiss::Shape:
        return "rule fetch.twin: not a shape measured cheaper";
    }
    return "?";
}

#if SWITCH_METAL_STATS
// Logs `r` the first time its state is drawn by its storage or barrier form in this process. The states logged are a
// fixed open-addressed table of keys, claimed by compare-and-swap, so encoding threads need no lock; past 1024 states
// nothing more is logged.
inline void noteStorageRoute(const StorageRoute &r)
{
    constexpr uint32_t kSlots = 1024;
    static std::atomic<uint64_t> logged[kSlots];
    if (!r.key)
        return;
    bool claimed = false;
    for (uint32_t probe = 0; probe < kSlots && !claimed; probe++) {
        uint64_t seen = 0;
        claimed =
            logged[(r.key + probe) & (kSlots - 1)].compare_exchange_strong(seen, r.key, std::memory_order_relaxed);
        if (seen == r.key)
            return;
    }
    if (!claimed)
        return;
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    os_log(log,
           "SwitchMetal: fetch route k%016llx takes its %{public}s form: fetch-free %{public}s (rung %u, format %u, "
           "readable %d, rop layers %u, 8-bit sources %d, accumulates %d); barrier twin %{public}s (%u triangles over "
           "%u px)",
           (unsigned long long)r.key, r.barrier ? "barrier" : "storage", freeMissName(r.free), r.rung, r.draw.format,
           r.draw.readable, r.draw.ropLayers, r.draw.eightBitSources, r.draw.accumulates,
           r.barrier ? "- (no storage form in the record)" : twinMissName(r.twin), r.triangles, r.areaPixels);
}
#endif

}  // namespace gm20b
