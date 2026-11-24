// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

struct SwitchWLANPeerIdentity {
    uint32_t generation;
    uint64_t incarnation;
    uint8_t address[6];
    int8_t rssi;

    bool same(const SwitchWLANPeerIdentity &other) const
    {
        return generation == other.generation && incarnation == other.incarnation &&
               !memcmp(address, other.address, sizeof address);
    }
};

struct SwitchWLANAwdlPresence {
    SwitchWLANPeerIdentity installed;
    bool installedRoute, published;

    bool ready(const SwitchWLANPeerIdentity &desired) const
    {
        return installedRoute && published && installed.same(desired);
    }

    enum class Change { None, Add, Remove };

    Change change(const SwitchWLANPeerIdentity *desired) const
    {
        if (installedRoute)
            return desired && installed.same(*desired) ? (published ? Change::None : Change::Add) : Change::Remove;
        return desired ? Change::Add : Change::None;
    }

    int complete(Change action, const SwitchWLANPeerIdentity &request, const SwitchWLANPeerIdentity *current)
    {
        if (action == Change::Remove) {
            const bool wasPublished = published;
            *this = {};
            return wasPublished ? -1 : 0;
        }
        installed = request;
        installedRoute = true;
        published = current && current->same(request);
        return published ? 1 : 0;
    }
};
