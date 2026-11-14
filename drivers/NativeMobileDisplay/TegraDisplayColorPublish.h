// SPDX-License-Identifier: BSD-2-Clause-Patent
// The colour unit's state between its three writers, with no lock any of them shares (TegraDisplayColor.h has the
// contract; TegraDisplayColorSequence.h decides what goes to DC A when):
//   * the setters (IOMobileFramebuffer's calls, any thread, serialized by their own lock) build each new set of tables
//     into a spare snapshot and publish it with one store;
//   * the filter (DC A's frame end, primary interrupt context) adopts the newest snapshot and is the only writer of the
//     CMU's LUT1, CSC and LUT2 words and of the windows' palettes;
//   * the action (the frame-end work-loop pass) is the only writer of CMU_ENABLE, which needs GENERAL_ACT_REQ, a
//     request other window A writers share on the gate.
// Each writer publishes its own state in one word the others read. Two exchanges need both sides to see each other
// (store, then load, both sequentially consistent; ARMv8's STLR then LDAR are ordered): a setter choosing a spare
// against the filter claiming the snapshot it reads, and the action enabling the CMU against the filter writing whole
// tables while it is off. Each generation published takes one frame-end hold before it is published, and the action
// releases the holds of every generation it has seen settled. C++, header only, for the driver and its host tests.
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
    static constexpr unsigned kSnapshots = 3, kNone = kSnapshots;

    // With the filter and the action stopped and the setters' lock held (attach, detach): the filter adopts the newest
    // snapshot at its first frame end and writes it whole; no generation published before holds the frame end.
    void reset(uint32_t colorControlFound)
    {
        colorControl = colorControlFound;
        released = generationOf(load(publishedWord, __ATOMIC_RELAXED));
        filter = {};
        filterAdopted = false;
        filterIndex = kNone;
        filterPaletteGeneration = 0;
        filterPaletteEpoch = 0;
        filterPaletteDirty = filterPaletteShown = false;
        action = {};
        filterStored = packFilter();
        store(filterWord, filterStored, __ATOMIC_RELEASE);
        store(actionWord, packAction(false), __ATOMIC_RELEASE);
        store(reading, kNone, __ATOMIC_RELEASE);
    }

    // --- setters (the caller's own lock held) ---

    // The snapshot neither published nor the filter's; the setter fills it in and publishes it.
    Snapshot &spare()
    {
        const uint64_t published = load(publishedWord, __ATOMIC_SEQ_CST);
        const unsigned busy = load(reading, __ATOMIC_SEQ_CST);
        for (unsigned i = 0; i < kSnapshots; ++i) {
            if (i != indexOf(published) && i != busy) {
                spareIndex = i;
                return snapshots[i];
            }
        }
        __builtin_trap();
    }

    // The spare goes out: one hold for its generation, then one store.
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
        adoptNewest();
        const Tables &pending = snapshots[filterIndex].tables;
        // An action between its intent to enable and its enable published counts as enabled: whole tables are written
        // only with the CMU off. The adoption's store is before this load, the action's intent store before its re-read
        // of this word, so one of the two sees the other (and while this word says pendingDirty, no action enables).
        const uint64_t asking = load(actionWord, __ATOMIC_SEQ_CST);
        ActionSequence asked = actionOf(asking);
        asked.enableAsked = asked.enableAsked || intentOf(asking);
        const auto actRequestOut = [&dc] {
            return (dc[kDcStateControl] & kGeneralActReq) != 0u;
        };
        const auto inBlank = [&dc] {
            return (dc[kDcDispDbgTiming] & kDbgVBlank) != 0u;
        };
        const Tables *written = filter.activeValid ? &active : nullptr;
        const Write write = atFrameEnd(filter, asked, pending, written, actRequestOut, inBlank);
        if (write == Write::kAll)
            writeTables(dc, pending, written);
        else if (write == Write::kCsc)
            writeCsc(dc, pending, active);
        if (write != Write::kNothing)
            active = pending;
        if (filterPaletteDirty && inBlank()) {
            writePalettes(dc, snapshots[filterIndex].palette);
            filterPaletteDirty = false;
            filterPaletteShown = true;
            filterPaletteEpoch = paletteEpochOf(filterPublished);
        }
        publishFilter(__ATOMIC_RELEASE);
    }

    // --- the action (the frame-end work-loop pass, under the gate) ---

    // What changed at DC A for the log: kKeep, or the enable written, with the reasons of the tables it is for.
    struct Step {
        Enable enable;
        bool want;
        uint32_t reasons;
    };

    template <class Dc> Step frameEndAction(Dc &dc, bool mayRequest)
    {
        const uint64_t seen = load(filterWord, __ATOMIC_SEQ_CST);
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        FilterSequence f = filterOf(seen);
        const bool newer = !adoptedOf(seen) || adoptedGenerationOf(seen) != generationOf(published);
        f.pendingDirty = f.pendingDirty || newer;
        const bool want = enabledOf(published);
        Enable enable = atFrameEndAction(f, action, want, mayRequest);
        if (enable == Enable::kOn) {
            store(actionWord, packAction(true), __ATOMIC_SEQ_CST);
            if (load(filterWord, __ATOMIC_SEQ_CST) != seen ||
                generationOf(load(publishedWord, __ATOMIC_SEQ_CST)) != generationOf(published))
                enable = Enable::kKeep;
        }
        if (enable != Enable::kKeep) {
            dc[kDcDispColorControl] = colorControl | (enable == Enable::kOn ? kCmuEnable : 0);
            dc[kDcStateControl] = kGeneralUpdate;
            dc[kDcStateControl] = kGeneralActReq;
            (void)static_cast<uint32_t>(dc[kDcStateControl]);
            Platform::deviceWritesDone();
            enableWritten(action, enable == Enable::kOn);
        }
        store(actionWord, packAction(false), __ATOMIC_RELEASE);
        if (!newer && settled(f, action, want))
            releaseThrough(adoptedGenerationOf(seen));
        return {enable, want, reasonsOf(published)};
    }

    // Any thread: CP_ENABLE once the newest per-channel gamma table's palette is in the windows (TegraDisplayPalette.h).
    uint32_t windowOptions() const
    {
        const uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        const uint64_t filtered = load(filterWord, __ATOMIC_ACQUIRE);
        const bool shown = paletteOnOf(published) && paletteShownOf(filtered) &&
                           filteredPaletteEpochOf(filtered) == paletteEpochOf(published);
        return shown ? kCpEnable : 0;
    }

    // --- with the filter stopped (its interrupt source disabled or not yet registered), under the gate ---

    // After DC A's power-up reset, before the request the resume latches it with: the newest tables, palette and enable.
    template <class Dc> void restore(Dc &dc)
    {
        filter = {};
        action = {};
        adoptNewest();
        filter.pendingDirty = false;
        const Snapshot &now = snapshots[filterIndex];
        if (paletteOnOf(filterPublished)) {
            writePalettes(dc, now.palette);
            filterPaletteDirty = false;
            filterPaletteShown = true;
            filterPaletteEpoch = paletteEpochOf(filterPublished);
        }
        if (now.tables.enabled) {
            writeTables(dc, now.tables, nullptr);
            active = now.tables;
            filter.activeValid = action.enableAsked = true;
            dc[kDcDispColorControl] = colorControl | kCmuEnable;
        }
        publishFilter(__ATOMIC_RELEASE);
        store(actionWord, packAction(false), __ATOMIC_RELEASE);
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

    // The published word: generation, snapshot index, the tables' enable and reasons (the action's), the palette's state
    // (windowOptions'), so nothing but the filter reads a snapshot a later setter may reuse.
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

    // The filter's word: the generation it adopted, its sequence, the palette it has shown.
    uint64_t packFilter() const
    {
        return uint64_t(filterAdopted ? generationOf(filterPublished) : 0) | uint64_t(filterAdopted) << 32 |
               uint64_t(filter.pendingDirty) << 33 | uint64_t(filter.activeValid) << 34 |
               uint64_t(filter.bypassWanted) << 35 | uint64_t(filterPaletteShown) << 36 |
               uint64_t(filter.latchedEpoch) << 40 | uint64_t(filterPaletteEpoch) << 56;
    }

    static bool adoptedOf(uint64_t w)
    {
        return ((w >> 32) & 1u) != 0u;
    }

    static uint32_t adoptedGenerationOf(uint64_t w)
    {
        return static_cast<uint32_t>(w);
    }

    static FilterSequence filterOf(uint64_t w)
    {
        FilterSequence f;
        f.pendingDirty = ((w >> 33) & 1u) != 0u;
        f.activeValid = ((w >> 34) & 1u) != 0u;
        f.bypassWanted = ((w >> 35) & 1u) != 0u;
        f.latchedEpoch = static_cast<uint16_t>(w >> 40);
        return f;
    }

    static bool paletteShownOf(uint64_t w)
    {
        return ((w >> 36) & 1u) != 0u;
    }

    static uint8_t filteredPaletteEpochOf(uint64_t w)
    {
        return static_cast<uint8_t>(w >> 56);
    }

    // The action's word: its sequence, and the intent to enable it holds while it checks the filter has not moved.
    uint64_t packAction(bool intent) const
    {
        return uint64_t(action.enableAsked) | uint64_t(intent) << 1 | uint64_t(action.bypassEpoch) << 16;
    }

    static ActionSequence actionOf(uint64_t w)
    {
        ActionSequence a;
        a.enableAsked = (w & 1u) != 0u;
        a.bypassEpoch = static_cast<uint16_t>(w >> 16);
        return a;
    }

    static bool intentOf(uint64_t w)
    {
        return ((w >> 1) & 1u) != 0u;
    }

    // The newest snapshot becomes the filter's: claimed (the setters skip it) before it is read, the claim checked
    // against a publish that came meanwhile, its generation stored before anything decides on it.
    void adoptNewest()
    {
        uint64_t published = load(publishedWord, __ATOMIC_ACQUIRE);
        if (filterAdopted && generationOf(published) == generationOf(filterPublished))
            return;
        for (;;) {
            store(reading, indexOf(published), __ATOMIC_SEQ_CST);
            const uint64_t again = load(publishedWord, __ATOMIC_SEQ_CST);
            if (again == published)
                break;
            published = again;
        }
        const unsigned index = indexOf(published);
        filterPaletteDirty =
            paletteOnOf(published) &&
            (filterPaletteDirty || !filterAdopted || snapshots[index].paletteGeneration != filterPaletteGeneration ||
             paletteEpochOf(published) != filterPaletteEpoch || !filterPaletteShown);
        filterPaletteGeneration = snapshots[index].paletteGeneration;
        if (!paletteOnOf(published))
            filterPaletteShown = false;
        filterIndex = index;
        filterPublished = published;
        filterAdopted = true;
        filter.pendingDirty = true;
        publishFilter(__ATOMIC_SEQ_CST);
    }

    void publishFilter(int order)
    {
        const uint64_t word = packFilter();
        if (word == filterStored && order != __ATOMIC_SEQ_CST)
            return;
        filterStored = word;
        store(filterWord, word, order);
    }

    template <class Dc> static void writeTables(Dc &dc, const Tables &tables, const Tables *written)
    {
        for (unsigned i = 0; i < kLut1Entries; ++i) {
            if (!written || written->lut1[i] != tables.lut1[i])
                dc[kDcCmuLut1] = i | static_cast<uint32_t>(tables.lut1[i]) << 16;
        }
        for (unsigned i = 0; i < kCscWords; ++i) {
            if (!written || written->csc[i] != tables.csc[i])
                dc[kDcCmuCsc + i] = tables.csc[i];
        }
        for (unsigned k = 0; k < kLut2Entries; ++k) {
            if (!written || written->lut2[k] != tables.lut2[k])
                dc[kDcCmuLut2] = k | static_cast<uint32_t>(tables.lut2[k]) << 16;
        }
    }

    template <class Dc> static void writeCsc(Dc &dc, const Tables &tables, const Tables &written)
    {
        for (unsigned i = 0; i < kCscWords; ++i) {
            if (written.csc[i] != tables.csc[i])
                dc[kDcCmuCsc + i] = tables.csc[i];
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
    uint64_t publishedWord = 0;  // the setters'
    unsigned reading = kNone;    // the filter's: the snapshot it reads
    unsigned spareIndex = 0;     // the setters'
    uint32_t released = 0;       // the action's: holds released through this generation
    // The filter's.
    uint64_t filterWord = 0, filterPublished = 0, filterStored = 0;
    FilterSequence filter;
    Tables active;
    unsigned filterIndex = kNone;
    uint32_t filterPaletteGeneration = 0;
    uint8_t filterPaletteEpoch = 0;
    bool filterAdopted = false, filterPaletteDirty = false, filterPaletteShown = false;
    // The action's.
    uint64_t actionWord = 0;
    ActionSequence action;
};

}  // namespace switch_color
