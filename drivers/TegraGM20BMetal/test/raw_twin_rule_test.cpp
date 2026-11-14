// SPDX-License-Identifier: MIT
// raw/runtime/raw_twin_rule.h: which draws of a storage form its barrier twin takes, a triangle at a time.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_overlap.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_twin_rule.h"

namespace {

raw::TwinAsk measured(uint32_t primitive, uint32_t count, uint64_t area)
{
    raw::TwinAsk a;
    a.shape = {primitive, count, 1, false, false};
    a.areaPixels = area;
    return a;
}

TEST(RawTwinRule, TheMeasuredDrawsTakeTheTwin)
{
    for (uint32_t quads : {1u, 4u, 16u})
        EXPECT_TRUE(raw::twinTaken(measured(3, 6 * quads, 64 * 64))) << quads;
    EXPECT_TRUE(raw::twinTaken(measured(3, 6, 1280 * 720)));
    EXPECT_TRUE(raw::twinTaken(measured(4, 4, 320 * 180)));
    EXPECT_TRUE(raw::twinTaken(measured(3, 3, 64 * 64)));
}

TEST(RawTwinRule, PastTheMeasuredTrianglesAndAreasTheStorageFormStays)
{
    EXPECT_FALSE(raw::twinTaken(measured(3, 3 * 33, 64 * 64)));
    EXPECT_FALSE(raw::twinTaken(measured(3, 6, 63 * 64)));
    EXPECT_FALSE(raw::twinTaken(measured(3, 6, 1280 * 721)));
    EXPECT_FALSE(raw::twinTaken(measured(3, 0, 64 * 64)));
}

// Every shape the twin's per-triangle draw refuses (selfRuns, sequentialRuns) keeps the storage form, which draws it
TEST(RawTwinRule, NoShapeTheTwinTakesIsOneSelfRunsRefuses)
{
    for (uint32_t primitive = 0; primitive <= 4; primitive++)
        for (uint32_t count = 0; count <= 3 * 40; count++)
            for (uint32_t instances : {1u, 2u})
                for (bool indirect : {false, true})
                    for (bool indexed : {false, true})
                        for (bool primitiveId : {false, true}) {
                            raw::TwinAsk a;
                            a.shape = {primitive, count, instances, indirect, indexed};
                            a.primitiveId = primitiveId;
                            a.areaPixels = 640 * 360;
                            if (!raw::twinTaken(a))
                                continue;
                            EXPECT_EQ(raw::fetchRefusal(true, primitiveId, a.shape), raw::FetchRefusal::None);
                            std::vector<std::pair<uint32_t, uint32_t>> runs;
                            std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
                            EXPECT_TRUE(raw::singlePrimitive(a.shape) ||
                                        raw::TriangleRuns::sequential(primitive, count, instances, runs, winding))
                                << primitive << " " << count << " " << instances << " " << indexed;
                        }
}

TEST(RawTwinRule, RefusedShapesKeepTheStorageForm)
{
    raw::TwinAsk a = measured(3, 6, 64 * 64);
    a.shape.instances = 2;
    EXPECT_FALSE(raw::twinTaken(a));
    a = measured(4, 6, 64 * 64);
    a.shape.indexed = true;
    EXPECT_FALSE(raw::twinTaken(a)) << "an indexed strip restarts where only its indices say";
    a = measured(3, 6, 64 * 64);
    a.shape.indirect = true;
    EXPECT_FALSE(raw::twinTaken(a));
    a = measured(3, 6, 64 * 64);
    a.primitiveId = true;
    EXPECT_FALSE(raw::twinTaken(a));
    a = measured(3, 6, 64 * 64);
    a.amplified = true;
    EXPECT_FALSE(raw::twinTaken(a));
    EXPECT_FALSE(raw::twinTaken(measured(1, 2, 64 * 64))) << "lines";
    EXPECT_FALSE(raw::twinTaken(measured(0, 1, 64 * 64))) << "a point";
}

}  // namespace
