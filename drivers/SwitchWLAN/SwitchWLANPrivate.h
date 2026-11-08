// SPDX-License-Identifier: BSD-2-Clause-Patent
// Internals shared by the Switch Wi-Fi radio driver's units (TegraPCIe.cpp, SwitchWLANChip.cpp,
// SwitchWLANMsgbuf.cpp, SwitchWLAN.cpp). Ported from MyEcoria/nxk user/wlan (MIT OR Apache-2.0, verified on an
// Erista Switch) and checked against what it was itself ported from: Linux brcmfmac (pcie.c, chip.c, msgbuf.c,
// commonring.c) and CTCaer's switch-l4t-kernel-4.9 pci-tegra.c / xusb-tegra210.c / clk-tegra210b01.c for the
// T210B01 (Switch OLED) differences nxk never needed.
#ifndef SWITCH_WLAN_PRIVATE_H
#define SWITCH_WLAN_PRIVATE_H

#include <IOKit/IOLib.h>
#include <stddef.h>
#include <stdint.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

// Every device register access, for the host's msgbuf model (test/switch_wlan_msgbuf_test.cpp counts the PCIe reads
// a received or sent frame costs); nothing in the kernel build.
#ifndef SWLAN_MMIO_TRACE
#define SWLAN_MMIO_TRACE(mmio, kind, offset) ((void)0)
#endif

namespace swlan {

// A device register block mapped by ml_io_map (Device-nGnRE). 32-bit accesses only, except the 16-bit ring indices
// and the byte reads of the firmware console in the chip's RAM (PCIe carries byte enables, Linux brcmfmac does the
// same with ioread16/ioread8).
struct Mmio {
    volatile uint8_t *base = nullptr;

    bool map(uint64_t pa, size_t length)
    {
        base = switch_mmio_map(pa, length);
        return base != nullptr;
    }

    uint32_t r32(uint32_t offset) const
    {
        SWLAN_MMIO_TRACE(this, 'r', offset);
        return switch_mmio_read32(base, offset);
    }

    void w32(uint32_t offset, uint32_t value) const
    {
        SWLAN_MMIO_TRACE(this, 'w', offset);
        switch_mmio_write32(base, offset, value);
    }

    uint16_t r16(uint32_t offset) const
    {
        SWLAN_MMIO_TRACE(this, 'r', offset);
        return switch_mmio_read16(base, offset);
    }

    void w16(uint32_t offset, uint16_t value) const
    {
        SWLAN_MMIO_TRACE(this, 'w', offset);
        switch_mmio_write16(base, offset, value);
    }

    uint8_t r8(uint32_t offset) const
    {
        return switch_mmio_read8(base, offset);
    }

    void modify(uint32_t offset, uint32_t clear, uint32_t set) const
    {
        w32(offset, (r32(offset) & ~clear) | set);
    }

    // Bounded wait for a status bit that has no interrupt (PLL locks, calibration, power partitions, link
    // training): true once (r32 & mask) == value, within `us`.
    bool wait(uint32_t offset, uint32_t mask, uint32_t value, uint32_t us) const
    {
        return switch_mmio_wait32(base, offset, mask, value, us);
    }

    // The same for waits measured in milliseconds (link training), sleeping 1 ms between looks as Linux does.
    bool waitMs(uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms) const
    {
        for (uint32_t waited = 0;; ++waited) {
            if ((r32(offset) & mask) == value)
                return true;
            if (waited >= ms)
                return false;
            IOSleep(1);
        }
    }
};

uint64_t nowNs();

// DMA memory shared with the chip goes through kits/SwitchDMA's cache maintenance: the AFI is not coherent with the
// CPU caches (Linux uses dma_alloc_coherent's non-cacheable remap; nxk an uncached mapping), so every line the chip
// reads is cleaned after the CPU writes it, and every line it writes is invalidated before the CPU reads it. Lines
// the chip writes are never dirty (SwitchWLANMsgbuf.cpp), so clean+invalidate costs what an invalidate would, and
// it also runs where the host tests build this code at EL0.

// Little-endian fields of firmware messages, any alignment (the build is -mstrict-align).
inline uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

inline uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// --- TegraPCIe.cpp: the T210B01 PCIe root complex, root port 1 (x1, UPHY lane 0), the BCM4356 behind it ---------
class TegraPcie {
public:
    struct Endpoint {
        uint16_t vendor, device;
        uint32_t bar0, bar2;  // physical addresses given to the chip's BAR0 (32 KiB registers) and BAR2 (4 MiB RAM)
        uint8_t linkSpeed, linkWidth;
    };

    // Rail, clocks, power partition, PLLE, UPHY, AFI, port 1, link, bus numbers and BARs. On failure `why` holds
    // one line naming the stage and the registers that decided it, and nothing below the root port is touched.
    bool bringUp(Endpoint &ep, char *why, size_t whyLength);
    // The chip from cold again after it was lost (nxk pcie.rs `restart`): PERST# asserted, WL_REG_ON cycled, PERST#
    // released, link trained, endpoint enumerated (to the same BARs). Rail, clocks, UPHY and AFI keep their setup.
    bool restart(Endpoint &ep, char *why, size_t whyLength);
    // The endpoint still decodes memory: a chip that reset itself comes back with its command register cleared.
    bool endpointDecodes();

    // Configuration space of the endpoint (bus 1, device 0) through the 4 KiB AFI window.
    uint32_t epConfigRead(uint32_t offset);
    // Read back and rewritten once if it did not take (brcmf_pcie_select_core: a posted BAR access may overtake
    // this non-posted one). Used for the BAR0 backplane window, which every chip register access moves.
    void epConfigWrite(uint32_t offset, uint32_t value);

    // MSI: the AFI decodes the chip's MSI writes to the page at msiPA into AFI_MSI_VEC0..7 and raises GIC SPI 99
    // (INTID 131); the chip's MSI capability is pointed at vector 0 and its INTx turned off. False (and `why`) when
    // the chip has no MSI capability.
    bool msiEnable(uint32_t msiPA, char *why, size_t whyLength);
    uint32_t msiTake();  // AFI_MSI_VEC0, write-1-to-clear what was read
    uint32_t msiPending() const;

    // One line for a report: AFI error latch, link state of port 1, endpoint command/status.
    void describe(char *out, size_t length);

private:
    Mmio car_, pmc_, gpio_, pinmux_, mc_, padctl_, pcie_, cs_, mselect_;
    bool mapped_ = false;
    uint32_t cfgPage_ = ~0u;

    bool map();
    static bool rail(char *why, size_t whyLength);
    bool powerUp(char *why, size_t whyLength);
    bool plle(char *why, size_t whyLength);
    bool uphyManagementClock(char *why, size_t whyLength);
    bool uphy(char *why, size_t whyLength);
    void pads();
    void moduleReset();
    void controller();
    void portEnable();
    void portReset();
    bool portLink();
    bool enumerate(Endpoint &ep, char *why, size_t whyLength);
    uint32_t rpRead(uint32_t offset) const;
    void rpWrite(uint32_t offset, uint32_t value) const;
    void rpModify(uint32_t offset, uint32_t clear, uint32_t set) const;
    void selectConfigPage(uint32_t address);
    void linkReport(char *out, size_t length);
};

// --- SwitchWLANChip.cpp: the BCM4356 behind the link (backplane, cores, firmware download) ---------------------
struct Core {
    uint16_t id;
    uint8_t rev;
    uint32_t base, wrap;
};

class Chip {
public:
    // BAR0 is a 4 KiB view of the chip's backplane; BAR2 its RAM (TCM) from backplane address 0.
    bool attach(TegraPcie *pcie, uint32_t bar0PA, uint32_t bar2PA, char *why, size_t whyLength);

    uint32_t read(uint32_t address);  // any backplane address, through the BAR0 window
    void write(uint32_t address, uint32_t value);

    const Mmio &tcm() const
    {
        return tcm_;
    }

    uint16_t id() const
    {
        return id_;
    }

    uint8_t rev() const
    {
        return rev_;
    }

    unsigned coreCount() const
    {
        return coreCount_;
    }

    const Core *find(uint16_t coreId) const;

    uint32_t ramBase() const
    {
        return ramBase_;
    }

    uint32_t ramSize() const
    {
        return ramSize_;
    }

    // brcmf_pcie_download_fw_nvram: ARM halted, image and NVRAM into RAM, ARM started, then the firmware's shared
    // area address awaited (it has no interrupt for that: Linux and nxk poll every 50 ms for up to 5 s).
    bool boot(const uint8_t *image, size_t imageLength, const uint8_t *nvram, size_t nvramLength, char *why,
              size_t whyLength);

    uint32_t shared() const
    {
        return shared_;
    }

    uint32_t sharedFlags() const
    {
        return sharedFlags_;
    }

    // The shared area's flags as the firmware has them now: all ones when the chip is gone from the bus,
    // kSharedDied bits when it asserted or trapped.
    uint32_t liveFlags() const
    {
        return tcm_.r32(shared_);
    }

    static constexpr uint32_t kSharedDied = 0x200 | 0x400;  // PCIE_SHARED_ASSERT | PCIE_SHARED_TRAP

    // What the firmware left when it died (assert expression/file/line or ARM trap registers), and the tail of
    // its console ring, each as one line.
    void crash(char *out, size_t length);
    void consoleTail(char *out, size_t length);

private:
    TegraPcie *pcie_ = nullptr;
    Mmio bar0_, tcm_;
    uint32_t window_ = ~0u;
    uint16_t id_ = 0;
    uint8_t rev_ = 0;
    Core cores_[24] = {};
    unsigned coreCount_ = 0;
    uint32_t ramBase_ = 0, ramSize_ = 0, shared_ = 0, sharedFlags_ = 0, console_ = 0;

    uint32_t erom(uint32_t &at, uint32_t &type);
    bool regaddr(uint32_t &at, uint32_t &base, uint32_t &wrap);
    void scanCores();
    void coreDisable(const Core &core, uint32_t prereset, uint32_t reset);
    void coreReset(const Core &core, uint32_t prereset, uint32_t reset, uint32_t postreset);
    void tcmCopy(uint32_t address, const uint8_t *data, size_t length);
};

// nxk fw.rs nvram() = brcmf_fw_nvram_strip: key=value pairs NUL-terminated, padded to a word, then the length
// token. Returns the converted length (0 if `out` is too small).
size_t nvramConvert(const uint8_t *text, size_t textLength, uint8_t *out, size_t outLength);

// --- SwitchWLANMsgbuf.cpp: Broadcom msgbuf rings over PCIe --------------------------------------------------------
struct Event {
    uint32_t type, status, reason, flags;
    uint8_t addr[6];
    uint8_t ifidx, bsscfgidx;  // wl_event_msg_t: the interface and bsscfg the event is about
    const uint8_t *data;
    uint32_t length;
};

// How the DMA area is cut up (Msgbuf::attach): its length and the receive and transmit buffers it holds. The large
// layout is the data path's (brcmfmac posts the firmware's max_rxbufpost, 255, and keeps up to 2048 frames in flight);
// the small one is the layout every boot before r17 ran, taken when 2 MiB contiguous below 4 GiB is not available.
struct AreaLayout {
    uint32_t length;
    uint16_t rxBuffers, txBuffers;
};

constexpr AreaLayout kAreaLarge = {2u * 1024 * 1024, 256, 256};
constexpr AreaLayout kAreaSmall = {512u * 1024, 64, 32};
constexpr uint32_t kTxBufferSize = 2048, kRxBufferSize = 2048;  // BRCMF_MSGBUF_MAX_PKT_SIZE

// The data path's counters (monotonic, except the peak): what one stats window moved and what it cost.
struct DataCounters {
    uint64_t rxFrames, rxBytes, rxReposted;  // completions taken; their frame bytes; buffers given back to the chip
    uint64_t txFrames, txBytes;              // frames posted and their bytes
    uint64_t txDone, txFailed;               // their completions; completions with a status
    uint64_t txRefused, txFull;              // sends refused (ring closed, too large, no room); txRoom() that found 0
    uint64_t doorbells, indexReads, drains;  // H2D doorbells rung; ring indices read from the chip's RAM; service()s
    uint32_t txInFlight, txPeak;             // frames the chip holds now; the most it held since the last snapshot
    uint32_t rxPosted;                       // receive buffers the chip holds now
    uint64_t txReread, txRereadDiffer;       // txReadBack: buffers read back from memory; those that differed
    uint32_t txRereadAt;                     // the first byte that differed, in the last one that did
};

class Msgbuf {
public:
    using EventHandler = void (*)(void *context, const Event &event);
    // Ethernet header + payload, and the firmware interface it came in on (0: the station)
    // `kind`: the rx completion's frame type (BCM_MSGBUF_RX_FRAME_*: 802.3 for the station, 802.11 in monitor mode).
    using FrameHandler = void (*)(void *context, const uint8_t *frame, size_t length, uint8_t ifidx, uint8_t kind);

    enum Flow {
        FLOW_CLOSED,
        FLOW_PENDING,
        FLOW_OPEN,
        FLOW_REFUSED,
        FLOW_DELETING
    };

    // Rings, scratch and ring-update buffers, control, receive and transmit buffers and the TX flow ring, all in one
    // contiguous DMA area below 4 GiB (the AFI's FPCI window is 32-bit and nxk keeps DMA32), told to the firmware
    // through its shared area. Also the MSI page (msiPA()). The area is allocated once and set up afresh on every
    // call, so a chip brought back after a reset (held in reset meanwhile: nothing DMAs into the old rings) reuses
    // it. Not yet interrupt-driven: call enableInterrupts() once the MSI source is ready.
    bool attach(Chip *chip, EventHandler events, FrameHandler frames, void *context, char *why, size_t whyLength);

    uint32_t msiPA() const
    {
        return msiPA_;
    }

    void enableInterrupts();
    void disableInterrupts();
    // One line for the bring-up log: the layout taken and what the firmware asked for.
    void describe(char *out, size_t length) const;

    // Work loop, on the MSI: mailbox status taken (mask dropped meanwhile, as brcmf_pcie_isr_thread, so an event
    // during the drain makes a fresh MSI), mailbox data answered, completion rings drained. Returns the mailbox
    // status that was seen; *halted set when the firmware reported a halt.
    uint32_t service(bool *halted);
    // Chip-side interrupt state for the lost-interrupt check: (MAILBOXINT & MAILBOXMASK) != 0 means the chip is
    // signalling; *raw gets the two registers for the panic line.
    bool chipSignalling(uint32_t *mailboxInt, uint32_t *mailboxMask);

    // Post one ioctl (brcmf_msgbuf_query_dcmd) for firmware interface `ifidx` (msgbuf_common_hdr byte 1: 0 the
    // station, the AWDL interface's index for its awdl_* iovars, as brcmf_fil_iovar_data_set on the AWDL ifp); the
    // answer arrives through service(): replyReady() then reply().
    bool ioctlSubmit(uint32_t cmd, uint8_t ifidx, const uint8_t *data, size_t length, size_t outLength, char *why,
                     size_t whyLength);

    bool replyReady() const
    {
        return replyReady_;
    }

    int16_t replyStatus() const
    {
        return replyStatus_;
    }

    const uint8_t *replyData() const
    {
        return reply_;
    }

    size_t replyLength() const
    {
        return replyLength_;
    }

    // The station's TX flow ring (brcmf_msgbuf_flowring_create_worker): one, for interface 0, priority 0, created
    // once per firmware run; the answer arrives through service() (flow() leaves FLOW_PENDING).
    bool flowCreateSubmit(const uint8_t destination[6], const uint8_t source[6]);

    Flow flow() const
    {
        return flow_;
    }

    uint16_t flowStatus() const
    {
        return flowStatus_;
    }

    // One Ethernet frame onto the flow ring at once (the supplicant's handshake frames): copied into a transmit
    // buffer, its header in the ring item. The request id its completion answers for, 0 when it cannot go now (flow
    // ring not open, every buffer in flight, ring full).
    uint32_t send(const uint8_t *frame, size_t length);
    bool sent(uint32_t id, uint16_t *status) const;

    // en0's frames (brcmf_msgbuf_txflow): txRoom() frames fit now; for each, the caller writes the frame into
    // txBuffer() (its one copy on the way out, straight from the mbuf) and txPost()s it; txFlush() then tells the chip
    // with one index write and one doorbell for the batch. A txRoom() of 0 is remembered: once completions have freed
    // enough buffers, txResume() turns true (once), and the caller starts its queue again (no frame is dropped here
    // for want of room: it waits in the stack's queue).
    unsigned txRoom();
    uint8_t *txBuffer();
    uint32_t txPost(size_t length);
    void txFlush();
    bool txResume();
    // After txFlush: the first `length` bytes of posted buffer `id` as the chip reads them (out of the cache, from
    // memory) against the `written` copy the CPU wrote, counted in DataCounters. Same gate as the post, so no
    // completion can recycle the buffer in between.
    void txReadBack(uint32_t id, const uint8_t *written, size_t length);

    // The AWDL interface's flow rings (the firmware's own AWDL, SwitchWLANAwdl.cpp): one per destination, group
    // addresses sharing the broadcast one, created on first use and deleted when the interface goes. Their memory is
    // at the end of the DMA area, after everything the station uses, so the station's layout never changes.
    // awdlSend: 1 posted, 0 kept until its flow ring is open (one frame per ring), -1 refused.
    static constexpr unsigned kAwdlFlows = 4;
    int awdlSend(uint8_t ifidx, const uint8_t *frame, size_t length);
    void awdlClose();  // flow rings deleted (or forgotten when the chip is gone)

    bool awdlAvailable() const
    {
        return awdlRings_ > 0;
    }

    unsigned awdlRingCount() const
    {
        return awdlRings_;
    }

    uint32_t awdlRefused() const
    {
        return awdlRefused_;
    }

    uint32_t framesReceived() const
    {
        return static_cast<uint32_t>(data_.rxFrames);
    }

    uint32_t framesSent() const
    {
        return static_cast<uint32_t>(data_.txFrames);
    }

    uint32_t sendsCompleted() const
    {
        return static_cast<uint32_t>(data_.txDone);
    }

    uint32_t sendsRefused() const
    {
        return static_cast<uint32_t>(data_.txRefused);
    }

    // Completions whose status was not 0 (the firmware did not get the frame out), and the last such status.
    uint32_t sendsFailed() const
    {
        return static_cast<uint32_t>(data_.txFailed);
    }

    uint16_t lastFailStatus() const
    {
        return txFailStatus_;
    }

    // The data path's counters; the peak of frames in flight starts again from those in flight now.
    DataCounters takeCounters();

    unsigned rxBufferCount() const
    {
        return rxCount_;
    }

    unsigned txBufferCount() const
    {
        return txCount_;
    }

private:
    struct Ring {
        uint8_t *va;
        uint32_t pa;
        uint16_t items, size;
        uint32_t wIndex, rIndex;  // TCM addresses of the write and read indices
        uint16_t pos;             // next item to write (H2D) or read (D2H)
        // H2D: the chip's read index as last read from its RAM (read again only when the ring looks full) and the
        // write index it was last told. D2H: the chip's write index as read at the interrupt.
        uint16_t seen, told;
        uint64_t consumed, cleared;  // D2H only: bytes read and bytes whose cache lines were zeroed (monotonic)
    };

    struct Buffer {
        uint8_t *va;
        uint32_t pa;
    };

    Chip *chip_ = nullptr;
    EventHandler events_ = nullptr;
    FrameHandler frames_ = nullptr;
    void *context_ = nullptr;
    uint8_t *area_ = nullptr;
    uint32_t areaPA_ = 0, areaUsed_ = 0, msiPA_ = 0;
    AreaLayout layout_ = {};
    void *memory_ = nullptr;           // IOBufferMemoryDescriptor, kept for the driver's lifetime
    Ring rings_[6 + kAwdlFlows] = {};  // the five common rings, the TX flow ring, then the AWDL flow rings

    struct AwdlFlow {
        uint8_t da[6];
        uint8_t ifidx;
        Flow state;
        uint64_t lastUse;
        Buffer pending;  // one frame held while its ring is being created
        uint16_t pendingLength;
    };

    AwdlFlow awdl_[kAwdlFlows] = {};
    unsigned awdlRings_ = 0;  // how many AWDL flow rings the area and the firmware allow
    uint64_t awdlUse_ = 0;
    uint32_t awdlRefused_ = 0;
    uint16_t maxFlowRings_ = 0, maxRxPost_ = 0;
    Buffer ioctlBuffer_ = {};
    Buffer control_[16] = {};
    // Receive buffers rxCount_ of kRxBufferSize from rxBase_, transmit buffers txCount_ of kTxBufferSize from txBase_
    // (area offsets; id n is buffer n - 1). Free transmit ids wait in a FIFO, so an id the supplicant waits on
    // (sent()) is not reused at once; each id's state and last status live in the same allocation (txSlots_).
    uint32_t rxBase_ = 0, txBase_ = 0;
    unsigned rxCount_ = 0, txCount_ = 0;
    uint16_t *txFree_ = nullptr;  // FIFO of free ids
    uint16_t *txStatus_ = nullptr;
    uint8_t *txBusy_ = nullptr;
    void *txSlots_ = nullptr;
    unsigned txFreeHead_ = 0, txFreeCount_ = 0;
    bool txWanted_ = false;
    DataCounters data_ = {};
    uint32_t pcie2_ = 0, dataOffset_ = 0, dtohMailbox_ = 0;
    uint16_t txFailStatus_ = 0;
    uint16_t transId_ = 0, flowStatus_ = 0;
    Flow flow_ = FLOW_CLOSED;
    bool replyReady_ = false;
    int16_t replyStatus_ = 0;
    uint8_t reply_[1536] = {};
    size_t replyLength_ = 0;

    bool allocateArea(char *why, size_t whyLength);
    bool allocate(uint32_t length, uint32_t align, Buffer &out);

    uint8_t *txVa(uint32_t id) const
    {
        return area_ + txBase_ + static_cast<size_t>(id - 1) * kTxBufferSize;
    }

    uint32_t txPa(uint32_t id) const
    {
        return areaPA_ + txBase_ + (id - 1) * kTxBufferSize;
    }

    uint8_t *rxVa(uint32_t id) const
    {
        return area_ + rxBase_ + static_cast<size_t>(id) * kRxBufferSize;
    }

    uint32_t rxPa(uint32_t id) const
    {
        return areaPA_ + rxBase_ + id * kRxBufferSize;
    }

    uint16_t readIndex(uint32_t tcmAddress);
    unsigned space(unsigned ring);
    uint8_t *reserve(unsigned ring);
    void advance(unsigned ring);
    void publish(unsigned ring);
    void commit(unsigned ring);
    bool postControl(uint32_t id);
    bool postRx(uint32_t id);
    void refresh(unsigned ring);
    bool next(unsigned ring, uint8_t *item);
    uint32_t postFrame(unsigned ring, uint8_t ifidx, size_t length);
    uint32_t post(unsigned ring, uint8_t ifidx, const uint8_t *frame, size_t length);
    void txRelease(uint32_t id, uint16_t status);
    void retire(unsigned ring);
    void drain();
};

}  // namespace swlan

#endif
