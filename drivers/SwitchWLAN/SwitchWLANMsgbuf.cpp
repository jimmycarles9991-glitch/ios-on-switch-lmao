// SPDX-License-Identifier: BSD-2-Clause-Patent
// Broadcom msgbuf protocol over PCIe for the BCM4356A3's FullMAC firmware: rings in host memory, their indices in
// the chip's RAM, doorbells through the PCIe2 core's mailbox, completions announced by MSI.
//
// Ported from MyEcoria/nxk user/wlan/src/msgbuf.rs (MIT OR Apache-2.0), itself Linux brcmfmac pcie.c,
// commonring.c and msgbuf.c; message layouts checked against CTCaer's switch-l4t-kernel-4.9 brcmfmac (shared area
// v5, the pre-v7 completion item sizes): control path (ioctls, firmware events), received frames, and one TX flow
// ring for interface 0 (a station needs no more: brcmfmac's STA mode keys flow rings on the interface alone).
// The data path's batching follows brcmfmac (ISC; Copyright (c) 2014 Broadcom Corporation): commonring.c's cached
// read pointer (brcmf_commonring_reserve_for_write reads the chip's index only when the ring looks full) and one
// write-index update and doorbell per batch (brcmf_commonring_write_complete), msgbuf.c's receive buffers reposted
// together after a completion pass (brcmf_msgbuf_rxbuf_data_fill) and its TX flush per batch
// (brcmf_msgbuf_txflow).
//
// Memory: the AFI is not cache coherent, and IOBufferMemoryDescriptor hands out a cacheable kernel mapping, so
// every buffer is cleaned/invalidated by hand (SwitchWLANPrivate.h). Completion rings need one more rule. nxk
// notes the write index in RAM can be seen before the item it announces has landed in host memory, and zeroes each
// item it consumed so a zero message type means "not landed yet" (no message type is 0). Zeroing an item with the
// CPU dirties a whole 64-byte cache line, which holds several 16/24/32-byte items; evicting that line later would
// overwrite items the chip wrote meanwhile. So lines are zeroed (and cleaned out of the cache) only once every
// item in them has been consumed, and the read index the chip sees advances only past zeroed lines: the chip never
// writes into a line the CPU may still hold dirty, and every slot it writes was zero before.
//
// Receive buffers are never written by the CPU after attach (the area is cleaned out of the cache then, and frames
// are only read, after their lines are invalidated), so a buffer goes back to the chip without cache maintenance.
// Transmit buffers are written once, by the caller copying the frame in, and cleaned before the batch is announced.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "SwitchWLANPrivate.h"
#include "../../kits/SwitchRing/SwitchRing.h"

namespace swlan {
namespace {

// Common rings (brcmf_ring_max_item / brcmf_ring_itemsize, shared v5): two host-to-dongle, three back.
enum : unsigned {
    CONTROL_SUBMIT,
    RXPOST_SUBMIT,
    CONTROL_COMPLETE,
    TX_COMPLETE,
    RX_COMPLETE,
    RING_COUNT
};

constexpr unsigned FLOW = RING_COUNT;  // the flow rings' slot after the common rings

constexpr uint16_t kRingItems[RING_COUNT] = {64, 512, 64, 1024, 512};
constexpr uint16_t kRingItemSize[RING_COUNT] = {40, 32, 24, 16, 32};

// Shared area fields (BRCMF_SHARED_*) and the ring info block (BRCMF_SHARED_RING_*).
enum : uint32_t {
    SHARED_MAX_RXBUFPOST = 34,
    SHARED_RX_DATAOFFSET = 36,
    SHARED_DTOH_MB_DATA = 44,
    SHARED_RING_INFO = 48,
    SHARED_SCRATCH_LEN = 52,
    SHARED_SCRATCH_ADDR = 56,
    SHARED_RINGUPD_LEN = 64,
    SHARED_RINGUPD_ADDR = 68,
};

enum : uint32_t {
    RINGINFO_MEM = 0,
    RINGINFO_H2D_W = 4,
    RINGINFO_H2D_R = 8,
    RINGINFO_D2H_W = 12,
    RINGINFO_D2H_R = 16
};

constexpr uint32_t kRingMemSize = 16, kRingMaxItem = 4, kRingLenItems = 6, kRingBaseAddr = 8;
constexpr uint32_t kSharedHostReadyDb1 = 0x10000000;

// PCIe2 core mailbox (core rev < 64).
constexpr uint32_t kMailboxInt = 0x48, kMailboxMask = 0x4C, kH2DMailbox0 = 0x140, kH2DMailbox1 = 0x144;
constexpr uint32_t kMbFn0 = 0x0300, kMbD2HDoorbells = 0x00FF0000;
constexpr uint32_t kD2HFwHalt = 0x10000000;

// Message types (brcmfmac msgbuf.c MSGBUF_TYPE_*).
enum : uint8_t {
    MSG_FLOW_RING_CREATE = 0x03,
    MSG_FLOW_RING_CREATE_CMPLT = 0x04,
    MSG_FLOW_RING_DELETE = 0x05,
    MSG_FLOW_RING_DELETE_CMPLT = 0x06,
    MSG_IOCTLPTR_REQ = 0x09,
    MSG_IOCTLPTR_REQ_ACK = 0x0A,
    MSG_IOCTLRESP_BUF_POST = 0x0B,
    MSG_IOCTL_CMPLT = 0x0C,
    MSG_EVENT_BUF_POST = 0x0D,
    MSG_WL_EVENT = 0x0E,
    MSG_TX_POST = 0x0F,
    MSG_TX_STATUS = 0x10,
    MSG_RXBUF_POST = 0x11,
    MSG_RX_CMPLT = 0x12,
};

constexpr uint32_t kIoctlReqPktId = 0xFFFE;
constexpr uint32_t kControlBuffers = 8, kControlBufferSize = 8192;  // ioctl responses 0..7, events 8..15
constexpr uint32_t kIoctlMax = 1518;                                // BRCMF_TX_IOCTL_MAX_MSG_SIZE
constexpr unsigned kRxFlags = 18, kRxFrameMask = 0x07;  // msgbuf_rx_complete flags: BRCMF_MSGBUF_PKT_FLAGS_FRAME_*
// brcmf_msgbuf's BRCMF_DEF_MAX_RXBUFPOST: what the chip takes when its shared area leaves max_rxbufpost 0.
constexpr uint16_t kDefaultMaxRxPost = 255;
// The TX flow ring: its id follows the two common H2D rings (BRCMF_H2D_MSGRING_FLOWRING_IDSTART), its indices
// follow theirs in RAM; 512 items of the pre-v7 48-byte TX post (BRCMF_H2D_TXFLOWRING_MAX_ITEM), more than there are
// transmit buffers, so a frame that has a buffer always has a slot.
constexpr uint16_t kFlowRingId = 2, kFlowItems = 512, kFlowItemSize = 48;
// The AWDL interface's flow rings follow the station's (ids 3..), smaller: AirDrop's traffic is a few peers.
constexpr uint16_t kAwdlFlowItems = 128;
constexpr uint32_t kRingInfoMaxFlowRings = 52;  // brcmf_pcie_dhi_ringinfo.max_flowrings (v5: all submission rings)
constexpr uint32_t kEthHeader = 14;
constexpr uint8_t kPktFrame8023 = 0x01;  // BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_3, priority 0
constexpr uint32_t kScratchLength = 8, kRingUpdLength = 1024;
constexpr uint32_t kLandingSpinUs = 10000;
// Completions taken from one ring before the others get their turn (a busy receive direction cannot hold back the
// transmit completions that free buffers for the other direction's ACKs).
constexpr unsigned kDrainBudget = 64;
// en0's queue is started again once this many transmit buffers are free: a batch worth a thread wakeup.
constexpr unsigned kTxResumeFree = 32;

constexpr AreaLayout kLayouts[] = {kAreaLarge, kAreaSmall};
// The station's flow ring holds at most the frames in flight, so with fewer transmit buffers than ring slots a free
// buffer always has a slot: txRoom() is the free buffers (the ring's read index is read only when a post finds it
// full by the index last seen).
static_assert(kAreaLarge.txBuffers < kFlowItems && kAreaSmall.txBuffers < kFlowItems, "flow ring under the buffers");

// Firmware event frames (brcmf_fweh_process_skb): Ethernet header, 10-byte Broadcom header, the big-endian event
// message, then its data.
constexpr uint16_t kEthPLinkCtl = 0x886C;
constexpr uint32_t kEventMsg = 14 + 10, kEventData = kEventMsg + 48;

}  // namespace

// The area, once per driver lifetime: the large layout, else the small one (one line); and the transmit ids'
// bookkeeping, sized for the large layout.
bool Msgbuf::allocateArea(char *why, size_t whyLength)
{
    uint64_t pa = 0;
    IOByteCount segment = 0;
    for (const AreaLayout &layout : kLayouts) {
        auto *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
                static_cast<IOOptionBits>(kIOMemoryMapperNone),
            layout.length, 0x00000000FFFFF000ULL);
        segment = 0;
        pa = memory ? memory->getPhysicalSegment(0, &segment, kIOMemoryMapperNone) : 0;
        if (memory && pa && segment >= layout.length && pa + layout.length <= (1ULL << 32)) {
            memory_ = memory;
            area_ = static_cast<uint8_t *>(memory->getBytesNoCopy());
            areaPA_ = static_cast<uint32_t>(pa);
            layout_ = layout;
            break;
        }
        OSSafeReleaseNULL(memory);
        if (layout.length == kAreaLarge.length) {
            IOLog("SwitchWLAN: msgbuf: %u KiB contiguous below 4 GiB not available (pa %#llx, run %llu); taking %u KiB "
                  "with %u receive and %u transmit buffers\n",
                  kAreaLarge.length / 1024, pa, static_cast<unsigned long long>(segment), kAreaSmall.length / 1024,
                  kAreaSmall.rxBuffers, kAreaSmall.txBuffers);
        }
    }
    if (!memory_) {
        snprintf(why, whyLength, "DMA area: %u bytes contiguous below 4 GiB not available (pa %#llx, run %llu)",
                 kAreaSmall.length, pa, static_cast<unsigned long long>(segment));
        return false;
    }
    constexpr size_t kSlots = kAreaLarge.txBuffers;
    txSlots_ = IOMallocZeroData(kSlots * (2 * sizeof(uint16_t) + 1));
    if (!txSlots_) {
        snprintf(why, whyLength, "no memory for the transmit ids");
        return false;
    }
    txFree_ = static_cast<uint16_t *>(txSlots_);
    txStatus_ = txFree_ + kSlots;
    txBusy_ = reinterpret_cast<uint8_t *>(txStatus_ + kSlots);
    return true;
}

bool Msgbuf::allocate(uint32_t length, uint32_t align, Buffer &out)
{
    const uint32_t at = (areaUsed_ + align - 1) & ~(align - 1);
    if (at + length > layout_.length)
        return false;
    areaUsed_ = at + length;
    out.va = area_ + at;
    out.pa = areaPA_ + at;
    return true;
}

bool Msgbuf::attach(Chip *chip, EventHandler events, FrameHandler frames, void *context, char *why, size_t whyLength)
{
    chip_ = chip;
    events_ = events;
    frames_ = frames;
    context_ = context;
    const Mmio &tcm = chip->tcm();
    const uint32_t shared = chip->shared();
    const Core *pcie2 = chip->find(0x83C);
    pcie2_ = pcie2 ? pcie2->base : 0;

    // One physically contiguous area below 4 GiB, zeroed and out of the caches before the chip learns of it.
    if (!memory_ && !allocateArea(why, whyLength))
        return false;
    areaUsed_ = 0;
    bzero(area_, layout_.length);
    switch_dcache_clean_invalidate(area_, layout_.length);
    flow_ = FLOW_CLOSED;
    flowStatus_ = 0;
    replyReady_ = false;
    awdlRings_ = 0;
    awdlRefused_ = 0;
    for (auto &f : awdl_)
        f = {};
    txWanted_ = false;
    data_.txInFlight = data_.txPeak = data_.rxPosted = 0;  // the rest counts on across chip resets

    Buffer msi;
    allocate(4096, 4096, msi);  // the MSI target page: the AFI decodes writes to it, memory never sees them
    msiPA_ = msi.pa;

    const uint32_t info = tcm.r32(shared + SHARED_RING_INFO);
    uint32_t mem = tcm.r32(info + RINGINFO_MEM);
    const uint32_t h2dW = tcm.r32(info + RINGINFO_H2D_W);
    const uint32_t h2dR = tcm.r32(info + RINGINFO_H2D_R);
    const uint32_t d2hW = tcm.r32(info + RINGINFO_D2H_W);
    const uint32_t d2hR = tcm.r32(info + RINGINFO_D2H_R);
    for (unsigned k = 0; k <= RING_COUNT; ++k) {
        const uint16_t items = k == FLOW ? kFlowItems : kRingItems[k];
        const uint16_t size = k == FLOW ? kFlowItemSize : kRingItemSize[k];
        Buffer b;
        // 64-byte aligned, so ring offsets and cache lines line up (the completion-ring rule above relies on it).
        if (!allocate(static_cast<uint32_t>(items) * size, 64, b)) {
            snprintf(why, whyLength, "DMA area too small for ring %u", k);
            return false;
        }
        if (k != FLOW) {  // the flow ring is described to the firmware by its create request instead
            tcm.w32(mem + kRingBaseAddr, b.pa);
            tcm.w32(mem + kRingBaseAddr + 4, 0);
            tcm.w16(mem + kRingMaxItem, items);
            tcm.w16(mem + kRingLenItems, size);
            mem += kRingMemSize;
        }
        // H2D indices: control submit, rx post, then the flow rings; D2H: the three completion rings.
        const bool h2d = k < 2 || k == FLOW;
        uint32_t slot = k < 2 ? 4 * k : 4 * (k - 2);
        if (k == FLOW)
            slot = 4 * kFlowRingId;
        rings_[k] = {b.va, b.pa, items, size, (h2d ? h2dW : d2hW) + slot, (h2d ? h2dR : d2hR) + slot, 0, 0, 0, 0, 0};
    }
    Buffer scratch;
    Buffer ringUpd;
    if (!allocate(kScratchLength, 64, scratch) || !allocate(kRingUpdLength, 64, ringUpd) ||
        !allocate(kIoctlMax + 18, 64, ioctlBuffer_)) {
        snprintf(why, whyLength, "DMA area too small");
        return false;
    }
    tcm.w32(shared + SHARED_SCRATCH_ADDR, scratch.pa);
    tcm.w32(shared + SHARED_SCRATCH_ADDR + 4, 0);
    tcm.w32(shared + SHARED_SCRATCH_LEN, kScratchLength);
    tcm.w32(shared + SHARED_RINGUPD_ADDR, ringUpd.pa);
    tcm.w32(shared + SHARED_RINGUPD_ADDR + 4, 0);
    tcm.w32(shared + SHARED_RINGUPD_LEN, kRingUpdLength);
    for (auto &buffer : control_) {
        if (!allocate(kControlBufferSize, 64, buffer)) {
            snprintf(why, whyLength, "DMA area too small for control buffers");
            return false;
        }
    }
    // Transmit buffers, every id free (oldest freed first out).
    Buffer txBuffers;
    txCount_ = layout_.txBuffers;
    if (!allocate(static_cast<uint32_t>(txCount_) * kTxBufferSize, 64, txBuffers)) {
        snprintf(why, whyLength, "DMA area too small for transmit buffers");
        return false;
    }
    txBase_ = static_cast<uint32_t>(txBuffers.va - area_);
    for (unsigned i = 0; i < txCount_; ++i) {
        txFree_[i] = static_cast<uint16_t>(i + 1);
        txBusy_[i] = 0;
        txStatus_[i] = 0;
    }
    txFreeHead_ = 0;
    txFreeCount_ = txCount_;
    // As many receive buffers as the firmware takes, up to ours.
    maxRxPost_ = tcm.r16(shared + SHARED_MAX_RXBUFPOST);
    const uint16_t takes = maxRxPost_ ? maxRxPost_ : kDefaultMaxRxPost;
    rxCount_ = layout_.rxBuffers < takes ? layout_.rxBuffers : takes;
    Buffer rxBuffers;
    if (!allocate(static_cast<uint32_t>(rxCount_) * kRxBufferSize, 64, rxBuffers)) {
        snprintf(why, whyLength, "DMA area too small for receive buffers");
        return false;
    }
    rxBase_ = static_cast<uint32_t>(rxBuffers.va - area_);
    dataOffset_ = tcm.r32(shared + SHARED_RX_DATAOFFSET);
    dtohMailbox_ = tcm.r32(shared + SHARED_DTOH_MB_DATA);

    // The AWDL flow rings, last, so none of the station's memory moves; as many as the firmware's flow rings
    // (v5: max_flowrings counts the two common submission rings too) and the area allow. Unused until AWDL runs.
    const uint16_t submissionRings = tcm.r16(info + kRingInfoMaxFlowRings);
    maxFlowRings_ = submissionRings > 2 ? static_cast<uint16_t>(submissionRings - 2) : 0;
    for (unsigned k = 0; k < kAwdlFlows && 1u + k < maxFlowRings_; ++k) {
        Buffer b;
        if (!allocate(static_cast<uint32_t>(kAwdlFlowItems) * kFlowItemSize, 64, b) ||
            !allocate(kTxBufferSize, 64, awdl_[k].pending))
            break;
        const uint32_t slot = 4u * (kFlowRingId + 1 + k);
        rings_[FLOW + 1 + k] = {b.va, b.pa, kAwdlFlowItems, kFlowItemSize, h2dW + slot, h2dR + slot, 0, 0, 0, 0, 0};
        awdlRings_ = k + 1;
    }

    for (uint32_t id = 0; id < 2 * kControlBuffers; ++id) {
        if (!postControl(id)) {
            snprintf(why, whyLength, "control submit ring full at buffer %u", id);
            return false;
        }
    }
    for (uint32_t id = 0; id < rxCount_; ++id) {
        if (!postRx(id)) {
            snprintf(why, whyLength, "rx post ring full at buffer %u", id);
            return false;
        }
    }
    publish(RXPOST_SUBMIT);
    return true;
}

void Msgbuf::describe(char *out, size_t length) const
{
    snprintf(out, length,
             "%u KiB DMA area at %#x: %u receive buffers posted (the firmware takes %u%s), %u transmit buffers, "
             "TX flow ring of %u",
             layout_.length / 1024, areaPA_, rxCount_, maxRxPost_ ? maxRxPost_ : kDefaultMaxRxPost,
             maxRxPost_ ? "" : ", its default", txCount_, kFlowItems);
}

void Msgbuf::enableInterrupts()
{
    chip_->write(pcie2_ + kMailboxMask, kMbD2HDoorbells | kMbFn0);
    // brcmf_pcie_hostready: some firmware waits for this doorbell before it serves the rings.
    if (chip_->sharedFlags() & kSharedHostReadyDb1)
        chip_->write(pcie2_ + kH2DMailbox1, 1);
}

void Msgbuf::disableInterrupts()
{
    chip_->write(pcie2_ + kMailboxMask, 0);
}

bool Msgbuf::chipSignalling(uint32_t *mailboxInt, uint32_t *mailboxMask)
{
    *mailboxInt = chip_->read(pcie2_ + kMailboxInt);
    *mailboxMask = chip_->read(pcie2_ + kMailboxMask);
    return *mailboxInt != 0xFFFFFFFF && (*mailboxInt & *mailboxMask) != 0;
}

// A ring index in the chip's RAM: a PCIe read, the one thing on the data path that waits for the bus.
uint16_t Msgbuf::readIndex(uint32_t tcmAddress)
{
    ++data_.indexReads;
    return chip_->tcm().r16(tcmAddress);
}

// Free slots on a host-to-dongle ring (one always stays free), by the chip's read index as last seen; read again
// only when that says the ring is full. A chip gone from the bus (all ones) leaves the ring full.
unsigned Msgbuf::space(unsigned ring)
{
    Ring &r = rings_[ring];
    const auto room = [&r] {
        return static_cast<unsigned>(switch_ring_pending(switch_ring_next(r.pos, r.items), r.seen, r.items));
    };
    if (room() == 0) {
        const uint16_t read = readIndex(r.rIndex);
        if (read < r.items)
            r.seen = read;
    }
    return room();
}

// Room for one more item on a host-to-dongle ring; nullptr when full.
uint8_t *Msgbuf::reserve(unsigned ring)
{
    const Ring &r = rings_[ring];
    if (!space(ring))
        return nullptr;
    uint8_t *item = r.va + static_cast<size_t>(r.pos) * r.size;
    memset(item, 0, r.size);
    return item;
}

// The reserved item written; the chip learns of it at the next publish().
void Msgbuf::advance(unsigned ring)
{
    Ring &r = rings_[ring];
    r.pos = static_cast<uint16_t>(switch_ring_next(r.pos, r.items));
}

// The items written since the last publish out of the cache (one range, two when they wrap; the barrier also
// completes any switch_dcache_clean_lines of their buffers), then the new write index, then one doorbell.
void Msgbuf::publish(unsigned ring)
{
    Ring &r = rings_[ring];
    if (r.pos == r.told)
        return;
    const uint32_t from = static_cast<uint32_t>(r.told) * r.size;
    const uint32_t to = static_cast<uint32_t>(r.pos) * r.size;
    if (r.pos > r.told) {
        switch_dcache_clean_lines(r.va + from, to - from);
    } else {
        switch_dcache_clean_lines(r.va + from, static_cast<uint32_t>(r.items) * r.size - from);
        switch_dcache_clean_lines(r.va, to);
    }
    switch_dcache_sync();
    chip_->tcm().w16(r.wIndex, r.pos);
    r.told = r.pos;
    chip_->write(pcie2_ + kH2DMailbox0, 1);
    ++data_.doorbells;
}

void Msgbuf::commit(unsigned ring)
{
    advance(ring);
    publish(ring);
}

// A control buffer back to the firmware: ioctl responses (ids 0..7) or events (8..15). Flushed first, so no line
// of it is dirty in the cache while the chip writes it.
bool Msgbuf::postControl(uint32_t id)
{
    switch_dcache_clean_invalidate(control_[id].va, kControlBufferSize);
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p)
        return false;
    p[0] = id < kControlBuffers ? MSG_IOCTLRESP_BUF_POST : MSG_EVENT_BUF_POST;
    put32(p + 4, id);
    put16(p + 8, static_cast<uint16_t>(kControlBufferSize));
    put32(p + 16, control_[id].pa);
    put32(p + 20, 0);
    commit(CONTROL_SUBMIT);
    return true;
}

// brcmf_msgbuf_rxbuf_data_post, without the doorbell: the caller publishes the batch.
bool Msgbuf::postRx(uint32_t id)
{
    uint8_t *p = reserve(RXPOST_SUBMIT);
    if (!p)
        return false;
    p[0] = MSG_RXBUF_POST;
    put32(p + 4, id);
    put16(p + 10, static_cast<uint16_t>(kRxBufferSize));
    put32(p + 24, rxPa(id));
    put32(p + 28, 0);
    advance(RXPOST_SUBMIT);
    ++data_.rxPosted;
    return true;
}

// The chip's write index of a completion ring, read once per interrupt (brcmf_commonring_get_read_ptr): whatever
// the chip writes after this read raises the mailbox interrupt again (service() cleared only the status it read), so
// the next interrupt takes it. The items it announces are invalidated here as one batch, completed by drain()'s one
// barrier, rather than one barrier per item.
void Msgbuf::refresh(unsigned ring)
{
    Ring &r = rings_[ring];
    const uint16_t write = readIndex(r.wIndex);
    if (write >= r.items) {  // a chip gone from the bus reads all ones: never chase that index
        IOLog("SwitchWLAN: ring %u write index %#x of %u items, chip gone?\n", ring, write, r.items);
        return;
    }
    r.seen = write;
    const uint32_t from = static_cast<uint32_t>(r.pos) * r.size;
    const uint32_t to = static_cast<uint32_t>(write) * r.size;
    if (write >= r.pos) {
        switch_dcache_clean_invalidate_lines(r.va + from, to - from);
    } else {
        switch_dcache_clean_invalidate_lines(r.va + from, static_cast<uint32_t>(r.items) * r.size - from);
        switch_dcache_clean_invalidate_lines(r.va, to);
    }
}

// Next completion of `ring` up to its write index as last read, copied out. An announced item still reading zero
// is given 10 ms to land (see the top of this file); past that the DMA path is broken, which a panic names.
bool Msgbuf::next(unsigned ring, uint8_t *item)
{
    Ring &r = rings_[ring];
    if (r.pos == r.seen)
        return false;
    const uint8_t *p = r.va + static_cast<size_t>(r.pos) * r.size;
    for (uint32_t waited = 0; !p[0]; waited += 10) {
        if (waited >= kLandingSpinUs) {
            panic("SwitchWLAN: ring %u item %u announced (write index %u) but still empty after %u ms: the chip's DMA "
                  "writes are not reaching memory (area %#x)",
                  ring, r.pos, r.seen, kLandingSpinUs / 1000, areaPA_);
        }
        IODelay(10);
        switch_dcache_clean_invalidate(p, r.size);
    }
    memcpy(item, p, r.size);
    r.pos = static_cast<uint16_t>(switch_ring_next(r.pos, r.items));
    r.consumed += r.size;
    return true;
}

// Lines whose items are all consumed: zeroed and cleaned out of the cache, then the read index advanced to the
// first item not wholly inside them.
void Msgbuf::retire(unsigned ring)
{
    Ring &r = rings_[ring];
    const uint32_t bytes = static_cast<uint32_t>(r.items) * r.size;
    const uint64_t target = r.consumed & ~static_cast<uint64_t>(SWITCH_DCACHE_LINE - 1);
    if (r.cleared >= target)
        return;
    while (r.cleared < target) {
        uint8_t *line = r.va + static_cast<uint32_t>(r.cleared % bytes);
        memset(line, 0, SWITCH_DCACHE_LINE);
        switch_dcache_clean_invalidate_lines(line, SWITCH_DCACHE_LINE);
        r.cleared += SWITCH_DCACHE_LINE;
    }
    switch_dcache_sync();
    chip_->tcm().w16(r.rIndex, static_cast<uint16_t>((r.cleared / r.size) % r.items));
}

void Msgbuf::txRelease(uint32_t id, uint16_t status)
{
    if (id < 1 || id > txCount_ || !txBusy_[id - 1])
        return;  // not one of ours in flight (a stale completion)
    txBusy_[id - 1] = 0;
    txStatus_[id - 1] = status;
    txFree_[switch_ring_advance(txFreeHead_, txFreeCount_, txCount_)] = static_cast<uint16_t>(id);
    ++txFreeCount_;
    --data_.txInFlight;
    ++data_.txDone;
    if (status) {
        ++data_.txFailed;
        txFailStatus_ = status;
    }
}

void Msgbuf::drain()
{
    uint8_t item[40];
    refresh(CONTROL_COMPLETE);
    refresh(TX_COMPLETE);
    refresh(RX_COMPLETE);
    switch_dcache_sync();
    for (bool more = true; more;) {
        while (next(CONTROL_COMPLETE, item)) {
            const uint32_t id = le32(item + 4);
            if (item[0] == MSG_IOCTL_CMPLT && id < kControlBuffers) {
                size_t length = le16(item + 12);
                if (length > sizeof(reply_))
                    length = sizeof(reply_);
                switch_dcache_clean_invalidate(control_[id].va, length);
                memcpy(reply_, control_[id].va, length);
                replyLength_ = length;
                replyStatus_ = static_cast<int16_t>(le16(item + 8));
                replyReady_ = true;
                postControl(id);
            } else if (item[0] == MSG_WL_EVENT && id >= kControlBuffers && id < 2 * kControlBuffers) {
                uint32_t length = le16(item + 12);
                if (dataOffset_ + length > kControlBufferSize)
                    length = kControlBufferSize - dataOffset_;
                const uint8_t *frame = control_[id].va + dataOffset_;
                switch_dcache_clean_invalidate(frame, length);
                if (length >= kEventData && ((static_cast<unsigned>(frame[12]) << 8) | frame[13]) == kEthPLinkCtl &&
                    events_) {
                    Event e;
                    e.flags = (static_cast<uint32_t>(frame[kEventMsg + 2]) << 8) | frame[kEventMsg + 3];
                    e.type = be32(frame + kEventMsg + 4);
                    e.status = be32(frame + kEventMsg + 8);
                    e.reason = be32(frame + kEventMsg + 12);
                    const uint32_t dataLength = be32(frame + kEventMsg + 20);
                    memcpy(e.addr, frame + kEventMsg + 24, 6);
                    e.ifidx = frame[kEventMsg + 46];  // after the 16-byte interface name
                    e.bsscfgidx = frame[kEventMsg + 47];
                    e.data = frame + kEventData;
                    e.length = dataLength < length - kEventData ? dataLength : length - kEventData;
                    events_(context_, e);
                }
                postControl(id);
            } else if (item[0] == MSG_FLOW_RING_CREATE_CMPLT && le16(item + 10) > kFlowRingId) {
                const unsigned k = le16(item + 10) - kFlowRingId - 1u;  // an AWDL flow ring
                if (k < awdlRings_ && awdl_[k].state == FLOW_PENDING) {
                    AwdlFlow &f = awdl_[k];
                    f.state = le16(item + 8) ? FLOW_REFUSED : FLOW_OPEN;
                    if (f.state == FLOW_OPEN && f.pendingLength)
                        post(FLOW + 1 + k, f.ifidx, f.pending.va, f.pendingLength);
                    f.pendingLength = 0;
                }
            } else if (item[0] == MSG_FLOW_RING_CREATE_CMPLT) {
                flowStatus_ = le16(item + 8);
                flow_ = flowStatus_ ? FLOW_REFUSED : FLOW_OPEN;
            } else if (item[0] == MSG_FLOW_RING_DELETE_CMPLT && le16(item + 10) > kFlowRingId) {
                const unsigned k = le16(item + 10) - kFlowRingId - 1u;
                if (k < awdlRings_)
                    awdl_[k].state = FLOW_CLOSED;
            }
            // MSG_IOCTLPTR_REQ_ACK and status messages (general and ring status) need no answer.
        }
        retire(CONTROL_COMPLETE);
        // Transmit completions first: they free the buffers the next batch (and the receive direction's ACKs) go
        // out of.
        unsigned n = 0;
        while (n < kDrainBudget && next(TX_COMPLETE, item)) {
            ++n;
            if (item[0] == MSG_TX_STATUS)
                txRelease(le32(item + 4), le16(item + 8));
        }
        retire(TX_COMPLETE);

        // brcmf_msgbuf_process_rx_complete: the frame at its own offset, or at the firmware's default one; each
        // buffer back to the chip after its frame was copied out, one doorbell for the pass. Every frame's lines are
        // invalidated first, with one barrier for the pass, before any frame is read.
        struct Arrival {
            uint32_t id;
            uint16_t offset, length;
            uint8_t ifidx, kind;
        };

        Arrival arrivals[kDrainBudget];
        unsigned arrived = 0;
        n = 0;
        while (n < kDrainBudget && next(RX_COMPLETE, item)) {
            ++n;
            const uint32_t id = le32(item + 4);
            if (item[0] != MSG_RX_CMPLT || id >= rxCount_)
                continue;
            const uint32_t offset = le16(item + 16) ? le16(item + 16) : dataOffset_;
            uint32_t length = le16(item + 14);
            if (offset >= kRxBufferSize) {
                length = 0;
            } else if (offset + length > kRxBufferSize) {
                length = kRxBufferSize - offset;
            }
            switch_dcache_clean_invalidate_lines(rxVa(id) + offset, length);
            arrivals[arrived++] = {id, static_cast<uint16_t>(offset), static_cast<uint16_t>(length), item[1],
                                   static_cast<uint8_t>(le16(item + kRxFlags) & kRxFrameMask)};
        }
        if (arrived)
            switch_dcache_sync();
        for (unsigned i = 0; i < arrived; ++i) {
            const Arrival &a = arrivals[i];
            --data_.rxPosted;
            ++data_.rxFrames;
            data_.rxBytes += a.length;
            if (frames_ && a.length >= kEthHeader)
                frames_(context_, rxVa(a.id) + a.offset, a.length, a.ifidx, a.kind);  // copied out here
            if (postRx(a.id))
                ++data_.rxReposted;
        }
        retire(RX_COMPLETE);
        publish(RXPOST_SUBMIT);
        // Another round while either ring still has completions announced at the interrupt.
        more =
            rings_[TX_COMPLETE].pos != rings_[TX_COMPLETE].seen || rings_[RX_COMPLETE].pos != rings_[RX_COMPLETE].seen;
    }
}

uint32_t Msgbuf::service(bool *halted)
{
    *halted = false;
    ++data_.drains;
    chip_->write(pcie2_ + kMailboxMask, 0);
    const uint32_t status = chip_->read(pcie2_ + kMailboxInt);
    if (status == 0xFFFFFFFF)
        return status;  // the chip is gone; the caller reports it, the mask stays off
    if (status)
        chip_->write(pcie2_ + kMailboxInt, status);
    if (status & kMbFn0) {
        const Mmio &tcm = chip_->tcm();
        const uint32_t data = tcm.r32(dtohMailbox_);
        if (data)
            tcm.w32(dtohMailbox_, 0);
        // Deep-sleep requests (D2H_DEV_DS_ENTER_REQ) are not granted (nxk): the ring indices live in the chip's RAM,
        // which a sleeping chip may not serve. Without the acknowledgement the firmware simply stays awake.
        if (data & kD2HFwHalt)
            *halted = true;
    }
    drain();
    if (!*halted)
        chip_->write(pcie2_ + kMailboxMask, kMbD2HDoorbells | kMbFn0);
    return status;
}

bool Msgbuf::ioctlSubmit(uint32_t cmd, uint8_t ifidx, const uint8_t *data, size_t length, size_t outLength, char *why,
                         size_t whyLength)
{
    size_t n = length > outLength ? length : outLength;
    // brcmf_msgbuf_tx_ioctl: a longer request (actframe's whole brcmf_fil_af_params_le) declares its length as the
    // output length and carries its first BRCMF_TX_IOCTL_MAX_MSG_SIZE bytes
    const uint16_t declared = static_cast<uint16_t>(n > 0xffff ? 0xffff : n);
    if (n > kIoctlMax)
        n = kIoctlMax;
    memset(ioctlBuffer_.va, 0, n);
    memcpy(ioctlBuffer_.va, data, length < n ? length : n);
    switch_dcache_clean(ioctlBuffer_.va, n);
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) {
        snprintf(why, whyLength, "control submit ring full");
        return false;
    }
    p[0] = MSG_IOCTLPTR_REQ;
    p[1] = ifidx;
    put32(p + 4, kIoctlReqPktId);
    put32(p + 8, cmd);
    put16(p + 12, ++transId_);
    put16(p + 14, static_cast<uint16_t>(n));  // input_buf_len
    put16(p + 16, declared);                  // output_buf_len
    put32(p + 24, ioctlBuffer_.pa);
    put32(p + 28, 0);
    replyReady_ = false;
    replyLength_ = 0;
    commit(CONTROL_SUBMIT);
    return true;
}

bool Msgbuf::flowCreateSubmit(const uint8_t destination[6], const uint8_t source[6])
{
    if (flow_ == FLOW_OPEN || flow_ == FLOW_PENDING)
        return true;
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p)
        return false;
    const Ring &f = rings_[FLOW];
    p[0] = MSG_FLOW_RING_CREATE;
    memcpy(p + 8, destination, 6);
    memcpy(p + 14, source, 6);
    put16(p + 22, kFlowRingId);
    put16(p + 28, f.items);
    put16(p + 30, f.size);
    put32(p + 32, f.pa);
    put32(p + 36, 0);
    flow_ = FLOW_PENDING;
    commit(CONTROL_SUBMIT);
    return true;
}

uint32_t Msgbuf::send(const uint8_t *frame, size_t length)
{
    if (flow_ != FLOW_OPEN) {
        ++data_.txRefused;
        return 0;
    }
    return post(FLOW, 0, frame, length);
}

unsigned Msgbuf::txRoom()
{
    const unsigned room = flow_ == FLOW_OPEN ? txFreeCount_ : 0;
    if (!room && flow_ == FLOW_OPEN) {
        if (!txWanted_)
            ++data_.txFull;
        txWanted_ = true;
    }
    return room;
}

uint8_t *Msgbuf::txBuffer()
{
    return flow_ == FLOW_OPEN && txFreeCount_ ? txVa(txFree_[txFreeHead_]) : nullptr;
}

uint32_t Msgbuf::txPost(size_t length)
{
    if (flow_ != FLOW_OPEN) {
        ++data_.txRefused;
        return 0;
    }
    return postFrame(FLOW, 0, length);
}

void Msgbuf::txFlush()
{
    publish(FLOW);
}

void Msgbuf::txReadBack(uint32_t id, const uint8_t *written, size_t length)
{
    if (id < 1 || id > txCount_ || length > kTxBufferSize)
        return;
    const uint8_t *chip = txVa(id);
    switch_dcache_clean_invalidate(chip, length);
    ++data_.txReread;
    for (size_t i = 0; i < length; ++i) {
        if (chip[i] != written[i]) {
            ++data_.txRereadDiffer;
            data_.txRereadAt = static_cast<uint32_t>(i);
            return;
        }
    }
}

bool Msgbuf::txResume()
{
    const unsigned enough = kTxResumeFree < txCount_ / 2 ? kTxResumeFree : txCount_ / 2;
    if (!txWanted_ || flow_ != FLOW_OPEN || txFreeCount_ < enough)
        return false;
    txWanted_ = false;
    return true;
}

DataCounters Msgbuf::takeCounters()
{
    const DataCounters c = data_;
    data_.txPeak = data_.txInFlight;
    return c;
}

// The frame already in the next free transmit buffer onto a TX flow ring of interface `ifidx`: its header in the
// ring item, the payload read from the buffer. The request id its completion answers for, 0 when it cannot go now.
// The ring's write index waits for publish().
uint32_t Msgbuf::postFrame(unsigned ring, uint8_t ifidx, size_t length)
{
    if (!txFreeCount_ || length < kEthHeader || length > kTxBufferSize) {
        ++data_.txRefused;
        return 0;
    }
    uint8_t *p = reserve(ring);
    if (!p) {
        ++data_.txRefused;
        return 0;
    }
    const uint16_t id = txFree_[txFreeHead_];
    txFreeHead_ = switch_ring_next(txFreeHead_, txCount_);
    --txFreeCount_;
    const uint8_t *frame = txVa(id);
    switch_dcache_clean_lines(frame, length);
    txBusy_[id - 1] = 1;
    p[0] = MSG_TX_POST;
    p[1] = ifidx;
    put32(p + 4, id);
    memcpy(p + 8, frame, kEthHeader);
    p[22] = kPktFrame8023;
    p[23] = 1;  // one segment
    put32(p + 32, txPa(id) + kEthHeader);
    put32(p + 36, 0);
    put16(p + 42, static_cast<uint16_t>(length - kEthHeader));
    advance(ring);
    ++data_.txFrames;
    data_.txBytes += length;
    if (++data_.txInFlight > data_.txPeak)
        data_.txPeak = data_.txInFlight;
    return id;
}

// One frame copied into a transmit buffer and onto `ring` at once (handshake frames, AWDL).
uint32_t Msgbuf::post(unsigned ring, uint8_t ifidx, const uint8_t *frame, size_t length)
{
    if (!txFreeCount_ || length < kEthHeader || length > kTxBufferSize) {
        ++data_.txRefused;
        return 0;
    }
    memcpy(txVa(txFree_[txFreeHead_]), frame, length);
    const uint32_t id = postFrame(ring, ifidx, length);
    if (id)
        publish(ring);
    return id;
}

bool Msgbuf::sent(uint32_t id, uint16_t *status) const
{
    if (id < 1 || id > txCount_ || txBusy_[id - 1])
        return false;
    *status = txStatus_[id - 1];
    return true;
}

// brcmf_flowring_create for a non-station interface: one ring per destination, every group address on the
// broadcast one. The first frame to a new destination is kept until its ring is open (drain sends it).
int Msgbuf::awdlSend(uint8_t ifidx, const uint8_t *frame, size_t length)
{
    if (length < kEthHeader || length > kTxBufferSize) {
        ++awdlRefused_;
        return -1;
    }
    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t *da = (frame[0] & 1) ? kBroadcast : frame;
    unsigned k = 0;
    unsigned free = awdlRings_;
    for (; k < awdlRings_; ++k) {
        if ((awdl_[k].state == FLOW_OPEN || awdl_[k].state == FLOW_PENDING) && awdl_[k].ifidx == ifidx &&
            !memcmp(awdl_[k].da, da, 6))
            break;
        if (free == awdlRings_ && (awdl_[k].state == FLOW_CLOSED || awdl_[k].state == FLOW_REFUSED))
            free = k;
    }
    if (k < awdlRings_) {
        AwdlFlow &f = awdl_[k];
        f.lastUse = ++awdlUse_;
        if (f.state == FLOW_OPEN)
            return post(FLOW + 1 + k, ifidx, frame, length) ? 1 : -1;
        if (f.pendingLength) {
            ++awdlRefused_;
            return -1;
        }
        memcpy(f.pending.va, frame, length);
        f.pendingLength = static_cast<uint16_t>(length);
        return 0;
    }
    if (free == awdlRings_) {  // every AWDL ring in use (or being deleted)
        ++awdlRefused_;
        return -1;
    }
    Ring &r = rings_[FLOW + 1 + free];
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) {
        ++awdlRefused_;
        return -1;
    }
    AwdlFlow &f = awdl_[free];
    memcpy(f.da, da, 6);
    f.ifidx = ifidx;
    f.state = FLOW_PENDING;
    f.lastUse = ++awdlUse_;
    memcpy(f.pending.va, frame, length);
    f.pendingLength = static_cast<uint16_t>(length);
    r.pos = r.seen = r.told = 0;  // a fresh ring: both indices from zero
    chip_->tcm().w16(r.wIndex, 0);
    bzero(r.va, static_cast<size_t>(r.items) * r.size);
    switch_dcache_clean_invalidate(r.va, static_cast<size_t>(r.items) * r.size);
    p[0] = MSG_FLOW_RING_CREATE;
    p[1] = ifidx;
    memcpy(p + 8, da, 6);
    memcpy(p + 14, frame + 6, 6);
    put16(p + 22, static_cast<uint16_t>(kFlowRingId + 1 + free));
    put16(p + 28, r.items);
    put16(p + 30, r.size);
    put32(p + 32, r.pa);
    put32(p + 36, 0);
    commit(CONTROL_SUBMIT);
    return 0;
}

void Msgbuf::awdlClose()
{
    for (unsigned k = 0; k < awdlRings_; ++k) {
        AwdlFlow &f = awdl_[k];
        f.pendingLength = 0;
        if (f.state != FLOW_OPEN && f.state != FLOW_PENDING)
            continue;
        uint8_t *p = reserve(CONTROL_SUBMIT);
        if (!p) {
            f.state = FLOW_CLOSED;  // the control ring is full: forget it (the chip is in trouble anyway)
            continue;
        }
        p[0] = MSG_FLOW_RING_DELETE;
        p[1] = f.ifidx;
        put16(p + 8, static_cast<uint16_t>(kFlowRingId + 1 + k));
        commit(CONTROL_SUBMIT);
        f.state = FLOW_DELETING;
    }
}

}  // namespace swlan
