// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h): the family's endpoint methods (stall, abort, address, toggle, endpoint numbers,
// activation, buffers).
#include "SwitchXUDC.h"

namespace {
// USB transfer types as passed to activateEndpoint.
enum : int {
    kTypeIsochronous = 1,
    kTypeBulk = 2,
    kTypeInterrupt = 3
};

uint32_t log2Floor(uint32_t v)
{
    uint32_t n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
}
}  // namespace

IOReturn SwitchXUDC::handleStallEndpoint(int address, bool stall)
{
    const uint32_t dci = dciFor(address);
    if (dci >= kMaxDci)
        return kIOReturnBadArgument;
    if (!dci) {
        if (stall)
            ep0Cancel(kIOReturnAborted);
        hw.setHalt(0, stall);
    } else if (endpoints[dci].isoch) {
        switch_refusal("usb-device", "isochronous-halt",
                       "halt of isochronous endpoint %#x refused: isochronous "
                       "endpoints have no halt (USB 2.0 9.4.5)",
                       static_cast<unsigned>(address));
        return kIOReturnUnsupported;
    } else if (stall) {
        hw.setHalt(dci, true);
        drainSoon();
    } else {
        hw.resetSequence(dci);
        kick(dci);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::handleAbortEndpoint(int address)
{
    const uint32_t dci = dciFor(address);
    if (dci >= kMaxDci)
        return kIOReturnBadArgument;
    if (!dci) {
        ep0Cancel(kIOReturnAborted);
    } else {
        cancel(dci, kIOReturnAborted);
    }
    return kIOReturnSuccess;
}

// SET_ADDRESS takes effect before the status stage (hekate, Linux tegra_xudc_ep0_set_address).
IOReturn SwitchXUDC::handleSetAddressPreStatusPhase(int address)
{
    hw.setAddress(static_cast<uint32_t>(address) & 0x7fu);
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::resetEndpointDataToggle(int address)
{
    const uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci || endpoints[dci].isoch)
        return kIOReturnSuccess;
    hw.resetSequence(dci);
    kick(dci);
    return kIOReturnSuccess;
}

// AppleUSBXDCI::provideEndpointIDsForConfiguration: per function interface, each alternate setting numbers its pipes
// from the next free IN/OUT endpoint; the widest alternate setting decides how many numbers the interface uses.
IOReturn SwitchXUDC::provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids)
{
    if (!interfaces || !ids)
        return kIOReturnBadArgument;
    uint32_t nextIn = 1;
    uint32_t nextOut = 1;
    uint32_t written = 0;
    for (unsigned i = 0; i < interfaces->getCount(); ++i) {
        auto *interface = reinterpret_cast<IOUSBDeviceInterface *>(interfaces->getObject(i));
        if (!interface)
            continue;
        uint32_t widestIn = 0;
        uint32_t widestOut = 0;
        const int alternates = interface->getAlternateSettingCount();
        for (int alternate = 0; alternate < alternates; ++alternate) {
            uint32_t in = 0;
            uint32_t out = 0;
            const int pipes = interface->getPipeCountForAlternateSetting(static_cast<uint8_t>(alternate));
            for (int p = 0; p < pipes; ++p) {
                IOUSBDevicePipe *pipe = interface->getPipeForAlternateSetting(p, static_cast<uint8_t>(alternate));
                const int direction = pipe ? pipe->getDirection() : -1;
                if (direction == 1) {
                    if (nextIn + in > 15)
                        return kIOReturnNoResources;
                    ids[written++] = static_cast<uint8_t>(0x80 | (nextIn + in++));
                } else if (direction == 0) {
                    if (nextOut + out > 15)
                        return kIOReturnNoResources;
                    ids[written++] = static_cast<uint8_t>(nextOut + out++);
                }
            }
            if (in > widestIn)
                widestIn = in;
            if (out > widestOut)
                widestOut = out;
        }
        nextIn += widestIn;
        nextOut += widestOut;
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::activateEndpoint(int address, int type, int maxPacketSize, int interval, int)
{
    const uint32_t dci = dciFor(address);
    if (!dci)
        return kIOReturnSuccess;  // EP0 is always ours
    if (dci >= kMaxDci || maxPacketSize <= 0)
        return kIOReturnBadArgument;
    const bool in = (static_cast<uint32_t>(address) & 0x80u) != 0;
    const bool isoch = type == kTypeIsochronous;
    uint32_t kind = 0;
    uint32_t period = 0;
    const uint32_t packet = static_cast<uint32_t>(maxPacketSize) & 0x7ffu;
    const uint32_t burst =
        endpointBurst(speed, type, static_cast<uint32_t>(maxPacketSize), static_cast<uint32_t>(superSpeedBulkMaxBurst));
    if (type == kTypeBulk) {
        kind = in ? EP_BULK_IN : EP_BULK_OUT;
    } else if (type == kTypeInterrupt) {
        kind = in ? EP_INTR_IN : EP_INTR_OUT;
        const uint32_t value = interval < 1 ? 1 : static_cast<uint32_t>(interval);
        period = speed == kSpeedFull ? log2Floor(value * 8) : (value > 16 ? 16 : value) - 1;  // 2^n x 125 us
    } else if (isoch) {
        uint32_t others = 0;
        for (uint32_t d = 2; d < kMaxDci; ++d)
            others += static_cast<uint32_t>(d != dci && endpoints[d].active && endpoints[d].isoch);
        if (others >= kMaxIsochEndpoints) {
            switch_refusal("usb-device", "isochronous-endpoints",
                           "isochronous endpoint %#x refused: the XUDC runs at most %u at once",
                           static_cast<unsigned>(address), kMaxIsochEndpoints);
            return kIOReturnNoResources;
        }
        kind = in ? EP_ISOCH_IN : EP_ISOCH_OUT;
        period = static_cast<uint32_t>(interval);  // bInterval as Linux tegra-xudc sets it
        if (interval < 1) {
            period = 1;
        } else if (interval > 16) {
            period = 16;
        }
    } else {
        switch_refusal_k(
            switch_refusal_key_with(switch_refusal_key("usb-device", "endpoint-type"), static_cast<uint64_t>(type)),
            "usb-device", "endpoint-type", "endpoint %#x of type %u refused: not a data endpoint type",
            static_cast<unsigned>(address), static_cast<unsigned>(type));
        return kIOReturnUnsupported;
    }
    if (!packet)
        return kIOReturnBadArgument;
    Endpoint &ep = endpoints[dci];
    if (ep.queue && ep.tds && (ep.count || ep.tds->busy()))
        cancel(dci, kIOReturnAborted);  // activated without a deactivation
    if (!ep.queue)
        ep.queue = IONewZero(Request, kQueueDepth);
    if (!ep.tds)
        ep.tds = IONewZero(Tds, 1);  // zeroed: empty, unfenced
    if (isoch && !ep.iso)
        ep.iso = IONewZero(IsocTds, 1);
    if (!ep.queue || !ep.tds || (isoch && !ep.iso))
        return kIOReturnNoMemory;
    ep.head = ep.count = 0;
    ep.serialBusy = false;
    ep.in = in;
    ep.isoch = isoch;
    ep.mps = packet;
    if (isoch)
        nanoseconds_to_absolutetime(isocIntervalNs(speed == kSpeedFull, period), &ep.isocInterval);
    // Every other data endpoint that is not isochronous, interrupt endpoints too, is paused while an isochronous one
    // is set up, so that the controller has the resources for it (T210 TRM 22.9.5.4, "Enabling ISO Endpoints"; both
    // Linux tegra-xudc copies pause the bulk ones only).
    uint32_t paused = 0;
    for (uint32_t d = 2; isoch && d < kMaxDci; ++d) {
        if (d != dci && endpoints[d].active && !endpoints[d].isoch) {
            hw.setPause(d, true);
            paused |= 1u << d;
        }
    }
    const bool ready = hw.initEndpoint(dci, kind, packet, period, burst);
    const bool reloaded = ready && hw.enableEndpoint(dci);
    for (uint32_t d = 2; d < kMaxDci; ++d) {
        if (paused & (1u << d))
            hw.setPause(d, false);
    }
    if (!ready) {
        IOLog("SwitchUSB: endpoint %#x (DCI %u) not activated: no memory for its transfer ring\n", address, dci);
        return kIOReturnNoMemory;
    }
    if (!reloaded)
        return kIOReturnTimeout;  // Hardware::settled said which register
    ep.active = true;
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::deactivateEndpoint(int address)
{
    const uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci)
        return kIOReturnSuccess;
    endpoints[dci].active = false;
    cancel(dci, kIOReturnAborted);
    hw.disableEndpoint(dci);
    for (uint32_t d = 2; d < kMaxDci; ++d) {
        if (endpoints[d].active)
            return kIOReturnSuccess;
    }
    hw.stop();
    return kIOReturnSuccess;
}

// AppleUSBXDCI::createIOBuffer gives pageable memory its DART maps. Here the controller reads and writes the buffer
// itself (SwitchXUDCData.cpp): up to 64 KiB, wired and one physically contiguous run below 4 GiB, so a request on
// it is one TRB, never bounced, and prepare() has nothing to wire; larger (or should that fail) pageable as before, the
// planner taking its pages wherever they are (arm64 ignores a below-4-GiB mask without contiguity: KMA_LOMEM is x86's).
IOBufferMemoryDescriptor *SwitchXUDC::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    if (capacity && capacity <= kTrbMaxBytes) {
        const IOOptionBits wired =
            (options & ~static_cast<IOOptionBits>(kIOMemoryPageable)) | kIOMemoryPhysicallyContiguous;
        if (IOBufferMemoryDescriptor *b =
                IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, wired, capacity, 0x00000000fffff000ULL))
            return b;
    }
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options | kIOMemoryPageable, capacity,
                                                            ~static_cast<uint64_t>(PAGE_MASK));
}
