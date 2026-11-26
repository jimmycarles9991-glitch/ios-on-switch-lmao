// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB-C port's transports, one of each type, each following XusbLanes.h's xusb_transport_step on its own.
// SwitchUSBPort.cpp drives IOAccessoryManager's IOPort with it from the role thread; the host tests drive a model of
// that IOPort, which refuses a second transport of a type as Apple's does.
#pragma once
#include <stdint.h>
#include "XusbLanes.h"

namespace xport {

inline constexpr uint32_t kUSB2 = 2, kUSB3 = 3;

// The port's side. create returns a retained transport with its data role set, not yet on the port.
struct Ops {
    void *(*create)(uint32_t type, enum xusb_port_role role);
    bool (*add)(void *transport, uint32_t type, uint32_t index);
    void (*remove)(uint32_t type, uint32_t index);
    void (*release)(void *transport);
    void (*note)(const char *what, uint32_t type, uint32_t index, enum xusb_port_role role, const char *why);
};

class Transports {
public:
    explicit constexpr Transports(const Ops &ops) : ops(&ops)
    {}

    // The USB 2.0 transport's data role, which is the port's.
    enum xusb_port_role role() const
    {
        return live[kUsb2Slot].role;
    }

    bool holdsRetired() const
    {
        return retired[kUsb2Slot].transport != nullptr || retired[kUsb3Slot].transport != nullptr;
    }

    // Removals first (USB 3, then USB 2.0), then additions (USB 2.0, then USB 3); a swap's removal and addition are in
    // this one call. A retired transport of a type that gets a new one comes out before it.
    void follow(enum xusb_port_role to, bool laneUp, bool ssReset, const char *why)
    {
        enum xusb_transport_step steps[kSlots];
        steps[kUsb2Slot] = xusb_transport_decide(live[kUsb2Slot].role, to != XUSB_PORT_EMPTY, to);
        steps[kUsb3Slot] =
            xusb_transport_decide(live[kUsb3Slot].role, xusb_usb3_transport_wanted(to, laneUp, ssReset), to);
        for (const unsigned slot : kRemovalOrder) {
            if (steps[slot] == XUSB_TRANSPORT_REMOVE || steps[slot] == XUSB_TRANSPORT_SWAP)
                drop(live[slot], slot, why);
        }
        for (const unsigned slot : kAdditionOrder) {
            if (steps[slot] == XUSB_TRANSPORT_ADD || steps[slot] == XUSB_TRANSPORT_SWAP) {
                drop(retired[slot], slot, retiredWhy);
                put(slot, to, why);
            }
        }
    }

    // Leaving the host role with a device on the root port: Apple's port sets the transports' data rate and signalling
    // frequencies as it destroys the device, so they stay on the port until dropRetired.
    void retire(const char *why)
    {
        for (const unsigned slot : kRemovalOrder) {
            drop(retired[slot], slot, retiredWhy);
            retired[slot] = live[slot];
            live[slot] = Slot{};
            if (retired[slot].transport)
                ops->note("kept until its device is torn down", kType[slot], kIndex[slot], retired[slot].role, why);
        }
        retiredWhy = why;
    }

    void dropRetired()
    {
        for (const unsigned slot : kRemovalOrder)
            drop(retired[slot], slot, retiredWhy);
    }

private:
    struct Slot {
        void *transport = nullptr;
        enum xusb_port_role role = XUSB_PORT_EMPTY;
    };

    static constexpr unsigned kUsb2Slot = 0, kUsb3Slot = 1, kSlots = 2;
    static constexpr unsigned kRemovalOrder[kSlots] = {kUsb3Slot, kUsb2Slot};
    static constexpr unsigned kAdditionOrder[kSlots] = {kUsb2Slot, kUsb3Slot};
    static constexpr uint32_t kType[kSlots] = {kUSB2, kUSB3};
    // A pair the port held together on a 24B119 device boot.
    static constexpr uint32_t kIndex[kSlots] = {0, 1};

    const Ops *ops;
    Slot live[kSlots], retired[kSlots];
    const char *retiredWhy = nullptr;

    void drop(Slot &slot, unsigned which, const char *why) const
    {
        if (!slot.transport)
            return;
        ops->remove(kType[which], kIndex[which]);
        ops->release(slot.transport);
        ops->note("removed", kType[which], kIndex[which], slot.role, why);
        slot = Slot{};
    }

    void put(unsigned which, enum xusb_port_role role, const char *why)
    {
        void *transport = ops->create(kType[which], role);
        if (!transport)
            return;
        if (!ops->add(transport, kType[which], kIndex[which])) {
            ops->release(transport);
            return;
        }
        live[which] = Slot{transport, role};
        ops->note("added", kType[which], kIndex[which], role, why);
    }
};

}  // namespace xport
