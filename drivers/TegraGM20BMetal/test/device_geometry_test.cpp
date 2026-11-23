// SPDX-License-Identifier: MIT
// The encode reads no buffer's contents on the CPU (gm20b::Device over the kernel model, every kernel call watched where it
// arrives): a pass's first draw of Core Animation's transform over vertices and a transform in buffers takes its pixel
// limit, and an indexed draw past its index buffer binds the buffer under the hardware's index limit (caps row 3D-B11),
// so neither takes a CPU read lease of the buffer nor copies its indices.
#include "device_harness.h"
#include "../record.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <array>
#include <atomic>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kStrip = 4, kSide = 256;
constexpr uint32_t kStreamFormat0 = 0x1c00, kStreamEnable = 1u << 12;

std::atomic<uint32_t> cpuReads{0};

int countCpuReads(uint32_t method, const void *)
{
    if (method == TegraGM20BMethodCpuAcquire)
        cpuReads.fetch_add(1, std::memory_order_relaxed);
    return KERN_SUCCESS;
}

// Core Animation's transform state as the device makes one: a float4 position at offset 0 of 16-byte vertices in buffer
// 0 (its region test reads those and the transform in buffer 2), colour 0 written whole
gm20b::RuntimeRecord transformRecord(uint64_t key)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 a0=31:0:0:16:1:1 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.pipeline.keyHash = key;
    r.pipeline.vertexSlot = 0;
    r.pipeline.vertexStride = 16;
    r.blob = {0x1a00, 0x1111, kStreamFormat0, kStreamEnable | 16};
    return r;
}

class DeviceGeometry : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer vertices, transform, indices;
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(64, vertices, true));
        ASSERT_TRUE(dev.createBuffer(64, transform, true));
        ASSERT_TRUE(dev.createBuffer(8, indices, true));
        const float corners[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const uint16_t four[4] = {0, 1, 2, 3};
        memcpy(vertices.cpu, corners, sizeof corners);
        memcpy(transform.cpu, identity, sizeof identity);
        memcpy(indices.cpu, four, sizeof four);
        std::string why;
        pl = dev.addRecord(transformRecord(0x9e01), true, why);
        ASSERT_NE(pl, nullptr) << why;
        ASSERT_TRUE(pl->caTransform && pl->positionComponents == 4);
        cpuReads = 0;
        kernel_model::arriving.store(&countCpuReads, std::memory_order_release);
    }

    void TearDown() override
    {
        dev.destroyBuffer(vertices);
        dev.destroyBuffer(transform);
        dev.destroyBuffer(indices);
        DeviceTest::TearDown();
    }

    // One command buffer of one cleared pass whose first draw `draw` records, over vertices and a transform in buffers
    template <typename Draw> bool pass(Draw &&draw)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return false;
        gm20b::PassDesc desc;
        desc.color[0].texture = &target;
        desc.color[0].load = gm20b::ColorAttachment::Clear;
        desc.count = 1;
        gm20b::RenderEncoder *e = cb->render(desc);
        if (!e)
            return false;
        e->setPipeline(pl);
        e->setBuffer(1, 0, &vertices, 0);
        e->setBuffer(1, 2, &transform, 0);
        const char *why = nullptr;
        const bool drawn = draw(*e, &why);
        EXPECT_TRUE(drawn) << (why ? why : "");
        e->end();
        uint32_t seqno = 0;
        return drawn && cb->commit(seqno);
    }

    // SET_INDEX_BUFFER_A..D of the model's jobs from `first` on
    static std::vector<std::array<uint32_t, 4>> indexBuffers(size_t first)
    {
        std::vector<std::array<uint32_t, 4>> out;
        for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
            const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
            std::vector<switch_gm20b_method> m(w.size());
            const long n = switch_gm20b_push_decode(w.data(), w.size(), m.data(), m.size());
            for (long i = 0; i + 3 < n; i++)
                if (!m[size_t(i)].subchannel && m[size_t(i)].method == NVB197_SET_INDEX_BUFFER_A)
                    out.push_back(
                        {m[size_t(i)].value, m[size_t(i) + 1].value, m[size_t(i) + 2].value, m[size_t(i) + 3].value});
        }
        return out;
    }
};

// A whole-target quad, the pass's first draw: its cover test would read the buffers' vertices and transform
TEST_F(DeviceGeometry, AFirstDrawOverBufferGeometryReadsNoBufferOnTheCpu)
{
    ASSERT_TRUE(pass([](gm20b::RenderEncoder &e, const char **why) {
        return e.draw(kStrip, 0, 4, 1, 0, why);
    }));
    EXPECT_EQ(cpuReads.load(), 0u) << "the encode took a CPU read of a buffer's vertices or transform";
}

// Six indices from a buffer of four: the buffer bound from its first index to its fetch end, index 0 past it from the
// hardware, no copy of its indices
TEST_F(DeviceGeometry, AnIndexedDrawPastItsBufferBindsItUnderTheIndexLimit)
{
    const size_t first = kernel_model::model.jobs.size();
    const gm20b::Buffer *ib = &indices;
    ASSERT_TRUE(pass([ib](gm20b::RenderEncoder &e, const char **why) {
        return e.drawIndexed(kTriangle, 6, 2, ib, 0, 1, 0, 0, why);
    }));
    EXPECT_EQ(cpuReads.load(), 0u) << "the encode took a CPU read of the index buffer";
    const auto bound = indexBuffers(first);
    ASSERT_EQ(bound.size(), 1u);
    const uint64_t last = indices.va + 7;
    EXPECT_EQ(bound[0], (std::array<uint32_t, 4>{uint32_t(indices.va >> 32), uint32_t(indices.va), uint32_t(last >> 32),
                                                 uint32_t(last)}));
}

}  // namespace
