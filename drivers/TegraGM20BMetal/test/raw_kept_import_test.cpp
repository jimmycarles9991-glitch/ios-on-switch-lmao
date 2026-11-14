// SPDX-License-Identifier: MIT
// raw::Device's kept surface imports over the kernel model (raw_device_harness.h): a surface's texture made again after
// its last one went takes the same import (no kernel import, free or layout ask), as Core Animation's texture over its
// pooled surface does every frame; at most eight are kept, the oldest given back first, and trim gives back every one.
#include "raw_device_harness.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdlib>
#include <unistd.h>

namespace {

constexpr uint32_t kRGBA8 = 70, kWidth = 256, kHeight = 64, kPitch = kWidth * 4;
constexpr uint64_t kBytes = 1 << 20;

class KeptImport : public harness::DeviceTest {
protected:
    struct Surface {
        void *pages = nullptr;
        CFTypeRef owner = nullptr;
        uint32_t id = 0;
    };

    std::vector<Surface> surfaces;

    void SetUp() override
    {
        DeviceTest::SetUp();
        for (uint32_t i = 0; i < 10; i++) {
            Surface s;
            ASSERT_EQ(posix_memalign(&s.pages, size_t(getpagesize()), kBytes), 0);
            s.owner = CFDataCreate(nullptr, nullptr, 0);
            s.id = 100 + i;
            surfaces.push_back(s);
        }
    }

    void TearDown() override
    {
        dev.trim();
        for (Surface &s : surfaces) {
            CFRelease(s.owner);
            free(s.pages);
        }
    }

    // A texture over surface `s`, made and destroyed: the import it took
    uint64_t frame(const Surface &s)
    {
        raw::Texture t;
        EXPECT_TRUE(
            dev.createSurfaceTexture(s.id, s.pages, kBytes, s.owner, 0, kWidth, kHeight, kPitch, kRGBA8, t, true));
        const uint64_t handle = t.import ? t.import->bo.handle : 0;
        dev.destroyTexture(t);
        return handle;
    }

    size_t imports() const
    {
        return kernel_model::model.imports.size();
    }
};

TEST_F(KeptImport, ASurfacesTextureEveryFrameTakesOneImport)
{
    const uint64_t first = frame(surfaces[0]);
    ASSERT_NE(first, 0u);
    const size_t made = imports(), freed = kernel_model::model.freed.size();
    for (int f = 0; f < 30; f++)
        EXPECT_EQ(frame(surfaces[0]), first) << f;
    EXPECT_EQ(imports(), made);
    EXPECT_EQ(kernel_model::model.freed.size(), freed);
}

TEST_F(KeptImport, EightAreKeptAndTrimGivesThemBack)
{
    std::vector<uint64_t> handles;
    for (const Surface &s : surfaces)
        handles.push_back(frame(s));
    // ten surfaces released in order: the two oldest given back, eight kept
    const std::vector<uint64_t> &freed = kernel_model::model.freed;
    ASSERT_EQ(freed.size(), 2u);
    EXPECT_EQ(freed[0], handles[0]);
    EXPECT_EQ(freed[1], handles[1]);
    EXPECT_EQ(frame(surfaces[9]), handles[9]);
    const raw::Device::Trimmed trimmed = dev.trim();
    EXPECT_EQ(freed.size(), 10u);
    EXPECT_EQ(trimmed.keptImports, 8u);
    EXPECT_EQ(trimmed.keptImportBytes, 8 * kBytes);
    EXPECT_EQ(dev.trim().keptImports, 0u) << "trim gave every one back";
}

}  // namespace
