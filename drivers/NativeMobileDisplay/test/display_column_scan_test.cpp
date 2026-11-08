// drivers/NativeMobileDisplay/NativeColumnScan.h on the host. A model of window A's fetch (L4T window.c's reading of
// the registers: SCAN_COLUMN turns a line into a source column, H_DIRECTION walks it back from ADDR_H_OFFSET, the DDAs
// step in 4.12, block-linear addresses are TegraGM20BLayout.h's) is first held to the one column scan that ran on
// hardware, the landscape UI before the portrait-native layout (NativeScanout.h). Then the test surface through the
// column-scan registers and the reference frame through the UI's must give the same panel, pixel for pixel, at every
// block height, reading inside the source and only chroma 128: so the DC's CRC of one equals the other's exactly when
// the hardware scans as modelled. Last, the run's frame-end decisions for a pass and for each failure.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../NativeColumnScan.h"

namespace {
using namespace native_column_scan;

constexpr uint32_t kSurfaceIova = 0x40000000u, kReferenceIova = 0x50000000u;

struct Memory {
    std::vector<uint8_t> surface = std::vector<uint8_t>(kSurfaceBytes),
                         reference = std::vector<uint8_t>(kReferenceBytes);
    uint32_t base = kSurfaceIova;  // the landscape anchor's own image lives at the surface's IOVA
    bool outside = false;

    uint8_t at(uint64_t iova)
    {
        if (iova >= kSurfaceIova && iova - kSurfaceIova < surface.size())
            return surface[iova - kSurfaceIova];
        if (iova >= kReferenceIova && iova - kReferenceIova < reference.size())
            return reference[iova - kReferenceIova];
        outside = true;
        return 0;
    }
};

// One fetched pixel: luma (or the BGRA word's blue), and whether its chroma was 128 (RGB: always).
struct Fetched {
    uint32_t value;
    bool neutral;
};

// Window A's fetch of panel line `line`, pixel `pixel`, as the registers say (no filtering: every test DDA is whole,
// so each sample is at phase 0, where the taps pass the centre through).
struct WindowModel {
    const Window &w;
    Memory &m;
    bool outsidePrescaled = false;

    uint32_t bpp() const
    {
        return w.depth == native_planes::kDepthYCbCr420SP ? 1 : 4;
    }

    uint64_t address(uint32_t start, uint32_t stride, uint32_t xB, uint32_t y) const
    {
        if ((w.kind & 3) == kBlockKind)
            return start + tgm_layout_offset_of(stride, (uint8_t)((w.kind >> 4) & 7), xB, y);
        return start + (uint64_t)y * stride + xB;
    }

    Fetched fetch(uint32_t line, uint32_t pixel)
    {
        const bool column = w.options & kScanColumn;
        const uint32_t hInc = w.dda & 0xffff, vInc = w.dda >> 16;
        // Along a line: source x (row scan) or source y (column scan), on the H DDA; across lines the other, on the V.
        const uint32_t along = (w.hInitial + pixel * hInc) >> 12, across = (w.vInitial + line * vInc) >> 12;
        const uint32_t xStep = column ? across : along, yStep = column ? along : across;
        const uint32_t x0 = w.hOffset / bpp();
        const int64_t x = (w.options & kHDirection) ? (int64_t)x0 - xStep : (int64_t)x0 + xStep;
        const int64_t y = (w.options & kVDirection) ? (int64_t)w.vOffset - yStep : (int64_t)w.vOffset + yStep;
        // PRESCALED_SIZE bounds the source the window reads: V lines (column scan: columns), H bytes of each.
        const uint32_t lines = w.prescaled >> 16, bytes = w.prescaled & 0xffff;
        if (x < 0 || y < 0 ||
            (column ? (uint64_t)x >= lines || (uint64_t)y * bpp() >= bytes
                    : (uint64_t)y >= lines || (uint64_t)x * bpp() >= bytes))
            outsidePrescaled = true;
        const uint32_t stride = w.lineStride & 0xffff, xB = (uint32_t)x * bpp();
        if (bpp() == 4) {
            uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i)
                word |= (uint32_t)m.at(address(w.start, stride, xB + i, (uint32_t)y)) << (8 * i);
            return {word, true};
        }
        const uint32_t chromaStride = w.lineStride >> 16, pair = (uint32_t)x & ~1u;
        const bool neutral = m.at(address(w.startU, chromaStride, pair, (uint32_t)y / 2)) == kChroma &&
                             m.at(address(w.startU, chromaStride, pair + 1, (uint32_t)y / 2)) == kChroma;
        return {m.at(address(w.start, stride, xB, (uint32_t)y)), neutral};
    }
};

TEST(ColumnScanWindow, TheModelScansTheLandscapeUiAsTheHardwareDid)
{
    // NativeScanout.h's rotated UI: a 1280x720 BGRA landscape frame in 16-GOB blocks, SCAN_COLUMN | H_DIRECTION from
    // the line's last byte, 1:1 (kPitchSize, kPitchPrescaled, unit DDA): panel line j shows landscape column 1279 - j.
    Memory m;
    const uint32_t stride = 1280 * 4;
    auto landscape = [](uint32_t x, uint32_t y) {
        return 0xff000000u | x << 12 | y;
    };
    fillBlockLinear(m.surface.data(), stride, 768, 4, [&](uint32_t xB, uint32_t y) {
        return (uint8_t)(landscape(xB / 4, y) >> (8 * (xB % 4)));
    });
    Window w = {};
    w.options = native_planes::kWinEnable | kScanColumn | kHDirection;
    w.depth = native_planes::kDepthB8G8R8A8;
    w.size = 1280u << 16 | 720;
    w.prescaled = 1280u << 16 | 720 * 4;
    w.dda = 0x10001000;
    w.lineStride = stride;
    w.kind = kBlockKind | 4 << 4;
    w.start = kSurfaceIova;
    w.hOffset = stride - 1;
    WindowModel model{w, m};
    for (uint32_t line = 0; line < kPanelHeight; line += 7)
        for (uint32_t pixel = 0; pixel < kPanelWidth; pixel += 5)
            ASSERT_EQ(model.fetch(line, pixel).value, landscape(1279 - line, pixel)) << line << "," << pixel;
    EXPECT_FALSE(model.outsidePrescaled);
    EXPECT_FALSE(m.outside);
}

TEST(ColumnScanWindow, RegistersFollowL4TsScanColumnRules)
{
    Window w;
    ASSERT_TRUE(scanWindow(kSurfaceIova, 4, w));
    EXPECT_EQ(w.options, 0x40040511u);  // WIN_ENABLE, CSC, V and H filters, SCAN_COLUMN, H_DIRECTION
    EXPECT_EQ(w.depth, 43u);
    EXPECT_EQ(w.position, 0u);
    EXPECT_EQ(w.size, 1280u << 16 | 720u);        // V_SIZE out_h (panel lines), H_SIZE out_w
    EXPECT_EQ(w.prescaled, 3840u << 16 | 2160u);  // V: w, H: h * Bpp
    EXPECT_EQ(w.dda, 0x30003000u);                // V: w / out_h, H: h / out_w
    EXPECT_EQ(w.hInitial, 0u);
    EXPECT_EQ(w.vInitial, 0u);
    EXPECT_EQ(w.lineStride, 3840u << 16 | 3840u);  // UV_LINE_STRIDE 31:16, LINE_STRIDE 15:0
    EXPECT_EQ(w.kind, 0x42u);
    EXPECT_EQ(w.start, kSurfaceIova);
    EXPECT_EQ(w.startU, kSurfaceIova + 3840u * 2176u);
    EXPECT_EQ(w.hOffset, 3839u);  // (x + w) * Bpp - 1
    EXPECT_EQ(w.vOffset, 0u);
    ASSERT_TRUE(scanWindow(kSurfaceIova, 1, w));
    EXPECT_EQ(w.kind, 0x12u);
    EXPECT_FALSE(scanWindow(kSurfaceIova, 5, w));
    EXPECT_FALSE(scanWindow(0xff800000u, 4, w));  // the chroma plane would pass 4 GiB
    EXPECT_LE(kSourceWidth, kPlanarRotationMaxWidth);
    EXPECT_LE(kSourceHeight, kPlanarRotationMaxHeight);
}

TEST(ColumnScanWindow, ReferenceIsTheUisOwnLayout)
{
    const Window w = referenceWindow(kReferenceIova, native_planes::kDepthB8G8R8A8);
    // hekate's pitch window geometry (kPitchSize, kPitchPrescaled, unit DDA) in the portrait UI's 16-GOB blocks.
    EXPECT_EQ(w.options, native_planes::kWinEnable);
    EXPECT_EQ(w.size, 1280u << 16 | 720u);
    EXPECT_EQ(w.prescaled, 1280u << 16 | 2880u);
    EXPECT_EQ(w.dda, 0x10001000u);
    EXPECT_EQ(w.lineStride, 2880u);
    EXPECT_EQ(w.kind, 0x42u);
    EXPECT_EQ(w.hOffset, 0u);
    EXPECT_EQ(w.start, kReferenceIova);
}

class ColumnScanImage : public testing::TestWithParam<int> {};

TEST_P(ColumnScanImage, ScanAndReferenceShowTheSamePanel)
{
    const uint8_t yLog2 = (uint8_t)GetParam();
    Memory m;
    fillSurface(m.surface.data(), yLog2);
    fillReference(m.reference.data());
    Window scan;
    ASSERT_TRUE(scanWindow(kSurfaceIova, yLog2, scan));
    const Window ref = referenceWindow(kReferenceIova, native_planes::kDepthB8G8R8A8);
    WindowModel scanned{scan, m}, shown{ref, m};
    for (uint32_t line = 0; line < kPanelHeight; ++line)
        for (uint32_t pixel = 0; pixel < kPanelWidth; ++pixel) {
            const Fetched s = scanned.fetch(line, pixel), r = shown.fetch(line, pixel);
            const uint8_t want = panelLuma(line, pixel);
            ASSERT_EQ(s.value, want) << "column scan at line " << line << " pixel " << pixel;
            ASSERT_TRUE(s.neutral) << "chroma at line " << line << " pixel " << pixel;
            ASSERT_EQ(r.value, greyBgra(want)) << "reference at line " << line << " pixel " << pixel;
        }
    EXPECT_FALSE(scanned.outsidePrescaled);
    EXPECT_FALSE(shown.outsidePrescaled);
    EXPECT_FALSE(m.outside);
}

INSTANTIATE_TEST_SUITE_P(BlockHeights, ColumnScanImage, testing::Values(0, 1, 4));

TEST(ColumnScanImage, EveryMirrorAndTurnLooksDifferent)
{
    // A scan mirrored on either axis (or both) shows another image, so its CRC cannot be the reference frame's.
    unsigned differLines = 0, differPixels = 0, differBoth = 0;
    for (uint32_t line = 0; line < kPanelHeight; ++line)
        for (uint32_t pixel = 0; pixel < kPanelWidth; ++pixel) {
            const uint8_t p = panelLuma(line, pixel);
            differLines += p != panelLuma(kPanelHeight - 1 - line, pixel);
            differPixels += p != panelLuma(line, kPanelWidth - 1 - pixel);
            differBoth += p != panelLuma(kPanelHeight - 1 - line, kPanelWidth - 1 - pixel);
        }
    EXPECT_GT(differLines, 1000u);
    EXPECT_GT(differPixels, 1000u);
    EXPECT_GT(differBoth, 1000u);
    // The border's 16 panel pixels on every edge, the landscape top left's square at the panel's last lines.
    EXPECT_EQ(panelLuma(0, 360), 235);
    EXPECT_EQ(panelLuma(1279, 360), 235);
    EXPECT_EQ(panelLuma(640, 15), 235);
    EXPECT_EQ(panelLuma(640, 704), 235);
    EXPECT_EQ(panelLuma(640, 16) == 235, false);
    EXPECT_EQ(panelLuma(1279 - 100, 50), 16);
    EXPECT_NE(panelLuma(100, 50), 16);
}

TEST(ColumnScanImage, EachLandscapePixelIsOneValue)
{
    for (uint32_t y = 0; y < kSourceHeight; ++y)
        for (uint32_t x = 0; x < kSourceWidth; ++x)
            ASSERT_EQ(lumaAt(x, y), lumaAt(x - x % kScale, y - y % kScale)) << x << "," << y;
}

TEST(ColumnScanImage, FillsRoundTripThroughTheLayout)
{
    std::vector<uint8_t> surface(kSurfaceBytes);
    fillSurface(surface.data(), 1);
    for (uint32_t y = 0; y < kSourceHeight; y += 37)
        for (uint32_t x = 0; x < kSourceWidth; x += 29)
            ASSERT_EQ(surface[tgm_layout_offset_of(kRowBytes, 1, x, y)], lumaAt(x, y));
    for (uint32_t y = 0; y < kChromaRows; y += 41)
        ASSERT_EQ(surface[kChromaOffset + tgm_layout_offset_of(kRowBytes, 1, 1001, y)], kChroma);
}

// --- The run ------------------------------------------------------------------------------------------------------------
struct Frame {
    bool quiet = true, requestOut = false;
    uint32_t crc = 0;
    bool underflow = false, asWritten = true;
};

struct Script {
    Run run;
    std::vector<Run::Action> actions;

    Run::Action step(const Frame &f)
    {
        const Run::Action a = run.frameEnd(f.quiet, f.requestOut, f.crc, f.underflow, f.asWritten);
        if (a != Run::kNothing)
            actions.push_back(a);
        return a;
    }

    // A healthy display: requests taken at the next boundary, the CRC 0 twice after the unit goes on, then the frame's.
    void reachScan(uint32_t reference)
    {
        run.begin();
        ASSERT_EQ(step({}), Run::kStart);
        step({});  // the reference latched
        for (uint32_t crc : {0u, 0u, reference, reference})
            if (step({true, false, crc}) == Run::kShowScan)
                break;
        ASSERT_EQ(run.stage, Run::kScanLatch);
        step({true, false, reference});  // the column scan latched
        ASSERT_EQ(run.stage, Run::kScan);
    }

    void scan(uint32_t crc, unsigned frames, bool underflow = false, bool asWritten = true)
    {
        for (unsigned i = 0; i < frames; ++i)
            step({true, false, crc, underflow, asWritten});
    }
};

TEST(ColumnScanRun, PassesPixelExact)
{
    Script s;
    s.reachScan(0x1234);
    s.step({true, false, 0x1234});  // frames 1-2 may still carry the reference's CRC
    s.step({true, false, 0x9999});
    s.scan(0x1234, 28);
    ASSERT_EQ(s.run.stage, Run::kRestoreLatch);
    EXPECT_EQ(s.step({true, false, 0x1234}), Run::kFinish);
    EXPECT_EQ(s.actions, (std::vector<Run::Action>{Run::kStart, Run::kShowScan, Run::kRestore, Run::kFinish}));
    EXPECT_TRUE(s.run.clean());
    EXPECT_TRUE(s.run.exact());
    EXPECT_EQ(s.run.reference, 0x1234u);
    EXPECT_EQ(s.run.scanFrames, Run::kScanFrames);
}

TEST(ColumnScanRun, CleanButNotTheReference)
{
    Script s;
    s.reachScan(0x1234);
    s.scan(0x5678, 30);
    s.step({});
    EXPECT_TRUE(s.run.clean());
    EXPECT_FALSE(s.run.exact());
}

TEST(ColumnScanRun, UnderflowUnstableOrNotAsWrittenIsNotClean)
{
    for (int fault = 0; fault < 3; ++fault) {
        Script s;
        s.reachScan(0x1234);
        s.scan(0x1234, 10);
        if (fault == 1)
            s.scan(0x4321, 1);
        else
            s.scan(0x1234, 1, fault == 0, fault != 2);
        s.scan(0x1234, 19);
        s.step({});
        EXPECT_FALSE(s.run.clean()) << fault;
        EXPECT_EQ(s.run.underflows, fault == 0 ? 1u : 0u);
        EXPECT_EQ(s.run.crcChanges, fault == 1 ? 2u : 0u);
        EXPECT_EQ(s.run.mismatches, fault == 2 ? 1u : 0u);
        EXPECT_TRUE(s.run.restored);
    }
}

TEST(ColumnScanRun, NeverQuietTouchesNothing)
{
    Script s;
    s.run.begin();
    Run::Action last = Run::kNothing;
    for (unsigned i = 0; i < Run::kWaitFrames && last == Run::kNothing; ++i)
        last = s.step({false});
    EXPECT_EQ(last, Run::kFinish);
    EXPECT_EQ(s.actions, std::vector<Run::Action>{Run::kFinish});
    EXPECT_FALSE(s.run.restoreWritten);
    EXPECT_STREQ(s.run.why, "the display never went quiet");
}

TEST(ColumnScanRun, FailuresRestoreThenFinish)
{
    {
        Script s;  // window A never takes the reference
        s.run.begin();
        s.step({});
        for (unsigned i = 0; i < Run::kLatchFrames; ++i)
            s.step({true, true});
        EXPECT_EQ(s.run.stage, Run::kRestoreLatch);
        EXPECT_EQ(s.step({}), Run::kFinish);
        EXPECT_EQ(s.run.failedAt, Run::kReferenceLatch);
        EXPECT_TRUE(s.run.restored);
    }
    {
        Script s;  // the CRC unit never latches
        s.run.begin();
        s.step({});
        s.step({});
        for (unsigned i = 0; i < Run::kTransitionFrames + Run::kZeroReads; ++i)
            s.step({true, false, 0});
        EXPECT_EQ(s.run.stage, Run::kRestoreLatch);
        EXPECT_STREQ(s.run.why, "the CRC unit reads 0");
    }
    {
        Script s;  // the reference frame's CRC never settles
        s.run.begin();
        s.step({});
        s.step({});
        for (uint32_t i = 0; i < Run::kTransitionFrames + Run::kSettleReads; ++i)
            s.step({true, false, 0x100 + i});
        EXPECT_EQ(s.run.stage, Run::kRestoreLatch);
        EXPECT_EQ(s.run.failedAt, Run::kReference);
    }
    {
        Script s;  // the restore never latches
        s.reachScan(0x1234);
        s.scan(0x1234, 30);
        for (unsigned i = 0; i + 1 < Run::kLatchFrames; ++i)
            EXPECT_EQ(s.step({true, true}), Run::kNothing);
        EXPECT_EQ(s.step({true, true}), Run::kFinish);
        EXPECT_FALSE(s.run.restored);
        EXPECT_STREQ(s.run.why, "window A did not take its registers back");
    }
}

TEST(ColumnScanRun, AbortWritesTheRestoreOnlyOnce)
{
    native_column_scan::Run r;
    EXPECT_FALSE(r.abort("off"));  // idle
    r.begin();
    EXPECT_FALSE(r.abort("off"));  // waiting: window A untouched
    EXPECT_FALSE(r.restoreWritten);
    Script s;
    s.reachScan(0x1234);
    EXPECT_TRUE(s.run.abort("off"));
    EXPECT_EQ(s.run.stage, Run::kIdle);
    EXPECT_STREQ(s.run.why, "off");
    Script t;
    t.reachScan(0x1234);
    t.scan(0x1234, 30);
    ASSERT_EQ(t.run.stage, Run::kRestoreLatch);
    EXPECT_FALSE(t.run.abort("off"));  // its restore is already written
    EXPECT_FALSE(t.run.clean());
}
}  // namespace
