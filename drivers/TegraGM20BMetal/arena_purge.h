// SPDX-License-Identifier: MIT
// When an MTLHeap's arena gives its pages back (arena.cpp), decided at the purge itself under the device's lock:
// a heap made NonVolatile after a purge pass began is never purged by it. setPurgeableState reads whether the pages went
// and sets the new volatility in the same step, so a NonVolatile that answers "not purged" keeps the contents it promised.
#pragma once

namespace gm20b {

// Whether `a` gives its pages back now: in use, not purged, no m2v view leasing it, the device idle and nothing
// committed, and volatile (or the app asked Empty of it)
template <class Arena> bool arenaPurgeable(const Arena &a, bool idle, bool committed, bool askedEmpty)
{
    return a.used && !a.purged && !a.leases && idle && !committed && (askedEmpty || a.isVolatile);
}

// The idle purge of every volatile arena, each judged as it is purged
template <class Arenas, class Purge> void purgeIdleArenas(Arenas &arenas, bool idle, bool committed, Purge &&purge)
{
    for (auto &a : arenas)
        if (arenaPurgeable(a, idle, committed, false))
            purge(a);
}

// setPurgeableState: whether the pages had gone (the answer is then Empty), and the arena volatile unless `keep`
template <class Arena> bool arenaSetVolatile(Arena &a, bool keep)
{
    a.isVolatile = !keep;
    return a.purged;
}

// A purged arena bound whole again by `bind`, under the device's lock, before a command buffer reaches it or anything is
// placed in it: a placement's storage is cut from the arena's bound pages as they are then. False: no memory for it, it
// stays purged.
template <class Arena, class Count, class Bind> bool arenaRestore(Arena &a, Count &purgedArenas, Bind &&bind)
{
    if (!a.purged)
        return true;
    if (!bind(a))
        return false;
    a.purged = false;
    purgedArenas--;
    return true;
}

}  // namespace gm20b
