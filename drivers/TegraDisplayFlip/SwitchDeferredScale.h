// SPDX-License-Identifier: BSD-2-Clause-Patent
// Deferred IOSurfaceAccelerator jobs: Core Animation sends a quarter-turned video layer through the scaler-rotator (TegraVIC),
// never to an internal display rotated (QuartzCore 21H461 AppleInternalDisplay::display_supports_rotation_p reads
// IOMobileFramebuffer's SupportsRotation only for display type 2, and the panel is type 0). When DC A's window A can scan the
// job's source S itself (column scan of planar YUV), the copy into the destination D is skipped: TegraVIC records
// D = rotate+scale(S) and completes the job, and the display, handed D in a swap, scans S. D's bytes are only made real
// (the recorded copy) before anything else reads them. Plain logic, no IOKit: TegraVIC and the display drive it under one
// lock and carry out what it returns; test/switch_deferred_scale_test.cpp holds it to its invariants.
//
// Invariants:
//  - Any access to D other than the display's scan copies first, so it reads the bytes the copy would have produced.
//  - S stays held (its IOSurface use count raised, so a decoder pool does not write it) while a deferred D may still be
//    scanned or copied from it: until D is copied and no scan of S remains, or the record goes.
//  - Every hold is dropped exactly once.
#pragma once
#include <stdint.h>

namespace SwitchDeferredScale {

constexpr unsigned kRecords = 8;

struct Job {
    uint64_t dest = 0, source = 0;  // first physical pages: the keys TegraVIC and the display both have
    int32_t srcX = 0, srcY = 0, srcW = 0, srcH = 0, dstX = 0, dstY = 0, dstW = 0, dstH = 0;
    uint32_t transform = 0;
};

enum class Access {
    CpuLock,         // IOSurfaceLock or a CPU mapping of D
    GpuImport,       // a GPU client imports D
    ScalerSource,    // D is another scaler job's source
    EngineRead,      // an encoder or JPEG engine reads D
    NewDestination,  // D is the destination of a new scaler job: its old content is never read
    Freed,           // D is destroyed
};

// What the caller does, in this order: run `job` (synchronously, before the access goes on) when `copy`, then drop the hold
// on `releaseSource` when it is nonzero.
struct Action {
    bool copy = false;
    Job job;
    uint64_t releaseSource = 0;
};

class Table {
public:
    // A qualifying job. False: no record free (or S itself is a deferred D): the caller copies now and records nothing.
    // `release` is set when the job replaced a record of the same D whose source no scan holds any more.
    bool record(const Job &job, uint64_t *release)
    {
        *release = 0;
        if (findLive(job.source) >= 0)
            return false;
        int slot = findLive(job.dest);
        if (slot >= 0) {
            Record &old = records[slot];
            if (old.scans) {
                slot = freeSlot();
                if (slot < 0)
                    return false;
                old.superseded = true;
            } else {
                *release = old.held ? old.job.source : 0;
                old = Record();
            }
        } else if ((slot = freeSlot()) < 0)
            return false;
        Record &r = records[slot];
        r = Record();
        r.used = r.held = true;
        r.job = job;
        return true;
    }

    // The display, building a swap with D: the source window A scans instead, or 0 (no deferred D: scan D as usual).
    uint64_t claim(uint64_t dest, Job *job)
    {
        const int slot = findLive(dest);
        if (slot < 0 || records[slot].copied)
            return 0;
        Record &r = records[slot];
        ++r.scans;
        *job = r.job;
        return r.job.source;
    }

    // The display, once a later frame latched: one claim of D's source is scanned no more.
    Action retire(uint64_t dest, uint64_t source)
    {
        Action a;
        for (Record &r : records)
            if (r.used && r.job.dest == dest && r.job.source == source && r.scans) {
                --r.scans;
                settle(r, a);
                return a;
            }
        return a;
    }

    // Any access to D but the display's scan.
    Action access(uint64_t dest, Access kind)
    {
        Action a;
        const int slot = findLive(dest);
        if (slot < 0)
            return a;
        Record &r = records[slot];
        if (kind == Access::NewDestination || kind == Access::Freed)
            r.superseded = true;
        else if (!r.copied) {
            a.copy = true;
            a.job = r.job;
            r.copied = true;
        }
        settle(r, a);
        return a;
    }

    bool deferred(uint64_t dest) const
    {
        const int slot = findLive(dest);
        return slot >= 0 && !records[slot].copied;
    }

    unsigned holds() const
    {
        unsigned n = 0;
        for (const Record &r : records)
            n += r.used && r.held;
        return n;
    }

private:
    struct Record {
        Job job;
        unsigned scans = 0;  // claims the display has not retired
        bool used = false, held = false;
        bool copied = false;      // D holds its real bytes
        bool superseded = false;  // D was rewritten or freed: only the scans of S keep the record
    };

    // A record drops its hold once nothing can read S through it: D copied or gone, and no scan of S left.
    static void settle(Record &r, Action &a)
    {
        if (r.scans || !(r.copied || r.superseded))
            return;
        if (r.held)
            a.releaseSource = r.job.source;
        r = Record();
    }

    int findLive(uint64_t dest) const
    {
        for (unsigned i = 0; i < kRecords; ++i)
            if (records[i].used && !records[i].superseded && records[i].job.dest == dest)
                return (int)i;
        return -1;
    }

    int freeSlot() const
    {
        for (unsigned i = 0; i < kRecords; ++i)
            if (!records[i].used)
                return (int)i;
        return -1;
    }

    Record records[kRecords];
};

}  // namespace SwitchDeferredScale
