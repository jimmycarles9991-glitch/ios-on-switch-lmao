// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalStatesKey.h"
#include "TegraGM20BMetalComplete.h"
#include "lock.h"

#include <mutex>
#include <unordered_set>

bool gm20bFirstNaming(uint64_t keyHash, uint32_t line)
{
    static gm20b::UnfairMutex lock;
    static std::unordered_set<uint64_t> named;
    std::lock_guard<gm20b::UnfairMutex> l(lock);
    return named.insert(keyHash * 31 + line).second;
}

void gm20bNoteMiss(const char *kind, uint64_t keyHash, const std::string &key, NSString *label)
{
#if SWITCH_METAL_STATS
    if (!gm20bFirstNaming(keyHash, 0))
        return;
    os_log(gm20bLog(), "SwitchMetal: %{public}s: a %{public}s state the pack lacks %016llx (%{public}s)", getprogname(),
           kind, (unsigned long long)keyHash, label.UTF8String ?: "-");
    gm20b::logHarvestParts(keyHash, label, key);
#else
    static_cast<void>(kind);
    static_cast<void>(keyHash);
    static_cast<void>(key);
    static_cast<void>(label);
#endif
}
