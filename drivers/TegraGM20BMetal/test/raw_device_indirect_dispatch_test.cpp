// SPDX-License-Identifier: MIT
// An indirect dispatch on raw::Device: the grid and a stage-in region a compute encoder reads from buffers reach the QMD
// and the root table the launch names only through the compute class's inline engine, whose data is the buffers' own
// words fetched after its method header (no CPU copy of them), behind a wait that lets the fetch see what the work before
// wrote. The test runs the submitted method stream through a model of that engine and reads the QMD and root table the
// launch would see.
#include "raw_device_harness.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "../raw/runtime/raw_compute.h"
#include <map>
#include <vector>

namespace {

constexpr uint32_t kRootPush = 0x700, kRegionWord = 4, kQmdRootLo = 29;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0)),
                   kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeightDepth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT);

raw::ComputeRecord kernel(bool stageIn)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufType[0] = raw::CbufRoot;
    r.cbufs = 1;
    r.kernel.regionWord = -1;
    r.kernel.qmd[kQmdCbufValid] |= 1;
    if (stageIn)
        r.kernel.stageInRegionWord = kRegionWord;
    return r;
}

const raw::ComputeRecord &plainKernel()
{
    static const raw::ComputeRecord r = kernel(false);
    return r;
}

const raw::ComputeRecord &stageInKernel()
{
    static const raw::ComputeRecord r = kernel(true);
    return r;
}

uint8_t *hostBytes(uint64_t va, uint64_t bytes)
{
    for (const auto &[at, bound] : kernel_model::model.binds) {
        auto found = kernel_model::model.memory.find(uint32_t(bound.first));
        if (va >= at && va + bytes <= at + bound.second && found != kernel_model::model.memory.end() &&
            va - at + bytes <= found->second.size())
            return found->second.data() + (va - at);
    }
    return nullptr;
}

// What a job's compute launches see: memory as the CPU left it, with each inline-engine write of the stream applied in
// order; the QMD address of each launch
struct Launches {
    std::vector<uint64_t> qmds;
    std::map<uint64_t, uint8_t> written;  // byte address: the byte the inline engine wrote there
    uint32_t inlineWrites = 0;
    bool waitedBeforeInline = false;

    uint32_t word(uint64_t va) const
    {
        uint32_t w = 0;
        const uint8_t *cpu = hostBytes(va, 4);
        if (cpu)
            memcpy(&w, cpu, 4);
        for (uint32_t b = 0; b < 4; b++)
            if (auto found = written.find(va + b); found != written.end())
                w = (w & ~(0xffu << 8 * b)) | uint32_t(found->second) << 8 * b;
        return w;
    }
};

Launches run(const std::vector<uint32_t> &w)
{
    Launches out;
    uint32_t length = 0, lines = 0, pending = 0;
    uint64_t outUpper = 0, outLower = 0, at = 0;
    bool waited = false, referenced = false;
    auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (m == 0x0050)
            referenced = waited;
        if (subc != raw::SUBC_COMPUTE)
            return;
        switch (m) {
        case NVB1C0_WAIT_FOR_IDLE:
            waited = true;
            referenced = false;
            break;
        case NVB1C0_LINE_LENGTH_IN:
            length = v;
            break;
        case NVB1C0_LINE_COUNT:
            lines = v;
            break;
        case NVB1C0_OFFSET_OUT_UPPER:
            outUpper = v;
            break;
        case NVB1C0_OFFSET_OUT:
            outLower = v;
            break;
        case NVB1C0_LAUNCH_DMA:
            pending = length * lines;
            at = outUpper << 32 | outLower;
            out.inlineWrites++;
            if (referenced)
                out.waitedBeforeInline = true;
            break;
        case NVB1C0_LOAD_INLINE_DATA:
            for (uint32_t b = 0; b < 4 && pending; b++, pending--)
                out.written[at++] = uint8_t(v >> 8 * b);
            break;
        case NVB1C0_SEND_PCAS_A:
            out.qmds.push_back(uint64_t(v) << 8);
            break;
        default:
            break;
        }
    };
    uint32_t op = 0, subc = 0, m = 0, left = 0;
    for (const uint32_t word : w) {
        if (left) {
            method(subc, m, word);
            left--;
            if (op == 1 || op == 5)
                m += 4;
            if (op == 5)
                op = 3;
            continue;
        }
        const uint32_t form = word >> 29;
        if (!word)
            continue;
        subc = word >> 13 & 7;
        m = (word & 0xfff) << 2;
        if (form == 4) {
            method(subc, m, word >> 16 & 0x1fff);
            continue;
        }
        op = form;
        left = word >> 16 & 0x1fff;
    }
    return out;
}

class DeviceIndirectDispatch : public harness::DeviceTest {
protected:
    // One indirect dispatch of `r` reading its grid at `offset` in `arguments`, with the stage-in region at `regionOffset`
    // in `region` where given: what the command buffer submitted
    const kernel_model::Job *dispatched(const raw::ComputeRecord &r, const raw::Buffer &arguments, uint64_t offset,
                                        const raw::Buffer *region = nullptr, uint64_t regionOffset = 0)
    {
        const raw::ComputeState *k = raw::placeCompute(dev, r);
        EXPECT_NE(k, nullptr);
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!k || !cb)
            return nullptr;
        const size_t before = kernel_model::model.jobs.size();
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            e.setStageInRegion({1, 1, 1, 1, 1, 1});
            e.setStageInRegionIndirect(region, regionOffset);
            const char *why = nullptr;
            EXPECT_TRUE(e.dispatchIndirect(&arguments, offset, &why)) << (why ? why : "");
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        submitted = {};
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
            const kernel_model::Job &job = kernel_model::model.jobs[j];
            submitted.words.insert(submitted.words.end(), job.words.begin(), job.words.end());
            submitted.pushes.insert(submitted.pushes.end(), job.pushes.begin(), job.pushes.end());
        }
        EXPECT_GT(kernel_model::model.jobs.size(), before);
        return kernel_model::model.jobs.size() > before ? &submitted : nullptr;
    }

    kernel_model::Job submitted;  // the command buffer's jobs, one after the other

    static uint64_t rootOf(const Launches &l, uint64_t qmd)
    {
        return uint64_t(l.word(qmd + 4 * (kQmdRootLo + 1)) & 0xff) << 32 | l.word(qmd + 4 * kQmdRootLo);
    }
};

TEST_F(DeviceIndirectDispatch, TheLaunchSeesTheGridTheBufferHoldsInItsQmdAndRootTable)
{
    raw::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(64, arguments));
    const uint32_t grid[3] = {70000, 3, 513};
    memcpy(arguments.cpu + 16, grid, sizeof grid);
    const kernel_model::Job *job = dispatched(plainKernel(), arguments, 16);
    ASSERT_NE(job, nullptr);
    const Launches l = run(job->words);
    ASSERT_EQ(l.qmds.size(), 1u);
    const uint64_t qmd = l.qmds[0];
    EXPECT_EQ(l.word(qmd + 4 * kQmdRasterWidth), grid[0]);
    EXPECT_EQ(l.word(qmd + 4 * kQmdRasterHeightDepth), grid[1] | grid[2] << 16);
    const uint64_t root = rootOf(l, qmd);
    for (uint32_t a = 0; a < 3; a++)
        EXPECT_EQ(l.word(root + 12 + 4 * a), grid[a]) << "cs.group_count[" << a << "]";
}

TEST_F(DeviceIndirectDispatch, TheCpuWritesNoGridTheGpuReadsTheBuffersWordsAfterAWait)
{
    raw::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(64, arguments));
    const uint32_t grid[3] = {5, 6, 7};
    memcpy(arguments.cpu, grid, sizeof grid);
    const kernel_model::Job *job = dispatched(plainKernel(), arguments, 0);
    ASSERT_NE(job, nullptr);
    const Launches l = run(job->words);
    ASSERT_EQ(l.qmds.size(), 1u);
    uint32_t cpu = 0;
    memcpy(&cpu, hostBytes(l.qmds[0] + 4 * kQmdRasterWidth, 4), 4);
    EXPECT_EQ(cpu, 0u) << "the QMD the CPU wrote holds no grid";
    EXPECT_TRUE(l.waitedBeforeInline) << "a wait for idle and SET_REFERENCE before the inline writes";
    // the root table's group count, then the QMD's width, height and depth
    const std::vector<std::pair<uint64_t, uint32_t>> want = {
        {arguments.va, 3}, {arguments.va, 1}, {arguments.va + 4, 1}, {arguments.va + 8, 1}};
    std::vector<std::pair<uint64_t, uint32_t>> fetched;
    for (size_t i = 0; i < job->pushes.size(); i++) {
        const TegraGM20BAbiPush &p = job->pushes[i];
        if (p.va < arguments.va || p.va >= arguments.va + 64)
            continue;
        fetched.push_back({p.va, p.dwords});
        EXPECT_TRUE(p.flags & TegraGM20BPushNoPrefetch) << "fetched only when the stream reaches it";
        ASSERT_GT(i, 0u);
        EXPECT_TRUE(job->pushes[i - 1].flags & TegraGM20BPushIncomplete) << "the header's push runs on into it";
    }
    EXPECT_EQ(fetched, want) << "the arguments' words are pushes of their own";
}

TEST_F(DeviceIndirectDispatch, AStageInRegionFromABufferReachesTheRootTablesRegionWords)
{
    raw::Buffer arguments, region;
    ASSERT_TRUE(dev.createBuffer(64, arguments) && dev.createBuffer(64, region));
    const uint32_t grid[3] = {1, 1, 1}, words[6] = {9, 8, 7, 300, 200, 100};
    memcpy(arguments.cpu, grid, sizeof grid);
    memcpy(region.cpu + 8, words, sizeof words);
    const kernel_model::Job *job = dispatched(stageInKernel(), arguments, 0, &region, 8);
    ASSERT_NE(job, nullptr);
    const Launches l = run(job->words);
    ASSERT_EQ(l.qmds.size(), 1u);
    const uint64_t root = rootOf(l, l.qmds[0]);
    const uint32_t want[8] = {9, 8, 7, 0, 300, 200, 100, 0};
    for (uint32_t i = 0; i < 8; i++)
        EXPECT_EQ(l.word(root + kRootPush + 4 * (kRegionWord - 1 + i)), want[i]) << "region word " << i;
}

TEST_F(DeviceIndirectDispatch, ArgumentsPastTheirBufferAreRefused)
{
    raw::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(16, arguments));
    const raw::ComputeState *k = raw::placeCompute(dev, plainKernel());
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_TRUE(k && cb);
    raw::ComputeEncoder e(*cb, dev);
    e.setState(k);
    const char *why = nullptr;
    EXPECT_FALSE(e.dispatchIndirect(&arguments, 8, &why));
    EXPECT_FALSE(e.dispatchIndirect(&arguments, 2, &why));
    e.end();
}

}  // namespace
