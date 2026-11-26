// UsbDmaCache.h on the host, against a model of one 64-byte cache line the XUSB host reads and writes without seeing
// the cache: memory, the CPU's cached copy (clean or dirty) and the whole line written back or dropped at once. The
// sequences are what IODMACommand gives a mapper (prepare -> iovmMapMemory, complete -> iovmUnmapMemory), with the
// CPU's read of an IN buffer after complete and also before it, since Apple's stack is written for IO-coherent SoCs.
// A buffer smaller than the line shares it with a neighbour the CPU reads and writes at every point of the transfer.
// The model fails a read that returns anything but what the controller wrote, a neighbour write that is lost, and a
// write-back that lands on what the controller wrote. A speculative fill may bring the line back at any point the CPU
// does not own it.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "../UsbDmaCache.h"

namespace {

constexpr uint64_t kLine = 64;

struct Line {
    std::array<uint8_t, kLine> memory{}, copy{};
    bool cached = false, dirty = false;
    bool clobbered = false;  // a write-back put stale bytes over what the controller wrote
    std::array<bool, kLine> deviceWrote{};

    void fill()
    {
        if (!cached) {
            copy = memory;
            cached = true;
            dirty = false;
        }
    }

    uint8_t cpuRead(uint32_t at)
    {
        fill();
        return copy[at];
    }

    void cpuWrite(uint32_t at, uint8_t v)
    {
        fill();
        copy[at] = v;
        dirty = true;
    }

    void writeBack()
    {
        if (!cached || !dirty)
            return;
        for (uint32_t i = 0; i < kLine; ++i) {
            if (deviceWrote[i] && copy[i] != memory[i])
                clobbered = true;
        }
        memory = copy;
        dirty = false;
    }

    void apply(UsbDmaCacheOp op)
    {
        if (op == UsbDmaCacheOp::None)
            return;
        writeBack();
        if (op == UsbDmaCacheOp::CleanAndInvalidate)
            cached = false;
    }

    void deviceWrite(uint32_t at, uint8_t v)
    {
        memory[at] = v;
        deviceWrote[at] = true;
    }
};

// One IN transfer of [offset, offset + length) in the line, the buffer at its own address or bounced as usbDmaPlan
// says. neighbour: a byte of the line outside the buffer, read and written by the CPU at each step (-1: none). The
// buffer is read after complete, or before it too when early. Returns whether the model saw a failure.
struct Run {
    uint32_t offset, length;
    int neighbour;
    bool early;
    bool fillDuringTransfer;
    uint32_t actual = UINT32_MAX;
};

bool failsWith(const Run &r)
{
    Line line;
    line.memory.fill(0xaa);
    uint8_t neighbourValue = 0x10;
    const auto touchNeighbour = [&] {
        if (r.neighbour < 0)
            return;
        (void)line.cpuRead(static_cast<uint32_t>(r.neighbour));
        line.cpuWrite(static_cast<uint32_t>(r.neighbour), ++neighbourValue);
    };
    for (uint32_t i = 0; i < r.length; ++i)
        line.cpuWrite(r.offset + i, static_cast<uint8_t>(0x80 + i));
    touchNeighbour();
    const UsbDmaPlan plan = usbDmaPlan(true, true, usbDmaWholeLines(r.offset, r.length, kLine));
    std::array<uint8_t, kLine> bounce{};
    if (plan.bounce) {
        for (uint32_t i = 0; i < r.length; ++i)
            bounce[r.offset + i] = line.cpuRead(r.offset + i);
    }
    const uint32_t supplied = r.actual < r.length ? r.actual : r.length;
    line.apply(plan.atMap);
    touchNeighbour();
    if (r.fillDuringTransfer)
        line.fill();
    for (uint32_t i = 0; i < supplied; ++i) {
        if (plan.bounce) {
            bounce[r.offset + i] = static_cast<uint8_t>(0x40 + i);  // the controller writes the bounce page
        } else {
            line.deviceWrite(r.offset + i, static_cast<uint8_t>(0x40 + i));
        }
    }
    touchNeighbour();
    bool failed = false;
    if (r.early && !plan.bounce && !r.fillDuringTransfer) {
        for (uint32_t i = 0; i < supplied; ++i)
            failed = failed || line.cpuRead(r.offset + i) != 0x40 + i;
    }
    line.apply(plan.atUnmap);
    if (plan.bounce) {
        for (uint32_t i = 0; i < r.length; ++i)
            line.cpuWrite(r.offset + i, bounce[r.offset + i]);  // the copy back, through the cache
    }
    touchNeighbour();
    for (uint32_t i = 0; i < r.length; ++i)
        failed = failed || line.cpuRead(r.offset + i) != (i < supplied ? 0x40 + i : 0x80 + i);
    if (r.neighbour >= 0)
        failed = failed || line.cpuRead(static_cast<uint32_t>(r.neighbour)) != neighbourValue;
    line.apply(UsbDmaCacheOp::CleanAndInvalidate);
    if (r.neighbour >= 0)
        failed = failed || line.memory[static_cast<uint32_t>(r.neighbour)] != neighbourValue;
    return failed || line.clobbered;
}

}  // namespace

// The failing reads on the 14 Nov boot: 2, 8 and 18 bytes of a heap buffer sharing its line with data the CPU uses.
// Every placement in the line, with the neighbour on either side, read after complete and (not bounced) before it.
TEST(UsbDmaCache, ShortAndCanceledInPreserveTheUnwrittenRange)
{
    for (const uint32_t supplied : {0u, 1u, 12u, 13u}) {
        EXPECT_FALSE(failsWith({3, 13, 2, false, true, supplied}));
        EXPECT_FALSE(failsWith({3, 13, 16, false, true, supplied}));
    }
}

TEST(UsbDmaCache, SmallInBuffersBesideCpuDataSeeTheDevicesBytes)
{
    for (const uint32_t length : {2u, 8u, 18u}) {
        for (uint32_t offset = 0; offset + length <= kLine; ++offset) {
            for (const int neighbour : {static_cast<int>(offset) - 1, static_cast<int>(offset + length)}) {
                if (neighbour < 0 || neighbour >= static_cast<int>(kLine))
                    continue;
                for (const bool early : {false, true}) {
                    for (const bool fill : {false, true}) {
                        EXPECT_FALSE(failsWith({offset, length, neighbour, early, fill}))
                            << length << " bytes at " << offset << ", neighbour " << neighbour
                            << (early ? ", read early" : "") << (fill ? ", filled" : "");
                    }
                }
            }
        }
    }
}

// The rules before this one, at the line: cleaning at prepare and cleaning and invalidating at complete writes the
// neighbour's line, holding the buffer's old bytes, back over the controller's.
TEST(UsbDmaCache, CleanAndInvalidateOfASharedLineLosesTheDevicesBytes)
{
    Line line;
    line.memory.fill(0xaa);
    line.cpuWrite(10, 0);
    line.apply(UsbDmaCacheOp::Clean);
    line.deviceWrite(10, 0x12);
    line.cpuWrite(11, 0x55);  // the neighbour
    line.apply(UsbDmaCacheOp::CleanAndInvalidate);
    EXPECT_TRUE(line.clobbered);
    EXPECT_NE(line.cpuRead(10), 0x12);
}

// A whole-line IN buffer (page-aligned bulk data) stays at its own address: no copy, read right after complete with a
// fill anywhere, and before complete without one.
TEST(UsbDmaCache, WholeLineInBuffersStayZeroCopy)
{
    EXPECT_FALSE(usbDmaPlan(true, true, true).bounce);
    for (const bool early : {false, true}) {
        for (const bool fill : {false, true})
            EXPECT_FALSE(failsWith({0, static_cast<uint32_t>(kLine), -1, early, fill})) << early << fill;
    }
}

// OUT (SETUP data, a stick's writes): cleaned at prepare at any alignment, the neighbour untouched, nothing written
// back after.
TEST(UsbDmaCache, OutIsInMemoryBeforeTheDeviceReadsAtAnyAlignment)
{
    const UsbDmaPlan plan = usbDmaPlan(true, false, false);
    EXPECT_FALSE(plan.bounce);
    Line line;
    line.cpuWrite(3, 0x77);
    line.cpuWrite(40, 0x10);
    line.apply(plan.atMap);
    EXPECT_EQ(line.memory[3], 0x77);
    line.cpuWrite(40, 0x11);
    line.apply(plan.atUnmap);
    EXPECT_EQ(line.cpuRead(40), 0x11);
}

TEST(UsbDmaCache, UncachedMemoryNeedsNothing)
{
    for (const bool writes : {false, true}) {
        for (const bool whole : {false, true}) {
            const UsbDmaPlan plan = usbDmaPlan(false, writes, whole);
            EXPECT_FALSE(plan.bounce);
            EXPECT_EQ(plan.atMap, UsbDmaCacheOp::None);
            EXPECT_EQ(plan.atUnmap, UsbDmaCacheOp::None);
        }
    }
    EXPECT_TRUE(usbDmaWholeLines(0x8000'0000, 128, 64));
    EXPECT_FALSE(usbDmaWholeLines(0x8000'0010, 18, 64));
    EXPECT_FALSE(usbDmaWholeLines(0x8000'0000, 18, 64));
}

namespace {

// A whole-line buffer used for three transfers in one direction, the CPU reading each IN or writing each OUT. Held: one
// map before the first and no unmap until after the last (Apple's remembered command); otherwise mapped and unmapped
// around each. Memory that is not cacheable the CPU reads and writes as memory. Returns whether the controller or the
// CPU ever saw anything but the transfer's bytes.
bool reusedFails(bool cacheable, bool in, bool held)
{
    Line line;
    line.memory.fill(0xaa);
    const UsbDmaPlan plan = usbDmaPlan(cacheable, in, true);
    if (held && cacheable)
        line.apply(plan.atMap);
    for (uint8_t transfer = 1; transfer <= 3; ++transfer) {
        const auto value = static_cast<uint8_t>(0x40 + transfer);
        if (!in) {
            if (cacheable) {
                line.cpuWrite(0, value);
            } else {
                line.memory[0] = value;
            }
        }
        if (!held && cacheable)
            line.apply(plan.atMap);
        if (in)
            line.deviceWrite(0, value);
        if (!in && line.memory[0] != value)
            return true;  // what the controller read
        if (!held && cacheable)
            line.apply(plan.atUnmap);
        if (in && (cacheable ? line.cpuRead(0) : line.memory[0]) != value)
            return true;
    }
    return line.clobbered;
}

}  // namespace

// The enumeration on boot 3: a cacheable buffer kept mapped serves the first read and every later one from the line
// that read cached (and OUT data stays in the CPU's cache). Write-combined, as the family's buffers are allocated now,
// it is right however long it is held; cacheable caller-supplied memory is right mapped per transfer.
TEST(UsbDmaCache, AHeldMappingIsRightOnlyWhenNotCacheable)
{
    for (const bool in : {true, false}) {
        EXPECT_TRUE(reusedFails(true, in, true)) << (in ? "IN" : "OUT");
        EXPECT_FALSE(reusedFails(false, in, true)) << (in ? "IN" : "OUT");
        EXPECT_FALSE(reusedFails(true, in, false)) << (in ? "IN" : "OUT");
    }
}

// A family buffer shorter than a line (the 13-byte CSW buffer) is write-combined, so it is never bounced: a bounce is
// copied only at map and unmap, which a held mapping never reaches.
TEST(UsbDmaCache, AnUncachedShortBufferIsNeverBounced)
{
    EXPECT_FALSE(usbDmaPlan(false, true, usbDmaWholeLines(0x8455'c000, 13, kLine)).bounce);
    EXPECT_TRUE(usbDmaPlan(true, true, usbDmaWholeLines(0x8455'c000, 13, kLine)).bounce);
}

// With the client buffer options set, AppleUSBXHCIRequest::prepare wraps a caller's descriptor that is not a family
// buffer in an IOSubMemoryDescriptor whose flags carry the options' 0x400 (direction | 0x400 | kIOMemoryThreadSafe).
// Over cacheable pages that wrapper is still maintained at map and at unmap; a write-combined family buffer is not.
TEST(UsbDmaCache, ASubRangeOverCacheableMemoryIsMaintained)
{
    constexpr uint32_t kIn = 1;
    constexpr uint32_t kOptionWriteCombine = 0x400;
    constexpr uint32_t kThreadSafe = 0x0010'0000;
    ASSERT_TRUE(usbDmaCacheable(kIn | kOptionWriteCombine | kThreadSafe));
    const UsbDmaPlan plan = usbDmaPlan(usbDmaCacheable(kIn | kOptionWriteCombine | kThreadSafe), true, true);
    EXPECT_EQ(plan.atMap, UsbDmaCacheOp::CleanAndInvalidate);
    EXPECT_EQ(plan.atUnmap, UsbDmaCacheOp::CleanAndInvalidate);

    constexpr uint32_t kWriteCombinedBuffer = 4u << kUsbDmaBufferCacheShift;
    EXPECT_FALSE(usbDmaCacheable(kIn | kWriteCombinedBuffer));
    EXPECT_FALSE(usbDmaCacheable(1u << kUsbDmaBufferCacheShift));  // inhibited
    EXPECT_TRUE(usbDmaCacheable(3u << kUsbDmaBufferCacheShift));   // copyback
}
