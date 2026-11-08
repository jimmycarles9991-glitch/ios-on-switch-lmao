// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01) XUSB device controller (XUDC): clocks, power, the USB2 PHY on OTG pad 0 (the USB-C port's D+/D-),
// event ring, endpoint contexts and transfer rings (TRB layout and ring sizes: XudcTransfer.h). USB2 high speed only.
// Used by SwitchXUDC.cpp, the backend of Apple's IOUSBDeviceController.
//
// Sequence: hekate bdk/usb/xusbd.c (xusb_device_init, _xusb_init_phy, _xusbd_init_device_clocks,
// xusb_device_enumerate); port reset, endpoint pause/reload/halt and the context bit layout: Linux
// drivers/usb/gadget/udc/tegra_xudc.c (switch-l4t-kernel-4.9). The caller serializes every call (one IOWorkLoop),
// except takeInterrupt(), which touches only ST and runs in the primary interrupt filter. Events are delivered by
// the XUDC interrupt (GIC SPI 44, INTID 76; CTRL.IE and the IPFS IP_INT mask are set by powerOn()).
//
// The controller does not snoop the A57 caches: every ring, context and data buffer is cleaned before the
// controller reads it and invalidated before the CPU reads what the controller wrote (switch_dcache_clean_invalidate).
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "XudcTransfer.h"
#include "XudcIsoch.h"

namespace xudc {

// Interrupt moderation (RT_IMOD, 250 ns units, Linux tegra_xudc): at least this long between two interrupts, so a burst
// of completions is one interrupt; a lone event is not delayed (the interval since the last one has long passed).
constexpr uint32_t kImodUs = 40;

// Event TRB as copied out of the ring.
struct Event {
    uint32_t d[4];

    uint32_t type() const
    {
        return (d[3] >> 10) & 0x3f;
    }

    uint32_t dci() const
    {
        return (d[3] >> 16) & 0x1f;
    }

    uint32_t code() const
    {
        return d[2] >> 24;
    }

    uint32_t residue() const
    {
        return d[2] & 0xffffff;
    }

    uint32_t trb() const
    {
        return d[0];
    }  // transfer events: TRB physical address

    uint16_t setupSequence() const
    {
        return d[2] & 0xffff;
    }
};

// Physically contiguous, cacheable DMA memory below 4 GiB.
// The XUSB partitions' module clocks and resets (Linux TEGRA210_CLK_*: bank * 32 + bit over banks L H U V W X Y).
constexpr unsigned kClockXusbHost = 2 * 32 + 25, kClockXusbDev = 2 * 32 + 31, kClockXusbSs = 4 * 32 + 28;

struct DmaBuffer {
    switch_dma_buffer buffer = {};
    uint8_t *va = nullptr;
    uint32_t pa = 0;
    uint32_t size = 0;
    bool allocate(uint32_t bytes);
    void release();
};

// UTMI pad 0 bring-up (PLLU, UTMIPLL, fuse calibration, bias tracking); fusePage is 0x7000F000. Also used by the host
// controller (XusbHost.cpp).
bool usb2PadInit(volatile uint8_t *car, volatile uint8_t *fusePage, volatile uint8_t *padctl);

// What the port did, as port-status events report it (PortChange::handle()).
struct PortChange {
    bool reset = false;         // bus reset ended: fresh EP0, address 0, configuration gone
    bool connected = false;     // a host drives the bus (CSC with CCS)
    bool disconnected = false;  // CSC without CCS
    bool suspended = false;     // link to U3 (host suspend, or the cable is gone)
    bool resumed = false;       // link back to U0
    bool fullSpeed = false;     // PORTSC.PS: the host runs us at full speed
};

class Hardware {
public:
    bool map();                                         // MMIO only, no access
    bool powerOn(char *why, size_t whySize);            // partitions, clocks, PHY, FPCI/IPFS, event ring, EP0
    void connect();                                     // VBUS/ID override, enable, port to RxDetect: pull-up on
    void disconnect();                                  // device mode and VBUS override off: the host sees a detach
    bool nextEvent(Event &event);                       // false when the ring is empty
    bool eventPending() const;                          // the next event is there (nextEvent would return it)
    void finishEvents();                                // ERDP update + EHB clear after a batch
    bool takeInterrupt();                               // ST.IP set? Then acknowledge it (W1C, IP only)
    bool interruptPending();                            // ST.IP, read only
    void describeInterrupt(char *buffer, size_t size);  // ST, CTRL, ERDP, EREP, PORTSC, IPFS mask: for a panic line
    PortChange portChange();                            // Linux __tegra_xudc_handle_port_status, every change acked

    // Endpoints (DCI = 2 * number + IN; EP0 is DCI 0). A data endpoint's first call allocates its kDataRingTrbs ring
    // (false: no DMA memory); maxBurst 0 below SuperSpeed.
    bool initEndpoint(uint32_t dci, uint32_t type, uint32_t maxPacket, uint32_t interval, uint32_t maxBurst = 0);
    void disableEndpoint(uint32_t dci);
    bool reloadEndpoint(uint32_t dci);
    void setHalt(uint32_t dci, bool halt);
    void setPause(uint32_t dci, bool pause);
    bool halted(uint32_t dci);
    void resetSequence(uint32_t dci);  // CLEAR_FEATURE(HALT): data toggle 0, ring position kept
    void dropQueued(uint32_t dci);     // discard every TRB not yet completed, keep the toggle
    uint32_t push(uint32_t dci, uint32_t bufferPA, uint32_t length, uint32_t flags);  // returns TRB PA
    // Data endpoints: one TRB at the enqueue position (buffer, length, TD size = TRBs after it in the TD, flags without
    // the cycle bit); returns its ring index. The ring's indices, the TRB at one (as written), the index of a TRB
    // address.
    uint32_t pushData(uint32_t dci, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags);

    uint32_t ringTrbs(uint32_t dci) const
    {
        return rings[dci].trbs;
    }

    uint32_t nextIndex(uint32_t dci, uint32_t index) const
    {
        return index + 1 == rings[dci].trbs - 1 ? 0 : index + 1;
    }

    uint32_t trbLength(uint32_t dci, uint32_t index) const
    {
        return rings[dci].trb[index * 4 + 2] & 0x1ffff;
    }

    uint64_t trbBuffer(uint32_t dci, uint32_t index) const
    {
        const size_t at = static_cast<size_t>(index) * 4;
        return rings[dci].trb[at] | static_cast<uint64_t>(rings[dci].trb[at + 1]) << 32;
    }

    bool trbIndex(uint32_t dci, uint32_t trbPA, uint32_t *index) const;

    bool ringReady(uint32_t dci) const
    {
        return rings[dci].trb != nullptr;
    }

    void doorbell(uint32_t dci, uint16_t sequence = 0);
    void setAddress(uint32_t address);
    void run();  // CTRL.RUN (first data endpoint)

private:
    struct Ring {
        volatile uint32_t *trb = nullptr;
        uint32_t pa = 0, enqueue = 0, cycle = 1, trbs = kRingTrbs;
        DmaBuffer own;  // a data endpoint's ring (EP0's is in `control`)
    };

    volatile uint8_t *car = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr, *xudc = nullptr;
    DmaBuffer control;  // contexts, EP0's transfer ring
    DmaBuffer events;   // the event ring's two segments of kEventSegmentTrbs
    Ring rings[kMaxDci];
    uint32_t eventIndex = 0, eventCycle = 1;

    uint32_t xr(uint32_t o)
    {
        return switch_mmio_read32(xudc, o);
    }

    void xw(uint32_t o, uint32_t v)
    {
        switch_mmio_write32(xudc, o, v);
    }

    static bool powerUp(unsigned partition, unsigned id);
    bool initPhy();
    void clearPortChange(uint32_t change);
    void resetEp0();
    volatile uint32_t *context(uint32_t dci) const;
    void resetRing(uint32_t dci);
    void toggleBit(uint32_t reg, uint32_t dci, bool set);
};

}  // namespace xudc

#include "XudcRequestMemory.h"
