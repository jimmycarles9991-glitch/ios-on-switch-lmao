// SPDX-License-Identifier: MIT
// When a client's pool grows ahead of need (memory.cpp Device::growAheadRoom). A chunk made ahead is footprint the
// process holds before it draws into it, and memorystatus kills a process past its limit, so a client grows ahead only
// while it stays below the share of that limit at which memorystatus warns it (xnu MEMSTAT_INACTIVE_WARNING_LEVEL): a
// daemon near its 15 MB limit never grows itself into a kill.
#pragma once
#include <cstdint>

namespace gm20b {

constexpr uint64_t kGrowAheadLimitPercent = 80;
// A client's grower starts at its second stall: its first chunk always stalls, and a process that never needs a second
// keeps no chunk ahead
constexpr uint32_t kClientGrowerStall = 2;

// Whether a `chunk` made now keeps a process with `footprint` bytes, `available` from its limit (task_info TASK_VM_INFO
// limit_bytes_remaining; 0 when it is at its limit or reports none), below that share of its limit
constexpr bool clientGrowAhead(uint64_t footprint, uint64_t available, uint64_t chunk)
{
    return available != 0 && footprint + chunk < (footprint + available) / 100 * kGrowAheadLimitPercent;
}

}  // namespace gm20b
