// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOMobileFramebuffer's frame information, as J120AP's display pipe (21H461 IOMFB::UPPipe_H9G) gives it, with J181AP's
// keys under iOS 27 (24B119's IOMobileFramebuffer.framework decodes the same entries): supportedFrameInfo (selector 74)
// names the keys, ChangeFrameInfo (selector 73, slot changeFrameInfo) picks a client's, and at each present
// the family's frameInfo_notify hands every client sendFrameInfo, which queues one entry on the client's shared data queue:
// uint64 {mask, swap id, one value per set bit, lowest bit first}. IOMobileFramebuffer.framework turns the entry into a
// dictionary for Core Animation's frame_info_callback, which matches it to its swap by id and reads Presentation_time and
// Requested_presentation.
//
// Those two are continuous time on this side of the user client: unless QuartzCore sets parameter 8 (only with
// CADeviceSupportsAndromeda), IOMobileFramebuffer.framework turns SwapSetTimestamps' absolute times into continuous
// ones (kern_ingest_timestamp) and the frame info's Issued_time, Presentation_time and Requested_presentation back
// (kern_egest_timestamp). The swap request's timestamp is already continuous; a present's mach_absolute_time stamp is not.
//
// J120 publishes 14 keys, J181 the first 11 of them (its key table ends at Performance_headroom: no Thermal_throttle,
// PRC_duration or CA_mode). These are the ones of the kind's iPad that DC A can answer truthfully. Thermal_throttle is 0
// because DC A scans at the panel's clock and has no thermal performance state. The per-layer, blend, colour-manager and
// dither CRCs, Issued_time, Performance_headroom, PRC_duration and CA_mode are pipe internals with no DC A counterpart;
// IOMobileFramebuffer refuses those keys to a client as it refuses Min_FrameTime on either iPad.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SwitchFrameTickets.h"

#ifndef SWITCH_IPSW_BUILD
#error "SwitchFrameInfo.h: the key set is the IPSW kind's; compile with -include switch_ipsw_volumes.h"
#endif

namespace switch_frame_info {

enum Key : uint32_t {
    kPresentationTime,
    kPresentationDelta,
    kRequestedPresentation,
    kThermalThrottle,
};

#if SWITCH_IPSW_DEVICE_J181AP
inline constexpr uint32_t kKeys = kThermalThrottle;
#else
inline constexpr uint32_t kKeys = kThermalThrottle + 1;
#endif

inline constexpr const char *kNames[kThermalThrottle + 1] = {"Presentation_time", "Presentation_delta",
                                                             "Requested_presentation", "Thermal_throttle"};
constexpr uint32_t kAllKeys = (1u << kKeys) - 1;
constexpr uint32_t kNotificationType = 3;  // EnableNotifications' frame-info type
constexpr size_t kKeyBytes = 64;
constexpr unsigned kEntryWords = 2 + kKeys;

// supportedFrameInfo's second output. J120's 1 makes IOMobileFramebuffer.framework map the client's shared data queue
// (IOConnectMapMemory type 3) and read entries from it; 0 would make it wait for async messages the family never sends.
constexpr uint32_t kSharedQueue = 1;

enum Answer {
    kOk,
    kBadArgument,
    kNoResources
};

inline Answer supported(uint32_t index, uint32_t *count, char *key, uint32_t *flags)
{
    if (index >= kKeys)
        return kBadArgument;
    *count = kKeys;
    *flags = kSharedQueue;
    const char *name = kNames[index];
    size_t i = 0;
    for (; name[i] && i + 1 < kKeyBytes; ++i)
        key[i] = name[i];
    for (; i < kKeyBytes; ++i)
        key[i] = 0;
    return kOk;
}

inline uint64_t continuousTime(uint64_t absolute, uint64_t nowAbsolute, uint64_t nowContinuous)
{
    return absolute + (nowContinuous - nowAbsolute);
}

// What a present hands every client through the family (frameInfo_notify passes it unchanged to sendFrameInfo); times
// continuous.
struct Frame {
    uint64_t presented, requested;
    uint32_t swapId;
};

// One client's entry. lastPresented is its previous Presentation_time, 0 before the first: J120 keeps it per client from
// 0, so a client's first Presentation_delta is the presentation time itself.
inline unsigned entry(uint32_t mask, const Frame &frame, uint64_t &lastPresented, uint64_t out[kEntryWords])
{
    mask &= kAllKeys;
    unsigned n = 0;
    out[n++] = mask;
    out[n++] = frame.swapId;
    for (uint32_t key = 0; key < kKeys; ++key) {
        if (!(mask & (1u << key)))
            continue;
        switch (key) {
        case kPresentationTime:
            out[n++] = frame.presented;
            break;
        case kPresentationDelta:
            out[n++] = frame.presented - lastPresented;
            break;
        case kRequestedPresentation:
            out[n++] = frame.requested;
            break;
        default:
            out[n++] = 0;
            break;
        }
    }
    lastPresented = frame.presented;
    return n;
}

// The clients with keys enabled, by the family's notification record (one per user client). Every call is under the
// display's work-loop gate: the family's registration, ChangeFrameInfo and frameInfo_notify all run gated.
class Clients {
public:
    static constexpr unsigned kMax = 8;

    struct Entry {
        const void *record;
        uint32_t mask;
        uint64_t lastPresented;
    };

    // A mask of 0 forgets the client, as J120's pipe then sends it nothing.
    Answer change(const void *record, uint32_t mask)
    {
        mask &= kAllKeys;
        Entry *free = nullptr;
        for (Entry &e : entries) {
            if (e.record == record) {
                if (mask) {
                    e.mask = mask;
                } else {
                    e = {};
                }
                return kOk;
            }
            if (!e.record && !free)
                free = &e;
        }
        if (!mask)
            return kOk;
        if (!free)
            return kNoResources;
        *free = {record, mask, 0};
        return kOk;
    }

    Entry *find(const void *record)
    {
        for (Entry &e : entries) {
            if (record && e.record == record)
                return &e;
        }
        return nullptr;
    }

    bool any() const
    {
        for (const Entry &e : entries) {
            if (e.record)
                return true;
        }
        return false;
    }

private:
    Entry entries[kMax] = {};
};

// Swaps between the kick taking them and the screen showing them, by take sequence. A present names the sequence of the
// swap it shows: that swap's frame comes out, and older ones go unsent (replaced before a frame showed them; Core
// Animation drops its frame info for every swap id up to the one it gets).
class Pending {
public:
    static constexpr unsigned kMax = 16;

    void take(uint64_t seq, uint32_t swapId, uint64_t requested)
    {
        if (!seq)
            return;
        Swap *slot = &swaps[0];
        for (Swap &s : swaps) {
            if (!s.seq) {
                slot = &s;
                break;
            }
            if (s.seq < slot->seq)
                slot = &s;
        }
        *slot = {seq, swapId, requested};
    }

    bool shown(uint64_t seq, uint64_t presented, Frame &frame)
    {
        bool found = false;
        for (Swap &s : swaps) {
            if (!s.seq || s.seq > seq)
                continue;
            if (s.seq == seq) {
                frame = {presented, s.requested, s.swapId};
                found = true;
            }
            s = {};
        }
        return found;
    }

private:
    struct Swap {
        uint64_t seq;
        uint32_t swapId;
        uint64_t requested;
    };

    Swap swaps[kMax] = {};
};

struct State {
    Clients clients;
    Pending pending;
    SwitchFrameTickets tickets;
};

}  // namespace switch_frame_info
