// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01) XUSB device controller (XUDC): clocks, power, the USB2 PHY on OTG pad 0 (the USB-C port's D+/D-),
// event ring, endpoint contexts and transfer rings (TRB layout and ring sizes: XudcTransfer.h).
// Used by SwitchXUDC (SwitchXUDC.h), the backend of Apple's IOUSBDeviceController.
//
// Sequence: hekate bdk/usb/xusbd.c (xusb_device_init, _xusb_init_phy, _xusbd_init_device_clocks,
// xusb_device_enumerate); port reset, endpoint pause/reload/halt and the context bit layout: Linux
// drivers/usb/gadget/udc/tegra_xudc.c (switch-l4t-kernel-4.9). The caller serializes every call (one IOWorkLoop),
// except takeInterrupt(), which touches only ST and runs in the primary interrupt filter. Events are delivered by
// the XUDC interrupt (GIC SPI 44, INTID 76; CTRL.IE and the IPFS IP_INT mask are set by powerOn()).
//
// The controller does not snoop the A57 caches. The transfer rings, the event ring and the contexts are write-combined
// (Normal non-cacheable): no cache line stands between the CPU and the controller, a TRB's other words are ordered
// before the word whose cycle bit hands it over, an event's words are read after the word whose cycle bit shows it
// written, and a context's stores before the register write that has the controller read it. Data buffers, EP0's buffer
// and the bounce pool stay cacheable (the CPU copies through them) and are cleaned and invalidated around each transfer.
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "XudcTransfer.h"
#include "XudcIsoch.h"
#include "XudcLink.h"

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

// Physically contiguous DMA memory below 4 GiB: cacheable, or write-combined for what only the controller and plain
// CPU loads and stores share.
struct DmaBuffer {
    switch_dma_buffer buffer = {};
    uint8_t *va = nullptr;
    uint32_t pa = 0;
    uint32_t size = 0;
    bool allocate(uint32_t bytes, bool writeCombined = false);
    void release();
};

// What the port did, as port-status events report it (PortChange::handle()).
struct PortChange {
    bool reset = false;         // bus reset ended: fresh EP0, address 0, configuration gone
    bool connected = false;     // a host drives the bus (CSC with CCS)
    bool disconnected = false;  // CSC without CCS
    bool suspended = false;     // link to U3 (host suspend, or the cable is gone)
    bool resumed = false;       // link back to U0
    int speed = kSpeedNone;
};

class Hardware {
public:
    bool map();                               // MMIO only, no access
    bool powerOn(char *why, size_t whySize);  // partitions, clocks, PHY, FPCI/IPFS, event ring, EP0
    void connect();                           // ID floating, enable, port to RxDetect; VBUS is setVbus's
    void setVbus(bool present);               // the VBUS override (Tegra X1 TRM 22.16.7.164, p. 1572)
    void disconnect();                        // device mode and VBUS override off: the host sees a detach
    bool nextEvent(Event &event);             // false when the ring is empty
    bool eventPending() const;                // the next event is there (nextEvent would return it)
    void finishEvents();                      // ERDP update + EHB clear after a batch
    bool takeInterrupt();                     // ST.IP or ST.DSE set? Then acknowledge them (W1C)

    bool nextTransferEventReady(uint32_t blockedDcis) const;
    using SettleSleep = void (*)(void *context, uint32_t mask, uint64_t remainingNs);
    void setSettleWait(SettleSleep fn, void *context);

    struct SystemError {  // the first ST.DSE: the IPFS's interrupt code and signature
        bool seen;
        uint32_t code, signature;
    } systemError = {};

    PortChange portChange();  // Linux __tegra_xudc_handle_port_status, every change acked

    // Endpoints (DCI = 2 * number + IN; EP0 is DCI 0). A data endpoint's first call allocates its kDataRingTrbs ring
    // (false: no DMA memory); maxBurst 0 below SuperSpeed.
    bool initEndpoint(uint32_t dci, uint32_t type, uint32_t maxPacket, uint32_t interval, uint32_t maxBurst = 0);
    bool enableEndpoint(uint32_t dci);  // an endpoint initEndpoint set up into service, CTRL.RUN with the first
    void disableEndpoint(uint32_t dci);
    bool reloadEndpoint(uint32_t dci);
    void setHalt(uint32_t dci, bool halt);
    void haltAfterBabble(uint32_t dci);
    void setPause(uint32_t dci, bool pause);
    bool halted(uint32_t dci);
    void resetSequence(uint32_t dci);  // CLEAR_FEATURE(HALT): data toggle 0, ring position kept
    void dropQueued(uint32_t dci);     // discard every TRB not yet completed, keep the toggle
    uint32_t push(uint32_t dci, uint32_t bufferPA, uint32_t length, uint32_t flags);  // returns TRB PA
    // Data endpoints: one TRB at the enqueue position (buffer, length, TD size = TRBs after it in the TD, flags without
    // the cycle bit); returns its ring index. The ring's indices, the TRB at one (as written), the index of a TRB
    // address.
    uint32_t pushData(uint32_t dci, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags);

    // pushData in two steps: the slot at the enqueue position (the link TRB passed when the ring wraps, after a TRB of
    // these flags), and its TRB written later. The controller stops at the slot until it is written.
    struct Slot {
        uint32_t index, cycle;
    };

    Slot reserveData(uint32_t dci, uint32_t flags);
    void fillData(uint32_t dci, Slot slot, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags);

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
        return rings[dci].written[index].length;
    }

    uint64_t trbBuffer(uint32_t dci, uint32_t index) const
    {
        return rings[dci].written[index].buffer;
    }

    bool trbIndex(uint32_t dci, uint32_t trbPA, uint32_t *index) const;

    bool ringReady(uint32_t dci) const
    {
        return rings[dci].trb != nullptr;
    }

    void doorbell(uint32_t dci, uint16_t sequence = 0);
    void setAddress(uint32_t address);
    void run();   // CTRL.RUN (first data endpoint)
    void stop();  // CTRL.RUN off (last data endpoint gone)

private:
    // A data ring's TRBs as written, in cacheable memory: completions read them back, and a load from the
    // write-combined ring goes to DRAM.
    struct Written {
        uint64_t buffer;
        uint32_t length;
    };

    struct Ring {
        volatile uint32_t *trb = nullptr;
        uint32_t pa = 0, enqueue = 0, cycle = 1, trbs = kRingTrbs;
        DmaBuffer own;               // a data endpoint's ring (EP0's is in `control`)
        Written *written = nullptr;  // data endpoints only
    };

    volatile uint8_t *car = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr, *xudc = nullptr;
    DmaBuffer control;  // contexts, EP0's transfer ring
    DmaBuffer events;   // the event ring's two segments of kEventSegmentTrbs
    Ring rings[kMaxDci];
    uint32_t eventIndex = 0, eventCycle = 1;
    SettleSleep settleSleep = nullptr;
    void *settleContext = nullptr;

    uint32_t xr(uint32_t o)
    {
        return switch_mmio_read32(xudc, o);
    }

    void xw(uint32_t o, uint32_t v)
    {
        switch_mmio_write32(xudc, o, v);
    }

    static bool powerUp(unsigned partition, unsigned id, const struct switch_pmc_mc_client *client);
    void clearPortChange(uint32_t change);
    void resetEp0();
    volatile uint32_t *context(uint32_t dci) const;
    void resetRing(uint32_t dci);
    bool reload(uint32_t dci);
    bool changeState(uint32_t reg, uint32_t bits, uint32_t value);
    bool toggleBit(uint32_t reg, uint32_t dci, bool set);
    bool settled(uint32_t offset, uint32_t mask, uint32_t value);
};

}  // namespace xudc

#include "XudcRequestMemory.h"
