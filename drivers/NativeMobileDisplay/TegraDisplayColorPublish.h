// SPDX-License-Identifier: BSD-2-Clause-Patent
// The colour unit's state between its three writers, with no lock any of them shares (TegraDisplayColor.h has the
// contract; TegraDisplayColorSequence.h documents DC A's unshadowed LUT/CSC and shadowed CMU_ENABLE rules):
//   * the setters (IOMobileFramebuffer's calls, any thread, serialized by their own lock) build each new set of tables
//     into a spare snapshot and publish it with one store;
//   * the action (the frame-end work-loop pass, interrupts enabled) is the sole writer of CMU_ENABLE, whole-table
//     LUT1/LUT2/CSC programming (only while CMU_ENABLE is off and its bypass has latched), and the windows' palettes;
//   * the filter (DC A's frame end, primary interrupt context) only writes the <=9 changed CSC words during vertical
//     blanking when CMU_ENABLE is already live and LUT1/LUT2 are unchanged; steady-state frames return after two loads.
// C++, header only, for the driver and its host tests.
#pragma once
#include <stdint.h>
#include <string.h>

#include "TegraDisplayColorMath.h"
#include "TegraDisplayColorSequence.h"
#include "TegraDisplayPalette.h"

namespace switch_color {

// DC A word indices (L4T drivers/video/tegra/dc/dc_reg.h). The CMU's LUTs and CSC are written straight to the unit, with
// no assembly copy (TegraDisplayColorSequence.h). DISP_COLOR_CONTROL is an assembly register. DBG_TIMING bit 15 is
// DBG_V_BLANK (TegraPanel.cpp reads it the same way).
constexpr uint32_t kDcStateControl = 0x041, kDcCmuCsc = 0x32a, kDcCmuLut1 = 0x336, kDcCmuLut2 = 0x337,
                   kDcDispColorControl = 0x430, kDcDispDbgTiming = 0x4f6;
constexpr uint32_t kGeneralActReq = 1u << 0, kGeneralUpdate = 1u << 8, kCmuEnable = 1u << 20, kDbgVBlank = 1u << 15;

struct Snapshot {
    Tables tables;
    uint32_t palette[kPaletteWords];
    uint32_t paletteGeneration;  // the generation that last changed the palette words
};

// Platform: hold() and release() a frame-end hold; deviceWritesDone() after a register read-back (a dsb in the kernel).
template <class Platform> class ColourPublication {
public:
    static constexpr unsigned kSnapshots = 4, kNone = kSnapshots;

    // With the filter and the action stopped and the setters' lock held (attach, detach): the action adopts the newest
    // snapshot at its first frame end and writes it whole; no generation published before holds the frame end.
    void reset(uint32_t colorControlFound)
    {
        colorControl = colorControlFound;
        const uint32_t pubGen = generationOf(load(publishedWord, __ATOMIC_RELAXED));
        released = pubGen;
        filterGen = 0;
        filterSeenCmuState = 0;
        filterTablesEpoch = 0;
        memset(filterCsc, 0, sizeof(filterCsc));
        filterAdopted = false;
        filterPending = false;
        action = {};
        actionLatchedEpoch = 0;
        actionLiveEpoch = 0;
        actionTablesEpoch = 0;
        actionSettledGen = 0;
        actionCscWrittenGen = 0;
        actionPaletteGen = 0;
        actionPaletteEpoch = 0;
        actionAdopted = false;
        actionActiveValid = false;
        actionPaletteShown = false;
        store(filterBusy, 0u, __ATOMIC_RELEASE);
        store(actionCmuState, 0u, __ATOMIC_RELEASE);
        store(filterCscDoneGen, pubGen, __ATOMIC_RELEASE);
        store(paletteWord, 0u, __ATOMIC_RELEASE);
        store(filterReading, kNone, __ATOMIC_RELEASE);
        store(actionReading, kNone, __ATOMIC_RELEASE);
    }

    // --- setters (the caller's own lock held) ---

    // The snapshot neither published nor held by the filter or action; the setter fills it in and publishes it.
    Snapshot &spare()
    {
        const uint64_t published = load(publishedWord, __ATOMIC_SEQ_CST);
        const unsigned fBusy = load(filterReading, __ATOMIC_SEQ_CST);
        const unsigned aBusy = load(actionReading, __ATOMIC_SEQ_CST);
        for (unsigned i = 0; i < kSnapshots; ++i) {
            if (i != indexOf(published) && i != fBusy && i != aBusy) {
                spareIndex = i;
                return snapshots[i];
            }
        }
        __builtin_trap();
    }

    // The spare (its tables and palette filled in) goes out with one store. Attached, its generation takes a frame-end
    // hold first, which the action releases once it has seen the generation settled; detached, nothing will.
    void publish(bool paletteOn, bool paletteChanged, bool attached)
    {
        const uint64_t before = load(publishedWord, __ATOMIC_RELAXED);
        const uint32_t generation = generationOf(before) + 1u;
        uint8_t epoch = paletteEpochOf(before);
        if (paletteOnOf(before) && !paletteOn)
            epoch = static_cast<uint8_t>(epoch + 1);
        Snapshot &next = snapshots[spareIndex];
        next.paletteGeneration =
            paletteChanged || !paletteOn ? generation : snapshots[indexOf(before)].paletteGeneration;
        if (attached)
            Platform::hold();
        else
            released = generation;
        store(publishedWord, packPublished(generation, spareIndex, next.tables, paletteOn, epoch), __ATOMIC_SEQ_CST);
    }

    // The newest snapshot (the setters' lock held, or nothing publishing).
    const Snapshot &newest() const
    {
        return snapshots[indexOf(load(publishedWord, __ATOMIC_RELAXED))];
    }

    // The first snapshot (init: nothing else runs yet); generation 0 holds nothing.
    Snapshot &initial()
    {
        return snapshots[0];
    }

    void initialPublished()
    {
        store(publishedWord, packPublished(0, 0, snapshots[0].tables, false, 0), __ATOMIC_RELEASE);
        released = 0;
    }

    // --- the filter (DC A's frame end; interrupts off; nothing here locks, sleeps or allocates) ---

    template <class Dc> void frameEnd(Dc &dc)
    {
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        const uint32_t pubGen = generationOf(published);
        const uint32_t cmuState = load(actionCmuState, __ATOMIC_ACQUIRE);
        if (!filterPending && filterAdopted && pubGen == filterGen && cmuState == filterSeenCmuState)
            return;
        // Dekker with frameEndAction: filterBusy=1 before reading actionCmuState guarantees action never mutates
        // actionActive or DC A CMU tables while we inspect actionActive and write the <=9 CSC words.
        store(filterBusy, 1u, __ATOMIC_SEQ_CST);
        const uint32_t liveState = load(actionCmuState, __ATOMIC_SEQ_CST);
        if ((liveState & 1u) == 0u) {
            filterGen = pubGen;
            filterSeenCmuState = liveState;
            filterAdopted = true;
            filterPending = false;
            store(filterBusy, 0u, __ATOMIC_RELEASE);
            return;
        }
        const uint64_t cur = claimNewest(published, filterReading);
        const uint32_t curGen = generationOf(cur);
        const Tables &pending = snapshots[indexOf(cur)].tables;
        if (filterTablesEpoch != actionTablesEpoch) {
            memcpy(filterCsc, actionActive.csc, sizeof(filterCsc));
            filterTablesEpoch = actionTablesEpoch;
        }
        if (!pending.enabled || lutsDiffer(pending, actionActive)) {
            filterGen = curGen;
            filterSeenCmuState = liveState;
            filterAdopted = true;
            filterPending = false;
        } else if ((dc[kDcDispDbgTiming] & kDbgVBlank) != 0u) {
            writeCsc(dc, pending.csc, filterCsc);
            memcpy(filterCsc, pending.csc, sizeof(filterCsc));
            filterGen = curGen;
            filterSeenCmuState = liveState;
            filterAdopted = true;
            filterPending = false;
            store(filterCscDoneGen, curGen, __ATOMIC_RELEASE);
        } else {
            filterPending = true;
        }
        store(filterReading, kNone, __ATOMIC_RELEASE);
        store(filterBusy, 0u, __ATOMIC_RELEASE);
    }

    // --- the action (the frame-end work-loop pass, under the gate, interrupts enabled) ---

    // What changed at DC A for the log: kKeep, or the enable written, with the reasons of the tables it is for.
    struct Step {
        Enable enable;
        bool want;
        uint32_t reasons;
    };

    template <class Dc> Step frameEndAction(Dc &dc, bool mayRequest)
    {
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        const uint32_t pubGen = generationOf(published);
        const bool want = enabledOf(published);
        if (actionAdopted && pubGen == actionSettledGen)
            return {Enable::kKeep, want, reasonsOf(published)};

        const uint64_t cur = claimNewest(published, actionReading);
        const uint32_t curGen = generationOf(cur);
        const bool curWant = enabledOf(cur);
        const uint32_t curReasons = reasonsOf(cur);
        const Snapshot &snap = snapshots[indexOf(cur)];

        if (paletteOnOf(cur)) {
            const uint8_t ep = paletteEpochOf(cur);
            if (!actionPaletteShown || snap.paletteGeneration != actionPaletteGen || ep != actionPaletteEpoch) {
                writePalettes(dc, snap.palette);
                actionPaletteGen = snap.paletteGeneration;
                actionPaletteEpoch = ep;
                actionPaletteShown = true;
                store(paletteWord, packPalette(true, ep), __ATOMIC_RELEASE);
            }
        } else if (actionPaletteShown) {
            actionPaletteShown = false;
            store(paletteWord, packPalette(false, actionPaletteEpoch), __ATOMIC_RELEASE);
        }

        Enable enable = Enable::kKeep;
        bool cmuSettled = false;

        if (action.enableAsked) {
            const bool needBypass = !curWant || !actionActiveValid || lutsDiffer(snap.tables, actionActive);
            if (needBypass) {
                if ((load(actionCmuState, __ATOMIC_RELAXED) & 1u) != 0u)
                    store(actionCmuState, actionLiveEpoch << 1, __ATOMIC_SEQ_CST);
                if (load(filterBusy, __ATOMIC_SEQ_CST) == 0u && mayRequest) {
                    writeEnable(dc, false);
                    enable = Enable::kOff;
                }
            } else {
                if ((load(actionCmuState, __ATOMIC_RELAXED) & 1u) == 0u) {
                    ++actionLiveEpoch;
                    store(actionCmuState, (actionLiveEpoch << 1) | 1u, __ATOMIC_SEQ_CST);
                }
                const uint32_t cscDone = load(filterCscDoneGen, __ATOMIC_ACQUIRE);
                if (curGen == actionCscWrittenGen || static_cast<int32_t>(cscDone - curGen) >= 0)
                    cmuSettled = true;
            }
        } else {
            if (action.bypassEpoch != actionLatchedEpoch && (dc[kDcStateControl] & kGeneralActReq) == 0u)
                actionLatchedEpoch = action.bypassEpoch;
            if (action.bypassEpoch == actionLatchedEpoch) {
                if ((load(actionCmuState, __ATOMIC_RELAXED) & 1u) != 0u)
                    store(actionCmuState, actionLiveEpoch << 1, __ATOMIC_SEQ_CST);
                if (load(filterBusy, __ATOMIC_SEQ_CST) == 0u) {
                    if (!curWant) {
                        cmuSettled = true;
                    } else {
                        if (!actionActiveValid || tablesDiffer(snap.tables, actionActive) ||
                            curGen != actionCscWrittenGen) {
                            writeTables(dc, snap.tables, actionActiveValid ? &actionActive : nullptr);
                            actionActive = snap.tables;
                            actionActiveValid = true;
                            ++actionTablesEpoch;
                            actionCscWrittenGen = curGen;
                        }
                        if (mayRequest) {
                            writeEnable(dc, true);
                            enable = Enable::kOn;
                            ++actionLiveEpoch;
                            store(actionCmuState, (actionLiveEpoch << 1) | 1u, __ATOMIC_SEQ_CST);
                            cmuSettled = true;
                        }
                    }
                }
            }
        }

        store(actionReading, kNone, __ATOMIC_RELEASE);
        if (cmuSettled) {
            actionAdopted = true;
            actionSettledGen = curGen;
            releaseThrough(curGen);
        }
        return {enable, curWant, curReasons};
    }

    // Any thread: CP_ENABLE once the newest per-channel gamma table's palette is in the windows (TegraDisplayPalette.h).
    uint32_t windowOptions() const
    {
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        const uint32_t pal = load(paletteWord, __ATOMIC_ACQUIRE);
        const bool shown =
            paletteOnOf(published) && ((pal & 1u) != 0u) && static_cast<uint8_t>(pal >> 8) == paletteEpochOf(published);
        return shown ? kCpEnable : 0;
    }

    // --- with the filter stopped (its interrupt source disabled or not yet registered), under the gate ---

    // After DC A's power-up reset, before the request the resume latches it with: the newest tables, palette and enable.
    template <class Dc> void restore(Dc &dc)
    {
        action = {};
        actionLatchedEpoch = 0;
        actionActiveValid = false;
        actionPaletteShown = false;
        actionAdopted = false;
        filterAdopted = false;
        filterPending = false;
        store(filterBusy, 0u, __ATOMIC_RELEASE);
        store(filterReading, kNone, __ATOMIC_RELEASE);
        store(actionReading, kNone, __ATOMIC_RELEASE);
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        const uint32_t pubGen = generationOf(published);
        const Snapshot &now = snapshots[indexOf(published)];
        if (paletteOnOf(published)) {
            writePalettes(dc, now.palette);
            actionPaletteGen = now.paletteGeneration;
            actionPaletteEpoch = paletteEpochOf(published);
            actionPaletteShown = true;
            store(paletteWord, packPalette(true, actionPaletteEpoch), __ATOMIC_RELEASE);
        } else {
            store(paletteWord, packPalette(false, 0), __ATOMIC_RELEASE);
        }
        if (now.tables.enabled) {
            writeTables(dc, now.tables, nullptr);
            actionActive = now.tables;
            actionActiveValid = true;
            ++actionTablesEpoch;
            actionCscWrittenGen = pubGen;
            action.enableAsked = true;
            dc[kDcDispColorControl] = colorControl | kCmuEnable;
            ++actionLiveEpoch;
            store(actionCmuState, (actionLiveEpoch << 1) | 1u, __ATOMIC_RELEASE);
        } else {
            store(actionCmuState, actionLiveEpoch << 1, __ATOMIC_RELEASE);
        }
        memcpy(filterCsc, actionActive.csc, sizeof(filterCsc));
        filterTablesEpoch = actionTablesEpoch;
        filterGen = pubGen;
        filterSeenCmuState = load(actionCmuState, __ATOMIC_RELAXED);
        filterAdopted = true;
        store(filterCscDoneGen, pubGen, __ATOMIC_RELEASE);
        actionAdopted = true;
        actionSettledGen = pubGen;
    }

    // The display going away: the CMU bypassed, every hold out released.
    template <class Dc> void stop(Dc &dc)
    {
        if (action.enableAsked) {
            dc[kDcDispColorControl] = colorControl;
            dc[kDcStateControl] = kGeneralUpdate;
            dc[kDcStateControl] = kGeneralActReq;
        }
        releaseThrough(generationOf(load(publishedWord, __ATOMIC_ACQUIRE)));
        reset(colorControl);
    }

    void releaseThrough(uint32_t generation)
    {
        for (; released != generation; ++released)
            Platform::release();
    }

    uint32_t holdsOut() const
    {
        return generationOf(load(publishedWord, __ATOMIC_ACQUIRE)) - released;
    }

    uint32_t publishedGeneration() const
    {
        return generationOf(load(publishedWord, __ATOMIC_ACQUIRE));
    }

    // The action's (under the gate): every hold through `generation` released, its tables and enable in DC A.
    bool settledThrough(uint32_t generation) const
    {
        return static_cast<int32_t>(released - generation) >= 0;
    }

    const ActionSequence &actionState() const
    {
        return action;
    }

    uint32_t colorControl = 0;

private:
    template <class T> static T load(const T &word, int order)
    {
        return __atomic_load_n(&word, order);
    }

    template <class T> static void store(T &word, T value, int order)
    {
        __atomic_store_n(&word, value, order);
    }

    static uint64_t packPublished(uint32_t generation, unsigned index, const Tables &tables, bool paletteOn,
                                  uint8_t epoch)
    {
        return uint64_t(generation) | uint64_t(index & 3u) << 32 | uint64_t(tables.enabled) << 34 |
               uint64_t(paletteOn) << 35 | uint64_t(tables.reasons & 0x3fu) << 36 | uint64_t(epoch) << 48;
    }

    static uint32_t generationOf(uint64_t w)
    {
        return static_cast<uint32_t>(w);
    }

    static unsigned indexOf(uint64_t w)
    {
        return static_cast<unsigned>(w >> 32) & 3u;
    }

    static bool enabledOf(uint64_t w)
    {
        return ((w >> 34) & 1u) != 0u;
    }

    static bool paletteOnOf(uint64_t w)
    {
        return ((w >> 35) & 1u) != 0u;
    }

    static uint32_t reasonsOf(uint64_t w)
    {
        return static_cast<uint32_t>(w >> 36) & 0x3fu;
    }

    static uint8_t paletteEpochOf(uint64_t w)
    {
        return static_cast<uint8_t>(w >> 48);
    }

    static uint32_t packPalette(bool shown, uint8_t epoch)
    {
        return uint32_t(shown) | (uint32_t(epoch) << 8);
    }

    static bool tablesDiffer(const Tables &a, const Tables &b)
    {
        return lutsDiffer(a, b) || memcmp(a.csc, b.csc, sizeof(a.csc)) != 0;
    }

    uint64_t claimNewest(uint64_t published, unsigned &readingSlot)
    {
        uint64_t cur = published;
        for (;;) {
            store(readingSlot, indexOf(cur), __ATOMIC_SEQ_CST);
            const uint64_t again = load(publishedWord, __ATOMIC_SEQ_CST);
            if (again == cur)
                return cur;
            cur = again;
        }
    }

    template <class Dc> void writeEnable(Dc &dc, bool on)
    {
        dc[kDcDispColorControl] = colorControl | (on ? kCmuEnable : 0u);
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
        (void)static_cast<uint32_t>(dc[kDcStateControl]);
        Platform::deviceWritesDone();
        enableWritten(action, on);
    }

    template <class Dc> static void writeTables(Dc &dc, const Tables &tables, const Tables *written)
    {
        for (unsigned i = 0; i < kLut1Entries; ++i) {
            if (!written || written->lut1[i] != tables.lut1[i])
                dc[kDcCmuLut1] = i | static_cast<uint32_t>(tables.lut1[i]) << 16;
        }
        // Always write all 9 CSC words: the filter may have updated DC A's CSC while CMU was live.
        for (unsigned i = 0; i < kCscWords; ++i)
            dc[kDcCmuCsc + i] = tables.csc[i];
        for (unsigned k = 0; k < kLut2Entries; ++k) {
            if (!written || written->lut2[k] != tables.lut2[k])
                dc[kDcCmuLut2] = k | static_cast<uint32_t>(tables.lut2[k]) << 16;
        }
    }

    template <class Dc>
    static void writeCsc(Dc &dc, const uint16_t (&csc)[kCscWords], const uint16_t (&written)[kCscWords])
    {
        for (unsigned i = 0; i < kCscWords; ++i) {
            if (written[i] != csc[i])
                dc[kDcCmuCsc + i] = csc[i];
        }
    }

    template <class Dc> static void writePalettes(Dc &dc, const uint32_t (&palette)[kPaletteWords])
    {
        for (const uint32_t base : kPaletteBase) {
            for (uint32_t i = 0; i < kPaletteWords; ++i)
                dc[base + i] = palette[i];
        }
    }

    Snapshot snapshots[kSnapshots];
    uint64_t publishedWord = 0;
    unsigned filterReading = kNone;
    unsigned actionReading = kNone;
    unsigned spareIndex = 0;
    uint32_t released = 0;
    uint32_t filterBusy = 0;
    uint32_t actionCmuState = 0;
    uint32_t filterCscDoneGen = 0;
    uint32_t paletteWord = 0;
    // The filter's.
    uint32_t filterGen = 0, filterSeenCmuState = 0, filterTablesEpoch = 0;
    uint16_t filterCsc[kCscWords] = {};
    bool filterAdopted = false, filterPending = false;
    // The action's.
    ActionSequence action;
    uint16_t actionLatchedEpoch = 0;
    uint32_t actionLiveEpoch = 0, actionTablesEpoch = 0;
    uint32_t actionSettledGen = 0, actionCscWrittenGen = 0;
    uint32_t actionPaletteGen = 0;
    uint8_t actionPaletteEpoch = 0;
    bool actionAdopted = false, actionActiveValid = false, actionPaletteShown = false;
    Tables actionActive;
};

}  // namespace switch_color
