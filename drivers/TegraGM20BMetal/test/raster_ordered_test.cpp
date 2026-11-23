// SPDX-License-Identifier: MIT
// A state whose fragment function names raster order groups (HW Test gpu.gm20b.raster_order): m2v runs its body in the
// pixel interlock with no colour in the storage form. The direct compile keeps that program (it was refused as "storage
// form: compiled shader does not match the ordered attachment contract" and made again without the interlock, its
// groups unordered) and the record carries PipelineRasterOrdered, which the runtime, the compiler's client and the pack
// accept only with the record's own ordered interlock control.
#include "../record_parts.h"
#include "../storage.h"
#include <gtest/gtest.h>

namespace {

constexpr uint32_t kInterlockControl = 0x1224, kPixel = 2, kSample = 1;

TEST(RasterOrdered, AnInterlockAroundNoStorageColourIsTheRasterOrderGroupsOrdering)
{
    EXPECT_TRUE(gm20b::interlockMatchesStorage(kPixel, 0));
    EXPECT_TRUE(gm20b::interlockMatchesStorage(kSample, 0));
    EXPECT_TRUE(gm20b::interlockMatchesStorage(0, 0));
    EXPECT_TRUE(gm20b::interlockMatchesStorage(kPixel, 1));
    EXPECT_FALSE(gm20b::interlockMatchesStorage(0, 1));
    EXPECT_FALSE(gm20b::interlockMatchesStorage(3, 0));
}

TEST(RasterOrdered, ARecordCarriesItsOrderedInterlockControl)
{
    gm20b::PackPipeline p = {};
    p.flags = gm20b::PipelineRasterOrdered;
    const uint32_t ordered[2] = {kInterlockControl, kPixel}, none[2] = {kInterlockControl, 0};
    EXPECT_TRUE(gm20b::usableStorageMetadata(p, ordered, 2, nullptr));
    EXPECT_FALSE(gm20b::usableStorageMetadata(p, none, 2, nullptr)) << "raster ordered with no interlock";
    EXPECT_FALSE(gm20b::usableStorageMetadata(p, nullptr, 0, nullptr)) << "raster ordered with no interlock control";
    p.flags = 0;
    EXPECT_FALSE(gm20b::usableStorageMetadata(p, ordered, 2, nullptr))
        << "an interlock the record does not say orders anything";
}

TEST(RasterOrdered, AStorageFormIsNotRasterOrdered)
{
    gm20b::PackPipeline p = {};
    p.flags = gm20b::PipelineStorageMetadata | gm20b::PipelineRasterOrdered;
    p.storageMask = p.storageWriteMask = 1;
    p.storageSamples = 1;
    p.storageTic[0] = 1;
    gm20b::setEntryCount(p, 1);
    p.entry[0].kind = gm20b::EntryFetch;
    p.entry[0].stage = 2;
    const uint32_t blob[4] = {kInterlockControl, kPixel, 0x1a00, 0};
    EXPECT_FALSE(gm20b::usableStorageMetadata(p, blob, 4, nullptr));
    p.flags = gm20b::PipelineStorageMetadata;
    EXPECT_TRUE(gm20b::usableStorageMetadata(p, blob, 4, nullptr));
}

// The record keeps its interlock control in its blob whether or not the pack's states vary it (they vary it only where some
// state's storage form sets it), so the runtime draws the program in the interlock and every validator accepts the flag
TEST(RasterOrdered, AnAssembledRecordKeepsItsInterlockControlAndFlag)
{
    gm20b::CapturedProgram vs, fs;
    vs.id = "vs";
    fs.id = "fs";
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    vs.cbufTypes = fs.cbufTypes = {gm20b::CbufRoot};
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.rasterOrdered = true;
    parts.regs = {{kInterlockControl, kPixel}, {0x1a00, 0}};
    gm20b::PackRules rules;
    rules.varying = {0x1a00};
    gm20b::RuntimeRecord out;
    std::string why;
    ASSERT_TRUE(
        gm20b::assembleRecord(parts, "samples=1 topo=0 ds=0/0 c0=70:0:0:1:0:0:1:0:0 | v vs | f fs", rules, out, why))
        << why;
    EXPECT_TRUE(out.pipeline.flags & gm20b::PipelineRasterOrdered);
    bool kept = false;
    for (size_t i = 0; i + 1 < out.blob.size(); i += 2)
        kept |= out.blob[i] == kInterlockControl && out.blob[i + 1] == kPixel;
    EXPECT_TRUE(kept) << "the blob carries SET_PIXEL_SHADER_INTERLOCK_CONTROL";
    EXPECT_TRUE(gm20b::usableStorageMetadata(out.pipeline, out.blob.data(), uint32_t(out.blob.size()),
                                             out.far.empty() ? nullptr : out.far.data()));
}

}  // namespace
