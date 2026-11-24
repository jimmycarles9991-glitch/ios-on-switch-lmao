// The SwitchWLAN data path on the host: drivers/SwitchWLAN/SwitchWLANMsgbuf.cpp itself (its own translation unit over
// switch_wlan_msgbuf_stubs.h, BUILD.bazel) driven against a model of the BCM4356's msgbuf firmware: the chip's RAM (shared
// area, ring descriptors and indices) as host memory, the PCIe2 core's mailbox registers, and a firmware that takes
// receive buffers and transmit posts off the H2D rings and writes completions onto the D2H rings by physical address,
// raising the mailbox interrupt each time. The driver's own Mmio reads of the chip's RAM (the PCIe round trips a
// frame costs), its register reads and writes, doorbells, interrupts and the bytes copied per frame are counted, and
// each scenario's counts are checked against what r8a (11-05) measured on the device before r17-wlanperf: at fast.com's
// 40 Mb/s about 1.3 frames per MSI, and 2069-3439 of 4714-6466 en0 frames per 5 s refused for want of one of 32
// transmit buffers (TCP's ACKs on a download, its data on an upload). Here: every frame of a download delivered intact
// with its buffer back in the next doorbell, one doorbell and about one chip-RAM index read per batch instead of per
// frame, an upload of 4000 frames with no drop (frames wait in en0's queue, a completion resumes it), the handshake
// path, the 512 KiB fallback, and the link decoding. One process per scenario (the driver's Msgbuf is a global).
#include <cstdio>
#include <cstring>  // SwitchWLANLink.h's snprintf, memset, memcmp, strlen
#include <deque>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../SwitchWLANLink.h"
#include "../SwitchWLANPrivate.h"
#include "../SwitchWLANRecovery.h"
#include "../SwitchWLANA3Wire.h"
#include "../SwitchWLANDma.h"
#include "../firmware/a3/wire.h"
#include "bazel/testing/fresh_process.h"
#include "bazel/testing/require.h"

namespace {

// ---- what the driver did -------------------------------------------------------------------------------------------
struct Counts {
    uint64_t tcmReads = 0, tcmWrites = 0;  // Mmio accesses to the chip's RAM (ring indices, shared area)
    uint64_t regReads = 0, regWrites = 0;  // backplane registers through BAR0 (mailbox)
    uint64_t doorbells = 0;                // H2D mailbox 0 writes
    uint64_t msis = 0;                     // interrupts taken (service() calls)
    uint64_t copiedBytes = 0;              // frame bytes the CPU copied: out of RX buffers, into TX buffers
};

Counts gCount;
bool gDmaMapFailure = false;
unsigned gDmaMapCalls = 0;
uint64_t gDmaMappedPa = 0;
uint64_t gDmaWritesAtMap = 0;
size_t gDmaMappedLength = 0;
uint8_t gDmaContentsAtMap[2] = {};

// ---- the chip's RAM ------------------------------------------------------------------------------------------------
constexpr uint32_t kTcmSize = 1u << 20;
uint8_t gTcm[kTcmSize];
constexpr uint32_t kShared = 0x1000, kDtohMb = 0x2000, kRingInfo = 0x3000, kRingMem = 0x4000;
uint32_t gSharedAddress = kShared;
uint32_t gRamBase = 0, gRamSize = kTcmSize;
constexpr uint32_t kH2DW = 0x5000, kH2DR = 0x5100, kD2HW = 0x5200, kD2HR = 0x5300;
constexpr uint32_t kPcie2Base = 0x18003000;
uint16_t gMaxRxPost = 255;

uint16_t tcm16(uint32_t a)
{
    return static_cast<uint16_t>(gTcm[a] | gTcm[a + 1] << 8);
}

uint32_t tcm32(uint32_t a)
{
    return tcm16(a) | static_cast<uint32_t>(tcm16(a + 2)) << 16;
}

void setTcm16(uint32_t a, uint16_t v)
{
    gTcm[a] = static_cast<uint8_t>(v);
    gTcm[a + 1] = static_cast<uint8_t>(v >> 8);
}

void setTcm32(uint32_t a, uint32_t v)
{
    setTcm16(a, static_cast<uint16_t>(v));
    setTcm16(a + 2, static_cast<uint16_t>(v >> 16));
}

void resetTcm()
{
    memset(gTcm, 0, sizeof(gTcm));
    setTcm16(kShared + 34, gMaxRxPost);  // max_rxbufpost
    setTcm32(kShared + 36, 0);           // rx_dataoffset
    setTcm32(kShared + 44, kDtohMb);
    setTcm32(kShared + 48, kRingInfo);
    setTcm32(kRingInfo + 0, kRingMem);
    setTcm32(kRingInfo + 4, kH2DW);
    setTcm32(kRingInfo + 8, kH2DR);
    setTcm32(kRingInfo + 12, kD2HW);
    setTcm32(kRingInfo + 16, kD2HR);
    setTcm16(kRingInfo + 52, 40);  // max_flowrings (v5: with the two common submission rings)
}

uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t *p)
{
    return le16(p) | static_cast<uint32_t>(le16(p + 2)) << 16;
}

void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

// ---- the firmware ----------------------------------------------------------------------------------------------------
struct Frame {
    std::vector<uint8_t> bytes;
    uint8_t ifidx = 0, kind = 1;  // the rx completion's interface and frame type (802.3)
};

Frame makeFrame(uint32_t seq, size_t length)
{
    Frame f;
    f.bytes.resize(length);
    for (size_t i = 0; i < length; ++i)
        f.bytes[i] = static_cast<uint8_t>((seq * 131 + i * 7) & 0xFF);
    f.bytes[12] = 0x08;  // IPv4
    f.bytes[13] = 0x00;
    put32(f.bytes.data() + 14, seq);
    return f;
}

class Firmware {
public:
    struct Ring {
        uint32_t pa = 0;
        uint16_t items = 0, size = 0, pos = 0;
        bool live = false;
    };

    uint32_t mbInt = 0, mbMask = 0;
    Ring h2d[2 + 8];                                      // control submit, rx post, flow rings by id 2..
    Ring d2h[3];                                          // control complete, tx complete, rx complete
    std::deque<std::pair<uint32_t, uint32_t>> rxBuffers;  // posted: id, pa
    std::deque<std::pair<uint32_t, uint32_t>> ioctlBuffers, eventBuffers;

    struct Held {
        uint32_t id;
        uint16_t ring;
    };

    std::deque<Held> txHeld;    // transmit posts fetched, waiting for the air
    std::vector<Frame> txSeen;  // frames as the chip DMA'd them
    unsigned holdLimit = 64;    // frames the firmware fetches into its own memory before the air takes them
    uint64_t rxStarved = 0;     // frames dropped for want of a posted receive buffer
    unsigned minPosted = ~0u;   // fewest receive buffers it held when a frame came
    uint16_t lastTxStatus = 0;
    uint16_t ioctlIn = 0, ioctlOut = 0;  // the last ioctl's input and output lengths
    uint16_t ioctlTrans = 0;
    uint32_t ioctlCmd = 0;
    bool holdIoctl = false;
    bool holdControlCredits = false, holdDeletes = false;
    uint16_t deleteStatus = 0;
    unsigned deletes = 0;
    uint8_t lastFlowSource[6] = {};
    std::vector<uint8_t> ioctlBytes;  // and what it carried

    uint8_t *va(uint32_t pa) const
    {
        if (pa < kFakeAreaPA || pa - kFakeAreaPA >= gAreaLength)
            throw std::runtime_error("DMA outside the area");
        return gAreaBytes + (pa - kFakeAreaPA);
    }

    void attach()
    {
        *this = Firmware{};
        static const uint16_t items[5] = {64, 512, 64, 1024, 512}, sizes[5] = {40, 32, 24, 16, 32};
        for (unsigned k = 0; k < 5; ++k) {
            Ring &r = k < 2 ? h2d[k] : d2h[k - 2];
            const uint32_t mem = kRingMem + 16 * k;
            r.pa = tcm32(mem + 8);
            r.items = tcm16(mem + 4);
            r.size = tcm16(mem + 6);
            r.live = true;
            if (r.items != items[k] || r.size != sizes[k])
                throw std::runtime_error("ring geometry");
        }
    }

    // The H2D rings up to the host's write indices (the firmware's own reads of its RAM are not PCIe for the host).
    void serviceH2D()
    {
        for (unsigned id = 0; id < sizeof(h2d) / sizeof(h2d[0]); ++id) {
            Ring &r = h2d[id];
            if (!r.live)
                continue;
            const uint16_t w = tcm16(kH2DW + 4 * id);
            while (r.pos != w) {
                const uint8_t *item = va(r.pa + static_cast<uint32_t>(r.pos) * r.size);
                if (id >= 2 && item[0] == 0x0F && txHeld.size() >= holdLimit)
                    break;  // leaves it in the ring
                take(id, item);
                r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
                if (id || !holdControlCredits)
                    setTcm16(kH2DR + 4 * id, r.pos);
            }
        }
    }

    // One completion onto a D2H ring and its doorbell interrupt; false when the host has not made room.
    bool push(unsigned k, const uint8_t *item)
    {
        Ring &r = d2h[k];
        if ((r.pos + 1) % r.items == tcm16(kD2HR + 4 * k))
            return false;
        memcpy(va(r.pa + static_cast<uint32_t>(r.pos) * r.size), item, r.size);
        r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
        setTcm16(kD2HW + 4 * k, r.pos);
        mbInt |= 0x10000;
        return true;
    }

    // A received frame into the oldest posted buffer.
    bool receive(const Frame &f)
    {
        return receiveMetadata(f, 0, static_cast<uint16_t>(f.bytes.size()));
    }

    bool receiveMetadata(const Frame &f, uint16_t offset, uint16_t length)
    {
        minPosted = rxBuffers.size() < minPosted ? static_cast<unsigned>(rxBuffers.size()) : minPosted;
        if (rxBuffers.empty()) {
            ++rxStarved;
            return false;
        }
        const auto [id, pa] = rxBuffers.front();
        rxBuffers.pop_front();
        if (!f.bytes.empty()) {
            const uint32_t at = offset ? offset : tcm32(kShared + 36);
            if (at >= 2048 || f.bytes.size() > 2048 - at)
                throw std::runtime_error("test RX write outside posted buffer");
            memcpy(va(pa) + at, f.bytes.data(), f.bytes.size());
        }
        uint8_t item[32] = {};
        item[0] = 0x12;
        item[1] = f.ifidx;
        put32(item + 4, id);
        put16(item + 14, length);
        put16(item + 16, offset);
        put16(item + 18, f.kind);
        if (!push(2, item))
            throw std::runtime_error("rx complete ring full");
        return true;
    }

    void event(const std::vector<uint8_t> &bytes, uint16_t length)
    {
        if (eventBuffers.empty())
            throw std::runtime_error("no posted event buffer");
        const auto [id, pa] = eventBuffers.front();
        eventBuffers.pop_front();
        const uint32_t offset = tcm32(kShared + 36);
        if (offset >= 8192 || bytes.size() > 8192 - offset)
            throw std::runtime_error("test event write outside posted buffer");
        memcpy(va(pa) + offset, bytes.data(), bytes.size());
        uint8_t item[24] = {};
        item[0] = 0x0e;
        put32(item + 4, id);
        put16(item + 12, length);
        if (!push(0, item))
            throw std::runtime_error("event complete ring full");
    }

    // The air took `n` of the frames held: their completions, oldest first.
    void transmitted(unsigned n, uint16_t status = 0)
    {
        for (; n && !txHeld.empty(); --n) {
            completeTx(txHeld.front().id, txHeld.front().ring, status);
            txHeld.pop_front();
        }
    }

    void completeTx(uint32_t id, uint16_t ring, uint16_t status = 0)
    {
        uint8_t item[16] = {};
        item[0] = 0x10;
        put32(item + 4, id);
        put16(item + 8, status);
        put16(item + 10, ring);
        if (!push(1, item))
            throw std::runtime_error("tx complete ring full");
    }

    void completeDelete(uint16_t ring)
    {
        uint8_t item[24] = {};
        item[0] = 0x06;
        put16(item + 8, deleteStatus);
        put16(item + 10, ring);
        if (!push(0, item))
            throw std::runtime_error("control complete ring full");
    }

    void reg(uint32_t offset, uint32_t value)
    {
        switch (offset) {
        case 0x48:
            mbInt &= ~value;
            break;  // write 1 to clear
        case 0x4C:
            mbMask = value;
            break;
        case 0x140:
            ++gCount.doorbells;
            break;
        default:
            break;
        }
    }

    uint32_t reg(uint32_t offset) const
    {
        return offset == 0x48 ? mbInt : offset == 0x4C ? mbMask : 0;
    }

    bool interrupting() const
    {
        return (mbInt & mbMask) != 0;
    }

    void completeIoctl(uint16_t transaction, uint32_t command, uint32_t value = 0x12345678, int16_t status = 0)
    {
        if (ioctlBuffers.empty())
            throw std::runtime_error("ioctl without a response buffer");
        const auto [id, pa] = ioctlBuffers.front();
        ioctlBuffers.pop_front();
        put32(va(pa), value);
        uint8_t out[24] = {};
        out[0] = 0x0C;
        put32(out + 4, id);
        put16(out + 8, static_cast<uint16_t>(status));
        put16(out + 12, 4);
        put16(out + 14, transaction);
        put32(out + 16, command);
        if (!push(0, out))
            throw std::runtime_error("control complete ring full");
    }

private:
    void control(const uint8_t *item)
    {
        uint8_t out[24] = {};
        switch (item[0]) {
        case 0x0B:
            ioctlBuffers.emplace_back(le32(item + 4), le32(item + 16));
            return;
        case 0x0D:
            eventBuffers.emplace_back(le32(item + 4), le32(item + 16));
            return;
        case 0x09: {  // an ioctl: its answer (four bytes) into the oldest response buffer
            ioctlIn = le16(item + 14);
            ioctlOut = le16(item + 16);
            ioctlTrans = le16(item + 12);
            ioctlCmd = le32(item + 8);
            ioctlBytes.assign(va(le32(item + 24)), va(le32(item + 24)) + ioctlIn);
            if (!holdIoctl)
                completeIoctl(ioctlTrans, ioctlCmd);
            return;
        }
        case 0x03: {  // flow ring create
            memcpy(lastFlowSource, item + 14, sizeof lastFlowSource);
            const uint16_t flowId = le16(item + 22);
            Ring &f = h2d[flowId];
            f.items = le16(item + 28);
            f.size = le16(item + 30);
            f.pa = le32(item + 32);
            f.pos = 0;
            f.live = true;
            out[0] = 0x04;
            put16(out + 10, flowId);
            break;
        }
        case 0x05:
            ++deletes;
            if (!holdDeletes)
                completeDelete(le16(item + 8));
            return;
        default:
            return;
        }
        if (!push(0, out))
            throw std::runtime_error("control complete ring full");
    }

    void take(unsigned id, const uint8_t *item)
    {
        if (id == 0)
            return control(item);
        if (id == 1) {
            if (item[0] != 0x11)
                throw std::runtime_error("rx post ring: not a buffer post");
            rxBuffers.emplace_back(le32(item + 4), le32(item + 24));
            return;
        }
        if (item[0] != 0x0F)
            throw std::runtime_error("flow ring: not a TX post");
        Frame f;
        f.bytes.assign(item + 8, item + 22);
        const uint8_t *payload = va(le32(item + 32));
        f.bytes.insert(f.bytes.end(), payload, payload + le16(item + 42));
        txSeen.push_back(std::move(f));
        txHeld.push_back({le32(item + 4), static_cast<uint16_t>(id)});
    }
};

Firmware gFw;

// ---- the driver around Msgbuf: what SwitchWLAN.cpp and SwitchWLANNet.cpp do with it ----------------------------------
std::vector<Frame> gReceived;
std::vector<std::vector<uint8_t>> gEventData;

void onFrame(void *, const uint8_t *frame, size_t length, uint8_t ifidx, uint8_t kind)
{
    gCount.copiedBytes += length;  // SwitchWLANNet on_rx: the one copy, into the mbuf
    gReceived.push_back(Frame{std::vector<uint8_t>(frame, frame + length), ifidx, kind});
}

void onEvent(void *, const swlan::Event &event)
{
    gEventData.emplace_back(event.data, event.data + event.length);
}

swlan::Chip gChip;
swlan::Msgbuf gMb;

// The interrupt: taken as SwitchWLAN::msiFired takes it, as long as the chip raises it.
unsigned gResumes = 0;
std::deque<Frame> gQueue;  // en0's output queue (the stack's)
void start();

void interrupts()
{
    while (gFw.interrupting()) {
        bool halted = false;
        ++gCount.msis;
        gMb.service(&halted);
        if (gMb.txResume()) {
            ++gResumes;
            start();
        }
    }
}

// en0's start routine (SwitchWLANNet if_start over SwitchWLAN::transmitFrames): what the radio takes, in batches of
// at most 64, one copy per frame from the queue into its transmit buffer, one doorbell per batch.
unsigned gBatches = 0;

void start()
{
    for (;;) {
        unsigned room = gMb.txRoom();
        if (!room || gQueue.empty())
            return;
        unsigned batch = room < 64 ? room : 64;
        ++gBatches;
        for (; batch && !gQueue.empty() && gMb.txRoom(); --batch) {
            const Frame &f = gQueue.front();
            uint8_t *buffer = gMb.txBuffer();
            memcpy(buffer, f.bytes.data(), f.bytes.size());
            gCount.copiedBytes += f.bytes.size();
            if (!gMb.txPost(f.bytes.size()))
                throw std::runtime_error("txPost refused with room");
            gQueue.pop_front();
        }
        gMb.txFlush();
    }
}

// The firmware's turn: everything the host rang for.
void firmware()
{
    gFw.serviceH2D();
    interrupts();
}

const uint8_t kAp[6] = {0x02, 0, 0, 0, 0, 1}, kUs[6] = {0x98, 0xb6, 0xe9, 0x5c, 0x10, 0xc8};

bool bringUp(uint16_t submissionRings = 40, uint32_t dataOffset = 0)
{
    resetTcm();
    setTcm16(kRingInfo + 52, submissionRings);
    setTcm32(kShared + 36, dataOffset);
    gFw = Firmware{};
    gCount = {};
    gReceived.clear();
    gEventData.clear();
    char why[200] = "";
    gChip.attach(nullptr, 0, 0, why, sizeof(why));
    if (!gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof(why))) {
        ADD_FAILURE() << "attach: " << why;
        return false;
    }
    gFw.attach();
    gMb.enableInterrupts();
    firmware();
    return true;
}

bool openFlow()
{
    gMb.flowCreateSubmit(kAp, kUs);
    firmware();
    return gMb.flow() == swlan::Msgbuf::FLOW_OPEN;
}

unsigned gPeak = 0;  // the most frames in flight seen (takeCounters starts its peak again each time)

uint32_t inFlight()
{
    const swlan::DataCounters d = gMb.takeCounters();
    gPeak = d.txPeak > gPeak ? d.txPeak : gPeak;
    return d.txInFlight;
}

void resetCounts()
{
    gCount = {};
    gBatches = gResumes = gPeak = 0;
}

// What a scenario measured: the counts, then what each scenario adds.
struct Result {
    uint64_t frames = 0, tcmReads = 0, tcmWrites = 0, regReads = 0, regWrites = 0, doorbells = 0, msis = 0;
    uint64_t copiedBytes = 0, indexReads = 0;
    unsigned rxPosted = 0, rxBuffers = 0, txBuffers = 0, txInFlight = 0, txPeak = 0;
    uint64_t txFull = 0, txRefused = 0, rxStarved = 0;
    unsigned minPosted = 0, batches = 0, resumes = 0;
    // attach, small
    size_t postedAtAttach = 0, lines = 0;
    std::string describe, log;
    // rx, tx
    bool intact = false;
    size_t queuedLeft = 0;
    // mixed
    unsigned acks = 0;
    size_t acksSeen = 0, received = 0;
    // eapol
    uint32_t id = 0;
    bool sentBefore = false, sentAfter = false, seen = false;
    unsigned status = 0;
};

Result counts(uint64_t frames)
{
    const swlan::DataCounters d = gMb.takeCounters();
    Result r;
    r.frames = frames;
    r.tcmReads = gCount.tcmReads;
    r.tcmWrites = gCount.tcmWrites;
    r.regReads = gCount.regReads;
    r.regWrites = gCount.regWrites;
    r.doorbells = gCount.doorbells;
    r.msis = gCount.msis;
    r.copiedBytes = gCount.copiedBytes;
    r.indexReads = d.indexReads;
    r.rxPosted = d.rxPosted;
    r.rxBuffers = gMb.rxBufferCount();
    r.txBuffers = gMb.txBufferCount();
    r.txInFlight = d.txInFlight;
    r.txPeak = d.txPeak > gPeak ? d.txPeak : gPeak;
    r.txFull = d.txFull;
    r.txRefused = d.txRefused;
    r.rxStarved = gFw.rxStarved;
    r.minPosted = gFw.minPosted == ~0u ? 0 : gFw.minPosted;
    r.batches = gBatches;
    r.resumes = gResumes;
    return r;
}

// ---- scenarios -------------------------------------------------------------------------------------------------------
Result attachScenario()
{
    REQUIRE(bringUp());
    char layout[200];
    gMb.describe(layout, sizeof(layout));
    Result r = counts(0);
    r.postedAtAttach = gFw.rxBuffers.size();
    r.lines = gLog.size();
    r.describe = layout;
    return r;
}

// A download: bursts of `burst` frames (an A-MPDU's worth) per interrupt, 1500-byte frames, every one delivered and
// intact, the receive buffers back with the chip after each interrupt.
Result rxScenario(unsigned frames, unsigned burst)
{
    REQUIRE(bringUp());
    resetCounts();
    gReceived.clear();
    unsigned sent = 0;
    while (sent < frames) {
        for (unsigned i = 0; i < burst && sent < frames; ++i, ++sent)
            gFw.receive(makeFrame(sent, 1514));
        firmware();  // the interrupt, then the reposts it rang for
        firmware();
    }
    bool intact = gReceived.size() == frames;
    for (unsigned i = 0; intact && i < frames; ++i)
        intact = gReceived[i].bytes == makeFrame(i, 1514).bytes;
    Result r = counts(frames);
    r.intact = intact;
    return r;
}

// An upload: `frames` queued at once (TCP's window), the firmware fetching at most `hold` and the air completing
// `perAir` per turn. Nothing may be dropped: what does not fit waits in the queue until a completion resumes it.
Result txScenario(unsigned frames, unsigned hold, unsigned perAir)
{
    REQUIRE(bringUp());
    REQUIRE(openFlow());
    resetCounts();
    gFw.holdLimit = hold;
    for (unsigned i = 0; i < frames; ++i)
        gQueue.push_back(makeFrame(i, 1514));
    start();
    for (unsigned turns = 0; (!gQueue.empty() || !gFw.txHeld.empty() || inFlight()) && turns < 100000; ++turns) {
        firmware();
        gFw.transmitted(perAir);
        firmware();
    }
    bool intact = gFw.txSeen.size() == frames && gQueue.empty();
    for (unsigned i = 0; intact && i < frames; ++i)
        intact = gFw.txSeen[i].bytes == makeFrame(i, 1514).bytes;
    Result r = counts(frames);
    r.intact = intact;
    r.queuedLeft = gQueue.size();
    return r;
}

// A download with its ACKs: one 66-byte ACK queued per two frames received, both directions through the same
// interrupts.
Result mixedScenario(unsigned frames, unsigned burst)
{
    REQUIRE(bringUp());
    REQUIRE(openFlow());
    resetCounts();
    gReceived.clear();
    gFw.holdLimit = 32;
    unsigned sent = 0, acks = 0;
    while (sent < frames) {
        for (unsigned i = 0; i < burst && sent < frames; ++i, ++sent)
            gFw.receive(makeFrame(sent, 1514));
        firmware();
        while (acks < sent / 2)
            gQueue.push_back(makeFrame(100000 + acks++, 66));
        start();
        firmware();
        gFw.transmitted(burst / 2);
        firmware();
    }
    for (unsigned turns = 0; (!gQueue.empty() || !gFw.txHeld.empty()) && turns < 1000; ++turns) {
        firmware();
        gFw.transmitted(64);
        firmware();
    }
    Result r = counts(static_cast<uint64_t>(frames) + acks);
    r.acks = acks;
    r.acksSeen = gFw.txSeen.size();
    r.received = gReceived.size();
    return r;
}

// The supplicant's handshake frame: sent at once, its completion's status read back by id.
Result eapolScenario()
{
    REQUIRE(bringUp());
    REQUIRE(openFlow());
    const Frame f = makeFrame(7, 121);
    const uint32_t id = gMb.send(f.bytes.data(), f.bytes.size());
    uint16_t status = 0xFFFF;
    const bool before = gMb.sent(id, &status);
    firmware();
    gFw.transmitted(1, 0);
    firmware();
    const bool after = gMb.sent(id, &status);
    Result r = counts(1);
    r.id = id;
    r.sentBefore = before;
    r.sentAfter = after;
    r.status = status;
    r.seen = gFw.txSeen.size() == 1 && gFw.txSeen[0].bytes == f.bytes;
    return r;
}

// No 2 MiB contiguous: the old layout, one line.
Result smallScenario()
{
    gFailAllocationsFrom = swlan::kAreaLarge.length;
    gLog.clear();
    REQUIRE(bringUp());
    Result r = counts(0);
    r.postedAtAttach = gFw.rxBuffers.size();
    r.lines = gLog.size();
    r.log = gLog.empty() ? "" : gLog.front();
    return r;
}

// SwitchWLANLink.h: chanspecs, an access point's elements, the best rate, a dump's counters.
void linkScenario()
{
    swlan_chanspec a, b, c, d;
    const bool okA = swlan_chanspec_decode(0x1006, SWLAN_IOTYPE_D11AC, &a);  // 2.4 GHz 6, 20 MHz
    const bool okB = swlan_chanspec_decode(0xE09B, SWLAN_IOTYPE_D11AC, &b);  // 5 GHz 80 MHz centre 155, ctl 149
    const bool okC = swlan_chanspec_decode(0xD926, SWLAN_IOTYPE_D11AC, &c);  // 5 GHz 40 MHz centre 38, upper: ctl 40
    const bool okD =
        swlan_chanspec_decode(0x2E06, SWLAN_IOTYPE_D11N, &d);  // d11n 2.4 GHz 40 MHz centre 6, upper: ctl 8
    // A VHT80 2x2 access point on 149: HT caps (SGI20/40, 2 streams), HT op (secondary above), VHT caps (SGI80,
    // 2 streams), VHT op (80 MHz, centre 155), HE caps.
    const uint8_t ie[] = {0,    4,    'h', 'o',  'm', 'e', 45, 26,  0x6F, 0x00, 0x17, 0xFF, 0xFF, 0,    0,    0, 0,
                          0,    0,    0,   0,    0,   0,   0,  0,   0,    0,    0,    0,    0,    0,    0,    0, 0,
                          61,   22,   149, 0x05, 0,   0,   0,  0,   0,    0,    0,    0,    0,    0,    0,    0, 0,
                          0,    0,    0,   0,    0,   0,   0,  191, 12,   0x20, 0,    0,    0,    0xFA, 0xFF, 0, 0,
                          0xFA, 0xFF, 0,   0,    192, 5,   1,  155, 0,    0,    0,    255,  3,    35,   0,    0};
    swlan_peer ap;
    swlan_peer_parse(ie, sizeof(ie), &ap);
    char text[160];
    swlan_peer_describe(&ap, text, sizeof(text));
    const char dump[] = "AMPDU Tx counters:\ntxampdu 120 txmpdu 1500 txmpduperampdu 13 noba 0\nAMPDU Rx counters:\n"
                        "rxampdu 400 rxmpdu 9000 rxmpduperampdu 23\n";
    uint32_t txampdu = 0, txmpdu = 0, rxampdu = 0, rxmpdu = 0, missing = 0;
    const bool dumpOk = swlan_dump_value(dump, sizeof(dump), "txampdu", &txampdu) &&
                        swlan_dump_value(dump, sizeof(dump), "txmpdu", &txmpdu) &&
                        swlan_dump_value(dump, sizeof(dump), "rxampdu", &rxampdu) &&
                        swlan_dump_value(dump, sizeof(dump), "rxmpdu", &rxmpdu);
    const bool noMissing = !swlan_dump_value(dump, sizeof(dump), "mpdu", &missing);
    char tenths[16];
    swlan_tenths(txmpdu, txampdu, tenths, sizeof(tenths));
    using V = std::vector<unsigned>;
    const auto chanspec = [](bool ok, const swlan_chanspec &cs) {
        return V{ok, cs.control, cs.center, cs.bw_mhz, static_cast<unsigned>(cs.five_ghz)};
    };
    EXPECT_EQ(chanspec(okA, a), (V{1, 6, 6, 20, 0})) << "0x1006: 2.4 GHz 6, 20 MHz";
    EXPECT_EQ(chanspec(okB, b), (V{1, 149, 155, 80, 1})) << "0xE09B: 5 GHz 80 MHz centre 155, ctl 149";
    EXPECT_EQ(chanspec(okC, c), (V{1, 40, 38, 40, 1})) << "0xD926: 5 GHz 40 MHz centre 38, upper: ctl 40";
    EXPECT_EQ(chanspec(okD, d), (V{1, 8, 6, 40, 0})) << "0x2E06 (d11n): 2.4 GHz 40 MHz centre 6, upper: ctl 8";
    EXPECT_STREQ(text, "HT 2 SS 40 MHz SGI, VHT 2 SS 80 MHz SGI, HE");
    EXPECT_EQ(swlan_link_best_kbps(&ap, 80), 866600u);
    EXPECT_EQ(swlan_link_best_kbps(&ap, 20), 173400u);
    EXPECT_EQ((V{dumpOk, txampdu, txmpdu, rxampdu, rxmpdu, noMissing}), (V{1, 120, 1500, 400, 9000, 1}));
    EXPECT_STREQ(tenths, "12.5");
}

}  // namespace

// ---- the Chip methods Msgbuf calls (SwitchWLANChip.cpp's, over the model) ----------------------------------------------
bool switch_wlan_dma_map(uint64_t pa, size_t length, char *why, size_t whyLength)
{
    ++gDmaMapCalls;
    gDmaMappedPa = pa;
    gDmaMappedLength = length;
    gDmaWritesAtMap = gCount.tcmWrites;
    if (gDmaMapCalls > 1) {
        gDmaContentsAtMap[0] = gAreaBytes[0];
        gDmaContentsAtMap[1] = gAreaBytes[gAreaLength - 1];
    }
    if (gDmaMapFailure) {
        gAreaBytes[0] = 0xa5;
        gAreaBytes[gAreaLength - 1] = 0x5a;
        snprintf(why, whyLength, "injected DMA mapping refusal");
        return false;
    }
    return true;
}

void swlan_test_mmio(const void *, char kind, uint32_t)
{
    ++(kind == 'r' ? gCount.tcmReads : gCount.tcmWrites);
}

namespace swlan {
bool Chip::attach(TegraPcie *, uint32_t, uint32_t, char *, size_t)
{
    tcm_.base = gTcm;
    shared_ = gSharedAddress;
    ramBase_ = gRamBase;
    ramSize_ = gRamSize;
    sharedFlags_ = 0x20005;
    coreCount_ = 1;
    cores_[0] = {0x83C, 7, kPcie2Base, kPcie2Base + 0x100000};
    return true;
}

uint32_t Chip::read(uint32_t address)
{
    ++gCount.regReads;
    return gFw.reg(address - kPcie2Base);
}

void Chip::write(uint32_t address, uint32_t value)
{
    ++gCount.regWrites;
    gFw.reg(address - kPcie2Base, value);
}

const Core *Chip::find(uint16_t coreId) const
{
    for (unsigned i = 0; i < coreCount_; ++i)
        if (cores_[i].id == coreId)
            return &cores_[i];
    return nullptr;
}
}  // namespace swlan

// --- One process per scenario (the driver's Msgbuf, the chip and the firmware model are globals) ---------------------
using switch_ios::test::ExpectPassesInFreshProcess;
constexpr uint64_t kFrame = 1514;

// Attach posts every receive buffer in one doorbell.
TEST(SwitchWlanMsgbuf, AttachPostsEveryReceiveBufferInOneDoorbell)
{
    ExpectPassesInFreshProcess([] {
        const Result r = attachScenario();
        EXPECT_EQ(gDmaMapCalls, 1u);
        EXPECT_EQ(gDmaMappedPa, kFakeAreaPA);
        EXPECT_EQ(gDmaMappedLength, gAreaLength);
        EXPECT_EQ(gDmaMappedLength, 2u * 1024 * 1024);
        EXPECT_EQ(gDmaWritesAtMap, 0u);
        EXPECT_EQ(r.rxBuffers, 255u);  // the firmware's max_rxbufpost (was 64)
        EXPECT_EQ(r.postedAtAttach, 255u);
        EXPECT_EQ(r.txBuffers, 256u);  // was 32
        EXPECT_EQ(r.lines, 0u);
        EXPECT_EQ(r.doorbells, 1u + 16u);  // the receive buffers together, then each of 16 control buffers
        EXPECT_NE(r.describe.find("2048 KiB DMA area"), std::string::npos) << r.describe;
        EXPECT_NE(r.describe.find("255 receive buffers posted (the firmware takes 255)"), std::string::npos)
            << r.describe;
    });
}

TEST(SwitchWlanMsgbuf, MappingFailureLeavesTheAreaUnchangedAndPublishesNoDeviceAddress)
{
    ExpectPassesInFreshProcess([] {
        resetTcm();
        gCount = {};
        gDmaMapFailure = true;
        char why[200] = {};
        REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
        EXPECT_FALSE(gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof why));
        EXPECT_STREQ(why, "injected DMA mapping refusal");
        EXPECT_EQ(gDmaMapCalls, 1u);
        EXPECT_EQ(gDmaMappedPa, kFakeAreaPA);
        EXPECT_EQ(gDmaMappedLength, gAreaLength);
        EXPECT_EQ(gCount.tcmWrites, 0u);
        EXPECT_EQ(gCount.regWrites, 0u);
        EXPECT_EQ(gAreaBytes[0], 0xa5u);
        EXPECT_EQ(gAreaBytes[gAreaLength - 1], 0x5au);
        EXPECT_FALSE(gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof why));
        EXPECT_EQ(gDmaMapCalls, 2u);
        EXPECT_EQ(gDmaContentsAtMap[0], 0xa5u);
        EXPECT_EQ(gDmaContentsAtMap[1], 0x5au);
        EXPECT_EQ(gCount.tcmWrites, 0u);
        gDmaMapFailure = false;
        REQUIRE(bringUp());
        EXPECT_EQ(gDmaMapCalls, 3u);
        EXPECT_EQ(gFw.rxBuffers.size(), gMb.rxBufferCount());
    });
}

TEST(SwitchWlanMsgbuf, AwdlRingBindingRangeTracksFirmwareSubmissionCapacity)
{
    ExpectPassesInFreshProcess([] {
        const uint16_t capacity[] = {3, 4, 7};
        const unsigned expected[] = {0, 1, 4};
        for (unsigned i = 0; i < 3; ++i) {
            REQUIRE(bringUp(capacity[i]));
            EXPECT_EQ(gMb.awdlFirstRing(), 3u);
            EXPECT_EQ(gMb.awdlRingCount(), expected[i]);
            EXPECT_EQ(gMb.awdlAvailable(), expected[i] != 0);
            if (gMb.awdlAvailable())
                EXPECT_LE(gMb.awdlFirstRing() + gMb.awdlRingCount(), capacity[i]);
        }
    });
}

// A download: every frame intact and its buffer back.
TEST(SwitchWlanMsgbuf, DownloadEveryFrameIntactAndBuffersBack)
{
    ExpectPassesInFreshProcess([] {
        const Result r = rxScenario(4000, 32);
        EXPECT_TRUE(r.intact);
        EXPECT_EQ(r.rxStarved, 0u);
        EXPECT_EQ(r.rxPosted, r.rxBuffers);  // every buffer back with the chip
        // One CPU copy per frame (out of the DMA buffer into the mbuf), nothing else.
        EXPECT_EQ(r.copiedBytes, 4000 * kFrame);
        // Per frame before r17: 2 index reads (write index per item, read index per repost) and 1 doorbell. Now about
        // one write-index read and one doorbell per interrupt's batch of 32.
        EXPECT_LE(static_cast<double>(r.tcmReads) / static_cast<double>(r.frames), 0.15);
        EXPECT_LE(static_cast<double>(r.doorbells) / static_cast<double>(r.frames), 1.0 / 16);
        EXPECT_GE(static_cast<double>(r.frames) / static_cast<double>(r.msis), 16.0);
    });
}

// Each frame's interface and type reach the handler: the type is the completion's le16 flags at +18 (brcmfmac's
// BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_3 = 1, FRAME_802_11 = 2, which monitor mode sets), not its interface byte.
TEST(SwitchWlanMsgbuf, ReceiveCarriesInterfaceAndFrameType)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        gReceived.clear();
        Frame station = makeFrame(1, 100);
        Frame monitor = makeFrame(2, 60);
        monitor.kind = 2;
        Frame other = makeFrame(3, 80);
        other.ifidx = 1;
        gFw.receive(station);
        gFw.receive(monitor);
        gFw.receive(other);
        firmware();
        ASSERT_EQ(gReceived.size(), 3u);
        EXPECT_EQ(gReceived[0].kind, 1u);
        EXPECT_EQ(gReceived[0].ifidx, 0u);
        EXPECT_EQ(gReceived[1].kind, 2u);
        EXPECT_EQ(gReceived[1].bytes, monitor.bytes);
        EXPECT_EQ(gReceived[2].ifidx, 1u);
        EXPECT_EQ(gReceived[2].kind, 1u);
    });
}

TEST(SwitchWlanMsgbuf, InvalidSharedReceiveOffsetIsRejectedBeforePublishingDma)
{
    ExpectPassesInFreshProcess([] {
        for (const uint32_t offset : {2048u, 8192u, UINT32_MAX}) {
            resetTcm();
            setTcm32(kShared + 36, offset);
            gCount = {};
            char why[200] = {};
            REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
            EXPECT_FALSE(gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof why));
            EXPECT_NE(std::string(why).find("invalid shared receive offset"), std::string::npos);
            EXPECT_EQ(gCount.tcmWrites, 0u);
            EXPECT_EQ(gAreaBytes, nullptr);
        }
        REQUIRE(bringUp(40, 2047));
        EXPECT_EQ(gFw.rxBuffers.size(), gMb.rxBufferCount());
    });
}

TEST(SwitchWlanMsgbuf, FirmwarePointerSpansAreValidatedBeforeDmaPublication)
{
    ExpectPassesInFreshProcess([] {
        char why[200] = {};
        gRamBase = 0x180000;
        gRamSize = 0xc0000;
        REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
        EXPECT_TRUE(gChip.containsRam(gRamBase, gRamSize));
        EXPECT_TRUE(gChip.containsRam(gRamBase + gRamSize - 80, 80));
        EXPECT_FALSE(gChip.containsRam(gRamBase + gRamSize - 76, 80));
        EXPECT_FALSE(gChip.containsRam(UINT32_MAX - 3, 80));
        EXPECT_FALSE(gChip.containsRam(gRamBase, UINT32_MAX));
        gRamSize = swlan::Chip::kTcmLength;
        REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
        EXPECT_FALSE(gChip.containsRam(gRamBase, 4));
        gRamBase = UINT32_MAX - 3;
        gRamSize = 80;
        REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
        EXPECT_FALSE(gChip.containsRam(gRamBase, 4));
        gRamBase = 0;
        gRamSize = kTcmSize;
        const struct { uint32_t field, value; } invalid[] = {
            {0, kTcmSize - 4}, {0, UINT32_MAX - 3}, {0, kShared + 1},
            {kShared + 48, kTcmSize - 4}, {kShared + 48, UINT32_MAX - 3},
            {kShared + 48, kRingInfo + 1}, {kRingInfo, kTcmSize - 16},
            {kRingInfo, kRingMem + 2}, {kRingInfo + 4, kTcmSize - 4},
            {kRingInfo + 8, UINT32_MAX - 3}, {kRingInfo + 8, kH2DR + 1},
            {kRingInfo + 12, kTcmSize - 4}, {kRingInfo + 16, kTcmSize - 4},
            {kShared + 44, kTcmSize}, {kShared + 44, kDtohMb + 1},
        };
        for (const auto &entry : invalid) {
            resetTcm();
            gSharedAddress = entry.field ? kShared : entry.value;
            if (entry.field)
                setTcm32(entry.field, entry.value);
            gCount = {};
            REQUIRE(gChip.attach(nullptr, 0, 0, why, sizeof why));
            EXPECT_FALSE(gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof why));
            EXPECT_NE(std::string(why).find("outside firmware RAM"), std::string::npos);
            EXPECT_EQ(gCount.tcmWrites, 0u);
            EXPECT_EQ(gDmaMapCalls, 0u);
        }
        gSharedAddress = kShared;
        REQUIRE(bringUp(40));
    });
}

TEST(SwitchWlanMsgbuf, EventCapacityIsCheckedBeforeCacheAccessAndTheBufferIsRecycled)
{
    ExpectPassesInFreshProcess([] {
        constexpr uint32_t offset = 32, capacity = 8192 - offset, header = 72;
        REQUIRE(bringUp(40, offset));
        std::vector<uint8_t> event(capacity, 0x5a);
        event[12] = 0x88;
        event[13] = 0x6c;
        memset(event.data() + 44, 0xff, 4);
        gFw.event(event, capacity);
        firmware();
        gFw.serviceH2D();
        ASSERT_EQ(gEventData.size(), 1u);
        EXPECT_EQ(gEventData[0], std::vector<uint8_t>(event.begin() + header, event.end()));
        EXPECT_EQ(gFw.eventBuffers.size(), 8u);
        const std::vector<uint8_t> prefix(event.begin(), event.begin() + header);
        for (const uint32_t length : {capacity + 1, static_cast<uint32_t>(UINT16_MAX)}) {
            gFw.event(prefix, static_cast<uint16_t>(length));
            firmware();
            gFw.serviceH2D();
            EXPECT_EQ(gEventData.size(), 1u);
            EXPECT_EQ(gFw.eventBuffers.size(), 8u);
        }
        gFw.event(prefix, 0);
        firmware();
        gFw.serviceH2D();
        EXPECT_EQ(gEventData.size(), 1u);
        gFw.event(event, capacity);
        firmware();
        gFw.serviceH2D();
        EXPECT_EQ(gEventData.size(), 2u);
        EXPECT_EQ(gFw.eventBuffers.size(), 8u);
    });
}

TEST(SwitchWlanMsgbuf, InvalidReceiveRangesAreDroppedAndEveryBufferIsReturned)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        const Frame prefix = makeFrame(1, 64), empty;
        for (const auto [offset, length] :
             {std::pair<uint16_t, uint16_t>{1, 2048}, {2047, 2}, {2048, 14}, {UINT16_MAX, UINT16_MAX}}) {
            REQUIRE(gFw.receiveMetadata(offset == 1 ? prefix : empty, offset, length));
            firmware();
            gFw.serviceH2D();
            EXPECT_TRUE(gReceived.empty());
            EXPECT_EQ(gFw.rxBuffers.size(), gMb.rxBufferCount());
            EXPECT_EQ(gMb.takeCounters().rxPosted, gMb.rxBufferCount());
        }
        REQUIRE(gFw.receive(prefix));
        firmware();
        gFw.serviceH2D();
        ASSERT_EQ(gReceived.size(), 1u);
        EXPECT_EQ(gReceived[0].bytes, prefix.bytes);
    });
}

TEST(SwitchWlanMsgbuf, SharedAndExplicitOffsetsPreserveStationAndMaximumA3Frames)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp(40, 32));
        const Frame station = makeFrame(1, 100);
        REQUIRE(gFw.receive(station));
        Frame raw = makeFrame(2, sizeof(switch_a3_raw_rx) + SWITCH_A3_MAX_FRAME);
        raw.kind = 2;
        switch_a3_raw_rx header = {};
        header.magic = SWITCH_A3_RAW_RX_MAGIC;
        header.version = SWITCH_A3_RAW_RX_VERSION;
        header.header_bytes = sizeof header;
        header.frame_bytes = SWITCH_A3_MAX_FRAME;
        header.flags = SWITCH_A3_RX_FCS_PRESENT;
        header.session = 1;
        header.clock_epoch = 2;
        header.config_generation = 3;
        header.tsf = 1;
        memcpy(raw.bytes.data(), &header, sizeof header);
        REQUIRE(gFw.receiveMetadata(raw, static_cast<uint16_t>(2048 - raw.bytes.size()),
                                    static_cast<uint16_t>(raw.bytes.size())));
        firmware();
        gFw.serviceH2D();
        ASSERT_EQ(gReceived.size(), 2u);
        EXPECT_EQ(gReceived[0].bytes, station.bytes);
        EXPECT_EQ(gReceived[1].bytes, raw.bytes);
        EXPECT_EQ(gReceived[1].kind, 2u);
        size_t frameLength = 0;
        EXPECT_TRUE(
            switch_a3_rx_decode(gReceived[1].bytes.data(), gReceived[1].bytes.size(), 1, 2, 3, &header, &frameLength));
        EXPECT_EQ(frameLength, SWITCH_A3_MAX_FRAME - 4);
        EXPECT_EQ(gFw.rxBuffers.size(), gMb.rxBufferCount());
    });
}

// brcmf_msgbuf_tx_ioctl's lengths: a request up to the message size goes whole; a longer one (actframe's 1837 bytes)
// declares its length as the output length and carries the first 1518 bytes.
TEST(SwitchWlanMsgbuf, LongIoctlDeclaresItsLengthAndCarriesTheMessageSize)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        static uint8_t request[1837];
        for (size_t i = 0; i < sizeof(request); ++i)
            request[i] = static_cast<uint8_t>(i * 7);
        char why[200] = "";
        REQUIRE(gMb.ioctlSubmit(263, 0, request, 40, 0, why, sizeof(why)));
        firmware();
        EXPECT_EQ(gFw.ioctlIn, 40u);
        EXPECT_EQ(gFw.ioctlOut, 40u);
        REQUIRE(gMb.ioctlSubmit(263, 0, request, sizeof(request), 0, why, sizeof(why)));
        firmware();
        EXPECT_EQ(gFw.ioctlIn, 1518u);
        EXPECT_EQ(gFw.ioctlOut, 1837u);
        EXPECT_EQ(gFw.ioctlBytes, std::vector<uint8_t>(request, request + 1518));
    });
}

TEST(SwitchWlanMsgbuf, IoctlRejectsMismatchedCompletionsAndRecyclesTheirBuffers)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        gFw.holdIoctl = true;
        const uint8_t request[4] = {};
        char why[200] = "";
        REQUIRE(gMb.ioctlSubmit(262, 0, request, sizeof(request), 4, why, sizeof(why)));
        firmware();
        const uint16_t transaction = gFw.ioctlTrans;
        for (unsigned i = 0; i < 16; ++i) {
            gFw.completeIoctl(static_cast<uint16_t>(transaction - 1), 262);
            firmware();
            EXPECT_FALSE(gMb.replyReady());
        }
        gFw.completeIoctl(transaction, 263);
        firmware();
        EXPECT_FALSE(gMb.replyReady());
        gFw.serviceH2D();
        EXPECT_EQ(gFw.ioctlBuffers.size(), 8u);
        gFw.completeIoctl(transaction, 262, 0x87654321, -23);
        firmware();
        REQUIRE(gMb.replyReady());
        EXPECT_EQ(gMb.replyStatus(), -23);
        EXPECT_EQ(gMb.replyLength(), 4u);
        EXPECT_EQ(le32(gMb.replyData()), 0x87654321u);
        gFw.completeIoctl(transaction, 262, 0xDEADBEEF);
        firmware();
        EXPECT_EQ(gMb.replyStatus(), -23);
        EXPECT_EQ(le32(gMb.replyData()), 0x87654321u);
        gFw.serviceH2D();
        EXPECT_EQ(gFw.ioctlBuffers.size(), 8u);
    });
}

TEST(SwitchWlanMsgbuf, RawEnvelopeRemainsCallerOwnedUntilActualDmaPost)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        REQUIRE(openFlow());
        REQUIRE(gMb.awdlAvailable());
        gFw.holdLimit = 4096;
        const unsigned buffers = gMb.txBufferCount();
        for (unsigned i = 0; i < buffers; ++i) {
            const Frame frame = makeFrame(i, 96);
            REQUIRE(gMb.send(frame.bytes.data(), frame.bytes.size()));
        }
        firmware();
        REQUIRE(gFw.txSeen.size() == buffers);
        Frame raw = makeFrame(0xa3, 1600);
        memset(raw.bytes.data() + 6, 0x57, 6);
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) < 0);
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        firmware();
        EXPECT_EQ(memcmp(gFw.lastFlowSource, kUs, sizeof kUs), 0);
        EXPECT_EQ(gFw.txSeen.size(), buffers);
        EXPECT_LT(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs), 0);
        gFw.transmitted(1);
        firmware();
        EXPECT_EQ(gFw.txSeen.size(), buffers);
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) == 1);
        firmware();
        REQUIRE(gFw.txSeen.size() == buffers + 1);
        EXPECT_EQ(gFw.txSeen.back().bytes, raw.bytes);
        EXPECT_EQ(gMb.takeCounters().txInFlight, buffers);
    });
}

TEST(SwitchWlanMsgbuf, PrivateOwnershipWaitsForDmaAndRingDeleteIndependentlyOfRadioStatus)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        REQUIRE(openFlow());
        gFw.holdLimit = 4096;
        const Frame ordinary = makeFrame(1, 96), raw = makeFrame(2, 96);
        REQUIRE(gMb.send(ordinary.bytes.data(), ordinary.bytes.size()));
        firmware();
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        EXPECT_TRUE(gMb.awdlClosed());
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) < 0);
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        EXPECT_FALSE(gMb.awdlClosed());
        gMb.awdlClose();
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        EXPECT_FALSE(gMb.awdlClosed());
        firmware();
        EXPECT_TRUE(gMb.awdlClosed());
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) < 0);
        firmware();
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) == 1);
        firmware();
        REQUIRE(gFw.txHeld.size() == 2);
        const auto pending = gFw.txHeld.back();
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
        switch_a3_tx_status result = {};
        result.magic = SWITCH_A3_TX_STATUS_MAGIC;
        result.version = SWITCH_A3_WIRE_VERSION;
        result.header_bytes = sizeof result;
        result.total_bytes = sizeof result;
        result.status = SWITCH_A3_TX_SENT;
        Frame radio;
        const auto *bytes = reinterpret_cast<const uint8_t *>(&result);
        radio.bytes.assign(bytes, bytes + sizeof result);
        radio.kind = 2;
        REQUIRE(gFw.receive(radio));
        interrupts();
        REQUIRE(gReceived.size() == 1);
        EXPECT_EQ(gReceived.back().bytes, radio.bytes);
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
        gMb.awdlClose();
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
        EXPECT_FALSE(gMb.awdlClosed());
        firmware();
        EXPECT_TRUE(gMb.awdlClosed());
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
        gFw.completeTx(pending.id, pending.ring);
        interrupts();
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        EXPECT_EQ(gMb.takeCounters().txInFlight, 1u);
        gFw.completeTx(pending.id, pending.ring);
        interrupts();
        EXPECT_EQ(gMb.awdlOutstanding(), 0u);
        EXPECT_EQ(gMb.takeCounters().txInFlight, 1u);
    });
}

TEST(SwitchWlanMsgbuf, NativeAwdlRetainsItsFirstFrameWhileFlowIsCreated)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        const Frame frame = makeFrame(0xa4, 96);
        REQUIRE(gMb.awdlSend(1, frame.bytes.data(), frame.bytes.size()) == 0);
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
        firmware();
        firmware();
        REQUIRE(gFw.txSeen.size() == 1);
        EXPECT_EQ(gFw.txSeen[0].bytes, frame.bytes);
        EXPECT_EQ(gMb.awdlOutstanding(), 1u);
    });
}

TEST(SwitchWlanMsgbuf, FullControlRingDefersDeletionUntilAnInterruptReturnsCredits)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        const Frame raw = makeFrame(3, 96);
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) < 0);
        firmware();
        firmware();
        gFw.holdControlCredits = true;
        char why[200] = "";
        const uint8_t request[4] = {};
        unsigned queued = 0;
        while (gMb.ioctlSubmit(262, 0, request, sizeof request, 4, why, sizeof why)) {
            REQUIRE(++queued < 64);
            firmware();
            REQUIRE(gMb.replyReady());
        }
        gMb.awdlClose();
        EXPECT_FALSE(gMb.awdlClosed());
        EXPECT_EQ(gFw.deletes, 0u);
        gFw.holdDeletes = true;
        gFw.holdControlCredits = false;
        setTcm16(kH2DR, gFw.h2d[0].pos);
        uint8_t returnedCredit[24] = {};
        returnedCredit[0] = 0x0a;
        REQUIRE(gFw.push(0, returnedCredit));
        interrupts();
        firmware();
        EXPECT_EQ(gFw.deletes, 1u);
        EXPECT_FALSE(gMb.awdlClosed());
        gMb.awdlClose();
        firmware();
        EXPECT_EQ(gFw.deletes, 1u);
        gFw.completeDelete(gMb.awdlFirstRing());
        interrupts();
        EXPECT_TRUE(gMb.awdlClosed());
    });
}

TEST(SwitchWlanMsgbuf, FailedRingDeletionRetainsOwnershipWithoutAnInterruptRetryLoop)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        EXPECT_FALSE(gMb.awdlFailed());
        const Frame raw = makeFrame(4, 96);
        REQUIRE(gMb.awdlSend(0, raw.bytes.data(), raw.bytes.size(), kUs) < 0);
        firmware();
        gFw.deleteStatus = 1;
        gMb.awdlClose();
        firmware();
        EXPECT_EQ(gFw.deletes, 1u);
        EXPECT_FALSE(gMb.awdlClosed());
        EXPECT_TRUE(gMb.awdlFailed());
        gMb.awdlClose();
        firmware();
        EXPECT_EQ(gFw.deletes, 1u);
        EXPECT_FALSE(gMb.awdlClosed());
        EXPECT_TRUE(gMb.awdlFailed());
    });
}

TEST(SwitchWlanMsgbuf, RecoveryRequestWaitsForTheRealIoctlCompletionWithoutMaskingIt)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        gFw.holdIoctl = true;
        const uint8_t request[] = {1, 2, 3, 4};
        char why[200] = {};
        REQUIRE(gMb.ioctlSubmit(262, 0, request, sizeof request, 4, why, sizeof why));
        firmware();
        swlan::SwitchRadioRecovery recovery;
        bool ioctlBusy = true;
        recovery.request();
        EXPECT_TRUE(recovery.watchdogDue(true, false, false, 1, 5000000000ULL));
        EXPECT_FALSE(recovery.canSubmit(true, false, false));
        EXPECT_FALSE(recovery.canReset(ioctlBusy));
        EXPECT_TRUE(recovery.canService(true, false, false));
        EXPECT_EQ(gFw.ioctlBytes, std::vector<uint8_t>(request, request + sizeof request));
        gFw.completeIoctl(gFw.ioctlTrans, 262, 0x12345678);
        if (recovery.canService(true, false, false))
            interrupts();
        REQUIRE(gMb.replyReady());
        EXPECT_EQ(le32(gMb.replyData()), 0x12345678u);
        EXPECT_EQ(gFw.mbMask, 0x00ff0300u);
        ioctlBusy = false;
        EXPECT_TRUE(recovery.canReset(ioctlBusy));
        EXPECT_FALSE(recovery.canSubmit(true, false, false));
    });
}

TEST(SwitchWlanMsgbuf, PendingIoctlKeepsItsInputBufferAndCannotAnswerTheNextRequest)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        gFw.holdIoctl = true;
        const uint8_t request[4] = {1, 2, 3, 4};
        const uint8_t next[4] = {5, 6, 7, 8};
        char why[200] = "";
        REQUIRE(gMb.ioctlSubmit(262, 0, request, sizeof(request), 4, why, sizeof(why)));
        EXPECT_FALSE(gMb.ioctlSubmit(262, 0, next, sizeof(next), 4, why, sizeof(why)));
        firmware();
        EXPECT_EQ(gFw.ioctlBytes, std::vector<uint8_t>(request, request + sizeof(request)));
        const uint16_t previous = gFw.ioctlTrans;
        gFw.completeIoctl(previous, 262);
        firmware();
        REQUIRE(gMb.replyReady());
        REQUIRE(gMb.ioctlSubmit(262, 0, next, sizeof(next), 4, why, sizeof(why)));
        firmware();
        EXPECT_EQ(gFw.ioctlBytes, std::vector<uint8_t>(next, next + sizeof(next)));
        const uint16_t current = gFw.ioctlTrans;
        EXPECT_NE(current, previous);
        gFw.completeIoctl(previous, 262, 0xDEADBEEF, -23);
        firmware();
        EXPECT_FALSE(gMb.replyReady());
        gFw.completeIoctl(current, 262);
        firmware();
        REQUIRE(gMb.replyReady());
        EXPECT_EQ(gMb.replyStatus(), 0);
        EXPECT_EQ(le32(gMb.replyData()), 0x12345678u);
        gFw.serviceH2D();
        EXPECT_EQ(gFw.ioctlBuffers.size(), 8u);
    });
}

TEST(SwitchWlanMsgbuf, ChipRecoveryForgetsThePendingIoctl)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        gFw.holdIoctl = true;
        const uint8_t request[4] = {};
        char why[200] = "";
        REQUIRE(gMb.ioctlSubmit(262, 0, request, sizeof(request), 4, why, sizeof(why)));
        firmware();
        const uint16_t previous = gFw.ioctlTrans;
        REQUIRE(bringUp());
        gFw.completeIoctl(previous, 262);
        firmware();
        EXPECT_FALSE(gMb.replyReady());
        REQUIRE(gMb.ioctlSubmit(262, 0, request, sizeof(request), 4, why, sizeof(why)));
        firmware();
        EXPECT_TRUE(gMb.replyReady());
        EXPECT_NE(gFw.ioctlTrans, previous);
    });
}

// Bursts of a full A-MPDU (beyond the 64 buffers the chip had before r17) never starve the chip.
TEST(SwitchWlanMsgbuf, DownloadBurstsOfAFullAmpduNeverStarveTheChip)
{
    ExpectPassesInFreshProcess([] {
        const Result r = rxScenario(2048, 128);
        EXPECT_TRUE(r.intact);
        EXPECT_EQ(r.rxStarved, 0u);
        EXPECT_GE(r.minPosted, 255u - 128u);
    });
}

// An upload: nothing dropped, frames wait for room.
TEST(SwitchWlanMsgbuf, UploadNothingDroppedFramesWaitForRoom)
{
    ExpectPassesInFreshProcess([] {
        const Result r = txScenario(4000, 64, 32);
        EXPECT_TRUE(r.intact);
        EXPECT_EQ(r.queuedLeft, 0u);
        EXPECT_EQ(r.txRefused, 0u);
        EXPECT_EQ(r.txInFlight, 0u);
        EXPECT_EQ(r.copiedBytes, 4000 * kFrame);  // one copy each, mbuf to DMA buffer (was two)
        EXPECT_LE(r.txPeak, r.txBuffers);
        EXPECT_LE(static_cast<double>(r.doorbells) / static_cast<double>(r.frames), 1.0 / 8);
    });
}

// The air takes 4 frames a turn while TCP queued 4000: every buffer goes into flight, en0's start routine stops, and
// completions resume it (no drop, no polling: the resume comes from the interrupt).
TEST(SwitchWlanMsgbuf, UploadIntoASlowAirRunsOutAndResumes)
{
    ExpectPassesInFreshProcess([] {
        const Result r = txScenario(4000, 512, 4);
        EXPECT_TRUE(r.intact);
        EXPECT_EQ(r.txPeak, r.txBuffers);
        EXPECT_GT(r.txFull, 0u);
        EXPECT_GT(r.resumes, 0u);
        EXPECT_EQ(r.txRefused, 0u);
    });
}

// A download with its ACKs through the same interrupts.
TEST(SwitchWlanMsgbuf, DownloadWithAcksThroughTheSameInterrupts)
{
    ExpectPassesInFreshProcess([] {
        const Result r = mixedScenario(4000, 32);
        EXPECT_EQ(r.received, 4000u);
        EXPECT_EQ(r.acksSeen, r.acks);
        EXPECT_EQ(r.txRefused, 0u);
        EXPECT_LE(static_cast<double>(r.tcmReads) / static_cast<double>(r.frames), 0.3);
        EXPECT_LE(static_cast<double>(r.doorbells) / static_cast<double>(r.frames), 0.25);
    });
}

// The handshake frame is sent and its status read.
TEST(SwitchWlanMsgbuf, HandshakeFrameSentAndItsStatusRead)
{
    ExpectPassesInFreshProcess([] {
        const Result r = eapolScenario();
        EXPECT_GT(r.id, 0u);
        EXPECT_FALSE(r.sentBefore);
        EXPECT_TRUE(r.sentAfter);
        EXPECT_EQ(r.status, 0u);
        EXPECT_TRUE(r.seen);
    });
}

// A posted buffer read back after the flush: what the chip fetched matches the CPU's copy, and a copy that does not
// match is counted with the first byte that differs.
TEST(SwitchWlanMsgbuf, TransmitBufferReadBackComparesWhatTheChipFetches)
{
    ExpectPassesInFreshProcess([] {
        REQUIRE(bringUp());
        REQUIRE(openFlow());
        const Frame f = makeFrame(3, 90);
        uint8_t *buffer = gMb.txBuffer();
        REQUIRE(buffer != nullptr);
        memcpy(buffer, f.bytes.data(), f.bytes.size());
        const uint32_t id = gMb.txPost(f.bytes.size());
        REQUIRE(id != 0);
        gMb.txFlush();
        gMb.txReadBack(id, f.bytes.data(), f.bytes.size());
        std::vector<uint8_t> other = f.bytes;
        other[41] ^= 0x20;
        gMb.txReadBack(id, other.data(), other.size());
        gMb.txReadBack(id, f.bytes.data(), swlan::kTxBufferSize + 1);
        const swlan::DataCounters d = gMb.takeCounters();
        EXPECT_EQ(d.txReread, 2u);
        EXPECT_EQ(d.txRereadDiffer, 1u);
        EXPECT_EQ(d.txRereadAt, 41u);
        firmware();
        EXPECT_EQ(gFw.txSeen.size(), 1u);
        if (!gFw.txSeen.empty())
            EXPECT_EQ(gFw.txSeen[0].bytes, f.bytes);
    });
}

// No contiguous 2 MiB: the old layout, with one line.
TEST(SwitchWlanMsgbuf, NoContiguous2MiBTakesTheOldLayoutWithOneLine)
{
    ExpectPassesInFreshProcess([] {
        const Result r = smallScenario();
        EXPECT_EQ(gDmaMappedLength, 512u * 1024);
        EXPECT_EQ(r.rxBuffers, 64u);
        EXPECT_EQ(r.txBuffers, 32u);
        EXPECT_EQ(r.postedAtAttach, 64u);
        EXPECT_EQ(r.lines, 1u);
        EXPECT_NE(r.log.find("2048 KiB contiguous below 4 GiB not available"), std::string::npos) << r.log;
    });
}

TEST(SwitchWlanMsgbuf, LinkDecoding)
{
    ExpectPassesInFreshProcess([] {
        linkScenario();
    });
}
